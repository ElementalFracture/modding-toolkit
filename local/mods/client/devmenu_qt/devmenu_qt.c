#include <windows.h>

static HWND  g_hwnd       = NULL;
static HANDLE g_ready_evt = NULL;

typedef void (*CommandDispatchFn)(const unsigned short *cmd, int len);
static CommandDispatchFn g_command_cb = NULL;

/* Private messages posted by devmenu_show/hide from arbitrary threads.
   PostMessage is non-blocking and never pumps the caller's message queue,
   avoiding re-entrancy when called from inside a vtable hook. */
#define WM_DM_SHOW (WM_USER + 1)
#define WM_DM_HIDE (WM_USER + 2)

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_DM_SHOW:
        ShowWindow(hwnd, SW_SHOW);
        return 0;
    case WM_DM_HIDE:
        ShowWindow(hwnd, SW_HIDE);
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
        200, 200, 480, 320,
        NULL, NULL, hInst, NULL
    );

    CreateWindowW(L"STATIC", L"It works!\nFakeGotoState hook is live.\nTilde is now intercepted.",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        20, 80, 420, 120,
        g_hwnd, NULL, hInst, NULL);

    /* Signal devmenu_init() that the window is ready. */
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
        PostMessage(g_hwnd, WM_DM_SHOW, 0, 0);
}

__declspec(dllexport) void devmenu_hide(void)
{
    if (g_hwnd)
        PostMessage(g_hwnd, WM_DM_HIDE, 0, 0);
}

__declspec(dllexport) void devmenu_set_command_callback(CommandDispatchFn cb)
{
    g_command_cb = cb;
}
