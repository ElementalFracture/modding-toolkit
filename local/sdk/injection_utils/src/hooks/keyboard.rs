use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};

#[cfg(windows)]
use winapi::{
    shared::minwindef::{LPARAM, LRESULT, WPARAM},
    um::winuser::{
        CallNextHookEx, DispatchMessageW, GetAsyncKeyState, GetMessageW, SetWindowsHookExW,
        TranslateMessage, UnhookWindowsHookEx, HC_ACTION, KBDLLHOOKSTRUCT, MSG, VK_CONTROL,
        VK_MENU, VK_SHIFT, WH_KEYBOARD_LL, WM_KEYDOWN,
    },
};

pub const VK_F8: u32 = 0x77;

type MenuFn = fn();

static IS_OPEN:         AtomicBool  = AtomicBool::new(false);
static OPEN_FN:         AtomicUsize = AtomicUsize::new(0);
static CLOSE_FN:        AtomicUsize = AtomicUsize::new(0);
static VK_TARGET:       AtomicUsize = AtomicUsize::new(0);
// Modifier mask: bit 0 = Shift, bit 1 = Ctrl, bit 2 = Alt.
// When 0 (no modifiers configured) the hook fires regardless of modifier state.
static MOD_TARGET:      AtomicUsize = AtomicUsize::new(0);
// When true, block the T key (0x54) so non-staff can't use map teleport.
// Cleared while the menu is open so users can still bind T as a hotkey.
static BLOCK_TELEPORT:  AtomicBool  = AtomicBool::new(false);

/// Update the menu toggle key at runtime (called when the user rebinds it).
pub fn update_menu_key(vk: u32, mods: u32) {
    VK_TARGET.store(if vk != 0 { vk } else { VK_F8 } as usize, Ordering::SeqCst);
    MOD_TARGET.store(mods as usize, Ordering::SeqCst);
}

/// Enable or disable map-teleport blocking for the T key.
/// Call with `true` for non-staff users, `false` for staff/dev.
pub fn set_block_teleport(block: bool) {
    BLOCK_TELEPORT.store(block, Ordering::SeqCst);
}

/// Install a low-level keyboard hook that toggles the dev menu on `vk_code`.
/// Runs on a dedicated thread with its own Win32 message loop (required by
/// WH_KEYBOARD_LL). The chosen key is consumed — it does not reach the game.
pub fn install(vk_code: u32, mods: u32, on_open: MenuFn, on_close: MenuFn) {
    VK_TARGET.store(vk_code as usize, Ordering::SeqCst);
    MOD_TARGET.store(mods as usize, Ordering::SeqCst);
    OPEN_FN.store(on_open  as usize, Ordering::SeqCst);
    CLOSE_FN.store(on_close as usize, Ordering::SeqCst);

    std::thread::spawn(|| {
        #[cfg(windows)]
        unsafe {
            let hook = SetWindowsHookExW(
                WH_KEYBOARD_LL,
                Some(ll_proc),
                std::ptr::null_mut(),
                0,
            );
            if hook.is_null() {
                utils::warning!("[keyboard]: SetWindowsHookExW failed");
                return;
            }
            utils::debug!("[keyboard]: keyboard hook installed");

            let mut msg: MSG = std::mem::zeroed();
            while GetMessageW(&mut msg, std::ptr::null_mut(), 0, 0) != 0 {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            UnhookWindowsHookEx(hook);
        }
    });
}

#[cfg(windows)]
unsafe extern "system" fn ll_proc(code: i32, wparam: WPARAM, lparam: LPARAM) -> LRESULT {
    if code == HC_ACTION as i32 && wparam == WM_KEYDOWN as WPARAM {
        let kb = &*(lparam as *const KBDLLHOOKSTRUCT);

        // Wine/Proton's XInput keyboard translation emits spurious VK_NUMLOCK
        // (0x90) events alongside every real key press.  Blocking here at the
        // LL hook level suppresses both the legacy WM_KEYDOWN path AND the raw
        // WM_INPUT path, so UE4's native key-binding dialog never sees them.
        if kb.vkCode == 0x90 {
            return 1;
        }

        // Block map teleport (hold T + click on map) for non-staff.
        // While the menu is open IS_OPEN=true, so T still reaches the
        // WndProc hotkey binder and users can bind T normally.
        if kb.vkCode == 0x54
            && BLOCK_TELEPORT.load(Ordering::Relaxed)
            && !IS_OPEN.load(Ordering::Relaxed)
        {
            return 1;
        }

        if kb.vkCode == VK_TARGET.load(Ordering::Relaxed) as u32 {
            let req_mods = MOD_TARGET.load(Ordering::Relaxed) as u32;
            let mods_ok = if req_mods == 0 {
                true // no modifier constraint — fire regardless of modifier state
            } else {
                let shift = GetAsyncKeyState(VK_SHIFT)   < 0;
                let ctrl  = GetAsyncKeyState(VK_CONTROL) < 0;
                let alt   = GetAsyncKeyState(VK_MENU)    < 0;
                shift == ((req_mods & 1) != 0)
                    && ctrl  == ((req_mods & 2) != 0)
                    && alt   == ((req_mods & 4) != 0)
            };
            if mods_ok {
                if IS_OPEN.load(Ordering::SeqCst) {
                    IS_OPEN.store(false, Ordering::SeqCst);
                    let f = CLOSE_FN.load(Ordering::Relaxed);
                    if f != 0 {
                        let close: MenuFn = std::mem::transmute(f as *const ());
                        close();
                    }
                } else {
                    IS_OPEN.store(true, Ordering::SeqCst);
                    let f = OPEN_FN.load(Ordering::Relaxed);
                    if f != 0 {
                        let open: MenuFn = std::mem::transmute(f as *const ());
                        open();
                    }
                }
                return 1; // consume — key does not reach the game
            }
        }
    }
    CallNextHookEx(std::ptr::null_mut(), code, wparam, lparam)
}
