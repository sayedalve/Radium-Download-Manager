#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define closesocket close
#endif

#include "engine_torrent/bencode.hpp"
#include "engine_torrent/peer_wire_protocol.hpp"
#include "engine_torrent/tracker_client.hpp"

namespace openidm::torrent::testing {

// 1. Mock BEP 15 UDP Tracker Server
class MockUdpTrackerServer {
public:
    explicit MockUdpTrackerServer(uint16_t port) : m_port(port), m_running(false), m_sock(INVALID_SOCKET) {}
    ~MockUdpTrackerServer() { Stop(); }

    void Start() {
        m_running = true;
        m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_sock == INVALID_SOCKET) return;

        sockaddr_in sin{};
        sin.sin_family = AF_INET;
        sin.sin_port = htons(m_port);
        sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(m_sock, reinterpret_cast<sockaddr*>(&sin), sizeof(sin)) != 0) return;

        m_thread = std::thread([this]() { Run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    void Stop() {
        m_running = false;
        if (m_sock != INVALID_SOCKET) {
            closesocket(m_sock);
            m_sock = INVALID_SOCKET;
        }
        if (m_thread.joinable()) m_thread.join();
    }

private:
    void Run() {
        uint8_t buf[2048];
        while (m_running) {
            sockaddr_in client{};
            int clen = sizeof(client);
            int r = recvfrom(m_sock, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&client), &clen);
            if (r <= 0) break;

            if (r >= 16) {
                uint64_t proto_or_conn = TrackerClient::ReadUint64BE(buf);
                (void)proto_or_conn;
                uint32_t action = PeerWireProtocol::ReadUint32BE(buf + 8);
                uint32_t tx_id = PeerWireProtocol::ReadUint32BE(buf + 12);

                if (action == 0) { // Connect
                    uint8_t resp[16];
                    PeerWireProtocol::WriteUint32BE(resp, 0); // action: connect
                    PeerWireProtocol::WriteUint32BE(resp + 4, tx_id);
                    TrackerClient::WriteUint64BE(resp + 8, 0x1122334455667788ULL); // connection_id
                    sendto(m_sock, reinterpret_cast<const char*>(resp), 16, 0, reinterpret_cast<sockaddr*>(&client), clen);
                } else if (action == 1) { // Announce
                    uint8_t resp[26];
                    PeerWireProtocol::WriteUint32BE(resp, 1); // action: announce
                    PeerWireProtocol::WriteUint32BE(resp + 4, tx_id);
                    PeerWireProtocol::WriteUint32BE(resp + 8, 1800); // interval
                    PeerWireProtocol::WriteUint32BE(resp + 12, 1);   // leechers
                    PeerWireProtocol::WriteUint32BE(resp + 16, 5);   // seeders
                    // 1 compact peer: 127.0.0.1:16881
                    resp[20] = 127; resp[21] = 0; resp[22] = 0; resp[23] = 1;
                    PeerWireProtocol::WriteUint16BE(resp + 24, 16881);
                    sendto(m_sock, reinterpret_cast<const char*>(resp), 26, 0, reinterpret_cast<sockaddr*>(&client), clen);
                }
            }
        }
    }

    uint16_t m_port;
    std::atomic<bool> m_running;
    SOCKET m_sock;
    std::thread m_thread;
};

// 2. Mock BEP 9 Magnet Metadata Exchange Peer
class MockMagnetMetadataPeerServer {
public:
    MockMagnetMetadataPeerServer(uint16_t port, const std::vector<uint8_t>& info_hash, std::string info_dict_bencode)
        : m_port(port), m_info_hash(info_hash), m_info_bencode(std::move(info_dict_bencode)), m_running(false), m_listen_sock(INVALID_SOCKET) {}
    ~MockMagnetMetadataPeerServer() { Stop(); }

    void Start() {
        m_running = true;
        m_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_listen_sock == INVALID_SOCKET) return;

        int opt = 1;
        setsockopt(m_listen_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

        sockaddr_in sin{};
        sin.sin_family = AF_INET;
        sin.sin_port = htons(m_port);
        sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(m_listen_sock, reinterpret_cast<sockaddr*>(&sin), sizeof(sin)) != 0) return;
        if (listen(m_listen_sock, 1) != 0) return;

        m_thread = std::thread([this]() { Run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    void Stop() {
        m_running = false;
        if (m_listen_sock != INVALID_SOCKET) {
            closesocket(m_listen_sock);
            m_listen_sock = INVALID_SOCKET;
        }
        if (m_thread.joinable()) m_thread.join();
    }

private:
    void Run() {
        SOCKET client = accept(m_listen_sock, nullptr, nullptr);
        if (client == INVALID_SOCKET) return;

        // 1. Handshake
        uint8_t hs[68];
        int hs_len = 0;
        while (hs_len < 68 && m_running) {
            int r = recv(client, reinterpret_cast<char*>(hs + hs_len), 68 - hs_len, 0);
            if (r <= 0) break;
            hs_len += r;
        }
        if (hs_len < 68) { closesocket(client); return; }

        // Send back Handshake
        auto resp_hs = PeerWireProtocol::CreateHandshake(m_info_hash, "-FL0100-MOCKMAG0001");
        send(client, reinterpret_cast<const char*>(resp_hs.data()), static_cast<int>(resp_hs.size()), 0);

        // Send BEP 10 Extended Handshake advertising ut_metadata = 1 and metadata_size
        auto ext_hs = PeerWireProtocol::CreateExtendedHandshake(1, m_info_bencode.size());
        send(client, reinterpret_cast<const char*>(ext_hs.data()), static_cast<int>(ext_hs.size()), 0);

        std::vector<uint8_t> buf;
        buf.reserve(65536);

        while (m_running) {
            uint8_t temp[4096];
            int r = recv(client, reinterpret_cast<char*>(temp), sizeof(temp), 0);
            if (r <= 0) break;
            buf.insert(buf.end(), temp, temp + r);

            while (buf.size() >= 4) {
                uint32_t payload_len = PeerWireProtocol::ReadUint32BE(buf.data());
                if (payload_len == 0) {
                    buf.erase(buf.begin(), buf.begin() + 4);
                    continue;
                }
                if (buf.size() < 4 + payload_len) break;

                auto msg = PeerWireProtocol::ParseMessage(buf.data(), 4 + payload_len);
                buf.erase(buf.begin(), buf.begin() + 4 + payload_len);
                if (!msg) continue;

                if (msg->id == BtMessageId::kExtended && msg->ext_id == 1) {
                    // Send BEP 9 ut_metadata Data piece 0
                    auto data_msg = PeerWireProtocol::CreateUtMetadataData(
                        1, 0, m_info_bencode.size(),
                        reinterpret_cast<const uint8_t*>(m_info_bencode.data()), m_info_bencode.size());
                    send(client, reinterpret_cast<const char*>(data_msg.data()), static_cast<int>(data_msg.size()), 0);
                }
            }
        }
        closesocket(client);
    }

    uint16_t m_port;
    std::vector<uint8_t> m_info_hash;
    std::string m_info_bencode;
    std::atomic<bool> m_running;
    SOCKET m_listen_sock;
    std::thread m_thread;
};

// 3. Mock BEP 5 DHT Node Server
class MockDhtNodeServer {
public:
    explicit MockDhtNodeServer(uint16_t port) : m_port(port), m_running(false), m_sock(INVALID_SOCKET) {}
    ~MockDhtNodeServer() { Stop(); }

    void Start() {
        m_running = true;
        m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_sock == INVALID_SOCKET) return;

        sockaddr_in sin{};
        sin.sin_family = AF_INET;
        sin.sin_port = htons(m_port);
        sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(m_sock, reinterpret_cast<sockaddr*>(&sin), sizeof(sin)) != 0) return;

        m_thread = std::thread([this]() { Run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    void Stop() {
        m_running = false;
        if (m_sock != INVALID_SOCKET) {
            closesocket(m_sock);
            m_sock = INVALID_SOCKET;
        }
        if (m_thread.joinable()) m_thread.join();
    }

private:
    void Run() {
        uint8_t buf[2048];
        while (m_running) {
            sockaddr_in client{};
            int clen = sizeof(client);
            int r = recvfrom(m_sock, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&client), &clen);
            if (r <= 0) break;

            std::string_view req_sv(reinterpret_cast<const char*>(buf), r);
            auto dec = BencodeParser::Decode(req_sv);
            if (dec.ok() && dec.value->is_dict()) {
                const auto& d = dec.value.value();
                std::string tx = d.contains("t") ? d["t"].as_string() : "gp";

                // Return BEP 5 response with 1 compact peer: 127.0.0.1:16881
                BencodeValue resp = BencodeValue::MakeDict();
                resp.as_dict()["t"] = BencodeValue(tx);
                resp.as_dict()["y"] = BencodeValue("r");

                BencodeValue r_dict = BencodeValue::MakeDict();
                r_dict.as_dict()["id"] = BencodeValue(std::string(20, 'D'));

                BencodeValue vals = BencodeValue::MakeList();
                uint8_t peer_comp[6] = {127, 0, 0, 1, static_cast<uint8_t>(16881 >> 8), static_cast<uint8_t>(16881 & 0xFF)};
                vals.as_list().push_back(BencodeValue(std::string(reinterpret_cast<const char*>(peer_comp), 6)));
                r_dict.as_dict()["values"] = std::move(vals);

                resp.as_dict()["r"] = std::move(r_dict);
                std::string enc = BencodeParser::Encode(resp);
                sendto(m_sock, enc.data(), static_cast<int>(enc.size()), 0, reinterpret_cast<sockaddr*>(&client), clen);
            }
        }
    }

    uint16_t m_port;
    std::atomic<bool> m_running;
    SOCKET m_sock;
    std::thread m_thread;
};

} // namespace openidm::torrent::testing
