use std::ffi::c_void;
use std::fs::OpenOptions;
use std::io::Write;
use std::sync::OnceLock;

use game_base::*;
use ue_types::*;
use utils::{debug, logln};

// Prevent double-init when both client_mod_loader phases run mod_main_sync.
static INIT_ONCE: OnceLock<()> = OnceLock::new();

static MOD_NAME: &str = "modding_debugger";

fn canary_path() -> std::path::PathBuf {
    let mut p = std::env::current_exe().unwrap_or_default();
    p.pop(); // Win64
    p.pop(); // Binaries
    p.pop(); // g3
    p.pop(); // install root
    p.push("Mods");
    p.push("dlls");
    p.push("md_ctor.txt");
    p
}

#[no_mangle]
fn mod_main(_base_addr: *const c_void) {
    // mod_main_sync is called directly by client_mod_loader Phase 1.
    // mod_main (Phase 2) has nothing additional to do.
}

#[no_mangle]
fn mod_main_sync(base_addr: *const c_void) {
    if INIT_ONCE.set(()).is_err() {
        return; // already initialised
    }

    unsafe { game_base::OFFSETS = game_base::offsets_client::get_offsets() };
    GameBase::initialize(MOD_NAME, base_addr);

    // Write canary now that we're past DllMain (safe to do file I/O here).
    let _ = std::fs::write(canary_path(), b"modding_debugger loaded\n");

    std::thread::spawn(|| {
        std::thread::sleep(std::time::Duration::from_millis(10_000));

        utils::log::set_print_to_console(Box::new(|msg| {
            if let Some(console) = GameConsole::get() {
                console.output_text(msg);
            }
        }));
        debug!("modding_debugger async init complete");

        GObjects::filter(|obj| {
            if obj.class().is_some()
                && obj.class().unwrap().full_name().contains("ObjectLibrary")
            {
                debug!(
                    "OBJ FOUND: {:?} - {:?} - {:?}",
                    Object::new(obj),
                    obj.full_name(),
                    obj.class().unwrap().full_name()
                );
            }
            false
        });

        injection_utils::hooks::console::add_command_intercept(intercept_console_command)
            .expect("[modding_debugger]: Could not intercept console commands");
    });
}

fn intercept_console_command(
    _console: Console,
    cmd: &FString,
) -> Result<bool, Box<dyn std::error::Error>> {
    let cmd_str = cmd
        .to_string()
        .trim_end_matches('\0')
        .to_string();

    let should_forward = match (cmd_str.as_str(), cmd_str.split_once(' ')) {
        (_, Some(("start_game", _))) => false,

        (_, Some(("withname", args))) => { find_objects_with_name(args); false }
        (_, Some(("withclass", args))) => { find_objects_with_class(args); false }

        // dumpmenu <label> — dump 2 KB of AGMainMenuGameMode to a .bin file.
        (_, Some(("dumpmenu", label))) => { dump_menu_object(label); false }

        // modedump — log vtable + UFunctions for UGGameModeSelection.
        ("modedump", _) => { dump_mode_selection_funcs(); false }

        (_, Some(("vftable", args))) => {
            let addr = usize::from_str_radix(args.trim_start_matches("0x"), 16)?;
            find_with_vf_table(addr as *const *const UnknownType);
            false
        }

        _ => true,
    };

    Ok(should_forward)
}

fn find_with_vf_table(table_addr: *const *const UnknownType) {
    let g_objects = GObjects::objects::<UnknownType>();
    logln!("Finding objects with VFTable: {:p}", table_addr);
    let mut count = 0usize;
    for i in 0..(g_objects.num_elements.to_native() - 1) {
        if let Some(item) = g_objects.item_at_idx(i as usize) {
            if let Some(obj) = unsafe { (*item).object::<UObject<*const UnknownType>>() } {
                count += 1;
                logln!("GOBJECTS[{}]: {:?}", i, obj.full_name());
            }
        }
    }
    if count == 0 { logln!("No results found for VFTable: {:p}", table_addr); }
}

fn find_objects_with_class(args: &str) {
    let g_objects = GObjects::objects::<UnknownType>();
    let mut count = 0usize;
    logln!("Finding objects with class: {:?}", args);
    for i in 0..(g_objects.num_elements.to_native() - 1) {
        let item = match g_objects.item_at_idx(i as usize) { Some(x) => x, None => continue };
        let obj = match unsafe { (*item).object::<UObject<*const UnknownType>>() } { Some(x) => x, None => continue };
        if let Some(class) = obj.class() {
            if class.full_name().contains(args) {
                logln!("GOBJECTS[{}]: {:?} ({:?})", i, obj.full_name(), class.full_name());
                count += 1;
            }
        }
    }
    if count == 0 { logln!("No results for class: {:?}", args); }
}

fn dump_menu_object(label: &str) {
    let game_mode_ptr = match MainMenu::game_mode() {
        Some(gm) => gm as *const AGMainMenuGameMode as *const u8,
        None => {
            logln!("dumpmenu: MainMenu::game_mode() returned None — open main menu first");
            return;
        }
    };

    const DUMP_SIZE: usize = 2048;
    let bytes: &[u8] = unsafe { std::slice::from_raw_parts(game_mode_ptr, DUMP_SIZE) };

    let path = {
        let mut p = match std::env::current_exe().ok() { Some(p) => p, None => return };
        p.pop(); p.pop(); p.pop(); p.pop();
        p.push("Mods"); p.push("dlls");
        p.push(format!("menu_dump_{}.bin", label));
        p
    };

    if let Ok(mut f) = OpenOptions::new().create(true).write(true).truncate(true).open(&path) {
        let _ = f.write_all(bytes);
        logln!("dumpmenu: wrote {} bytes to {:?}", DUMP_SIZE, path);
    }

    const MENU_MGR_OFFSET: usize = 0x450;
    let menu_mgr_ptr = unsafe {
        let slot = game_mode_ptr.add(MENU_MGR_OFFSET) as *const *const u8;
        *slot
    };
    if !menu_mgr_ptr.is_null() {
        let mgr_bytes: &[u8] = unsafe { std::slice::from_raw_parts(menu_mgr_ptr, 512) };
        let mgr_path = path.with_file_name(format!("menu_dump_{}_mgr.bin", label));
        if let Ok(mut f) = OpenOptions::new().create(true).write(true).truncate(true).open(&mgr_path) {
            let _ = f.write_all(mgr_bytes);
            logln!("dumpmenu: wrote 512 bytes of menu_manager to {:?}", mgr_path);
        }
    }

    let hex: String = bytes[..64].chunks(16).enumerate()
        .map(|(i, chunk)| {
            let hex_part: String = chunk.iter().map(|b| format!("{:02x} ", b)).collect();
            let asc_part: String = chunk.iter().map(|&b| if b >= 0x20 && b < 0x7f { b as char } else { '.' }).collect();
            format!("{:03x}: {:<48}  {}", i * 16, hex_part, asc_part)
        })
        .collect::<Vec<_>>().join("\n");
    logln!("dumpmenu [{}] first 64 bytes:\n{}", label, hex);
}

fn dump_mode_selection_funcs() {
    let base = GameBase::singleton().at_offset(0) as usize;
    logln!("Image base: {:#x}", base);
    logln!("=== GObjects scan: GameModeSelection ===");
    let g_objects = GObjects::objects::<UnknownType>();
    let mut gm_count = 0usize;

    for i in 0..(g_objects.num_elements.to_native() - 1) {
        let item = match g_objects.item_at_idx(i as usize) { Some(x) => x, None => continue };
        let obj = match unsafe { (*item).object::<UObject<*const UnknownType>>() } { Some(x) => x, None => continue };
        let full = obj.full_name();
        let class_str = obj.class().map(|c| c.full_name()).unwrap_or_default();

        if class_str.contains("GameModeSelection") {
            gm_count += 1;
            logln!("  [{}] {} class={} addr={:p}", i, full, class_str, obj as *const _);
            let vtbl_ptr: *const *const u8 = unsafe {
                *(obj as *const UObject<*const UnknownType> as *const *const *const u8)
            };
            logln!("       vtable={:p}", vtbl_ptr);
            for slot in 0usize..64 {
                let fn_ptr = unsafe { *vtbl_ptr.add(slot) } as usize;
                if fn_ptr == 0 { break; }
                let offset = fn_ptr.wrapping_sub(base);
                let bytes: String = unsafe { std::slice::from_raw_parts(fn_ptr as *const u8, 8) }
                    .iter().map(|b| format!("{:02x}", b)).collect::<Vec<_>>().join(" ");
                logln!("       vt[{:2}] off={:#010x}  {}", slot, offset, bytes);
            }
        }

        if class_str.contains("Function")
            && (full.contains("SelectMode") || full.contains("SetModeAndPop") || full.contains("GameModeSelection"))
        {
            logln!("  UFunc [{}] {}", i, full);
            let func_ptr_slot = unsafe {
                (obj as *const UObject<*const UnknownType> as *const u8).add(0xC8) as *const *const u8
            };
            let func_ptr = unsafe { *func_ptr_slot } as usize;
            let offset = func_ptr.wrapping_sub(base);
            logln!("         Func={:#x} off={:#010x}", func_ptr, offset);
        }
    }

    if gm_count == 0 {
        logln!("No GameModeSelection objects found — open the mode selection menu first");
    }
}


fn find_objects_with_name(args: &str) {
    let g_objects = GObjects::objects::<UnknownType>();
    let mut count = 0usize;
    logln!("Finding objects with name: {:?}", args);
    for i in 0..(g_objects.num_elements.to_native() - 1) {
        let item = match g_objects.item_at_idx(i as usize) { Some(x) => x, None => continue };
        let obj = match unsafe { (*item).object::<UObject<*const UnknownType>>() } { Some(x) => x, None => continue };
        if obj.full_name().contains(args) {
            let class_name = obj.class().map(|c| c.full_name()).unwrap_or_else(|| "?".into());
            logln!("GOBJECTS[{}]: {:?} ({:?})", i, obj.full_name(), class_name);
            count += 1;
        }
    }
    if count == 0 { logln!("No results for name: {:?}", args); }
}
