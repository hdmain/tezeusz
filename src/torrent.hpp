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

// Blocking until finished, failed, cancelled, or timeout.
// Returns true when the torrent finished downloading (files in outDir).
bool downloadMagnet(const std::string& magnet,
                    const std::string& outDir,
                    const std::function<bool()>& shouldCancel,
                    const std::function<void(const Progress&)>& onProgress,
                    std::string* err,
                    int timeoutSec = 7200);

} // namespace torrent
