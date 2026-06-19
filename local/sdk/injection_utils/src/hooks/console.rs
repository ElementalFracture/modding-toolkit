use std::sync::OnceLock;
use ue_types::{FString, UConsole};
use game_base::{Console, GameBase, GameConsole};
use utils::{debug, warning};

type RawConsoleFn = unsafe extern "C" fn(*const UConsole, *const FString);
pub type CommandInterceptShiv = fn(Console, &FString) -> Result<bool, Box<dyn std::error::Error>>;

static CMD_SHIV: OnceLock<CommandInterceptShiv> = OnceLock::new();
static ORIG_CMD:  OnceLock<RawConsoleFn>         = OnceLock::new();

// UConsoleVFTable::console_command byte offset (vtable slot 73 = 0x248 / 8).
const CONSOLE_CMD_VT_OFFSET: isize = 0x248;

/// Patch the UConsole vtable slot for ConsoleCommand via VirtualProtect.
/// Each mod DLL carries its own copy of this function and the two statics, so
/// multiple mods chaining through the same slot is safe — each saves the
/// previous slot value as its original and the calls naturally chain.
pub fn add_command_intercept(
    shiv_fn: CommandInterceptShiv,
) -> Result<(), Box<dyn std::error::Error>> {
    GameConsole::wait_for_loaded();
    let console = GameConsole::get().ok_or("UConsole not found in GObjects")?;

    let _ = CMD_SHIV.set(shiv_fn);

    unsafe {
        let console_raw: *const u8 = std::mem::transmute(console);
        let vtbl: *mut *mut u8     = *(console_raw as *const *mut *mut u8);
        let slot                   = (CONSOLE_CMD_VT_OFFSET / 8) as usize;
        let slot_ptr               = vtbl.add(slot) as *mut *mut u8;

        let orig: RawConsoleFn = std::mem::transmute(*slot_ptr);
        let _ = ORIG_CMD.set(orig);

        #[cfg(windows)]
        {
            use winapi::um::memoryapi::VirtualProtect;
            use winapi::um::winnt::PAGE_READWRITE;
            let mut old_prot = 0u32;
            if VirtualProtect(slot_ptr as *mut _, 8, PAGE_READWRITE, &mut old_prot) == 0 {
                return Err("VirtualProtect(RW) failed for console_command slot".into());
            }
            *slot_ptr = hooked_console_command as *mut u8;
            let mut dummy = 0u32;
            VirtualProtect(slot_ptr as *mut _, 8, old_prot, &mut dummy);
        }
    }

    debug!(
        "[{}]: console command intercept installed (vtable slot 73)",
        GameBase::singleton().mod_name
    );
    Ok(())
}

unsafe extern "C" fn hooked_console_command(console: *const UConsole, cmd: *const FString) {
    let call_orig = || {
        if let Some(orig) = ORIG_CMD.get() {
            orig(console, cmd);
        }
    };

    let shiv = match CMD_SHIV.get() {
        Some(s) => s,
        None => { call_orig(); return; }
    };

    let cmd_ref = match cmd.as_ref() {
        Some(r) => r,
        None    => { call_orig(); return; }
    };

    match (*shiv)(Console::new(console), cmd_ref) {
        Ok(true)  => call_orig(),
        Ok(false) => {}
        Err(err)  => {
            warning!(
                "[{}]: console intercept error: {:?}",
                GameBase::singleton().mod_name,
                err
            );
            call_orig();
        }
    }
}
