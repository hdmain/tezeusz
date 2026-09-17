#include "torrent.hpp"

#include <libtorrent/session.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/alert_types.hpp>

#include <chrono>
#include <filesystem>
#include <vector>

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
    case st::checking_resume_data: return "resume";
    default: return "…";
    }
}

bool downloadMagnet(const std::string& magnet,
                    const std::string& outDir,
                    const std::function<bool()>& shouldCancel,
                    const std::function<void(const Progress&)>& onProgress,
                    std::string* err,
                    int timeoutSec) {
    if (magnet.empty()) {
        if (err) *err = "Pusty magnet";
        return false;
    }
    std::error_code ec;
    fs::create_directories(outDir, ec);

    lt::settings_pack pack;
    pack.set_int(lt::settings_pack::alert_mask,
                 lt::alert_category::error |
                 lt::alert_category::status |
                 lt::alert_category::storage);
    pack.set_bool(lt::settings_pack::enable_dht, true);
    pack.set_bool(lt::settings_pack::enable_lsd, true);
    pack.set_bool(lt::settings_pack::enable_upnp, true);
    pack.set_bool(lt::settings_pack::enable_natpmp, true);
    pack.set_bool(lt::settings_pack::announce_to_all_trackers, true);
    pack.set_bool(lt::settings_pack::announce_to_all_tiers, true);
    pack.set_int(lt::settings_pack::stop_tracker_timeout, 2);
    // Don't keep seeding forever (Radarr/qBit "seed time 0" style)
    pack.set_int(lt::settings_pack::seed_time_limit, 1);
    pack.set_int(lt::settings_pack::share_ratio_limit, 1);

    lt::session ses(pack);

    lt::error_code pec;
    lt::add_torrent_params atp = lt::parse_magnet_uri(magnet, pec);
    if (pec) {
        if (err) *err = "Zły magnet: " + pec.message();
        return false;
    }
    atp.save_path = outDir;
    atp.flags |= lt::torrent_flags::sequential_download;
    atp.flags &= ~lt::torrent_flags::paused;
    atp.flags &= ~lt::torrent_flags::auto_managed;

    lt::torrent_handle h;
    try {
        h = ses.add_torrent(std::move(atp));
    } catch (std::exception& e) {
        if (err) *err = std::string("add_torrent: ") + e.what();
        return false;
    }

    const auto started = std::chrono::steady_clock::now();
    bool finished = false;
    std::string failMsg;

    while (!finished) {
        if (shouldCancel && shouldCancel()) {
            ses.remove_torrent(h, lt::session::delete_files);
            if (err) *err = "Anulowane";
            return false;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - started).count();
        if (elapsed > timeoutSec) {
            ses.remove_torrent(h);
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
            Progress p;
            p.fraction = st.progress;
            p.downloadedMb = st.total_done / (1024.0 * 1024.0);
            p.totalMb = st.total_wanted / (1024.0 * 1024.0);
            p.peers = st.num_peers;
            p.seeds = st.num_seeds;
            p.downloadRateKBs = st.download_payload_rate / 1024.0;
            p.state = stateName(st.state);
            onProgress(p);
        }

        if (!finished)
            ses.wait_for_alert(std::chrono::milliseconds(500));
    }

    if (!failMsg.empty() && !finished) {
        ses.remove_torrent(h);
        if (err) *err = failMsg;
        return false;
    }

    // Stop seeding; keep files
    try {
        h.pause();
        ses.remove_torrent(h);
    } catch (...) {}

    return true;
}

} // namespace torrent
