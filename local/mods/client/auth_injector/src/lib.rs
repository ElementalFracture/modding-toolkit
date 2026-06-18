use std::ffi::c_void;
use std::fs;
use std::fs::OpenOptions;
use std::io::Write;
use std::path::PathBuf;
use std::sync::{OnceLock, Mutex};
use retour::static_detour;

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

// Token loaded from disk at startup.  None = no token file / empty file.
static TOKEN: OnceLock<Vec<u8>> = OnceLock::new();

// 2×-encoded `/Game/Maps/` — the header that precedes the join URL in a
// vanilla Spellbreak UDP connection packet.
const ENCODED_HEADER: &[u8] = &[94, 142, 194, 218, 202, 94, 154, 194, 224, 230, 94];
// 2×-encoded '-' (0x2D × 2 = 0x5A) — separates ComputerName from hex in UID.
const ENCODED_DASH: u8 = 0x5A;

/// True if `b` is a 2×-encoded hex digit (0-9 or a-f or A-F).
#[inline]
fn is_2x_hex(b: u8) -> bool {
    let c = b >> 1;
    (b'0'..=b'9').contains(&c) || (b'a'..=b'f').contains(&c) || (b'A'..=b'F').contains(&c)
}

/// Find the UID segment in `data` starting from `search_start`.
/// Returns `(seg_start, seg_end)` where `data[seg_start..seg_end]` is the
/// UID segment (excluding the terminating 0x00).
fn find_uid_segment(data: &[u8], search_start: usize) -> Option<(usize, usize)> {
    let mut seg_start = search_start;
    let len = data.len();
    let mut i = search_start;

    loop {
        let at_end = i >= len;
        let is_sep  = !at_end && data[i] == 0x00;

        if at_end || is_sep {
            let seg = &data[seg_start..i];
            // Look for last 0x5A such that exactly 32 2×-hex bytes follow to end.
            if seg.len() >= 35 {
                for j in (0..seg.len().saturating_sub(32)).rev() {
                    if seg[j] == ENCODED_DASH
                        && j + 1 + 32 == seg.len()
                        && seg[j + 1..].iter().all(|&b| is_2x_hex(b))
                    {
                        return Some((seg_start, i));
                    }
                }
            }
            if at_end { break; }
            seg_start = i + 1;
        }
        i += 1;
    }
    None
}

type SendToFn   = fn(usize, *const u8, i32, i32, *const u8, i32)  -> i32;
type RecvFromFn = fn(usize, *mut u8,   i32, i32, *mut u8,   *mut i32) -> i32;

static_detour! {
    static SendToHook:   fn(usize, *const u8, i32, i32, *const u8, i32)  -> i32;
    static RecvFromHook: fn(usize, *mut u8,   i32, i32, *mut u8,   *mut i32) -> i32;
}

// ── Recvfrom hook ─────────────────────────────────────────────────────────────

fn hooked_recvfrom(
    s:       usize,
    buf:     *mut u8,
    len:     i32,
    flags:   i32,
    from:    *mut u8,
    fromlen: *mut i32,
) -> i32 {
    loop {
        let n = RecvFromHook.call(s, buf, len, flags, from, fromlen);
        if n <= 0 {
            return n;
        }
        let data = unsafe { std::slice::from_raw_parts(buf as *const u8, n as usize) };
        if let Some(rest) = data.strip_prefix(b"EF_AUTH:") {
            // Format: EF_AUTH:<t|f>:<username>
            let is_cheat = rest.first() == Some(&b't');
            let username = rest.get(2..)
                .and_then(|b| std::str::from_utf8(b).ok())
                .unwrap_or("")
                .to_owned();
            write_auth_result(is_cheat, &username);
            log!("EF_AUTH received: is_cheat={is_cheat} username={username}");
            // Loop — fetch the next real game packet; this one is consumed.
            continue;
        }
        return n;
    }
}

fn write_auth_result(is_cheat: bool, username: &str) {
    if let Some(mut p) = install_root() {
        p.push("Mods");
        p.push("dlls");
        p.push("auth_result.txt");
        let content = format!(
            "IS_CHEAT={}\nUSERNAME={}\n",
            if is_cheat { "true" } else { "false" },
            username,
        );
        let _ = std::fs::write(&p, content);
    }
}

unsafe fn install_recvfrom_hook() -> Result<(), Box<dyn std::error::Error>> {
    #[cfg(windows)]
    use winapi::um::libloaderapi::{GetModuleHandleA, GetProcAddress};

    let module = GetModuleHandleA(b"ws2_32.dll\0".as_ptr() as *const i8);
    if module.is_null() {
        return Err("ws2_32.dll not loaded".into());
    }
    let addr = GetProcAddress(module, b"recvfrom\0".as_ptr() as *const i8);
    if addr.is_null() {
        return Err("recvfrom not found in ws2_32.dll".into());
    }
    let fn_ptr: RecvFromFn = std::mem::transmute(addr);
    RecvFromHook.initialize(fn_ptr, hooked_recvfrom)?;
    RecvFromHook.enable()?;
    Ok(())
}

// ── Entry point ───────────────────────────────────────────────────────────────

#[no_mangle]
fn mod_main(_base_addr: *const c_void) {
    if let Some(mut p) = install_root() {
        p.push("Mods");
        p.push("dlls");
        p.push("auth_injector.log");
        if let Ok(f) = OpenOptions::new().create(true).append(true).open(&p) {
            LOG.set(Mutex::new(f)).ok();
        }
    }

    log!("Starting...");

    match load_token() {
        Some(token) => {
            let preview = std::str::from_utf8(&token[..token.len().min(8)]).unwrap_or("?");
            log!("Token loaded: {preview}... ({} bytes)", token.len());
            TOKEN.set(token).ok();
        }
        None => {
            log!("No auth_token.txt found — connecting without authentication");
            return;
        }
    }

    unsafe {
        match install_sendto_hook() {
            Ok(()) => log!("sendto hooked — token will be injected on connect"),
            Err(e) => log!("Failed to hook sendto: {e}"),
        }
        match install_recvfrom_hook() {
            Ok(()) => log!("recvfrom hooked — will intercept EF_AUTH response"),
            Err(e) => log!("Failed to hook recvfrom: {e}"),
        }
    }
}

// ── Path helpers ──────────────────────────────────────────────────────────────

fn install_root() -> Option<PathBuf> {
    let mut p = std::env::current_exe().ok()?;
    p.pop(); // g3-Win64-Test.exe / g3.exe
    p.pop(); // Win64
    p.pop(); // Binaries
    p.pop(); // g3
    Some(p)
}

fn load_token() -> Option<Vec<u8>> {
    let mut p = install_root()?;
    p.push("Mods");
    p.push("commands");
    p.push("auth_token.txt");
    let raw = fs::read_to_string(&p).ok()?;
    let trimmed = raw.trim();
    if trimmed.is_empty() { return None; }
    Some(trimmed.as_bytes().to_vec())
}

// ── Winsock hook ──────────────────────────────────────────────────────────────

unsafe fn install_sendto_hook() -> Result<(), Box<dyn std::error::Error>> {
    #[cfg(windows)]
    use winapi::um::libloaderapi::{GetModuleHandleA, GetProcAddress};

    let module = GetModuleHandleA(b"ws2_32.dll\0".as_ptr() as *const i8);
    if module.is_null() {
        return Err("ws2_32.dll not loaded".into());
    }
    let addr = GetProcAddress(module, b"sendto\0".as_ptr() as *const i8);
    if addr.is_null() {
        return Err("sendto not found in ws2_32.dll".into());
    }

    let fn_ptr: SendToFn = std::mem::transmute(addr);
    SendToHook.initialize(fn_ptr, hooked_sendto)?;
    SendToHook.enable()?;
    Ok(())
}

fn hooked_sendto(
    s:     usize,
    buf:   *const u8,
    len:   i32,
    flags: i32,
    to:    *const u8,
    tolen: i32,
) -> i32 {
    if len > 0 && !buf.is_null() {
        let mut data = unsafe { std::slice::from_raw_parts(buf, len as usize) }.to_vec();
        if try_inject_token(&mut data) {
            return SendToHook.call(s, data.as_ptr(), len, flags, to, tolen);
        }
    }
    SendToHook.call(s, buf, len, flags, to, tolen)
}

// ── Token injection ───────────────────────────────────────────────────────────

fn try_inject_token(data: &mut Vec<u8>) -> bool {
    let token = match TOKEN.get() {
        Some(t) => t,
        None => return false,
    };
    if token.len() < 32 {
        log!("Token too short ({} bytes) — need at least 32", token.len());
        return false;
    }

    let header_pos = match data
        .windows(ENCODED_HEADER.len())
        .position(|w| w == ENCODED_HEADER)
    {
        Some(p) => p,
        None => return false,
    };

    let (seg_start, seg_end) = match find_uid_segment(data, header_pos + ENCODED_HEADER.len()) {
        Some(pair) => pair,
        None => return false,
    };
    let seg_len = seg_end - seg_start;

    // Write 32 2×-encoded token bytes into the UID slot; zero the rest.
    for i in 0..32 {
        data[seg_start + i] = token[i].wrapping_mul(2);
    }
    for i in 32..seg_len {
        data[seg_start + i] = 0x00;
    }

    let preview = std::str::from_utf8(&token[..8]).unwrap_or("?");
    log!("Injected token {preview}... ({}/{}  bytes, 2x-enc)", 32, seg_len);

    true
}
