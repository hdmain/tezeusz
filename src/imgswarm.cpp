#include "imgswarm.hpp"
#include "stack.hpp"
#include "util.hpp"

#include <libtorrent/session.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/entry.hpp>
#include <libtorrent/hasher.hpp>
#include <libtorrent/kademlia/ed25519.hpp>
#include <libtorrent/kademlia/item.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
namespace lt = libtorrent;

namespace imgswarm {
namespace {

std::mutex g_mu;
std::condition_variable g_cv;
std::unique_ptr<lt::session> g_ses;
std::thread g_alertThread;
std::atomic<bool> g_stop{true};
std::unordered_map<std::string, lt::torrent_handle> g_seeds; // url -> handle
std::deque<std::string> g_seedOrder;
constexpr int kMaxSeeds = 48;

// Per-seed upload tracking (estimate complete poster transfers to peers).
struct SeedTrack {
    std::int64_t fileSize = 0;
    std::int64_t lastPayloadUpload = 0;
};
std::unordered_map<std::string, SeedTrack> g_seedTrack;
std::atomic<uint64_t> g_offeredSession{0};
std::atomic<uint64_t> g_sentSession{0};
std::atomic<uint64_t> g_bytesUploaded{0};
std::unordered_set<std::string> g_offeredUrls;

bool isImageCdnUrl(const std::string& url) {
    return url.find("image.tmdb.org/") != std::string::npos
        || url.find("m.media-amazon.com/") != std::string::npos
        || url.find("imdb.com/") != std::string::npos;
}

std::array<char, 32> urlSeed(const std::string& url) {
    lt::hasher256 h;
    h.update("tezeusz-cpp-imgswarm-v1|");
    h.update(url);
    auto d = h.final();
    std::array<char, 32> out{};
    std::memcpy(out.data(), d.data(), 32);
    return out;
}

std::tuple<lt::dht::public_key, lt::dht::secret_key> keypairForUrl(const std::string& url) {
    return lt::dht::ed25519_create_keypair(urlSeed(url));
}

std::string fileNameForUrl(const std::string& url) {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : url) { h ^= c; h *= 1099511628211ull; }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%016llx.img", (unsigned long long)h);
    return buf;
}

// Local hex helpers - libtorrent::aux::{to,from}_hex are TORRENT_EXTRA_EXPORT
// and are not linked from the shared MinGW package on CI.
std::string toHex(lt::sha1_hash const& ih) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.resize(lt::sha1_hash::size() * 2);
    auto const* p = reinterpret_cast<unsigned char const*>(ih.data());
    for (int i = 0; i < lt::sha1_hash::size(); ++i) {
        out[(size_t)i * 2] = kHex[p[i] >> 4];
        out[(size_t)i * 2 + 1] = kHex[p[i] & 0xf];
    }
    return out;
}

bool fromHex(const std::string& hex, lt::sha1_hash& out) {
    if (hex.size() != (size_t)lt::sha1_hash::size() * 2) return false;
    auto* dest = reinterpret_cast<unsigned char*>(out.data());
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = nibble(hex[i]), lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        dest[i / 2] = (unsigned char)((hi << 4) | lo);
    }
    return true;
}

void trimSeedsLocked() {
    while ((int)g_seeds.size() > kMaxSeeds && !g_seedOrder.empty()) {
        auto url = g_seedOrder.front();
        g_seedOrder.pop_front();
        auto it = g_seeds.find(url);
        if (it == g_seeds.end()) continue;
        try {
            if (g_ses) g_ses->remove_torrent(it->second);
        } catch (...) {}
        g_seeds.erase(it);
        g_seedTrack.erase(url);
    }
}

// Count complete poster uploads: each time payload upload grows by ~fileSize, credit one send.
void refreshUploadCountersLocked() {
    std::int64_t totalBytes = 0;
    uint64_t newSends = 0;
    for (auto& kv : g_seeds) {
        const std::string& url = kv.first;
        lt::torrent_handle& h = kv.second;
        if (!h.is_valid()) continue;
        lt::torrent_status st;
        try {
            st = h.status();
        } catch (...) {
            continue;
        }
        std::int64_t up = st.total_payload_upload;
        if (up < 0) up = 0;
        totalBytes += up;

        auto& tr = g_seedTrack[url];
        if (tr.fileSize <= 0) {
            try {
                auto ti = h.torrent_file();
                if (ti) tr.fileSize = ti->total_size();
            } catch (...) {}
        }
        if (tr.fileSize < 64) {
            tr.lastPayloadUpload = up;
            continue;
        }
        if (up > tr.lastPayloadUpload) {
            std::int64_t delta = up - tr.lastPayloadUpload;
            // How many full poster copies did this delta cover?
            std::int64_t adds = delta / tr.fileSize;
            // Also count a near-complete remainder once (≥90% of one poster).
            std::int64_t rem = delta % tr.fileSize;
            if (rem >= (tr.fileSize * 9) / 10)
                ++adds;
            if (adds > 0)
                newSends += (uint64_t)adds;
            tr.lastPayloadUpload = up;
        }
    }
    g_bytesUploaded.store((uint64_t)totalBytes);
    if (newSends > 0) {
        g_sentSession.fetch_add(newSends);
        auto& cfg = stack::StackConfig::get();
        cfg.imageP2pSentTotal += newSends;
        // Persist occasionally so lifetime stats survive restarts.
        static uint64_t sinceSave = 0;
        sinceSave += newSends;
        if (sinceSave >= 3) {
            sinceSave = 0;
            try { cfg.save(); } catch (...) {}
        }
    }
}

void publishInfoHash(const std::string& url, lt::sha1_hash const& ih) {
    if (!g_ses) return;
    auto keys = keypairForUrl(url);
    auto pk = std::get<0>(keys);
    auto sk = std::get<1>(keys);
    std::string hex = toHex(ih);
    try {
        g_ses->dht_put_item(pk.bytes,
            [hex, pk, sk](lt::entry& item, std::array<char, 64>& sig,
                          std::int64_t& seq, std::string const& salt) {
                item = hex;
                ++seq;
                std::vector<char> v;
                lt::bencode(std::back_inserter(v), item);
                auto s = lt::dht::sign_mutable_item(v, salt, lt::dht::sequence_number(seq), pk, sk);
                sig = s.bytes;
            });
    } catch (...) {}
}

std::shared_ptr<lt::torrent_info> makeTorrentInfo(const std::string& url, const std::string& filePath) {
    std::error_code ec;
    if (!fs::is_regular_file(filePath, ec)) return nullptr;
    auto sz = fs::file_size(filePath, ec);
    if (ec || sz < 64 || sz > 40ull * 1024 * 1024) return nullptr;

    std::string name = fileNameForUrl(url);
    lt::file_storage stor;
    stor.add_file(name, (std::int64_t)sz);

    lt::create_torrent ct(stor, 16 * 1024);
    ct.set_creator("tezeusz-cpp");
    ct.add_url_seed(url); // hybrid: peers + original CDN
    lt::error_code lec;
    lt::set_piece_hashes(ct, fs::path(filePath).parent_path().string(), lec);
    if (lec) return nullptr;

    lt::entry e = ct.generate();
    std::vector<char> buf;
    lt::bencode(std::back_inserter(buf), e);
    try {
        return std::make_shared<lt::torrent_info>(buf, lt::from_span);
    } catch (...) {
        return nullptr;
    }
}

void alertLoop() {
    while (!g_stop.load()) {
        lt::session* ses = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            ses = g_ses.get();
        }
        if (!ses) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        ses->wait_for_alert(std::chrono::milliseconds(200));
        std::vector<lt::alert*> alerts;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (!g_ses) continue;
            g_ses->pop_alerts(&alerts);
        }
        // alerts processed mainly for DHT liveness; waiters poll session state
        (void)alerts;
        g_cv.notify_all();
    }
}

} // namespace

void init() {
    if (const char* dis = std::getenv("TEZEUSZ_DISABLE_IMGSWARM"); dis && dis[0] && dis[0] != '0')
        return;
    if (!stack::StackConfig::get().imageP2p)
        return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_ses) return;
    g_stop = false;

    try {
        lt::settings_pack pack;
        pack.set_int(lt::settings_pack::alert_mask,
                     lt::alert_category::error |
                     lt::alert_category::status |
                     lt::alert_category::dht |
                     lt::alert_category::storage);
        pack.set_bool(lt::settings_pack::enable_dht, true);
        pack.set_bool(lt::settings_pack::enable_lsd, true);
        pack.set_bool(lt::settings_pack::enable_upnp, true);
        pack.set_bool(lt::settings_pack::enable_natpmp, true);
        pack.set_bool(lt::settings_pack::announce_to_all_trackers, true);
        pack.set_bool(lt::settings_pack::announce_to_all_tiers, true);
        pack.set_int(lt::settings_pack::connections_limit, 200);
        pack.set_int(lt::settings_pack::active_seeds, 40);
        pack.set_int(lt::settings_pack::active_downloads, 8);
        pack.set_int(lt::settings_pack::active_limit, 60);
        pack.set_str(lt::settings_pack::dht_bootstrap_nodes,
                     "router.bittorrent.com:6881,router.utorrent.com:6881,"
                     "dht.transmissionbt.com:6881,dht.libtorrent.org:25401");
        // Ephemeral port - fixed 6889 can fail/conflict on multi-user machines.
        pack.set_str(lt::settings_pack::listen_interfaces, "0.0.0.0:0");
        g_ses = std::make_unique<lt::session>(pack);
        g_alertThread = std::thread(alertLoop);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "tezeusz: imgswarm disabled (%s)\n", e.what());
        g_ses.reset();
        g_stop = true;
    } catch (...) {
        std::fprintf(stderr, "tezeusz: imgswarm disabled (unknown error)\n");
        g_ses.reset();
        g_stop = true;
    }
}

void abortFetches() { g_stop = true; g_cv.notify_all(); }

void shutdown() {
    g_stop = true;
    g_cv.notify_all();
    if (g_alertThread.joinable()) g_alertThread.join();
    std::lock_guard<std::mutex> lk(g_mu);
    refreshUploadCountersLocked();
    try { stack::StackConfig::get().save(); } catch (...) {}
    g_seeds.clear();
    g_seedOrder.clear();
    g_seedTrack.clear();
    g_ses.reset();
}

void offer(const std::string& url, const std::string& filePath) {
    if (!stack::StackConfig::get().imageP2p) return;
    if (url.empty() || filePath.empty() || !isImageCdnUrl(url)) return;
    if (g_stop.load()) return;
    init();
    if (g_stop.load()) return;

    std::shared_ptr<lt::torrent_info> ti = makeTorrentInfo(url, filePath);
    if (!ti) return;

    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_ses) return;
    if (g_seeds.count(url)) return;

    lt::add_torrent_params atp;
    atp.ti = ti;
    atp.save_path = fs::path(filePath).parent_path().string();
    atp.flags |= lt::torrent_flags::seed_mode;
    atp.flags &= ~lt::torrent_flags::paused;
    atp.flags &= ~lt::torrent_flags::auto_managed;
    try {
        atp.url_seeds.push_back(url);
    } catch (...) {}

    try {
        auto h = g_ses->add_torrent(std::move(atp));
        g_seeds[url] = h;
        g_seedOrder.push_back(url);
        {
            SeedTrack tr;
            std::error_code ec;
            tr.fileSize = (std::int64_t)fs::file_size(filePath, ec);
            if (ec || tr.fileSize < 0) tr.fileSize = 0;
            g_seedTrack[url] = tr;
        }
        if (g_offeredUrls.insert(url).second)
            g_offeredSession.fetch_add(1);
        trimSeedsLocked();
        publishInfoHash(url, ti->info_hashes().get_best());
    } catch (...) {}
}

bool tryFetch(const std::string& url, const std::string& destPath, int timeoutMs) {
    if (!stack::StackConfig::get().imageP2p) return false;
    if (url.empty() || destPath.empty() || !isImageCdnUrl(url)) return false;
    if (timeoutMs < 200) return false;
    init();
    if (g_stop.load()) return false;

    auto keys = keypairForUrl(url);
    auto const& pub = std::get<0>(keys);
    auto start = std::chrono::steady_clock::now();
    auto deadline = start + std::chrono::milliseconds(timeoutMs);

    lt::sha1_hash infoHash{};
    bool gotHash = false;

    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_ses) return false;
        try {
            g_ses->dht_get_item(pub.bytes);
        } catch (...) {
            return false;
        }
    }

    // Poll DHT mutable item alerts
    while (std::chrono::steady_clock::now() < deadline && !gotHash && !g_stop.load()) {
        lt::session* ses = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            ses = g_ses.get();
        }
        if (!ses) return false;
        ses->wait_for_alert(std::chrono::milliseconds(80));
        std::vector<lt::alert*> alerts;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (!g_ses) return false;
            g_ses->pop_alerts(&alerts);
            for (lt::alert* a : alerts) {
                if (auto* m = lt::alert_cast<lt::dht_mutable_item_alert>(a)) {
                    if (m->key != pub.bytes) continue;
                    if (m->item.type() != lt::entry::string_t) continue;
                    std::string hex = m->item.string();
                    if (fromHex(hex, infoHash)) {
                        gotHash = true;
                        break;
                    }
                }
            }
        }
        if (!gotHash)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!gotHash || g_stop.load()) return false;

    std::string saveDir = fs::path(destPath).parent_path().string();
    std::string name = fileNameForUrl(url);
    std::string magnet = "magnet:?xt=urn:btih:" + toHex(infoHash) + "&dn=" + name;

    lt::torrent_handle h;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_ses) return false;
        lt::error_code ec;
        lt::add_torrent_params atp = lt::parse_magnet_uri(magnet, ec);
        if (ec) return false;
        atp.save_path = saveDir;
        atp.flags &= ~lt::torrent_flags::paused;
        atp.flags &= ~lt::torrent_flags::auto_managed;
        try {
            atp.url_seeds.push_back(url);
        } catch (...) {}
        try {
            h = g_ses->add_torrent(std::move(atp));
        } catch (...) {
            return false;
        }
    }

    bool ok = false;
    while (std::chrono::steady_clock::now() < deadline && !g_stop.load()) {
        lt::torrent_status st = h.status();
        if (st.is_finished || st.state == lt::torrent_status::finished ||
            st.state == lt::torrent_status::seeding) {
            ok = true;
            break;
        }
        if (st.errc) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (g_stop.load()) {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_ses) {
            try { g_ses->remove_torrent(h, lt::session::delete_files); } catch (...) {}
        }
        return false;
    }

    std::string downloaded = (fs::path(saveDir) / name).string();
    std::error_code ec;
    if (ok && fs::exists(downloaded, ec)) {
        if (downloaded != destPath) {
            fs::rename(downloaded, destPath, ec);
            if (ec) {
                fs::copy_file(downloaded, destPath, fs::copy_options::overwrite_existing, ec);
                fs::remove(downloaded, ec);
            }
        }
        // Keep seeding under this URL
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_seeds[url] = h;
            g_seedOrder.push_back(url);
            {
                SeedTrack tr;
                std::error_code ec2;
                tr.fileSize = (std::int64_t)fs::file_size(destPath, ec2);
                if (ec2 || tr.fileSize < 0) tr.fileSize = 0;
                g_seedTrack[url] = tr;
            }
            if (g_offeredUrls.insert(url).second)
                g_offeredSession.fetch_add(1);
            trimSeedsLocked();
        }
        publishInfoHash(url, infoHash);
        return fs::exists(destPath, ec) && fs::file_size(destPath, ec) >= 64;
    }

    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_ses) {
            try { g_ses->remove_torrent(h, lt::session::delete_files); } catch (...) {}
        }
    }
    return false;
}

Stats stats() {
    Stats s;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_ses)
            refreshUploadCountersLocked();
        s.activeSeeds = (int)g_seeds.size();
    }
    s.offeredSession = g_offeredSession.load();
    s.sentSession = g_sentSession.load();
    s.bytesUploaded = g_bytesUploaded.load();
    s.sentTotal = stack::StackConfig::get().imageP2pSentTotal;
    return s;
}

} // namespace imgswarm
