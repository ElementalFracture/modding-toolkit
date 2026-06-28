use serde::{Serialize, Deserialize};
use std::fs;
use std::io::{BufWriter, Write};
use std::net::TcpStream;
use std::sync::{Mutex, OnceLock, mpsc};
use std::thread;
use std::time::Duration;

#[derive(Serialize, Deserialize, Clone, Default, Debug)]
pub struct MatchState {
    pub state: String,
    pub map: String,
    pub match_start_time: f32,
    pub players: Vec<Player>,
    pub events: Vec<String>,
}

#[derive(Serialize, Deserialize, Clone, Default, Debug)]
pub struct Player {
    pub username: String,
    pub id: u32,
    pub ping: f32,
    pub is_inactive: bool,
    pub is_spectator: bool,
    pub is_bot: bool,
}

pub const STATE_FILE: &str = "match_state.json";
const   STATE_FILE_TMP: &str = "match_state.json.tmp";

const PUSH_ADDR: &str = "127.0.0.1:4950";

/// Events the game-thread hooks push into the channel.
/// The background writer thread owns MatchState and applies these.
#[derive(Debug)]
pub enum StateEvent {
    /// Fired from RestartPlayer hook — player joined or respawned.
    /// Carries a full player snapshot collected on the game thread.
    PlayerSnapshot(Player),

    /// Fired from AGameMode::Logout hook.
    /// TODO: find offset and wire up in lib.rs (same pattern as RestartPlayer).
    PlayerLeft(String),

    /// Fired from AGameMode::SetMatchState hook.
    /// TODO: find offset and wire up in lib.rs.
    PhaseChanged { new_phase: String, map: String, start_time: f32 },

    /// Full player list refresh, collected on the game thread.
    /// Used on initial ready signal and after a phase transition.
    FullRefresh(Vec<Player>, String, String, f32),
}

/// Background writer thread entry point.
/// Owns MatchState entirely — never reads UE4 memory.
pub fn run_writer(rx: mpsc::Receiver<StateEvent>) {
    let mut state = MatchState::default();
    state.state = "Initializing".to_string();

    // Spawn push thread: connects to Python and sends JSON lines on qualifying events.
    let (push_tx, push_rx) = mpsc::channel::<String>();
    thread::spawn(move || maintain_push_connection(push_rx));

    for event in rx {
        if apply_event(&mut state, event) {
            update_tcp_cache(&state);
            write_state_file(&state);
            if let Ok(json) = serde_json::to_string(&state) {
                let _ = push_tx.send(json);
            }
        }
    }
}

/// Maintains a persistent outbound TCP connection to the Python push receiver.
/// Reconnects automatically on failure. Drops events that can't be delivered
/// after one reconnect attempt to avoid unbounded blocking.
fn maintain_push_connection(rx: mpsc::Receiver<String>) {
    let mut conn: Option<BufWriter<TcpStream>> = None;

    for json in rx {
        if !try_send(&mut conn, &json) {
            // Connection lost — try once to reconnect and resend.
            conn = try_connect();
            if !try_send(&mut conn, &json) {
                conn = None; // Will reconnect on the next event.
            }
        }
    }
}

fn try_connect() -> Option<BufWriter<TcpStream>> {
    for attempt in 1u32..=5 {
        match TcpStream::connect(PUSH_ADDR) {
            Ok(s) => {
                println!("[match_tracker] Push connection established to {}", PUSH_ADDR);
                return Some(BufWriter::new(s));
            }
            Err(e) => {
                println!("[match_tracker] Push connect attempt {}: {}", attempt, e);
                thread::sleep(Duration::from_secs(2));
            }
        }
    }
    None
}

fn try_send(conn: &mut Option<BufWriter<TcpStream>>, json: &str) -> bool {
    let Some(ref mut c) = conn else { return false };
    c.write_all(json.as_bytes()).is_ok()
        && c.write_all(b"\n").is_ok()
        && c.flush().is_ok()
}

pub fn apply_event(state: &mut MatchState, event: StateEvent) -> bool {
    match event {
        StateEvent::PlayerSnapshot(player) => {
            match state.players.iter().position(|p| p.id == player.id) {
                Some(idx) => { state.players[idx] = player; }
                None => {
                    state.events.push(format!("PlayerJoined: {}", player.username));
                    state.players.push(player);
                }
            }
            true
        }

        StateEvent::PlayerLeft(username) => {
            let before = state.players.len();
            state.players.retain(|p| p.username != username);
            if state.players.len() < before {
                state.events.push(format!("PlayerLeft: {}", username));
                true
            } else {
                false
            }
        }

        StateEvent::PhaseChanged { new_phase, map, start_time } => {
            if state.state != new_phase {
                state.events.push(format!(
                    "MatchPhaseChanged: {} -> {}", state.state, new_phase
                ));
                state.state = new_phase;
                state.map = map;
                state.match_start_time = start_time;
                true
            } else {
                false
            }
        }

        StateEvent::FullRefresh(players, phase, map, start_time) => {
            state.players = players;
            state.state = phase;
            state.map = map;
            state.match_start_time = start_time;
            true
        }
    }
}

fn write_state_file(state: &MatchState) {
    if let Ok(json) = serde_json::to_string_pretty(state) {
        if fs::write(STATE_FILE_TMP, &json).is_ok() {
            let _ = fs::rename(STATE_FILE_TMP, STATE_FILE);
        }
    }
}

// ── TCP fallback cache (for the debug get_players command on port 4951) ───────

static TCP_CACHE: OnceLock<Mutex<MatchState>> = OnceLock::new();

pub fn tcp_cache() -> &'static Mutex<MatchState> {
    TCP_CACHE.get_or_init(|| Mutex::new(MatchState::default()))
}

pub fn get_match_state() -> MatchState {
    tcp_cache().lock().unwrap().clone()
}

pub fn update_tcp_cache(state: &MatchState) {
    *tcp_cache().lock().unwrap() = state.clone();
}
