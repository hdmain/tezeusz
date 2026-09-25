#pragma once
#include "gl_compat.hpp"
#include <GLFW/glfw3.h>
#include <string>
#include <unordered_map>
#include <deque>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <vector>
#include <cstdint>
#include <chrono>

struct ImageEntry {
    GLuint tex = 0;
    int w = 0, h = 0;
    bool failed = false;
    int attempts = 0;
    // true while queued/downloading/decoding (no tex yet, not permanently given up)
    bool loading = false;
};

// Async downloader + decoder, GL upload happens on main thread.
class ImageCache {
public:
    static ImageCache& instance();

    void init();
    void shutdown();

    // Request image by URL. Non-blocking. Returns pointer to entry (may be empty while loading).
    // Failed entries auto-retry with backoff so posters fill in progressively.
    const ImageEntry* request(const std::string& url);
    // Upload any finished decodes to GL textures. Call once per frame on main thread.
    void pump();
    // True if downloads/uploads/retries are in flight (keep animating / don't idle-sleep).
    bool busy() const;
    // Load a local file synchronously into a texture (for bundled assets).
    GLuint loadLocal(const std::string& path, int* outW = nullptr, int* outH = nullptr);
    // Drop failed/stuck entries so visible images get re-downloaded (F5 / retry).
    void purgeFailed();
    // Drop everything (textures included, main thread only) and re-download what's on screen.
    void purgeAll();

    const ImageEntry* find(const std::string& url) {
        std::lock_guard<std::mutex> l(mtx_);
        auto it = entries_.find(url);
        return it == entries_.end() ? nullptr : &it->second;
    }

    GLuint missingPosterTex = 0;

private:
    ImageCache() = default;
    struct WorkItem { std::string url; int attempt = 0; };
    struct DecodedItem { std::string url; std::vector<uint8_t> rgba; int w = 0, h = 0; bool failed = false; int attempt = 0; };

    void workerLoop();
    void enqueueLocked(const std::string& url, int attempt);

    std::unordered_map<std::string, ImageEntry> entries_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> retryAt_;
    std::deque<WorkItem> queue_;
    std::deque<DecodedItem> decoded_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::vector<std::thread> workers_;
    bool stop_ = false;
    int activeDownloads_ = 0;
    // Global pacing across workers (steady_clock ticks)
    std::chrono::steady_clock::time_point nextStart_{};
    static constexpr int MAX_ACTIVE = 4;
    static constexpr int MAX_ATTEMPTS = 8;
    // Keep GPU uploads light so scrolling stays smooth while posters stream in.
    static constexpr int UPLOADS_PER_FRAME = 2;
};
