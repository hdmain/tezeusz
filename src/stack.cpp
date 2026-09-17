#include "stack.hpp"
#include "app.hpp"
#include "core.hpp"
#include "http.hpp"
#include "torrent.hpp"
#include "util.hpp"
#include "json.hpp"

#include <filesystem>
#include <thread>
#include <atomic>
#include <deque>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <cctype>
#include <cstdio>
#include <cstring>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace stack {

void syncAppStatuses();

std::recursive_mutex g_mu;
StackConfig g_cfg;
std::vector<MediaRequest> g_reqs;
std::deque<std::string> g_queue;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_pumpScheduled{false};

namespace {

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string newId() {
    static std::atomic<uint32_t> n{1};
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%llx-%u", (unsigned long long)nowMs(), n.fetch_add(1));
    return buf;
}

std::string storePath() { return util::appDataPath("requests.json"); }
std::string cfgPath() { return util::appDataPath("stack.json"); }

std::string toLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

void saveRequestsLocked() {
    json arr = json::array();
    for (auto& r : g_reqs) {
        arr.push_back({
            {"id", r.id}, {"mediaType", r.mediaType == MediaType::TV ? "tv" : "movie"},
            {"tmdbId", r.tmdbId}, {"title", r.title}, {"originalTitle", r.originalTitle},
            {"year", r.year}, {"imdbId", r.imdbId},
            {"seasons", r.seasons}, {"status", (int)r.status}, {"message", r.message},
            {"releaseTitle", r.releaseTitle}, {"magnetOrUrl", r.magnetOrUrl},
            {"torrentHash", r.torrentHash}, {"libraryPath", r.libraryPath},
            {"progress", r.progress}, {"createdAt", r.createdAt}, {"updatedAt", r.updatedAt}
        });
    }
    util::writeFile(storePath(), arr.dump(2));
}

MediaRequest* findReqLocked(const std::string& id) {
    for (auto& r : g_reqs) if (r.id == id) return &r;
    return nullptr;
}

void setStatus(MediaRequest& r, ReqStatus s, const std::string& msg = {}) {
    r.status = s;
    if (!msg.empty()) r.message = msg;
    r.updatedAt = nowMs();
}

void loadRequests() {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    g_reqs.clear();
    g_queue.clear();
    std::string raw = util::readFile(storePath());
    if (raw.empty()) return;
    try {
        for (auto& j : json::parse(raw)) {
            MediaRequest r;
            r.id = j.value("id", newId());
            r.mediaType = j.value("mediaType", "movie") == "tv" ? MediaType::TV : MediaType::Movie;
            r.tmdbId = j.value("tmdbId", 0);
            r.title = j.value("title", "");
            r.originalTitle = j.value("originalTitle", "");
            r.year = j.value("year", "");
            r.imdbId = j.value("imdbId", "");
            if (j.contains("seasons") && j["seasons"].is_array())
                r.seasons = j["seasons"].get<std::vector<int>>();
            r.status = (ReqStatus)j.value("status", 0);
            r.message = j.value("message", "");
            r.releaseTitle = j.value("releaseTitle", "");
            r.magnetOrUrl = j.value("magnetOrUrl", "");
            r.torrentHash = j.value("torrentHash", "");
            r.libraryPath = j.value("libraryPath", "");
            r.progress = j.value("progress", 0.0);
            r.createdAt = j.value("createdAt", nowMs());
            r.updatedAt = j.value("updatedAt", r.createdAt);
            if (r.status == ReqStatus::Pending || r.status == ReqStatus::Searching ||
                r.status == ReqStatus::Downloading || r.status == ReqStatus::Importing)
                g_queue.push_back(r.id);
            g_reqs.push_back(std::move(r));
        }
    } catch (...) {}
}

// -------------------- built-in release search (apibay / TPB mirror) --------------------

struct Release {
    std::string title, magnet, indexer;
    int seeders = 0, score = 0;
    long long size = 0;
};

int qualityScore(const std::string& title, const std::string& pref) {
    std::string t = toLower(title);
    int base = 100;
    if (t.find("2160p") != std::string::npos || t.find("4k") != std::string::npos) base = 400;
    else if (t.find("1080p") != std::string::npos) base = 300;
    else if (t.find("720p") != std::string::npos) base = 200;
    else if (t.find("480p") != std::string::npos) base = 50;

    std::string p = toLower(pref);
    if (p == "2160p" && base >= 400) base += 200;
    else if (p == "1080p" && base == 300) base += 200;
    else if (p == "720p" && base == 200) base += 200;

    if (t.find("bluray") != std::string::npos || t.find("blu-ray") != std::string::npos) base += 40;
    if (t.find("web-dl") != std::string::npos || t.find("webdl") != std::string::npos) base += 30;
    if (t.find("webrip") != std::string::npos) base += 20;
    if (t.find("x265") != std::string::npos || t.find("hevc") != std::string::npos) base += 15;
    if (t.find("cam") != std::string::npos || t.find("hdcam") != std::string::npos ||
        t.find("telesync") != std::string::npos) base -= 500;
    return base;
}

bool titleMatches(const std::string& release, const std::string& wantTitle) {
    std::string r = toLower(release), t = toLower(wantTitle), norm;
    for (char c : t) norm += (std::isalnum((unsigned char)c) ? c : ' ');
    std::istringstream iss(norm);
    std::string tok;
    int hit = 0, need = 0;
    while (iss >> tok) {
        if (tok.size() < 2) continue;
        need++;
        if (r.find(tok) != std::string::npos) hit++;
    }
    if (need == 0) return true;
    return hit >= std::max(1, (need + 1) / 2);
}

std::string makeMagnet(const std::string& infoHash, const std::string& name) {
    std::string m = "magnet:?xt=urn:btih:" + infoHash + "&dn=" + util::urlEncode(name);
    m += "&tr=" + util::urlEncode("udp://tracker.opentrackr.org:1337/announce");
    m += "&tr=" + util::urlEncode("udp://open.stealth.si:80/announce");
    m += "&tr=" + util::urlEncode("udp://tracker.openbittorrent.com:6969/announce");
    m += "&tr=" + util::urlEncode("udp://exodus.desync.com:6969/announce");
    return m;
}

std::vector<Release> searchApibay(const std::string& query) {
    std::vector<Release> out;
    auto r = http::get("https://apibay.org/q.php?q=" + util::urlEncode(query), "application/json");
    if (!r.ok() || r.body.empty() || r.body == "[]" || r.body.find("No results") != std::string::npos)
        return out;
    try {
        auto arr = json::parse(r.body);
        if (!arr.is_array()) return out;
        for (auto& j : arr) {
            // apibay returns id=="0" name=="No results." on miss
            std::string id = j.value("id", "");
            std::string name = j.value("name", "");
            if (id == "0" || name.empty() || name == "No results.") continue;
            std::string hash = j.value("info_hash", "");
            if (hash.empty()) continue;
            Release rel;
            rel.title = name;
            rel.magnet = makeMagnet(hash, name);
            rel.indexer = "builtin";
            rel.seeders = 0;
            try { rel.seeders = std::stoi(j.value("seeders", "0")); } catch (...) {}
            try { rel.size = std::stoll(j.value("size", "0")); } catch (...) {}
            out.push_back(std::move(rel));
        }
    } catch (...) {}
    return out;
}

void mergeReleases(std::vector<Release>& into, std::vector<Release> add) {
    for (auto& r : add) {
        bool dup = false;
        for (auto& e : into) {
            if (e.magnet == r.magnet || e.title == r.title) { dup = true; break; }
        }
        if (!dup) into.push_back(std::move(r));
    }
}

// Radarr/Prowlarr-style: IMDb id first, then original (EN) title + year, then localized.
std::vector<Release> searchReleases(const MediaRequest& req) {
    auto& cfg = StackConfig::get();
    std::string seasonSuffix;
    if (req.mediaType == MediaType::TV && !req.seasons.empty()) {
        char b[16];
        std::snprintf(b, sizeof(b), " S%02d", req.seasons[0]);
        seasonSuffix = b;
    }

    const std::string& orig = !req.originalTitle.empty() ? req.originalTitle : req.title;
    std::vector<Release> all;
    bool fromImdb = false;

    // 1) IMDb id — strongest signal (apibay indexes tt…)
    if (!req.imdbId.empty()) {
        auto hit = searchApibay(req.imdbId);
        if (!hit.empty()) { mergeReleases(all, std::move(hit)); fromImdb = true; }
    }

    // 2) originalTitle + year (what Radarr sends to indexers)
    if (all.empty() || all.size() < 3) {
        if (!orig.empty()) {
            std::string q = orig + (req.year.empty() ? "" : " " + req.year) + seasonSuffix;
            mergeReleases(all, searchApibay(q));
        }
    }

    // 3) originalTitle alone
    if (all.empty() && !orig.empty())
        mergeReleases(all, searchApibay(orig + seasonSuffix));

    // 4) localized title fallback (Polish etc.)
    if (all.empty() && !req.title.empty() && req.title != orig) {
        std::string q = req.title + (req.year.empty() ? "" : " " + req.year) + seasonSuffix;
        mergeReleases(all, searchApibay(q));
        if (all.empty()) mergeReleases(all, searchApibay(req.title + seasonSuffix));
    }

    std::vector<Release> filtered;
    for (auto& r : all) {
        // IMDb hits are trusted; otherwise require token match vs original or localized title
        if (!fromImdb) {
            bool ok = titleMatches(r.title, orig) ||
                      (!req.title.empty() && titleMatches(r.title, req.title));
            if (!ok) continue;
        } else if (!orig.empty() && !titleMatches(r.title, orig) &&
                   !req.title.empty() && !titleMatches(r.title, req.title)) {
            // Soft prefer matching titles when mixed bag; keep high-seeder IMDb hits anyway
            if (r.seeders < 5) continue;
        }
        if (r.seeders > 0 && r.seeders < cfg.minSeeders) continue;
        r.score = qualityScore(r.title, cfg.preferredQuality) + std::min(r.seeders, 200);
        if (r.size > 0 && r.size < 40ll * 1024 * 1024) r.score -= 80;
        // Prefer year in release name when we have one
        if (!req.year.empty() && toLower(r.title).find(req.year) != std::string::npos)
            r.score += 50;
        filtered.push_back(std::move(r));
    }
    std::sort(filtered.begin(), filtered.end(), [](auto& a, auto& b) { return a.score > b.score; });
    return filtered;
}

// Fill missing imdbId / originalTitle from TMDB (same data Radarr uses).
void enrichFromTmdb(MediaRequest& req) {
    if (!req.imdbId.empty() && !req.originalTitle.empty()) return;
    if (req.tmdbId <= 0) return;
    std::string path = req.mediaType == MediaType::TV
        ? "/tv/" + std::to_string(req.tmdbId)
        : "/movie/" + std::to_string(req.tmdbId);
    std::string url = std::string("https://api.themoviedb.org/3") + path +
        "?api_key=" + cfg::apiKey() +
        "&append_to_response=external_ids&language=en-US";
    auto r = http::get(url, "application/json");
    if (!r.ok()) return;
    try {
        auto j = json::parse(r.body);
        if (req.originalTitle.empty()) {
            req.originalTitle = j.value("original_title", "");
            if (req.originalTitle.empty())
                req.originalTitle = j.value("original_name", "");
        }
        if (req.imdbId.empty()) {
            req.imdbId = j.value("imdb_id", "");
            if (req.imdbId.empty() && j.contains("external_ids"))
                req.imdbId = j["external_ids"].value("imdb_id", "");
        }
        if (req.year.empty()) {
            std::string d = j.value("release_date", "");
            if (d.empty()) d = j.value("first_air_date", "");
            if (d.size() >= 4) req.year = d.substr(0, 4);
        }
    } catch (...) {}
}

bool isVideoFile(const fs::path& p) {
    auto e = toLower(p.extension().string());
    return e == ".mkv" || e == ".mp4" || e == ".avi" || e == ".m4v" || e == ".ts" || e == ".m2ts";
}

std::string findBiggestVideo(const fs::path& root) {
    std::string best;
    uintmax_t bestSz = 0;
    std::error_code ec;
    if (!fs::exists(root, ec)) return {};
    if (fs::is_regular_file(root, ec) && isVideoFile(root)) return root.string();
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;
        if (!isVideoFile(it->path())) continue;
        auto sz = it->file_size(ec);
        if (sz > bestSz) { bestSz = sz; best = it->path().string(); }
    }
    return best;
}

uintmax_t folderBytes(const fs::path& root) {
    uintmax_t total = 0;
    std::error_code ec;
    if (!fs::exists(root, ec)) return 0;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (it->is_regular_file(ec)) total += it->file_size(ec);
    }
    return total;
}

std::string sanitizeName(std::string s) {
    for (char& c : s) {
        if (strchr("<>:\"/\\|?*", c)) c = ' ';
    }
    return util::trim(s);
}

std::string importToLibrary(const MediaRequest& req, const std::string& srcPath) {
    auto& cfg = StackConfig::get();
    std::error_code ec;
    if (srcPath.empty() || !fs::exists(srcPath, ec)) return {};
    std::string video = findBiggestVideo(srcPath);
    if (video.empty()) return {};

    fs::path src = video;
    std::string folderName = sanitizeName(req.title);
    if (!req.year.empty()) folderName += " (" + req.year + ")";

    fs::path destDir, destFile;
    if (req.mediaType == MediaType::Movie) {
        destDir = fs::path(cfg.moviesPath) / folderName;
        destFile = destDir / (folderName + src.extension().string());
    } else {
        int season = req.seasons.empty() ? 1 : req.seasons[0];
        char sn[32];
        std::snprintf(sn, sizeof(sn), "Season %02d", season);
        destDir = fs::path(cfg.tvPath) / folderName / sn;
        destFile = destDir / src.filename();
    }
    fs::create_directories(destDir, ec);
    if (fs::equivalent(src, destFile, ec)) return destFile.string();
    ec.clear();
    fs::copy_file(src, destFile, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        ec.clear();
        fs::rename(src, destFile, ec);
        if (ec) return {};
    }
    return destFile.string();
}

bool libraryHas(const MediaRequest& req) {
    auto& cfg = StackConfig::get();
    std::string folderName = sanitizeName(req.title);
    if (!req.year.empty()) folderName += " (" + req.year + ")";
    std::error_code ec;
    fs::path dir = req.mediaType == MediaType::Movie
                       ? fs::path(cfg.moviesPath) / folderName
                       : fs::path(cfg.tvPath) / folderName;
    return fs::exists(dir, ec) && !findBiggestVideo(dir).empty();
}

void processOne(const std::string& id) {
    MediaRequest snap;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        auto* r = findReqLocked(id);
        if (!r) return;
        snap = *r;
    }

    auto fail = [&](const std::string& msg) {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        if (auto* r = findReqLocked(id)) {
            setStatus(*r, ReqStatus::Failed, msg);
            saveRequestsLocked();
        }
        syncAppStatuses();
    };

    if (libraryHas(snap)) {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        if (auto* r = findReqLocked(id)) {
            setStatus(*r, ReqStatus::Available, "Już w bibliotece");
            r->progress = 1;
            saveRequestsLocked();
        }
        syncAppStatuses();
        return;
    }

    {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        if (auto* r = findReqLocked(id))
            setStatus(*r, ReqStatus::Searching, "Szukanie wydań…");
        saveRequestsLocked();
    }
    syncAppStatuses();

    // Resolve IMDb + original title like Radarr before hitting indexers
    enrichFromTmdb(snap);
    {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        if (auto* r = findReqLocked(id)) {
            if (!snap.originalTitle.empty()) r->originalTitle = snap.originalTitle;
            if (!snap.imdbId.empty()) r->imdbId = snap.imdbId;
            if (!snap.year.empty()) r->year = snap.year;
            saveRequestsLocked();
        }
    }

    auto releases = searchReleases(snap);
    if (releases.empty()) {
        std::string hint = !snap.originalTitle.empty() ? snap.originalTitle : snap.title;
        if (!snap.imdbId.empty()) hint += " (" + snap.imdbId + ")";
        fail("Nie znaleziono wydań torrent dla: " + hint);
        return;
    }

    const Release& best = releases.front();
    std::string jobDir;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        if (auto* r = findReqLocked(id)) {
            r->releaseTitle = best.title;
            r->magnetOrUrl = best.magnet;
            setStatus(*r, ReqStatus::Downloading,
                      "Pobieranie: " + best.title.substr(0, 60) +
                      " (" + std::to_string(best.seeders) + " seedów)");
            saveRequestsLocked();
        }
        jobDir = (fs::path(StackConfig::get().downloadPath) / id).string();
    }
    syncAppStatuses();

    std::string err;
    auto shouldCancel = [&]() -> bool {
        if (g_stop.load()) return true;
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        auto* r = findReqLocked(id);
        return !r || r->status == ReqStatus::Declined;
    };

    bool ok = torrent::downloadMagnet(
        best.magnet, jobDir, shouldCancel,
        [&](const torrent::Progress& p) {
            core::setPeers(p.peers + p.seeds);
            std::lock_guard<std::recursive_mutex> lk(g_mu);
            if (auto* r = findReqLocked(id)) {
                r->progress = std::min(0.95, std::max(0.02, p.fraction));
                char msg[192];
                std::snprintf(msg, sizeof(msg), "%s… %.0f/%.0f MB · %d peerów · %.0f KB/s",
                              p.state.c_str(), p.downloadedMb, p.totalMb, p.peers + p.seeds,
                              p.downloadRateKBs);
                r->message = msg;
                r->updatedAt = nowMs();
                static int tick = 0;
                if ((++tick % 4) == 0) saveRequestsLocked();
            }
            syncAppStatuses();
        },
        &err);

    core::setPeers(0);

    if (shouldCancel()) return;

    std::string video = findBiggestVideo(jobDir);
    if (!ok || video.empty()) {
        fail(err.empty() ? "Pobieranie nie powiodło się (brak pliku wideo)" : err);
        return;
    }

    {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        if (auto* r = findReqLocked(id))
            setStatus(*r, ReqStatus::Importing, "Kopiowanie do biblioteki…");
        saveRequestsLocked();
    }
    syncAppStatuses();

    std::string dest = importToLibrary(snap, jobDir);
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    if (auto* r = findReqLocked(id)) {
        if (dest.empty()) {
            r->libraryPath = video;
            setStatus(*r, ReqStatus::Available, "Pobrano (folder pobierania)");
        } else {
            r->libraryPath = dest;
            setStatus(*r, ReqStatus::Available, "W bibliotece");
        }
        r->progress = 1;
        saveRequestsLocked();
    }
    syncAppStatuses();
}

void schedulePump();

void pumpOnce() {
    g_pumpScheduled.store(false);
    if (g_stop.load()) return;
    std::string id;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        if (g_queue.empty()) return;
        id = g_queue.front();
        g_queue.pop_front();
    }
    try {
        processOne(id);
    } catch (const std::exception& e) {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        if (auto* r = findReqLocked(id)) {
            setStatus(*r, ReqStatus::Failed, e.what());
            saveRequestsLocked();
        }
    } catch (...) {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        if (auto* r = findReqLocked(id)) {
            setStatus(*r, ReqStatus::Failed, "Nieznany błąd");
            saveRequestsLocked();
        }
    }
    syncAppStatuses();
    // Continue queue on a worker — never on UI thread.
    schedulePump();
}

void schedulePump() {
    if (g_stop.load()) return;
    bool expected = false;
    if (!g_pumpScheduled.compare_exchange_strong(expected, true)) return;
    core::enqueue([] { pumpOnce(); });
}

} // namespace

// -------------------- public --------------------

StackConfig& StackConfig::get() { return g_cfg; }

void StackConfig::load() {
    std::string raw = util::readFile(cfgPath());
    moviesPath = util::appDataPath("library\\movies");
    tvPath = util::appDataPath("library\\tv");
    downloadPath = util::appDataPath("downloads");
    if (!raw.empty()) {
        try {
            auto j = json::parse(raw);
            moviesPath = j.value("moviesPath", moviesPath);
            tvPath = j.value("tvPath", tvPath);
            downloadPath = j.value("downloadPath", downloadPath);
            minSeeders = j.value("minSeeders", minSeeders);
            preferredQuality = j.value("preferredQuality", preferredQuality);
            autoStart = j.value("autoStart", autoStart);
        } catch (...) {}
    }
    std::error_code ec;
    fs::create_directories(moviesPath, ec);
    fs::create_directories(tvPath, ec);
    fs::create_directories(downloadPath, ec);
    save();
}

void StackConfig::save() const {
    json j{
        {"moviesPath", moviesPath}, {"tvPath", tvPath}, {"downloadPath", downloadPath},
        {"minSeeders", minSeeders}, {"preferredQuality", preferredQuality},
        {"autoStart", autoStart}
    };
    util::writeFile(cfgPath(), j.dump(2));
}

void init() {
    g_cfg.load();
    loadRequests();
    g_stop = false;
    syncAppStatuses();
    schedulePump();
}

void shutdown() {
    g_stop = true;
    core::setPeers(0);
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    saveRequestsLocked();
    g_cfg.save();
}

void tick() {
    static double last = 0;
    double now = nowMs() / 1000.0;
    if (now - last < 0.25) return;
    last = now;
    syncAppStatuses();
}

void syncAppStatuses() {
    auto& a = app();
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    for (auto& r : g_reqs)
        a.requestStatus[App::key(r.mediaType, r.tmdbId)] = uiStatus(r.status);
}

std::string requestMedia(MediaType type, int tmdbId, const std::string& title,
                         const std::string& year, const std::string& imdbId,
                         const std::vector<int>& seasons,
                         const std::string& originalTitle) {
    std::string id;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        for (auto& r : g_reqs) {
            if (r.tmdbId == tmdbId && r.mediaType == type) {
                if (r.status == ReqStatus::Failed || r.status == ReqStatus::Declined) {
                    r.status = ReqStatus::Pending;
                    r.message = "Ponawianie…";
                    r.progress = 0;
                    r.updatedAt = nowMs();
                    if (!title.empty()) r.title = title;
                    if (!originalTitle.empty()) r.originalTitle = originalTitle;
                    if (!year.empty()) r.year = year;
                    if (!imdbId.empty()) r.imdbId = imdbId;
                    if (!seasons.empty()) r.seasons = seasons;
                    g_queue.push_back(r.id);
                    id = r.id;
                    saveRequestsLocked();
                    break;
                }
                id = r.id;
                break;
            }
        }
        if (id.empty()) {
            MediaRequest r;
            r.id = newId();
            r.mediaType = type;
            r.tmdbId = tmdbId;
            r.title = title;
            r.originalTitle = originalTitle;
            r.year = year;
            r.imdbId = imdbId;
            r.seasons = seasons;
            r.status = ReqStatus::Pending;
            r.message = "W kolejce";
            r.createdAt = r.updatedAt = nowMs();
            id = r.id;
            g_reqs.push_back(r);
            if (g_cfg.autoStart) g_queue.push_back(id);
            saveRequestsLocked();
        } else if (g_cfg.autoStart) {
            auto* r = findReqLocked(id);
            if (r && r->status == ReqStatus::Pending) {
                bool already = false;
                for (auto& q : g_queue) if (q == id) already = true;
                if (!already) g_queue.push_back(id);
            }
        }
        app().requestStatus[App::key(type, tmdbId)] = 1;
    }
    schedulePump();
    return id;
}

std::string requestMedia(const Details& d, const std::vector<int>& seasons) {
    return requestMedia(d.mediaType, d.id, d.title, d.year(), d.imdbId, seasons, d.originalTitle);
}

std::vector<MediaRequest> listRequests() {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    return g_reqs;
}

bool cancelRequest(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    auto* r = findReqLocked(id);
    if (!r) return false;
    setStatus(*r, ReqStatus::Declined, "Anulowane");
    saveRequestsLocked();
    return true;
}

} // namespace stack
