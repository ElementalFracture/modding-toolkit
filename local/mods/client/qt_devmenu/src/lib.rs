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
            warning!("[{}]: devmenu_imgui.dll absent — menu key opens nothing", MOD_NAME);
        }

        match injection_utils::hooks::console_open::suppress_native_console() {
            Ok(())  => debug!("[{}]: native console suppressed", MOD_NAME),
            Err(e)  => warning!("[{}]: console suppression failed — {}", MOD_NAME, e),
        }

        // Prime auth at game start: triggers the connection toast and arms the
        // teleport block before the user ever opens the menu.
        {
            let (is_cheat, is_dev, username) = read_auth_result();
            injection_utils::hooks::keyboard::set_block_teleport(!is_cheat && !is_dev);
            window::set_staff(is_cheat, is_dev, &username);
        }

        let (vk, mods) = window::get_menu_key();
        window::set_menu_key_callback(on_menu_key_changed);
        injection_utils::hooks::keyboard::install(vk, mods, on_menu_open, on_menu_close);
        debug!("[{}]: keyboard hook installed (vk=0x{:02X} mods={})", MOD_NAME, vk, mods);
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

/// Called by devmenu_imgui.dll (on the render thread) when the user rebinds
/// the menu toggle key.  Updates the low-level keyboard hook atomically.
unsafe extern "C" fn on_menu_key_changed(vk: u32, mods: u32) {
    injection_utils::hooks::keyboard::update_menu_key(vk, mods);
    debug!("[{}]: menu key updated (vk=0x{:02X} mods={})", MOD_NAME, vk, mods);
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
    debug!("[{}]: showing dev menu", MOD_NAME);
    let (is_cheat, is_dev, username) = read_auth_result();
    injection_utils::hooks::keyboard::set_block_teleport(!is_cheat && !is_dev);
    window::set_staff(is_cheat, is_dev, &username);
    window::show();
}

fn on_menu_close() {
    debug!("[{}]: hiding dev menu", MOD_NAME);
    window::hide();
}
