#pragma once
#include <string>
#include <vector>

// UI translations: English is the default (embedded). Other languages load from
// locales/<code>.json next to the exe, under APP_LOCALE_DIR, or in app data.
namespace i18n {

void init();
void setLanguage(const std::string& code); // "en", "pl", …
const std::string& language();
std::vector<std::string> availableLanguages();

// Lookup by key. Falls back to English default, then to the key itself.
const char* tr(const char* key);

} // namespace i18n
