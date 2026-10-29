// SPDX-License-Identifier: Apache-2.0
// OpenIDM / Flux — BitTorrent Metainfo & Magnet URI Parser & Piece Validator
#include "engine_torrent/torrent_parser.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "common/crypto_utils.hpp"
#include "common/path_sanitizer.hpp"
#include "brisk_engine/http_client.hpp"

namespace openidm::torrent {

bool TorrentParser::GetBencodeValueRange(std::string_view s, size_t& pos, size_t depth) {
    if (depth > 64 || pos >= s.size()) return false;
    char c = s[pos];
    if (c == 'i') {
        size_t e = s.find('e', pos + 1);
        if (e == std::string_view::npos) return false;
        pos = e + 1;
        return true;
    } else if (c == 'l' || c == 'd') {
        ++pos;
        while (pos < s.size() && s[pos] != 'e') {
            if (!GetBencodeValueRange(s, pos, depth + 1)) return false;
        }
        if (pos >= s.size() || s[pos] != 'e') return false;
        ++pos; // skip 'e'
        return true;
    } else if (std::isdigit(static_cast<unsigned char>(c))) {
        size_t colon = s.find(':', pos);
        if (colon == std::string_view::npos) return false;
        std::string_view len_str = s.substr(pos, colon - pos);
        char* end_ptr = nullptr;
        unsigned long long u_len = std::strtoull(std::string(len_str).c_str(), &end_ptr, 10);
        pos = colon + 1;
        if (u_len > (s.size() - pos)) return false;
        pos += static_cast<size_t>(u_len);
        return true;
    }
    return false;
}

namespace {

std::string ToLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

std::vector<uint8_t> HexToBytes(std::string_view hex) {
    std::vector<uint8_t> bytes;
    if (hex.size() % 2 != 0) return bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        char hi = hex[i];
        char lo = hex[i + 1];
        auto hex_val = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int h = hex_val(hi);
        int l = hex_val(lo);
        if (h < 0 || l < 0) return {};
        bytes.push_back(static_cast<uint8_t>((h << 4) | l));
    }
    return bytes;
}

std::vector<uint8_t> Base32ToBytes(std::string_view b32) {
    // RFC 4648 Base32 alphabet: A-Z (0-25), 2-7 (26-31)
    std::vector<uint8_t> out;
    uint32_t buffer = 0;
    int bits_left = 0;

    for (char c : b32) {
        int val = -1;
        if (c >= 'A' && c <= 'Z') val = c - 'A';
        else if (c >= 'a' && c <= 'z') val = c - 'a';
        else if (c >= '2' && c <= '7') val = c - '2' + 26;
        else if (c == '=') break;
        if (val < 0) continue;

        buffer = (buffer << 5) | static_cast<uint32_t>(val);
        bits_left += 5;
        if (bits_left >= 8) {
            bits_left -= 8;
            out.push_back(static_cast<uint8_t>((buffer >> bits_left) & 0xFF));
        }
    }
    return out;
}

std::string BytesToHex(const uint8_t* data, size_t len) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) {
        oss << std::setw(2) << static_cast<unsigned int>(data[i]);
    }
    return oss.str();
}

} // namespace

Result<TorrentInfo> TorrentParser::ParseTorrentFile(const std::filesystem::path& file_path) {
    std::string path_str = file_path.string();
    if (path_str.find("magnet:?") == 0) {
        return Result<TorrentInfo>::Err("ERR_TORRENT_MAGNET_METADATA",
            "Magnet link requires metadata exchange; cannot parse directly as static torrent file");
    }
    if (path_str.find("http://") == 0 || path_str.find("https://") == 0) {
        std::string torrent_bytes;
        std::atomic<bool> cancel{false};
        std::map<std::string, std::string> headers = {
            {"User-Agent", "Flux/0.3.0"}
        };
        auto get_res = brisk::HttpClient::PerformGet(
            path_str,
            headers,
            std::nullopt,
            std::nullopt,
            15000,
            false,
            cancel,
            [&](const brisk::HttpResponseMeta& meta) -> brisk::HeaderAction {
                if (meta.status_code != 200) return brisk::HeaderAction::kAbort;
                return brisk::HeaderAction::kContinue;
            },
            [&](const uint8_t* data, size_t len) -> bool {
                if (torrent_bytes.size() + len > 10 * 1024 * 1024) return false;
                torrent_bytes.append(reinterpret_cast<const char*>(data), len);
                return true;
            });
        if (!get_res.ok()) {
            return Result<TorrentInfo>::Err(get_res.error->code, "Failed to download remote torrent file: " + get_res.error->message);
        }
        return ParseTorrentBuffer(torrent_bytes);
    }

    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs) {
        return Result<TorrentInfo>::Err("ERR_TORRENT_FILE_OPEN", "Cannot open torrent file: " + file_path.string());
    }
    std::stringstream ss;
    ss << ifs.rdbuf();
    std::string content = ss.str();
    return ParseTorrentBuffer(content);
}

Result<TorrentInfo> TorrentParser::ParseTorrentBuffer(std::string_view data) {
    auto root_res = BencodeParser::Decode(data);
    if (!root_res.ok()) {
        return Result<TorrentInfo>::Err(root_res.error->code, root_res.error->message);
    }
    const auto& root = root_res.value.value();
    if (!root.is_dict() || !root.contains("info")) {
        return Result<TorrentInfo>::Err("ERR_TORRENT_MALFORMED", "Torrent metainfo missing root 'info' dictionary");
    }

    TorrentInfo info;
    info.is_magnet = false;

    // Trackers
    if (root.contains("announce") && root["announce"].is_string()) {
        info.announce = root["announce"].as_string();
        info.announce_list.push_back(info.announce);
    }
    if (root.contains("announce-list") && root["announce-list"].is_list()) {
        for (const auto& tier : root["announce-list"].as_list()) {
            if (tier.is_list()) {
                for (const auto& tr : tier.as_list()) {
                    if (tr.is_string()) {
                        const std::string& u = tr.as_string();
                        if (std::find(info.announce_list.begin(), info.announce_list.end(), u) == info.announce_list.end()) {
                            info.announce_list.push_back(u);
                        }
                    }
                }
            }
        }
    }

    if (root.contains("comment") && root["comment"].is_string()) {
        info.comment = root["comment"].as_string();
    }
    if (root.contains("created by") && root["created by"].is_string()) {
        info.created_by = root["created by"].as_string();
    }
    if (root.contains("creation date") && root["creation date"].is_int()) {
        info.creation_date = root["creation date"].as_int();
    }

    // Info dictionary
    const auto& info_dict = root["info"];
    if (!info_dict.is_dict()) {
        return Result<TorrentInfo>::Err("ERR_TORRENT_MALFORMED", "'info' must be a bencode dictionary");
    }

    // Compute info_hash from raw slice of original bencoded torrent file
    std::string_view info_raw_slice;
    size_t info_key = data.find("4:info");
    if (info_key != std::string_view::npos) {
        size_t val_start = info_key + 6;
        size_t val_end = val_start;
        if (val_start < data.size() && data[val_start] == 'd' && GetBencodeValueRange(data, val_end)) {
            info_raw_slice = data.substr(val_start, val_end - val_start);
        }
    }

    if (!info_raw_slice.empty()) {
        info.info_hash_raw = CryptoUtils::ComputeBufferSha1Raw(
            reinterpret_cast<const uint8_t*>(info_raw_slice.data()), info_raw_slice.size());
    } else {
        std::string encoded_info = BencodeParser::Encode(info_dict);
        info.info_hash_raw = CryptoUtils::ComputeBufferSha1Raw(
            reinterpret_cast<const uint8_t*>(encoded_info.data()), encoded_info.size());
    }
    info.info_hash_hex = ToLower(BytesToHex(info.info_hash_raw.data(), info.info_hash_raw.size()));

    if (info_dict.contains("name") && info_dict["name"].is_string()) {
        info.name = PathSanitizer::SanitizeFileName(info_dict["name"].as_string());
    } else {
        info.name = "torrent_" + info.info_hash_hex.substr(0, 8);
    }

    if (info_dict.contains("piece length") && info_dict["piece length"].is_int()) {
        int64_t pl = info_dict["piece length"].as_int();
        if (pl <= 0 || pl > 134217728) {
            return Result<TorrentInfo>::Err("ERR_TORRENT_MALFORMED", "Invalid piece length in torrent metainfo");
        }
        info.piece_length = static_cast<uint64_t>(pl);
    } else {
        return Result<TorrentInfo>::Err("ERR_TORRENT_MALFORMED", "Missing piece length in torrent metainfo");
    }

    // Pieces
    if (info_dict.contains("pieces") && info_dict["pieces"].is_string()) {
        const std::string& p_str = info_dict["pieces"].as_string();
        if (p_str.empty() || p_str.size() % 20 != 0) {
            return Result<TorrentInfo>::Err("ERR_TORRENT_PIECES", "Pieces string length not a non-zero multiple of 20");
        }
        size_t count = p_str.size() / 20;
        info.pieces.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(p_str.data() + (i * 20));
            info.pieces.emplace_back(p, p + 20);
        }
    } else {
        return Result<TorrentInfo>::Err("ERR_TORRENT_PIECES", "Missing pieces in torrent metainfo");
    }

    // Files: single file vs multi-file
    if (info_dict.contains("files") && info_dict["files"].is_list()) {
        // Multi-file
        uint64_t current_offset = 0;
        for (const auto& f : info_dict["files"].as_list()) {
            if (!f.is_dict() || !f.contains("length") || !f.contains("path") || !f["length"].is_int()) continue;
            int64_t flen = f["length"].as_int();
            if (flen < 0) {
                return Result<TorrentInfo>::Err("ERR_TORRENT_MALFORMED", "Negative file length in multi-file torrent");
            }
            uint64_t len = static_cast<uint64_t>(flen);
            if (len > UINT64_MAX - current_offset) {
                return Result<TorrentInfo>::Err("ERR_TORRENT_MALFORMED", "File offset integer overflow in multi-file torrent");
            }
            std::filesystem::path rel_path;
            if (f["path"].is_list()) {
                for (const auto& seg : f["path"].as_list()) {
                    if (seg.is_string()) {
                        rel_path /= PathSanitizer::SanitizeFileName(seg.as_string());
                    }
                }
            }
            if (rel_path.empty()) rel_path = "unnamed.bin";

            TorrentFileEntry entry;
            entry.relative_path = rel_path;
            entry.length = len;
            entry.offset_in_torrent = current_offset;
            current_offset += len;

            info.files.push_back(std::move(entry));
        }
        info.total_length = current_offset;
    } else if (info_dict.contains("length") && info_dict["length"].is_int()) {
        // Single file
        int64_t flen = info_dict["length"].as_int();
        if (flen <= 0) {
            return Result<TorrentInfo>::Err("ERR_TORRENT_MALFORMED", "Invalid single file length in torrent metainfo");
        }
        uint64_t len = static_cast<uint64_t>(flen);
        TorrentFileEntry entry;
        entry.relative_path = info.name;
        entry.length = len;
        entry.offset_in_torrent = 0;
        info.files.push_back(std::move(entry));
        info.total_length = len;
    } else {
        return Result<TorrentInfo>::Err("ERR_TORRENT_MALFORMED", "Missing file length in torrent metainfo");
    }

    if (info.total_length == 0 || info.files.empty()) {
        return Result<TorrentInfo>::Err("ERR_TORRENT_MALFORMED", "Total torrent length is 0 or no valid files found");
    }

    // Verify pieces count against total_length and piece_length
    size_t expected_pieces = static_cast<size_t>((info.total_length + info.piece_length - 1) / info.piece_length);
    if (info.pieces.size() != expected_pieces) {
        return Result<TorrentInfo>::Err("ERR_TORRENT_PIECES",
            "Piece count mismatch: expected " + std::to_string(expected_pieces) +
            ", got " + std::to_string(info.pieces.size()));
    }

    return Result<TorrentInfo>::Ok(std::move(info));
}

Result<TorrentInfo> TorrentParser::ParseInfoDictionary(
    std::string_view info_dict_bencode,
    const std::string& announce,
    const std::vector<std::string>& announce_list) {

    auto dec_res = BencodeParser::Decode(info_dict_bencode);
    if (!dec_res.ok()) return Result<TorrentInfo>::Err(dec_res.error->code, dec_res.error->message);
    const auto& info_dict = dec_res.value.value();
    if (!info_dict.is_dict()) return Result<TorrentInfo>::Err("ERR_TORRENT_MALFORMED", "info must be dict");

    TorrentInfo info;
    info.is_magnet = false;
    info.announce = announce;
    info.announce_list = announce_list;
    if (info.announce_list.empty() && !announce.empty()) info.announce_list.push_back(announce);

    info.info_hash_raw = CryptoUtils::ComputeBufferSha1Raw(reinterpret_cast<const uint8_t*>(info_dict_bencode.data()), info_dict_bencode.size());
    info.info_hash_hex = ToLower(BytesToHex(info.info_hash_raw.data(), info.info_hash_raw.size()));

    if (info_dict.contains("name") && info_dict["name"].is_string()) {
        info.name = PathSanitizer::SanitizeFileName(info_dict["name"].as_string());
    } else {
        info.name = "torrent_" + info.info_hash_hex.substr(0, 8);
    }

    if (info_dict.contains("piece length") && info_dict["piece length"].is_int()) {
        int64_t pl = info_dict["piece length"].as_int();
        if (pl <= 0 || pl > 134217728) return Result<TorrentInfo>::Err("ERR_TORRENT_MALFORMED", "Invalid piece length");
        info.piece_length = static_cast<uint64_t>(pl);
    } else {
        return Result<TorrentInfo>::Err("ERR_TORRENT_MALFORMED", "Missing piece length");
    }

    if (info_dict.contains("pieces") && info_dict["pieces"].is_string()) {
        const std::string& p_str = info_dict["pieces"].as_string();
        if (p_str.empty() || p_str.size() % 20 != 0) return Result<TorrentInfo>::Err("ERR_TORRENT_PIECES", "Invalid pieces");
        size_t count = p_str.size() / 20;
        info.pieces.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(p_str.data() + (i * 20));
            info.pieces.emplace_back(p, p + 20);
        }
    } else {
        return Result<TorrentInfo>::Err("ERR_TORRENT_PIECES", "Missing pieces");
    }

    if (info_dict.contains("files") && info_dict["files"].is_list()) {
        uint64_t current_offset = 0;
        for (const auto& f : info_dict["files"].as_list()) {
            if (!f.is_dict() || !f.contains("length") || !f.contains("path") || !f["length"].is_int()) continue;
            int64_t flen = f["length"].as_int();
            if (flen < 0) return Result<TorrentInfo>::Err("ERR_TORRENT_MALFORMED", "Negative file length");
            uint64_t len = static_cast<uint64_t>(flen);
            std::filesystem::path rel_path;
            if (f["path"].is_list()) {
                for (const auto& seg : f["path"].as_list()) {
                    if (seg.is_string()) rel_path /= PathSanitizer::SanitizeFileName(seg.as_string());
                }
            }
            if (rel_path.empty()) rel_path = "unnamed.bin";
            TorrentFileEntry entry{rel_path, len, current_offset, true};
            current_offset += len;
            info.files.push_back(std::move(entry));
        }
        info.total_length = current_offset;
    } else if (info_dict.contains("length") && info_dict["length"].is_int()) {
        int64_t flen = info_dict["length"].as_int();
        if (flen <= 0) return Result<TorrentInfo>::Err("ERR_TORRENT_MALFORMED", "Invalid length");
        uint64_t len = static_cast<uint64_t>(flen);
        TorrentFileEntry entry{info.name, len, 0, true};
        info.files.push_back(std::move(entry));
        info.total_length = len;
    } else {
        return Result<TorrentInfo>::Err("ERR_TORRENT_MALFORMED", "Missing length");
    }

    if (info.total_length == 0 || info.files.empty()) return Result<TorrentInfo>::Err("ERR_TORRENT_MALFORMED", "Zero length");
    size_t expected_pieces = static_cast<size_t>((info.total_length + info.piece_length - 1) / info.piece_length);
    if (info.pieces.size() != expected_pieces) return Result<TorrentInfo>::Err("ERR_TORRENT_PIECES", "Piece count mismatch");
    return Result<TorrentInfo>::Ok(std::move(info));
}

Result<TorrentInfo> TorrentParser::ParseMagnetUri(std::string_view magnet_uri) {
    std::string s(magnet_uri);
    std::string prefix = "magnet:?";
    if (s.compare(0, prefix.size(), prefix) != 0) {
        return Result<TorrentInfo>::Err("ERR_INVALID_MAGNET", "Magnet URI must begin with magnet:?");
    }

    TorrentInfo info;
    info.is_magnet = true;

    std::string query = s.substr(prefix.size());
    std::stringstream ss(query);
    std::string param;

    while (std::getline(ss, param, '&')) {
        if (param.empty()) continue;
        size_t eq = param.find('=');
        if (eq == std::string::npos) continue;
        std::string key = param.substr(0, eq);
        std::string val = PathSanitizer::UrlDecode(param.substr(eq + 1));

        if (key == "xt") {
            // Exact Topic: urn:btih:<hash>
            std::string btih_prefix = "urn:btih:";
            if (val.compare(0, btih_prefix.size(), btih_prefix) == 0) {
                std::string hash = val.substr(btih_prefix.size());
                if (hash.size() == 40) {
                    info.info_hash_hex = ToLower(hash);
                    info.info_hash_raw = HexToBytes(info.info_hash_hex);
                } else if (hash.size() == 32) {
                    info.info_hash_raw = Base32ToBytes(hash);
                    info.info_hash_hex = ToLower(BytesToHex(info.info_hash_raw.data(), info.info_hash_raw.size()));
                }
            }
        } else if (key == "dn") {
            // Display Name
            info.name = PathSanitizer::SanitizeFileName(val);
        } else if (key == "tr") {
            // Tracker
            if (std::find(info.announce_list.begin(), info.announce_list.end(), val) == info.announce_list.end()) {
                info.announce_list.push_back(val);
            }
            if (info.announce.empty()) info.announce = val;
        }
    }

    if (info.info_hash_hex.empty() || info.info_hash_raw.size() != 20) {
        return Result<TorrentInfo>::Err("ERR_MAGNET_NO_HASH", "Magnet URI missing valid urn:btih hash");
    }

    if (info.name.empty()) {
        info.name = "magnet_" + info.info_hash_hex.substr(0, 8);
    }

    return Result<TorrentInfo>::Ok(std::move(info));
}

bool TorrentParser::VerifyPiece(const uint8_t* piece_data, size_t piece_len, const std::vector<uint8_t>& expected_sha1) {
    if (expected_sha1.size() != 20) return false;
    auto computed = CryptoUtils::ComputeBufferSha1Raw(piece_data, piece_len);
    return computed == expected_sha1;
}

bool TorrentParser::VerifyPieceHex(const uint8_t* piece_data, size_t piece_len, std::string_view expected_sha1_hex) {
    auto computed = CryptoUtils::ComputeBufferSha1Hex(std::string_view(reinterpret_cast<const char*>(piece_data), piece_len));
    return ToLower(computed) == ToLower(expected_sha1_hex);
}

} // namespace openidm::torrent
