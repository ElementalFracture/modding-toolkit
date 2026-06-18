use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use ue_types::{FName, UConsole};
use game_base::GameConsole;
use utils::debug;

type ConsoleOpenShiv  = fn();
type ConsoleCloseShiv = fn();

static mut OPEN_SHIV:  Option<ConsoleOpenShiv>  = None;
static mut CLOSE_SHIV: Option<ConsoleCloseShiv> = None;
static IS_QT_OPEN: AtomicBool  = AtomicBool::new(false);
static INSTALLED:  AtomicBool  = AtomicBool::new(false);

// FName comparison indices for the three states we care about, populated at
// install time by calling the game's own FName constructor.  The hook compares
// against these directly — no string conversion inside the hot path.
static TYPING_IDX: AtomicU32 = AtomicU32::new(u32::MAX);
static OPEN_IDX:   AtomicU32 = AtomicU32::new(u32::MAX);
static NONE_IDX:   AtomicU32 = AtomicU32::new(u32::MAX);

const INSTALLED_PATH: &str =
    "Z:\\var\\home\\doobs\\.local\\share\\Steam\\steamapps\\common\\Spellbreak\\Mods\\dlls\\fgs_hook_installed.txt";

pub fn intercept_console_open(
    on_open:  ConsoleOpenShiv,
    on_close: Option<ConsoleCloseShiv>,
) -> Result<(), Box<dyn std::error::Error>> {
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

        OPEN_SHIV  = Some(on_open);
        CLOSE_SHIV = on_close;

        *slot_ptr = hook_fake_goto_state as *mut u8;
        INSTALLED.store(true, Ordering::SeqCst);

        let mut dummy = 0u32;
        VirtualProtect(slot_ptr as *mut _, 8, old_prot, &mut dummy);
    }

    // Pre-cache FName comparison indices — must happen after INSTALLED is set
    // (i.e. after the game's name system is ready) but outside the hot path.
    cache_fname_indices();

    let ti = TYPING_IDX.load(Ordering::Relaxed);
    let oi = OPEN_IDX.load(Ordering::Relaxed);
    let ni = NONE_IDX.load(Ordering::Relaxed);
    debug!("[console_open]: slot {} (orig={:#x}) typing={} open={} none={}", slot, orig_fn, ti, oi, ni);
    let _ = std::fs::write(
        INSTALLED_PATH,
        format!("slot={} orig={:#x} typing_idx={} open_idx={} none_idx={}\n", slot, orig_fn, ti, oi, ni).as_bytes(),
    );

    Ok(())
}

/// Populate the three FName comparison indices by calling the game's own
/// FName(TCHAR*, EFindName) constructor (FNAME_Add = 1).
fn cache_fname_indices() {
    // MSVC x64 member function: RCX=this(*mut FName), RDX=name(*const u16), R8=find_type(i32)
    type FNameCtor = unsafe extern "C" fn(this: *mut FName, name: *const u16, find_type: i32);

    let ctor: FNameCtor = unsafe {
        std::mem::transmute(
            game_base::GameBase::singleton()
                .at_offset(game_base::OFFSETS.base_funcs.fname_str_constructor),
        )
    };

    for (name, slot) in &[
        ("Typing", &TYPING_IDX),
        ("Open",   &OPEN_IDX),
        ("None",   &NONE_IDX),
    ] {
        let wide: Vec<u16> = name.encode_utf16().chain(std::iter::once(0u16)).collect();
        let mut fname = FName { comparison_idx: 0u32.into(), display_idx: 0u32.into(), number: 0u32.into() };
        unsafe { ctor(&mut fname, wide.as_ptr(), 1); }
        slot.store(fname.comparison_idx.to_native(), Ordering::SeqCst);
    }
}

/// Called in place of UConsole::FakeGotoState.
/// Compares by pre-cached FName index — no game string calls on the hot path.
unsafe extern "C" fn hook_fake_goto_state(_this: *const UConsole, next_state: *const FName) {
    if next_state.is_null() { return; }
    let idx        = (*next_state).comparison_idx.to_native();
    let typing_idx = TYPING_IDX.load(Ordering::Relaxed);
    let open_idx   = OPEN_IDX.load(Ordering::Relaxed);
    let none_idx   = NONE_IDX.load(Ordering::Relaxed);

    if idx == typing_idx || idx == open_idx {
        IS_QT_OPEN.store(true, Ordering::SeqCst);
        if let Some(shiv) = OPEN_SHIV { shiv(); }
        // Do not call original — native console stays hidden.
    } else if idx == none_idx && IS_QT_OPEN.load(Ordering::SeqCst) {
        IS_QT_OPEN.store(false, Ordering::SeqCst);
        if let Some(shiv) = CLOSE_SHIV { shiv(); }
        // Do not call original — we own the lifecycle.
    } else {
        // All other state transitions (game-menu navigation, HUD states, etc.)
        // are silently ignored.  Calling the original crashes, likely due to a
        // re-entrancy or subclass calling-convention issue specific to this build.
        // The toggle approach confirmed that skipping these calls is safe.
    }
}
