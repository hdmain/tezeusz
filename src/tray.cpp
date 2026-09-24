#include "tray.hpp"
#include "util.hpp"
#include "i18n.hpp"
#include <GLFW/glfw3.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include <stb_image.h>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace tray {
namespace {

GLFWwindow* g_win = nullptr;
std::atomic<bool> g_hidden{false};
std::atomic<bool> g_wantShow{false};
std::atomic<bool> g_wantQuit{false};
std::vector<uint8_t> g_rgba;
int g_iconW = 0, g_iconH = 0;
bool g_ok = false;

bool loadIconRgba(const std::string& path) {
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!px || w <= 0 || h <= 0) {
        if (px) stbi_image_free(px);
        return false;
    }
    g_iconW = w;
    g_iconH = h;
    g_rgba.assign(px, px + (size_t)w * h * 4);
    stbi_image_free(px);
    return true;
}

#ifdef _WIN32
constexpr UINT WM_TRAY = WM_APP + 42;
constexpr UINT ID_TRAY = 1;
constexpr UINT ID_SHOW = 1001;
constexpr UINT ID_QUIT = 1002;

HWND g_hwnd = nullptr;
HICON g_hicon = nullptr;
NOTIFYICONDATAW g_nid{};
WNDPROC g_prevProc = nullptr;
std::mutex g_mu;

HICON iconFromRgba(const uint8_t* rgba, int w, int h) {
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = w;
    bi.bV5Height = -h; // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!color || !bits) {
        if (color) DeleteObject(color);
        return nullptr;
    }
    auto* dst = static_cast<uint8_t*>(bits);
    for (int i = 0; i < w * h; i++) {
        dst[i * 4 + 0] = rgba[i * 4 + 2]; // B
        dst[i * 4 + 1] = rgba[i * 4 + 1]; // G
        dst[i * 4 + 2] = rgba[i * 4 + 0]; // R
        dst[i * 4 + 3] = rgba[i * 4 + 3]; // A
    }
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = CreateBitmap(w, h, 1, 1, nullptr);
    HICON icon = CreateIconIndirect(&ii);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    DeleteObject(color);
    return icon;
}

void showMenu() {
    if (!g_hwnd) return;
    POINT pt;
    GetCursorPos(&pt);
    auto toWide = [](const char* utf8) -> std::wstring {
        if (!utf8 || !utf8[0]) return L"";
        int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
        std::wstring out(n > 0 ? (size_t)n - 1 : 0, L'\0');
        if (n > 1) MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), n);
        return out;
    };
    std::wstring show = toWide(i18n::tr("tray.show"));
    std::wstring quit = toWide(i18n::tr("tray.quit"));
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_SHOW, show.c_str());
    AppendMenuW(menu, MF_STRING, ID_QUIT, quit.c_str());
    SetForegroundWindow(g_hwnd);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                              pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(menu);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    if (cmd == ID_SHOW) g_wantShow.store(true);
    if (cmd == ID_QUIT) g_wantQuit.store(true);
}

LRESULT CALLBACK trayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TRAY) {
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            g_wantShow.store(true);
            return 0;
        case WM_RBUTTONUP:
            showMenu();
            return 0;
        }
    }
    if (msg == WM_COMMAND) {
        if (LOWORD(wp) == ID_SHOW) g_wantShow.store(true);
        if (LOWORD(wp) == ID_QUIT) g_wantQuit.store(true);
        return 0;
    }
    if (g_prevProc) return CallWindowProcW(g_prevProc, hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool platformInit() {
    if (!g_win) return false;
    g_hwnd = glfwGetWin32Window(g_win);
    if (!g_hwnd) return false;

    if (!g_rgba.empty())
        g_hicon = iconFromRgba(g_rgba.data(), g_iconW, g_iconH);
    if (!g_hicon)
        g_hicon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512)); // IDI_APPLICATION

    g_prevProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(trayWndProc)));

    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = ID_TRAY;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_hicon;
    lstrcpynW(g_nid.szTip, L"Seerr", 128);
    if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) return false;
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    return true;
}

void platformShutdown() {
    if (g_hwnd) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_prevProc) {
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_prevProc));
            g_prevProc = nullptr;
        }
    }
    if (g_hicon) {
        DestroyIcon(g_hicon);
        g_hicon = nullptr;
    }
    g_hwnd = nullptr;
}

void platformTick() {}

#else
// ---- Linux: StatusNotifierItem + minimal DBusMenu via libdbus-1 (dlopen) ----

enum { DBUS_BUS_SESSION = 0 };
enum {
    DBUS_TYPE_INVALID = 0, DBUS_TYPE_BYTE = 'y', DBUS_TYPE_BOOLEAN = 'b',
    DBUS_TYPE_INT16 = 'n', DBUS_TYPE_UINT16 = 'q', DBUS_TYPE_INT32 = 'i',
    DBUS_TYPE_UINT32 = 'u', DBUS_TYPE_STRING = 's', DBUS_TYPE_OBJECT_PATH = 'o',
    DBUS_TYPE_ARRAY = 'a', DBUS_TYPE_VARIANT = 'v', DBUS_TYPE_STRUCT = 'r',
    DBUS_TYPE_DICT_ENTRY = 'e'
};
enum { DBUS_HANDLER_RESULT_HANDLED = 0, DBUS_HANDLER_RESULT_NOT_YET_HANDLED = 1 };

struct DBusError {
    char* name; char* message;
    unsigned dummy1 : 1; unsigned dummy2 : 1; unsigned dummy3 : 1;
    unsigned dummy4 : 1; unsigned dummy5 : 1;
    void* padding1;
};
struct DBusConnection;
struct DBusMessage;
// Real DBusMessageIter is a fixed-size struct (not opaque) — must match ABI.
struct DBusMessageIter {
    void* dummy1;
    void* dummy2;
    uint32_t dummy3;
    int dummy4;
    int dummy5;
    int dummy6;
    int dummy7;
    int dummy8;
    int dummy9;
    int dummy10;
    int dummy11;
    int pad1;
    int pad2;
    void* pad3;
};
using DBusHandleMessageFunction = int (*)(DBusConnection*, DBusMessage*, void*);

using Fn_error_init = void (*)(DBusError*);
using Fn_error_free = void (*)(DBusError*);
using Fn_error_is_set = int (*)(const DBusError*);
using Fn_bus_get = DBusConnection* (*)(int, DBusError*);
using Fn_connection_unref = void (*)(DBusConnection*);
using Fn_connection_flush = void (*)(DBusConnection*);
using Fn_connection_read_write = int (*)(DBusConnection*, int);
using Fn_connection_dispatch = int (*)(DBusConnection*);
using Fn_connection_get_is_connected = int (*)(DBusConnection*);
using Fn_connection_register_object_path = int (*)(DBusConnection*, const char*, const void*, void*);
using Fn_connection_unregister_object_path = int (*)(DBusConnection*, const char*);
using Fn_bus_request_name = int (*)(DBusConnection*, const char*, unsigned, DBusError*);
using Fn_bus_release_name = int (*)(DBusConnection*, const char*, DBusError*);
using Fn_message_new_method_call = DBusMessage* (*)(const char*, const char*, const char*, const char*);
using Fn_message_new_method_return = DBusMessage* (*)(DBusMessage*);
using Fn_message_unref = void (*)(DBusMessage*);
using Fn_message_append_args = int (*)(DBusMessage*, int, ...);
using Fn_message_get_member = const char* (*)(DBusMessage*);
using Fn_message_get_interface = const char* (*)(DBusMessage*);
using Fn_message_get_path = const char* (*)(DBusMessage*);
using Fn_message_is_method_call = int (*)(DBusMessage*, const char*, const char*);
using Fn_message_iter_init = int (*)(DBusMessage*, DBusMessageIter*);
using Fn_message_iter_init_append = void (*)(DBusMessage*, DBusMessageIter*);
using Fn_message_iter_append_basic = int (*)(DBusMessageIter*, int, const void*);
using Fn_message_iter_open_container = int (*)(DBusMessageIter*, int, const char*, DBusMessageIter*);
using Fn_message_iter_close_container = int (*)(DBusMessageIter*, DBusMessageIter*);
using Fn_message_iter_get_arg_type = int (*)(DBusMessageIter*);
using Fn_message_iter_get_basic = void (*)(DBusMessageIter*, void*);
using Fn_message_iter_next = int (*)(DBusMessageIter*);
using Fn_message_iter_recurse = void (*)(DBusMessageIter*, DBusMessageIter*);
using Fn_connection_send = int (*)(DBusConnection*, DBusMessage*, unsigned*);
using Fn_connection_send_with_reply_and_block = DBusMessage* (*)(DBusConnection*, DBusMessage*, int, DBusError*);
using Fn_message_get_args = int (*)(DBusMessage*, DBusError*, int, ...);

struct DBusObjectPathVTable {
    void (*unregister)(DBusConnection*, void*);
    DBusHandleMessageFunction message_function;
    void* pad1; void* pad2; void* pad3; void* pad4;
};

struct DBusApi {
    void* mod = nullptr;
    Fn_error_init error_init = nullptr;
    Fn_error_free error_free = nullptr;
    Fn_error_is_set error_is_set = nullptr;
    Fn_bus_get bus_get = nullptr;
    Fn_connection_unref connection_unref = nullptr;
    Fn_connection_flush connection_flush = nullptr;
    Fn_connection_read_write connection_read_write = nullptr;
    Fn_connection_dispatch connection_dispatch = nullptr;
    Fn_connection_get_is_connected connection_get_is_connected = nullptr;
    Fn_connection_register_object_path connection_register_object_path = nullptr;
    Fn_connection_unregister_object_path connection_unregister_object_path = nullptr;
    Fn_bus_request_name bus_request_name = nullptr;
    Fn_bus_release_name bus_release_name = nullptr;
    Fn_message_new_method_call message_new_method_call = nullptr;
    Fn_message_new_method_return message_new_method_return = nullptr;
    Fn_message_unref message_unref = nullptr;
    Fn_message_append_args message_append_args = nullptr;
    Fn_message_get_member message_get_member = nullptr;
    Fn_message_get_interface message_get_interface = nullptr;
    Fn_message_get_path message_get_path = nullptr;
    Fn_message_is_method_call message_is_method_call = nullptr;
    Fn_message_iter_init message_iter_init = nullptr;
    Fn_message_iter_init_append message_iter_init_append = nullptr;
    Fn_message_iter_append_basic message_iter_append_basic = nullptr;
    Fn_message_iter_open_container message_iter_open_container = nullptr;
    Fn_message_iter_close_container message_iter_close_container = nullptr;
    Fn_message_iter_get_arg_type message_iter_get_arg_type = nullptr;
    Fn_message_iter_get_basic message_iter_get_basic = nullptr;
    Fn_message_iter_next message_iter_next = nullptr;
    Fn_message_iter_recurse message_iter_recurse = nullptr;
    Fn_connection_send connection_send = nullptr;
    Fn_connection_send_with_reply_and_block send_with_reply_and_block = nullptr;
    Fn_message_get_args message_get_args = nullptr;
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
        L(a.connection_read_write, "dbus_connection_read_write") &&
        L(a.connection_dispatch, "dbus_connection_dispatch") &&
        L(a.connection_get_is_connected, "dbus_connection_get_is_connected") &&
        L(a.connection_register_object_path, "dbus_connection_register_object_path") &&
        L(a.connection_unregister_object_path, "dbus_connection_unregister_object_path") &&
        L(a.bus_request_name, "dbus_bus_request_name") &&
        L(a.bus_release_name, "dbus_bus_release_name") &&
        L(a.message_new_method_call, "dbus_message_new_method_call") &&
        L(a.message_new_method_return, "dbus_message_new_method_return") &&
        L(a.message_unref, "dbus_message_unref") &&
        L(a.message_append_args, "dbus_message_append_args") &&
        L(a.message_get_member, "dbus_message_get_member") &&
        L(a.message_get_interface, "dbus_message_get_interface") &&
        L(a.message_get_path, "dbus_message_get_path") &&
        L(a.message_is_method_call, "dbus_message_is_method_call") &&
        L(a.message_iter_init, "dbus_message_iter_init") &&
        L(a.message_iter_init_append, "dbus_message_iter_init_append") &&
        L(a.message_iter_append_basic, "dbus_message_iter_append_basic") &&
        L(a.message_iter_open_container, "dbus_message_iter_open_container") &&
        L(a.message_iter_close_container, "dbus_message_iter_close_container") &&
        L(a.message_iter_get_arg_type, "dbus_message_iter_get_arg_type") &&
        L(a.message_iter_get_basic, "dbus_message_iter_get_basic") &&
        L(a.message_iter_next, "dbus_message_iter_next") &&
        L(a.message_iter_recurse, "dbus_message_iter_recurse") &&
        L(a.connection_send, "dbus_connection_send") &&
        L(a.send_with_reply_and_block, "dbus_connection_send_with_reply_and_block") &&
        L(a.message_get_args, "dbus_message_get_args");
    return a;
}

DBusConnection* g_conn = nullptr;
std::string g_busName;
std::string g_itemPath = "/StatusNotifierItem";
std::string g_menuPath = "/Menu";
std::vector<uint8_t> g_argbNet; // network-order ARGB bytes for IconPixmap
int g_menuRev = 1;

void appendVariantString(DBusMessageIter* parent, const char* key, const char* val) {
    auto& d = dbus();
    DBusMessageIter entry, var;
    d.message_iter_open_container(parent, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    d.message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    d.message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &var);
    d.message_iter_append_basic(&var, DBUS_TYPE_STRING, &val);
    d.message_iter_close_container(&entry, &var);
    d.message_iter_close_container(parent, &entry);
}

void appendVariantBool(DBusMessageIter* parent, const char* key, int val) {
    auto& d = dbus();
    DBusMessageIter entry, var;
    d.message_iter_open_container(parent, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    d.message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    d.message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &var);
    d.message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &val);
    d.message_iter_close_container(&entry, &var);
    d.message_iter_close_container(parent, &entry);
}

void appendVariantInt(DBusMessageIter* parent, const char* key, int32_t val) {
    auto& d = dbus();
    DBusMessageIter entry, var;
    d.message_iter_open_container(parent, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    d.message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    d.message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "i", &var);
    d.message_iter_append_basic(&var, DBUS_TYPE_INT32, &val);
    d.message_iter_close_container(&entry, &var);
    d.message_iter_close_container(parent, &entry);
}

void appendIconPixmap(DBusMessageIter* parent) {
    auto& d = dbus();
    DBusMessageIter entry, var, arr, st, bytes;
    const char* key = "IconPixmap";
    d.message_iter_open_container(parent, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    d.message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    d.message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "a(iiay)", &var);
    d.message_iter_open_container(&var, DBUS_TYPE_ARRAY, "(iiay)", &arr);
    if (!g_argbNet.empty()) {
        d.message_iter_open_container(&arr, DBUS_TYPE_STRUCT, nullptr, &st);
        int32_t w = g_iconW, h = g_iconH;
        d.message_iter_append_basic(&st, DBUS_TYPE_INT32, &w);
        d.message_iter_append_basic(&st, DBUS_TYPE_INT32, &h);
        d.message_iter_open_container(&st, DBUS_TYPE_ARRAY, "y", &bytes);
        for (uint8_t b : g_argbNet)
            d.message_iter_append_basic(&bytes, DBUS_TYPE_BYTE, &b);
        d.message_iter_close_container(&st, &bytes);
        d.message_iter_close_container(&arr, &st);
    }
    d.message_iter_close_container(&var, &arr);
    d.message_iter_close_container(&entry, &var);
    d.message_iter_close_container(parent, &entry);
}

void appendMenuItem(DBusMessageIter* parent, int32_t id, const char* label, bool enabled) {
    auto& d = dbus();
    DBusMessageIter item, props, children;
    d.message_iter_open_container(parent, DBUS_TYPE_STRUCT, nullptr, &item);
    d.message_iter_append_basic(&item, DBUS_TYPE_INT32, &id);
    d.message_iter_open_container(&item, DBUS_TYPE_ARRAY, "{sv}", &props);
    appendVariantString(&props, "label", label);
    appendVariantBool(&props, "enabled", enabled ? 1 : 0);
    d.message_iter_close_container(&item, &props);
    d.message_iter_open_container(&item, DBUS_TYPE_ARRAY, "(ia{sv}av)", &children);
    d.message_iter_close_container(&item, &children);
    d.message_iter_close_container(parent, &item);
}

void replyGetAllSni(DBusMessage* msg) {
    auto& d = dbus();
    DBusMessage* r = d.message_new_method_return(msg);
    if (!r) return;
    DBusMessageIter root, arr;
    d.message_iter_init_append(r, &root);
    d.message_iter_open_container(&root, DBUS_TYPE_ARRAY, "{sv}", &arr);
    appendVariantString(&arr, "Category", "ApplicationStatus");
    appendVariantString(&arr, "Id", "seerr");
    appendVariantString(&arr, "Title", "Seerr");
    appendVariantString(&arr, "Status", "Active");
    appendVariantString(&arr, "IconName", "seerr");
    const char* menuPath = g_menuPath.c_str();
    {
        DBusMessageIter entry, var;
        const char* key = "Menu";
        d.message_iter_open_container(&arr, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        d.message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        d.message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "o", &var);
        d.message_iter_append_basic(&var, DBUS_TYPE_OBJECT_PATH, &menuPath);
        d.message_iter_close_container(&entry, &var);
        d.message_iter_close_container(&arr, &entry);
    }
    appendIconPixmap(&arr);
    d.message_iter_close_container(&root, &arr);
    d.connection_send(g_conn, r, nullptr);
    d.message_unref(r);
}

void replyGetLayout(DBusMessage* msg) {
    auto& d = dbus();
    DBusMessage* r = d.message_new_method_return(msg);
    if (!r) return;
    DBusMessageIter root, layout, props, children;
    d.message_iter_init_append(r, &root);
    d.message_iter_append_basic(&root, DBUS_TYPE_UINT32, &g_menuRev);
    d.message_iter_open_container(&root, DBUS_TYPE_STRUCT, nullptr, &layout);
    int32_t rootId = 0;
    d.message_iter_append_basic(&layout, DBUS_TYPE_INT32, &rootId);
    d.message_iter_open_container(&layout, DBUS_TYPE_ARRAY, "{sv}", &props);
    appendVariantString(&props, "children-display", "submenu");
    d.message_iter_close_container(&layout, &props);
    d.message_iter_open_container(&layout, DBUS_TYPE_ARRAY, "(ia{sv}av)", &children);
    // children must be variants of (ia{sv}av) per dbusmenu — use av of variants
    // Spec: children is av where each is variant of (ia{sv}av). Simpler hosts accept direct structs
    // in the array type "(ia{sv}av)". We'll emit struct children directly.
    appendMenuItem(&children, 1, i18n::tr("tray.show"), true);
    appendMenuItem(&children, 2, i18n::tr("tray.quit"), true);
    d.message_iter_close_container(&layout, &children);
    d.message_iter_close_container(&root, &layout);
    d.connection_send(g_conn, r, nullptr);
    d.message_unref(r);
}

int onMessage(DBusConnection*, DBusMessage* msg, void*) {
    auto& d = dbus();
    const char* iface = d.message_get_interface(msg);
    const char* member = d.message_get_member(msg);
    const char* path = d.message_get_path(msg);
    if (!iface || !member || !path) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (d.message_is_method_call(msg, "org.freedesktop.DBus.Properties", "GetAll")) {
        if (g_itemPath == path) { replyGetAllSni(msg); return DBUS_HANDLER_RESULT_HANDLED; }
        if (g_menuPath == path) {
            DBusMessage* r = d.message_new_method_return(msg);
            if (r) {
                DBusMessageIter root, arr;
                d.message_iter_init_append(r, &root);
                d.message_iter_open_container(&root, DBUS_TYPE_ARRAY, "{sv}", &arr);
                appendVariantString(&arr, "Version", "4");
                appendVariantString(&arr, "Status", "normal");
                appendVariantString(&arr, "TextDirection", "ltr");
                d.message_iter_close_container(&root, &arr);
                d.connection_send(g_conn, r, nullptr);
                d.message_unref(r);
            }
            return DBUS_HANDLER_RESULT_HANDLED;
        }
    }

    if (d.message_is_method_call(msg, "org.kde.StatusNotifierItem", "Activate") ||
        d.message_is_method_call(msg, "org.kde.StatusNotifierItem", "SecondaryActivate")) {
        g_wantShow.store(true);
        DBusMessage* r = d.message_new_method_return(msg);
        if (r) { d.connection_send(g_conn, r, nullptr); d.message_unref(r); }
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (d.message_is_method_call(msg, "org.kde.StatusNotifierItem", "ContextMenu")) {
        // Hosts with DBusMenu use Menu; still acknowledge.
        DBusMessage* r = d.message_new_method_return(msg);
        if (r) { d.connection_send(g_conn, r, nullptr); d.message_unref(r); }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (d.message_is_method_call(msg, "com.canonical.dbusmenu", "GetLayout")) {
        replyGetLayout(msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (d.message_is_method_call(msg, "com.canonical.dbusmenu", "AboutToShow")) {
        DBusMessage* r = d.message_new_method_return(msg);
        if (r) {
            int need = 0;
            d.message_append_args(r, DBUS_TYPE_BOOLEAN, &need, DBUS_TYPE_INVALID);
            d.connection_send(g_conn, r, nullptr);
            d.message_unref(r);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (d.message_is_method_call(msg, "com.canonical.dbusmenu", "Event")) {
        // Event(id, eventId, data, timestamp) — clicked
        DBusMessageIter it;
        if (d.message_iter_init(msg, &it) && d.message_iter_get_arg_type(&it) == DBUS_TYPE_INT32) {
            int32_t id = 0;
            d.message_iter_get_basic(&it, &id);
            d.message_iter_next(&it);
            if (d.message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) {
                const char* ev = nullptr;
                d.message_iter_get_basic(&it, &ev);
                if (ev && std::strcmp(ev, "clicked") == 0) {
                    if (id == 1) g_wantShow.store(true);
                    if (id == 2) g_wantQuit.store(true);
                }
            }
        }
        DBusMessage* r = d.message_new_method_return(msg);
        if (r) { d.connection_send(g_conn, r, nullptr); d.message_unref(r); }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (d.message_is_method_call(msg, "org.freedesktop.DBus.Introspectable", "Introspect")) {
        const char* xml =
            "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
            "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">"
            "<node></node>";
        DBusMessage* r = d.message_new_method_return(msg);
        if (r) {
            d.message_append_args(r, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID);
            d.connection_send(g_conn, r, nullptr);
            d.message_unref(r);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    (void)iface; (void)member;
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

bool registerWithWatcher() {
    auto& d = dbus();
    DBusError err{};
    d.error_init(&err);
    DBusMessage* m = d.message_new_method_call(
        "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
        "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem");
    if (!m) return false;
    const char* name = g_busName.c_str();
    d.message_append_args(m, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
    DBusMessage* r = d.send_with_reply_and_block(g_conn, m, 3000, &err);
    d.message_unref(m);
    if (r) d.message_unref(r);
    bool ok = !d.error_is_set(&err);
    if (d.error_is_set(&err)) d.error_free(&err);
    return ok;
}

bool platformInit() {
    auto& d = dbus();
    if (!d.ok) return false;

    // Build network-order ARGB pixmap
    if (!g_rgba.empty()) {
        g_argbNet.resize((size_t)g_iconW * g_iconH * 4);
        for (int i = 0; i < g_iconW * g_iconH; i++) {
            g_argbNet[i * 4 + 0] = g_rgba[i * 4 + 3]; // A
            g_argbNet[i * 4 + 1] = g_rgba[i * 4 + 0]; // R
            g_argbNet[i * 4 + 2] = g_rgba[i * 4 + 1]; // G
            g_argbNet[i * 4 + 3] = g_rgba[i * 4 + 2]; // B
        }
    }

    DBusError err{};
    d.error_init(&err);
    g_conn = d.bus_get(DBUS_BUS_SESSION, &err);
    if (!g_conn) {
        if (d.error_is_set(&err)) d.error_free(&err);
        return false;
    }

    g_busName = "org.kde.StatusNotifierItem-" + std::to_string((long)getpid()) + "-1";
    if (d.bus_request_name(g_conn, g_busName.c_str(), 0, &err) < 0) {
        if (d.error_is_set(&err)) d.error_free(&err);
        d.connection_unref(g_conn);
        g_conn = nullptr;
        return false;
    }

    DBusObjectPathVTable vt{};
    vt.message_function = onMessage;
    if (!d.connection_register_object_path(g_conn, g_itemPath.c_str(), &vt, nullptr) ||
        !d.connection_register_object_path(g_conn, g_menuPath.c_str(), &vt, nullptr)) {
        d.bus_release_name(g_conn, g_busName.c_str(), &err);
        d.connection_unref(g_conn);
        g_conn = nullptr;
        return false;
    }

    if (!registerWithWatcher()) {
        // Still keep the item exported — some hosts discover by name.
        std::fprintf(stderr, "seerr: StatusNotifierWatcher register failed (tray may be hidden)\n");
    }
    d.connection_flush(g_conn);
    return true;
}

void platformShutdown() {
    auto& d = dbus();
    if (!g_conn || !d.ok) return;
    DBusError err{};
    d.error_init(&err);
    d.connection_unregister_object_path(g_conn, g_itemPath.c_str());
    d.connection_unregister_object_path(g_conn, g_menuPath.c_str());
    d.bus_release_name(g_conn, g_busName.c_str(), &err);
    if (d.error_is_set(&err)) d.error_free(&err);
    d.connection_unref(g_conn);
    g_conn = nullptr;
}

void platformTick() {
    auto& d = dbus();
    if (!g_conn || !d.ok) return;
    d.connection_read_write(g_conn, 0);
    // DBUS_DISPATCH_DATA_REMAINS == 1
    while (d.connection_dispatch(g_conn) == 1) {}
}
#endif

} // anon

bool init(GLFWwindow* win, const std::string& iconPngPath) {
    g_win = win;
    g_hidden = false;
    g_wantShow = false;
    g_wantQuit = false;
    if (!iconPngPath.empty())
        loadIconRgba(iconPngPath);
    g_ok = platformInit();
    if (!g_ok)
        std::fprintf(stderr, "seerr: system tray unavailable\n");
    return g_ok;
}

void shutdown() {
    if (!g_ok) return;
    platformShutdown();
    g_ok = false;
    g_win = nullptr;
    g_hidden = false;
}

void tick() {
    if (!g_ok) return;
    platformTick();
}

void hideToTray() {
    if (!g_win) return;
    glfwHideWindow(g_win);
    g_hidden.store(true);
}

void showFromTray() {
    if (!g_win) return;
    glfwShowWindow(g_win);
    glfwFocusWindow(g_win);
    g_hidden.store(false);
}

bool isHidden() { return g_hidden.load(); }

bool consumeShowRequest() {
    return g_wantShow.exchange(false);
}

bool consumeQuitRequest() {
    return g_wantQuit.exchange(false);
}

} // namespace tray
