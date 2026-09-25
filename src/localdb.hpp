#pragma once
#include "tmdb.hpp"
#include "i18n.hpp"
#include <cstdint>
#include <set>
#include <string>
#include <vector>

// Local Seerr-like data (blocklist / issues / profile) — no remote API.
namespace localdb {

enum class IssueType { Video = 1, Audio = 2, Subtitles = 3, Other = 4 };
enum class IssueStatus { Open = 1, Resolved = 2 };

inline const char* issueTypeLabel(IssueType t) {
    switch (t) {
    case IssueType::Video: return i18n::tr("issue.video");
    case IssueType::Audio: return i18n::tr("issue.audio");
    case IssueType::Subtitles: return i18n::tr("issue.subtitles");
    case IssueType::Other: return i18n::tr("issue.other");
    }
    return "?";
}

inline const char* issueStatusLabel(IssueStatus s) {
    return s == IssueStatus::Open ? i18n::tr("issues.open") : i18n::tr("issues.resolved");
}

struct BlockItem {
    MediaType mediaType = MediaType::Movie;
    int tmdbId = 0;
    std::string title;
    std::string year;
    std::string posterPath;
    int64_t createdAt = 0;
};

struct Issue {
    std::string id;
    MediaType mediaType = MediaType::Movie;
    int tmdbId = 0;
    std::string title;
    std::string year;
    std::string posterPath;
    IssueType issueType = IssueType::Other;
    IssueStatus status = IssueStatus::Open;
    std::string message;
    int64_t createdAt = 0;
    int64_t updatedAt = 0;
};

struct LocalUser {
    std::string displayName = "Tezeusz";
    std::string email = "local@seerr";
    int64_t createdAt = 0;
};

void init();
void shutdown();

bool isBlocked(MediaType t, int tmdbId);
void addBlock(const Details& d);
void addBlock(MediaType t, int tmdbId, const std::string& title,
              const std::string& year, const std::string& posterPath);
bool removeBlock(MediaType t, int tmdbId);
std::vector<BlockItem> listBlock(const std::string& search = {});

std::string addIssue(MediaType t, int tmdbId, const std::string& title,
                     const std::string& year, const std::string& posterPath,
                     IssueType type, const std::string& message);
bool setIssueStatus(const std::string& id, IssueStatus s);
bool removeIssue(const std::string& id);
std::vector<Issue> listIssues(IssueStatus filter = IssueStatus::Open, bool all = false);

LocalUser& user();
void saveUser();

void loadWatchlist(std::set<std::string>& out);
void saveWatchlist(const std::set<std::string>& wl);

// Crash-safe resume positions for in-app playback (keyed by video file path).
// Saved continuously while watching — not only on close.
struct PlaybackProgress {
    double position = 0;   // 0..1
    int64_t timeMs = 0;
    int64_t durationMs = 0;
    int64_t updatedAt = 0;
};

// Per-file audio / subtitle choice (survives finished / cleared resume points).
struct PlaybackTracks {
    bool hasSubtitle = false;
    bool hasAudio = false;
    int subtitleId = -1;       // -1 = off
    int audioId = -1;
    std::string subtitleName;  // preferred match key (stable across remux)
    std::string audioName;
};

// Returns true if a usable resume point exists (~>30s and <~95%).
bool getPlaybackProgress(const std::string& path, PlaybackProgress* out);
// Persist current position (throttled by caller). Clears position near start/end
// but keeps track preferences.
void savePlaybackProgress(const std::string& path, double position, int64_t timeMs, int64_t durationMs);
void clearPlaybackProgress(const std::string& path);

bool getPlaybackTracks(const std::string& path, PlaybackTracks* out);
void savePlaybackTracks(const std::string& path, int subtitleId, const std::string& subtitleName,
                        int audioId, const std::string& audioName);

} // namespace localdb
