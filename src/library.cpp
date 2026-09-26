#include "library.hpp"
#include "stack.hpp"
#include "util.hpp"
#include "zipwrite.hpp"
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cstdio>

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

    auto matchReq = [&](MediaType t, const std::string& title, const std::string& year) -> const stack::MediaRequest* {
        for (auto& r : reqs) {
            if (r.mediaType != t) continue;
            if (r.status != stack::ReqStatus::Available) continue;
            if (util::iequals(r.title, title) && (year.empty() || r.year == year))
                return &r;
        }
        return nullptr;
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
                    out.push_back(std::move(it));
                }
                continue;
            }
            // Skip Archives folder inside library roots
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
            if (auto* r = matchReq(t, it.title, it.year))
                it.tmdbId = r->tmdbId;
            for (auto& r : reqs) {
                if (r.libraryPath.empty()) continue;
                if (r.libraryPath == video ||
                    r.libraryPath.find(ent.path().filename().string()) != std::string::npos) {
                    it.tmdbId = r.tmdbId;
                    it.mediaType = r.mediaType;
                    if (it.title.empty()) it.title = r.title;
                    if (it.year.empty()) it.year = r.year;
                }
            }
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

    notifyRemoved(item, titleDir);
    return zipPath.string();
}

} // namespace library
