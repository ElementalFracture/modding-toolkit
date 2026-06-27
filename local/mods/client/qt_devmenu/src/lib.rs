mod window;

use std::ffi::{c_void, CStr};
use std::os::raw::c_char;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;
use utils::{debug, warning};

static MOD_NAME: &str = "qt_devmenu";

// In-process auth state — written by mod_receive_auth (called from auth_injector),
// read by get_auth_state().  Never touches disk.
static AUTH_IS_CHEAT: AtomicBool   = AtomicBool::new(false);
static AUTH_IS_DEV:   AtomicBool   = AtomicBool::new(false);
static AUTH_USERNAME: Mutex<String> = Mutex::new(String::new());

/// Called by auth_injector.dll (via GetProcAddress) when the server's EF_AUTH
/// packet is intercepted.  Stores the result in memory and applies it immediately.
#[no_mangle]
pub unsafe extern "C" fn mod_receive_auth(
    is_cheat: i32,
    is_dev: i32,
    username: *const c_char,
) {
    let uname = if username.is_null() {
        String::new()
    } else {
        CStr::from_ptr(username).to_str().unwrap_or("").to_owned()
    };

    AUTH_IS_CHEAT.store(is_cheat != 0, Ordering::SeqCst);
    AUTH_IS_DEV.store(is_dev != 0, Ordering::SeqCst);
    if let Ok(mut g) = AUTH_USERNAME.lock() { *g = uname.clone(); }

    injection_utils::hooks::keyboard::set_block_teleport(is_cheat == 0 && is_dev == 0);
    window::set_staff(is_cheat != 0, is_dev != 0, &uname);
}

fn get_auth_state() -> (bool, bool, String) {
    let is_cheat = AUTH_IS_CHEAT.load(Ordering::SeqCst);
    let is_dev   = AUTH_IS_DEV.load(Ordering::SeqCst);
    let username = AUTH_USERNAME.lock().map(|g| g.clone()).unwrap_or_default();
    (is_cheat, is_dev, username)
}

#[no_mangle]
fn mod_main(_base_addr: *const c_void) {}

#[no_mangle]
fn mod_main_sync(base_addr: *const c_void) {
    unsafe { game_base::OFFSETS = game_base::offsets_client::get_offsets() };
    game_base::GameBase::initialize(MOD_NAME, base_addr);

    std::thread::spawn(|| {
        debug!("[{}]: loading ImGui overlay…", MOD_NAME);
        if window::init() {
            debug!("[{}]: devmenu_imgui.dll loaded OK", MOD_NAME);
        } else {
            warning!("[{}]: devmenu_imgui.dll absent — menu key opens nothing", MOD_NAME);
        }

        match injection_utils::hooks::console_open::suppress_native_console() {
            Ok(())  => debug!("[{}]: native console suppressed", MOD_NAME),
            Err(e)  => warning!("[{}]: console suppression failed — {}", MOD_NAME, e),
        }

        // Apply whatever auth state we have so far.  If auth_injector hasn't
        // received the EF_AUTH packet yet the atomics are default (false / ""),
        // which means: block teleport for safety, no toast yet.  mod_receive_auth
        // will fire once the server responds and update everything then.
        {
            let (is_cheat, is_dev, username) = get_auth_state();
            injection_utils::hooks::keyboard::set_block_teleport(!is_cheat && !is_dev);
            window::set_staff(is_cheat, is_dev, &username);
        }

        let (vk, mods) = window::get_menu_key();
        window::set_menu_key_callback(on_menu_key_changed);
        injection_utils::hooks::keyboard::install(vk, mods, on_menu_open, on_menu_close);
        debug!("[{}]: keyboard hook installed (vk=0x{:02X} mods={})", MOD_NAME, vk, mods);
    });
}

/// Called by devmenu_imgui.dll (render thread) when the user rebinds the menu key.
unsafe extern "C" fn on_menu_key_changed(vk: u32, mods: u32) {
    injection_utils::hooks::keyboard::update_menu_key(vk, mods);
    debug!("[{}]: menu key updated (vk=0x{:02X} mods={})", MOD_NAME, vk, mods);
}

fn on_menu_open() {
    debug!("[{}]: showing dev menu", MOD_NAME);
    let (is_cheat, is_dev, username) = get_auth_state();
    injection_utils::hooks::keyboard::set_block_teleport(!is_cheat && !is_dev);
    window::set_staff(is_cheat, is_dev, &username);
    window::show();
}

fn on_menu_close() {
    debug!("[{}]: hiding dev menu", MOD_NAME);
    window::hide();
}
