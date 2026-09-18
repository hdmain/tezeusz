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
            {"progress", r.progress}, {"createdAt", r.createdAt}, {"updatedAt", r.updatedAt},
            {"preferredQuality", r.preferredQuality}
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
            r.preferredQuality = j.value("preferredQuality", "");
            // Drop cancelled entries older than 30 minutes (also on startup)
            if (r.status == ReqStatus::Declined &&
                r.updatedAt > 0 && r.updatedAt < nowMs() - 30LL * 60LL * 1000LL)
                continue;
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
    if (t.find("2160p") != std::string::npos || t.find("4k") != std::string::npos ||
        t.find("uhd") != std::string::npos) base = 400;
    else if (t.find("1080p") != std::string::npos) base = 300;
    else if (t.find("720p") != std::string::npos) base = 200;
    else if (t.find("480p") != std::string::npos) base = 50;

    std::string p = toLower(pref);
    if (p.empty() || p == "any" || p == "dowolna") {
        // soft preference toward higher quality
        if (base >= 300) base += 40;
    } else if (p == "2160p" || p == "4k") {
        if (base >= 400) base += 250;
        else base -= 80;
    } else if (p == "1080p") {
        if (base == 300) base += 250;
        else if (base >= 400) base -= 40; // 4K ok but not preferred
        else if (base == 200) base -= 60;
        else base -= 100;
    } else if (p == "720p") {
        if (base == 200) base += 250;
        else if (base == 300) base -= 30;
        else if (base >= 400) base -= 80;
        else base -= 40;
    }

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
        std::string qpref = req.preferredQuality.empty() ? cfg.preferredQuality : req.preferredQuality;
        r.score = qualityScore(r.title, qpref) + std::min(r.seeders, 200);
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
        if (r->status == ReqStatus::Declined || r->status == ReqStatus::Available)
            return;
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

    auto stoppedOrGone = [&]() -> bool {
        if (g_stop.load()) return true;
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        auto* r = findReqLocked(id);
        return !r || r->status == ReqStatus::Declined;
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

    std::string magnet = snap.magnetOrUrl;
    std::string jobDir = (fs::path(StackConfig::get().downloadPath) / id).string();
    const bool resuming = !magnet.empty();

    if (!resuming) {
        {
            std::lock_guard<std::recursive_mutex> lk(g_mu);
            if (auto* r = findReqLocked(id))
                setStatus(*r, ReqStatus::Searching, "Szukanie wydań…");
            saveRequestsLocked();
        }
        syncAppStatuses();

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
        magnet = best.magnet;
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
        }
    } else {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        if (auto* r = findReqLocked(id)) {
            std::string label = !r->releaseTitle.empty() ? r->releaseTitle : r->title;
            setStatus(*r, ReqStatus::Downloading,
                      "Wznawianie: " + label.substr(0, 60));
            saveRequestsLocked();
        }
    }
    syncAppStatuses();

    // Already finished on disk (e.g. closed during import) — skip re-download
    {
        std::string existing = findBiggestVideo(jobDir);
        if (!existing.empty() && snap.progress >= 0.95) {
            // fall through to import below
        } else {
            std::string err;
            auto pollStop = [&]() -> torrent::StopAction {
                if (g_stop.load()) return torrent::StopAction::PauseKeep;
                std::lock_guard<std::recursive_mutex> lk(g_mu);
                auto* r = findReqLocked(id);
                if (!r || r->status == ReqStatus::Declined)
                    return torrent::StopAction::CancelDelete;
                return torrent::StopAction::Continue;
            };

            bool ok = torrent::downloadMagnet(
                magnet, jobDir, pollStop,
                [&](const torrent::Progress& p) {
                    core::setPeers(p.peers);
                    std::lock_guard<std::recursive_mutex> lk(g_mu);
                    if (auto* r = findReqLocked(id)) {
                        r->progress = std::min(0.95, std::max(0.02, p.fraction));

                        char eta[32] = "—";
                        double remainMb = std::max(0.0, p.totalMb - p.downloadedMb);
                        if (p.downloadRateKBs > 8.0 && remainMb > 0.05) {
                            double sec = (remainMb * 1024.0) / p.downloadRateKBs;
                            if (sec < 60)
                                std::snprintf(eta, sizeof(eta), "%.0fs", sec);
                            else if (sec < 3600)
                                std::snprintf(eta, sizeof(eta), "%dm %02ds",
                                              (int)(sec / 60), (int)sec % 60);
                            else if (sec < 86400)
                                std::snprintf(eta, sizeof(eta), "%dh %02dm",
                                              (int)(sec / 3600), ((int)(sec / 60)) % 60);
                            else
                                std::snprintf(eta, sizeof(eta), "%dd %dh",
                                              (int)(sec / 86400), ((int)(sec / 3600)) % 24);
                        }

                        char msg[256];
                        if (p.seeds > 0)
                            std::snprintf(msg, sizeof(msg),
                                          "%s… %.0f/%.0f MB · %d peerów (%d seed) · %.0f KB/s · ETA %s",
                                          p.state.c_str(), p.downloadedMb, p.totalMb,
                                          p.peers, p.seeds, p.downloadRateKBs, eta);
                        else
                            std::snprintf(msg, sizeof(msg),
                                          "%s… %.0f/%.0f MB · %d peerów · %.0f KB/s · ETA %s",
                                          p.state.c_str(), p.downloadedMb, p.totalMb,
                                          p.peers, p.downloadRateKBs, eta);
                        r->message = msg;
                        r->updatedAt = nowMs();
                        static int saveTick = 0;
                        if ((++saveTick % 4) == 0) saveRequestsLocked();
                    }
                    syncAppStatuses();
                },
                &err);

            core::setPeers(0);

            if (stoppedOrGone()) return; // Declined kept, or PauseKeep → Downloading for next launch

            std::string video = findBiggestVideo(jobDir);
            if (!ok || video.empty()) {
                fail(err.empty() ? "Pobieranie nie powiodło się (brak pliku wideo)" : err);
                return;
            }
        }
    }

    if (stoppedOrGone()) return;

    std::string video = findBiggestVideo(jobDir);
    if (video.empty()) {
        fail("Pobieranie nie powiodło się (brak pliku wideo)");
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
    static double lastPurge = 0;
    double now = nowMs() / 1000.0;
    if (now - last < 0.25) return;
    last = now;

    // Remove cancelled requests from the list after 30 minutes
    if (now - lastPurge >= 5.0) {
        lastPurge = now;
        const int64_t cutoff = nowMs() - 30LL * 60LL * 1000LL;
        bool changed = false;
        {
            std::lock_guard<std::recursive_mutex> lk(g_mu);
            auto it = g_reqs.begin();
            while (it != g_reqs.end()) {
                if (it->status == ReqStatus::Declined && it->updatedAt > 0 && it->updatedAt < cutoff) {
                    it = g_reqs.erase(it);
                    changed = true;
                } else {
                    ++it;
                }
            }
            if (changed) saveRequestsLocked();
        }
        if (changed) {
            // Drop stale UI keys so cancelled titles can be requested again cleanly
            app().requestStatus.clear();
            syncAppStatuses();
        }
    }

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
                         const std::string& originalTitle,
                         const std::string& preferredQuality) {
    std::string id;
    std::string q = preferredQuality;
    if (q.empty()) q = StackConfig::get().preferredQuality;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        for (auto& r : g_reqs) {
            if (r.tmdbId == tmdbId && r.mediaType == type) {
                if (r.status == ReqStatus::Failed || r.status == ReqStatus::Declined) {
                    r.status = ReqStatus::Pending;
                    r.message = "Ponawianie…";
                    r.progress = 0;
                    r.magnetOrUrl.clear();
                    r.releaseTitle.clear();
                    r.updatedAt = nowMs();
                    if (!title.empty()) r.title = title;
                    if (!originalTitle.empty()) r.originalTitle = originalTitle;
                    if (!year.empty()) r.year = year;
                    if (!imdbId.empty()) r.imdbId = imdbId;
                    if (!seasons.empty()) r.seasons = seasons;
                    r.preferredQuality = q;
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
            r.preferredQuality = q;
            r.status = ReqStatus::Pending;
            r.message = "W kolejce (" + q + ")";
            r.createdAt = r.updatedAt = nowMs();
            id = r.id;
            g_reqs.push_back(r);
            if (g_cfg.autoStart) g_queue.push_back(id);
            saveRequestsLocked();
        } else if (g_cfg.autoStart) {
            auto* r = findReqLocked(id);
            if (r && r->status == ReqStatus::Pending) {
                bool already = false;
                for (auto& qid : g_queue) if (qid == id) already = true;
                if (!already) g_queue.push_back(id);
            }
        }
        app().requestStatus[App::key(type, tmdbId)] = 1;
    }
    schedulePump();
    return id;
}

std::string requestMedia(const Details& d, const std::vector<int>& seasons,
                         const std::string& preferredQuality) {
    return requestMedia(d.mediaType, d.id, d.title, d.year(), d.imdbId, seasons, d.originalTitle,
                        preferredQuality);
}

static std::string qualityLabelOf(const std::string& title) {
    std::string t = toLower(title);
    if (t.find("2160p") != std::string::npos || t.find("4k") != std::string::npos ||
        t.find("uhd") != std::string::npos) return "2160p";
    if (t.find("1080p") != std::string::npos) return "1080p";
    if (t.find("720p") != std::string::npos) return "720p";
    if (t.find("480p") != std::string::npos) return "480p";
    return "—";
}

std::vector<ReleaseHit> searchReleasesInteractive(
    MediaType type, int tmdbId, const std::string& title, const std::string& year,
    const std::string& imdbId, const std::vector<int>& seasons,
    const std::string& originalTitle, const std::string& preferredQuality) {
    MediaRequest snap;
    snap.mediaType = type;
    snap.tmdbId = tmdbId;
    snap.title = title;
    snap.year = year;
    snap.imdbId = imdbId;
    snap.seasons = seasons;
    snap.originalTitle = originalTitle;
    snap.preferredQuality = preferredQuality.empty()
        ? StackConfig::get().preferredQuality : preferredQuality;
    enrichFromTmdb(snap);
    auto raw = searchReleases(snap);
    std::vector<ReleaseHit> out;
    out.reserve(raw.size());
    for (auto& r : raw) {
        ReleaseHit h;
        h.title = r.title;
        h.magnet = r.magnet;
        h.seeders = r.seeders;
        h.score = r.score;
        h.sizeBytes = r.size;
        h.quality = qualityLabelOf(r.title);
        out.push_back(std::move(h));
    }
    return out;
}

std::string requestWithRelease(MediaType type, int tmdbId, const std::string& title,
                               const std::string& year, const std::string& imdbId,
                               const std::vector<int>& seasons, const std::string& originalTitle,
                               const std::string& preferredQuality,
                               const std::string& magnet, const std::string& releaseTitle) {
    if (magnet.empty()) return {};
    std::string q = preferredQuality.empty() ? StackConfig::get().preferredQuality : preferredQuality;
    std::string id;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        MediaRequest* existing = nullptr;
        for (auto& r : g_reqs) {
            if (r.tmdbId == tmdbId && r.mediaType == type) { existing = &r; break; }
        }
        if (existing) {
            existing->title = title.empty() ? existing->title : title;
            if (!originalTitle.empty()) existing->originalTitle = originalTitle;
            if (!year.empty()) existing->year = year;
            if (!imdbId.empty()) existing->imdbId = imdbId;
            if (!seasons.empty()) existing->seasons = seasons;
            existing->preferredQuality = q;
            existing->magnetOrUrl = magnet;
            existing->releaseTitle = releaseTitle;
            existing->progress = 0;
            existing->libraryPath.clear();
            setStatus(*existing, ReqStatus::Pending, "Wybrane wydanie — w kolejce");
            id = existing->id;
            g_queue.erase(std::remove(g_queue.begin(), g_queue.end(), id), g_queue.end());
            g_queue.push_front(id);
            saveRequestsLocked();
        } else {
            MediaRequest r;
            r.id = newId();
            r.mediaType = type;
            r.tmdbId = tmdbId;
            r.title = title;
            r.originalTitle = originalTitle;
            r.year = year;
            r.imdbId = imdbId;
            r.seasons = seasons;
            r.preferredQuality = q;
            r.magnetOrUrl = magnet;
            r.releaseTitle = releaseTitle;
            r.status = ReqStatus::Pending;
            r.message = "Wybrane wydanie — w kolejce";
            r.createdAt = r.updatedAt = nowMs();
            id = r.id;
            g_reqs.push_back(std::move(r));
            g_queue.push_front(id);
            saveRequestsLocked();
        }
        app().requestStatus[App::key(type, tmdbId)] = 1;
    }
    schedulePump();
    return id;
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
    g_queue.erase(std::remove(g_queue.begin(), g_queue.end(), id), g_queue.end());
    saveRequestsLocked();
    return true;
}

void onLibraryRemoved(const std::string& pathOrFolder) {
    if (pathOrFolder.empty()) return;
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    bool changed = false;
    for (auto& r : g_reqs) {
        if (r.libraryPath.empty()) continue;
        if (r.libraryPath == pathOrFolder ||
            pathOrFolder.find(r.libraryPath) == 0 ||
            r.libraryPath.find(pathOrFolder) == 0) {
            r.libraryPath.clear();
            r.progress = 0;
            setStatus(r, ReqStatus::Declined, "Usunięte z biblioteki");
            changed = true;
        }
    }
    if (changed) {
        saveRequestsLocked();
        syncAppStatuses();
    }
}

} // namespace stack
