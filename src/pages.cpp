#include "app.hpp"
#include "widgets.hpp"
#include "imgcache.hpp"
#include "util.hpp"
#include "ytplayer.hpp"
#include "stack.hpp"
#include "localdb.hpp"
#include "platform.hpp"
#include "library.hpp"
#include "player.hpp"
#include "svgicons.hpp"
#include "subs.hpp"
#include "i18n.hpp"
#include "updater.hpp"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <future>
#include <map>
#include <set>
#include <vector>

App& app() { static App a; return a; }

// ------------------------------------------------------------------ layout metrics
static const float CARD_W = 160.0f;
static const float CARD_H = 240.0f; // 2:3 poster
static const float GAP = 12.0f;

// ------------------------------------------------------------------ theme colors

static ImU32 SLIDER_TITLE = 0, ATTR = 0, ATTR2 = 0, WHITE = IM_COL32(255,255,255,255), TXT = 0, TXT2 = 0;
static void initColors() {
    static bool done = false;
    if (done) return; done = true;
    SLIDER_TITLE = theme::c("#d1d5db");
    ATTR = theme::c("#9ca3af");
    ATTR2 = theme::c("#d1d5db");
    TXT = theme::c("#e5e7eb");
    TXT2 = theme::c("#9ca3af");
}

static std::map<std::string, float> g_scrolls;

static void handleCardClick(const MediaItem& it, int r) {
    auto& a = app();
    if (it.mediaType == MediaType::Person) {
        if (r == 1)
            platform::openUrl("https://www.themoviedb.org/person/" + std::to_string(it.id));
        return;
    }
    std::string k = App::key(it.mediaType, it.id);
    if (r == 1) a.openDetails(it.mediaType, it.id);
    else if (r == 2) {
        openRequestQualityDialog(it.mediaType, it.id, it.title, it.year(), "", it.originalTitle);
    }
    else if (r == 3) {
        if (a.watchlist.count(k)) a.watchlist.erase(k); else a.watchlist.insert(k);
        localdb::saveWatchlist(a.watchlist);
    }
}

// ------------------------------------------------------------------ slider row (direct draw into scrolling window)

static void sliderRow(const std::string& title, PagedResult& pr) {
    initColors();
    ImVec2 wpos = ImGui::GetWindowPos();
    float w = ImGui::GetWindowWidth();
    float contentX = ImGui::GetCursorStartPos().x;
    ImGui::SetCursorPosX(contentX);
    ImVec2 cur = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    float rowY = cur.y + 36;
    float rowH = CARD_H;
    // Always reserve layout space; skip draw work when the row is off-screen
    ImGui::Dummy(ImVec2(0, 36 + rowH + 16));
    ImGui::SetCursorPosX(contentX);

    float viewTop = wpos.y;
    float viewBot = wpos.y + ImGui::GetWindowHeight();
    bool rowVisible = (rowY + rowH > viewTop - 40.0f) && (rowY < viewBot + 40.0f);
    if (!rowVisible) return;

    dl->AddText(G.b28, 20, ImVec2(cur.x, cur.y + 4), SLIDER_TITLE, title.c_str());

    auto& sc = g_scrolls[title];
    float totalW = pr.results.size() * (CARD_W + GAP);
    float maxScroll = std::max(0.0f, totalW - (w - (cur.x - wpos.x)) - 16);
    bool hovering = ImGui::IsMouseHoveringRect(ImVec2(cur.x, rowY), ImVec2(wpos.x + w, rowY + rowH));
    if (hovering && ImGui::GetIO().MouseWheelH != 0)
        sc = std::min(std::max(0.0f, sc - ImGui::GetIO().MouseWheelH * 160.0f), maxScroll);

    if (!pr.loaded) {
        if (!pr.error.empty()) {
            dl->AddText(G.r16, 14, ImVec2(cur.x, rowY + CARD_H / 2 - 8), theme::c("#ef4444"),
                        (std::string(i18n::tr("common.load_error")) + ": " + pr.error).c_str());
        } else {
            for (int i = 0; i < 14 && (float)(i * (CARD_W + GAP)) < w + CARD_W * 2; i++) {
                ImVec2 p(cur.x + i * (CARD_W + GAP), rowY);
                dl->AddRectFilled(p, ImVec2(p.x + CARD_W, p.y + rowH), theme::c("#1f2937"), 16);
            }
        }
        return;
    }

    if (maxScroll > 0) {
        if (sc > 1) {
            ImVec2 ac(cur.x - 2, rowY + rowH / 2 - 24);
            ImGui::SetCursorScreenPos(ac);
            if (w::iconButton(dl, ("##l" + title).c_str(), ac, ImVec2(ac.x + 30, ac.y + 48),
                              [](ImDrawList* d2, ImVec2 c, float s, ImU32 col) { icons::chevron(d2, c, s, col, true); },
                              theme::c("#111827/B3"), theme::c("#111827/E6"), 0, IM_COL32(255,255,255,220)))
                sc = std::max(0.0f, sc - 600);
        }
        if (sc < maxScroll - 1) {
            ImVec2 ac(wpos.x + w - 28, rowY + rowH / 2 - 24);
            ImGui::SetCursorScreenPos(ac);
            if (w::iconButton(dl, ("##r" + title).c_str(), ac, ImVec2(ac.x + 30, ac.y + 48),
                              [](ImDrawList* d2, ImVec2 c, float s, ImU32 col) { icons::chevron(d2, c, s, col, false); },
                              theme::c("#111827/B3"), theme::c("#111827/E6"), 0, IM_COL32(255,255,255,220)))
                sc = std::min(maxScroll, sc + 600);
        }
    }

    auto& a = app();
    dl->PushClipRect(ImVec2(cur.x - 2, rowY - 4), ImVec2(wpos.x + w - 2, rowY + rowH + 10), true);
    int idx = 0;
    for (auto& it : pr.results) {
        if (localdb::isBlocked(it.mediaType, it.id)) continue;
        float x = cur.x + idx * (CARD_W + GAP) - sc;
        idx++;
        if (x > wpos.x + w + 10) break;
        if (x + CARD_W < cur.x - 4) continue;
        ImGui::SetCursorScreenPos(ImVec2(x, rowY));
            int r = w::titleCard(it, ImVec2(x, rowY), CARD_W, rowH,
                                 a.isWatchlisted(it.mediaType, it.id), a.statusOf(it.mediaType, it.id));
            if (r) handleCardClick(it, r);
    }
    dl->PopClipRect();
    ImGui::SetCursorPosX(contentX);
}

// ------------------------------------------------------------------ grid

static void renderGrid(PagedResult& pr, std::function<void(int)> setPage) {
    initColors();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 wpos = ImGui::GetWindowPos();
    float w = ImGui::GetWindowWidth() - (origin.x - wpos.x) - 8;
    int cols = std::max(2, (int)((w + GAP) / (CARD_W + GAP)));

    if (!pr.loaded) {
        if (!pr.error.empty()) {
            dl->AddText(G.sb22, 18, ImVec2(origin.x, origin.y + 60), theme::c("#ef4444"), (std::string(i18n::tr("common.error")) + ": " + pr.error).c_str());
            dl->AddText(G.r16, 14, ImVec2(origin.x, origin.y + 86), ATTR, i18n::tr("common.retry_f5"));
        } else {
            float y = origin.y + 8;
            for (int i = 0; i < cols * 3; i++) {
                ImVec2 p(origin.x + (i % cols) * (CARD_W + GAP), y + (i / cols) * (CARD_H + GAP));
                dl->AddRectFilled(p, ImVec2(p.x + CARD_W, p.y + CARD_H), theme::c("#1f2937"), 16);
            }
        }
        return;
    }
    float y0 = origin.y + 10;
    int i = 0;
    auto& a = app();
    float viewTop = wpos.y - 40.0f;
    float viewBot = wpos.y + ImGui::GetWindowHeight() + 40.0f;
    for (auto& it : pr.results) {
        if (localdb::isBlocked(it.mediaType, it.id)) continue;
        float x = origin.x + (i % cols) * (CARD_W + GAP);
        float yy = y0 + (i / cols) * (CARD_H + GAP);
        i++;
        if (yy + CARD_H < viewTop || yy > viewBot) continue; // vertical cull
        ImGui::SetCursorScreenPos(ImVec2(x, yy));
        int r = w::titleCard(it, ImVec2(x, yy), CARD_W, CARD_H,
                             a.isWatchlisted(it.mediaType, it.id), a.statusOf(it.mediaType, it.id));
        if (r) handleCardClick(it, r);
    }
    int rowsShown = (i + cols - 1) / cols;
    float totalH = y0 + (float)rowsShown * (CARD_H + GAP);
    ImGui::SetCursorScreenPos(ImVec2(origin.x, totalH + 10));

    // pagination
    if (pr.total_pages > 1) {
        int curPage = pr.page;
        float cx = origin.x + w / 2;
        std::string pageLbl = std::to_string(curPage) + " / " + std::to_string(pr.total_pages);
        ImVec2 ps = G.m16->CalcTextSizeA(14, FLT_MAX, 0, pageLbl.c_str());
        ImVec2 pmin(cx - ps.x / 2 - 110, totalH + 14);
        if (curPage > 1) {
            if (w::button(dl, "##pgprev", pmin, ImVec2(pmin.x + 96, pmin.y + 32), i18n::tr("common.prev"),
                          theme::c("#374151"), theme::c("#4b5563"), theme::c("#6b7280"), theme::c("#4b5563"),
                          TXT2, G.m16, 14)) setPage(curPage - 1);
        }
        dl->AddText(G.m16, 14, ImVec2(cx - ps.x / 2, pmin.y + 8), TXT2, pageLbl.c_str());
        ImVec2 nmin(cx + ps.x / 2 + 14, pmin.y);
        if (curPage < pr.total_pages) {
            if (w::button(dl, "##pgnext", nmin, ImVec2(nmin.x + 96, nmin.y + 32), i18n::tr("common.next"),
                          theme::c("#374151"), theme::c("#4b5563"), theme::c("#6b7280"), theme::c("#4b5563"),
                          TXT2, G.m16, 14)) setPage(curPage + 1);
        }
    }
}

// ------------------------------------------------------------------ generic paged data slot

struct Paged {
    std::function<AsyncReq<PagedResult>(int)> fetch; // request builder
    int page = 1;
    AsyncReq<PagedResult> req;
    PagedResult res;
    unsigned seenReload = 0xFFFFFFFFu;

    void request(int p) { page = p; res = {}; req = fetch ? fetch(p) : AsyncReq<PagedResult>{}; }
    void ensure() {
        auto& a = app();
        if (a.shouldReload(seenReload) || (!req.valid() && (!res.loaded && res.error.empty()))) {
            if (fetch) request(page);
        }
        if (req.valid() && !res.loaded && req.ready()) res = req.take();
    }
};

static void gridPageTitle(ImDrawList* dl, ImVec2 origin, const std::string& t) {
    dl->AddText(G.b36, 26, ImVec2(origin.x, origin.y), WHITE, t.c_str());
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + 46));
}

static void renderPagedPage(const std::string& heading, Paged& pd) {
    initColors();
    pd.ensure();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    gridPageTitle(dl, ImGui::GetCursorScreenPos(), heading);
    renderGrid(pd.res, [&](int p) { pd.request(p); });
}

// ------------------------------------------------------------------ pages

void renderDiscoverGrid(bool tv) {
    static Paged movies{ nullptr, 1 };
    static Paged tvp{ nullptr, 1 };
    Paged& pd = tv ? tvp : movies;
    if (!pd.fetch) {
        pd.fetch = [tv](int p) {
            std::map<std::string, std::string> params = {{"sort_by", "popularity.desc"}};
            if (tv) params["type"] = "tv";
            return Tmdb::discover(p, params);
        };
        pd.request(1);
    }
    renderPagedPage(tv ? i18n::tr("nav.tv") : i18n::tr("nav.movies"), pd);
}

void renderGenrePage() {
    static Paged pd{ nullptr, 1 };
    static GenreSel cur;
    auto& a = app();
    if (cur.id != a.genre.id || cur.tv != a.genre.tv || !pd.fetch) {
        cur = a.genre;
        pd.fetch = [cur](int p) {
            return Tmdb::discover(p, {{"type", cur.tv ? "tv" : "movie"},
                                      {"with_genres", std::to_string(cur.id)},
                                      {"sort_by", "popularity.desc"}});
        };
        pd.request(1);
    }
    renderPagedPage(cur.name, pd);
}

void renderTrendingPage() {
    static Paged pd{ nullptr, 1 };
    if (!pd.fetch) {
        pd.fetch = [](int p) { return Tmdb::trending("all", "week"); };
        pd.request(1);
    }
    renderPagedPage(i18n::tr("discover.trending"), pd);
}

void renderSearch() {
    initColors();
    auto& a = app();
    static AsyncReq<PagedResult> req;
    static PagedResult res;
    static SearchFilter lastFilter = SearchFilter::All;
    static std::string debounceSrc;
    static double lastChange = -1;
    static unsigned seen = 0xFFFFFFFFu;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float availW = ImGui::GetContentRegionAvail().x;

    // Search field first — update query before debounce
    {
        float barH = 46.0f;
        ImVec2 bp = origin;
        ImVec2 be(origin.x + availW, origin.y + barH);
        bool hov = ImGui::IsMouseHoveringRect(bp, be);
        dl->AddRectFilled(bp, be, hov ? theme::c("#1f2937") : theme::c("#111827"), 22);
        dl->AddRect(bp, be, hov ? theme::c("#818cf8") : theme::c("#4b5563"), 22, 0, 1.4f);
        icons::search(dl, ImVec2(bp.x + 22, (bp.y + be.y) * 0.5f), 16, theme::c("#c7d2fe"));

        ImGui::SetCursorScreenPos(ImVec2(bp.x + 46, bp.y + 7));
        // Always reserve room for the clear button so the field width doesn't jump
        // (and steal focus) on the first typed character.
        ImGui::PushItemWidth(availW - 88.f);
        char buf[256] = {};
        strncpy(buf, a.searchInput.c_str(), sizeof(buf) - 1);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(0.72f, 0.76f, 0.85f, 1.0f));
        if (a.focusSearchInput) {
            ImGui::SetKeyboardFocusHere();
            a.focusSearchInput = false;
        }
        ImGui::InputTextWithHint("##search_page", i18n::tr("search.hint"), buf, sizeof(buf));
        ImGui::PopStyleColor(5);
        ImGui::PopItemWidth();
        std::string prev = a.searchInput;
        a.searchInput = buf;

        auto goHome = [&]() {
            a.searchInput.clear();
            a.lastQuery.clear();
            debounceSrc.clear();
            lastChange = -1;
            res = {};
            req = {};
            a.focusHomeSearch = true;
            a.navigate(Page::Discover);
        };

        if (!a.searchInput.empty()) {
            ImVec2 cl(be.x - 30, (bp.y + be.y) * 0.5f - 9);
            if (w::iconButton(dl, "##sclr", cl, ImVec2(cl.x + 18, cl.y + 18),
                              [](ImDrawList* d2, ImVec2 c, float s, ImU32 col2) {
                                  icons::close(d2, c, s, col2, 1.6f);
                              },
                              0, theme::c("#1f2937"), 0, theme::c("#9ca3af"), true)) {
                goHome();
                return;
            }
        } else if (!prev.empty()) {
            // Cleared the last character — back to Discover (trending etc.)
            goHome();
            return;
        }
        origin.y = be.y + 16;
        ImGui::SetCursorScreenPos(origin);
    }

    // Debounce: only restart timer when the typed text actually changes
    if (a.searchInput != debounceSrc) {
        debounceSrc = a.searchInput;
        lastChange = ImGui::GetTime();
    }
    bool filterDirty = (a.searchFilter != lastFilter);
    if (lastChange >= 0 && ImGui::GetTime() - lastChange > 0.30 && a.lastQuery != a.searchInput) {
        a.lastQuery = a.searchInput;
        lastFilter = a.searchFilter;
        res = {};
        req = a.searchInput.empty()
                  ? AsyncReq<PagedResult>{}
                  : Tmdb::search(a.searchInput, 1, a.searchFilter);
    } else if (filterDirty && !a.lastQuery.empty()) {
        lastFilter = a.searchFilter;
        res = {};
        req = Tmdb::search(a.lastQuery, 1, a.searchFilter);
    }
    if (a.shouldReload(seen) && !a.lastQuery.empty()) {
        res = {};
        req = Tmdb::search(a.lastQuery, 1, a.searchFilter);
    }
    if (req.valid() && !res.loaded && req.ready()) res = req.take();

    // Header + Seerr-style filter chips
    dl->AddText(G.b36, 26, ImVec2(origin.x, origin.y), WHITE,
                a.lastQuery.empty() ? i18n::tr("search.title") : i18n::tr("search.results_title"));
    float chipY = origin.y + 42;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, chipY));

    const char* chipLabels[] = {
        i18n::tr("common.all"), i18n::tr("nav.movies"), i18n::tr("nav.tv"), i18n::tr("search.people")
    };
    static const SearchFilter chipVals[] = {
        SearchFilter::All, SearchFilter::Movies, SearchFilter::Tv, SearchFilter::People
    };
    float cx = origin.x;
    for (int i = 0; i < 4; i++) {
        bool sel = (a.searchFilter == chipVals[i]);
        ImFont* f = G.m16;
        float fs = 13.f;
        ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, chipLabels[i]);
        ImVec2 bp(cx, chipY);
        ImVec2 be(cx + ts.x + 28, chipY + 30);
        ImU32 bg = sel ? theme::c("#4f46e5") : theme::c("#1f2937");
        ImU32 bgH = sel ? theme::c("#6366f1") : theme::c("#374151");
        ImU32 bd = sel ? theme::c("#6366f1") : theme::c("#4b5563");
        bool hov = ImGui::IsMouseHoveringRect(bp, be);
        dl->AddRectFilled(bp, be, hov ? bgH : bg, 16);
        dl->AddRect(bp, be, bd, 16, 0, 1.0f);
        dl->AddText(f, fs, ImVec2(bp.x + 14, bp.y + (30 - ts.y) * 0.5f),
                    IM_COL32(255, 255, 255, sel ? 255 : 210), chipLabels[i]);
        ImGui::SetCursorScreenPos(bp);
        if (ImGui::InvisibleButton(("##sf" + std::to_string(i)).c_str(), ImVec2(be.x - bp.x, 30)))
            a.searchFilter = chipVals[i];
        cx = be.x + 8;
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x, chipY + 44));

    if (a.lastQuery.empty() && a.searchInput.empty()) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddText(G.sb22, 18, ImVec2(p.x, p.y + 40), theme::c("#6b7280"),
                    i18n::tr("search.empty_title"));
        dl->AddText(G.r14, 13, ImVec2(p.x, p.y + 68), theme::c("#4b5563"),
                    i18n::tr("search.empty_sub"));
        return;
    }
    if (a.lastQuery.empty() || (req.valid() && !res.loaded)) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddText(G.r16, 14, ImVec2(p.x, p.y + 20), ATTR, i18n::tr("search.searching"));
        return;
    }

    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        std::string sub = "\"" + a.lastQuery + "\"";
        if (res.loaded && res.error.empty())
            sub += "  ·  " + std::to_string(res.total_results) + " " + i18n::tr("search.results");
        else if (res.loaded && !res.error.empty())
            sub += std::string("  ·  ") + i18n::tr("search.error");
        dl->AddText(G.r14, 13, ImVec2(p.x, p.y), ATTR, sub.c_str());
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 28));
    }

    if (res.loaded && !res.error.empty()) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddText(G.sb18, 16, ImVec2(p.x, p.y + 16), theme::c("#ef4444"), res.error.c_str());
        return;
    }

    if (res.loaded && res.results.empty()) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddText(G.sb18, 16, ImVec2(p.x, p.y + 24), theme::c("#9ca3af"),
                    i18n::tr("search.no_results"));
        return;
    }

    renderGrid(res, [&](int p) {
        res = {};
        req = Tmdb::search(a.lastQuery, p, a.searchFilter);
    });
}

// ------------------------------------------------------------------ discover home

struct HomeRow { const char* titleKey; AsyncReq<PagedResult> req; PagedResult res; std::function<AsyncReq<PagedResult>()> make; };
static std::vector<HomeRow>& homeRows() { static std::vector<HomeRow> v; return v; }

void renderDiscover() {
    initColors();
    auto& rows = homeRows();
    auto& a = app();
    static unsigned seen = 0xFFFFFFFFu;
    bool doReload = a.shouldReload(seen);

    // Prominent Seerr-style search prompt on the home page
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 origin = ImGui::GetCursorScreenPos();
        float availW = ImGui::GetContentRegionAvail().x;
        float barH = 48.0f;
        ImVec2 bp = origin;
        ImVec2 be(origin.x + availW, origin.y + barH);
        bool hov = ImGui::IsMouseHoveringRect(bp, be);
        dl->AddRectFilled(bp, be, hov ? theme::c("#1f2937") : theme::c("#111827"), 24);
        dl->AddRect(bp, be, hov ? theme::c("#818cf8") : theme::c("#4b5563"), 24, 0, 1.5f);
        icons::search(dl, ImVec2(bp.x + 24, (bp.y + be.y) * 0.5f), 18, theme::c("#c7d2fe"));

        ImGui::SetCursorScreenPos(ImVec2(bp.x + 48, bp.y + 8));
        ImGui::PushItemWidth(availW - 72);
        char buf[256] = {};
        strncpy(buf, a.searchInput.c_str(), sizeof(buf) - 1);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(0.72f, 0.76f, 0.85f, 1.0f));
        ImGui::PushFont(G.m18 ? G.m18 : ImGui::GetFont());
        if (a.focusHomeSearch) {
            ImGui::SetKeyboardFocusHere();
            a.focusHomeSearch = false;
        }
        bool enter = ImGui::InputTextWithHint("##home_search", i18n::tr("search.hint"),
                                              buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopFont();
        ImGui::PopStyleColor(5);
        ImGui::PopItemWidth();

        std::string prev = a.searchInput;
        a.searchInput = buf;
        // Typing on home jumps to Search — reclaim keyboard focus on the next page's field.
        if (!a.searchInput.empty() && (a.searchInput != prev || enter)) {
            a.focusSearchInput = true;
            a.navigate(Page::Search);
        }

        ImGui::SetCursorScreenPos(ImVec2(origin.x, be.y + 18));
        ImGui::Dummy(ImVec2(availW, 0));
    }

    if (rows.empty()) {
        std::string today = [] {
            using namespace std::chrono;
            auto t = system_clock::to_time_t(system_clock::now());
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            char b[16];
            std::snprintf(b, sizeof(b), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
            return std::string(b);
        }();
        rows.push_back({"discover.trending", {}, {}, []{ return Tmdb::trending("all", "week"); }});
        rows.push_back({"discover.popular_movies", {}, {}, []{ return Tmdb::discover(1, {{"sort_by","popularity.desc"}}); }});
        rows.push_back({"discover.upcoming", {}, {}, [today]{ return Tmdb::discover(1, {{"primary_release_date.gte", today},{"sort_by","popularity.desc"}}); }});
        rows.push_back({"discover.popular_tv", {}, {}, []{ return Tmdb::discover(1, {{"type","tv"},{"sort_by","popularity.desc"}}); }});
        rows.push_back({"discover.top_tv", {}, {}, []{ return Tmdb::discover(1, {{"type","tv"},{"vote_count.gte","500"},{"sort_by","vote_average.desc"}}); }});
    }
    for (auto& r : rows) {
        if (doReload) { r.res = {}; r.req = r.make(); }
        if (r.req.valid() && !r.res.loaded && r.req.ready()) r.res = r.req.take();
        sliderRow(i18n::tr(r.titleKey), r.res);
    }
}

// ------------------------------------------------------------------ details

struct DetailsHolder {
    MediaType type = MediaType::Movie;
    int id = -1;
    AsyncReq<Details> dreq;
    Details d;
    AsyncReq<PagedResult> recReq, simReq;
    PagedResult recRes, simRes;
    unsigned seen = 0xFFFFFFFFu;
    bool failedOnce = false;   // details request finished but returned nothing usable
    bool retried = false;      // one automatic refetch before showing the error
};
static DetailsHolder& dh() { static DetailsHolder h; return h; }

void renderDetails(MediaType type, int id) {
    initColors();
    auto& a = app();
    DetailsHolder& h = dh();
    bool fresh = h.id != id || h.type != type;
    bool doReload = fresh || a.shouldReload(h.seen);
    if (fresh) { h.type = type; h.id = id; }
    if (doReload) {
        h.d = {}; h.recRes = {}; h.simRes = {};
        h.failedOnce = false; h.retried = false;
        h.dreq = (type == MediaType::TV) ? Tmdb::tv(id) : Tmdb::movie(id);
        h.recReq = Tmdb::recommendations(type, id);
        h.simReq = Tmdb::similar(type, id);
    }
    if (h.dreq.valid() && h.d.id == 0 && h.dreq.ready()) {
        h.d = h.dreq.take();
        if (h.d.id == 0) { // request completed but failed (bad key / network) -> refetch once
            if (!h.retried) {
                h.retried = true;
                h.dreq = (type == MediaType::TV) ? Tmdb::tv(id) : Tmdb::movie(id);
                h.recReq = Tmdb::recommendations(type, id);
                h.simReq = Tmdb::similar(type, id);
            } else {
                h.failedOnce = true;
            }
        }
    }
    if (h.recReq.valid() && !h.recRes.loaded && h.recReq.ready()) h.recRes = h.recReq.take();
    if (h.simReq.valid() && !h.simRes.loaded && h.simReq.ready()) h.simRes = h.simReq.take();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 wpos = ImGui::GetWindowPos();
    float w = ImGui::GetContentRegionAvail().x;
    Details& d = h.d;

    if (d.id == 0) {
        dl->AddRectFilled(origin, ImVec2(origin.x + w, origin.y + 300), theme::c("#1f2937"), 16);
        if (h.failedOnce)
            dl->AddText(G.r16, 15, ImVec2(origin.x + 10, origin.y + 320), theme::c("#ef4444"),
                        i18n::tr("details.load_error"));
        else
            dl->AddText(G.r16, 15, ImVec2(origin.x + 10, origin.y + 320), ATTR, i18n::tr("common.loading"));
        ImGui::Dummy(ImVec2(w, 360));
        return;
    }

    // ---------------- backdrop header ----------------
    float headerH = 300;
    {
        const ImageEntry* be = ImageCache::instance().request(d.backdropUrl("w1920_and_h800_multi_faces"));
        // Full-bleed within content column (ignore left pad for hero, like seerr)
        float hx = wpos.x;
        float hw = ImGui::GetWindowWidth();
        dl->PushClipRect(ImVec2(hx, origin.y), ImVec2(hx + hw, origin.y + headerH), true);
        if (be && be->tex) w::imageCover(dl, be->tex, be->w, be->h, ImVec2(hx, origin.y), ImVec2(hw, headerH));
        else dl->AddRectFilled(ImVec2(hx, origin.y), ImVec2(hx + hw, origin.y + headerH), theme::c("#0f1422"));
        w::gradientRect(dl, ImVec2(hx, origin.y), ImVec2(hx + hw, origin.y + headerH),
                        theme::c("#111827/78"), theme::c("#111827"));
        dl->PopClipRect();
    }

    // ---------------- poster + title block ----------------
    float posterW = 180, posterH = 270;
    float px = origin.x, py = origin.y + headerH - 90;
    {
        const ImageEntry* pe = ImageCache::instance().request(d.posterUrl("w600_and_h900_bestv2"));
        dl->AddRectFilled(ImVec2(px, py), ImVec2(px + posterW, py + posterH), theme::c("#1f2937"), 14);
        if (pe && pe->tex) {
            w::imageCoverRounded(dl, pe->tex, pe->w, pe->h, ImVec2(px, py), ImVec2(posterW, posterH), 14);
        } else if (pe && (pe->loading || !pe->failed)) {
            icons::spinner(dl, ImVec2(px + posterW / 2, py + posterH / 2), 40, IM_COL32(255, 255, 255, 200));
        } else {
            GLuint m = ImageCache::instance().missingPosterTex;
            if (m) w::imageCoverRounded(dl, m, 300, 450, ImVec2(px, py), ImVec2(posterW, posterH), 14);
        }
        dl->AddRect(ImVec2(px, py), ImVec2(px + posterW, py + posterH), theme::c("#374151"), 14, 0, 1.0f);
    }

    float tx = px + posterW + 22;
    float ty = py;
    float rightW = origin.x + w - tx - 8;

    static const std::map<std::string, std::string> statusPl = {
        {"Released", i18n::tr("details.status_released")}, {"Post Production", i18n::tr("details.status_post")}, {"In Production", i18n::tr("details.status_in_production")},
        {"Planned", i18n::tr("details.status_planned")}, {"Rumored", i18n::tr("details.status_rumored")}, {"Continuing", i18n::tr("details.status_continuing")}, {"Ended", i18n::tr("details.status_ended")}
    };
    {
        auto it = statusPl.find(d.status);
        if (it != statusPl.end()) {
            w::badge(dl, ImVec2(tx, ty), it->second, theme::c("#374151/D9"), theme::c("#4b5563"), G.r14, 12, TXT);
            ty += 30;
        }
    }
    ty += w::textClamped(dl, ImVec2(tx, ty), rightW, d.title, G.b36, 28, WHITE, 2, 34);
    {
        std::string yr = d.year();
        if (!yr.empty()) {
            dl->AddText(G.sb18, 17, ImVec2(tx, ty), ATTR, ("(" + yr + ")").c_str());
            ty += 24;
        }
    }
    {
        std::string attrs;
        if (d.runtime > 0) attrs += util::runtimeStr(d.runtime);
        for (size_t i = 0; i < d.genres.size(); i++) { if (!attrs.empty()) attrs += " • "; attrs += d.genres[i].name; }
        if (!attrs.empty()) ty += w::textClamped(dl, ImVec2(tx, ty + 2), rightW, attrs, G.r14, 14, TXT2, 2, 18);
    }

    float btnY = ty + 14;
    int curStatus = a.statusOf(type, id);
    {
        std::string lbl = i18n::tr("details.request");
        ImU32 c0 = theme::c("#4f46e5"), c1 = theme::c("#6366f1"), c2 = theme::c("#4338ca");
        if (curStatus == 1) {
            lbl = i18n::tr("common.in_progress");
            c0 = theme::c("#f59e0b"); c1 = theme::c("#fbbf24"); c2 = theme::c("#d97706");
        } else if (curStatus == 2) {
            lbl = i18n::tr("common.failed_retry");
            c0 = theme::c("#dc2626"); c1 = theme::c("#ef4444"); c2 = theme::c("#b91c1c");
        } else if (curStatus == 3) {
            lbl = i18n::tr("common.available");
            c0 = theme::c("#10b981"); c1 = theme::c("#34d399"); c2 = theme::c("#059669");
        }
        if (w::button(dl, "##dreq", ImVec2(tx, btnY), ImVec2(tx + 170, btnY + 38), lbl,
                      c0, c1, c2, 0, WHITE, G.m18, 15, 14)) {
            if (curStatus == 0 || curStatus == 2) {
                openRequestQualityDialog(type, id, d.title, d.year(), d.imdbId, d.originalTitle, d.seasons);
            } else if (curStatus == 1) {
                a.navigate(Page::Requests);
            }
        }
        float bx = tx + 182;
        if (d.bestTrailer()) {
            if (w::button(dl, "##dtr", ImVec2(bx, btnY), ImVec2(bx + 130, btnY + 38), i18n::tr("details.trailer"),
                          theme::c("#374151"), theme::c("#4b5563"), theme::c("#6b7280"), theme::c("#4b5563"),
                          TXT, G.m18, 15))
                ytplayer::open(d.bestTrailer()->key, appMainHwnd());
            bx += 142;
        }
        if (!d.imdbId.empty()) {
            if (w::button(dl, "##dimdb", ImVec2(bx, btnY), ImVec2(bx + 130, btnY + 38), "IMDb",
                          theme::c("#374151"), theme::c("#4b5563"), theme::c("#6b7280"), theme::c("#4b5563"),
                          theme::c("#f5c518"), G.m18, 15))
                platform::openUrl(d.imdbUrl());
        }
    }
    // watchlist / blocklist / report issue (Seerr-style actions)
    {
        bool wl = a.isWatchlisted(type, id);
        std::string k = App::key(type, id);
        bool blocked = localdb::isBlocked(type, id);
        float row2 = btnY + 44;
        if (w::button(dl, "##dwl", ImVec2(tx, row2), ImVec2(tx + 210, row2 + 28),
                      wl ? i18n::tr("details.watchlist_remove") : i18n::tr("details.watchlist_add"),
                      theme::c("#111827/01"), theme::c("#1f2937"), theme::c("#374151"), 0,
                      wl ? theme::c("#fcd34d") : ATTR, G.r14, 13, 12)) {
            if (wl) a.watchlist.erase(k); else a.watchlist.insert(k);
            localdb::saveWatchlist(a.watchlist);
        }
        float ax = tx + 220;
        if (w::button(dl, "##dblk", ImVec2(ax, row2), ImVec2(ax + 150, row2 + 28),
                      blocked ? i18n::tr("details.unblock") : i18n::tr("details.block"),
                      blocked ? theme::c("#7f1d1d") : theme::c("#111827/01"),
                      blocked ? theme::c("#991b1b") : theme::c("#1f2937"),
                      theme::c("#374151"), 0,
                      blocked ? theme::c("#fecaca") : ATTR, G.r14, 13, 12)) {
            if (blocked) localdb::removeBlock(type, id);
            else localdb::addBlock(d);
        }
        ax += 160;
        if (w::button(dl, "##dissue", ImVec2(ax, row2), ImVec2(ax + 150, row2 + 28),
                      i18n::tr("details.report"),
                      theme::c("#111827/01"), theme::c("#1f2937"), theme::c("#374151"), 0,
                      ATTR, G.r14, 13, 12)) {
            ImGui::OpenPopup("##issue_modal");
        }

        if (ImGui::BeginPopupModal("##issue_modal", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
            ImGui::TextUnformatted(i18n::tr("details.report"));
            ImGui::Separator();
            static int issueType = 1;
            static char msgBuf[512] = {};
            ImGui::TextUnformatted(i18n::tr("common.type"));
            ImGui::RadioButton(i18n::tr("issue.video"), &issueType, 1); ImGui::SameLine();
            ImGui::RadioButton(i18n::tr("issue.audio"), &issueType, 2); ImGui::SameLine();
            ImGui::RadioButton(i18n::tr("issue.subtitles"), &issueType, 3); ImGui::SameLine();
            ImGui::RadioButton(i18n::tr("issue.other"), &issueType, 4);
            ImGui::TextUnformatted(i18n::tr("common.description"));
            ImGui::InputTextMultiline("##imsg", msgBuf, sizeof(msgBuf), ImVec2(420, 90));
            if (ImGui::Button(i18n::tr("common.submit"), ImVec2(120, 32))) {
                localdb::addIssue(type, id, d.title, d.year(), d.posterPath,
                                  (localdb::IssueType)issueType, msgBuf);
                msgBuf[0] = 0;
                ImGui::CloseCurrentPopup();
                a.navigate(Page::Issues);
            }
            ImGui::SameLine();
            if (ImGui::Button(i18n::tr("common.cancel"), ImVec2(120, 32))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    float bodyY = std::max(py + posterH, btnY + 84) + 10;

    // vote circle
    w::voteCircle(dl, ImVec2(origin.x + w - 52, py + 10), 22, d.voteAverage);

    dl->AddText(G.sb22, 18, ImVec2(origin.x, bodyY), TXT, i18n::tr("details.overview"));
    bodyY += 26;
    bodyY += w::textClamped(dl, ImVec2(origin.x, bodyY), w - 8, d.overview.empty() ? i18n::tr("details.no_overview") : d.overview,
                            G.r16, 15.5f, TXT, 0, 22) + 10;

    {
        struct Meta { std::string label, val; };
        std::vector<Meta> metas;
        if (type == MediaType::Movie) {
            if (d.budget) metas.push_back({i18n::tr("details.budget"), util::moneyStr((double)d.budget)});
            if (d.revenue) metas.push_back({i18n::tr("details.revenue"), util::moneyStr((double)d.revenue)});
        }
        if (!d.productionCompanies.empty()) {
            std::string s;
            for (size_t i = 0; i < std::min<size_t>(3, d.productionCompanies.size()); i++) { if (i) s += ", "; s += d.productionCompanies[i]; }
            metas.push_back({i18n::tr("details.studios"), s});
        }
        if (!d.spokenLanguages.empty()) {
            std::string s;
            for (size_t i = 0; i < std::min<size_t>(3, d.spokenLanguages.size()); i++) { if (i) s += ", "; s += d.spokenLanguages[i]; }
            metas.push_back({i18n::tr("details.languages"), s});
        }
        if (!d.originalTitle.empty() && d.originalTitle != d.title)
            metas.push_back({i18n::tr("details.original_title"), d.originalTitle});
        if (!metas.empty()) {
            bodyY += 6;
            float mx = origin.x;
            for (auto& m : metas) {
                float colW = std::max(160.0f, w / (float)metas.size() - 12);
                dl->AddText(G.sb18, 14, ImVec2(mx, bodyY), ATTR, m.label.c_str());
                dl->AddText(G.r14, 14, ImVec2(mx, bodyY + 20), TXT2, m.val.c_str());
                mx += colW + 12;
            }
            bodyY += 54;
        }
    }

    if (!d.cast.empty()) {
        bodyY += 8;
        dl->AddText(G.sb22, 18, ImVec2(origin.x, bodyY), TXT, i18n::tr("details.cast"));
        bodyY += 28;
        float cx = origin.x, csize = 100;
        int n = std::min<int>(d.cast.size(), (int)((w + 16) / (csize + 14)));
        for (int i = 0; i < n; i++) {
            ImGui::SetCursorScreenPos(ImVec2(cx, bodyY));
            w::castCard(d.cast[i], ImVec2(cx, bodyY), csize);
            cx += csize + 14;
        }
        bodyY += csize + 66;
    }

    if (type == MediaType::TV && !d.seasons.empty()) {
        bodyY += 8;
        dl->AddText(G.sb22, 18, ImVec2(origin.x, bodyY), TXT, i18n::tr("details.seasons"));
        bodyY += 28;
        float sx = origin.x;
        for (auto& s : d.seasons) {
            if (sx + 62 > origin.x + w) break;
            char b[16]; snprintf(b, sizeof(b), "%d", s.seasonNumber);
            float bw = 62, bh = 58;
            dl->AddRectFilled(ImVec2(sx, bodyY), ImVec2(sx + bw, bodyY + bh), theme::c("#1f2937"), 14);
            dl->AddRect(ImVec2(sx, bodyY), ImVec2(sx + bw, bodyY + bh), theme::c("#374151"), 14, 0, 1.0f);
            ImVec2 ts = G.sb18->CalcTextSizeA(20, FLT_MAX, 0, b);
            dl->AddText(G.sb18, 20, ImVec2(sx + bw / 2 - ts.x / 2, bodyY + 8), TXT, b);
            std::string ec = std::to_string(s.episodeCount) + i18n::tr("details.episodes_short");
            dl->AddText(G.r14, 11, ImVec2(sx + bw / 2 - G.r14->CalcTextSizeA(11, FLT_MAX, 0, ec.c_str()).x / 2, bodyY + 36),
                        theme::c("#6b7280"), ec.c_str());
            sx += bw + 8;
        }
        bodyY += 74;
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x, bodyY + 4));
    sliderRow(i18n::tr("details.recommendations"), h.recRes);
    sliderRow(i18n::tr("details.similar"), h.simRes);
}

// ------------------------------------------------------------------ stubs

void renderStub(const char* what) {
    initColors();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    dl->AddText(G.sb26, 24, ImVec2(origin.x + w / 2 - 220, origin.y + 140), ATTR, what);
    dl->AddText(G.r16, 15, ImVec2(origin.x + w / 2 - 280, origin.y + 172), theme::c("#4b5563"),
                i18n::tr("stub.not_wired"));
}

static void pageHeader(ImDrawList* dl, ImVec2 origin, const char* title, const char* sub) {
    dl->AddText(G.sb26, 24, ImVec2(origin.x, origin.y), TXT, title);
    dl->AddText(G.r14, 13, ImVec2(origin.x, origin.y + 32), ATTR, sub);
}

void renderBlocklist() {
    initColors();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    pageHeader(dl, origin, i18n::tr("blocklist.title"), i18n::tr("blocklist.sub"));

    static char search[128] = {};
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + 60));
    ImGui::PushItemWidth(std::min(360.0f, w - 20));
    ImGui::InputTextWithHint("##blsearch", i18n::tr("common.search_short"), search, sizeof(search));
    ImGui::PopItemWidth();

    auto items = localdb::listBlock(search);
    float y = origin.y + 100;
    if (items.empty()) {
        dl->AddText(G.r16, 15, ImVec2(origin.x, y), ATTR2,
                    i18n::tr("blocklist.empty"));
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y + 40));
        return;
    }

    for (auto& b : items) {
        ImVec2 p0(origin.x, y), p1(origin.x + w - 8, y + 88);
        dl->AddRectFilled(p0, p1, theme::c("#1f2937"), 16);
        dl->AddRect(p0, p1, theme::c("#374151"), 16, 0, 1.0f);

        // mini poster
        MediaItem stub;
        stub.mediaType = b.mediaType;
        stub.id = b.tmdbId;
        stub.posterPath = b.posterPath;
        std::string url = stub.posterUrl("w92");
        if (!url.empty()) {
            auto* e = ImageCache::instance().request(url);
            if (e && e->tex) {
                dl->AddImageRounded((ImTextureID)(intptr_t)e->tex,
                                    ImVec2(p0.x + 12, p0.y + 10), ImVec2(p0.x + 60, p0.y + 78),
                                    ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 12);
            } else {
                dl->AddRectFilled(ImVec2(p0.x + 12, p0.y + 10), ImVec2(p0.x + 60, p0.y + 78),
                                  theme::c("#111827"), 12);
            }
        }

        const char* type = b.mediaType == MediaType::TV ? i18n::tr("common.tv") : i18n::tr("common.movie");
        dl->AddText(G.r14, 12, ImVec2(p0.x + 74, p0.y + 12), ATTR, type);
        std::string title = b.title + (b.year.empty() ? "" : " (" + b.year + ")");
        dl->AddText(G.sb18, 16, ImVec2(p0.x + 74, p0.y + 30), TXT, title.c_str());
        w::badge(dl, ImVec2(p0.x + 74, p0.y + 54), i18n::tr("blocklist.blocked"),
                 theme::c("#7f1d1d"), theme::c("#991b1b"), G.r14, 12, theme::c("#fecaca"));

        ImGui::SetCursorScreenPos(ImVec2(p0.x + 12, p0.y + 10));
        if (ImGui::InvisibleButton(("##blopen" + std::to_string(b.tmdbId)).c_str(), ImVec2(w - 200, 68)))
            app().openDetails(b.mediaType, b.tmdbId);

        if (w::button(dl, ("##blrm" + std::to_string(b.tmdbId)).c_str(),
                      ImVec2(p1.x - 150, p0.y + 28), ImVec2(p1.x - 14, p0.y + 58),
                      "Odblokuj", theme::c("#374151"), theme::c("#4b5563"), theme::c("#6b7280"), 0,
                      TXT, G.r14, 13, 12))
            localdb::removeBlock(b.mediaType, b.tmdbId);

        y += 100;
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, y + 8));
}

void renderIssues() {
    initColors();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    pageHeader(dl, origin, i18n::tr("issues.title"), i18n::tr("issues.sub"));

    static int filter = 1; // 0 all, 1 open, 2 resolved
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + 60));
    ImGui::RadioButton(i18n::tr("issues.open"), &filter, 1); ImGui::SameLine();
    ImGui::RadioButton(i18n::tr("issues.resolved"), &filter, 2); ImGui::SameLine();
    ImGui::RadioButton(i18n::tr("common.all"), &filter, 0);

    bool all = filter == 0;
    auto status = filter == 2 ? localdb::IssueStatus::Resolved : localdb::IssueStatus::Open;
    auto items = localdb::listIssues(status, all);

    float y = origin.y + 100;
    if (items.empty()) {
        dl->AddText(G.r16, 15, ImVec2(origin.x, y), ATTR2,
                    i18n::tr("issues.empty"));
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y + 40));
        return;
    }

    auto& usr = localdb::user();
    for (auto& iss : items) {
        ImVec2 p0(origin.x, y), p1(origin.x + w - 8, y + 100);
        dl->AddRectFilled(p0, p1, theme::c("#1f2937"), 16);
        dl->AddRect(p0, p1, theme::c("#374151"), 16, 0, 1.0f);

        MediaItem stub;
        stub.mediaType = iss.mediaType;
        stub.id = iss.tmdbId;
        stub.posterPath = iss.posterPath;
        std::string url = stub.posterUrl("w92");
        if (!url.empty()) {
            auto* e = ImageCache::instance().request(url);
            if (e && e->tex) {
                dl->AddImageRounded((ImTextureID)(intptr_t)e->tex,
                                    ImVec2(p0.x + 12, p0.y + 12), ImVec2(p0.x + 60, p0.y + 88),
                                    ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 12);
            } else {
                dl->AddRectFilled(ImVec2(p0.x + 12, p0.y + 12), ImVec2(p0.x + 60, p0.y + 88),
                                  theme::c("#111827"), 12);
            }
        }

        std::string title = iss.title + (iss.year.empty() ? "" : " (" + iss.year + ")");
        dl->AddText(G.sb18, 16, ImVec2(p0.x + 74, p0.y + 12), TXT, title.c_str());

        std::string preview = iss.message;
        if (preview.size() > 90) preview = preview.substr(0, 90) + "…";
        if (!preview.empty())
            dl->AddText(G.r14, 13, ImVec2(p0.x + 74, p0.y + 36), TXT2, preview.c_str());

        ImVec2 bpos(p0.x + 74, p0.y + 58);
        auto bs = w::badge(dl, bpos,
                           iss.status == localdb::IssueStatus::Open ? i18n::tr("issues.open") : i18n::tr("issues.resolved"),
                           iss.status == localdb::IssueStatus::Open ? theme::c("#92400e") : theme::c("#065f46"),
                           iss.status == localdb::IssueStatus::Open ? theme::c("#b45309") : theme::c("#047857"),
                           G.r14, 12,
                           iss.status == localdb::IssueStatus::Open ? theme::c("#fde68a") : theme::c("#a7f3d0"));
        bpos.x += bs.x + 8;
        w::badge(dl, bpos, localdb::issueTypeLabel(iss.issueType),
                 theme::c("#312e81"), theme::c("#4338ca"), G.r14, 12, theme::c("#c7d2fe"));

        char opened[128];
        std::snprintf(opened, sizeof(opened), "przez %s", usr.displayName.c_str());
        dl->AddText(G.r14, 11, ImVec2(p0.x + 74, p0.y + 80), ATTR, opened);

        ImGui::SetCursorScreenPos(ImVec2(p0.x + 12, p0.y + 12));
        if (ImGui::InvisibleButton(("##issopen" + iss.id).c_str(), ImVec2(w - 280, 76)))
            app().openDetails(iss.mediaType, iss.tmdbId);

        float bx = p1.x - 250;
        if (iss.status == localdb::IssueStatus::Open) {
            if (w::button(dl, ("##issres" + iss.id).c_str(),
                          ImVec2(bx, p0.y + 34), ImVec2(bx + 110, p0.y + 62),
                          i18n::tr("issues.resolve"), theme::c("#065f46"), theme::c("#047857"), theme::c("#059669"), 0,
                          TXT, G.r14, 13, 12))
                localdb::setIssueStatus(iss.id, localdb::IssueStatus::Resolved);
        } else {
            if (w::button(dl, ("##issre" + iss.id).c_str(),
                          ImVec2(bx, p0.y + 34), ImVec2(bx + 110, p0.y + 62),
                          i18n::tr("issues.reopen"), theme::c("#92400e"), theme::c("#b45309"), theme::c("#d97706"), 0,
                          TXT, G.r14, 13, 12))
                localdb::setIssueStatus(iss.id, localdb::IssueStatus::Open);
        }
        if (w::button(dl, ("##issdel" + iss.id).c_str(),
                      ImVec2(bx + 120, p0.y + 34), ImVec2(bx + 230, p0.y + 62),
                      i18n::tr("common.delete"), theme::c("#374151"), theme::c("#4b5563"), theme::c("#6b7280"), 0,
                      TXT, G.r14, 13, 12))
            localdb::removeIssue(iss.id);

        y += 112;
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, y + 8));
}

void renderUsers() {
    initColors();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    pageHeader(dl, origin, i18n::tr("users.title"),
               i18n::tr("users.sub"));

    auto& usr = localdb::user();
    auto reqs = stack::listRequests();
    int reqCount = (int)reqs.size();

    // profile card
    float y = origin.y + 64;
    ImVec2 p0(origin.x, y), p1(origin.x + w - 8, y + 160);
    dl->AddRectFilled(p0, p1, theme::c("#1f2937"), 16);
    dl->AddRect(p0, p1, theme::c("#374151"), 16, 0, 1.0f);

    ImVec2 av(p0.x + 40, p0.y + 50);
    dl->AddCircleFilled(av, 28, theme::c("#6366f1"));
    char initial[8] = "T";
    if (!usr.displayName.empty()) {
        initial[0] = (char)std::toupper((unsigned char)usr.displayName[0]);
        initial[1] = 0;
    }
    ImVec2 isz = G.sb22->CalcTextSizeA(22, FLT_MAX, 0, initial);
    dl->AddText(G.sb22, 22, ImVec2(av.x - isz.x / 2, av.y - isz.y / 2), WHITE, initial);

    dl->AddText(G.sb18, 16, ImVec2(p0.x + 84, p0.y + 22), TXT, i18n::tr("users.local_profile"));
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 84, p0.y + 48));
    static char nameBuf[128] = {};
    static char emailBuf[128] = {};
    static bool loaded = false;
    if (!loaded) {
        std::snprintf(nameBuf, sizeof(nameBuf), "%s", usr.displayName.c_str());
        std::snprintf(emailBuf, sizeof(emailBuf), "%s", usr.email.c_str());
        loaded = true;
    }
    ImGui::PushItemWidth(std::min(320.0f, w - 120));
    ImGui::InputTextWithHint("##uname", i18n::tr("users.display_name"), nameBuf, sizeof(nameBuf));
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 84, p0.y + 80));
    ImGui::InputTextWithHint("##uemail", i18n::tr("users.email"), emailBuf, sizeof(emailBuf));
    ImGui::PopItemWidth();

    ImGui::SetCursorScreenPos(ImVec2(p0.x + 84, p0.y + 114));
    if (ImGui::Button(i18n::tr("users.save_profile"), ImVec2(140, 30))) {
        usr.displayName = nameBuf;
        usr.email = emailBuf;
        localdb::saveUser();
    }

    // table header row (Seerr User List style)
    y = p1.y + 24;
    dl->AddText(G.sb18, 16, ImVec2(origin.x, y), TXT, i18n::tr("users.list"));
    y += 28;

    ImVec2 th0(origin.x, y), th1(origin.x + w - 8, y + 36);
    dl->AddRectFilled(th0, th1, theme::c("#111827"), 14);
    float cols[5] = {0.32f, 0.14f, 0.18f, 0.14f, 0.22f};
    const char* headers[] = {i18n::tr("users.col_user"), i18n::tr("users.col_requests"), i18n::tr("users.col_type"), i18n::tr("users.col_role"), ""};
    float hx = th0.x + 16;
    for (int i = 0; i < 5; i++) {
        dl->AddText(G.r14, 12, ImVec2(hx, th0.y + 10), ATTR, headers[i]);
        hx += (w - 24) * cols[i];
    }
    y += 44;

    ImVec2 r0(origin.x, y), r1(origin.x + w - 8, y + 64);
    dl->AddRectFilled(r0, r1, theme::c("#1f2937"), 14);
    dl->AddRect(r0, r1, theme::c("#374151"), 14, 0, 1.0f);

    ImVec2 cav(r0.x + 28, r0.y + 32);
    dl->AddCircleFilled(cav, 16, theme::c("#6366f1"));
    dl->AddText(G.sb18, 14, ImVec2(cav.x - 5, cav.y - 8), WHITE, initial);

    float cx = r0.x + 56;
    dl->AddText(G.sb18, 14, ImVec2(cx, r0.y + 14), TXT, usr.displayName.c_str());
    dl->AddText(G.r14, 12, ImVec2(cx, r0.y + 36), ATTR, usr.email.c_str());

    cx = r0.x + (w - 24) * cols[0] + 16;
    char rc[32]; std::snprintf(rc, sizeof(rc), "%d", reqCount);
    dl->AddText(G.m16, 14, ImVec2(cx, r0.y + 24), TXT, rc);

    cx = r0.x + (w - 24) * (cols[0] + cols[1]) + 16;
    w::badge(dl, ImVec2(cx, r0.y + 22), i18n::tr("users.local"), theme::c("#1e3a5f"), theme::c("#2563eb"),
             G.r14, 12, theme::c("#93c5fd"));

    cx = r0.x + (w - 24) * (cols[0] + cols[1] + cols[2]) + 16;
    w::badge(dl, ImVec2(cx, r0.y + 22), i18n::tr("users.owner"), theme::c("#4c1d95"), theme::c("#7c3aed"),
             G.r14, 12, theme::c("#ddd6fe"));

    ImGui::SetCursorScreenPos(ImVec2(origin.x, r1.y + 24));
    ImGui::TextColored(ImVec4(0.6f, 0.65f, 0.72f, 1),
                       "Import z Jellyfin/Plex nie jest potrzebny — to lokalny stack bez kont serwerowych.");
}

void renderRequests() {
    initColors();
    stack::tick();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    dl->AddText(G.sb26, 24, ImVec2(origin.x, origin.y), TXT, i18n::tr("requests.title"));
    dl->AddText(G.r14, 13, ImVec2(origin.x, origin.y + 32), ATTR,
                i18n::tr("requests.sub"));

    auto reqs = stack::listRequests();
    float y = origin.y + 64;
    if (reqs.empty()) {
        dl->AddText(G.r16, 15, ImVec2(origin.x, y), ATTR2, i18n::tr("requests.empty"));
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y + 40));
        return;
    }
    // newest first
    std::reverse(reqs.begin(), reqs.end());
    for (auto& r : reqs) {
        ImVec2 p0(origin.x, y), p1(origin.x + w - 8, y + 72);
        dl->AddRectFilled(p0, p1, theme::c("#1f2937"), 16);
        dl->AddRect(p0, p1, theme::c("#374151"), 16, 0, 1.0f);

        ImU32 stCol = theme::c("#9ca3af");
        if (r.status == stack::ReqStatus::Available) stCol = theme::c("#10b981");
        else if (r.status == stack::ReqStatus::Failed) stCol = theme::c("#ef4444");
        else if (r.status == stack::ReqStatus::Downloading || r.status == stack::ReqStatus::Searching)
            stCol = theme::c("#f59e0b");

        std::string type = r.mediaType == MediaType::TV ? i18n::tr("common.tv") : i18n::tr("common.movie");
        if (!r.preferredQuality.empty()) type += " · " + r.preferredQuality;
        dl->AddText(G.r14, 12, ImVec2(p0.x + 14, p0.y + 10), ATTR, type.c_str());
        dl->AddText(G.sb18, 16, ImVec2(p0.x + 14, p0.y + 28), TXT, r.title.c_str());
        std::string sub = std::string(stack::statusLabel(r.status));
        if (!r.message.empty()) sub += " — " + r.message;
        if (r.status == stack::ReqStatus::Downloading)
            sub += "  " + std::to_string((int)(r.progress * 100)) + "%";
        dl->AddText(G.r14, 12, ImVec2(p0.x + 14, p0.y + 50), stCol, sub.c_str());

        ImGui::SetCursorScreenPos(ImVec2(p1.x - 96, p0.y + 22));
        if (r.status != stack::ReqStatus::Available && r.status != stack::ReqStatus::Declined) {
            if (w::button(dl, ("##can" + r.id).c_str(), ImVec2(p1.x - 96, p0.y + 22), ImVec2(p1.x - 14, p0.y + 50),
                          i18n::tr("common.cancel"), theme::c("#374151"), theme::c("#4b5563"), theme::c("#6b7280"), 0,
                          TXT, G.r14, 13, 12))
                stack::cancelRequest(r.id);
        } else if (r.status == stack::ReqStatus::Available) {
            if (w::button(dl, ("##open" + r.id).c_str(), ImVec2(p1.x - 96, p0.y + 22), ImVec2(p1.x - 14, p0.y + 50),
                          i18n::tr("common.play"), theme::c("#065f46"), theme::c("#047857"), theme::c("#059669"), 0,
                          TXT, G.r14, 13, 12)) {
                std::string path = r.libraryPath.empty() ? stack::StackConfig::get().moviesPath : r.libraryPath;
                player::open(library::resolvePlayable(path), r.title);
            }
        }
        y += 84;
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, y + 8));
}

void renderLibrary() {
    initColors();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;

    dl->AddText(G.sb26, 24, ImVec2(origin.x, origin.y), TXT, i18n::tr("library.title"));
    dl->AddText(G.r14, 13, ImVec2(origin.x, origin.y + 32), ATTR,
                i18n::tr("library.sub"));

    static std::vector<library::Item> items;
    static double lastScan = 0;
    static std::map<std::string, AsyncReq<Details>> posterReqs;
    static std::map<std::string, std::string> posterPaths; // id -> poster_path
    static library::Item ctxItem;
    static bool openProps = false;
    static bool openDelete = false;
    static std::string deleteErr;

    double now = ImGui::GetTime();
    if (items.empty() || now - lastScan > 3.0) {
        items = library::scan();
        lastScan = now;
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + 56));
    if (ImGui::Button(i18n::tr("common.refresh"), ImVec2(100, 28))) {
        items = library::scan();
        lastScan = now;
    }
    ImGui::SameLine();
    char countBuf[64];
    std::snprintf(countBuf, sizeof(countBuf), "%d %s", (int)items.size(), i18n::tr("library.titles"));
    ImGui::TextColored(ImVec4(0.61f, 0.64f, 0.69f, 1), "%s", countBuf);

    float y = origin.y + 100;
    if (items.empty()) {
        dl->AddText(G.r16, 15, ImVec2(origin.x, y), ATTR2,
                    i18n::tr("library.empty"));
        auto& cfg = stack::StackConfig::get();
        dl->AddText(G.r14, 12, ImVec2(origin.x, y + 28), ATTR,
                    (std::string(i18n::tr("library.movies")) + ": " + cfg.moviesPath).c_str());
        dl->AddText(G.r14, 12, ImVec2(origin.x, y + 48), ATTR,
                    (std::string(i18n::tr("library.tv")) + ": " + cfg.tvPath).c_str());
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y + 80));
        return;
    }

    // resolve posters for items with tmdbId
    for (auto& it : items) {
        if (it.tmdbId <= 0 || posterPaths.count(it.id)) continue;
        if (!posterReqs.count(it.id)) {
            posterReqs[it.id] = (it.mediaType == MediaType::TV) ? Tmdb::tv(it.tmdbId) : Tmdb::movie(it.tmdbId);
        } else if (posterReqs[it.id].valid() && posterReqs[it.id].ready()) {
            auto d = posterReqs[it.id].take();
            posterPaths[it.id] = d.posterPath;
            if (!it.posterPath.empty()) posterPaths[it.id] = it.posterPath;
            else it.posterPath = d.posterPath;
        }
    }

    const float cardW = 160.0f, cardH = 240.0f, gap = 14.0f;
    int cols = std::max(2, (int)((w + gap) / (cardW + gap)));
    int i = 0;
    for (auto& it : items) {
        float x = origin.x + (i % cols) * (cardW + gap);
        float yy = y + (i / cols) * (cardH + 56);
        i++;

        ImVec2 p0(x, yy), p1(x + cardW, yy + cardH);
        dl->AddRectFilled(p0, p1, theme::c("#1f2937"), 16);

        bool drew = false;
        std::string pp = it.posterPath;
        if (pp.empty()) {
            auto pit = posterPaths.find(it.id);
            if (pit != posterPaths.end()) pp = pit->second;
        }
        if (!pp.empty()) {
            MediaItem stub;
            stub.posterPath = pp;
            auto* e = ImageCache::instance().request(stub.posterUrl("w300_and_h450_face"));
            if (e && e->tex) {
                w::imageCoverRounded(dl, e->tex, e->w, e->h, p0, ImVec2(cardW, cardH), 16);
                drew = true;
            } else if (e) {
                icons::spinner(dl, ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f), 36, IM_COL32(255, 255, 255, 180));
                drew = true;
            }
        }
        if (!drew) {
            GLuint m = ImageCache::instance().missingPosterTex;
            if (m) w::imageCoverRounded(dl, m, 300, 450, p0, ImVec2(cardW, cardH), 16);
            else {
                svgicon::draw(dl, svgicon::Play,
                              ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f - 10),
                              36, theme::c("#00a4dc"));
                const char* kind = it.mediaType == MediaType::TV ? i18n::tr("common.tv") : i18n::tr("common.movie");
                ImVec2 ks = G.r14->CalcTextSizeA(12, FLT_MAX, 0, kind);
                dl->AddText(G.r14, 12, ImVec2((p0.x + p1.x - ks.x) * 0.5f, (p0.y + p1.y) * 0.5f + 24), ATTR, kind);
            }
        }
        dl->AddRect(p0, p1, theme::c("#374151"), 16, 0, 1.0f);

        ImGui::SetCursorScreenPos(p0);
        ImGui::PushID(it.id.c_str());
        if (ImGui::InvisibleButton("##libcard", ImVec2(cardW, cardH))) {
            player::open(it.path, it.title + (it.year.empty() ? "" : " (" + it.year + ")"));
        }
        bool hov = ImGui::IsItemHovered();
        if (ImGui::BeginPopupContextItem("##libctx")) {
            ctxItem = it;
            if (ImGui::MenuItem(i18n::tr("common.play"))) {
                player::open(it.path, it.title + (it.year.empty() ? "" : " (" + it.year + ")"));
            }
            if (ImGui::MenuItem(i18n::tr("common.open_folder"))) {
                std::string folder = it.folder.empty()
                    ? std::filesystem::path(it.path).parent_path().string()
                    : it.folder;
                platform::openPath(folder);
            }
            if (ImGui::MenuItem(i18n::tr("common.properties")))
                openProps = true;
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.45f, 0.45f, 1));
            if (ImGui::MenuItem(i18n::tr("common.delete"))) {
                deleteErr.clear();
                openDelete = true;
            }
            ImGui::PopStyleColor();
            ImGui::EndPopup();
        }
        ImGui::PopID();
        if (hov) {
            dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 120), 16);
            dl->AddCircleFilled(ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f), 28, IM_COL32(0, 164, 220, 230));
            svgicon::draw(dl, svgicon::Play,
                          ImVec2((p0.x + p1.x) * 0.5f + 2, (p0.y + p1.y) * 0.5f),
                          22, IM_COL32(255, 255, 255, 255));
        }

        std::string label = it.title;
        if (label.size() > 22) label = label.substr(0, 20) + "…";
        dl->AddText(G.m16, 14, ImVec2(x, yy + cardH + 8), TXT, label.c_str());
        if (!it.year.empty())
            dl->AddText(G.r14, 12, ImVec2(x, yy + cardH + 28), ATTR, it.year.c_str());
    }

    int rows = (i + cols - 1) / cols;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, y + rows * (cardH + 56) + 16));

    // ---- Właściwości ----
    if (openProps) {
        ImGui::OpenPopup("##lib_props");
        openProps = false;
    }
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("##lib_props", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.91f, 0.93f, 1));
        ImGui::TextUnformatted(i18n::tr("common.properties"));
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Spacing();

        auto row = [](const char* k, const std::string& v) {
            ImGui::TextColored(ImVec4(0.61f, 0.64f, 0.69f, 1), "%s", k);
            ImGui::SameLine(140);
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 340);
            ImGui::TextUnformatted(v.empty() ? "—" : v.c_str());
            ImGui::PopTextWrapPos();
        };

        std::string titleLine = ctxItem.title;
        if (!ctxItem.year.empty()) titleLine += " (" + ctxItem.year + ")";
        row(i18n::tr("common.title"), titleLine);
        row(i18n::tr("common.type"), ctxItem.mediaType == MediaType::TV ? i18n::tr("library.type_tv") : i18n::tr("library.type_movie"));
        if (ctxItem.tmdbId > 0)
            row("TMDB ID", std::to_string(ctxItem.tmdbId));

        int64_t sz = ctxItem.sizeBytes;
        std::error_code ec;
        namespace fs = std::filesystem;
        if (sz <= 0 && !ctxItem.path.empty() && fs::exists(ctxItem.path, ec))
            sz = (int64_t)fs::file_size(ctxItem.path, ec);
        row(i18n::tr("library.size"), library::formatSize(sz));

        std::string ext;
        if (!ctxItem.path.empty()) {
            ext = fs::path(ctxItem.path).extension().string();
            if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
            for (auto& c : ext) c = (char)std::toupper((unsigned char)c);
        }
        row(i18n::tr("library.container"), ext);

        std::string modified;
        if (!ctxItem.path.empty() && fs::exists(ctxItem.path, ec)) {
            auto ft = fs::last_write_time(ctxItem.path, ec);
            if (!ec) {
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
                std::time_t tt = std::chrono::system_clock::to_time_t(sctp);
                std::tm tm{};
#ifdef _WIN32
                localtime_s(&tm, &tt);
#else
                localtime_r(&tt, &tm);
#endif
                char tbuf[64];
                std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M", &tm);
                modified = tbuf;
            }
        }
        row(i18n::tr("library.modified"), modified);
        row(i18n::tr("library.file"), ctxItem.path);
        row(i18n::tr("library.folder"), ctxItem.folder);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (ImGui::Button(i18n::tr("common.open_folder"), ImVec2(140, 32))) {
            std::string folder = ctxItem.folder.empty()
                ? fs::path(ctxItem.path).parent_path().string()
                : ctxItem.folder;
            platform::openPath(folder);
        }
        ImGui::SameLine();
        if (ImGui::Button(i18n::tr("common.close"), ImVec2(120, 32)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- Usuń ----
    if (openDelete) {
        ImGui::OpenPopup("##lib_delete");
        openDelete = false;
    }
    if (ImGui::BeginPopupModal("##lib_delete", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.55f, 0.55f, 1));
        ImGui::TextUnformatted(i18n::tr("library.delete_title"));
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextWrapped(i18n::tr("library.delete_confirm"),
                           ctxItem.title.c_str());
        if (!ctxItem.folder.empty())
            ImGui::TextColored(ImVec4(0.61f, 0.64f, 0.69f, 1), "%s", ctxItem.folder.c_str());
        else if (!ctxItem.path.empty())
            ImGui::TextColored(ImVec4(0.61f, 0.64f, 0.69f, 1), "%s", ctxItem.path.c_str());
        if (!deleteErr.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.96f, 0.4f, 0.4f, 1), "%s", deleteErr.c_str());
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (ImGui::Button(i18n::tr("common.delete"), ImVec2(120, 32))) {
            std::string err;
            if (library::removeItem(ctxItem, &err)) {
                items = library::scan();
                lastScan = ImGui::GetTime();
                posterPaths.erase(ctxItem.id);
                posterReqs.erase(ctxItem.id);
                ImGui::CloseCurrentPopup();
            } else {
                deleteErr = err.empty() ? i18n::tr("library.delete_failed") : err;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(i18n::tr("common.cancel"), ImVec2(120, 32)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void renderSettings() {
    initColors();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float availW = ImGui::GetContentRegionAvail().x;
    float availH = ImGui::GetContentRegionAvail().y;

    dl->AddText(G.sb26, 26, ImVec2(origin.x, origin.y), TXT, i18n::tr("settings.title"));
    dl->AddText(G.r14, 13, ImVec2(origin.x, origin.y + 34), ATTR, i18n::tr("settings.sub"));

    auto& cfg = stack::StackConfig::get();
    static char movies[512], tv[512], dlpath[512];
    static char apiKey[128], osUser[128], osPass[128];
    static bool loaded = false;
    if (!loaded) {
        std::snprintf(movies, sizeof(movies), "%s", cfg.moviesPath.c_str());
        std::snprintf(tv, sizeof(tv), "%s", cfg.tvPath.c_str());
        std::snprintf(dlpath, sizeof(dlpath), "%s", cfg.downloadPath.c_str());
        std::snprintf(apiKey, sizeof(apiKey), "%s", cfg.subsApiKey.c_str());
        std::snprintf(osUser, sizeof(osUser), "%s", cfg.subsUsername.c_str());
        std::snprintf(osPass, sizeof(osPass), "%s", cfg.subsPassword.c_str());
        loaded = true;
    }

    enum Tab { TabGeneral = 0, TabLibrary, TabDownloads, TabSubs, TabUpdates, TabCount };
    static int tab = TabGeneral;
    static const char* tabKeys[] = {
        "settings.tab_general", "settings.tab_library", "settings.tab_downloads",
        "settings.tab_subs", "settings.tab_updates"
    };

    const float navW = 168.f;
    const float gap = 20.f;
    const float top = 72.f;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + top));

    // ---- left nav ----
    ImGui::BeginChild("##settings_nav", ImVec2(navW, availH - top - 8), false,
                      ImGuiWindowFlags_NoScrollbar);
    {
        ImDrawList* ndl = ImGui::GetWindowDrawList();
        ImVec2 np = ImGui::GetCursorScreenPos();
        float y = np.y;
        for (int i = 0; i < TabCount; i++) {
            ImVec2 a(np.x, y), b(np.x + navW - 8, y + 40);
            bool hov = ImGui::IsMouseHoveringRect(a, b);
            bool sel = (tab == i);
            ImU32 bg = sel ? theme::c("#4f46e5") : (hov ? theme::c("#1f2937") : 0);
            if (bg) ndl->AddRectFilled(a, b, bg, 10.f);
            if (sel) ndl->AddRectFilled(ImVec2(a.x, a.y + 8), ImVec2(a.x + 3, b.y - 8), theme::c("#a5b4fc"), 2.f);
            ndl->AddText(G.m16, 14, ImVec2(a.x + 16, a.y + 12),
                         sel ? WHITE : (hov ? TXT : ATTR), i18n::tr(tabKeys[i]));
            ImGui::SetCursorScreenPos(a);
            ImGui::InvisibleButton(("##stab" + std::to_string(i)).c_str(), ImVec2(navW - 8, 40));
            if (ImGui::IsItemClicked()) tab = i;
            y += 44;
        }
    }
    ImGui::EndChild();

    ImGui::SameLine(0, gap);

    // ---- content card ----
    float contentW = std::max(280.f, availW - navW - gap - 8);
    ImGui::BeginChild("##settings_body", ImVec2(contentW, availH - top - 8), false);

    ImDrawList* bdl = ImGui::GetWindowDrawList();
    float innerW = ImGui::GetContentRegionAvail().x;
    float fieldW = std::min(520.f, innerW - 48);

    auto sectionTitle = [&](const char* title, const char* hint) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        bdl->AddText(G.sb22, 18, p, TXT, title);
        ImGui::Dummy(ImVec2(1, 26));
        if (hint && hint[0]) {
            ImVec2 hp = ImGui::GetCursorScreenPos();
            float hw = innerW - 8;
            // wrap hint manually via TextWrapped in muted style
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ATTR));
            ImGui::PushTextWrapPos(hp.x + hw);
            ImGui::TextUnformatted(hint);
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(1, 10));
        }
        ImGui::Dummy(ImVec2(1, 4));
    };

    auto fieldLabel = [&](const char* label) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        bdl->AddText(G.m16, 13, p, ATTR2, label);
        ImGui::Dummy(ImVec2(1, 20));
    };

    auto styledInputs = []() {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(theme::c("#111827")));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImGui::ColorConvertU32ToFloat4(theme::c("#1f2937")));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImGui::ColorConvertU32ToFloat4(theme::c("#1f2937")));
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(theme::c("#374151")));
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(theme::c("#374151")));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(theme::c("#4b5563")));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4(theme::c("#4f46e5")));
        ImGui::PushStyleColor(ImGuiCol_Header, ImGui::ColorConvertU32ToFloat4(theme::c("#4f46e5")));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::ColorConvertU32ToFloat4(theme::c("#6366f1")));
        ImGui::PushStyleColor(ImGuiCol_CheckMark, ImGui::ColorConvertU32ToFloat4(theme::c("#a5b4fc")));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImGui::ColorConvertU32ToFloat4(theme::c("#6366f1")));
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImGui::ColorConvertU32ToFloat4(theme::c("#818cf8")));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 10));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 14));
    };
    auto popStyled = []() {
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(12);
    };

    // Card background
    {
        ImVec2 c0 = ImGui::GetWindowPos();
        ImVec2 c1(c0.x + ImGui::GetWindowSize().x, c0.y + ImGui::GetWindowSize().y);
        bdl->AddRectFilled(c0, c1, theme::c("#1f2937"), 14.f);
        bdl->AddRect(c0, c1, theme::c("#374151"), 14.f, 0, 1.f);
    }
    ImGui::Dummy(ImVec2(1, 8));
    ImGui::Indent(20);
    ImGui::PushItemWidth(fieldW);
    styledInputs();

    if (tab == TabGeneral) {
        sectionTitle(i18n::tr("settings.tab_general"), i18n::tr("settings.general_hint"));
        fieldLabel(i18n::tr("settings.ui_language"));
        {
            const char* uiCodes[] = { "en", "pl" };
            const char* uiLabels[] = { "English", "Polski" };
            int ui = 0;
            for (int i = 0; i < 2; i++)
                if (cfg.uiLanguage == uiCodes[i]) ui = i;
            if (ImGui::Combo("##uilang", &ui, uiLabels, 2)) {
                cfg.uiLanguage = uiCodes[ui];
                i18n::setLanguage(cfg.uiLanguage);
                cfg::syncFromUi(cfg.uiLanguage);
                app().reloadSeq++;
                cfg.save();
            }
        }
        ImGui::Dummy(ImVec2(1, 8));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ATTR));
        ImGui::TextWrapped("%s", i18n::tr("settings.save_hint"));
        ImGui::PopStyleColor();
    } else if (tab == TabLibrary) {
        sectionTitle(i18n::tr("settings.tab_library"), i18n::tr("settings.library_hint"));
        fieldLabel(i18n::tr("settings.movies_path"));
        ImGui::InputText("##movies", movies, sizeof(movies));
        fieldLabel(i18n::tr("settings.tv_path"));
        ImGui::InputText("##tv", tv, sizeof(tv));
        ImGui::Dummy(ImVec2(1, 6));
        if (ImGui::Button(i18n::tr("settings.scan_library"), ImVec2(180, 36)))
            subs::scanLibrary();
        if (!subs::statusMessage().empty()) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ATTR));
            ImGui::TextUnformatted(subs::statusMessage().c_str());
            ImGui::PopStyleColor();
        }
    } else if (tab == TabDownloads) {
        sectionTitle(i18n::tr("settings.tab_downloads"), i18n::tr("settings.downloads_hint"));
        fieldLabel(i18n::tr("settings.download_path"));
        ImGui::InputText("##dl", dlpath, sizeof(dlpath));
        ImGui::Dummy(ImVec2(1, 4));
        ImGui::Checkbox(i18n::tr("settings.auto_start"), &cfg.autoStart);
        fieldLabel(i18n::tr("settings.min_seeders"));
        ImGui::SliderInt("##seeders", &cfg.minSeeders, 0, 50);
        fieldLabel(i18n::tr("settings.default_quality"));
        {
            const char* quals[] = { "any", "720p", "1080p", "2160p" };
            const char* labels[] = { i18n::tr("settings.quality_any"), "720p", "1080p", "2160p (4K)" };
            int qi = 2;
            for (int i = 0; i < 4; i++) if (cfg.preferredQuality == quals[i]) qi = i;
            if (ImGui::Combo("##defq", &qi, labels, 4))
                cfg.preferredQuality = quals[qi];
        }
    } else if (tab == TabSubs) {
        sectionTitle(i18n::tr("settings.tab_subs"), i18n::tr("settings.subs_hint1"));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ATTR));
        ImGui::TextWrapped("%s", i18n::tr("settings.subs_hint2"));
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(1, 8));
        ImGui::Checkbox(i18n::tr("settings.subs_auto"), &cfg.subsAuto);
        fieldLabel(i18n::tr("settings.subs_lang"));
        {
            static const char* langs[] = {
                "pl", "en", "de", "fr", "es", "it", "pt", "ru", "uk", "cs", "sk",
                "hu", "nl", "sv", "no", "da", "fi", "ja", "ko", "zh", "ar", "tr"
            };
            static const char* labels[] = {
                "Polski", "English", "Deutsch", "Français", "Español", "Italiano", "Português",
                "Русский", "Українська", "Čeština", "Slovenčina", "Magyar", "Nederlands",
                "Svenska", "Norsk", "Dansk", "Suomi", "日本語", "한국어", "中文", "العربية", "Türkçe"
            };
            int li = 0;
            for (int i = 0; i < (int)(sizeof(langs) / sizeof(langs[0])); i++)
                if (cfg.subsPreferredLang == langs[i]) li = i;
            if (ImGui::Combo("##sublang", &li, labels, (int)(sizeof(labels) / sizeof(labels[0]))))
                cfg.subsPreferredLang = langs[li];
        }
        ImGui::Dummy(ImVec2(1, 10));
        {
            ImVec2 p = ImGui::GetCursorScreenPos();
            bdl->AddText(G.sb18, 15, p, TXT, i18n::tr("settings.subs_os_section"));
            ImGui::Dummy(ImVec2(1, 24));
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ATTR));
        ImGui::TextWrapped("%s", i18n::tr("settings.subs_os_hint"));
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(1, 6));
        fieldLabel(i18n::tr("settings.subs_api_key"));
        ImGui::InputText("##osapikey", apiKey, sizeof(apiKey));
        fieldLabel(i18n::tr("settings.subs_login"));
        ImGui::InputText("##osuser", osUser, sizeof(osUser));
        fieldLabel(i18n::tr("settings.subs_password"));
        ImGui::InputText("##ospass", osPass, sizeof(osPass), ImGuiInputTextFlags_Password);
        ImGui::Dummy(ImVec2(1, 4));
        if (ImGui::Button(i18n::tr("settings.subs_save"), ImVec2(180, 36))) {
            cfg.subsApiKey = apiKey;
            cfg.subsUsername = osUser;
            cfg.subsPassword = osPass;
            cfg.save();
        }
    } else if (tab == TabUpdates) {
        sectionTitle(i18n::tr("settings.tab_updates"), i18n::tr("update.settings_hint"));
        {
            auto st = updater::state();
            ImU32 badgeBg = theme::c("#374151");
            if (st == updater::State::UpToDate) badgeBg = theme::c("#065f46");
            else if (st == updater::State::Downloading || st == updater::State::Ready) badgeBg = theme::c("#1e3a8a");
            else if (st == updater::State::Error) badgeBg = theme::c("#7f1d1d");
            else if (st == updater::State::Disabled) badgeBg = theme::c("#374151");

            std::string line = updater::statusText();
            if (st == updater::State::Downloading)
                line += "  ·  " + std::to_string(updater::downloadPercent()) + "%";

            ImVec2 p = ImGui::GetCursorScreenPos();
            ImVec2 ts = G.m16 ? G.m16->CalcTextSizeA(14, FLT_MAX, 0, line.c_str())
                              : ImGui::CalcTextSize(line.c_str());
            ImVec2 br(p.x + ts.x + 24, p.y + ts.y + 16);
            bdl->AddRectFilled(p, br, badgeBg, 10.f);
            bdl->AddText(G.m16, 14, ImVec2(p.x + 12, p.y + 8), WHITE, line.c_str());
            ImGui::Dummy(ImVec2(1, br.y - p.y + 12));
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ATTR));
        ImGui::Text("%s %s", i18n::tr("update.current"),
#ifndef SEERR_VERSION
                    "dev"
#else
                    SEERR_VERSION
#endif
        );
        if (!updater::remoteVersion().empty())
            ImGui::Text("%s %s", i18n::tr("update.remote"), updater::remoteVersion().c_str());
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(1, 8));
        if (ImGui::Button(i18n::tr("update.check_now"), ImVec2(200, 36)))
            updater::checkNow();
    }

    ImGui::Dummy(ImVec2(1, 16));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(1, 10));

    // Save footer (paths + download prefs)
    if (tab == TabLibrary || tab == TabDownloads || tab == TabGeneral) {
        if (ImGui::Button(i18n::tr("common.save"), ImVec2(160, 38))) {
            cfg.moviesPath = movies;
            cfg.tvPath = tv;
            cfg.downloadPath = dlpath;
            cfg.save();
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ATTR));
        ImGui::TextUnformatted(i18n::tr("settings.saved_toast"));
        ImGui::PopStyleColor();
    }

    popStyled();
    ImGui::PopItemWidth();
    ImGui::Unindent(20);
    ImGui::Dummy(ImVec2(1, 16));
    ImGui::EndChild();
}

// ------------------------------------------------------------------ request quality + interactive search

namespace {
struct RequestDlg {
    bool wantOpen = false;
    bool pendingOpenInteractive = false;
    MediaType type = MediaType::Movie;
    int id = 0;
    std::string title, year, imdbId, originalTitle;
    int qualityIdx = 2; // 1080p

    // TV seasons / episodes
    std::vector<SeasonInfo> availableSeasons;
    std::set<int> selectedSeasons;
    bool seasonsLoading = false;
    AsyncReq<Details> seasonsReq;
    bool pickEpisodes = false;
    int episodeSeason = -1;
    std::vector<EpisodeInfo> availableEpisodes;
    std::set<int> selectedEpisodes;
    bool episodesLoading = false;
    AsyncReq<std::vector<EpisodeInfo>> episodesReq;

    AsyncReq<std::vector<stack::ReleaseHit>> searchReq;
    std::vector<stack::ReleaseHit> hits;
    bool searching = false;
    bool searchDone = false;
    std::string searchErr;
    int selected = -1;

    std::vector<int> seasonsVec() const {
        std::vector<int> v(selectedSeasons.begin(), selectedSeasons.end());
        std::sort(v.begin(), v.end());
        return v;
    }
    std::vector<int> episodesVec() const {
        if (!pickEpisodes || selectedSeasons.size() != 1) return {};
        std::vector<int> v(selectedEpisodes.begin(), selectedEpisodes.end());
        std::sort(v.begin(), v.end());
        return v;
    }
    bool canSubmit() const {
        if (type != MediaType::TV) return true;
        if (seasonsLoading || availableSeasons.empty()) return false;
        if (selectedSeasons.empty()) return false;
        if (pickEpisodes && selectedSeasons.size() == 1) {
            if (episodesLoading) return false;
            if (selectedEpisodes.empty()) return false;
        }
        return true;
    }
};
RequestDlg& reqDlg() { static RequestDlg d; return d; }

const char* kQualValues[] = { "any", "720p", "1080p", "2160p" };

const char* qualLabel(int i) {
    static const char* fixed[] = { nullptr, "720p", "1080p", "2160p (4K)" };
    if (i == 0) return i18n::tr("settings.quality_any");
    if (i >= 1 && i < 4) return fixed[i];
    return "?";
}

void applyAvailableSeasons(RequestDlg& d, const std::vector<SeasonInfo>& seasons) {
    d.availableSeasons.clear();
    for (auto& s : seasons) {
        if (s.seasonNumber <= 0) continue; // skip specials
        d.availableSeasons.push_back(s);
    }
    d.selectedSeasons.clear();
    d.pickEpisodes = false;
    d.episodeSeason = -1;
    d.availableEpisodes.clear();
    d.selectedEpisodes.clear();
    // default: select all regular seasons
    for (auto& s : d.availableSeasons)
        d.selectedSeasons.insert(s.seasonNumber);
}

void pollSeasonFetch(RequestDlg& d) {
    if (!d.seasonsLoading || !d.seasonsReq.valid()) return;
    if (!d.seasonsReq.ready()) return;
    try {
        Details det = d.seasonsReq.take();
        if (d.imdbId.empty() && !det.imdbId.empty()) d.imdbId = det.imdbId;
        if (d.year.empty() && !det.year().empty()) d.year = det.year();
        if (d.originalTitle.empty() && !det.originalTitle.empty()) d.originalTitle = det.originalTitle;
        applyAvailableSeasons(d, det.seasons);
    } catch (...) {
        d.availableSeasons.clear();
    }
    d.seasonsLoading = false;
}

void pollEpisodeFetch(RequestDlg& d) {
    if (!d.episodesLoading || !d.episodesReq.valid()) return;
    if (!d.episodesReq.ready()) return;
    try {
        d.availableEpisodes = d.episodesReq.take();
        d.selectedEpisodes.clear();
        for (auto& e : d.availableEpisodes)
            d.selectedEpisodes.insert(e.episodeNumber);
    } catch (...) {
        d.availableEpisodes.clear();
        d.selectedEpisodes.clear();
    }
    d.episodesLoading = false;
}

void startInteractiveSearch(RequestDlg& d) {
    if (d.searching && d.searchReq.valid() && !d.searchReq.ready())
        return;
    d.hits.clear();
    d.searchErr.clear();
    d.selected = -1;
    d.searching = true;
    d.searchDone = false;
    const char* q = kQualValues[std::clamp(d.qualityIdx, 0, 3)];
    MediaType type = d.type;
    int id = d.id;
    std::string title = d.title, year = d.year, imdb = d.imdbId, orig = d.originalTitle;
    std::vector<int> seasons = d.seasonsVec();
    std::vector<int> episodes = d.episodesVec();
    std::string prefQ = q;
    d.searchReq.fut = std::async(std::launch::async,
        [type, id, title, year, imdb, seasons, orig, prefQ, episodes]() {
            return stack::searchReleasesInteractive(
                type, id, title, year, imdb, seasons, orig, prefQ, episodes);
        });
}

void pollInteractiveSearch(RequestDlg& d) {
    if (!d.searching || !d.searchReq.valid()) return;
    if (!d.searchReq.ready()) return;
    try {
        d.hits = d.searchReq.take();
        d.searchErr.clear();
    } catch (const std::exception& e) {
        d.hits.clear();
        d.searchErr = e.what();
    } catch (...) {
        d.hits.clear();
        d.searchErr = i18n::tr("req.search_error");
    }
    d.searching = false;
    d.searchDone = true;
}

void grabSelectedRelease(RequestDlg& d) {
    if (d.selected < 0 || d.selected >= (int)d.hits.size()) return;
    const auto& h = d.hits[d.selected];
    const char* q = kQualValues[std::clamp(d.qualityIdx, 0, 3)];
    stack::requestWithRelease(d.type, d.id, d.title, d.year, d.imdbId, d.seasonsVec(),
                              d.originalTitle, q, h.magnet, h.title, d.episodesVec());
}
} // namespace

void openRequestQualityDialog(MediaType type, int id, const std::string& title,
                              const std::string& year, const std::string& imdbId,
                              const std::string& originalTitle,
                              const std::vector<SeasonInfo>& availableSeasons) {
    auto& d = reqDlg();
    d.wantOpen = true;
    d.type = type;
    d.id = id;
    d.title = title;
    d.year = year;
    d.imdbId = imdbId;
    d.originalTitle = originalTitle;
    d.qualityIdx = 2;
    d.pickEpisodes = false;
    d.episodeSeason = -1;
    d.availableEpisodes.clear();
    d.selectedEpisodes.clear();
    d.episodesLoading = false;
    d.seasonsLoading = false;
    auto& def = stack::StackConfig::get().preferredQuality;
    for (int i = 0; i < 4; i++) if (def == kQualValues[i]) d.qualityIdx = i;

    if (type == MediaType::TV) {
        if (!availableSeasons.empty()) {
            applyAvailableSeasons(d, availableSeasons);
        } else {
            d.availableSeasons.clear();
            d.selectedSeasons.clear();
            d.seasonsLoading = true;
            d.seasonsReq = Tmdb::tv(id);
        }
    } else {
        d.availableSeasons.clear();
        d.selectedSeasons.clear();
    }
}

void renderRequestQualityDialog() {
    auto& d = reqDlg();
    pollSeasonFetch(d);
    pollEpisodeFetch(d);
    pollInteractiveSearch(d);

    if (d.wantOpen) {
        ImGui::OpenPopup("##req_quality");
        d.wantOpen = false;
    }
    if (d.pendingOpenInteractive) {
        ImGui::OpenPopup("##req_interactive");
        d.pendingOpenInteractive = false;
    }

    ImGuiViewport* vp = ImGui::GetMainViewport();

    // ── Quality / seasons / mode picker ───────────────────────────────
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(d.type == MediaType::TV ? 520.f : 460.f, 0), ImGuiCond_Appearing);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 22));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 10));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.10f, 0.12f, 0.18f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.29f, 0.33f, 0.39f, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.91f, 0.93f, 1));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.31f, 0.27f, 0.90f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.39f, 0.40f, 0.95f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.26f, 0.22f, 0.79f, 1));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.99f, 0.99f, 1.f, 1));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.12f, 0.16f, 0.22f, 1));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.18f, 0.22f, 0.30f, 1));

    if (ImGui::BeginPopupModal("##req_quality", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                               ImGuiWindowFlags_NoMove)) {
        ImGui::PushFont(G.sb22 ? G.sb22 : ImGui::GetFont());
        ImGui::TextUnformatted(i18n::tr("req.dialog_title"));
        ImGui::PopFont();
        ImGui::Spacing();

        std::string head = d.title;
        if (!d.year.empty()) head += " (" + d.year + ")";
        ImGui::PushFont(G.m18 ? G.m18 : ImGui::GetFont());
        ImGui::TextWrapped("%s", head.c_str());
        ImGui::PopFont();
        if (!d.originalTitle.empty() && d.originalTitle != d.title) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.61f, 0.64f, 0.69f, 1));
            ImGui::TextWrapped(i18n::tr("req.original"), d.originalTitle.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.61f, 0.64f, 0.69f, 1));
        ImGui::TextUnformatted(i18n::tr("req.preferred_quality"));
        ImGui::PopStyleColor();

        for (int i = 0; i < 4; i++) {
            bool sel = (d.qualityIdx == i);
            if (sel) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.31f, 0.27f, 0.90f, 1));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.39f, 0.40f, 0.95f, 1));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.26f, 0.22f, 0.79f, 1));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.16f, 0.22f, 1));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.26f, 0.32f, 1));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f, 0.22f, 0.28f, 1));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.80f, 0.83f, 0.88f, 1));
            }
            char qid[32];
            std::snprintf(qid, sizeof(qid), "%s##q%d", qualLabel(i), i);
            if (ImGui::Button(qid, ImVec2(i == 0 ? 92.f : (i == 3 ? 110.f : 78.f), 34)))
                d.qualityIdx = i;
            ImGui::PopStyleColor(4);
            if (i < 3) ImGui::SameLine();
        }

        // ── Seasons / episodes (TV) ───────────────────────────────────
        if (d.type == MediaType::TV) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.61f, 0.64f, 0.69f, 1));
            ImGui::TextUnformatted(i18n::tr("details.seasons"));
            ImGui::PopStyleColor();

            if (d.seasonsLoading) {
                ImGui::TextUnformatted(i18n::tr("req.loading_seasons"));
            } else if (d.availableSeasons.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.97f, 0.44f, 0.44f, 1));
                ImGui::TextWrapped("%s", i18n::tr("req.no_seasons"));
                ImGui::PopStyleColor();
            } else {
                if (ImGui::SmallButton(i18n::tr("req.all_seasons"))) {
                    for (auto& s : d.availableSeasons)
                        d.selectedSeasons.insert(s.seasonNumber);
                    d.pickEpisodes = false;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(i18n::tr("req.none_seasons"))) {
                    d.selectedSeasons.clear();
                    d.pickEpisodes = false;
                    d.selectedEpisodes.clear();
                }

                float chipW = 0;
                float maxW = ImGui::GetContentRegionAvail().x;
                for (auto& s : d.availableSeasons) {
                    bool on = d.selectedSeasons.count(s.seasonNumber) > 0;
                    char label[48];
                    std::snprintf(label, sizeof(label), "S%d (%d)##ss%d",
                                  s.seasonNumber, s.episodeCount, s.seasonNumber);
                    if (on) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.31f, 0.27f, 0.90f, 1));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.39f, 0.40f, 0.95f, 1));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.26f, 0.22f, 0.79f, 1));
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.16f, 0.22f, 1));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.26f, 0.32f, 1));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f, 0.22f, 0.28f, 1));
                    }
                    ImVec2 ts = ImGui::CalcTextSize(label);
                    float bw = ts.x + 18.f;
                    if (chipW > 0 && chipW + bw + 8 > maxW) {
                        chipW = 0;
                    } else if (chipW > 0) {
                        ImGui::SameLine(0, 8);
                    }
                    if (ImGui::Button(label, ImVec2(bw, 30))) {
                        if (on) d.selectedSeasons.erase(s.seasonNumber);
                        else d.selectedSeasons.insert(s.seasonNumber);
                        if (d.selectedSeasons.size() != 1) {
                            d.pickEpisodes = false;
                            d.selectedEpisodes.clear();
                            d.availableEpisodes.clear();
                        }
                    }
                    ImGui::PopStyleColor(3);
                    chipW += (chipW > 0 ? 8.f : 0.f) + bw;
                }

                if (d.selectedSeasons.size() == 1) {
                    ImGui::Spacing();
                    bool prevPick = d.pickEpisodes;
                    ImGui::Checkbox(i18n::tr("req.pick_episodes"), &d.pickEpisodes);
                    if (d.pickEpisodes && (!prevPick || d.episodeSeason != *d.selectedSeasons.begin())) {
                        d.episodeSeason = *d.selectedSeasons.begin();
                        d.episodesLoading = true;
                        d.availableEpisodes.clear();
                        d.selectedEpisodes.clear();
                        d.episodesReq = Tmdb::tvSeason(d.id, d.episodeSeason);
                    }
                    if (!d.pickEpisodes) {
                        d.availableEpisodes.clear();
                        d.selectedEpisodes.clear();
                        d.episodeSeason = -1;
                    }

                    if (d.pickEpisodes) {
                        if (d.episodesLoading) {
                            ImGui::TextUnformatted(i18n::tr("req.loading_episodes"));
                        } else if (d.availableEpisodes.empty()) {
                            ImGui::TextUnformatted(i18n::tr("req.no_episodes"));
                        } else {
                            if (ImGui::SmallButton(i18n::tr("req.all_episodes"))) {
                                for (auto& e : d.availableEpisodes)
                                    d.selectedEpisodes.insert(e.episodeNumber);
                            }
                            ImGui::SameLine();
                            if (ImGui::SmallButton(i18n::tr("req.none_episodes")))
                                d.selectedEpisodes.clear();

                            ImGui::BeginChild("##ep_list", ImVec2(0, 160), ImGuiChildFlags_Borders);
                            for (auto& e : d.availableEpisodes) {
                                bool on = d.selectedEpisodes.count(e.episodeNumber) > 0;
                                char lbl[160];
                                std::snprintf(lbl, sizeof(lbl), "E%02d  %s", e.episodeNumber,
                                              e.name.empty() ? i18n::tr("common.untitled") : e.name.c_str());
                                if (ImGui::Checkbox(lbl, &on)) {
                                    if (on) d.selectedEpisodes.insert(e.episodeNumber);
                                    else d.selectedEpisodes.erase(e.episodeNumber);
                                }
                            }
                            ImGui::EndChild();
                        }
                    }
                } else if (d.selectedSeasons.size() > 1) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.58f, 0.64f, 1));
                    ImGui::TextWrapped("%s", i18n::tr("req.multi_season"));
                    ImGui::PopStyleColor();
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.97f, 0.55f, 0.35f, 1));
                    ImGui::TextWrapped("%s", i18n::tr("req.need_season"));
                    ImGui::PopStyleColor();
                }
            }
        }

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.58f, 0.64f, 1));
        ImGui::TextWrapped("%s", i18n::tr("req.interactive"));
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        float btnW = 128.f;
        float gap = 10.f;
        float total = btnW * 3 + gap * 2;
        float startX = (ImGui::GetContentRegionAvail().x - total) * 0.5f;
        if (startX > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + startX);

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.26f, 0.32f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.34f, 0.40f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f, 0.22f, 0.28f, 1));
        if (ImGui::Button(i18n::tr("common.cancel"), ImVec2(btnW, 38)))
            ImGui::CloseCurrentPopup();
        ImGui::PopStyleColor(3);

        bool ok = d.canSubmit() && !d.searching;
        ImGui::SameLine(0, gap);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.35f, 0.48f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.14f, 0.45f, 0.60f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.30f, 0.42f, 1));
        ImGui::BeginDisabled(!ok);
        if (ImGui::Button(i18n::tr("req.interactive_btn"), ImVec2(btnW, 38))) {
            ImGui::CloseCurrentPopup();
            d.pendingOpenInteractive = true;
            startInteractiveSearch(d);
        }
        ImGui::EndDisabled();
        ImGui::PopStyleColor(3);

        ImGui::SameLine(0, gap);
        ImGui::BeginDisabled(!ok);
        if (ImGui::Button(i18n::tr("req.auto"), ImVec2(btnW, 38))) {
            const char* q = kQualValues[std::clamp(d.qualityIdx, 0, 3)];
            stack::requestMedia(d.type, d.id, d.title, d.year, d.imdbId, d.seasonsVec(),
                                d.originalTitle, q, d.episodesVec());
            ImGui::CloseCurrentPopup();
            app().navigate(Page::Requests);
        }
        ImGui::EndDisabled();

        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(9);
    ImGui::PopStyleVar(4);

    // ── Interactive Search (Radarr-style) ─────────────────────────────
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(780, 540), ImGuiCond_Appearing);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 18));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.10f, 0.12f, 0.18f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.29f, 0.33f, 0.39f, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.91f, 0.93f, 1));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.12f, 0.23f, 0.37f, 1));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.15f, 0.39f, 0.92f, 1));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.11f, 0.30f, 0.85f, 1));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.31f, 0.27f, 0.90f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.39f, 0.40f, 0.95f, 1));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.09f, 0.14f, 1));
    ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, ImVec4(0.12f, 0.14f, 0.20f, 1));
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, ImVec4(0.22f, 0.25f, 0.32f, 0.4f));
    ImGui::PushStyleColor(ImGuiCol_TableRowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, ImVec4(1, 1, 1, 0.02f));

    if (ImGui::BeginPopupModal("##req_interactive", nullptr,
                               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove)) {
        ImGui::PushFont(G.sb22 ? G.sb22 : ImGui::GetFont());
        ImGui::TextUnformatted(i18n::tr("req.interactive_title"));
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.61f, 0.64f, 0.69f, 1));
        std::string sub = d.title;
        if (!d.year.empty()) sub += " (" + d.year + ")";
        ImGui::TextWrapped("  %s", sub.c_str());
        ImGui::PopStyleColor();

        if (d.searchDone && d.searchErr.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.58f, 0.64f, 1));
            ImGui::Text("%d %s  ·  %s: %s", (int)d.hits.size(), i18n::tr("req.releases_count"),
                        i18n::tr("common.quality"),
                        qualLabel(std::clamp(d.qualityIdx, 0, 3)));
            ImGui::PopStyleColor();
        }

        ImGui::Separator();

        float footerH = 52.0f;
        float listH = ImGui::GetContentRegionAvail().y - footerH - 4.0f;
        if (listH < 120.f) listH = 120.f;

        ImGui::BeginChild("##is_list", ImVec2(0, listH), ImGuiChildFlags_Borders);

        if (d.searching) {
            ImGui::Spacing();
            ImGui::TextUnformatted(i18n::tr("req.searching"));
        } else if (!d.searchErr.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.97f, 0.44f, 0.44f, 1));
            ImGui::TextWrapped("%s", d.searchErr.c_str());
            ImGui::PopStyleColor();
        } else if (d.hits.empty() && d.searchDone) {
            ImGui::TextUnformatted(i18n::tr("req.no_hits"));
        } else if (ImGui::BeginTable("##is_tbl", 5,
                                     ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                     ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                                     ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(i18n::tr("req.col_title"), ImGuiTableColumnFlags_WidthStretch, 3.5f);
            ImGui::TableSetupColumn(i18n::tr("req.col_quality"), ImGuiTableColumnFlags_WidthFixed, 70.f);
            ImGui::TableSetupColumn(i18n::tr("req.col_size"), ImGuiTableColumnFlags_WidthFixed, 80.f);
            ImGui::TableSetupColumn("Seeders", ImGuiTableColumnFlags_WidthFixed, 70.f);
            ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed, 55.f);
            ImGui::TableHeadersRow();

            for (int i = 0; i < (int)d.hits.size(); i++) {
                auto& h = d.hits[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                bool sel = (d.selected == i);
                ImGui::PushID(i);
                if (ImGui::Selectable(h.title.c_str(), sel,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    d.selected = i;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        grabSelectedRelease(d);
                        ImGui::CloseCurrentPopup();
                        app().navigate(Page::Requests);
                    }
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(h.quality.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(library::formatSize(h.sizeBytes).c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%d", h.seeders);
                ImGui::TableNextColumn();
                ImGui::Text("%d", h.score);
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();

        float btnW = 140.f;
        float gap = 10.f;
        if (ImGui::Button(i18n::tr("common.close"), ImVec2(btnW, 38)))
            ImGui::CloseCurrentPopup();
        ImGui::SameLine(0, gap);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.26f, 0.32f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.34f, 0.40f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f, 0.22f, 0.28f, 1));
        ImGui::BeginDisabled(d.searching);
        if (ImGui::Button(i18n::tr("common.refresh"), ImVec2(btnW, 38)))
            startInteractiveSearch(d);
        ImGui::EndDisabled();
        ImGui::PopStyleColor(3);
        ImGui::SameLine(0, gap);
        bool canGrab = d.selected >= 0 && d.selected < (int)d.hits.size() && !d.searching;
        ImGui::BeginDisabled(!canGrab);
        if (ImGui::Button(i18n::tr("req.download"), ImVec2(btnW, 38))) {
            grabSelectedRelease(d);
            ImGui::CloseCurrentPopup();
            app().navigate(Page::Requests);
        }
        ImGui::EndDisabled();

        ImGui::EndPopup();
    }

    ImGui::PopStyleColor(13);
    ImGui::PopStyleVar(4);
}

