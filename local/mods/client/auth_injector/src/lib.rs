use std::ffi::c_void;
use std::fs;
use std::fs::OpenOptions;
use std::io::Write;
use std::io::{BufRead, BufReader};
use std::net::TcpStream;
use std::path::PathBuf;
use std::sync::{OnceLock, Mutex};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;
use retour::static_detour;

static INITIALIZED: AtomicBool = AtomicBool::new(false);

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
            // Format: EF_AUTH:<cheat>:<dev>:<username>  (legacy: EF_AUTH:<cheat>:<username>)
            let mut parts = rest.splitn(3, |&b| b == b':');
            let cheat_part = parts.next().unwrap_or(&[]);
            let second     = parts.next().unwrap_or(&[]);
            let third      = parts.next();
            let (is_cheat, is_dev, username) = if let Some(uname) = third {
                (cheat_part == b"t", second == b"t", std::str::from_utf8(uname).unwrap_or("").to_owned())
            } else {
                (cheat_part == b"t", false, std::str::from_utf8(second).unwrap_or("").to_owned())
            };
            notify_devmenu(is_cheat, is_dev, &username);
            log!("EF_AUTH received: is_cheat={is_cheat} is_dev={is_dev} username={username}");
            // Loop — fetch the next real game packet; this one is consumed.
            continue;
        }
        return n;
    }
}

// Push auth state directly into the devmenu DLL's in-process memory.
// Tries devmenu_imgui.dll first (current system), then qt_devmenu.dll (legacy).
fn notify_devmenu(is_cheat: bool, is_dev: bool, username: &str) {
    #[cfg(windows)]
    unsafe {
        use std::ffi::CString;
        use winapi::um::libloaderapi::{GetModuleHandleA, GetProcAddress};

        type SetStaffFn = unsafe extern "C" fn(i32, i32, *const i8);

        let cname = match CString::new(username) {
            Ok(s)  => s,
            Err(_) => { log!("notify_devmenu: username contained null byte"); return; }
        };

        // Primary: devmenu_imgui.dll / devmenu_set_staff
        let module = GetModuleHandleA(b"devmenu_imgui.dll\0".as_ptr() as *const i8);
        if !module.is_null() {
            let proc = GetProcAddress(module, b"devmenu_set_staff\0".as_ptr() as *const i8);
            if !proc.is_null() {
                let f: SetStaffFn = std::mem::transmute(proc);
                f(is_cheat as i32, is_dev as i32, cname.as_ptr());
                log!("notify_devmenu: called devmenu_imgui.dll::devmenu_set_staff");
                return;
            }
        }

        // Fallback: qt_devmenu.dll / mod_receive_auth (legacy loader)
        let module = GetModuleHandleA(b"qt_devmenu.dll\0".as_ptr() as *const i8);
        if module.is_null() {
            log!("notify_devmenu: neither devmenu_imgui.dll nor qt_devmenu.dll loaded");
            return;
        }
        let proc = GetProcAddress(module, b"mod_receive_auth\0".as_ptr() as *const i8);
        if proc.is_null() {
            log!("notify_devmenu: mod_receive_auth not found in qt_devmenu.dll");
            return;
        }
        let f: SetStaffFn = std::mem::transmute(proc);
        f(is_cheat as i32, is_dev as i32, cname.as_ptr());
        log!("notify_devmenu: called qt_devmenu.dll::mod_receive_auth (legacy)");
    }
}

// Notify devmenu that a token was validated at game launch (pre-auth).
// Retries for up to 15 seconds to handle the case where devmenu_imgui.dll
// loads after the auth response arrives.
fn notify_devmenu_preauth(username: &str, is_staff: bool, is_dev: bool) {
    #[cfg(windows)]
    unsafe {
        use std::ffi::CString;
        use winapi::um::libloaderapi::{GetModuleHandleA, GetProcAddress};

        type TokenLoadedFn = unsafe extern "C" fn(*const i8, i32, i32);

        let cname = match CString::new(username) {
            Ok(s)  => s,
            Err(_) => { log!("notify_devmenu_preauth: username contained null byte"); return; }
        };

        let deadline = std::time::Instant::now() + Duration::from_secs(15);
        loop {
            let module = GetModuleHandleA(b"devmenu_imgui.dll\0".as_ptr() as *const i8);
            if !module.is_null() {
                let proc = GetProcAddress(module, b"devmenu_token_loaded\0".as_ptr() as *const i8);
                if !proc.is_null() {
                    let f: TokenLoadedFn = std::mem::transmute(proc);
                    f(cname.as_ptr(), is_staff as i32, is_dev as i32);
                    log!("notify_devmenu_preauth: devmenu_token_loaded called (elapsed {}ms)",
                         (Duration::from_secs(15) - deadline.saturating_duration_since(std::time::Instant::now())).as_millis());
                    return;
                }
                log!("notify_devmenu_preauth: devmenu_imgui.dll loaded but devmenu_token_loaded not found");
                return;
            }

            if std::time::Instant::now() >= deadline {
                log!("notify_devmenu_preauth: timed out waiting for devmenu_imgui.dll");
                return;
            }
            std::thread::sleep(Duration::from_millis(250));
        }
    }
}

// Attempt pre-authentication at game launch against the EF auth server.
// Reads the server host from Mods/commands/ef_server.txt (one line: hostname).
// If unavailable, silently skips — auth will still complete on first server join.
fn pre_auth_at_launch(token: &[u8]) {
    let host = match load_auth_host() {
        Some(h) => h,
        None => {
            log!("pre_auth_at_launch: no ef_server.txt — skipping pre-auth");
            return;
        }
    };
    let addr_str = format!("{}:4948", host);
    log!("pre_auth_at_launch: connecting to {addr_str}");

    let sa: std::net::SocketAddr = match addr_str.parse() {
        Ok(a)  => a,
        Err(e) => { log!("pre_auth_at_launch: invalid address '{addr_str}': {e}"); return; }
    };

    let stream = match TcpStream::connect_timeout(&sa, Duration::from_secs(5)) {
        Ok(s)  => s,
        Err(e) => { log!("pre_auth_at_launch: connect failed: {e}"); return; }
    };
    let _ = stream.set_read_timeout(Some(Duration::from_secs(8)));

    let mut stream_w = match stream.try_clone() {
        Ok(s) => s,
        Err(e) => { log!("pre_auth_at_launch: clone failed: {e}"); return; }
    };

    let token_str = std::str::from_utf8(token).unwrap_or("");
    if let Err(e) = stream_w.write_all(format!("{token_str}\n").as_bytes()) {
        log!("pre_auth_at_launch: write failed: {e}");
        return;
    }

    let reader = BufReader::new(stream);
    for line in reader.lines().take(1) {
        match line {
            Ok(resp) if resp.starts_with("OK ") => {
                let parts: Vec<&str> = resp[3..].splitn(3, ' ').collect();
                if parts.len() == 3 {
                    let username = parts[0];
                    let is_staff = parts[1] == "1";
                    let is_dev   = parts[2] == "1";
                    log!("pre_auth_at_launch: OK username={username} staff={is_staff} dev={is_dev}");
                    notify_devmenu_preauth(username, is_staff, is_dev);
                }
            }
            Ok(resp) => log!("pre_auth_at_launch: server replied: {resp}"),
            Err(e)   => log!("pre_auth_at_launch: read error: {e}"),
        }
    }
}

fn load_auth_host() -> Option<String> {
    let mut p = install_root()?;
    p.push("Mods");
    p.push("commands");
    p.push("ef_server.txt");
    let raw = fs::read_to_string(&p).ok()?;
    let host = raw.trim().to_owned();
    if host.is_empty() { return None; }
    Some(host)
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
    if INITIALIZED.swap(true, Ordering::SeqCst) {
        return;
    }

    if let Some(mut p) = install_root() {
        p.push("Mods");
        p.push("dlls");
        p.push("auth_injector.log");
        if let Ok(f) = OpenOptions::new().create(true).append(true).open(&p) {
            LOG.set(Mutex::new(f)).ok();
        }
    }

    log!("Starting...");

    // Remove any auth_result.txt left by older versions — auth state must not live on disk.
    if let Some(mut p) = install_root() {
        p.push("Mods"); p.push("dlls"); p.push("auth_result.txt");
        if p.exists() {
            let _ = std::fs::remove_file(&p);
            log!("Removed stale auth_result.txt");
        }
    }

    let token = match load_token() {
        Some(token) => {
            let preview = std::str::from_utf8(&token[..token.len().min(8)]).unwrap_or("?");
            log!("Token loaded: {preview}... ({} bytes)", token.len());
            token
        }
        None => {
            log!("No auth_token.txt found — connecting without authentication");
            return;
        }
    };

    // Pre-auth at launch — runs in a background thread so it doesn't stall
    // the game startup.  On success, calls devmenu_token_loaded() with auth info.
    {
        let tok_clone = token.clone();
        std::thread::spawn(move || pre_auth_at_launch(&tok_clone));
    }

    TOKEN.set(token).ok();

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
