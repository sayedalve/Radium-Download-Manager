// SPDX-License-Identifier: GPL-3.0-or-later
// OpenIDM / Flux — BitTorrent DHT Client (BEP 5 Kademlia DHT Iterative Peer Discovery)
#pragma once

#include <chrono>
#include <cstdint>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define closesocket close
#endif

#include "common/logger.hpp"
#include "common/types.hpp"
#include "engine_torrent/bencode.hpp"
#include "engine_torrent/peer_wire_protocol.hpp"

namespace openidm::torrent {

class DhtClient {
public:
    static std::vector<PeerEndpoint> QueryDhtNodes(
        const std::vector<std::pair<std::string, uint16_t>>& bootstrap_nodes,
        const std::vector<uint8_t>& info_hash_raw,
        const std::string& my_node_id,
        uint32_t timeout_ms = 3000) {

        std::vector<PeerEndpoint> found_peers;
        if (info_hash_raw.size() < 20 || bootstrap_nodes.empty()) return found_peers;

        SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) return found_peers;

        // Bind socket to 0.0.0.0:0 so it can receive responses reliably
        sockaddr_in local_bind{};
        local_bind.sin_family = AF_INET;
        local_bind.sin_addr.s_addr = htonl(INADDR_ANY);
        local_bind.sin_port = htons(0);
        bind(sock, reinterpret_cast<sockaddr*>(&local_bind), sizeof(local_bind));

#ifdef _WIN32
        // Disable WSAECONNRESET from receiving ICMP port unreachable packets on Windows
        #ifndef SIO_UDP_CONNRESET
        #define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
        #endif
        BOOL bNewBehavior = FALSE;
        DWORD dwBytesReturned = 0;
        WSAIoctl(sock, SIO_UDP_CONNRESET, &bNewBehavior, sizeof(bNewBehavior), NULL, 0, &dwBytesReturned, NULL, NULL);

        DWORD to_val = 50; // 50ms fast polling per recv cycle
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to_val), sizeof(to_val));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to_val), sizeof(to_val));
#else
        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

        std::string info_hash_str(reinterpret_cast<const char*>(info_hash_raw.data()), 20);

        std::set<std::string> queried_nodes;
        std::set<std::string> outstanding_transactions;
        std::set<std::string> seen_peers;
        std::vector<std::pair<std::string, uint16_t>> pending_nodes = bootstrap_nodes;
        uint32_t transaction_counter = 0;

        auto send_query_to = [&](const std::string& host, uint16_t port) {
            std::string key = host + ":" + std::to_string(port);
            if (queried_nodes.count(key)) return;
            queried_nodes.insert(key);

            std::string transaction_id = "g" + std::to_string(++transaction_counter);
            outstanding_transactions.insert(transaction_id);
            BencodeValue query_dict = BencodeValue::MakeDict();
            query_dict.as_dict()["t"] = BencodeValue(transaction_id);
            query_dict.as_dict()["y"] = BencodeValue("q");
            query_dict.as_dict()["q"] = BencodeValue("get_peers");
            BencodeValue args = BencodeValue::MakeDict();
            args.as_dict()["id"] = BencodeValue(my_node_id);
            args.as_dict()["info_hash"] = BencodeValue(info_hash_str);
            query_dict.as_dict()["a"] = std::move(args);
            std::string query_bencoded = BencodeParser::Encode(query_dict);

            // Fast path: if host is an IPv4 string, use inet_pton directly without DNS overhead
            sockaddr_in dest{};
            dest.sin_family = AF_INET;
            dest.sin_port = htons(port);
            if (inet_pton(AF_INET, host.c_str(), &dest.sin_addr) == 1) {
                sendto(sock, query_bencoded.data(), static_cast<int>(query_bencoded.size()), 0,
                       reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
                return;
            }

            // Fallback: DNS resolution for domain names
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            hints.ai_protocol = IPPROTO_UDP;

            addrinfo* res = nullptr;
            std::string port_str = std::to_string(port);
            if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) == 0 && res) {
                sendto(sock, query_bencoded.data(), static_cast<int>(query_bencoded.size()), 0, res->ai_addr, static_cast<int>(res->ai_addrlen));
                freeaddrinfo(res);
            }
        };

        // Query initial bootstrap tier
        for (const auto& [host, port] : pending_nodes) {
            send_query_to(host, port);
        }
        pending_nodes.clear();

        std::vector<uint8_t> recv_buf(4096);
        auto start = std::chrono::steady_clock::now();

        // Iterative lookup loop (up to timeout_ms or until sufficient peers found)
        while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() < timeout_ms) {
            sockaddr_in from_addr{};
            int from_len = sizeof(from_addr);
            int r = recvfrom(sock, reinterpret_cast<char*>(recv_buf.data()), static_cast<int>(recv_buf.size()), 0,
                             reinterpret_cast<sockaddr*>(&from_addr), &from_len);
            if (r > 0) {
                std::string_view resp_sv(reinterpret_cast<const char*>(recv_buf.data()), r);
                auto dec_res = BencodeParser::Decode(resp_sv);
                if (dec_res.ok() && dec_res.value->is_dict()) {
                    const auto& d = dec_res.value.value();
                    if (!d.contains("t") || !d["t"].is_string() ||
                        !outstanding_transactions.erase(d["t"].as_string())) {
                        OPENIDM_LOG_WARN("DhtClient", "Ignoring DHT response with unknown transaction id");
                        continue;
                    }
                    if (d.contains("r") && d["r"].is_dict()) {
                        const auto& r_dict = d["r"];
                        // Case 1: direct peer values list: "values": ["compact1", "compact2"]
                        if (r_dict.contains("values") && r_dict["values"].is_list()) {
                            for (const auto& v : r_dict["values"].as_list()) {
                                if (v.is_string()) {
                                    auto peers = PeerWireProtocol::ExtractPeersFromCompact(v.as_string());
                                    for (auto& p : peers) {
                                        std::string pkey = p.ip + ":" + std::to_string(p.port);
                                        if (seen_peers.insert(pkey).second) {
                                            found_peers.push_back(std::move(p));
                                        }
                                    }
                                }
                            }
                        }
                        // Case 2: Iterative node discovery: compact 26-byte nodes (20-byte ID + 6-byte IP/port)
                        if (r_dict.contains("nodes") && r_dict["nodes"].is_string()) {
                            const std::string& nodes_str = r_dict["nodes"].as_string();
                            if (nodes_str.size() % 26 != 0) {
                                OPENIDM_LOG_WARN("DhtClient", "Ignoring malformed compact node payload of " + std::to_string(nodes_str.size()) + " bytes");
                                continue;
                            }
                            size_t n_cnt = nodes_str.size() / 26;
                            for (size_t i = 0; i < n_cnt && i < 16; ++i) {
                                const uint8_t* p = reinterpret_cast<const uint8_t*>(nodes_str.data() + (i * 26) + 20);
                                auto [ip, port] = PeerWireProtocol::UnpackCompactIpv4(p);
                                if (!ip.empty() && port > 0) {
                                    // Query newly discovered closest node recursively
                                    send_query_to(ip, port);
                                }
                            }
                        }
                    }
                }
            }
            if (found_peers.size() >= 50) break;
        }

        closesocket(sock);
        return found_peers;
    }
};

} // namespace openidm::torrent
