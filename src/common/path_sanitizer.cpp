// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/path_sanitizer.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace openidm {

std::string PathSanitizer::UrlDecode(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() &&
            std::isxdigit(static_cast<unsigned char>(in[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
            char hex[3] = {in[i + 1], in[i + 2], 0};
            out.push_back(static_cast<char>(std::strtol(hex, nullptr, 16)));
            i += 2;
        } else if (in[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

namespace {


bool IsReservedDosName(std::string_view stem) {
    std::string upper;
    for (char c : stem) upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    static const std::set<std::string> kReserved = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };
    return kReserved.count(upper) > 0;
}

std::string TruncateUtf8Safe(const std::string& s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    size_t pos = max_bytes;
    while (pos > 0 && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80) {
        --pos;
    }
    return s.substr(0, pos);
}

} // namespace

std::string TaskTypeToString(TaskType t) {
    switch (t) {
        case TaskType::kHttpFile: return "HTTP_FILE";
        case TaskType::kStreamHls: return "STREAM_HLS";
        case TaskType::kStreamDash: return "STREAM_DASH";
        case TaskType::kStreamMss: return "STREAM_MSS";
        case TaskType::kFtp: return "FTP";
        case TaskType::kTorrent: return "TORRENT";
        case TaskType::kRtsp: return "RTSP";
        case TaskType::kSiteGrabber: return "SITE_GRABBER";
    }
    return "HTTP_FILE";
}

TaskType TaskTypeFromString(const std::string& s) {
    if (s == "STREAM_HLS") return TaskType::kStreamHls;
    if (s == "STREAM_DASH") return TaskType::kStreamDash;
    if (s == "STREAM_MSS") return TaskType::kStreamMss;
    if (s == "FTP" || s == "FTPS") return TaskType::kFtp;
    if (s == "TORRENT" || s == "BITTORRENT") return TaskType::kTorrent;
    if (s == "RTSP" || s == "RTSPS" || s == "MMS" || s == "RTMP") return TaskType::kRtsp;
    if (s == "SITE_GRABBER" || s == "CRAWLER") return TaskType::kSiteGrabber;
    return TaskType::kHttpFile;
}

std::string TaskStateToString(TaskState s) {
    switch (s) {
        case TaskState::kQueued: return "QUEUED";
        case TaskState::kProbing: return "PROBING";
        case TaskState::kDownloading: return "DOWNLOADING";
        case TaskState::kMuxing: return "MUXING";
        case TaskState::kVerifying: return "VERIFYING";
        case TaskState::kFinalizing: return "FINALIZING";
        case TaskState::kPaused: return "PAUSED";
        case TaskState::kCompleted: return "COMPLETED";
        case TaskState::kFailed: return "FAILED";
        case TaskState::kCanceled: return "CANCELED";
    }
    return "QUEUED";
}

TaskState TaskStateFromString(const std::string& s) {
    if (s == "PROBING") return TaskState::kProbing;
    if (s == "DOWNLOADING") return TaskState::kDownloading;
    if (s == "MUXING") return TaskState::kMuxing;
    if (s == "VERIFYING") return TaskState::kVerifying;
    if (s == "FINALIZING") return TaskState::kFinalizing;
    if (s == "PAUSED") return TaskState::kPaused;
    if (s == "COMPLETED") return TaskState::kCompleted;
    if (s == "FAILED") return TaskState::kFailed;
    if (s == "CANCELED") return TaskState::kCanceled;
    return TaskState::kQueued;
}

std::string ConflictPolicyToString(ConflictPolicy p) {
    switch (p) {
        case ConflictPolicy::kAutoRename:   return "auto_rename";
        case ConflictPolicy::kOverwrite:    return "overwrite";
        case ConflictPolicy::kFailIfExists: return "fail_if_exists";
    }
    return "auto_rename";
}

ConflictPolicy ConflictPolicyFromString(const std::string& s) {
    if (s == "overwrite" || s == "OVERWRITE") return ConflictPolicy::kOverwrite;
    if (s == "fail_if_exists" || s == "FAIL_IF_EXISTS") return ConflictPolicy::kFailIfExists;
    return ConflictPolicy::kAutoRename;
}

std::string InferCategoryFromFilename(const std::string& filename) {
    auto dot = filename.find_last_of('.');
    if (dot == std::string::npos) return "General";
    std::string ext = filename.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    static const std::set<std::string> kVideo = {"mp4", "mkv", "webm", "avi", "mov", "flv", "ts", "m4v", "wmv"};
    static const std::set<std::string> kAudio = {"mp3", "m4a", "flac", "wav", "ogg", "aac", "opus"};
    static const std::set<std::string> kCompressed = {"zip", "rar", "7z", "tar", "gz", "bz2", "xz", "tgz", "zst"};
    static const std::set<std::string> kDocuments = {"pdf", "doc", "docx", "xls", "xlsx", "ppt", "pptx", "txt", "md", "epub", "csv"};
    static const std::set<std::string> kPrograms = {"exe", "msi", "iso", "img", "dmg", "deb", "rpm", "apk", "msix"};
    static const std::set<std::string> kImages = {"jpg", "jpeg", "png", "gif", "webp", "svg", "bmp", "ico", "tiff"};
    static const std::set<std::string> kTorrents = {"torrent"};

    if (kVideo.count(ext)) return "Video";
    if (kAudio.count(ext)) return "Audio";
    if (kCompressed.count(ext)) return "Compressed";
    if (kDocuments.count(ext)) return "Documents";
    if (kPrograms.count(ext)) return "Programs";
    if (kImages.count(ext)) return "Images";
    if (kTorrents.count(ext)) return "Torrents";
    return "General";
}

std::string InferCategoryFromMime(const std::string& mime_type) {
    if (mime_type.empty()) return "General";
    std::string m = mime_type;
    for (char& c : m) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto semi = m.find(';');
    if (semi != std::string::npos) m = m.substr(0, semi);
    while (!m.empty() && (m.back() == ' ' || m.back() == '\t')) m.pop_back();

    if (m.rfind("video/", 0) == 0 || m == "application/x-mpegurl" || m == "application/vnd.apple.mpegurl") return "Video";
    if (m.rfind("audio/", 0) == 0) return "Audio";
    if (m.rfind("image/", 0) == 0) return "Images";
    if (m == "application/pdf" || m == "application/msword" ||
        m == "application/vnd.openxmlformats-officedocument.wordprocessingml.document" ||
        m == "application/vnd.ms-excel" ||
        m == "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet" ||
        m == "application/vnd.ms-powerpoint" ||
        m == "application/vnd.openxmlformats-officedocument.presentationml.presentation" ||
        m == "application/epub+zip" || m == "text/plain" || m == "text/markdown" || m == "text/csv") {
        return "Documents";
    }
    if (m == "application/zip" || m == "application/x-tar" || m == "application/x-rar-compressed" ||
        m == "application/x-7z-compressed" || m == "application/gzip" || m == "application/x-bzip2" ||
        m == "application/x-xz" || m == "application/zstd") {
        return "Compressed";
    }
    if (m == "application/x-msdownload" || m == "application/vnd.microsoft.portable-executable" ||
        m == "application/x-apple-diskimage" || m == "application/vnd.android.package-archive" ||
        m == "application/x-iso9660-image" || m == "application/x-msi") {
        return "Programs";
    }
    if (m == "application/x-bittorrent") return "Torrents";
    return "General";
}

int64_t NowEpochMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string GenerateUuidV4() {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t ab = dist(rng);
    uint64_t cd = dist(rng);
    ab = (ab & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    cd = (cd & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    char buf[40];
    snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx",
             static_cast<uint32_t>(ab >> 32),
             static_cast<uint32_t>((ab >> 16) & 0xFFFF),
             static_cast<uint32_t>(ab & 0xFFFF),
             static_cast<uint32_t>(cd >> 48),
             static_cast<unsigned long long>(cd & 0xFFFFFFFFFFFFULL));
    return std::string(buf);
}

std::string PathSanitizer::SanitizeFileName(std::string_view raw_name, std::string_view default_name) {
    std::string s(raw_name);
    auto last_slash = s.find_last_of("/\\");
    if (last_slash != std::string::npos) {
        s = s.substr(last_slash + 1);
    }
    if (s.size() >= 2 && std::isalpha(static_cast<unsigned char>(s[0])) && s[1] == ':') {
        s = s.substr(2);
    }
    auto colon_pos = s.find(':');
    if (colon_pos != std::string::npos) {
        s = s.substr(0, colon_pos);
    }

    std::string cleaned;
    cleaned.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0xE2 && i + 2 < s.size() &&
            static_cast<unsigned char>(s[i + 1]) == 0x80 &&
            (static_cast<unsigned char>(s[i + 2]) >= 0xAA && static_cast<unsigned char>(s[i + 2]) <= 0xAE)) {
            i += 2;
            continue;
        }
        if (c < 0x20 || c == 0x7F || c == '<' || c == '>' || c == ':' ||
            c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') {
            cleaned.push_back('_');
        } else {
            cleaned.push_back(static_cast<char>(c));
        }
    }

    while (!cleaned.empty() && (cleaned.front() == ' ' || cleaned.front() == '.')) {
        cleaned.erase(cleaned.begin());
    }
    while (!cleaned.empty() && (cleaned.back() == ' ' || cleaned.back() == '.')) {
        cleaned.pop_back();
    }

    if (cleaned.empty() || cleaned == "." || cleaned == "..") {
        cleaned = std::string(default_name);
    }

    std::string stem = cleaned;
    std::string ext;
    auto dot = cleaned.find_last_of('.');
    if (dot != std::string::npos && dot > 0 && cleaned.size() - dot <= 16) {
        stem = cleaned.substr(0, dot);
        ext = cleaned.substr(dot);
    }

    while (!stem.empty() && (stem.back() == ' ' || stem.back() == '.')) {
        stem.pop_back();
    }
    if (stem.empty()) stem = "download";

    if (IsReservedDosName(stem)) {
        stem = "_" + stem;
    }

    stem = TruncateUtf8Safe(stem, 200);
    return stem + ext;
}

std::string PathSanitizer::ExtractFromContentDisposition(std::string_view header_value) {
    std::string lower(header_value);
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    auto star_pos = lower.find("filename*=");
    if (star_pos != std::string::npos) {
        std::string_view val = header_value.substr(star_pos + 10);
        while (!val.empty() && val.front() == ' ') val.remove_prefix(1);
        auto semi = val.find(';');
        if (semi != std::string_view::npos) val = val.substr(0, semi);
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            val = val.substr(1, val.size() - 2);
        }
        auto quote2 = val.find("''");
        if (quote2 != std::string_view::npos) {
            val = val.substr(quote2 + 2);
        }
        std::string decoded = UrlDecode(val);
        if (!decoded.empty()) return SanitizeFileName(decoded);
    }

    auto fn_pos = lower.find("filename=");
    if (fn_pos != std::string::npos) {
        std::string_view val = header_value.substr(fn_pos + 9);
        while (!val.empty() && val.front() == ' ') val.remove_prefix(1);
        if (!val.empty() && val.front() == '"') {
            val.remove_prefix(1);
            auto end_q = val.find('"');
            if (end_q != std::string_view::npos) val = val.substr(0, end_q);
        } else {
            auto semi = val.find(';');
            if (semi != std::string_view::npos) val = val.substr(0, semi);
        }
        std::string decoded = UrlDecode(val);
        if (!decoded.empty()) return SanitizeFileName(decoded);
    }
    return "";
}

std::string PathSanitizer::ExtractFromUrl(std::string_view url, std::string_view fallback_ext) {
    std::string_view work = url;
    auto q = work.find('?');
    if (q != std::string_view::npos) work = work.substr(0, q);
    auto h = work.find('#');
    if (h != std::string_view::npos) work = work.substr(0, h);

    auto scheme = work.find("://");
    if (scheme != std::string_view::npos) {
        work = work.substr(scheme + 3);
        auto slash = work.find('/');
        if (slash != std::string_view::npos) {
            work = work.substr(slash + 1);
        } else {
            work = "";
        }
    }
    while (!work.empty() && work.back() == '/') work.remove_suffix(1);
    auto last_slash = work.find_last_of('/');
    if (last_slash != std::string_view::npos) {
        work = work.substr(last_slash + 1);
    }

    std::string decoded = UrlDecode(work);
    if (decoded.empty()) decoded = "download";
    std::string sanitized = SanitizeFileName(decoded);
    if (!fallback_ext.empty() && sanitized.find('.') == std::string::npos) {
        sanitized += (fallback_ext.front() == '.' ? std::string(fallback_ext) : "." + std::string(fallback_ext));
    }
    return sanitized;
}

Result<std::filesystem::path> PathSanitizer::ResolveSafeOutputPath(
    const std::filesystem::path& save_dir,
    std::string_view candidate_filename,
    ConflictPolicy policy) {
    std::error_code ec;
    std::filesystem::create_directories(save_dir, ec);
    if (ec) {
        return Result<std::filesystem::path>::Err("ERR_STORAGE_DIR_CREATE", "Failed to create save directory: " + ec.message());
    }

    std::filesystem::path canonical_dir = std::filesystem::weakly_canonical(save_dir, ec);
    if (ec) canonical_dir = save_dir;

    std::string safe_name = SanitizeFileName(candidate_filename);
    std::filesystem::path target = canonical_dir / safe_name;

    if (target.parent_path() != canonical_dir) {
        return Result<std::filesystem::path>::Err("ERR_SEC_PATH_TRAVERSAL", "Resolved path escaped target directory");
    }

    if (!std::filesystem::exists(target, ec)) {
        return Result<std::filesystem::path>::Ok(target);
    }

    if (policy == ConflictPolicy::kOverwrite) {
        return Result<std::filesystem::path>::Ok(target);
    }
    if (policy == ConflictPolicy::kFailIfExists) {
        return Result<std::filesystem::path>::Err("ERR_FILE_EXISTS", "Destination file already exists");
    }

    std::string stem = target.stem().string();
    std::string ext = target.extension().string();
    for (int i = 1; i <= 9999; ++i) {
        std::filesystem::path candidate = canonical_dir / (stem + " (" + std::to_string(i) + ")" + ext);
        // Motrix FinalNamePicker isTaken pattern: verify neither the candidate output file
        // NOR its corresponding in-progress .part file exists before picking the slot.
        if (!std::filesystem::exists(candidate, ec) && !std::filesystem::exists(candidate.string() + ".part", ec)) {
            return Result<std::filesystem::path>::Ok(candidate);
        }
    }
    return Result<std::filesystem::path>::Err("ERR_FILE_RENAME_EXHAUSTED", "Could not find free auto-rename slot");
}

Result<std::filesystem::path> PathSanitizer::CommitPartFile(
    const std::filesystem::path& part_file,
    const std::filesystem::path& save_dir,
    std::string_view final_filename,
    ConflictPolicy policy,
    uint64_t expected_bytes) {
    std::error_code ec;
    if (!std::filesystem::exists(part_file, ec)) {
        return Result<std::filesystem::path>::Err("ERR_STORAGE_PART_MISSING", "Part file does not exist: " + part_file.string());
    }

    if (expected_bytes > 0) {
        uint64_t actual_size = std::filesystem::file_size(part_file, ec);
        if (!ec && actual_size != expected_bytes) {
            return Result<std::filesystem::path>::Err(
                "ERR_STORAGE_PART_SIZE_MISMATCH",
                "Part file size (" + std::to_string(actual_size) + ") does not match expected size (" + std::to_string(expected_bytes) + ")");
        }
    }

    std::filesystem::path canonical_dir = std::filesystem::weakly_canonical(save_dir, ec);
    std::filesystem::create_directories(canonical_dir, ec);

    std::string safe_name = SanitizeFileName(final_filename);
    std::filesystem::path initial_target = canonical_dir / safe_name;

    // Helper for attempting an atomic non-replacing move
    auto try_atomic_move_no_replace = [](const std::filesystem::path& src, const std::filesystem::path& dst) -> int {
        // Return 0 on success, 1 on destination already exists, -1 on other failure
#ifdef _WIN32
        std::wstring wsrc = src.wstring();
        std::wstring wdst = dst.wstring();
        // MoveFileExW with flag 0 does NOT overwrite existing destination
        if (MoveFileExW(wsrc.c_str(), wdst.c_str(), 0)) {
            return 0;
        }
        DWORD err = GetLastError();
        if (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS) {
            return 1;
        }
        // If failed due to cross-volume, try CopyFileW with bFailIfExists = TRUE
        if (err == ERROR_NOT_SAME_DEVICE) {
            if (CopyFileW(wsrc.c_str(), wdst.c_str(), TRUE)) {
                DeleteFileW(wsrc.c_str());
                return 0;
            }
            DWORD copy_err = GetLastError();
            if (copy_err == ERROR_ALREADY_EXISTS || copy_err == ERROR_FILE_EXISTS) {
                return 1;
            }
            // Clean up potentially incomplete destination file on failure
            DeleteFileW(wdst.c_str());
        }
        return -1;
#else
        // POSIX Atomic non-replacing move:
        // 1. Try link(src, dst) which is atomic in POSIX kernel and fails with EEXIST if dst exists.
        //    Crucially, it never silently overwrites dst.
        if (::link(src.c_str(), dst.c_str()) == 0) {
            ::unlink(src.c_str());
            return 0;
        }
        if (errno == EEXIST) {
            return 1;
        }

        // 2. Cross-device (EXDEV) or filesystem where link() is rejected (e.g., FAT/exFAT):
        //    Atomically create dst exclusively with O_CREAT | O_EXCL to prevent concurrent races.
        int dst_fd = ::open(dst.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (dst_fd < 0) {
            if (errno == EEXIST) return 1;
            return -1;
        }

        int src_fd = ::open(src.c_str(), O_RDONLY);
        if (src_fd < 0) {
            ::close(dst_fd);
            ::unlink(dst.c_str());
            return -1;
        }

        char buf[65536];
        bool copy_ok = true;
        while (true) {
            ssize_t n = ::read(src_fd, buf, sizeof(buf));
            if (n < 0) { copy_ok = false; break; }
            if (n == 0) break;
            ssize_t written = 0;
            while (written < n) {
                ssize_t w = ::write(dst_fd, buf + written, n - written);
                if (w <= 0) { copy_ok = false; break; }
                written += w;
            }
            if (!copy_ok) break;
        }
        ::close(src_fd);
        ::close(dst_fd);

        if (copy_ok) {
            ::unlink(src.c_str());
            return 0;
        } else {
            ::unlink(dst.c_str()); // Clean up partial output on failure!
            return -1;
        }
#endif
    };

    if (policy == ConflictPolicy::kFailIfExists) {
        int mv = try_atomic_move_no_replace(part_file, initial_target);
        if (mv == 0) {
            return Result<std::filesystem::path>::Ok(initial_target);
        }
        if (mv == 1) {
            return Result<std::filesystem::path>::Err("ERR_FILE_EXISTS", "Destination file already exists: " + initial_target.string());
        }
        return Result<std::filesystem::path>::Err("ERR_STORAGE_COMMIT_FAILED", "Failed to commit part file to: " + initial_target.string());
    }

    if (policy == ConflictPolicy::kOverwrite) {
#ifdef _WIN32
        std::wstring wsrc = part_file.wstring();
        std::wstring wdst = initial_target.wstring();
        if (MoveFileExW(wsrc.c_str(), wdst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            return Result<std::filesystem::path>::Ok(initial_target);
        }
        if (CopyFileW(wsrc.c_str(), wdst.c_str(), FALSE)) {
            DeleteFileW(wsrc.c_str());
            return Result<std::filesystem::path>::Ok(initial_target);
        }
        return Result<std::filesystem::path>::Err("ERR_STORAGE_COMMIT_FAILED", "Overwrite commit failed: " + std::to_string(GetLastError()));
#else
        std::filesystem::rename(part_file, initial_target, ec);
        if (!ec) return Result<std::filesystem::path>::Ok(initial_target);
        std::filesystem::copy_file(part_file, initial_target, std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) {
            std::filesystem::remove(part_file, ec);
            return Result<std::filesystem::path>::Ok(initial_target);
        }
        return Result<std::filesystem::path>::Err("ERR_STORAGE_COMMIT_FAILED", "Overwrite commit failed: " + ec.message());
#endif
    }

    // AutoRename policy:
    int initial_mv = try_atomic_move_no_replace(part_file, initial_target);
    if (initial_mv == 0) {
        return Result<std::filesystem::path>::Ok(initial_target);
    }

    std::string stem = initial_target.stem().string();
    std::string ext = initial_target.extension().string();
    for (int i = 1; i <= 9999; ++i) {
        std::filesystem::path candidate = canonical_dir / (stem + " (" + std::to_string(i) + ")" + ext);
        int cand_mv = try_atomic_move_no_replace(part_file, candidate);
        if (cand_mv == 0) {
            return Result<std::filesystem::path>::Ok(candidate);
        }
        if (cand_mv == 1) {
            continue; // Candidate already exists; try next slot
        }
        return Result<std::filesystem::path>::Err("ERR_STORAGE_COMMIT_FAILED", "Failed to commit candidate: " + candidate.string());
    }

    return Result<std::filesystem::path>::Err("ERR_FILE_RENAME_EXHAUSTED", "Could not find free auto-rename slot");
}

std::filesystem::path PathSanitizer::GetExecutableDir(const char* argv0) {
    std::error_code ec;
#ifdef _WIN32
    wchar_t buf[32768]{};
    DWORD len = GetModuleFileNameW(nullptr, buf, 32767);
    if (len > 0 && len < 32767) {
        std::filesystem::path p(buf);
        if (p.has_parent_path()) return p.parent_path();
    }
#else
    auto self_exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec && self_exe.has_parent_path()) {
        return self_exe.parent_path();
    }
#endif
    if (argv0 && argv0[0] != '\0') {
        auto p = std::filesystem::weakly_canonical(argv0, ec);
        if (!ec && p.has_parent_path()) return p.parent_path();
    }
    return std::filesystem::current_path(ec);
}

bool PathSanitizer::MigrateLegacyDataIfNeeded(const std::filesystem::path& target_radium_dir) {
    std::error_code ec;
    // Only migrate if target does not already have a state.db
    if (std::filesystem::exists(target_radium_dir / "state.db", ec)) {
        return false;
    }

    std::vector<std::filesystem::path> legacy_dirs;
#ifdef _WIN32
    if (const char* local_app_data = std::getenv("LOCALAPPDATA")) {
        if (local_app_data[0] != '\0') {
            legacy_dirs.push_back(std::filesystem::path(local_app_data) / "Flux Download Manager");
            legacy_dirs.push_back(std::filesystem::path(local_app_data) / "OpenIDM");
        }
    }
#else
    if (const char* env_flux = std::getenv("FLUX_DATA_DIR")) {
        if (env_flux[0] != '\0') legacy_dirs.push_back(std::filesystem::path(env_flux));
    }
    if (const char* env_dir = std::getenv("OPENIDM_DATA_DIR")) {
        if (env_dir[0] != '\0') legacy_dirs.push_back(std::filesystem::path(env_dir));
    }
    legacy_dirs.push_back(std::filesystem::temp_directory_path(ec) / "Flux_Data");
    legacy_dirs.push_back(std::filesystem::temp_directory_path(ec) / "openidm_data");
#endif

    for (const auto& legacy_dir : legacy_dirs) {
        if (std::filesystem::exists(legacy_dir / "state.db", ec)) {
            std::filesystem::create_directories(target_radium_dir, ec);
            const char* files_to_copy[] = {"state.db", "state.db-wal", "state.db-shm", "settings.json"};
            for (const char* f : files_to_copy) {
                std::filesystem::path src = legacy_dir / f;
                if (std::filesystem::exists(src, ec)) {
                    std::filesystem::copy_file(src, target_radium_dir / f, std::filesystem::copy_options::update_existing, ec);
                }
            }
            return true;
        }
    }
    return false;
}

std::filesystem::path PathSanitizer::GetDefaultAppDataDir() {
    std::error_code ec;
#ifdef _WIN32
    if (const char* env_radium = std::getenv("RADIUM_DATA_DIR")) {
        if (env_radium[0] != '\0') return std::filesystem::path(env_radium);
    }
    if (const char* env_flux = std::getenv("FLUX_DATA_DIR")) {
        if (env_flux[0] != '\0') return std::filesystem::path(env_flux);
    }
    if (const char* local_app_data = std::getenv("LOCALAPPDATA")) {
        if (local_app_data[0] != '\0') {
            std::filesystem::path radium_dir = std::filesystem::path(local_app_data) / "Radium Download Manager";
            MigrateLegacyDataIfNeeded(radium_dir);
            return radium_dir;
        }
    }
#else
    if (const char* env_radium = std::getenv("RADIUM_DATA_DIR")) {
        if (env_radium[0] != '\0') return std::filesystem::path(env_radium);
    }
    if (const char* env_flux = std::getenv("FLUX_DATA_DIR")) {
        if (env_flux[0] != '\0') return std::filesystem::path(env_flux);
    }
    if (const char* env_legacy = std::getenv("OPENIDM_DATA_DIR")) {
        if (env_legacy[0] != '\0') return std::filesystem::path(env_legacy);
    }
#endif
    std::filesystem::path fallback_dir = std::filesystem::temp_directory_path(ec) / "Radium_Data";
    MigrateLegacyDataIfNeeded(fallback_dir);
    return fallback_dir;
}

std::filesystem::path PathSanitizer::GetDefaultDownloadsDir(const std::filesystem::path& exe_dir) {
    (void)exe_dir;
#ifdef _WIN32
    if (const char* env_radium_dl = std::getenv("RADIUM_DOWNLOAD_DIR")) {
        if (env_radium_dl[0] != '\0') return std::filesystem::path(env_radium_dl);
    }
    if (const char* env_dl = std::getenv("FLUX_DOWNLOAD_DIR")) {
        if (env_dl[0] != '\0') return std::filesystem::path(env_dl);
    }
    if (const char* user_profile = std::getenv("USERPROFILE")) {
        if (user_profile[0] != '\0') {
            return std::filesystem::path(user_profile) / "Downloads" / "Radium";
        }
    }
#else
    if (const char* env_radium_dl = std::getenv("RADIUM_DOWNLOAD_DIR")) {
        if (env_radium_dl[0] != '\0') return std::filesystem::path(env_radium_dl);
    }
    if (const char* env_dl = std::getenv("FLUX_DOWNLOAD_DIR")) {
        if (env_dl[0] != '\0') return std::filesystem::path(env_dl);
    }
    if (const char* env_dl_old = std::getenv("OPENIDM_DOWNLOAD_DIR")) {
        if (env_dl_old[0] != '\0') return std::filesystem::path(env_dl_old);
    }
#endif
    std::error_code ec;
    return std::filesystem::temp_directory_path(ec) / "Radium_Downloads";
}

std::filesystem::path PathSanitizer::FindWorkerBinary(
    std::string_view worker_name,
    const std::filesystem::path& exe_dir) {
    std::error_code ec;
    std::string base(worker_name);
    if (base.size() > 4 && base.substr(base.size() - 4) == ".exe") {
        base = base.substr(0, base.size() - 4);
    }

#ifdef _WIN32
    const std::string native_name = base + ".exe";
#else
    const std::string native_name = base;
#endif

    // 1. Search bundled and local tools directories.
    // Prioritize tools/win-x64 as the single standard location, followed by tools/
    // for backward compatibility with existing installations.
    std::vector<std::filesystem::path> search_dirs;
    search_dirs.push_back(exe_dir / "tools" / "win-x64");
    search_dirs.push_back(exe_dir / "tools");

    // Check AppData tools directories (for portable or user-level stream workers)
    auto app_data_dir = GetDefaultAppDataDir();
    search_dirs.push_back(app_data_dir / "tools" / "win-x64");
    search_dirs.push_back(app_data_dir / "tools");

#ifdef _WIN32
    if (const char* local_app_data = std::getenv("LOCALAPPDATA")) {
        if (local_app_data[0] != '\0') {
            std::filesystem::path legacy_dir = std::filesystem::path(local_app_data) / "OpenIDM" / "tools";
            search_dirs.push_back(legacy_dir / "win-x64");
            search_dirs.push_back(legacy_dir);
        }
    }
#endif

    // Search parent directories up to 3 levels (for test and development builds in build/win-x64/Release/)
    std::filesystem::path cur = exe_dir;
    for (int depth = 0; depth < 3; ++depth) {
        if (!cur.has_parent_path()) break;
        cur = cur.parent_path();
        search_dirs.push_back(cur / "tools" / "win-x64");
        search_dirs.push_back(cur / "tools");
    }

    search_dirs.push_back(std::filesystem::current_path(ec) / "tools" / "win-x64");
    search_dirs.push_back(std::filesystem::current_path(ec) / "tools");
    search_dirs.push_back(exe_dir);

    for (const auto& dir : search_dirs) {
        if (dir.empty()) continue;
        std::filesystem::path candidate = dir / native_name;
        if (std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
        if (base == "yt-dlp") {
            std::filesystem::path min_candidate = dir / "yt-dlp_min.exe";
            if (std::filesystem::exists(min_candidate, ec)) {
                return min_candidate;
            }
        }
    }

    // 2. Search system PATH environment variable (; on Windows, : on POSIX)
    // Allows Core installation users to use existing system FFmpeg / worker installations
    if (const char* path_env = std::getenv("PATH")) {
#ifdef _WIN32
        const char sep = ';';
#else
        const char sep = ':';
#endif
        std::stringstream ss(path_env);
        std::string item;
        while (std::getline(ss, item, sep)) {
            if (item.empty()) continue;
            std::filesystem::path candidate = std::filesystem::path(item) / native_name;
            if (std::filesystem::exists(candidate, ec)) {
                return candidate;
            }
            if (base == "yt-dlp") {
                std::filesystem::path min_candidate = std::filesystem::path(item) / "yt-dlp_min.exe";
                if (std::filesystem::exists(min_candidate, ec)) {
                    return min_candidate;
                }
            }
        }
    }

    // 3. Check well-known installation paths for Node.js if searching for node
    if (base == "node") {
        const std::filesystem::path standard_node_paths[] = {
            "C:\\Program Files\\nodejs\\node.exe",
            "C:\\Program Files (x86)\\nodejs\\node.exe"
        };
        for (const auto& np : standard_node_paths) {
            if (std::filesystem::exists(np, ec)) return np;
        }
    }

    // Fall back to the standard location under the executable directory
    return exe_dir / "tools" / "win-x64" / native_name;
}

Result<uint64_t> PathSanitizer::CheckAvailableDiskSpace(
    const std::filesystem::path& dir,
    uint64_t required_bytes,
    uint64_t reserve_bytes) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (!std::filesystem::exists(dir, ec)) {
        return Result<uint64_t>::Err(
            "ERR_DESTINATION_UNAVAILABLE",
            "Destination directory is unavailable or cannot be created: " + dir.string(),
            false,
            true);
    }
    auto sp = std::filesystem::space(dir, ec);
    if (ec || sp.available == static_cast<std::uintmax_t>(-1)) {
        // If filesystem does not report space, allow operation to proceed to write check
        return Result<uint64_t>::Ok(UINT64_MAX);
    }
    uint64_t avail = static_cast<uint64_t>(sp.available);
    bool overflow = (required_bytes > UINT64_MAX - reserve_bytes);
    uint64_t needed = overflow ? UINT64_MAX : (required_bytes + reserve_bytes);
    if (required_bytes > 0 && (overflow || avail < needed)) {
        return Result<uint64_t>::Err(
            "ERR_DISK_FULL",
            "Insufficient disk space in " + dir.string() + ": required " +
                std::to_string(needed / 1048576ULL) + " MiB, available " +
                std::to_string(avail / 1048576ULL) + " MiB",
            false,
            true);
    }
    return Result<uint64_t>::Ok(avail);
}

bool PathSanitizer::IsWebpageWatchUrl(std::string_view url) {
    if (url.empty()) return false;
    std::string lower(url);
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // YouTube watch and shorts URLs
    if (lower.find("youtube.com/watch") != std::string::npos ||
        lower.find("youtube.com/shorts/") != std::string::npos ||
        lower.find("youtu.be/") != std::string::npos) {
        return true;
    }

    // Vimeo watch URLs (e.g. vimeo.com/12345678)
    if (lower.find("vimeo.com/") != std::string::npos &&
        lower.find(".mp4") == std::string::npos &&
        lower.find(".m3u8") == std::string::npos &&
        lower.find(".mpd") == std::string::npos) {
        return true;
    }

    // Twitch stream/video URLs
    if (lower.find("twitch.tv/") != std::string::npos &&
        lower.find(".m3u8") == std::string::npos &&
        lower.find(".mpd") == std::string::npos) {
        return true;
    }

    // Generic HTML pages without direct media format
    size_t qmark = lower.find('?');
    std::string path = (qmark != std::string::npos) ? lower.substr(0, qmark) : lower;
    if (path.ends_with(".html") || path.ends_with(".htm") || path.ends_with(".php") ||
        path.ends_with(".asp") || path.ends_with(".aspx")) {
        return true;
    }
    return false;
}

} // namespace openidm
