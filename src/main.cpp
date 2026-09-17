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

#ifdef _WIN32
#include <windows.h>
#endif
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

static const float SIDEBAR_W = 256.0f;
static const float NAVBAR_H = 64.0f;

static GLuint g_logoTex = 0;
static int g_logoW = 0, g_logoH = 0;
static void* g_mainHwnd = nullptr;

void* appMainHwnd() { return g_mainHwnd; }
void setAppMainHwnd(void* hwnd) { g_mainHwnd = hwnd; }

// ------------------------------------------------------------------ sidebar

enum SideItem { SI_DISCOVER, SI_MOVIES, SI_TV, SI_REQUESTS, SI_BLOCKLIST, SI_ISSUES, SI_USERS, SI_SETTINGS, SI_COUNT };
static const char* SIDE_LABELS[SI_COUNT] = {
    "Odkrywaj", "Filmy", "Seriale", "Żądania", "Blokada", "Problemy", "Użytkownicy", "Ustawienia"
};

static void sidebarIcon(int idx, ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    switch (idx) {
        case SI_DISCOVER: icons::sparkles(dl, c, s, col); break;
        case SI_MOVIES: icons::film(dl, c, s, col); break;
        case SI_TV: icons::tv(dl, c, s, col); break;
        case SI_REQUESTS: icons::clock(dl, c, s, col); break;
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
            ImU32 c0 = theme::withA(theme::c("#4f46e5"), aa);
            ImU32 c1 = theme::withA(theme::c("#6a4fe0"), aa);
            ImU32 c2 = theme::withA(theme::c("#9333ea"), aa);
            ImU32 c3 = theme::withA(theme::c("#7a3ce5"), aa);
            if (hv) {
                c0 = theme::withA(theme::c("#6366f1"), aa);
                c1 = theme::withA(theme::c("#7c5cf3"), aa);
                c2 = theme::withA(theme::c("#a855f7"), aa);
                c3 = theme::withA(theme::c("#8b4bf4"), aa);
            }
            dl->AddRectFilledMultiColor(p0, p1, c0, c1, c2, c3);
        } else if (ha > 0.01f) {
            dl->AddRectFilled(p0, p1, theme::withA(theme::c("#374151"), ha), 6);
        }

        float iconS = 20.0f + 2.0f * ha + 1.0f * aa;
        float iconX = p0.x + 24.0f + 2.0f * ha;
        int aIcon = (int)(180 + 75 * std::max(ha, aa));
        sidebarIcon(i, dl, ImVec2(iconX, p0.y + 20), iconS, IM_COL32(255, 255, 255, aIcon));

        float textA = 0.72f + 0.28f * std::max(ha, aa);
        dl->AddText(G.m16, 15.5f,
                    ImVec2(p0.x + 44.0f + 1.5f * ha,
                           p0.y + (40 - G.m16->CalcTextSizeA(15.5f, FLT_MAX, 0, SIDE_LABELS[i]).y) / 2),
                    IM_COL32(255, 255, 255, (int)(textA * 255)), SIDE_LABELS[i]);
        y += 56;

        if (cl) {
            switch (i) {
                case SI_DISCOVER: a.navigate(Page::Discover); break;
                case SI_MOVIES: a.navigate(Page::Movies); break;
                case SI_TV: a.navigate(Page::Tv); break;
                case SI_REQUESTS: a.navigate(Page::Requests); break;
                case SI_BLOCKLIST: a.navigate(Page::Blocklist); break;
                case SI_ISSUES: a.navigate(Page::Issues); break;
                case SI_USERS: a.navigate(Page::Users); break;
                case SI_SETTINGS: a.navigate(Page::Settings); break;
                default: break;
            }
        }
    }

    // stats box at bottom (RAM / CPU / peers)
    {
        auto st = core::stats();
        ImVec2 p0(wpos.x + 16, wpos.y + H - 72);
        ImVec2 p1(wpos.x + SIDEBAR_W - 16, wpos.y + H - 16);
        ImGui::SetCursorScreenPos(p0);
        ImGui::PushID("ver");
        bool cl = ImGui::InvisibleButton("##ver", ImVec2(p1.x - p0.x, p1.y - p0.y));
        bool hv = ImGui::IsItemHovered();
        ImGui::PopID();
        static float verH = 0;
        verH = animToward(verH, hv ? 1.0f : 0.0f, 12.0f);
        dl->AddRectFilled(p0, p1, theme::withA(theme::c("#374151"), 0.70f + 0.30f * verH), 6);

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

// ------------------------------------------------------------------ navbar

static void renderNavbar(App& a, bool scrolled) {
    ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoBackground;
    ImGui::SetNextWindowPos(ImVec2(SIDEBAR_W, 0));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x - SIDEBAR_W, NAVBAR_H));
    ImGui::Begin("##navbar", nullptr, wf);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wpos = ImGui::GetWindowPos();
    float w = ImGui::GetWindowWidth();
    if (scrolled) { // bg-gray-700/80 + blur
        dl->AddRectFilled(wpos, ImVec2(wpos.x + w, wpos.y + NAVBAR_H), theme::c("#374151/CC"));
        dl->AddLine(ImVec2(wpos.x, wpos.y + NAVBAR_H), ImVec2(wpos.x + w, wpos.y + NAVBAR_H), theme::c("#111827/66"));
    }

    // search input
    float sbW = std::min(440.0f, w - 120);
    ImVec2 sbMin(wpos.x + 16, wpos.y + 14);
    ImVec2 sbMax(sbMin.x + sbW, wpos.y + NAVBAR_H - 14);
    dl->AddRectFilled(sbMin, sbMax, theme::c("#111827/99"), 8);
    dl->AddRect(sbMin, sbMax, theme::c("#4b5563"), 8, 0, 1.0f);
    icons::search(dl, ImVec2(sbMin.x + 20, (sbMin.y + sbMax.y) / 2), 15, theme::c("#9ca3af"));

    ImGui::SetCursorScreenPos(ImVec2(sbMin.x + 36, sbMin.y + 6));
    ImGui::PushItemWidth(sbMax.x - sbMin.x - 60);
    char buf[256] = {};
    strncpy(buf, a.searchInput.c_str(), sizeof(buf) - 1);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImVec4(0.31f, 0.27f, 0.79f, 0.5f));
    bool enter = ImGui::InputTextWithHint("##search", "Szukaj filmów i seriali…", buf, sizeof(buf),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleColor(3);
    ImGui::PopItemWidth();
    bool changedFocus = ImGui::IsItemActive();
    a.searchInput = buf;
    if (enter && !a.searchInput.empty()) a.navigate(Page::Search);
    if (!a.searchInput.empty()) {
        ImVec2 cl(sbMax.x - 26, (sbMin.y + sbMax.y) / 2 - 9);
        if (w::iconButton(dl, "##clr", cl, ImVec2(cl.x + 18, cl.y + 18),
                          [](ImDrawList* d2, ImVec2 c, float s, ImU32 col2) { icons::close(d2, c, s, col2, 1.6f); },
                          0, theme::c("#1f2937"), 0, theme::c("#9ca3af"), true))
            a.searchInput.clear();
    }
    (void)changedFocus;

    // user avatar (gradient circle, like seerr's local user)
    {
        ImVec2 c(wpos.x + w - 30, NAVBAR_H / 2);
        float r = 15;
        dl->AddCircleFilled(c, r, theme::c("#6366f1"));
        char ini[2] = {'T', 0};
        auto& u = localdb::user();
        if (!u.displayName.empty())
            ini[0] = (char)std::toupper((unsigned char)u.displayName[0]);
        dl->AddText(G.sb18, 14, ImVec2(c.x - 5, c.y - 8), IM_COL32(255, 255, 255, 255), ini);
        dl->AddCircle(c, r + 2, theme::c("#4f46e5"), 24, 1.0f);
        if (ImGui::IsMouseHoveringRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r)) &&
            ImGui::IsMouseClicked(0))
            a.navigate(Page::Users);
    }
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
        case Page::Blocklist: renderBlocklist(); break;
        case Page::Issues: renderIssues(); break;
        case Page::Users: renderUsers(); break;
        case Page::Settings: renderSettings(); break;
        case Page::Stub: renderStub("Wkrótce"); break;
    }
}

// ------------------------------------------------------------------ main

int main() {
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    glfwWindowHint(GLFW_SAMPLES, 0); // UI doesn't need MSAA — saves GPU fillrate

    GLFWwindow* win = glfwCreateWindow(1500, 900, "Seerr C++ - Media Discovery", nullptr, nullptr);
    if (!win) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
#ifdef _WIN32
    setAppMainHwnd(glfwGetWin32Window(win));
#else
    setAppMainHwnd(nullptr);
#endif

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    initFonts();
    ImageCache::instance().init();
    svgicon::init();
    core::init(3);
    stack::init();
    localdb::init();
    localdb::loadWatchlist(app().watchlist);

    // assets
    {
        std::string dir = std::string(APP_ASSET_DIR) + "/";
        g_logoTex = ImageCache::instance().loadLocal(dir + "logo_full.png", &g_logoW, &g_logoH);
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
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_FrameBg] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.067f, 0.094f, 0.157f, 1.0f);      // #111827
    st.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.294f, 0.337f, 0.392f, 0.75f);   // #4b5563
    st.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.42f, 0.46f, 0.52f, 0.95f);
    st.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.5f, 0.54f, 0.6f, 1.0f);
    st.ScrollbarSize = 10;
    st.GrabRounding = st.ScrollbarSize / 2;

    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    App& a = app();

    while (!glfwWindowShouldClose(win)) {
        // Idle: sleep until input or ~30Hz while images are still loading (spinners)
        if (ImageCache::instance().busy())
            glfwWaitEventsTimeout(1.0 / 30.0);
        else
            glfwWaitEventsTimeout(0.05);
        ImageCache::instance().pump();
        core::tick();
        stack::tick();
        ytplayer::tick();

        int fbw, fbh;
        glfwGetFramebufferSize(win, &fbw, &fbh);
        glViewport(0, 0, fbw, fbh);
        glClearColor(0.067f, 0.094f, 0.157f, 1.0f); // gray-900
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // F5 = hard reload (clear failed images + refetch data), like Ctrl+F5 in browser
        if (ImGui::IsKeyPressed(ImGuiKey_F5)) {
            ImageCache::instance().purgeFailed();
            a.reloadSeq++;
        }

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
        bool scrolled = ImGui::GetScrollY() > 6;
        float left = ImGui::GetCursorStartPos().x; // WindowPadding.x
        float topPad = NAVBAR_H + 8;

        auto& ap = a;
        bool subpage = ap.page != Page::Discover;
        if (subpage) {
            ImGui::SetCursorPos(ImVec2(left, topPad));
            ImVec2 backMin = ImGui::GetCursorScreenPos();
            if (w::button(dl, "##back", backMin, ImVec2(backMin.x + 84, backMin.y + 26), "« Wróć",
                          theme::c("#111827/01"), theme::c("#1f2937"), theme::c("#374151"), 0,
                          theme::c("#9ca3af"), G.r14, 13, 5))
                ap.back();
            ImGui::SetCursorPos(ImVec2(left, topPad + 36));
        } else {
            ImGui::SetCursorPos(ImVec2(left, topPad));
        }
        renderContentPage(ap);
        ImGui::End();
        ImGui::PopStyleVar();

        renderSidebar(a);
        renderNavbar(a, scrolled);

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }

    svgicon::shutdown();
    stack::shutdown();
    core::shutdown();
    ImageCache::instance().shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
