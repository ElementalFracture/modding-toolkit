#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Called once from the Rust mod's background thread, immediately after
 * LoadLibraryA succeeds.  MUST return immediately.
 *
 * Internally: spawn a dedicated OS thread, create QApplication + DevMenuWindow
 * there, and call QApplication::exec() on that thread.  The window starts
 * hidden and stays hidden until devmenu_show() is called.
 */
__declspec(dllexport) void devmenu_init(void);

/**
 * Make the dev-menu window visible and raise it to the front.
 * Thread-safe: may be called from any thread (the UE4 hook fires on the
 * game's main thread).  Uses QMetaObject::invokeMethod with QueuedConnection.
 */
__declspec(dllexport) void devmenu_show(void);

/**
 * Hide the dev-menu window.  Same thread-safety guarantee as devmenu_show.
 */
__declspec(dllexport) void devmenu_hide(void);

/**
 * Register the command dispatcher.  Call once, immediately after devmenu_init
 * returns.  When the user submits a command in the window, `cb` is invoked on
 * the Qt thread with the command as a UTF-16 string and its codeunit count.
 *
 * The callback should forward the command to the UE4 console.  Note that it
 * fires on the Qt thread — keep it short and thread-safe.
 */
typedef void (*DevMenuCommandCallback)(const wchar_t *cmd, int len);
__declspec(dllexport) void devmenu_set_command_callback(DevMenuCommandCallback cb);

#ifdef __cplusplus
}
#endif
