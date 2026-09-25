#pragma once
#include "tmdb.hpp"
#include <set>
#include <map>
#include <string>

enum class Page {
    Discover, Movies, Tv, Genre, Search, MovieDetails, TvDetails, TrendingPage,
    Requests, Library, Blocklist, Issues, Users, Settings, Stub
};

struct GenreSel { int id = 0; std::string name; bool tv = false; };

struct App {
    Page page = Page::Discover;
    Page prevPage = Page::Discover;

    // details target
    MediaType detailType = MediaType::Movie;
    int detailId = 0;

    GenreSel genre;

    // search (Seerr-style: live query + type filter chips)
    std::string searchInput;
    std::string lastQuery;
    SearchFilter searchFilter = SearchFilter::All;
    AsyncReq<PagedResult> searchReq;
    bool focusSearchInput = false; // set when navigating home→search mid-typing

    // watchlist (tmdb id set per type), requests status (0 unknown..3 available)
    std::set<std::string> watchlist;              // key "movie:123"
    std::map<std::string, int> requestStatus;     // key -> status

    // cached genre lists
    AsyncReq<std::vector<Genre>> movieGenresReq, tvGenresReq;
    std::vector<Genre> movieGenres, tvGenres;

    // discover sliders (home)
    std::vector<AsyncReq<PagedResult>> sliders;

    static std::string key(MediaType t, int id) { return std::string(mediaTypeName(t)) + ":" + std::to_string(id); }
    bool isWatchlisted(MediaType t, int id) { return watchlist.count(key(t, id)) > 0; }
    int statusOf(MediaType t, int id) { auto it = requestStatus.find(key(t, id)); return it == requestStatus.end() ? 0 : it->second; }

    void navigate(Page p) { prevPage = page; page = p; }
    void openDetails(MediaType t, int id) {
        detailType = t; detailId = id;
        navigate(t == MediaType::TV ? Page::TvDetails : Page::MovieDetails);
    }
    void back() { page = prevPage; }

    // bumped on F5; pages compare to their local copy and re-fetch
    unsigned reloadSeq = 0;
    bool shouldReload(unsigned& seen) { if (seen != reloadSeq) { seen = reloadSeq; return true; } return false; }
};

App& app();

// TMDB api key config (config.json next to exe / in %APPDATA%\SeerrCpp, field "tmdb_api_key";
// env var TMDB_API_KEY also honored). Content language follows UI (stack.json uiLanguage);
// missing localized fields fall back to English.
namespace cfg {
const char* apiKey();
const char* language();
void setLanguage(const std::string& tmdbLang); // e.g. "pl-PL", "en-US"
void syncFromUi(const std::string& uiCode);    // "pl" → pl-PL, else en-US
}

// page render entry points (pages.cpp)
void renderDiscover();
void renderDiscoverGrid(bool tv);
void renderGenrePage();
void renderSearch();
void renderTrendingPage();
void renderDetails(MediaType type, int id);
void renderStub(const char* what);
void renderRequests();
void renderLibrary();
void renderBlocklist();
void renderIssues();
void renderUsers();
void renderSettings();
void renderRequestQualityDialog(); // modal: jakość + sezony/odcinki
void openRequestQualityDialog(MediaType type, int id, const std::string& title,
                              const std::string& year, const std::string& imdbId,
                              const std::string& originalTitle,
                              const std::vector<SeasonInfo>& availableSeasons = {});

// GLFW Win32 HWND for owned popups (trailer player). Set from main.
void* appMainHwnd();
void setAppMainHwnd(void* hwnd);

// (chrome is internal to main.cpp)
