#include "ytplayer.hpp"
#include "core.hpp"
#include "http.hpp"
#include "i18n.hpp"
#include "platform.hpp"
#include "player.hpp"
#include "util.hpp"
#include "json.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using json = nlohmann::json;

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

int heightOf(const json& f) {
    if (f.contains("height") && f["height"].is_number_integer())
        return f["height"].get<int>();
    std::string q = f.value("qualityLabel", f.value("quality", ""));
    int v = 0;
    for (char c : q) {
        if (c >= '0' && c <= '9') v = v * 10 + (c - '0');
        else if (v > 0) break;
    }
    return v;
}

bool isAudioOnly(const json& f) {
    std::string mime = util::lower(f.value("mimeType", ""));
    return mime.rfind("audio/", 0) == 0;
}

bool isVideo(const json& f) {
    std::string mime = util::lower(f.value("mimeType", ""));
    return mime.rfind("video/", 0) == 0;
}

// Lightweight YouTube InnerTube (ANDROID client) — returns direct stream URLs,
// no JS signature decrypt, no yt-dlp, no browser.
bool tryInnerTube(const std::string& videoId, std::string* videoUrl, std::string* audioUrl) {
    if (videoUrl) videoUrl->clear();
    if (audioUrl) audioUrl->clear();

    struct Client {
        const char* name;
        const char* version;
        const char* ua;
        const char* clientNameHeader; // X-YouTube-Client-Name
    };
    // ANDROID regularly exposes clear `url` fields (no signatureCipher).
    static const Client clients[] = {
        {"ANDROID", "20.10.38",
         "com.google.android.youtube/20.10.38 (Linux; U; Android 11) gzip", "3"},
        {"ANDROID", "19.29.37",
         "com.google.android.youtube/19.29.37 (Linux; U; Android 11) gzip", "3"},
    };

    for (auto& c : clients) {
        json body = {
            {"context",
             {{"client",
               {{"clientName", c.name},
                {"clientVersion", c.version},
                {"androidSdkVersion", 30},
                {"hl", "en"},
                {"gl", "US"},
                {"userAgent", c.ua}}}}},
            {"videoId", videoId},
            {"contentCheckOk", true},
            {"racyCheckOk", true},
        };
        std::string headers =
            std::string("User-Agent: ") + c.ua + "\r\n"
            "X-YouTube-Client-Name: " + c.clientNameHeader + "\r\n"
            "X-YouTube-Client-Version: " + c.version + "\r\n";

        auto r = http::post("https://www.youtube.com/youtubei/v1/player?prettyPrint=false",
                            body.dump(), "application/json", headers);
        if (!r.ok()) continue;

        try {
            auto j = json::parse(r.body, nullptr, false);
            if (!j.is_object()) continue;
            auto status = j.value("/playabilityStatus/status"_json_pointer, std::string{});
            if (status != "OK" && status != "LIVE_STREAM_OFFLINE") continue;
            if (!j.contains("streamingData") || !j["streamingData"].is_object()) continue;
            auto& sd = j["streamingData"];

            // Prefer adaptive 720p (video + audio). Muxed progressives are often only 360p.
            constexpr int kTargetH = 720;

            auto pickAudio = [&](std::string* outAud) {
                if (!outAud || !sd.contains("adaptiveFormats") || !sd["adaptiveFormats"].is_array())
                    return;
                int bestAudBr = -1;
                for (auto& f : sd["adaptiveFormats"]) {
                    if (!f.is_object() || !isAudioOnly(f)) continue;
                    std::string url = f.value("url", "");
                    if (!looksLikeUrl(url)) continue;
                    int br = f.value("bitrate", f.value("averageBitrate", 0));
                    std::string mime = util::lower(f.value("mimeType", ""));
                    // Prefer m4a/mp4a for libVLC + mp4 video
                    if (br > bestAudBr ||
                        (br == bestAudBr && mime.find("mp4a") != std::string::npos)) {
                        bestAudBr = br;
                        *outAud = url;
                    }
                }
            };

            auto pickAdaptiveVideo = [&](bool require720, std::string* outVid) -> bool {
                if (!sd.contains("adaptiveFormats") || !sd["adaptiveFormats"].is_array())
                    return false;
                std::string bestVid;
                int bestScore = -1;
                for (auto& f : sd["adaptiveFormats"]) {
                    if (!f.is_object() || !isVideo(f)) continue;
                    std::string url = f.value("url", "");
                    if (!looksLikeUrl(url)) continue;
                    int h = heightOf(f);
                    if (h <= 0) continue;
                    if (require720 && h != kTargetH) continue;
                    if (!require720 && h > kTargetH) continue; // never above target unless exact-720 pass
                    // Exact 720 wins; otherwise highest below 720.
                    int score = (h == kTargetH) ? 100000 + h : h;
                    std::string mime = util::lower(f.value("mimeType", ""));
                    bool avc = mime.find("avc1") != std::string::npos;
                    if (score > bestScore || (score == bestScore && avc)) {
                        bestScore = score;
                        bestVid = url;
                    }
                }
                if (bestVid.empty()) return false;
                *outVid = bestVid;
                return true;
            };

            // 1) Exact 720p adaptive + audio
            {
                std::string vid, aud;
                if (pickAdaptiveVideo(true, &vid)) {
                    pickAudio(&aud);
                    *videoUrl = vid;
                    if (audioUrl) *audioUrl = aud;
                    return true;
                }
            }
            // 2) Best adaptive ≤720p + audio
            {
                std::string vid, aud;
                if (pickAdaptiveVideo(false, &vid)) {
                    pickAudio(&aud);
                    *videoUrl = vid;
                    if (audioUrl) *audioUrl = aud;
                    return true;
                }
            }
            // 3) Muxed progressive (often 360p only) — last resort single URL
            if (sd.contains("formats") && sd["formats"].is_array()) {
                std::string best;
                int bestScore = -1;
                for (auto& f : sd["formats"]) {
                    if (!f.is_object()) continue;
                    std::string url = f.value("url", "");
                    if (!looksLikeUrl(url)) continue;
                    int h = heightOf(f);
                    if (h <= 0) h = 360;
                    int score = (h == kTargetH) ? 100000 + h : (h <= kTargetH ? h : -1);
                    if (score < 0) continue;
                    if (score > bestScore) {
                        bestScore = score;
                        best = url;
                    }
                }
                if (!best.empty()) {
                    *videoUrl = best;
                    return true;
                }
            }

            // 4) HLS manifest if present
            std::string hls = sd.value("hlsManifestUrl", "");
            if (looksLikeUrl(hls)) {
                *videoUrl = hls;
                return true;
            }
        } catch (...) {}
    }
    return false;
}

bool resolveStream(const std::string& videoId, std::string* videoUrl, std::string* audioUrl) {
    if (videoId.empty() || !videoUrl) return false;
    return tryInnerTube(videoId, videoUrl, audioUrl);
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
        std::string video, audio;
        bool ok = resolveStream(videoId, &video, &audio);
        std::lock_guard<std::mutex> lk(job->mu);
        job->url = std::move(video);
        job->audioUrl = std::move(audio);
        job->ok = ok && !job->url.empty();
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
        platform::openUrl("https://www.youtube.com/watch?v=" + videoId);
    }
}

} // namespace ytplayer
