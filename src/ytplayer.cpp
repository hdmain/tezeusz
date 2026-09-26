#include "ytplayer.hpp"
#include "core.hpp"
#include "http.hpp"
#include "i18n.hpp"
#include "platform.hpp"
#include "player.hpp"
#include "util.hpp"
#include "json.hpp"

#include <atomic>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>
#include <cstdlib>
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace ytplayer {
namespace {

struct PendingPlay {
    std::mutex mu;
    bool done = false;
    std::string url;
    std::string audioUrl;
    std::string title;
    std::string videoId;
    bool ok = false;
};

std::mutex g_mu;
std::shared_ptr<PendingPlay> g_pending;
std::atomic<bool> g_busy{false};

bool looksLikeUrl(const std::string& s) {
    return s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0;
}

int parseQuality(const std::string& q) {
    int v = 0;
    for (char c : q) {
        if (c >= '0' && c <= '9') v = v * 10 + (c - '0');
        else if (v > 0) break;
    }
    return v;
}

// Prefer muxed progressive MP4 around 720p (single URL for libVLC).
std::string pickMuxed(const json& streams, const char* urlKey, const char* qualityKey,
                      const char* videoOnlyKey) {
    std::string best;
    int bestQ = -1;
    if (!streams.is_array()) return {};
    for (auto& s : streams) {
        if (!s.is_object()) continue;
        if (videoOnlyKey && s.contains(videoOnlyKey) && s[videoOnlyKey].is_boolean() &&
            s[videoOnlyKey].get<bool>())
            continue;
        std::string url = s.value(urlKey, "");
        if (!looksLikeUrl(url)) continue;
        std::string mime = util::lower(s.value("mimeType", s.value("type", "")));
        if (!mime.empty() && mime.find("video") == std::string::npos &&
            mime.find("mp4") == std::string::npos && mime.find("webm") == std::string::npos)
            continue;
        int q = parseQuality(s.value(qualityKey, s.value("qualityLabel", "")));
        if (q <= 0) q = s.value("height", 0);
        // Prefer <=720, then closest below, else anything
        int score = q;
        if (q > 720) score = 720 - (q - 720); // penalize higher
        if (score > bestQ) {
            bestQ = score;
            best = url;
        }
    }
    return best;
}

std::string tryInvidious(const std::string& videoId) {
    static const char* hosts[] = {
        "https://inv.riverside.rocks",
        "https://invidious.protokolla.fi",
        "https://yewtu.be",
        "https://inv.nadeko.net",
        "https://invidious.nerdvpn.de",
        "https://iv.ggtyler.dev",
    };
    for (auto* host : hosts) {
        auto r = http::get(std::string(host) + "/api/v1/videos/" + videoId);
        if (!r.ok()) continue;
        try {
            auto j = json::parse(r.body, nullptr, false);
            if (!j.is_object()) continue;
            std::string url = pickMuxed(j.value("formatStreams", json::array()), "url", "quality", nullptr);
            if (!url.empty()) return url;
            std::string hls = j.value("hlsUrl", "");
            if (looksLikeUrl(hls)) return hls;
            // Don't use adaptiveFormats — usually video/audio split.
        } catch (...) {}
    }
    return {};
}

std::string tryPiped(const std::string& videoId) {
    static const char* hosts[] = {
        "https://pipedapi.kavin.rocks",
        "https://pipedapi.adminforge.de",
        "https://pipedapi.ducks.party",
        "https://api.piped.private.coffee",
    };
    for (auto* host : hosts) {
        auto r = http::get(std::string(host) + "/streams/" + videoId);
        if (!r.ok()) continue;
        try {
            auto j = json::parse(r.body, nullptr, false);
            if (!j.is_object()) continue;
            std::string url = pickMuxed(j.value("videoStreams", json::array()), "url", "quality", "videoOnly");
            if (!url.empty()) return url;
            std::string hls = j.value("hls", "");
            if (looksLikeUrl(hls)) return hls;
            std::string dash = j.value("dash", "");
            if (looksLikeUrl(dash)) return dash;
        } catch (...) {}
    }
    return {};
}

std::string runCapture(const std::string& cmdLine) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return {};
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::string mutableCmd = cmdLine;
    BOOL ok = CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return {};
    }
    std::string out;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0)
        out.append(buf, buf + n);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 45000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return out;
#else
    FILE* p = popen(cmdLine.c_str(), "r");
    if (!p) return {};
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof(buf), p)) out += buf;
    pclose(p);
    return out;
#endif
}

std::string shellQuote(const std::string& s) {
#ifdef _WIN32
    std::string o = "\"";
    for (char c : s) {
        if (c == '"') o += "\\\"";
        else o += c;
    }
    o += "\"";
    return o;
#else
    std::string o = "'";
    for (char c : s) {
        if (c == '\'') o += "'\\''";
        else o += c;
    }
    o += "'";
    return o;
#endif
}

std::string findYtDlp() {
    std::error_code ec;
    const fs::path candidates[] = {
        fs::path(util::exeDir()) / "yt-dlp.exe",
        fs::path(util::exeDir()) / "yt-dlp",
        fs::path(util::appDataPath("yt-dlp.exe")),
        fs::path(util::appDataPath("yt-dlp")),
    };
    for (auto& p : candidates) {
        if (fs::exists(p, ec) && fs::is_regular_file(p, ec) && fs::file_size(p, ec) > 100000)
            return p.string();
    }
#ifdef _WIN32
    return {};
#else
    // May be on PATH
    return "yt-dlp";
#endif
}

bool ensureYtDlp(std::string* outPath) {
    std::string existing = findYtDlp();
#ifdef _WIN32
    if (!existing.empty()) {
        if (outPath) *outPath = existing;
        return true;
    }
#else
    if (!existing.empty() && existing != "yt-dlp") {
        if (outPath) *outPath = existing;
        return true;
    }
    // Probe PATH
    if (system("yt-dlp --version >/dev/null 2>&1") == 0) {
        if (outPath) *outPath = "yt-dlp";
        return true;
    }
#endif

#ifdef _WIN32
    const char* url = "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe";
    const std::string dest = util::appDataPath("yt-dlp.exe");
#else
    const char* url = "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp";
    const std::string dest = util::appDataPath("yt-dlp");
#endif
    std::string err;
    auto bytes = http::getBinaryLong(url, &err, 120);
    if (bytes.size() < 100000) return false;
    {
        std::ofstream f(dest, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write((const char*)bytes.data(), (std::streamsize)bytes.size());
        if (!f) return false;
    }
#ifndef _WIN32
    chmod(dest.c_str(), 0755);
#endif
    if (outPath) *outPath = dest;
    return true;
}

std::string tryYtDlp(const std::string& videoId, std::string* audioOut) {
    if (audioOut) audioOut->clear();
    std::string bin;
    if (!ensureYtDlp(&bin) || bin.empty()) return {};
    const std::string watch = "https://www.youtube.com/watch?v=" + videoId;
    // -g / --skip-download: only print stream URLs — never write the video to disk.
    std::string cmd = shellQuote(bin) +
        " --skip-download -g -f \"bv*[height<=720]+ba/b\" "
        "--no-playlist --no-warnings --no-check-certificates "
        "--no-part --no-mtime " +
        shellQuote(watch);
    std::string out = runCapture(cmd);
    std::vector<std::string> urls;
    size_t i = 0;
    while (i < out.size()) {
        size_t e = out.find('\n', i);
        if (e == std::string::npos) e = out.size();
        std::string line = util::trim(out.substr(i, e - i));
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (looksLikeUrl(line)) urls.push_back(line);
        i = e + 1;
    }
    if (urls.empty()) return {};
    if (urls.size() >= 2 && audioOut) *audioOut = urls[1];
    return urls[0];
}

std::string resolveStreamUrl(const std::string& videoId, std::string* audioOut) {
    if (videoId.empty()) return {};
    if (auto u = tryYtDlp(videoId, audioOut); !u.empty()) return u;
    if (audioOut) audioOut->clear();
    if (auto u = tryInvidious(videoId); !u.empty()) return u;
    if (auto u = tryPiped(videoId); !u.empty()) return u;
    return {};
}

} // namespace

void open(const std::string& videoId, const std::string& title) {
    if (videoId.empty()) return;
    if (g_busy.exchange(true)) return;

    auto job = std::make_shared<PendingPlay>();
    job->title = title.empty() ? (std::string(i18n::tr("details.trailer")) + " — " + videoId)
                               : title;
    job->videoId = videoId;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_pending = job;
    }

    core::enqueue([job, videoId]() {
        std::string audio;
        std::string url = resolveStreamUrl(videoId, &audio);
        std::lock_guard<std::mutex> lk(job->mu);
        job->url = std::move(url);
        job->audioUrl = std::move(audio);
        job->ok = !job->url.empty();
        job->done = true;
        g_busy.store(false);
    });
}

bool isOpen() { return player::isOpen(); }
void close() { player::close(); }

void tick() {
    std::shared_ptr<PendingPlay> job;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_pending) return;
        {
            std::lock_guard<std::mutex> jlk(g_pending->mu);
            if (!g_pending->done) return;
        }
        job = g_pending;
        g_pending.reset();
    }

    std::string url, audio, title, videoId;
    bool ok = false;
    {
        std::lock_guard<std::mutex> jlk(job->mu);
        url = job->url;
        audio = job->audioUrl;
        title = job->title;
        videoId = job->videoId;
        ok = job->ok;
    }

    if (ok && !url.empty()) {
        player::open(url, title, audio);
    } else {
        // Last resort: system browser (never spawn a headless browser ourselves).
        platform::openUrl("https://www.youtube.com/watch?v=" + videoId);
    }
}

} // namespace ytplayer
