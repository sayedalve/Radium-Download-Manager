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

#include "engine_torrent/peer_wire_protocol.hpp"

namespace openidm::torrent::testing {

class MockPeerServer {
public:
    MockPeerServer(uint16_t port, const std::vector<uint8_t>& info_hash, const std::vector<uint8_t>& payload)
        : m_port(port), m_info_hash(info_hash), m_payload(payload), m_running(false) {}

    ~MockPeerServer() {
        Stop();
    }

    void Start() {
        m_running = true;
        m_thread = std::thread([this]() { Run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    void Stop() {
        m_running = false;
        if (m_listen_sock != INVALID_SOCKET) {
            closesocket(m_listen_sock);
            m_listen_sock = INVALID_SOCKET;
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    uint16_t Port() const { return m_port; }

private:
    void Run() {
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
        if (hs_len < 68) {
            closesocket(client);
            return;
        }

        // Send back handshake
        std::string peer_id = "-FL0100-MOCKPEER0001";
        auto resp_hs = PeerWireProtocol::CreateHandshake(m_info_hash, peer_id);
        send(client, reinterpret_cast<const char*>(resp_hs.data()), static_cast<int>(resp_hs.size()), 0);

        // Send Bitfield: 1 piece (all complete)
        std::vector<bool> pieces = {true};
        auto bf = PeerWireProtocol::CreateBitfield(pieces);
        send(client, reinterpret_cast<const char*>(bf.data()), static_cast<int>(bf.size()), 0);

        // Send Unchoke
        auto unchoke = PeerWireProtocol::CreateUnchoke();
        send(client, reinterpret_cast<const char*>(unchoke.data()), static_cast<int>(unchoke.size()), 0);

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

                if (msg->id == BtMessageId::kRequest) {
                    uint32_t p_idx = msg->piece_index;
                    uint32_t begin = msg->block_begin;
                    uint32_t len = msg->block_length;
                    if (begin + len <= m_payload.size()) {
                        auto p_msg = PeerWireProtocol::CreatePiece(p_idx, begin, m_payload.data() + begin, len);
                        send(client, reinterpret_cast<const char*>(p_msg.data()), static_cast<int>(p_msg.size()), 0);
                    }
                }
            }
        }

        closesocket(client);
    }

    uint16_t m_port;
    std::vector<uint8_t> m_info_hash;
    std::vector<uint8_t> m_payload;
    std::atomic<bool> m_running;
    SOCKET m_listen_sock{INVALID_SOCKET};
    std::thread m_thread;
};

} // namespace openidm::torrent::testing
