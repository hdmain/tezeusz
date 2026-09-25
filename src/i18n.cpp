#include "i18n.hpp"
#include "json.hpp"
#include "util.hpp"
#include <map>
#include <mutex>

using json = nlohmann::json;

namespace i18n {
namespace {

// Function-local statics avoid static-initialization-order fiasco when another
// TU calls i18n::tr() from a global constructor (e.g. old subs g_status).
std::mutex& mu() {
    static std::mutex m;
    return m;
}
std::string& lang() {
    static std::string s = "en";
    return s;
}
std::map<std::string, std::string>& enDict() {
    static std::map<std::string, std::string> m;
    return m;
}
std::map<std::string, std::string>& curDict() {
    static std::map<std::string, std::string> m;
    return m;
}
std::string& fallbackBuf() {
    static std::string s;
    return s;
}

void seedEnglish() {
    auto& g_en = enDict();
    if (!g_en.empty()) return;
    // Nav
    g_en["nav.discover"] = "Discover";
    g_en["nav.movies"] = "Movies";
    g_en["nav.tv"] = "TV Shows";
    g_en["nav.requests"] = "Requests";
    g_en["nav.library"] = "Library";
    g_en["nav.blocklist"] = "Blocklist";
    g_en["nav.issues"] = "Issues";
    g_en["nav.users"] = "Users";
    g_en["nav.settings"] = "Settings";
    g_en["nav.back"] = "« Back";
    g_en["nav.coming_soon"] = "Coming soon";

    // Common
    g_en["common.loading"] = "Loading…";
    g_en["common.error"] = "Error";
    g_en["common.retry_f5"] = "Press F5 to retry, or check the TMDB key in config.json.";
    g_en["common.load_error"] = "Load error";
    g_en["common.refresh"] = "Refresh";
    g_en["common.cancel"] = "Cancel";
    g_en["common.delete"] = "Delete";
    g_en["common.play"] = "Play";
    g_en["common.open_folder"] = "Open folder";
    g_en["common.properties"] = "Properties";
    g_en["common.submit"] = "Submit";
    g_en["common.none"] = "None";
    g_en["common.title"] = "Title";
    g_en["common.quality"] = "Quality";
    g_en["common.save"] = "Save";
    g_en["common.movie"] = "MOVIE";
    g_en["common.tv"] = "TV";
    g_en["common.person"] = "PERSON";
    g_en["common.request"] = "Request";
    g_en["common.available"] = "Available";
    g_en["common.failed_retry"] = "Failed — retry";
    g_en["common.untitled"] = "(untitled)";
    g_en["common.prev"] = "« Back";
    g_en["common.next"] = "Next »";
    g_en["common.close"] = "Close";
    g_en["common.in_progress"] = "In progress…";
    g_en["common.type"] = "Type";
    g_en["common.description"] = "Description";
    g_en["common.all"] = "All";
    g_en["common.search_short"] = "Search…";

    // Search / discover
    g_en["search.hint"] = "Search movies, TV shows, people…";
    g_en["search.title"] = "Search";
    g_en["search.results_title"] = "Search results";
    g_en["search.empty_title"] = "Type a movie, TV show, or person name…";
    g_en["search.empty_sub"] = "Results appear as you type — like Seerr / Jellyseerr.";
    g_en["search.results"] = "results";
    g_en["search.error"] = "error";
    g_en["search.no_results"] = "No results. Try a different query or filter.";
    g_en["search.searching"] = "Searching…";
    g_en["search.people"] = "People";
    g_en["discover.trending"] = "Trending";
    g_en["discover.trending_day"] = "Trending today";
    g_en["discover.upcoming"] = "Coming soon to theaters";
    g_en["discover.popular_movies"] = "Popular movies";
    g_en["discover.now_playing"] = "Now playing";
    g_en["discover.top_movies"] = "Top rated movies";
    g_en["discover.popular_tv"] = "Popular TV shows";
    g_en["discover.on_the_air"] = "On the air";
    g_en["discover.airing_today"] = "Airing today";
    g_en["discover.top_tv"] = "Top rated TV shows";
    g_en["discover.load_failed"] = "Failed to load";

    // Details
    g_en["details.load_error"] =
        "Failed to load details (check TMDB key in config.json)  (F5 = retry)";
    g_en["details.request"] = "Request";
    g_en["details.watchlist_add"] = "☆ Add to watchlist";
    g_en["details.watchlist_remove"] = "★ Remove from watchlist";
    g_en["details.report"] = "Report issue";
    g_en["details.overview"] = "Overview";
    g_en["details.budget"] = "Budget";
    g_en["details.revenue"] = "Revenue";
    g_en["details.languages"] = "Languages";
    g_en["details.original_title"] = "Original title";
    g_en["details.status_planned"] = "Planned";
    g_en["details.status_rumored"] = "Rumored";
    g_en["details.status_continuing"] = "Continuing";
    g_en["details.status_ended"] = "Ended";
    g_en["details.status_released"] = "Released";
    g_en["details.status_post"] = "Post Production";
    g_en["details.status_in_production"] = "In Production";
    g_en["details.trailer"] = "Trailer";
    g_en["details.block"] = "Block";
    g_en["details.unblock"] = "Unblock";
    g_en["details.no_overview"] = "No overview.";
    g_en["details.studios"] = "Production companies";
    g_en["details.cast"] = "Cast";
    g_en["details.seasons"] = "Seasons";
    g_en["details.episodes_short"] = " eps.";
    g_en["details.recommendations"] = "Recommendations";
    g_en["details.similar"] = "Similar";
    g_en["issue.video"] = "Video";
    g_en["issue.audio"] = "Audio";
    g_en["issue.subtitles"] = "Subtitles";
    g_en["issue.other"] = "Other";

    // Stub / blocklist / issues / users
    g_en["stub.not_wired"] = "This section is not wired to the local stack yet.";
    g_en["blocklist.title"] = "Blocklist";
    g_en["blocklist.sub"] = "Manage blocked titles (like in Seerr).";
    g_en["blocklist.empty"] = "No blocked titles. Use “Block” on a details page.";
    g_en["blocklist.blocked"] = "Blocked";
    g_en["issues.title"] = "Issues";
    g_en["issues.sub"] = "Media issue reports (like in Seerr).";
    g_en["issues.empty"] = "No issues. Report them from a movie/TV details page.";
    g_en["issues.open"] = "Open";
    g_en["issues.resolved"] = "Resolved";
    g_en["issues.resolve"] = "Resolve";
    g_en["issues.reopen"] = "Reopen";
    g_en["users.title"] = "Users";
    g_en["users.sub"] =
        "Local app owner (no Jellyfin import — everything in one program).";
    g_en["users.display_name"] = "Display name";
    g_en["users.email"] = "Email";
    g_en["users.local_profile"] = "Local profile";
    g_en["users.save_profile"] = "Save profile";
    g_en["users.local"] = "Local";
    g_en["users.list"] = "User list";
    g_en["users.col_user"] = "User";
    g_en["users.col_requests"] = "Requests";
    g_en["users.col_type"] = "Type";
    g_en["users.col_role"] = "Role";
    g_en["users.owner"] = "Owner";

    // Requests / library
    g_en["requests.title"] = "Requests";
    g_en["requests.sub"] = "Search → libtorrent → library (no external *arr / qBit)";
    g_en["requests.empty"] = "No requests. Click “Request” on a movie or TV show.";
    g_en["library.title"] = "Library";
    g_en["library.sub"] = "Click to watch · right-click for options (delete, properties…)";
    g_en["library.titles"] = "titles";
    g_en["library.empty"] =
        "Library is empty. Request a movie and wait for the download — it will show up here.";
    g_en["library.movies"] = "Movies";
    g_en["library.tv"] = "TV Shows";
    g_en["library.delete_title"] = "Remove from library";
    g_en["library.delete_confirm"] =
        "Delete “%s” from disk? This cannot be undone.";
    g_en["library.delete_failed"] = "Failed to delete";
    g_en["library.size"] = "Size";
    g_en["library.container"] = "Container";
    g_en["library.modified"] = "Modified";
    g_en["library.file"] = "File";
    g_en["library.folder"] = "Folder";
    g_en["library.type_movie"] = "Movie";
    g_en["library.type_tv"] = "TV Show";

    // Settings
    g_en["settings.title"] = "Settings";
    g_en["settings.sub"] =
        "Everything runs in one app — torrent search + libtorrent + library";
    g_en["settings.tab_general"] = "General";
    g_en["settings.tab_library"] = "Library";
    g_en["settings.tab_downloads"] = "Downloads";
    g_en["settings.tab_subs"] = "Subtitles";
    g_en["settings.tab_updates"] = "Updates";
    g_en["settings.general_hint"] = "Language and app preferences.";
    g_en["settings.library_hint"] = "Folders where finished movies and TV shows are stored.";
    g_en["settings.downloads_hint"] = "Where torrents download before import into the library.";
    g_en["settings.save_hint"] = "Config file: %APPDATA%\\SeerrCpp\\stack.json (Linux: ~/.local/share/SeerrCpp).";
    g_en["settings.saved_toast"] = "Saved to disk";
    g_en["settings.ui_language"] = "UI language";
    g_en["settings.movies_path"] = "Movies folder";
    g_en["settings.tv_path"] = "TV shows folder";
    g_en["settings.download_path"] = "Download folder";
    g_en["settings.auto_start"] = "Start downloads automatically";
    g_en["settings.min_seeders"] = "Minimum seeders";
    g_en["settings.default_quality"] = "Default quality";
    g_en["settings.quality_any"] = "Any";
    g_en["settings.subs"] = "Subtitles";
    g_en["settings.subs_hint1"] =
        "Works without a key via TheSubDB (EN) and NapiProjekt (PL) using the video hash.";
    g_en["settings.subs_hint2"] =
        "An OpenSubtitles key unlocks more languages and TV episodes (optional).";
    g_en["settings.subs_os_section"] = "OpenSubtitles (optional)";
    g_en["settings.subs_os_hint"] =
        "Get a free API key at opensubtitles.com/consumers — a free account is enough.";
    g_en["settings.subs_api_key"] = "API key";
    g_en["settings.subs_login"] = "Username";
    g_en["settings.subs_auto"] = "Download subtitles automatically";
    g_en["settings.subs_lang"] = "Preferred language";
    g_en["settings.subs_password"] = "Password";
    g_en["settings.subs_save"] = "Save subtitle login";
    g_en["settings.scan_library"] = "Scan library";
    g_en["update.settings_title"] = "Updates";
    g_en["update.settings_hint"] =
        "Checks GitHub in the background and downloads only changed files (SHA-256).";
    g_en["update.check_now"] = "Check for updates";
    g_en["update.current"] = "Installed:";
    g_en["update.remote"] = "Available:";
    g_en["update.idle"] = "Automatic updates enabled";
    g_en["update.checking"] = "Checking for updates…";
    g_en["update.up_to_date"] = "You are up to date";
    g_en["update.downloading"] = "Downloading update";
    g_en["update.ready"] = "Update ready — restarting…";
    g_en["update.applying"] = "Applying update…";
    g_en["update.disabled"] = "Updates disabled";
    g_en["update.not_writable"] = "Install folder is not writable — auto-update unavailable";
    g_en["update.check_failed"] = "Could not check for updates";
    g_en["update.bad_manifest"] = "Invalid update manifest";
    g_en["update.bad_platform"] = "Update is for a different platform";
    g_en["update.download_failed"] = "Download failed";
    g_en["update.checksum_failed"] = "Checksum mismatch";
    g_en["update.write_failed"] = "Could not write update files";
    g_en["update.apply_failed"] = "Could not start update installer";

    // Request dialog
    g_en["req.dialog_title"] = "Request";
    g_en["req.preferred_quality"] = "Preferred quality";
    g_en["req.original"] = "Original: %s";
    g_en["req.loading_seasons"] = "Loading seasons…";
    g_en["req.no_seasons"] = "No season list. Try again from the TV show page.";
    g_en["req.none_seasons"] = "None";
    g_en["req.all_seasons"] = "All";
    g_en["req.loading_episodes"] = "Loading episodes…";
    g_en["req.no_episodes"] = "No episodes in this season.";
    g_en["req.none_episodes"] = "No episodes";
    g_en["req.all_episodes"] = "All episodes";
    g_en["req.pick_episodes"] = "Pick specific episodes";
    g_en["req.need_season"] = "Select at least one season.";
    g_en["req.multi_season"] =
        "Multiple seasons — whole seasons will be downloaded (no episode pick).";
    g_en["req.interactive"] = "Interactive Search — release list, you pick.";
    g_en["req.interactive_title"] = "Interactive Search";
    g_en["req.interactive_btn"] = "Interactive";
    g_en["req.auto"] = "Automatic";
    g_en["req.download"] = "Download";
    g_en["req.releases_count"] = "releases";
    g_en["req.searching"] = "Searching releases…";
    g_en["req.no_hits"] = "No results. Change quality and refresh.";
    g_en["req.search_error"] = "Search failed";
    g_en["req.col_title"] = "Title";
    g_en["req.col_quality"] = "Quality";
    g_en["req.col_size"] = "Size";

    // Request status labels
    g_en["status.pending"] = "Pending";
    g_en["status.searching"] = "Searching";
    g_en["status.downloading"] = "Downloading";
    g_en["status.importing"] = "Importing";
    g_en["status.available"] = "Available";
    g_en["status.failed"] = "Failed";
    g_en["status.declined"] = "Cancelled";

    // Stack messages
    g_en["stack.already_library"] = "Already in library";
    g_en["stack.searching_releases"] = "Searching releases…";
    g_en["stack.no_releases"] = "No torrent releases found for: ";
    g_en["stack.downloading"] = "Downloading: ";
    g_en["stack.seeders"] = " seeders)";
    g_en["stack.resuming"] = "Resuming: ";
    g_en["stack.download_failed"] = "Download failed (no video file)";
    g_en["stack.importing"] = "Copying to library…";
    g_en["stack.downloaded_folder"] = "Downloaded (download folder)";
    g_en["stack.unknown_error"] = "Unknown error";
    g_en["stack.retrying"] = "Retrying…";
    g_en["stack.picked_queued"] = "Release selected — queued";
    g_en["stack.cancelled"] = "Cancelled";
    g_en["stack.removed_library"] = "Removed from library";
    g_en["stack.progress_seed"] =
        "%s… %.0f/%.0f MB · %d peers (%d seed) · %.0f KB/s · ETA %s";
    g_en["stack.progress"] = "%s… %.0f/%.0f MB · %d peers · %.0f KB/s · ETA %s";
    g_en["stack.season"] = "Season %02d";

    // Subtitles status
    g_en["subs.idle"] = "Subtitles: idle";
    g_en["subs.thesubdb"] = "Subtitles: TheSubDB (hash)…";
    g_en["subs.saved"] = "Subtitles: saved ";
    g_en["subs.napi"] = "Subtitles: NapiProjekt (hash)…";
    g_en["subs.need_key"] = "Subtitles: set OpenSubtitles API key";
    g_en["subs.need_login"] =
        "Subtitles: set OpenSubtitles login (required for download)";
    g_en["subs.logging_in"] = "Subtitles: signing in…";
    g_en["subs.login_error"] = "Subtitles: login error (";
    g_en["subs.no_token"] = "Subtitles: no token in response";
    g_en["subs.bad_login"] = "Subtitles: bad login response";
    g_en["subs.weak_match"] = "Subtitles: weak match (";
    g_en["subs.searching"] = "Subtitles: searching ";
    g_en["subs.search_error"] = "Subtitles: search error ";
    g_en["subs.bad_search"] = "Subtitles: bad search response";
    g_en["subs.no_match"] = "Subtitles: no good match ";
    g_en["subs.downloading"] = "Subtitles: downloading ";
    g_en["subs.session_expired"] = "Subtitles: session expired — try again";
    g_en["subs.daily_limit"] = "Subtitles: OpenSubtitles daily limit reached";
    g_en["subs.download_error"] = "Subtitles: download error (";
    g_en["subs.no_link"] = "Subtitles: no download link";
    g_en["subs.fetch_failed"] = "Subtitles: failed to fetch file (";
    g_en["subs.not_srt"] = "Subtitles: downloaded file does not look like SRT";
    g_en["subs.save_failed"] = "Subtitles: could not save ";
    g_en["subs.missing"] = "Subtitles: no ";
    g_en["subs.need_os_key"] = " — add OpenSubtitles key for more sources";
    g_en["subs.exception"] = "Subtitles: exception while downloading";
    g_en["subs.done"] = "Subtitles: done";
    g_en["subs.queued_scan"] = "Subtitles: scan queued (";
    g_en["subs.queued_items"] = " items)";
    g_en["subs.auto_off"] = "Subtitles: auto disabled";
    g_en["subs.scanning"] = "Subtitles: scanning library…";
    g_en["subs.weak_marker"] = "weak match";

    // Player / quit
    g_en["player.loading"] = "Loading…";
    g_en["player.error"] = "Playback error";
    g_en["player.subtitles"] = "Subtitles";
    g_en["player.settings"] = "Settings";
    g_en["player.open_failed"] = "Could not open";
    g_en["player.file_missing"] = "File does not exist";
    g_en["player.file_open_failed"] = "Could not open file";
    g_en["player.open_external"] = "Open externally";
    g_en["player.audio"] = "Audio";
    g_en["player.audio_track"] = "Audio track";
    g_en["player.window"] = "Window";
    g_en["player.fullscreen"] = "Fullscreen";
    g_en["player.off"] = "Off";
    g_en["player.stats"] = "Stats for nerds";
    g_en["player.hide_stats"] = "Hide Stats for nerds";
    g_en["player.shortcuts"] =
        "Space pause | ←/→ ±10s | F fullscreen | M mute | C subtitles | ` / Ctrl+Shift+I stats | Esc";
    g_en["player.no_vlc"] = "libVLC not found — install VLC or place libvlc next to seerr.exe";
    g_en["player.vlc_load_failed"] = "Failed to load libVLC";
    g_en["yt.connecting"] = "Connecting to YouTube…";
    g_en["yt.loading"] = "Loading trailer…";
    g_en["yt.window_title"] = "Trailer — Seerr";
    g_en["yt.vlc_fail"] = "VLC failed to play trailer";
    g_en["yt.timeout"] = "Trailer load timeout";
    g_en["quit.closing"] = "Shutting down…";
    g_en["quit.title"] = "Shutting down Seerr…";
    g_en["quit.ui"] = "Releasing icons and UI…";
    g_en["quit.torrents"] = "Stopping torrent downloads…";
    g_en["quit.subs"] = "Stopping subtitles…";
    g_en["quit.jobs"] = "Stopping background jobs…";
    g_en["quit.cache"] = "Closing image cache and P2P…";
    g_en["quit.finishing"] = "Finishing…";
    g_en["quit.error"] = "Error while shutting down…";
    g_en["tray.show"] = "Show Seerr";
    g_en["tray.quit"] = "Quit";
    g_en["tray.tip"] = "Seerr";

    // Torrent
    g_en["torrent.empty_magnet"] = "Empty magnet";
    g_en["torrent.bad_magnet"] = "Bad magnet: ";

    // Months (English display)
    g_en["month.1"] = "January";
    g_en["month.2"] = "February";
    g_en["month.3"] = "March";
    g_en["month.4"] = "April";
    g_en["month.5"] = "May";
    g_en["month.6"] = "June";
    g_en["month.7"] = "July";
    g_en["month.8"] = "August";
    g_en["month.9"] = "September";
    g_en["month.10"] = "October";
    g_en["month.11"] = "November";
    g_en["month.12"] = "December";
}

std::string tryReadLocale(const std::string& code) {
    const std::string name = code + ".json";
    std::vector<std::string> paths = {
        util::localeDir() + "/" + name,
        util::exeDir() + "/locales/" + name,
        util::appDataPath("locales/" + name),
    };
#ifdef APP_LOCALE_DIR
    paths.push_back(std::string(APP_LOCALE_DIR) + "/" + name);
#endif
    for (auto& p : paths) {
        std::string s = util::readFile(p);
        if (!s.empty()) return s;
    }
    return {};
}

void loadOverlay(const std::string& code) {
    auto& g_cur = curDict();
    g_cur = enDict();
    if (code.empty() || code == "en") return;
    std::string raw = tryReadLocale(code);
    if (raw.empty()) return;
    try {
        auto j = json::parse(raw);
        if (!j.is_object()) return;
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.value().is_string())
                g_cur[it.key()] = it.value().get<std::string>();
        }
    } catch (...) {}
}

std::string readUiLangFromConfig() {
    auto tryFile = [](const std::string& p) -> std::string {
        std::string s = util::readFile(p);
        if (s.empty()) return {};
        try {
            auto j = json::parse(s);
            if (j.contains("ui_language") && j["ui_language"].is_string())
                return j["ui_language"].get<std::string>();
            if (j.contains("uiLanguage") && j["uiLanguage"].is_string())
                return j["uiLanguage"].get<std::string>();
        } catch (...) {}
        return {};
    };
    std::string v = tryFile(util::appDataPath("stack.json"));
    if (!v.empty()) return v;
    v = tryFile(util::exeDir() + "/config.json");
    if (!v.empty()) return v;
    return tryFile(util::appDataPath("config.json"));
}

} // namespace

void init() {
    std::lock_guard<std::mutex> lk(mu());
    seedEnglish();
    std::string code = readUiLangFromConfig();
    if (code.empty()) code = "en";
    lang() = code;
    loadOverlay(lang());
}

void setLanguage(const std::string& code) {
    std::lock_guard<std::mutex> lk(mu());
    seedEnglish();
    lang() = code.empty() ? "en" : code;
    loadOverlay(lang());
}

const std::string& language() {
    std::lock_guard<std::mutex> lk(mu());
    return lang();
}

std::vector<std::string> availableLanguages() {
    return {"en", "pl"};
}

const char* tr(const char* key) {
    if (!key) return "";
    std::lock_guard<std::mutex> lk(mu());
    seedEnglish();
    auto& g_cur = curDict();
    auto& g_en = enDict();
    auto it = g_cur.find(key);
    if (it != g_cur.end()) return it->second.c_str();
    auto en = g_en.find(key);
    if (en != g_en.end()) return en->second.c_str();
    fallbackBuf() = key;
    return fallbackBuf().c_str();
}

} // namespace i18n
