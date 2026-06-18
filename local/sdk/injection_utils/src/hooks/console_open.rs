use std::sync::atomic::{AtomicBool, Ordering};
use ue_types::UConsole;
use game_base::GameConsole;
use utils::debug;

static INSTALLED: AtomicBool = AtomicBool::new(false);

const INSTALLED_PATH: &str =
    "Z:\\var\\home\\doobs\\.local\\share\\Steam\\steamapps\\common\\Spellbreak\\Mods\\dlls\\fgs_hook_installed.txt";

/// Patch UConsole vtable slot 94 (FakeGotoState) so the native UE4 console
/// can never open. The dev menu is triggered independently via a keyboard hook.
pub fn suppress_native_console() -> Result<(), Box<dyn std::error::Error>> {
    if INSTALLED.load(Ordering::SeqCst) {
        return Ok(());
    }

    let vtable_offset = unsafe { game_base::OFFSETS.vf_tables.console_fake_goto_state };
    if vtable_offset == 0 {
        return Err("console_fake_goto_state offset is 0".into());
    }

    GameConsole::wait_for_loaded();
    let console = GameConsole::get().ok_or("UConsole not found in GObjects")?;

    let console_raw: *const u8 = unsafe { std::mem::transmute(console) };
    let vtbl: *mut *mut u8 = unsafe { *(console_raw as *const *mut *mut u8) };
    let slot     = (vtable_offset / 8) as usize;
    let slot_ptr = unsafe { vtbl.add(slot) } as *mut *mut u8;
    let orig_fn  = unsafe { *slot_ptr } as usize;

    #[cfg(windows)]
    unsafe {
        use winapi::um::memoryapi::VirtualProtect;
        use winapi::um::winnt::PAGE_READWRITE;

        let mut old_prot = 0u32;
        if VirtualProtect(slot_ptr as *mut _, 8, PAGE_READWRITE, &mut old_prot) == 0 {
            return Err("VirtualProtect(RW) failed".into());
        }
        *slot_ptr = hook_fake_goto_state as *mut u8;
        INSTALLED.store(true, Ordering::SeqCst);
        let mut dummy = 0u32;
        VirtualProtect(slot_ptr as *mut _, 8, old_prot, &mut dummy);
    }

    debug!("[console_open]: slot {} (orig={:#x}) native console suppressed", slot, orig_fn);
    let _ = std::fs::write(
        INSTALLED_PATH,
        format!("slot={} orig={:#x}\n", slot, orig_fn).as_bytes(),
    );

    Ok(())
}

/// Replaces UConsole::FakeGotoState. All calls are dropped — the native console
/// is permanently disabled. Calling the original crashes (re-entrancy in this
/// build), and ignoring unknown states is safe (confirmed during discovery).
unsafe extern "C" fn hook_fake_goto_state(_this: *const UConsole, _next_state: *const u8) {}
