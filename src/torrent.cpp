#include "torrent.hpp"
#include "i18n.hpp"

#include <libtorrent/session.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/peer_info.hpp>

#include <chrono>
#include <filesystem>
#include <vector>
#include <string>
#include <thread>
#include <algorithm>

namespace fs = std::filesystem;
namespace lt = libtorrent;

namespace torrent {

static const char* stateName(lt::torrent_status::state_t s) {
    using st = lt::torrent_status::state_t;
    switch (s) {
    case st::checking_files: return "sprawdzanie";
    case st::downloading_metadata: return "metadata";
    case st::downloading: return "pobieranie";
    case st::finished: return "gotowe";
    case st::seeding: return "seeding";
    case st::checking_resume_data: return "wznawianie";
    default: return "…";
    }
}

bool downloadMagnet(const std::string& magnet,
                    const std::string& outDir,
                    const std::function<StopAction()>& pollStop,
                    const std::function<void(const Progress&)>& onProgress,
                    std::string* err,
                    int timeoutSec) {
    if (magnet.empty()) {
        if (err) *err = i18n::tr("torrent.empty_magnet");
        return false;
    }
    std::error_code ec;
    fs::create_directories(outDir, ec);

    const int cores = std::max(2, (int)std::thread::hardware_concurrency());

    lt::settings_pack pack;
    pack.set_int(lt::settings_pack::alert_mask,
                 lt::alert_category::error |
                 lt::alert_category::status |
                 lt::alert_category::peer |
                 lt::alert_category::tracker |
                 lt::alert_category::storage);
    pack.set_bool(lt::settings_pack::enable_dht, true);
    pack.set_bool(lt::settings_pack::enable_lsd, true);
    pack.set_bool(lt::settings_pack::enable_upnp, true);
    pack.set_bool(lt::settings_pack::enable_natpmp, true);
    pack.set_bool(lt::settings_pack::announce_to_all_trackers, true);
    pack.set_bool(lt::settings_pack::announce_to_all_tiers, true);
    pack.set_bool(lt::settings_pack::enable_outgoing_utp, true);
    pack.set_bool(lt::settings_pack::enable_incoming_utp, true);
    pack.set_bool(lt::settings_pack::enable_outgoing_tcp, true);
    pack.set_bool(lt::settings_pack::enable_incoming_tcp, true);

    pack.set_int(lt::settings_pack::connections_limit, 500);
    pack.set_int(lt::settings_pack::connections_slack, 20);
    pack.set_int(lt::settings_pack::max_peerlist_size, 4000);
    pack.set_int(lt::settings_pack::max_paused_peerlist_size, 1000);
    pack.set_int(lt::settings_pack::unchoke_slots_limit, 100);
    pack.set_int(lt::settings_pack::connection_speed, 80);
    pack.set_int(lt::settings_pack::active_downloads, 8);
    pack.set_int(lt::settings_pack::active_seeds, 5);
    pack.set_int(lt::settings_pack::active_checking, 2);
    pack.set_int(lt::settings_pack::active_limit, 20);
    pack.set_int(lt::settings_pack::active_dht_limit, 880);
    pack.set_int(lt::settings_pack::active_tracker_limit, 1600);
    pack.set_int(lt::settings_pack::request_timeout, 20);
    pack.set_int(lt::settings_pack::peer_timeout, 60);
    pack.set_int(lt::settings_pack::inactivity_timeout, 60);
    pack.set_int(lt::settings_pack::stop_tracker_timeout, 5);
    pack.set_int(lt::settings_pack::aio_threads, std::min(8, cores));
    pack.set_int(lt::settings_pack::checking_mem_usage, 2048);
    pack.set_int(lt::settings_pack::max_queued_disk_bytes, 32 * 1024 * 1024);
    pack.set_int(lt::settings_pack::send_buffer_watermark, 3 * 1024 * 1024);
    pack.set_int(lt::settings_pack::send_buffer_low_watermark, 512 * 1024);
    pack.set_int(lt::settings_pack::send_buffer_watermark_factor, 150);
    pack.set_int(lt::settings_pack::download_rate_limit, 0);
    pack.set_int(lt::settings_pack::upload_rate_limit, 0);
    pack.set_bool(lt::settings_pack::rate_limit_ip_overhead, false);
    pack.set_bool(lt::settings_pack::anonymous_mode, false);
    pack.set_bool(lt::settings_pack::strict_end_game_mode, false);
    pack.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, true);
    pack.set_int(lt::settings_pack::seed_time_limit, 1);
    pack.set_int(lt::settings_pack::share_ratio_limit, 1);
    pack.set_str(lt::settings_pack::dht_bootstrap_nodes,
                 "router.bittorrent.com:6881,router.utorrent.com:6881,"
                 "dht.transmissionbt.com:6881,dht.libtorrent.org:25401");

    lt::session ses(pack);

    lt::error_code pec;
    lt::add_torrent_params atp = lt::parse_magnet_uri(magnet, pec);
    if (pec) {
        if (err) *err = std::string(i18n::tr("torrent.bad_magnet")) + pec.message();
        return false;
    }
    atp.save_path = outDir;
    // Existing pieces in outDir are checked automatically → resume after restart
    atp.flags &= ~lt::torrent_flags::sequential_download;
    atp.flags &= ~lt::torrent_flags::paused;
    atp.flags &= ~lt::torrent_flags::auto_managed;
    atp.flags &= ~lt::torrent_flags::disable_dht;
    atp.flags &= ~lt::torrent_flags::disable_lsd;
    atp.flags &= ~lt::torrent_flags::disable_pex;
    atp.max_connections = 200;
    atp.max_uploads = -1;

    lt::torrent_handle h;
    try {
        h = ses.add_torrent(std::move(atp));
    } catch (std::exception& e) {
        if (err) *err = std::string("add_torrent: ") + e.what();
        return false;
    }

    try {
        h.set_max_connections(200);
        h.set_max_uploads(-1);
        h.unset_flags(lt::torrent_flags::sequential_download);
    } catch (...) {}

    const auto started = std::chrono::steady_clock::now();
    bool finished = false;
    std::string failMsg;
    int announceTick = 0;

    while (!finished) {
        StopAction stop = pollStop ? pollStop() : StopAction::Continue;
        if (stop == StopAction::CancelDelete) {
            ses.remove_torrent(h, lt::session::delete_files);
            if (err) *err = "Anulowane";
            return false;
        }
        if (stop == StopAction::PauseKeep) {
            try {
                h.pause();
                ses.remove_torrent(h); // keep incomplete files for next launch
            } catch (...) {}
            if (err) *err = "Wstrzymane";
            return false;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - started).count();
        if (elapsed > timeoutSec) {
            try { h.pause(); ses.remove_torrent(h); } catch (...) {}
            if (err) *err = "Timeout pobierania";
            return false;
        }

        std::vector<lt::alert*> alerts;
        ses.pop_alerts(&alerts);
        for (lt::alert* a : alerts) {
            if (auto* ea = lt::alert_cast<lt::torrent_error_alert>(a)) {
                failMsg = ea->message();
            }
            if (auto* fa = lt::alert_cast<lt::torrent_finished_alert>(a)) {
                if (fa->handle == h) finished = true;
            }
        }

        if ((++announceTick % 20) == 0) {
            try {
                h.force_reannounce();
                h.force_dht_announce();
            } catch (...) {}
        }

        lt::torrent_status st = h.status();
        if (st.errc) {
            failMsg = st.errc.message();
            break;
        }
        if (st.is_finished || st.state == lt::torrent_status::finished ||
            st.state == lt::torrent_status::seeding) {
            finished = true;
        }

        if (onProgress) {
            int connected = st.num_peers;
            int seeds = st.num_seeds;
            try {
                std::vector<lt::peer_info> peers;
                h.get_peer_info(peers);
                if (!peers.empty()) {
                    connected = (int)peers.size();
                    seeds = 0;
                    for (auto& pi : peers) {
                        if (pi.flags & lt::peer_info::seed) ++seeds;
                    }
                } else if (st.num_connections > connected) {
                    connected = st.num_connections;
                }
            } catch (...) {
                if (st.num_connections > connected) connected = st.num_connections;
            }

            Progress p;
            p.fraction = st.progress;
            p.downloadedMb = st.total_done / (1024.0 * 1024.0);
            p.totalMb = st.total_wanted / (1024.0 * 1024.0);
            p.peers = connected;
            p.seeds = seeds;
            p.downloadRateKBs = st.download_payload_rate / 1024.0;
            p.state = stateName(st.state);
            if (connected == 0 && st.list_peers > 0)
                p.state = std::string(stateName(st.state)) + " (swarm " + std::to_string(st.list_peers) + ")";
            onProgress(p);
        }

        if (!finished)
            ses.wait_for_alert(std::chrono::milliseconds(250));
    }

    if (!failMsg.empty() && !finished) {
        try { h.pause(); ses.remove_torrent(h); } catch (...) {}
        if (err) *err = failMsg;
        return false;
    }

    try {
        h.pause();
        ses.remove_torrent(h);
    } catch (...) {}

    return true;
}

} // namespace torrent
