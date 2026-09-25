#pragma once
#include <string>

// Background differential updater (SHA-256 per file).
// Downloads only missing/changed blobs from the continuous GitHub release,
// then restarts into the new build.
namespace updater {

enum class State {
    Idle = 0,
    Checking,
    UpToDate,
    Downloading,
    Ready,       // staged; will restart soon
    Applying,
    Disabled,    // install dir not writable / env disabled
    Error
};

void init();
void shutdown();
void tick(); // UI thread — progress + auto-apply when safe

State state();
std::string statusText();   // short UI string
std::string remoteVersion();
int downloadPercent();      // 0..100 while Downloading
bool wantsQuitForApply();   // main should begin graceful quit

// Manual trigger (Settings).
void checkNow();

} // namespace updater
