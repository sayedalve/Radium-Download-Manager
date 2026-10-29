// SPDX-License-Identifier: Apache-2.0
// OpenIDM / Flux — BitTorrent Engine Coordinator
// Concurrent peer swarm, BEP 9 metadata exchange, BEP 15 UDP trackers, DHT peer discovery, Seeding & Restart Recovery
#include "engine_torrent/torrent_engine.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <set>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
#endif

#include "common/logger.hpp"
#include "common/path_sanitizer.hpp"
#include "engine_torrent/dht_client.hpp"
#include "engine_torrent/multi_file_storage.hpp"
#include "engine_torrent/peer_wire_protocol.hpp"
#include "engine_torrent/piece_manager.hpp"
#include "engine_torrent/tracker_client.hpp"

namespace openidm::torrent {

namespace {

#ifdef _WIN32
struct WinSockInit {
    WinSockInit() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~WinSockInit() {
        WSACleanup();
    }
};

void EnsureWinSock() {
    static WinSockInit s_init;
}
#else
void EnsureWinSock() {}
#endif

struct UniqueSocket {
    SOCKET s{INVALID_SOCKET};
    UniqueSocket() = default;
    explicit UniqueSocket(SOCKET sock) : s(sock) {}
    ~UniqueSocket() {
        if (s != INVALID_SOCKET) {
            closesocket(s);
        }
    }
    UniqueSocket(const UniqueSocket&) = delete;
    UniqueSocket& operator=(const UniqueSocket&) = delete;
    UniqueSocket(UniqueSocket&& o) noexcept : s(o.s) { o.s = INVALID_SOCKET; }
    UniqueSocket& operator=(UniqueSocket&& o) noexcept {
        if (s != INVALID_SOCKET) closesocket(s);
        s = o.s;
        o.s = INVALID_SOCKET;
        return *this;
    }
};

static bool SendAll(SOCKET s, const uint8_t* data, size_t len) {
    size_t total_sent = 0;
    while (total_sent < len) {
        int r = send(s, reinterpret_cast<const char*>(data + total_sent), static_cast<int>(len - total_sent), 0);
        if (r <= 0) return false;
        total_sent += static_cast<size_t>(r);
    }
    return true;
}

} // namespace

Result<void> TorrentEngine::ExecuteTorrentDownload(
    TaskModel& task,
    TorrentInfo& torrent_info,
    std::atomic<bool>& pause_requested,
    std::atomic<bool>& cancel_requested,
    std::atomic<uint64_t>& speed_limit_bps,
    TorrentProgressCallback on_progress,
    std::shared_ptr<brisk::TokenBucketLimiter> global_limiter,
    std::atomic<uint64_t>* global_speed_limit_bps,
    uint32_t max_peers,
    bool seed_after_completion,
    double max_seed_ratio,
    uint32_t max_seed_time_sec,
    TorrentMetadataSelectionCallback on_metadata) {

    EnsureWinSock();

    std::string client_peer_id = PeerWireProtocol::GeneratePeerId();

    // 0. Magnet link: exchange metadata via BEP 9 (ut_metadata) if needed
    if (torrent_info.is_magnet && (torrent_info.total_length == 0 || torrent_info.num_pieces() == 0)) {
        task.status_detail = "Fetching magnet metadata via BEP 9...";
        if (on_progress) {
            TorrentProgress tp;
            tp.task_id = task.task_id;
            tp.state = TaskState::kDownloading;
            tp.status_detail = task.status_detail;
            on_progress(tp);
        }

        std::vector<std::string> ann_urls = torrent_info.announce_list;
        if (ann_urls.empty() && !torrent_info.announce.empty()) ann_urls.push_back(torrent_info.announce);

        // Tracker announce for magnet
        std::vector<PeerEndpoint> magnet_peers;
        if (!ann_urls.empty()) {
            auto tr_res = TrackerClient::AnnounceAny(ann_urls, torrent_info.info_hash_raw, client_peer_id, 6881, 0, 0, 0, "started", 4000);
            if (tr_res.ok()) magnet_peers = tr_res.value->peers;
        }

        // Query DHT bootstrap nodes if tracker gave no peers
        if (magnet_peers.empty()) {
            std::vector<std::pair<std::string, uint16_t>> bootstrap_nodes = {
                {std::string("dht.transmissionbt.com"), static_cast<uint16_t>(6881)},
                {std::string("dht.libtorrent.org"), static_cast<uint16_t>(25401)},
                {std::string("router.bittorrent.com"), static_cast<uint16_t>(6881)},
                {std::string("router.utorrent.com"), static_cast<uint16_t>(6881)}
            };
            magnet_peers = DhtClient::QueryDhtNodes(bootstrap_nodes, torrent_info.info_hash_raw, client_peer_id, 3000);
        }

        std::string fetched_metadata_bencode;
        for (const auto& peer : magnet_peers) {
            if (cancel_requested.load() || pause_requested.load()) break;

            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET) continue;
            UniqueSocket sock(s);

#ifdef _WIN32
            DWORD to_ms = 4000;
            setsockopt(sock.s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to_ms), sizeof(to_ms));
            setsockopt(sock.s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to_ms), sizeof(to_ms));
#else
            struct timeval tv;
            tv.tv_sec = 4;
            tv.tv_usec = 0;
            setsockopt(sock.s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(sock.s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

            sockaddr_in saddr{};
            saddr.sin_family = AF_INET;
            saddr.sin_port = htons(peer.port);
            if (inet_pton(AF_INET, peer.ip.c_str(), &saddr.sin_addr) <= 0) continue;
            if (connect(sock.s, reinterpret_cast<sockaddr*>(&saddr), sizeof(saddr)) == SOCKET_ERROR) continue;

            auto hs = PeerWireProtocol::CreateHandshake(torrent_info.info_hash_raw, client_peer_id);
            if (send(sock.s, reinterpret_cast<const char*>(hs.data()), static_cast<int>(hs.size()), 0) <= 0) continue;

            uint8_t peer_hs[68];
            int hs_read = 0;
            while (hs_read < 68) {
                int r = recv(sock.s, reinterpret_cast<char*>(peer_hs + hs_read), 68 - hs_read, 0);
                if (r <= 0) break;
                hs_read += r;
            }
            if (!PeerWireProtocol::VerifyHandshake(peer_hs, hs_read, torrent_info.info_hash_raw)) continue;

            // Send BEP 10 extended handshake advertising ut_metadata support
            auto ext_hs = PeerWireProtocol::CreateExtendedHandshake(1, 0);
            send(sock.s, reinterpret_cast<const char*>(ext_hs.data()), static_cast<int>(ext_hs.size()), 0);

            uint8_t peer_ut_metadata_id = 0;
            uint64_t metadata_size = 0;
            std::vector<uint8_t> recv_buf;
            recv_buf.reserve(65536);
            std::vector<uint8_t> metadata_bytes;

            while (!cancel_requested.load() && !pause_requested.load()) {
                uint8_t temp[16384];
                int r = recv(sock.s, reinterpret_cast<char*>(temp), sizeof(temp), 0);
                if (r <= 0) break;
                recv_buf.insert(recv_buf.end(), temp, temp + r);

                bool got_all_metadata = false;
                while (recv_buf.size() >= 4) {
                    uint32_t payload_len = PeerWireProtocol::ReadUint32BE(recv_buf.data());
                    if (payload_len == 0) {
                        recv_buf.erase(recv_buf.begin(), recv_buf.begin() + 4);
                        continue;
                    }
                    if (recv_buf.size() < 4 + payload_len) break;

                    auto msg_opt = PeerWireProtocol::ParseMessage(recv_buf.data(), 4 + payload_len);
                    recv_buf.erase(recv_buf.begin(), recv_buf.begin() + 4 + payload_len);
                    if (!msg_opt) continue;

                    if (msg_opt->id == BtMessageId::kExtended) {
                        if (msg_opt->ext_id == kExtHandshakeId) {
                            // Parse extension handshake dictionary
                            std::string_view hs_str(reinterpret_cast<const char*>(msg_opt->payload.data()), msg_opt->payload.size());
                            auto dec_hs = BencodeParser::Decode(hs_str);
                            if (dec_hs.ok() && dec_hs.value->is_dict()) {
                                const auto& hsd = dec_hs.value.value();
                                if (hsd.contains("m") && hsd["m"].is_dict() && hsd["m"].contains("ut_metadata") && hsd["m"]["ut_metadata"].is_int()) {
                                    peer_ut_metadata_id = static_cast<uint8_t>(hsd["m"]["ut_metadata"].as_int());
                                }
                                if (hsd.contains("metadata_size") && hsd["metadata_size"].is_int()) {
                                    metadata_size = static_cast<uint64_t>(hsd["metadata_size"].as_int());
                                }
                            }
                            if (peer_ut_metadata_id > 0 && metadata_size > 0 && metadata_size <= 10 * 1024 * 1024) {
                                metadata_bytes.resize(static_cast<size_t>(metadata_size));
                                // Request piece 0 (or all pieces)
                                uint32_t num_pieces = static_cast<uint32_t>((metadata_size + 16383) / 16384);
                                for (uint32_t pi = 0; pi < num_pieces; ++pi) {
                                    auto ut_req = PeerWireProtocol::CreateUtMetadataRequest(peer_ut_metadata_id, pi);
                                    send(sock.s, reinterpret_cast<const char*>(ut_req.data()), static_cast<int>(ut_req.size()), 0);
                                }
                            }
                        } else if (msg_opt->ext_id == 1 || msg_opt->ext_id == peer_ut_metadata_id) {
                            // ut_metadata data message
                            std::string_view msg_sv(reinterpret_cast<const char*>(msg_opt->payload.data()), msg_opt->payload.size());
                            size_t pos = 0;
                            if (TorrentParser::GetBencodeValueRange(msg_sv, pos)) {
                                auto d_res = BencodeParser::Decode(msg_sv.substr(0, pos));
                                if (d_res.ok() && d_res.value->is_dict()) {
                                    const auto& md = d_res.value.value();
                                    if (md.contains("msg_type") && md["msg_type"].as_int() == kUtMetadataData) {
                                        uint32_t piece_idx = static_cast<uint32_t>(md.contains("piece") ? md["piece"].as_int() : 0);
                                        const uint8_t* raw_data = msg_opt->payload.data() + pos;
                                        size_t raw_len = msg_opt->payload.size() - pos;
                                        size_t offset = piece_idx * 16384;
                                        if (offset + raw_len <= metadata_bytes.size()) {
                                            std::memcpy(metadata_bytes.data() + offset, raw_data, raw_len);
                                        }
                                        if (piece_idx + 1 >= (metadata_size + 16383) / 16384) {
                                            got_all_metadata = true;
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                if (got_all_metadata) break;
            }

            if (!metadata_bytes.empty()) {
                auto parsed_res = TorrentParser::ParseInfoDictionary(
                    std::string_view(reinterpret_cast<const char*>(metadata_bytes.data()), metadata_bytes.size()),
                    torrent_info.announce,
                    torrent_info.announce_list);
                if (parsed_res.ok()) {
                    torrent_info = parsed_res.value.value();
                    break;
                }
            }
        }

        if (torrent_info.total_length == 0 || torrent_info.num_pieces() == 0) {
            task.state = TaskState::kFailed;
            task.error_code = "ERR_MAGNET_METADATA_UNSUPPORTED";
            task.error_message = "Failed to retrieve magnet metainfo from available peers or DHT";
            task.status_detail = "Failed: Magnet metadata retrieval failed";
            return Result<void>::Err("ERR_MAGNET_METADATA_UNSUPPORTED", task.error_message);
        }

        if (on_metadata) {
            task.status_detail = "Select files to download...";
            if (on_progress) {
                TorrentProgress tp;
                tp.task_id = task.task_id;
                tp.state = TaskState::kDownloading;
                tp.total_bytes = torrent_info.selected_bytes();
                tp.total_pieces = static_cast<uint32_t>(torrent_info.num_pieces());
                tp.status_detail = task.status_detail;
                on_progress(tp);
            }
            auto selection_res = on_metadata(torrent_info);
            if (!selection_res.ok()) {
                task.state = TaskState::kPaused;
                task.pause_reason = PauseReason::kUser;
                task.error_code = selection_res.error->code;
                task.error_message = selection_res.error->message;
                task.status_detail = "Paused: " + selection_res.error->message;
                return selection_res;
            }
            // Persist the post-metadata selection on the task before any files are
            // allocated. The controller stores this model at worker completion,
            // allowing a restart to reconstruct the same selective torrent mask.
            task.torrent_file_paths.clear();
            task.torrent_file_selection.clear();
            task.torrent_file_paths.reserve(torrent_info.files.size());
            task.torrent_file_selection.reserve(torrent_info.files.size());
            for (const auto& file : torrent_info.files) {
                task.torrent_file_paths.push_back(file.relative_path.generic_string());
                task.torrent_file_selection.push_back(file.selected ? 1 : 0);
            }
        }
    }

    // 1. Metainfo Pre-flight Validation
    if (torrent_info.total_length == 0 || torrent_info.num_pieces() == 0 || torrent_info.piece_length == 0) {
        task.state = TaskState::kFailed;
        task.error_code = "ERR_TORRENT_MALFORMED";
        task.error_message = "Invalid torrent metainfo: zero length or zero pieces";
        task.status_detail = "Failed: Invalid metainfo";
        OPENIDM_LOG_ERROR("TorrentEngine", "Malformed metainfo for task " + task.task_id);
        return Result<void>::Err("ERR_TORRENT_MALFORMED", "Invalid torrent metainfo: zero length or zero pieces");
    }

    if (!torrent_info.has_selected_files()) {
        task.state = TaskState::kFailed;
        task.error_code = "ERR_TORRENT_NO_FILES_SELECTED";
        task.error_message = "At least one torrent file must be selected";
        task.status_detail = "Failed: No files selected";
        return Result<void>::Err(task.error_code, task.error_message);
    }
    uint64_t total_bytes = torrent_info.selected_bytes();
    task.total_bytes = total_bytes;
    task.total_segments = 0;
    task.state = TaskState::kDownloading;
    task.status_detail = "Allocating storage files...";

    if (on_progress) {
        TorrentProgress tp;
        tp.task_id = task.task_id;
        tp.state = task.state;
        tp.total_bytes = total_bytes;
        tp.total_pieces = static_cast<uint32_t>(torrent_info.num_pieces());
        tp.status_detail = task.status_detail;
        on_progress(tp);
    }

    // 2. Storage Setup (aria2 MultiDiskAdaptor)
    auto storage = std::make_shared<MultiFileStorage>();
    auto stor_res = storage->Initialize(task.save_dir, torrent_info);
    if (!stor_res.ok()) {
        task.state = TaskState::kFailed;
        task.error_code = stor_res.error->code;
        task.error_message = stor_res.error->message;
        task.status_detail = "Failed: " + stor_res.error->message;
        return stor_res;
    }
    storage->AllocateFiles();

    // 3. Piece & Block Scheduler (aria2 PieceStorage)
    auto piece_mgr = std::make_shared<PieceManager>(torrent_info, storage);
    task.total_segments = static_cast<uint32_t>(piece_mgr->RequiredPieces());

    // Resume / restart recovery: verify existing pieces on disk
    size_t recovered = piece_mgr->CheckExistingPieces();
    if (recovered > 0) {
        task.completed_segments = static_cast<uint32_t>(piece_mgr->CompletedPieces());
        task.downloaded_bytes = piece_mgr->DownloadedBytes();
        OPENIDM_LOG_INFO("TorrentEngine", "Recovered " + std::to_string(recovered) + " pieces from existing disk data for task " + task.task_id);
    }

    OPENIDM_LOG_INFO("TorrentEngine", "Starting BitTorrent download for task " + task.task_id +
                                      " (hash: " + torrent_info.info_hash_hex +
                                      ", pieces: " + std::to_string(torrent_info.num_pieces()) + ")");

    // If already fully completed from on-disk pieces
    if (piece_mgr->IsAllCompleted()) {
        task.state = TaskState::kCompleted;
        task.downloaded_bytes = total_bytes;
        task.completed_segments = task.total_segments;
        task.status_detail = "Completed (BitTorrent)";
        if (on_progress) {
            TorrentProgress tp;
            tp.task_id = task.task_id;
            tp.state = TaskState::kCompleted;
            tp.downloaded_bytes = total_bytes;
            tp.total_bytes = total_bytes;
            tp.completed_pieces = task.total_segments;
            tp.total_pieces = task.total_segments;
            tp.status_detail = task.status_detail;
            on_progress(tp);
        }
        return Result<void>::Ok();
    }

    // 4. Tracker Announcement & DHT Peer Discovery
    task.status_detail = "Connecting to trackers...";
    if (on_progress) {
        TorrentProgress tp;
        tp.task_id = task.task_id;
        tp.state = task.state;
        tp.total_bytes = total_bytes;
        tp.total_pieces = static_cast<uint32_t>(torrent_info.num_pieces());
        tp.status_detail = task.status_detail;
        on_progress(tp);
    }

    std::vector<std::string> announce_urls = torrent_info.announce_list;
    if (announce_urls.empty() && !torrent_info.announce.empty()) {
        announce_urls.push_back(torrent_info.announce);
    }

    std::vector<PeerEndpoint> peer_swarm;
    if (!announce_urls.empty()) {
        auto tr_res = TrackerClient::AnnounceAny(
            announce_urls,
            torrent_info.info_hash_raw,
            client_peer_id,
            6881,
            task.uploaded_bytes,
            task.downloaded_bytes,
            total_bytes - task.downloaded_bytes,
            "started",
            4000);

        if (tr_res.ok()) {
            peer_swarm = tr_res.value->peers;
        } else {
            OPENIDM_LOG_WARN("TorrentEngine", "All announce trackers failed for task " + task.task_id + ": " + tr_res.error->message);
        }
    }

    // Secondary DHT Discovery if peer swarm is small
    if (peer_swarm.size() < 20) {
        std::vector<std::pair<std::string, uint16_t>> bootstrap_nodes = {
            {std::string("router.bittorrent.com"), static_cast<uint16_t>(6881)},
            {std::string("dht.transmissionbt.com"), static_cast<uint16_t>(6881)},
            {std::string("router.utorrent.com"), static_cast<uint16_t>(6881)}
        };
        auto dht_peers = DhtClient::QueryDhtNodes(bootstrap_nodes, torrent_info.info_hash_raw, client_peer_id, 1000);
        for (const auto& dp : dht_peers) {
            if (std::none_of(peer_swarm.begin(), peer_swarm.end(), [&](const PeerEndpoint& p) { return p.ip == dp.ip && p.port == dp.port; })) {
                peer_swarm.push_back(dp);
            }
        }
    }

    bool ever_discovered_peer = !peer_swarm.empty();

    task.status_detail = peer_swarm.empty()
        ? "No peers yet; continuing tracker/DHT discovery..."
        : "Connected to tracker; swarm has " + std::to_string(peer_swarm.size()) + " peers...";
    if (on_progress) {
        TorrentProgress tp;
        tp.task_id = task.task_id;
        tp.state = task.state;
        tp.total_bytes = total_bytes;
        tp.completed_pieces = task.completed_segments;
        tp.total_pieces = task.total_segments;
        tp.status_detail = task.status_detail;
        on_progress(tp);
    }

    auto start_time = std::chrono::steady_clock::now();
    auto task_limiter = std::make_shared<brisk::TokenBucketLimiter>();

    // Incoming peer support is intentionally kept in the same engine and
    // storage context as outbound peers. The listener advertises only pieces
    // that are both verified and fully selected; selective torrents must not
    // serve zero-filled ranges belonging to unselected files.
    std::mutex upload_mutex;
    std::atomic<bool> incoming_stop{false};
    std::thread incoming_listener([&]() {
        SOCKET listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_socket == INVALID_SOCKET) return;

        UniqueSocket listener(listen_socket);
        int reuse = 1;
        setsockopt(listener.s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        bind_addr.sin_port = htons(6881);
        if (bind(listener.s, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) == SOCKET_ERROR ||
            listen(listener.s, 16) == SOCKET_ERROR) {
            OPENIDM_LOG_WARN("TorrentEngine", "Incoming peer listener unavailable on port 6881");
            return;
        }

        auto recv_exact = [](SOCKET peer, uint8_t* dst, size_t length) {
            size_t received = 0;
            while (received < length) {
                int r = recv(peer, reinterpret_cast<char*>(dst + received), static_cast<int>(length - received), 0);
                if (r <= 0) return false;
                received += static_cast<size_t>(r);
            }
            return true;
        };

        while (!incoming_stop.load() && !cancel_requested.load() && !pause_requested.load()) {
            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(listener.s, &read_set);
            timeval wait_time{0, 250000};
#ifdef _WIN32
            int ready = select(0, &read_set, nullptr, nullptr, &wait_time);
#else
            int ready = select(listener.s + 1, &read_set, nullptr, nullptr, &wait_time);
#endif
            if (ready <= 0) continue;

            SOCKET peer_socket = accept(listener.s, nullptr, nullptr);
            if (peer_socket == INVALID_SOCKET) continue;
            UniqueSocket peer(peer_socket);
#ifdef _WIN32
            DWORD timeout_ms = 4000;
            setsockopt(peer.s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
            setsockopt(peer.s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#endif

            uint8_t peer_handshake[68]{};
            if (!recv_exact(peer.s, peer_handshake, sizeof(peer_handshake)) ||
                !PeerWireProtocol::VerifyHandshake(peer_handshake, sizeof(peer_handshake), torrent_info.info_hash_raw)) {
                continue;
            }
            auto response_handshake = PeerWireProtocol::CreateHandshake(torrent_info.info_hash_raw, client_peer_id);
            if (!SendAll(peer.s, response_handshake.data(), response_handshake.size())) continue;

            auto verified = piece_mgr->HavePieces();
            std::vector<bool> upload_have(verified.size(), false);
            for (size_t piece_index = 0; piece_index < verified.size(); ++piece_index) {
                upload_have[piece_index] = verified[piece_index] &&
                    torrent_info.selected_bytes_in_piece(piece_index) == torrent_info.get_piece_size(piece_index);
            }
            auto bitfield = PeerWireProtocol::CreateBitfield(upload_have);
            if (!bitfield.empty() && !SendAll(peer.s, bitfield.data(), bitfield.size())) continue;
            auto unchoke = PeerWireProtocol::CreateUnchoke();
            if (!SendAll(peer.s, unchoke.data(), unchoke.size())) continue;

            while (!incoming_stop.load() && !cancel_requested.load() && !pause_requested.load()) {
                uint8_t length_buf[4]{};
                if (!recv_exact(peer.s, length_buf, sizeof(length_buf))) break;
                uint32_t payload_length = PeerWireProtocol::ReadUint32BE(length_buf);
                if (payload_length == 0) continue;
                if (payload_length > (1u << 20)) break;
                std::vector<uint8_t> message(4 + payload_length);
                std::copy(length_buf, length_buf + 4, message.begin());
                if (!recv_exact(peer.s, message.data() + 4, payload_length)) break;
                auto parsed = PeerWireProtocol::ParseMessage(message.data(), message.size());
                if (!parsed || parsed->id != BtMessageId::kRequest) continue;

                const auto& request = *parsed;
                if (request.piece_index >= upload_have.size() || !upload_have[request.piece_index] ||
                    request.block_length == 0 || request.block_length > 32768) continue;
                uint64_t piece_size = torrent_info.get_piece_size(request.piece_index);
                if (static_cast<uint64_t>(request.block_begin) + request.block_length > piece_size) continue;

                uint64_t offset = static_cast<uint64_t>(request.piece_index) * torrent_info.piece_length + request.block_begin;
                std::vector<uint8_t> block(request.block_length);
                auto read_result = storage->ReadBytes(offset, block.data(), block.size());
                if (!read_result.ok()) continue;
                auto piece_message = PeerWireProtocol::CreatePiece(request.piece_index, request.block_begin, block.data(), block.size());
                if (!SendAll(peer.s, piece_message.data(), piece_message.size())) break;

                uint64_t uploaded = 0;
                uint64_t upload_speed = 0;
                {
                    std::lock_guard<std::mutex> metric_lock(upload_mutex);
                    task.uploaded_bytes += block.size();
                    auto elapsed_ms = std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start_time).count());
                    task.upload_speed_bps = (task.uploaded_bytes * 1000ULL) / static_cast<uint64_t>(elapsed_ms);
                    uploaded = task.uploaded_bytes;
                    upload_speed = task.upload_speed_bps;
                }
                if (on_progress) {
                    TorrentProgress progress;
                    progress.task_id = task.task_id;
                    progress.state = task.state;
                    progress.downloaded_bytes = task.downloaded_bytes;
                    progress.total_bytes = total_bytes;
                    progress.speed_bps = task.speed_bps;
                    progress.uploaded_bytes = uploaded;
                    progress.upload_speed_bps = upload_speed;
                    progress.completed_pieces = task.completed_segments;
                    progress.total_pieces = task.total_segments;
                    progress.status_detail = "Uploading verified pieces";
                    on_progress(progress);
                }
            }
        }
    });

    struct IncomingListenerGuard {
        std::atomic<bool>& stop;
        std::thread& worker;
        ~IncomingListenerGuard() {
            stop.store(true);
            if (worker.joinable()) worker.join();
        }
    } incoming_guard{incoming_stop, incoming_listener};

    // Peer connection worker lambda
    std::mutex swarm_mutex;
    std::atomic<uint32_t> active_connections{0};
    uint32_t connection_limit = std::max<uint32_t>(1, std::min<uint32_t>(max_peers, 64));

    auto peer_worker = [&](PeerEndpoint peer) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return;
        UniqueSocket sock(s);

#ifdef _WIN32
        DWORD to_ms = 4000;
        setsockopt(sock.s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to_ms), sizeof(to_ms));
        setsockopt(sock.s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to_ms), sizeof(to_ms));
#else
        struct timeval tv;
        tv.tv_sec = 4;
        tv.tv_usec = 0;
        setsockopt(sock.s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock.s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

        sockaddr_in saddr{};
        saddr.sin_family = AF_INET;
        saddr.sin_port = htons(peer.port);
        if (inet_pton(AF_INET, peer.ip.c_str(), &saddr.sin_addr) <= 0) return;
        if (connect(sock.s, reinterpret_cast<sockaddr*>(&saddr), sizeof(saddr)) == SOCKET_ERROR) return;

        auto hs = PeerWireProtocol::CreateHandshake(torrent_info.info_hash_raw, client_peer_id);
        if (!SendAll(sock.s, hs.data(), hs.size())) return;

        uint8_t peer_hs[68];
        int hs_read = 0;
        while (hs_read < 68) {
            int r = recv(sock.s, reinterpret_cast<char*>(peer_hs + hs_read), 68 - hs_read, 0);
            if (r <= 0) break;
            hs_read += r;
        }

        std::string remote_peer_id;
        if (!PeerWireProtocol::VerifyHandshake(peer_hs, hs_read, torrent_info.info_hash_raw, &remote_peer_id)) return;

        // Exchange unchoke, interested, bitfield
        auto interested = PeerWireProtocol::CreateInterested();
        SendAll(sock.s, interested.data(), interested.size());
        auto unchoke_msg = PeerWireProtocol::CreateUnchoke();
        SendAll(sock.s, unchoke_msg.data(), unchoke_msg.size());

        auto my_have = piece_mgr->HavePieces();
        if (std::any_of(my_have.begin(), my_have.end(), [](bool b) { return b; })) {
            auto bf = PeerWireProtocol::CreateBitfield(my_have);
            SendAll(sock.s, bf.data(), bf.size());
        }

        std::vector<bool> peer_have(torrent_info.num_pieces(), false);
        bool peer_choked_us = true;
        std::vector<uint8_t> recv_buf;
        recv_buf.reserve(65536);
        std::vector<BlockRequest> in_flight_requests;

        active_connections++;

        while (!cancel_requested.load() && !pause_requested.load() && !piece_mgr->IsAllCompleted()) {
            uint8_t temp[16384];
            int r = recv(sock.s, reinterpret_cast<char*>(temp), sizeof(temp), 0);
            if (r <= 0) break;

            if (speed_limit_bps.load(std::memory_order_relaxed) > 0) {
                task_limiter->Consume(static_cast<size_t>(r), speed_limit_bps, cancel_requested);
            }
            if (global_limiter && global_speed_limit_bps && global_speed_limit_bps->load(std::memory_order_relaxed) > 0) {
                global_limiter->Consume(static_cast<size_t>(r), *global_speed_limit_bps, cancel_requested);
            }

            recv_buf.insert(recv_buf.end(), temp, temp + r);

            while (recv_buf.size() >= 4) {
                uint32_t payload_len = PeerWireProtocol::ReadUint32BE(recv_buf.data());
                if (payload_len == 0) {
                    recv_buf.erase(recv_buf.begin(), recv_buf.begin() + 4);
                    continue;
                }
                if (recv_buf.size() < 4 + payload_len) break;

                auto msg_opt = PeerWireProtocol::ParseMessage(recv_buf.data(), 4 + payload_len);
                recv_buf.erase(recv_buf.begin(), recv_buf.begin() + 4 + payload_len);
                if (!msg_opt) continue;
                const auto& msg = *msg_opt;

                if (msg.id == BtMessageId::kChoke) {
                    peer_choked_us = true;
                } else if (msg.id == BtMessageId::kUnchoke) {
                    peer_choked_us = false;
                } else if (msg.id == BtMessageId::kHaveAll) {
                    std::fill(peer_have.begin(), peer_have.end(), true);
                } else if (msg.id == BtMessageId::kHaveNone) {
                    std::fill(peer_have.begin(), peer_have.end(), false);
                } else if (msg.id == BtMessageId::kHave) {
                    if (msg.piece_index < peer_have.size()) {
                        peer_have[msg.piece_index] = true;
                    }
                } else if (msg.id == BtMessageId::kBitfield) {
                    for (size_t i = 0; i < peer_have.size(); ++i) {
                        size_t byte_idx = i / 8;
                        size_t bit_idx = 7 - (i % 8);
                        if (byte_idx < msg.payload.size()) {
                            peer_have[i] = (msg.payload[byte_idx] & (1 << bit_idx)) != 0;
                        }
                    }
                } else if (msg.id == BtMessageId::kPiece) {
                    auto fit = std::find_if(in_flight_requests.begin(), in_flight_requests.end(),
                        [&](const BlockRequest& br) {
                            return br.piece_index == msg.piece_index && br.begin == msg.block_begin;
                        });
                    if (fit != in_flight_requests.end()) {
                        in_flight_requests.erase(fit);
                    }

                    bool verified = false;
                    bool corrupt = false;
                    piece_mgr->ProcessBlock(msg.piece_index, msg.block_begin, msg.payload.data(), msg.payload.size(), verified, corrupt);

                    if (verified) {
                        auto have_msg = PeerWireProtocol::CreateHave(msg.piece_index);
                        send(sock.s, reinterpret_cast<const char*>(have_msg.data()), static_cast<int>(have_msg.size()), 0);

                        task.completed_segments = static_cast<uint32_t>(piece_mgr->CompletedPieces());
                        task.downloaded_bytes = piece_mgr->DownloadedBytes();
                    }

                    // Report received blocks immediately so the task does not
                    // sit at 0% while the first (possibly large) piece is being
                    // assembled. Incomplete bytes remain transient; resume
                    // recovery still trusts only hash-verified pieces.
                    const uint64_t received_bytes = piece_mgr->ReceivedBytes();
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed_ms = std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count());
                    task.speed_bps = (received_bytes * 1000ULL) / static_cast<uint64_t>(elapsed_ms);
                    task.status_detail = "Downloading pieces: " + std::to_string(task.completed_segments) +
                                         " / " + std::to_string(task.total_segments) +
                                         " verified; " + std::to_string(received_bytes / (1024 * 1024)) +
                                         " MiB received (peers: " + std::to_string(active_connections.load()) + ")";

                    if (on_progress) {
                        TorrentProgress tp;
                        tp.task_id = task.task_id;
                        tp.state = TaskState::kDownloading;
                        tp.downloaded_bytes = task.downloaded_bytes;
                        tp.received_bytes = received_bytes;
                        tp.total_bytes = total_bytes;
                        tp.completed_pieces = task.completed_segments;
                        tp.total_pieces = task.total_segments;
                        tp.speed_bps = task.speed_bps;
                        tp.uploaded_bytes = task.uploaded_bytes;
                        tp.upload_speed_bps = task.upload_speed_bps;
                        tp.status_detail = task.status_detail;
                        on_progress(tp);
                    }
                } else if (msg.id == BtMessageId::kRequest) {
                    // Upload piece block to peer
                    if (msg.piece_index < torrent_info.num_pieces() &&
                        msg.block_length > 0 && msg.block_length <= 32768) {
                        auto cur_have = piece_mgr->HavePieces();
                        if (msg.piece_index < cur_have.size() && cur_have[msg.piece_index]) {
                            uint64_t piece_len = torrent_info.get_piece_size(msg.piece_index);
                            if (static_cast<uint64_t>(msg.block_begin) + msg.block_length <= piece_len) {
                                uint64_t torrent_offset = static_cast<uint64_t>(msg.piece_index) * torrent_info.piece_length + msg.block_begin;
                                std::vector<uint8_t> block_buf(msg.block_length);
                                auto r_res = storage->ReadBytes(torrent_offset, block_buf.data(), msg.block_length);
                                if (r_res.ok()) {
                                    auto p_msg = PeerWireProtocol::CreatePiece(msg.piece_index, msg.block_begin, block_buf.data(), msg.block_length);
                                    send(sock.s, reinterpret_cast<const char*>(p_msg.data()), static_cast<int>(p_msg.size()), 0);
                                    std::lock_guard<std::mutex> metric_lock(upload_mutex);
                                    task.uploaded_bytes += msg.block_length;
                                    auto now = std::chrono::steady_clock::now();
                                    auto elapsed_ms = std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count());
                                    task.upload_speed_bps = (task.uploaded_bytes * 1000ULL) / static_cast<uint64_t>(elapsed_ms);
                                }
                            }
                        }
                    }
                }
            }

            if (!peer_choked_us && !piece_mgr->IsAllCompleted()) {
                size_t slots = (kDefaultMaxOutstandingRequests > in_flight_requests.size())
                    ? (kDefaultMaxOutstandingRequests - in_flight_requests.size()) : 0;
                if (slots > 0) {
                    auto reqs = piece_mgr->GetNextRequestsForPeer(peer_have, slots);
                    for (const auto& rq : reqs) {
                        auto req_bytes = PeerWireProtocol::CreateRequest(rq.piece_index, rq.begin, rq.length);
                        if (SendAll(sock.s, req_bytes.data(), req_bytes.size())) {
                            in_flight_requests.push_back(rq);
                        } else {
                            piece_mgr->CancelInFlightRequests({rq});
                            break;
                        }
                    }
                }
            }
        }

        if (!in_flight_requests.empty()) {
            piece_mgr->CancelInFlightRequests(in_flight_requests);
            in_flight_requests.clear();
        }

        active_connections--;
    };

    // 5. Dynamic Peer Swarm Dispatcher Loop
    // Continually launches workers up to connection_limit, rotating through peer_swarm
    // and re-querying trackers/DHT if needed, until complete, paused, or canceled.
    std::vector<std::thread> workers;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::atomic<bool> dispatcher_stop{false};
    std::deque<PeerEndpoint> peer_queue(peer_swarm.begin(), peer_swarm.end());
    std::set<std::string> active_endpoints;
    auto last_announce_time = std::chrono::steady_clock::now();
    const auto discovery_deadline = last_announce_time + std::chrono::seconds(8);

    auto swarm_worker = [&]() {
        while (!dispatcher_stop.load() && !cancel_requested.load() && !pause_requested.load() && !piece_mgr->IsAllCompleted()) {
            PeerEndpoint peer;
            {
                std::unique_lock<std::mutex> lk(queue_mutex);
                queue_cv.wait_for(lk, std::chrono::milliseconds(250), [&]() {
                    return dispatcher_stop.load() || cancel_requested.load() || pause_requested.load() || piece_mgr->IsAllCompleted() || !peer_queue.empty();
                });
                if (dispatcher_stop.load() || cancel_requested.load() || pause_requested.load() || piece_mgr->IsAllCompleted()) break;
                if (peer_queue.empty()) continue;
                peer = peer_queue.front();
                peer_queue.pop_front();
                std::string ep_key = peer.ip + ":" + std::to_string(peer.port);
                if (active_endpoints.count(ep_key)) {
                    continue;
                }
                active_endpoints.insert(ep_key);
            }

            peer_worker(peer);

            {
                std::lock_guard<std::mutex> lk(queue_mutex);
                std::string ep_key = peer.ip + ":" + std::to_string(peer.port);
                active_endpoints.erase(ep_key);
            }

            // Brief pause before pulling next peer to avoid socket thrashing
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    };

    size_t pool_size = std::min<size_t>(connection_limit, std::max<size_t>(4, peer_swarm.size()));
    for (size_t i = 0; i < pool_size; ++i) {
        workers.emplace_back(swarm_worker);
    }

    // Monitor loop: if all pieces complete, or user pauses/cancels, break
    while (!cancel_requested.load() && !pause_requested.load() && !piece_mgr->IsAllCompleted()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

        // Keep workers alive while discovery replenishes a dead initial peer batch.
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_announce_time).count() >= 1) {
            last_announce_time = now;
            std::vector<PeerEndpoint> discovered;
            if (!announce_urls.empty()) {
                auto tr_res = TrackerClient::AnnounceAny(
                    announce_urls,
                    torrent_info.info_hash_raw,
                    client_peer_id,
                    6881,
                    task.uploaded_bytes,
                    task.downloaded_bytes,
                    total_bytes - task.downloaded_bytes,
                    "",
                    1000);
                if (tr_res.ok()) {
                    for (const auto& p : tr_res.value->peers) {
                        discovered.push_back(p);
                    }
                }
            }
            bool queue_empty = false;
            {
                std::lock_guard<std::mutex> lk(queue_mutex);
                queue_empty = peer_queue.empty();
            }
            if (discovered.empty() && queue_empty && active_connections.load() == 0) {
                std::vector<std::pair<std::string, uint16_t>> bootstrap_nodes = {
                    {"router.bittorrent.com", static_cast<uint16_t>(6881)},
                    {"dht.transmissionbt.com", static_cast<uint16_t>(6881)},
                    {"router.utorrent.com", static_cast<uint16_t>(6881)}
                };
                discovered = DhtClient::QueryDhtNodes(bootstrap_nodes, torrent_info.info_hash_raw, client_peer_id, 500);
            }
            if (!discovered.empty()) {
                ever_discovered_peer = true;
                std::lock_guard<std::mutex> lk(queue_mutex);
                for (const auto& p : discovered) {
                    std::string ep_key = p.ip + ":" + std::to_string(p.port);
                    if (!active_endpoints.count(ep_key) &&
                        std::none_of(peer_queue.begin(), peer_queue.end(), [&](const PeerEndpoint& queued) {
                            return queued.ip == p.ip && queued.port == p.port;
                        })) {
                        peer_queue.push_back(p);
                    }
                }
                queue_cv.notify_all();
            }
        }

        // Only fail after the bounded discovery window; a temporarily empty queue
        // is recoverable because the waiting workers remain available.
        bool any_active = (active_connections.load() > 0);
        {
            std::lock_guard<std::mutex> lk(queue_mutex);
            if (now >= discovery_deadline && !any_active && peer_queue.empty()) {
                break;
            }
        }
    }

    dispatcher_stop.store(true);
    queue_cv.notify_all();
    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

    if (cancel_requested.load()) {
        task.state = TaskState::kCanceled;
        task.status_detail = "Canceled";
        return Result<void>::Err("ERR_CANCELED", "BitTorrent task canceled by user");
    }

    if (pause_requested.load()) {
        task.state = TaskState::kPaused;
        task.status_detail = "Paused";
        return Result<void>::Err("ERR_PAUSED", "BitTorrent task paused by user");
    }

    if (piece_mgr->IsAllCompleted()) {
        task.state = TaskState::kCompleted;
        task.downloaded_bytes = total_bytes;
        task.completed_segments = task.total_segments;
        task.speed_bps = 0;
        task.status_detail = "Completed (BitTorrent)";

        // Announce completion to tracker
        TrackerClient::AnnounceAny(announce_urls, torrent_info.info_hash_raw, client_peer_id, 6881, task.uploaded_bytes, total_bytes, 0, "completed", 4000);

        if (on_progress) {
            TorrentProgress tp;
            tp.task_id = task.task_id;
            tp.state = TaskState::kCompleted;
            tp.downloaded_bytes = total_bytes;
            tp.total_bytes = total_bytes;
            tp.completed_pieces = task.total_segments;
            tp.total_pieces = task.total_segments;
            tp.speed_bps = 0;
            tp.uploaded_bytes = task.uploaded_bytes;
            tp.upload_speed_bps = 0;
            tp.status_detail = task.status_detail;
            on_progress(tp);
        }

        // 6. Persistent Seeding Session if requested
        if (seed_after_completion) {
            auto seed_start = std::chrono::steady_clock::now();
            task.status_detail = "Seeding to BitTorrent swarm...";
            if (on_progress) {
                TorrentProgress tp;
                tp.task_id = task.task_id;
                tp.state = TaskState::kCompleted;
                tp.downloaded_bytes = total_bytes;
                tp.total_bytes = total_bytes;
                tp.completed_pieces = task.total_segments;
                tp.total_pieces = task.total_segments;
                tp.status_detail = task.status_detail;
                on_progress(tp);
            }

            while (!cancel_requested.load() && !pause_requested.load()) {
                double ratio = (total_bytes > 0) ? (static_cast<double>(task.uploaded_bytes) / static_cast<double>(total_bytes)) : 0.0;
                if (max_seed_ratio > 0.0 && ratio >= max_seed_ratio) {
                    break;
                }
                if (max_seed_time_sec > 0) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - seed_start).count();
                    if (elapsed >= max_seed_time_sec) break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            // Announce stopped to tracker
            TrackerClient::AnnounceAny(announce_urls, torrent_info.info_hash_raw, client_peer_id, 6881, task.uploaded_bytes, total_bytes, 0, "stopped", 3000);
        }

        return Result<void>::Ok();
    }

    task.state = TaskState::kFailed;
    if (!ever_discovered_peer) {
        task.error_code = "ERR_TORRENT_NO_PEERS";
        task.error_message = "No BitTorrent peers available from announce trackers or DHT";
        task.status_detail = "Failed: No peers available";
        return Result<void>::Err("ERR_TORRENT_NO_PEERS", task.error_message);
    }
    task.error_code = "ERR_TORRENT_TRANSFER_INTERRUPTED";
    task.error_message = "Peer connection closed before torrent completion";
    task.status_detail = "Failed: Transfer interrupted";
    return Result<void>::Err("ERR_TORRENT_TRANSFER_INTERRUPTED", task.error_message);
}

} // namespace openidm::torrent
