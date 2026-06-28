use std::ffi::c_void;
use std::fmt::Write as FmtWrite;
use std::net::{TcpListener, TcpStream};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{self, Sender};
use std::sync::OnceLock;
use std::thread;
use std::panic;
use std::io::{Read, Write};
use retour::static_detour;
use game_base::*;
use ue_types::*;

mod match_state;
use match_state::*;

static MOD_NAME: &str = "match_tracker";

// Channel sender shared between all game-thread hooks.
// Populated once in mod_main_sync before any hooks fire.
static EVENT_TX: OnceLock<Sender<StateEvent>> = OnceLock::new();

// Fires once on the first RestartPlayer call to dump the GameMode vtable.
// This lets us locate Logout and SetMatchState by their slots relative to RestartPlayer.
static VTABLE_DUMPED: AtomicBool = AtomicBool::new(false);

fn tx() -> &'static Sender<StateEvent> {
    EVENT_TX.get().expect("EVENT_TX not initialized")
}

// ── Hook: AGameModeBase::RestartPlayer ────────────────────────────────────────
// Fires on the game thread when a player spawns or respawns.
// Reading APlayerState here is safe — we're on the same thread that owns it.

static_detour! {
    static RestartPlayer: fn(*const AGameModeBase, *const APlayerController);
}

unsafe fn hook_restart_player(
    this: *const AGameModeBase,
    controller: *const APlayerController,
) {
    // Call through first so the player is fully initialized when we read them
    RestartPlayer.call(this, controller);

    // One-time vtable dump: walk this->vtable, find RestartPlayer's slot,
    // then print ±20 neighbors so we can identify Logout and SetMatchState.
    if !VTABLE_DUMPED.swap(true, Ordering::SeqCst) {
        let base        = GameBase::singleton().at_offset(0) as usize;
        let rp_addr     = GameBase::singleton().at_offset(0x233CE50) as usize;
        let vtable      = *(this as *const *const usize);

        println!("[match_tracker] === VTABLE DUMP ===");
        println!("[match_tracker] base        = {:#x}", base);
        println!("[match_tracker] RestartPlayer runtime addr = {:#x}  RVA = {:#x}", rp_addr, rp_addr.wrapping_sub(base));

        // Find the RestartPlayer slot in the vtable.
        let mut rp_slot: Option<usize> = None;
        for i in 0..512usize {
            let entry = *vtable.add(i);
            if entry == 0 { break; }
            if entry == rp_addr {
                rp_slot = Some(i);
                break;
            }
        }

        match rp_slot {
            None => println!("[match_tracker] RestartPlayer not found in first 512 vtable slots"),
            Some(slot) => {
                println!("[match_tracker] RestartPlayer is vtable[{}]", slot);
                let lo = slot.saturating_sub(20);
                let hi = slot + 20;
                for i in lo..=hi {
                    let entry = *vtable.add(i);
                    let rva   = entry.wrapping_sub(base);
                    let tag   = if i == slot { "  <-- RestartPlayer" } else { "" };
                    println!("[match_tracker] vtable[{:3}] = {:#x}  RVA {:#010x}{}", i, entry, rva, tag);
                }
            }
        }
        println!("[match_tracker] === END VTABLE DUMP ===");

        // Also write to a file since Wine stdout may not be captured.
        {
            let mut out = String::new();
            let _ = writeln!(out, "base        = {:#x}", base);
            let _ = writeln!(out, "RestartPlayer runtime addr = {:#x}  RVA = {:#x}", rp_addr, rp_addr.wrapping_sub(base));
            if let Some(slot) = rp_slot {
                let lo = slot.saturating_sub(20);
                let hi = slot + 20;
                for i in lo..=hi {
                    let entry = *vtable.add(i);
                    let rva   = entry.wrapping_sub(base);
                    let tag   = if i == slot { "  <-- RestartPlayer" } else { "" };
                    let _ = writeln!(out, "vtable[{:3}] = {:#x}  RVA {:#010x}{}", i, entry, rva, tag);
                }
            } else {
                let _ = writeln!(out, "RestartPlayer not found in first 512 vtable slots");
            }
            let _ = std::fs::write("vtable_dump.txt", out);
        }
    }

    // Collect player data on the game thread — no background thread touches UE4 memory
    if let Some(game_mode) = GameMode::mode() {
        let state_opt = game_mode.state();
        let players = game_mode.players();

        // Find the player matching this controller by checking the player array.
        // Send a FullRefresh so the writer has the authoritative list after each spawn.
        let player_list: Vec<match_state::Player> = players.iter().map(|p| match_state::Player {
            username:     p.player_name.to_string(),
            id:           p.player_id.to_native(),
            ping:         p.exact_ping.to_native(),
            is_inactive:  p.flags.b_is_inactive(),
            is_spectator: p.flags.b_is_spectator(),
            is_bot:       p.flags.b_is_a_bot(),
        }).collect();

        let (phase, map, start_time) = state_opt.map(|s| (
            s.match_phase.to_string(),
            GameWorld::world()
                .and_then(|w| w.base())
                .map(|b| b.url.map.to_string())
                .unwrap_or_default(),
            s.match_start_time.to_native(),
        )).unwrap_or_default();

        let _ = tx().send(StateEvent::FullRefresh(player_list, phase, map, start_time));
    }
}

// ── Hook: AGameMode::Logout ───────────────────────────────────────────────────
// TODO: find the offset for Logout in offsets_server.rs (same process used to
//       find RestartPlayer at 0x233CE50). Add it to GameOffsets and wire it here.
//
// static_detour! {
//     static Logout: fn(*const AGameModeBase, *const AController);
// }
//
// unsafe fn hook_logout(this: *const AGameModeBase, exiting: *const AController) {
//     // Read the player name BEFORE calling through — after logout the state is gone
//     let username = /* read from exiting->PlayerState->PlayerName */;
//     Logout.call(this, exiting);
//     let _ = tx().send(StateEvent::PlayerLeft(username));
// }

// ── Hook: AGameMode::SetMatchState ────────────────────────────────────────────
// TODO: find the offset for SetMatchState.
//
// static_detour! {
//     static SetMatchState: fn(*const AGameModeBase, *const FName);
// }
//
// unsafe fn hook_set_match_state(this: *const AGameModeBase, new_state: *const FName) {
//     SetMatchState.call(this, new_state);
//     if let Some(game_mode) = GameMode::mode() {
//         let state_opt = game_mode.state();
//         let (phase, map, start_time) = ...;
//         let _ = tx().send(StateEvent::PhaseChanged { new_phase: phase, map, start_time });
//     }
// }

// ── Entry point ───────────────────────────────────────────────────────────────

#[no_mangle]
fn mod_main_sync(base_addr: *const c_void) {
    unsafe { game_base::OFFSETS = game_base::offsets_server::get_offsets() };
    GameBase::initialize(MOD_NAME, base_addr);

    println!("[match_tracker] Starting...");

    // Channel: game-thread hooks → background file writer
    let (tx, rx) = mpsc::channel::<StateEvent>();
    EVENT_TX.set(tx).expect("EVENT_TX already set");

    // Background thread: owns MatchState, writes file, updates TCP cache.
    // Never reads UE4 memory.
    thread::spawn(move || match_state::run_writer(rx));

    // Wire RestartPlayer hook (game thread)
    unsafe {
        let restart_player_fn: fn(*const AGameModeBase, *const APlayerController) =
            std::mem::transmute(GameBase::singleton().at_offset(0x233CE50));
        RestartPlayer
            .initialize(restart_player_fn, |this, controller| {
                hook_restart_player(this, controller);
            })
            .expect("Failed to hook RestartPlayer");
        RestartPlayer.enable().expect("Failed to enable RestartPlayer hook");
    }

    // TCP listener stays for debug/tooling use (get_players now reads from cache, not UE4)
    thread::spawn(|| listen_for_connections());

    println!("[match_tracker] Ready. Hooks live, writer thread running.");
}

// ── TCP debug interface ───────────────────────────────────────────────────────

const MESSAGE_SIZE: usize = 1;

fn handle_client(mut stream: TcpStream) {
    loop {
        let message = read_message(&mut stream);
        if message.is_err() { break; }

        match message.unwrap().trim() {
            "" => (),
            "quit" => break,

            "get_key_addresses" => {
                let _ = stream.write(format!("WORLD PROXY: {:p}\n", WorldProxy::proxy().expect("No World Proxy")).as_bytes());
                let _ = stream.write(format!("WORLD: {:p}\n",       WorldProxy::world().expect("No World")).as_bytes());
                let _ = stream.write(format!("LEVEL: {:p}\n",       WorldProxy::level().expect("No Level")).as_bytes());
                let _ = stream.write(format!("GAME INSTANCE: {:p}\n", GameInstance::instance().expect("No Game Instance").base().unwrap()).as_bytes());
                break;
            }

            "get_players" => {
                // Reads from our own Mutex cache — zero UE4 memory access
                let state = get_match_state();
                let json = serde_json::to_string(&state).unwrap_or_default();
                let _ = stream.write(json.as_bytes());
                let _ = stream.write(b"\n");
                break;
            }

            msg => {
                println!("[match_tracker] Unknown command: {msg}");
                break;
            }
        }
    }
}

fn read_message(stream: &mut TcpStream) -> Result<String, std::string::FromUtf8Error> {
    let mut received: Vec<u8> = vec![];
    let mut rx_bytes = [0u8; MESSAGE_SIZE];
    loop {
        let n = stream.read(&mut rx_bytes);
        if n.is_err() { break; }
        let n = n.unwrap();
        if rx_bytes[0] == b'\n' { break; }
        received.extend_from_slice(&rx_bytes[..n]);
        if n < MESSAGE_SIZE { break; }
    }
    String::from_utf8(received)
}

fn listen_for_connections() {
    let listener = TcpListener::bind("0.0.0.0:4951").expect("Could not open TCP port 4951");
    println!("[match_tracker] TCP debug interface on :4951");
    for stream in listener.incoming() {
        if let Ok(stream) = stream {
            thread::spawn(|| {
                panic::catch_unwind(|| handle_client(stream));
            });
        }
    }
}
