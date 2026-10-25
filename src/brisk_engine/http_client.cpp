// SPDX-License-Identifier: GPL-3.0-or-later
#include "brisk_engine/http_client.hpp"
#include "common/crypto_utils.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

namespace openidm::brisk {

namespace {

std::string ToLower(std::string_view sv) {
    std::string s(sv);
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Trim(std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
        sv.remove_suffix(1);
    }
    return std::string(sv);
}

} // namespace

bool HttpClient::IsPrivateOrLoopbackIp(uint32_t ip) {
    // 0.0.0.0/8 (Current network)
    if ((ip >> 24) == 0) return true;
    // 10.0.0.0/8 (RFC 1918)
    if ((ip >> 24) == 10) return true;
    // 100.64.0.0/10 (Carrier-Grade NAT / Shared Address Space RFC 6598)
    if (ip >= 0x64400000 && ip <= 0x647FFFFF) return true;
    // 127.0.0.0/8 (Loopback)
    if ((ip >> 24) == 127) return true;
    // 169.254.0.0/16 (Link-Local APIPA)
    if (ip >= 0xA9FE0000 && ip <= 0xA9FEFFFF) return true;
    // 172.16.0.0/12 (RFC 1918: 172.16.0.0 - 172.31.255.255)
    if (ip >= 0xAC100000 && ip <= 0xAC1FFFFF) return true;
    // 192.0.0.0/24 (IETF Protocol Assignments RFC 6890)
    if (ip >= 0xC0000000 && ip <= 0xC00000FF) return true;
    // 192.0.2.0/24 (TEST-NET-1)
    if (ip >= 0xC0000200 && ip <= 0xC00002FF) return true;
    // 192.168.0.0/16 (RFC 1918: 192.168.0.0 - 192.168.255.255)
    if (ip >= 0xC0A80000 && ip <= 0xC0A8FFFF) return true;
    // 198.18.0.0/15 (Network Interconnect Device Benchmark RFC 2544)
    if (ip >= 0xC6120000 && ip <= 0xC613FFFF) return true;
    // 198.51.100.0/24 (TEST-NET-2)
    if (ip >= 0xC6336400 && ip <= 0xC63364FF) return true;
    // 203.0.113.0/24 (TEST-NET-3)
    if (ip >= 0xCB007100 && ip <= 0xCB0071FF) return true;
    // 224.0.0.0/4 (Multicast / Reserved)
    if ((ip >> 24) >= 224) return true;
    // 255.255.255.255 (Limited Broadcast)
    if (ip == 0xFFFFFFFF) return true;
    return false;
}

bool HttpClient::IsPrivateOrLoopbackIpv6(const uint8_t bytes[16]) {
    // ::1 (Loopback)
    bool is_loopback = true;
    for (int i = 0; i < 15; ++i) {
        if (bytes[i] != 0) is_loopback = false;
    }
    if (is_loopback && bytes[15] == 1) return true;

    // :: (Unspecified)
    bool is_zero = true;
    for (int i = 0; i < 16; ++i) {
        if (bytes[i] != 0) is_zero = false;
    }
    if (is_zero) return true;

    // fe80::/10 (Link-Local)
    if (bytes[0] == 0xFE && (bytes[1] & 0xC0) == 0x80) return true;

    // fc00::/7 (Unique Local Address ULA: fc00:: to fdff::)
    if ((bytes[0] & 0xFE) == 0xFC) return true;

    // ff00::/8 (IPv6 Multicast)
    if (bytes[0] == 0xFF) return true;

    // 2001:db8::/32 (Documentation)
    if (bytes[0] == 0x20 && bytes[1] == 0x01 && bytes[2] == 0x0D && bytes[3] == 0xB8) return true;

    // 64:ff9b::/96 (IPv4/IPv6 translation)
    if (bytes[0] == 0x00 && bytes[1] == 0x64 && bytes[2] == 0xFF && bytes[3] == 0x9B) {
        bool mid_zero = true;
        for (int i = 4; i < 12; ++i) if (bytes[i] != 0) mid_zero = false;
        if (mid_zero) {
            uint32_t ip4 = (static_cast<uint32_t>(bytes[12]) << 24) |
                           (static_cast<uint32_t>(bytes[13]) << 16) |
                           (static_cast<uint32_t>(bytes[14]) << 8) |
                           static_cast<uint32_t>(bytes[15]);
            return HttpClient::IsPrivateOrLoopbackIp(ip4);
        }
    }

    // IPv4-mapped IPv6 ::ffff:0:0/96
    bool is_ipv4_mapped = true;
    for (int i = 0; i < 10; ++i) {
        if (bytes[i] != 0) is_ipv4_mapped = false;
    }
    if (is_ipv4_mapped && bytes[10] == 0xFF && bytes[11] == 0xFF) {
        uint32_t ip4 = (static_cast<uint32_t>(bytes[12]) << 24) |
                       (static_cast<uint32_t>(bytes[13]) << 16) |
                       (static_cast<uint32_t>(bytes[14]) << 8) |
                       static_cast<uint32_t>(bytes[15]);
        return HttpClient::IsPrivateOrLoopbackIp(ip4);
    }

    return false;
}

struct DnsResolutionCacheEntry {
    bool is_private{false};
    std::chrono::steady_clock::time_point expires_at;
};
static std::mutex s_dns_resolution_mutex;
static std::unordered_map<std::string, DnsResolutionCacheEntry> s_dns_resolution_cache;

bool HttpClient::CheckHostResolvesToPrivate(const std::string& host) {
#ifdef _WIN32
    static bool wsa_init = false;
    if (!wsa_init) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        wsa_init = true;
    }
#endif
    std::string h = ToLower(host);
    if (h == "localhost" || h == "127.0.0.1" || h == "::1") return true;

    {
        std::lock_guard<std::mutex> lk(s_dns_resolution_mutex);
        auto it = s_dns_resolution_cache.find(h);
        if (it != s_dns_resolution_cache.end() && it->second.expires_at > std::chrono::steady_clock::now()) {
            return it->second.is_private;
        }
    }

    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    int r = getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (r != 0 || !res) {
        return false;
    }
    bool is_private = false;
    for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
        if (p->ai_family == AF_INET) {
            auto* sin = reinterpret_cast<struct sockaddr_in*>(p->ai_addr);
            uint32_t ip = ntohl(sin->sin_addr.s_addr);
            if (HttpClient::IsPrivateOrLoopbackIp(ip)) {
                is_private = true;
                break;
            }
        } else if (p->ai_family == AF_INET6) {
            auto* sin6 = reinterpret_cast<struct sockaddr_in6*>(p->ai_addr);
            const uint8_t* b = reinterpret_cast<const uint8_t*>(&sin6->sin6_addr);
            if (HttpClient::IsPrivateOrLoopbackIpv6(b)) {
                is_private = true;
                break;
            }
        }
    }
    freeaddrinfo(res);
    {
        std::lock_guard<std::mutex> lk(s_dns_resolution_mutex);
        s_dns_resolution_cache[h] = {is_private, std::chrono::steady_clock::now() + std::chrono::seconds(60)};
    }
    return is_private;
}

namespace {

bool IsLoopbackHost(std::string_view host) {
    std::string h = ToLower(host);
    if (h == "localhost" || h == "127.0.0.1" || h == "::1") return true;
    if (h.rfind("127.", 0) == 0 || h.rfind("169.254.", 0) == 0 ||
        h.rfind("10.", 0) == 0 || h.rfind("192.168.", 0) == 0 ||
        h.rfind("fe80:", 0) == 0 || h.rfind("fc", 0) == 0 || h.rfind("fd", 0) == 0) {
        return true;
    }
    // Check 172.16.x.x - 172.31.x.x (RFC 1918)
    if (h.rfind("172.", 0) == 0) {
        auto dot2 = h.find('.', 4);
        if (dot2 != std::string::npos) {
            try {
                int oct = std::stoi(h.substr(4, dot2 - 4));
                if (oct >= 16 && oct <= 31) return true;
            } catch (...) {}
        }
    }
    return false;
}

} // namespace

bool ParseContentRange(std::string_view val, uint64_t& start, uint64_t& end, uint64_t& total) {
    std::string s = Trim(val);
    if (ToLower(s.substr(0, 6)) != "bytes ") return false;
    s = Trim(s.substr(6));
    auto slash = s.find('/');
    if (slash == std::string::npos) return false;
    std::string range_part = s.substr(0, slash);
    std::string total_part = s.substr(slash + 1);
    auto dash = range_part.find('-');
    if (dash == std::string::npos) return false;
    try {
        start = std::stoull(range_part.substr(0, dash));
        end = std::stoull(range_part.substr(dash + 1));
        if (total_part != "*") {
            total = std::stoull(total_part);
            if (total == 0 || start >= total || end >= total) {
                return false;
            }
        } else {
            total = 0;
        }
        return start <= end;
    } catch (...) {
        return false;
    }
}

namespace {

void PopulateHeaderField(HttpResponseMeta& meta, const std::string& k, const std::string& v) {
    std::string kl = ToLower(k);
    meta.raw_headers[kl] = v;
    if (kl == "content-length") {
        try {
            meta.content_length = std::stoull(v);
            meta.has_content_length = true;
        } catch (...) {}
    } else if (kl == "transfer-encoding") {
        if (ToLower(v).find("chunked") != std::string::npos) {
            meta.is_chunked = true;
        }
    } else if (kl == "accept-ranges") {
        if (ToLower(v).find("bytes") != std::string::npos) {
            meta.accept_ranges_bytes = true;
        }
    } else if (kl == "content-range") {
        if (ParseContentRange(v, meta.range_start, meta.range_end, meta.range_total)) {
            meta.is_partial_content = true;
            meta.accept_ranges_bytes = true;
        }
    } else if (kl == "etag") {
        meta.etag = v;
    } else if (kl == "last-modified") {
        meta.last_modified = v;
    } else if (kl == "content-disposition") {
        meta.content_disposition = v;
    } else if (kl == "content-type") {
        meta.content_type = v;
    } else if (kl == "retry-after") {
        try {
            meta.retry_after_sec = std::stoi(v);
        } catch (...) {
            // Check if Retry-After is an HTTP-date (e.g. "Wed, 21 Oct 2026 07:28:00 GMT")
            std::tm tm_buf{};
            std::istringstream ss_date(v);
            ss_date >> std::get_time(&tm_buf, "%a, %d %b %Y %H:%M:%S");
            if (!ss_date.fail()) {
#ifdef _WIN32
                time_t target_t = _mkgmtime(&tm_buf);
#else
                time_t target_t = timegm(&tm_buf);
#endif
                time_t now_t = std::time(nullptr);
                if (target_t > now_t) {
                    meta.retry_after_sec = static_cast<int>(target_t - now_t);
                }
            }
        }
        if (meta.retry_after_sec < 0) meta.retry_after_sec = 0;
        if (meta.retry_after_sec > 3600) meta.retry_after_sec = 3600; // Cap to 1 hour
    }
}

// Normalizes path removing "." and ".." segments according to RFC 3986 Section 5.2.4
std::string RemoveDotSegments(std::string_view input_path) {
    std::vector<std::string> segments;
    std::string s(input_path);
    std::stringstream ss(s);
    std::string seg;
    bool is_absolute = (!input_path.empty() && input_path.front() == '/');

    while (std::getline(ss, seg, '/')) {
        if (seg.empty() || seg == ".") {
            continue;
        }
        if (seg == "..") {
            if (!segments.empty()) {
                segments.pop_back();
            }
        } else {
            segments.push_back(seg);
        }
    }

    std::string result;
    if (is_absolute) result += "/";
    for (size_t i = 0; i < segments.size(); ++i) {
        result += segments[i];
        if (i + 1 < segments.size() || (!input_path.empty() && input_path.back() == '/')) {
            result += "/";
        }
    }
    if (result.empty()) result = "/";
    return result;
}

#ifdef _WIN32
std::wstring Utf8ToWide(std::string_view sv) {
    if (sv.empty()) return {};
    int sz = MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), nullptr, 0);
    if (sz <= 0) return {};
    std::wstring out(static_cast<size_t>(sz), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), out.data(), sz);
    return out;
}

std::string WideToUtf8(std::wstring_view wv) {
    if (wv.empty()) return {};
    int sz = WideCharToMultiByte(CP_UTF8, 0, wv.data(), static_cast<int>(wv.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(sz), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wv.data(), static_cast<int>(wv.size()), out.data(), sz, nullptr, nullptr);
    return out;
}
#endif

} // namespace

bool ChunkedDecoder::Feed(const char* data, size_t len, const std::function<bool(const uint8_t*, size_t)>& on_chunk) {
    if (len > 0) {
        buffer.append(data, len);
    }
    while (true) {
        if (state == State::kComplete || state == State::kError) {
            return state == State::kComplete;
        }

        if (state == State::kChunkHeader) {
            auto crlf = buffer.find("\r\n");
            if (crlf == std::string::npos) {
                if (buffer.size() > 65536) {
                    state = State::kError;
                    error_message = "Chunk header line exceeded 64 KiB";
                    return false;
                }
                return true; // Need more data
            }
            std::string header_line = buffer.substr(0, crlf);
            buffer.erase(0, crlf + 2);

            auto semi = header_line.find(';');
            std::string hex_str = (semi != std::string::npos) ? header_line.substr(0, semi) : header_line;
            size_t start = hex_str.find_first_not_of(" \t");
            size_t end = hex_str.find_last_not_of(" \t");
            if (start == std::string::npos) {
                state = State::kError;
                error_message = "Empty chunk size in header";
                return false;
            }
            hex_str = hex_str.substr(start, end - start + 1);

            try {
                size_t idx = 0;
                current_chunk_size = std::stoull(hex_str, &idx, 16);
                if (idx != hex_str.size()) {
                    state = State::kError;
                    error_message = "Malformed hex chunk size: " + hex_str;
                    return false;
                }
            } catch (...) {
                state = State::kError;
                error_message = "Failed to parse chunk size: " + hex_str;
                return false;
            }

            current_chunk_remaining = current_chunk_size;
            if (current_chunk_size == 0) {
                state = State::kTrailers;
            } else {
                state = State::kChunkData;
            }
        }

        if (state == State::kChunkData) {
            if (current_chunk_remaining > 0 && !buffer.empty()) {
                size_t to_deliver = std::min<size_t>(buffer.size(), static_cast<size_t>(current_chunk_remaining));
                if (on_chunk && !on_chunk(reinterpret_cast<const uint8_t*>(buffer.data()), to_deliver)) {
                    state = State::kError;
                    error_message = "Consumer rejected chunk data";
                    return false;
                }
                buffer.erase(0, to_deliver);
                current_chunk_remaining -= to_deliver;
            }
            if (current_chunk_remaining == 0) {
                state = State::kChunkCrlf;
            } else {
                return true;
            }
        }

        if (state == State::kChunkCrlf) {
            if (buffer.size() < 2) {
                return true;
            }
            if (buffer[0] != '\r' || buffer[1] != '\n') {
                state = State::kError;
                error_message = "Missing CRLF after chunk data";
                return false;
            }
            buffer.erase(0, 2);
            state = State::kChunkHeader;
        }

        if (state == State::kTrailers) {
            if (buffer.rfind("\r\n", 0) == 0) {
                buffer.erase(0, 2);
                state = State::kComplete;
                return true;
            }
            auto end_trailers = buffer.find("\r\n\r\n");
            if (end_trailers != std::string::npos) {
                buffer.erase(0, end_trailers + 4);
                state = State::kComplete;
                return true;
            }
            if (buffer.size() > 65536) {
                state = State::kError;
                error_message = "Trailers exceeded 64 KiB";
                return false;
            }
            return true;
        }
    }
}

std::string HttpClient::ResolveRedirectTarget(const ParsedUrl& base, const std::string& loc) {
    if (loc.empty()) return "";
    // 1. Absolute URI with scheme (e.g. http:// or https://)
    if (loc.find("://") != std::string::npos) {
        return loc;
    }
    // 2. Protocol-relative URL (e.g. //localhost:18082/full.bin)
    if (loc.rfind("//", 0) == 0) {
        return base.scheme + ":" + loc;
    }
    std::string authority = base.scheme + "://" + base.host;
    if ((base.scheme == "http" && base.port != 80) || (base.scheme == "https" && base.port != 443)) {
        authority += ":" + std::to_string(base.port);
    }
    // 3. Absolute path (e.g. /path/to/resource)
    if (loc.front() == '/') {
        auto q_pos = loc.find('?');
        std::string path_part = (q_pos == std::string::npos) ? loc : loc.substr(0, q_pos);
        std::string query_part = (q_pos == std::string::npos) ? "" : loc.substr(q_pos);
        return authority + RemoveDotSegments(path_part) + query_part;
    }
    // 4. Query-only reference (e.g. ?query=val)
    if (loc.front() == '?') {
        std::string base_path = base.path_and_query.empty() ? "/" : base.path_and_query;
        auto q_pos = base_path.find('?');
        if (q_pos != std::string::npos) {
            base_path = base_path.substr(0, q_pos);
        }
        return authority + base_path + loc;
    }
    // 5. Relative path (e.g. subpath/file.bin or ../file.bin)
    std::string base_dir = "/";
    std::string clean_path = base.path_and_query;
    auto q_pos = clean_path.find('?');
    if (q_pos != std::string::npos) {
        clean_path = clean_path.substr(0, q_pos);
    }
    auto last_slash = clean_path.rfind('/');
    if (last_slash != std::string::npos) {
        base_dir = clean_path.substr(0, last_slash + 1);
    }
    auto loc_q_pos = loc.find('?');
    std::string loc_path = (loc_q_pos == std::string::npos) ? loc : loc.substr(0, loc_q_pos);
    std::string loc_query = (loc_q_pos == std::string::npos) ? "" : loc.substr(loc_q_pos);
    std::string combined = base_dir + loc_path;
    return authority + RemoveDotSegments(combined) + loc_query;
}

Result<ParsedUrl> HttpClient::ParseUrl(std::string_view url) {
    std::string u = Trim(url);
    auto scheme_pos = u.find("://");
    if (scheme_pos == std::string::npos) {
        return Result<ParsedUrl>::Err("ERR_INVALID_URL", "Missing URL scheme");
    }
    ParsedUrl out;
    out.scheme = ToLower(u.substr(0, scheme_pos));
    if (out.scheme != "http" && out.scheme != "https") {
        return Result<ParsedUrl>::Err("ERR_UNSAFE_SCHEME", "Only http:// and https:// schemes are permitted");
    }
    out.port = (out.scheme == "https") ? 443 : 80;

    std::string rest = u.substr(scheme_pos + 3);
    size_t auth_end = rest.find_first_of("/?#");
    std::string authority = (auth_end == std::string::npos) ? rest : rest.substr(0, auth_end);
    std::string path_and_query_raw = (auth_end == std::string::npos) ? "/" : rest.substr(auth_end);

    // Strip fragment (#) from path_and_query as HTTP clients never send fragments to servers
    auto frag_pos = path_and_query_raw.find('#');
    std::string path_and_query = (frag_pos == std::string::npos) ? path_and_query_raw : path_and_query_raw.substr(0, frag_pos);

    if (path_and_query.empty()) {
        path_and_query = "/";
    } else if (path_and_query.front() == '?') {
        path_and_query = "/" + path_and_query;
    }
    out.path_and_query = path_and_query;

    if (authority.empty()) {
        return Result<ParsedUrl>::Err("ERR_INVALID_URL", "Empty URL host");
    }

    auto at_pos = authority.rfind('@');
    if (at_pos != std::string::npos) {
        out.userinfo = authority.substr(0, at_pos);
        authority = authority.substr(at_pos + 1);
        if (authority.empty()) {
            return Result<ParsedUrl>::Err("ERR_INVALID_URL", "Empty URL host after userinfo");
        }
    }

    if (authority.front() == '[') {
        auto close_b = authority.find(']');
        if (close_b == std::string::npos) return Result<ParsedUrl>::Err("ERR_INVALID_URL", "Invalid IPv6 host");
        out.host = authority.substr(1, close_b - 1);
        if (close_b + 1 < authority.size()) {
            if (authority[close_b + 1] != ':') {
                return Result<ParsedUrl>::Err("ERR_INVALID_URL", "Invalid character after IPv6 bracket");
            }
            std::string port_str = authority.substr(close_b + 2);
            try {
                size_t p_idx = 0;
                int p = std::stoi(port_str, &p_idx);
                if (p_idx != port_str.size() || p <= 0 || p > 65535) {
                    return Result<ParsedUrl>::Err("ERR_INVALID_URL", "Port out of range");
                }
                out.port = static_cast<uint16_t>(p);
            } catch (...) {
                return Result<ParsedUrl>::Err("ERR_INVALID_URL", "Invalid port number");
            }
        }
    } else {
        auto colon = authority.find(':');
        if (colon != std::string::npos) {
            out.host = authority.substr(0, colon);
            std::string port_str = authority.substr(colon + 1);
            try {
                size_t p_idx = 0;
                int p = std::stoi(port_str, &p_idx);
                if (p_idx != port_str.size() || p <= 0 || p > 65535) {
                    return Result<ParsedUrl>::Err("ERR_INVALID_URL", "Port out of range");
                }
                out.port = static_cast<uint16_t>(p);
            } catch (...) {
                return Result<ParsedUrl>::Err("ERR_INVALID_URL", "Invalid port number");
            }
        } else {
            out.host = authority;
        }
    }

    out.is_loopback_or_private = IsLoopbackHost(out.host);
    return Result<ParsedUrl>::Ok(std::move(out));
}

bool HttpClient::IsLinkExpiredStatus(int status_code, std::string_view www_authenticate) {
    if (status_code == 403 || status_code == 410) return true;
    if (status_code == 401) {
        if (www_authenticate.empty()) return true;
        std::string auth_lower = ToLower(www_authenticate);
        if (auth_lower.find("basic") != std::string::npos ||
            auth_lower.find("ntlm") != std::string::npos ||
            auth_lower.find("negotiate") != std::string::npos ||
            auth_lower.find("digest") != std::string::npos) {
            return false;
        }
        return true;
    }
    return false;
}

Result<HttpResponseMeta> HttpClient::MapHttpStatusToError(const HttpResponseMeta& meta) {
    if (meta.status_code == 407) {
        return Result<HttpResponseMeta>::Err("ERR_PROXY_AUTH_REQUIRED", "Proxy authentication required (HTTP 407)", false, true);
    }
    std::string www_auth;
    auto auth_it = meta.raw_headers.find("www-authenticate");
    if (auth_it != meta.raw_headers.end()) www_auth = auth_it->second;

    if (IsLinkExpiredStatus(meta.status_code, www_auth)) {
        return Result<HttpResponseMeta>::Err("ERR_HTTP_403_EXPIRED_OR_AUTH", "HTTP " + std::to_string(meta.status_code) + " (Link Expired or Auth Required)", false, true);
    }
    if (meta.status_code == 401) {
        return Result<HttpResponseMeta>::Err("ERR_HTTP_AUTH_CHALLENGE", "HTTP 401 Authorization Challenge Required", false, true);
    }
    if (meta.status_code == 404) return Result<HttpResponseMeta>::Err("ERR_HTTP_404_NOT_FOUND", "HTTP 404 Not Found", false, false);
    if (meta.status_code == 412) return Result<HttpResponseMeta>::Err("ERR_HTTP_VALIDATOR_MISMATCH", "HTTP 412 Precondition Failed", false, true);
    if (meta.status_code == 416) return Result<HttpResponseMeta>::Err("ERR_HTTP_RANGE_NOT_SATISFIABLE", "HTTP 416 Range Not Satisfiable", false, true);
    if (meta.status_code == 429) return Result<HttpResponseMeta>::Err("ERR_HTTP_429_RATE_LIMITED", "HTTP 429 Too Many Requests", true, true);
    if (meta.status_code >= 500) return Result<HttpResponseMeta>::Err("ERR_HTTP_5XX_TRANSIENT", "HTTP " + std::to_string(meta.status_code) + " Server Error", true, true);
    return Result<HttpResponseMeta>::Err("ERR_HTTP_CLIENT", "HTTP status " + std::to_string(meta.status_code));
}

Result<HttpResponseMeta> HttpClient::PerformGet(
    const std::string& initial_url,
    const std::map<std::string, std::string>& headers,
    std::optional<uint64_t> range_start,
    std::optional<uint64_t> range_end,
    int timeout_ms,
    bool allow_loopback_redirect,
    std::atomic<bool>& cancel_flag,
    HeaderCallback on_headers,
    std::function<bool(const uint8_t* data, size_t len)> on_chunk,
    const HttpNetworkOptions& net_opts) {

    std::string current_url = initial_url;
    std::map<std::string, std::string> current_headers = headers;
    ParsedUrl initial_pu;
    bool started_on_public_host = false;

    for (int redirect_hop = 0; redirect_hop <= 10; ++redirect_hop) {
        if (cancel_flag.load()) {
            return Result<HttpResponseMeta>::Err("ERR_CANCELED", "Transfer canceled");
        }

        auto parsed_res = ParseUrl(current_url);
        if (!parsed_res.ok()) {
            return Result<HttpResponseMeta>::Err(parsed_res.error->code, parsed_res.error->message);
        }
        ParsedUrl pu = *parsed_res.value;
        bool host_is_private = pu.is_loopback_or_private || HttpClient::CheckHostResolvesToPrivate(pu.host);
        if (!allow_loopback_redirect && host_is_private) {
            return Result<HttpResponseMeta>::Err(
                "ERR_NET_UNSAFE_HOST",
                "Blocked fetch to loopback/private/internal address: " + current_url);
        }
        if (redirect_hop == 0) {
            initial_pu = pu;
            started_on_public_host = !host_is_private;
        } else {
            if (started_on_public_host && host_is_private && !allow_loopback_redirect) {
                return Result<HttpResponseMeta>::Err(
                    "ERR_NET_UNSAFE_REDIRECT",
                    "Blocked unsafe redirect from public host to loopback/private address: " + current_url);
            }
            // Cross-origin redirect credential stripping (SEC-01)
            bool origin_changed = (pu.scheme != initial_pu.scheme ||
                                   ToLower(pu.host) != ToLower(initial_pu.host) ||
                                   pu.port != initial_pu.port);
            if (origin_changed) {
                for (auto it = current_headers.begin(); it != current_headers.end();) {
                    std::string kl = ToLower(it->first);
                    if (kl == "authorization" || kl == "cookie" || kl == "proxy-authorization") {
                        it = current_headers.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        }

        // Auto-inject HTTP Basic Authorization from URL userinfo only for same-origin
        if (!pu.userinfo.empty()) {
            bool origin_matches = (redirect_hop == 0) ||
                                  (pu.scheme == initial_pu.scheme &&
                                   ToLower(pu.host) == ToLower(initial_pu.host) &&
                                   pu.port == initial_pu.port);
            if (origin_matches) {
                bool has_auth = false;
                for (const auto& [hk, hv] : current_headers) {
                    if (ToLower(hk) == "authorization") {
                        has_auth = true;
                        break;
                    }
                }
                if (!has_auth) {
                    current_headers["Authorization"] = "Basic " + CryptoUtils::Base64Encode(pu.userinfo);
                }
            }
        }

#ifdef _WIN32
        // ---------------------------------------------------------------------
        // Native Windows 11 WinHTTP Implementation (BUG-003 Fix)
        // ---------------------------------------------------------------------
        DWORD access_type = WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;
        LPCWSTR proxy_name = WINHTTP_NO_PROXY_NAME;
        LPCWSTR proxy_bypass = WINHTTP_NO_PROXY_BYPASS;
        std::wstring wproxy;
        std::wstring wbypass;

        if (net_opts.proxy_mode == "direct") {
            access_type = WINHTTP_ACCESS_TYPE_NO_PROXY;
        } else if (net_opts.proxy_mode == "manual" && !net_opts.proxy_server.empty()) {
            access_type = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
            wproxy = Utf8ToWide(net_opts.proxy_server);
            proxy_name = wproxy.c_str();
            if (!net_opts.proxy_bypass.empty()) {
                wbypass = Utf8ToWide(net_opts.proxy_bypass);
                proxy_bypass = wbypass.c_str();
            }
        }

        HINTERNET hSession = WinHttpOpen(
            L"Radium/0.3.2 (Windows NT 10.0; Win64; x64)",
            access_type,
            proxy_name,
            proxy_bypass,
            0);
        if (!hSession) {
            return Result<HttpResponseMeta>::Err("ERR_NET_INIT", "WinHttpOpen failed", true);
        }
        int resolve_timeout = std::clamp(timeout_ms, 2000, 8000);
        int connect_timeout = std::clamp(timeout_ms, 3000, 10000);
        int send_timeout = std::clamp(timeout_ms, 5000, 15000);
        int recv_timeout = std::clamp(timeout_ms, 5000, 20000);
        WinHttpSetTimeouts(hSession, resolve_timeout, connect_timeout, send_timeout, recv_timeout);

        // Enable modern HTTP/2 protocol flag for multiplexing and TCP efficiency on supported Windows versions
        DWORD http_protocols = WINHTTP_PROTOCOL_FLAG_HTTP2;
        WinHttpSetOption(hSession, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &http_protocols, sizeof(http_protocols));

        std::wstring whost = Utf8ToWide(pu.host);
        HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(), pu.port, 0);
        if (!hConnect) {
            DWORD conn_err = GetLastError();
            WinHttpCloseHandle(hSession);
            return Result<HttpResponseMeta>::Err("ERR_NET_CONNECT", "WinHttpConnect failed for " + pu.host + " (error " + std::to_string(conn_err) + ")", true);
        }

        std::wstring wpath = Utf8ToWide(pu.path_and_query);
        DWORD req_flags = (pu.scheme == "https") ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hRequest = WinHttpOpenRequest(
            hConnect, L"GET", wpath.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, req_flags);
        if (!hRequest) {
            DWORD req_err = GetLastError();
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return Result<HttpResponseMeta>::Err("ERR_NET_REQUEST", "WinHttpOpenRequest failed (error " + std::to_string(req_err) + ")", true);
        }

        // Disable automatic WinHTTP redirects so OpenIDM's SSRF redirect guard inspects every hop
        DWORD disable_redirects = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_DISABLE_FEATURE, &disable_redirects, sizeof(disable_redirects));

        // Optimize TCP read buffer for high-speed segmented downloading
        DWORD read_buf_size = 262144; // 256 KiB socket read buffer
        WinHttpSetOption(hRequest, WINHTTP_OPTION_READ_BUFFER_SIZE, &read_buf_size, sizeof(read_buf_size));

        // Windows Integrated Authentication (Negotiate / NTLM autologon for domain/intranet)
        if (net_opts.enable_integrated_auth) {
            DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_MEDIUM;
            WinHttpSetOption(hRequest, WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon, sizeof(autologon));
        }

        // Explicit proxy credentials (RFC 7235 / WinHTTP)
        if (!net_opts.proxy_username.empty()) {
            std::wstring wp_user = Utf8ToWide(net_opts.proxy_username);
            std::wstring wp_pass = Utf8ToWide(net_opts.proxy_password);
            WinHttpSetCredentials(hRequest, WINHTTP_AUTH_TARGET_PROXY, WINHTTP_AUTH_SCHEME_BASIC, wp_user.c_str(), wp_pass.c_str(), nullptr);
        }

        // Explicit origin server credentials
        if (!net_opts.server_auth_username.empty()) {
            std::wstring ws_user = Utf8ToWide(net_opts.server_auth_username);
            std::wstring ws_pass = Utf8ToWide(net_opts.server_auth_password);
            WinHttpSetCredentials(hRequest, WINHTTP_AUTH_TARGET_SERVER, WINHTTP_AUTH_SCHEME_BASIC, ws_user.c_str(), ws_pass.c_str(), nullptr);
        }

        auto cleanup_winhttp = [&]() {
            if (hRequest) { WinHttpCloseHandle(hRequest); hRequest = nullptr; }
            if (hConnect) { WinHttpCloseHandle(hConnect); hConnect = nullptr; }
            if (hSession) { WinHttpCloseHandle(hSession); hSession = nullptr; }
        };

        std::ostringstream hdr_stream;
        for (const auto& [k, v] : current_headers) {
            if (k.find('\r') != std::string::npos || k.find('\n') != std::string::npos ||
                v.find('\r') != std::string::npos || v.find('\n') != std::string::npos) {
                continue; // Prevent CRLF header injection (SEC-04)
            }
            if (ToLower(k) == "range" || ToLower(k) == "host" || ToLower(k) == "connection") continue;
            hdr_stream << k << ": " << v << "\r\n";
        }
        if (range_start.has_value()) {
            hdr_stream << "Range: bytes=" << *range_start << "-";
            if (range_end.has_value()) hdr_stream << *range_end;
            hdr_stream << "\r\n";
        }
        std::wstring wheaders = Utf8ToWide(hdr_stream.str());
        if (!wheaders.empty()) {
            WinHttpAddRequestHeaders(hRequest, wheaders.c_str(), static_cast<DWORD>(wheaders.size()), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        }

        if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(hRequest, nullptr)) {
            DWORD winhttp_err = GetLastError();
            cleanup_winhttp();
            std::string err_code = "ERR_NET_SEND";
            std::string err_desc = "WinHTTP request/response failed (error " + std::to_string(winhttp_err) + ")";
            bool is_retryable = true;
            if (winhttp_err == ERROR_WINHTTP_TIMEOUT || winhttp_err == ERROR_WINHTTP_RESPONSE_DRAIN_OVERFLOW) {
                err_code = "ERR_NET_TIMEOUT";
                err_desc = "WinHTTP request timed out waiting for server response (error " + std::to_string(winhttp_err) + ")";
            } else if (winhttp_err == ERROR_WINHTTP_CANNOT_CONNECT) {
                err_code = "ERR_NET_CONNECT";
                err_desc = "WinHTTP cannot connect to server (error " + std::to_string(winhttp_err) + ")";
            } else if (winhttp_err == ERROR_WINHTTP_CONNECTION_ERROR || winhttp_err == 12030 || winhttp_err == 12031) {
                err_code = "ERR_NET_CONNECTION_RESET";
                err_desc = "WinHTTP connection was reset or aborted by server (error " + std::to_string(winhttp_err) + ")";
            } else if (winhttp_err == ERROR_WINHTTP_NAME_NOT_RESOLVED) {
                err_code = "ERR_NET_DNS";
                err_desc = "WinHTTP host name could not be resolved (error 12007)";
            } else if (winhttp_err == ERROR_WINHTTP_SECURE_FAILURE) {
                err_code = "ERR_SSL_PROTOCOL";
                err_desc = "WinHTTP secure channel / SSL certificate verification failure (error 12175)";
            }
            return Result<HttpResponseMeta>::Err(err_code, err_desc, is_retryable);
        }

        DWORD status_code = 0;
        DWORD status_sz = sizeof(status_code);
        WinHttpQueryHeaders(
            hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status_code,
            &status_sz,
            WINHTTP_NO_HEADER_INDEX);

        DWORD raw_hdr_bytes = 0;
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &raw_hdr_bytes, WINHTTP_NO_HEADER_INDEX);
        std::wstring raw_whdrs(raw_hdr_bytes / sizeof(wchar_t), L'\0');
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw_whdrs.data(), &raw_hdr_bytes, WINHTTP_NO_HEADER_INDEX);

        HttpResponseMeta meta;
        meta.status_code = static_cast<int>(status_code);
        meta.final_url = current_url;

        std::istringstream hstream(WideToUtf8(raw_whdrs));
        std::string line;
        std::getline(hstream, line); // Skip status line
        while (std::getline(hstream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            PopulateHeaderField(meta, Trim(line.substr(0, colon)), Trim(line.substr(colon + 1)));
        }

        if (meta.status_code == 301 || meta.status_code == 302 || meta.status_code == 303 ||
            meta.status_code == 307 || meta.status_code == 308) {
            cleanup_winhttp();
            auto loc_it = meta.raw_headers.find("location");
            if (loc_it == meta.raw_headers.end() || loc_it->second.empty()) {
                return Result<HttpResponseMeta>::Err("ERR_HTTP_REDIRECT_NO_LOCATION", "Redirect missing Location header");
            }
            std::string loc = loc_it->second;
            current_url = ResolveRedirectTarget(pu, loc);
            continue;
        }

        if (on_headers) {
            HeaderAction action = on_headers(meta);
            if (action == HeaderAction::kStopSuccess) {
                cleanup_winhttp();
                return Result<HttpResponseMeta>::Ok(std::move(meta));
            }
            if (action == HeaderAction::kAbort) {
                cleanup_winhttp();
                if (meta.status_code >= 400) {
                    return MapHttpStatusToError(meta);
                }
                return Result<HttpResponseMeta>::Err("ERR_HTTP_ABORTED", "Transfer aborted after headers");
            }
        }

        if (meta.status_code >= 400) {
            cleanup_winhttp();
            return MapHttpStatusToError(meta);
        }

        uint64_t bytes_delivered = 0;
        constexpr size_t kSocketBufSize = 262144;
        auto rbuf = std::make_unique<uint8_t[]>(kSocketBufSize);
        while (!meta.has_content_length || bytes_delivered < meta.content_length) {
            if (cancel_flag.load()) {
                cleanup_winhttp();
                return Result<HttpResponseMeta>::Err("ERR_CANCELED", "Transfer canceled");
            }
            DWORD bytes_read = 0;
            if (!WinHttpReadData(hRequest, rbuf.get(), static_cast<DWORD>(kSocketBufSize), &bytes_read)) {
                cleanup_winhttp();
                return Result<HttpResponseMeta>::Err("ERR_NET_TIMEOUT", "WinHttpReadData timeout or error", true, true);
            }
            if (bytes_read == 0) break;
            if (on_chunk && !on_chunk(rbuf.get(), bytes_read)) {
                cleanup_winhttp();
                return Result<HttpResponseMeta>::Err("ERR_CONSUMER_ABORTED", "Body consumer aborted transfer");
            }
            bytes_delivered += bytes_read;
        }
        cleanup_winhttp();
        if (meta.has_content_length && bytes_delivered < meta.content_length) {
            return Result<HttpResponseMeta>::Err("ERR_NET_CONNECTION_RESET", "Premature EOF before Content-Length", true, true);
        }
        return Result<HttpResponseMeta>::Ok(std::move(meta));
#else
        // ---------------------------------------------------------------------
        // POSIX Socket + OpenSSL Implementation
        // ---------------------------------------------------------------------
        std::string connect_host = pu.host;
        if (const char* host_map = std::getenv("FLUX_HOST_RESOLVER_MAP")) {
            std::string map_s(host_map);
            std::string lower_map = ToLower(map_s);
            std::string key = ToLower(pu.host) + "=";
            auto pos = lower_map.find(key);
            if (pos != std::string::npos) {
                auto val_start = pos + key.size();
                auto comma = map_s.find(',', val_start);
                connect_host = Trim(map_s.substr(val_start, comma == std::string::npos ? std::string::npos : comma - val_start));
            }
        }

        struct addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* res = nullptr;
        std::string port_str = std::to_string(pu.port);
        if (getaddrinfo(connect_host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
            return Result<HttpResponseMeta>::Err("ERR_NET_DNS", "Failed to resolve host: " + pu.host, true);
        }

        int sock = -1;
        for (struct addrinfo* rp = res; rp != nullptr; rp = rp->ai_next) {
            if (started_on_public_host && !allow_loopback_redirect) {
                if (rp->ai_family == AF_INET) {
                    auto* sin = reinterpret_cast<struct sockaddr_in*>(rp->ai_addr);
                    if (HttpClient::IsPrivateOrLoopbackIp(ntohl(sin->sin_addr.s_addr))) {
                        continue;
                    }
                } else if (rp->ai_family == AF_INET6) {
                    auto* sin6 = reinterpret_cast<struct sockaddr_in6*>(rp->ai_addr);
                    if (HttpClient::IsPrivateOrLoopbackIpv6(reinterpret_cast<const uint8_t*>(&sin6->sin6_addr))) {
                        continue;
                    }
                }
            }

            sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (sock < 0) continue;

            struct timeval tv{};
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

            if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0) {
                break;
            }
            close(sock);
            sock = -1;
        }
        freeaddrinfo(res);

        if (sock < 0) {
            return Result<HttpResponseMeta>::Err("ERR_NET_CONNECT", "Connection failed to " + pu.host + ":" + port_str, true);
        }

        SSL_CTX* ssl_ctx = nullptr;
        SSL* ssl = nullptr;
        auto cleanup_conn = [&]() {
            if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); ssl = nullptr; }
            if (ssl_ctx) { SSL_CTX_free(ssl_ctx); ssl_ctx = nullptr; }
            if (sock >= 0) { close(sock); sock = -1; }
        };

        if (pu.scheme == "https") {
            ssl_ctx = SSL_CTX_new(TLS_client_method());
            if (!ssl_ctx) {
                cleanup_conn();
                return Result<HttpResponseMeta>::Err("ERR_TLS_INIT", "SSL_CTX_new failed");
            }
            SSL_CTX_set_default_verify_paths(ssl_ctx);
            SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, nullptr);
            X509_VERIFY_PARAM* param = SSL_CTX_get0_param(ssl_ctx);
            if (param) {
                X509_VERIFY_PARAM_set1_host(param, pu.host.c_str(), pu.host.size());
            }

            ssl = SSL_new(ssl_ctx);
            SSL_set_tlsext_host_name(ssl, pu.host.c_str());
            SSL_set_fd(ssl, sock);
            if (SSL_connect(ssl) != 1) {
                long verify_err = SSL_get_verify_result(ssl);
                cleanup_conn();
                if (verify_err != X509_V_OK) {
                    return Result<HttpResponseMeta>::Err(
                        "ERR_TLS_CERT_INVALID",
                        std::string("TLS Certificate verification failed: ") + X509_verify_cert_error_string(verify_err),
                        false, false);
                }
                return Result<HttpResponseMeta>::Err("ERR_TLS_HANDSHAKE", "TLS handshake failed", true);
            }
        }

        auto write_all = [&](const char* buf, size_t len) -> bool {
            size_t sent = 0;
            while (sent < len) {
                ssize_t n = ssl ? SSL_write(ssl, buf + sent, static_cast<int>(len - sent))
                                : send(sock, buf + sent, len - sent, 0);
                if (n <= 0) return false;
                sent += static_cast<size_t>(n);
            }
            return true;
        };

        auto read_some = [&](char* buf, size_t cap) -> ssize_t {
            return ssl ? SSL_read(ssl, buf, static_cast<int>(cap))
                       : recv(sock, buf, cap, 0);
        };

        std::ostringstream req;
        req << "GET " << pu.path_and_query << " HTTP/1.1\r\n";
        req << "Host: " << pu.host;
        if ((pu.scheme == "http" && pu.port != 80) || (pu.scheme == "https" && pu.port != 443)) {
            req << ":" << pu.port;
        }
        req << "\r\n";
        req << "Connection: close\r\n";

        bool has_ua = false;
        for (const auto& [k, v] : current_headers) {
            if (k.find('\r') != std::string::npos || k.find('\n') != std::string::npos ||
                v.find('\r') != std::string::npos || v.find('\n') != std::string::npos) {
                continue;
            }
            if (ToLower(k) == "user-agent") has_ua = true;
            if (ToLower(k) == "range" || ToLower(k) == "host" || ToLower(k) == "connection") continue;
            req << k << ": " << v << "\r\n";
        }
        if (!has_ua) {
            req << "User-Agent: Radium/0.3.2 (Windows NT 10.0; Win64; x64)\r\n";
        }
        if (range_start.has_value()) {
            req << "Range: bytes=" << *range_start << "-";
            if (range_end.has_value()) {
                req << *range_end;
            }
            req << "\r\n";
        }
        req << "\r\n";

        std::string req_str = req.str();
        if (!write_all(req_str.data(), req_str.size())) {
            cleanup_conn();
            return Result<HttpResponseMeta>::Err("ERR_NET_SEND", "Failed to send HTTP request", true);
        }

        std::string header_buf;
        header_buf.reserve(4096);
        std::vector<char> tmp_vec(8192);
        char* tmp = tmp_vec.data();
        size_t header_end_pos = std::string::npos;
        while ((header_end_pos = header_buf.find("\r\n\r\n")) == std::string::npos) {
            if (cancel_flag.load()) {
                cleanup_conn();
                return Result<HttpResponseMeta>::Err("ERR_CANCELED", "Canceled during header read");
            }
            if (header_buf.size() > 65536) {
                cleanup_conn();
                return Result<HttpResponseMeta>::Err("ERR_HTTP_HEADER_OVERFLOW", "Response headers exceeded 64 KiB");
            }
            ssize_t n = read_some(tmp, tmp_vec.size());
            if (n <= 0) {
                cleanup_conn();
                return Result<HttpResponseMeta>::Err("ERR_NET_TIMEOUT", "Socket read timeout or closed before headers", true);
            }
            header_buf.append(tmp, static_cast<size_t>(n));
        }

        std::string raw_header_block = header_buf.substr(0, header_end_pos);
        std::string leftover_body = header_buf.substr(header_end_pos + 4);

        HttpResponseMeta meta;
        meta.final_url = current_url;

        std::istringstream hstream(raw_header_block);
        std::string status_line;
        if (!std::getline(hstream, status_line)) {
            cleanup_conn();
            return Result<HttpResponseMeta>::Err("ERR_HTTP_MALFORMED", "Empty HTTP status line");
        }
        {
            std::istringstream sline(status_line);
            std::string proto;
            sline >> proto >> meta.status_code;
        }

        std::string line;
        while (std::getline(hstream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            PopulateHeaderField(meta, Trim(line.substr(0, colon)), Trim(line.substr(colon + 1)));
        }

        if (meta.status_code == 301 || meta.status_code == 302 || meta.status_code == 303 ||
            meta.status_code == 307 || meta.status_code == 308) {
            cleanup_conn();
            auto loc_it = meta.raw_headers.find("location");
            if (loc_it == meta.raw_headers.end() || loc_it->second.empty()) {
                return Result<HttpResponseMeta>::Err("ERR_HTTP_REDIRECT_NO_LOCATION", "Redirect missing Location header");
            }
            std::string loc = loc_it->second;
            current_url = ResolveRedirectTarget(pu, loc);
            continue;
        }

        if (on_headers) {
            HeaderAction action = on_headers(meta);
            if (action == HeaderAction::kStopSuccess) {
                cleanup_conn();
                return Result<HttpResponseMeta>::Ok(std::move(meta));
            }
            if (action == HeaderAction::kAbort) {
                cleanup_conn();
                if (meta.status_code >= 400) {
                    return MapHttpStatusToError(meta);
                }
                return Result<HttpResponseMeta>::Err("ERR_HTTP_ABORTED", "Transfer aborted after headers");
            }
        }

        if (meta.status_code >= 400) {
            cleanup_conn();
            return MapHttpStatusToError(meta);
        }

        if (meta.is_chunked) {
            ChunkedDecoder decoder;
            if (!leftover_body.empty()) {
                if (!decoder.Feed(leftover_body.data(), leftover_body.size(), on_chunk)) {
                    cleanup_conn();
                    return Result<HttpResponseMeta>::Err("ERR_HTTP_CHUNK_FRAMING", "Malformed chunked transfer: " + decoder.error_message);
                }
            }
            while (!decoder.IsComplete()) {
                if (cancel_flag.load()) {
                    cleanup_conn();
                    return Result<HttpResponseMeta>::Err("ERR_CANCELED", "Transfer canceled");
                }
                ssize_t n = read_some(tmp, tmp_vec.size());
                if (n <= 0) {
                    cleanup_conn();
                    if (decoder.IsComplete()) break;
                    return Result<HttpResponseMeta>::Err("ERR_NET_CONNECTION_RESET", "Premature EOF in chunked body", true, true);
                }
                if (!decoder.Feed(tmp, static_cast<size_t>(n), on_chunk)) {
                    cleanup_conn();
                    return Result<HttpResponseMeta>::Err("ERR_HTTP_CHUNK_FRAMING", "Malformed chunked transfer: " + decoder.error_message);
                }
            }
            cleanup_conn();
            return Result<HttpResponseMeta>::Ok(std::move(meta));
        }

        uint64_t bytes_delivered = 0;
        if (!leftover_body.empty()) {
            if (on_chunk && !on_chunk(reinterpret_cast<const uint8_t*>(leftover_body.data()), leftover_body.size())) {
                cleanup_conn();
                return Result<HttpResponseMeta>::Err("ERR_CONSUMER_ABORTED", "Body consumer aborted transfer");
            }
            bytes_delivered += leftover_body.size();
        }

        while (!meta.has_content_length || bytes_delivered < meta.content_length) {
            if (cancel_flag.load()) {
                cleanup_conn();
                return Result<HttpResponseMeta>::Err("ERR_CANCELED", "Transfer canceled");
            }
            ssize_t n = read_some(tmp, tmp_vec.size());
            if (n == 0) break;
            if (n < 0) {
                cleanup_conn();
                return Result<HttpResponseMeta>::Err("ERR_NET_TIMEOUT", "Socket stall/timeout during body read", true, true);
            }
            if (on_chunk && !on_chunk(reinterpret_cast<const uint8_t*>(tmp), static_cast<size_t>(n))) {
                cleanup_conn();
                return Result<HttpResponseMeta>::Err("ERR_CONSUMER_ABORTED", "Body consumer aborted transfer");
            }
            bytes_delivered += static_cast<uint64_t>(n);
        }

        cleanup_conn();
        if (meta.has_content_length && bytes_delivered < meta.content_length) {
            return Result<HttpResponseMeta>::Err("ERR_NET_CONNECTION_RESET", "Premature EOF before Content-Length", true, true);
        }
        return Result<HttpResponseMeta>::Ok(std::move(meta));
#endif
    }

    return Result<HttpResponseMeta>::Err("ERR_HTTP_TOO_MANY_REDIRECTS", "Exceeded 10 redirects");
}

Result<HttpResponseMeta> HttpClient::PerformGet(
    const std::string& initial_url,
    const std::map<std::string, std::string>& headers,
    std::optional<uint64_t> range_start,
    std::optional<uint64_t> range_end,
    int timeout_ms,
    bool allow_loopback_redirect,
    std::atomic<bool>& cancel_flag,
    std::function<bool(const HttpResponseMeta&)> on_headers,
    std::function<bool(const uint8_t* data, size_t len)> on_chunk,
    const HttpNetworkOptions& net_opts) {
    HeaderCallback cb = nullptr;
    if (on_headers) {
        cb = [on_headers](const HttpResponseMeta& meta) {
            return on_headers(meta) ? HeaderAction::kContinue : HeaderAction::kAbort;
        };
    }
    return PerformGet(
        initial_url, headers, range_start, range_end, timeout_ms, allow_loopback_redirect, cancel_flag, cb, on_chunk, net_opts);
}

Result<HttpResponseMeta> HttpClient::ProbeResource(
    const std::string& url,
    const std::map<std::string, std::string>& headers,
    const std::map<std::string, std::string>& auth_headers,
    const HttpNetworkOptions& net_opts,
    bool allow_loopback,
    int timeout_ms) {
    std::map<std::string, std::string> combined = headers;
    for (const auto& [k, v] : auth_headers) {
        combined[k] = v;
    }
    std::atomic<bool> cancel{false};
    return PerformGet(
        url,
        combined,
        std::nullopt,
        std::nullopt,
        timeout_ms,
        allow_loopback,
        cancel,
        [](const HttpResponseMeta&) { return HeaderAction::kStopSuccess; }, // Header-only probe stop
        nullptr,
        net_opts);
}

} // namespace openidm::brisk
