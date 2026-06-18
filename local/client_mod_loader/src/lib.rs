/*!
 * client_mod_loader — XInput1_3 proxy DLL for Spellbreak client mod loading.
 *
 * Deploy as:  <GameDir>/g3/Binaries/Win64/xinput1_3.dll
 *
 * Why this works
 * --------------
 * Spellbreak.exe statically imports XINPUT1_3.dll.  Proton intercepts
 * steam_api64.dll's dynamic load, so the existing mod_loader never fires on
 * the client.  Placing a proxy DLL under a statically-linked import
 * guarantees our DllMain runs before the first game frame.
 *
 * On DLL_PROCESS_ATTACH we:
 *   1. Immediately load the real system xinput1_3.dll (from System32) so all
 *      forwarded XInput calls work transparently.
 *   2. Spawn a thread that waits 5 s then loads every *.dll in
 *      <InstallRoot>/Mods/dlls/ and calls mod_main(base_addr) on each — the
 *      same contract as the server-side mod_loader.
 */

use std::ffi::c_void;
use std::path::PathBuf;
use std::sync::OnceLock;
use std::{fs, thread};
use std::time::Duration;

#[cfg(windows)]
use winapi::{
    shared::minwindef::{BOOL, DWORD, HINSTANCE, LPVOID},
    um::libloaderapi::{GetModuleHandleA, GetProcAddress, LoadLibraryA},
    um::sysinfoapi::GetSystemDirectoryA,
};

// ── Real XInput function pointers ─────────────────────────────────────────────

type RawFn = unsafe extern "system" fn();

struct XInputFns {
    get_state:   RawFn,
    set_state:   RawFn,
    get_caps:    RawFn,
    enable:      RawFn,
    get_dsound:  RawFn,
    get_battery: RawFn,
    get_key:     RawFn,
    ord100:      RawFn,
    ord101:      RawFn,
    ord102:      RawFn,
    ord103:      RawFn,
}

unsafe impl Send for XInputFns {}
unsafe impl Sync for XInputFns {}

static REAL: OnceLock<XInputFns> = OnceLock::new();

unsafe fn noop() {}

unsafe fn load_real_xinput() {
    // Build full system32 path to avoid recursively loading ourselves.
    let mut buf = [0u8; 260];
    let len = GetSystemDirectoryA(buf.as_mut_ptr() as *mut i8, 260) as usize;
    let sys = std::str::from_utf8(&buf[..len]).unwrap_or("C:\\Windows\\System32");
    let path = format!("{sys}\\xinput1_3.dll\0");
    let h = LoadLibraryA(path.as_ptr() as *const i8);
    if h.is_null() {
        return;
    }

    macro_rules! proc_named {
        ($name:literal) => {{
            let p = GetProcAddress(h, concat!($name, "\0").as_ptr() as *const i8);
            if p.is_null() { std::mem::transmute(noop as unsafe fn()) }
            else           { std::mem::transmute(p) }
        }};
    }
    macro_rules! proc_ord {
        ($ord:literal) => {{
            let p = GetProcAddress(h, $ord as usize as *const i8);
            if p.is_null() { std::mem::transmute(noop as unsafe fn()) }
            else           { std::mem::transmute(p) }
        }};
    }

    let _ = REAL.set(XInputFns {
        get_state:   proc_named!("XInputGetState"),
        set_state:   proc_named!("XInputSetState"),
        get_caps:    proc_named!("XInputGetCapabilities"),
        enable:      proc_named!("XInputEnable"),
        get_dsound:  proc_named!("XInputGetDSoundAudioDeviceGuids"),
        get_battery: proc_named!("XInputGetBatteryInformation"),
        get_key:     proc_named!("XInputGetKeystroke"),
        ord100:      proc_ord!(100),
        ord101:      proc_ord!(101),
        ord102:      proc_ord!(102),
        ord103:      proc_ord!(103),
    });
}

// ── XInput forwarding exports ──────────────────────────────────────────────────

#[no_mangle]
pub unsafe extern "system" fn XInputGetState(dw_user: DWORD, p_state: *mut c_void) -> DWORD {
    if let Some(f) = REAL.get() {
        let fp: unsafe extern "system" fn(DWORD, *mut c_void) -> DWORD = std::mem::transmute(f.get_state);
        return fp(dw_user, p_state);
    }
    0xFFFF_FFFF // ERROR_DEVICE_NOT_CONNECTED
}

#[no_mangle]
pub unsafe extern "system" fn XInputSetState(dw_user: DWORD, p_vib: *mut c_void) -> DWORD {
    if let Some(f) = REAL.get() {
        let fp: unsafe extern "system" fn(DWORD, *mut c_void) -> DWORD = std::mem::transmute(f.set_state);
        return fp(dw_user, p_vib);
    }
    0xFFFF_FFFF
}

#[no_mangle]
pub unsafe extern "system" fn XInputGetCapabilities(dw_user: DWORD, dw_flags: DWORD, p_caps: *mut c_void) -> DWORD {
    if let Some(f) = REAL.get() {
        let fp: unsafe extern "system" fn(DWORD, DWORD, *mut c_void) -> DWORD = std::mem::transmute(f.get_caps);
        return fp(dw_user, dw_flags, p_caps);
    }
    0xFFFF_FFFF
}

#[no_mangle]
pub unsafe extern "system" fn XInputEnable(enable: BOOL) {
    if let Some(f) = REAL.get() {
        let fp: unsafe extern "system" fn(BOOL) = std::mem::transmute(f.enable);
        fp(enable);
    }
}

#[no_mangle]
pub unsafe extern "system" fn XInputGetDSoundAudioDeviceGuids(dw_user: DWORD, p_render: *mut c_void, p_capture: *mut c_void) -> DWORD {
    if let Some(f) = REAL.get() {
        let fp: unsafe extern "system" fn(DWORD, *mut c_void, *mut c_void) -> DWORD = std::mem::transmute(f.get_dsound);
        return fp(dw_user, p_render, p_capture);
    }
    0xFFFF_FFFF
}

#[no_mangle]
pub unsafe extern "system" fn XInputGetBatteryInformation(dw_user: DWORD, dev_type: u8, p_batt: *mut c_void) -> DWORD {
    if let Some(f) = REAL.get() {
        let fp: unsafe extern "system" fn(DWORD, u8, *mut c_void) -> DWORD = std::mem::transmute(f.get_battery);
        return fp(dw_user, dev_type, p_batt);
    }
    0xFFFF_FFFF
}

#[no_mangle]
pub unsafe extern "system" fn XInputGetKeystroke(dw_user: DWORD, reserved: DWORD, p_ks: *mut c_void) -> DWORD {
    if let Some(f) = REAL.get() {
        let fp: unsafe extern "system" fn(DWORD, DWORD, *mut c_void) -> DWORD = std::mem::transmute(f.get_key);
        return fp(dw_user, reserved, p_ks);
    }
    0xFFFF_FFFF
}

// Ordinal-only exports (100–103)
#[no_mangle] pub unsafe extern "system" fn XInputOrd100(a: DWORD, b: *mut c_void) -> DWORD {
    if let Some(f) = REAL.get() {
        let fp: unsafe extern "system" fn(DWORD, *mut c_void) -> DWORD = std::mem::transmute(f.ord100);
        return fp(a, b);
    }
    0xFFFF_FFFF
}
#[no_mangle] pub unsafe extern "system" fn XInputOrd101(a: DWORD, b: *mut c_void) -> DWORD {
    if let Some(f) = REAL.get() {
        let fp: unsafe extern "system" fn(DWORD, *mut c_void) -> DWORD = std::mem::transmute(f.ord101);
        return fp(a, b);
    }
    0xFFFF_FFFF
}
#[no_mangle] pub unsafe extern "system" fn XInputOrd102(a: DWORD) -> DWORD {
    if let Some(f) = REAL.get() {
        let fp: unsafe extern "system" fn(DWORD) -> DWORD = std::mem::transmute(f.ord102);
        return fp(a);
    }
    0xFFFF_FFFF
}
#[no_mangle] pub unsafe extern "system" fn XInputOrd103(a: DWORD) -> DWORD {
    if let Some(f) = REAL.get() {
        let fp: unsafe extern "system" fn(DWORD) -> DWORD = std::mem::transmute(f.ord103);
        return fp(a);
    }
    0xFFFF_FFFF
}

// ── DllMain ────────────────────────────────────────────────────────────────────

#[no_mangle]
pub extern "system" fn DllMain(
    _h_module: HINSTANCE,
    dw_reason: DWORD,
    _lp_reserved: LPVOID,
) -> BOOL {
    if dw_reason == 1 {
        // DLL_PROCESS_ATTACH — load real XInput immediately, then start mod loader thread.
        unsafe { load_real_xinput(); }
        thread::spawn(|| load_client_mods());
    }
    1
}

// ── Mod loading ────────────────────────────────────────────────────────────────

fn install_root() -> Option<PathBuf> {
    // exe is at .../Spellbreak/g3/Binaries/Win64/Spellbreak.exe
    let mut p = std::env::current_exe().ok()?;
    p.pop(); // Spellbreak.exe
    p.pop(); // Win64
    p.pop(); // Binaries
    p.pop(); // g3
    Some(p)  // → .../Spellbreak/
}

fn is_self(path: &std::path::Path) -> bool {
    path.file_name()
        .and_then(|n| n.to_str())
        .map(|n| n.eq_ignore_ascii_case("xinput1_3.dll"))
        .unwrap_or(false)
}

fn load_client_mods() {
    let root = match install_root() {
        Some(r) => r,
        None => return,
    };

    let _ = fs::write(root.join("Mods/dlls/client_loader_debug.txt"), b"client_mod_loader ran\n");

    let dll_dir = root.join("Mods").join("dlls");
    let pattern = format!("{}/**/*.dll", dll_dir.display());

    let base_addr = unsafe { GetModuleHandleA(std::ptr::null()) as *const c_void };

    // Phase 1 — load every mod DLL and call mod_main_sync immediately.
    // Mirrors the server-side mod_loader's load_sync_mods phase.
    // mod_main_sync is used for hooks that must be installed early
    // (e.g. modding_debugger's console intercept, asset_buddy's PAK bypass).
    type ModMainFn = unsafe extern fn(*const c_void);

    let entries = glob::glob(&pattern).into_iter().flatten().flatten()
        .filter(|p| !is_self(p));

    for path in entries {
        let lib = match unsafe {
            libloading::os::windows::Library::load_with_flags(
                &path,
                libloading::os::windows::LOAD_IGNORE_CODE_AUTHZ_LEVEL,
            )
        } {
            Ok(l)  => l,
            Err(e) => {
                let _ = fs::write(
                    root.join(format!("Mods/dlls/load_err_{}.txt",
                        path.file_name().and_then(|n| n.to_str()).unwrap_or("unknown"))),
                    format!("LoadLibrary failed: {:?}\n", e).as_bytes(),
                );
                continue;
            }
        };

        if let Ok(sym) = unsafe { lib.get::<ModMainFn>(b"mod_main_sync") } {
            let _ = std::panic::catch_unwind(|| unsafe { sym(base_addr) });
        }

        std::mem::forget(lib);
    }

    // Phase 2 — after the game has had time to initialise, call mod_main.
    thread::sleep(Duration::from_secs(5));

    let entries = glob::glob(&pattern).into_iter().flatten().flatten()
        .filter(|p| !is_self(p));

    for path in entries {
        // LoadLibrary returns the cached handle for an already-loaded DLL.
        let lib = match unsafe {
            libloading::os::windows::Library::load_with_flags(
                &path,
                libloading::os::windows::LOAD_IGNORE_CODE_AUTHZ_LEVEL,
            )
        } {
            Ok(l)  => l,
            Err(_) => continue,
        };

        if let Ok(sym) = unsafe { lib.get::<ModMainFn>(b"mod_main") } {
            let _ = std::panic::catch_unwind(|| unsafe { sym(base_addr) });
        }

        std::mem::forget(lib);
    }
}
