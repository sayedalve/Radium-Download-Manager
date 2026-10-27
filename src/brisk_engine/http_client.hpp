// SPDX-License-Identifier: GPL-3.0-or-later
// OpenIDM — HTTP/1.1 & HTTPS Ranged Client (POSIX Sockets + OpenSSL & Windows WinHTTP)
// Implements FR-001, FR-002, FR-004, RISK-006, THREAT-005.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include "common/types.hpp"

namespace openidm::brisk {

struct ParsedUrl {
    std::string scheme; // "http" or "https"
    std::string host;
    uint16_t port{80};
    std::string path_and_query{"/"};
    bool is_loopback_or_private{false};
    std::string userinfo; // e.g. "user:pass"
};

struct ContentRangeResult {
    bool is_valid{false};
    bool is_unsatisfied{false};
    bool has_total{false};
    uint64_t range_start{0};
    uint64_t range_end{0};
    uint64_t range_total{0};
};

struct HttpResponseMeta {
    int status_code{0};
    std::string final_url;
    uint64_t content_length{0};
    bool has_content_length{false};
    bool accept_ranges_bytes{false};
    bool accept_ranges_none{false};
    bool is_partial_content{false};
    bool is_unsatisfied_range{false};
    bool has_range_total{false};
    bool content_range_malformed{false};
    uint64_t range_start{0};
    uint64_t range_end{0};
    uint64_t range_total{0};
    std::string etag;
    std::string last_modified;
    std::string content_disposition;
    std::string content_type;
    int retry_after_sec{0};
    bool is_chunked{false};
    std::map<std::string, std::string> raw_headers;
};

// Explicit outcome for HTTP header callback: distinguishes intentional probe stop,
// normal body continuation, and error abortion (Phase 1.1 / FDM-001 / FDM-004).
enum class HeaderAction {
    kContinue = 0,    // Continue reading response body
    kStopSuccess = 1, // Intentional stop after headers (probe); returns Ok(meta) without body
    kAbort = 2        // Abort transfer with error; returns Err(...)
};

using HeaderCallback = std::function<HeaderAction(const HttpResponseMeta&)>;

// State machine for decoding HTTP/1.1 chunked transfer encoding (RFC 9112 Section 7.1)
// Removes chunk framing (hex size, extensions, chunk CRLFs, trailers) and delivers
// pure entity body bytes to the consumer (FDM-003, TG-04).
class ChunkedDecoder {
public:
    enum class State {
        kChunkHeader,
        kChunkData,
        kChunkCrlf,
        kTrailers,
        kComplete,
        kError
    };

    State state{State::kChunkHeader};
    uint64_t current_chunk_size{0};
    uint64_t current_chunk_remaining{0};
    std::string buffer;
    std::string error_message;

    bool Feed(const char* data, size_t len, const std::function<bool(const uint8_t*, size_t)>& on_chunk);
    bool IsComplete() const { return state == State::kComplete; }
    bool HasError() const { return state == State::kError; }
};

struct HttpNetworkOptions {
    std::string proxy_mode{"system"};                 // "direct", "system", "manual"
    std::string proxy_server;                         // e.g. "http://127.0.0.1:8080"
    std::string proxy_bypass;                         // e.g. "<local>;*.internal"
    std::string proxy_username;
    std::string proxy_password;                       // Decrypted plaintext for WinHTTP API
    std::string server_auth_username;
    std::string server_auth_password;                 // Decrypted plaintext for WinHTTP API
    bool enable_integrated_auth{true};                // Windows Negotiate / NTLM autologon
};

class HttpClient {
public:
    using HttpNetworkOptions = openidm::brisk::HttpNetworkOptions;

    static Result<ParsedUrl> ParseUrl(std::string_view url);
    static std::string ResolveRedirectTarget(const ParsedUrl& base, const std::string& loc);

    // Validates whether an IPv4 address (host byte order) belongs to private, loopback,
    // link-local, carrier-grade NAT, multicast, reserved, or broadcast ranges.
    static bool IsPrivateOrLoopbackIp(uint32_t ip);

    // Validates whether an IPv6 address (16 raw network bytes) belongs to loopback,
    // unspecified, link-local, unique-local (ULA), multicast, or IPv4-mapped private ranges.
    static bool IsPrivateOrLoopbackIpv6(const uint8_t bytes[16]);

    // Performs DNS resolution on the hostname and returns true if any resolved address
    // belongs to a private, loopback, or internal address space.
    static bool CheckHostResolvesToPrivate(const std::string& host);

    // Executes an HTTP/HTTPS GET request with optional byte range [range_start, range_end].
    // Calls on_headers once response headers arrive; if on_headers returns false, aborts before reading body.
    // Maps an HTTP status code (>= 400) to its designated Result error code
    static Result<HttpResponseMeta> MapHttpStatusToError(const HttpResponseMeta& meta);

    // Executes an HTTP/HTTPS GET request with optional byte range [range_start, range_end].
    // Calls on_headers once response headers arrive:
    //   - HeaderAction::kContinue: proceeds to read body (checks status >= 400 as error)
    //   - HeaderAction::kStopSuccess: terminates cleanly with Ok(meta) (for header-only probes)
    //   - HeaderAction::kAbort: aborts transfer and returns Err(...)
    // Calls on_chunk for each body chunk; if on_chunk returns false or cancel_flag becomes true, aborts immediately.
    static Result<HttpResponseMeta> PerformGet(
        const std::string& url,
        const std::map<std::string, std::string>& headers,
        std::optional<uint64_t> range_start,
        std::optional<uint64_t> range_end,
        int timeout_ms,
        bool allow_loopback_redirect,
        std::atomic<bool>& cancel_flag,
        HeaderCallback on_headers,
        std::function<bool(const uint8_t* data, size_t len)> on_chunk,
        const HttpNetworkOptions& net_opts = {});

    // Overload for legacy boolean callback callers (true -> kContinue, false -> kAbort)
    static Result<HttpResponseMeta> PerformGet(
        const std::string& url,
        const std::map<std::string, std::string>& headers,
        std::optional<uint64_t> range_start,
        std::optional<uint64_t> range_end,
        int timeout_ms,
        bool allow_loopback_redirect,
        std::atomic<bool>& cancel_flag,
        std::function<bool(const HttpResponseMeta&)> on_headers,
        std::function<bool(const uint8_t* data, size_t len)> on_chunk,
        const HttpNetworkOptions& net_opts = {});

    // Probes target resource headers (HEAD/GET with early termination on headers)
    static Result<HttpResponseMeta> ProbeResource(
        const std::string& url,
        const std::map<std::string, std::string>& headers = {},
        const std::map<std::string, std::string>& auth_headers = {},
        const HttpNetworkOptions& net_opts = {},
        bool allow_loopback = false,
        int timeout_ms = 10000);

    // Determines if an HTTP response status indicates an expired link or session (XDM / RFC 7235)
    static bool IsLinkExpiredStatus(int status_code, std::string_view www_authenticate = "");
};

// Parses RFC 9110 / RFC 7233 Content-Range header value (e.g. "bytes 0-1023/2048")
// Enforces range satisfaction validation (rejects start >= total || end >= total || total == 0).
ContentRangeResult ParseContentRangeExtended(std::string_view val);
bool ParseContentRange(std::string_view val, uint64_t& start, uint64_t& end, uint64_t& total);

} // namespace openidm::brisk
