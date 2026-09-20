#include "player.hpp"
#include "stack.hpp"
#include "util.hpp"
#include "core.hpp"
#include "gl_compat.hpp"
#include "widgets.hpp"
#include "platform.hpp"
#include "svgicons.hpp"
#include "i18n.hpp"
#include <GLFW/glfw3.h>
#include "imgui.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>
#include <algorithm>
#include <cstdio>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;
namespace player {
namespace {

struct libvlc_instance_t;
struct libvlc_media_t;
struct libvlc_media_player_t;
struct libvlc_event_manager_t;
struct libvlc_track_description_t {
    int i_id; char* psz_name; libvlc_track_description_t* p_next;
};
using libvlc_time_t = int64_t;
enum libvlc_event_e {
    libvlc_MediaPlayerPlaying = 0x104,
    libvlc_MediaPlayerPaused = 0x105,
    libvlc_MediaPlayerStopped = 0x106,
    libvlc_MediaPlayerEndReached = 0x109,
    libvlc_MediaPlayerEncounteredError = 0x10a,
};
struct libvlc_event_t { int type; void* p_obj; char pad[64]; };
using libvlc_callback_t = void (*)(const libvlc_event_t*, void*);
using libvlc_video_lock_cb = void* (*)(void*, void**);
using libvlc_video_unlock_cb = void (*)(void*, void*, void* const*);
using libvlc_video_display_cb = void (*)(void*, void*);
using libvlc_video_format_cb = unsigned (*)(void**, char*, unsigned*, unsigned*, unsigned*, unsigned*);
using libvlc_video_cleanup_cb = void (*)(void*);

#ifdef _WIN32
using LibHandle = HMODULE;
static LibHandle loadLib(const wchar_t* p) { return LoadLibraryW(p); }
static void* getSym(LibHandle h, const char* n) { return (void*)GetProcAddress(h, n); }
static void closeLib(LibHandle h) { if (h) FreeLibrary(h); }
static std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring o(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), o.data(), n);
    return o;
}
#else
using LibHandle = void*;
static LibHandle loadLib(const char* p) { return dlopen(p, RTLD_NOW | RTLD_LOCAL); }
static void* getSym(LibHandle h, const char* n) { return dlsym(h, n); }
static void closeLib(LibHandle h) { if (h) dlclose(h); }
#endif

LibHandle g_mod = nullptr;
libvlc_instance_t* (*p_libvlc_new)(int, const char* const*) = nullptr;
void (*p_libvlc_release)(libvlc_instance_t*) = nullptr;
libvlc_media_t* (*p_libvlc_media_new_path)(libvlc_instance_t*, const char*) = nullptr;
void (*p_libvlc_media_release)(libvlc_media_t*) = nullptr;
libvlc_media_player_t* (*p_libvlc_media_player_new_from_media)(libvlc_media_t*) = nullptr;
void (*p_libvlc_media_player_release)(libvlc_media_player_t*) = nullptr;
int (*p_libvlc_media_player_play)(libvlc_media_player_t*) = nullptr;
void (*p_libvlc_media_player_stop)(libvlc_media_player_t*) = nullptr;
void (*p_libvlc_media_player_set_pause)(libvlc_media_player_t*, int) = nullptr;
int (*p_libvlc_media_player_is_playing)(libvlc_media_player_t*) = nullptr;
void (*p_libvlc_media_player_set_position)(libvlc_media_player_t*, float) = nullptr;
float (*p_libvlc_media_player_get_position)(libvlc_media_player_t*) = nullptr;
libvlc_time_t (*p_libvlc_media_player_get_time)(libvlc_media_player_t*) = nullptr;
libvlc_time_t (*p_libvlc_media_player_get_length)(libvlc_media_player_t*) = nullptr;
int (*p_libvlc_audio_set_volume)(libvlc_media_player_t*, int) = nullptr;
int (*p_libvlc_audio_set_mute)(libvlc_media_player_t*, int) = nullptr;
void (*p_libvlc_video_set_callbacks)(libvlc_media_player_t*, libvlc_video_lock_cb, libvlc_video_unlock_cb, libvlc_video_display_cb, void*) = nullptr;
void (*p_libvlc_video_set_format_callbacks)(libvlc_media_player_t*, libvlc_video_format_cb, libvlc_video_cleanup_cb) = nullptr;
int (*p_libvlc_video_get_spu)(libvlc_media_player_t*) = nullptr;
int (*p_libvlc_video_set_spu)(libvlc_media_player_t*, int) = nullptr;
libvlc_track_description_t* (*p_libvlc_video_get_spu_description)(libvlc_media_player_t*) = nullptr;
libvlc_track_description_t* (*p_libvlc_audio_get_track_description)(libvlc_media_player_t*) = nullptr;
int (*p_libvlc_audio_get_track)(libvlc_media_player_t*) = nullptr;
int (*p_libvlc_audio_set_track)(libvlc_media_player_t*, int) = nullptr;
void (*p_libvlc_track_description_list_release)(libvlc_track_description_t*) = nullptr;
libvlc_event_manager_t* (*p_libvlc_media_player_event_manager)(libvlc_media_player_t*) = nullptr;
int (*p_libvlc_event_attach)(libvlc_event_manager_t*, libvlc_event_e, libvlc_callback_t, void*) = nullptr;
libvlc_media_t* (*p_libvlc_media_player_get_media)(libvlc_media_player_t*) = nullptr;
float (*p_libvlc_media_player_get_rate)(libvlc_media_player_t*) = nullptr;
float (*p_libvlc_media_player_get_fps)(libvlc_media_player_t*) = nullptr;
int (*p_libvlc_video_get_size)(libvlc_media_player_t*, unsigned, unsigned*, unsigned*) = nullptr;
int (*p_libvlc_media_get_stats)(libvlc_media_t*, void*) = nullptr; // libvlc_media_stats_t*

// Mirrors libvlc_media_stats_t (VLC 3.x layout)
struct VlcMediaStats {
    int i_read_bytes = 0;
    float f_input_bitrate = 0;
    int i_demux_read_bytes = 0;
    float f_demux_bitrate = 0;
    int i_demux_corrupted = 0;
    int i_demux_discontinuity = 0;
    int i_decoded_video = 0;
    int i_decoded_audio = 0;
    int i_displayed_pictures = 0;
    int i_lost_pictures = 0;
    int i_played_abuffers = 0;
    int i_lost_abuffers = 0;
    int i_sent_packets = 0;
    int i_sent_bytes = 0;
    float f_send_bitrate = 0;
};

State g_st{};
std::mutex g_frameMu;
std::vector<uint8_t> g_pixels, g_pixelsUpload;
unsigned g_pixW = 0, g_pixH = 0;
unsigned g_texAllocW = 0, g_texAllocH = 0;
std::atomic<bool> g_frameReady{false};
std::atomic<bool> g_tracksDirty{true};
std::atomic<bool> g_cbEnabled{false}; // video/event callbacks must no-op when false
std::atomic<uint64_t> g_openGen{0};
std::atomic<uint64_t> g_framesDisplayed{0};
libvlc_instance_t* g_vlc = nullptr;
libvlc_media_player_t* g_mp = nullptr;
GLFWwindow* g_host = nullptr;
int g_prevX = 0, g_prevY = 0, g_prevW = 0, g_prevH = 0;
bool g_showControls = true;
float g_idleTimer = 0;
float g_controlsAlpha = 1.f; // 0..1 fade
bool g_userPaused = false;   // explicit pause — not libvlc is_playing flicker
bool g_mouseInChrome = false;
bool g_cursorHidden = false;
double g_ignoreMouseUntil = 0; // suppress spurious deltas after cursor mode change
bool g_showSubsMenu = false, g_showAudioMenu = false, g_showSettings = false;
bool g_showStats = false;
float g_instantFps = 0;
uint64_t g_fpsFrameMark = 0;
double g_fpsTimeMark = 0;
unsigned g_srcW = 0, g_srcH = 0;
VlcMediaStats g_vlcStats{};
int g_statsDecodedVideo0 = -1;
int g_statsDisplayed0 = -1;

struct PendingOpen {
    bool ready = false;
    bool ok = false;
    uint64_t gen = 0;
    std::string error;
    libvlc_instance_t* vlc = nullptr;
    libvlc_media_player_t* mp = nullptr;
};
std::mutex g_pendingMu;
PendingOpen g_pending;

bool loadVlc(std::string* err) {
    if (g_mod) return true;
#ifdef _WIN32
    std::string root = util::libvlcDir();
    if (root.empty()) { if (err) *err = i18n::tr("player.no_vlc"); return false; }
    SetDllDirectoryW(widen(root).c_str());
    SetEnvironmentVariableA("VLC_PLUGIN_PATH", (root + "\\plugins").c_str());
    g_mod = loadLib(widen(root + "\\libvlc.dll").c_str());
#else
    // System packages: libvlc5 / vlc
    for (auto* c : {
             "libvlc.so.5",
             "libvlc.so",
             "/usr/lib/x86_64-linux-gnu/libvlc.so.5",
             "/usr/lib/libvlc.so.5",
         }) {
        g_mod = loadLib(c);
        if (g_mod) break;
    }
#endif
    if (!g_mod) { if (err) *err = i18n::tr("player.vlc_load_failed"); return false; }
#define L(n) do { p_##n = (decltype(p_##n))getSym(g_mod, #n); if (!p_##n) { if (err) *err = "brak " #n; closeLib(g_mod); g_mod = nullptr; return false; } } while (0)
    L(libvlc_new); L(libvlc_release); L(libvlc_media_new_path); L(libvlc_media_release);
    L(libvlc_media_player_new_from_media); L(libvlc_media_player_release);
    L(libvlc_media_player_play); L(libvlc_media_player_stop); L(libvlc_media_player_set_pause);
    L(libvlc_media_player_is_playing); L(libvlc_media_player_set_position); L(libvlc_media_player_get_position);
    L(libvlc_media_player_get_time); L(libvlc_media_player_get_length);
    L(libvlc_audio_set_volume); L(libvlc_audio_set_mute);
    L(libvlc_video_set_callbacks); L(libvlc_video_set_format_callbacks);
    L(libvlc_video_get_spu); L(libvlc_video_set_spu); L(libvlc_video_get_spu_description);
    L(libvlc_audio_get_track_description); L(libvlc_audio_get_track); L(libvlc_audio_set_track);
    L(libvlc_track_description_list_release); L(libvlc_media_player_event_manager); L(libvlc_event_attach);
#undef L
    // Optional symbols for Stats for nerds
    p_libvlc_media_player_get_media = (decltype(p_libvlc_media_player_get_media))getSym(g_mod, "libvlc_media_player_get_media");
    p_libvlc_media_player_get_rate = (decltype(p_libvlc_media_player_get_rate))getSym(g_mod, "libvlc_media_player_get_rate");
    p_libvlc_media_player_get_fps = (decltype(p_libvlc_media_player_get_fps))getSym(g_mod, "libvlc_media_player_get_fps");
    p_libvlc_video_get_size = (decltype(p_libvlc_video_get_size))getSym(g_mod, "libvlc_video_get_size");
    p_libvlc_media_get_stats = (decltype(p_libvlc_media_get_stats))getSym(g_mod, "libvlc_media_get_stats");
    return true;
}

void releaseVlcObjects(libvlc_media_player_t* mp, libvlc_instance_t* vlc) {
    // stop is idempotent; may already have been stopped on the UI thread
    if (mp && p_libvlc_media_player_stop) {
        try { p_libvlc_media_player_stop(mp); } catch (...) {}
    }
    if (mp && p_libvlc_media_player_release) {
        try { p_libvlc_media_player_release(mp); } catch (...) {}
    }
    if (vlc && p_libvlc_release) {
        try { p_libvlc_release(vlc); } catch (...) {}
    }
}

// Valid plane pointer even when callbacks are disabled (VLC must not get nullptr).
alignas(16) static uint8_t g_dummyPlane[64];

void* vlcLock(void*, void** planes) {
    if (!g_cbEnabled.load(std::memory_order_acquire)) {
        *planes = g_dummyPlane;
        return nullptr;
    }
    std::lock_guard<std::mutex> lk(g_frameMu);
    if (!g_cbEnabled.load(std::memory_order_relaxed) || g_pixels.empty()) {
        *planes = g_dummyPlane;
        return nullptr;
    }
    *planes = g_pixels.data();
    return nullptr;
}
void vlcUnlock(void*, void*, void* const*) {}
void vlcDisplay(void*, void*) {
    if (!g_cbEnabled.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(g_frameMu);
    if (!g_cbEnabled.load(std::memory_order_relaxed)) return;
    if (g_pixels.size() != (size_t)g_pixW * g_pixH * 4u) return;
    g_pixelsUpload.swap(g_pixels);
    if (g_pixels.size() != (size_t)g_pixW * g_pixH * 4u)
        g_pixels.assign((size_t)g_pixW * g_pixH * 4u, 0);
    g_frameReady = true;
    g_framesDisplayed.fetch_add(1);
}
unsigned vlcFormat(void**, char* chroma, unsigned* width, unsigned* height, unsigned* pitches, unsigned* lines) {
    memcpy(chroma, "RGBA", 4);
    if (!g_cbEnabled.load(std::memory_order_acquire)) {
        *width = 16; *height = 16;
        pitches[0] = 64; lines[0] = 16;
        return 1;
    }
    g_srcW = *width;
    g_srcH = *height;
    const unsigned maxW = 1280u;
    if (*width > maxW) {
        float s = (float)maxW / (float)*width;
        *width = maxW;
        *height = std::max(16u, (unsigned)std::lround(*height * s));
    }
    std::lock_guard<std::mutex> lk(g_frameMu);
    if (!g_cbEnabled.load(std::memory_order_relaxed)) {
        *width = 16; *height = 16;
        pitches[0] = 64; lines[0] = 16;
        return 1;
    }
    g_pixW = std::max(16u, *width);
    g_pixH = std::max(16u, *height);
    pitches[0] = g_pixW * 4;
    lines[0] = g_pixH;
    size_t need = (size_t)g_pixW * g_pixH * 4u;
    g_pixels.assign(need, 0);
    g_pixelsUpload.assign(need, 0);
    g_st.videoW = (int)g_pixW;
    g_st.videoH = (int)g_pixH;
    return 1;
}
void vlcCleanup(void*) {
    std::lock_guard<std::mutex> lk(g_frameMu);
    g_pixels.clear();
    g_pixelsUpload.clear();
    g_pixW = g_pixH = 0;
}
void onEvent(const libvlc_event_t* ev, void*) {
    if (!ev || !g_cbEnabled.load(std::memory_order_acquire)) return;
    if (ev->type == libvlc_MediaPlayerPlaying) { g_st.playing = true; g_st.paused = false; g_st.ready = true; g_st.loading = false; g_tracksDirty = true; }
    if (ev->type == libvlc_MediaPlayerPaused) g_st.paused = true;
    if (ev->type == libvlc_MediaPlayerStopped || ev->type == libvlc_MediaPlayerEndReached) { g_st.playing = false; g_st.paused = true; }
    if (ev->type == libvlc_MediaPlayerEncounteredError) { g_st.failed = true; g_st.loading = false; g_st.error = i18n::tr("player.error"); }
}
void refreshTracks() {
    if (!g_mp) return;
    g_st.subtitles.clear();
    g_st.audioTracks.clear();
    if (auto* list = p_libvlc_video_get_spu_description(g_mp)) {
        for (auto* t = list; t; t = t->p_next)
            g_st.subtitles.push_back({t->i_id, t->psz_name ? t->psz_name : ("Sub " + std::to_string(t->i_id))});
        p_libvlc_track_description_list_release(list);
        g_st.subtitleId = p_libvlc_video_get_spu(g_mp);

        // Prefer preferred language, then EN (Bazarr-style)
        std::string pref = util::lower(stack::StackConfig::get().subsPreferredLang);
        if (pref.empty()) pref = "pl";
        auto matchLang = [](const std::string& name, const std::string& lang) {
            std::string n = util::lower(name);
            return n.find(lang) != std::string::npos ||
                   (lang == "pl" && (n.find("polski") != std::string::npos || n.find("polish") != std::string::npos)) ||
                   (lang == "en" && (n.find("english") != std::string::npos || n.find("angiel") != std::string::npos));
        };
        int pick = -1;
        for (auto& t : g_st.subtitles)
            if (t.id >= 0 && matchLang(t.name, pref)) { pick = t.id; break; }
        if (pick < 0 && pref != "en") {
            for (auto& t : g_st.subtitles)
                if (t.id >= 0 && matchLang(t.name, "en")) { pick = t.id; break; }
        }
        if (pick >= 0 && pick != g_st.subtitleId) {
            p_libvlc_video_set_spu(g_mp, pick);
            g_st.subtitleId = pick;
        }
    }
    if (auto* list = p_libvlc_audio_get_track_description(g_mp)) {
        for (auto* t = list; t; t = t->p_next)
            g_st.audioTracks.push_back({t->i_id, t->psz_name ? t->psz_name : ("Audio " + std::to_string(t->i_id))});
        p_libvlc_track_description_list_release(list);
        g_st.audioId = p_libvlc_audio_get_track(g_mp);
    }
}
void ensureTex(unsigned w, unsigned h) {
    if (!g_st.tex) {
        GLuint t = 0;
        glGenTextures(1, &t);
        g_st.tex = t;
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        g_texAllocW = g_texAllocH = 0;
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)g_st.tex);
    if (g_texAllocW != w || g_texAllocH != h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        g_texAllocW = w;
        g_texAllocH = h;
    }
}
void uploadFrame() {
    if (!g_cbEnabled.load(std::memory_order_acquire)) return;
    if (!g_frameReady.exchange(false)) return;
    std::vector<uint8_t> local;
    unsigned w = 0, h = 0;
    {
        std::lock_guard<std::mutex> lk(g_frameMu);
        local.swap(g_pixelsUpload);
        w = g_pixW; h = g_pixH;
        if (g_pixelsUpload.size() != (size_t)w * h * 4u && w && h)
            g_pixelsUpload.assign((size_t)w * h * 4u, 0);
    }
    if (local.empty() || !w || !h) return;
    ensureTex(w, h);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, local.data());
    g_st.videoW = (int)w;
    g_st.videoH = (int)h;
}
std::string fmtTime(int64_t ms) {
    if (ms < 0) ms = 0;
    int s = (int)(ms / 1000);
    int h = s / 3600; s %= 3600;
    int m = s / 60; s %= 60;
    char b[32];
    if (h > 0) std::snprintf(b, sizeof(b), "%d:%02d:%02d", h, m, s);
    else std::snprintf(b, sizeof(b), "%d:%02d", m, s);
    return b;
}

void applyPendingOpen() {
    PendingOpen p;
    {
        std::lock_guard<std::mutex> lk(g_pendingMu);
        if (!g_pending.ready) return;
        p = g_pending;
        g_pending = PendingOpen{};
    }
    if (p.gen != g_openGen.load()) {
        core::enqueue([mp = p.mp, vlc = p.vlc]() { releaseVlcObjects(mp, vlc); });
        return;
    }
    if (!p.ok) {
        g_st.failed = true;
        g_st.loading = false;
        g_st.error = p.error.empty() ? i18n::tr("player.open_failed") : p.error;
        if (p.mp || p.vlc)
            core::enqueue([mp = p.mp, vlc = p.vlc]() { releaseVlcObjects(mp, vlc); });
        return;
    }
    g_vlc = p.vlc;
    g_mp = p.mp;
    g_cbEnabled.store(true, std::memory_order_release);
    g_st.loading = false;
    g_st.playing = true;
    g_st.paused = false;
    g_tracksDirty = true;
}

void startOpenJob(uint64_t gen, std::string path, int volume) {
    core::enqueue([gen, path = std::move(path), volume]() {
        PendingOpen out;
        out.gen = gen;
        out.ready = true;

        auto fail = [&](const std::string& e) {
            out.ok = false;
            out.error = e;
            releaseVlcObjects(out.mp, out.vlc);
            out.mp = nullptr;
            out.vlc = nullptr;
            std::lock_guard<std::mutex> lk(g_pendingMu);
            if (gen == g_openGen.load()) g_pending = out;
            else releaseVlcObjects(out.mp, out.vlc);
        };

        if (gen != g_openGen.load()) return;

        std::string err;
        if (!loadVlc(&err)) { fail(err); return; }
        std::error_code ec;
        if (!fs::exists(path, ec)) { fail(i18n::tr("player.file_missing")); return; }

        std::string pref = util::lower(stack::StackConfig::get().subsPreferredLang);
        if (pref.empty()) pref = "pl";
        std::string subLangArg = "--sub-language=" + pref + ",en";
        const char* args[] = {
            "--no-video-title-show",
            "--quiet",
            "--network-caching=300",
            "--file-caching=300",
            "--sub-autodetect-file",
            "--sub-autodetect-fuzzy=1",
            subLangArg.c_str(),
        };
        auto* vlc = p_libvlc_new(7, args);
        if (!vlc) vlc = p_libvlc_new(0, nullptr);
        if (!vlc) { fail("libvlc_new failed"); return; }
        out.vlc = vlc;

        if (gen != g_openGen.load()) { releaseVlcObjects(nullptr, vlc); return; }

        auto* media = p_libvlc_media_new_path(vlc, path.c_str());
        if (!media) { fail(i18n::tr("player.file_open_failed")); return; }
        auto* mp = p_libvlc_media_player_new_from_media(media);
        p_libvlc_media_release(media);
        if (!mp) { fail("media_player failed"); return; }
        out.mp = mp;

        p_libvlc_video_set_callbacks(mp, vlcLock, vlcUnlock, vlcDisplay, nullptr);
        p_libvlc_video_set_format_callbacks(mp, vlcFormat, vlcCleanup);
        auto* em = p_libvlc_media_player_event_manager(mp);
        p_libvlc_event_attach(em, libvlc_MediaPlayerPlaying, onEvent, nullptr);
        p_libvlc_event_attach(em, libvlc_MediaPlayerPaused, onEvent, nullptr);
        p_libvlc_event_attach(em, libvlc_MediaPlayerStopped, onEvent, nullptr);
        p_libvlc_event_attach(em, libvlc_MediaPlayerEndReached, onEvent, nullptr);
        p_libvlc_event_attach(em, libvlc_MediaPlayerEncounteredError, onEvent, nullptr);
        p_libvlc_audio_set_volume(mp, volume);

        if (gen != g_openGen.load()) {
            releaseVlcObjects(mp, vlc);
            return;
        }

        g_cbEnabled.store(true, std::memory_order_release);
        p_libvlc_media_player_play(mp);
        out.ok = true;
        out.error.clear();

        std::lock_guard<std::mutex> lk(g_pendingMu);
        if (gen != g_openGen.load()) {
            releaseVlcObjects(mp, vlc);
            return;
        }
        g_pending = out;
    });
}

} // anon

void bindWindow(GLFWwindow* w) { g_host = w; }

bool open(const std::string& path, const std::string& title) {
    close();
    uint64_t gen = ++g_openGen;
    g_st = State{};
    g_st.open = true;
    g_st.loading = true;
    g_st.title = title.empty() ? fs::path(path).stem().string() : title;
    g_st.path = path;
    g_st.volume = 80;
    g_showControls = true;
    g_idleTimer = 0;
    g_controlsAlpha = 1.f;
    g_userPaused = false;
    g_mouseInChrome = false;
    g_cursorHidden = false;
    g_ignoreMouseUntil = 0;
    g_showSubsMenu = g_showAudioMenu = g_showSettings = false;
    g_tracksDirty = true;
    g_frameReady = false;
    g_framesDisplayed = 0;
    g_fpsFrameMark = 0;
    g_fpsTimeMark = 0;
    g_instantFps = 0;
    g_srcW = g_srcH = 0;
    g_vlcStats = {};
    g_statsDecodedVideo0 = -1;
    g_statsDisplayed0 = -1;
    {
        std::lock_guard<std::mutex> lk(g_pendingMu);
        g_pending = PendingOpen{};
    }
    startOpenJob(gen, path, g_st.volume);
    return true;
}

void close() {
    // Invalidate generation first so in-flight open jobs discard their players
    g_openGen.fetch_add(1);
    g_cbEnabled.store(false, std::memory_order_release);
    g_frameReady.store(false);

    if (g_host) glfwSetInputMode(g_host, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    g_cursorHidden = false;

    const bool wasFs = g_st.fullscreen;

    auto* mp = g_mp;
    auto* vlc = g_vlc;
    g_mp = nullptr;
    g_vlc = nullptr;

    // Stop on the UI thread so decoder callbacks finish before we free frame buffers.
    // Async release without stop was racing vlcLock/display → intermittent crashes.
    if (mp && p_libvlc_media_player_stop) {
        try { p_libvlc_media_player_stop(mp); } catch (...) {}
    }

    {
        std::lock_guard<std::mutex> lk(g_frameMu);
        g_pixels.clear();
        g_pixels.shrink_to_fit();
        g_pixelsUpload.clear();
        g_pixelsUpload.shrink_to_fit();
        g_pixW = g_pixH = 0;
    }

    if (g_st.tex) {
        GLuint t = (GLuint)g_st.tex;
        glDeleteTextures(1, &t);
        g_st.tex = 0;
    }
    g_texAllocW = g_texAllocH = 0;

    {
        std::lock_guard<std::mutex> lk(g_pendingMu);
        if (g_pending.ready) {
            auto stale = g_pending;
            g_pending = PendingOpen{};
            if (stale.mp || stale.vlc)
                core::enqueue([mp = stale.mp, vlc = stale.vlc]() { releaseVlcObjects(mp, vlc); });
        }
    }

    if (mp || vlc)
        core::enqueue([mp, vlc]() { releaseVlcObjects(mp, vlc); });

    if (wasFs && g_host) {
        glfwSetWindowMonitor(g_host, nullptr, g_prevX, g_prevY,
                             g_prevW > 0 ? g_prevW : 1500, g_prevH > 0 ? g_prevH : 900, 0);
    }

    g_showSubsMenu = g_showAudioMenu = g_showSettings = false;
    g_showStats = false;
    g_userPaused = false;
    g_controlsAlpha = 1.f;
    g_showControls = true;
    g_st = State{};
}

bool isOpen() { return g_st.open; }

void togglePause() {
    if (!g_mp || g_st.loading) return;
    bool now = p_libvlc_media_player_is_playing(g_mp) != 0;
    p_libvlc_media_player_set_pause(g_mp, now ? 1 : 0);
    g_st.paused = now; g_st.playing = !now;
    g_userPaused = now; // paused when we were playing
    g_showControls = true; g_idleTimer = 0; g_controlsAlpha = 1.f;
}
void setPosition(double f) {
    if (!g_mp || g_st.loading) return;
    f = std::clamp(f, 0.0, 1.0);
    p_libvlc_media_player_set_position(g_mp, (float)f);
    g_st.position = f;
    g_showControls = true; g_idleTimer = 0;
}
void seekRelative(double seconds) {
    if (!g_mp || g_st.loading || g_st.durationMs <= 0) return;
    setPosition(p_libvlc_media_player_get_position(g_mp) + seconds / (g_st.durationMs / 1000.0));
}
void setVolume(int v) {
    v = std::clamp(v, 0, 100);
    g_st.volume = v;
    if (g_mp) p_libvlc_audio_set_volume(g_mp, g_st.muted ? 0 : v);
}
void toggleMute() {
    g_st.muted = !g_st.muted;
    if (g_mp) p_libvlc_audio_set_mute(g_mp, g_st.muted ? 1 : 0);
}
void toggleFullscreen() {
    if (!g_host) return;
    if (!g_st.fullscreen) {
        glfwGetWindowPos(g_host, &g_prevX, &g_prevY);
        glfwGetWindowSize(g_host, &g_prevW, &g_prevH);
        GLFWmonitor* mon = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(mon);
        glfwSetWindowMonitor(g_host, mon, 0, 0, mode->width, mode->height, mode->refreshRate);
    g_st.fullscreen = true;
    } else {
        glfwSetWindowMonitor(g_host, nullptr, g_prevX, g_prevY,
                             g_prevW > 0 ? g_prevW : 1500, g_prevH > 0 ? g_prevH : 900, 0);
        g_st.fullscreen = false;
    }
    g_showControls = true; g_idleTimer = 0; g_controlsAlpha = 1.f;
    g_ignoreMouseUntil = ImGui::GetTime() + 0.25; // FS switch floods mouse events
    if (g_cursorHidden && g_host) {
        g_cursorHidden = false;
        glfwSetInputMode(g_host, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }
}
void setSubtitle(int id) { g_st.subtitleId = id; if (g_mp) p_libvlc_video_set_spu(g_mp, id); }
void setAudioTrack(int id) { g_st.audioId = id; if (g_mp) p_libvlc_audio_set_track(g_mp, id); }
void cycleSubtitle() {
    if (g_st.subtitles.empty()) return;
    std::vector<int> ids = { -1 };
    for (auto& t : g_st.subtitles) if (t.id >= 0) ids.push_back(t.id);
    int idx = 0;
    for (size_t i = 0; i < ids.size(); i++) if (ids[i] == g_st.subtitleId) idx = (int)i;
    setSubtitle(ids[(idx + 1) % ids.size()]);
    g_showControls = true;
}
void toggleStats() { g_showStats = !g_showStats; g_showControls = true; }
bool statsVisible() { return g_showStats; }

void tick() {
    if (!g_st.open) return;
    applyPendingOpen();
    if (!g_st.open) return; // closed while applying / discarded
    uploadFrame();
    if (g_mp && !g_st.loading && g_cbEnabled.load(std::memory_order_acquire)) {
        g_st.position = p_libvlc_media_player_get_position(g_mp);
        g_st.timeMs = p_libvlc_media_player_get_time(g_mp);
        g_st.durationMs = p_libvlc_media_player_get_length(g_mp);
        g_st.playing = p_libvlc_media_player_is_playing(g_mp) != 0;
        g_st.paused = !g_st.playing;
        if (g_tracksDirty.exchange(false) || (g_st.ready && g_st.subtitles.empty() && g_st.timeMs > 1500))
            refreshTracks();
        if (p_libvlc_video_get_size) {
            unsigned sw = 0, sh = 0;
            if (p_libvlc_video_get_size(g_mp, 0, &sw, &sh) == 0 && sw && sh) {
                g_srcW = sw; g_srcH = sh;
            }
        }
        if (p_libvlc_media_get_stats && p_libvlc_media_player_get_media) {
            if (auto* media = p_libvlc_media_player_get_media(g_mp)) {
                VlcMediaStats st{};
                if (p_libvlc_media_get_stats(media, &st)) {
                    if (g_statsDecodedVideo0 < 0) {
                        g_statsDecodedVideo0 = st.i_decoded_video;
                        g_statsDisplayed0 = st.i_displayed_pictures;
                    }
                    g_vlcStats = st;
                }
                p_libvlc_media_release(media);
            }
        }
    }
    // Instant FPS from our display callbacks
    {
        double now = ImGui::GetTime();
        if (g_fpsTimeMark <= 0) { g_fpsTimeMark = now; g_fpsFrameMark = g_framesDisplayed.load(); }
        else if (now - g_fpsTimeMark >= 0.5) {
            uint64_t f = g_framesDisplayed.load();
            g_instantFps = (float)((f - g_fpsFrameMark) / (now - g_fpsTimeMark));
            g_fpsFrameMark = f;
            g_fpsTimeMark = now;
        }
    }
    ImGuiIO& io = ImGui::GetIO();
    const double nowT = ImGui::GetTime();
    const float move2 = io.MouseDelta.x * io.MouseDelta.x + io.MouseDelta.y * io.MouseDelta.y;
    // Exclusive fullscreen + cursor hide/show injects fake MouseDelta on Windows
    const bool ignoreMouse = nowT < g_ignoreMouseUntil;
    const float moveThresh = g_st.fullscreen ? 16.f : 2.25f; // ~4px FS / ~1.5px windowed
    const bool mouseMoved = !ignoreMouse && move2 > moveThresh;
    const bool clicked = !ignoreMouse && (io.MouseClicked[0] || io.MouseClicked[1]);
    const bool menuOpen = g_showSubsMenu || g_showAudioMenu || g_showSettings;
    const bool forceChrome = g_userPaused || g_st.failed || g_st.loading || menuOpen;
    // In fullscreen, resting on the bar must not pin the overlay forever
    const bool hoverKeeps = g_mouseInChrome && g_showControls && !g_st.fullscreen;

    if (mouseMoved || clicked) {
        g_showControls = true;
        g_idleTimer = 0;
    } else if (forceChrome || hoverKeeps) {
        g_showControls = true;
        g_idleTimer = 0;
    } else {
        g_idleTimer += io.DeltaTime;
        // Don't require libvlc is_playing — it flickers false in exclusive FS
        if (g_idleTimer > 2.f && !g_userPaused && !g_st.loading && !g_st.failed) {
            g_showControls = false;
            g_showSubsMenu = g_showAudioMenu = g_showSettings = false;
        }
    }

    // Smooth fade
    float targetA = (g_showControls || forceChrome) ? 1.f : 0.f;
    float speed = targetA > g_controlsAlpha ? 14.f : 10.f;
    g_controlsAlpha += (targetA - g_controlsAlpha) * (1.f - expf(-speed * io.DeltaTime));
    if (g_controlsAlpha < 0.01f) g_controlsAlpha = 0.f;
    if (g_controlsAlpha > 0.99f) g_controlsAlpha = 1.f;

    // Cursor hide with hysteresis — mode flips themselves generate mouse deltas on Win+FS
    if (g_host) {
        bool wantHide = g_controlsAlpha < 0.04f && !forceChrome && !g_st.failed && !g_st.loading;
        if (wantHide && !g_cursorHidden) {
            g_cursorHidden = true;
            glfwSetInputMode(g_host, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
            g_ignoreMouseUntil = nowT + 0.2;
            g_mouseInChrome = false;
        } else if (!wantHide && g_cursorHidden) {
            g_cursorHidden = false;
            glfwSetInputMode(g_host, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            g_ignoreMouseUntil = nowT + 0.2;
        }
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        if (menuOpen) {
            g_showSubsMenu = g_showAudioMenu = g_showSettings = false;
        } else if (g_st.fullscreen) toggleFullscreen();
        else close();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Space)) togglePause();
    if (ImGui::IsKeyPressed(ImGuiKey_F)) toggleFullscreen();
    if (ImGui::IsKeyPressed(ImGuiKey_M)) toggleMute();
    if (ImGui::IsKeyPressed(ImGuiKey_C)) cycleSubtitle();
    if (ImGui::IsKeyPressed(ImGuiKey_I) && io.KeyCtrl && io.KeyShift) toggleStats();
    if (ImGui::IsKeyPressed(ImGuiKey_GraveAccent)) toggleStats();
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) seekRelative(-10);
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) seekRelative(10);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) setVolume(g_st.volume + 5);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) setVolume(g_st.volume - 5);
}
const State& state() { return g_st; }

bool render() {
    if (!g_st.open) return false;
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 disp = io.DisplaySize;
    float top = 0.f;
    ImGui::SetNextWindowPos(ImVec2(0, top));
    ImGui::SetNextWindowSize(ImVec2(disp.x, disp.y - top));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 1));
    ImGui::Begin("##jfplayer", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wpos = ImGui::GetWindowPos();
    ImVec2 wsize = ImGui::GetWindowSize();

    if (g_st.tex && g_st.videoW > 0 && g_st.videoH > 0) {
        float ar = (float)g_st.videoW / (float)g_st.videoH, war = wsize.x / std::max(1.f, wsize.y);
        ImVec2 vmin, vmax;
        if (ar > war) { float h = wsize.x / ar; vmin = ImVec2(wpos.x, wpos.y + (wsize.y - h) * 0.5f); vmax = ImVec2(wpos.x + wsize.x, vmin.y + h); }
        else { float w = wsize.y * ar; vmin = ImVec2(wpos.x + (wsize.x - w) * 0.5f, wpos.y); vmax = ImVec2(vmin.x + w, wpos.y + wsize.y); }
        dl->AddImage((ImTextureID)(intptr_t)g_st.tex, vmin, vmax);
    } else if (g_st.loading) {
        ImVec2 c(wpos.x + wsize.x * 0.5f, wpos.y + wsize.y * 0.5f - 8.f);
        w::orbitSpinner(dl, c, 16.f, 3.6f);
        const char* msg = i18n::tr("player.loading");
        ImFont* f = G.r16 ? G.r16 : ImGui::GetFont();
        float fs = G.r16 ? 15.f : ImGui::GetFontSize();
        ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, msg);
        dl->AddText(f, fs, ImVec2(c.x - ts.x * 0.5f, c.y + 36.f), IM_COL32(156, 163, 175, 255), msg);
    } else if (g_st.failed) {
        const char* msg = g_st.error.empty() ? i18n::tr("player.error") : g_st.error.c_str();
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(wpos.x + (wsize.x - ts.x) * 0.5f, wpos.y + (wsize.y - ts.y) * 0.5f),
                    IM_COL32(248, 113, 113, 255), msg);
        ImGui::SetCursorScreenPos(ImVec2(wpos.x + wsize.x * 0.5f - 110, wpos.y + wsize.y * 0.5f + 36));
        if (ImGui::Button(i18n::tr("player.open_external"), ImVec2(200, 34))) platform::openPath(g_st.path);
        ImGui::SameLine();
        if (ImGui::Button(i18n::tr("common.close"), ImVec2(90, 34))) close();
    } else {
        ImVec2 c(wpos.x + wsize.x * 0.5f, wpos.y + wsize.y * 0.5f - 8.f);
        w::orbitSpinner(dl, c, 16.f, 3.6f);
        const char* msg = i18n::tr("player.loading");
        ImFont* f = G.r16 ? G.r16 : ImGui::GetFont();
        float fs = G.r16 ? 15.f : ImGui::GetFontSize();
        ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, msg);
        dl->AddText(f, fs, ImVec2(c.x - ts.x * 0.5f, c.y + 36.f), IM_COL32(156, 163, 175, 255), msg);
    }

    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0) && !g_showSubsMenu && !g_showAudioMenu && !g_showSettings && !g_st.loading) {
        if (io.MousePos.y > wpos.y + 80 && io.MousePos.y < wpos.y + wsize.y - 100) togglePause();
    }

    // Chrome hit zones — keep overlay while pointer rests on title/controls bars (windowed only)
    const float topZone = 110.f, botZone = 160.f;
    if (g_cursorHidden || g_st.fullscreen) {
        g_mouseInChrome = false;
    } else {
        g_mouseInChrome =
            (io.MousePos.y >= wpos.y && io.MousePos.y <= wpos.y + topZone) ||
            (io.MousePos.y >= wpos.y + wsize.y - botZone && io.MousePos.y <= wpos.y + wsize.y);
    }

    const bool forceChrome = g_userPaused || g_st.failed || g_st.loading;
    const float a = forceChrome ? 1.f : g_controlsAlpha;
    const bool drawChrome = a > 0.02f;
    const bool interactChrome = a > 0.45f; // ignore tiny fade clicks

    if (drawChrome) {
        auto withA = [](ImU32 c, float f) -> ImU32 {
            int aa = (int)(((c >> 24) & 0xFF) * f);
            return (c & 0x00FFFFFF) | ((ImU32)std::clamp(aa, 0, 255) << 24);
        };
        dl->AddRectFilledMultiColor(wpos, ImVec2(wpos.x + wsize.x, wpos.y + 110),
                                    withA(IM_COL32(0, 0, 0, 200), a), withA(IM_COL32(0, 0, 0, 200), a),
                                    withA(IM_COL32(0, 0, 0, 0), a), withA(IM_COL32(0, 0, 0, 0), a));
        dl->AddText(G.sb26, 22, ImVec2(wpos.x + 28, wpos.y + 28), withA(IM_COL32(255, 255, 255, 255), a), g_st.title.c_str());
        if (interactChrome) {
            ImGui::SetCursorScreenPos(ImVec2(wpos.x + wsize.x - 56, wpos.y + 22));
            if (ImGui::InvisibleButton("##pclose", ImVec2(36, 36))) {
                close();
                ImGui::End();
                ImGui::PopStyleColor();
                ImGui::PopStyleVar();
                return false;
            }
        }
        svgicon::draw(dl, svgicon::Close, ImVec2(wpos.x + wsize.x - 38, wpos.y + 40), 18, withA(IM_COL32(255, 255, 255, 230), a));

        dl->AddRectFilledMultiColor(ImVec2(wpos.x, wpos.y + wsize.y - 160), ImVec2(wpos.x + wsize.x, wpos.y + wsize.y),
                                    withA(IM_COL32(0, 0, 0, 0), a), withA(IM_COL32(0, 0, 0, 0), a),
                                    withA(IM_COL32(0, 0, 0, 230), a), withA(IM_COL32(0, 0, 0, 230), a));
        float scrubY = wpos.y + wsize.y - 78, x0 = wpos.x + 28, x1 = wpos.x + wsize.x - 28;
        bool scrubAct = false, scrubHov = false;
        if (interactChrome) {
            ImGui::SetCursorScreenPos(ImVec2(x0, scrubY - 8));
            ImGui::InvisibleButton("##scrub", ImVec2(x1 - x0, 20));
            scrubAct = ImGui::IsItemActive();
            scrubHov = ImGui::IsItemHovered();
            if (scrubAct && g_st.durationMs > 0)
                setPosition(std::clamp((io.MousePos.x - x0) / (x1 - x0), 0.f, 1.f));
        }
        dl->AddRectFilled(ImVec2(x0, scrubY), ImVec2(x1, scrubY + 4), withA(IM_COL32(255, 255, 255, 40), a), 2);
        float played = x0 + (float)g_st.position * (x1 - x0);
        dl->AddRectFilled(ImVec2(x0, scrubY), ImVec2(played, scrubY + 4), withA(IM_COL32(0, 164, 220, 255), a), 2);
        if (scrubHov || scrubAct) dl->AddCircleFilled(ImVec2(played, scrubY + 2), 7, withA(IM_COL32(255, 255, 255, 255), a));
        auto t0 = fmtTime(g_st.timeMs), t1 = fmtTime(g_st.durationMs);
        dl->AddText(G.r14, 13, ImVec2(x0, scrubY + 12), withA(IM_COL32(220, 220, 220, 255), a), t0.c_str());
        ImVec2 t1s = G.r14->CalcTextSizeA(13, FLT_MAX, 0, t1.c_str());
        dl->AddText(G.r14, 13, ImVec2(x1 - t1s.x, scrubY + 12), withA(IM_COL32(220, 220, 220, 255), a), t1.c_str());

        float cy = wpos.y + wsize.y - 42, cx = wpos.x + 48;
        auto hit = [&](const char* id, float x, float y, float s, auto onClick) {
            if (!interactChrome) return;
            ImGui::SetCursorScreenPos(ImVec2(x - s * 0.5f, y - s * 0.5f));
            if (ImGui::InvisibleButton(id, ImVec2(s, s))) onClick();
        };
        hit("##play", cx, cy, 36, [] { togglePause(); });
        if (g_userPaused || g_st.paused || g_st.loading)
            svgicon::draw(dl, svgicon::Play, ImVec2(cx, cy), 18, withA(IM_COL32(255, 255, 255, 255), a));
        else {
            dl->AddRectFilled(ImVec2(cx - 7, cy - 9), ImVec2(cx - 2, cy + 9), withA(IM_COL32(255, 255, 255, 255), a));
            dl->AddRectFilled(ImVec2(cx + 2, cy - 9), ImVec2(cx + 7, cy + 9), withA(IM_COL32(255, 255, 255, 255), a));
        }
        cx += 50;
        hit("##b10", cx, cy, 32, [] { seekRelative(-10); });
        svgicon::draw(dl, svgicon::ChevronLeft, ImVec2(cx, cy), 16, withA(IM_COL32(230, 230, 230, 230), a));
        cx += 42;
        hit("##f10", cx, cy, 32, [] { seekRelative(10); });
        svgicon::draw(dl, svgicon::ChevronRight, ImVec2(cx, cy), 16, withA(IM_COL32(230, 230, 230, 230), a));
        cx += 50;
        hit("##mute", cx, cy, 32, [] { toggleMute(); });
        dl->AddText(G.sb18, 12, ImVec2(cx - 10, cy - 8), withA(IM_COL32(230, 230, 230, 230), a), g_st.muted ? "MUT" : "VOL");
        cx += 28;
        if (interactChrome) {
            ImGui::SetCursorScreenPos(ImVec2(cx, cy - 10));
            ImGui::PushItemWidth(110);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1, 1, 1, 0.12f * a));
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0, 0.64f, 0.86f, a));
            int vol = g_st.volume;
            if (ImGui::SliderInt("##vol", &vol, 0, 100, "")) setVolume(vol);
            ImGui::PopStyleColor(2);
            ImGui::PopItemWidth();
        }

        float rx = wpos.x + wsize.x - 40;
        hit("##fs", rx, cy, 36, [] { toggleFullscreen(); });
        svgicon::draw(dl, svgicon::External, ImVec2(rx, cy), 16, withA(IM_COL32(230, 230, 230, 230), a));
        rx -= 48;
        hit("##set", rx, cy, 36, [] {
            g_showSettings = !g_showSettings; g_showSubsMenu = g_showAudioMenu = false;
            g_showControls = true; g_idleTimer = 0;
        });
        svgicon::draw(dl, svgicon::Cog, ImVec2(rx, cy), 16, withA(IM_COL32(230, 230, 230, 230), a));
        rx -= 48;
        hit("##cc", rx, cy, 36, [] {
            g_showSubsMenu = !g_showSubsMenu; g_showAudioMenu = g_showSettings = false;
            g_showControls = true; g_idleTimer = 0;
        });
        dl->AddRectFilled(ImVec2(rx - 16, cy - 10), ImVec2(rx + 16, cy + 10),
                          withA(g_st.subtitleId >= 0 ? IM_COL32(0, 164, 220, 220) : IM_COL32(255, 255, 255, 30), a), 4);
        dl->AddText(G.sb18, 12, ImVec2(rx - 10, cy - 8), withA(IM_COL32(255, 255, 255, 255), a), "CC");
        rx -= 48;
        hit("##aud", rx, cy, 36, [] {
            g_showAudioMenu = !g_showAudioMenu; g_showSubsMenu = g_showSettings = false;
            g_showControls = true; g_idleTimer = 0;
        });
        svgicon::draw(dl, svgicon::Users, ImVec2(rx, cy), 15, withA(IM_COL32(230, 230, 230, 230), a));
        rx -= 52;
        hit("##nerd", rx, cy, 36, [] { toggleStats(); });
        dl->AddText(G.sb18, 11, ImVec2(rx - 14, cy - 8),
                    withA(g_showStats ? IM_COL32(0, 164, 220, 255) : IM_COL32(230, 230, 230, 230), a), "SFN");
    }

    if (g_showStats) {
        float panelW = 360.f, panelH = 268.f;
        ImVec2 p0(wpos.x + 16, wpos.y + 16);
        ImVec2 p1(p0.x + panelW, p0.y + panelH);
        dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 200), 16);
        dl->AddRect(p0, p1, IM_COL32(0, 164, 220, 160), 16, 0, 1.0f);
        float y = p0.y + 12;
        auto line = [&](const char* k, const char* v) {
            dl->AddText(G.r14, 13, ImVec2(p0.x + 14, y), IM_COL32(156, 163, 175, 255), k);
            dl->AddText(G.r14, 13, ImVec2(p0.x + 150, y), IM_COL32(243, 244, 246, 255), v);
            y += 18;
        };
        dl->AddText(G.sb18, 14, ImVec2(p0.x + 14, y), IM_COL32(0, 164, 220, 255), "Stats for nerds");
        y += 22;
        char b[160];
        std::string file = fs::path(g_st.path).filename().string();
        if (file.size() > 36) file = file.substr(0, 34) + "..";
        line("Video ID / file", file.c_str());
        std::snprintf(b, sizeof(b), "%dx%d / %dx%d",
                      (int)wsize.x, (int)wsize.y,
                      g_st.videoW > 0 ? g_st.videoW : (int)g_pixW,
                      g_st.videoH > 0 ? g_st.videoH : (int)g_pixH);
        line("Viewport / Video", b);
        std::snprintf(b, sizeof(b), "%ux%u", g_srcW ? g_srcW : g_pixW, g_srcH ? g_srcH : g_pixH);
        line("Current / Optimal Res", b);
        float fpsLib = (p_libvlc_media_player_get_fps && g_mp) ? p_libvlc_media_player_get_fps(g_mp) : 0.f;
        std::snprintf(b, sizeof(b), "%.1f / %.1f", g_instantFps, fpsLib > 0 ? fpsLib : g_instantFps);
        line("Frames per second", b);
        float br = g_vlcStats.f_demux_bitrate > 0 ? g_vlcStats.f_demux_bitrate : g_vlcStats.f_input_bitrate;
        double mbps = br * 8.0 / 1e6;
        if (mbps <= 0 && g_instantFps > 0 && g_st.videoW > 0)
            mbps = (g_st.videoW * g_st.videoH * 4.0 * g_instantFps * 8.0) / 1e6 * 0.15;
        std::snprintf(b, sizeof(b), "%.2f Mbps", mbps);
        line("Current / Optimal Bitrate", b);
        int dropped = g_vlcStats.i_lost_pictures;
        int decoded = g_statsDecodedVideo0 >= 0
            ? std::max(0, g_vlcStats.i_decoded_video - g_statsDecodedVideo0) : g_vlcStats.i_decoded_video;
        int shown = g_statsDisplayed0 >= 0
            ? std::max(0, g_vlcStats.i_displayed_pictures - g_statsDisplayed0) : (int)g_framesDisplayed.load();
        std::snprintf(b, sizeof(b), "%d / %d", dropped, shown > 0 ? shown : decoded);
        line("Dropped / Total Frames", b);
        std::snprintf(b, sizeof(b), "%d%% %s", g_st.volume, g_st.muted ? "(muted)" : "");
        line("Volume / Normalized", b);
        const char* aud = "n/a";
        for (auto& t : g_st.audioTracks) if (t.id == g_st.audioId) { aud = t.name.c_str(); break; }
        line("Audio track", aud);
        std::snprintf(b, sizeof(b), "%.1fs / %.1fs", g_st.timeMs / 1000.0, g_st.durationMs / 1000.0);
        line("Buffer / Duration", b);
        std::snprintf(b, sizeof(b), "%s  ·  Ctrl+Shift+I", g_st.playing ? "playing" : "paused");
        line("Mystery Text", b);
    }

    auto popup = [&](const char* title, std::vector<Track>& tracks, int cur, bool off, bool& flag, void (*apply)(int)) {
        if (!flag) return;
        float h = 48.f + 34.f * ((int)tracks.size() + (off ? 1 : 0));
        ImGui::SetNextWindowPos(ImVec2(wpos.x + wsize.x - 304, wpos.y + wsize.y - 170 - h));
        ImGui::SetNextWindowSize(ImVec2(280, h));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.11f, 0.11f, 0.13f, 0.97f));
        ImGui::Begin(title, &flag, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar);
        ImGui::TextUnformatted(title);
        ImGui::Separator();
        if (off && ImGui::Selectable(i18n::tr("player.off"), cur < 0)) { apply(-1); flag = false; }
        for (auto& t : tracks)
            if (ImGui::Selectable(t.name.c_str(), t.id == cur)) { apply(t.id); flag = false; }
        ImGui::End();
        ImGui::PopStyleColor();
    };
    popup(i18n::tr("player.subtitles"), g_st.subtitles, g_st.subtitleId, true, g_showSubsMenu, [](int id) { setSubtitle(id); });
    popup(i18n::tr("player.audio_track"), g_st.audioTracks, g_st.audioId, false, g_showAudioMenu, [](int id) { setAudioTrack(id); });

    if (g_showSettings) {
        ImGui::SetNextWindowPos(ImVec2(wpos.x + wsize.x - 320, wpos.y + wsize.y - 340));
        ImGui::SetNextWindowSize(ImVec2(300, 260));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.11f, 0.11f, 0.13f, 0.97f));
        ImGui::Begin("##pset", &g_showSettings, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar);
        ImGui::TextUnformatted(i18n::tr("player.settings"));
        ImGui::Separator();
        if (ImGui::Button(g_st.fullscreen ? i18n::tr("player.window") : i18n::tr("player.fullscreen"), ImVec2(-1, 32))) toggleFullscreen();
        if (ImGui::Button(i18n::tr("player.subtitles"), ImVec2(-1, 32))) { g_showSettings = false; g_showSubsMenu = true; }
        if (ImGui::Button(i18n::tr("player.audio"), ImVec2(-1, 32))) { g_showSettings = false; g_showAudioMenu = true; }
        if (ImGui::Button(g_showStats ? i18n::tr("player.hide_stats") : i18n::tr("player.stats"), ImVec2(-1, 32))) toggleStats();
        ImGui::TextWrapped("%s", i18n::tr("player.shortcuts"));
        ImGui::End();
        ImGui::PopStyleColor();
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    return true;
}

} // namespace player
