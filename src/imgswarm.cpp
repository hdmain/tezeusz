#include "imgswarm.hpp"

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
#include <libtorrent/hex.hpp>
#include <libtorrent/kademlia/ed25519.hpp>
#include <libtorrent/kademlia/item.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
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

bool isImageCdnUrl(const std::string& url) {
    return url.find("image.tmdb.org/") != std::string::npos
        || url.find("m.media-amazon.com/") != std::string::npos
        || url.find("imdb.com/") != std::string::npos;
}

std::array<char, 32> urlSeed(const std::string& url) {
    lt::hasher256 h;
    h.update("seerr-cpp-imgswarm-v1|");
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

std::string toHex(lt::sha1_hash const& ih) {
    return lt::aux::to_hex({ih.data(), std::size_t(lt::sha1_hash::size())});
}

bool fromHex(const std::string& hex, lt::sha1_hash& out) {
    if (hex.size() != 40) return false;
    return lt::aux::from_hex(hex, out.data());
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
    ct.set_creator("seerr-cpp");
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
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_ses) return;
    g_stop = false;

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
    pack.set_str(lt::settings_pack::listen_interfaces, "0.0.0.0:6889");
    g_ses = std::make_unique<lt::session>(pack);
    g_alertThread = std::thread(alertLoop);
}

void abortFetches() { g_stop = true; g_cv.notify_all(); }

void shutdown() {
    g_stop = true;
    g_cv.notify_all();
    if (g_alertThread.joinable()) g_alertThread.join();
    std::lock_guard<std::mutex> lk(g_mu);
    g_seeds.clear();
    g_seedOrder.clear();
    g_ses.reset();
}

void offer(const std::string& url, const std::string& filePath) {
    if (url.empty() || filePath.empty() || !isImageCdnUrl(url)) return;
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
        trimSeedsLocked();
        publishInfoHash(url, ti->info_hashes().get_best());
    } catch (...) {}
}

bool tryFetch(const std::string& url, const std::string& destPath, int timeoutMs) {
    if (url.empty() || destPath.empty() || !isImageCdnUrl(url)) return false;
    if (g_stop.load() || timeoutMs < 200) return false;

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

} // namespace imgswarm
