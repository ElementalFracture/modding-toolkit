use serde::{Serialize, Deserialize};
use std::fs;

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
pub fn run_writer(rx: std::sync::mpsc::Receiver<StateEvent>) {
    let mut state = MatchState::default();
    state.state = "WaitingForServer".to_string();

    for event in rx {
        if apply_event(&mut state, event) {
            update_tcp_cache(&state);
            write_state_file_inner(&state);
        }
    }
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

fn write_state_file_inner(state: &MatchState) {
    if let Ok(json) = serde_json::to_string_pretty(state) {
        println!("[match_tracker] writing state:\n{}", json);
        if fs::write(STATE_FILE_TMP, &json).is_ok() {
            let _ = fs::rename(STATE_FILE_TMP, STATE_FILE);
        }
    }
}

/// TCP fallback: return a snapshot via a shared cache rather than reading UE4 memory.
/// The cache is updated by the writer thread after each event.
use std::sync::{Mutex, OnceLock};

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
