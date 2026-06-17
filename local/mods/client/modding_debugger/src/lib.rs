use std::ffi::c_void;
use std::fs::OpenOptions;
use std::io::Write;

#[ctor::ctor]
fn dll_load_canary() {
    let _ = std::fs::write(
        "Z:\\var\\home\\doobs\\.local\\share\\Steam\\steamapps\\common\\Spellbreak\\Mods\\dlls\\md_ctor.txt",
        b"modding_debugger loaded",
    );
}

use game_base::*;
use ue_types::*;
use utils::{debug, logln};

static MOD_NAME: &'static str = "modding_debugger";

#[no_mangle]
fn mod_main(_base_addr: *const c_void) {
    // mod_main_sync was already called by load_sync_mods; don't reinitialise hooks
}

#[no_mangle]
fn mod_main_sync(base_addr: *const c_void) {
    unsafe { game_base::OFFSETS = game_base::offsets_client::get_offsets() };
    
    GameBase::initialize(MOD_NAME, base_addr);
    injection_utils::hooks::various::intercept().unwrap();

    std::thread::spawn(|| {
        std::thread::sleep(std::time::Duration::from_millis(10000));

        // Logs debug message to in-game console
        utils::log::set_print_to_console(Box::new(|msg| {
            let console = GameConsole::get();
            if console.is_some() { console.unwrap().output_text(msg) };
        }));
        debug!("Loading mod-debugger async code...");

        GObjects::filter(|obj| {
            if obj.class().is_some() && obj.class().unwrap().full_name().contains("ObjectLibrary") {
                debug!("OBJ FOUND: {:?} - {:?} - {:?}", Object::new(obj), obj.full_name(), obj.class().unwrap().full_name());
            }

            false
        });

        injection_utils::hooks::console::add_command_intercept(intercept_console_command).expect(format!("[{}]: Could not intercept Console Commands!", MOD_NAME).as_str());
    });
}

fn intercept_console_command(_console: Console, cmd: &FString) -> Result<bool, Box<dyn std::error::Error>> {
    let cmd_str = cmd.to_string().trim_end_matches([0x00 as char]).to_string();

    let should_forward = match (cmd_str.as_str(), cmd_str.split_once(" ")) {
        (_, Some(("start_game", _))) => {
            // MainMenu::connect_to_server("127.0.0.1", "7777", "");
            false
        },

        (_, Some(("withname", args))) => {
            find_objects_with_name(args);
            false
        },

        (_, Some(("withclass", args))) => {
            find_objects_with_class(args);
            false
        },

        // dumpmenu <label>
        // Dumps 512 raw bytes of the AGMainMenuGameMode object to
        // Mods/dlls/menu_dump_<label>.bin so we can diff two dumps
        // (e.g. after clicking Solos vs Squads) to find the mode field.
        (_, Some(("dumpmenu", label))) => {
            dump_menu_object(label);
            false
        },

        // modedump
        // Open the mode selection menu first, then run this.
        // Logs vtable entries of UGGameModeSelection and looks up any
        // registered "SelectMode"/"SetModeAndPop" UFunctions so we can
        // pin down the offset needed for the auth_injector hook.
        ("modedump", _) => {
            dump_mode_selection_funcs();
            false
        },

        (_, Some(("vftable", args))) => {
            let without_prefix = args.trim_start_matches("0x");
            let from_hex = usize::from_str_radix(without_prefix, 16)?;
            find_with_vf_table(from_hex as *const *const UnknownType);

            false
        }

        _ => true
    };

    Ok(should_forward)
}

fn find_with_vf_table(table_addr: *const *const UnknownType) {
    let g_objects = GObjects::objects::<UnknownType>();

    logln!("Finding objects with VFTable: {:p}", table_addr);
    let mut count: usize = 0;

    for i in 0..(g_objects.num_elements.to_native()-1) {
        match g_objects.item_at_idx(i as usize) {
            Some(item) => {
                match unsafe { (*item).object::<UObject<*const UnknownType>>() } {
                    Some(obj) => {
                        count += 1;
                        logln!("GOBJECTS[{:?}]: {:?}", i, obj.full_name());
                    }
                    _ => ()
                }
                
            }

            None => ()
        }
    }

    if count == 0 { logln!("No results found for VFTable: {:p}", table_addr); }
}

fn find_objects_with_class(args: &str) {
    let g_objects = GObjects::objects::<UnknownType>();

    let mut count: usize = 0;
    logln!("Finding objects with Class Name: {:?}", args);
    for i in 0..(g_objects.num_elements.to_native()-1) {
        let item = g_objects.item_at_idx(i as usize);
        if !item.is_some() { continue };

        let obj = unsafe { (*item.unwrap()).object::<UObject<*const UnknownType>>() };
        if !obj.is_some() { continue };

        let obj = obj.unwrap();

        match obj.class() {
            Some(class) => {
                if class.full_name().contains(args) {
                    logln!("GOBJECTS[{:?}]: {:?} ({:?})", i, obj.full_name(), obj.class().unwrap().full_name());
                    count += 1;
                }
            }

            None => ()
        }
        
        
    }
    if count == 0 { logln!("No results found for Class Name: {:?}", args); }
}

/// Dump 512 raw bytes starting at the AGMainMenuGameMode pointer to
/// Mods/dlls/menu_dump_<label>.bin.  Run with different labels after
/// clicking each mode button, then diff the files to find the mode field.
fn dump_menu_object(label: &str) {
    let game_mode_ptr = match MainMenu::game_mode() {
        Some(gm) => gm as *const AGMainMenuGameMode as *const u8,
        None => {
            logln!("dumpmenu: MainMenu::game_mode() returned None — are you in the main menu?");
            return;
        }
    };

    const DUMP_SIZE: usize = 2048;
    let bytes: &[u8] = unsafe { std::slice::from_raw_parts(game_mode_ptr, DUMP_SIZE) };

    // Build path: <exe>/../../../Mods/dlls/menu_dump_<label>.bin
    let path = {
        let mut p = match std::env::current_exe().ok() {
            Some(p) => p,
            None => {
                logln!("dumpmenu: could not get exe path");
                return;
            }
        };
        p.pop(); // Win64
        p.pop(); // Binaries
        p.pop(); // g3
        p.pop(); // Spellbreak install root
        p.push("Mods");
        p.push("dlls");
        p.push(format!("menu_dump_{}.bin", label));
        p
    };

    match OpenOptions::new().create(true).write(true).truncate(true).open(&path) {
        Ok(mut f) => {
            let _ = f.write_all(bytes);
            logln!("dumpmenu: wrote {} bytes to {:?}", DUMP_SIZE, path);
        }
        Err(e) => logln!("dumpmenu: failed to write {:?}: {}", path, e),
    }

    // Also dump the menu_manager object (pointer at offset 0x450 in AGMainMenuGameMode).
    // AGameMode=0x438, main_menu_instance=8, test_server_ip(FString)=16 → 0x450
    const MENU_MGR_OFFSET: usize = 0x450;
    let menu_mgr_ptr = unsafe {
        let ptr_slot = game_mode_ptr.add(MENU_MGR_OFFSET) as *const *const u8;
        *ptr_slot
    };
    if !menu_mgr_ptr.is_null() {
        let mgr_bytes: &[u8] = unsafe { std::slice::from_raw_parts(menu_mgr_ptr, 512) };
        let mgr_path = path.with_file_name(format!("menu_dump_{}_mgr.bin", label));
        if let Ok(mut f) = OpenOptions::new().create(true).write(true).truncate(true).open(&mgr_path) {
            let _ = f.write_all(mgr_bytes);
            logln!("dumpmenu: wrote 512 bytes of menu_manager to {:?}", mgr_path);
        }
    } else {
        logln!("dumpmenu: menu_manager pointer at 0x450 is null");
    }

    // Also log a hex preview of the first 64 bytes for quick inspection
    let hex: String = bytes[..64].chunks(16)
        .enumerate()
        .map(|(i, chunk)| {
            let hex_part: String = chunk.iter().map(|b| format!("{:02x} ", b)).collect();
            let asc_part: String = chunk.iter().map(|&b| if b >= 0x20 && b < 0x7f { b as char } else { '.' }).collect();
            format!("{:03x}: {:<48}  {}", i * 16, hex_part, asc_part)
        })
        .collect::<Vec<_>>()
        .join("\n");
    logln!("dumpmenu [{}] first 64 bytes:\n{}", label, hex);
}

/// Find UGGameModeSelection objects and dump their C++ vtable + any registered
/// SelectMode / SetModeAndPop UFunctions, with image-base-relative offsets.
/// Open the mode selection menu before running this command.
fn dump_mode_selection_funcs() {
    let base = GameBase::singleton().at_offset(0) as usize;
    logln!("Image base (runtime): {:#x}", base);

    logln!("=== GObjects scan: GameModeSelection ===");
    let g_objects = GObjects::objects::<UnknownType>();
    let mut gm_count: usize = 0;

    for i in 0..(g_objects.num_elements.to_native() - 1) {
        let item = g_objects.item_at_idx(i as usize);
        let obj = match item {
            Some(it) => match unsafe { (*it).object::<UObject<*const UnknownType>>() } {
                Some(o) => o,
                None => continue,
            },
            None => continue,
        };

        let full = obj.full_name();
        let class_str = obj.class().map(|c| c.full_name()).unwrap_or_default();

        // Objects whose class name contains GameModeSelection (the widget instances)
        if class_str.contains("GameModeSelection") {
            gm_count += 1;
            logln!("  [{}] {}", i, full);
            logln!("       class={}", class_str);
            logln!("       addr={:p}", obj as *const _);

            // Read the C++ vtable pointer stored at offset 0 of the object.
            let vtbl_ptr: *const *const u8 =
                unsafe { *(obj as *const UObject<*const UnknownType> as *const *const *const u8) };
            logln!("       vtable={:p}", vtbl_ptr);

            for slot in 0usize..64 {
                let fn_ptr = unsafe { *vtbl_ptr.add(slot) } as usize;
                if fn_ptr == 0 { break; }
                let offset = fn_ptr.wrapping_sub(base);
                let bytes: String = unsafe {
                    std::slice::from_raw_parts(fn_ptr as *const u8, 8)
                }
                .iter()
                .map(|b| format!("{:02x}", b))
                .collect::<Vec<_>>()
                .join(" ");
                logln!("       vt[{:2}] off={:#010x}  {}", slot, offset, bytes);
            }
        }

        // UFunction objects named SelectMode or SetModeAndPop
        if class_str.contains("Function")
            && (full.contains("SelectMode") || full.contains("SetModeAndPop")
                || full.contains("GameModeSelection"))
        {
            logln!("  UFunc [{}] {}", i, full);
            // UFunction::Func is at offset 0xC8 from UObject base (UE4 4.2x layout)
            let func_ptr_slot =
                unsafe { (obj as *const UObject<*const UnknownType> as *const u8).add(0xC8) }
                    as *const *const u8;
            let func_ptr = unsafe { *func_ptr_slot } as usize;
            let offset = func_ptr.wrapping_sub(base);
            let bytes: String = if func_ptr > 0x10000 {
                unsafe { std::slice::from_raw_parts(func_ptr as *const u8, 8) }
                    .iter()
                    .map(|b| format!("{:02x}", b))
                    .collect::<Vec<_>>()
                    .join(" ")
            } else {
                "null".to_string()
            };
            logln!("         Func={:#x} off={:#010x}  {}", func_ptr, offset, bytes);
        }
    }

    if gm_count == 0 {
        logln!("  No GameModeSelection objects found — open the mode selection menu first");
    }
}

fn find_objects_with_name(args: &str) {
    let g_objects = GObjects::objects::<UnknownType>();

    let mut count: usize = 0;
    logln!("Finding objects with Class Name: {:?}", args);
    for i in 0..(g_objects.num_elements.to_native()-1) {
        let item = g_objects.item_at_idx(i as usize);
        if !item.is_some() { continue };

        let obj = unsafe { (*item.unwrap()).object::<UObject<*const UnknownType>>() };
        if !obj.is_some() { continue };

        let obj = obj.unwrap();
        
        if obj.full_name().contains(args) {
            let class_name = match obj.class() {
                Some(class) => class.full_name(),
                None => "?".to_string()
            };

            logln!("GOBJECTS[{:?}]: {:?} ({:?})", i, obj.full_name(), class_name);
            count += 1;
        }
    }
    if count == 0 { logln!("No results found for Object Name: {:?}", args); }
}