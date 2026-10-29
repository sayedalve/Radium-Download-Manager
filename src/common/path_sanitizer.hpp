// SPDX-License-Identifier: Apache-2.0
// Radium -- Path Sanitization, Content-Disposition Parsing, Worker Discovery & Disk Space Checks
// Implements NFR-006, FR-001, FR-026, THREAT-003, BUG-005, BUG-009.
#pragma once

#include <filesystem>
#include <string>
#include "common/types.hpp"

namespace openidm {

class PathSanitizer {
public:
    // Sanitizes an untrusted filename (from Content-Disposition, URL, or webpage <title>)
    // into a safe basename with zero directory traversal, ADS, or DOS device name risks.
    static std::string SanitizeFileName(std::string_view raw_name, std::string_view default_name = "download.bin");

    // Extracts filename from HTTP Content-Disposition header (supporting RFC 5987 filename*=UTF-8''...)
    static std::string ExtractFromContentDisposition(std::string_view header_value);

    // Extracts basename from a URL path
    static std::string ExtractFromUrl(std::string_view url, std::string_view fallback_ext = "");

    // URL percent-decoding helper
    static std::string UrlDecode(std::string_view in);

    // Resolves a target path strictly inside save_dir, applying ConflictPolicy (Auto-Rename (1), Overwrite, etc.)
    static Result<std::filesystem::path> ResolveSafeOutputPath(
        const std::filesystem::path& save_dir,
        std::string_view candidate_filename,
        ConflictPolicy policy = ConflictPolicy::kAutoRename);

    // Atomically commits a .part file to its final destination path with optional size verification
    static Result<std::filesystem::path> CommitPartFile(
        const std::filesystem::path& part_file,
        const std::filesystem::path& save_dir,
        std::string_view final_filename,
        ConflictPolicy policy = ConflictPolicy::kAutoRename,
        uint64_t expected_bytes = 0);

    // Resolves the directory containing the running Radium executable (GetModuleFileNameW / /proc/self/exe)
    static std::filesystem::path GetExecutableDir(const char* argv0 = nullptr);

    // Safely migrates legacy OpenIDM/Flux state database and settings to Radium Download Manager
    static bool MigrateLegacyDataIfNeeded(const std::filesystem::path& target_radium_dir);

    // Resolves platform-appropriate application data directory (%LOCALAPPDATA%\Radium Download Manager on Windows 11)
    static std::filesystem::path GetDefaultAppDataDir();

    // Resolves platform-appropriate default downloads directory (%USERPROFILE%\Downloads\Radium on Windows 11)
    static std::filesystem::path GetDefaultDownloadsDir(const std::filesystem::path& exe_dir);
    static std::filesystem::path GetDefaultDownloadDir() { return GetDefaultDownloadsDir(GetExecutableDir()); }

    // Dynamically discovers a worker executable (e.g., "N_m3u8DL-RE", "ffmpeg", or "yt-dlp") across:
    //   1. <exe_dir>/tools/win-x64/<name>(.exe) [standard packaged location]
    //   2. <exe_dir>/tools/<name>(.exe) [backward compatibility for existing installations]
    //   3. %LOCALAPPDATA%/Radium Download Manager/tools/win-x64/ and tools/
    //   4. Parent directories up to 3 levels (development/test builds)
    //   5. System PATH environment variable (; on Windows, : on POSIX)
    //   6. Fallback: <exe_dir>/tools/win-x64/<name>(.exe)
    static std::filesystem::path FindWorkerBinary(
        std::string_view worker_name,
        const std::filesystem::path& exe_dir);

    // Verifies that target directory has at least required_bytes + reserve_bytes available free space
    static Result<uint64_t> CheckAvailableDiskSpace(
        const std::filesystem::path& dir,
        uint64_t required_bytes,
        uint64_t reserve_bytes = 4ULL * 1048576ULL);

    // Classifies whether a URL is an HTML webpage (e.g. YouTube watch page, Vimeo page)
    // rather than a direct downloadable media file or stream manifest
    static bool IsWebpageWatchUrl(std::string_view url);
};

} // namespace openidm
