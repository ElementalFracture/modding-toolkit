mod window;

use std::ffi::c_void;
use std::path::PathBuf;
use utils::{debug, warning};

static MOD_NAME: &str = "qt_devmenu";

#[no_mangle]
fn mod_main(_base_addr: *const c_void) {}

#[no_mangle]
fn mod_main_sync(base_addr: *const c_void) {
    unsafe { game_base::OFFSETS = game_base::offsets_client::get_offsets() };
    game_base::GameBase::initialize(MOD_NAME, base_addr);

    std::thread::spawn(|| {
        // Hook the DXGI factory vtable before suppress_native_console blocks so
        // we intercept the game's swap chain creation at T+0, not T+9.
        debug!("[{}]: loading ImGui overlay…", MOD_NAME);
        if window::init() {
            debug!("[{}]: devmenu_imgui.dll loaded OK", MOD_NAME);
        } else {
            warning!("[{}]: devmenu_imgui.dll absent — F8 opens nothing", MOD_NAME);
        }

        match injection_utils::hooks::console_open::suppress_native_console() {
            Ok(())  => debug!("[{}]: native console suppressed", MOD_NAME),
            Err(e)  => warning!("[{}]: console suppression failed — {}", MOD_NAME, e),
        }

        injection_utils::hooks::keyboard::install(
            injection_utils::hooks::keyboard::VK_F8,
            on_menu_open,
            on_menu_close,
        );
        debug!("[{}]: F8 keyboard hook installed", MOD_NAME);
    });
}

fn install_root() -> Option<PathBuf> {
    let mut p = std::env::current_exe().ok()?;
    p.pop(); // exe
    p.pop(); // Win64
    p.pop(); // Binaries
    p.pop(); // g3
    Some(p)
}

fn read_auth_result() -> (bool, bool, String) {
    let path = install_root()
        .map(|mut p| { p.push("Mods"); p.push("dlls"); p.push("auth_result.txt"); p });
    let Some(path) = path else { return (false, false, String::new()); };
    let Ok(contents) = std::fs::read_to_string(&path) else { return (false, false, String::new()); };

    let is_cheat = contents.contains("IS_CHEAT=true");
    let is_dev   = contents.contains("IS_DEV=true");
    let username = contents.lines()
        .find_map(|l| l.strip_prefix("USERNAME="))
        .unwrap_or("")
        .to_owned();
    (is_cheat, is_dev, username)
}

fn on_menu_open() {
    debug!("[{}]: F8 — showing dev menu", MOD_NAME);
    let (is_cheat, is_dev, username) = read_auth_result();
    window::set_staff(is_cheat, is_dev, &username);
    window::show();
}

fn on_menu_close() {
    debug!("[{}]: F8 — hiding dev menu", MOD_NAME);
    window::hide();
}
