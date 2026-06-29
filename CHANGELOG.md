# Elemental Fracture — Client Mod Changelog

---

## 0.3.0

### Match Selector (`server_router.dll`) — new in package

The match selector is now distributed as part of the mod package. It hooks `sendto` in `ws2_32.dll` and sends a 5-byte side-channel UDP control packet (`EFM:<mode>`) to the router immediately before the first outgoing game packet. The router reads this, stores the mode preference for the client's IP, and routes the subsequent UE4 handshake accordingly. The join packet itself is forwarded completely unmodified.

Previously this was done by writing `server_mode.txt` / `server_endpoint.txt` files to disk and reading `server_routes.txt` for endpoint config. That approach had a race condition and required manual route config files. The new method is in-process, zero-file, and fires atomically with the connection.

Exports `get_current_mode()` for other DLLs to query the active game mode.

### Auth — no more `auth_result.txt`

Auth state is no longer written to disk. `auth_injector` now pushes auth directly into `qt_devmenu`'s in-process memory by resolving `mod_receive_auth` from `qt_devmenu.dll` via `GetProcAddress`. `qt_devmenu` stores it in atomics (`AUTH_IS_CHEAT`, `AUTH_IS_DEV`, `AUTH_USERNAME`) and applies it immediately — teleport blocking and staff status update the moment the server's `EF_AUTH` packet is intercepted.

On startup, `auth_injector` removes any stale `auth_result.txt` left by 0.2.0 or earlier.

The Account panel in the dev menu (`devmenu_imgui`) no longer reads or displays the auth token from disk.

---

## 0.2.0

### Dev Menu

- Key rebind: `qt_devmenu` loads `devmenu_set_menu_key_callback` from `devmenu_imgui.dll` at startup so the low-level keyboard hook updates atomically whenever the user saves a new menu key in the UI.
- Account panel: displays username, cheat status, and dev status read from `auth_result.txt`.
- Toast notification on connection.
- Auth state is primed at startup (before the keyboard hook installs) so teleport blocking is armed before the first menu open.
- Perk switching implemented.

### Auth

- `auth_injector` parses the extended `EF_AUTH:<cheat>:<dev>:<username>` packet format and writes `IS_CHEAT`, `IS_DEV`, and `USERNAME` to `auth_result.txt`.

### Loader

- `client_mod_loader` moves `LoadLibraryA` calls off `DllMain` into a dedicated `CreateThread` entry point to avoid loader-lock deadlock on native Windows.

---

## 0.1.0

Initial release. Item spawner, character manager, staff commands, dev command panel. Basic `StartMatch` button. Auth injector and xinput loader.
