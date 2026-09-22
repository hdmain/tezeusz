#include "localdb.hpp"
#include "util.hpp"
#include "json.hpp"
#include <atomic>
#include <chrono>
#include <mutex>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <vector>

using json = nlohmann::json;

namespace localdb {

namespace {

std::mutex g_mu;
std::vector<BlockItem> g_block;
std::vector<Issue> g_issues;
LocalUser g_user;
bool g_loaded = false;

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

std::string toLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

void saveBlockLocked() {
    json arr = json::array();
    for (auto& b : g_block) {
        arr.push_back({
            {"mediaType", b.mediaType == MediaType::TV ? "tv" : "movie"},
            {"tmdbId", b.tmdbId},
            {"title", b.title},
            {"year", b.year},
            {"posterPath", b.posterPath},
            {"createdAt", b.createdAt}
        });
    }
    util::writeFile(util::appDataPath("blocklist.json"), arr.dump(2));
}

void saveIssuesLocked() {
    json arr = json::array();
    for (auto& i : g_issues) {
        arr.push_back({
            {"id", i.id},
            {"mediaType", i.mediaType == MediaType::TV ? "tv" : "movie"},
            {"tmdbId", i.tmdbId},
            {"title", i.title},
            {"year", i.year},
            {"posterPath", i.posterPath},
            {"issueType", (int)i.issueType},
            {"status", (int)i.status},
            {"message", i.message},
            {"createdAt", i.createdAt},
            {"updatedAt", i.updatedAt}
        });
    }
    util::writeFile(util::appDataPath("issues.json"), arr.dump(2));
}

void saveUserLocked() {
    json j = {
        {"displayName", g_user.displayName},
        {"email", g_user.email},
        {"createdAt", g_user.createdAt}
    };
    util::writeFile(util::appDataPath("user.json"), j.dump(2));
}

void loadAll() {
    if (g_loaded) return;
    g_loaded = true;

    {
        std::string raw = util::readFile(util::appDataPath("blocklist.json"));
        if (!raw.empty()) {
            try {
                for (auto& j : json::parse(raw)) {
                    BlockItem b;
                    b.mediaType = j.value("mediaType", "movie") == "tv" ? MediaType::TV : MediaType::Movie;
                    b.tmdbId = j.value("tmdbId", 0);
                    b.title = j.value("title", "");
                    b.year = j.value("year", "");
                    b.posterPath = j.value("posterPath", "");
                    b.createdAt = j.value("createdAt", nowMs());
                    if (b.tmdbId > 0) g_block.push_back(std::move(b));
                }
            } catch (...) {}
        }
    }
    {
        std::string raw = util::readFile(util::appDataPath("issues.json"));
        if (!raw.empty()) {
            try {
                for (auto& j : json::parse(raw)) {
                    Issue i;
                    i.id = j.value("id", newId());
                    i.mediaType = j.value("mediaType", "movie") == "tv" ? MediaType::TV : MediaType::Movie;
                    i.tmdbId = j.value("tmdbId", 0);
                    i.title = j.value("title", "");
                    i.year = j.value("year", "");
                    i.posterPath = j.value("posterPath", "");
                    i.issueType = (IssueType)j.value("issueType", (int)IssueType::Other);
                    i.status = (IssueStatus)j.value("status", (int)IssueStatus::Open);
                    i.message = j.value("message", "");
                    i.createdAt = j.value("createdAt", nowMs());
                    i.updatedAt = j.value("updatedAt", i.createdAt);
                    g_issues.push_back(std::move(i));
                }
            } catch (...) {}
        }
    }
    {
        std::string raw = util::readFile(util::appDataPath("user.json"));
        if (!raw.empty()) {
            try {
                auto j = json::parse(raw);
                g_user.displayName = j.value("displayName", g_user.displayName);
                g_user.email = j.value("email", g_user.email);
                g_user.createdAt = j.value("createdAt", nowMs());
            } catch (...) {}
        }
        if (g_user.createdAt == 0) g_user.createdAt = nowMs();
    }
}

} // namespace

void init() {
    std::lock_guard<std::mutex> lk(g_mu);
    loadAll();
}

void shutdown() {}

bool isBlocked(MediaType t, int tmdbId) {
    std::lock_guard<std::mutex> lk(g_mu);
    loadAll();
    for (auto& b : g_block)
        if (b.mediaType == t && b.tmdbId == tmdbId) return true;
    return false;
}

void addBlock(const Details& d) {
    addBlock(d.mediaType, d.id, d.title, d.year(), d.posterPath);
}

void addBlock(MediaType t, int tmdbId, const std::string& title,
              const std::string& year, const std::string& posterPath) {
    std::lock_guard<std::mutex> lk(g_mu);
    loadAll();
    for (auto& b : g_block)
        if (b.mediaType == t && b.tmdbId == tmdbId) return;
    BlockItem b;
    b.mediaType = t;
    b.tmdbId = tmdbId;
    b.title = title;
    b.year = year;
    b.posterPath = posterPath;
    b.createdAt = nowMs();
    g_block.push_back(std::move(b));
    saveBlockLocked();
}

bool removeBlock(MediaType t, int tmdbId) {
    std::lock_guard<std::mutex> lk(g_mu);
    loadAll();
    auto it = std::remove_if(g_block.begin(), g_block.end(), [&](const BlockItem& b) {
        return b.mediaType == t && b.tmdbId == tmdbId;
    });
    if (it == g_block.end()) return false;
    g_block.erase(it, g_block.end());
    saveBlockLocked();
    return true;
}

std::vector<BlockItem> listBlock(const std::string& search) {
    std::lock_guard<std::mutex> lk(g_mu);
    loadAll();
    std::vector<BlockItem> out = g_block;
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.createdAt > b.createdAt; });
    if (!search.empty()) {
        std::string q = toLower(search);
        out.erase(std::remove_if(out.begin(), out.end(), [&](const BlockItem& b) {
            return toLower(b.title).find(q) == std::string::npos &&
                   std::to_string(b.tmdbId).find(q) == std::string::npos;
        }), out.end());
    }
    return out;
}

std::string addIssue(MediaType t, int tmdbId, const std::string& title,
                     const std::string& year, const std::string& posterPath,
                     IssueType type, const std::string& message) {
    std::lock_guard<std::mutex> lk(g_mu);
    loadAll();
    Issue i;
    i.id = newId();
    i.mediaType = t;
    i.tmdbId = tmdbId;
    i.title = title;
    i.year = year;
    i.posterPath = posterPath;
    i.issueType = type;
    i.status = IssueStatus::Open;
    i.message = message;
    i.createdAt = i.updatedAt = nowMs();
    std::string id = i.id;
    g_issues.push_back(std::move(i));
    saveIssuesLocked();
    return id;
}

bool setIssueStatus(const std::string& id, IssueStatus s) {
    std::lock_guard<std::mutex> lk(g_mu);
    loadAll();
    for (auto& i : g_issues) {
        if (i.id == id) {
            i.status = s;
            i.updatedAt = nowMs();
            saveIssuesLocked();
            return true;
        }
    }
    return false;
}

bool removeIssue(const std::string& id) {
    std::lock_guard<std::mutex> lk(g_mu);
    loadAll();
    auto it = std::remove_if(g_issues.begin(), g_issues.end(),
                             [&](const Issue& i) { return i.id == id; });
    if (it == g_issues.end()) return false;
    g_issues.erase(it, g_issues.end());
    saveIssuesLocked();
    return true;
}

std::vector<Issue> listIssues(IssueStatus filter, bool all) {
    std::lock_guard<std::mutex> lk(g_mu);
    loadAll();
    std::vector<Issue> out;
    for (auto& i : g_issues) {
        if (all || i.status == filter) out.push_back(i);
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.createdAt > b.createdAt; });
    return out;
}

LocalUser& user() {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        loadAll();
    }
    return g_user;
}

void saveUser() {
    std::lock_guard<std::mutex> lk(g_mu);
    loadAll();
    saveUserLocked();
}

void loadWatchlist(std::set<std::string>& out) {
    out.clear();
    std::string raw = util::readFile(util::appDataPath("watchlist.json"));
    if (raw.empty()) return;
    try {
        for (auto& j : json::parse(raw)) {
            if (j.is_string()) out.insert(j.get<std::string>());
        }
    } catch (...) {}
}

void saveWatchlist(const std::set<std::string>& wl) {
    json arr = json::array();
    for (auto& k : wl) arr.push_back(k);
    util::writeFile(util::appDataPath("watchlist.json"), arr.dump(2));
}

namespace {

std::string playbackKey(const std::string& path) {
    std::string k = path;
#ifdef _WIN32
    for (auto& c : k) {
        if (c == '/') c = '\\';
        else c = (char)std::tolower((unsigned char)c);
    }
#else
    // Keep as-is on case-sensitive filesystems
#endif
    return k;
}

bool writeAtomic(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    if (!util::writeFile(tmp, data)) return false;
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        std::filesystem::rename(tmp, path, ec);
    }
    return !ec;
}

json loadPlaybackMap() {
    std::string raw = util::readFile(util::appDataPath("playback.json"));
    if (raw.empty()) return json::object();
    try {
        auto j = json::parse(raw);
        if (j.is_object()) return j;
    } catch (...) {}
    return json::object();
}

void savePlaybackMap(const json& j) {
    writeAtomic(util::appDataPath("playback.json"), j.dump(2));
}

} // namespace

bool getPlaybackProgress(const std::string& path, PlaybackProgress* out) {
    if (path.empty() || !out) return false;
    json m = loadPlaybackMap();
    auto it = m.find(playbackKey(path));
    if (it == m.end() || !it->is_object()) return false;
    PlaybackProgress p;
    p.position = it->value("position", 0.0);
    p.timeMs = it->value("timeMs", (int64_t)0);
    p.durationMs = it->value("durationMs", (int64_t)0);
    p.updatedAt = it->value("updatedAt", (int64_t)0);
    // Ignore tiny / finished entries
    if (p.timeMs < 30'000) return false;
    if (p.position >= 0.95 || (p.durationMs > 0 && p.durationMs - p.timeMs < 60'000))
        return false;
    *out = p;
    return true;
}

void clearPlaybackProgress(const std::string& path) {
    if (path.empty()) return;
    json m = loadPlaybackMap();
    std::string k = playbackKey(path);
    if (!m.contains(k)) return;
    m.erase(k);
    savePlaybackMap(m);
}

void savePlaybackProgress(const std::string& path, double position, int64_t timeMs, int64_t durationMs) {
    if (path.empty()) return;
    position = std::clamp(position, 0.0, 1.0);

    // Near start → drop resume marker
    if (timeMs < 30'000 || position < 0.01) {
        clearPlaybackProgress(path);
        return;
    }
    // Near end → finished, clear
    if (position >= 0.95 || (durationMs > 0 && durationMs - timeMs < 60'000)) {
        clearPlaybackProgress(path);
        return;
    }

    json m = loadPlaybackMap();
    m[playbackKey(path)] = {
        {"position", position},
        {"timeMs", timeMs},
        {"durationMs", durationMs},
        {"updatedAt", nowMs()}
    };
    // Cap map size — keep newest 200 entries
    if (m.size() > 200) {
        std::vector<std::pair<int64_t, std::string>> order;
        for (auto it = m.begin(); it != m.end(); ++it) {
            int64_t t = it.value().is_object() ? it.value().value("updatedAt", (int64_t)0) : 0;
            order.push_back({t, it.key()});
        }
        std::sort(order.begin(), order.end());
        while (order.size() > 200) {
            m.erase(order.front().second);
            order.erase(order.begin());
        }
    }
    savePlaybackMap(m);
}

} // namespace localdb
