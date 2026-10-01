#pragma once
#include <string>
#include <vector>

// Offline AI lector (Piper TTS): download engine + voice models, synthesize a
// timed voice-over from sidecar subtitles, play as VLC audio slave.
namespace lector {

struct VoiceInfo {
    std::string id;       // e.g. "pl_PL-darkman-medium"
    std::string lang;     // ISO 639-1, e.g. "pl"
    std::string locale;   // e.g. "pl_PL"
    std::string name;     // display name
    std::string quality;  // "medium" / "low" / "high"
    std::string hfPath;   // path under piper-voices repo
};

enum class State {
    Disabled = 0,
    NeedEngine,
    NeedVoice,
    Downloading,
    Ready,
    Error
};

void init();
void shutdown();
void tick();

const std::vector<VoiceInfo>& catalog();
std::vector<VoiceInfo> voicesForLang(const std::string& lang);

State state();
float progress(); // 0..1 while downloading / generating
std::string statusMessage();
std::string statusDetail(); // e.g. "12.4 / 25.1 MB  ·  3.2 MB/s  ·  ETA 0:04"
std::string errorMessage();

bool engineReady();
bool voiceReady(const std::string& voiceId);
std::string voicePath(const std::string& voiceId); // .onnx path when ready

// Ensure Piper binary (+ selected voice) is present; kicks off background download.
void ensureSetup(const std::string& voiceId = {});

// Download / verify a specific voice model (onnx + json).
void downloadVoice(const std::string& voiceId);

// Synthesize a short sample and return a playable WAV path (may start async;
// empty until ready — call again / poll samplePath).
void playSample(const std::string& voiceId);
std::string samplePath(const std::string& voiceId);

// Find sidecar SRT next to video (preferredLang first, then any .srt).
std::string findSubtitle(const std::string& videoPath, const std::string& preferredLang);

// Background: SRT → timed WAV next to the video. Poll jobBusy / jobProgress.
void startGenerate(const std::string& videoPath, const std::string& srtPath,
                   const std::string& voiceId);
bool jobBusy();
float jobProgress();
std::string jobMessage();
std::string jobOutputPath(); // last successful (or in-progress target) WAV
std::string jobError();

// Path that would be / is used for this video+voice.
std::string outputPathFor(const std::string& videoPath, const std::string& voiceId);

// file:/// URI suitable for libVLC :input-slave=
std::string fileUri(const std::string& path);

} // namespace lector
