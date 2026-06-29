// In-game ImGui overlay for Spellbreak dev menu.
//
// Hooks IDXGISwapChain::Present (vtable slot 8) so the UI renders inside the game's frame.
// Layout: left nav sidebar + right content panel.

#include <atomic>
#include <cstring>
#include <cstdio>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <xinput.h>

// ── Debug log ─────────────────────────────────────────────────────────────────

static void dbg(const char *msg)
{
    // Resolve log path once: <game_root>\Mods\dlls\devmenu_debug.txt
    static char s_path[MAX_PATH] = {};
    if (!s_path[0]) {
        char exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe, sizeof(exe));
        // Walk back past g3.exe, Win64, Binaries, g3 (4 separators)
        char *p = exe + strlen(exe);
        for (int n = 0; n < 4 && p > exe; --p)
            if (*p == '\\') ++n;
        *(p + 2) = '\0';  // keep up to the separator
        snprintf(s_path, sizeof(s_path),
                 "%sMods\\dlls\\devmenu_debug.txt", exe + (exe[0] ? 0 : 0));
        // Simpler: just use a fixed relative path from CWD as fallback
        if (s_path[0] == '\0')
            snprintf(s_path, sizeof(s_path), "devmenu_debug.txt");
    }

    HANDLE f = CreateFileA(s_path,
                           FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(f, msg, (DWORD)strlen(msg), &w, nullptr);
    WriteFile(f, "\r\n", 2, &w, nullptr);
    CloseHandle(f);
}

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

// ── C ABI ─────────────────────────────────────────────────────────────────────

extern "C" {
    typedef void (*CommandDispatchFn)(const unsigned short *cmd, int len);
    typedef void (*MenuKeyChangedFn)(unsigned vk, unsigned mods);
    __declspec(dllexport) void devmenu_init();
    __declspec(dllexport) void devmenu_show();
    __declspec(dllexport) void devmenu_hide();
    __declspec(dllexport) void devmenu_set_staff(int is_staff, int is_dev, const char *username);
    __declspec(dllexport) void devmenu_set_command_callback(CommandDispatchFn cb);
    __declspec(dllexport) void devmenu_get_menu_key(unsigned *vk, unsigned *mods);
    __declspec(dllexport) void devmenu_set_menu_key_callback(MenuKeyChangedFn cb);
    // Called by auth_injector when a token file is found at game launch,
    // BEFORE connecting to any game server.  Shows a toast on first rendered frame.
    __declspec(dllexport) void devmenu_token_loaded(const char *username, int is_staff, int is_dev);
}

// ── Auth / display state ──────────────────────────────────────────────────────

static std::atomic<bool> g_show     { false };
static std::atomic<bool> g_is_staff { false };
static std::atomic<bool> g_is_dev   { false };
static char              g_username[256] {};
static CRITICAL_SECTION  g_state_cs;
static CommandDispatchFn g_command_cb = nullptr;

// ── Hotkeys ───────────────────────────────────────────────────────────────────

static void hk_drop_amulet();
static void hk_drop_belt();
static void hk_drop_boots();
static void hk_drop_rune();
static void hk_drop_secondary();
static void hk_drop_all();
static void hk_heal();

struct HotkeyDef {
    const char *label;
    void (*action)();
    UINT vk;         // keyboard primary (0 = unbound)
    UINT mods;       // Shift=1, Ctrl=2, Alt=4
    UINT vk2;        // chord key (0 = none)
    WORD ctrl_btn;   // XInput button mask (0 = unbound)
    BYTE ctrl_player;// XInput player index (0-3)
};
static HotkeyDef g_hotkeys[] = {
    { "Drop Amulet",           hk_drop_amulet,    0, 0, 0, 0, 0 },
    { "Drop Belt",             hk_drop_belt,      0, 0, 0, 0, 0 },
    { "Drop Boots",            hk_drop_boots,     0, 0, 0, 0, 0 },
    { "Drop Rune",             hk_drop_rune,      0, 0, 0, 0, 0 },
    { "Drop Secondary Wep",    hk_drop_secondary, 0, 0, 0, 0, 0 },
    { "Drop All (no primary)", hk_drop_all,       0, 0, 0, 0, 0 },
    { "Heal",                  hk_heal,           0, 0, 0, 0, 0 },
    // Must stay last — index is referenced by k_menu_key_idx.
    // action=nullptr: toggling is handled by the low-level keyboard hook in
    // qt_devmenu, not the WndProc hotkey loop.  vk2 is intentionally ignored
    // for this entry (LL hooks don't support chord keys).
    { "Open / Close Menu",     nullptr,            0x77 /* F8 */, 0, 0, 0, 0 },
};
static const int k_hotkey_count = (int)(sizeof(g_hotkeys) / sizeof(g_hotkeys[0]));
static const int k_menu_key_idx = k_hotkey_count - 1;
static int       g_listening_idx      = -1;  // keyboard listen
static int       g_ctrl_listening_idx = -1;  // controller listen
static WORD      g_ctrl_prev[4]       = {};  // previous XInput button state per player
static void      save_hotkeys();  // defined after persistence helpers

// XInput button definitions (including synthetic trigger masks 0x4000/0x8000).
struct CtrlBtnDef { WORD mask; const char *name; };
static const CtrlBtnDef k_ctrl_btns[] = {
    { XINPUT_GAMEPAD_A,              "A"       },
    { XINPUT_GAMEPAD_B,              "B"       },
    { XINPUT_GAMEPAD_X,              "X"       },
    { XINPUT_GAMEPAD_Y,              "Y"       },
    { XINPUT_GAMEPAD_LEFT_SHOULDER,  "LB"      },
    { XINPUT_GAMEPAD_RIGHT_SHOULDER, "RB"      },
    { 0x4000,                        "LT"      },
    { 0x2000,                        "RT"      },
    { XINPUT_GAMEPAD_DPAD_UP,        "D-Up"    },
    { XINPUT_GAMEPAD_DPAD_DOWN,      "D-Down"  },
    { XINPUT_GAMEPAD_DPAD_LEFT,      "D-Left"  },
    { XINPUT_GAMEPAD_DPAD_RIGHT,     "D-Right" },
    { XINPUT_GAMEPAD_START,          "Start"   },
    { XINPUT_GAMEPAD_BACK,           "Back"    },
    { XINPUT_GAMEPAD_LEFT_THUMB,     "LS"      },
    { XINPUT_GAMEPAD_RIGHT_THUMB,    "RS"      },
};
static const int k_ctrl_btn_count = (int)(sizeof(k_ctrl_btns) / sizeof(k_ctrl_btns[0]));

static WORD xinput_sample(BYTE player)
{
    XINPUT_STATE st = {};
    if (XInputGetState(player, &st) != ERROR_SUCCESS) return 0;
    WORD b = st.Gamepad.wButtons;
    if (st.Gamepad.bLeftTrigger  > 64) b |= 0x4000;
    if (st.Gamepad.bRightTrigger > 64) b |= 0x2000;
    return b;
}

static const char *ctrl_btn_name(WORD mask)
{
    for (int i = 0; i < k_ctrl_btn_count; ++i)
        if (k_ctrl_btns[i].mask == mask) return k_ctrl_btns[i].name;
    return "?";
}

// Callback invoked (from any thread) whenever the menu key binding changes.
typedef void (*MenuKeyChangedFn)(unsigned vk, unsigned mods);
static MenuKeyChangedFn g_menu_key_cb = nullptr;

// ── Stuck-input tracking ──────────────────────────────────────────────────────
// Bitmask of VKs whose WM_KEYUP was consumed by the overlay (ImGui had keyboard
// focus so the game never saw the release).  Flushed as synthetic WM_KEYUP
// messages when the menu closes.
static uint64_t g_eaten_keyup[4] = {};
static inline void eaten_set(UINT vk) { if (vk < 256) g_eaten_keyup[vk>>6] |=  (1ULL<<(vk&63)); }
static inline void eaten_clr(UINT vk) { if (vk < 256) g_eaten_keyup[vk>>6] &= ~(1ULL<<(vk&63)); }
static inline bool eaten_get(UINT vk) { return vk < 256 && (g_eaten_keyup[vk>>6] & (1ULL<<(vk&63))); }

// ── D3D / ImGui state ─────────────────────────────────────────────────────────

static ID3D11Device           *g_device  = nullptr;
static ID3D11DeviceContext    *g_context = nullptr;
static ID3D11RenderTargetView *g_rtv     = nullptr;
static std::atomic<HWND>       g_hwnd    { nullptr };
static std::atomic<bool>       g_imgui_ready { false };

// ── Bindable key whitelist ────────────────────────────────────────────────────

// Only accept keys a user would consciously press as a hotkey.
// Filters synthetic events (e.g. VK_NUMLOCK=0x90) the engine injects on mouse clicks.
static bool is_bindable_key(UINT vk)
{
    if (vk >= 'A' && vk <= 'Z') return true;
    if (vk >= '0' && vk <= '9') return true;
    if (vk >= VK_F1 && vk <= VK_F12) return true;
    switch (vk) {
    case VK_SPACE: case VK_TAB: case VK_INSERT: case VK_DELETE:
    case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_OEM_1: case VK_OEM_2: case VK_OEM_3: case VK_OEM_4:
    case VK_OEM_5: case VK_OEM_6: case VK_OEM_7:
    case VK_OEM_MINUS: case VK_OEM_PLUS: case VK_OEM_COMMA: case VK_OEM_PERIOD:
        return true;
    }
    return false;
}

// ── Present vtable hook ───────────────────────────────────────────────────────

typedef HRESULT (STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain *, UINT, UINT);
static PresentFn g_orig_present = nullptr;

// ── WndProc hook ─────────────────────────────────────────────────────────────

// Initialized to DefWindowProcW so hooked_wndproc can safely call it before
// the wndproc-install thread has written the real value.
static WNDPROC g_orig_wndproc = DefWindowProcW;
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static LRESULT CALLBACK hooked_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // Spellbreak's UE4 engine injects a synthetic WM_KEYDOWN VK_NUMLOCK on
    // every mouse click as an internal focus signal.  If it reaches the game's
    // own key-binding dialog it gets captured as the new binding.  Drop it here
    // unconditionally — Num Lock is not a bindable game key in Spellbreak.
    if (msg == WM_KEYDOWN && static_cast<UINT>(wp) == VK_NUMLOCK)
        return 0;

    // Cancel controller-listen on Escape too.
    if (msg == WM_KEYDOWN && static_cast<UINT>(wp) == VK_ESCAPE && g_ctrl_listening_idx >= 0) {
        g_ctrl_listening_idx = -1;
        return 0;
    }

    // Key bind capture — highest priority, fires even when an input field has focus.
    // Only accepts whitelisted VK codes so synthetic engine events (e.g. VK_NUMLOCK)
    // don't accidentally snap up the listener before the user presses a real key.
    if (msg == WM_KEYDOWN && g_listening_idx >= 0) {
        UINT vk = static_cast<UINT>(wp);
        if (vk == VK_ESCAPE) {
            g_listening_idx = -1;
        } else if (is_bindable_key(vk)) {
            UINT mods = 0;
            if (GetKeyState(VK_SHIFT)   & 0x8000) mods |= 1;
            if (GetKeyState(VK_CONTROL) & 0x8000) mods |= 2;
            if (GetKeyState(VK_MENU)    & 0x8000) mods |= 4;
            UINT vk2 = 0;
            // The menu key entry is handled by a low-level keyboard hook that
            // doesn't support chord keys — don't capture vk2 for it.
            if (g_listening_idx != k_menu_key_idx) {
                for (UINT k = 1; k < 256 && vk2 == 0; ++k) {
                    if (k != vk && is_bindable_key(k) && (GetKeyState(k) & 0x8000))
                        vk2 = k;
                }
            }
            g_hotkeys[g_listening_idx].vk   = vk;
            g_hotkeys[g_listening_idx].mods  = mods;
            g_hotkeys[g_listening_idx].vk2   = vk2;
            g_listening_idx = -1;
            save_hotkeys();
        }
        // Non-whitelisted key: stay in listening mode and consume the event.
        return 0;
    }

    if (g_show.load() && g_imgui_ready.load()) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);

        // Block WM_INPUT unconditionally — UE4 routes camera rotation through raw
        // mouse input, which bypasses WantCaptureMouse entirely.
        if (msg == WM_INPUT) return 0;

        // Block all mouse messages regardless of WantCaptureMouse.
        // WM_LBUTTONDBLCLK must be here: rapid clicks generate it instead of
        // the second WM_LBUTTONDOWN, and if it leaks the game sees a press with
        // no release (WM_LBUTTONUP is also blocked) → stuck attack.
        switch (msg) {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
        case WM_MOUSEWHEEL:
            return 0;
        }

        const ImGuiIO &io = ImGui::GetIO();
        if (io.WantCaptureKeyboard) {
            switch (msg) {
            case WM_KEYDOWN: case WM_KEYUP:
            case WM_SYSKEYDOWN: case WM_SYSKEYUP:
            case WM_CHAR:
                // Track consumed key-ups so we can re-inject them on menu close.
                if (msg == WM_KEYUP || msg == WM_SYSKEYUP)
                    eaten_set(static_cast<UINT>(wp));
                return 0;
            }
        }
    }

    // Global hotkeys — fire only when the menu is closed.
    if (msg == WM_KEYDOWN && !g_show.load()) {
        UINT vk = static_cast<UINT>(wp);
        UINT cur_mods = 0;
        if (GetKeyState(VK_SHIFT)   & 0x8000) cur_mods |= 1;
        if (GetKeyState(VK_CONTROL) & 0x8000) cur_mods |= 2;
        if (GetKeyState(VK_MENU)    & 0x8000) cur_mods |= 4;
        for (int i = 0; i < k_hotkey_count; ++i) {
            if (!g_hotkeys[i].action || g_hotkeys[i].vk == 0)  continue;
            if (g_hotkeys[i].vk   != vk)        continue;
            if (g_hotkeys[i].mods != cur_mods)   continue;
            if (g_hotkeys[i].vk2  != 0 && !(GetKeyState(g_hotkeys[i].vk2) & 0x8000)) continue;
            g_hotkeys[i].action();
            return 0;
        }
    }

    return CallWindowProcW(g_orig_wndproc, hwnd, msg, wp, lp);
}

// ── Dispatch helpers ──────────────────────────────────────────────────────────

static void dispatch_ascii(const char *c)
{
    const int n = static_cast<int>(strlen(c));
    if (n <= 0 || !g_command_cb) return;
    wchar_t wide[512] {};
    for (int i = 0; i < n && i < 511; ++i)
        wide[i] = static_cast<wchar_t>(static_cast<unsigned char>(c[i]));
    g_command_cb(reinterpret_cast<const unsigned short *>(wide), n);
}

static void heal_player()
{
    dispatch_ascii("ApplyPlayerEffect GameplayEffect:BP_Effect_Player_Adjust_Health 150");
    dispatch_ascii("ApplyPlayerEffect GameplayEffect:BP_Effect_Player_Stoneskin 250");
}

static void hk_drop_amulet()    { dispatch_ascii("DropEquipment Amulet"); }
static void hk_drop_belt()      { dispatch_ascii("DropEquipment Belt"); }
static void hk_drop_boots()     { dispatch_ascii("DropEquipment Boots"); }
static void hk_drop_rune()      { dispatch_ascii("DropEquipment Rune"); }
static void hk_drop_secondary() { dispatch_ascii("DropEquipment Weapon.Secondary"); }
static void hk_drop_all()
{
    dispatch_ascii("DropEquipment Amulet");
    dispatch_ascii("DropEquipment Belt");
    dispatch_ascii("DropEquipment Boots");
    dispatch_ascii("DropEquipment Rune");
    dispatch_ascii("DropEquipment Weapon.Secondary");
}
static void hk_heal()           { heal_player(); }

// ── Nav / menu state ──────────────────────────────────────────────────────────

// Sections: 0=Match  1=Items  2=Loadouts  3=Staff  4=Dev
static int  g_section        = 0;
static int  g_item           = 0;
static bool g_in_right       = false;
static bool g_enter_consumed = false;

// ── Option tables ─────────────────────────────────────────────────────────────

struct ElemInfo { const char *display; const char *cmd; };
static const ElemInfo k_elements[] = {
    { "Fire",   "Fire"   },
    { "Ice",    "Ice"    },
    { "Shock",  "Shock"  },
    { "Earth",  "Earth"  },
    { "Wind",   "Wind"   },
    { "Poison", "Poison" },
};
static const int k_elem_count = 6;

// Primary gauntlet element for each class (matches k_classes order).
static const int k_class_elem[] = {
    0,  // Pyromancer  → Fire
    3,  // Stoneshaper → Earth
    2,  // Conduit     → Shock
    5,  // Toxicologist→ Poison
    1,  // Frostborn   → Ice
    4,  // Tempest     → Wind
};

struct RuneInfo { const char *display; const char *cmd; bool has_tier; };
static const RuneInfo k_runes[] = {
    { "Blink",          "Blink",          false },
    { "Dash",           "Dash",           true  },
    { "Springstep",     "Springstep",     true  },
    { "Invisibility",   "Invisibility",   true  },
    { "Shadowstep",     "Shadowstep",     true  },
    { "Featherfall",    "Featherfall",    true  },
    { "Flight",         "Flight",         true  },
    { "Teleportation",  "Teleportation",  true  },
    { "Gateway",        "Gateway",        true  },
    { "Wolf's Blood",   "Wolfsblood",     true  },
    { "Chronomaster",   "Chronomaster",   true  },
};
static const int k_rune_count = 11;

struct EquipInfo { const char *display; const char *cmd_key; int rarity; };  // rarity: 0=Common 1=Uncommon 2=Rare 3=Epic 4=Legendary

static const EquipInfo k_amulets[] = {
    { "Legendary Amulet",                    "Tier_5",         4 },
    { "Thinkers Amulet",                     "Thinkers",       1 },
    { "Rare Amulet of the Berserker",        "Berserker",      2 },
    { "Amulet of the Vandal",                "Vandal",         2 },
    { "Rare Amulet",                         "Capture",        2 },
    { "Infected Amulet",                     "Infected",       3 },
    { "Amulet of Icy Refraction",            "Refraction_Icy", 3 },
    { "Conflagration Amulet",                "Conflagration",  3 },
    { "Epic Amulet of the Slayer",           "Slayer",         3 },
    { "Crosswinds Amulet",                   "Crosswinds",     3 },
    { "Earthbind Amulet",                    "Eruption",       3 },
    { "Reactive Amulet",                     "Reactive",       3 },
    { "Epic Amulet of the Mender",           "Mender",         3 },
    { "Epic Amulet of the Survivor",         "Survivor",       3 },
    { "Legendary Amulet of the Behemoth",    "Behemoth",       4 },
    { "Legendary Amulet of the Spellslinger","Spellslinger",   4 },
    { "Legendary Amulet of the Scribe",      "Scribe",         4 },
};
static const int k_amulet_count = (int)(sizeof(k_amulets)/sizeof(k_amulets[0]));

static const EquipInfo k_boots[] = {
    { "Legendary Boots",                 "Tier_5",        4 },
    { "Slowfall Boots",                  "Slowfall",      1 },
    { "Boots of the Cat",                "Cat",           1 },
    { "Boots of the Scribe",             "Scribe",        2 },
    { "Rare Boots of the Berserker",     "Berserker",     2 },
    { "Boots of the Mouse",              "Mouse",         2 },
    { "Rare Boots of the Spellslinger",  "Spellslinger",  2 },
    { "Epic Boots of the Behemoth",      "Behemoth",      3 },
    { "Epic Boots of the Mender",        "Mender",        3 },
    { "Epic Boots of the Baron",         "Baron",         3 },
    { "Legendary Boots of the Slayer",   "Slayer",        4 },
};
static const int k_boot_count = (int)(sizeof(k_boots)/sizeof(k_boots[0]));

static const EquipInfo k_belts[] = {
    { "Legendary Belt",                  "Tier_5",          4 },
    { "Belt of the Wanderer",            "Wanderer",        4 },
    { "Belt of Constitution",            "Constitution",    1 },
    { "Belt of Earth and Wind",          "Earth_Wind",      2 },
    { "Belt of Fire and Ice",            "Fire_Ice",        2 },
    { "Belt of Lightning & Poison",      "Lightning_Poison",2 },
    { "Belt of Regeneration",            "Regeneration",    2 },
    { "Rare Belt of the Slayer",         "Slayer",          2 },
    { "Reinforced Belt",                 "Reinforced",      2 },
    { "Epic Belt of the Survivor",       "Survivor",        3 },
    { "Epic Belt of the Mender",         "Mender",          3 },
    { "Epic Belt of the Scribe",         "Scribe",          3 },
    { "Epic Belt of the Baron",          "Baron",           3 },
    { "Epic Belt of the Spellslinger",   "Spellslinger",    3 },
    { "Legendary Belt of the Berserker", "Berserker",       4 },
};
static const int k_belt_count = (int)(sizeof(k_belts)/sizeof(k_belts[0]));

// Rarity display names and colors (Common→Legendary, index 0–4).
// These map to Tier_1–Tier_5 in console commands.
static const char *k_rarity_names[] = {
    "Common", "Uncommon", "Rare", "Epic", "Legendary"
};
static const ImU32 k_rarity_colors[] = {
    IM_COL32(0xFF, 0xFF, 0xFF, 0xFF),  // Common    #FFFFFF
    IM_COL32(0x1E, 0xFF, 0x00, 0xFF),  // Uncommon  #1EFF00
    IM_COL32(0x00, 0x70, 0xDD, 0xFF),  // Rare      #0070DD
    IM_COL32(0xA3, 0x35, 0xEE, 0xFF),  // Epic      #A335EE
    IM_COL32(0xFF, 0x80, 0x00, 0xFF),  // Legendary #FF8000
};

static const char *k_bot_diffs[] = { "Very Easy", "Easy", "Medium", "Hard", "No Bots" };
static const char *k_arenas[]    = { "Hymnwood", "Halcyon", "Dustpool", "Bogmore", "Banehelm" };

static const char *k_classes[] = {
    "Pyromancer", "Stoneshaper", "Conduit", "Toxicologist", "Frostborn", "Tempest"
};
static const int k_class_count = 6;

// ── Perk trees ────────────────────────────────────────────────────────────────

struct PerkInfo { const char *display; const char *cmd; int cost; };

// Mind tree — display name, BP_Perk_ suffix, point cost.
static const PerkInfo k_mind_perks[] = {
    { "Intelligent",   "Intelligent",  1 },
    { "Blight",        "Inspired",     2 },
    { "Curious",       "Curious",      2 },
    { "Fellowship",    "Fellowship",   2 },
    { "Specialist",    "Hardened",     2 },
    { "Ambidextrous",  "Ambidextrous", 3 },
    { "Foresight",     "Foresight",    3 },
    { "Runic Fluency", "Runic_Fluency",4 },
};
static const int k_mind_count = 8;

// Body tree.
static const PerkInfo k_body_perks[] = {
    { "Tough",       "Tough",       1 },
    { "Scavenger",   "Scavenging",  2 },
    { "Thirsty",     "Thirsty",     2 },
    { "Fortitude",   "Fortitude",   3 },
    { "Recovery",    "Recovery",    3 },
    { "Vampiric",    "Vampiric",    3 },
    { "Vital Stone", "Vital_Stone", 3 },
    { "Harmony",     "Harmony",     4 },
    { "Vigor",       "Vigor",       4 },
};
static const int k_body_count = 9;

// Spirit tree.
static const PerkInfo k_spirit_perks[] = {
    { "Escapist",     "Escapist",     1 },
    { "Mystical",     "Mystical",     1 },
    { "Dexterity",    "Dexterity",    2 },
    { "Dilution",     "Tracking",     2 },
    { "Spellslinger", "Spellslinger", 2 },
    { "Vivify",       "Athletic",     2 },
    { "Wellspring",   "Wellspring",   2 },
    { "Recklessness", "Recklessness", 3 },
};
static const int k_spirit_count = 8;

// ── Rarity combo helper ───────────────────────────────────────────────────────

// Draws a BeginCombo with colored text for each rarity tier.
// idx is 0–4 (Common–Legendary). Width should accommodate "Legendary".
static bool rarity_combo(const char *id, int *idx, float width = 90.0f)
{
    ImGui::SetNextItemWidth(width);
    ImGui::PushStyleColor(ImGuiCol_Text, k_rarity_colors[*idx]);
    bool open = ImGui::BeginCombo(id, k_rarity_names[*idx]);
    ImGui::PopStyleColor();
    bool changed = false;
    if (open) {
        for (int i = 0; i < 5; ++i) {
            bool sel = (*idx == i);
            ImGui::PushStyleColor(ImGuiCol_Text, k_rarity_colors[i]);
            if (ImGui::Selectable(k_rarity_names[i], sel)) { *idx = i; changed = true; }
            ImGui::PopStyleColor();
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// ── Equipment combo (rarity-colored) ─────────────────────────────────────────

// allow_none=true: idx may be -1 (shows "None" at top of list).
static bool equip_combo(const char *id, int *idx, const EquipInfo *items, int count,
                        float width = 220.0f, bool allow_none = false)
{
    const char *preview;
    ImU32 col = IM_COL32(255, 255, 255, 255);
    if (allow_none && *idx < 0) {
        preview = "None";
    } else if (*idx >= 0 && *idx < count) {
        preview = items[*idx].display;
        col = k_rarity_colors[items[*idx].rarity];
    } else {
        preview = "(?)";
    }
    ImGui::SetNextItemWidth(width);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    bool open = ImGui::BeginCombo(id, preview);
    ImGui::PopStyleColor();
    bool changed = false;
    if (open) {
        if (allow_none) {
            bool sel = (*idx < 0);
            if (ImGui::Selectable("None", sel)) { *idx = -1; changed = true; }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        for (int i = 0; i < count; ++i) {
            bool sel = (*idx == i);
            ImGui::PushStyleColor(ImGuiCol_Text, k_rarity_colors[items[i].rarity]);
            if (ImGui::Selectable(items[i].display, sel)) { *idx = i; changed = true; }
            ImGui::PopStyleColor();
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// ── Helpers for amulet / boot / belt spawn commands ──────────────────────────

static void spawn_amulet(int idx, int amt)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "SpawnAmulet Loot:BP_Item_Amulet_%s %d", k_amulets[idx].cmd_key, amt);
    dispatch_ascii(buf);
}

static void spawn_boot(int idx, int amt)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "SpawnBoot Loot:BP_Item_Boots_%s %d", k_boots[idx].cmd_key, amt);
    dispatch_ascii(buf);
    // Named boots (non-generic-tier) don't auto-grant armor via the console command;
    // apply it explicitly based on rarity so the pickup feels correct.
    if (strncmp(k_boots[idx].cmd_key, "Tier_", 5) != 0) {
        int armor = k_boots[idx].rarity;          // rarity 0=Common→0, 1=Uncommon→1, …, 4=Legendary→4
        if (armor > 0) {
            snprintf(buf, sizeof(buf),
                "ApplyPlayerEffect GameplayEffect:BP_Effect_Player_Adjust_Armor %d", armor);
            dispatch_ascii(buf);
        }
    }
}

static void spawn_belt(int idx, int amt)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "SpawnBelt Loot:BP_Item_Belt_%s %d", k_belts[idx].cmd_key, amt);
    dispatch_ascii(buf);
}

// ── Loadout system ────────────────────────────────────────────────────────────

struct Loadout {
    char name[64];
    bool valid;
    int  class_idx;       // -1=unset, 0-5
    bool has_offhand;
    int  off_elem, off_tier;
    bool has_rune;
    int  rune_idx, rune_tier;
    int  amulet_idx;      // -1=none
    int  boots_idx;
    int  belt_idx;
    int  mind_perk;       // -1=none, 0+ index into k_mind_perks
    int  body_perk;
    int  spirit_perk;
    int  level_ups;
    bool upgrade_mind, upgrade_body, upgrade_spirit;
    char boss_cmd[1024];
    // v3 additions — old save files will mismatch sizeof and be discarded.
    bool has_primary_tier; // spawn primary gauntlet at a specific tier
    int  pri_tier;         // 0-4 (Common–Legendary)
    bool has_offhand2;     // second offhand gauntlet (Spellslinger / future default)
    int  off2_elem, off2_tier;
};

static Loadout g_loadouts[10];
static int     g_loadout_sel = 0;

static void reset_loadout_slot(int i)
{
    Loadout &l = g_loadouts[i];
    memset(&l, 0, sizeof(l));
    snprintf(l.name, sizeof(l.name), "Loadout %d", i + 1);
    l.class_idx  = l.amulet_idx = l.boots_idx = l.belt_idx = -1;
    l.mind_perk  = l.body_perk = l.spirit_perk = -1;
    l.pri_tier   = l.off2_tier = 4; // default Legendary
}

static void init_loadouts()
{
    for (int i = 0; i < 10; ++i) reset_loadout_slot(i);
}

static const char *boss_cmd_find_dangerous(const char *cmd); // defined below

static void apply_loadout(const Loadout &l)
{
    char buf[256];
    dispatch_ascii("DropEquipment");
    if (l.class_idx >= 0) {
        snprintf(buf, sizeof(buf),
            "ChooseCharacterClass CharacterClass:BP_CharacterClass_%s",
            k_classes[l.class_idx]);
        dispatch_ascii(buf);
        // Optionally re-spawn the primary gauntlet at a specific tier.
        if (l.has_primary_tier) {
            snprintf(buf, sizeof(buf), "SpawnGauntlet Loot:BP_Item_Weapon_%s_Tier_%d 1",
                k_elements[k_class_elem[l.class_idx]].cmd, l.pri_tier + 1);
            dispatch_ascii(buf);
        }
    }
    if (l.has_offhand) {
        snprintf(buf, sizeof(buf), "SpawnGauntlet Loot:BP_Item_Weapon_%s_Tier_%d 1",
            k_elements[l.off_elem].cmd, l.off_tier + 1);
        dispatch_ascii(buf);
    }
    if (l.has_offhand2) {
        snprintf(buf, sizeof(buf), "SpawnGauntlet Loot:BP_Item_Weapon_%s_Tier_%d 1",
            k_elements[l.off2_elem].cmd, l.off2_tier + 1);
        dispatch_ascii(buf);
    }
    if (l.has_rune) {
        if (k_runes[l.rune_idx].has_tier)
            snprintf(buf, sizeof(buf), "SpawnRune Loot:BP_Item_Rune_%s_Tier_%d 1",
                k_runes[l.rune_idx].cmd, l.rune_tier + 1);
        else
            snprintf(buf, sizeof(buf), "SpawnRune Loot:BP_Item_Rune_%s 1",
                k_runes[l.rune_idx].cmd);
        dispatch_ascii(buf);
    }
    if (l.amulet_idx >= 0) spawn_amulet(l.amulet_idx, 1);
    if (l.boots_idx  >= 0) spawn_boot(l.boots_idx, 1);
    if (l.belt_idx   >= 0) spawn_belt(l.belt_idx, 1);

    dispatch_ascii("ResetCharacterPerks");
    if (l.mind_perk >= 0) {
        snprintf(buf, sizeof(buf), "ChooseCharacterPerk CharacterPerk:BP_Perk_%s",
            k_mind_perks[l.mind_perk].cmd);
        dispatch_ascii(buf);
    }
    if (l.body_perk >= 0) {
        snprintf(buf, sizeof(buf), "ChooseCharacterPerk CharacterPerk:BP_Perk_%s",
            k_body_perks[l.body_perk].cmd);
        dispatch_ascii(buf);
    }
    if (l.spirit_perk >= 0) {
        snprintf(buf, sizeof(buf), "ChooseCharacterPerk CharacterPerk:BP_Perk_%s",
            k_spirit_perks[l.spirit_perk].cmd);
        dispatch_ascii(buf);
    }
    for (int i = 0; i < l.level_ups; ++i)
        dispatch_ascii("LevelUpCharacterClass");
    // UpgradeCharacterPerk advances perk rank by 1 (requires a scroll).
    // Call 3× to reach max rank (rank 1→4 needs 3 upgrades).
    if (l.upgrade_mind)   for (int i = 0; i < 3; ++i) dispatch_ascii("UpgradeCharacterPerk Mind");
    if (l.upgrade_body)   for (int i = 0; i < 3; ++i) dispatch_ascii("UpgradeCharacterPerk Body");
    if (l.upgrade_spirit) for (int i = 0; i < 3; ++i) dispatch_ascii("UpgradeCharacterPerk Spirit");
    if (l.boss_cmd[0] && !boss_cmd_find_dangerous(l.boss_cmd))
        dispatch_ascii(l.boss_cmd);
}

// ── Boss command safety ───────────────────────────────────────────────────────

static const char *k_boss_cmd_blocked[] = {
    "God", "FastCooldowns", "ToggleHUD", "ToggleDebugCamera",
    "SetNoMatchBotAggro", "Superspeed", nullptr,
};

// Returns the first blocked keyword found, or nullptr if the command is safe.
static const char *boss_cmd_find_dangerous(const char *cmd)
{
    for (int i = 0; k_boss_cmd_blocked[i]; ++i) {
        const char *kw = k_boss_cmd_blocked[i];
        const char *p  = cmd;
        while (*p) {
            if (strncmp(p, kw, strlen(kw)) == 0) {
                char after = p[strlen(kw)];
                if (after == '\0' || after == ' ' || after == '\t')
                    return kw;
            }
            ++p;
        }
    }
    return nullptr;
}

// Parse a boss command string and apply recognisable loadout commands to l.
// Handles ChooseCharacterClass, SpawnGauntlet, SpawnRune, SpawnAmulet, SpawnBoot,
// SpawnBelt, and ChooseCharacterPerk directives.
static void parse_boss_cmd_into_loadout(Loadout &l, const char *cmd)
{
    char tmp[1024]; strncpy(tmp, cmd, sizeof(tmp) - 1); tmp[sizeof(tmp)-1] = '\0';
    char *tok = tmp;
    while (*tok) {
        // Skip whitespace / leading junk
        while (*tok == ' ' || *tok == '\t') ++tok;
        if (!*tok) break;
        // Find end of "word" (first space after non-space)
        char *end = tok;
        while (*end && *end != ' ' && *end != '\t') ++end;
        char saved = *end; *end = '\0';
        const char *word = tok;
        tok = end; if (saved) ++tok;

        // Collect remainder of this logical command (until next recognised keyword)
        // For simplicity: treat everything as one flat token stream.
        // ChooseCharacterClass CharacterClass:BP_CharacterClass_<X>
        if (strcmp(word, "ChooseCharacterClass") == 0) {
            while (*tok == ' ') ++tok;
            const char *pfx = "CharacterClass:BP_CharacterClass_";
            if (strncmp(tok, pfx, strlen(pfx)) == 0) {
                const char *cls = tok + strlen(pfx);
                char *cend = tok; while (*cend && *cend != ' ') ++cend; *cend = '\0'; tok = cend + (*cend ? 1 : 0);
                for (int i = 0; i < k_class_count; ++i) {
                    if (strcmp(cls, k_classes[i]) == 0) { l.class_idx = i; break; }
                }
            }
        }
        // SpawnGauntlet Loot:BP_Item_Weapon_<ELEM>_Tier_<N>
        else if (strcmp(word, "SpawnGauntlet") == 0) {
            while (*tok == ' ') ++tok;
            const char *pfx = "Loot:BP_Item_Weapon_";
            if (strncmp(tok, pfx, strlen(pfx)) == 0) {
                char piece[64]; strncpy(piece, tok + strlen(pfx), sizeof(piece)-1);
                piece[sizeof(piece)-1] = '\0';
                char *sp = strchr(piece, ' '); if (sp) *sp = '\0';
                tok += strlen(pfx) + strlen(piece) + (sp ? 1 : 0);
                // piece = "<ELEM>_Tier_<N>"
                for (int i = 0; i < k_elem_count; ++i) {
                    size_t elen = strlen(k_elements[i].cmd);
                    if (strncmp(piece, k_elements[i].cmd, elen) == 0 && piece[elen] == '_') {
                        int tier = atoi(piece + elen + strlen("Tier_")) - 1;
                        if (tier >= 0 && tier <= 4) {
                            if (!l.has_offhand) {
                                l.has_offhand = true; l.off_elem = i; l.off_tier = tier;
                            } else if (!l.has_offhand2) {
                                l.has_offhand2 = true; l.off2_elem = i; l.off2_tier = tier;
                            }
                        }
                        break;
                    }
                }
            }
        }
        // SpawnRune Loot:BP_Item_Rune_<X>[_Tier_<N>]
        else if (strcmp(word, "SpawnRune") == 0) {
            while (*tok == ' ') ++tok;
            const char *pfx = "Loot:BP_Item_Rune_";
            if (strncmp(tok, pfx, strlen(pfx)) == 0) {
                char piece[64]; strncpy(piece, tok + strlen(pfx), sizeof(piece)-1);
                piece[sizeof(piece)-1] = '\0';
                char *sp = strchr(piece, ' '); if (sp) *sp = '\0';
                tok += strlen(pfx) + strlen(piece) + (sp ? 1 : 0);
                for (int i = 0; i < k_rune_count; ++i) {
                    size_t rlen = strlen(k_runes[i].cmd);
                    if (strncmp(piece, k_runes[i].cmd, rlen) == 0 &&
                        (piece[rlen] == '\0' || piece[rlen] == '_')) {
                        l.has_rune = true; l.rune_idx = i;
                        if (k_runes[i].has_tier) {
                            const char *tp = strstr(piece + rlen, "Tier_");
                            l.rune_tier = tp ? atoi(tp + 5) - 1 : 4;
                        }
                        break;
                    }
                }
            }
        }
        // ChooseCharacterPerk CharacterPerk:BP_Perk_<X>
        else if (strcmp(word, "ChooseCharacterPerk") == 0) {
            while (*tok == ' ') ++tok;
            const char *pfx = "CharacterPerk:BP_Perk_";
            if (strncmp(tok, pfx, strlen(pfx)) == 0) {
                char piece[64]; strncpy(piece, tok + strlen(pfx), sizeof(piece)-1);
                piece[sizeof(piece)-1] = '\0';
                char *sp = strchr(piece, ' '); if (sp) *sp = '\0';
                tok += strlen(pfx) + strlen(piece) + (sp ? 1 : 0);
                bool found = false;
                for (int i = 0; i < k_mind_count && !found; ++i)
                    if (strcmp(piece, k_mind_perks[i].cmd) == 0) { l.mind_perk = i; found = true; }
                for (int i = 0; i < k_body_count && !found; ++i)
                    if (strcmp(piece, k_body_perks[i].cmd) == 0) { l.body_perk = i; found = true; }
                for (int i = 0; i < k_spirit_count && !found; ++i)
                    if (strcmp(piece, k_spirit_perks[i].cmd) == 0) { l.spirit_perk = i; found = true; }
            }
        }
        else {
            // Unknown word — skip to next space (it's an argument of something already processed)
            while (*tok && *tok != ' ') ++tok;
        }
    }
}

// ── Per-section state ─────────────────────────────────────────────────────────

// Match
static int  s_num_bots = 5;
static int  s_bot_diff = 2;
static bool s_boons    = true;
static bool s_zones    = true;
static bool s_npcs     = true;

// Items
static int  s_off_elem = 0, s_off_tier = 4, s_off_amt = 1;
static int  s_rune_idx = 0, s_r_tier   = 4, s_r_amt   = 1;
static int  s_am_idx   = 0, s_am_amt   = 1;
static int  s_bt_idx   = 0, s_bt_amt   = 1;
static int  s_belt_idx = 0, s_belt_amt = 1;
static int  s_mind_perk_idx    = 0;
static int  s_body_perk_idx    = 0;
static int  s_spirit_perk_idx  = 0;
// Which perk is currently applied per tree (-1 = none). Derived total replaces accumulator.
static int  s_mind_perk_active   = -1;
static int  s_body_perk_active   = -1;
static int  s_spirit_perk_active = -1;

// Staff
static bool  s_god_mode           = false;
static bool  s_fast_cooldowns     = false;
static bool  s_toggle_hud         = false;
static bool  s_toggle_debug_cam   = false;
static bool  s_allow_round_end    = true;
static bool  s_no_aggro           = false;
static float s_superspeed      = 1.0f;
static int   s_arena_idx       = 0;
static int   s_friend_pts      = 100;
static int   s_enemy_pts       = 100;
static char  s_staff_cmd[512]  = "";

// Dev
static char  s_effect_buf[256] = "";

// ── Row helpers ───────────────────────────────────────────────────────────────

static bool row(int idx, const char *label)
{
    bool is_sel = g_in_right && g_item == idx;
    if (is_sel)
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(45, 65, 125, 210));
    ImGui::TableSetColumnIndex(0);
    char sel_id[16];
    snprintf(sel_id, sizeof(sel_id), "##s%d", idx);
    bool clicked = ImGui::Selectable(sel_id, false,
        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
        ImVec2(0, ImGui::GetFrameHeight()));
    if (clicked) { g_item = idx; g_in_right = true; }
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::TextUnformatted(label);
    return is_sel;
}

static bool kbd_enter(int idx)
{
    return g_in_right && g_item == idx
        && !g_enter_consumed
        && !ImGui::IsAnyItemActive()
        && ImGui::IsKeyPressed(ImGuiKey_Enter, false);
}

// ── Section: Match Settings ───────────────────────────────────────────────────

static void draw_match()
{
    if (!ImGui::BeginTable("##match", 2, ImGuiTableFlags_None)) return;
    ImGui::TableSetupColumn("lbl",  ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("ctrl", ImGuiTableColumnFlags_WidthStretch);

    ImGui::TableNextRow();
    bool sel0 = row(0, "Start Match");
    ImGui::TableSetColumnIndex(1);
    if (ImGui::Button("Run##sm", ImVec2(-1, 0)) || (sel0 && kbd_enter(0)))
        dispatch_ascii("StartMatch");

    const bool is_dev   = g_is_dev.load();
    const bool is_staff = g_is_staff.load();
    const int  bot_max  = is_dev ? 9999 : (is_staff ? 100 : 42);
    if (s_num_bots > bot_max) s_num_bots = bot_max;

    ImGui::TableNextRow();
    row(1, "Num Bots");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-65.0f);
    char bot_fmt[32]; snprintf(bot_fmt, sizeof(bot_fmt), "%%d / %d", bot_max);
    ImGui::DragInt("##nb", &s_num_bots, 0.2f, 0, bot_max, bot_fmt);
    ImGui::SameLine();
    if (ImGui::Button("Set##nb", ImVec2(-1,0))) {
        char buf[64]; snprintf(buf, sizeof(buf), "SetNumMatchBots %d", s_num_bots);
        dispatch_ascii(buf);
    }

    ImGui::TableNextRow();
    row(2, "Bot Difficulty");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-65.0f);
    ImGui::Combo("##bd", &s_bot_diff, k_bot_diffs, 5);
    ImGui::SameLine();
    if (ImGui::Button("Set##bd", ImVec2(-1,0))) {
        char buf[64]; snprintf(buf, sizeof(buf), "SetMatchBotDifficulty %d", s_bot_diff);
        dispatch_ascii(buf);
    }

    ImGui::TableNextRow();
    bool sel3 = row(3, "Spawn Bot");
    ImGui::TableSetColumnIndex(1);
    if (ImGui::Button("Spawn##sb", ImVec2(-1,0)) || (sel3 && kbd_enter(3)))
        dispatch_ascii("SpawnMatchBot");

    // ── Stop Countdown ──
    ImGui::TableNextRow();
    bool sel4 = row(4, "Stop Countdown");
    ImGui::TableSetColumnIndex(1);
    if (ImGui::Button("Run##stcd", ImVec2(-1,0)) || (sel4 && kbd_enter(4)))
        dispatch_ascii("StopCountdown");

    // ── Dominion toggles ──
    auto tog_match = [&](int idx, const char *lbl, bool &state,
                         const char *on_cmd, const char *off_cmd) {
        ImGui::TableNextRow();
        bool sel = row(idx, lbl);
        ImGui::TableSetColumnIndex(1);
        char id[32]; snprintf(id, sizeof(id), "%s###mt%d", state ? "ON" : "OFF", idx);
        if (ImGui::Button(id, ImVec2(-1,0)) || (sel && kbd_enter(idx))) {
            state = !state;
            dispatch_ascii(state ? on_cmd : off_cmd);
        }
    };

    tog_match(5, "Boons", s_boons, "ToggleBoons 1", "ToggleBoons 0");
    tog_match(6, "Zones", s_zones, "ToggleZones 1", "ToggleZones 0");
    tog_match(7, "NPCs",  s_npcs,  "ToggleNPCs 1",  "ToggleNPCs 0");

    ImGui::EndTable();
}

// ── Section: Item Spawner ─────────────────────────────────────────────────────

static void draw_items()
{
    if (!ImGui::BeginTable("##items", 2, ImGuiTableFlags_None)) return;
    ImGui::TableSetupColumn("lbl",  ImGuiTableColumnFlags_WidthFixed, 120.0f);
    ImGui::TableSetupColumn("ctrl", ImGuiTableColumnFlags_WidthStretch);

    // ── Offhand Gauntlet ──
    {
        const char *en[6];
        for (int i = 0; i < k_elem_count; ++i) en[i] = k_elements[i].display;

        ImGui::TableNextRow();
        bool sel = row(0, "Offhand Gauntlet");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(70.0f); ImGui::Combo("##ge", &s_off_elem, en, k_elem_count);
        ImGui::SameLine();
        rarity_combo("##gt", &s_off_tier);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(40.0f); ImGui::DragInt("##ga", &s_off_amt, 0.1f, 1, 99, "x%d");
        ImGui::SameLine();
        if (ImGui::Button("Spawn##g") || (sel && kbd_enter(0))) {
            char buf[128];
            snprintf(buf, sizeof(buf), "SpawnGauntlet Loot:BP_Item_Weapon_%s_Tier_%d %d",
                k_elements[s_off_elem].cmd, s_off_tier + 1, s_off_amt);
            dispatch_ascii(buf);
        }
    }

    // ── Rune ──
    {
        const char *rn[k_rune_count];
        for (int i = 0; i < k_rune_count; ++i) rn[i] = k_runes[i].display;

        ImGui::TableNextRow();
        bool sel = row(1, "Rune");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(100.0f); ImGui::Combo("##rn", &s_rune_idx, rn, k_rune_count);
        if (k_runes[s_rune_idx].has_tier) {
            ImGui::SameLine();
            rarity_combo("##rt", &s_r_tier);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(40.0f); ImGui::DragInt("##ra", &s_r_amt, 0.1f, 1, 99, "x%d");
        ImGui::SameLine();
        if (ImGui::Button("Spawn##r") || (sel && kbd_enter(1))) {
            char buf[128];
            if (k_runes[s_rune_idx].has_tier)
                snprintf(buf, sizeof(buf), "SpawnRune Loot:BP_Item_Rune_%s_Tier_%d %d",
                    k_runes[s_rune_idx].cmd, s_r_tier + 1, s_r_amt);
            else
                snprintf(buf, sizeof(buf), "SpawnRune Loot:BP_Item_Rune_%s %d",
                    k_runes[s_rune_idx].cmd, s_r_amt);
            dispatch_ascii(buf);
        }
    }

    // ── Amulet ──
    {
        ImGui::TableNextRow();
        bool sel = row(2, "Amulet");
        ImGui::TableSetColumnIndex(1);
        equip_combo("##am", &s_am_idx, k_amulets, k_amulet_count, 220.0f);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(40.0f); ImGui::DragInt("##ama", &s_am_amt, 0.1f, 1, 99, "x%d");
        ImGui::SameLine();
        if (ImGui::Button("Spawn##am") || (sel && kbd_enter(2)))
            spawn_amulet(s_am_idx, s_am_amt);
    }

    // ── Boot ──
    {
        ImGui::TableNextRow();
        bool sel = row(3, "Boot");
        ImGui::TableSetColumnIndex(1);
        equip_combo("##bt", &s_bt_idx, k_boots, k_boot_count, 220.0f);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(40.0f); ImGui::DragInt("##bta", &s_bt_amt, 0.1f, 1, 99, "x%d");
        ImGui::SameLine();
        if (ImGui::Button("Spawn##bt") || (sel && kbd_enter(3)))
            spawn_boot(s_bt_idx, s_bt_amt);
    }

    // ── Belt ──
    {
        ImGui::TableNextRow();
        bool sel = row(4, "Belt");
        ImGui::TableSetColumnIndex(1);
        equip_combo("##blt", &s_belt_idx, k_belts, k_belt_count, 220.0f);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(40.0f); ImGui::DragInt("##blta", &s_belt_amt, 0.1f, 1, 99, "x%d");
        ImGui::SameLine();
        if (ImGui::Button("Spawn##blt") || (sel && kbd_enter(4)))
            spawn_belt(s_belt_idx, s_belt_amt);
    }

    // ── Set Class (6 buttons, 2 rows of 3) ──
    {
        ImGui::TableNextRow();
        row(5, "Set Class");
        ImGui::TableSetColumnIndex(1);
        const float w = (ImGui::GetContentRegionAvail().x - 8.0f) / 3.0f;
        for (int i = 0; i < 3; ++i) {
            if (i > 0) ImGui::SameLine(0.0f, 4.0f);
            char id[48]; snprintf(id, sizeof(id), "%s##c%d", k_classes[i], i);
            if (ImGui::Button(id, ImVec2(w, 0))) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                    "ChooseCharacterClass CharacterClass:BP_CharacterClass_%s", k_classes[i]);
                dispatch_ascii(buf);
            }
        }
        for (int i = 3; i < k_class_count; ++i) {
            if (i > 3) ImGui::SameLine(0.0f, 4.0f);
            char id[48]; snprintf(id, sizeof(id), "%s##c%d", k_classes[i], i);
            if (ImGui::Button(id, ImVec2(w, 0))) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                    "ChooseCharacterClass CharacterClass:BP_CharacterClass_%s", k_classes[i]);
                dispatch_ascii(buf);
            }
        }
    }

    // ── Level Up Class (Once / Max 3x) ──
    {
        ImGui::TableNextRow();
        bool sel = row(6, "Level Up Class");
        ImGui::TableSetColumnIndex(1);
        const float w2 = (ImGui::GetContentRegionAvail().x - 4.0f) / 2.0f;
        if (ImGui::Button("Once##lu1", ImVec2(w2, 0)) || (sel && kbd_enter(6)))
            dispatch_ascii("LevelUpCharacterClass");
        ImGui::SameLine(0.0f, 4.0f);
        if (ImGui::Button("Max (3x)##lu3", ImVec2(w2, 0))) {
            dispatch_ascii("LevelUpCharacterClass");
            dispatch_ascii("LevelUpCharacterClass");
            dispatch_ascii("LevelUpCharacterClass");
        }
    }

    // ── Upgrade Perks ──
    {
        ImGui::TableNextRow();
        row(7, "Upgrade Perks");
        ImGui::TableSetColumnIndex(1);
        const float w = (ImGui::GetContentRegionAvail().x - 8.0f) / 3.0f;
        if (ImGui::Button("Mind##up",   ImVec2(w, 0))) dispatch_ascii("UpgradeCharacterPerk Mind");
        ImGui::SameLine(0.0f, 4.0f);
        if (ImGui::Button("Body##up",   ImVec2(w, 0))) dispatch_ascii("UpgradeCharacterPerk Body");
        ImGui::SameLine(0.0f, 4.0f);
        if (ImGui::Button("Spirit##up", ImVec2(w, 0))) dispatch_ascii("UpgradeCharacterPerk Spirit");
    }

    // ── Add Perk (tree-based with cost tracking) ──
    // Auto-applies as soon as the combo selection changes.  Resets all trees
    // first so switching one perk never clobbers the others.
    auto perk_add_row = [&](int row_idx, const char *tree_lbl,
                             const char *combo_id,
                             const PerkInfo *perks, int count,
                             int &sel_idx, int &active_idx, int tree_idx) {
        ImGui::TableNextRow();
        bool sel = row(row_idx, tree_lbl);
        ImGui::TableSetColumnIndex(1);

        const char *names[12];
        for (int i = 0; i < count; ++i) names[i] = perks[i].display;

        ImGui::SetNextItemWidth(-1.0f);
        bool changed = ImGui::Combo(combo_id, &sel_idx, names, count);
        if (changed || (sel && kbd_enter(row_idx))) {
            dispatch_ascii("ResetCharacterPerks");
            const PerkInfo *all_trees[] = { k_mind_perks, k_body_perks, k_spirit_perks };
            int all_active[] = { s_mind_perk_active, s_body_perk_active, s_spirit_perk_active };
            all_active[tree_idx] = sel_idx;
            for (int t = 0; t < 3; ++t) {
                if (all_active[t] >= 0) {
                    char buf[128];
                    snprintf(buf, sizeof(buf), "ChooseCharacterPerk CharacterPerk:BP_Perk_%s",
                        all_trees[t][all_active[t]].cmd);
                    dispatch_ascii(buf);
                }
            }
            active_idx = sel_idx;
        }
    };

    perk_add_row(8,  "Mind Perk",   "##prkm",
                 k_mind_perks,   k_mind_count,   s_mind_perk_idx,   s_mind_perk_active, 0);
    perk_add_row(9,  "Body Perk",   "##prkb",
                 k_body_perks,   k_body_count,   s_body_perk_idx,   s_body_perk_active, 1);
    perk_add_row(10, "Spirit Perk", "##prks",
                 k_spirit_perks, k_spirit_count, s_spirit_perk_idx, s_spirit_perk_active, 2);

    // ── Points tracker — derived from active perks, not accumulated ──
    {
        int pts = 0;
        if (s_mind_perk_active   >= 0) pts += k_mind_perks[s_mind_perk_active].cost;
        if (s_body_perk_active   >= 0) pts += k_body_perks[s_body_perk_active].cost;
        if (s_spirit_perk_active >= 0) pts += k_spirit_perks[s_spirit_perk_active].cost;

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("Points Used");
        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text,
            pts > 7 ? IM_COL32(255, 80, 80, 255) : IM_COL32(120, 220, 120, 255));
        ImGui::Text("%d / 7", pts);
        ImGui::PopStyleColor();
    }

    // ── Reset Perks ──
    {
        ImGui::TableNextRow();
        bool sel = row(11, "Reset Perks");
        ImGui::TableSetColumnIndex(1);
        if (ImGui::Button("Reset##rp", ImVec2(-1,0)) || (sel && kbd_enter(11))) {
            dispatch_ascii("ResetCharacterPerks");
            s_mind_perk_active   = -1;
            s_body_perk_active   = -1;
            s_spirit_perk_active = -1;
        }
    }

    ImGui::EndTable();
}

// ── Persistence ──────────────────────────────────────────────────────────────

static void get_dll_dir(char *out, int sz)
{
    HMODULE hm = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&devmenu_init), &hm);
    GetModuleFileNameA(hm, out, sz);
    char *sep = strrchr(out, '\\');
    if (sep) *(sep + 1) = '\0';
    else     out[0]     = '\0';
}

static void save_loadouts()
{
    char dir[MAX_PATH]; get_dll_dir(dir, sizeof(dir));
    char path[MAX_PATH]; snprintf(path, sizeof(path), "%sdevmenu_loadouts.bin", dir);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(g_loadouts, sizeof(g_loadouts), 1, f);
    fclose(f);
}

static void load_loadouts()
{
    char dir[MAX_PATH]; get_dll_dir(dir, sizeof(dir));
    char path[MAX_PATH]; snprintf(path, sizeof(path), "%sdevmenu_loadouts.bin", dir);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    // Discard file if size doesn't match — struct changed between versions.
    long sz = 0;
    if (fseek(f, 0, SEEK_END) == 0) sz = ftell(f);
    rewind(f);
    if (sz == (long)sizeof(g_loadouts))
        fread(g_loadouts, sizeof(g_loadouts), 1, f);
    fclose(f);
}

static void save_hotkeys()
{
    char dir[MAX_PATH]; get_dll_dir(dir, sizeof(dir));
    char path[MAX_PATH]; snprintf(path, sizeof(path), "%sdevmenu_hotkeys.bin", dir);
    FILE *f = fopen(path, "wb");
    if (f) {
        int n = k_hotkey_count;
        fwrite(&n, sizeof(n), 1, f);
        for (int i = 0; i < k_hotkey_count; ++i) {
            fwrite(&g_hotkeys[i].vk,          sizeof(UINT), 1, f);
            fwrite(&g_hotkeys[i].mods,        sizeof(UINT), 1, f);
            fwrite(&g_hotkeys[i].vk2,         sizeof(UINT), 1, f);
            fwrite(&g_hotkeys[i].ctrl_btn,    sizeof(WORD), 1, f);
            fwrite(&g_hotkeys[i].ctrl_player, sizeof(BYTE), 1, f);
        }
        fclose(f);
    } else {
        dbg("save_hotkeys: fopen failed — callback will still fire");
    }

    // Fire callback regardless of file write outcome so the LL hook
    // always picks up the new key immediately in the current session.
    {
        char lbuf[80];
        snprintf(lbuf, sizeof(lbuf), "save_hotkeys: cb=%p vk=0x%02X mods=%u",
                 (void*)g_menu_key_cb,
                 g_hotkeys[k_menu_key_idx].vk,
                 g_hotkeys[k_menu_key_idx].mods);
        dbg(lbuf);
    }
    if (g_menu_key_cb)
        g_menu_key_cb(g_hotkeys[k_menu_key_idx].vk,
                      g_hotkeys[k_menu_key_idx].mods);
}

static void load_hotkeys()
{
    char dir[MAX_PATH]; get_dll_dir(dir, sizeof(dir));
    char path[MAX_PATH]; snprintf(path, sizeof(path), "%sdevmenu_hotkeys.bin", dir);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    int n = 0;
    if (fread(&n, sizeof(n), 1, f) != 1) { fclose(f); return; }
    // Detect format by file size: old=12 bytes/entry (vk+mods+vk2), new=19 bytes/entry (+ctrl_btn+ctrl_player)
    long file_sz = 0;
    if (fseek(f, 0, SEEK_END) == 0) file_sz = ftell(f);
    rewind(f); fread(&n, sizeof(n), 1, f);  // re-read count after rewind
    bool has_ctrl = (file_sz >= (long)(sizeof(int) + n * (3*sizeof(UINT) + sizeof(WORD) + sizeof(BYTE))));
    int to_load = n < k_hotkey_count ? n : k_hotkey_count;
    for (int i = 0; i < to_load; ++i) {
        fread(&g_hotkeys[i].vk,   sizeof(UINT), 1, f);
        fread(&g_hotkeys[i].mods, sizeof(UINT), 1, f);
        fread(&g_hotkeys[i].vk2,  sizeof(UINT), 1, f);
        if (has_ctrl) {
            fread(&g_hotkeys[i].ctrl_btn,    sizeof(WORD), 1, f);
            fread(&g_hotkeys[i].ctrl_player, sizeof(BYTE), 1, f);
        }
    }
    fclose(f);
}

// ── VK key name helper ────────────────────────────────────────────────────────

static const char *vk_name(UINT vk)
{
    static char buf[16];
    if (vk == 0) return "(none)";
    if (vk >= 'A' && vk <= 'Z') { buf[0] = (char)vk; buf[1] = 0; return buf; }
    if (vk >= '0' && vk <= '9') { buf[0] = (char)vk; buf[1] = 0; return buf; }
    if (vk >= VK_F1 && vk <= VK_F12) {
        snprintf(buf, sizeof(buf), "F%d", vk - VK_F1 + 1); return buf;
    }
    switch (vk) {
    case VK_SPACE:      return "Space";
    case VK_TAB:        return "Tab";
    case VK_INSERT:     return "Ins";
    case VK_DELETE:     return "Del";
    case VK_HOME:       return "Home";
    case VK_END:        return "End";
    case VK_PRIOR:      return "PgUp";
    case VK_NEXT:       return "PgDn";
    case VK_OEM_1:      return ";";
    case VK_OEM_2:      return "/";
    case VK_OEM_3:      return "`";
    case VK_OEM_4:      return "[";
    case VK_OEM_5:      return "\\";
    case VK_OEM_6:      return "]";
    case VK_OEM_7:      return "'";
    case VK_OEM_MINUS:  return "-";
    case VK_OEM_PLUS:   return "=";
    case VK_OEM_COMMA:  return ",";
    case VK_OEM_PERIOD: return ".";
    }
    snprintf(buf, sizeof(buf), "0x%02X", vk); return buf;
}

// ── Section: Character Controls ───────────────────────────────────────────────

static void draw_char()
{
    if (!ImGui::BeginTable("##char", 2, ImGuiTableFlags_None)) return;
    ImGui::TableSetupColumn("lbl",  ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("ctrl", ImGuiTableColumnFlags_WidthStretch);

    auto drop_row = [&](int idx, const char *lbl, const char *btn_id, const char *cmd) {
        ImGui::TableNextRow();
        bool sel = row(idx, lbl);
        ImGui::TableSetColumnIndex(1);
        if (ImGui::Button(btn_id, ImVec2(-1, 0)) || (sel && kbd_enter(idx)))
            dispatch_ascii(cmd);
    };

    drop_row(0, "Drop Amulet",    "Drop##dam", "DropEquipment Amulet");
    drop_row(1, "Drop Belt",      "Drop##dbl", "DropEquipment Belt");
    drop_row(2, "Drop Boots",     "Drop##dbt", "DropEquipment Boots");
    drop_row(3, "Drop Rune",      "Drop##drn", "DropEquipment Rune");
    drop_row(4, "Drop Secondary", "Drop##dsc", "DropEquipment Weapon.Secondary");

    ImGui::TableNextRow();
    bool sel5 = row(5, "Drop All");
    ImGui::TableSetColumnIndex(1);
    if (ImGui::Button("Drop All##dall", ImVec2(-1, 0)) || (sel5 && kbd_enter(5)))
        hk_drop_all();

    ImGui::TableNextRow();
    bool sel6 = row(6, "Heal");
    ImGui::TableSetColumnIndex(1);
    if (ImGui::Button("Heal##hl", ImVec2(-1, 0)) || (sel6 && kbd_enter(6)))
        heal_player();

    ImGui::TableNextRow();
    bool sel7 = row(7, "Die");
    ImGui::TableSetColumnIndex(1);
    if (ImGui::Button("Die##di", ImVec2(-1, 0)) || (sel7 && kbd_enter(7)))
        dispatch_ascii("Die");

    ImGui::EndTable();
}

// ── Section: Hotkeys ──────────────────────────────────────────────────────────

static const char *hotkey_label(const HotkeyDef &hk)
{
    static char buf[64];
    if (hk.vk == 0) return "(none)";
    buf[0] = '\0';
    if (hk.mods & 2) strncat(buf, "Ctrl+",  sizeof(buf) - 1);
    if (hk.mods & 1) strncat(buf, "Shift+", sizeof(buf) - 1);
    if (hk.mods & 4) strncat(buf, "Alt+",   sizeof(buf) - 1);
    strncat(buf, vk_name(hk.vk), sizeof(buf) - 1);
    if (hk.vk2) {
        strncat(buf, "+", sizeof(buf) - 1);
        strncat(buf, vk_name(hk.vk2), sizeof(buf) - 1);
    }
    return buf;
}

static void draw_hotkeys()
{
    ImGui::TextDisabled("Bound keys fire when the menu is closed.  Esc cancels binding.");
    ImGui::Spacing();

    if (!ImGui::BeginTabBar("##hktabs")) return;

    // ── Keyboard tab ──────────────────────────────────────────────────────────
    if (ImGui::BeginTabItem("Keyboard")) {
        if (ImGui::BeginTable("##hktbl", 3, ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Key",    ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("##btns", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableHeadersRow();

            for (int i = 0; i < k_hotkey_count; ++i) {
                if (i == k_menu_key_idx) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Spacing(); ImGui::TextDisabled("-- Menu --"); ImGui::Spacing();
                }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(g_hotkeys[i].label);

                ImGui::TableSetColumnIndex(1);
                bool listening = (g_listening_idx == i);
                if (listening) {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 200, 50, 255));
                    ImGui::TextUnformatted("(press key...)");
                    ImGui::PopStyleColor();
                } else {
                    ImGui::TextUnformatted(hotkey_label(g_hotkeys[i]));
                }

                ImGui::TableSetColumnIndex(2);
                char bind_id[20], clr_id[20];
                snprintf(bind_id, sizeof(bind_id), "Bind##kbhk%d",  i);
                snprintf(clr_id,  sizeof(clr_id),  "Clear##kbhk%d", i);
                if (listening) {
                    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(120, 80, 0, 255));
                    ImGui::Button(bind_id, ImVec2(50, 0));
                    ImGui::PopStyleColor();
                } else {
                    if (ImGui::Button(bind_id, ImVec2(50, 0))) {
                        g_ctrl_listening_idx = -1;
                        g_listening_idx = i;
                    }
                }
                ImGui::SameLine(0, 4);
                ImGui::BeginDisabled(g_hotkeys[i].vk == 0);
                if (ImGui::Button(clr_id, ImVec2(-1, 0))) {
                    g_hotkeys[i].vk = 0; g_hotkeys[i].mods = 0; g_hotkeys[i].vk2 = 0;
                    if (g_listening_idx == i) g_listening_idx = -1;
                    save_hotkeys();
                }
                ImGui::EndDisabled();
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    // ── Controller tab ────────────────────────────────────────────────────────
    if (ImGui::BeginTabItem("Controller")) {
        ImGui::TextDisabled("Requires an XInput (Xbox) controller.  Esc cancels binding.");
        ImGui::Spacing();

        if (ImGui::BeginTable("##ctrltbl", 3, ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Action",  ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Button",  ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("##cbtns", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableHeadersRow();

            for (int i = 0; i < k_hotkey_count; ++i) {
                if (!g_hotkeys[i].action) continue;  // Skip menu-open entry
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(g_hotkeys[i].label);

                ImGui::TableSetColumnIndex(1);
                bool clistening = (g_ctrl_listening_idx == i);
                if (clistening) {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 200, 50, 255));
                    ImGui::TextUnformatted("(press...)");
                    ImGui::PopStyleColor();
                } else if (g_hotkeys[i].ctrl_btn) {
                    char lbl[32];
                    snprintf(lbl, sizeof(lbl), "P%d: %s",
                             g_hotkeys[i].ctrl_player + 1,
                             ctrl_btn_name(g_hotkeys[i].ctrl_btn));
                    ImGui::TextUnformatted(lbl);
                } else {
                    ImGui::TextDisabled("(none)");
                }

                ImGui::TableSetColumnIndex(2);
                char bind_id[20], clr_id[20];
                snprintf(bind_id, sizeof(bind_id), "Bind##chk%d",  i);
                snprintf(clr_id,  sizeof(clr_id),  "Clear##chk%d", i);
                if (clistening) {
                    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(120, 80, 0, 255));
                    ImGui::Button(bind_id, ImVec2(50, 0));
                    ImGui::PopStyleColor();
                } else {
                    if (ImGui::Button(bind_id, ImVec2(50, 0))) {
                        g_listening_idx = -1;
                        g_ctrl_listening_idx = i;
                    }
                }
                ImGui::SameLine(0, 4);
                ImGui::BeginDisabled(g_hotkeys[i].ctrl_btn == 0);
                if (ImGui::Button(clr_id, ImVec2(-1, 0))) {
                    g_hotkeys[i].ctrl_btn    = 0;
                    g_hotkeys[i].ctrl_player = 0;
                    if (g_ctrl_listening_idx == i) g_ctrl_listening_idx = -1;
                    save_hotkeys();
                }
                ImGui::EndDisabled();
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
}

// ── Section: Loadouts ─────────────────────────────────────────────────────────

static void draw_loadouts()
{
    ImGui::BeginChild("##llist", ImVec2(140.0f, 0.0f), true);
    for (int i = 0; i < 10; ++i) {
        char label[80];
        if (g_loadouts[i].valid)
            snprintf(label, sizeof(label), "%d. %s", i+1, g_loadouts[i].name);
        else
            snprintf(label, sizeof(label), "%d. (empty)", i+1);
        if (ImGui::Selectable(label, g_loadout_sel == i))
            g_loadout_sel = i;
    }
    ImGui::EndChild();

    ImGui::SameLine(0.0f, 6.0f);

    ImGui::BeginChild("##ledit", ImVec2(0.0f, 0.0f), false);
    Loadout &l = g_loadouts[g_loadout_sel];

    ImGui::Text("Slot %d  Name:", g_loadout_sel + 1);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##lname", l.name, sizeof(l.name));

    ImGui::Separator();
    ImGui::Spacing();

    // ── Class selection ──
    ImGui::TextUnformatted("Class:");
    ImGui::SameLine();
    if (l.class_idx >= 0) {
        ImGui::TextUnformatted(k_classes[l.class_idx]);
        ImGui::SameLine();
        ImGui::TextDisabled("(primary: %s)", k_elements[k_class_elem[l.class_idx]].display);
    } else {
        ImGui::TextDisabled("(none)");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear##lcc")) l.class_idx = -1;

    {
        const float w = (ImGui::GetContentRegionAvail().x - 8.0f) / 3.0f;
        for (int i = 0; i < 3; ++i) {
            if (i > 0) ImGui::SameLine(0.0f, 4.0f);
            char id[48]; snprintf(id, sizeof(id), "%s##lcls%d", k_classes[i], i);
            bool active = (l.class_idx == i);
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(50, 85, 170, 230));
            if (ImGui::Button(id, ImVec2(w, 0))) l.class_idx = i;
            if (active) ImGui::PopStyleColor();
        }
        for (int i = 3; i < k_class_count; ++i) {
            if (i > 3) ImGui::SameLine(0.0f, 4.0f);
            char id[48]; snprintf(id, sizeof(id), "%s##lcls%d", k_classes[i], i);
            bool active = (l.class_idx == i);
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(50, 85, 170, 230));
            if (ImGui::Button(id, ImVec2(w, 0))) l.class_idx = i;
            if (active) ImGui::PopStyleColor();
        }
    }

    // Primary gauntlet tier (element is fixed by class).
    if (l.class_idx >= 0) {
        ImGui::Spacing();
        ImGui::Checkbox("Primary Tier##lpt", &l.has_primary_tier);
        ImGui::SameLine();
        ImGui::BeginDisabled(!l.has_primary_tier);
        rarity_combo("##lptt", &l.pri_tier);
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", k_elements[k_class_elem[l.class_idx]].display);
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Items");
    ImGui::Spacing();

    if (ImGui::BeginTable("##lgear", 2, ImGuiTableFlags_None)) {
        ImGui::TableSetupColumn("lbl",  ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("ctrl", ImGuiTableColumnFlags_WidthStretch);

        // Offhand gauntlet
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Checkbox("Offhand##loh", &l.has_offhand);
        ImGui::TableSetColumnIndex(1);
        {
            const char *en[6]; for (int i = 0; i < k_elem_count; ++i) en[i] = k_elements[i].display;
            ImGui::BeginDisabled(!l.has_offhand);
            ImGui::SetNextItemWidth(70.0f); ImGui::Combo("##lg2e", &l.off_elem, en, k_elem_count);
            ImGui::SameLine();
            rarity_combo("##lg2t", &l.off_tier);
            ImGui::EndDisabled();
        }

        // Second offhand gauntlet (Spellslinger / future default)
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Checkbox("Offhand 2##loh2", &l.has_offhand2);
        ImGui::TableSetColumnIndex(1);
        {
            const char *en[6]; for (int i = 0; i < k_elem_count; ++i) en[i] = k_elements[i].display;
            ImGui::BeginDisabled(!l.has_offhand2);
            ImGui::SetNextItemWidth(70.0f); ImGui::Combo("##lg3e", &l.off2_elem, en, k_elem_count);
            ImGui::SameLine();
            rarity_combo("##lg3t", &l.off2_tier);
            ImGui::EndDisabled();
        }

        // Rune
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Checkbox("Rune##lrh", &l.has_rune);
        ImGui::TableSetColumnIndex(1);
        {
            const char *rn[k_rune_count]; for (int i = 0; i < k_rune_count; ++i) rn[i] = k_runes[i].display;
            ImGui::BeginDisabled(!l.has_rune);
            ImGui::SetNextItemWidth(100.0f); ImGui::Combo("##lrn", &l.rune_idx, rn, k_rune_count);
            if (k_runes[l.rune_idx].has_tier) {
                ImGui::SameLine();
                rarity_combo("##lrt", &l.rune_tier);
            }
            ImGui::EndDisabled();
        }

        // Amulet
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Amulet");
            ImGui::TableSetColumnIndex(1);
            equip_combo("##lam", &l.amulet_idx, k_amulets, k_amulet_count, -1.0f, true);
        }

        // Boots
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Boots");
            ImGui::TableSetColumnIndex(1);
            equip_combo("##lbt", &l.boots_idx, k_boots, k_boot_count, -1.0f, true);
        }

        // Belt
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Belt");
            ImGui::TableSetColumnIndex(1);
            equip_combo("##lblt", &l.belt_idx, k_belts, k_belt_count, -1.0f, true);
        }

        ImGui::EndTable();
    }

    // ── Perks (tree-filtered with costs) ──
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Perks");
    ImGui::Spacing();

    if (ImGui::BeginTable("##lperks", 2, ImGuiTableFlags_None)) {
        ImGui::TableSetupColumn("tree", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn("pick", ImGuiTableColumnFlags_WidthStretch);

        auto perk_row = [&](const char *tree_label, const char *combo_id,
                            const PerkInfo *perks, int count, int &sel) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(tree_label);
            ImGui::TableSetColumnIndex(1);

            const char *items[12];
            items[0] = "None";
            for (int i = 0; i < count; ++i) items[i+1] = perks[i].display;

            int combo_sel = sel + 1;
            ImGui::SetNextItemWidth(sel >= 0 ? -50.0f : -1.0f);
            if (ImGui::Combo(combo_id, &combo_sel, items, count + 1))
                sel = combo_sel - 1;
            if (sel >= 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("%dpt", perks[sel].cost);
            }
        };

        perk_row("Mind",   "##lpm", k_mind_perks,   k_mind_count,   l.mind_perk);
        perk_row("Body",   "##lpb", k_body_perks,   k_body_count,   l.body_perk);
        perk_row("Spirit", "##lps", k_spirit_perks, k_spirit_count, l.spirit_perk);

        ImGui::EndTable();
    }

    // Cost summary
    {
        int total_pts = 0;
        if (l.mind_perk   >= 0) total_pts += k_mind_perks[l.mind_perk].cost;
        if (l.body_perk   >= 0) total_pts += k_body_perks[l.body_perk].cost;
        if (l.spirit_perk >= 0) total_pts += k_spirit_perks[l.spirit_perk].cost;
        const bool over = total_pts > 7;
        ImGui::PushStyleColor(ImGuiCol_Text,
            over ? IM_COL32(255, 80, 80, 255) : IM_COL32(120, 220, 120, 255));
        ImGui::Text("Perk points: %d / 7", total_pts);
        ImGui::PopStyleColor();
    }

    // ── Level ups + perk upgrades ──
    ImGui::Spacing();
    ImGui::TextUnformatted("Level Ups:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(55.0f);
    ImGui::DragInt("##llu", &l.level_ups, 0.1f, 0, 10, "%dx");
    ImGui::SameLine(0.0f, 14.0f);
    ImGui::TextUnformatted("Upgrade:");
    ImGui::SameLine();
    ImGui::Checkbox("Mind##lum",   &l.upgrade_mind);
    ImGui::SameLine();
    ImGui::Checkbox("Body##lub",   &l.upgrade_body);
    ImGui::SameLine();
    ImGui::Checkbox("Spirit##lus", &l.upgrade_spirit);

    // ── Boss Command ──
    ImGui::Spacing();
    ImGui::TextUnformatted("Boss Command (optional — dispatched last on Apply):");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##lboss", l.boss_cmd, sizeof(l.boss_cmd));
    // Warn if the field contains dangerous commands that will be blocked on Apply.
    if (l.boss_cmd[0]) {
        const char *bad = boss_cmd_find_dangerous(l.boss_cmd);
        if (bad) {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 100, 60, 255));
            ImGui::TextUnformatted("  WARNING: blocked command detected (will not execute):");
            ImGui::SameLine();
            ImGui::TextUnformatted(bad);
            ImGui::PopStyleColor();
        }
    }
    if (ImGui::SmallButton("Parse → Loadout##lbparse") && l.boss_cmd[0])
        parse_boss_cmd_into_loadout(l, l.boss_cmd);

    // ── Action buttons ──
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("Save##ls", ImVec2(90, 0))) {
        l.valid = true;
        save_loadouts();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!l.valid);
    if (ImGui::Button("Apply##la", ImVec2(90, 0)))
        apply_loadout(l);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Delete##ld", ImVec2(90, 0)))
        reset_loadout_slot(g_loadout_sel);

    ImGui::EndChild();
}

// ── Section: Staff Commands ───────────────────────────────────────────────────

static void draw_staff()
{
    if (!g_is_staff.load()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.85f, 0.3f, 0.3f, 1.0f), "Authorization required");
        ImGui::TextDisabled("Staff credentials not verified for this session.");
        return;
    }

    if (!ImGui::BeginTable("##staff", 2, ImGuiTableFlags_None)) return;
    ImGui::TableSetupColumn("lbl",  ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("ctrl", ImGuiTableColumnFlags_WidthStretch);

    auto btn = [&](int idx, const char *lbl, const char *cmd) {
        ImGui::TableNextRow();
        bool sel = row(idx, lbl);
        ImGui::TableSetColumnIndex(1);
        char id[32]; snprintf(id, sizeof(id), "Run##s%d", idx);
        if (ImGui::Button(id, ImVec2(-1, 0)) || (sel && kbd_enter(idx)))
            dispatch_ascii(cmd);
    };

    auto tog = [&](int idx, const char *lbl, bool &state,
                   const char *on_cmd, const char *off_cmd) {
        ImGui::TableNextRow();
        bool sel = row(idx, lbl);
        ImGui::TableSetColumnIndex(1);
        char id[32]; snprintf(id, sizeof(id), "%s###t%d", state ? "ON" : "OFF", idx);
        if (ImGui::Button(id, ImVec2(-1, 0)) || (sel && kbd_enter(idx))) {
            state = !state;
            dispatch_ascii(state ? on_cmd : off_cmd);
        }
    };

    auto section_hdr = [&](const char *label) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Spacing(); ImGui::Separator();
        ImGui::TextDisabled("%s", label);
        ImGui::TableSetColumnIndex(1);
        ImGui::Spacing();
    };

    tog(0, "God Mode",            s_god_mode,           "God", "God");
    tog(1, "Fast Cooldowns",      s_fast_cooldowns,     "FastCooldowns", "FastCooldowns");
    btn(2, "Die",                 "Die");

    ImGui::TableNextRow();
    row(3, "Superspeed");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-65.0f);
    ImGui::DragFloat("##spd", &s_superspeed, 0.05f, 0.1f, 20.0f, "%.1fx");
    ImGui::SameLine();
    if (ImGui::Button("Set##spd", ImVec2(-1,0))) {
        char buf[64]; snprintf(buf, sizeof(buf), "Superspeed %.4g", s_superspeed);
        dispatch_ascii(buf);
    }

    tog(4, "Toggle HUD",          s_toggle_hud,         "ToggleHUD", "ToggleHUD");
    tog(5, "Toggle Debug Camera", s_toggle_debug_cam,   "ToggleDebugCamera", "ToggleDebugCamera");
    btn(6, "Switch Team",         "SwitchTeam");

    ImGui::TableNextRow();
    row(7, "Friendly Points");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-65.0f);
    ImGui::DragInt("##fp", &s_friend_pts, 1.0f, 0, 9999, "%d pts");
    ImGui::SameLine();
    if (ImGui::Button("Add##fp", ImVec2(-1,0))) {
        char buf[64]; snprintf(buf, sizeof(buf), "AddFriendlyPoints %d", s_friend_pts);
        dispatch_ascii(buf);
    }

    ImGui::TableNextRow();
    row(8, "Enemy Points");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-65.0f);
    ImGui::DragInt("##ep", &s_enemy_pts, 1.0f, 0, 9999, "%d pts");
    ImGui::SameLine();
    if (ImGui::Button("Add##ep", ImVec2(-1,0))) {
        char buf[64]; snprintf(buf, sizeof(buf), "AddEnemyPoints %d", s_enemy_pts);
        dispatch_ascii(buf);
    }

    section_hdr("Match Flow");
    btn(9,  "Infinite Match",   "StartInfiniteMatch");
    tog(10, "Allow Round End",  s_allow_round_end,
        "SetAllowRoundEnd 1", "SetAllowRoundEnd 0");
    btn(11, "Resume Countdown", "ResumeCountdown");
    btn(12, "Stop Circles",     "StopCircles");
    btn(13, "Close Circle",     "CloseCircle");

    section_hdr("Bots");
    btn(14, "Reveal Nearby Bots", "RevealNearbyMatchBots");
    tog(15, "No Bot Aggro", s_no_aggro,
        "SetNoMatchBotAggro true", "SetNoMatchBotAggro false");

    section_hdr("Custom Command");
    ImGui::TableNextRow();
    row(16, "Command");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-65.0f);
    bool cmd_enter = ImGui::InputText("##scmd", s_staff_cmd, sizeof(s_staff_cmd),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Run##scmd", ImVec2(-1,0)) || cmd_enter) && s_staff_cmd[0])
        dispatch_ascii(s_staff_cmd);

    ImGui::EndTable();
}

// ── Section: Dev Settings ─────────────────────────────────────────────────────

static void draw_dev()
{
    if (!g_is_dev.load()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.85f, 0.3f, 0.3f, 1.0f), "Authorization required");
        ImGui::TextDisabled("Developer credentials not verified for this session.");
        return;
    }

    if (!ImGui::BeginTable("##dev", 2, ImGuiTableFlags_None)) return;
    ImGui::TableSetupColumn("lbl",  ImGuiTableColumnFlags_WidthFixed, 140.0f);
    ImGui::TableSetupColumn("ctrl", ImGuiTableColumnFlags_WidthStretch);

    ImGui::TableNextRow();
    row(0, "Apply Effect");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-75.0f);
    bool eff_enter = ImGui::InputText("##eff", s_effect_buf, sizeof(s_effect_buf),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Apply##eff", ImVec2(-1,0)) || eff_enter) && s_effect_buf[0]) {
        char buf[320];
        snprintf(buf, sizeof(buf), "ApplyPlayerEffect %s", s_effect_buf);
        dispatch_ascii(buf);
    }

    ImGui::EndTable();
}

// ── UE4 Log viewer ───────────────────────────────────────────────────────────

enum LogLevel { LOG_INFO = 0, LOG_DEBUG = 1, LOG_WARN = 2, LOG_ERROR = 3 };

static LogLevel classify_line(const char *p, int len)
{
    auto has = [&](const char *kw) {
        int klen = (int)strlen(kw);
        for (int i = 0; i <= len - klen; ++i)
            if (strncmp(p + i, kw, klen) == 0) return true;
        return false;
    };
    if (has("Error:")  || has("Fatal:"))        return LOG_ERROR;
    if (has("Warning:"))                         return LOG_WARN;
    if (has("VeryVerbose:") || has("Verbose:")) return LOG_DEBUG;
    return LOG_INFO;
}

static void get_ue4_log_path(char *out, int sz)
{
    char base[MAX_PATH];
    get_dll_dir(base, sizeof(base));
    int n = (int)strlen(base);
    if (n > 0 && (base[n-1] == '\\' || base[n-1] == '/')) base[--n] = '\0';
    // Go up two directories: dlls/ -> Mods/ -> <root>/
    for (int up = 0; up < 2; ++up) {
        char *sep = strrchr(base, '\\');
        if (!sep) sep = strrchr(base, '/');
        if (sep) *sep = '\0';
    }
    snprintf(out, sz, "%s\\g3\\Saved\\Logs\\g3-%lu.log", base, (unsigned long)GetCurrentProcessId());
}

static bool log_stristr(const char *hay, int hlen, const char *needle)
{
    int nlen = (int)strlen(needle);
    if (!nlen) return true;
    for (int i = 0; i <= hlen - nlen; ++i) {
        bool ok = true;
        for (int j = 0; j < nlen && ok; ++j)
            ok = tolower((unsigned char)hay[i+j]) == tolower((unsigned char)needle[j]);
        if (ok) return true;
    }
    return false;
}

static void draw_log()
{
    if (!g_is_dev.load()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.85f, 0.3f, 0.3f, 1.0f), "Authorization required");
        ImGui::TextDisabled("Developer credentials not verified for this session.");
        return;
    }

    static char  s_filter[128]        = {};
    static bool  s_auto_scroll        = true;
    static bool  s_lv_info            = true;
    static bool  s_lv_debug           = true;
    static bool  s_lv_warn            = true;
    static bool  s_lv_error           = true;
    static char  s_log_path[MAX_PATH] = {};
    static bool  s_path_ready         = false;
    static char *s_log_buf            = nullptr;
    static int   s_log_len            = 0;
    static float s_next_refresh       = -1.0f;
    static const int k_buf_sz         = 256 * 1024;

    if (!s_path_ready) {
        get_ue4_log_path(s_log_path, sizeof(s_log_path));
        s_log_buf    = new char[k_buf_sz];
        s_log_buf[0] = '\0';
        s_path_ready = true;
    }

    float now      = (float)ImGui::GetTime();
    bool  refreshed = false;
    if (now >= s_next_refresh) {
        s_next_refresh = now + 0.5f;
        FILE *f = fopen(s_log_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long file_sz   = ftell(f);
            long read_from = file_sz > (k_buf_sz - 1) ? file_sz - (k_buf_sz - 1) : 0;
            fseek(f, read_from, SEEK_SET);
            s_log_len = (int)fread(s_log_buf, 1, k_buf_sz - 1, f);
            s_log_buf[s_log_len] = '\0';
            fclose(f);
            // skip partial first line when seeked mid-file
            if (read_from > 0) {
                char *nl = (char *)memchr(s_log_buf, '\n', s_log_len);
                if (nl) {
                    int skip = (int)(nl + 1 - s_log_buf);
                    s_log_len -= skip;
                    memmove(s_log_buf, nl + 1, s_log_len + 1);
                }
            }
            refreshed = true;
        }
    }

    ImGui::SetNextItemWidth(240.0f);
    ImGui::InputText("##logfilter", s_filter, sizeof(s_filter));
    ImGui::SameLine();
    if (ImGui::Button("Clear##lf")) s_filter[0] = '\0';
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll##log", &s_auto_scroll);
    ImGui::SameLine();
    if (ImGui::Button("Refresh##log")) s_next_refresh = -1.0f;

    // Level filter row
    ImGui::Spacing();
    auto lv_toggle = [](const char *lbl, bool &flag, ImVec4 on_col) {
        ImVec4 col = flag ? on_col : ImVec4(on_col.x*0.35f, on_col.y*0.35f, on_col.z*0.35f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::Checkbox(lbl, &flag);
        ImGui::PopStyleColor();
        ImGui::SameLine();
    };
    lv_toggle("INFO##lvi",  s_lv_info,  ImVec4(0.80f, 0.80f, 0.80f, 1.0f));
    lv_toggle("DEBUG##lvd", s_lv_debug, ImVec4(0.55f, 0.78f, 1.00f, 1.0f));
    lv_toggle("WARN##lvw",  s_lv_warn,  ImVec4(1.00f, 0.78f, 0.20f, 1.0f));
    lv_toggle("ERROR##lve", s_lv_error, ImVec4(1.00f, 0.35f, 0.35f, 1.0f));
    if (ImGui::SmallButton("ALL##lva"))
        s_lv_info = s_lv_debug = s_lv_warn = s_lv_error = true;

    ImGui::Spacing();
    ImGui::TextDisabled("%s", s_log_path);
    ImGui::Spacing();

    ImGui::BeginChild("##logchild", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1));

    static const ImVec4 k_lv_cols[] = {
        ImVec4(0.80f, 0.80f, 0.80f, 1.0f),  // INFO
        ImVec4(0.55f, 0.78f, 1.00f, 1.0f),  // DEBUG
        ImVec4(1.00f, 0.78f, 0.20f, 1.0f),  // WARN
        ImVec4(1.00f, 0.35f, 0.35f, 1.0f),  // ERROR
    };
    static const bool *k_lv_flags[] = { &s_lv_info, &s_lv_debug, &s_lv_warn, &s_lv_error };

    const char *p   = s_log_buf;
    const char *end = s_log_buf + s_log_len;
    while (p < end) {
        const char *eol      = (const char *)memchr(p, '\n', end - p);
        int         line_len = eol ? (int)(eol - p) : (int)(end - p);
        if (line_len > 0) {
            bool text_ok = !s_filter[0] || log_stristr(p, line_len, s_filter);
            if (text_ok) {
                LogLevel lv = classify_line(p, line_len);
                if (*k_lv_flags[lv]) {
                    ImGui::PushStyleColor(ImGuiCol_Text, k_lv_cols[lv]);
                    ImGui::TextUnformatted(p, p + line_len);
                    ImGui::PopStyleColor();
                }
            }
        }
        p = eol ? eol + 1 : end;
    }

    ImGui::PopStyleVar();

    if (s_auto_scroll && refreshed)
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
}

// ── Section: Account Status ───────────────────────────────────────────────────

static void draw_account()
{
    ImGui::Spacing();

    EnterCriticalSection(&g_state_cs);
    char username[256];
    strncpy(username, g_username, sizeof(username) - 1);
    username[255] = '\0';
    LeaveCriticalSection(&g_state_cs);

    if (!username[0]) {
        ImGui::TextColored(ImVec4(0.85f, 0.3f, 0.3f, 1.0f), "Not connected");
        ImGui::TextDisabled("Authentication has not completed for this session.");
        return;
    }

    const bool is_staff = g_is_staff.load();
    const bool is_dev   = g_is_dev.load();

    if (!ImGui::BeginTable("##acctbl", 2, ImGuiTableFlags_BordersInnerV)) return;
    ImGui::TableSetupColumn("lbl", ImGuiTableColumnFlags_WidthFixed, 130.0f);
    ImGui::TableSetupColumn("val", ImGuiTableColumnFlags_WidthStretch);

    auto info_row = [&](const char *lbl, const char *val) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("%s", lbl);
        ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(val);
    };

    info_row("Connected as", username);
    info_row("Auth method",  "Discord");

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("Role");
    ImGui::TableSetColumnIndex(1);
    if (is_dev)
        ImGui::TextColored(ImVec4(1.00f, 0.50f, 0.00f, 1.0f), "Developer");
    else if (is_staff)
        ImGui::TextColored(ImVec4(0.50f, 0.85f, 1.00f, 1.0f), "Staff");
    else
        ImGui::TextColored(ImVec4(0.70f, 0.70f, 0.70f, 1.0f), "Player");

    ImGui::EndTable();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.50f, 0.50f, 0.50f, 1.0f),
        "Player stats (exiles, wins, etc.)  \xe2\x80\x94  coming soon");
}

// ── Connection toast ──────────────────────────────────────────────────────────

static std::atomic<float> g_toast_timer    { 0.0f };
static std::atomic<bool>  g_toast_shown    { false };
static std::atomic<bool>  g_auth_received  { false };
static char               g_toast_text[256] = {};

// Renders a fade-in/hold/fade-out notification in the top-left.
// Must be called between ImGui::NewFrame() and ImGui::Render().
static void render_toast_window()
{
    float t = g_toast_timer.load(std::memory_order_relaxed);
    if (t <= 0.0f) return;

    const float TOTAL = 4.5f;
    const float FADE  = 0.5f;
    float alpha = 1.0f;
    if (t > TOTAL - FADE) alpha = (TOTAL - t) / FADE;
    else if (t < FADE)    alpha = t / FADE;
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;

    ImGui::SetNextWindowBgAlpha(0.82f * alpha);
    ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(320.0f, 0.0f), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##toast", nullptr,
        ImGuiWindowFlags_NoTitleBar    | ImGuiWindowFlags_NoResize  |
        ImGuiWindowFlags_NoMove        | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoInputs      | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoNav         | ImGuiWindowFlags_NoDecoration);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.20f, 1.00f, 0.45f, alpha));
    ImGui::TextUnformatted(g_toast_text);
    ImGui::PopStyleColor();
    ImGui::End();
    ImGui::PopStyleVar();

    float dt = ImGui::GetIO().DeltaTime;
    float new_t = t - dt;
    g_toast_timer.store(new_t > 0.0f ? new_t : 0.0f, std::memory_order_relaxed);
}

// ── Left nav ──────────────────────────────────────────────────────────────────

static void draw_nav()
{
    static const char *labels[] = {
        "Match Settings",
        "Item Spawner",
        "Character",
        "Hotkeys",
        "Loadouts",
        "Account",
        "Staff Commands",
        "Dev Settings",
        "UE4 Log",
    };

    const bool is_staff = g_is_staff.load();
    const bool is_dev   = g_is_dev.load();
    const bool authed[] = { true, true, true, true, true, true, is_staff, is_dev, is_dev };

    for (int i = 0; i < 9; ++i) {
        if (i == 6) { ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing(); }

        bool in_section = g_section == i;
        bool nav_focus  = in_section && !g_in_right;

        if (!authed[i])
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.45f, 0.45f, 1.0f));

        ImGui::PushID(i);
        if (ImGui::Selectable(labels[i], nav_focus || in_section)) {
            g_section  = i;
            g_in_right = false;
            g_item     = 0;
        }
        ImGui::PopID();

        if (!authed[i])
            ImGui::PopStyleColor();
    }
}

// ── Keyboard nav ──────────────────────────────────────────────────────────────

static int section_item_count()
{
    switch (g_section) {
    case 0: return 8;   // Match: StartMatch, NumBots, BotDiff, SpawnBot, StopCD, Boons, Zones, NPCs
    case 1: return 12;  // Items: Gauntlet–Belt(5), Class, LevelUp, UpgradePerks, Mind, Body, Spirit, Reset
    case 2: return 8;   // Character: Drop Amulet/Belt/Boots/Rune/Secondary/All, Heal, Die
    case 3: return 0;   // Hotkeys: table-based, no keyboard nav
    case 4: return 0;   // Loadouts: no keyboard nav
    case 5: return 0;   // Account: no keyboard nav
    case 6: return g_is_staff.load() ? 17 : 0;
    case 7: return g_is_dev.load()   ?  1 : 0;
    case 8: return 0;   // UE4 Log: no keyboard nav
    }
    return 0;
}

static void handle_nav_keys()
{
    g_enter_consumed = false;
    if (ImGui::IsAnyItemActive()) return;

    const bool dn = ImGui::IsKeyPressed(ImGuiKey_DownArrow,  true);
    const bool up = ImGui::IsKeyPressed(ImGuiKey_UpArrow,    true);
    const bool rt = ImGui::IsKeyPressed(ImGuiKey_RightArrow, false);
    const bool lt = ImGui::IsKeyPressed(ImGuiKey_LeftArrow,  false);
    const bool bs = ImGui::IsKeyPressed(ImGuiKey_Backspace,  false);
    const bool en = ImGui::IsKeyPressed(ImGuiKey_Enter,      false);

    if (!g_in_right) {
        if (dn) g_section = (g_section + 1) % 9;
        if (up) g_section = (g_section + 8) % 9;
        if (rt || en) {
            g_in_right = true;
            g_item = 0;
            if (en) g_enter_consumed = true;
        }
    } else {
        const int n = section_item_count();
        if (n > 0) {
            if (dn) g_item = (g_item + 1) % n;
            if (up) g_item = (g_item + n - 1) % n;
        }
        if (lt || bs) g_in_right = false;
    }
}

// ── Controller polling ────────────────────────────────────────────────────────

static void poll_controller()
{
    for (BYTE p = 0; p < 4; ++p) {
        WORD cur      = xinput_sample(p);
        WORD pressed  = cur & ~g_ctrl_prev[p];
        g_ctrl_prev[p] = cur;
        if (!pressed) continue;

        // Pick the single highest-priority bit that was newly pressed.
        WORD single = 0;
        for (int j = 0; j < k_ctrl_btn_count && !single; ++j)
            if (pressed & k_ctrl_btns[j].mask) single = k_ctrl_btns[j].mask;
        if (!single) continue;

        if (g_ctrl_listening_idx >= 0) {
            g_hotkeys[g_ctrl_listening_idx].ctrl_btn    = single;
            g_hotkeys[g_ctrl_listening_idx].ctrl_player = p;
            g_ctrl_listening_idx = -1;
            save_hotkeys();
            continue;
        }

        // Fire hotkeys bound to this controller button (only when menu is closed).
        if (!g_show.load()) {
            for (int i = 0; i < k_hotkey_count; ++i) {
                if (!g_hotkeys[i].action)             continue;
                if (!g_hotkeys[i].ctrl_btn)           continue;
                if (g_hotkeys[i].ctrl_btn    != single) continue;
                if (g_hotkeys[i].ctrl_player != p)    continue;
                g_hotkeys[i].action();
            }
        }
    }
}

// ── ImGui render ──────────────────────────────────────────────────────────────

static void render_frame()
{
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGui::GetIO().MouseDrawCursor = true;

    poll_controller();
    handle_nav_keys();

    char title[320];
    EnterCriticalSection(&g_state_cs);
    if (g_is_dev.load() && g_username[0])
        snprintf(title, sizeof(title), "Game Control Menu  -  %s  -  [DEV]", g_username);
    else if (g_is_staff.load() && g_username[0])
        snprintf(title, sizeof(title), "Game Control Menu  -  %s  -  [STAFF]", g_username);
    else if (g_username[0])
        snprintf(title, sizeof(title), "Game Control Menu  -  %s", g_username);
    else
        snprintf(title, sizeof(title), "Game Control Menu");
    LeaveCriticalSection(&g_state_cs);

    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(780, 480), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(80, 80),    ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(540, 320), ImVec2(9999, 9999));
    ImGui::Begin(title, &open);

    ImGui::BeginChild("##nav", ImVec2(180.0f, 0.0f), false);
    draw_nav();
    ImGui::EndChild();

    ImGui::SameLine(0.0f, 6.0f);

    ImVec2 sep_min = ImGui::GetCursorScreenPos();
    sep_min.x -= 3.0f;
    ImVec2 sep_max = sep_min;
    sep_max.y += ImGui::GetContentRegionAvail().y;
    ImGui::GetWindowDrawList()->AddLine(sep_min, sep_max, IM_COL32(80, 80, 80, 180), 1.0f);

    ImGui::BeginChild("##content", ImVec2(0.0f, 0.0f), false);
    switch (g_section) {
    case 0: draw_match();    break;
    case 1: draw_items();    break;
    case 2: draw_char();     break;
    case 3: draw_hotkeys();  break;
    case 4: draw_loadouts(); break;
    case 5: draw_account();  break;
    case 6: draw_staff();    break;
    case 7: draw_dev();      break;
    case 8: draw_log();      break;
    }
    ImGui::EndChild();

    ImGui::End();

    if (!open) g_show.store(false);

    render_toast_window();

    ImGui::Render();
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

// ── D3D initialization ────────────────────────────────────────────────────────

static void create_rtv(IDXGISwapChain *swap)
{
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    ID3D11Texture2D *back = nullptr;
    if (SUCCEEDED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back))) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

static void init_imgui(IDXGISwapChain *swap)
{
    dbg("init_imgui: start");
    swap->GetDevice(__uuidof(ID3D11Device), (void **)&g_device);
    dbg("init_imgui: GetDevice done");
    g_device->GetImmediateContext(&g_context);
    dbg("init_imgui: GetImmediateContext done");
    create_rtv(swap);
    dbg("init_imgui: create_rtv done");

    DXGI_SWAP_CHAIN_DESC desc {};
    swap->GetDesc(&desc);
    char buf[128];
    snprintf(buf, sizeof(buf), "init_imgui: hwnd=%p", (void*)desc.OutputWindow);
    dbg(buf);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    dbg("init_imgui: ImGui::CreateContext done");
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename  = nullptr;

    ImGui::StyleColorsDark();
    ImGuiStyle &s = ImGui::GetStyle();
    s.Alpha              = 0.95f;
    s.WindowRounding     = 5.0f;
    s.FrameRounding      = 3.0f;
    s.ItemSpacing        = ImVec2(6, 5);
    s.Colors[ImGuiCol_TitleBg]       = ImVec4(0.08f, 0.08f, 0.12f, 1.0f);
    s.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.14f, 0.14f, 0.22f, 1.0f);
    s.Colors[ImGuiCol_ChildBg]       = ImVec4(0.06f, 0.06f, 0.09f, 1.0f);

    dbg("init_imgui: calling ImGui_ImplWin32_Init");
    ImGui_ImplWin32_Init(desc.OutputWindow);
    dbg("init_imgui: ImGui_ImplWin32_Init done");
    ImGui_ImplDX11_Init(g_device, g_context);
    dbg("init_imgui: ImGui_ImplDX11_Init done");

    g_hwnd.store(desc.OutputWindow, std::memory_order_release);
    g_imgui_ready.store(true, std::memory_order_release);
    dbg("init_imgui: ready");
}

// ── Present hook ──────────────────────────────────────────────────────────────

static HRESULT STDMETHODCALLTYPE hooked_present(IDXGISwapChain *swap, UINT sync, UINT flags)
{
    if (!g_imgui_ready.load(std::memory_order_acquire))
        init_imgui(swap);

    // Start the connection toast on the first rendered frame after auth lands.
    // Deferring to here (rather than devmenu_set_staff) ensures the timer starts
    // while the game world is visible, not during the loading screen.
    if (g_imgui_ready.load(std::memory_order_relaxed)
        && g_auth_received.load(std::memory_order_acquire)
        && !g_toast_shown.exchange(true, std::memory_order_acq_rel)) {
        g_toast_timer.store(4.5f, std::memory_order_relaxed);
        dbg("toast: started");
    }

    const bool show_menu  = g_show.load();
    const bool show_toast = g_imgui_ready.load()
                         && g_toast_timer.load(std::memory_order_relaxed) > 0.0f;

    if (show_menu) {
        render_frame();
    } else if (show_toast) {
        // Minimal ImGui frame for toast only — no input handling.
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGui::GetIO().MouseDrawCursor = false;
        render_toast_window();
        ImGui::Render();
        if (g_context && g_rtv) {
            g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
    } else if (g_imgui_ready.load()) {
        ImGui::GetIO().MouseDrawCursor = false;
    }

    return g_orig_present(swap, sync, flags);
}

// ── Vtable patch ──────────────────────────────────────────────────────────────

static bool patch_present(IDXGISwapChain *swap)
{
    void **vtbl = *reinterpret_cast<void ***>(swap);
    void **slot  = &vtbl[8];
    // Already hooked (e.g. game created a second swap chain sharing the same vtable).
    // Don't overwrite g_orig_present with our own hook pointer.
    if (*slot == reinterpret_cast<void *>(&hooked_present))
        return true;
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
        return false;
    g_orig_present = reinterpret_cast<PresentFn>(*slot);
    *slot = reinterpret_cast<void *>(&hooked_present);
    VirtualProtect(slot, sizeof(void *), old, &old);
    return true;
}

// ── Factory hook helpers ──────────────────────────────────────────────────────

typedef HRESULT (STDMETHODCALLTYPE *CreateSwapChainFn)(
    IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
typedef HRESULT (STDMETHODCALLTYPE *CreateSwapChainForHwndFn)(
    IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);

static CreateSwapChainFn        g_orig_create_sc      = nullptr;
static CreateSwapChainForHwndFn g_orig_create_sc_hwnd = nullptr;

static HRESULT STDMETHODCALLTYPE hooked_create_sc(
    IDXGIFactory *fac, IUnknown *dev, DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **ppSC)
{
    dbg("hooked_create_sc: fired");
    HRESULT hr = g_orig_create_sc(fac, dev, desc, ppSC);
    char buf[128];
    snprintf(buf, sizeof(buf), "hooked_create_sc: orig hr=0x%08lX", (unsigned long)hr);
    dbg(buf);
    if (SUCCEEDED(hr) && ppSC && *ppSC) {
        patch_present(*ppSC);
        dbg("hooked_create_sc: present patched");
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hooked_create_sc_hwnd(
    IDXGIFactory2 *fac, IUnknown *dev, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1 *desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fdesc,
    IDXGIOutput *output, IDXGISwapChain1 **ppSC)
{
    dbg("hooked_create_sc_hwnd: fired");
    HRESULT hr = g_orig_create_sc_hwnd(fac, dev, hwnd, desc, fdesc, output, ppSC);
    char buf[128];
    snprintf(buf, sizeof(buf), "hooked_create_sc_hwnd: orig hr=0x%08lX", (unsigned long)hr);
    dbg(buf);
    if (SUCCEEDED(hr) && ppSC && *ppSC) {
        patch_present(reinterpret_cast<IDXGISwapChain *>(*ppSC));
        dbg("hooked_create_sc_hwnd: present patched");
    }
    return hr;
}

static void patch_vtbl_slot(void **vtbl, int slot, void *hook, void **orig)
{
    void **entry = &vtbl[slot];
    DWORD old = 0;
    if (!VirtualProtect(entry, sizeof(void *), PAGE_READWRITE, &old))
        return;
    *orig  = *entry;
    *entry = hook;
    VirtualProtect(entry, sizeof(void *), old, &old);
}

static void hook_factory()
{
    typedef HRESULT (WINAPI *CreateDXGIFactory2Fn)(UINT, REFIID, void **);
    typedef HRESULT (WINAPI *CreateDXGIFactory1Fn)(REFIID, void **);

    dbg("hook_factory: start");

    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (!dxgi) { dbg("hook_factory: LoadLibraryW(dxgi.dll) FAILED"); return; }
    dbg("hook_factory: dxgi.dll loaded");

    auto *cf2 = reinterpret_cast<CreateDXGIFactory2Fn>(
        GetProcAddress(dxgi, "CreateDXGIFactory2"));
    if (cf2) {
        dbg("hook_factory: CreateDXGIFactory2 found, calling...");
        IDXGIFactory2 *fac = nullptr;
        HRESULT hr = cf2(0, __uuidof(IDXGIFactory2), reinterpret_cast<void **>(&fac));
        char buf[128];
        snprintf(buf, sizeof(buf), "hook_factory: CreateDXGIFactory2 hr=0x%08lX fac=%p", (unsigned long)hr, (void*)fac);
        dbg(buf);
        if (SUCCEEDED(hr) && fac) {
            void **vtbl = *reinterpret_cast<void ***>(fac);
            snprintf(buf, sizeof(buf), "hook_factory: vtbl=%p slot10=%p slot15=%p",
                     (void*)vtbl, vtbl[10], vtbl[15]);
            dbg(buf);
            patch_vtbl_slot(vtbl, 10,
                reinterpret_cast<void *>(&hooked_create_sc),
                reinterpret_cast<void **>(&g_orig_create_sc));
            dbg("hook_factory: slot 10 (CreateSwapChain) patched");
            patch_vtbl_slot(vtbl, 15,
                reinterpret_cast<void *>(&hooked_create_sc_hwnd),
                reinterpret_cast<void **>(&g_orig_create_sc_hwnd));
            dbg("hook_factory: slot 15 (CreateSwapChainForHwnd) patched");
            fac->Release();
            dbg("hook_factory: factory released, done");
            return;
        }
    }

    dbg("hook_factory: falling back to CreateDXGIFactory1");
    auto *cf1 = reinterpret_cast<CreateDXGIFactory1Fn>(
        GetProcAddress(dxgi, "CreateDXGIFactory1"));
    if (!cf1) { dbg("hook_factory: CreateDXGIFactory1 not found"); return; }
    IDXGIFactory1 *fac = nullptr;
    if (FAILED(cf1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&fac)))) {
        dbg("hook_factory: CreateDXGIFactory1 FAILED");
        return;
    }
    void **vtbl = *reinterpret_cast<void ***>(fac);
    patch_vtbl_slot(vtbl, 10,
        reinterpret_cast<void *>(&hooked_create_sc),
        reinterpret_cast<void **>(&g_orig_create_sc));
    dbg("hook_factory: slot 10 (CreateSwapChain) patched via factory1");
    fac->Release();
    dbg("hook_factory: done (factory1 path)");
}

// ── WndProc install thread ────────────────────────────────────────────────────

// Waits for init_imgui (render thread) to publish g_hwnd, then installs the
// WndProc subclass from this independent thread.  This must not run on the
// render thread — see the comment in init_imgui for the deadlock explanation.
static DWORD WINAPI wndproc_install_thread(LPVOID)
{
    for (int i = 0; i < 300; ++i) {
        HWND hwnd = g_hwnd.load(std::memory_order_acquire);
        if (hwnd) {
            g_orig_wndproc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                                  reinterpret_cast<LONG_PTR>(hooked_wndproc)));
            return 0;
        }
        Sleep(100);
    }
    return 0;
}

// ── Exports ───────────────────────────────────────────────────────────────────

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        InitializeCriticalSection(&g_state_cs);
    else if (reason == DLL_PROCESS_DETACH)
        DeleteCriticalSection(&g_state_cs);
    return TRUE;
}

extern "C" void devmenu_init()
{
    dbg("devmenu_init: start");
    init_loadouts();
    dbg("devmenu_init: init_loadouts done");
    load_loadouts();
    dbg("devmenu_init: load_loadouts done");
    load_hotkeys();
    dbg("devmenu_init: load_hotkeys done");
    hook_factory();
    dbg("devmenu_init: hook_factory done");

    // Spawn the WndProc installer on a thread that is not part of UE4's
    // render/game-thread synchronisation pair.
    HANDLE t = CreateThread(nullptr, 0, wndproc_install_thread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    dbg("devmenu_init: complete");
}

extern "C" void devmenu_get_menu_key(unsigned *vk, unsigned *mods)
{
    if (vk)   *vk   = g_hotkeys[k_menu_key_idx].vk   ? g_hotkeys[k_menu_key_idx].vk   : 0x77u;
    if (mods) *mods = g_hotkeys[k_menu_key_idx].mods;
}

extern "C" void devmenu_set_menu_key_callback(MenuKeyChangedFn cb)
{
    g_menu_key_cb = cb;
}

extern "C" void devmenu_show()
{
    g_show.store(true);
    HWND hwnd = g_hwnd.load();
    if (hwnd) {
        // Release all physically-held keys so UE4's raw-input state clears.
        // (UE4 routes keyboard through WM_INPUT; when we start blocking it the
        // key-up events never arrive, keeping the game in a "key held" state.)
        for (UINT vk = 1; vk < 256; vk++)
            if (GetAsyncKeyState(vk) & 0x8000)
                PostMessageW(hwnd, WM_KEYUP, vk, 0);
        PostMessageW(hwnd, WM_LBUTTONUP, 0, 0);
        PostMessageW(hwnd, WM_RBUTTONUP, 0, 0);
        PostMessageW(hwnd, WM_MBUTTONUP, 0, 0);
    }
}
extern "C" void devmenu_hide()
{
    HWND hwnd = g_hwnd.load();
    if (hwnd) {
        // Re-inject any key-ups that were eaten while ImGui had keyboard focus,
        // and release mouse buttons in case a rapid-click left one stuck.
        for (UINT vk = 1; vk < 256; vk++) {
            if (eaten_get(vk)) {
                PostMessageW(hwnd, WM_KEYUP, vk, 0);
                eaten_clr(vk);
            }
        }
        PostMessageW(hwnd, WM_LBUTTONUP, 0, 0);
        PostMessageW(hwnd, WM_RBUTTONUP, 0, 0);
        PostMessageW(hwnd, WM_MBUTTONUP, 0, 0);
    }
    g_show.store(false);
    if (g_imgui_ready.load()) ImGui::GetIO().MouseDrawCursor = false;
}

extern "C" void devmenu_set_staff(int is_staff, int is_dev, const char *username)
{
    EnterCriticalSection(&g_state_cs);
    if (username && username[0]) {
        // Valid auth response — update everything.
        g_is_staff.store(is_staff != 0);
        g_is_dev.store(is_dev != 0);
        strncpy(g_username, username, sizeof(g_username) - 1);
        g_username[255] = '\0';
        // Prepare toast text; hooked_present will start the countdown once
        // the renderer is ready, so the toast appears in the game world rather
        // than expiring during the loading screen.
        if (!g_toast_shown.load(std::memory_order_relaxed)) {
            const char *role = is_dev  ? "Developer"
                             : is_staff ? "Staff"
                                        : "Player";
            snprintf(g_toast_text, sizeof(g_toast_text),
                     "Connected as %s  [%s]", username, role);
        }
        g_auth_received.store(true, std::memory_order_release);
    } else if (!g_auth_received.load(std::memory_order_relaxed)) {
        // No-auth response and no prior auth — clear everything.
        // If devmenu_token_loaded or a previous EF_AUTH already ran,
        // don't evict that state (e.g. REQUIRE_AUTH=false on the server).
        g_is_staff.store(false);
        g_is_dev.store(false);
        g_username[0] = '\0';
    }
    LeaveCriticalSection(&g_state_cs);
}

extern "C" void devmenu_set_command_callback(CommandDispatchFn cb)
{
    g_command_cb = cb;
}

extern "C" void devmenu_token_loaded(const char *username, int is_staff, int is_dev)
{
    // Pre-auth from server at game launch — update auth state and queue toast.
    // The toast timer is started on the first rendered frame (hooked_present),
    // so it shows in the game world rather than during the loading screen.
    g_is_staff.store(is_staff != 0);
    g_is_dev.store(is_dev != 0);
    EnterCriticalSection(&g_state_cs);
    if (username && username[0]) {
        strncpy(g_username, username, sizeof(g_username) - 1);
        g_username[255] = '\0';
        if (!g_toast_shown.load(std::memory_order_relaxed)) {
            const char *role = is_dev  ? "Developer"
                             : is_staff ? "Staff"
                                        : "Player";
            snprintf(g_toast_text, sizeof(g_toast_text),
                     "Authenticated as %s  [%s]", username, role);
        }
        g_auth_received.store(true, std::memory_order_release);
    }
    LeaveCriticalSection(&g_state_cs);
}
