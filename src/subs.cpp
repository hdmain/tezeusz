#include "subs.hpp"
#include "stack.hpp"
#include "http.hpp"
#include "library.hpp"
#include "core.hpp"
#include "util.hpp"
#include "i18n.hpp"
#include "json.hpp"
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#include <zlib.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace subs {
namespace {

struct Job {
    std::string videoPath;
    MediaType type = MediaType::Movie;
    int tmdbId = 0;
    std::string imdbId;
    std::string title;
    int season = 0;
    int episode = 0;
    std::string sceneName; // original release name (torrent) — Bazarr refiner hint
};

std::mutex g_mu;
std::condition_variable g_cv;
std::deque<Job> g_q;
std::set<std::string> g_queued; // video|lang keys currently queued/done this session
std::atomic<bool> g_stop{false};
std::atomic<bool> g_busy{false};
std::thread g_worker;
std::string g_status = i18n::tr("subs.idle");
std::string g_token;
int64_t g_tokenAt = 0;

// Snapshot of subtitle settings, refreshed on the UI thread (tick) so the
// worker never reads StackConfig strings that the UI might be writing (race).
struct Snap {
    bool autoOn = true;
    std::string lang = "pl";
    std::string apiKey, username, password;
};
std::mutex g_cfgMu;
Snap g_cfg;

void refreshConfig() {
    auto& c = stack::StackConfig::get();
    Snap s;
    s.autoOn = c.subsAuto;
    s.lang = c.subsPreferredLang;
    s.apiKey = c.subsApiKey;
    s.username = c.subsUsername;
    s.password = c.subsPassword;
    std::lock_guard<std::mutex> lk(g_cfgMu);
    g_cfg = std::move(s);
}

Snap cfgSnap() {
    std::lock_guard<std::mutex> lk(g_cfgMu);
    return g_cfg;
}

constexpr const char* kApiBase = "https://api.opensubtitles.com/api/v1";
constexpr const char* kUserAgent = "SeerrCpp v1.0";

bool isVideoExt(const fs::path& p) {
    auto e = util::lower(p.extension().string());
    return e == ".mkv" || e == ".mp4" || e == ".avi" || e == ".m4v" ||
           e == ".ts" || e == ".m2ts" || e == ".webm" || e == ".mov";
}

void setStatus(std::string s) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_status = std::move(s);
}

std::string apiHeaders(const std::string& apiKey, const std::string& token = {}) {
    std::string h = "Api-Key: " + apiKey + "\r\nUser-Agent: " + kUserAgent + "\r\n";
    if (!token.empty())
        h += "Authorization: Bearer " + token + "\r\n";
    return h;
}

std::string stripImdb(std::string id) {
    if (id.size() > 2 && (id[0] == 't' || id[0] == 'T') && (id[1] == 't' || id[1] == 'T'))
        id = id.substr(2);
    return id;
}

bool parseSeasonEpisode(const std::string& name, int& season, int& episode) {
    season = 0;
    episode = 0;
    std::string s = util::lower(name);
    // s01e02 / s1e2
    for (size_t i = 0; i + 3 < s.size(); ++i) {
        if (s[i] != 's') continue;
        size_t j = i + 1;
        int se = 0, ep = 0;
        if (j >= s.size() || !std::isdigit((unsigned char)s[j])) continue;
        while (j < s.size() && std::isdigit((unsigned char)s[j])) se = se * 10 + (s[j++] - '0');
        if (j >= s.size() || s[j] != 'e') continue;
        ++j;
        if (j >= s.size() || !std::isdigit((unsigned char)s[j])) continue;
        while (j < s.size() && std::isdigit((unsigned char)s[j])) ep = ep * 10 + (s[j++] - '0');
        if (se > 0 && ep > 0) { season = se; episode = ep; return true; }
    }
    // 1x02
    for (size_t i = 0; i + 2 < s.size(); ++i) {
        if (!std::isdigit((unsigned char)s[i])) continue;
        size_t j = i;
        int se = 0, ep = 0;
        while (j < s.size() && std::isdigit((unsigned char)s[j])) se = se * 10 + (s[j++] - '0');
        if (j >= s.size() || s[j] != 'x') continue;
        ++j;
        if (j >= s.size() || !std::isdigit((unsigned char)s[j])) continue;
        while (j < s.size() && std::isdigit((unsigned char)s[j])) ep = ep * 10 + (s[j++] - '0');
        if (se > 0 && ep > 0) { season = se; episode = ep; return true; }
    }
    return false;
}

std::string sidecarPath(const fs::path& video, const std::string& lang) {
    return (video.parent_path() / (video.stem().string() + "." + lang + ".srt")).string();
}

bool hasSidecar(const fs::path& video, const std::string& lang) {
    std::error_code ec;
    return fs::exists(sidecarPath(video, lang), ec);
}

std::string movieHash(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    auto fsize = (uint64_t)f.tellg();
    if (fsize < 65536) return {};
    uint64_t hash = fsize;
    auto addBlock = [&](uint64_t offset) {
        f.seekg((std::streamoff)offset, std::ios::beg);
        for (int i = 0; i < 8192; ++i) {
            uint64_t tmp = 0;
            f.read(reinterpret_cast<char*>(&tmp), 8);
            if (!f) break;
            hash += tmp;
        }
        f.clear();
    };
    addBlock(0);
    addBlock(fsize - 65536);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)hash);
    return buf;
}

bool looksLikeSrt(const std::string& body) {
    if (body.size() < 8) return false;
    // gzip?
    if ((unsigned char)body[0] == 0x1f && (unsigned char)body[1] == 0x8b) return false;
    std::string head = body.substr(0, 64);
    if (head.rfind("WEBVTT", 0) == 0) return true;
    // strip BOM
    size_t i = 0;
    if (body.size() >= 3 && (unsigned char)body[0] == 0xEF) i = 3;
    while (i < body.size() && (body[i] == '\r' || body[i] == '\n' || body[i] == ' ')) ++i;
    return i < body.size() && std::isdigit((unsigned char)body[i]);
}

// ------------------------------------------------------------------ MD5 (for TheSubDB / NapiProjekt)
std::string md5Hex(const std::string& input) {
    static const uint32_t K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
    static const uint32_t S[64] = {
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
    std::string msg = input;
    uint64_t bits = (uint64_t)input.size() * 8;
    msg += (char)0x80;
    while (msg.size() % 64 != 56) msg += (char)0;
    for (int i = 0; i < 8; ++i) msg += (char)((bits >> (8 * i)) & 0xff);
    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t M[16];
        for (int i = 0; i < 16; ++i)
            M[i] = (uint32_t)(uint8_t)msg[off + i * 4] |
                   ((uint32_t)(uint8_t)msg[off + i * 4 + 1] << 8) |
                   ((uint32_t)(uint8_t)msg[off + i * 4 + 2] << 16) |
                   ((uint32_t)(uint8_t)msg[off + i * 4 + 3] << 24);
        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (uint32_t i = 0; i < 64; ++i) {
            uint32_t F, g;
            if (i < 16)      { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D;          g = (3 * i + 5) % 16; }
            else             { F = C ^ (B | ~D);       g = (7 * i) % 16; }
            F += A + K[i] + M[g];
            A = D; D = C; C = B;
            B += (F << S[i]) | (F >> (32 - S[i]));
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }
    char buf[33];
    int p = 0;
    auto put = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i, p += 2)
            std::snprintf(buf + p, 3, "%02x", (v >> (8 * i)) & 0xff);
    };
    put(a0); put(b0); put(c0); put(d0);
    buf[p] = 0;
    return buf;
}

// TheSubDB: md5(first 64 KiB + last 64 KiB)
std::string theSubDbHash(const std::string& path) {
    const size_t RS = 64 * 1024;
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto sz = (size_t)f.tellg();
    if (sz < RS * 2) return {};
    std::string data(RS * 2, '\0');
    f.seekg(0);
    f.read(&data[0], (std::streamsize)RS);
    f.seekg((std::streamoff)(sz - RS));
    f.read(&data[(size_t)RS], (std::streamsize)RS);
    std::string h = md5Hex(data);
    for (auto& c : h) c = (char)std::toupper((unsigned char)c);
    return h;
}

// NapiProjekt: md5(first 10 MiB), lowercase
std::string napiHash(const std::string& path) {
    const size_t RS = 10u * 1024u * 1024u;
    std::ifstream f(path, std::ios::binary);
    std::string data(RS, '\0');
    f.read(&data[0], (std::streamsize)RS);
    data.resize((size_t)f.gcount());
    if (data.size() < 65536) return {};
    return md5Hex(data);
}

// NapiProjekt sub-hash (from subliminal: unit_napisy/dl.php 't' param)
std::string napiSubhash(const std::string& h) {
    static const int idx[5] = {0xe, 0x3, 0x6, 0x8, 0x2};
    static const int mul[5] = {2, 2, 5, 4, 3};
    static const int add[5] = {0, 0xd, 0x10, 0xb, 0x5};
    auto hexv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    std::string out;
    for (int i = 0; i < 5; ++i) {
        int t = add[i] + hexv(h[idx[i]]);
        if (t + 2 > (int)h.size()) return {};
        int v = hexv(h[t]) * 16 + hexv(h[t + 1]);
        char buf[3];
        std::snprintf(buf, sizeof(buf), "%x", (v * mul[i]) & 0xf);
        out += buf[0];
    }
    return out;
}

// gunzip via zlib (NapiProjekt returns gzipped srt)
std::string gunzip(const std::string& in) {
    if (in.size() < 2 || (unsigned char)in[0] != 0x1f || (unsigned char)in[1] != 0x8b)
        return in; // not gzipped
    z_stream zs{};
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) return {};
    zs.next_in = (Bytef*)in.data();
    zs.avail_in = (uInt)in.size();
    std::string out;
    char buf[16384];
    int r = Z_OK;
    do {
        zs.next_out = (Bytef*)buf;
        zs.avail_out = sizeof(buf);
        r = inflate(&zs, Z_NO_FLUSH);
        if (r != Z_OK && r != Z_STREAM_END && r != Z_BUF_ERROR) { inflateEnd(&zs); return {}; }
        out.append(buf, sizeof(buf) - zs.avail_out);
        if (r == Z_BUF_ERROR && zs.avail_in == 0) break;
    } while (r != Z_STREAM_END);
    inflateEnd(&zs);
    return r == Z_STREAM_END ? out : std::string();
}

// ------------------------------------------------------------------ keyless providers (Bazarr-style)
// TheSubDB: hash-based, English only. NapiProjekt: hash-based, Polish.
bool tryTheSubDb(const Job& job, const std::string& lang) {
    if (util::lower(lang) != "en") return false;
    std::string h = theSubDbHash(job.videoPath);
    if (h.empty()) return false;
    std::string ua = "SubDB/1.0 (SeerrCpp/1.0; https://github.com/)";
    std::string q = std::string("action=search&hash=") + h;
    auto r = http::get("http://api.thesubdb.com/?" + q, "text/plain", "User-Agent: " + ua + "\r\n");
    if (r.status == 404) return false;
    if (!r.ok()) return false;
    std::vector<std::string> langs = util::split(r.body, ',');
    bool haveEn = false;
    for (auto& l : langs) if (util::trim(util::lower(l)) == "en") haveEn = true;
    if (!haveEn) return false;
    setStatus(i18n::tr("subs.thesubdb"));
    auto d = http::get("http://api.thesubdb.com/?action=download&hash=" + h + "&language=en",
                       "text/plain", "User-Agent: " + ua + "\r\n");
    if (!d.ok() || d.body.empty()) return false;
    std::string content = gunzip(d.body);
    if (content.empty()) content = d.body;
    if (!looksLikeSrt(content)) return false;
    std::string outPath = sidecarPath(job.videoPath, "en");
    if (!util::writeFile(outPath, content)) return false;
    setStatus(std::string(i18n::tr("subs.saved")) + fs::path(outPath).filename().string() + " (TheSubDB)");
    return true;
}

bool tryNapiProjekt(const Job& job, const std::string& lang) {
    if (util::lower(lang) != "pl") return false;
    std::string h = napiHash(job.videoPath);
    if (h.empty()) return false;
    std::string sub = napiSubhash(h);
    if (sub.empty()) return false;
    setStatus(i18n::tr("subs.napi"));
    std::string url = "http://napiprojekt.pl/unit_napisy/dl.php"
        "?v=dreambox&kolejka=false&nick=&pass=&napios=Linux&l=PL&f=" + h + "&t=" + sub;
    auto r = http::get(url, "*/*", "");
    if (!r.ok() || r.body.empty()) return false;
    if (r.body.rfind("NPc0", 0) == 0) return false; // not found
    std::string content = gunzip(r.body);
    if (content.empty()) content = r.body;
    if (!looksLikeSrt(content)) return false;
    std::string outPath = sidecarPath(job.videoPath, "pl");
    if (!util::writeFile(outPath, content)) return false;
    setStatus(std::string(i18n::tr("subs.saved")) + fs::path(outPath).filename().string() + " (NapiProjekt)");
    return true;
}

// Try all keyless providers for one language; true = subtitle file is now present.
bool tryKeyless(const Job& job, const std::string& lang) {
    if (lang == "en") return tryTheSubDb(job, lang);
    if (lang == "pl") return tryNapiProjekt(job, lang);
    return false;
}

bool ensureToken(const Snap& cfg) {
    if (cfg.apiKey.empty()) {
        setStatus(i18n::tr("subs.need_key"));
        return false;
    }
    // Cached token ~50 min
    int64_t now = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_token.empty() && now - g_tokenAt < 50LL * 60 * 1000)
            return true;
    }
    if (cfg.username.empty() || cfg.password.empty()) {
        setStatus(i18n::tr("subs.need_login"));
        return false;
    }
    setStatus(i18n::tr("subs.logging_in"));
    json body{{"username", cfg.username}, {"password", cfg.password}};
    auto r = http::post(std::string(kApiBase) + "/login", body.dump(), "application/json",
                        apiHeaders(cfg.apiKey));
    if (!r.ok()) {
        setStatus(std::string(i18n::tr("subs.login_error")) + std::to_string(r.status) + ")");
        return false;
    }
    try {
        auto j = json::parse(r.body);
        std::string tok = j.value("token", "");
        if (tok.empty()) {
            setStatus(i18n::tr("subs.no_token"));
            return false;
        }
        std::lock_guard<std::mutex> lk(g_mu);
        g_token = tok;
        g_tokenAt = now;
        return true;
    } catch (...) {
        setStatus(i18n::tr("subs.bad_login"));
        return false;
    }
}

std::string tokenCopy() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_token;
}

// ------------------------------------------------------------- Bazarr-style scoring
// Replicates subliminal_patch/score.py DEFAULT_SCORES + guessit-style release parsing.

struct ReleaseInfo {
    std::string title;
    int year = 0;
    int season = 0, episode = 0;
    std::string resolution;  // "720p" "1080p" "2160p" "480p"...
    std::string source;      // bluray|remux|webdl|webrip|web|hdtv|dvd|hdcam|telesync|screener|unknown
    std::string videoCodec;  // x264|h264|x265|h265|hevc|av1|xvid|divx|...
    std::string releaseGroup;
    std::string edition;     // remastered|extended|uncut|directors cut|imax|theatrical|repack|proper|internal|unrated|special
    std::string streamingService; // netflix|amzn|hulu|dsny|atvp|hmax|pcok|max|nrl|stan|cr|...
    std::vector<uint64_t> hashes;
};

bool isWordChar(char c) {
    return std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == '+' || c == '.';
}

// Token search where delimiters are anything except word chars.
bool tokenHas(const std::vector<std::string>& toks, const std::string& t) {
    for (auto& s : toks) if (s == t) return true;
    return false;
}

std::vector<std::string> tokenizeName(const std::string& lower) {
    // split on '.', ' ', '-', '_' but NOT inside alphanumeric words like "x264"
    std::vector<std::string> out;
    std::string cur;
    for (char c : lower) {
        if (c == '.' || c == ' ' || c == '_' || c == '-' || c == '(' || c == ')') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string normSource(const std::string& tok) {
    // map raw token to canonical source
    auto starts = [&](const char* p) { return tok.rfind(p, 0) == 0; };
    if (tok == "bluray" || tok == "blu-ray" || tok == "brdisk" || tok == "bdrip" || tok == "bdmux") return "bluray";
    if (starts("remux")) return "remux";
    if (tok == "uhd" || tok == "uhdbluray") return "bluray";
    if (tok == "webdl" || tok == "web-dl" || tok == "webdl" ) return "webdl";
    if (tok == "webrip" || tok == "web-rip") return "webrip";
    if (tok == "web" || tok == "webux") return "web";
    if (tok == "hdtv" || tok == "dvb" || tok == "sdtv" || tok == "pdtv" || tok == "hrib") return "hdtv";
    if (tok == "dvd" || tok == "dvdr" || tok == "dvdrip" || tok == "dvdmux") return "dvd";
    if (tok == "hdcam" || tok == "hd-ts" || tok == "ts" || tok == "telesync" || tok == "telescreen") return "telesync";
    if (tok == "hdts" ) return "telesync";
    if (tok == "scr" || tok == "screener" || tok == "r5" || tok == "cam" || tok == "tc") return "screener";
    return {};
}

std::string findSource(const std::string& lower, const std::vector<std::string>& toks) {
    // multiword / glued first
    auto contains = [&](const char* s) { return lower.find(s) != std::string::npos; };
    std::string best;
    int bestRank = -1;
    auto consider = [&](const std::string& src, int rank) {
        if (rank > bestRank) { bestRank = rank; best = src; }
    };
    for (auto& t : toks) {
        std::string s = normSource(t);
        if (s == "bluray") consider("bluray", 3);
        else if (s == "remux") consider("remux", 4);
        else if (s == "webdl") consider("webdl", 5);
        else if (s == "webrip") consider("webrip", 5);
        else if (s == "web") consider("web", 5);
        else if (s == "hdtv") consider("hdtv", 2);
        else if (s == "dvd") consider("dvd", 2);
        else if (s == "telesync") consider("telesync", 1);
        else if (s == "screener") consider("screener", 1);
    }
    (void)contains;
    if (best.empty() && contains("web")) best = "web";
    return best;
}

std::string findResolution(const std::vector<std::string>& toks) {
    for (auto& t : toks)
        if ((t == "480p" || t == "576p" || t == "720p" || t == "1080p" || t == "2160p" || t == "4320p"))
            return t;
    return {};
}

std::string findVideoCodec(const std::string& lower, const std::vector<std::string>& toks) {
    for (auto& t : toks) {
        if (t == "x264" || t == "h264" || t == "avc") return "h264";
        if (t == "x265" || t == "h265" || t == "hevc") return "hevc";
        if (t == "av1") return "av1";
        if (t == "xvid") return "xvid";
        if (t == "divx") return "divx";
        if (t == "vp9") return "vp9";
        if (t == "mpeg2" || t == "mpg2") return "mpeg2";
    }
    if (lower.find("h.264") != std::string::npos) return "h264";
    if (lower.find("h.265") != std::string::npos) return "hevc";
    return {};
}

std::string findStreamingService(const std::vector<std::string>& toks) {
    static const char* svcs[] = {"nf","netflix","amazon","amzn","ws","hulu","dsny","disney","disneyplus",
        "atc","atvp","appletv","hmax","max","hbo","pcok","peacock","nrl","paramount","pmtp","stb","stan",
        "bravia","cr","crunchyroll","red","pathe","pu","plutv","tvpm","ipt","itv","bcgb"," bbc","rtetalk"};
    for (auto& t : toks)
        for (auto* s : svcs)
            if (t == s) return s;
    return {};
}

std::string findEdition(const std::vector<std::string>& toks) {
    auto joined = [&]() {
        // allow "directors-cut" / "director s cut" approximations via tokens
        return std::string();
    };
    (void)joined;
    static const char* eds[] = {"remastered","imax","extended","extendedcut","uncut","unrated",
        "special","limited","theatrical","alternatescut","alternativecut"};
    for (auto* e : eds) if (tokenHas(toks, e)) return e;
    if (tokenHas(toks, "directors") || tokenHas(toks, "director")) {
        for (auto& t : toks) if (t == "cut") return "directors cut";
    }
    static const char* vers[] = {"repack","proper","internal","v2","v3","v4","readnfo","fix24"};
    for (auto* v : vers) if (tokenHas(toks, v)) return v;
    return {};
}

std::string guessTitle(const std::string& name) {
    // Title = leading words until year "(20xx)" or first purely-dotted token that looks metadata-y
    std::string out;
    size_t lp = name.find(" (");
    if (lp != std::string::npos) return util::trim(name.substr(0, lp));
    // dotted scene name
    auto parts = util::split(name, '.');
    for (size_t i = 0; i < parts.size(); ++i) {
        const std::string& p = parts[i];
        if (p.empty()) continue;
        bool year = p.size() == 4 && std::all_of(p.begin(), p.end(), [](unsigned char c) { return std::isdigit(c); })
                    && p >= "1900" && p <= "2099";
        if (year) break;
        std::string l = util::lower(p);
        if (normSource(l) == "bluray" || normSource(l) == "remux" || normSource(l) == "webdl" ||
            normSource(l) == "webrip" || l == "web" || l == "hdtv" || l == "dvd" ||
            l == "1080p" || l == "720p" || l == "2160p" || l == "480p" || l == "576p")
            break;
        if (!out.empty()) out += " ";
        out += p;
    }
    return util::trim(out);
}

ReleaseInfo parseReleaseName(const std::string& name) {
    ReleaseInfo r;
    std::string lower = util::lower(name);
    auto toks = tokenizeName(lower);
    r.title = guessTitle(name);
    // year: first 4-digit token 19xx/20xx after title
    for (auto& t : toks) {
        if (t.size() == 4 && std::all_of(t.begin(), t.end(), [](unsigned char c) { return std::isdigit(c); })) {
            int y = std::atoi(t.c_str());
            if (y >= 1900 && y <= 2099) { r.year = y; break; }
        }
    }
    int s = 0, e = 0;
    if (parseSeasonEpisode(lower, s, e)) { r.season = s; r.episode = e; }
    r.resolution = findResolution(toks);
    r.source = findSource(lower, toks);
    r.videoCodec = findVideoCodec(lower, toks);
    r.streamingService = findStreamingService(toks);
    r.edition = findEdition(toks);
    // release group: text after last '-' before extension, if alpha-ish
    {
        fs::path p(name);
        std::string stem = p.stem().string();
        auto dash = stem.rfind('-');
        if (dash != std::string::npos && dash + 1 < stem.size()) {
            std::string g = stem.substr(dash + 1);
            bool ok = g.size() <= 20;
            for (char c : g) if (!std::isalnum((unsigned char)c) && c != '.' && c != '-' && c != '_') ok = false;
            if (ok && g.find(' ') == std::string::npos && g != std::to_string(r.year))
                r.releaseGroup = util::lower(g);
        }
    }
    return r;
}

uint64_t parseHexHash(const std::string& s) {
    if (s.size() != 16) return 0;
    uint64_t h = 0;
    for (char c : s) {
        h <<= 4;
        if (c >= '0' && c <= '9') h |= (uint64_t)(c - '0');
        else if (c >= 'a' && c <= 'f') h |= (uint64_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') h |= (uint64_t)(c - 'A' + 10);
        else return 0;
    }
    return h;
}

// Movie weights (subliminal_patch score.py DEFAULT_SCORES["movie"])
constexpr int MOVIE_HASH = 179, MOVIE_TITLE = 60, MOVIE_YEAR = 40, MOVIE_SOURCE = 30,
              MOVIE_EDITION = 30, MOVIE_GROUP = 15, MOVIE_MINOR = 1;
constexpr int MOVIE_MAX = MOVIE_TITLE + MOVIE_YEAR + MOVIE_SOURCE + MOVIE_EDITION + MOVIE_GROUP +
                          4 * MOVIE_MINOR; // 181
// Episode weights
constexpr int EP_HASH = 359, EP_SERIES = 160, EP_YEAR = 90, EP_SEASON = 30, EP_EPISODE = 30,
              EP_SOURCE = 25, EP_GROUP = 20, EP_MINOR = 1;
constexpr int EP_MAX = EP_SERIES + EP_YEAR + EP_SEASON + EP_EPISODE + EP_SOURCE + EP_GROUP +
                       4 * EP_MINOR; // 332

std::string normSrcForCompare(const std::string& a, const std::string& b) {
    // webdl/webrip/web blur together; remux counts as bluray-family
    auto fam = [](std::string s) {
        if (s == "webdl" || s == "webrip" || s == "web") return std::string("web");
        if (s == "remux" || s == "bluray") return std::string("bluray");
        if (s == "telesync" || s == "screener") return std::string("cam");
        return s;
    };
    std::string fa = fam(a), fb = fam(b);
    return (!fa.empty() && fa == fb) ? fa : std::string();
}

struct SubCandidate {
    int fileId = 0;
    std::string fileName;
    std::string language;
    int downloadCount = 0;
    int cdNumber = 1;
    bool hashMatch = false;
    int score = 0;
    bool hasHash = false; // candidate carries a known matching hash entry
};

// Compute Bazarr/subliminal score for one subtitle release vs video.
int scoreSubtitle(const SubCandidate& cand, const ReleaseInfo& video, bool isEpisode,
                  const std::string& videoImdb, const std::string& candImdb) {
    std::string rel = util::lower(cand.fileName);
    auto toks = tokenizeName(rel);
    ReleaseInfo sub = parseReleaseName(cand.fileName);
    sub.year = sub.year ? sub.year : 0;

    std::set<std::string> matches;
    if (cand.hashMatch) matches.insert("hash");
    // imdb equality → title+year (movies) / series+year+season+episode (eps)
    bool imdbEq = !videoImdb.empty() && !candImdb.empty() &&
                  stripImdb(videoImdb) == stripImdb(candImdb);

    if (!isEpisode) {
        if (imdbEq) { matches.insert("title"); matches.insert("year"); }
        else {
            if (!video.title.empty() && !sub.title.empty()) {
                std::string vt = util::lower(video.title), st = util::lower(sub.title);
                if (vt == st || vt.find(st) != std::string::npos || st.find(vt) != std::string::npos)
                    matches.insert("title");
            }
            if (video.year && sub.year == video.year) matches.insert("year");
        }
        std::string fam = normSrcForCompare(video.source, sub.source);
        if (!fam.empty()) matches.insert("source");
        // edition: repack/proper/internal on a *subtitle* name usually means matching that release
        if (!video.edition.empty() && !sub.edition.empty() && video.edition == sub.edition)
            matches.insert("edition");
        // streaming service counts as source-ish minor
        if (!video.streamingService.empty() && video.streamingService == sub.streamingService)
            matches.insert("streaming_service");
        if (!video.resolution.empty() && video.resolution == sub.resolution)
            matches.insert("resolution");
        if (!video.videoCodec.empty() && video.videoCodec == sub.videoCodec)
            matches.insert("video_codec");
        if (!video.releaseGroup.empty() && video.releaseGroup == sub.releaseGroup)
            matches.insert("release_group");
        if (matches.count("hash")) {
            // hash is only valid when video_codec/source also agree (subliminal movie_hash_valid_if)
            if (matches.count("video_codec") && matches.count("source")) {
                // Valid hash ⇒ discard everything else, score = hash weight
                return MOVIE_HASH;
            }
            matches.erase("hash");
        }
        int w = 0;
        if (matches.count("title")) w += MOVIE_TITLE;
        if (matches.count("year")) w += MOVIE_YEAR;
        if (matches.count("source")) w += MOVIE_SOURCE;
        if (matches.count("edition")) w += MOVIE_EDITION;
        if (matches.count("release_group")) w += MOVIE_GROUP;
        if (matches.count("resolution") || matches.count("video_codec") ||
            matches.count("streaming_service") || matches.count("hearing_impaired"))
            w += MOVIE_MINOR * (int)(matches.count("resolution") + matches.count("video_codec") +
                                     matches.count("streaming_service"));
        return w;
    }

    // episode
    if (imdbEq) {
        matches.insert("series"); matches.insert("year");
        matches.insert("season"); matches.insert("episode");
    } else {
        if (!video.title.empty() && !sub.title.empty()) {
            std::string vt = util::lower(video.title), st = util::lower(sub.title);
            if (vt == st || vt.find(st) != std::string::npos || st.find(vt) != std::string::npos)
                matches.insert("series");
        }
        if (video.year && sub.year == video.year) matches.insert("year");
        if (video.season && sub.season == video.season) matches.insert("season");
        if (video.episode && sub.episode == video.episode) matches.insert("episode");
    }
    std::string fam = normSrcForCompare(video.source, sub.source);
    if (!fam.empty()) matches.insert("source");
    if (!video.releaseGroup.empty() && video.releaseGroup == sub.releaseGroup)
        matches.insert("release_group");
    if (!video.resolution.empty() && video.resolution == sub.resolution)
        matches.insert("resolution");
    if (!video.videoCodec.empty() && video.videoCodec == sub.videoCodec)
        matches.insert("video_codec");
    if (!video.streamingService.empty() && video.streamingService == sub.streamingService)
        matches.insert("streaming_service");
    if (matches.count("hash")) {
        // valid if series+season+episode+source
        if (matches.count("series") && matches.count("season") &&
            matches.count("episode") && matches.count("source"))
            return EP_HASH;
        matches.erase("hash");
    }
    int w = 0;
    if (matches.count("series")) w += EP_SERIES;
    if (matches.count("year")) w += EP_YEAR;
    if (matches.count("season")) w += EP_SEASON;
    if (matches.count("episode")) w += EP_EPISODE;
    if (matches.count("source")) w += EP_SOURCE;
    if (matches.count("release_group")) w += EP_GROUP;
    w += EP_MINOR * (int)(matches.count("resolution") + matches.count("video_codec") +
                          matches.count("streaming_service") + matches.count("audio_codec"));
    return w;
}

// Parse subtitle release hashes from API "infos"/"movies" rows.
std::vector<uint64_t> candHashes(const json& attrs) {
    std::vector<uint64_t> out;
    if (attrs.contains("movies") && attrs["movies"].is_array()) {
        for (auto& m : attrs["movies"]) {
            if (m.is_object() && m.contains("moviehashes") && m["moviehashes"].is_array()) {
                for (auto& h : m["moviehashes"])
                    if (h.is_string()) {
                        uint64_t v = parseHexHash(h.get<std::string>());
                        if (v) out.push_back(v);
                    }
            }
        }
    }
    return out;
}

int pickBestFileId(const json& data, const std::string& lang,
                   const ReleaseInfo& video, const std::string& videoHashHex,
                   const std::string& videoImdb, bool isEpisode, int minScore) {
    int bestId = 0, bestScore = -1, bestCount = -1;
    std::string bestName;
    if (!data.is_array()) return 0;
    for (auto& item : data) {
        auto attrs = item.value("attributes", json::object());
        if (util::lower(attrs.value("language", "")) != util::lower(lang)) continue;
        auto files = attrs.value("files", json::array());
        if (!files.is_array() || files.empty()) continue;

        SubCandidate c;
        c.fileId = files[0].value("file_id", 0);
        if (c.fileId <= 0) continue;
        c.fileName = files[0].value("file_name", std::string());
        c.downloadCount = attrs.value("download_count", 0);
        c.cdNumber = files[0].value("cd_number", 1);
        if (c.cdNumber != 1) continue; // skip multi-cd part 2+

        // subtitle-side feature imdb id if present
        std::string candImdb;
        if (attrs.contains("feature_details") && attrs["feature_details"].is_object())
            candImdb = attrs["feature_details"].value("imdb_id", std::string());

        // hash match against our computed moviehash
        if (!videoHashHex.empty()) {
            uint64_t want = parseHexHash(videoHashHex);
            for (uint64_t h : candHashes(attrs)) {
                if (want && h && (h & 0xFFFFFFFFFFFFFFFFull) == want) { c.hashMatch = true; break; }
            }
            // API also exposes top-level moviehash on the subtitle when using moviehash filter
            std::string mh = attrs.value("moviehash", std::string());
            if (!mh.empty() && parseHexHash(mh) == want) c.hashMatch = true;
            // release sometimes embeds hash as 16-hex token in file name
            auto toks = tokenizeName(util::lower(c.fileName));
            for (auto& t : toks) if (parseHexHash(t) == want) { c.hashMatch = true; break; }
        }

        c.score = scoreSubtitle(c, video, isEpisode, videoImdb, candImdb);

        // Bazarr: prefer >= min score; among those pick highest score, then download_count.
        bool better = false;
        if (c.score > bestScore) better = true;
        else if (c.score == bestScore && c.downloadCount > bestCount) better = true;
        if (better) {
            bestScore = c.score;
            bestCount = c.downloadCount;
            bestId = c.fileId;
            bestName = c.fileName;
        }
    }
    // min score acts as adaptive-search floor (percent of max), but a hash match always passes
    int maxScore = isEpisode ? EP_MAX : MOVIE_MAX;
    int thresh = (int)(maxScore * (minScore / 100.0));
    if (bestId > 0 && bestScore < thresh) {
        // keep only if hash matched, else fail like Bazarr (no low-quality grab)
        // re-check if the chosen one had hash: approximate by score jump
        if (bestScore < MOVIE_HASH && bestScore < EP_HASH) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "score %d < min %d", bestScore, thresh);
            setStatus(std::string(i18n::tr("subs.weak_match")) + buf + ") — " + bestName);
            return 0;
        }
    }
    return bestId;
}

bool downloadOne(const Job& job, const std::string& lang, const Snap& cfg) {
    fs::path video(job.videoPath);
    if (hasSidecar(video, lang)) return true;

    bool isEpisode = (job.type == MediaType::TV || job.season > 0);

    // Build the video "release object" exactly like Bazarr: guess from scene name
    // when available, else from the filename; then overlay DB title/year/ids.
    std::string nameForGuess = job.sceneName.empty() ? video.filename().string() : job.sceneName;
    ReleaseInfo info = parseReleaseName(nameForGuess);
    // Prefer authoritative metadata from the request/library over filename guesses.
    if (!job.title.empty()) {
        // Only trust DB title for the title match if filename guess looks unrelated.
        if (info.title.empty()) info.title = job.title;
    }
    if (isEpisode) {
        if (job.season > 0) info.season = job.season;
        if (job.episode > 0) info.episode = job.episode;
    }
    std::string videoHashHex = movieHash(job.videoPath);

    std::map<std::string, std::string> q;
    q["languages"] = lang;
    // Ask for many candidates so the scorer has real choices; the scoring below
    // is what actually decides (like Bazarr), download_count is only a tiebreak.
    q["limit"] = "40";
    if (isEpisode) {
        q["type"] = "episode";
        if (job.tmdbId > 0) q["parent_tmdb_id"] = std::to_string(job.tmdbId);
        if (job.season > 0) q["season_number"] = std::to_string(job.season);
        if (job.episode > 0) q["episode_number"] = std::to_string(job.episode);
    } else {
        q["type"] = "movie";
        if (job.tmdbId > 0) q["tmdb_id"] = std::to_string(job.tmdbId);
    }
    if (!job.imdbId.empty()) q["imdb_id"] = stripImdb(job.imdbId);
    if (!videoHashHex.empty()) {
        // Hash search is the most accurate path (Bazarr queries moviehash first).
        q["moviehash"] = videoHashHex;
    } else if (!job.title.empty() && job.tmdbId <= 0 && job.imdbId.empty()) {
        q["query"] = job.title;
    }

    std::string url = std::string(kApiBase) + "/subtitles?" + util::buildQuery(q);
    setStatus(std::string(i18n::tr("subs.searching")) + lang + "…");
    auto sr = http::get(url, "application/json", apiHeaders(cfg.apiKey, tokenCopy()));
    if (!sr.ok()) {
        setStatus(std::string(i18n::tr("subs.search_error")) + lang + " (" + std::to_string(sr.status) + ")");
        return false;
    }

    // Bazarr score floors: movies 50%, episodes 70% of max; hash always passes.
    int minScore = isEpisode ? 70 : 50;
    int fileId = 0;
    try {
        auto j = json::parse(sr.body);
        auto data = j.value("data", json::array());
        fileId = pickBestFileId(data, lang, info, videoHashHex, job.imdbId, isEpisode, minScore);
        // If a strict moviehash query returned nothing scorable, retry a broader
        // title/tmdb query (still scored) — matches Bazarr trying multiple hints.
        if (fileId <= 0) {
            q.erase("moviehash");
            if (isEpisode && !job.title.empty())
                q["query"] = job.title + " s" +
                    (job.season < 10 ? "0" : "") + std::to_string(job.season) +
                    "e" + (job.episode < 10 ? "0" : "") + std::to_string(job.episode);
            else if (!job.title.empty())
                q["query"] = job.title;
            std::string url2 = std::string(kApiBase) + "/subtitles?" + util::buildQuery(q);
            auto sr2 = http::get(url2, "application/json", apiHeaders(cfg.apiKey, tokenCopy()));
            if (sr2.ok()) {
                auto j2 = json::parse(sr2.body);
                fileId = pickBestFileId(j2.value("data", json::array()), lang, info,
                                        videoHashHex, job.imdbId, isEpisode, minScore);
            }
        }
    } catch (...) {
        setStatus(i18n::tr("subs.bad_search"));
        return false;
    }
    if (fileId <= 0) {
        if (statusMessage().find(i18n::tr("subs.weak_marker")) == std::string::npos)
            setStatus(std::string(i18n::tr("subs.no_match")) + lang + " for " + video.filename().string());
        return false;
    }

    setStatus(std::string(i18n::tr("subs.downloading")) + lang + "…");
    json body{{"file_id", fileId}};
    auto dr = http::post(std::string(kApiBase) + "/download", body.dump(), "application/json",
                         apiHeaders(cfg.apiKey, tokenCopy()));
    if (dr.status == 401 || dr.status == 403) {
        // force re-login next time
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_token.clear();
            g_tokenAt = 0;
        }
        setStatus(i18n::tr("subs.session_expired"));
        return false;
    }
    if (dr.status == 406) {
        setStatus(i18n::tr("subs.daily_limit"));
        return false;
    }
    if (!dr.ok()) {
        setStatus(std::string(i18n::tr("subs.download_error")) + std::to_string(dr.status) + ")");
        return false;
    }

    std::string link;
    try {
        auto j = json::parse(dr.body);
        link = j.value("link", "");
    } catch (...) {}
    if (link.empty()) {
        setStatus(i18n::tr("subs.no_link"));
        return false;
    }

    std::string err;
    auto bytes = http::getBinary(link, &err);
    if (bytes.empty()) {
        setStatus(std::string(i18n::tr("subs.fetch_failed")) + err + ")");
        return false;
    }
    std::string content(bytes.begin(), bytes.end());
    if (!looksLikeSrt(content)) {
        // try as text response somehow
        setStatus(i18n::tr("subs.not_srt"));
        return false;
    }

    std::string outPath = sidecarPath(video, lang);
    if (!util::writeFile(outPath, content)) {
        setStatus(std::string(i18n::tr("subs.save_failed")) + outPath);
        return false;
    }
    setStatus(std::string(i18n::tr("subs.saved")) + fs::path(outPath).filename().string());
    return true;
}

void processJob(Job job) {
    Snap cfg = cfgSnap();
    if (!cfg.autoOn) return;

    std::vector<std::string> langs;
    langs.push_back("en");
    std::string pref = util::lower(cfg.lang);
    if (pref.empty()) pref = "pl";
    if (pref != "en") langs.push_back(pref);

    // Infer SxxExx if missing
    if ((job.type == MediaType::TV || job.season <= 0) && job.episode <= 0) {
        int s = 0, e = 0;
        if (parseSeasonEpisode(fs::path(job.videoPath).filename().string(), s, e)) {
            job.season = s;
            job.episode = e;
            if (job.type != MediaType::TV) job.type = MediaType::TV;
        }
    }

    bool osConfigured = !cfg.apiKey.empty();

    for (auto& lang : langs) {
        if (g_stop.load()) return;
        if (hasSidecar(job.videoPath, lang)) continue;

        // Bazarr queries ALL enabled providers and picks the best score. We keep the
        // same idea but simpler: try keyless providers first (exact hash match),
        // then fall back to OpenSubtitles when configured.
        bool got = tryKeyless(job, lang);

        if (!got && osConfigured) {
            if (ensureToken(cfg)) {
                downloadOne(job, lang, cfg);
                got = hasSidecar(job.videoPath, lang);
            }
        }

        if (!got && !osConfigured && lang == pref)
            setStatus(std::string(i18n::tr("subs.missing")) + lang + i18n::tr("subs.need_os_key"));
        // gentle rate limit
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
}

void workerMain() {
    while (!g_stop.load()) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait_for(lk, std::chrono::milliseconds(500), [] {
                return g_stop.load() || !g_q.empty();
            });
            if (g_stop.load()) break;
            if (g_q.empty()) {
                g_busy = false;
                continue;
            }
            job = std::move(g_q.front());
            g_q.pop_front();
            g_busy = true;
        }
        std::string pathKey = job.videoPath;
        try {
            processJob(std::move(job));
        } catch (...) {
            setStatus(i18n::tr("subs.exception"));
        }
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_queued.erase(pathKey); // allow re-queue after finish/fail
        }
    }
    g_busy = false;
}

std::string jobKey(const Job& j, const std::string& lang) {
    return j.videoPath + "|" + lang;
}

void pushJob(Job j) {
    if (j.videoPath.empty()) return;
    std::error_code ec;
    if (!fs::exists(j.videoPath, ec) || !fs::is_regular_file(j.videoPath, ec)) return;
    if (!isVideoExt(j.videoPath)) return;

    auto cfg = cfgSnap();
    std::string pref = util::lower(cfg.lang);
    if (pref.empty()) pref = "pl";

    bool need = !hasSidecar(j.videoPath, "en") || (pref != "en" && !hasSidecar(j.videoPath, pref));
    if (!need) return;

    std::lock_guard<std::mutex> lk(g_mu);
    std::string k = j.videoPath;
    if (g_queued.count(k)) return;
    g_queued.insert(k);
    g_q.push_back(std::move(j));
    g_cv.notify_one();
}

} // namespace

void init() {
    g_stop = false;
    refreshConfig();
    setStatus(i18n::tr("subs.done"));
    if (g_worker.joinable()) return;
    g_worker = std::thread(workerMain);
    // Delayed library scan
    core::enqueue([] {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        if (!g_stop.load() && stack::StackConfig::get().subsAuto)
            scanLibrary();
    });
}

void shutdown() {
    g_stop = true;
    g_cv.notify_all();
    if (g_worker.joinable()) g_worker.join();
    std::lock_guard<std::mutex> lk(g_mu);
    g_q.clear();
    g_queued.clear();
    g_token.clear();
}

void tick() {
    // keep the worker's config snapshot fresh (cheap; once per second is plenty)
    static int64_t lastMs = 0;
    int64_t now = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - lastMs < 1000) return;
    lastMs = now;
    refreshConfig();
}

void enqueue(const std::string& videoPath, MediaType type, int tmdbId,
             const std::string& imdbId, const std::string& title,
             int season, int episode, const std::string& sceneName) {
    if (!stack::StackConfig::get().subsAuto) return;
    Job j;
    j.videoPath = videoPath;
    j.type = type;
    j.tmdbId = tmdbId;
    j.imdbId = imdbId;
    j.title = title;
    j.season = season;
    j.episode = episode;
    j.sceneName = sceneName;
    pushJob(std::move(j));
}

void enqueuePath(const std::string& pathOrFolder, MediaType type, int tmdbId,
                 const std::string& imdbId, const std::string& title,
                 const std::string& sceneName) {
    if (!stack::StackConfig::get().subsAuto) return;
    std::error_code ec;
    fs::path root(pathOrFolder);
    if (!fs::exists(root, ec)) return;

    auto enqueueFile = [&](const fs::path& p) {
        int s = 0, e = 0;
        parseSeasonEpisode(p.filename().string(), s, e);
        // Pass the torrent's release folder name as scene hint when provided.
        enqueue(p.string(), type, tmdbId, imdbId, title, s, e, sceneName);
    };

    if (fs::is_regular_file(root, ec) && isVideoExt(root)) {
        enqueueFile(root);
        return;
    }
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;
        if (!isVideoExt(it->path())) continue;
        enqueueFile(it->path());
    }
}

void doScanLibrary() {
    auto items = library::scan();
    int n = 0;
    for (auto& it : items) {
        std::string playable = library::resolvePlayable(it.path.empty() ? it.folder : it.path);
        if (playable.empty()) continue;
        // For folders with many episodes, enqueue whole folder
        std::error_code ec;
        if (!it.folder.empty() && fs::is_directory(it.folder, ec) && it.mediaType == MediaType::TV) {
            enqueuePath(it.folder, it.mediaType, it.tmdbId, {}, it.title);
        } else {
            enqueue(playable, it.mediaType, it.tmdbId, {}, it.title, 0, 0);
        }
        ++n;
    }
    // Also Available requests
    for (auto& r : stack::listRequests()) {
        if (r.status != stack::ReqStatus::Available) continue;
        if (r.libraryPath.empty()) continue;
        enqueuePath(r.libraryPath, r.mediaType, r.tmdbId, r.imdbId, r.title);
        ++n;
    }
    setStatus(std::string(i18n::tr("subs.queued_scan")) + std::to_string(n) + i18n::tr("subs.queued_items"));
}

void scanLibrary() {
    if (!cfgSnap().autoOn) {
        setStatus(i18n::tr("subs.auto_off"));
        return;
    }
    setStatus(i18n::tr("subs.scanning"));
    // Disk walk + per-file sidecar checks are slow — never run them on the UI thread.
    core::enqueue([] { doScanLibrary(); });
}

std::string statusMessage() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_status;
}

bool busy() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_busy.load() || !g_q.empty();
}

} // namespace subs
