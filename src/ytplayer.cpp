#include "ytplayer.hpp"
#include "i18n.hpp"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <string>
#include <filesystem>

// libVLC loaded at runtime from vendor/libvlc (no PE import → exe starts without DLLs beside it).
struct libvlc_instance_t;
struct libvlc_media_t;
struct libvlc_media_player_t;
struct libvlc_event_manager_t;
struct libvlc_event_t;

enum libvlc_event_e {
    libvlc_MediaPlayerPlaying = 0x104,
    libvlc_MediaPlayerEndReached = 0x109,
    libvlc_MediaPlayerEncounteredError = 0x10a,
};

struct libvlc_event_t {
    int type;
    void* p_obj;
    union {
        char pad[64];
    } u;
};

using libvlc_callback_t = void (*)(const libvlc_event_t*, void*);

namespace ytplayer {
namespace {

namespace fs = std::filesystem;

using Fn_libvlc_new = libvlc_instance_t* (*)(int, const char* const*);
using Fn_libvlc_release = void (*)(libvlc_instance_t*);
using Fn_libvlc_media_new_location = libvlc_media_t* (*)(libvlc_instance_t*, const char*);
using Fn_libvlc_media_add_option = void (*)(libvlc_media_t*, const char*);
using Fn_libvlc_media_release = void (*)(libvlc_media_t*);
using Fn_libvlc_media_player_new_from_media = libvlc_media_player_t* (*)(libvlc_media_t*);
using Fn_libvlc_media_player_release = void (*)(libvlc_media_player_t*);
using Fn_libvlc_media_player_set_hwnd = void (*)(libvlc_media_player_t*, void*);
using Fn_libvlc_media_player_play = int (*)(libvlc_media_player_t*);
using Fn_libvlc_media_player_stop = void (*)(libvlc_media_player_t*);
using Fn_libvlc_media_player_pause = void (*)(libvlc_media_player_t*);
using Fn_libvlc_media_player_is_playing = int (*)(libvlc_media_player_t*);
using Fn_libvlc_media_player_event_manager = libvlc_event_manager_t* (*)(libvlc_media_player_t*);
using Fn_libvlc_event_attach = int (*)(libvlc_event_manager_t*, libvlc_event_e, libvlc_callback_t, void*);

HMODULE g_libvlcMod = nullptr;
Fn_libvlc_new p_libvlc_new = nullptr;
Fn_libvlc_release p_libvlc_release = nullptr;
Fn_libvlc_media_new_location p_libvlc_media_new_location = nullptr;
Fn_libvlc_media_add_option p_libvlc_media_add_option = nullptr;
Fn_libvlc_media_release p_libvlc_media_release = nullptr;
Fn_libvlc_media_player_new_from_media p_libvlc_media_player_new_from_media = nullptr;
Fn_libvlc_media_player_release p_libvlc_media_player_release = nullptr;
Fn_libvlc_media_player_set_hwnd p_libvlc_media_player_set_hwnd = nullptr;
Fn_libvlc_media_player_play p_libvlc_media_player_play = nullptr;
Fn_libvlc_media_player_stop p_libvlc_media_player_stop = nullptr;
Fn_libvlc_media_player_pause p_libvlc_media_player_pause = nullptr;
Fn_libvlc_media_player_is_playing p_libvlc_media_player_is_playing = nullptr;
Fn_libvlc_media_player_event_manager p_libvlc_media_player_event_manager = nullptr;
Fn_libvlc_event_attach p_libvlc_event_attach = nullptr;

std::atomic<bool> g_open{false};
std::atomic<bool> g_closing{false};
std::atomic<bool> g_playing{false};
std::atomic<bool> g_playFailed{false};
HWND g_hwnd = nullptr;
std::mutex g_mu;
std::wstring g_status;
std::string g_videoId;
std::thread g_worker;

libvlc_instance_t* g_vlc = nullptr;
libvlc_media_player_t* g_mp = nullptr;

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}

void setStatus(const std::wstring& s) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_status = s;
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, TRUE);
}

void centerOnParent(HWND hwnd, HWND parent) {
    RECT rc{}, pr{};
    GetWindowRect(hwnd, &rc);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    if (parent && IsWindow(parent)) {
        GetWindowRect(parent, &pr);
        SetWindowPos(hwnd, nullptr,
                     pr.left + ((pr.right - pr.left) - ww) / 2,
                     pr.top + ((pr.bottom - pr.top) - wh) / 2,
                     0, 0, SWP_NOSIZE | SWP_NOZORDER);
    } else {
        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        SetWindowPos(hwnd, nullptr, (sw - ww) / 2, (sh - wh) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
}

std::string libvlcRoot() {
    std::string v = std::string(APP_ASSET_DIR) + "\\libvlc";
    if (fs::exists(v + "\\libvlc.dll")) return v;
    const char* candidates[] = {
        "C:\\Program Files\\VideoLAN\\VLC",
        "C:\\Program Files (x86)\\VideoLAN\\VLC",
    };
    for (auto* c : candidates) {
        if (fs::exists(std::string(c) + "\\libvlc.dll")) return c;
    }
    return v;
}

template <typename T>
bool loadFn(HMODULE m, const char* name, T& out) {
    out = reinterpret_cast<T>(GetProcAddress(m, name));
    return out != nullptr;
}

bool loadLibVlc(std::string* err) {
    if (g_libvlcMod) return true;
    const std::string root = libvlcRoot();
    const std::wstring dll = widen(root + "\\libvlc.dll");
    if (!fs::exists(root + "\\libvlc.dll")) {
        if (err) *err = "brak libvlc.dll w " + root;
        return false;
    }
    SetDllDirectoryW(widen(root).c_str());
    SetEnvironmentVariableA("VLC_PLUGIN_PATH", (root + "\\plugins").c_str());
    g_libvlcMod = LoadLibraryW(dll.c_str());
    if (!g_libvlcMod) {
        if (err) *err = "LoadLibrary libvlc.dll failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    bool ok =
        loadFn(g_libvlcMod, "libvlc_new", p_libvlc_new) &&
        loadFn(g_libvlcMod, "libvlc_release", p_libvlc_release) &&
        loadFn(g_libvlcMod, "libvlc_media_new_location", p_libvlc_media_new_location) &&
        loadFn(g_libvlcMod, "libvlc_media_add_option", p_libvlc_media_add_option) &&
        loadFn(g_libvlcMod, "libvlc_media_release", p_libvlc_media_release) &&
        loadFn(g_libvlcMod, "libvlc_media_player_new_from_media", p_libvlc_media_player_new_from_media) &&
        loadFn(g_libvlcMod, "libvlc_media_player_release", p_libvlc_media_player_release) &&
        loadFn(g_libvlcMod, "libvlc_media_player_set_hwnd", p_libvlc_media_player_set_hwnd) &&
        loadFn(g_libvlcMod, "libvlc_media_player_play", p_libvlc_media_player_play) &&
        loadFn(g_libvlcMod, "libvlc_media_player_stop", p_libvlc_media_player_stop) &&
        loadFn(g_libvlcMod, "libvlc_media_player_pause", p_libvlc_media_player_pause) &&
        loadFn(g_libvlcMod, "libvlc_media_player_is_playing", p_libvlc_media_player_is_playing) &&
        loadFn(g_libvlcMod, "libvlc_media_player_event_manager", p_libvlc_media_player_event_manager) &&
        loadFn(g_libvlcMod, "libvlc_event_attach", p_libvlc_event_attach);
    if (!ok) {
        if (err) *err = "brak symboli w libvlc.dll";
        FreeLibrary(g_libvlcMod);
        g_libvlcMod = nullptr;
        return false;
    }
    return true;
}

void openInBrowser(const std::string& videoId) {
    std::wstring watch = L"https://www.youtube.com/watch?v=" + widen(videoId);
    ShellExecuteW(nullptr, L"open", watch.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void destroyPlayer() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_mp && p_libvlc_media_player_stop && p_libvlc_media_player_release) {
        p_libvlc_media_player_stop(g_mp);
        p_libvlc_media_player_release(g_mp);
        g_mp = nullptr;
    }
    if (g_vlc && p_libvlc_release) {
        p_libvlc_release(g_vlc);
        g_vlc = nullptr;
    }
    g_playing = false;
}

void paintStatus(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc; GetClientRect(hwnd, &rc);
    HBRUSH br = CreateSolidBrush(RGB(17, 24, 39));
    FillRect(hdc, &rc, br);
    DeleteObject(br);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(229, 231, 235));
    std::wstring status;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        status = g_status;
    }
    DrawTextW(hdc, status.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT:
        if (!g_playing.load()) paintStatus(hwnd);
        else {
            PAINTSTRUCT ps; BeginPaint(hwnd, &ps); EndPaint(hwnd, &ps);
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { DestroyWindow(hwnd); return 0; }
        if (wp == VK_SPACE && g_mp && p_libvlc_media_player_is_playing && p_libvlc_media_player_pause && p_libvlc_media_player_play) {
            if (p_libvlc_media_player_is_playing(g_mp)) p_libvlc_media_player_pause(g_mp);
            else p_libvlc_media_player_play(g_mp);
            return 0;
        }
        break;
    case WM_DESTROY:
        destroyPlayer();
        g_hwnd = nullptr;
        g_open = false;
        g_closing = false;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND createPlayerWindow(HWND parent) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"SeerrTrailerPlayer";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RegisterClassW(&wc);
        reg = true;
    }
    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_APPWINDOW,
        L"SeerrTrailerPlayer", widen(i18n::tr("yt.window_title")).c_str(),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 960, 540,
        parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (parent && IsWindow(parent))
        SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, (LONG_PTR)parent);
    centerOnParent(hwnd, parent);
    return hwnd;
}

static void onVlcEvent(const libvlc_event_t* ev, void*) {
    switch (ev->type) {
    case libvlc_MediaPlayerPlaying:
        g_playing = true;
        setStatus(L"");
        if (g_hwnd) InvalidateRect(g_hwnd, nullptr, TRUE);
        break;
    case libvlc_MediaPlayerEncounteredError:
        g_playFailed = true;
        break;
    default:
        break;
    }
}

bool tryNativePlay(const std::string& videoId, std::string* err) {
    if (!loadLibVlc(err)) return false;

    const std::string root = libvlcRoot();
    SetCurrentDirectoryW(widen(root).c_str());

    std::string pluginArg = "--plugin-path=" + root + "\\plugins";
    const char* vargs[] = {
        "--intf", "dummy",
        "--no-video-title-show",
        "--quiet",
        "--network-caching=2000",
        "--no-osd",
        pluginArg.c_str(),
    };

    g_vlc = p_libvlc_new((int)(sizeof(vargs) / sizeof(vargs[0])), vargs);
    if (!g_vlc) {
        if (err) *err = "libvlc_new failed";
        return false;
    }

    const std::string url = "https://www.youtube.com/watch?v=" + videoId;
    libvlc_media_t* media = p_libvlc_media_new_location(g_vlc, url.c_str());
    if (!media) {
        if (err) *err = "libvlc_media_new_location failed";
        return false;
    }
    p_libvlc_media_add_option(media, ":network-caching=2000");

    g_mp = p_libvlc_media_player_new_from_media(media);
    p_libvlc_media_release(media);
    if (!g_mp) {
        if (err) *err = "libvlc_media_player_new failed";
        return false;
    }

    p_libvlc_media_player_set_hwnd(g_mp, g_hwnd);

    libvlc_event_manager_t* em = p_libvlc_media_player_event_manager(g_mp);
    p_libvlc_event_attach(em, (libvlc_event_e)libvlc_MediaPlayerPlaying, onVlcEvent, nullptr);
    p_libvlc_event_attach(em, (libvlc_event_e)libvlc_MediaPlayerEncounteredError, onVlcEvent, nullptr);

    g_playFailed = false;
    g_playing = false;
    setStatus(widen(i18n::tr("yt.connecting")));
    if (p_libvlc_media_player_play(g_mp) != 0) {
        if (err) *err = "libvlc_media_player_play failed";
        return false;
    }

    for (int i = 0; i < 200 && !g_closing.load(); ++i) {
        if (g_playing.load()) return true;
        if (g_playFailed.load()) {
            if (err) *err = i18n::tr("yt.vlc_fail");
            return false;
        }
        Sleep(100);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return g_playing.load();
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!IsWindow(g_hwnd)) return false;
    }

    if (g_playing.load()) return true;
    if (err) *err = i18n::tr("yt.timeout");
    return false;
}

void workerMain(std::string videoId, HWND parent) {
    HWND hwnd = createPlayerWindow(parent);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_hwnd = hwnd;
    }
    setStatus(widen(i18n::tr("yt.loading")));

    std::string err;
    if (!tryNativePlay(videoId, &err)) {
        openInBrowser(videoId);
        destroyPlayer();
        if (IsWindow(hwnd)) DestroyWindow(hwnd);
        g_hwnd = nullptr;
        g_open = false;
        return;
    }

    MSG msg;
    while (g_open.load() && IsWindow(hwnd)) {
        BOOL r = GetMessageW(&msg, nullptr, 0, 0);
        if (r <= 0) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (g_playFailed.load()) {
            openInBrowser(videoId);
            break;
        }
    }

    destroyPlayer();
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    g_hwnd = nullptr;
    g_open = false;
}

} // namespace

bool isOpen() { return g_open.load(); }

void close() {
    g_closing = true;
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        hwnd = g_hwnd;
    }
    if (hwnd && IsWindow(hwnd)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
}

void tick() {}

void open(const std::string& videoId, void* parentHwnd) {
    if (videoId.empty()) return;
    if (g_open.load()) {
        close();
        for (int i = 0; i < 100 && g_open.load(); ++i) Sleep(20);
    }
    if (g_open.exchange(true)) return;
    g_videoId = videoId;
    g_closing = false;
    g_playing = false;
    g_playFailed = false;
    HWND parent = (HWND)parentHwnd;
    if (g_worker.joinable()) g_worker.detach();
    g_worker = std::thread(workerMain, videoId, parent);
}

} // namespace ytplayer

#else // !_WIN32

#include "platform.hpp"

namespace ytplayer {

void open(const std::string& videoId, void*) {
    if (videoId.empty()) return;
    platform::openUrl("https://www.youtube.com/watch?v=" + videoId);
}
bool isOpen() { return false; }
void close() {}
void tick() {}

} // namespace ytplayer

#endif
