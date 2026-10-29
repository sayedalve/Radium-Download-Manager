// SPDX-License-Identifier: Apache-2.0
// OpenIDM — Multi-Tier Duplicate Task Detector
// Inspired by XDM (subhra74/xdm DuplicateIndex):
//   Checks matching in strict priority tiers:
//     1. SAME_URL: Exact or normalized URL match (scheme/host lowercased, default port omitted, fragment dropped, query preserved)
//     2. SAME_NAME_AND_SIZE: Case-insensitive filename match AND non-zero known total bytes match
//     3. SAME_ETAG: Strong ETag match paired with server host (excluding weak W/ ETags)
//   Within each tier, the newest task record takes precedence.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "brisk_engine/http_client.hpp"
#include "common/types.hpp"

namespace openidm::core {

enum class DuplicateKind {
    kNone = 0,
    kSameUrl = 1,
    kSameNameAndSize = 2,
    kSameEtag = 3
};

inline const char* DuplicateKindToString(DuplicateKind kind) {
    switch (kind) {
        case DuplicateKind::kSameUrl: return "SAME_URL";
        case DuplicateKind::kSameNameAndSize: return "SAME_NAME_AND_SIZE";
        case DuplicateKind::kSameEtag: return "SAME_ETAG";
        default: return "NONE";
    }
}

struct DuplicateMatch {
    DuplicateKind kind{DuplicateKind::kNone};
    TaskModel matched_task;
};

class DuplicateDetector {
public:
    static std::string Trim(std::string_view sv) {
        size_t start = 0;
        while (start < sv.size() && std::isspace(static_cast<unsigned char>(sv[start]))) ++start;
        size_t end = sv.size();
        while (end > start && std::isspace(static_cast<unsigned char>(sv[end - 1]))) --end;
        return std::string(sv.substr(start, end - start));
    }

    static std::string ToLower(std::string_view sv) {
        std::string s(sv);
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    static bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) {
                return false;
            }
        }
        return true;
    }

    // Normalizes a 40-character hex or 32-character base32 BitTorrent info-hash
    // into canonical 40-character lowercase hex form (Motrix normalizeBtInfoHash pattern)
    static std::string NormalizeBtInfoHash(std::string_view value) {
        std::string s = Trim(value);
        if (s.size() == 40) {
            bool all_hex = true;
            for (char c : s) {
                if (!std::isxdigit(static_cast<unsigned char>(c))) {
                    all_hex = false;
                    break;
                }
            }
            if (all_hex) return ToLower(s);
        }
        if (s.size() == 32) {
            // Base32 decoding (RFC 4648 / Motrix BASE32_ALPHABET 'A-Z2-7')
            static const char kBase32Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
            uint32_t bits = 0;
            int bit_count = 0;
            std::vector<uint8_t> bytes;
            bytes.reserve(20);

            for (char c : s) {
                char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                const char* pos = std::strchr(kBase32Alphabet, upper);
                if (!pos) return "";
                int digit = static_cast<int>(pos - kBase32Alphabet);
                bits = (bits << 5) | static_cast<uint32_t>(digit);
                bit_count += 5;
                while (bit_count >= 8) {
                    bit_count -= 8;
                    bytes.push_back(static_cast<uint8_t>((bits >> bit_count) & 0xFF));
                    bits &= ((1u << bit_count) - 1);
                }
            }
            if (bytes.size() == 20) {
                static const char kHexChars[] = "0123456789abcdef";
                std::string hex;
                hex.reserve(40);
                for (uint8_t b : bytes) {
                    hex.push_back(kHexChars[(b >> 4) & 0x0F]);
                    hex.push_back(kHexChars[b & 0x0F]);
                }
                return hex;
            }
        }
        return "";
    }

    // Extracts canonical 40-character hex info-hash from magnet URI (Motrix extractMagnetInfoHash pattern)
    static std::string ExtractMagnetInfoHash(std::string_view magnet_uri) {
        std::string s(magnet_uri);
        auto pos = s.find("xt=urn:btih:");
        if (pos == std::string::npos) return "";
        std::string_view after = std::string_view(s).substr(pos + 12);
        auto end_pos = after.find_first_of("&#");
        std::string raw_hash = std::string(after.substr(0, end_pos));
        return NormalizeBtInfoHash(raw_hash);
    }

    // Normalizes URL following XDM & Motrix algorithms:
    // - Trimmed whitespace
    // - Magnet links normalized to canonical 40-character hex BTIH
    // - Scheme and host converted to lowercase
    // - Default HTTP (80) and HTTPS (443) ports removed
    // - Path preserved (defaults to /)
    // - Query string preserved (picks specific file/session)
    // - Fragment (#...) dropped
    static std::string NormalizeUrl(std::string_view raw_url) {
        std::string trimmed = Trim(raw_url);
        if (trimmed.empty()) return "";

        // Motrix BitTorrent Magnet Canonicalization: normalize any magnet URI to canonical 40-hex infohash
        if (trimmed.compare(0, 8, "magnet:?") == 0 || trimmed.compare(0, 7, "magnet:") == 0) {
            std::string canonical_hash = ExtractMagnetInfoHash(trimmed);
            if (!canonical_hash.empty()) {
                return "magnet:?xt=urn:btih:" + canonical_hash;
            }
        }

        auto parsed = brisk::HttpClient::ParseUrl(trimmed);
        if (!parsed.ok()) {
            auto hash_pos = trimmed.find('#');
            return (hash_pos == std::string::npos) ? trimmed : trimmed.substr(0, hash_pos);
        }

        std::string norm;
        norm.reserve(trimmed.size() + 8);
        norm += ToLower(parsed.value->scheme);
        norm += "://";
        if (!parsed.value->userinfo.empty()) {
            norm += parsed.value->userinfo;
            norm += '@';
        }
        norm += ToLower(parsed.value->host);

        bool is_default_port = (parsed.value->scheme == "http" && parsed.value->port == 80) ||
                               (parsed.value->scheme == "https" && parsed.value->port == 443);
        if (!is_default_port && parsed.value->port > 0) {
            norm += ':';
            norm += std::to_string(parsed.value->port);
        }

        norm += parsed.value->path_and_query;
        return norm;
    }

    // Pairs strong ETags with host to prevent cross-host false positives.
    // Weak ETags (starting with W/ or w/) are ignored per RFC 7232.
    static std::string EtagHostKey(std::string_view raw_url, std::string_view etag) {
        std::string tag = Trim(etag);
        if (tag.empty()) return "";
        if (tag.size() >= 2 && (tag[0] == 'W' || tag[0] == 'w') && tag[1] == '/') {
            return ""; // Weak ETag
        }

        auto parsed = brisk::HttpClient::ParseUrl(raw_url);
        if (!parsed.ok() || parsed.value->host.empty()) return "";

        return ToLower(parsed.value->host) + "\n" + tag;
    }

    // Evaluates duplicate match across existing tasks in strict priority tiers:
    // 1. Same URL (strongest match)
    // 2. Same Name and Size (case-insensitive filename AND known size > 0)
    // 3. Same Strong ETag from the same server host
    // Picks the newest task within each tier.
    static DuplicateMatch FindDuplicate(
        const std::vector<TaskModel>& tasks,
        const std::string& url,
        const std::string& file_name = "",
        uint64_t file_size = 0,
        const std::string& etag = "",
        const std::string& exclude_task_id = "") {

        std::string norm_target_url = NormalizeUrl(url);
        std::string target_etag_key = EtagHostKey(url, etag);
        std::string clean_name = Trim(file_name);

        const TaskModel* best_url_match = nullptr;
        const TaskModel* best_name_size_match = nullptr;
        const TaskModel* best_etag_match = nullptr;

        for (const auto& t : tasks) {
            if (!exclude_task_id.empty() && t.task_id == exclude_task_id) continue;

            // Tier 1: Check Same URL
            if (!norm_target_url.empty()) {
                std::string norm_task_url = NormalizeUrl(t.url);
                if (norm_target_url == norm_task_url) {
                    if (!best_url_match || t.created_at_ms > best_url_match->created_at_ms) {
                        best_url_match = &t;
                    }
                }
            }

            // Tier 2: Check Same Name and Size
            if (!clean_name.empty() && file_size > 0 && t.total_bytes == file_size) {
                if (EqualsIgnoreCase(t.file_name, clean_name)) {
                    if (!best_name_size_match || t.created_at_ms > best_name_size_match->created_at_ms) {
                        best_name_size_match = &t;
                    }
                }
            }

            // Tier 3: Check Same Strong ETag from Same Host
            if (!target_etag_key.empty() && !t.etag.empty()) {
                std::string task_etag_key = EtagHostKey(t.url, t.etag);
                if (!task_etag_key.empty() && task_etag_key == target_etag_key) {
                    if (!best_etag_match || t.created_at_ms > best_etag_match->created_at_ms) {
                        best_etag_match = &t;
                    }
                }
            }
        }

        // Return strongest tier match
        if (best_url_match) {
            return DuplicateMatch{DuplicateKind::kSameUrl, *best_url_match};
        }
        if (best_name_size_match) {
            return DuplicateMatch{DuplicateKind::kSameNameAndSize, *best_name_size_match};
        }
        if (best_etag_match) {
            return DuplicateMatch{DuplicateKind::kSameEtag, *best_etag_match};
        }

        return DuplicateMatch{DuplicateKind::kNone, {}};
    }

    static DuplicateMatch FindDuplicate(
        const std::map<std::string, TaskModel>& tasks_map,
        const std::string& url,
        const std::string& file_name = "",
        uint64_t file_size = 0,
        const std::string& etag = "",
        const std::string& exclude_task_id = "") {

        std::vector<TaskModel> task_list;
        task_list.reserve(tasks_map.size());
        for (const auto& [id, t] : tasks_map) {
            task_list.push_back(t);
        }
        return FindDuplicate(task_list, url, file_name, file_size, etag, exclude_task_id);
    }
};

} // namespace openidm::core
