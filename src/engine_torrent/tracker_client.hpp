// SPDX-License-Identifier: GPL-3.0-or-later
// OpenIDM / Flux — BitTorrent Tracker Client (BEP 3 HTTP/HTTPS, BEP 15 UDP Trackers, BEP 23 Compact)
#pragma once

#include <chrono>
#include <cstdint>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
#endif

#include "brisk_engine/http_client.hpp"
#include "common/logger.hpp"
#include "common/types.hpp"
#include "engine_torrent/bencode.hpp"
#include "engine_torrent/peer_wire_protocol.hpp"

namespace openidm::torrent {

struct TrackerAnnounceResponse {
    bool ok{false};
    std::string failure_reason;
    std::string warning_message;
    uint32_t interval{1800};
    uint32_t min_interval{300};
    uint32_t complete{0};   // seeders
    uint32_t incomplete{0}; // leechers
    std::vector<PeerEndpoint> peers;
};

class TrackerClient {
public:
    static inline uint64_t ReadUint64BE(const uint8_t* p) {
        return (static_cast<uint64_t>(p[0]) << 56) |
               (static_cast<uint64_t>(p[1]) << 48) |
               (static_cast<uint64_t>(p[2]) << 40) |
               (static_cast<uint64_t>(p[3]) << 32) |
               (static_cast<uint64_t>(p[4]) << 24) |
               (static_cast<uint64_t>(p[5]) << 16) |
               (static_cast<uint64_t>(p[6]) << 8) |
               static_cast<uint64_t>(p[7]);
    }

    static inline void WriteUint64BE(uint8_t* p, uint64_t val) {
        p[0] = static_cast<uint8_t>((val >> 56) & 0xFF);
        p[1] = static_cast<uint8_t>((val >> 48) & 0xFF);
        p[2] = static_cast<uint8_t>((val >> 40) & 0xFF);
        p[3] = static_cast<uint8_t>((val >> 32) & 0xFF);
        p[4] = static_cast<uint8_t>((val >> 24) & 0xFF);
        p[5] = static_cast<uint8_t>((val >> 16) & 0xFF);
        p[6] = static_cast<uint8_t>((val >> 8) & 0xFF);
        p[7] = static_cast<uint8_t>(val & 0xFF);
    }

    // Percent-encodes binary byte buffer according to RFC 3986 (aria2 util::percentEncode)
    static std::string PercentEncodeBinary(const uint8_t* data, size_t len) {
        std::string out;
        if (!data || len == 0) return out;
        out.reserve(len * 3);
        const char hex[] = "0123456789ABCDEF";

        for (size_t i = 0; i < len; ++i) {
            uint8_t b = data[i];
            if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') ||
                b == '-' || b == '_' || b == '.' || b == '~') {
                out.push_back(static_cast<char>(b));
            } else {
                out.push_back('%');
                out.push_back(hex[(b >> 4) & 0x0F]);
                out.push_back(hex[b & 0x0F]);
            }
        }
        return out;
    }

    static Result<TrackerAnnounceResponse> ParseTrackerResponse(std::string_view response_body) {
        auto dec_res = BencodeParser::Decode(response_body);
        if (!dec_res.ok()) {
            return Result<TrackerAnnounceResponse>::Err("ERR_TORRENT_TRACKER_MALFORMED", "Failed to decode tracker response bencode");
        }

        const auto& root = dec_res.value.value();
        if (!root.is_dict()) {
            return Result<TrackerAnnounceResponse>::Err("ERR_TORRENT_TRACKER_MALFORMED", "Tracker response is not a bencode dictionary");
        }

        TrackerAnnounceResponse resp;
        resp.ok = true;

        if (root.contains("failure reason") && root["failure reason"].is_string()) {
            resp.ok = false;
            resp.failure_reason = root["failure reason"].as_string();
            return Result<TrackerAnnounceResponse>::Err("ERR_TORRENT_TRACKER_FAILURE", resp.failure_reason);
        }

        if (root.contains("warning message") && root["warning message"].is_string()) {
            resp.warning_message = root["warning message"].as_string();
        }

        if (root.contains("interval") && root["interval"].is_int()) {
            resp.interval = static_cast<uint32_t>(root["interval"].as_int());
        }
        if (root.contains("min interval") && root["min interval"].is_int()) {
            resp.min_interval = static_cast<uint32_t>(root["min interval"].as_int());
        }
        if (root.contains("complete") && root["complete"].is_int()) {
            resp.complete = static_cast<uint32_t>(root["complete"].as_int());
        }
        if (root.contains("incomplete") && root["incomplete"].is_int()) {
            resp.incomplete = static_cast<uint32_t>(root["incomplete"].as_int());
        }

        resp.peers = PeerWireProtocol::ExtractPeersFromBencode(root);
        return Result<TrackerAnnounceResponse>::Ok(std::move(resp));
    }

    static Result<TrackerAnnounceResponse> AnnounceHttp(
        const std::string& announce_url,
        const std::vector<uint8_t>& info_hash_raw,
        const std::string& peer_id,
        uint16_t port,
        uint64_t uploaded,
        uint64_t downloaded,
        uint64_t left,
        std::string_view event = "started",
        uint32_t timeout_ms = 10000) {

        if (info_hash_raw.size() < kInfoHashLength) {
            return Result<TrackerAnnounceResponse>::Err("ERR_TORRENT_INVALID_HASH", "Invalid info_hash for tracker announce");
        }

        std::string sep = (announce_url.find('?') == std::string::npos) ? "?" : "&";
        std::string full_url = announce_url + sep +
            "info_hash=" + PercentEncodeBinary(info_hash_raw.data(), kInfoHashLength) +
            "&peer_id=" + PercentEncodeBinary(reinterpret_cast<const uint8_t*>(peer_id.data()), peer_id.size()) +
            "&port=" + std::to_string(port) +
            "&uploaded=" + std::to_string(uploaded) +
            "&downloaded=" + std::to_string(downloaded) +
            "&left=" + std::to_string(left) +
            "&compact=1";

        if (!event.empty()) {
            full_url += "&event=" + std::string(event);
        }

        std::string response_body;
        std::atomic<bool> cancel_flag{false};
        std::map<std::string, std::string> req_headers;
        req_headers["User-Agent"] = "Radium/0.3.2 (BitTorrent; Win64)";
        req_headers["Accept"] = "*/*";

        brisk::HttpClient::HttpNetworkOptions net_opts;
        auto get_res = brisk::HttpClient::PerformGet(
            full_url,
            req_headers,
            std::nullopt,
            std::nullopt,
            timeout_ms,
            true,
            cancel_flag,
            [](const brisk::HttpResponseMeta& meta) -> bool {
                return meta.status_code >= 200 && meta.status_code < 400;
            },
            [&](const uint8_t* data, size_t len) -> bool {
                if (data && len > 0) {
                    response_body.append(reinterpret_cast<const char*>(data), len);
                }
                return true;
            },
            net_opts);

        if (!get_res.ok()) {
            return Result<TrackerAnnounceResponse>::Err(get_res.error->code, get_res.error->message);
        }

        return ParseTrackerResponse(response_body);
    }

    // BEP 15 UDP Tracker Protocol Implementation
    static Result<TrackerAnnounceResponse> AnnounceUdp(
        const std::string& announce_url,
        const std::vector<uint8_t>& info_hash_raw,
        const std::string& peer_id,
        uint16_t port,
        uint64_t uploaded,
        uint64_t downloaded,
        uint64_t left,
        std::string_view event = "started",
        uint32_t timeout_ms = 4000) {

        if (info_hash_raw.size() < kInfoHashLength) {
            return Result<TrackerAnnounceResponse>::Err("ERR_TORRENT_INVALID_HASH", "Invalid info_hash for UDP tracker announce");
        }

        std::string host_port = announce_url.substr(6); // strip udp://
        size_t slash_pos = host_port.find('/');
        if (slash_pos != std::string::npos) {
            host_port = host_port.substr(0, slash_pos);
        }

        std::string host;
        std::string port_str = "80";
        size_t colon_pos = host_port.find(':');
        if (colon_pos != std::string::npos) {
            host = host_port.substr(0, colon_pos);
            port_str = host_port.substr(colon_pos + 1);
        } else {
            host = host_port;
        }

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;

        addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
            return Result<TrackerAnnounceResponse>::Err("ERR_TORRENT_TRACKER_DNS", "Failed to resolve UDP tracker host: " + host);
        }

        SOCKET sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sock == INVALID_SOCKET) {
            freeaddrinfo(res);
            return Result<TrackerAnnounceResponse>::Err("ERR_TORRENT_TRACKER_SOCKET", "Failed to create UDP socket");
        }

#ifdef _WIN32
        // Disable WSAECONNRESET from receiving ICMP port unreachable packets on Windows
        #ifndef SIO_UDP_CONNRESET
        #define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
        #endif
        BOOL bNewBehavior = FALSE;
        DWORD dwBytesReturned = 0;
        WSAIoctl(sock, SIO_UDP_CONNRESET, &bNewBehavior, sizeof(bNewBehavior), NULL, 0, &dwBytesReturned, NULL, NULL);

        DWORD to_val = timeout_ms;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to_val), sizeof(to_val));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to_val), sizeof(to_val));
#else
        timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint32_t> dist;
        uint32_t transaction_id = dist(gen);

        // 1. Connection Request (16 bytes)
        // Offset  Size    Name            Value
        // 0       int64_t protocol_id     0x41727101980
        // 8       int32_t action          0 // connect
        // 12      int32_t transaction_id
        uint8_t conn_req[16];
        WriteUint64BE(conn_req, 0x41727101980ULL);
        PeerWireProtocol::WriteUint32BE(conn_req + 8, 0); // action: 0
        PeerWireProtocol::WriteUint32BE(conn_req + 12, transaction_id);

        if (sendto(sock, reinterpret_cast<const char*>(conn_req), 16, 0, res->ai_addr, static_cast<int>(res->ai_addrlen)) <= 0) {
            closesocket(sock);
            freeaddrinfo(res);
            return Result<TrackerAnnounceResponse>::Err("ERR_TORRENT_TRACKER_SEND", "Failed to send UDP connect request");
        }

        // 2. Connection Response (16 bytes)
        // Offset  Size    Name            Value
        // 0       int32_t action          0 // connect
        // 4       int32_t transaction_id
        // 8       int64_t connection_id
        uint8_t conn_resp[16];
        int r = recvfrom(sock, reinterpret_cast<char*>(conn_resp), 16, 0, nullptr, nullptr);
        if (r < 16) {
            closesocket(sock);
            freeaddrinfo(res);
            return Result<TrackerAnnounceResponse>::Err("ERR_TORRENT_TRACKER_TIMEOUT", "UDP tracker connect timeout or truncated response");
        }

        uint32_t resp_action = PeerWireProtocol::ReadUint32BE(conn_resp);
        uint32_t resp_tx = PeerWireProtocol::ReadUint32BE(conn_resp + 4);
        if (resp_action != 0 || resp_tx != transaction_id) {
            closesocket(sock);
            freeaddrinfo(res);
            return Result<TrackerAnnounceResponse>::Err("ERR_TORRENT_TRACKER_PROTOCOL", "UDP tracker invalid connect reply");
        }

        uint64_t connection_id = ReadUint64BE(conn_resp + 8);

        // 3. Announce Request (98 bytes)
        // 0       int64_t connection_id
        // 8       int32_t action          1 // announce
        // 12      int32_t transaction_id
        // 16      int8_t[20] info_hash
        // 36      int8_t[20] peer_id
        // 56      int64_t downloaded
        // 64      int64_t left
        // 72      int64_t uploaded
        // 80      int32_t event           0: none, 1: completed, 2: started, 3: stopped
        // 84      uint32_t IP address     0 // default
        // 88      uint32_t key            unique key
        // 92      int32_t num_want        -1 // default
        // 96      uint16_t port
        uint8_t ann_req[98];
        WriteUint64BE(ann_req, connection_id);
        PeerWireProtocol::WriteUint32BE(ann_req + 8, 1); // action: 1 (announce)
        transaction_id = dist(gen);
        PeerWireProtocol::WriteUint32BE(ann_req + 12, transaction_id);
        std::memcpy(ann_req + 16, info_hash_raw.data(), 20);
        std::memcpy(ann_req + 36, peer_id.data(), std::min<size_t>(20, peer_id.size()));
        WriteUint64BE(ann_req + 56, downloaded);
        WriteUint64BE(ann_req + 64, left);
        WriteUint64BE(ann_req + 72, uploaded);

        uint32_t ev_code = 0;
        if (event == "completed") ev_code = 1;
        else if (event == "started") ev_code = 2;
        else if (event == "stopped") ev_code = 3;
        PeerWireProtocol::WriteUint32BE(ann_req + 80, ev_code);
        PeerWireProtocol::WriteUint32BE(ann_req + 84, 0); // IP address default
        PeerWireProtocol::WriteUint32BE(ann_req + 88, dist(gen)); // key
        PeerWireProtocol::WriteUint32BE(ann_req + 92, static_cast<uint32_t>(-1)); // num_want default
        PeerWireProtocol::WriteUint16BE(ann_req + 96, port);

        if (sendto(sock, reinterpret_cast<const char*>(ann_req), 98, 0, res->ai_addr, static_cast<int>(res->ai_addrlen)) <= 0) {
            closesocket(sock);
            freeaddrinfo(res);
            return Result<TrackerAnnounceResponse>::Err("ERR_TORRENT_TRACKER_SEND", "Failed to send UDP announce request");
        }

        // 4. Announce Response (min 20 bytes)
        // 0       int32_t action          1 // announce
        // 4       int32_t transaction_id
        // 8       int32_t interval
        // 12      int32_t leechers
        // 16      int32_t seeders
        // 20 + 6 * n  IP + Port (compact)
        std::vector<uint8_t> ann_resp(4096);
        r = recvfrom(sock, reinterpret_cast<char*>(ann_resp.data()), static_cast<int>(ann_resp.size()), 0, nullptr, nullptr);
        closesocket(sock);
        freeaddrinfo(res);

        if (r < 20) {
            return Result<TrackerAnnounceResponse>::Err("ERR_TORRENT_TRACKER_TIMEOUT", "UDP tracker announce timeout or truncated response");
        }

        resp_action = PeerWireProtocol::ReadUint32BE(ann_resp.data());
        resp_tx = PeerWireProtocol::ReadUint32BE(ann_resp.data() + 4);

        if (resp_action == 3) { // Error
            std::string err_msg(reinterpret_cast<const char*>(ann_resp.data() + 8), r - 8);
            return Result<TrackerAnnounceResponse>::Err("ERR_TORRENT_TRACKER_FAILURE", err_msg);
        }

        if (resp_action != 1 || resp_tx != transaction_id) {
            return Result<TrackerAnnounceResponse>::Err("ERR_TORRENT_TRACKER_PROTOCOL", "UDP tracker invalid announce reply");
        }

        TrackerAnnounceResponse tr;
        tr.ok = true;
        tr.interval = PeerWireProtocol::ReadUint32BE(ann_resp.data() + 8);
        tr.incomplete = PeerWireProtocol::ReadUint32BE(ann_resp.data() + 12);
        tr.complete = PeerWireProtocol::ReadUint32BE(ann_resp.data() + 16);

        size_t peers_bytes = r - 20;
        if (peers_bytes >= 6) {
            tr.peers = PeerWireProtocol::ExtractPeersFromCompact(
                std::string_view(reinterpret_cast<const char*>(ann_resp.data() + 20), peers_bytes));
        }

        return Result<TrackerAnnounceResponse>::Ok(std::move(tr));
    }

    // Announces to tracker list with automatic HTTP/HTTPS/UDP tier failover and peer aggregation
    static Result<TrackerAnnounceResponse> AnnounceAny(
        const std::vector<std::string>& announce_urls,
        const std::vector<uint8_t>& info_hash_raw,
        const std::string& peer_id,
        uint16_t port,
        uint64_t uploaded,
        uint64_t downloaded,
        uint64_t left,
        std::string_view event = "started",
        uint32_t timeout_ms = 4000) {

        if (announce_urls.empty()) {
            return Result<TrackerAnnounceResponse>::Err("ERR_TORRENT_NO_TRACKERS", "No announce tracker URLs provided");
        }

        TrackerAnnounceResponse merged_tr;
        merged_tr.ok = false;
        std::set<std::string> seen_peers;
        std::string last_error = "All announce trackers failed";
        size_t success_trackers = 0;

        for (const auto& url : announce_urls) {
            Result<TrackerAnnounceResponse> res = Result<TrackerAnnounceResponse>::Err("ERR_TORRENT_TRACKER_UNSUPPORTED", "Unsupported protocol");
            if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) {
                OPENIDM_LOG_INFO("TrackerClient", "Attempting announce to HTTP tracker: " + url);
                res = AnnounceHttp(url, info_hash_raw, peer_id, port, uploaded, downloaded, left, event, timeout_ms);
            } else if (url.rfind("udp://", 0) == 0) {
                OPENIDM_LOG_INFO("TrackerClient", "Attempting announce to UDP tracker: " + url);
                res = AnnounceUdp(url, info_hash_raw, peer_id, port, uploaded, downloaded, left, event, timeout_ms);
            }

            if (res.ok()) {
                success_trackers++;
                merged_tr.ok = true;
                if (res.value->interval > 0) merged_tr.interval = res.value->interval;
                merged_tr.complete = std::max(merged_tr.complete, res.value->complete);
                merged_tr.incomplete = std::max(merged_tr.incomplete, res.value->incomplete);

                size_t new_peers = 0;
                for (auto& p : res.value->peers) {
                    std::string key = p.ip + ":" + std::to_string(p.port);
                    if (seen_peers.insert(key).second) {
                        merged_tr.peers.push_back(std::move(p));
                        new_peers++;
                    }
                }
                OPENIDM_LOG_INFO("TrackerClient", "Tracker " + url + " succeeded: returned " +
                                                   std::to_string(res.value->peers.size()) + " peers (" +
                                                   std::to_string(new_peers) + " new, total pool: " +
                                                   std::to_string(merged_tr.peers.size()) + ")");

                // If we have collected at least 50 peers, we can stop querying remaining trackers to save bandwidth
                if (merged_tr.peers.size() >= 50) {
                    OPENIDM_LOG_INFO("TrackerClient", "Sufficient peers collected (" + std::to_string(merged_tr.peers.size()) + "); stopping tracker queries");
                    break;
                }
            } else {
                last_error = res.error->message;
                OPENIDM_LOG_WARN("TrackerClient", "Tracker announce failed for " + url + ": " + last_error);
            }
        }

        if (success_trackers > 0 || !merged_tr.peers.empty()) {
            return Result<TrackerAnnounceResponse>::Ok(std::move(merged_tr));
        }

        return Result<TrackerAnnounceResponse>::Err("ERR_TORRENT_TRACKER_UNAVAILABLE", last_error);
    }
};

} // namespace openidm::torrent
