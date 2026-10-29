// SPDX-License-Identifier: Apache-2.0
// OpenIDM / Flux — Non-Extracting ZIP Archive Central Directory Previewer & Security Guard
#include "archive/zip_preview.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace openidm::archive {

namespace {

#pragma pack(push, 1)
struct ZipEOCD {
    uint32_t signature;
    uint16_t disk_number;
    uint16_t cd_start_disk;
    uint16_t total_entries_disk;
    uint16_t total_entries;
    uint32_t cd_size;
    uint32_t cd_offset;
    uint16_t comment_length;
};

struct ZipCDHeader {
    uint32_t signature;
    uint16_t version_made_by;
    uint16_t version_needed;
    uint16_t flags;
    uint16_t compression_method;
    uint16_t last_mod_time;
    uint16_t last_mod_date;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t filename_length;
    uint16_t extra_length;
    uint16_t comment_length;
    uint16_t disk_start;
    uint16_t internal_attrs;
    uint32_t external_attrs;
    uint32_t local_header_offset;
};
#pragma pack(pop)

} // namespace

bool ZipPreviewer::IsZipSlipPath(std::string_view path) {
    if (path.empty()) return false;

    // Check for drive letters (e.g. C:)
    if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':') {
        return true;
    }

    // Check for NTFS Alternate Data Streams (ADS)
    if (path.find(':') != std::string_view::npos) {
        return true;
    }

    // Check leading slash
    if (path.front() == '/' || path.front() == '\\') {
        return true;
    }

    // Check directory traversal segments (../ or ..\)
    std::string s(path);
    std::replace(s.begin(), s.end(), '\\', '/');

    size_t pos = 0;
    while (pos < s.size()) {
        size_t next_slash = s.find('/', pos);
        std::string segment = (next_slash == std::string::npos) ? s.substr(pos) : s.substr(pos, next_slash - pos);
        if (segment == "..") {
            return true;
        }
        if (next_slash == std::string::npos) break;
        pos = next_slash + 1;
    }

    return false;
}

Result<ZipArchiveSummary> ZipPreviewer::InspectArchive(const std::filesystem::path& file_path) {
    std::ifstream ifs(file_path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        return Result<ZipArchiveSummary>::Err("ERR_ZIP_OPEN", "Cannot open zip archive: " + file_path.string());
    }

    std::streamsize file_size = ifs.tellg();
    if (file_size < static_cast<std::streamsize>(sizeof(ZipEOCD))) {
        return Result<ZipArchiveSummary>::Err("ERR_ZIP_TOO_SMALL", "File is too small to be a valid ZIP archive");
    }

    // Read trailing buffer for EOCD search (max comment size is 65535)
    std::streamsize search_size = std::min<std::streamsize>(file_size, 65535 + sizeof(ZipEOCD));
    std::vector<uint8_t> tail_buffer(static_cast<size_t>(search_size));

    ifs.seekg(file_size - search_size, std::ios::beg);
    ifs.read(reinterpret_cast<char*>(tail_buffer.data()), search_size);

    // Search backwards for EOCD signature: 0x06054b50
    const uint32_t kEocdSig = 0x06054b50;
    std::streamsize eocd_idx = -1;

    for (std::streamsize i = search_size - static_cast<std::streamsize>(sizeof(ZipEOCD)); i >= 0; --i) {
        uint32_t sig = 0;
        std::memcpy(&sig, &tail_buffer[static_cast<size_t>(i)], sizeof(sig));
        if (sig == kEocdSig) {
            eocd_idx = i;
            break;
        }
    }

    if (eocd_idx == -1) {
        return Result<ZipArchiveSummary>::Err("ERR_ZIP_EOCD_NOT_FOUND", "Could not find End of Central Directory record");
    }

    ZipEOCD eocd;
    std::memcpy(&eocd, &tail_buffer[static_cast<size_t>(eocd_idx)], sizeof(ZipEOCD));

    uint64_t u_file_size = static_cast<uint64_t>(file_size);
    if (eocd.cd_offset >= u_file_size || eocd.cd_size > (u_file_size - eocd.cd_offset)) {
        return Result<ZipArchiveSummary>::Err("ERR_ZIP_CORRUPT", "Central Directory offset exceeds file size");
    }

    const uint32_t kMaxCentralDirectoryBytes = 64 * 1024 * 1024; // 64 MB max CD
    if (eocd.cd_size > kMaxCentralDirectoryBytes) {
        return Result<ZipArchiveSummary>::Err("ERR_ZIP_CORRUPT", "Central Directory size exceeds 64 MB limit");
    }

    // Read Central Directory records
    ifs.seekg(eocd.cd_offset, std::ios::beg);
    if (!ifs) {
        return Result<ZipArchiveSummary>::Err("ERR_ZIP_CORRUPT", "Failed to seek to Central Directory");
    }
    std::vector<uint8_t> cd_buffer(eocd.cd_size);
    ifs.read(reinterpret_cast<char*>(cd_buffer.data()), eocd.cd_size);
    if (ifs.gcount() != static_cast<std::streamsize>(eocd.cd_size)) {
        return Result<ZipArchiveSummary>::Err("ERR_ZIP_CORRUPT", "Incomplete read of Central Directory");
    }

    return InspectBuffer(cd_buffer.data(), cd_buffer.size());
}

Result<ZipArchiveSummary> ZipPreviewer::InspectBuffer(const uint8_t* data, size_t length) {
    ZipArchiveSummary summary;
    const uint32_t kCdHeaderSig = 0x02014b50;
    const size_t kMaxZipEntries = 100000;
    size_t offset = 0;

    while (offset + sizeof(ZipCDHeader) <= length) {
        if (summary.entries.size() >= kMaxZipEntries) {
            summary.is_potential_zip_bomb = true;
            break;
        }

        uint32_t sig = 0;
        std::memcpy(&sig, data + offset, sizeof(sig));
        if (sig != kCdHeaderSig) {
            break;
        }

        ZipCDHeader hdr;
        std::memcpy(&hdr, data + offset, sizeof(ZipCDHeader));
        offset += sizeof(ZipCDHeader);

        uint64_t variable_lengths = static_cast<uint64_t>(hdr.filename_length) +
                                    static_cast<uint64_t>(hdr.extra_length) +
                                    static_cast<uint64_t>(hdr.comment_length);
        if (variable_lengths > (length - offset)) {
            return Result<ZipArchiveSummary>::Err("ERR_ZIP_CORRUPT", "Malformed Central Directory variable lengths exceed buffer");
        }

        std::string filename(reinterpret_cast<const char*>(data + offset), hdr.filename_length);
        offset += hdr.filename_length;
        offset += hdr.extra_length;
        offset += hdr.comment_length;

        ZipEntryInfo entry;
        entry.file_path = filename;
        entry.uncompressed_size = hdr.uncompressed_size;
        entry.compressed_size = hdr.compressed_size;
        entry.crc32 = hdr.crc32;
        entry.compression_method = hdr.compression_method;
        entry.is_encrypted = (hdr.flags & 1) != 0;
        entry.is_directory = (!filename.empty() && (filename.back() == '/' || filename.back() == '\\'));
        entry.is_suspicious_path = IsZipSlipPath(filename);

        // Date formatting: DOS date/time format
        uint32_t year = 1980 + ((hdr.last_mod_date >> 9) & 0x7F);
        uint32_t month = (hdr.last_mod_date >> 5) & 0x0F;
        uint32_t day = hdr.last_mod_date & 0x1F;
        uint32_t hour = (hdr.last_mod_time >> 11) & 0x1F;
        uint32_t minute = (hdr.last_mod_time >> 5) & 0x3F;
        char date_buf[32];
        snprintf(date_buf, sizeof(date_buf), "%04u-%02u-%02u %02u:%02u", year, month, day, hour, minute);
        entry.modified_date_str = date_buf;

        if (entry.is_encrypted) summary.has_encrypted_entries = true;
        if (entry.is_suspicious_path) summary.has_zip_slip_risk = true;

        if (entry.is_directory) {
            summary.total_folders++;
        } else {
            summary.total_files++;
            summary.total_uncompressed_bytes += entry.uncompressed_size;
            summary.total_compressed_bytes += entry.compressed_size;
        }

        summary.entries.push_back(std::move(entry));
    }

    // Zip Bomb detection
    if (summary.total_compressed_bytes > 0) {
        double ratio = static_cast<double>(summary.total_uncompressed_bytes) / static_cast<double>(summary.total_compressed_bytes);
        if (ratio > 100.0 && summary.total_uncompressed_bytes > 104857600ULL) { // >100:1 ratio and >100MB
            summary.is_potential_zip_bomb = true;
        }
    }
    if (summary.total_uncompressed_bytes > 21474836480ULL) { // > 20 GB
        summary.is_potential_zip_bomb = true;
    }

    return Result<ZipArchiveSummary>::Ok(std::move(summary));
}

} // namespace openidm::archive
