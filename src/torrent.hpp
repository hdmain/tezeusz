#pragma once
#include <atomic>
#include <functional>
#include <string>

// In-process BitTorrent via libtorrent (Windows + Linux). No aria2 / qBit.
namespace torrent {

struct Progress {
    double fraction = 0;       // 0..1
    double downloadedMb = 0;
    double totalMb = 0;
    int peers = 0;
    int seeds = 0;
    double downloadRateKBs = 0;
    std::string state;
};

enum class StopAction {
    Continue = 0,
    CancelDelete = 1, // user cancelled — remove incomplete files
    PauseKeep = 2     // app shutting down — keep files for resume
};

// Blocking until finished, failed, cancelled, or timeout.
// Returns true when the torrent finished downloading (files in outDir).
// Existing files in outDir are checked and resumed automatically.
bool downloadMagnet(const std::string& magnet,
                    const std::string& outDir,
                    const std::function<StopAction()>& pollStop,
                    const std::function<void(const Progress&)>& onProgress,
                    std::string* err,
                    int timeoutSec = 7200);

} // namespace torrent
