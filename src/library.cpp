#include "library.hpp"
#include "stack.hpp"
#include "util.hpp"
#include "i18n.hpp"
#include "zipwrite.hpp"
#include "json.hpp"
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>

using json = nlohmann::json;

namespace fs = std::filesystem;

namespace library {
namespace {

bool isVideoExt(const fs::path& p) {
    auto e = util::lower(p.extension().string());
    return e == ".mkv" || e == ".mp4" || e == ".avi" || e == ".m4v" ||
           e == ".ts" || e == ".m2ts" || e == ".webm" || e == ".mov";
}

std::string biggestVideoIn(const fs::path& root) {
    std::string best;
    uintmax_t bestSz = 0;
    std::error_code ec;
    if (!fs::exists(root, ec)) return {};
    if (fs::is_regular_file(root, ec) && isVideoExt(root)) return root.string();
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;
        if (!isVideoExt(it->path())) continue;
        auto sz = it->file_size(ec);
        if (sz > bestSz) { bestSz = sz; best = it->path().string(); }
    }
    return best;
}

void parseFolderName(const std::string& folder, std::string& title, std::string& year) {
    title = folder;
    year.clear();
    if (folder.size() > 6 && folder.back() == ')') {
        auto lp = folder.rfind(" (");
        if (lp != std::string::npos && folder.size() - lp == 7) {
            std::string y = folder.substr(lp + 2, 4);
            bool digits = true;
            for (char c : y) if (!std::isdigit((unsigned char)c)) digits = false;
            if (digits) {
                title = util::trim(folder.substr(0, lp));
                year = y;
            }
        }
    }
}

std::string makeId(MediaType t, const std::string& path) {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : path) { h ^= c; h *= 1099511628211ull; }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s-%llx", mediaTypeName(t), (unsigned long long)h);
    return buf;
}

std::string normKey(std::string p) {
    for (auto& c : p) if (c == '\\') c = '/';
#ifdef _WIN32
    p = util::lower(p);
#endif
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

bool samePath(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    auto aa = fs::absolute(a, ec);
    auto bb = fs::absolute(b, ec);
    return normKey(aa.string()) == normKey(bb.string());
}

bool pathUnder(const fs::path& root, const fs::path& p) {
    if (root.empty() || p.empty()) return false;
    std::error_code ec;
    auto r = normKey(fs::absolute(root, ec).string());
    auto c = normKey(fs::absolute(p, ec).string());
    if (r.empty() || c.empty()) return false;
    if (c == r) return true;
    if (c.size() < r.size()) return false;
    if (c.compare(0, r.size(), r) != 0) return false;
    return c[r.size()] == '/';
}

std::vector<std::string> allRoots() {
    auto& cfg = stack::StackConfig::get();
    std::vector<std::string> roots = cfg.allMoviesPaths();
    auto tv = cfg.allTvPaths();
    roots.insert(roots.end(), tv.begin(), tv.end());
    return roots;
}

// Title folder = direct child of a library root (e.g. Movies/Title (2020)).
fs::path resolveTitleDir(const Item& item) {
    std::error_code ec;
    fs::path start;
    if (!item.folder.empty() && fs::exists(item.folder, ec))
        start = fs::absolute(item.folder, ec);
    else if (!item.path.empty())
        start = fs::absolute(fs::path(item.path).parent_path(), ec);
    if (start.empty()) return {};

    auto roots = allRoots();
    fs::path p = start;
    for (int depth = 0; depth < 8; ++depth) {
        fs::path parent = p.parent_path();
        if (parent == p) break;
        for (auto& root : roots) {
            if (samePath(p, root)) return {};
            if (samePath(parent, root) && fs::is_directory(p, ec))
                return p;
        }
        p = parent;
    }

    if (!item.folder.empty() && fs::is_directory(item.folder, ec)) {
        for (auto& root : roots) {
            if (pathUnder(root, item.folder) && !samePath(item.folder, root))
                return fs::absolute(item.folder, ec);
        }
    }
    return {};
}

void notifyRemoved(const Item& item, const fs::path& deleted) {
    stack::onLibraryRemoved(item.path);
    if (!item.folder.empty())
        stack::onLibraryRemoved(item.folder);
    if (!deleted.empty())
        stack::onLibraryRemoved(deleted.string());
}

} // namespace

std::vector<Item> scan() {
    std::vector<Item> out;
    auto& cfg = stack::StackConfig::get();
    auto reqs = stack::listRequests();

    auto matchReq = [&](MediaType t, const std::string& title, const std::string& year,
                        bool availableOnly) -> const stack::MediaRequest* {
        const stack::MediaRequest* soft = nullptr;
        for (auto& r : reqs) {
            if (r.mediaType != t) continue;
            const bool titleOk = util::iequals(r.title, title) ||
                                 (!r.originalTitle.empty() && util::iequals(r.originalTitle, title));
            if (!titleOk) continue;
            if (!year.empty() && !r.year.empty() && r.year != year) continue;
            if (availableOnly) {
                if (r.status == stack::ReqStatus::Available) return &r;
            } else {
                if (r.status == stack::ReqStatus::Available) return &r;
                if (!soft) soft = &r;
            }
        }
        return availableOnly ? nullptr : soft;
    };

    auto attachMeta = [&](Item& it, MediaType t, const std::string& folderName) {
        if (auto* r = matchReq(t, it.title, it.year, !it.archived))
            it.tmdbId = r->tmdbId;
        for (auto& r : reqs) {
            if (r.libraryPath.empty()) continue;
            if (r.libraryPath == it.path ||
                (!folderName.empty() && r.libraryPath.find(folderName) != std::string::npos)) {
                it.tmdbId = r.tmdbId;
                it.mediaType = r.mediaType;
                if (it.title.empty()) it.title = r.title;
                if (it.year.empty()) it.year = r.year;
            }
        }
    };

    auto addFromFolder = [&](MediaType t, const fs::path& root) {
        std::error_code ec;
        if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) return;
        for (auto& ent : fs::directory_iterator(root, ec)) {
            if (ec) { ec.clear(); continue; }
            if (!ent.is_directory(ec)) {
                if (ent.is_regular_file(ec) && isVideoExt(ent.path())) {
                    Item it;
                    it.path = ent.path().string();
                    it.folder = root.string();
                    it.title = ent.path().stem().string();
                    it.mediaType = t;
                    it.id = makeId(t, it.path);
                    it.sizeBytes = (int64_t)ent.file_size(ec);
                    attachMeta(it, t, {});
                    out.push_back(std::move(it));
                }
                continue;
            }
            // Active titles: skip Archives (handled below)
            if (util::iequals(ent.path().filename().string(), "Archives")) continue;

            std::string video = biggestVideoIn(ent.path());
            if (video.empty()) continue;
            Item it;
            it.path = video;
            it.folder = ent.path().string();
            parseFolderName(ent.path().filename().string(), it.title, it.year);
            it.mediaType = t;
            it.id = makeId(t, it.path);
            std::error_code ec2;
            it.sizeBytes = (int64_t)fs::file_size(video, ec2);
            attachMeta(it, t, ent.path().filename().string());
            out.push_back(std::move(it));
        }

        // Archived ZIPs under <root>/Archives/*.zip
        fs::path arch = root / "Archives";
        if (!fs::exists(arch, ec) || !fs::is_directory(arch, ec)) return;
        for (auto& ent : fs::directory_iterator(arch, ec)) {
            if (ec) { ec.clear(); continue; }
            if (!ent.is_regular_file(ec)) continue;
            if (util::lower(ent.path().extension().string()) != ".zip") continue;
            Item it;
            it.archived = true;
            it.path = ent.path().string();
            it.folder = arch.string();
            parseFolderName(ent.path().stem().string(), it.title, it.year);
            it.mediaType = t;
            it.id = makeId(t, it.path);
            it.sizeBytes = (int64_t)ent.file_size(ec);
            attachMeta(it, t, ent.path().stem().string());
            out.push_back(std::move(it));
        }
    };

    for (auto& p : cfg.allMoviesPaths())
        addFromFolder(MediaType::Movie, p);
    for (auto& p : cfg.allTvPaths())
        addFromFolder(MediaType::TV, p);

    for (auto& r : reqs) {
        if (r.status != stack::ReqStatus::Available) continue;
        if (r.libraryPath.empty()) continue;
        std::error_code ec;
        if (!fs::exists(r.libraryPath, ec)) continue;
        bool already = false;
        for (auto& it : out) {
            if (it.path == r.libraryPath || it.folder == r.libraryPath) { already = true; break; }
        }
        if (already) continue;
        // Don't resurrect from a .zip path as an active title
        if (util::lower(fs::path(r.libraryPath).extension().string()) == ".zip") continue;
        Item it;
        it.path = fs::is_directory(r.libraryPath, ec) ? biggestVideoIn(r.libraryPath) : r.libraryPath;
        if (it.path.empty()) continue;
        it.title = r.title;
        it.year = r.year;
        it.mediaType = r.mediaType;
        it.tmdbId = r.tmdbId;
        it.folder = fs::path(it.path).parent_path().string();
        it.id = makeId(r.mediaType, it.path);
        it.sizeBytes = (int64_t)fs::file_size(it.path, ec);
        out.push_back(std::move(it));
    }

    std::sort(out.begin(), out.end(), [](const Item& a, const Item& b) {
        if (a.archived != b.archived) return !a.archived && b.archived; // active first
        return util::lower(a.title) < util::lower(b.title);
    });
    return out;
}

std::string resolvePlayable(const std::string& pathOrFolder) {
    std::error_code ec;
    fs::path p(pathOrFolder);
    if (!fs::exists(p, ec)) return pathOrFolder;
    if (fs::is_regular_file(p, ec) && isVideoExt(p)) return p.string();
    if (fs::is_directory(p, ec)) {
        auto v = biggestVideoIn(p);
        if (!v.empty()) return v;
    }
    return pathOrFolder;
}

std::string formatSize(int64_t bytes) {
    if (bytes < 0) bytes = 0;
    const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    char buf[64];
    if (u == 0) std::snprintf(buf, sizeof(buf), "%lld %s", (long long)bytes, units[u]);
    else if (v >= 100) std::snprintf(buf, sizeof(buf), "%.0f %s", v, units[u]);
    else std::snprintf(buf, sizeof(buf), "%.1f %s", v, units[u]);
    return buf;
}

bool removeItem(const Item& item, std::string* err) {
    std::error_code ec;

    // Archived ZIP — just delete the archive file.
    if (item.archived) {
        fs::path zip = item.path;
        if (zip.empty() || !fs::exists(zip, ec) ||
            util::lower(zip.extension().string()) != ".zip") {
            if (err) *err = "Nie znaleziono archiwum";
            return false;
        }
        fs::remove(zip, ec);
        if (ec) {
            if (err) *err = ec.message();
            return false;
        }
        notifyRemoved(item, zip);
        return true;
    }

    fs::path titleDir = resolveTitleDir(item);
    fs::path target = titleDir;

    if (target.empty()) {
        if (!item.path.empty() && fs::exists(item.path, ec))
            target = fs::absolute(item.path, ec);
        else if (!item.folder.empty() && fs::exists(item.folder, ec))
            target = fs::absolute(item.folder, ec);
    }
    if (target.empty() || !fs::exists(target, ec)) {
        if (err) *err = "Nie znaleziono pliku na dysku";
        return false;
    }

    for (auto& root : allRoots()) {
        if (samePath(target, root)) {
            if (err) *err = "Nie można usunąć katalogu biblioteki";
            return false;
        }
    }

    bool wasFile = fs::is_regular_file(target, ec);
    fs::path parentOfFile = wasFile ? target.parent_path() : fs::path{};

    fs::remove_all(target, ec);
    if (ec) {
        if (err) *err = ec.message();
        return false;
    }

    if (wasFile && !parentOfFile.empty()) {
        bool under = false;
        for (auto& root : allRoots())
            if (pathUnder(root, parentOfFile) && !samePath(parentOfFile, root)) under = true;
        if (under && fs::is_directory(parentOfFile, ec) && fs::is_empty(parentOfFile, ec))
            fs::remove(parentOfFile, ec);
    }

    notifyRemoved(item, titleDir.empty() ? target : titleDir);
    return true;
}

std::string archiveItem(const Item& item, std::string* err) {
    std::error_code ec;
    fs::path titleDir = resolveTitleDir(item);
    if (titleDir.empty() || !fs::is_directory(titleDir, ec)) {
        if (err) *err = "Nie znaleziono folderu tytułu";
        return {};
    }
    for (auto& root : allRoots()) {
        if (samePath(titleDir, root)) {
            if (err) *err = "Nie można archiwizować katalogu biblioteki";
            return {};
        }
    }

    fs::path archiveRoot;
    for (auto& root : allRoots()) {
        if (pathUnder(root, titleDir)) {
            archiveRoot = fs::path(root) / "Archives";
            break;
        }
    }
    if (archiveRoot.empty())
        archiveRoot = titleDir.parent_path() / "Archives";

    std::string zipName = titleDir.filename().string() + ".zip";
    for (char& c : zipName) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|')
            c = '_';
    }
    fs::path zipPath = archiveRoot / zipName;

    std::string zipErr;
    if (!zipwrite::zipDirectory(titleDir, zipPath, &zipErr)) {
        if (err) *err = zipErr.empty() ? "Kompresja nieudana" : zipErr;
        return {};
    }

    fs::remove_all(titleDir, ec);
    if (ec && err)
        *err = std::string("Archiwum OK, ale nie usunięto oryginału: ") + ec.message();

    stack::onLibraryArchived(item.path, titleDir.empty() ? item.folder : titleDir.string(),
                             zipPath.string());
    return zipPath.string();
}

std::string exportItem(const Item& item, std::string* err) {
    std::error_code ec;
    fs::path titleDir = resolveTitleDir(item);
    if (titleDir.empty() || !fs::is_directory(titleDir, ec)) {
        if (err) *err = i18n::tr("library.export_no_folder");
        return {};
    }
    for (auto& root : allRoots()) {
        if (samePath(titleDir, root)) {
            if (err) *err = i18n::tr("library.export_root_denied");
            return {};
        }
    }

    // Match request for richer metadata (imdb / original title).
    std::string originalTitle, imdbId;
    int tmdbId = item.tmdbId;
    for (auto& r : stack::listRequests()) {
        if (r.mediaType != item.mediaType) continue;
        bool hit = (tmdbId > 0 && r.tmdbId == tmdbId) ||
                   (util::iequals(r.title, item.title) &&
                    (item.year.empty() || r.year.empty() || r.year == item.year));
        if (!hit) continue;
        if (tmdbId <= 0 && r.tmdbId > 0) tmdbId = r.tmdbId;
        if (originalTitle.empty()) originalTitle = r.originalTitle;
        if (imdbId.empty()) imdbId = r.imdbId;
        break;
    }

    fs::path exportRoot;
    for (auto& root : allRoots()) {
        if (pathUnder(root, titleDir)) {
            exportRoot = fs::path(root) / "Exports";
            break;
        }
    }
    if (exportRoot.empty())
        exportRoot = titleDir.parent_path() / "Exports";

    std::string zipName = titleDir.filename().string() + ".zip";
    for (char& c : zipName) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|')
            c = '_';
    }
    fs::path zipPath = exportRoot / zipName;
    fs::create_directories(exportRoot, ec);

    json manifest{
        {"seerrExportVersion", 1},
        {"title", item.title},
        {"originalTitle", originalTitle},
        {"year", item.year},
        {"mediaType", item.mediaType == MediaType::TV ? "tv" : "movie"},
        {"tmdbId", tmdbId},
        {"imdbId", imdbId},
        {"folderName", titleDir.filename().string()}
    };

    // Write manifest outside the title folder (some libraries are read-only /
    // OneDrive-locked) and inject it into the ZIP as an extra entry.
    fs::path manifestPath = exportRoot / (zipName + ".seerr-meta.tmp");
    {
        std::ofstream mf(manifestPath, std::ios::binary | std::ios::trunc);
        if (!mf) {
            // Fallback to system temp if Exports isn't writable either.
            manifestPath = fs::temp_directory_path(ec) / ("seerr-export-" + zipName + ".json");
            mf.open(manifestPath, std::ios::binary | std::ios::trunc);
            if (!mf) {
                if (err) *err = i18n::tr("library.export_manifest_failed");
                return {};
            }
        }
        mf << manifest.dump(2);
        if (!mf) {
            mf.close();
            fs::remove(manifestPath, ec);
            if (err) *err = i18n::tr("library.export_manifest_failed");
            return {};
        }
    }

    std::vector<zipwrite::ExtraFile> extras = {
        { manifestPath, "seerr-export.json" }
    };
    std::string zipErr;
    const bool ok = zipwrite::zipDirectory(titleDir, zipPath, &zipErr, &extras);
    fs::remove(manifestPath, ec);
    if (!ok) {
        if (err) *err = zipErr.empty() ? i18n::tr("library.export_failed") : zipErr;
        return {};
    }
    return zipPath.string();
}

std::string importExportZip(const std::string& zipPath, std::string* err) {
    std::error_code ec;
    if (zipPath.empty() || !fs::exists(zipPath, ec)) {
        if (err) *err = i18n::tr("library.import_missing");
        return {};
    }

    fs::path staging = fs::temp_directory_path(ec) / ("seerr-import-" + std::to_string(
        (unsigned long long)std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(staging, ec);

    std::string unzipErr;
    if (!zipwrite::unzipToDirectory(zipPath, staging, &unzipErr)) {
        fs::remove_all(staging, ec);
        if (err) *err = unzipErr.empty() ? i18n::tr("library.import_failed") : unzipErr;
        return {};
    }

    json manifest = json::object();
    fs::path manifestPath = staging / "seerr-export.json";
    if (fs::exists(manifestPath, ec)) {
        try {
            std::ifstream mf(manifestPath.string(), std::ios::binary);
            manifest = json::parse(mf, nullptr, false);
            if (manifest.is_discarded()) manifest = json::object();
        } catch (...) { manifest = json::object(); }
    }

    std::string title = manifest.value("title", "");
    std::string year = manifest.value("year", "");
    std::string originalTitle = manifest.value("originalTitle", "");
    std::string imdbId = manifest.value("imdbId", "");
    std::string folderName = manifest.value("folderName", "");
    int tmdbId = manifest.value("tmdbId", 0);
    MediaType type = (manifest.value("mediaType", "movie") == "tv") ? MediaType::TV
                                                                    : MediaType::Movie;

    if (folderName.empty()) {
        // Fall back to zip stem or first directory inside staging.
        folderName = fs::path(zipPath).stem().string();
        for (auto& ent : fs::directory_iterator(staging, ec)) {
            if (ent.is_directory(ec) && ent.path().filename() != "." &&
                ent.path().filename().string() != "seerr-export.json") {
                // keep flat zip contents; folderName from zip stem
                break;
            }
        }
    }
    if (title.empty()) {
        parseFolderName(folderName, title, year);
    }

    auto& cfg = stack::StackConfig::get();
    fs::path destRoot = (type == MediaType::TV) ? fs::path(cfg.tvPath) : fs::path(cfg.moviesPath);
    fs::path destDir = destRoot / folderName;
    if (fs::exists(destDir, ec)) {
        // Avoid clobbering — add suffix.
        for (int i = 2; i < 50; ++i) {
            fs::path alt = destRoot / (folderName + " (" + std::to_string(i) + ")");
            if (!fs::exists(alt, ec)) { destDir = alt; break; }
        }
    }
    fs::create_directories(destRoot, ec);

    // Move extracted files into destDir (skip the manifest).
    fs::create_directories(destDir, ec);
    for (auto it = fs::recursive_directory_iterator(staging, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;
        if (it->path().filename() == "seerr-export.json") continue;
        fs::path rel = fs::relative(it->path(), staging, ec);
        if (ec) { rel = it->path().filename(); ec.clear(); }
        fs::path out = destDir / rel;
        fs::create_directories(out.parent_path(), ec);
        fs::rename(it->path(), out, ec);
        if (ec) {
            ec.clear();
            fs::copy_file(it->path(), out, fs::copy_options::overwrite_existing, ec);
        }
    }
    fs::remove_all(staging, ec);

    std::string video = biggestVideoIn(destDir);
    if (video.empty()) {
        if (err) *err = i18n::tr("library.import_no_video");
        return {};
    }

    stack::onLibraryImported(type, tmdbId, title, year, imdbId, originalTitle, video);
    return video;
}

} // namespace library
