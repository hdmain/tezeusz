#include "platform.hpp"
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>

#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace platform {
namespace {

bool g_idleActive = false;

#ifdef _WIN32
void idleInhibitApply(bool active, const char*) {
    if (active)
        SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
    else
        SetThreadExecutionState(ES_CONTINUOUS);
}
#else
// ---- D-Bus (dlopen libdbus-1) — no hard link dependency ----
enum { DBUS_BUS_SESSION = 0, DBUS_BUS_SYSTEM = 1 };
enum { DBUS_TYPE_INVALID = 0, DBUS_TYPE_STRING = 's', DBUS_TYPE_UINT32 = 'u',
       DBUS_TYPE_INT32 = 'i', DBUS_TYPE_UNIX_FD = 'h', DBUS_TYPE_OBJECT_PATH = 'o' };
enum { DBUS_HANDLER_RESULT_HANDLED = 0 };

struct DBusError { char* name; char* message; unsigned int dummy1 : 1; unsigned int dummy2 : 1;
                   unsigned int dummy3 : 1; unsigned int dummy4 : 1; unsigned int dummy5 : 1;
                   void* padding1; };
struct DBusConnection;
struct DBusMessage;
struct DBusMessageIter;

using Fn_dbus_error_init = void (*)(DBusError*);
using Fn_dbus_error_free = void (*)(DBusError*);
using Fn_dbus_error_is_set = int (*)(const DBusError*);
using Fn_dbus_bus_get = DBusConnection* (*)(int, DBusError*);
using Fn_dbus_connection_unref = void (*)(DBusConnection*);
using Fn_dbus_connection_flush = void (*)(DBusConnection*);
using Fn_dbus_message_new_method_call = DBusMessage* (*)(const char*, const char*, const char*, const char*);
using Fn_dbus_message_unref = void (*)(DBusMessage*);
using Fn_dbus_message_append_args = int (*)(DBusMessage*, int, ...);
using Fn_dbus_connection_send_with_reply_and_block =
    DBusMessage* (*)(DBusConnection*, DBusMessage*, int, DBusError*);
using Fn_dbus_message_get_args = int (*)(DBusMessage*, DBusError*, int, ...);

struct DBusApi {
    void* mod = nullptr;
    Fn_dbus_error_init error_init = nullptr;
    Fn_dbus_error_free error_free = nullptr;
    Fn_dbus_error_is_set error_is_set = nullptr;
    Fn_dbus_bus_get bus_get = nullptr;
    Fn_dbus_connection_unref connection_unref = nullptr;
    Fn_dbus_connection_flush connection_flush = nullptr;
    Fn_dbus_message_new_method_call message_new_method_call = nullptr;
    Fn_dbus_message_unref message_unref = nullptr;
    Fn_dbus_message_append_args message_append_args = nullptr;
    Fn_dbus_connection_send_with_reply_and_block send_with_reply_and_block = nullptr;
    Fn_dbus_message_get_args message_get_args = nullptr;
    bool ok = false;
};

DBusApi& dbus() {
    static DBusApi a;
    if (a.mod || a.ok) return a;
    a.mod = dlopen("libdbus-1.so.3", RTLD_LAZY);
    if (!a.mod) a.mod = dlopen("libdbus-1.so", RTLD_LAZY);
    if (!a.mod) return a;
    auto L = [&](auto& fn, const char* n) {
        fn = reinterpret_cast<std::decay_t<decltype(fn)>>(dlsym(a.mod, n));
        return fn != nullptr;
    };
    a.ok =
        L(a.error_init, "dbus_error_init") &&
        L(a.error_free, "dbus_error_free") &&
        L(a.error_is_set, "dbus_error_is_set") &&
        L(a.bus_get, "dbus_bus_get") &&
        L(a.connection_unref, "dbus_connection_unref") &&
        L(a.connection_flush, "dbus_connection_flush") &&
        L(a.message_new_method_call, "dbus_message_new_method_call") &&
        L(a.message_unref, "dbus_message_unref") &&
        L(a.message_append_args, "dbus_message_append_args") &&
        L(a.send_with_reply_and_block, "dbus_connection_send_with_reply_and_block") &&
        L(a.message_get_args, "dbus_message_get_args");
    return a;
}

enum class SsKind { None, Freedesktop, Gnome };
SsKind g_ssKind = SsKind::None;
uint32_t g_ssCookie = 0;
int g_loginFd = -1;

void releaseScreenSaver() {
    if (g_ssKind == SsKind::None) return;
    auto& d = dbus();
    if (!d.ok) { g_ssKind = SsKind::None; g_ssCookie = 0; return; }
    DBusError err{};
    d.error_init(&err);
    DBusConnection* c = d.bus_get(DBUS_BUS_SESSION, &err);
    if (c) {
        DBusMessage* m = nullptr;
        if (g_ssKind == SsKind::Freedesktop) {
            m = d.message_new_method_call(
                "org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver",
                "org.freedesktop.ScreenSaver", "UnInhibit");
        } else {
            m = d.message_new_method_call(
                "org.gnome.SessionManager", "/org/gnome/SessionManager",
                "org.gnome.SessionManager", "Uninhibit");
        }
        if (m) {
            d.message_append_args(m, DBUS_TYPE_UINT32, &g_ssCookie, DBUS_TYPE_INVALID);
            DBusMessage* r = d.send_with_reply_and_block(c, m, 2000, &err);
            if (r) d.message_unref(r);
            d.message_unref(m);
        }
        d.connection_flush(c);
        d.connection_unref(c);
    }
    if (d.error_is_set(&err)) d.error_free(&err);
    g_ssKind = SsKind::None;
    g_ssCookie = 0;
}

bool acquireScreenSaver(const char* reason) {
    auto& d = dbus();
    if (!d.ok) return false;
    DBusError err{};
    d.error_init(&err);
    DBusConnection* c = d.bus_get(DBUS_BUS_SESSION, &err);
    if (!c) {
        if (d.error_is_set(&err)) d.error_free(&err);
        return false;
    }
    const char* app = "seerr";
    const char* why = reason && reason[0] ? reason : "Playing video";
    bool ok = false;

    // Prefer freedesktop.ScreenSaver (GNOME/KDE/Pop / most desktops)
    DBusMessage* m = d.message_new_method_call(
        "org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver",
        "org.freedesktop.ScreenSaver", "Inhibit");
    if (m) {
        d.message_append_args(m, DBUS_TYPE_STRING, &app, DBUS_TYPE_STRING, &why, DBUS_TYPE_INVALID);
        DBusMessage* r = d.send_with_reply_and_block(c, m, 2000, &err);
        if (r) {
            uint32_t cookie = 0;
            if (d.message_get_args(r, &err, DBUS_TYPE_UINT32, &cookie, DBUS_TYPE_INVALID)) {
                g_ssCookie = cookie;
                g_ssKind = SsKind::Freedesktop;
                ok = true;
            }
            d.message_unref(r);
        }
        d.message_unref(m);
    }

    // Fallback: GNOME SessionManager (idle inhibit flag = 8)
    if (!ok) {
        if (d.error_is_set(&err)) { d.error_free(&err); d.error_init(&err); }
        DBusMessage* m2 = d.message_new_method_call(
            "org.gnome.SessionManager", "/org/gnome/SessionManager",
            "org.gnome.SessionManager", "Inhibit");
        if (m2) {
            uint32_t xid = 0;
            uint32_t flags = 8; // InhibitIdle
            d.message_append_args(m2,
                DBUS_TYPE_STRING, &app,
                DBUS_TYPE_UINT32, &xid,
                DBUS_TYPE_STRING, &why,
                DBUS_TYPE_UINT32, &flags,
                DBUS_TYPE_INVALID);
            DBusMessage* r = d.send_with_reply_and_block(c, m2, 2000, &err);
            if (r) {
                uint32_t cookie = 0;
                if (d.message_get_args(r, &err, DBUS_TYPE_UINT32, &cookie, DBUS_TYPE_INVALID)) {
                    g_ssCookie = cookie;
                    g_ssKind = SsKind::Gnome;
                    ok = true;
                }
                d.message_unref(r);
            }
            d.message_unref(m2);
        }
    }

    d.connection_flush(c);
    d.connection_unref(c);
    if (d.error_is_set(&err)) d.error_free(&err);
    return ok;
}

void releaseLogin1() {
    if (g_loginFd >= 0) {
        ::close(g_loginFd);
        g_loginFd = -1;
    }
}

bool acquireLogin1(const char* reason) {
    auto& d = dbus();
    if (!d.ok) return false;
    DBusError err{};
    d.error_init(&err);
    DBusConnection* c = d.bus_get(DBUS_BUS_SYSTEM, &err);
    if (!c) {
        if (d.error_is_set(&err)) d.error_free(&err);
        return false;
    }
    const char* what = "idle:sleep";
    const char* who = "seerr";
    const char* why = reason && reason[0] ? reason : "Playing video";
    const char* mode = "block";
    bool ok = false;
    DBusMessage* m = d.message_new_method_call(
        "org.freedesktop.login1", "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager", "Inhibit");
    if (m) {
        d.message_append_args(m,
            DBUS_TYPE_STRING, &what,
            DBUS_TYPE_STRING, &who,
            DBUS_TYPE_STRING, &why,
            DBUS_TYPE_STRING, &mode,
            DBUS_TYPE_INVALID);
        DBusMessage* r = d.send_with_reply_and_block(c, m, 2000, &err);
        if (r) {
            int fd = -1;
            if (d.message_get_args(r, &err, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID) && fd >= 0) {
                g_loginFd = fd;
                ok = true;
            }
            d.message_unref(r);
        }
        d.message_unref(m);
    }
    d.connection_flush(c);
    d.connection_unref(c);
    if (d.error_is_set(&err)) d.error_free(&err);
    return ok;
}

void idleInhibitApply(bool active, const char* reason) {
    if (active) {
        if (g_ssKind == SsKind::None) (void)acquireScreenSaver(reason);
        if (g_loginFd < 0) (void)acquireLogin1(reason);
    } else {
        releaseScreenSaver();
        releaseLogin1();
    }
}
#endif

} // anon

void openUrl(const std::string& url) {
    if (url.empty()) return;
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    std::string cmd = "xdg-open \"" + url + "\" >/dev/null 2>&1 &";
    (void)std::system(cmd.c_str());
#endif
}

void openPath(const std::string& path) {
    if (path.empty()) return;
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    std::string cmd = "xdg-open \"" + path + "\" >/dev/null 2>&1 &";
    (void)std::system(cmd.c_str());
#endif
}

void applyDarkTitlebar(GLFWwindow* win) {
    if (!win) return;
#ifdef _WIN32
    HWND hwnd = glfwGetWin32Window(win);
    if (!hwnd) return;

    // Keep the default Windows caption/buttons — just switch to the modern dark theme.
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    // Win11 colored caption (falls back gracefully on older builds)
    COLORREF border = RGB(17, 24, 39);   // #111827
    COLORREF caption = RGB(31, 41, 55);  // #1f2937
    COLORREF text = RGB(229, 231, 235);  // #e5e7eb
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &text, sizeof(text));
#else
    (void)win;
#endif
}

void setIdleInhibit(bool active, const char* reason) {
    if (active) {
#ifndef _WIN32
        // Retry if D-Bus wasn't ready the first time (session just starting, etc.).
        const bool incomplete = (g_ssKind == SsKind::None && g_loginFd < 0);
        if (g_idleActive && !incomplete) return;
#else
        if (g_idleActive) return;
#endif
        g_idleActive = true;
        idleInhibitApply(true, reason);
        return;
    }
    if (!g_idleActive) return;
    g_idleActive = false;
    idleInhibitApply(false, reason);
}

} // namespace platform
