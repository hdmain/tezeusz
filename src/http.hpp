#pragma once
#include <string>
#include <future>
#include <vector>
#include <cstdint>

struct HttpResponse {
    int status = 0;
    std::string body;
    std::string err;
    std::string setCookie; // raw Set-Cookie header if present
    bool ok() const { return err.empty() && status >= 200 && status < 300; }
};

namespace http {

HttpResponse get(const std::string& url, const std::string& accept = "application/json",
                 const std::string& extraHeaders = "");
std::vector<uint8_t> getBinary(const std::string& url, std::string* err = nullptr);
// Longer timeout for update blobs / large payloads (seconds; 0 = default).
std::vector<uint8_t> getBinaryLong(const std::string& url, std::string* err = nullptr, int timeoutSec = 180);
std::future<HttpResponse> getAsync(const std::string& url, const std::string& accept = "application/json");

// POST with body. contentType e.g. "application/json" or "application/x-www-form-urlencoded".
// Optional extraHeaders: "Cookie: SID=...\r\n" etc.
HttpResponse post(const std::string& url, const std::string& body,
                  const std::string& contentType = "application/json",
                  const std::string& extraHeaders = "");

// Abort in-flight requests (quit / shutdown). Next call opens a fresh session.
void abortPending();

} // namespace http
