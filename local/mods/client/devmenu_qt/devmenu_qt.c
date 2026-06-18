#include <windows.h>
#include <stdio.h>

static HWND g_hwnd            = NULL;
static HANDLE g_ready_evt     = NULL;

typedef void (*CommandDispatchFn)(const unsigned short *cmd, int len);
static CommandDispatchFn g_command_cb = NULL;

/* Staff-only controls — hidden until devmenu_set_staff(1, ...) is called. */
static HWND g_sep_staff       = NULL;
static HWND g_lbl_staff       = NULL;
static HWND g_btn_start_match = NULL;
static HWND g_btn_god_mode    = NULL;

#define ID_BTN_START_MATCH  101
#define ID_EDIT_CMD         102
#define ID_BTN_RUN          103
#define ID_BTN_GOD_MODE     104

static void dispatch(const wchar_t *cmd, int len)
{
    if (g_command_cb)
        g_command_cb((const unsigned short *)cmd, len);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_BTN_START_MATCH:
            dispatch(L"StartMatch", 10);
            break;
        case ID_BTN_GOD_MODE:
            dispatch(L"God", 3);
            break;
        case ID_BTN_RUN: {
            HWND edit = GetDlgItem(hwnd, ID_EDIT_CMD);
            wchar_t buf[512] = {0};
            int len = GetWindowTextW(edit, buf, 512);
            if (len > 0)
                dispatch(buf, len);
            SetWindowTextW(edit, L"");
            break;
        }
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static DWORD WINAPI window_thread(LPVOID param)
{
    HINSTANCE hInst = GetModuleHandleW(NULL);

    WNDCLASSW wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"DevMenuWindow";
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"DevMenuWindow",
        L"Dev Menu",
        WS_OVERLAPPEDWINDOW,
        200, 200, 480, 260,
        NULL, NULL, hInst, NULL
    );

    /* ── Free-form command input ─────────────────────────────────────────── */
    CreateWindowW(L"STATIC", L"Command",
        WS_CHILD | WS_VISIBLE,
        12, 12, 60, 16, g_hwnd, NULL, hInst, NULL);

    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        12, 30, 340, 24, g_hwnd, (HMENU)ID_EDIT_CMD, hInst, NULL);

    CreateWindowW(L"BUTTON", L"Run",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        358, 30, 60, 24, g_hwnd, (HMENU)ID_BTN_RUN, hInst, NULL);

    /* ── Staff section — hidden until devmenu_set_staff(1, ...) ─────────── */
    g_sep_staff = CreateWindowW(L"STATIC", L"────────────────── Staff Commands",
        WS_CHILD,
        12, 68, 420, 16, g_hwnd, NULL, hInst, NULL);

    g_lbl_staff = CreateWindowW(L"STATIC", L"Match",
        WS_CHILD,
        12, 90, 60, 16, g_hwnd, NULL, hInst, NULL);

    g_btn_start_match = CreateWindowW(L"BUTTON", L"Start Match",
        WS_CHILD | BS_PUSHBUTTON,
        12, 108, 100, 28, g_hwnd, (HMENU)ID_BTN_START_MATCH, hInst, NULL);

    g_btn_god_mode = CreateWindowW(L"BUTTON", L"God Mode",
        WS_CHILD | BS_PUSHBUTTON,
        120, 108, 100, 28, g_hwnd, (HMENU)ID_BTN_GOD_MODE, hInst, NULL);

    SetEvent(g_ready_evt);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}

__declspec(dllexport) void devmenu_init(void)
{
    g_ready_evt = CreateEventW(NULL, TRUE, FALSE, NULL);
    CreateThread(NULL, 0, window_thread, NULL, 0, NULL);
    WaitForSingleObject(g_ready_evt, INFINITE);
    CloseHandle(g_ready_evt);
    g_ready_evt = NULL;
}

__declspec(dllexport) void devmenu_show(void)
{
    if (g_hwnd)
        ShowWindow(g_hwnd, SW_SHOW);
}

__declspec(dllexport) void devmenu_hide(void)
{
    if (g_hwnd)
        ShowWindow(g_hwnd, SW_HIDE);
}

__declspec(dllexport) void devmenu_set_staff(int is_staff, const char *username)
{
    if (!g_hwnd) return;

    int cmd = is_staff ? SW_SHOW : SW_HIDE;
    if (g_sep_staff)       ShowWindow(g_sep_staff,       cmd);
    if (g_lbl_staff)       ShowWindow(g_lbl_staff,       cmd);
    if (g_btn_start_match) ShowWindow(g_btn_start_match, cmd);
    if (g_btn_god_mode)    ShowWindow(g_btn_god_mode,    cmd);

    if (is_staff && username && username[0]) {
        char title[256];
        snprintf(title, sizeof(title),
                 "Game Control Menu - %s - [STAFF]", username);
        SetWindowTextA(g_hwnd, title);
    } else {
        SetWindowTextA(g_hwnd, "Dev Menu");
    }
}

__declspec(dllexport) void devmenu_set_command_callback(CommandDispatchFn cb)
{
    g_command_cb = cb;
}
