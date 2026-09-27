#pragma once
#include <cstdint>
#include <string>

// Optional BitTorrent swarm for poster/backdrop images (TMDB CDN URLs).
// Fetch order (HTTP vs P2P) is controlled by StackConfig::imageFetchPreferP2p.
namespace imgswarm {

struct Stats {
    int activeSeeds = 0;           // posters currently offered to the swarm
    uint64_t offeredSession = 0;   // unique posters offered this run
    uint64_t sentSession = 0;      // complete poster transfers out this run
    uint64_t sentTotal = 0;        // lifetime complete transfers (persisted)
    uint64_t bytesUploaded = 0;    // payload bytes uploaded this run
};

void init();
void shutdown();
// Signal in-flight tryFetch loops to exit quickly (before joining workers).
void abortFetches();

// Announce + seed a file already on disk (non-blocking).
void offer(const std::string& url, const std::string& filePath);

// Try to download from peers into destPath. Returns true if destPath is a usable image file.
bool tryFetch(const std::string& url, const std::string& destPath, int timeoutMs = 3000);

// Refresh upload counters from libtorrent (safe to call from UI thread).
Stats stats();

} // namespace imgswarm
