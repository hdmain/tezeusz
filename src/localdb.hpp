#pragma once
#include "tmdb.hpp"
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
    case IssueType::Video: return "Wideo";
    case IssueType::Audio: return "Audio";
    case IssueType::Subtitles: return "Napisy";
    case IssueType::Other: return "Inne";
    }
    return "?";
}

inline const char* issueStatusLabel(IssueStatus s) {
    return s == IssueStatus::Open ? "Otwarte" : "Rozwiązane";
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

} // namespace localdb
