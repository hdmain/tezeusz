#include "util.hpp"
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <unistd.h>
#include <cstdlib>
#include <filesystem>
#endif
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cmath>

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

static const char* MONTHS_PL[] = { "stycznia","lutego","marca","kwietnia","maja","czerwca",
                                   "lipca","sierpnia","września","października","listopada","grudnia" };

std::string formatDatePl(const std::string& iso) {
    // 2024-05-10
    if (iso.size() < 10 || iso[4] != '-') return iso;
    int m = atoi(iso.substr(5, 2).c_str());
    int d = atoi(iso.substr(8, 2).c_str());
    if (m < 1 || m > 12) return iso;
    std::ostringstream os;
    os << d << " " << MONTHS_PL[m - 1] << " " << iso.substr(0, 4);
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

} // namespace util
