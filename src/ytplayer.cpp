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
#include <cstring>
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

std::string watchUrl(const std::string& videoId) {
    return "https://www.youtube.com/watch?v=" + videoId;
}

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

std::string extractBetween(const std::string& s, const char* key, char end) {
    auto p = s.find(key);
    if (p == std::string::npos) return {};
    p += std::strlen(key);
    auto e = s.find(end, p);
    if (e == std::string::npos || e <= p) return {};
    return s.substr(p, e - p);
}

// Turn raw Set-Cookie blob(s) into a Cookie request header (name=value; …).
std::string cookieHeaderFromSetCookie(const std::string& raw) {
    if (raw.empty()) return {};
    std::string out;
    size_t i = 0;
    while (i < raw.size()) {
        // Each Set-Cookie may be joined with "; " - keep only name=value pairs
        // that look like cookies (skip Path=, Domain=, Expires=, Secure, HttpOnly…).
        size_t eq = raw.find('=', i);
        if (eq == std::string::npos) break;
        size_t nameStart = i;
        while (nameStart < eq && (raw[nameStart] == ' ' || raw[nameStart] == ';' ||
                                  raw[nameStart] == '\r' || raw[nameStart] == '\n'))
            ++nameStart;
        std::string name = raw.substr(nameStart, eq - nameStart);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
        size_t valStart = eq + 1;
        size_t semi = raw.find(';', valStart);
        size_t valEnd = (semi == std::string::npos) ? raw.size() : semi;
        std::string value = raw.substr(valStart, valEnd - valStart);

        const std::string lower = util::lower(name);
        bool attr = (lower == "path" || lower == "domain" || lower == "expires" ||
                     lower == "max-age" || lower == "samesite" || lower == "secure" ||
                     lower == "httponly" || lower == "priority" || lower.empty());
        if (!attr && !name.empty()) {
            if (!out.empty()) out += "; ";
            out += name + "=" + value;
        }
        i = (semi == std::string::npos) ? raw.size() : semi + 1;
    }
    return out;
}

struct YtSession {
    std::string apiKey;
    std::string visitorData;
    std::string cookie; // Cookie: header value
};

// Fetch watch page cookies + InnerTube key / visitorData (needed after YT hardened ANDROID).
YtSession bootstrapSession(const std::string& videoId) {
    YtSession s;
    // API key comes from the watch page (INNERTUBE_API_KEY). No hardcoded AIza…
    // keys - GitHub secret scanning treats those as credentials even when they are
    // public YouTube client keys shared by every InnerTube client.

    const std::string ua =
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";
    std::string headers =
        std::string("User-Agent: ") + ua + "\r\n"
        "Accept-Language: en-US,en;q=0.9\r\n";

    auto r = http::get(watchUrl(videoId), "text/html,*/*", headers);
    if (!r.setCookie.empty())
        s.cookie = cookieHeaderFromSetCookie(r.setCookie);

    // Consent / SOCS - many regions block player without these.
    if (s.cookie.find("CONSENT=") == std::string::npos)
        s.cookie += (s.cookie.empty() ? "" : "; ") + std::string("CONSENT=YES+");
    if (s.cookie.find("SOCS=") == std::string::npos)
        s.cookie += (s.cookie.empty() ? "" : "; ") + std::string("SOCS=CAI");

    if (!r.body.empty()) {
        if (auto k = extractBetween(r.body, "\"INNERTUBE_API_KEY\":\"", '"'); !k.empty())
            s.apiKey = k;
        if (auto v = extractBetween(r.body, "\"VISITOR_DATA\":\"", '"'); !v.empty())
            s.visitorData = v;
        else if (auto v = extractBetween(r.body, "\"visitorData\":\"", '"'); !v.empty())
            s.visitorData = v;
    }
    return s;
}

bool pickFromStreamingData(const json& sd, std::string* videoUrl, std::string* audioUrl) {
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
            if (!require720 && h > kTargetH) continue;
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

    {
        std::string vid, aud;
        if (pickAdaptiveVideo(true, &vid)) {
            pickAudio(&aud);
            *videoUrl = vid;
            if (audioUrl) *audioUrl = aud;
            return true;
        }
    }
    {
        std::string vid, aud;
        if (pickAdaptiveVideo(false, &vid)) {
            pickAudio(&aud);
            *videoUrl = vid;
            if (audioUrl) *audioUrl = aud;
            return true;
        }
    }
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
    std::string hls = sd.value("hlsManifestUrl", "");
    if (looksLikeUrl(hls)) {
        *videoUrl = hls;
        return true;
    }
        return false;
    }

// Lightweight YouTube InnerTube - prefers clients that still return clear `url` fields.
bool tryInnerTube(const std::string& videoId, std::string* videoUrl, std::string* audioUrl) {
    if (videoUrl) videoUrl->clear();
    if (audioUrl) audioUrl->clear();

    YtSession sess = bootstrapSession(videoId);

    struct Client {
        const char* name;
        const char* version;
        const char* ua;
        const char* clientNameHeader;
        bool embedded = false;
    };
    static const Client clients[] = {
        {"ANDROID", "20.10.38",
         "com.google.android.youtube/20.10.38 (Linux; U; Android 14) gzip", "3", false},
        {"ANDROID", "19.35.36",
         "com.google.android.youtube/19.35.36 (Linux; U; Android 14) gzip", "3", false},
        {"IOS", "19.45.4",
         "com.google.ios.youtube/19.45.4 (iPhone16,2; U; CPU iOS 17_5_1 like Mac OS X)", "5", false},
        {"TVHTML5_SIMPLY_EMBEDDED_PLAYER", "2.0",
         "Mozilla/5.0 (ChromiumStylePlatform) Cobalt/Version", "85", true},
        {"ANDROID_VR", "1.60.19",
         "com.google.android.apps.youtube.vr.oculus/1.60.19 (Linux; U; Android 12)", "28", false},
    };

    for (auto& c : clients) {
        json clientObj = {
            {"clientName", c.name},
            {"clientVersion", c.version},
            {"hl", "en"},
            {"gl", "US"},
            {"userAgent", c.ua},
        };
        if (std::string(c.name).find("ANDROID") != std::string::npos)
            clientObj["androidSdkVersion"] = 34;
        if (!sess.visitorData.empty())
            clientObj["visitorData"] = sess.visitorData;

        json body = {
            {"context", {{"client", clientObj}}},
            {"videoId", videoId},
            {"contentCheckOk", true},
            {"racyCheckOk", true},
        };
        if (c.embedded) {
            body["context"]["thirdParty"] = {{"embedUrl", "https://www.youtube.com"}};
            body["playbackContext"] = {
                {"contentPlaybackContext", {{"html5Preference", "HTML5_PREF_WANTS"}}}};
        }

        std::string headers =
            std::string("User-Agent: ") + c.ua + "\r\n"
            "X-YouTube-Client-Name: " + c.clientNameHeader + "\r\n"
            "X-YouTube-Client-Version: " + c.version + "\r\n"
            "Origin: https://www.youtube.com\r\n";
        if (!sess.cookie.empty())
            headers += "Cookie: " + sess.cookie + "\r\n";
        if (!sess.visitorData.empty())
            headers += "X-Goog-Visitor-Id: " + sess.visitorData + "\r\n";

        std::string url = "https://www.youtube.com/youtubei/v1/player?prettyPrint=false";
        if (!sess.apiKey.empty())
            url += "&key=" + sess.apiKey;
        auto r = http::post(url, body.dump(), "application/json", headers);
        if (!r.ok()) continue;

        try {
            auto j = json::parse(r.body, nullptr, false);
            if (!j.is_object()) continue;
            auto status = j.value("/playabilityStatus/status"_json_pointer, std::string{});
            if (status != "OK" && status != "LIVE_STREAM_OFFLINE") continue;
            if (!j.contains("streamingData") || !j["streamingData"].is_object()) continue;
            if (pickFromStreamingData(j["streamingData"], videoUrl, audioUrl))
                return true;
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
    job->title = title.empty() ? (std::string(i18n::tr("details.trailer")) + " - " + videoId)
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
        platform::openUrl(watchUrl(id));
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

    const std::string yt = watchUrl(videoId);
    if (ok && !url.empty()) {
        // Pass watch URL so error → "Open externally" opens YouTube, not googlevideo CDN.
        player::open(url, title, audio, yt);
    } else {
        platform::openUrl(yt);
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
