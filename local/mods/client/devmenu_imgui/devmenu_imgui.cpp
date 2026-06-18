// In-game ImGui overlay for Spellbreak dev menu.
//
// Hooks IDXGISwapChain::Present (vtable slot 8) so the UI renders directly
// inside the game's frame.  Same five-export C ABI as the old devmenu_qt.dll.
//
// Initialization sequence:
//   1. devmenu_init() — create dummy window + D3D11 device, patch vtable.
//   2. First real Present call — grab game device/context/HWND, init ImGui.
//   3. Every subsequent Present — if g_show, render the ImGui frame.

// Standard C++ headers first — MinGW requires these before <windows.h>.
#include <atomic>
#include <cstring>
#include <cstdio>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

// ── C ABI ─────────────────────────────────────────────────────────────────────

extern "C" {
    typedef void (*CommandDispatchFn)(const unsigned short *cmd, int len);
    __declspec(dllexport) void devmenu_init();
    __declspec(dllexport) void devmenu_show();
    __declspec(dllexport) void devmenu_hide();
    __declspec(dllexport) void devmenu_set_staff(int is_staff, const char *username);
    __declspec(dllexport) void devmenu_set_command_callback(CommandDispatchFn cb);
}

// ── Menu state ────────────────────────────────────────────────────────────────

static std::atomic<bool> g_show     { false };
static std::atomic<bool> g_is_staff { false };
static char              g_username[256] {};
static CRITICAL_SECTION  g_state_cs;   // guards g_username; init'd in devmenu_init
static CommandDispatchFn g_command_cb = nullptr;
// Set to true once on first Present; tells render_frame to re-focus the input.
static std::atomic<bool> g_focus_input { false };

// ── D3D / ImGui state ─────────────────────────────────────────────────────────

static ID3D11Device           *g_device  = nullptr;
static ID3D11DeviceContext    *g_context = nullptr;
static ID3D11RenderTargetView *g_rtv     = nullptr;
static HWND                    g_hwnd    = nullptr;
static std::atomic<bool>       g_imgui_ready { false };

// ── Present vtable hook ───────────────────────────────────────────────────────

typedef HRESULT (STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain *, UINT, UINT);
static PresentFn g_orig_present = nullptr;

// ── WndProc hook (for ImGui input) ───────────────────────────────────────────

static WNDPROC g_orig_wndproc = nullptr;

extern IMGUI_IMPL_API LRESULT
ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

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

static void dispatch_wide(const wchar_t *cmd, int len)
{
    if (g_command_cb)
        g_command_cb(reinterpret_cast<const unsigned short *>(cmd), len);
}

static void dispatch_ascii(const char *cmd)
{
    const int len = static_cast<int>(strlen(cmd));
    if (len <= 0) return;
    wchar_t wide[512] {};
    for (int i = 0; i < len && i < 511; ++i)
        wide[i] = static_cast<wchar_t>(static_cast<unsigned char>(cmd[i]));
    dispatch_wide(wide, len);
}

// ── ImGui initialization (first real Present call) ───────────────────────────

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
    s.Alpha          = 0.93f;
    s.WindowRounding = 5.0f;
    s.FrameRounding  = 3.0f;
    // Slightly more prominent title bar
    s.Colors[ImGuiCol_TitleBg]       = ImVec4(0.08f, 0.08f, 0.12f, 1.0f);
    s.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.14f, 0.14f, 0.22f, 1.0f);

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    g_orig_wndproc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(hooked_wndproc)));

    g_imgui_ready.store(true, std::memory_order_release);
}

// ── ImGui render (called every Present while g_show) ─────────────────────────

static void render_frame()
{
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGui::GetIO().MouseDrawCursor = true;

    // Build title.
    bool is_staff = g_is_staff.load();
    char title[320];
    EnterCriticalSection(&g_state_cs);
    if (is_staff && g_username[0])
        snprintf(title, sizeof(title),
                 "Game Control Menu  -  %s  -  [STAFF]", g_username);
    else
        snprintf(title, sizeof(title), "Game Control Menu");
    LeaveCriticalSection(&g_state_cs);

    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(80, 80), ImGuiCond_FirstUseEver);
    ImGui::Begin(title, &open, ImGuiWindowFlags_AlwaysAutoResize);

    // ── Free-form command input ────────────────────────────────────────────
    static char cmd_buf[512] {};
    static bool focus_next = false;

    if (g_focus_input.exchange(false) || focus_next) {
        ImGui::SetKeyboardFocusHere(0);
        focus_next = false;
    }

    ImGui::SetNextItemWidth(330.0f);
    const bool enter = ImGui::InputText("##cmd", cmd_buf, sizeof(cmd_buf),
                                        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Run") || enter) {
        if (cmd_buf[0]) {
            dispatch_ascii(cmd_buf);
            cmd_buf[0] = '\0';
        }
        focus_next = true;
    }

    // ── Staff section ──────────────────────────────────────────────────────
    if (is_staff) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.2f, 1.0f), "Staff Commands");
        if (ImGui::Button("Start Match"))
            dispatch_wide(L"StartMatch", 10);
        ImGui::SameLine();
        if (ImGui::Button("God Mode"))
            dispatch_wide(L"God", 3);
    }

    ImGui::End();

    // X button closed the window — propagate to g_show.
    if (!open) g_show.store(false);

    ImGui::Render();
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

// ── Hooked Present ────────────────────────────────────────────────────────────

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
    // IDXGISwapChain vtable: Present is at index 8.
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

// ── Exports ───────────────────────────────────────────────────────────────────

extern "C" void devmenu_init()
{
    InitializeCriticalSection(&g_state_cs);

    // Create a throw-away device+swapchain to get the IDXGISwapChain vtable.
    WNDCLASSEXW wc {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"_EF_ImGuiInit";
    RegisterClassExW(&wc);

    HWND dummy = CreateWindowExW(0, L"_EF_ImGuiInit", L"",
                                 WS_OVERLAPPED, 0, 0, 8, 8,
                                 nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd {};
    sd.BufferCount        = 1;
    sd.BufferDesc.Format  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow       = dummy;
    sd.SampleDesc.Count   = 1;
    sd.Windowed           = TRUE;
    sd.SwapEffect         = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device        *dev  = nullptr;
    ID3D11DeviceContext *ctx  = nullptr;
    IDXGISwapChain      *swap = nullptr;
    D3D_FEATURE_LEVEL    fl;

    D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &sd, &swap, &dev, &fl, &ctx);

    if (swap) {
        patch_present(swap);
        swap->Release();
    }
    if (ctx) ctx->Release();
    if (dev) dev->Release();

    DestroyWindow(dummy);
    UnregisterClassW(L"_EF_ImGuiInit", wc.hInstance);
}

extern "C" void devmenu_show()
{
    g_focus_input.store(true);
    g_show.store(true);
}

extern "C" void devmenu_hide()
{
    g_show.store(false);
    if (g_imgui_ready.load())
        ImGui::GetIO().MouseDrawCursor = false;
}

extern "C" void devmenu_set_staff(int is_staff, const char *username)
{
    g_is_staff.store(is_staff != 0);
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
