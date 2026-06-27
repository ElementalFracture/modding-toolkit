/// ImGui overlay bridge — loads `devmenu_imgui.dll` at runtime and delegates
/// all GUI work to it via a stable C ABI.
///
/// The DLL must export these symbols with C linkage:
///
///   void devmenu_init()
///       Called once on mod load from a background Rust thread.  Hooks the
///       game's IDXGISwapChain::Present vtable so the overlay renders inside
///       the game's own frame.  The window starts hidden.
///
///   void devmenu_show() / devmenu_hide()
///       Toggle overlay visibility.  Thread-safe (atomic flag).
///
///   void devmenu_set_staff(int is_staff, int is_dev, const char *username)
///       Update auth state.  is_staff enables Staff Commands panel;
///       is_dev enables Dev Settings panel.
///
///   void devmenu_set_command_callback(fn(cmd: *const u16, len: i32))
///       Register the Rust command dispatcher.  Invoked on the render thread
///       when a command is submitted from the overlay.
///
///   void devmenu_get_menu_key(unsigned *vk, unsigned *mods)
///       Returns the currently configured menu toggle key (defaults to F8).
///
///   void devmenu_set_menu_key_callback(fn(vk: u32, mods: u32))
///       Register a callback fired whenever the user rebinds the menu key.

type VoidFn           = unsafe extern "C" fn();
type SetStaffFn       = unsafe extern "C" fn(i32, i32, *const i8);
type SetCallbackFn    = unsafe extern "C" fn(CommandDispatchFn);
type GetMenuKeyFn     = unsafe extern "C" fn(*mut u32, *mut u32);
type SetMenuKeyCbFn   = unsafe extern "C" fn(MenuKeyChangedFn);
/// UTF-16 command text dispatched from the overlay to the Rust side.
type CommandDispatchFn  = unsafe extern "C" fn(cmd: *const u16, len: i32);
/// Fired by devmenu_imgui when the user rebinds the menu toggle key.
pub type MenuKeyChangedFn = unsafe extern "C" fn(vk: u32, mods: u32);

struct QtBridge {
    #[cfg(windows)]
    _lib: winapi::shared::minwindef::HMODULE,
    show:            VoidFn,
    hide:            VoidFn,
    set_staff:       SetStaffFn,
    get_menu_key:    GetMenuKeyFn,
    set_menu_key_cb: SetMenuKeyCbFn,
}

// SAFETY: written once during init, then read-only.
unsafe impl Send for QtBridge {}
unsafe impl Sync for QtBridge {}

static BRIDGE: std::sync::OnceLock<Option<QtBridge>> = std::sync::OnceLock::new();

/// Load `devmenu_imgui.dll`, call `devmenu_init`, and register callbacks.
/// Returns `true` if successful.
pub fn init() -> bool {
    BRIDGE.get_or_init(load_bridge).is_some()
}

pub fn show() {
    if let Some(Some(b)) = BRIDGE.get() {
        unsafe { (b.show)(); }
    }
}

pub fn hide() {
    if let Some(Some(b)) = BRIDGE.get() {
        unsafe { (b.hide)(); }
    }
}

pub fn set_staff(is_staff: bool, is_dev: bool, username: &str) {
    if let Some(Some(b)) = BRIDGE.get() {
        let mut cstr: Vec<i8> = username.bytes().map(|b| b as i8).collect();
        cstr.push(0);
        unsafe {
            (b.set_staff)(
                if is_staff { 1 } else { 0 },
                if is_dev   { 1 } else { 0 },
                cstr.as_ptr(),
            );
        }
    }
}

/// Returns `(vk_code, modifier_mask)` for the menu toggle key.
/// Modifier mask: bit 0 = Shift, bit 1 = Ctrl, bit 2 = Alt.
pub fn get_menu_key() -> (u32, u32) {
    if let Some(Some(b)) = BRIDGE.get() {
        let mut vk: u32 = 0;
        let mut mods: u32 = 0;
        unsafe { (b.get_menu_key)(&mut vk, &mut mods); }
        return (vk, mods);
    }
    (0x77, 0) // F8 default when bridge absent
}

/// Register the callback that fires when the user rebinds the menu key.
pub fn set_menu_key_callback(cb: MenuKeyChangedFn) {
    if let Some(Some(b)) = BRIDGE.get() {
        unsafe { (b.set_menu_key_cb)(cb); }
    }
}

/// Called from the render thread when the player submits a command.
/// Forwards the UTF-16 text to the UE4 console.
unsafe extern "C" fn command_dispatch(cmd: *const u16, len: i32) {
    if cmd.is_null() || len <= 0 {
        return;
    }
    let slice = std::slice::from_raw_parts(cmd, len as usize);
    let s = String::from_utf16_lossy(slice);
    if let Some(console) = game_base::GameConsole::get() {
        console.console_command(&s);
    }
}

#[cfg(windows)]
fn load_bridge() -> Option<QtBridge> {
    use winapi::um::libloaderapi::{GetProcAddress, LoadLibraryA};

    unsafe {
        let handle = LoadLibraryA(b"devmenu_imgui.dll\0".as_ptr() as *const i8);
        if handle.is_null() {
            utils::warning!("[qt_devmenu]: devmenu_imgui.dll not found — overlay disabled");
            return None;
        }

        macro_rules! sym {
            ($name:literal) => {{
                let p = GetProcAddress(handle, concat!($name, "\0").as_ptr() as *const i8);
                if p.is_null() {
                    utils::warning!(
                        "[qt_devmenu]: devmenu_imgui.dll is missing export `{}`",
                        $name
                    );
                    return None;
                }
                p
            }};
        }

        let init_fn:          VoidFn         = std::mem::transmute(sym!("devmenu_init"));
        let show_fn:          VoidFn         = std::mem::transmute(sym!("devmenu_show"));
        let hide_fn:          VoidFn         = std::mem::transmute(sym!("devmenu_hide"));
        let set_staff_fn:     SetStaffFn     = std::mem::transmute(sym!("devmenu_set_staff"));
        let set_cb_fn:        SetCallbackFn  = std::mem::transmute(sym!("devmenu_set_command_callback"));
        let get_menu_key_fn:  GetMenuKeyFn   = std::mem::transmute(sym!("devmenu_get_menu_key"));
        let set_mk_cb_fn:     SetMenuKeyCbFn = std::mem::transmute(sym!("devmenu_set_menu_key_callback"));

        // Hook the DXGI Present vtable; returns once the hook is installed.
        (init_fn)();

        // Register the Rust command dispatcher before any show() can be called.
        (set_cb_fn)(command_dispatch);

        Some(QtBridge {
            _lib: handle,
            show: show_fn,
            hide: hide_fn,
            set_staff: set_staff_fn,
            get_menu_key: get_menu_key_fn,
            set_menu_key_cb: set_mk_cb_fn,
        })
    }
}

#[cfg(not(windows))]
fn load_bridge() -> Option<QtBridge> {
    None
}
