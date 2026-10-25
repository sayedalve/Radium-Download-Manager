// SPDX-License-Identifier: GPL-3.0-or-later
// OpenIDM / Flux — Multi-File Storage Adaptor
// Direct reuse of aria2 MultiDiskAdaptor mapping and boundary offset calculation algorithms
#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "common/logger.hpp"
#include "common/path_sanitizer.hpp"
#include "common/types.hpp"
#include "engine_torrent/torrent_parser.hpp"

namespace openidm::torrent {

class MultiFileStorage {
public:
    MultiFileStorage() = default;
    ~MultiFileStorage() = default;

    Result<void> Initialize(
        const std::filesystem::path& base_dir,
        const TorrentInfo& info) {
        m_base_dir = base_dir;
        m_total_length = info.total_length;
        m_files.clear();

        std::error_code ec;
        std::filesystem::create_directories(m_base_dir, ec);

        if (!info.files.empty()) {
            m_files.reserve(info.files.size());
            for (const auto& f : info.files) {
                FileMapping fm;
                fm.entry = f;
                fm.full_path = m_base_dir / f.relative_path;
                m_files.push_back(fm);
            }
        } else {
            // Single-file torrent
            FileMapping fm;
            fm.entry.relative_path = PathSanitizer::SanitizeFileName(info.name);
            fm.entry.length = info.total_length;
            fm.entry.offset_in_torrent = 0;
            fm.entry.selected = true;
            fm.full_path = m_base_dir / fm.entry.relative_path;
            m_files.push_back(fm);
        }

        return Result<void>::Ok();
    }

    Result<void> AllocateFiles() {
        std::error_code ec;
        for (const auto& fm : m_files) {
            if (!fm.entry.selected) continue; // Skip unselected files
            std::filesystem::path parent = fm.full_path.parent_path();
            if (!parent.empty() && !std::filesystem::exists(parent, ec)) {
                std::filesystem::create_directories(parent, ec);
            }
            if (!std::filesystem::exists(fm.full_path, ec)) {
                std::ofstream ofs(fm.full_path, std::ios::binary | std::ios::out);
                if (!ofs) {
                    return Result<void>::Err("ERR_STORAGE_CREATE", "Failed to create file: " + fm.full_path.string());
                }
            }
        }
        return Result<void>::Ok();
    }

    void SetFileSelection(size_t file_index, bool selected) {
        if (file_index < m_files.size()) {
            m_files[file_index].entry.selected = selected;
        }
    }

    // Direct adoption of aria2 MultiDiskAdaptor::writeData
    Result<void> WriteBytes(uint64_t torrent_offset, const uint8_t* data, size_t length) {
        if (!data || length == 0) return Result<void>::Ok();
        if (m_files.empty()) {
            return Result<void>::Err("ERR_STORAGE_UNINITIALIZED", "Storage uninitialized");
        }
        if (torrent_offset + length > m_total_length) {
            return Result<void>::Err("ERR_STORAGE_OUT_OF_BOUNDS", "Write past torrent total length");
        }

        uint64_t cur_offset = torrent_offset;
        size_t rem = length;
        const uint8_t* src = data;
        size_t file_idx = FindFileIndex(cur_offset);

        while (rem > 0 && file_idx < m_files.size()) {
            const auto& fm = m_files[file_idx];
            uint64_t file_start = fm.entry.offset_in_torrent;
            uint64_t file_end = file_start + fm.entry.length;
            if (cur_offset >= file_end) {
                file_idx++;
                continue;
            }

            uint64_t offset_in_file = cur_offset - file_start;
            uint64_t avail_in_file = fm.entry.length - offset_in_file;
            size_t to_write = static_cast<size_t>(std::min<uint64_t>(rem, avail_in_file));

            if (fm.entry.selected) {
                std::error_code ec;
                std::filesystem::path parent = fm.full_path.parent_path();
                if (!parent.empty() && !std::filesystem::exists(parent, ec)) {
                    std::filesystem::create_directories(parent, ec);
                }

                // Open with read/write in binary mode without truncation
                std::fstream fs(fm.full_path, std::ios::in | std::ios::out | std::ios::binary);
                if (!fs) {
                    fs.clear();
                    fs.open(fm.full_path, std::ios::out | std::ios::binary);
                    fs.close();
                    fs.open(fm.full_path, std::ios::in | std::ios::out | std::ios::binary);
                }
                if (!fs) {
                    return Result<void>::Err("ERR_STORAGE_WRITE", "Cannot open for write: " + fm.full_path.string());
                }

                fs.seekp(static_cast<std::streamoff>(offset_in_file), std::ios::beg);
                fs.write(reinterpret_cast<const char*>(src), static_cast<std::streamsize>(to_write));
                fs.flush();
                if (!fs.good()) {
                    return Result<void>::Err("ERR_STORAGE_WRITE", "Write failed on: " + fm.full_path.string());
                }
            }

            src += to_write;
            cur_offset += to_write;
            rem -= to_write;
            file_idx++;
        }

        return Result<void>::Ok();
    }

    // Direct adoption of aria2 MultiDiskAdaptor::readData
    Result<void> ReadBytes(uint64_t torrent_offset, uint8_t* buffer, size_t length) {
        if (!buffer || length == 0) return Result<void>::Ok();
        if (m_files.empty()) {
            return Result<void>::Err("ERR_STORAGE_UNINITIALIZED", "Storage uninitialized");
        }
        if (torrent_offset + length > m_total_length) {
            return Result<void>::Err("ERR_STORAGE_OUT_OF_BOUNDS", "Read past torrent total length");
        }

        uint64_t cur_offset = torrent_offset;
        size_t rem = length;
        uint8_t* dst = buffer;
        size_t file_idx = FindFileIndex(cur_offset);

        while (rem > 0 && file_idx < m_files.size()) {
            const auto& fm = m_files[file_idx];
            uint64_t file_start = fm.entry.offset_in_torrent;
            uint64_t file_end = file_start + fm.entry.length;
            if (cur_offset >= file_end) {
                file_idx++;
                continue;
            }

            uint64_t offset_in_file = cur_offset - file_start;
            uint64_t avail_in_file = fm.entry.length - offset_in_file;
            size_t to_read = static_cast<size_t>(std::min<uint64_t>(rem, avail_in_file));

            if (fm.entry.selected && std::filesystem::exists(fm.full_path)) {
                std::ifstream ifs(fm.full_path, std::ios::binary);
                if (!ifs) {
                    return Result<void>::Err("ERR_STORAGE_READ", "Cannot open for read: " + fm.full_path.string());
                }

                ifs.seekg(static_cast<std::streamoff>(offset_in_file), std::ios::beg);
                ifs.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(to_read));
                if (!ifs.good() && !ifs.eof()) {
                    return Result<void>::Err("ERR_STORAGE_READ", "Read failed on: " + fm.full_path.string());
                }
            } else {
                std::memset(dst, 0, to_read);
            }

            dst += to_read;
            cur_offset += to_read;
            rem -= to_read;
            file_idx++;
        }

        return Result<void>::Ok();
    }

    std::vector<TorrentFileEntry> GetFileEntries() const {
        std::vector<TorrentFileEntry> entries;
        entries.reserve(m_files.size());
        for (const auto& fm : m_files) entries.push_back(fm.entry);
        return entries;
    }

private:
    struct FileMapping {
        TorrentFileEntry entry;
        std::filesystem::path full_path;
    };

    std::vector<FileMapping> m_files;
    std::filesystem::path m_base_dir;
    uint64_t m_total_length{0};

    // Direct adoption of aria2 findFirstDiskWriterEntry / binary search
    size_t FindFileIndex(uint64_t offset) const {
        if (m_files.empty()) return 0;
        auto it = std::upper_bound(m_files.begin(), m_files.end(), offset,
            [](uint64_t off, const FileMapping& fm) {
                return off < fm.entry.offset_in_torrent;
            });
        if (it == m_files.begin()) return 0;
        return static_cast<size_t>(std::distance(m_files.begin(), it - 1));
    }
};

} // namespace openidm::torrent
