// In-game ImGui overlay for Spellbreak dev menu.
//
// Hooks IDXGISwapChain::Present (vtable slot 8) so the UI renders directly
// inside the game's frame.
//
// Layout: left nav sidebar (sections) + right content panel (commands).
// Keyboard: Up/Down navigate items, Right/Enter enters panel, Left/Backspace goes back.

// Standard C++ headers first — MinGW requires these before <windows.h>.
#include <atomic>
#include <cstring>
#include <cstdio>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

// ── C ABI ─────────────────────────────────────────────────────────────────────

extern "C" {
    typedef void (*CommandDispatchFn)(const unsigned short *cmd, int len);
    __declspec(dllexport) void devmenu_init();
    __declspec(dllexport) void devmenu_show();
    __declspec(dllexport) void devmenu_hide();
    __declspec(dllexport) void devmenu_set_staff(int is_staff, int is_dev, const char *username);
    __declspec(dllexport) void devmenu_set_command_callback(CommandDispatchFn cb);
}

// ── Auth / display state ──────────────────────────────────────────────────────

static std::atomic<bool> g_show     { false };
static std::atomic<bool> g_is_staff { false };
static std::atomic<bool> g_is_dev   { false };
static char              g_username[256] {};
static CRITICAL_SECTION  g_state_cs;
static CommandDispatchFn g_command_cb = nullptr;

// ── D3D / ImGui state ─────────────────────────────────────────────────────────

static ID3D11Device           *g_device  = nullptr;
static ID3D11DeviceContext    *g_context = nullptr;
static ID3D11RenderTargetView *g_rtv     = nullptr;
static HWND                    g_hwnd    = nullptr;
static std::atomic<bool>       g_imgui_ready { false };

// ── Present vtable hook ───────────────────────────────────────────────────────

typedef HRESULT (STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain *, UINT, UINT);
static PresentFn g_orig_present = nullptr;

// ── WndProc hook ─────────────────────────────────────────────────────────────

static WNDPROC g_orig_wndproc = nullptr;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static LRESULT CALLBACK hooked_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_show.load() && g_imgui_ready.load()) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
        const ImGuiIO &io = ImGui::GetIO();
        if (io.WantCaptureMouse) {
            switch (msg) {
            case WM_LBUTTONDOWN: case WM_LBUTTONUP:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP:
            case WM_MOUSEMOVE:   case WM_MOUSEWHEEL:
                return 0;
            }
        }
        if (io.WantCaptureKeyboard) {
            switch (msg) {
            case WM_KEYDOWN: case WM_KEYUP:
            case WM_SYSKEYDOWN: case WM_SYSKEYUP:
            case WM_CHAR:
                return 0;
            }
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

// ── Nav / menu state ──────────────────────────────────────────────────────────

static int  g_section        = 0;     // 0=Match 1=Bot 2=Items 3=Staff 4=Dev
static int  g_item           = 0;     // highlighted item in right panel
static bool g_in_right       = false; // keyboard focus: false=nav, true=content
static bool g_enter_consumed = false; // prevent same-frame nav→exec double-fire

// ── Per-section state ─────────────────────────────────────────────────────────

static bool  s_allow_round_end = true;

static int   s_num_bots = 5;
static bool  s_no_aggro = false;
static int   s_bot_diff = 2;

static int   s_g_elem = 0, s_g_tier = 2, s_g_amt = 1;
static char  s_rune_name[32] = "Sprint";
static int   s_r_tier = 2, s_r_amt = 1;
static int   s_am_tier = 2, s_am_amt = 1;
static int   s_bt_tier = 2, s_bt_amt = 1;
static char  s_class_buf[64] = "";
static char  s_perk_buf[64]  = "";

static float s_superspeed  = 1.0f;
static bool  s_boons       = true;
static bool  s_zones       = true;
static bool  s_npcs        = true;
static int   s_arena_idx   = 0;
static int   s_friend_pts  = 100;
static int   s_enemy_pts   = 100;

static char  s_effect_buf[256] = "";

// ── Option tables ─────────────────────────────────────────────────────────────

static const char *k_elements[]  = {"Fire","Frost","Lightning","Stone","Wind","Plague"};
static const char *k_tiers[]     = {"Tier 1","Tier 2","Tier 3","Tier 4","Tier 5"};
static const char *k_bot_diffs[] = {"Very Easy","Easy","Medium","Hard","No Bots"};
static const char *k_arenas[]    = {"Hymnwood","Halcyon","Dustpool","Bogmore","Banehelm"};

// ── Row helpers ───────────────────────────────────────────────────────────────

// Call after TableNextRow. Highlights the row if it is the selected item and
// draws an invisible full-row selectable to capture mouse clicks.
// Returns whether the row is keyboard-selected.
static bool row(int idx, const char *label)
{
    bool is_sel = g_in_right && g_item == idx;
    if (is_sel)
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(45, 65, 125, 210));

    ImGui::TableSetColumnIndex(0);
    bool clicked = ImGui::Selectable("##s", false,
        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
        ImVec2(0, ImGui::GetFrameHeight()));
    if (clicked) { g_item = idx; g_in_right = true; }
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::TextUnformatted(label);
    return is_sel;
}

// Returns true if the keyboard Enter was pressed on this item.
// g_enter_consumed prevents the same Enter that moved focus from nav to content
// from also immediately executing the first item.
static bool kbd_enter(int idx)
{
    return g_in_right && g_item == idx
        && !g_enter_consumed
        && !ImGui::IsAnyItemActive()
        && ImGui::IsKeyPressed(ImGuiKey_Enter, false);
}

// ── Section: Match ────────────────────────────────────────────────────────────

static void draw_match()
{
    if (!ImGui::BeginTable("##match", 2, ImGuiTableFlags_None)) return;
    ImGui::TableSetupColumn("lbl",  ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("ctrl", ImGuiTableColumnFlags_WidthFixed, 120.0f);

    auto btn = [&](int idx, const char *lbl, const char *cmd) {
        ImGui::TableNextRow();
        bool sel = row(idx, lbl);
        ImGui::TableSetColumnIndex(1);
        char id[24]; snprintf(id, sizeof(id), "Run##%d", idx);
        if (ImGui::Button(id, ImVec2(-1, 0)) || (sel && kbd_enter(idx)))
            dispatch_ascii(cmd);
    };

    btn(0, "Start Match",          "StartMatch");
    btn(1, "Start Infinite Match", "StartInfiniteMatch");

    ImGui::TableNextRow();
    bool sel2 = row(2, "Allow Round End");
    ImGui::TableSetColumnIndex(1);
    if (ImGui::Button(s_allow_round_end ? "ON###are" : "OFF###are", ImVec2(-1,0))
        || (sel2 && kbd_enter(2))) {
        s_allow_round_end = !s_allow_round_end;
        dispatch_ascii(s_allow_round_end ? "SetAllowRoundEnd 1" : "SetAllowRoundEnd 0");
    }

    btn(3, "Stop Countdown",   "StopCountdown");
    btn(4, "Resume Countdown", "ResumeCountdown");
    btn(5, "Stop Circles",     "StopCircles");
    btn(6, "Close Circle",     "CloseCircle");

    ImGui::EndTable();
}

// ── Section: Bot ─────────────────────────────────────────────────────────────

static void draw_bot()
{
    if (!ImGui::BeginTable("##bot", 2, ImGuiTableFlags_None)) return;
    ImGui::TableSetupColumn("lbl",  ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("ctrl", ImGuiTableColumnFlags_WidthFixed, 160.0f);

    ImGui::TableNextRow();
    bool sel0 = row(0, "Spawn Bot");
    ImGui::TableSetColumnIndex(1);
    if (ImGui::Button("Spawn##sb", ImVec2(-1,0)) || (sel0 && kbd_enter(0)))
        dispatch_ascii("SpawnMatchBot");

    ImGui::TableNextRow();
    bool sel1 = row(1, "Reveal Nearby Bots");
    ImGui::TableSetColumnIndex(1);
    if (ImGui::Button("Reveal##rnb", ImVec2(-1,0)) || (sel1 && kbd_enter(1)))
        dispatch_ascii("RevealNearbyMatchBots");

    ImGui::TableNextRow();
    row(2, "Num Bots");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-65.0f);
    ImGui::DragInt("##nb", &s_num_bots, 0.2f, 0, 20, "%d bots");
    ImGui::SameLine();
    if (ImGui::Button("Set##nb", ImVec2(-1,0))) {
        char buf[64]; snprintf(buf, sizeof(buf), "SetNumMatchBots %d", s_num_bots);
        dispatch_ascii(buf);
    }

    ImGui::TableNextRow();
    bool sel3 = row(3, "No Bot Aggro");
    ImGui::TableSetColumnIndex(1);
    if (ImGui::Button(s_no_aggro ? "ON###nba" : "OFF###nba", ImVec2(-1,0))
        || (sel3 && kbd_enter(3))) {
        s_no_aggro = !s_no_aggro;
        dispatch_ascii(s_no_aggro ? "SetNoMatchBotAggro true" : "SetNoMatchBotAggro false");
    }

    ImGui::TableNextRow();
    row(4, "Bot Difficulty");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-65.0f);
    ImGui::Combo("##bd", &s_bot_diff, k_bot_diffs, 5);
    ImGui::SameLine();
    if (ImGui::Button("Set##bd", ImVec2(-1,0))) {
        char buf[64]; snprintf(buf, sizeof(buf), "SetMatchBotDifficulty %d", s_bot_diff);
        dispatch_ascii(buf);
    }

    ImGui::EndTable();
}

// ── Section: Items ────────────────────────────────────────────────────────────

static void draw_items()
{
    if (!ImGui::BeginTable("##items", 2, ImGuiTableFlags_None)) return;
    ImGui::TableSetupColumn("lbl",  ImGuiTableColumnFlags_WidthFixed, 120.0f);
    ImGui::TableSetupColumn("ctrl", ImGuiTableColumnFlags_WidthStretch);

    // Gauntlet
    ImGui::TableNextRow();
    bool sel0 = row(0, "Spawn Gauntlet");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(80.0f);  ImGui::Combo("##ge", &s_g_elem, k_elements, 6);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(68.0f);  ImGui::Combo("##gt", &s_g_tier, k_tiers, 5);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(44.0f);  ImGui::DragInt("##ga", &s_g_amt, 0.1f, 1, 99, "x%d");
    ImGui::SameLine();
    if (ImGui::Button("Spawn##g") || (sel0 && kbd_enter(0))) {
        char buf[128];
        snprintf(buf, sizeof(buf), "SpawnGauntlet Loot:BP_Item_Weapon_%s_Tier_%d %d",
            k_elements[s_g_elem], s_g_tier + 1, s_g_amt);
        dispatch_ascii(buf);
    }

    // Rune
    ImGui::TableNextRow();
    bool sel1 = row(1, "Spawn Rune");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputText("##rn", s_rune_name, sizeof(s_rune_name));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(68.0f);  ImGui::Combo("##rt", &s_r_tier, k_tiers, 5);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(44.0f);  ImGui::DragInt("##ra", &s_r_amt, 0.1f, 1, 99, "x%d");
    ImGui::SameLine();
    if (ImGui::Button("Spawn##r") || (sel1 && kbd_enter(1))) {
        char buf[128];
        snprintf(buf, sizeof(buf), "SpawnRune Loot:BP_Item_Rune_%s_Tier_%d %d",
            s_rune_name, s_r_tier + 1, s_r_amt);
        dispatch_ascii(buf);
    }

    // Amulet
    ImGui::TableNextRow();
    bool sel2 = row(2, "Spawn Amulet");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(68.0f);  ImGui::Combo("##amt", &s_am_tier, k_tiers, 5);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(44.0f);  ImGui::DragInt("##ama", &s_am_amt, 0.1f, 1, 99, "x%d");
    ImGui::SameLine();
    if (ImGui::Button("Spawn##am") || (sel2 && kbd_enter(2))) {
        char buf[128];
        snprintf(buf, sizeof(buf), "SpawnAmulet Loot:BP_Item_Amulet_Tier_%d %d",
            s_am_tier + 1, s_am_amt);
        dispatch_ascii(buf);
    }

    // Boot
    ImGui::TableNextRow();
    bool sel3 = row(3, "Spawn Boot");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(68.0f);  ImGui::Combo("##btt", &s_bt_tier, k_tiers, 5);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(44.0f);  ImGui::DragInt("##bta", &s_bt_amt, 0.1f, 1, 99, "x%d");
    ImGui::SameLine();
    if (ImGui::Button("Spawn##bt") || (sel3 && kbd_enter(3))) {
        char buf[128];
        snprintf(buf, sizeof(buf), "SpawnBoot Loot:BP_Item_Boots_Tier_%d %d",
            s_bt_tier + 1, s_bt_amt);
        dispatch_ascii(buf);
    }

    // Set Class
    ImGui::TableNextRow();
    row(4, "Set Class");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-65.0f);
    bool cls_enter = ImGui::InputText("##cls", s_class_buf, sizeof(s_class_buf),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Set##cls", ImVec2(-1,0)) || cls_enter) && s_class_buf[0]) {
        char buf[128];
        snprintf(buf, sizeof(buf), "ChooseCharacterClass CharacterClass:BP_CharacterClass_%s",
            s_class_buf);
        dispatch_ascii(buf);
    }

    // Add Perk
    ImGui::TableNextRow();
    row(5, "Add Perk");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-65.0f);
    bool prk_enter = ImGui::InputText("##prk", s_perk_buf, sizeof(s_perk_buf),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Add##prk", ImVec2(-1,0)) || prk_enter) && s_perk_buf[0]) {
        char buf[128];
        snprintf(buf, sizeof(buf), "ChooseCharacterPerk CharacterPerk:BP_Perk_%s", s_perk_buf);
        dispatch_ascii(buf);
    }

    // Reset Perks
    ImGui::TableNextRow();
    bool sel6 = row(6, "Reset Perks");
    ImGui::TableSetColumnIndex(1);
    if (ImGui::Button("Reset##rp", ImVec2(-1,0)) || (sel6 && kbd_enter(6)))
        dispatch_ascii("ResetCharacterPerks");

    ImGui::EndTable();
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
    ImGui::TableSetupColumn("ctrl", ImGuiTableColumnFlags_WidthFixed, 160.0f);

    auto btn = [&](int idx, const char *lbl, const char *cmd) {
        ImGui::TableNextRow();
        bool sel = row(idx, lbl);
        ImGui::TableSetColumnIndex(1);
        char id[24]; snprintf(id, sizeof(id), "Run##s%d", idx);
        if (ImGui::Button(id, ImVec2(-1, 0)) || (sel && kbd_enter(idx)))
            dispatch_ascii(cmd);
    };

    auto tog = [&](int idx, const char *lbl, bool &state,
                   const char *on_cmd, const char *off_cmd) {
        ImGui::TableNextRow();
        bool sel = row(idx, lbl);
        ImGui::TableSetColumnIndex(1);
        char id[24]; snprintf(id, sizeof(id), "%s###t%d", state ? "ON" : "OFF", idx);
        if (ImGui::Button(id, ImVec2(-1, 0)) || (sel && kbd_enter(idx))) {
            state = !state;
            dispatch_ascii(state ? on_cmd : off_cmd);
        }
    };

    btn(0, "God Mode",           "God");
    btn(1, "Fast Cooldowns",     "FastCooldowns");
    btn(2, "Die",                "Die");

    // Superspeed
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

    btn(4, "Level Up Class",     "LevelUpCharacterClass");
    btn(5, "Toggle HUD",         "ToggleHUD");
    btn(6, "Toggle Debug Camera","ToggleDebugCamera");

    tog(7,  "Boons", s_boons, "ToggleBoons 1",  "ToggleBoons 0");
    tog(8,  "Zones", s_zones, "ToggleZones 1",  "ToggleZones 0");
    tog(9,  "NPCs",  s_npcs,  "ToggleNPCs 1",   "ToggleNPCs 0");

    // Set Arena
    ImGui::TableNextRow();
    row(10, "Set Arena");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-65.0f);
    ImGui::Combo("##ar", &s_arena_idx, k_arenas, 5);
    ImGui::SameLine();
    if (ImGui::Button("Set##ar", ImVec2(-1,0))) {
        char buf[64]; snprintf(buf, sizeof(buf), "SetArena %s", k_arenas[s_arena_idx]);
        dispatch_ascii(buf);
    }

    btn(11, "Switch Team", "SwitchTeam");

    // Add Friendly Points
    ImGui::TableNextRow();
    row(12, "Friendly Points");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-65.0f);
    ImGui::DragInt("##fp", &s_friend_pts, 1.0f, 0, 9999, "%d pts");
    ImGui::SameLine();
    if (ImGui::Button("Add##fp", ImVec2(-1,0))) {
        char buf[64]; snprintf(buf, sizeof(buf), "AddFriendlyPoints %d", s_friend_pts);
        dispatch_ascii(buf);
    }

    // Add Enemy Points
    ImGui::TableNextRow();
    row(13, "Enemy Points");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-65.0f);
    ImGui::DragInt("##ep", &s_enemy_pts, 1.0f, 0, 9999, "%d pts");
    ImGui::SameLine();
    if (ImGui::Button("Add##ep", ImVec2(-1,0))) {
        char buf[64]; snprintf(buf, sizeof(buf), "AddEnemyPoints %d", s_enemy_pts);
        dispatch_ascii(buf);
    }

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

    // Apply Player Effect
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

    // Teleport (in-game sequence, not a console command)
    ImGui::TableNextRow();
    row(1, "Teleport");
    ImGui::TableSetColumnIndex(1);
    ImGui::TextDisabled("Press M (map), hold T, click destination");

    ImGui::EndTable();
}

// ── Left nav ──────────────────────────────────────────────────────────────────

static void draw_nav()
{
    static const char *labels[] = {
        "Match Settings",
        "Bot Settings",
        "Item Settings",
        "Staff Commands",
        "Dev Settings",
    };

    const bool is_staff = g_is_staff.load();
    const bool is_dev   = g_is_dev.load();
    const bool authed[] = { true, true, true, is_staff, is_dev };

    for (int i = 0; i < 5; ++i) {
        if (i == 3) { ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing(); }

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
    case 0: return 7;
    case 1: return 5;
    case 2: return 7;
    case 3: return g_is_staff.load() ? 14 : 0;
    case 4: return g_is_dev.load()   ?  2 : 0;
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
        if (dn) g_section = (g_section + 1) % 5;
        if (up) g_section = (g_section + 4) % 5;
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

// ── ImGui render ──────────────────────────────────────────────────────────────

static void render_frame()
{
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGui::GetIO().MouseDrawCursor = true;

    handle_nav_keys();

    // Build title bar string.
    char title[320];
    EnterCriticalSection(&g_state_cs);
    if (g_is_staff.load() && g_username[0])
        snprintf(title, sizeof(title),
                 "Game Control Menu  -  %s  -  [STAFF]", g_username);
    else
        snprintf(title, sizeof(title), "Game Control Menu");
    LeaveCriticalSection(&g_state_cs);

    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(700, 400), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(80, 80),    ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(480, 250), ImVec2(9999, 9999));
    ImGui::Begin(title, &open);

    // ── Left nav (fixed 180 px) ────────────────────────────────────────────
    ImGui::BeginChild("##nav", ImVec2(180.0f, 0.0f), false);
    draw_nav();
    ImGui::EndChild();

    ImGui::SameLine(0.0f, 6.0f);

    // Vertical separator line
    ImVec2 sep_min = ImGui::GetCursorScreenPos();
    sep_min.x -= 3.0f;
    ImVec2 sep_max = sep_min;
    sep_max.y += ImGui::GetContentRegionAvail().y;
    ImGui::GetWindowDrawList()->AddLine(sep_min, sep_max,
        IM_COL32(80, 80, 80, 180), 1.0f);

    // ── Right content ──────────────────────────────────────────────────────
    ImGui::BeginChild("##content", ImVec2(0.0f, 0.0f), false);
    switch (g_section) {
    case 0: draw_match(); break;
    case 1: draw_bot();   break;
    case 2: draw_items(); break;
    case 3: draw_staff(); break;
    case 4: draw_dev();   break;
    }
    ImGui::EndChild();

    ImGui::End();

    if (!open) g_show.store(false);

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
    swap->GetDevice(__uuidof(ID3D11Device), (void **)&g_device);
    g_device->GetImmediateContext(&g_context);
    create_rtv(swap);

    DXGI_SWAP_CHAIN_DESC desc {};
    swap->GetDesc(&desc);
    g_hwnd = desc.OutputWindow;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
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

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    g_orig_wndproc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(hooked_wndproc)));

    g_imgui_ready.store(true, std::memory_order_release);
}

// ── Present hook ──────────────────────────────────────────────────────────────

static HRESULT STDMETHODCALLTYPE hooked_present(IDXGISwapChain *swap, UINT sync, UINT flags)
{
    if (!g_imgui_ready.load(std::memory_order_acquire))
        init_imgui(swap);

    if (g_show.load())
        render_frame();
    else if (g_imgui_ready.load())
        ImGui::GetIO().MouseDrawCursor = false;

    return g_orig_present(swap, sync, flags);
}

// ── Vtable patch ──────────────────────────────────────────────────────────────

static bool patch_present(IDXGISwapChain *swap)
{
    void **vtbl = *reinterpret_cast<void ***>(swap);
    void **slot  = &vtbl[8];
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
    HRESULT hr = g_orig_create_sc(fac, dev, desc, ppSC);
    if (SUCCEEDED(hr) && ppSC && *ppSC)
        patch_present(*ppSC);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hooked_create_sc_hwnd(
    IDXGIFactory2 *fac, IUnknown *dev, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1 *desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fdesc,
    IDXGIOutput *output, IDXGISwapChain1 **ppSC)
{
    HRESULT hr = g_orig_create_sc_hwnd(fac, dev, hwnd, desc, fdesc, output, ppSC);
    if (SUCCEEDED(hr) && ppSC && *ppSC)
        patch_present(reinterpret_cast<IDXGISwapChain *>(*ppSC));
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

// Install hooks on IDXGIFactory::CreateSwapChain (slot 10) and
// IDXGIFactory2::CreateSwapChainForHwnd (slot 15) by creating a throwaway
// factory and patching its class vtable.  We never call D3D11CreateDevice here.
static void hook_factory()
{
    typedef HRESULT (WINAPI *CreateDXGIFactory2Fn)(UINT, REFIID, void **);
    typedef HRESULT (WINAPI *CreateDXGIFactory1Fn)(REFIID, void **);

    // Load dxgi.dll (idempotent — returns the already-loaded module if present).
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (!dxgi) return;

    // Prefer DXGI 1.2 so we also hook CreateSwapChainForHwnd (UE4's path).
    auto *cf2 = reinterpret_cast<CreateDXGIFactory2Fn>(
        GetProcAddress(dxgi, "CreateDXGIFactory2"));
    if (cf2) {
        IDXGIFactory2 *fac = nullptr;
        if (SUCCEEDED(cf2(0, __uuidof(IDXGIFactory2), reinterpret_cast<void **>(&fac)))) {
            void **vtbl = *reinterpret_cast<void ***>(fac);
            patch_vtbl_slot(vtbl, 10,
                reinterpret_cast<void *>(&hooked_create_sc),
                reinterpret_cast<void **>(&g_orig_create_sc));
            patch_vtbl_slot(vtbl, 15,
                reinterpret_cast<void *>(&hooked_create_sc_hwnd),
                reinterpret_cast<void **>(&g_orig_create_sc_hwnd));
            fac->Release();
            return;
        }
    }

    // Fallback: DXGI 1.1 — hook CreateSwapChain only.
    auto *cf1 = reinterpret_cast<CreateDXGIFactory1Fn>(
        GetProcAddress(dxgi, "CreateDXGIFactory1"));
    if (!cf1) return;
    IDXGIFactory1 *fac = nullptr;
    if (FAILED(cf1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&fac)))) return;
    void **vtbl = *reinterpret_cast<void ***>(fac);
    patch_vtbl_slot(vtbl, 10,
        reinterpret_cast<void *>(&hooked_create_sc),
        reinterpret_cast<void **>(&g_orig_create_sc));
    fac->Release();
}

// ── Exports ───────────────────────────────────────────────────────────────────

extern "C" void devmenu_init()
{
    InitializeCriticalSection(&g_state_cs);
    hook_factory();
}

extern "C" void devmenu_show() { g_show.store(true); }
extern "C" void devmenu_hide()
{
    g_show.store(false);
    if (g_imgui_ready.load()) ImGui::GetIO().MouseDrawCursor = false;
}

extern "C" void devmenu_set_staff(int is_staff, int is_dev, const char *username)
{
    g_is_staff.store(is_staff != 0);
    g_is_dev.store(is_dev != 0);
    EnterCriticalSection(&g_state_cs);
    if (username && username[0])
        strncpy(g_username, username, sizeof(g_username) - 1);
    else
        g_username[0] = '\0';
    LeaveCriticalSection(&g_state_cs);
}

extern "C" void devmenu_set_command_callback(CommandDispatchFn cb)
{
    g_command_cb = cb;
}
