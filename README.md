# Elemental Fracture — Modding Toolkit

Rust workspace of DLL mods for the Spellbreak client and server.

## Structure

```
modding-toolkit/
  local/                  ← client & server DLL mods (Rust workspace)
    mod_loader/           ← shim that DLL-loads all mods at game startup
    client_mod_loader/    ← client-specific loader variant
    sdk/                  ← shared crates (ue_types, game_base, injection_utils, …)
    mods/
      client/             ← client-side mods (Windows x64 DLLs)
        auth_injector/
        asset_buddy/
        pathfinder/
        qt_devmenu/       ← replaces tilde dev console with a Qt window
        tdm_helper/
        modding_debugger/
        server_router/    (abandoned)
      server/
        match_tracker/    ← streams match state to the elefrac proxy
  remote/
    server_controller/    ← Elixir-based remote server management
```

## Building

```bash
cd modding-toolkit/local
cargo build --release
```

To run the SDK unit tests:

```bash
cargo test -p ue_types --lib
```

## SDK Crates

| Crate | Purpose |
|---|---|
| `sdk/ue_types` | Unreal Engine type definitions (`FString`, `UObject`, etc.) |
| `sdk/game_base` | Game-specific types, memory offsets, base address resolution |
| `sdk/injection_utils` | Hook helpers: console command intercept, asset manager patches |
| `sdk/utils` | Logging, debug output |
| `sdk/memory_management` | Heap-managed type helpers (`FString` allocation, etc.) |

## Client Mods

### `auth_injector`

Injects the player's auth token into every outgoing UDP join packet so the elefrac proxy can authenticate the player without a separate login flow.

- Reads a 64-char hex token from `Mods/commands/auth_token.txt` at startup
- Hooks `sendto` (WinSock)
- Locates the UID field (`ComputerName-32hexchars`) by scanning for the 2×-encoded `/Game/Maps/` header
- Writes the first 32 bytes of the token into the UID slot, 2×-encoded (`byte * 2`)

### `asset_buddy`

Enables loading custom game assets from disk without a signed PAK file.

- Bypasses UE4's PAK signature and restriction checks at startup (`mod_main_sync`)
- Forces the asset manager to load assets directly from disk
- Reads a `manifest.json` at runtime to mount virtual asset paths to filesystem paths

### `pathfinder`

Developer/debugging utility that exposes engine state through in-game console commands.

- `getplayerpos` — prints the local player's world coordinates to the in-game console

### `tdm_helper` (compiled as `better_commands`)

Remaps console commands to corrected equivalents, working around command name changes in the private server binary.

- `choosecharacterperk <perk>` — remaps removed perk names to their current equivalents
- `heal` — applies health and stoneskin gameplay effects
- `level up` — max-levels all perk trees

### `qt_devmenu`

Replaces the UE4 developer console (tilde `~`) with a Qt `QMainWindow`.

The mod is split into two compiled artifacts:

| Artifact | Language | Role |
|---|---|---|
| `qt_devmenu.dll` | Rust | Hooks `UConsole::FakeGotoState`; loads `devmenu_qt.dll` at runtime |
| `devmenu_qt.dll` | C++ / Qt Widgets | Owns the Qt event loop and the window |

When tilde is pressed the UE4 console is suppressed and the Qt window appears. Commands entered in the window are forwarded to the UE4 console via a registered C callback (`devmenu_set_command_callback`), so all existing console commands continue to work. Escape hides the window.

Build `devmenu_qt.dll` from `mods/client/qt_devmenu/qt_ui/`:
- Windows: `build_windows.bat` (requires Qt and CMake on PATH)
- mingw64: `build_mingw.sh`

**Prerequisite**: `offsets.vf_tables.console_fake_goto_state` must be set in `sdk/game_base/src/offsets_client.rs`. Run `dumpconsole` in the `modding_debugger` (via the tilde console) to discover the correct byte offset, then set it there. Until it is set the mod logs a warning and the native console falls through unchanged.

### `modding_debugger`

DLL load canary and hook intercept tool used during mod development.

- Writes a canary file on DLL load (`#[ctor]`) to confirm the loader picked up the DLL
- Hooks various engine internals and surfaces their output to the in-game console

## Server Mods

### `match_tracker`

Runs inside the Wine game server container. Streams live match state (player list, match status) to the elefrac Python proxy via TCP on port 4951, which then rebroadcasts it on the broadcast port (`:8777`) for the Discord bot.

## Planned Packages

`injection_utils::hooks::console::add_command_intercept` (used by `tdm_helper` and `pathfinder`) supports stacking multiple interceptors. Future packages planned:

| Package | Purpose |
|---|---|
| `command_logger` | Intercepts every console command and appends it to a log file — useful for auditing what commands players or cheat tools attempt |
| `command_guard` | Intercepts commands by name or prefix and drops them before they reach the engine — allows selectively disabling console commands in shipping builds |
