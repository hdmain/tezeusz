#include "library.hpp"
#include "stack.hpp"
#include "util.hpp"
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
    // "Title (2024)"
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
                // loose file in root
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
            if (auto* r = matchReq(t, it.title, it.year)) {
                it.tmdbId = r->tmdbId;
                // poster not on request — leave empty; UI can still show placeholder
            }
            // also match by libraryPath
            for (auto& r : reqs) {
                if (r.libraryPath.empty()) continue;
                if (r.libraryPath == video || r.libraryPath.find(ent.path().filename().string()) != std::string::npos) {
                    it.tmdbId = r.tmdbId;
                    it.mediaType = r.mediaType;
                    if (it.title.empty()) it.title = r.title;
                    if (it.year.empty()) it.year = r.year;
                }
            }
            out.push_back(std::move(it));
        }
    };

    addFromFolder(MediaType::Movie, cfg.moviesPath);
    addFromFolder(MediaType::TV, cfg.tvPath);

    // Available requests whose files aren't under scanned folders
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
    auto& cfg = stack::StackConfig::get();

    auto underRoot = [&](const fs::path& root, const fs::path& p) -> bool {
        if (root.empty()) return false;
        auto r = fs::weakly_canonical(root, ec);
        auto c = fs::weakly_canonical(p, ec);
        if (ec || r.empty() || c.empty()) return false;
        auto rs = r.string(), cs = c.string();
        if (cs.size() < rs.size()) return false;
        if (cs.compare(0, rs.size(), rs) != 0) return false;
        return cs.size() == rs.size() || cs[rs.size()] == '\\' || cs[rs.size()] == '/';
    };

    // Prefer deleting the title folder (keeps library clean)
    fs::path target;
    if (!item.folder.empty()) {
        fs::path folder(item.folder);
        bool isTitleFolder =
            underRoot(cfg.moviesPath, folder) || underRoot(cfg.tvPath, folder);
        // Don't delete the library root itself
        auto moviesCanon = fs::weakly_canonical(cfg.moviesPath, ec);
        auto tvCanon = fs::weakly_canonical(cfg.tvPath, ec);
        auto folderCanon = fs::weakly_canonical(folder, ec);
        if (isTitleFolder && folderCanon != moviesCanon && folderCanon != tvCanon &&
            fs::is_directory(folder, ec)) {
            target = folder;
        }
    }
    if (target.empty()) {
        if (!item.path.empty() && fs::exists(item.path, ec))
            target = item.path;
        else if (!item.folder.empty() && fs::exists(item.folder, ec))
            target = item.folder;
    }
    if (target.empty()) {
        if (err) *err = "Nie znaleziono pliku";
        return false;
    }

    fs::remove_all(target, ec);
    if (ec) {
        if (err) *err = ec.message();
        return false;
    }

    stack::onLibraryRemoved(item.path);
    if (!item.folder.empty() && item.folder != item.path)
        stack::onLibraryRemoved(item.folder);
    return true;
}

} // namespace library
