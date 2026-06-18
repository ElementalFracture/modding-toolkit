#include "devmenu_qt.h"
#include "DevMenuWindow.h"

#include <QApplication>
#include <QMetaObject>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

// ---------------------------------------------------------------------------
// Globals shared between the init thread and the show/hide exports
// ---------------------------------------------------------------------------
static DevMenuWindow          *g_window    = nullptr;
static std::atomic<bool>       g_ready     { false };
static std::mutex              g_mutex;
static std::condition_variable g_cv;

// ---------------------------------------------------------------------------
// Qt event-loop thread
// ---------------------------------------------------------------------------
static void qt_thread_main()
{
    // QApplication requires argc/argv — synthesise a minimal set.
    static int    argc = 1;
    static char   argv0[] = "devmenu_qt";
    static char  *argv[]  = { argv0, nullptr };

    QApplication app(argc, argv);

    g_window = new DevMenuWindow();
    // Window starts hidden; devmenu_show() will make it visible.

    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_ready.store(true, std::memory_order_release);
    }
    g_cv.notify_all();

    app.exec();     // blocks until QApplication::quit() is called
    delete g_window;
    g_window = nullptr;
}

// ---------------------------------------------------------------------------
// C-ABI exports
// ---------------------------------------------------------------------------
extern "C" {

void devmenu_init()
{
    // Spawn the Qt event-loop thread and return immediately so the Rust side
    // (which called us from its own background thread) is not blocked.
    std::thread(qt_thread_main).detach();

    // Wait until QApplication + DevMenuWindow are constructed so that
    // subsequent show/hide calls can safely reference g_window.
    std::unique_lock<std::mutex> lk(g_mutex);
    g_cv.wait(lk, [] { return g_ready.load(std::memory_order_acquire); });
}

void devmenu_show()
{
    if (!g_window)
        return;
    // QMetaObject::invokeMethod is thread-safe and queues onto the Qt thread.
    QMetaObject::invokeMethod(g_window, "show", Qt::QueuedConnection);
    QMetaObject::invokeMethod(g_window, "raise", Qt::QueuedConnection);
    QMetaObject::invokeMethod(g_window, "activateWindow", Qt::QueuedConnection);
}

void devmenu_hide()
{
    if (!g_window)
        return;
    QMetaObject::invokeMethod(g_window, "hide", Qt::QueuedConnection);
}

void devmenu_set_command_callback(DevMenuCommandCallback cb)
{
    if (g_window)
        g_window->setCommandCallback(cb);
}

} // extern "C"
