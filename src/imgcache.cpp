#include "imgcache.hpp"
#include "http.hpp"
#include "util.hpp"
#include "imgswarm.hpp"
#include "gl_compat.hpp"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <chrono>
#include <thread>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstdio>

namespace fs = std::filesystem;

ImageCache& ImageCache::instance() {
    static ImageCache c;
    return c;
}

static std::string cacheRoot() {
    static std::string root;
    if (root.empty()) {
        root = util::appDataPath("imgcache");
        std::error_code ec;
        fs::create_directories(root, ec);
    }
    return root;
}

// Stable, path-safe name from URL (FNV-1a 64-bit hex).
static std::string cacheFileName(const std::string& url) {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : url) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx.img", (unsigned long long)h);
    return buf;
}

static std::string cachePath(const std::string& url) {
    return cacheRoot() + "\\" + cacheFileName(url);
}

static bool readBinaryFile(const std::string& path, std::vector<uint8_t>* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    auto n = (std::streamoff)f.tellg();
    if (n <= 0 || n > 40 * 1024 * 1024) return false;
    f.seekg(0, std::ios::beg);
    out->resize((size_t)n);
    f.read(reinterpret_cast<char*>(out->data()), n);
    return (bool)f || f.eof();
}

static bool writeBinaryFile(const std::string& path, const std::vector<uint8_t>& data) {
    if (data.empty()) return false;
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        fs::rename(tmp, path, ec);
    }
    return !ec;
}

void ImageCache::init() {
    cacheRoot();
    imgswarm::init();
    nextStart_ = std::chrono::steady_clock::now();
    for (int i = 0; i < 2; i++) workers_.emplace_back([this] { workerLoop(); });
    missingPosterTex = loadLocal(util::assetDir() + "/poster_missing.png");
}

void ImageCache::shutdown() {
    {
        std::lock_guard<std::mutex> l(mtx_);
        stop_ = true;
    }
    imgswarm::abortFetches(); // unblock workers stuck in P2P tryFetch
    cv_.notify_all();
    for (auto& t : workers_) if (t.joinable()) t.join();
    workers_.clear();
    imgswarm::shutdown();
}

static GLuint uploadTexture(std::vector<uint8_t>& rgba, int w, int h) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

void ImageCache::enqueueLocked(const std::string& url, int attempt) {
    auto& e = entries_[url];
    e.failed = false;
    e.loading = true;
    e.attempts = attempt;
    queue_.push_back({url, attempt});
    cv_.notify_one();
}

const ImageEntry* ImageCache::request(const std::string& url) {
    if (url.empty()) return nullptr;
    std::lock_guard<std::mutex> l(mtx_);
    auto it = entries_.find(url);
    if (it == entries_.end()) {
        entries_[url] = ImageEntry{};
        enqueueLocked(url, 0);
        it = entries_.find(url);
        return &it->second;
    }
    auto& e = it->second;
    if (e.tex) return &e;

    if (e.failed && e.attempts < MAX_ATTEMPTS) {
        auto now = std::chrono::steady_clock::now();
        auto rit = retryAt_.find(url);
        if (rit == retryAt_.end() || now >= rit->second) {
            retryAt_.erase(url);
            enqueueLocked(url, e.attempts);
        } else {
            e.loading = true;
        }
    }
    return &e;
}

bool ImageCache::busy() const {
    std::lock_guard<std::mutex> l(mtx_);
    return activeDownloads_ > 0 || !decoded_.empty() || !queue_.empty();
}

void ImageCache::pump() {
    for (int n = 0; n < UPLOADS_PER_FRAME; n++) {
        DecodedItem item;
        {
            std::lock_guard<std::mutex> l(mtx_);
            if (decoded_.empty()) break;
            item = std::move(decoded_.front());
            decoded_.pop_front();
            if (item.failed) {
                auto& e = entries_[item.url];
                e.attempts = item.attempt + 1;
                e.failed = true;
                e.loading = e.attempts < MAX_ATTEMPTS;
                int shift = std::min(item.attempt, 4);
                auto delay = std::chrono::milliseconds(400 * (1 << shift));
                if (delay > std::chrono::milliseconds(8000)) delay = std::chrono::milliseconds(8000);
                retryAt_[item.url] = std::chrono::steady_clock::now() + delay;
                continue;
            }
        }

        GLuint tex = uploadTexture(item.rgba, item.w, item.h);
        item.rgba.clear();
        item.rgba.shrink_to_fit();

        {
            std::lock_guard<std::mutex> l(mtx_);
            auto& e = entries_[item.url];
            if (e.tex) { GLuint old = e.tex; glDeleteTextures(1, &old); }
            e.tex = tex;
            e.w = item.w; e.h = item.h;
            e.attempts = item.attempt + 1;
            e.failed = false;
            e.loading = false;
            retryAt_.erase(item.url);
        }
    }
}

GLuint ImageCache::loadLocal(const std::string& path, int* outW, int* outH) {
    std::string data = util::readFile(path);
    if (data.empty()) return 0;
    int w, h, ch;
    stbi_set_flip_vertically_on_load(0);
    unsigned char* px = stbi_load_from_memory((const stbi_uc*)data.data(), (int)data.size(), &w, &h, &ch, 4);
    if (!px) return 0;
    std::vector<uint8_t> rgba(px, px + (size_t)w * h * 4);
    stbi_image_free(px);
    if (outW) *outW = w;
    if (outH) *outH = h;
    return uploadTexture(rgba, w, h);
}

static bool decodeImageBytes(const std::vector<uint8_t>& bytes, std::vector<uint8_t>* rgba, int* w, int* h) {
    if (bytes.empty() || !rgba || !w || !h) return false;
    int cw, ch, cn;
    stbi_set_flip_vertically_on_load(0);
    unsigned char* px = stbi_load_from_memory((const stbi_uc*)bytes.data(), (int)bytes.size(), &cw, &ch, &cn, 4);
    if (!px) return false;
    rgba->assign(px, px + (size_t)cw * ch * 4);
    *w = cw;
    *h = ch;
    stbi_image_free(px);
    return true;
}

void ImageCache::workerLoop() {
    for (;;) {
        WorkItem item;
        {
            std::unique_lock<std::mutex> l(mtx_);
            cv_.wait(l, [&] { return stop_ || (!queue_.empty() && activeDownloads_ < MAX_ACTIVE); });
            if (stop_) return;
            auto now = std::chrono::steady_clock::now();
            if (now < nextStart_) {
                bool diskHit = !queue_.empty() && fs::exists(cachePath(queue_.front().url));
                if (!diskHit) {
                    auto wait = nextStart_ - now;
                    l.unlock();
                    std::this_thread::sleep_for(wait);
                    l.lock();
                    if (stop_) return;
                    if (queue_.empty() || activeDownloads_ >= MAX_ACTIVE) continue;
                }
            }
            item = queue_.front();
            queue_.pop_front();
            activeDownloads_++;
        }

        DecodedItem out;
        out.url = item.url;
        out.attempt = item.attempt;

        const std::string path = cachePath(item.url);
        std::vector<uint8_t> bytes;
        bool fromDisk = readBinaryFile(path, &bytes) && bytes.size() >= 64;

        if (fromDisk) {
            if (!decodeImageBytes(bytes, &out.rgba, &out.w, &out.h)) {
                std::error_code ec;
                fs::remove(path, ec);
                fromDisk = false;
                bytes.clear();
            } else {
                // Already cached — share with other Seerr peers (HTTP remains primary for new fetches)
                imgswarm::offer(item.url, path);
            }
        }

        if (!fromDisk) {
            std::string err;
            // Primary: normal CDN / HTTP
            bytes = http::getBinary(item.url, &err);
            bool badData = (!bytes.empty() && bytes.size() < 4000);
            if (bytes.empty() || badData) {
                // Secondary: BitTorrent swarm between Seerr users
                if (imgswarm::tryFetch(item.url, path, 3000) && readBinaryFile(path, &bytes) && bytes.size() >= 64) {
                    if (!decodeImageBytes(bytes, &out.rgba, &out.w, &out.h))
                        out.failed = true;
                    else
                        imgswarm::offer(item.url, path);
                } else {
                    out.failed = true;
                }
            } else if (!decodeImageBytes(bytes, &out.rgba, &out.w, &out.h)) {
                out.failed = true;
            } else {
                writeBinaryFile(path, bytes);
                imgswarm::offer(item.url, path);
            }
            {
                std::lock_guard<std::mutex> l(mtx_);
                nextStart_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(60);
            }
        }

        bytes.clear();
        bytes.shrink_to_fit();
        {
            std::lock_guard<std::mutex> l(mtx_);
            activeDownloads_--;
            decoded_.push_back(std::move(out));
        }
        cv_.notify_all();
    }
}

void ImageCache::purgeFailed() {
    std::lock_guard<std::mutex> l(mtx_);
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.failed || (!it->second.tex && it->second.loading)) {
            retryAt_.erase(it->first);
            it = entries_.erase(it);
        } else ++it;
    }
}

void ImageCache::purgeAll() {
    // RAM only — disk cache stays so posters reload quickly after F5.
    std::lock_guard<std::mutex> l(mtx_);
    for (auto& kv : entries_)
        if (kv.second.tex) { GLuint t = kv.second.tex; glDeleteTextures(1, &t); }
    entries_.clear();
    queue_.clear();
    decoded_.clear();
    retryAt_.clear();
}
