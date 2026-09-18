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
    std::string path;          // video file
    std::string folder;        // containing folder
    std::string posterPath;    // TMDB poster if known
    MediaType mediaType = MediaType::Movie;
    int tmdbId = 0;
    int64_t sizeBytes = 0;
};

// Scan moviesPath/tvPath (+ Available requests) for playable videos.
std::vector<Item> scan();

// If path is a folder, pick the largest video inside; otherwise return path.
std::string resolvePlayable(const std::string& pathOrFolder);

// Delete title from disk (folder if under library, else file). Returns false on error.
bool removeItem(const Item& item, std::string* err = nullptr);

// Human-readable size, e.g. "1.4 GB".
std::string formatSize(int64_t bytes);

} // namespace library
