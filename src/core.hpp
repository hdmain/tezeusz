#pragma once
#include <functional>
#include <string>
#include <cstdint>

// App core: worker pool + process stats. UI thread must never do network/IO.
namespace core {

struct Stats {
    double cpuPct = 0;     // 0..100+
    double ramMb = 0;      // working set
    int peers = 0;         // active torrent peers (libtorrent)
    int jobsPending = 0;
    int jobsRunning = 0;
};

void init(int workers = 3);
void shutdown();
void tick(); // cheap: refresh RAM/CPU; call from UI loop

// Fire-and-forget background job (HTTP, torrent, disk). Never call from UI with blocking work.
void enqueue(std::function<void()> fn);

Stats stats();
void setPeers(int n); // updated by download stack

} // namespace core
