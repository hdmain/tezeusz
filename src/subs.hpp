#pragma once
#include "tmdb.hpp"
#include <string>

namespace subs {

void init();
void shutdown();
void tick();

// Queue download of EN + preferred-language sidecars next to the video.
// sceneName = original release name (from the torrent) when known — Bazarr-style
// refiner uses it to guess resolution/source/group for scoring.
void enqueue(const std::string& videoPath, MediaType type, int tmdbId,
             const std::string& imdbId = {}, const std::string& title = {},
             int season = 0, int episode = 0, const std::string& sceneName = {});

// Walk a folder (or single file) and enqueue every video missing subs.
void enqueuePath(const std::string& pathOrFolder, MediaType type, int tmdbId,
                 const std::string& imdbId = {}, const std::string& title = {},
                 const std::string& sceneName = {});

// Scan library + Available requests for missing EN / preferred sidecars.
void scanLibrary();

// Last status line for settings UI.
std::string statusMessage();
bool busy();

} // namespace subs
