#include "app.hpp"
#include "widgets.hpp"
#include "imgcache.hpp"
#include "util.hpp"
#include "ytplayer.hpp"
#include "stack.hpp"
#include "localdb.hpp"
#include "platform.hpp"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>

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
    std::string k = App::key(it.mediaType, it.id);
    if (r == 1) a.openDetails(it.mediaType, it.id);
    else if (r == 2) {
        stack::requestMedia(it.mediaType, it.id, it.title, it.year(), "", {}, it.originalTitle);
        a.navigate(Page::Requests);
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
                        ("Błąd wczytywania: " + pr.error + "  (F5 = ponów)").c_str());
        } else {
            for (int i = 0; i < 14 && (float)(i * (CARD_W + GAP)) < w + CARD_W * 2; i++) {
                ImVec2 p(cur.x + i * (CARD_W + GAP), rowY);
                dl->AddRectFilled(p, ImVec2(p.x + CARD_W, p.y + rowH), theme::c("#1f2937"), 12);
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
            dl->AddText(G.sb22, 18, ImVec2(origin.x, origin.y + 60), theme::c("#ef4444"), ("Błąd: " + pr.error).c_str());
            dl->AddText(G.r16, 14, ImVec2(origin.x, origin.y + 86), ATTR, "Naciśnij F5 aby ponowić, lub sprawdź klucz TMDB w config.json.");
        } else {
            float y = origin.y + 8;
            for (int i = 0; i < cols * 3; i++) {
                ImVec2 p(origin.x + (i % cols) * (CARD_W + GAP), y + (i / cols) * (CARD_H + GAP));
                dl->AddRectFilled(p, ImVec2(p.x + CARD_W, p.y + CARD_H), theme::c("#1f2937"), 12);
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
            if (w::button(dl, "##pgprev", pmin, ImVec2(pmin.x + 96, pmin.y + 32), "« Wstecz",
                          theme::c("#374151"), theme::c("#4b5563"), theme::c("#6b7280"), theme::c("#4b5563"),
                          TXT2, G.m16, 14)) setPage(curPage - 1);
        }
        dl->AddText(G.m16, 14, ImVec2(cx - ps.x / 2, pmin.y + 8), TXT2, pageLbl.c_str());
        ImVec2 nmin(cx + ps.x / 2 + 14, pmin.y);
        if (curPage < pr.total_pages) {
            if (w::button(dl, "##pgnext", nmin, ImVec2(nmin.x + 96, nmin.y + 32), "Dalej »",
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
    renderPagedPage(tv ? "Seriale" : "Filmy", pd);
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
    renderPagedPage("Na czasie", pd);
}

void renderSearch() {
    initColors();
    auto& a = app();
    static AsyncReq<PagedResult> req;
    static PagedResult res;
    static std::string resQuery;
    static double lastChange = -1;
    static unsigned seen = 0xFFFFFFFFu;

    if (a.searchInput != a.lastQuery) lastChange = ImGui::GetTime();
    if (a.searchInput != a.lastQuery && lastChange > 0 && ImGui::GetTime() - lastChange > 0.45) {
        a.lastQuery = a.searchInput;
        res = {};
        req = a.searchInput.empty() ? AsyncReq<PagedResult>{} : Tmdb::searchMulti(a.searchInput, 1);
    }
    if (a.shouldReload(seen) && !a.lastQuery.empty()) { res = {}; req = Tmdb::searchMulti(a.lastQuery, 1); }
    if (req.valid() && !res.loaded && req.ready()) res = req.take();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    if (a.lastQuery.empty()) {
        dl->AddText(G.sb26, 24, ImVec2(origin.x, origin.y + 120), theme::c("#4b5563"),
                    "Wpisz tytuł w pole wyszukiwania u góry, aby znaleźć filmy i seriale.");
        return;
    }
    std::string head = "Wyniki dla \"" + a.lastQuery + "\"";
    dl->AddText(G.b36, 26, ImVec2(origin.x, origin.y), WHITE, head.c_str());
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + 46));
    renderGrid(res, [&](int p) { res = {}; req = Tmdb::searchMulti(a.lastQuery, p); });
}

// ------------------------------------------------------------------ discover home

struct HomeRow { std::string title; AsyncReq<PagedResult> req; PagedResult res; std::function<AsyncReq<PagedResult>()> make; };
static std::vector<HomeRow>& homeRows() { static std::vector<HomeRow> v; return v; }

void renderDiscover() {
    initColors();
    auto& rows = homeRows();
    auto& a = app();
    static unsigned seen = 0xFFFFFFFFu;
    bool doReload = a.shouldReload(seen);

    if (rows.empty()) {
        std::string today = [] {
            SYSTEMTIME st; GetLocalTime(&st);
            char b[16]; snprintf(b, sizeof(b), "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
            return std::string(b);
        }();
        rows.push_back({"Na czasie", {}, {}, []{ return Tmdb::trending("all", "week"); }});
        rows.push_back({"Popularne filmy", {}, {}, []{ return Tmdb::discover(1, {{"sort_by","popularity.desc"}}); }});
        rows.push_back({"Wkrótce w kinie", {}, {}, [today]{ return Tmdb::discover(1, {{"primary_release_date.gte", today},{"sort_by","popularity.desc"}}); }});
        rows.push_back({"Popularne seriale", {}, {}, []{ return Tmdb::discover(1, {{"type","tv"},{"sort_by","popularity.desc"}}); }});
        rows.push_back({"Najlepiej oceniane seriale", {}, {}, []{ return Tmdb::discover(1, {{"type","tv"},{"vote_count.gte","500"},{"sort_by","vote_average.desc"}}); }});
    }
    for (auto& r : rows) {
        if (doReload) { r.res = {}; r.req = r.make(); }
        if (r.req.valid() && !r.res.loaded && r.req.ready()) r.res = r.req.take();
        sliderRow(r.title, r.res);
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
        dl->AddRectFilled(origin, ImVec2(origin.x + w, origin.y + 300), theme::c("#1f2937"), 10);
        if (h.failedOnce)
            dl->AddText(G.r16, 15, ImVec2(origin.x + 10, origin.y + 320), theme::c("#ef4444"),
                        "Błąd wczytywania szczegółów (sprawdź klucz TMDB w config.json)  (F5 = ponów)");
        else
            dl->AddText(G.r16, 15, ImVec2(origin.x + 10, origin.y + 320), ATTR, "Ładowanie…");
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
        dl->AddRectFilled(ImVec2(px, py), ImVec2(px + posterW, py + posterH), theme::c("#1f2937"), 8);
        if (pe && pe->tex) {
            w::imageCoverRounded(dl, pe->tex, pe->w, pe->h, ImVec2(px, py), ImVec2(posterW, posterH), 8);
        } else if (pe && (pe->loading || !pe->failed)) {
            icons::spinner(dl, ImVec2(px + posterW / 2, py + posterH / 2), 40, IM_COL32(255, 255, 255, 200));
        } else {
            GLuint m = ImageCache::instance().missingPosterTex;
            if (m) w::imageCoverRounded(dl, m, 300, 450, ImVec2(px, py), ImVec2(posterW, posterH), 8);
        }
        dl->AddRect(ImVec2(px, py), ImVec2(px + posterW, py + posterH), theme::c("#374151"), 8, 0, 1.0f);
    }

    float tx = px + posterW + 22;
    float ty = py;
    float rightW = origin.x + w - tx - 8;

    static const std::map<std::string, std::string> statusPl = {
        {"Released", "Wydano"}, {"Post Production", "Postprodukcja"}, {"In Production", "W produkcji"},
        {"Planned", "Planowany"}, {"Rumored", "Plotki"}, {"Continuing", "W emisji"}, {"Ended", "Zakończony"}
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
        std::string lbl = "Zażądaj";
        ImU32 c0 = theme::c("#4f46e5"), c1 = theme::c("#6366f1"), c2 = theme::c("#4338ca");
        if (curStatus == 1) {
            lbl = "W toku…";
            c0 = theme::c("#f59e0b"); c1 = theme::c("#fbbf24"); c2 = theme::c("#d97706");
        } else if (curStatus == 2) {
            lbl = "Błąd — ponów";
            c0 = theme::c("#dc2626"); c1 = theme::c("#ef4444"); c2 = theme::c("#b91c1c");
        } else if (curStatus == 3) {
            lbl = "Dostępne";
            c0 = theme::c("#10b981"); c1 = theme::c("#34d399"); c2 = theme::c("#059669");
        }
        if (w::button(dl, "##dreq", ImVec2(tx, btnY), ImVec2(tx + 170, btnY + 38), lbl,
                      c0, c1, c2, 0, WHITE, G.m18, 15, 8)) {
            if (curStatus == 0 || curStatus == 2) {
                stack::requestMedia(d);
                a.navigate(Page::Requests);
            } else if (curStatus == 1) {
                a.navigate(Page::Requests);
            }
        }
        float bx = tx + 182;
        if (d.bestTrailer()) {
            if (w::button(dl, "##dtr", ImVec2(bx, btnY), ImVec2(bx + 130, btnY + 38), "Zwiastun",
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
                      wl ? "★ Usuń z obserwowanych" : "☆ Dodaj do obserwowanych",
                      theme::c("#111827/01"), theme::c("#1f2937"), theme::c("#374151"), 0,
                      wl ? theme::c("#fcd34d") : ATTR, G.r14, 13, 6)) {
            if (wl) a.watchlist.erase(k); else a.watchlist.insert(k);
            localdb::saveWatchlist(a.watchlist);
        }
        float ax = tx + 220;
        if (w::button(dl, "##dblk", ImVec2(ax, row2), ImVec2(ax + 150, row2 + 28),
                      blocked ? "Odblokuj" : "Zablokuj",
                      blocked ? theme::c("#7f1d1d") : theme::c("#111827/01"),
                      blocked ? theme::c("#991b1b") : theme::c("#1f2937"),
                      theme::c("#374151"), 0,
                      blocked ? theme::c("#fecaca") : ATTR, G.r14, 13, 6)) {
            if (blocked) localdb::removeBlock(type, id);
            else localdb::addBlock(d);
        }
        ax += 160;
        if (w::button(dl, "##dissue", ImVec2(ax, row2), ImVec2(ax + 150, row2 + 28),
                      "Zgłoś problem",
                      theme::c("#111827/01"), theme::c("#1f2937"), theme::c("#374151"), 0,
                      ATTR, G.r14, 13, 6)) {
            ImGui::OpenPopup("##issue_modal");
        }

        if (ImGui::BeginPopupModal("##issue_modal", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
            ImGui::TextUnformatted("Zgłoś problem");
            ImGui::Separator();
            static int issueType = 1;
            static char msgBuf[512] = {};
            ImGui::TextUnformatted("Typ");
            ImGui::RadioButton("Wideo", &issueType, 1); ImGui::SameLine();
            ImGui::RadioButton("Audio", &issueType, 2); ImGui::SameLine();
            ImGui::RadioButton("Napisy", &issueType, 3); ImGui::SameLine();
            ImGui::RadioButton("Inne", &issueType, 4);
            ImGui::TextUnformatted("Opis");
            ImGui::InputTextMultiline("##imsg", msgBuf, sizeof(msgBuf), ImVec2(420, 90));
            if (ImGui::Button("Wyślij", ImVec2(120, 32))) {
                localdb::addIssue(type, id, d.title, d.year(), d.posterPath,
                                  (localdb::IssueType)issueType, msgBuf);
                msgBuf[0] = 0;
                ImGui::CloseCurrentPopup();
                a.navigate(Page::Issues);
            }
            ImGui::SameLine();
            if (ImGui::Button("Anuluj", ImVec2(120, 32))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    float bodyY = std::max(py + posterH, btnY + 84) + 10;

    // vote circle
    w::voteCircle(dl, ImVec2(origin.x + w - 52, py + 10), 22, d.voteAverage);

    dl->AddText(G.sb22, 18, ImVec2(origin.x, bodyY), TXT, "Fabuła");
    bodyY += 26;
    bodyY += w::textClamped(dl, ImVec2(origin.x, bodyY), w - 8, d.overview.empty() ? "Brak opisu." : d.overview,
                            G.r16, 15.5f, TXT, 0, 22) + 10;

    {
        struct Meta { std::string label, val; };
        std::vector<Meta> metas;
        if (type == MediaType::Movie) {
            if (d.budget) metas.push_back({"Budżet", util::moneyStr((double)d.budget)});
            if (d.revenue) metas.push_back({"Wpływy", util::moneyStr((double)d.revenue)});
        }
        if (!d.productionCompanies.empty()) {
            std::string s;
            for (size_t i = 0; i < std::min<size_t>(3, d.productionCompanies.size()); i++) { if (i) s += ", "; s += d.productionCompanies[i]; }
            metas.push_back({"Firmy produkcyjne", s});
        }
        if (!d.spokenLanguages.empty()) {
            std::string s;
            for (size_t i = 0; i < std::min<size_t>(3, d.spokenLanguages.size()); i++) { if (i) s += ", "; s += d.spokenLanguages[i]; }
            metas.push_back({"Języki", s});
        }
        if (!d.originalTitle.empty() && d.originalTitle != d.title)
            metas.push_back({"Tytuł oryginalny", d.originalTitle});
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
        dl->AddText(G.sb22, 18, ImVec2(origin.x, bodyY), TXT, "Obsada");
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
        dl->AddText(G.sb22, 18, ImVec2(origin.x, bodyY), TXT, "Sezony");
        bodyY += 28;
        float sx = origin.x;
        for (auto& s : d.seasons) {
            if (sx + 62 > origin.x + w) break;
            char b[16]; snprintf(b, sizeof(b), "%d", s.seasonNumber);
            float bw = 62, bh = 58;
            dl->AddRectFilled(ImVec2(sx, bodyY), ImVec2(sx + bw, bodyY + bh), theme::c("#1f2937"), 8);
            dl->AddRect(ImVec2(sx, bodyY), ImVec2(sx + bw, bodyY + bh), theme::c("#374151"), 8, 0, 1.0f);
            ImVec2 ts = G.sb18->CalcTextSizeA(20, FLT_MAX, 0, b);
            dl->AddText(G.sb18, 20, ImVec2(sx + bw / 2 - ts.x / 2, bodyY + 8), TXT, b);
            std::string ec = std::to_string(s.episodeCount) + " odc.";
            dl->AddText(G.r14, 11, ImVec2(sx + bw / 2 - G.r14->CalcTextSizeA(11, FLT_MAX, 0, ec.c_str()).x / 2, bodyY + 36),
                        theme::c("#6b7280"), ec.c_str());
            sx += bw + 8;
        }
        bodyY += 74;
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x, bodyY + 4));
    sliderRow("Rekomendacje", h.recRes);
    sliderRow("Podobne", h.simRes);
}

// ------------------------------------------------------------------ stubs

void renderStub(const char* what) {
    initColors();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    dl->AddText(G.sb26, 24, ImVec2(origin.x + w / 2 - 220, origin.y + 140), ATTR, what);
    dl->AddText(G.r16, 15, ImVec2(origin.x + w / 2 - 280, origin.y + 172), theme::c("#4b5563"),
                "Ta sekcja nie jest jeszcze podpięta pod lokalny stack.");
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
    pageHeader(dl, origin, "Blokada", "Zarządzaj zablokowanymi tytułami (jak w Seerr).");

    static char search[128] = {};
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + 60));
    ImGui::PushItemWidth(std::min(360.0f, w - 20));
    ImGui::InputTextWithHint("##blsearch", "Szukaj…", search, sizeof(search));
    ImGui::PopItemWidth();

    auto items = localdb::listBlock(search);
    float y = origin.y + 100;
    if (items.empty()) {
        dl->AddText(G.r16, 15, ImVec2(origin.x, y), ATTR2,
                    "Brak zablokowanych tytułów. Użyj „Zablokuj” na stronie szczegółów.");
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y + 40));
        return;
    }

    for (auto& b : items) {
        ImVec2 p0(origin.x, y), p1(origin.x + w - 8, y + 88);
        dl->AddRectFilled(p0, p1, theme::c("#1f2937"), 10);
        dl->AddRect(p0, p1, theme::c("#374151"), 10, 0, 1.0f);

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
                                    ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 6);
            } else {
                dl->AddRectFilled(ImVec2(p0.x + 12, p0.y + 10), ImVec2(p0.x + 60, p0.y + 78),
                                  theme::c("#111827"), 6);
            }
        }

        const char* type = b.mediaType == MediaType::TV ? "SERIAL" : "FILM";
        dl->AddText(G.r14, 12, ImVec2(p0.x + 74, p0.y + 12), ATTR, type);
        std::string title = b.title + (b.year.empty() ? "" : " (" + b.year + ")");
        dl->AddText(G.sb18, 16, ImVec2(p0.x + 74, p0.y + 30), TXT, title.c_str());
        w::badge(dl, ImVec2(p0.x + 74, p0.y + 54), "Zablokowane",
                 theme::c("#7f1d1d"), theme::c("#991b1b"), G.r14, 12, theme::c("#fecaca"));

        ImGui::SetCursorScreenPos(ImVec2(p0.x + 12, p0.y + 10));
        if (ImGui::InvisibleButton(("##blopen" + std::to_string(b.tmdbId)).c_str(), ImVec2(w - 200, 68)))
            app().openDetails(b.mediaType, b.tmdbId);

        if (w::button(dl, ("##blrm" + std::to_string(b.tmdbId)).c_str(),
                      ImVec2(p1.x - 150, p0.y + 28), ImVec2(p1.x - 14, p0.y + 58),
                      "Odblokuj", theme::c("#374151"), theme::c("#4b5563"), theme::c("#6b7280"), 0,
                      TXT, G.r14, 13, 6))
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
    pageHeader(dl, origin, "Problemy", "Zgłoszenia problemów z mediami (jak w Seerr).");

    static int filter = 1; // 0 all, 1 open, 2 resolved
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + 60));
    ImGui::RadioButton("Otwarte", &filter, 1); ImGui::SameLine();
    ImGui::RadioButton("Rozwiązane", &filter, 2); ImGui::SameLine();
    ImGui::RadioButton("Wszystkie", &filter, 0);

    bool all = filter == 0;
    auto status = filter == 2 ? localdb::IssueStatus::Resolved : localdb::IssueStatus::Open;
    auto items = localdb::listIssues(status, all);

    float y = origin.y + 100;
    if (items.empty()) {
        dl->AddText(G.r16, 15, ImVec2(origin.x, y), ATTR2,
                    "Brak problemów. Zgłoś je ze strony szczegółów filmu/serialu.");
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y + 40));
        return;
    }

    auto& usr = localdb::user();
    for (auto& iss : items) {
        ImVec2 p0(origin.x, y), p1(origin.x + w - 8, y + 100);
        dl->AddRectFilled(p0, p1, theme::c("#1f2937"), 10);
        dl->AddRect(p0, p1, theme::c("#374151"), 10, 0, 1.0f);

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
                                    ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 6);
            } else {
                dl->AddRectFilled(ImVec2(p0.x + 12, p0.y + 12), ImVec2(p0.x + 60, p0.y + 88),
                                  theme::c("#111827"), 6);
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
                           iss.status == localdb::IssueStatus::Open ? "Otwarte" : "Rozwiązane",
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
                          "Rozwiąż", theme::c("#065f46"), theme::c("#047857"), theme::c("#059669"), 0,
                          TXT, G.r14, 13, 6))
                localdb::setIssueStatus(iss.id, localdb::IssueStatus::Resolved);
        } else {
            if (w::button(dl, ("##issre" + iss.id).c_str(),
                          ImVec2(bx, p0.y + 34), ImVec2(bx + 110, p0.y + 62),
                          "Otwórz", theme::c("#92400e"), theme::c("#b45309"), theme::c("#d97706"), 0,
                          TXT, G.r14, 13, 6))
                localdb::setIssueStatus(iss.id, localdb::IssueStatus::Open);
        }
        if (w::button(dl, ("##issdel" + iss.id).c_str(),
                      ImVec2(bx + 120, p0.y + 34), ImVec2(bx + 230, p0.y + 62),
                      "Usuń", theme::c("#374151"), theme::c("#4b5563"), theme::c("#6b7280"), 0,
                      TXT, G.r14, 13, 6))
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
    pageHeader(dl, origin, "Użytkownicy",
               "Lokalny właściciel aplikacji (bez importu z Jellyfin — wszystko w jednym programie).");

    auto& usr = localdb::user();
    auto reqs = stack::listRequests();
    int reqCount = (int)reqs.size();

    // profile card
    float y = origin.y + 64;
    ImVec2 p0(origin.x, y), p1(origin.x + w - 8, y + 160);
    dl->AddRectFilled(p0, p1, theme::c("#1f2937"), 12);
    dl->AddRect(p0, p1, theme::c("#374151"), 12, 0, 1.0f);

    ImVec2 av(p0.x + 40, p0.y + 50);
    dl->AddCircleFilled(av, 28, theme::c("#6366f1"));
    char initial[8] = "T";
    if (!usr.displayName.empty()) {
        initial[0] = (char)std::toupper((unsigned char)usr.displayName[0]);
        initial[1] = 0;
    }
    ImVec2 isz = G.sb22->CalcTextSizeA(22, FLT_MAX, 0, initial);
    dl->AddText(G.sb22, 22, ImVec2(av.x - isz.x / 2, av.y - isz.y / 2), WHITE, initial);

    dl->AddText(G.sb18, 16, ImVec2(p0.x + 84, p0.y + 22), TXT, "Profil lokalny");
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
    ImGui::InputTextWithHint("##uname", "Nazwa wyświetlana", nameBuf, sizeof(nameBuf));
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 84, p0.y + 80));
    ImGui::InputTextWithHint("##uemail", "E-mail", emailBuf, sizeof(emailBuf));
    ImGui::PopItemWidth();

    ImGui::SetCursorScreenPos(ImVec2(p0.x + 84, p0.y + 114));
    if (ImGui::Button("Zapisz profil", ImVec2(140, 30))) {
        usr.displayName = nameBuf;
        usr.email = emailBuf;
        localdb::saveUser();
    }

    // table header row (Seerr User List style)
    y = p1.y + 24;
    dl->AddText(G.sb18, 16, ImVec2(origin.x, y), TXT, "Lista użytkowników");
    y += 28;

    ImVec2 th0(origin.x, y), th1(origin.x + w - 8, y + 36);
    dl->AddRectFilled(th0, th1, theme::c("#111827"), 8);
    float cols[5] = {0.32f, 0.14f, 0.18f, 0.14f, 0.22f};
    const char* headers[] = {"Użytkownik", "Żądania", "Typ", "Rola", ""};
    float hx = th0.x + 16;
    for (int i = 0; i < 5; i++) {
        dl->AddText(G.r14, 12, ImVec2(hx, th0.y + 10), ATTR, headers[i]);
        hx += (w - 24) * cols[i];
    }
    y += 44;

    ImVec2 r0(origin.x, y), r1(origin.x + w - 8, y + 64);
    dl->AddRectFilled(r0, r1, theme::c("#1f2937"), 8);
    dl->AddRect(r0, r1, theme::c("#374151"), 8, 0, 1.0f);

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
    w::badge(dl, ImVec2(cx, r0.y + 22), "Lokalny", theme::c("#1e3a5f"), theme::c("#2563eb"),
             G.r14, 12, theme::c("#93c5fd"));

    cx = r0.x + (w - 24) * (cols[0] + cols[1] + cols[2]) + 16;
    w::badge(dl, ImVec2(cx, r0.y + 22), "Właściciel", theme::c("#4c1d95"), theme::c("#7c3aed"),
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
    dl->AddText(G.sb26, 24, ImVec2(origin.x, origin.y), TXT, "Żądania");
    dl->AddText(G.r14, 13, ImVec2(origin.x, origin.y + 32), ATTR,
                "Szukanie → libtorrent → biblioteka (bez zewnętrznych *arr / qBit)");

    auto reqs = stack::listRequests();
    float y = origin.y + 64;
    if (reqs.empty()) {
        dl->AddText(G.r16, 15, ImVec2(origin.x, y), ATTR2, "Brak żądań. Kliknij „Zażądaj” na filmie lub serialu.");
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y + 40));
        return;
    }
    // newest first
    std::reverse(reqs.begin(), reqs.end());
    for (auto& r : reqs) {
        ImVec2 p0(origin.x, y), p1(origin.x + w - 8, y + 72);
        dl->AddRectFilled(p0, p1, theme::c("#1f2937"), 10);
        dl->AddRect(p0, p1, theme::c("#374151"), 10, 0, 1.0f);

        ImU32 stCol = theme::c("#9ca3af");
        if (r.status == stack::ReqStatus::Available) stCol = theme::c("#10b981");
        else if (r.status == stack::ReqStatus::Failed) stCol = theme::c("#ef4444");
        else if (r.status == stack::ReqStatus::Downloading || r.status == stack::ReqStatus::Searching)
            stCol = theme::c("#f59e0b");

        std::string type = r.mediaType == MediaType::TV ? "SERIAL" : "FILM";
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
                          "Anuluj", theme::c("#374151"), theme::c("#4b5563"), theme::c("#6b7280"), 0,
                          TXT, G.r14, 13, 6))
                stack::cancelRequest(r.id);
        } else if (r.status == stack::ReqStatus::Available) {
            if (w::button(dl, ("##open" + r.id).c_str(), ImVec2(p1.x - 96, p0.y + 22), ImVec2(p1.x - 14, p0.y + 50),
                          "Otwórz", theme::c("#065f46"), theme::c("#047857"), theme::c("#059669"), 0,
                          TXT, G.r14, 13, 6)) {
                std::string path = r.libraryPath.empty() ? stack::StackConfig::get().moviesPath : r.libraryPath;
                platform::openPath(path);
            }
        }
        y += 84;
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, y + 8));
}

void renderSettings() {
    initColors();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    dl->AddText(G.sb26, 24, ImVec2(origin.x, origin.y), TXT, "Ustawienia");
    dl->AddText(G.r14, 13, ImVec2(origin.x, origin.y + 32), ATTR,
                "Wszystko działa w jednej apce — wyszukiwanie torrentów + libtorrent + biblioteka");

    auto& cfg = stack::StackConfig::get();
    static char movies[512], tv[512], dlpath[512];
    static bool loaded = false;
    if (!loaded) {
        std::snprintf(movies, sizeof(movies), "%s", cfg.moviesPath.c_str());
        std::snprintf(tv, sizeof(tv), "%s", cfg.tvPath.c_str());
        std::snprintf(dlpath, sizeof(dlpath), "%s", cfg.downloadPath.c_str());
        loaded = true;
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + 64));
    ImGui::PushItemWidth(std::min(560.0f, w - 20));
    ImGui::TextUnformatted("Filmy (biblioteka)");
    ImGui::InputText("##movies", movies, sizeof(movies));
    ImGui::TextUnformatted("Seriale (biblioteka)");
    ImGui::InputText("##tv", tv, sizeof(tv));
    ImGui::TextUnformatted("Folder pobierania");
    ImGui::InputText("##dl", dlpath, sizeof(dlpath));
    ImGui::Checkbox("Auto-start pobierania", &cfg.autoStart);
    ImGui::SliderInt("Min. seeders", &cfg.minSeeders, 0, 50);
    ImGui::PopItemWidth();

    ImGui::Spacing();
    if (ImGui::Button("Zapisz", ImVec2(140, 36))) {
        cfg.moviesPath = movies;
        cfg.tvPath = tv;
        cfg.downloadPath = dlpath;
        cfg.save();
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("Zapis: %APPDATA%\\SeerrCpp\\stack.json");
}
