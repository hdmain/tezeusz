#include "ytplayer.hpp"
#include "core.hpp"
#include "http.hpp"
#include "i18n.hpp"
#include "platform.hpp"
#include "player.hpp"
#include "util.hpp"
#include "widgets.hpp"
#include "json.hpp"
#include "imgui.h"

#include <atomic>
#include <cfloat>
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
    bool cancelled = false;
    std::string url;
    std::string audioUrl;
    std::string title;
    std::string videoId;
    bool ok = false;
};

std::mutex g_mu;
std::shared_ptr<PendingPlay> g_pending;
std::atomic<bool> g_busy{false};
std::string g_activeVideoId; // for overlay / Go to YouTube while resolving


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
        g_activeVideoId = videoId;
    }

    core::enqueue([job, videoId]() {
        std::string video, audio;
        bool ok = resolveStream(videoId, &video, &audio);
        std::lock_guard<std::mutex> lk(job->mu);
        if (job->cancelled) {
            job->done = true;
            g_busy.store(false);
            return;
        }
        job->url = std::move(video);
        job->audioUrl = std::move(audio);
        job->ok = ok && !job->url.empty();
        job->done = true;
        g_busy.store(false);
    });
}

bool isOpen() { return player::isOpen(); }

bool isResolving() {
    if (g_busy.load()) return true;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_pending) return false;
    std::lock_guard<std::mutex> jlk(g_pending->mu);
    return !g_pending->done && !g_pending->cancelled;
}

void cancel() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_pending) {
        std::lock_guard<std::mutex> jlk(g_pending->mu);
        g_pending->cancelled = true;
        g_pending->done = true;
    }
    g_pending.reset();
    g_activeVideoId.clear();
    g_busy.store(false);
}

void openOnYoutube() {
    std::string id;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        id = g_activeVideoId;
        if (id.empty() && g_pending) {
            std::lock_guard<std::mutex> jlk(g_pending->mu);
            id = g_pending->videoId;
        }
    }
    cancel();
    if (!id.empty())
        platform::openUrl("https://www.youtube.com/watch?v=" + id);
}

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
        g_activeVideoId.clear();
    }

    std::string url, audio, title, videoId;
    bool ok = false;
    bool cancelled = false;
    {
        std::lock_guard<std::mutex> jlk(job->mu);
        url = job->url;
        audio = job->audioUrl;
        title = job->title;
        videoId = job->videoId;
        ok = job->ok;
        cancelled = job->cancelled;
    }
    if (cancelled) return;

    if (ok && !url.empty()) {
        player::open(url, title, audio);
    } else {
        platform::openUrl("https://www.youtube.com/watch?v=" + videoId);
    }
}

void drawOverlay() {
    if (!isResolving()) return;

    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    const ImVec2 dsp = io.DisplaySize;
    fg->AddRectFilled(ImVec2(0, 0), dsp, IM_COL32(6, 10, 18, 180));

    const float cardW = 340.f, cardH = 200.f;
    const ImVec2 cardMin((dsp.x - cardW) * 0.5f, (dsp.y - cardH) * 0.5f);
    const ImVec2 cardMax(cardMin.x + cardW, cardMin.y + cardH);
    fg->AddRectFilled(cardMin, cardMax, IM_COL32(17, 24, 39, 245), 16.f);
    fg->AddRect(cardMin, cardMax, IM_COL32(55, 65, 81, 255), 16.f, 0, 1.2f);

    const ImVec2 spinCtr(cardMin.x + cardW * 0.5f, cardMin.y + 62.f);
    w::orbitSpinner(fg, spinCtr, 16.f, 3.4f);

    const char* msg = i18n::tr("yt.loading");
    ImFont* font = ImGui::GetFont();
    const float fs = 15.f;
    ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.f, msg);
    fg->AddText(font, fs,
                ImVec2(cardMin.x + (cardW - ts.x) * 0.5f, cardMin.y + 100.f),
                IM_COL32(209, 213, 219, 255), msg);

    // Full-screen input blocker + card button (ImGui — Windows and Linux)
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(dsp);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::Begin("##yt_resolve_overlay", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse);
    // Eat clicks on the dimmed backdrop
    ImGui::InvisibleButton("##yt_block", dsp);

    const float btnW = 200.f, btnH = 36.f;
    ImGui::SetCursorPos(ImVec2(cardMin.x + (cardW - btnW) * 0.5f, cardMin.y + cardH - btnH - 24.f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.25f, 0.32f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.29f, 0.33f, 0.41f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f, 0.20f, 0.26f, 1.f));
    if (ImGui::Button(i18n::tr("yt.goto_youtube"), ImVec2(btnW, btnH)))
        openOnYoutube();
    ImGui::PopStyleColor(3);
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

} // namespace ytplayer
