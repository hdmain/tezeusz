#pragma once
#include "tmdb.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace library {

struct Item {
    std::string id;
    std::string title;
    std::string year;
    std::string path;          // video file (or .zip when archived)
    std::string folder;        // containing folder
    std::string posterPath;    // TMDB poster if known
    MediaType mediaType = MediaType::Movie;
    int tmdbId = 0;
    int64_t sizeBytes = 0;
    bool archived = false;     // ZIP under Archives/ - shown greyed at end of library
};

// Scan movies/TV primary + extra roots (+ Available requests) for playable videos.
std::vector<Item> scan();

// If path is a folder, pick the largest video inside; otherwise return path.
std::string resolvePlayable(const std::string& pathOrFolder);

// Delete title folder from disk (not just from the UI list).
bool removeItem(const Item& item, std::string* err = nullptr);

// Compress title folder to Archives/*.zip, then remove originals from the library.
// Returns the zip path on success (empty string on failure - see err).
std::string archiveItem(const Item& item, std::string* err = nullptr);

// Pack title folder to Exports/*.zip for transfer to another device (keeps originals).
// Embeds seerr-export.json with TMDB metadata for import.
std::string exportItem(const Item& item, std::string* err = nullptr);

// Import a Seerr export ZIP into the local library (no re-download).
// Returns the library video/folder path on success.
std::string importExportZip(const std::string& zipPath, std::string* err = nullptr);

// Human-readable size, e.g. "1.4 GB".
std::string formatSize(int64_t bytes);

} // namespace library
