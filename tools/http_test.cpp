#include <windows.h>
#include <winhttp.h>
#include <cstdio>
#include <string>

#pragma comment(lib, "winhttp.lib")

static int tryUrl(const std::wstring& url, const wchar_t* mode) {
    HINTERNET s = nullptr, c = nullptr, r = nullptr;
    // parse
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = L"", path[1024] = L"";
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 1023;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { printf("%S: crack failed %lu\n", mode, GetLastError()); return 1; }

    DWORD access = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
    if (wcscmp(mode, L"direct") == 0) access = WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;

    s = WinHttpOpen(L"SeerrCpp/1.0", access, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { printf("%S: open failed %lu\n", mode, GetLastError()); return 1; }
    WinHttpSetTimeouts(s, 5000, 10000, 15000, 30000);
    INTERNET_PORT port = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    c = WinHttpConnect(s, host, port, 0);
    if (!c) { printf("%S: connect failed %lu\n", mode, GetLastError()); WinHttpCloseHandle(s); return 1; }
    r = WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                           (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
    if (!r) { printf("%S: openreq failed %lu\n", mode, GetLastError()); goto end; }
    if (!WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(r, nullptr)) {
        printf("%S: send/recv failed %lu\n", mode, GetLastError()); goto end;
    }
    {
        DWORD code = 0, sz = sizeof(code);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
        DWORD avail = 0, total = 0;
        WinHttpQueryDataAvailable(r, &avail);
        printf("%S: OK http=%lu avail=%lu\n", mode, code, avail);
    }
end:
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return 0;
}

int main() {
    std::wstring api = L"https://api.themoviedb.org/3/trending/movie/week?api_key=431a8708161bcd1f1fbe7536137e61ed";
    std::wstring img = L"https://image.tmdb.org/t/p/w300_and_h450_face/8deNRbzUW7Zh5RGiFIdwEbVtNjC.jpg";
    printf("-- api automatic --\n"); tryUrl(api, L"automatic");
    printf("-- api direct --\n");    tryUrl(api, L"direct");
    printf("-- img automatic --\n"); tryUrl(img, L"automatic");
    printf("-- img direct --\n");    tryUrl(img, L"direct");
    return 0;
}
