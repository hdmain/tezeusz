#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#endif
#include <GLFW/glfw3.h>
#ifdef _WIN32
#include <GLFW/glfw3native.h>
#endif
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include "app.hpp"
#include "widgets.hpp"
#include "imgcache.hpp"
#include "svgicons.hpp"
#include "util.hpp"
#include "ytplayer.hpp"
#include "stack.hpp"
#include "core.hpp"
#include "localdb.hpp"
#include "platform.hpp"
#include "player.hpp"
#include "subs.hpp"
#include "i18n.hpp"
#include "gl_compat.hpp"
#include "tray.hpp"
#include "updater.hpp"

#ifdef _WIN32
#include <windows.h>
#endif
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>

static const float SIDEBAR_W = 256.0f;

static GLuint g_logoTex = 0;
static int g_logoW = 0, g_logoH = 0;
static void* g_mainHwnd = nullptr;

void* appMainHwnd() { return g_mainHwnd; }
void setAppMainHwnd(void* hwnd) { g_mainHwnd = hwnd; }

// ------------------------------------------------------------------ sidebar

enum SideItem { SI_DISCOVER, SI_MOVIES, SI_TV, SI_REQUESTS, SI_LIBRARY, SI_BLOCKLIST, SI_ISSUES, SI_USERS, SI_SETTINGS, SI_COUNT };
static const char* sideLabel(int i) {
    switch (i) {
    case SI_DISCOVER: return i18n::tr("nav.discover");
    case SI_MOVIES: return i18n::tr("nav.movies");
    case SI_TV: return i18n::tr("nav.tv");
    case SI_REQUESTS: return i18n::tr("nav.requests");
    case SI_LIBRARY: return i18n::tr("nav.library");
    case SI_BLOCKLIST: return i18n::tr("nav.blocklist");
    case SI_ISSUES: return i18n::tr("nav.issues");
    case SI_USERS: return i18n::tr("nav.users");
    case SI_SETTINGS: return i18n::tr("nav.settings");
    default: return "";
    }
}

static void sidebarIcon(int idx, ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    switch (idx) {
        case SI_DISCOVER: icons::sparkles(dl, c, s, col); break;
        case SI_MOVIES: icons::film(dl, c, s, col); break;
        case SI_TV: icons::tv(dl, c, s, col); break;
        case SI_REQUESTS: icons::clock(dl, c, s, col); break;
        case SI_LIBRARY: icons::play(dl, c, s, col); break;
        case SI_BLOCKLIST: icons::eyeslash(dl, c, s, col); break;
        case SI_ISSUES: icons::exclaim(dl, c, s, col); break;
        case SI_USERS: icons::users(dl, c, s, col); break;
        case SI_SETTINGS: icons::cog(dl, c, s, col); break;
    }
}

static float animToward(float cur, float target, float speed) {
    float dt = ImGui::GetIO().DeltaTime;
    float t = 1.0f - expf(-speed * dt);
    return cur + (target - cur) * t;
}

static void renderSidebar(App& a) {
    // Keep sidebar above content (seerr: fixed w-64 column). Do NOT use
    // NoBringToFrontOnFocus — otherwise focused content redraws over the nav.
    ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                          ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoBackground;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(SIDEBAR_W, ImGui::GetIO().DisplaySize.y));
    ImGui::Begin("##sidebar", nullptr, wf);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wpos = ImGui::GetWindowPos();
    float H = ImGui::GetWindowHeight();

    dl->AddRectFilled(wpos, ImVec2(wpos.x + SIDEBAR_W, wpos.y + H), theme::c("#1f2937"));

    // logo (seerr: relative block h-24 w-64 p-4, object-contain)
    if (g_logoTex && g_logoW && g_logoH) {
        float maxW = SIDEBAR_W - 48, maxH = 66;
        float sc = std::min(maxW / (float)g_logoW, maxH / (float)g_logoH);
        float iw = g_logoW * sc, ih = g_logoH * sc;
        ImVec2 p(wpos.x + 24 + (maxW - iw) / 2, wpos.y + 20 + (maxH - ih) / 2);
        dl->AddImage((ImTextureID)(intptr_t)g_logoTex, p, ImVec2(p.x + iw, p.y + ih));
    }

    float y = wpos.y + 122;
    int active = -1;
    switch (a.page) {
        case Page::Discover: case Page::TrendingPage: active = SI_DISCOVER; break;
        case Page::Movies: active = SI_MOVIES; break;
        case Page::Tv: active = SI_TV; break;
        case Page::Genre: active = a.genre.tv ? SI_TV : SI_MOVIES; break;
        case Page::Search: active = -1; break;
        case Page::MovieDetails: case Page::TvDetails: active = a.detailType == MediaType::TV ? SI_TV : SI_MOVIES; break;
        case Page::Requests: active = SI_REQUESTS; break;
        case Page::Library: active = SI_LIBRARY; break;
        case Page::Blocklist: active = SI_BLOCKLIST; break;
        case Page::Issues: active = SI_ISSUES; break;
        case Page::Users: active = SI_USERS; break;
        case Page::Settings: active = SI_SETTINGS; break;
        case Page::Stub: active = -1; break;
    }

    static float hoverAmt[SI_COUNT] = {};
    static float activeAmt[SI_COUNT] = {};

    for (int i = 0; i < SI_COUNT; i++) {
        ImVec2 p0(wpos.x + 16, y);
        ImVec2 p1(wpos.x + SIDEBAR_W - 16, y + 40);
        ImGui::SetCursorScreenPos(p0);
        ImGui::PushID(i);
        bool cl = ImGui::InvisibleButton("##si", ImVec2(p1.x - p0.x, 40));
        bool hv = ImGui::IsItemHovered();
        ImGui::PopID();
        bool act = i == active;

        hoverAmt[i] = animToward(hoverAmt[i], hv ? 1.0f : 0.0f, 14.0f);
        activeAmt[i] = animToward(activeAmt[i], act ? 1.0f : 0.0f, 12.0f);
        float ha = hoverAmt[i], aa = activeAmt[i];

        if (aa > 0.01f) {
            // Rounded solid (MultiColor can't round) — soft indigo pill
            ImU32 fill = theme::withA(theme::c(hv ? "#7c5cf3" : "#6366f1"), aa);
            dl->AddRectFilled(p0, p1, fill, 14.0f);
            if (aa > 0.5f)
                dl->AddRect(p0, p1, theme::withA(theme::c("#a78bfa"), aa * 0.35f), 14.0f, 0, 1.0f);
        } else if (ha > 0.01f) {
            dl->AddRectFilled(p0, p1, theme::withA(theme::c("#374151"), ha), 14.0f);
        }

        float iconS = 20.0f + 2.0f * ha + 1.0f * aa;
        float iconX = p0.x + 24.0f + 2.0f * ha;
        int aIcon = (int)(180 + 75 * std::max(ha, aa));
        sidebarIcon(i, dl, ImVec2(iconX, p0.y + 20), iconS, IM_COL32(255, 255, 255, aIcon));

        float textA = 0.72f + 0.28f * std::max(ha, aa);
        dl->AddText(G.m16, 15.5f,
                    ImVec2(p0.x + 44.0f + 1.5f * ha,
                           p0.y + (40 - G.m16->CalcTextSizeA(15.5f, FLT_MAX, 0, sideLabel(i)).y) / 2),
                    IM_COL32(255, 255, 255, (int)(textA * 255)), sideLabel(i));
        y += 56;

        if (cl) {
            switch (i) {
                case SI_DISCOVER: a.navigate(Page::Discover); break;
                case SI_MOVIES: a.navigate(Page::Movies); break;
                case SI_TV: a.navigate(Page::Tv); break;
                case SI_REQUESTS: a.navigate(Page::Requests); break;
                case SI_LIBRARY: a.navigate(Page::Library); break;
                case SI_BLOCKLIST: a.navigate(Page::Blocklist); break;
                case SI_ISSUES: a.navigate(Page::Issues); break;
                case SI_USERS: a.navigate(Page::Users); break;
                case SI_SETTINGS: a.navigate(Page::Settings); break;
                default: break;
            }
        }
    }

    // stats box at bottom (RAM / CPU / peers) + user avatar
    {
        auto st = core::stats();
        float boxTop = wpos.y + H - 108;
        // avatar row
        {
            ImVec2 c(wpos.x + 36, boxTop + 14);
            float r = 14;
            dl->AddCircleFilled(c, r, theme::c("#6366f1"));
            char ini[2] = {'T', 0};
            auto& u = localdb::user();
            if (!u.displayName.empty())
                ini[0] = (char)std::toupper((unsigned char)u.displayName[0]);
            dl->AddText(G.sb18, 13, ImVec2(c.x - 4, c.y - 7), IM_COL32(255, 255, 255, 255), ini);
            dl->AddCircle(c, r + 2, theme::c("#4f46e5"), 24, 1.0f);
            const char* name = u.displayName.empty() ? "Konto" : u.displayName.c_str();
            dl->AddText(G.m16, 13, ImVec2(c.x + 22, c.y - 8), theme::c("#e5e7eb"), name);
            ImGui::SetCursorScreenPos(ImVec2(wpos.x + 16, boxTop));
            if (ImGui::InvisibleButton("##user", ImVec2(SIDEBAR_W - 32, 28)))
                a.navigate(Page::Users);
        }

        ImVec2 p0(wpos.x + 16, boxTop + 36);
        ImVec2 p1(wpos.x + SIDEBAR_W - 16, wpos.y + H - 16);
        ImGui::SetCursorScreenPos(p0);
        ImGui::PushID("ver");
        bool cl = ImGui::InvisibleButton("##ver", ImVec2(p1.x - p0.x, p1.y - p0.y));
        bool hv = ImGui::IsItemHovered();
        ImGui::PopID();
        static float verH = 0;
        verH = animToward(verH, hv ? 1.0f : 0.0f, 12.0f);
        dl->AddRectFilled(p0, p1, theme::withA(theme::c("#374151"), 0.70f + 0.30f * verH), 14.0f);

        char line1[96], line2[96];
        std::snprintf(line1, sizeof(line1), "RAM %.0f MB  ·  CPU %.0f%%", st.ramMb, st.cpuPct);
        std::snprintf(line2, sizeof(line2), "Peery %d  ·  joby %d/%d",
                      st.peers, st.jobsRunning, st.jobsRunning + st.jobsPending);
        dl->AddText(G.r14, 12, ImVec2(p0.x + 10, p0.y + 10), theme::c("#e5e7eb"), line1);
        dl->AddText(G.r14, 11, ImVec2(p0.x + 10, p0.y + 30), theme::c("#9ca3af"), line2);
        if (cl) platform::openUrl("https://github.com/seerr-team/seerr");
    }
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImGui::End();
}

// ------------------------------------------------------------------ page dispatcher

static void renderContentPage(App& a) {
    switch (a.page) {
        case Page::Discover: renderDiscover(); break;
        case Page::Movies: renderDiscoverGrid(false); break;
        case Page::Tv: renderDiscoverGrid(true); break;
        case Page::TrendingPage: renderTrendingPage(); break;
        case Page::Genre: renderGenrePage(); break;
        case Page::Search: renderSearch(); break;
        case Page::MovieDetails: renderDetails(MediaType::Movie, a.detailId); break;
        case Page::TvDetails: renderDetails(MediaType::TV, a.detailId); break;
        case Page::Requests: renderRequests(); break;
        case Page::Library: renderLibrary(); break;
        case Page::Blocklist: renderBlocklist(); break;
        case Page::Issues: renderIssues(); break;
        case Page::Users: renderUsers(); break;
        case Page::Settings: renderSettings(); break;
        case Page::Stub: renderStub(i18n::tr("nav.coming_soon")); break;
    }
}

// ------------------------------------------------------------------ main

static void glfwErr(int code, const char* desc) {
    std::fprintf(stderr, "seerr glfw: %d %s\n", code, desc ? desc : "");
}

#ifdef _WIN32
static void winFatal(const char* title, const char* msg) {
    MessageBoxA(nullptr, msg, title, MB_OK | MB_ICONERROR);
}
#else
static void winFatal(const char*, const char*) {}
#endif

int main() {
    std::fprintf(stderr, "seerr: starting\n");
    std::fflush(stderr);

    glfwSetErrorCallback(glfwErr);

#if !defined(_WIN32) && !defined(__APPLE__)
    // Prefer X11 whenever DISPLAY is set. WSLg / Ubuntu often export both
    // DISPLAY and WAYLAND_DISPLAY; GLFW 3.3 then picks Wayland and commonly
    // segfaults on GL context creation. GLFW 3.4+ can take GLFW_PLATFORM_X11.
    if (const char* dpy = std::getenv("DISPLAY"); dpy && dpy[0]) {
        unsetenv("WAYLAND_DISPLAY");
    }
#  if defined(GLFW_PLATFORM) && defined(GLFW_PLATFORM_X11)
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
#  endif
#endif

    if (!glfwInit()) {
#if !defined(_WIN32) && !defined(__APPLE__) && defined(GLFW_PLATFORM) && defined(GLFW_ANY_PLATFORM)
        glfwInitHint(GLFW_PLATFORM, GLFW_ANY_PLATFORM);
        if (!glfwInit()) {
            winFatal("Seerr", "Failed to initialize GLFW / OpenGL.");
            return 1;
        }
#else
        winFatal("Seerr", "Failed to initialize GLFW / OpenGL.\n\n"
                          "Install or update your GPU drivers, then try again.");
        return 1;
#endif
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, 1);
#endif
    glfwWindowHint(GLFW_SAMPLES, 0); // UI doesn't need MSAA — saves GPU fillrate

    GLFWwindow* win = glfwCreateWindow(1500, 900, "Seerr C++ - Media Discovery", nullptr, nullptr);
    if (!win) {
        // Some drivers reject Core 3.3 — fall back to any available OpenGL.
        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_SAMPLES, 0);
        win = glfwCreateWindow(1500, 900, "Seerr C++ - Media Discovery", nullptr, nullptr);
    }
    if (!win) {
        glfwTerminate();
        winFatal("Seerr", "Could not create the OpenGL window.\n\n"
                          "Update GPU drivers or try another GPU.");
        return 1;
    }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
#ifdef _WIN32
    setAppMainHwnd(glfwGetWin32Window(win));
#else
    setAppMainHwnd(nullptr);
#endif
    platform::applyDarkTitlebar(win);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    // ImGui's embedded GLVND/EGL loader must run before we resolve our own entry points.
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    if (!ImGui_ImplOpenGL3_Init("#version 330")) {
        std::fprintf(stderr, "seerr: OpenGL renderer init failed\n");
        winFatal("Seerr", "OpenGL renderer init failed.");
        return 1;
    }
    std::fprintf(stderr, "seerr: imgui GL ok\n");

    if (!seerrLoadGL()) {
        std::fprintf(stderr, "seerr: failed to load OpenGL functions\n");
        winFatal("Seerr", "Failed to load OpenGL functions.");
        glfwDestroyWindow(win);
        glfwTerminate();
        return 1;
    }
    std::fprintf(stderr, "seerr: GL entry points ok\n");
    std::fflush(stderr);

    initFonts();
    i18n::init();
    ImageCache::instance().init();
    svgicon::init();
    core::init(3);
    stack::init();
    subs::init();
    localdb::init();
    localdb::loadWatchlist(app().watchlist);
    updater::init();

    // assets
    {
        std::string dir = util::assetDir() + "/";
        g_logoTex = ImageCache::instance().loadLocal(dir + "logo_full.png", &g_logoW, &g_logoH);
        tray::init(win, dir + "icon.png");
#ifdef _WIN32
        int iw = 0, ih = 0;
        GLuint icon = ImageCache::instance().loadLocal(dir + "icon.png", &iw, &ih);
        if (icon && iw && ih) {
            glBindTexture(GL_TEXTURE_2D, icon);
            std::vector<unsigned char> px((size_t)iw * ih * 4);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            glBindTexture(GL_TEXTURE_2D, 0);
            GLFWimage img; img.width = iw; img.height = ih; img.pixels = px.data();
            glfwSetWindowIcon(win, 1, &img);
        }
#endif
    }

    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowPadding = ImVec2(0, 0);
    st.FramePadding = ImVec2(12, 8);
    st.ItemSpacing = ImVec2(10, 8);
    st.WindowRounding = 14.0f;
    st.ChildRounding = 14.0f;
    st.FrameRounding = 12.0f;
    st.PopupRounding = 14.0f;
    st.ScrollbarRounding = 12.0f;
    st.GrabRounding = 12.0f;
    st.TabRounding = 12.0f;
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_FrameBg] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.12f, 0.18f, 0.98f);
    st.Colors[ImGuiCol_Border] = ImVec4(0.29f, 0.33f, 0.39f, 0.55f);
    st.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.067f, 0.094f, 0.157f, 1.0f);      // #111827
    st.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.294f, 0.337f, 0.392f, 0.75f);   // #4b5563
    st.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.42f, 0.46f, 0.52f, 0.95f);
    st.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.5f, 0.54f, 0.6f, 1.0f);
    st.ScrollbarSize = 10;
    st.GrabMinSize = 14;

    player::bindWindow(win);

    App& a = app();

    // Graceful quit: heavy work on a background thread so the UI spinner stays live
    static std::atomic<bool> quitting{false};
    static std::atomic<bool> quitBgDone{false};
    static std::atomic<bool> quitStarted{false};
    static std::mutex quitStatusMu;
    static std::string quitStatus;
    if (quitStatus.empty()) quitStatus = i18n::tr("quit.closing");
    static std::thread quitThread;

    auto setQuitStatus = [](const char* s) {
        std::lock_guard<std::mutex> lk(quitStatusMu);
        quitStatus = s;
    };
    auto getQuitStatus = []() -> std::string {
        std::lock_guard<std::mutex> lk(quitStatusMu);
        return quitStatus;
    };

    glfwSetWindowCloseCallback(win, [](GLFWwindow* w) {
        // Close → tray (downloads keep running). Quit from the tray menu.
        glfwSetWindowShouldClose(w, GLFW_FALSE);
        tray::hideToTray();
    });

    while (true) {
        tray::tick();
        if (tray::consumeQuitRequest() && !quitting.load())
            quitting.store(true);
        if (updater::wantsQuitForApply() && !quitting.load())
            quitting.store(true);
        if (tray::consumeShowRequest())
            tray::showFromTray();

        if (quitting.load())
            glfwWaitEventsTimeout(1.0 / 60.0);
        else if (tray::isHidden())
            glfwWaitEventsTimeout(0.25);
        else if (!glfwGetWindowAttrib(win, GLFW_FOCUSED))
            glfwWaitEventsTimeout(1.0 / 30.0);
        else
            // Visible + focused: let vsync (SwapInterval) pace the loop.
            // Never throttle to 20/30fps while images load — that made Discover feel stuck.
            glfwPollEvents();

        if (quitting.load() && tray::isHidden())
            tray::showFromTray();

        if (!quitting.load()) {
            updater::tick();
            ImageCache::instance().pump();
            core::tick();
            stack::tick();
            subs::tick();
            ytplayer::tick();
            player::tick();
        }

        int fbw, fbh;
        glfwGetFramebufferSize(win, &fbw, &fbh);
        glViewport(0, 0, fbw, fbh);
        glClearColor(0.067f, 0.094f, 0.157f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGuiIO& io = ImGui::GetIO();

        if (quitting.load()) {
            // Kick off: GL-safe player close on UI thread, then background shutdown
            if (!quitStarted.exchange(true)) {
                setQuitStatus(i18n::tr("quit.torrents"));
                try { player::close(); } catch (...) {}
                setQuitStatus(i18n::tr("quit.torrents"));
                quitThread = std::thread([setQuitStatus]() {
                    try {
                        setQuitStatus(i18n::tr("quit.torrents"));
                        stack::shutdown();
                        setQuitStatus(i18n::tr("quit.subs"));
                        try { subs::shutdown(); } catch (...) {}
                        setQuitStatus(i18n::tr("quit.jobs"));
                        core::shutdown();
                        setQuitStatus(i18n::tr("quit.cache"));
                        ImageCache::instance().shutdown();
                        setQuitStatus(i18n::tr("quit.finishing"));
                    } catch (...) {
                        setQuitStatus(i18n::tr("quit.error"));
                    }
                    quitBgDone.store(true);
                });
            }

            ImDrawList* dl = ImGui::GetForegroundDrawList();
            dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(17, 24, 39, 240));
            const char* title = i18n::tr("quit.title");
            ImVec2 ts = G.sb26 ? G.sb26->CalcTextSizeA(22, FLT_MAX, 0, title)
                               : ImGui::CalcTextSize(title);
            ImVec2 tp((io.DisplaySize.x - ts.x) * 0.5f, io.DisplaySize.y * 0.42f);
            if (G.sb26) dl->AddText(G.sb26, 22, tp, IM_COL32(229, 231, 235, 255), title);
            else dl->AddText(tp, IM_COL32(229, 231, 235, 255), title);

            std::string status = getQuitStatus();
            ImVec2 ss = G.r16 ? G.r16->CalcTextSizeA(15, FLT_MAX, 0, status.c_str())
                              : ImGui::CalcTextSize(status.c_str());
            ImVec2 sp((io.DisplaySize.x - ss.x) * 0.5f, tp.y + 40);
            if (G.r16) dl->AddText(G.r16, 15, sp, IM_COL32(156, 163, 175, 255), status.c_str());
            else dl->AddText(sp, IM_COL32(156, 163, 175, 255), status.c_str());

            ImVec2 c(io.DisplaySize.x * 0.5f, sp.y + 48);
            w::orbitSpinner(dl, c, 14.f, 3.2f);

            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(win);

            if (quitBgDone.load()) {
                if (quitThread.joinable()) quitThread.join();
                setQuitStatus(i18n::tr("quit.ui"));
                try { updater::shutdown(); } catch (...) {}
                try { tray::shutdown(); } catch (...) {}
                try { svgicon::shutdown(); } catch (...) {}
                ImGui_ImplOpenGL3_Shutdown();
                ImGui_ImplGlfw_Shutdown();
                ImGui::DestroyContext();
                glfwDestroyWindow(win);
                glfwTerminate();
                return 0;
            }
            continue;
        }

        // F5 = hard reload (clear failed images + refetch data), like Ctrl+F5 in browser
        if (ImGui::IsKeyPressed(ImGuiKey_F5) && !player::isOpen()) {
            ImageCache::instance().purgeFailed();
            a.reloadSeq++;
        }
        // Ctrl+Q = quit (window X only hides to tray)
        if (ImGui::IsKeyPressed(ImGuiKey_Q) && (io.KeyCtrl || io.KeySuper) && !player::isOpen())
            quitting.store(true);

        if (player::isOpen()) {
            player::render();
        } else {
        // content column only (seerr: lg:ml-64) — never draw under the fixed sidebar
        ImGuiWindowFlags cwf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground;
        ImGui::SetNextWindowPos(ImVec2(SIDEBAR_W, 0));
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x - SIDEBAR_W, io.DisplaySize.y));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26, 0));
        ImGui::Begin("##content", nullptr, cwf);
        // start each page at the top (previously: content appeared scrolled-in on open)
        {
            static Page lastPage = Page::Discover;
            static int lastDetailId = -1;
            if (a.page != lastPage || a.detailId != lastDetailId) {
                ImGui::SetScrollY(0.0f);
                lastPage = a.page; lastDetailId = a.detailId;
            }
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 cpos = ImGui::GetWindowPos();
        float cw = ImGui::GetWindowWidth();
        // top gradient like seerr layout (from-gray-800 to-gray-900)
        {
            float gh = std::min(256.0f, io.DisplaySize.y * 0.55f);
            dl->AddRectFilledMultiColor(cpos, ImVec2(cpos.x + cw, cpos.y + gh),
                                        theme::c("#1f2937"), theme::c("#1f2937"), theme::c("#111827"), theme::c("#111827"));
        }
        float left = ImGui::GetCursorStartPos().x; // WindowPadding.x
        float topPad = 16.0f;

        auto& ap = a;
        bool subpage = ap.page != Page::Discover;
        if (subpage) {
            ImGui::SetCursorPos(ImVec2(left, topPad));
            ImVec2 backMin = ImGui::GetCursorScreenPos();
            if (w::button(dl, "##back", backMin, ImVec2(backMin.x + 84, backMin.y + 26), i18n::tr("nav.back"),
                          theme::c("#111827/01"), theme::c("#1f2937"), theme::c("#374151"), 0,
                          theme::c("#9ca3af"), G.r14, 13, 12))
                ap.back();
            ImGui::SetCursorPos(ImVec2(left, topPad + 36));
        } else {
            ImGui::SetCursorPos(ImVec2(left, topPad));
        }
        renderContentPage(ap);
        ImGui::End();
        ImGui::PopStyleVar();

        renderSidebar(a);
        renderRequestQualityDialog();
        }

        // Update toast (download / restart)
        {
            auto ust = updater::state();
            if (ust == updater::State::Downloading || ust == updater::State::Ready ||
                ust == updater::State::Applying) {
                ImDrawList* fdl = ImGui::GetForegroundDrawList();
                std::string msg = updater::statusText();
                if (ust == updater::State::Downloading)
                    msg += "  " + std::to_string(updater::downloadPercent()) + "%";
                ImVec2 ts = ImGui::CalcTextSize(msg.c_str());
                ImVec2 pad(14, 10);
                ImVec2 br(io.DisplaySize.x - 16, io.DisplaySize.y - 16);
                ImVec2 tl(br.x - ts.x - pad.x * 2, br.y - ts.y - pad.y * 2);
                fdl->AddRectFilled(tl, br, IM_COL32(17, 24, 39, 230), 10.f);
                fdl->AddRect(tl, br, IM_COL32(0, 164, 220, 180), 10.f);
                fdl->AddText(ImVec2(tl.x + pad.x, tl.y + pad.y), IM_COL32(229, 231, 235, 255), msg.c_str());
            }
        }

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }

    return 0;
}
