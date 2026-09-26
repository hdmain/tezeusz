#include "util.hpp"
#include "i18n.hpp"
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <unistd.h>
#include <cstdlib>
#endif
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace util {

std::string urlEncode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else {
            static const char* hex = "0123456789ABCDEF";
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string buildQuery(const std::map<std::string, std::string>& params) {
    std::string q;
    for (auto& [k, v] : params) {
        if (v.empty()) continue;
        if (!q.empty()) q += '&';
        q += urlEncode(k);
        q += '=';
        q += urlEncode(v);
    }
    return q;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool iequals(const std::string& a, const std::string& b) {
    return lower(a) == lower(b);
}

bool containsCI(const std::string& hay, const std::string& needle) {
    return lower(hay).find(lower(needle)) != std::string::npos;
}

std::string yearOf(const std::string& date) {
    if (date.size() >= 4 && isdigit((unsigned char)date[0])) return date.substr(0, 4);
    return "";
}

std::string formatDate(const std::string& iso) {
    // 2024-05-10
    if (iso.size() < 10 || iso[4] != '-') return iso;
    int m = atoi(iso.substr(5, 2).c_str());
    int d = atoi(iso.substr(8, 2).c_str());
    if (m < 1 || m > 12) return iso;
    char key[16];
    std::snprintf(key, sizeof(key), "month.%d", m);
    const char* month = i18n::tr(key);
    std::ostringstream os;
    if (i18n::language() == "pl")
        os << d << " " << month << " " << iso.substr(0, 4);
    else
        os << month << " " << d << ", " << iso.substr(0, 4);
    return os.str();
}

std::string runtimeStr(int minutes) {
    if (minutes <= 0) return "";
    std::ostringstream os;
    if (minutes >= 60) os << (minutes / 60) << "h ";
    os << (minutes % 60) << "m";
    return os.str();
}

std::string moneyStr(double v) {
    std::ostringstream raw;
    raw << (long long)v;
    std::string s = raw.str();
    std::string out;
    int cnt = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
        if (cnt && cnt % 3 == 0) out += ',';
        out += *it;
        cnt++;
    }
    reverse(out.begin(), out.end());
    return "$" + out;
}

std::string numberK(double v) {
    std::ostringstream os;
    if (v >= 1000000) os << (long long)(v / 1000000) << "M";
    else if (v >= 1000) os << (v >= 10000 ? (long long)(v / 1000) : (long long)(v / 100) / 10.0 * 10) ;
    else os << (long long)v;
    if (v >= 1000 && v < 1000000) {
        std::ostringstream o2; o2.precision(1); o2 << std::fixed << (v / 1000.0) << "k";
        return o2.str();
    }
    return os.str();
}

std::string appDataPath(const std::string& file) {
#ifdef _WIN32
    PWSTR dir = nullptr;
    std::string base = ".";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &dir))) {
        char buf[1024];
        WideCharToMultiByte(CP_UTF8, 0, dir, -1, buf, sizeof(buf), nullptr, nullptr);
        CoTaskMemFree(dir);
        base = buf;
    }
    std::string appDir = base + "\\SeerrCpp";
    CreateDirectoryA(appDir.c_str(), nullptr);
    return appDir + "\\" + file;
#else
    const char* home = std::getenv("HOME");
    std::string base = home ? std::string(home) + "/.local/share" : ".";
    std::string appDir = base + "/SeerrCpp";
    std::error_code ec;
    std::filesystem::create_directories(appDir, ec);
    return appDir + "/" + file;
#endif
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool writeFile(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << data;
    return true;
}

bool isValidUtf8(const std::string& s) {
    const unsigned char* p = (const unsigned char*)s.data();
    size_t n = s.size(), i = 0;
    while (i < n) {
        unsigned char c = p[i];
        if (c <= 0x7F) { ++i; continue; }
        int need = 0;
        if ((c & 0xE0) == 0xC0) need = 1;
        else if ((c & 0xF0) == 0xE0) need = 2;
        else if ((c & 0xF8) == 0xF0) need = 3;
        else return false;
        if (i + need >= n) return false;
        for (int k = 1; k <= need; ++k)
            if ((p[i + k] & 0xC0) != 0x80) return false;
        i += 1 + need;
    }
    return true;
}

// Windows-1250 (Central European) → Unicode code points for 0x80..0xFF.
static const uint16_t kCp1250[128] = {
    0x20AC, 0x0081, 0x201A, 0x0083, 0x201E, 0x2026, 0x2020, 0x2021,
    0x0088, 0x2030, 0x0160, 0x2039, 0x015A, 0x0164, 0x017D, 0x0179,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x0098, 0x2122, 0x0161, 0x203A, 0x015B, 0x0165, 0x017E, 0x017A,
    0x00A0, 0x02C7, 0x02D8, 0x0141, 0x00A4, 0x0104, 0x00A6, 0x00A7,
    0x00A8, 0x00A9, 0x015E, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x017B,
    0x00B0, 0x00B1, 0x02DB, 0x0142, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
    0x00B8, 0x0105, 0x015F, 0x00BB, 0x013D, 0x02DD, 0x013E, 0x017C,
    0x0154, 0x00C1, 0x00C2, 0x0102, 0x00C4, 0x0139, 0x0106, 0x00C7,
    0x010C, 0x00C9, 0x0118, 0x00CB, 0x011A, 0x00CD, 0x00CE, 0x010E,
    0x0110, 0x0143, 0x0147, 0x00D3, 0x00D4, 0x0150, 0x00D6, 0x00D7,
    0x0158, 0x016E, 0x00DA, 0x0170, 0x00DC, 0x00DD, 0x0162, 0x00DF,
    0x0155, 0x00E1, 0x00E2, 0x0103, 0x00E4, 0x013A, 0x0107, 0x00E7,
    0x010D, 0x00E9, 0x0119, 0x00EB, 0x011B, 0x00ED, 0x00EE, 0x010F,
    0x0111, 0x0144, 0x0148, 0x00F3, 0x00F4, 0x0151, 0x00F6, 0x00F7,
    0x0159, 0x016F, 0x00FA, 0x0171, 0x00FC, 0x00FD, 0x0163, 0x02D9,
};

static void appendUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) out.push_back((char)cp);
    else if (cp <= 0x7FF) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

static std::string fromCp1250(const std::string& bytes) {
    std::string out;
    out.reserve(bytes.size() + bytes.size() / 4);
    for (unsigned char c : bytes) {
        if (c < 0x80) out.push_back((char)c);
        else appendUtf8(out, kCp1250[c - 0x80]);
    }
    return out;
}

std::string toUtf8(const std::string& bytes) {
    if (bytes.empty()) return bytes;
    // Strip UTF-8 BOM if present
    std::string s = bytes;
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF)
        s.erase(0, 3);
    if (isValidUtf8(s)) return s;
    // Legacy Polish SRTs (NapiProjekt etc.) are Windows-1250.
    return fromCp1250(s);
}

bool ensureUtf8File(const std::string& path) {
    std::string raw = readFile(path);
    if (raw.empty()) return false;
    if (isValidUtf8(raw.size() >= 3 && (unsigned char)raw[0] == 0xEF ? raw.substr(3) : raw))
        return false;
    std::string utf = toUtf8(raw);
    if (utf == raw) return false;
    return writeFile(path, utf);
}

std::string exeDir() {
#ifdef _WIN32
    char buf[2048];
    GetModuleFileNameA(nullptr, buf, sizeof(buf));
    std::string p = buf;
    size_t s = p.find_last_of("\\/");
    return s == std::string::npos ? "." : p.substr(0, s);
#else
    char buf[2048];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n < 0) return ".";
    buf[n] = 0;
    std::string p = buf;
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? "." : p.substr(0, s);
#endif
}

namespace {

std::string normalizeDir(const std::string& p) {
    std::error_code ec;
    auto c = std::filesystem::weakly_canonical(p, ec);
    if (!ec) return c.string();
    return p;
}

bool looksLikeAssetRoot(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    return fs::exists(dir + "/fonts/Inter-Regular.ttf", ec)
        || fs::exists(dir + "/logo_full.png", ec);
}

bool looksLikeLocaleRoot(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    return fs::exists(dir + "/en.json", ec) || fs::exists(dir + "/pl.json", ec);
}

} // namespace

std::string assetDir() {
    static std::string cached;
    if (!cached.empty()) return cached;

    std::string exe = exeDir();
    std::vector<std::string> cands = {
        exe + "/../share/seerr",
        exe + "/share/seerr",
        "/usr/share/seerr",
        "/usr/local/share/seerr",
#ifdef APP_ASSET_DIR
        APP_ASSET_DIR,
#endif
        exe,
    };
    for (auto& c : cands) {
        std::string n = normalizeDir(c);
        if (looksLikeAssetRoot(n)) {
            cached = n;
            return cached;
        }
    }
#ifdef APP_ASSET_DIR
    cached = APP_ASSET_DIR;
#else
    cached = ".";
#endif
    return cached;
}

std::string localeDir() {
    static std::string cached;
    if (!cached.empty()) return cached;

    std::string assets = assetDir();
    std::string exe = exeDir();
    std::vector<std::string> cands = {
        assets + "/locales",
        exe + "/../share/seerr/locales",
        exe + "/locales",
        "/usr/share/seerr/locales",
        "/usr/local/share/seerr/locales",
#ifdef APP_LOCALE_DIR
        APP_LOCALE_DIR,
#endif
    };
    for (auto& c : cands) {
        std::string n = normalizeDir(c);
        if (looksLikeLocaleRoot(n)) {
            cached = n;
            return cached;
        }
    }
#ifdef APP_LOCALE_DIR
    cached = APP_LOCALE_DIR;
#else
    cached = assets + "/locales";
#endif
    return cached;
}

std::string libvlcDir() {
#ifdef _WIN32
    static std::string cached;
    static bool once = false;
    if (once) return cached;
    once = true;

    auto hasDll = [](const std::string& dir) {
        namespace fs = std::filesystem;
        std::error_code ec;
        return fs::exists(dir + "\\libvlc.dll", ec) || fs::exists(dir + "/libvlc.dll", ec);
    };

    std::string exe = exeDir();
    std::string assets = assetDir();
    std::vector<std::string> cands = {
        exe + "\\libvlc",
        exe + "/libvlc",
        assets + "\\libvlc",
        assets + "/libvlc",
#ifdef APP_ASSET_DIR
        std::string(APP_ASSET_DIR) + "\\libvlc",
#endif
        "C:\\Program Files\\VideoLAN\\VLC",
        "C:\\Program Files (x86)\\VideoLAN\\VLC",
    };
    // Also accept VLC laid flat next to the exe (some portable layouts).
    cands.insert(cands.begin(), exe);

    for (auto& c : cands) {
        if (hasDll(c)) {
            cached = normalizeDir(c);
            return cached;
        }
    }
    return cached; // empty
#else
    return {};
#endif
}

DiskSpace diskSpace(const std::string& path) {
    DiskSpace out;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p = path.empty() ? fs::current_path(ec) : fs::path(path);
    // Walk up to an existing ancestor so space() works for not-yet-created folders.
    while (!p.empty() && !fs::exists(p, ec)) {
        auto parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    if (p.empty() || !fs::exists(p, ec)) {
#ifdef _WIN32
        if (!path.empty() && path.size() >= 2 && path[1] == ':')
            p = fs::path(path.substr(0, 2) + "\\");
#else
        p = "/";
#endif
    }
    auto si = fs::space(p, ec);
    if (ec) return out;
    out.capacity = si.capacity;
    out.free = si.free;
    out.available = si.available;
    out.ok = out.capacity > 0;
    return out;
}

std::vector<VolumeInfo> listVolumes() {
    std::vector<VolumeInfo> out;
#ifdef _WIN32
    char buf[512];
    DWORD n = GetLogicalDriveStringsA(sizeof(buf) - 1, buf);
    if (n == 0 || n >= sizeof(buf)) return out;
    for (char* p = buf; *p; p += std::strlen(p) + 1) {
        UINT type = GetDriveTypeA(p);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        VolumeInfo v;
        v.root = p;
        v.label = std::string(p, 2); // "C:"
        if (type == DRIVE_REMOVABLE) v.label += " (USB)";
        v.space = diskSpace(v.root);
        if (v.space.ok) out.push_back(std::move(v));
    }
#else
    auto tryAdd = [&](const std::string& root, const std::string& label) {
        for (auto& e : out)
            if (e.root == root) return;
        auto sp = diskSpace(root);
        if (!sp.ok || sp.capacity < (uintmax_t)1 * 1024 * 1024 * 1024) return; // skip tiny
        VolumeInfo v;
        v.root = root;
        v.label = label.empty() ? root : label;
        v.space = sp;
        out.push_back(std::move(v));
    };
    tryAdd("/", "/");
    std::ifstream mounts("/proc/mounts");
    std::string line;
    while (std::getline(mounts, line)) {
        std::istringstream iss(line);
        std::string dev, mnt, type;
        if (!(iss >> dev >> mnt >> type)) continue;
        if (mnt == "/" || mnt == "/boot" || mnt == "/boot/efi" || mnt == "/snap" ||
            mnt.rfind("/snap/", 0) == 0 || mnt.rfind("/run/", 0) == 0 ||
            mnt.rfind("/sys", 0) == 0 || mnt.rfind("/proc", 0) == 0 ||
            mnt.rfind("/dev", 0) == 0)
            continue;
        if (type != "ext4" && type != "ext3" && type != "xfs" && type != "btrfs" &&
            type != "ntfs" && type != "fuseblk" && type != "vfat" && type != "exfat" &&
            type != "zfs")
            continue;
        if (dev.rfind("/dev/", 0) != 0 && type != "fuseblk" && type != "zfs") continue;
        tryAdd(mnt, mnt);
    }
#endif
    return out;
}

} // namespace util
