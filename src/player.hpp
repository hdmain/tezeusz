#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct GLFWwindow;

// In-app video player (Jellyfin-style overlay). Uses libVLC when available.
namespace player {

struct Track {
    int id = -1;
    std::string name;
};

struct State {
    bool open = false;
    bool loading = false;
    bool playing = false;
    bool paused = false;
    bool fullscreen = false;
    bool ready = false;
    bool failed = false;
    std::string title;
    std::string path;
    std::string error;
    double position = 0;
    int64_t timeMs = 0;
    int64_t durationMs = 0;
    int volume = 80;
    bool muted = false;
    int videoW = 0, videoH = 0;
    unsigned tex = 0;
    std::vector<Track> subtitles;
    int subtitleId = -1;
    std::vector<Track> audioTracks;
    int audioId = -1;
};

void bindWindow(GLFWwindow* w);
bool open(const std::string& path, const std::string& title);
void close();
bool isOpen();

void togglePause();
void seekRelative(double seconds);
void setPosition(double frac01);
void setVolume(int vol0to100);
void toggleMute();
void toggleFullscreen();
void setSubtitle(int id);
void setAudioTrack(int id);
void cycleSubtitle();
void toggleStats();
bool statsVisible();

void tick();
const State& state();
bool render();

} // namespace player
