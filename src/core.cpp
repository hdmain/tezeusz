#include "core.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#else
#include <unistd.h>
#endif

namespace core {
namespace {

std::mutex g_jobMu;
std::condition_variable g_jobCv;
std::deque<std::function<void()>> g_jobs;
std::vector<std::thread> g_workers;
std::atomic<bool> g_stop{false};
std::atomic<int> g_running{0};
std::atomic<int> g_peers{0};
std::atomic<int> g_pending{0};

std::mutex g_statMu;
Stats g_stats{};

#ifdef _WIN32
ULARGE_INTEGER g_lastCpu{};
ULARGE_INTEGER g_lastSys{};
bool g_cpuInit = false;
#else
uint64_t g_lastCpu = 0;
uint64_t g_lastSys = 0;
bool g_cpuInit = false;
#endif

void workerLoop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lk(g_jobMu);
            g_jobCv.wait(lk, [&] { return g_stop.load() || !g_jobs.empty(); });
            if (g_stop.load() && g_jobs.empty()) return;
            if (g_jobs.empty()) continue;
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
            g_pending.store((int)g_jobs.size());
        }
        g_running.fetch_add(1);
        try {
            if (job) job();
        } catch (...) {}
        g_running.fetch_sub(1);
    }
}

#ifdef _WIN32
void refreshProcessStats() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    double ramMb = 0;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc)))
        ramMb = (double)pmc.WorkingSetSize / (1024.0 * 1024.0);

    FILETIME ct{}, et{}, kt{}, ut{};
    double cpu = 0;
    if (GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut)) {
        ULARGE_INTEGER cpuNow{}, sysNow{};
        cpuNow.LowPart = ut.dwLowDateTime; cpuNow.HighPart = ut.dwHighDateTime;
        ULARGE_INTEGER k; k.LowPart = kt.dwLowDateTime; k.HighPart = kt.dwHighDateTime;
        cpuNow.QuadPart += k.QuadPart;

        FILETIME idle{}, kernel{}, user{};
        if (GetSystemTimes(&idle, &kernel, &user)) {
            ULARGE_INTEGER ku, uu;
            ku.LowPart = kernel.dwLowDateTime; ku.HighPart = kernel.dwHighDateTime;
            uu.LowPart = user.dwLowDateTime; uu.HighPart = user.dwHighDateTime;
            sysNow.QuadPart = ku.QuadPart + uu.QuadPart;

            if (g_cpuInit && sysNow.QuadPart > g_lastSys.QuadPart) {
                double dCpu = (double)(cpuNow.QuadPart - g_lastCpu.QuadPart);
                double dSys = (double)(sysNow.QuadPart - g_lastSys.QuadPart);
                SYSTEM_INFO si{}; GetSystemInfo(&si);
                int n = (int)si.dwNumberOfProcessors;
                if (n < 1) n = 1;
                cpu = (dCpu / dSys) * 100.0 * n;
                if (cpu < 0) cpu = 0;
                if (cpu > 100.0 * n) cpu = 100.0 * n;
            }
            g_lastCpu = cpuNow;
            g_lastSys = sysNow;
            g_cpuInit = true;
        }
    }

    std::lock_guard<std::mutex> lk(g_statMu);
    g_stats.cpuPct = cpu;
    g_stats.ramMb = ramMb;
    g_stats.peers = g_peers.load();
    g_stats.jobsPending = g_pending.load();
    g_stats.jobsRunning = g_running.load();
}
#else
void refreshProcessStats() {
    double ramMb = 0;
    {
        std::ifstream f("/proc/self/statm");
        long pages = 0, rss = 0;
        if (f >> pages >> rss) {
            long page = sysconf(_SC_PAGESIZE);
            ramMb = (double)rss * (double)page / (1024.0 * 1024.0);
        }
    }

    double cpu = 0;
    uint64_t cpuNow = 0, sysNow = 0;
    {
        std::ifstream f("/proc/self/stat");
        std::string ignore;
        // pid comm state ... utime(14) stime(15)
        long utime = 0, stime = 0;
        if (f >> ignore >> ignore >> ignore) {
            for (int i = 0; i < 10; i++) f >> ignore;
            f >> utime >> stime;
            cpuNow = (uint64_t)utime + (uint64_t)stime;
        }
    }
    {
        std::ifstream f("/proc/stat");
        std::string cpuLabel;
        uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0;
        if (f >> cpuLabel >> user >> nice >> system >> idle >> iowait >> irq >> softirq)
            sysNow = user + nice + system + idle + iowait + irq + softirq;
    }
    if (g_cpuInit && sysNow > g_lastSys) {
        double dCpu = (double)(cpuNow - g_lastCpu);
        double dSys = (double)(sysNow - g_lastSys);
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        if (n < 1) n = 1;
        cpu = (dCpu / dSys) * 100.0 * (double)n;
        if (cpu < 0) cpu = 0;
    }
    g_lastCpu = cpuNow;
    g_lastSys = sysNow;
    g_cpuInit = true;

    std::lock_guard<std::mutex> lk(g_statMu);
    g_stats.cpuPct = cpu;
    g_stats.ramMb = ramMb;
    g_stats.peers = g_peers.load();
    g_stats.jobsPending = g_pending.load();
    g_stats.jobsRunning = g_running.load();
}
#endif

} // namespace

void init(int workers) {
    g_stop = false;
    if (workers < 1) workers = 1;
    if (workers > 8) workers = 8;
    for (int i = 0; i < workers; i++)
        g_workers.emplace_back(workerLoop);
    refreshProcessStats();
}

void shutdown() {
    {
        std::lock_guard<std::mutex> lk(g_jobMu);
        g_stop = true;
    }
    g_jobCv.notify_all();
    for (auto& t : g_workers) if (t.joinable()) t.join();
    g_workers.clear();
    std::lock_guard<std::mutex> lk(g_jobMu);
    g_jobs.clear();
}

void tick() {
    static double last = 0;
    double now = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - last < 0.4) return;
    last = now;
    refreshProcessStats();
}

void enqueue(std::function<void()> fn) {
    if (!fn || g_stop.load()) return;
    {
        std::lock_guard<std::mutex> lk(g_jobMu);
        g_jobs.push_back(std::move(fn));
        g_pending.store((int)g_jobs.size());
    }
    g_jobCv.notify_one();
}

Stats stats() {
    std::lock_guard<std::mutex> lk(g_statMu);
    Stats s = g_stats;
    s.peers = g_peers.load();
    s.jobsPending = g_pending.load();
    s.jobsRunning = g_running.load();
    return s;
}

void setPeers(int n) { g_peers.store(n < 0 ? 0 : n); }

} // namespace core
