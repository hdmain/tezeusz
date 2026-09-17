#pragma once
#include <string>
#include <vector>
#include <map>
#include <future>
#include "json.hpp"

using json = nlohmann::json;

// ---------------- Data model (mirrors seerr's TMDB interfaces) ----------------

enum class MediaType { Movie, TV, Person };
const char* mediaTypeName(MediaType t);

struct Genre { int id = 0; std::string name; };

struct CastMember {
    int id = 0;
    std::string name, character, profilePath, creditId;
};
struct CrewMember {
    int id = 0;
    std::string name, job, department, profilePath;
};
struct Video {
    std::string key, name, site, type;
    bool official = false;
    bool isYouTube() const { return site == "YouTube" && !key.empty(); }
    bool isTrailer() const { return type == "Trailer"; }
    bool isTeaser() const { return type == "Teaser"; }
    bool isPlayablePromo() const {
        return isYouTube() && (isTrailer() || isTeaser() || type == "Clip" || type == "Featurette");
    }
};

struct MediaItem {
    MediaType mediaType = MediaType::Movie;
    int id = 0;
    std::string title, originalTitle, overview, posterPath, backdropPath;
    std::string releaseDate;   // movie: release_date, tv: first_air_date
    double voteAverage = 0;
    int voteCount = 0;
    std::vector<int> genreIds;
    // tv extras
    int numberOfSeasons = 0, numberOfEpisodes = 0;
    std::string status;
    // Cached w300 poster URL (built once) — avoids string alloc every frame per card
    mutable std::string posterUrl300;

    std::string year() const;
    std::string posterUrl(const std::string& size = "w300_and_h450_face") const;
    std::string backdropUrl(const std::string& size = "w1280") const;
};

struct SeasonInfo {
    int id = 0, seasonNumber = 0, episodeCount = 0;
    std::string name, overview, posterPath, airDate;
};

struct Details {
    MediaType mediaType = MediaType::Movie;
    int id = 0;
    std::string title, originalTitle, tagline, overview, posterPath, backdropPath;
    std::string releaseDate, status, imdbId, homepage;
    int runtime = 0;                       // movies only
    double voteAverage = 0; int voteCount = 0;
    double popularity = 0;
    long long budget = 0, revenue = 0;     // movies only
    std::vector<Genre> genres;
    std::vector<std::string> spokenLanguages;
    std::vector<CastMember> cast;
    std::vector<CrewMember> crew;          // directors first
    std::vector<Video> videos;
    std::vector<std::string> productionCompanies;
    std::vector<std::string> originCountries;
    // tv
    std::vector<SeasonInfo> seasons;
    std::string nextEpisodeAirDate;
    std::string lastEpisodeAirDate;
    int numberOfSeasons = 0, numberOfEpisodes = 0;

    std::string year() const;
    std::string posterUrl(const std::string& size = "w600_and_h900_bestv2") const;
    std::string backdropUrl(const std::string& size = "w1920_and_h800_multi_faces") const;
    std::string tmdbUrl() const;
    std::string imdbUrl() const;
    const Video* bestTrailer() const;
};

struct PagedResult {
    int page = 1, total_pages = 1, total_results = 0;
    std::vector<MediaItem> results;
    bool loaded = false;
    std::string error;
};

// ---------------- Async request wrapper ----------------

template <typename T>
struct AsyncReq {
    std::future<T> fut;
    bool valid() const { return fut.valid(); }
    bool ready() { return valid() && fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }
    T take() { return fut.get(); }
};

// ---------------- TMDB client (same endpoints seerr uses) ----------------

class Tmdb {
public:
    static const char* BASE;      // api.themoviedb.org/3
    static const char* IMAGES;    // image.tmdb.org
    static const std::string& key(); // from config.json / TMDB_API_KEY env, else public seerr key

    static std::string img(const std::string& size, const std::string& path);

    // all async; parse on main thread when ready
    static AsyncReq<PagedResult> trending(const std::string& media, const std::string& window = "week");
    static AsyncReq<PagedResult> discover(int page, const std::map<std::string, std::string>& params);
    static AsyncReq<PagedResult> searchMulti(const std::string& query, int page = 1);
    static AsyncReq<PagedResult> movieList(const std::string& endpoint); // popular/upcoming/now_playing/top_rated
    static AsyncReq<PagedResult> tvList(const std::string& endpoint);    // popular/on_the_air/upcoming/top_rated
    static AsyncReq<PagedResult> genreFilms(int genreId, bool tv, int page = 1);
    static AsyncReq<PagedResult> similar(MediaType t, int id);
    static AsyncReq<PagedResult> recommendations(MediaType t, int id);
    static AsyncReq<Details> movie(int id);
    static AsyncReq<Details> tv(int id);
    static AsyncReq<std::vector<Genre>> genres(bool tv);
    static AsyncReq<std::vector<std::string>> languages();
};

// parse helpers exposed for tests / caching
PagedResult parsePaged(const json& j, MediaType defaultType);
Details parseMovie(const json& j);
Details parseTv(const json& j);
