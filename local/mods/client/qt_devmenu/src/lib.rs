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
        // Install the hook first — intercept_console_open calls wait_for_loaded()
        // internally which spins until UConsole appears in GObjects.  No fixed
        // sleep needed; the hook lands as early as the engine allows.
        match injection_utils::hooks::console_open::intercept_console_open(
            on_console_open,
            Some(on_console_close),
        ) {
            Ok(())  => debug!("[{}]: FakeGotoState hook installed", MOD_NAME),
            Err(e)  => warning!("[{}]: hook failed — {}", MOD_NAME, e),
        }

        // Load the Qt bridge after the hook is live.  If devmenu_qt.dll is absent
        // or slow to init, tilde presses will be swallowed but show nothing — the
        // native console will never reappear.
        debug!("[{}]: initialising Qt bridge…", MOD_NAME);
        if window::init() {
            debug!("[{}]: devmenu_qt.dll loaded OK", MOD_NAME);
        } else {
            warning!("[{}]: devmenu_qt.dll absent — tilde opens nothing", MOD_NAME);
        }
    });
}

fn on_console_open() {
    debug!("[{}]: tilde pressed — showing Qt dev menu", MOD_NAME);
    window::show();
}

fn on_console_close() {
    debug!("[{}]: close requested — hiding Qt dev menu", MOD_NAME);
    window::hide();
}
