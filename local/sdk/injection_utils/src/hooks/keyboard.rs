use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};

#[cfg(windows)]
use winapi::{
    shared::minwindef::{LPARAM, LRESULT, WPARAM},
    um::winuser::{
        CallNextHookEx, DispatchMessageW, GetMessageW, SetWindowsHookExW, TranslateMessage,
        UnhookWindowsHookEx, HC_ACTION, KBDLLHOOKSTRUCT, MSG, WH_KEYBOARD_LL, WM_KEYDOWN,
    },
};

pub const VK_F8: u32 = 0x77;

type MenuFn = fn();

static IS_OPEN:  AtomicBool  = AtomicBool::new(false);
static OPEN_FN:  AtomicUsize = AtomicUsize::new(0);
static CLOSE_FN: AtomicUsize = AtomicUsize::new(0);
static VK_TARGET: AtomicUsize = AtomicUsize::new(0);

/// Install a low-level keyboard hook that toggles the dev menu on `vk_code`.
/// Runs on a dedicated thread with its own Win32 message loop (required by
/// WH_KEYBOARD_LL). The chosen key is consumed — it does not reach the game.
pub fn install(vk_code: u32, on_open: MenuFn, on_close: MenuFn) {
    VK_TARGET.store(vk_code as usize, Ordering::SeqCst);
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
            utils::debug!("[keyboard]: F8 hook installed");

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
        if kb.vkCode == VK_TARGET.load(Ordering::Relaxed) as u32 {
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
    CallNextHookEx(std::ptr::null_mut(), code, wparam, lparam)
}
