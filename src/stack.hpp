#pragma once
#include "tmdb.hpp"
#include <string>
#include <vector>

// All-in-one download stack inside this app (no Prowlarr / Radarr / qBittorrent / aria2 required).
// Indexer: apibay · Engine: libtorrent · Library: local folders.
namespace stack {

enum class ReqStatus {
    Pending = 0,
    Searching,
    Downloading,
    Importing,
    Available,
    Failed,
    Declined
};

// App card/button status: 0 = can request, 1 = in progress, 2 = failed (retry), 3 = available
inline int uiStatus(ReqStatus s) {
    switch (s) {
    case ReqStatus::Available: return 3;
    case ReqStatus::Failed: return 2;
    case ReqStatus::Declined: return 0;
    default: return 1;
    }
}

inline const char* statusLabel(ReqStatus s) {
    switch (s) {
    case ReqStatus::Pending: return "Oczekuje";
    case ReqStatus::Searching: return "Szukanie";
    case ReqStatus::Downloading: return "Pobieranie";
    case ReqStatus::Importing: return "Import";
    case ReqStatus::Available: return "Dostępne";
    case ReqStatus::Failed: return "Błąd";
    case ReqStatus::Declined: return "Anulowane";
    }
    return "?";
}

struct StackConfig {
    std::string moviesPath;
    std::string tvPath;
    std::string downloadPath;
    int minSeeders = 2;
    std::string preferredQuality = "1080p";
    bool autoStart = true;

    static StackConfig& get();
    void load();
    void save() const;
};

struct MediaRequest {
    std::string id;
    MediaType mediaType = MediaType::Movie;
    int tmdbId = 0;
    std::string title;
    std::string originalTitle; // English / original — used for indexer search (Radarr-style)
    std::string year;
    std::string imdbId;
    std::vector<int> seasons;
    ReqStatus status = ReqStatus::Pending;
    std::string message;
    std::string releaseTitle;
    std::string magnetOrUrl;
    std::string torrentHash;
    std::string libraryPath;
    double progress = 0;
    int64_t createdAt = 0;
    int64_t updatedAt = 0;
};

void init();
void shutdown();
void tick();

std::string requestMedia(const Details& d, const std::vector<int>& seasons = {});
std::string requestMedia(MediaType type, int tmdbId, const std::string& title,
                         const std::string& year, const std::string& imdbId = {},
                         const std::vector<int>& seasons = {},
                         const std::string& originalTitle = {});

std::vector<MediaRequest> listRequests();
bool cancelRequest(const std::string& id);
void syncAppStatuses();

} // namespace stack
