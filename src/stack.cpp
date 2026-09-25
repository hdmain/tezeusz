#include "stack.hpp"
#include "app.hpp"
#include "core.hpp"
#include "http.hpp"
#include "torrent.hpp"
#include "util.hpp"
#include "library.hpp"
#include "subs.hpp"
#include "i18n.hpp"
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
            {"seasons", r.seasons}, {"episodes", r.episodes},
            {"status", (int)r.status}, {"message", r.message},
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
            if (j.contains("episodes") && j["episodes"].is_array())
                r.episodes = j["episodes"].get<std::vector<int>>();
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

// "700.12 MB" / "2.06 GB" → bytes
long long parseHumanSize(const std::string& s) {
    size_t i = 0;
    double v = 0;
    try { v = std::stod(s, &i); } catch (...) { return 0; }
    std::string u = toLower(s.substr(i));
    if (u.find("tb") != std::string::npos) return (long long)(v * 1e12);
    if (u.find("gb") != std::string::npos) return (long long)(v * 1e9);
    if (u.find("mb") != std::string::npos) return (long long)(v * 1e6);
    if (u.find("kb") != std::string::npos) return (long long)(v * 1e3);
    return (long long)v;
}

std::string xmlUnescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '&') {
            auto sub = [&](const char* ent, char ch) {
                size_t n = std::strlen(ent);
                if (s.compare(i, n, ent) == 0) { out += ch; i += n; return true; }
                return false;
            };
            if (sub("&amp;", '&')) continue;
            if (sub("&lt;", '<')) continue;
            if (sub("&gt;", '>')) continue;
            if (sub("&quot;", '"')) continue;
            if (sub("&#39;", '\'')) continue;
            if (sub("&apos;", '\'')) continue;
        }
        out += s[i++];
    }
    return out;
}

// YTS — movies only, official JSON API, no key. Domains rotate often (DMCA),
// so try a few known mirrors until one answers.
inline std::vector<Release> searchYtsOne(const std::string& host, const std::string& query) {
    std::vector<Release> out;
    std::string url = "https://" + host + "/api/v2/list_movies.json?limit=15&query_parameters=" +
                      util::urlEncode(query);
    auto r = http::get(url, "application/json");
    if (!r.ok()) return out;
    try {
        auto j = json::parse(r.body);
        if (!j.contains("data") || !j["data"].contains("movies")) return out;
        for (auto& m : j["data"]["movies"]) {
            if (!m.is_object()) continue;
            std::string name = m.value("title", "");
            int year = 0;
            try { year = m.value("year", 0); } catch (...) {}
            if (!m.contains("torrents") || !m["torrents"].is_array()) continue;
            for (auto& t : m["torrents"]) {
                if (!t.is_object()) continue;
                std::string hash = t.value("hash", "");
                if (hash.empty()) continue;
                std::string quality = t.value("quality", "");
                std::string type = t.value("type", "");
                std::string codec = t.value("codec", "");
                Release rel;
                rel.title = name + (year ? " (" + std::to_string(year) + ")" : "") +
                            " " + quality + (type == "remux" ? " Remux" : "") + " " + codec;
                rel.magnet = makeMagnet(hash, rel.title);
                rel.indexer = "yts";
                rel.seeders = t.value("seeds", 0);
                rel.size = parseHumanSize(t.value("size", ""));
                out.push_back(std::move(rel));
            }
        }
    } catch (...) {}
    return out;
}

// YTS entry point: try known mirrors until one answers with data.
std::vector<Release> searchYts(const std::string& query) {
    static const char* hosts[] = {"yts.gg", "yts.mx", "yts.lt", "yts.ag", "yts.rs"};
    for (auto* h : hosts) {
        auto out = searchYtsOne(h, query);
        if (!out.empty()) return out;
    }
    return {};
}

// Nyaa.si — anime/serial + packs, public RSS, no key.
std::vector<Release> searchNyaa(const std::string& query) {
    std::vector<Release> out;
    auto r = http::get("https://nyaa.si/?page=rss&q=" + util::urlEncode(query) + "&c=0_0&f=0",
                       "application/rss+xml,*/*");
    if (!r.ok() || r.body.find("<item>") == std::string::npos) return out;
    const std::string& b = r.body;
    size_t pos = 0;
    while ((pos = b.find("<item>", pos)) != std::string::npos) {
        size_t end = b.find("</item>", pos);
        if (end == std::string::npos) break;
        std::string item = b.substr(pos + 6, end - pos - 6);
        pos = end;
        auto tag = [&](const char* name) -> std::string {
            std::string o = std::string("<") + name;
            size_t a = item.find(o);
            if (a == std::string::npos) return {};
            size_t gt = item.find('>', a);
            if (gt == std::string::npos) return {};
            std::string close = "</" + std::string(name) + ">";
            size_t z = item.find(close, gt);
            if (z == std::string::npos) return {};
            return item.substr(gt + 1, z - gt - 1);
        };
        std::string title = xmlUnescape(tag("title"));
        if (title.empty()) continue;
        // guid is a permalink these days; hash comes from nyaa:infoHash (fallback: guid)
        std::string hash = tag("nyaa:infoHash");
        if (hash.empty()) {
            std::string guid = xmlUnescape(tag("guid"));
            if (guid.rfind("magnet:", 0) == 0) {
                size_t a = guid.find("btih:");
                if (a != std::string::npos) hash = guid.substr(a + 5, 40);
            }
        }
        for (auto& c : hash) c = (char)std::tolower((unsigned char)c);
        if (hash.size() != 40) continue;
        Release rel;
        rel.title = title;
        rel.magnet = makeMagnet(hash, title);
        rel.indexer = "nyaa";
        rel.seeders = std::atoi(tag("nyaa:seeders").c_str());
        rel.size = parseHumanSize(tag("nyaa:size"));
        out.push_back(std::move(rel));
    }
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
        char b[32];
        if (req.seasons.size() == 1 && !req.episodes.empty()) {
            if (req.episodes.size() == 1)
                std::snprintf(b, sizeof(b), " S%02dE%02d", req.seasons[0], req.episodes[0]);
            else
                std::snprintf(b, sizeof(b), " S%02dE%02d", req.seasons[0], req.episodes.front());
            seasonSuffix = b;
        } else if (req.seasons.size() == 1) {
            std::snprintf(b, sizeof(b), " S%02d", req.seasons[0]);
            seasonSuffix = b;
        }
        // multiple seasons → no Sxx (prefer complete / pack releases)
    }

    const std::string& orig = !req.originalTitle.empty() ? req.originalTitle : req.title;
    std::vector<Release> all;
    bool fromImdb = false;

    // Query helper: TPB + YTS (movies) + Nyaa in parallel-ish (sequential is fine,
    // http is no longer globally locked).
    auto queryAll = [&](const std::string& q) {
        mergeReleases(all, searchApibay(q));
        if (req.mediaType != MediaType::TV) mergeReleases(all, searchYts(q));
        mergeReleases(all, searchNyaa(q));
    };

    // 1) IMDb id — strongest signal (apibay indexes tt…)
    if (!req.imdbId.empty()) {
        auto hit = searchApibay(req.imdbId);
        if (!hit.empty()) { mergeReleases(all, std::move(hit)); fromImdb = true; }
    }

    // 2) originalTitle + year (what Radarr sends to indexers)
    {
        std::string q = orig + (req.year.empty() ? "" : " " + req.year) + seasonSuffix;
        if (!orig.empty() && (all.empty() || all.size() < 5))
            queryAll(q);
    }

    // 3) originalTitle alone — still nothing solid, cast a wider net
    if (all.size() < 3 && !orig.empty())
        queryAll(orig + seasonSuffix);

    // 4) localized title fallback (Polish etc.)
    if (all.empty() && !req.title.empty() && req.title != orig) {
        std::string q = req.title + (req.year.empty() ? "" : " " + req.year) + seasonSuffix;
        queryAll(q);
        if (all.empty()) queryAll(req.title + seasonSuffix);
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
        std::snprintf(sn, sizeof(sn), i18n::tr("stack.season"), season);
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
            setStatus(*r, ReqStatus::Available, i18n::tr("stack.already_library"));
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
                setStatus(*r, ReqStatus::Searching, i18n::tr("stack.searching_releases"));
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
            fail(std::string(i18n::tr("stack.no_releases")) + hint);
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
                          std::string(i18n::tr("stack.downloading")) + best.title.substr(0, 60) +
                          " (" + std::to_string(best.seeders) + i18n::tr("stack.seeders"));
                saveRequestsLocked();
            }
        }
    } else {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        if (auto* r = findReqLocked(id)) {
            std::string label = !r->releaseTitle.empty() ? r->releaseTitle : r->title;
            setStatus(*r, ReqStatus::Downloading,
                      std::string(i18n::tr("stack.resuming")) + label.substr(0, 60));
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
                                          i18n::tr("stack.progress_seed"),
                                          p.state.c_str(), p.downloadedMb, p.totalMb,
                                          p.peers, p.seeds, p.downloadRateKBs, eta);
                        else
                            std::snprintf(msg, sizeof(msg),
                                          i18n::tr("stack.progress"),
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
                fail(err.empty() ? i18n::tr("stack.download_failed") : err);
                return;
            }
        }
    }

    if (stoppedOrGone()) return;

    std::string video = findBiggestVideo(jobDir);
    if (video.empty()) {
        fail(i18n::tr("stack.download_failed"));
        return;
    }

    {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        if (auto* r = findReqLocked(id))
            setStatus(*r, ReqStatus::Importing, i18n::tr("stack.importing"));
        saveRequestsLocked();
    }
    syncAppStatuses();

    std::string dest = importToLibrary(snap, jobDir);
    std::string playable;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        if (auto* r = findReqLocked(id)) {
            if (dest.empty()) {
                r->libraryPath = video;
                setStatus(*r, ReqStatus::Available, i18n::tr("stack.downloaded_folder"));
                playable = video;
            } else {
                r->libraryPath = dest;
                // No dedicated key for "In library" after import — use available status label.
                setStatus(*r, ReqStatus::Available, i18n::tr("status.available"));
                playable = dest;
            }
            r->progress = 1;
            saveRequestsLocked();
        }
    }
    syncAppStatuses();
    // Bazarr-style: auto-fetch EN + preferred language subtitles, scoring the
    // release name (jobDir folder / release title) against OpenSubtitles candidates.
    if (!playable.empty()) {
        std::string scene = snap.releaseTitle; // original torrent release name
        subs::enqueuePath(playable, snap.mediaType, snap.tmdbId, snap.imdbId, snap.title, scene);
    }
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
            setStatus(*r, ReqStatus::Failed, i18n::tr("stack.unknown_error"));
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
            autoUpdate = j.value("autoUpdate", autoUpdate);
            uiLanguage = j.value("uiLanguage", j.value("ui_language", uiLanguage));
            subsAuto = j.value("subsAuto", subsAuto);
            subsPreferredLang = j.value("subsPreferredLang", subsPreferredLang);
            subsApiKey = j.value("subsApiKey", subsApiKey);
            subsUsername = j.value("subsUsername", subsUsername);
            subsPassword = j.value("subsPassword", subsPassword);
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
        {"autoStart", autoStart},
        {"autoUpdate", autoUpdate},
        {"uiLanguage", uiLanguage},
        {"subsAuto", subsAuto}, {"subsPreferredLang", subsPreferredLang},
        {"subsApiKey", subsApiKey}, {"subsUsername", subsUsername},
        {"subsPassword", subsPassword}
    };
    util::writeFile(cfgPath(), j.dump(2));
}

void init() {
    g_cfg.load();
    i18n::setLanguage(g_cfg.uiLanguage);
    cfg::syncFromUi(g_cfg.uiLanguage);
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
                         const std::string& preferredQuality,
                         const std::vector<int>& episodes) {
    std::string id;
    std::string q = preferredQuality;
    if (q.empty()) q = StackConfig::get().preferredQuality;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        for (auto& r : g_reqs) {
            if (r.tmdbId == tmdbId && r.mediaType == type) {
                if (r.status == ReqStatus::Failed || r.status == ReqStatus::Declined) {
                    r.status = ReqStatus::Pending;
                    r.message = i18n::tr("stack.retrying");
                    r.progress = 0;
                    r.magnetOrUrl.clear();
                    r.releaseTitle.clear();
                    r.updatedAt = nowMs();
                    if (!title.empty()) r.title = title;
                    if (!originalTitle.empty()) r.originalTitle = originalTitle;
                    if (!year.empty()) r.year = year;
                    if (!imdbId.empty()) r.imdbId = imdbId;
                    if (!seasons.empty()) r.seasons = seasons;
                    r.episodes = episodes;
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
            r.episodes = episodes;
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
                         const std::string& preferredQuality,
                         const std::vector<int>& episodes) {
    return requestMedia(d.mediaType, d.id, d.title, d.year(), d.imdbId, seasons, d.originalTitle,
                        preferredQuality, episodes);
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
    const std::string& originalTitle, const std::string& preferredQuality,
    const std::vector<int>& episodes) {
    MediaRequest snap;
    snap.mediaType = type;
    snap.tmdbId = tmdbId;
    snap.title = title;
    snap.year = year;
    snap.imdbId = imdbId;
    snap.seasons = seasons;
    snap.episodes = episodes;
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
                               const std::string& magnet, const std::string& releaseTitle,
                               const std::vector<int>& episodes) {
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
            existing->episodes = episodes;
            existing->preferredQuality = q;
            existing->magnetOrUrl = magnet;
            existing->releaseTitle = releaseTitle;
            existing->progress = 0;
            existing->libraryPath.clear();
            setStatus(*existing, ReqStatus::Pending, i18n::tr("stack.picked_queued"));
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
            r.episodes = episodes;
            r.preferredQuality = q;
            r.magnetOrUrl = magnet;
            r.releaseTitle = releaseTitle;
            r.status = ReqStatus::Pending;
            r.message = i18n::tr("stack.picked_queued");
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
    setStatus(*r, ReqStatus::Declined, i18n::tr("stack.cancelled"));
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
            setStatus(r, ReqStatus::Declined, i18n::tr("stack.removed_library"));
            changed = true;
        }
    }
    if (changed) {
        saveRequestsLocked();
        syncAppStatuses();
    }
}

} // namespace stack
