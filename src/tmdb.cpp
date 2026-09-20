#include "tmdb.hpp"
#include "app.hpp"
#include "http.hpp"
#include "util.hpp"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <set>

const char* Tmdb::BASE = "https://api.themoviedb.org/3";
const char* Tmdb::IMAGES = "https://image.tmdb.org/t/p";

namespace cfg {
static std::string g_key, g_lang;
static bool g_loaded = false;
static void load() {
    if (g_loaded) return;
    g_loaded = true;
    g_lang = "en-US";
    // seerr ships a public TMDB key in server/api/themoviedb/index.ts; use it as fallback
    g_key = "431a8708161bcd1f1fbe7536137e61ed";
    auto tryFile = [&](const std::string& p) {
        std::string s = util::readFile(p);
        if (s.empty()) return;
        try {
            json j = json::parse(s);
            if (j.contains("tmdb_api_key") && j["tmdb_api_key"].is_string() &&
                !j["tmdb_api_key"].get<std::string>().empty())
                g_key = j["tmdb_api_key"].get<std::string>();
            // Optional override; stack::init / UI language normally wins via syncFromUi.
            if (j.contains("language") && j["language"].is_string())
                g_lang = j["language"].get<std::string>();
        } catch (...) {}
    };
    tryFile(util::exeDir() + "/config.json");
    tryFile(util::appDataPath("config.json"));
    if (const char* env = std::getenv("TMDB_API_KEY")) {
        if (env[0]) g_key = env;
    }
}
const char* apiKey() { load(); return g_key.c_str(); }
const char* language() { load(); return g_lang.c_str(); }
void setLanguage(const std::string& tmdbLang) {
    load();
    if (!tmdbLang.empty()) g_lang = tmdbLang;
}
void syncFromUi(const std::string& uiCode) {
    if (uiCode == "pl") setLanguage("pl-PL");
    else setLanguage("en-US");
}
} // namespace cfg

const std::string& Tmdb::key() { static std::string s; s = cfg::apiKey(); return s; }

std::string Tmdb::img(const std::string& size, const std::string& path) {
    if (path.empty()) return "";
    return std::string(IMAGES) + "/" + size + path;
}

const char* mediaTypeName(MediaType t) {
    switch (t) { case MediaType::Movie: return "movie"; case MediaType::TV: return "tv"; default: return "person"; }
}

std::string MediaItem::year() const { return util::yearOf(releaseDate); }
std::string MediaItem::posterUrl(const std::string& size) const {
    if (posterPath.empty()) return {};
    if (size == "w300_and_h450_face") {
        if (posterUrl300.empty())
            posterUrl300 = std::string(Tmdb::IMAGES) + "/w300_and_h450_face" + posterPath;
        return posterUrl300;
    }
    return std::string(Tmdb::IMAGES) + "/" + size + posterPath;
}
std::string MediaItem::backdropUrl(const std::string& size) const {
    return backdropPath.empty() ? "" : std::string(Tmdb::IMAGES) + "/" + size + backdropPath;
}

std::string Details::year() const { return util::yearOf(releaseDate); }
std::string Details::posterUrl(const std::string& size) const {
    return posterPath.empty() ? "" : std::string(Tmdb::IMAGES) + "/" + size + posterPath;
}
std::string Details::backdropUrl(const std::string& size) const {
    return backdropPath.empty() ? "" : std::string(Tmdb::IMAGES) + "/" + size + backdropPath;
}
std::string Details::tmdbUrl() const {
    return std::string("https://www.themoviedb.org/") + mediaTypeName(mediaType) + "/" + std::to_string(id);
}
std::string Details::imdbUrl() const {
    return imdbId.empty() ? "" : "https://www.imdb.com/title/" + imdbId + "/";
}
const Video* Details::bestTrailer() const {
    const Video* bestTrailer = nullptr;
    const Video* bestTeaser = nullptr;
    const Video* bestAny = nullptr;
    for (auto& v : videos) {
        if (!v.isYouTube()) continue;
        if (v.key == "4xhQBBObkGk" || v.key == "LrQvln0xkXw") continue; // seerr skips these
        if (v.isTrailer()) {
            if (!bestTrailer || (v.official && !bestTrailer->official)) bestTrailer = &v;
        } else if (v.isTeaser()) {
            if (!bestTeaser || (v.official && !bestTeaser->official)) bestTeaser = &v;
        } else if (v.isPlayablePromo()) {
            if (!bestAny) bestAny = &v;
        }
    }
    if (bestTrailer) return bestTrailer;
    if (bestTeaser) return bestTeaser;
    return bestAny;
}

// ------------------------------------------------------------------ parsing

static std::string jstr(const json& j, const char* key) {
    if (!j.contains(key)) return "";
    const auto& v = j[key];
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    return "";
}
static double jnum(const json& j, const char* key, double dflt = 0) {
    if (!j.contains(key) || !j[key].is_number()) return dflt;
    return j[key].get<double>();
}
static bool jbool(const json& j, const char* key, bool dflt = false) {
    if (!j.contains(key)) return dflt;
    const auto& v = j[key];
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_string()) {
        auto s = v.get<std::string>();
        return s == "true" || s == "1" || s == "True";
    }
    if (v.is_number_integer()) return v.get<int>() != 0;
    return dflt;
}

static void parseVideosInto(Details& d, const json& j) {
    if (!j.contains("videos") || !j["videos"].contains("results")) return;
    for (auto& v : j["videos"]["results"]) {
        Video vid;
        vid.key = jstr(v, "key");
        vid.name = jstr(v, "name");
        vid.site = jstr(v, "site");
        vid.type = jstr(v, "type");
        vid.official = jbool(v, "official");
        if (!vid.key.empty()) d.videos.push_back(std::move(vid));
    }
}

static MediaType typeFromStr(const std::string& s) {
    if (s == "movie") return MediaType::Movie;
    if (s == "tv") return MediaType::TV;
    return MediaType::Person;
}

MediaItem itemFromJson(const json& r, MediaType defaultType) {
    MediaItem m;
    m.mediaType = r.contains("media_type") ? typeFromStr(jstr(r, "media_type")) : defaultType;
    m.id = (int)jnum(r, "id");
    m.title = jstr(r, "title");
    if (m.title.empty()) m.title = jstr(r, "name");
    m.originalTitle = jstr(r, "original_title");
    if (m.originalTitle.empty()) m.originalTitle = jstr(r, "original_name");
    m.overview = jstr(r, "overview");
    m.posterPath = jstr(r, "poster_path");
    m.backdropPath = jstr(r, "backdrop_path");
    m.releaseDate = jstr(r, "release_date");
    if (m.releaseDate.empty()) m.releaseDate = jstr(r, "first_air_date");
    m.voteAverage = jnum(r, "vote_average");
    m.voteCount = (int)jnum(r, "vote_count");
    if (r.contains("genre_ids")) for (auto& g : r["genre_ids"]) m.genreIds.push_back(g.get<int>());
    m.numberOfSeasons = (int)jnum(r, "number_of_seasons");
    m.numberOfEpisodes = (int)jnum(r, "number_of_episodes");
    m.status = jstr(r, "status");

    if (m.mediaType == MediaType::Person || defaultType == MediaType::Person) {
        m.mediaType = MediaType::Person;
        std::string profile = jstr(r, "profile_path");
        if (m.posterPath.empty()) m.posterPath = profile;
        m.knownForDepartment = jstr(r, "known_for_department");
        if (m.overview.empty() && r.contains("known_for") && r["known_for"].is_array()) {
            std::string kf;
            for (auto& k : r["known_for"]) {
                std::string t = jstr(k, "title");
                if (t.empty()) t = jstr(k, "name");
                if (t.empty()) continue;
                if (!kf.empty()) kf += ", ";
                kf += t;
                if (kf.size() > 90) break;
            }
            m.overview = std::move(kf);
        }
    }
    return m;
}

PagedResult parsePaged(const json& j, MediaType defaultType, bool includePeople) {
    PagedResult p;
    p.loaded = true;
    if (!j.contains("results") || !j["results"].is_array()) { p.error = "unexpected response"; return p; }
    p.page = (int)jnum(j, "page", 1);
    p.total_pages = (int)jnum(j, "total_pages", 1);
    p.total_results = (int)jnum(j, "total_results", 0);
    for (auto& r : j["results"]) {
        if (!includePeople && r.contains("media_type") && jstr(r, "media_type") == "person")
            continue;
        MediaItem item = itemFromJson(r, defaultType);
        if (!includePeople && item.mediaType == MediaType::Person)
            continue;
        p.results.push_back(std::move(item));
    }
    return p;
}

Details parseMovie(const json& j) {
    Details d;
    d.mediaType = MediaType::Movie;
    d.id = (int)jnum(j, "id");
    d.title = jstr(j, "title");
    d.originalTitle = jstr(j, "original_title");
    d.tagline = jstr(j, "tagline");
    d.overview = jstr(j, "overview");
    d.posterPath = jstr(j, "poster_path");
    d.backdropPath = jstr(j, "backdrop_path");
    d.releaseDate = jstr(j, "release_date");
    d.status = jstr(j, "status");
    d.imdbId = jstr(j, "imdb_id");
    // TV (and some movie payloads) put IMDb under external_ids
    if (d.imdbId.empty() && j.contains("external_ids") && j["external_ids"].is_object())
        d.imdbId = jstr(j["external_ids"], "imdb_id");
    d.homepage = jstr(j, "homepage");
    d.runtime = (int)jnum(j, "runtime");
    d.voteAverage = jnum(j, "vote_average");
    d.voteCount = (int)jnum(j, "vote_count");
    d.popularity = jnum(j, "popularity");
    d.budget = (long long)jnum(j, "budget");
    d.revenue = (long long)jnum(j, "revenue");
    if (j.contains("genres")) for (auto& g : j["genres"]) d.genres.push_back({(int)jnum(g, "id"), jstr(g, "name")});
    if (j.contains("spoken_languages")) for (auto& l : j["spoken_languages"]) d.spokenLanguages.push_back(jstr(l, "english_name"));
    if (j.contains("production_companies")) for (auto& c : j["production_companies"]) {
        auto n = jstr(c, "name"); if (!n.empty()) d.productionCompanies.push_back(n);
    }
    if (j.contains("origin_country")) for (auto& c : j["origin_country"]) d.originCountries.push_back(c.get<std::string>());
    if (j.contains("credits") && j["credits"].contains("cast")) {
        int n = 0;
        for (auto& c : j["credits"]["cast"]) {
            if (n++ >= 20) break;
            CastMember m; m.id = (int)jnum(c, "id"); m.name = jstr(c, "name");
            m.character = jstr(c, "character"); m.profilePath = jstr(c, "profile_path");
            d.cast.push_back(m);
        }
    }
    if (j.contains("credits") && j["credits"].contains("crew")) {
        for (auto& c : j["credits"]["crew"]) {
            auto job = jstr(c, "job");
            if (job == "Director" || job == "Screenplay" || job == "Writer") {
                CrewMember m; m.id = (int)jnum(c, "id"); m.name = jstr(c, "name");
                m.job = job; m.department = jstr(c, "department"); m.profilePath = jstr(c, "profile_path");
                d.crew.push_back(m);
            }
        }
    }
    if (j.contains("videos") && j["videos"].contains("results")) {
        parseVideosInto(d, j);
    }
    return d;
}

Details parseTv(const json& j) {
    Details d = parseMovie(j); // same scalar field names, mostly
    d.mediaType = MediaType::TV;
    d.title = jstr(j, "name");
    d.originalTitle = jstr(j, "original_name");
    d.releaseDate = jstr(j, "first_air_date");
    d.numberOfSeasons = (int)jnum(j, "number_of_seasons");
    d.numberOfEpisodes = (int)jnum(j, "number_of_episodes");
    d.nextEpisodeAirDate = jstr(j, "next_episode_to_air").empty() ? "" : jstr(j, "next_episode_to_air");
    if (j.contains("last_episode_to_air") && j["last_episode_to_air"].is_object())
        d.lastEpisodeAirDate = jstr(j["last_episode_to_air"], "air_date");
    // videos already parsed in parseMovie — don't clear them
    if (d.videos.empty()) parseVideosInto(d, j);
    d.seasons.clear();
    if (j.contains("seasons")) for (auto& s : j["seasons"]) {
        SeasonInfo si;
        si.id = (int)jnum(s, "id"); si.seasonNumber = (int)jnum(s, "season_number");
        si.name = jstr(s, "name"); si.overview = jstr(s, "overview");
        si.posterPath = jstr(s, "poster_path"); si.airDate = jstr(s, "air_date");
        si.episodeCount = (int)jnum(s, "episode_count");
        d.seasons.push_back(si);
    }
    return d;
}

// ------------------------------------------------------------------ requests

static std::string withKey(const std::string& path, std::map<std::string, std::string> params) {
    params["api_key"] = cfg::apiKey();
    if (!params.count("language"))
        params["language"] = cfg::language();
    return std::string(Tmdb::BASE) + path + "?" + util::buildQuery(params);
}

static AsyncReq<PagedResult> pagedReq(const std::string& url, MediaType defaultType,
                                      bool includePeople = false) {
    AsyncReq<PagedResult> req;
    req.fut = std::async(std::launch::async, [url, defaultType, includePeople]() -> PagedResult {
        PagedResult p;
        auto r = http::get(url);
        if (!r.ok()) { p.error = r.err.empty() ? "HTTP " + std::to_string(r.status) : r.err; return p; }
        try { p = parsePaged(json::parse(r.body), defaultType, includePeople); }
        catch (std::exception& e) { p.error = e.what(); }
        return p;
    });
    return req;
}

AsyncReq<PagedResult> Tmdb::trending(const std::string& media, const std::string& window) {
    return pagedReq(withKey("/trending/" + media + "/" + window, {}), MediaType::Movie);
}

AsyncReq<PagedResult> Tmdb::discover(int page, const std::map<std::string, std::string>& params) {
    bool tv = params.count("type") && params.at("type") == "tv";
    auto p = params;
    p.erase("type");
    p["page"] = std::to_string(page);
    p["include_adult"] = "false";
    std::string path = tv ? "/discover/tv" : "/discover/movie";
    MediaType t = tv ? MediaType::TV : MediaType::Movie;
    return pagedReq(withKey(path, p), t);
}

AsyncReq<PagedResult> Tmdb::searchMulti(const std::string& query, int page) {
    return search(query, page, SearchFilter::All);
}

AsyncReq<PagedResult> Tmdb::search(const std::string& query, int page, SearchFilter filter) {
    std::map<std::string, std::string> params = {
        {"query", query}, {"page", std::to_string(page)}, {"include_adult", "false"}
    };
    switch (filter) {
    case SearchFilter::Movies:
        return pagedReq(withKey("/search/movie", params), MediaType::Movie);
    case SearchFilter::Tv:
        return pagedReq(withKey("/search/tv", params), MediaType::TV);
    case SearchFilter::People:
        return pagedReq(withKey("/search/person", params), MediaType::Person, true);
    case SearchFilter::All:
    default:
        return pagedReq(withKey("/search/multi", params), MediaType::Movie, true);
    }
}

AsyncReq<PagedResult> Tmdb::movieList(const std::string& endpoint) {
    return pagedReq(withKey("/movie/" + endpoint, {}), MediaType::Movie);
}
AsyncReq<PagedResult> Tmdb::tvList(const std::string& endpoint) {
    return pagedReq(withKey("/tv/" + endpoint, {}), MediaType::TV);
}

AsyncReq<PagedResult> Tmdb::genreFilms(int genreId, bool tv, int page) {
    return discover(page, {{"type", tv ? "tv" : "movie"},
                           {"with_genres", std::to_string(genreId)},
                           {"sort_by", "popularity.desc"}});
}

AsyncReq<PagedResult> Tmdb::similar(MediaType t, int id) {
    std::string base = t == MediaType::TV ? "/tv/" : "/movie/";
    return pagedReq(withKey(base + std::to_string(id) + "/similar", {}), t);
}
AsyncReq<PagedResult> Tmdb::recommendations(MediaType t, int id) {
    std::string base = t == MediaType::TV ? "/tv/" : "/movie/";
    return pagedReq(withKey(base + std::to_string(id) + "/recommendations", {}), t);
}

// Fill empty localized text fields from an English payload (TMDB leaves them blank when
// no translation exists for the requested language).
static void mergeEnText(Details& d, const Details& en) {
    if (d.title.empty()) d.title = en.title;
    if (d.tagline.empty()) d.tagline = en.tagline;
    if (d.overview.empty()) d.overview = en.overview;
    if (d.genres.empty() && !en.genres.empty()) d.genres = en.genres;
    for (size_t i = 0; i < d.seasons.size() && i < en.seasons.size(); i++) {
        if (d.seasons[i].name.empty()) d.seasons[i].name = en.seasons[i].name;
        if (d.seasons[i].overview.empty()) d.seasons[i].overview = en.seasons[i].overview;
    }
}

static AsyncReq<Details> detailsReq(const std::string& path, bool tv) {
    AsyncReq<Details> req;
    req.fut = std::async(std::launch::async, [path, tv]() -> Details {
        Details d;
        std::string lang = cfg::language();
        // Like seerr: request localized + English video languages so trailers aren't missing for pl-PL etc.
        auto r = http::get(withKey(path, {
            {"append_to_response", "credits,videos,external_ids"},
            {"include_video_language", lang + ",en,null"}
        }));
        if (!r.ok()) return d;
        try {
            json j = json::parse(r.body);
            d = tv ? parseTv(j) : parseMovie(j);
        } catch (...) { return d; }

        const bool nonEn = lang.rfind("en", 0) != 0;
        bool needText = nonEn && (d.overview.empty() || d.title.empty() || d.tagline.empty());
        if (!needText && nonEn) {
            for (auto& s : d.seasons)
                if (s.overview.empty() || s.name.empty()) { needText = true; break; }
        }

        // Fallback: missing localized text + trailers from English
        bool hasTrailer = false;
        for (auto& v : d.videos) if (v.isTrailer() && v.isYouTube()) { hasTrailer = true; break; }
        if (nonEn && (needText || !hasTrailer)) {
            auto r2 = http::get(withKey(path, {
                {"language", "en-US"},
                {"append_to_response", needText ? "credits,videos,external_ids" : "videos"},
                {"include_video_language", "en"}
            }));
            if (r2.ok()) {
                try {
                    json j2 = json::parse(r2.body);
                    if (needText) {
                        Details en = tv ? parseTv(j2) : parseMovie(j2);
                        mergeEnText(d, en);
                        if (!hasTrailer) {
                            std::set<std::string> have;
                            for (auto& v : d.videos) have.insert(v.key);
                            for (auto& v : en.videos) {
                                if (v.isYouTube() && (v.isTrailer() || v.isTeaser()) && !have.count(v.key))
                                    d.videos.push_back(std::move(v));
                            }
                            for (auto& v : d.videos)
                                if (v.isTrailer() && v.isYouTube()) { hasTrailer = true; break; }
                        }
                    }
                    if (!hasTrailer) {
                        std::set<std::string> have;
                        for (auto& v : d.videos) have.insert(v.key);
                        Details tmp;
                        parseVideosInto(tmp, j2);
                        for (auto& v : tmp.videos) {
                            if (v.isYouTube() && (v.isTrailer() || v.isTeaser()) && !have.count(v.key))
                                d.videos.push_back(std::move(v));
                        }
                    }
                } catch (...) {}
            }
        }
        return d;
    });
    return req;
}

AsyncReq<Details> Tmdb::movie(int id) {
    return detailsReq("/movie/" + std::to_string(id), false);
}
AsyncReq<Details> Tmdb::tv(int id) {
    return detailsReq("/tv/" + std::to_string(id), true);
}

AsyncReq<std::vector<EpisodeInfo>> Tmdb::tvSeason(int tvId, int seasonNumber) {
    AsyncReq<std::vector<EpisodeInfo>> req;
    req.fut = std::async(std::launch::async, [tvId, seasonNumber]() -> std::vector<EpisodeInfo> {
        std::vector<EpisodeInfo> out;
        std::string path = "/tv/" + std::to_string(tvId) + "/season/" + std::to_string(seasonNumber);
        auto r = http::get(withKey(path, {}));
        if (!r.ok()) return out;
        try {
            json j = json::parse(r.body);
            if (!j.contains("episodes") || !j["episodes"].is_array()) return out;
            for (auto& e : j["episodes"]) {
                EpisodeInfo ep;
                ep.id = (int)jnum(e, "id");
                ep.episodeNumber = (int)jnum(e, "episode_number");
                ep.seasonNumber = (int)jnum(e, "season_number", (double)seasonNumber);
                ep.name = jstr(e, "name");
                ep.overview = jstr(e, "overview");
                ep.airDate = jstr(e, "air_date");
                ep.stillPath = jstr(e, "still_path");
                out.push_back(std::move(ep));
            }
        } catch (...) { return out; }

        // EN backup for missing episode names/overviews when UI isn't English
        std::string lang = cfg::language();
        if (lang.rfind("en", 0) == 0) return out;
        bool need = false;
        for (auto& ep : out) if (ep.name.empty() || ep.overview.empty()) { need = true; break; }
        if (!need) return out;
        auto r2 = http::get(withKey(path, {{"language", "en-US"}}));
        if (!r2.ok()) return out;
        try {
            json j2 = json::parse(r2.body);
            if (!j2.contains("episodes") || !j2["episodes"].is_array()) return out;
            for (auto& e : j2["episodes"]) {
                int num = (int)jnum(e, "episode_number");
                for (auto& ep : out) {
                    if (ep.episodeNumber != num) continue;
                    if (ep.name.empty()) ep.name = jstr(e, "name");
                    if (ep.overview.empty()) ep.overview = jstr(e, "overview");
                    break;
                }
            }
        } catch (...) {}
        return out;
    });
    return req;
}

AsyncReq<std::vector<Genre>> Tmdb::genres(bool tv) {
    AsyncReq<std::vector<Genre>> req;
    req.fut = std::async(std::launch::async, [tv]() -> std::vector<Genre> {
        std::vector<Genre> out;
        auto r = http::get(withKey(tv ? "/genre/tv/list" : "/genre/movie/list", {}));
        if (!r.ok()) return out;
        try {
            json j = json::parse(r.body);
            for (auto& g : j["genres"]) out.push_back({(int)jnum(g, "id"), jstr(g, "name")});
        } catch (...) {}
        return out;
    });
    return req;
}

AsyncReq<std::vector<std::string>> Tmdb::languages() {
    AsyncReq<std::vector<std::string>> req;
    req.fut = std::async(std::launch::async, []() -> std::vector<std::string> {
        std::vector<std::string> out;
        auto r = http::get(std::string(BASE) + "/configuration/languages?api_key=" + cfg::apiKey(),
                           "application/json");
        if (!r.ok()) return out;
        try {
            json j = json::parse(r.body);
            for (auto& l : j) {
                std::string iso = jstr(l, "iso_639_1");
                std::string name = jstr(l, "name");
                if (!iso.empty() && !name.empty()) out.push_back(name + "|" + iso);
            }
        } catch (...) {}
        return out;
    });
    return req;
}
