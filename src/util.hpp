#pragma once
#include <string>
#include <vector>
#include <map>
#include <optional>

namespace util {

std::string urlEncode(const std::string& s);
std::string buildQuery(const std::map<std::string, std::string>& params);
std::vector<std::string> split(const std::string& s, char sep);
std::string trim(const std::string& s);
bool iequals(const std::string& a, const std::string& b);
std::string yearOf(const std::string& date);           // "2024-05-10" -> "2024"
std::string formatDate(const std::string& iso);        // locale-aware date from ISO
inline std::string formatDatePl(const std::string& iso) { return formatDate(iso); }
std::string runtimeStr(int minutes);                   // 142 -> "2h 22m"
std::string moneyStr(double v);                        // 1234567 -> "$1,234,567"
std::string numberK(double v);                         // 45600 -> "45.6k"
bool containsCI(const std::string& hay, const std::string& needle);
std::string lower(std::string s);
std::string appDataPath(const std::string& file);
std::string readFile(const std::string& path);
bool writeFile(const std::string& path, const std::string& data);
std::string exeDir();

// Shared data root (fonts, icons, images). Resolves installed /usr/share/seerr
// as well as the compile-time APP_ASSET_DIR used for local builds.
std::string assetDir();
// Translation JSON directory (…/locales).
std::string localeDir();

} // namespace util
