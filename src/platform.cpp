#include "platform.hpp"
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
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
#include <commdlg.h>
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
#include <cstdio>
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
// ---- D-Bus (dlopen libdbus-1) - no hard link dependency ----
enum { DBUS_BUS_SESSION = 0, DBUS_BUS_SYSTEM = 1 };
enum {
    DBUS_TYPE_INVALID = 0,
    DBUS_TYPE_BYTE = 'y',
    DBUS_TYPE_BOOLEAN = 'b',
    DBUS_TYPE_INT16 = 'n',
    DBUS_TYPE_UINT16 = 'q',
    DBUS_TYPE_INT32 = 'i',
    DBUS_TYPE_UINT32 = 'u',
    DBUS_TYPE_INT64 = 'x',
    DBUS_TYPE_UINT64 = 't',
    DBUS_TYPE_DOUBLE = 'd',
    DBUS_TYPE_STRING = 's',
    DBUS_TYPE_OBJECT_PATH = 'o',
    DBUS_TYPE_SIGNATURE = 'g',
    DBUS_TYPE_UNIX_FD = 'h',
    DBUS_TYPE_ARRAY = 'a',
    DBUS_TYPE_VARIANT = 'v',
    DBUS_TYPE_STRUCT = 'r',
    DBUS_TYPE_DICT_ENTRY = 'e'
};
enum { DBUS_HANDLER_RESULT_HANDLED = 0, DBUS_DISPATCH_DATA_REMAINS = 1 };

struct DBusError {
    char* name; char* message;
    unsigned int dummy1 : 1; unsigned int dummy2 : 1;
    unsigned int dummy3 : 1; unsigned int dummy4 : 1; unsigned int dummy5 : 1;
    void* padding1;
};
struct DBusConnection;
struct DBusMessage;
// Real DBusMessageIter is a fixed-size struct (not opaque) - must match ABI.
struct DBusMessageIter {
    void* dummy1; void* dummy2; uint32_t dummy3; int dummy4; int dummy5;
    int dummy6; int dummy7; int dummy8; int dummy9; int dummy10; int dummy11; int pad;
};

using Fn_dbus_error_init = void (*)(DBusError*);
using Fn_dbus_error_free = void (*)(DBusError*);
using Fn_dbus_error_is_set = int (*)(const DBusError*);
using Fn_dbus_bus_get = DBusConnection* (*)(int, DBusError*);
using Fn_dbus_connection_unref = void (*)(DBusConnection*);
using Fn_dbus_connection_flush = void (*)(DBusConnection*);
using Fn_dbus_connection_read_write = int (*)(DBusConnection*, int);
using Fn_dbus_connection_dispatch = int (*)(DBusConnection*);
using Fn_dbus_connection_pop_message = DBusMessage* (*)(DBusConnection*);
using Fn_dbus_bus_add_match = void (*)(DBusConnection*, const char*, DBusError*);
using Fn_dbus_bus_remove_match = void (*)(DBusConnection*, const char*, DBusError*);
using Fn_dbus_message_new_method_call = DBusMessage* (*)(const char*, const char*, const char*, const char*);
using Fn_dbus_message_unref = void (*)(DBusMessage*);
using Fn_dbus_message_append_args = int (*)(DBusMessage*, int, ...);
using Fn_dbus_connection_send_with_reply_and_block =
    DBusMessage* (*)(DBusConnection*, DBusMessage*, int, DBusError*);
using Fn_dbus_message_get_args = int (*)(DBusMessage*, DBusError*, int, ...);
using Fn_dbus_message_is_signal = int (*)(DBusMessage*, const char*, const char*);
using Fn_dbus_message_get_path = const char* (*)(DBusMessage*);
using Fn_dbus_message_iter_init = int (*)(DBusMessage*, DBusMessageIter*);
using Fn_dbus_message_iter_init_append = void (*)(DBusMessage*, DBusMessageIter*);
using Fn_dbus_message_iter_append_basic = int (*)(DBusMessageIter*, int, const void*);
using Fn_dbus_message_iter_open_container = int (*)(DBusMessageIter*, int, const char*, DBusMessageIter*);
using Fn_dbus_message_iter_close_container = int (*)(DBusMessageIter*, DBusMessageIter*);
using Fn_dbus_message_iter_get_arg_type = int (*)(DBusMessageIter*);
using Fn_dbus_message_iter_get_basic = void (*)(DBusMessageIter*, void*);
using Fn_dbus_message_iter_next = int (*)(DBusMessageIter*);
using Fn_dbus_message_iter_recurse = void (*)(DBusMessageIter*, DBusMessageIter*);

struct DBusApi {
    void* mod = nullptr;
    Fn_dbus_error_init error_init = nullptr;
    Fn_dbus_error_free error_free = nullptr;
    Fn_dbus_error_is_set error_is_set = nullptr;
    Fn_dbus_bus_get bus_get = nullptr;
    Fn_dbus_connection_unref connection_unref = nullptr;
    Fn_dbus_connection_flush connection_flush = nullptr;
    Fn_dbus_connection_read_write connection_read_write = nullptr;
    Fn_dbus_connection_dispatch connection_dispatch = nullptr;
    Fn_dbus_connection_pop_message connection_pop_message = nullptr;
    Fn_dbus_bus_add_match bus_add_match = nullptr;
    Fn_dbus_bus_remove_match bus_remove_match = nullptr;
    Fn_dbus_message_new_method_call message_new_method_call = nullptr;
    Fn_dbus_message_unref message_unref = nullptr;
    Fn_dbus_message_append_args message_append_args = nullptr;
    Fn_dbus_connection_send_with_reply_and_block send_with_reply_and_block = nullptr;
    Fn_dbus_message_get_args message_get_args = nullptr;
    Fn_dbus_message_is_signal message_is_signal = nullptr;
    Fn_dbus_message_get_path message_get_path = nullptr;
    Fn_dbus_message_iter_init message_iter_init = nullptr;
    Fn_dbus_message_iter_init_append message_iter_init_append = nullptr;
    Fn_dbus_message_iter_append_basic message_iter_append_basic = nullptr;
    Fn_dbus_message_iter_open_container message_iter_open_container = nullptr;
    Fn_dbus_message_iter_close_container message_iter_close_container = nullptr;
    Fn_dbus_message_iter_get_arg_type message_iter_get_arg_type = nullptr;
    Fn_dbus_message_iter_get_basic message_iter_get_basic = nullptr;
    Fn_dbus_message_iter_next message_iter_next = nullptr;
    Fn_dbus_message_iter_recurse message_iter_recurse = nullptr;
    bool ok = false;
    bool portalOk = false;
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
    a.portalOk = a.ok &&
        L(a.connection_read_write, "dbus_connection_read_write") &&
        L(a.connection_dispatch, "dbus_connection_dispatch") &&
        L(a.connection_pop_message, "dbus_connection_pop_message") &&
        L(a.bus_add_match, "dbus_bus_add_match") &&
        L(a.bus_remove_match, "dbus_bus_remove_match") &&
        L(a.message_is_signal, "dbus_message_is_signal") &&
        L(a.message_get_path, "dbus_message_get_path") &&
        L(a.message_iter_init, "dbus_message_iter_init") &&
        L(a.message_iter_init_append, "dbus_message_iter_init_append") &&
        L(a.message_iter_append_basic, "dbus_message_iter_append_basic") &&
        L(a.message_iter_open_container, "dbus_message_iter_open_container") &&
        L(a.message_iter_close_container, "dbus_message_iter_close_container") &&
        L(a.message_iter_get_arg_type, "dbus_message_iter_get_arg_type") &&
        L(a.message_iter_get_basic, "dbus_message_iter_get_basic") &&
        L(a.message_iter_next, "dbus_message_iter_next") &&
        L(a.message_iter_recurse, "dbus_message_iter_recurse");
    return a;
}

static std::string fileUriToPath(std::string uri) {
    if (uri.size() < 7 || uri.compare(0, 7, "file://") != 0) return {};
    std::string p = uri.substr(7);
    if (p.size() >= 9 && p.compare(0, 9, "localhost") == 0)
        p = p.substr(9);
    std::string out;
    out.reserve(p.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < p.size(); ++i) {
        if (p[i] == '%' && i + 2 < p.size()) {
            int hi = hex(p[i + 1]), lo = hex(p[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back((char)((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(p[i]);
    }
    return out;
}

// xdg-desktop-portal FileChooser - works with GNOME/KDE/XFCE/etc. native dialogs.
static std::string pickOpenFilePortal(const char* title, const char* filterLabel,
                                      const char* filterPattern) {
    auto& d = dbus();
    if (!d.portalOk) return {};

    DBusError err{};
    d.error_init(&err);
    DBusConnection* c = d.bus_get(DBUS_BUS_SESSION, &err);
    if (!c) {
        if (d.error_is_set(&err)) d.error_free(&err);
        return {};
    }

    DBusMessage* m = d.message_new_method_call(
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.FileChooser",
        "OpenFile");
    if (!m) {
        d.connection_unref(c);
        if (d.error_is_set(&err)) d.error_free(&err);
        return {};
    }

    DBusMessageIter args;
    d.message_iter_init_append(m, &args);
    const char* parent = "";
    const char* titleStr = (title && title[0]) ? title : "Open file";
    d.message_iter_append_basic(&args, DBUS_TYPE_STRING, &parent);
    d.message_iter_append_basic(&args, DBUS_TYPE_STRING, &titleStr);

    DBusMessageIter opts;
    d.message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &opts);

    // filters: a(sa(us))
    {
        const char* key = "filters";
        const char* fname = (filterLabel && filterLabel[0]) ? filterLabel : "Files";
        const char* globPat = (filterPattern && filterPattern[0]) ? filterPattern : "*";
        uint32_t globType = 0; // 0 = glob

        DBusMessageIter entry, var, filters, filterStruct, patterns, pattern;
        d.message_iter_open_container(&opts, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        d.message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        d.message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "a(sa(us))", &var);
        d.message_iter_open_container(&var, DBUS_TYPE_ARRAY, "(sa(us))", &filters);
        d.message_iter_open_container(&filters, DBUS_TYPE_STRUCT, nullptr, &filterStruct);
        d.message_iter_append_basic(&filterStruct, DBUS_TYPE_STRING, &fname);
        d.message_iter_open_container(&filterStruct, DBUS_TYPE_ARRAY, "(us)", &patterns);
        d.message_iter_open_container(&patterns, DBUS_TYPE_STRUCT, nullptr, &pattern);
        d.message_iter_append_basic(&pattern, DBUS_TYPE_UINT32, &globType);
        d.message_iter_append_basic(&pattern, DBUS_TYPE_STRING, &globPat);
        d.message_iter_close_container(&patterns, &pattern);
        d.message_iter_close_container(&filterStruct, &patterns);
        d.message_iter_close_container(&filters, &filterStruct);
        d.message_iter_close_container(&var, &filters);
        d.message_iter_close_container(&entry, &var);
        d.message_iter_close_container(&opts, &entry);
    }
    // multiple = false
    {
        const char* key = "multiple";
        int multiple = 0;
        DBusMessageIter entry, var;
        d.message_iter_open_container(&opts, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        d.message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        d.message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &var);
        d.message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &multiple);
        d.message_iter_close_container(&entry, &var);
        d.message_iter_close_container(&opts, &entry);
    }
    d.message_iter_close_container(&args, &opts);

    DBusMessage* reply = d.send_with_reply_and_block(c, m, 5000, &err);
    d.message_unref(m);
    if (!reply) {
        d.connection_unref(c);
        if (d.error_is_set(&err)) d.error_free(&err);
        return {};
    }

    const char* requestPath = nullptr;
    if (!d.message_get_args(reply, &err, DBUS_TYPE_OBJECT_PATH, &requestPath, DBUS_TYPE_INVALID) ||
        !requestPath) {
        d.message_unref(reply);
        d.connection_unref(c);
        if (d.error_is_set(&err)) d.error_free(&err);
        return {};
    }
    std::string reqPath = requestPath;
    d.message_unref(reply);

    std::string match = "type='signal',interface='org.freedesktop.portal.Request',"
                        "member='Response',path='" + reqPath + "'";
    d.bus_add_match(c, match.c_str(), &err);
    d.connection_flush(c);

    std::string chosen;
    const double deadline = (double)::time(nullptr) + 600.0; // 10 min for the user
    bool done = false;
    while (!done && (double)::time(nullptr) < deadline) {
        d.connection_read_write(c, 200);
        while (d.connection_dispatch(c) == DBUS_DISPATCH_DATA_REMAINS) {}
        while (DBusMessage* sig = d.connection_pop_message(c)) {
            if (d.message_is_signal(sig, "org.freedesktop.portal.Request", "Response")) {
                const char* path = d.message_get_path(sig);
                if (path && reqPath == path) {
                    DBusMessageIter it;
                    if (d.message_iter_init(sig, &it) &&
                        d.message_iter_get_arg_type(&it) == DBUS_TYPE_UINT32) {
                        uint32_t response = 1;
                        d.message_iter_get_basic(&it, &response);
                        d.message_iter_next(&it); // results a{sv}
                        if (response == 0 &&
                            d.message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
                            DBusMessageIter dict;
                            d.message_iter_recurse(&it, &dict);
                            while (d.message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
                                DBusMessageIter entry;
                                d.message_iter_recurse(&dict, &entry);
                                const char* key = nullptr;
                                if (d.message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
                                    d.message_iter_get_basic(&entry, &key);
                                    d.message_iter_next(&entry);
                                }
                                if (key && std::strcmp(key, "uris") == 0 &&
                                    d.message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
                                    DBusMessageIter var, arr;
                                    d.message_iter_recurse(&entry, &var);
                                    if (d.message_iter_get_arg_type(&var) == DBUS_TYPE_ARRAY) {
                                        d.message_iter_recurse(&var, &arr);
                                        if (d.message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
                                            const char* uri = nullptr;
                                            d.message_iter_get_basic(&arr, &uri);
                                            if (uri) chosen = fileUriToPath(uri);
                                        }
                                    }
                                }
                                d.message_iter_next(&dict);
                            }
                        }
                        done = true;
                    }
                }
            }
            d.message_unref(sig);
            if (done) break;
        }
    }

    if (d.error_is_set(&err)) { d.error_free(&err); d.error_init(&err); }
    d.bus_remove_match(c, match.c_str(), &err);
    d.connection_flush(c);
    d.connection_unref(c);
    if (d.error_is_set(&err)) d.error_free(&err);
    return chosen;
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
    const char* app = "tezeusz";
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
    const char* who = "tezeusz";
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

std::string pickOpenFile(const char* title, const char* filterLabel, const char* filterPattern,
                         std::string* err) {
    if (err) err->clear();
#ifdef _WIN32
    (void)err;
    char file[MAX_PATH] = {};
    char filter[256] = {};
    // OPENFILENAME wants "Label\0pattern\0All\0*.*\0\0"
    std::string label = filterLabel && filterLabel[0] ? filterLabel : "Files";
    std::string pattern = filterPattern && filterPattern[0] ? filterPattern : "*.*";
    size_t n = 0;
    auto append = [&](const std::string& s) {
        for (char c : s) {
            if (n + 2 >= sizeof(filter)) return;
            filter[n++] = c;
        }
        if (n + 1 < sizeof(filter)) filter[n++] = '\0';
    };
    append(label);
    append(pattern);
    append("All files");
    append("*.*");
    if (n < sizeof(filter)) filter[n] = '\0';

    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title ? title : "Open";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&ofn)) return {};
    return std::string(file);
#else
    // 1) xdg-desktop-portal - native dialog for the current desktop (any file manager DE)
    std::string path = pickOpenFilePortal(title, filterLabel, filterPattern);
    if (!path.empty()) return path;

    (void)filterLabel;
    std::string pattern = filterPattern && filterPattern[0] ? filterPattern : "*";
    // zenity wants "Name | pat1 pat2"
    if (!pattern.empty() && pattern[0] == '*')
        pattern = std::string(filterLabel && filterLabel[0] ? filterLabel : "Files") + " | " + pattern;

    auto shQuote = [](const std::string& s) {
        std::string o = "'";
        for (char c : s) {
            if (c == '\'') o += "'\\''";
            else o += c;
        }
        o += "'";
        return o;
    };
    auto haveCmd = [](const char* bin) {
        std::string c = std::string("command -v ") + bin + " >/dev/null 2>&1";
        return std::system(c.c_str()) == 0;
    };
    auto run = [](const std::string& cmd) -> std::string {
        FILE* f = popen(cmd.c_str(), "r");
        if (!f) return {};
        char buf[2048] = {};
        std::string out;
        while (fgets(buf, sizeof(buf), f)) out += buf;
        const int st = pclose(f);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        if (st != 0 && out.empty()) return {};
        return out;
    };

    const std::string t = title ? title : "Open file";

    // 2) Fallback CLI dialogs (older / minimal desktops)
    if (haveCmd("zenity")) {
        path = run("zenity --file-selection --title=" + shQuote(t) +
                   " --file-filter=" + shQuote(pattern) +
                   " --file-filter=" + shQuote("All | *") +
                   " 2>/dev/null");
    }
    if (path.empty() && haveCmd("yad")) {
        path = run("yad --file --title=" + shQuote(t) +
                   " --file-filter=" + shQuote(pattern) +
                   " 2>/dev/null");
    }
    if (path.empty() && haveCmd("kdialog")) {
        std::string kdFilter = "*.zip|ZIP";
        if (filterPattern && filterPattern[0])
            kdFilter = std::string(filterPattern) + "|" +
                       (filterLabel && filterLabel[0] ? filterLabel : "Files");
        path = run("kdialog --getopenfilename . " + shQuote(kdFilter) + " 2>/dev/null");
    }
    if (path.empty() && haveCmd("qarma")) {
        path = run("qarma --file-selection --title=" + shQuote(t) + " 2>/dev/null");
    }

    if (path.empty() && err) {
        const bool anyCli = haveCmd("zenity") || haveCmd("yad") || haveCmd("kdialog") || haveCmd("qarma");
        if (!dbus().portalOk && !anyCli)
            *err = "no_file_dialog";
    }
    return path;
#endif
}

void applyDarkTitlebar(GLFWwindow* win) {
    if (!win) return;
#ifdef _WIN32
    HWND hwnd = glfwGetWin32Window(win);
    if (!hwnd) return;

    // Keep the default Windows caption/buttons - just switch to the modern dark theme.
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
