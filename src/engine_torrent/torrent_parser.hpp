// SPDX-License-Identifier: GPL-3.0-or-later
// OpenIDM / Flux — BitTorrent Metainfo & Magnet URI Parser & Piece Validator
#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.hpp"
#include "engine_torrent/bencode.hpp"

namespace openidm::torrent {

struct TorrentFileEntry {
    std::filesystem::path relative_path;
    uint64_t length{0};
    uint64_t offset_in_torrent{0}; // Start offset across concatenated file tree
    bool selected{true};           // Multi-file selection support
};

struct TorrentInfo {
    std::string name;
    std::string info_hash_hex;
    std::vector<uint8_t> info_hash_raw;
    uint64_t piece_length{0};
    std::vector<std::vector<uint8_t>> pieces; // List of 20-byte SHA-1 hashes
    std::vector<TorrentFileEntry> files;
    uint64_t total_length{0};
    std::string announce;
    std::vector<std::string> announce_list;
    std::string comment;
    std::string created_by;
    int64_t creation_date{0};
    bool is_magnet{false};

    size_t num_pieces() const { return pieces.size(); }
    uint64_t selected_bytes() const {
        if (files.empty()) return total_length;
        uint64_t selected = 0;
        for (const auto& file : files) {
            if (file.selected) selected += file.length;
        }
        return selected;
    }
    bool has_selected_files() const {
        return files.empty() || std::any_of(files.begin(), files.end(), [](const TorrentFileEntry& file) { return file.selected; });
    }
    bool piece_required(size_t piece_index) const {
        if (files.empty()) return piece_index < num_pieces();
        const uint64_t piece_start = static_cast<uint64_t>(piece_index) * piece_length;
        const uint64_t piece_end = piece_start + get_piece_size(piece_index);
        for (const auto& file : files) {
            if (file.selected && file.length > 0 && piece_start < file.offset_in_torrent + file.length && piece_end > file.offset_in_torrent) return true;
        }
        return false;
    }
    uint64_t selected_bytes_in_piece(size_t piece_index) const {
        if (files.empty()) return get_piece_size(piece_index);
        const uint64_t piece_start = static_cast<uint64_t>(piece_index) * piece_length;
        const uint64_t piece_end = piece_start + get_piece_size(piece_index);
        uint64_t selected = 0;
        for (const auto& file : files) {
            if (!file.selected || file.length == 0) continue;
            const uint64_t start = std::max(piece_start, file.offset_in_torrent);
            const uint64_t end = std::min(piece_end, file.offset_in_torrent + file.length);
            if (end > start) selected += end - start;
        }
        return selected;
    }
    uint64_t get_piece_size(size_t piece_index) const {
        if (piece_index >= pieces.size() || piece_length == 0 || total_length == 0) return 0;
        if (piece_index == pieces.size() - 1) {
            uint64_t remainder = total_length % piece_length;
            return remainder == 0 ? piece_length : remainder;
        }
        return piece_length;
    }
};

class TorrentParser {
public:
    static Result<TorrentInfo> ParseTorrentFile(const std::filesystem::path& file_path);
    static Result<TorrentInfo> ParseTorrentBuffer(std::string_view data);
    static Result<TorrentInfo> ParseInfoDictionary(std::string_view info_dict_bencode, const std::string& announce = "", const std::vector<std::string>& announce_list = {});
    static Result<TorrentInfo> ParseMagnetUri(std::string_view magnet_uri);

    static bool GetBencodeValueRange(std::string_view s, size_t& pos, size_t depth = 0);
    static bool VerifyPiece(const uint8_t* piece_data, size_t piece_len, const std::vector<uint8_t>& expected_sha1);
    static bool VerifyPieceHex(const uint8_t* piece_data, size_t piece_len, std::string_view expected_sha1_hex);
};

} // namespace openidm::torrent
