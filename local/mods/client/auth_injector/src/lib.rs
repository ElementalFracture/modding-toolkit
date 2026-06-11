/**
 * auth_injector — Elemental Fracture client auth mod
 *
 * Reads a token from  <GameDir>/Mods/commands/auth_token.txt  and injects it
 * into the outgoing UDP join packet so the elefrac proxy can identify and
 * authenticate the player.
 *
 * How it works
 * ------------
 * Vanilla Spellbreak encodes its join URL with each ASCII byte doubled (×2).
 * After the URL, the same 2×-encoded section contains the hardware UID:
 *   ComputerName-<32 uppercase hex chars>
 * separated from other segments by null bytes.
 *
 * This mod hooks `sendto` (ws2_32.dll) and, on every outgoing UDP packet,
 * finds that UID segment by scanning for the pattern:
 *   [any bytes][0x5A = 2×'-'][32 bytes that are all 2×-encoded hex chars][0x00]
 *
 * It then overwrites the ENTIRE segment (computer name + dash + hex suffix)
 * with the first 32 bytes of the token (2×-encoded), zeroing the remainder.
 * The Name field is left completely untouched — the player's Steam name
 * propagates through unchanged and appears in-game as expected.
 *
 * The elefrac proxy reads the 32-char hex value out of the UID slot,
 * matches it as a token prefix against the database, and identifies the
 * player without touching the Name field at all.
 *
 * Token file
 * ----------
 * Place the token issued by the Discord bot in:
 *   <GameDir>/Mods/commands/auth_token.txt
 * One token per file, leading/trailing whitespace is stripped.
 * Delete or empty the file to connect without authentication.
 */

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

// sendto from ws2_32.dll.  On x64 Windows all calling conventions are the same,
// so we can use a plain fn pointer (retour 0.1.0 does not support extern annotations
// in static_detour!).
type SendToFn = fn(usize, *const u8, i32, i32, *const u8, i32) -> i32;

static_detour! {
    static SendToHook: fn(usize, *const u8, i32, i32, *const u8, i32) -> i32;
}

// ── Entry point ───────────────────────────────────────────────────────────────

#[no_mangle]
fn mod_main(_base_addr: *const c_void) {
    // Open our own log file in Mods/dlls/ — UE4 holds the game log with
    // exclusive write access so we can't append there.
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
    }
}

// ── Path helpers ──────────────────────────────────────────────────────────────

/// Returns the g3 content directory (.../Spellbreak/g3/).
fn game_dir() -> Option<PathBuf> {
    let mut p = std::env::current_exe().ok()?;
    p.pop(); // removes g3.exe
    p.pop(); // Win64
    p.pop(); // Binaries
    Some(p)
}

/// Returns the Spellbreak install root (.../Spellbreak/).
fn install_root() -> Option<PathBuf> {
    let mut p = game_dir()?;
    p.pop(); // g3
    Some(p)
}

fn token_path() -> Option<PathBuf> {
    let mut p = install_root()?;
    p.push("Mods");
    p.push("commands");
    p.push("auth_token.txt");
    Some(p)
}

/// Finds the most-recently-modified g3-*.log in Saved/Logs/ — the same glob
/// the elefrac supervisor uses on the server side.

fn load_token() -> Option<Vec<u8>> {
    let path = token_path()?;
    let raw = fs::read_to_string(&path).ok()?;
    let trimmed = raw.trim();
    if trimmed.is_empty() {
        return None;
    }
    // log! not available yet when load_token() first runs — println is fine here
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
        // Work on a copy to avoid mutating the caller's buffer.
        let mut data = unsafe { std::slice::from_raw_parts(buf, len as usize) }.to_vec();
        if try_inject_token(&mut data) {
            return SendToHook.call(s, data.as_ptr(), len, flags, to, tolen);
        }
    }
    SendToHook.call(s, buf, len, flags, to, tolen)
}

// ── Token injection ───────────────────────────────────────────────────────────

/// Finds the 2×-encoded hardware UID segment in `data` and overwrites it
/// with the first 32 bytes of the token (each byte doubled), zeroing the
/// remainder of the slot.  The Name field is left completely untouched.
///
/// The server reads the 32 2×-encoded hex chars out of the UID field,
/// decodes them, and matches the result as a token prefix against the DB.
/// Returns true if a replacement was made.
fn try_inject_token(data: &mut Vec<u8>) -> bool {
    let token = match TOKEN.get() {
        Some(t) => t,
        None => return false,
    };
    if token.len() < 32 {
        log!("Token too short ({} bytes) — need at least 32", token.len());
        return false;
    }

    // Locate the 2×-encoded /Game/Maps/ header.
    let header_pos = match data
        .windows(ENCODED_HEADER.len())
        .position(|w| w == ENCODED_HEADER)
    {
        Some(p) => p,
        None => return false,
    };

    // Find the UID segment after the header.
    let (seg_start, seg_end) = match find_uid_segment(data, header_pos + ENCODED_HEADER.len()) {
        Some(pair) => pair,
        None => return false,
    };
    let seg_len = seg_end - seg_start;

    // Write the first 32 token chars (2×-encoded) into the slot start.
    for i in 0..32 {
        data[seg_start + i] = token[i].wrapping_mul(2);
    }
    // Zero out the rest of the original slot (removes the computer name).
    for i in 32..seg_len {
        data[seg_start + i] = 0x00;
    }

    let preview = std::str::from_utf8(&token[..8]).unwrap_or("?");
    log!("Injected token {preview}... into UID slot (32/{seg_len} bytes, 2x-enc)");

    true
}
