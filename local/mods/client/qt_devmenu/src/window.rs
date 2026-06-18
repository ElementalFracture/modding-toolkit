/// Qt window bridge — loads `devmenu_qt.dll` at runtime and delegates all
/// GUI work to it via a stable C ABI.
///
/// The DLL must export four symbols with C linkage:
///
///   void devmenu_init()
///       Called once on mod load from a background Rust thread.  Blocks until
///       QApplication + DevMenuWindow are ready, then returns.  Internally it
///       spawns a dedicated OS thread, creates QApplication + DevMenuWindow
///       there, and calls QApplication::exec() on that thread.  The window
///       starts hidden.
///
///   void devmenu_show()
///       Make the window visible and raise it.  Thread-safe: uses
///       QMetaObject::invokeMethod with Qt::QueuedConnection.
///
///   void devmenu_hide()
///       Hide the window.  Same thread-safety guarantee as devmenu_show.
///
///   void devmenu_set_command_callback(fn(cmd: *const u16, len: i32))
///       Register the Rust command dispatcher.  Called once immediately after
///       devmenu_init returns.  When the player submits a command in the window,
///       the callback is invoked on the Qt thread with the UTF-16 command text.

type VoidFn       = unsafe extern "C" fn();
type SetCallbackFn = unsafe extern "C" fn(CommandDispatchFn);
/// UTF-16 command text dispatched from the Qt window to the Rust side.
type CommandDispatchFn = unsafe extern "C" fn(cmd: *const u16, len: i32);

struct QtBridge {
    #[cfg(windows)]
    _lib: winapi::shared::minwindef::HMODULE,
    show: VoidFn,
    hide: VoidFn,
}

// SAFETY: written once during init, then read-only.
unsafe impl Send for QtBridge {}
unsafe impl Sync for QtBridge {}

static BRIDGE: std::sync::OnceLock<Option<QtBridge>> = std::sync::OnceLock::new();

/// Load `devmenu_qt.dll`, call `devmenu_init`, and register the command
/// callback.  Returns `true` if successful.
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

/// Called from the Qt thread when the player submits a command.
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
        let handle = LoadLibraryA(b"devmenu_qt.dll\0".as_ptr() as *const i8);
        if handle.is_null() {
            utils::warning!("[qt_devmenu]: devmenu_qt.dll not found — Qt window disabled");
            return None;
        }

        macro_rules! sym {
            ($name:literal) => {{
                let p = GetProcAddress(handle, concat!($name, "\0").as_ptr() as *const i8);
                if p.is_null() {
                    utils::warning!(
                        "[qt_devmenu]: devmenu_qt.dll is missing export `{}`",
                        $name
                    );
                    return None;
                }
                p
            }};
        }

        let init_fn:    VoidFn        = std::mem::transmute(sym!("devmenu_init"));
        let show_fn:    VoidFn        = std::mem::transmute(sym!("devmenu_show"));
        let hide_fn:    VoidFn        = std::mem::transmute(sym!("devmenu_hide"));
        let set_cb_fn:  SetCallbackFn = std::mem::transmute(sym!("devmenu_set_command_callback"));

        // Spin up the Qt event loop; blocks until QApplication + window are ready.
        (init_fn)();

        // Register the Rust command dispatcher before any show() can be called.
        (set_cb_fn)(command_dispatch);

        Some(QtBridge { _lib: handle, show: show_fn, hide: hide_fn })
    }
}

#[cfg(not(windows))]
fn load_bridge() -> Option<QtBridge> {
    None
}
