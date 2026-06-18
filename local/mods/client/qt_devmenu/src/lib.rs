mod window;

use std::ffi::c_void;
use utils::{debug, warning};

static MOD_NAME: &str = "qt_devmenu";

#[no_mangle]
fn mod_main(_base_addr: *const c_void) {}

#[no_mangle]
fn mod_main_sync(base_addr: *const c_void) {
    unsafe { game_base::OFFSETS = game_base::offsets_client::get_offsets() };
    game_base::GameBase::initialize(MOD_NAME, base_addr);

    std::thread::spawn(|| {
        // Suppress the native UE4 console. Waits internally for UConsole to
        // appear in GObjects before patching the vtable.
        match injection_utils::hooks::console_open::suppress_native_console() {
            Ok(())  => debug!("[{}]: native console suppressed", MOD_NAME),
            Err(e)  => warning!("[{}]: console suppression failed — {}", MOD_NAME, e),
        }

        // Load the Qt bridge.
        debug!("[{}]: initialising Qt bridge…", MOD_NAME);
        if window::init() {
            debug!("[{}]: devmenu_qt.dll loaded OK", MOD_NAME);
        } else {
            warning!("[{}]: devmenu_qt.dll absent — F8 opens nothing", MOD_NAME);
        }

        // Install the F8 keyboard hook. Runs on its own thread with a Win32
        // message loop, independent of UE4's input system.
        injection_utils::hooks::keyboard::install(
            injection_utils::hooks::keyboard::VK_F8,
            on_menu_open,
            on_menu_close,
        );
        debug!("[{}]: F8 keyboard hook installed", MOD_NAME);
    });
}

fn on_menu_open() {
    debug!("[{}]: F8 pressed — showing dev menu", MOD_NAME);
    window::show();
}

fn on_menu_close() {
    debug!("[{}]: F8 pressed — hiding dev menu", MOD_NAME);
    window::hide();
}
