// SPDX-License-Identifier: Apache-2.0
// OpenIDM / Flux â€” BitTorrent Peer Wire Protocol (BEP 3, BEP 23, BEP 52)
// Direct reuse of aria2 BitTorrent wire framing, handshake, and compact peer unpacking algorithms
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/crypto_utils.hpp"
#include "engine_torrent/bencode.hpp"

namespace openidm::torrent {

// Constants directly aligned with aria2 BtConstants.h
constexpr size_t kInfoHashLength = 20;
constexpr size_t kPieceHashLength = 20;
constexpr size_t kPeerIdLength = 20;
constexpr size_t kBlockLength = 16384; // 16 KB standard block length
constexpr size_t kHandshakeLength = 68;
constexpr size_t kDefaultMaxOutstandingRequests = 6;
constexpr size_t kCompactLenIpv4 = 6;
constexpr size_t kCompactLenIpv6 = 18;

// BitTorrent Protocol Message IDs (BEP 3)
enum class BtMessageId : uint8_t {
    kChoke = 0,
    kUnchoke = 1,
    kInterested = 2,
    kNotInterested = 3,
    kHave = 4,
    kBitfield = 5,
    kRequest = 6,
    kPiece = 7,
    kCancel = 8,
    kPort = 9,
    kSuggestPiece = 13,
    kHaveAll = 14,
    kHaveNone = 15,
    kRejectRequest = 16,
    kAllowedFast = 17,
    kExtended = 20
};

// Extension Protocol Message IDs (BEP 10)
constexpr uint8_t kExtHandshakeId = 0;

// Extension sub-messages for BEP 9 (ut_metadata)
constexpr int64_t kUtMetadataRequest = 0;
constexpr int64_t kUtMetadataData = 1;
constexpr int64_t kUtMetadataReject = 2;

struct PeerEndpoint {
    std::string ip;
    uint16_t port{0};
    std::string peer_id;
};

struct BtMessage {
    BtMessageId id{BtMessageId::kChoke};
    uint32_t piece_index{0};
    uint32_t block_begin{0};
    uint32_t block_length{0};
    uint8_t ext_id{0};            // For Extended message (BEP 10)
    std::vector<uint8_t> payload; // For Bitfield, Piece, or Extended data
};

class PeerWireProtocol {
public:
    static inline uint32_t ReadUint32BE(const uint8_t* p) {
        return (static_cast<uint32_t>(p[0]) << 24) |
               (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) |
               static_cast<uint32_t>(p[3]);
    }

    static inline void WriteUint32BE(uint8_t* p, uint32_t val) {
        p[0] = static_cast<uint8_t>((val >> 24) & 0xFF);
        p[1] = static_cast<uint8_t>((val >> 16) & 0xFF);
        p[2] = static_cast<uint8_t>((val >> 8) & 0xFF);
        p[3] = static_cast<uint8_t>(val & 0xFF);
    }

    static inline uint16_t ReadUint16BE(const uint8_t* p) {
        return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]));
    }

    static inline void WriteUint16BE(uint8_t* p, uint16_t val) {
        p[0] = static_cast<uint8_t>((val >> 8) & 0xFF);
        p[1] = static_cast<uint8_t>(val & 0xFF);
    }

    // Generates client Peer ID following Azureus/aria2 style: "-FL0100-" + 12 alphanumeric random characters
    static std::string GeneratePeerId() {
        std::string peer_id = "-FL0100-";
        const char charset[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<size_t> dist(0, sizeof(charset) - 2);
        for (int i = 0; i < 12; ++i) {
            peer_id.push_back(charset[dist(gen)]);
        }
        return peer_id;
    }

    // Handshake serialization (BEP 3 & aria2 BtHandshakeMessage.cc)
    static std::vector<uint8_t> CreateHandshake(
        const std::vector<uint8_t>& info_hash,
        std::string_view peer_id) {
        std::vector<uint8_t> hs(kHandshakeLength, 0);
        hs[0] = 19;
        std::memcpy(&hs[1], "BitTorrent protocol", 19);
        // Reserved 8 bytes: enable Fast extension (reserved[7] |= 0x04) and Extension protocol (reserved[5] |= 0x10)
        hs[25] |= 0x10;
        hs[27] |= 0x04;
        if (info_hash.size() >= kInfoHashLength) {
            std::memcpy(&hs[28], info_hash.data(), kInfoHashLength);
        }
        size_t id_len = std::min<size_t>(kPeerIdLength, peer_id.size());
        if (id_len > 0) {
            std::memcpy(&hs[48], peer_id.data(), id_len);
        }
        return hs;
    }

    // Handshake validation
    static bool VerifyHandshake(
        const uint8_t* data,
        size_t len,
        const std::vector<uint8_t>& expected_info_hash,
        std::string* out_peer_id = nullptr) {
        if (len < kHandshakeLength) return false;
        if (data[0] != 19) return false;
        if (std::memcmp(&data[1], "BitTorrent protocol", 19) != 0) return false;
        if (expected_info_hash.size() >= kInfoHashLength &&
            std::memcmp(&data[28], expected_info_hash.data(), kInfoHashLength) != 0) {
            return false;
        }
        if (out_peer_id) {
            out_peer_id->assign(reinterpret_cast<const char*>(&data[48]), kPeerIdLength);
        }
        return true;
    }

    // Message creation helpers (aria2 DefaultBtMessageFactory.cc)
    static std::vector<uint8_t> CreateKeepAlive() {
        std::vector<uint8_t> msg(4, 0);
        return msg;
    }

    static std::vector<uint8_t> CreateSimpleMessage(BtMessageId id) {
        std::vector<uint8_t> msg(5, 0);
        WriteUint32BE(msg.data(), 1);
        msg[4] = static_cast<uint8_t>(id);
        return msg;
    }

    static std::vector<uint8_t> CreateChoke() { return CreateSimpleMessage(BtMessageId::kChoke); }
    static std::vector<uint8_t> CreateUnchoke() { return CreateSimpleMessage(BtMessageId::kUnchoke); }
    static std::vector<uint8_t> CreateInterested() { return CreateSimpleMessage(BtMessageId::kInterested); }
    static std::vector<uint8_t> CreateNotInterested() { return CreateSimpleMessage(BtMessageId::kNotInterested); }

    static std::vector<uint8_t> CreateHave(uint32_t piece_index) {
        std::vector<uint8_t> msg(9, 0);
        WriteUint32BE(msg.data(), 5);
        msg[4] = static_cast<uint8_t>(BtMessageId::kHave);
        WriteUint32BE(&msg[5], piece_index);
        return msg;
    }

    static std::vector<uint8_t> CreateBitfield(const std::vector<bool>& pieces) {
        size_t bitfield_bytes = (pieces.size() + 7) / 8;
        std::vector<uint8_t> msg(5 + bitfield_bytes, 0);
        WriteUint32BE(msg.data(), static_cast<uint32_t>(1 + bitfield_bytes));
        msg[4] = static_cast<uint8_t>(BtMessageId::kBitfield);
        for (size_t i = 0; i < pieces.size(); ++i) {
            if (pieces[i]) {
                msg[5 + (i / 8)] |= static_cast<uint8_t>(1 << (7 - (i % 8)));
            }
        }
        return msg;
    }

    static std::vector<uint8_t> CreateRequest(uint32_t piece_index, uint32_t begin, uint32_t length) {
        std::vector<uint8_t> msg(17, 0);
        WriteUint32BE(msg.data(), 13);
        msg[4] = static_cast<uint8_t>(BtMessageId::kRequest);
        WriteUint32BE(&msg[5], piece_index);
        WriteUint32BE(&msg[9], begin);
        WriteUint32BE(&msg[13], length);
        return msg;
    }

    static std::vector<uint8_t> CreatePiece(uint32_t piece_index, uint32_t begin, const uint8_t* block_data, size_t block_len) {
        std::vector<uint8_t> msg(13 + block_len, 0);
        WriteUint32BE(msg.data(), static_cast<uint32_t>(9 + block_len));
        msg[4] = static_cast<uint8_t>(BtMessageId::kPiece);
        WriteUint32BE(&msg[5], piece_index);
        WriteUint32BE(&msg[9], begin);
        if (block_data && block_len > 0) {
            std::memcpy(&msg[13], block_data, block_len);
        }
        return msg;
    }

    static std::vector<uint8_t> CreateCancel(uint32_t piece_index, uint32_t begin, uint32_t length) {
        std::vector<uint8_t> msg(17, 0);
        WriteUint32BE(msg.data(), 13);
        msg[4] = static_cast<uint8_t>(BtMessageId::kCancel);
        WriteUint32BE(&msg[5], piece_index);
        WriteUint32BE(&msg[9], begin);
        WriteUint32BE(&msg[13], length);
        return msg;
    }

    // BEP 10 Extended message creation
    static std::vector<uint8_t> CreateExtendedMessage(uint8_t ext_id, const uint8_t* payload, size_t payload_len) {
        std::vector<uint8_t> msg(6 + payload_len, 0);
        WriteUint32BE(msg.data(), static_cast<uint32_t>(2 + payload_len));
        msg[4] = static_cast<uint8_t>(BtMessageId::kExtended);
        msg[5] = ext_id;
        if (payload && payload_len > 0) {
            std::memcpy(&msg[6], payload, payload_len);
        }
        return msg;
    }

    // BEP 10 Extended Handshake (supports ut_metadata = 1, metadata_size)
    static std::vector<uint8_t> CreateExtendedHandshake(uint32_t ut_metadata_id = 1, uint64_t metadata_size = 0) {
        BencodeValue root = BencodeValue::MakeDict();
        BencodeValue m = BencodeValue::MakeDict();
        m.as_dict()["ut_metadata"] = BencodeValue(static_cast<int64_t>(ut_metadata_id));
        root.as_dict()["m"] = std::move(m);
        root.as_dict()["v"] = BencodeValue("Radium 1.0.0");
        if (metadata_size > 0) {
            root.as_dict()["metadata_size"] = BencodeValue(static_cast<int64_t>(metadata_size));
        }
        std::string enc = BencodeParser::Encode(root);
        return CreateExtendedMessage(kExtHandshakeId, reinterpret_cast<const uint8_t*>(enc.data()), enc.size());
    }

    // BEP 9 ut_metadata Request (msg_type = 0)
    static std::vector<uint8_t> CreateUtMetadataRequest(uint8_t peer_ut_metadata_id, uint32_t piece_idx) {
        BencodeValue d = BencodeValue::MakeDict();
        d.as_dict()["msg_type"] = BencodeValue(kUtMetadataRequest);
        d.as_dict()["piece"] = BencodeValue(static_cast<int64_t>(piece_idx));
        std::string enc = BencodeParser::Encode(d);
        return CreateExtendedMessage(peer_ut_metadata_id, reinterpret_cast<const uint8_t*>(enc.data()), enc.size());
    }

    // BEP 9 ut_metadata Data (msg_type = 1)
    static std::vector<uint8_t> CreateUtMetadataData(uint8_t peer_ut_metadata_id, uint32_t piece_idx, uint64_t total_size, const uint8_t* piece_data, size_t piece_len) {
        BencodeValue d = BencodeValue::MakeDict();
        d.as_dict()["msg_type"] = BencodeValue(kUtMetadataData);
        d.as_dict()["piece"] = BencodeValue(static_cast<int64_t>(piece_idx));
        d.as_dict()["total_size"] = BencodeValue(static_cast<int64_t>(total_size));
        std::string bencode_header = BencodeParser::Encode(d);

        std::vector<uint8_t> payload(bencode_header.size() + piece_len);
        std::memcpy(payload.data(), bencode_header.data(), bencode_header.size());
        if (piece_data && piece_len > 0) {
            std::memcpy(payload.data() + bencode_header.size(), piece_data, piece_len);
        }
        return CreateExtendedMessage(peer_ut_metadata_id, payload.data(), payload.size());
    }

    // Message parsing from framed bytes (length prefix + ID + payload)
    static std::optional<BtMessage> ParseMessage(const uint8_t* data, size_t len) {
        if (len < 4) return std::nullopt;
        uint32_t payload_len = ReadUint32BE(data);
        if (payload_len == 0) {
            // Keep-alive
            return std::nullopt;
        }
        if (len < 4 + payload_len) {
            return std::nullopt; // Incomplete message
        }
        uint8_t id_byte = data[4];
        BtMessage msg;
        msg.id = static_cast<BtMessageId>(id_byte);

        switch (msg.id) {
            case BtMessageId::kChoke:
            case BtMessageId::kUnchoke:
            case BtMessageId::kInterested:
            case BtMessageId::kNotInterested:
                return msg;

            case BtMessageId::kHave:
                if (payload_len >= 5) {
                    msg.piece_index = ReadUint32BE(&data[5]);
                    return msg;
                }
                break;

            case BtMessageId::kBitfield:
                if (payload_len >= 1) {
                    msg.payload.assign(&data[5], &data[4 + payload_len]);
                    return msg;
                }
                break;

            case BtMessageId::kRequest:
            case BtMessageId::kCancel:
                if (payload_len >= 13) {
                    msg.piece_index = ReadUint32BE(&data[5]);
                    msg.block_begin = ReadUint32BE(&data[9]);
                    msg.block_length = ReadUint32BE(&data[13]);
                    return msg;
                }
                break;

            case BtMessageId::kPiece:
                if (payload_len >= 9) {
                    msg.piece_index = ReadUint32BE(&data[5]);
                    msg.block_begin = ReadUint32BE(&data[9]);
                    msg.block_length = payload_len - 9;
                    msg.payload.assign(&data[13], &data[4 + payload_len]);
                    return msg;
                }
                break;

            case BtMessageId::kExtended:
                if (payload_len >= 2) {
                    msg.ext_id = data[5];
                    if (payload_len > 2) {
                        msg.payload.assign(&data[6], &data[4 + payload_len]);
                    }
                    return msg;
                }
                break;

            case BtMessageId::kHaveAll:
            case BtMessageId::kHaveNone:
                return msg;

            case BtMessageId::kSuggestPiece:
            case BtMessageId::kAllowedFast:
                if (payload_len >= 5) {
                    msg.piece_index = ReadUint32BE(&data[5]);
                    return msg;
                }
                break;

            case BtMessageId::kRejectRequest:
                if (payload_len >= 13) {
                    msg.piece_index = ReadUint32BE(&data[5]);
                    msg.block_begin = ReadUint32BE(&data[9]);
                    msg.block_length = ReadUint32BE(&data[13]);
                    return msg;
                }
                break;

            case BtMessageId::kPort:
                if (payload_len >= 3) {
                    msg.block_begin = ReadUint16BE(&data[5]); // store port in block_begin
                    return msg;
                }
                break;

            default:
                break;
        }
        return std::nullopt;
    }

    // Compact peer unpacking (direct reuse of aria2 unpackcompact / bittorrent::extractPeer)
    static std::pair<std::string, uint16_t> UnpackCompactIpv4(const uint8_t* compact_6bytes) {
        if (!compact_6bytes) return {"", uint16_t{0}};
        std::string ip = std::to_string(compact_6bytes[0]) + "." +
                         std::to_string(compact_6bytes[1]) + "." +
                         std::to_string(compact_6bytes[2]) + "." +
                         std::to_string(compact_6bytes[3]);
        uint16_t port = ReadUint16BE(&compact_6bytes[4]);
        if (port == 0) return {"", uint16_t{0}};
        return {ip, port};
    }

    static std::pair<std::string, uint16_t> UnpackCompactIpv6(const uint8_t* compact_18bytes) {
        if (!compact_18bytes) return {"", uint16_t{0}};
        char hex[64];
        std::snprintf(hex, sizeof(hex),
            "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
            compact_18bytes[0], compact_18bytes[1], compact_18bytes[2], compact_18bytes[3],
            compact_18bytes[4], compact_18bytes[5], compact_18bytes[6], compact_18bytes[7],
            compact_18bytes[8], compact_18bytes[9], compact_18bytes[10], compact_18bytes[11],
            compact_18bytes[12], compact_18bytes[13], compact_18bytes[14], compact_18bytes[15]);
        uint16_t port = ReadUint16BE(&compact_18bytes[16]);
        if (port == 0) return {"", uint16_t{0}};
        return {std::string(hex), port};
    }

    static std::vector<PeerEndpoint> ExtractPeersFromCompact(std::string_view compact_peers) {
        std::vector<PeerEndpoint> endpoints;
        const size_t unit = kCompactLenIpv4;
        if (compact_peers.size() % unit != 0) return endpoints;

        const uint8_t* p = reinterpret_cast<const uint8_t*>(compact_peers.data());
        size_t count = compact_peers.size() / unit;
        endpoints.reserve(count);

        for (size_t i = 0; i < count; ++i) {
            auto [ip, port] = UnpackCompactIpv4(p + (i * unit));
            if (!ip.empty() && port > 0) {
                endpoints.push_back(PeerEndpoint{ip, port, ""});
            }
        }
        return endpoints;
    }

    static std::vector<PeerEndpoint> ExtractPeersFromBencode(const BencodeValue& root_or_peers) {
        std::vector<PeerEndpoint> endpoints;
        if (root_or_peers.is_string()) {
            return ExtractPeersFromCompact(root_or_peers.as_string());
        }
        if (root_or_peers.is_list()) {
            for (const auto& item : root_or_peers.as_list()) {
                if (item.is_dict()) {
                    std::string ip;
                    uint16_t port = 0;
                    std::string peer_id;
                    if (item.contains("ip") && item["ip"].is_string()) {
                        ip = item["ip"].as_string();
                    }
                    if (item.contains("port") && item["port"].is_int()) {
                        port = static_cast<uint16_t>(item["port"].as_int());
                    }
                    if (item.contains("peer id") && item["peer id"].is_string()) {
                        peer_id = item["peer id"].as_string();
                    }
                    if (!ip.empty() && port > 0) {
                        endpoints.push_back(PeerEndpoint{ip, port, peer_id});
                    }
                }
            }
            return endpoints;
        }
        if (root_or_peers.is_dict() && root_or_peers.contains("peers")) {
            return ExtractPeersFromBencode(root_or_peers["peers"]);
        }
        return endpoints;
    }
};

} // namespace openidm::torrent
