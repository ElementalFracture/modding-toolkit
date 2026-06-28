use std::ffi::c_void;
use std::io::Write;
use std::sync::{OnceLock, Mutex};
use std::sync::atomic::{AtomicU8, AtomicBool, Ordering};
use game_base::*;
use ue_types::*;
use ue_types::ue_endian::u32le;
use retour::static_detour;

// ── File logger ───────────────────────────────────────────────────────────────

static LOG: OnceLock<Mutex<std::fs::File>> = OnceLock::new();

macro_rules! log {
    ($($arg:tt)*) => {{
        let line = format!("{}\n", format!($($arg)*));
        if let Some(lock) = LOG.get() {
            if let Ok(mut f) = lock.lock() {
                let _ = f.write_all(line.as_bytes());
            }
        }
    }};
}

fn init_log(log_path: std::path::PathBuf) {
    if let Ok(f) = std::fs::OpenOptions::new()
        .create(true).write(true).truncate(true)
        .open(&log_path)
    {
        let _ = LOG.set(Mutex::new(f));
    }
}

// ── Current mode (set by SetMode hook, read by sendto hook) ──────────────────

// Stores the last mode char selected by the player: s/d/q/c/t/x.
// 'x' = unknown / no selection yet.
static CURRENT_MODE: AtomicU8 = AtomicU8::new(b'x');

// True once the EFM control packet for the current mode has been sent.
// Reset to false every time SetMode fires so each new mode triggers a fresh send.
static EFM_NOTIFIED: AtomicBool = AtomicBool::new(false);

/// Called by auth_injector (or any other DLL in-process) to get the current mode.
#[no_mangle]
pub extern "C" fn get_current_mode() -> u8 {
    CURRENT_MODE.load(Ordering::Relaxed)
}

// ── HlString (UE4 heap-allocated string) ─────────────────────────────────────

#[derive(Debug, Copy, Clone)]
#[repr(C, align(0x8))]
struct HlString {
    pub len: u32le,
    _pad: [u8; 4],
    pub s: *const u8,
}

impl std::fmt::Display for HlString {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let n = u32::from(self.len) as usize;
        let bytes: Vec<u8> = (0..n).map(|i| unsafe { *self.s.add(i) }).collect();
        write!(f, "{}", String::from_utf8_lossy(&bytes))
    }
}

// ── Winsock sendto signature ──────────────────────────────────────────────────

type SendToFn = fn(usize, *const u8, i32, i32, *const u8, i32) -> i32;

// ── 2×-encoded join URL header ────────────────────────────────────────────────

// "/Game/Maps/" with each byte doubled — used to identify join packets.
const ENCODED_HEADER: &[u8] = &[94, 142, 194, 218, 202, 94, 154, 194, 224, 230, 94];

// ── Detours ───────────────────────────────────────────────────────────────────

static_detour! {
    static SetMode: fn(*const UGameInstance, *const HlString, *const HlString);
    static SendTo:  fn(usize, *const u8, i32, i32, *const u8, i32) -> i32;
}

// ── SetMode hook ──────────────────────────────────────────────────────────────

fn hook_set_mode(
    game_instance: *const UGameInstance,
    mode:    *const HlString,
    variant: *const HlString,
) {
    if mode.is_null() {
        log!("SetMode: null mode ptr");
        SetMode.call(game_instance, mode, variant);
        return;
    }

    let mode_str = unsafe { (*mode).to_string() };
    let mode_char = mode_char_for(&mode_str);
    CURRENT_MODE.store(mode_char as u8, Ordering::Relaxed);
    // Reset so the next sendto call re-sends the EFM control packet.
    EFM_NOTIFIED.store(false, Ordering::Relaxed);
    log!("SetMode: mode='{}' → char='{}'", mode_str, mode_char as char);

    SetMode.call(game_instance, mode, variant);
}

fn mode_char_for(mode: &str) -> u8 {
    match mode.to_lowercase().as_str() {
        "solo"       => b's',
        "duo"        => b'd',
        "squad"      => b'q',
        "rumblsquad" => b'q',
        "capture"    => b'c',
        "tdm"        => b't',
        _            => b'x',
    }
}

// ── Sendto hook — sends EFM side-channel packet before first game packet ──────
//
// Instead of modifying the join URL (which corrupts UE4's FString length field
// and causes "corrupted packet data" on the server), we send a tiny 5-byte
// control packet "EFM:s" on the same socket and to the same destination as the
// first outgoing game packet.  The router reads this, stores the mode preference
// for the client IP, and routes the subsequent real handshake accordingly.
// The join packet itself is forwarded completely unmodified.

fn hook_sendto(
    s:     usize,
    buf:   *const u8,
    len:   i32,
    flags: i32,
    to:    *const u8,
    tolen: i32,
) -> i32 {
    if len > 0 && !buf.is_null() {
        let mode = CURRENT_MODE.load(Ordering::Relaxed);
        if mode != b'x' && mode != 0 {
            // Send EFM once per mode change, on the first outgoing UDP packet.
            if !EFM_NOTIFIED.swap(true, Ordering::Relaxed) {
                let msg = format!("EFM:{}", mode as char);
                let r = SendTo.call(
                    s, msg.as_ptr() as *const u8, msg.len() as i32,
                    flags, to, tolen,
                );
                log!("Sent EFM:{} control packet to router (result={})", mode as char, r);
            }
        }
    }
    // Always forward the original packet completely unmodified.
    SendTo.call(s, buf, len, flags, to, tolen)
}

// ── Entry point ───────────────────────────────────────────────────────────────

#[no_mangle]
fn mod_main(base_addr: *const c_void) {
    let _ = std::fs::write(
        "Z:\\var\\home\\doobs\\.local\\share\\Steam\\steamapps\\common\\Spellbreak\\Mods\\dlls\\sr_canary.txt",
        b"v5 mod_main called",
    );

    GameBase::initialize("server_router", base_addr);

    init_log(dlls_dir().join("server_router.log"));
    log!("server_router v5 starting, base={:p}", base_addr);

    unsafe {
        // SetMode hook — detect which game mode the player selected.
        let set_mode_ptr: fn(*const UGameInstance, *const HlString, *const HlString) =
            std::mem::transmute(GameBase::singleton().at_offset(0x39910C0));
        log!("SetMode fn_ptr={:p}", set_mode_ptr as *const ());
        match SetMode.initialize(set_mode_ptr, hook_set_mode) {
            Ok(_)  => {}
            Err(e) => { log!("SetMode.initialize failed: {}", e); return; }
        }
        match SetMode.enable() {
            Ok(_)  => log!("SetMode hook active"),
            Err(e) => { log!("SetMode.enable failed: {}", e); return; }
        }

        // sendto hook — sends EFM side-channel control packet before first game packet.
        #[cfg(windows)]
        {
            use winapi::um::libloaderapi::{GetModuleHandleA, GetProcAddress};
            let ws2 = GetModuleHandleA(b"ws2_32.dll\0".as_ptr() as *const i8);
            if ws2.is_null() {
                log!("ws2_32.dll not loaded — sendto hook skipped");
            } else {
                let addr = GetProcAddress(ws2, b"sendto\0".as_ptr() as *const i8);
                if addr.is_null() {
                    log!("sendto not found in ws2_32.dll");
                } else {
                    let fn_ptr: SendToFn = std::mem::transmute(addr);
                    match SendTo.initialize(fn_ptr, hook_sendto) {
                        Ok(_)  => {}
                        Err(e) => { log!("SendTo.initialize failed: {}", e); return; }
                    }
                    match SendTo.enable() {
                        Ok(_)  => log!("sendto hook active — will send EFM side-channel on mode selection"),
                        Err(e) => { log!("SendTo.enable failed: {}", e); return; }
                    }
                }
            }
        }
    }

    log!("mod_main done");
}

// ── Path helpers ──────────────────────────────────────────────────────────────

fn dlls_dir() -> std::path::PathBuf {
    let mut p = std::env::current_exe().unwrap_or_default();
    p.pop(); // Win64
    p.pop(); // Binaries
    p.pop(); // g3
    p.pop(); // install root
    p.push("Mods");
    p.push("dlls");
    p
}
