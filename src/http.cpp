#include "http.hpp"
#include "util.hpp"
#include <thread>
#include <chrono>
#include <mutex>
#include <cstring>
#include <future>
#include <functional>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#else
#include <curl/curl.h>
#endif

namespace http {

// NOTE: no global mutex here — WinHTTP sessions are thread-safe (each request
// gets its own connect/request handles) and the curl path uses per-call
// handles. Serializing everything here used to starve the UI while the subs
// worker retried a slow endpoint (freeze on tab switches).

#ifdef _WIN32

static HINTERNET sharedSession() {
    static HINTERNET s = [] {
        HINTERNET sess = WinHttpOpen(L"SeerrCpp/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!sess)
            sess = WinHttpOpen(L"SeerrCpp/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (sess) WinHttpSetTimeouts(sess, 5000, 10000, 15000, 30000);
        return sess;
    }();
    return s;
}

static HINTERNET longSession(int timeoutSec) {
    // Dedicated session for large downloads (updates).
    static thread_local HINTERNET s = nullptr;
    static thread_local int lastT = 0;
    if (s && lastT == timeoutSec) return s;
    if (s) { WinHttpCloseHandle(s); s = nullptr; }
    s = WinHttpOpen(L"SeerrCpp/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s)
        s = WinHttpOpen(L"SeerrCpp/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    int t = timeoutSec > 0 ? timeoutSec * 1000 : 180000;
    if (s) WinHttpSetTimeouts(s, 10000, 30000, 60000, t);
    lastT = timeoutSec;
    return s;
}

static void splitUrl(const std::string& url, std::wstring& host, std::wstring& path, INTERNET_PORT& port) {
    std::string u = url;
    port = INTERNET_DEFAULT_HTTPS_PORT;
    if (u.rfind("https://", 0) == 0) u = u.substr(8);
    else if (u.rfind("http://", 0) == 0) { u = u.substr(7); port = INTERNET_DEFAULT_HTTP_PORT; }

    size_t slash = u.find('/');
    std::string hostA = slash == std::string::npos ? u : u.substr(0, slash);
    std::string pathA = slash == std::string::npos ? "/" : u.substr(slash);
    size_t colon = hostA.find(':');
    if (colon != std::string::npos) {
        port = (INTERNET_PORT)atoi(hostA.c_str() + colon + 1);
        hostA = hostA.substr(0, colon);
    }
    host.assign(hostA.begin(), hostA.end());
    path.assign(pathA.begin(), pathA.end());
}

static HttpResponse getOnce(const std::string& url, const std::string& accept, const std::string& extraHeaders) {
    HttpResponse r;
    HINTERNET session = sharedSession(), connect = nullptr, request = nullptr;
    auto cleanup = [&]() {
        if (request) WinHttpCloseHandle(request);
        if (connect) WinHttpCloseHandle(connect);
    };

    std::wstring host, path;
    INTERNET_PORT port;
    splitUrl(url, host, path, port);

    if (!session) { r.err = "WinHttpOpen failed"; return r; }

    connect = WinHttpConnect(session, host.c_str(), port, 0);
    if (!connect) { r.err = "WinHttpConnect failed: " + std::to_string(GetLastError()); cleanup(); return r; }

    request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 port == INTERNET_DEFAULT_HTTPS_PORT ? WINHTTP_FLAG_SECURE : 0);
    if (!request) { r.err = "WinHttpOpenRequest failed: " + std::to_string(GetLastError()); cleanup(); return r; }

    std::wstring headers = L"Accept: " + std::wstring(accept.begin(), accept.end()) + L"\r\n";
    if (!extraHeaders.empty())
        headers += std::wstring(extraHeaders.begin(), extraHeaders.end());
    if (!WinHttpSendRequest(request, headers.c_str(), (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        r.err = "request failed: " + std::to_string(GetLastError());
        cleanup();
        return r;
    }

    DWORD statusCode = 0, size = sizeof(statusCode);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size, WINHTTP_NO_HEADER_INDEX);
    r.status = (int)statusCode;

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0) break;
        std::string chunk(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), avail, &read) || read == 0) break;
        r.body.append(chunk.data(), read);
    }
    cleanup();
    return r;
}

static HttpResponse postOnce(const std::string& url, const std::string& body,
                             const std::string& contentType, const std::string& extraHeaders) {
    HttpResponse r;
    HINTERNET session = sharedSession(), connect = nullptr, request = nullptr;
    auto cleanup = [&]() {
        if (request) WinHttpCloseHandle(request);
        if (connect) WinHttpCloseHandle(connect);
    };

    std::wstring host, path;
    INTERNET_PORT port;
    splitUrl(url, host, path, port);
    if (!session) { r.err = "WinHttpOpen failed"; return r; }

    connect = WinHttpConnect(session, host.c_str(), port, 0);
    if (!connect) { r.err = "WinHttpConnect failed: " + std::to_string(GetLastError()); cleanup(); return r; }

    request = WinHttpOpenRequest(connect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 port == INTERNET_DEFAULT_HTTPS_PORT ? WINHTTP_FLAG_SECURE : 0);
    if (!request) { r.err = "WinHttpOpenRequest failed: " + std::to_string(GetLastError()); cleanup(); return r; }

    std::wstring headers = L"Content-Type: " + std::wstring(contentType.begin(), contentType.end()) + L"\r\n";
    if (!extraHeaders.empty())
        headers += std::wstring(extraHeaders.begin(), extraHeaders.end());

    void* data = body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data();
    DWORD dataLen = (DWORD)body.size();
    if (!WinHttpSendRequest(request, headers.c_str(), (DWORD)-1, data, dataLen, dataLen, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        r.err = "post failed: " + std::to_string(GetLastError());
        cleanup();
        return r;
    }

    DWORD statusCode = 0, size = sizeof(statusCode);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size, WINHTTP_NO_HEADER_INDEX);
    r.status = (int)statusCode;

    DWORD cookieSize = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_SET_COOKIE, WINHTTP_HEADER_NAME_BY_INDEX,
                        WINHTTP_NO_OUTPUT_BUFFER, &cookieSize, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && cookieSize > 0) {
        std::wstring cookie((cookieSize / sizeof(wchar_t)) + 1, 0);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_SET_COOKIE, WINHTTP_HEADER_NAME_BY_INDEX,
                                cookie.data(), &cookieSize, WINHTTP_NO_HEADER_INDEX)) {
            r.setCookie.assign(cookie.begin(), cookie.end());
            while (!r.setCookie.empty() && r.setCookie.back() == '\0') r.setCookie.pop_back();
        }
    }

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0) break;
        std::string chunk(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), avail, &read) || read == 0) break;
        r.body.append(chunk.data(), read);
    }
    cleanup();
    return r;
}

#else // curl

static size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

static size_t headerCb(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* r = static_cast<HttpResponse*>(userdata);
    size_t total = size * nitems;
    std::string line(buffer, total);
    if (line.rfind("Set-Cookie:", 0) == 0 || line.rfind("set-cookie:", 0) == 0) {
        auto v = util::trim(line.substr(line.find(':') + 1));
        if (!r->setCookie.empty()) r->setCookie += "; ";
        r->setCookie += v;
    }
    return total;
}

static void ensureCurl() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

static HttpResponse getOnce(const std::string& url, const std::string& accept, const std::string& extraHeaders) {
    ensureCurl();
    HttpResponse r;
    CURL* curl = curl_easy_init();
    if (!curl) { r.err = "curl_easy_init failed"; return r; }
    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, ("Accept: " + accept).c_str());
    if (!extraHeaders.empty()) {
        // extraHeaders may contain \r\n-separated lines
        for (auto& line : util::split(extraHeaders, '\n')) {
            auto t = util::trim(line);
            if (!t.empty() && t.back() == '\r') t.pop_back();
            if (!t.empty()) hdrs = curl_slist_append(hdrs, t.c_str());
        }
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &r);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "SeerrCpp/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    CURLcode code = curl_easy_perform(curl);
    if (code != CURLE_OK) r.err = curl_easy_strerror(code);
    else {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        r.status = (int)status;
    }
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return r;
}

static HttpResponse postOnce(const std::string& url, const std::string& body,
                             const std::string& contentType, const std::string& extraHeaders) {
    ensureCurl();
    HttpResponse r;
    CURL* curl = curl_easy_init();
    if (!curl) { r.err = "curl_easy_init failed"; return r; }
    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, ("Content-Type: " + contentType).c_str());
    if (!extraHeaders.empty()) {
        for (auto& line : util::split(extraHeaders, '\n')) {
            auto t = util::trim(line);
            if (!t.empty() && t.back() == '\r') t.pop_back();
            if (!t.empty()) hdrs = curl_slist_append(hdrs, t.c_str());
        }
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &r);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "SeerrCpp/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    CURLcode code = curl_easy_perform(curl);
    if (code != CURLE_OK) r.err = curl_easy_strerror(code);
    else {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        r.status = (int)status;
    }
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return r;
}

#endif

HttpResponse get(const std::string& url, const std::string& accept, const std::string& extraHeaders) {
    HttpResponse r;
    for (int attempt = 0; attempt < 3; attempt++) {
        r = getOnce(url, accept, extraHeaders);
        if (r.err.empty() && r.status != 429 && r.status < 500) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(300 * (attempt + 1)));
    }
    return r;
}

std::vector<uint8_t> getBinary(const std::string& url, std::string* err) {
    HttpResponse r = get(url, "image/*,*/*");
    if (err && !r.err.empty()) *err = r.err;
    if (!r.ok()) { if (err && r.err.empty()) *err = "status " + std::to_string(r.status); return {}; }
    std::vector<uint8_t> out(r.body.size());
    if (!out.empty()) std::memcpy(out.data(), r.body.data(), out.size());
    return out;
}

std::vector<uint8_t> getBinaryLong(const std::string& url, std::string* err, int timeoutSec) {
#ifdef _WIN32
    // Follow redirects (GitHub release assets → objects.githubusercontent.com).
    std::function<std::vector<uint8_t>(const std::string&, int)> downloadOnce =
        [&](const std::string& u, int depth) -> std::vector<uint8_t> {
        if (depth > 8) {
            if (err) *err = "too many redirects";
            return {};
        }
        HINTERNET session = longSession(timeoutSec), connect = nullptr, request = nullptr;
        auto cleanup = [&]() {
            if (request) WinHttpCloseHandle(request);
            if (connect) WinHttpCloseHandle(connect);
        };
        std::wstring host, path;
        INTERNET_PORT port;
        splitUrl(u, host, path, port);
        if (!session) { if (err) *err = "WinHttpOpen failed"; return {}; }
        connect = WinHttpConnect(session, host.c_str(), port, 0);
        if (!connect) { if (err) *err = "WinHttpConnect failed"; cleanup(); return {}; }
        DWORD flags = (port == INTERNET_DEFAULT_HTTPS_PORT) ? WINHTTP_FLAG_SECURE : 0;
        request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!request) { if (err) *err = "WinHttpOpenRequest failed"; cleanup(); return {}; }

        // Disable auto-redirect so we can rebuild the absolute URL ourselves
        // (WinHTTP sometimes drops the body on cross-host 302 to GitHub CDN).
        DWORD redir = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &redir, sizeof(redir));

        std::wstring headers = L"Accept: */*\r\n";
        if (!WinHttpSendRequest(request, headers.c_str(), (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request, nullptr)) {
            if (err) *err = "request failed: " + std::to_string(GetLastError());
            cleanup();
            return {};
        }
        DWORD statusCode = 0, size = sizeof(statusCode);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size, WINHTTP_NO_HEADER_INDEX);

        if (statusCode == 301 || statusCode == 302 || statusCode == 303 || statusCode == 307 || statusCode == 308) {
            DWORD locSize = 0;
            WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                WINHTTP_NO_OUTPUT_BUFFER, &locSize, WINHTTP_NO_HEADER_INDEX);
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || locSize == 0) {
                if (err) *err = "redirect without Location";
                cleanup();
                return {};
            }
            std::wstring loc(locSize / sizeof(wchar_t) + 1, 0);
            if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                     loc.data(), &locSize, WINHTTP_NO_HEADER_INDEX)) {
                if (err) *err = "redirect Location read failed";
                cleanup();
                return {};
            }
            while (!loc.empty() && loc.back() == L'\0') loc.pop_back();
            std::string next(loc.begin(), loc.end());
            cleanup();
            if (next.rfind("http://", 0) != 0 && next.rfind("https://", 0) != 0) {
                // relative Location
                size_t scheme = u.find("://");
                size_t hostStart = scheme == std::string::npos ? 0 : scheme + 3;
                size_t pathStart = u.find('/', hostStart);
                std::string origin = pathStart == std::string::npos ? u : u.substr(0, pathStart);
                if (!next.empty() && next[0] == '/') next = origin + next;
                else next = origin + "/" + next;
            }
            return downloadOnce(next, depth + 1);
        }

        std::string body;
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0) break;
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), avail, &read) || read == 0) break;
            body.append(chunk.data(), read);
        }
        cleanup();
        if (statusCode < 200 || statusCode >= 300) {
            if (err) *err = "status " + std::to_string((int)statusCode);
            return {};
        }
        if (err) err->clear();
        std::vector<uint8_t> out(body.size());
        if (!out.empty()) std::memcpy(out.data(), body.data(), out.size());
        return out;
    };
    return downloadOnce(url, 0);
#else
    ensureCurl();
    HttpResponse r;
    CURL* curl = curl_easy_init();
    if (!curl) { if (err) *err = "curl_easy_init failed"; return {}; }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "SeerrCpp/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)(timeoutSec > 0 ? timeoutSec : 180));
    CURLcode code = curl_easy_perform(curl);
    if (code != CURLE_OK) {
        if (err) *err = curl_easy_strerror(code);
        curl_easy_cleanup(curl);
        return {};
    }
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (status < 200 || status >= 300) {
        if (err) *err = "status " + std::to_string((int)status);
        return {};
    }
    std::vector<uint8_t> out(r.body.size());
    if (!out.empty()) std::memcpy(out.data(), r.body.data(), out.size());
    return out;
#endif
}

std::future<HttpResponse> getAsync(const std::string& url, const std::string& accept) {
    return std::async(std::launch::async, [url, accept]() { return get(url, accept); });
}

HttpResponse post(const std::string& url, const std::string& body,
                  const std::string& contentType, const std::string& extraHeaders) {
    HttpResponse r;
    for (int attempt = 0; attempt < 3; attempt++) {
        r = postOnce(url, body, contentType, extraHeaders);
        if (r.err.empty() && r.status != 429 && r.status < 500) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(300 * (attempt + 1)));
    }
    return r;
}

} // namespace http
