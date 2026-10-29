// SPDX-License-Identifier: Apache-2.0
// OpenIDM / Flux — Non-Extracting ZIP Archive Central Directory Previewer & Security Guard
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.hpp"

namespace openidm::archive {

struct ZipEntryInfo {
    std::string file_path;
    uint64_t uncompressed_size{0};
    uint64_t compressed_size{0};
    uint32_t crc32{0};
    uint16_t compression_method{0}; // 0 = Stored, 8 = Deflated
    bool is_directory{false};
    bool is_encrypted{false};
    bool is_suspicious_path{false}; // Zip Slip indicator (traversal, absolute path, ADS)
    std::string modified_date_str;
};

struct ZipArchiveSummary {
    std::vector<ZipEntryInfo> entries;
    uint64_t total_uncompressed_bytes{0};
    uint64_t total_compressed_bytes{0};
    uint32_t total_files{0};
    uint32_t total_folders{0};
    bool has_encrypted_entries{false};
    bool is_potential_zip_bomb{false};
    bool has_zip_slip_risk{false};
    std::string comment;
};

class ZipPreviewer {
public:
    ZipPreviewer() = default;
    ~ZipPreviewer() = default;

    // Parses the Central Directory of a local file without extracting file bodies
    static Result<ZipArchiveSummary> InspectArchive(const std::filesystem::path& file_path);

    // Parses Central Directory from a byte buffer (e.g. tail of partial download)
    static Result<ZipArchiveSummary> InspectBuffer(const uint8_t* data, size_t length);

    // Tests whether a file path contains path traversal or dangerous elements
    static bool IsZipSlipPath(std::string_view path);
};

} // namespace openidm::archive
