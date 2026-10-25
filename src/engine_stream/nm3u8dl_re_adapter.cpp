// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine_stream/nm3u8dl_re_adapter.hpp"

#include "brisk_engine/http_client.hpp"
#include "common/json_mini.hpp"
#include "common/logger.hpp"
#include "common/path_sanitizer.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace openidm::stream {

namespace {

std::string TrimView(std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
        sv.remove_suffix(1);
    }
    return std::string(sv);
}

std::string ToLowerStr(std::string_view sv) {
    std::string s(sv);
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string StripGooglevideoRangeParams(const std::string& url) {
    if (url.find("googlevideo.com") == std::string::npos && url.find("/videoplayback") == std::string::npos) {
        return url;
    }
    auto qpos = url.find('?');
    if (qpos == std::string::npos) return url;
    std::string base = url.substr(0, qpos);
    std::string query = url.substr(qpos + 1);
    std::ostringstream kept;
    bool first = true;
    std::istringstream qss(query);
    std::string pair;
    while (std::getline(qss, pair, '&')) {
        if (pair.empty()) continue;
        auto eq = pair.find('=');
        std::string key = (eq == std::string::npos) ? pair : pair.substr(0, eq);
        if (key == "range" || key == "rn" || key == "rbuf" || key == "ump" || key == "srfvp" || key == "alr" || key == "cpn") {
            continue;
        }
        if (!first) kept << "&";
        kept << pair;
        first = false;
    }
    std::string qs = kept.str();
    return qs.empty() ? base : (base + "?" + qs);
}

std::string ExtractBalancedJsonObject(const std::string& s, size_t start_pos) {
    size_t open_brace = s.find('{', start_pos);
    if (open_brace == std::string::npos) return "";
    int depth = 0;
    bool in_str = false;
    bool escaped = false;
    for (size_t i = open_brace; i < s.size(); ++i) {
        char c = s[i];
        if (in_str) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_str = false;
            }
        } else {
            if (c == '"') {
                in_str = true;
            } else if (c == '{') {
                ++depth;
            } else if (c == '}') {
                --depth;
                if (depth == 0) {
                    return s.substr(open_brace, i - open_brace + 1);
                }
            }
        }
    }
    return "";
}

std::string EscapeRspArg(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\r' || c == '\n' || c == '\0') continue;
        if (c == '"') out += "\\\"";
        else out.push_back(c);
    }
    return out;
}

std::string EscapeMuxPath(const std::filesystem::path& p) {
    std::string s = p.string();
    std::string out;
    for (char c : s) {
        if (c == ':') out += "\\:";
        else out.push_back(c);
    }
    return out;
}

// Verifies that muxed MP4/MKV output file is non-empty and starts with valid container magic bytes
} // namespace

bool Nm3u8dlReAdapter::VerifyContainerMagicBytes(const std::filesystem::path& file_path, const std::string& fmt) {
    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs) return false;
    uint8_t hdr[16]{};
    ifs.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
    std::streamsize got = ifs.gcount();
    if (got < 4) return false;

    std::string f = ToLowerStr(fmt);

    // Matroska / WebM EBML header: 0x1A 0x45 0xDF 0xA3
    if (f == "mkv" || f == "webm") {
        return (got >= 4 && hdr[0] == 0x1A && hdr[1] == 0x45 && hdr[2] == 0xDF && hdr[3] == 0xA3);
    }
    // ISO Base Media File Format (MP4, M4A, MOV): bytes 4..7 == "ftyp", "moov", "free", "mdat"
    if (f == "mp4" || f == "m4a" || f == "mov") {
        if (got >= 8 &&
            (std::memcmp(hdr + 4, "ftyp", 4) == 0 ||
             std::memcmp(hdr + 4, "moov", 4) == 0 ||
             std::memcmp(hdr + 4, "free", 4) == 0 ||
             std::memcmp(hdr + 4, "mdat", 4) == 0)) {
            return true;
        }
        return false;
    }
    // MP3: ID3 header ("ID3") or MPEG audio frame sync (0xFF followed by 0xE0..0xFF)
    if (f == "mp3") {
        if (got >= 3 && hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3') return true;
        if (got >= 2 && hdr[0] == 0xFF && (hdr[1] & 0xE0) == 0xE0) return true;
        return false;
    }
    // AAC: ADTS frame sync (0xFF followed by 0xF0..0xFF)
    if (f == "aac") {
        if (got >= 2 && hdr[0] == 0xFF && (hdr[1] & 0xF0) == 0xF0) return true;
        return false;
    }
    // FLAC: "fLaC" (0x66 0x4C 0x61 0x43)
    if (f == "flac") {
        return (got >= 4 && hdr[0] == 'f' && hdr[1] == 'L' && hdr[2] == 'a' && hdr[3] == 'C');
    }
    // Ogg / Opus: "OggS" (0x4F 0x67 0x67 0x53)
    if (f == "ogg" || f == "opus") {
        return (got >= 4 && hdr[0] == 'O' && hdr[1] == 'g' && hdr[2] == 'g' && hdr[3] == 'S');
    }
    // MPEG-TS: 0x47 sync byte
    if (f == "ts") {
        return (got >= 1 && hdr[0] == 0x47);
    }
    // Fallback for other media container formats: require at least 8 valid bytes
    return got >= 8;
}

std::string Nm3u8dlReAdapter::BuildYtDlpFormatSelector(
    std::string_view requested_filter,
    int target_height,
    bool wants_audio_only,
    std::string_view preferred_container) {
    std::string filter_s = ToLowerStr(TrimView(requested_filter));
    std::string cont = ToLowerStr(TrimView(preferred_container));
    if (cont.empty()) cont = "mp4";

    if (wants_audio_only || filter_s == "audio" || filter_s == "audio-only") {
        return "bestaudio[ext=m4a]/bestaudio[ext=aac]/bestaudio/best/ba";
    }

    if (target_height > 0) {
        std::string hs = std::to_string(target_height);
        int min_w = 0;
        int min_h = 0;
        if (target_height >= 2160) {
            min_w = 2600;
            min_h = 1450;
        } else if (target_height >= 1440) {
            min_w = 1950;
            min_h = 1100;
        } else if (target_height >= 1080) {
            min_w = 1300;
            min_h = 750;
        } else if (target_height >= 720) {
            min_w = 900;
            min_h = 500;
        } else if (target_height >= 480) {
            min_w = 650;
            min_h = 370;
        } else if (target_height >= 360) {
            min_w = 450;
            min_h = 250;
        } else {
            min_w = static_cast<int>(target_height * 1.2);
            min_h = static_cast<int>(target_height * 0.7);
        }
        std::string ws = std::to_string(min_w);
        std::string h_min = std::to_string(min_h);

        // Build a robust selector that supports:
        // 1. Standard 16:9 (e.g. 1920x1080)
        // 2. Cinematic/Widescreen 2:1 and 2.39:1 scope (e.g. 1920x960, 1920x800)
        // 3. Classic 4:3 (e.g. 1440x1080)
        // 4. Vertical videos / Shorts (e.g. 1080x1920 where width is 1080)
        // Strictly prevents silent fallback to lower qualities like 360p (/18).
        if (cont == "mkv" || cont == "webm") {
            return "bestvideo[height<=" + hs + "][width>=" + ws + "]+bestaudio/"
                   "bestvideo[height=" + hs + "]+bestaudio/"
                   "bestvideo[width=" + hs + "]+bestaudio/"
                   "bestvideo[height<=" + hs + "][height>=" + h_min + "]+bestaudio";
        }

        // When MP4 is requested, prioritize universally compatible H.264/AVC1 streams
        // (which matches the standard bitrate estimates and hardware decoders), then AV1/VP9 in MP4 container
        return "bestvideo[height<=" + hs + "][width>=" + ws + "][vcodec^=avc1]+bestaudio[ext=m4a]/"
               "bestvideo[height=" + hs + "][vcodec^=avc1]+bestaudio[ext=m4a]/"
               "bestvideo[width=" + hs + "][vcodec^=avc1]+bestaudio[ext=m4a]/"
               "bestvideo[height<=" + hs + "][height>=" + h_min + "][vcodec^=avc1]+bestaudio[ext=m4a]/"
               "bestvideo[height<=" + hs + "][width>=" + ws + "][ext=mp4]+bestaudio[ext=m4a]/"
               "bestvideo[height=" + hs + "][ext=mp4]+bestaudio[ext=m4a]/"
               "bestvideo[width=" + hs + "][ext=mp4]+bestaudio[ext=m4a]/"
               "bestvideo[height<=" + hs + "][height>=" + h_min + "][ext=mp4]+bestaudio[ext=m4a]/"
               "bestvideo[height<=" + hs + "][width>=" + ws + "]+bestaudio/"
               "bestvideo[height=" + hs + "]+bestaudio/"
               "bestvideo[width=" + hs + "]+bestaudio/"
               "bestvideo[height<=" + hs + "][height>=" + h_min + "]+bestaudio";
    }

    if (!filter_s.empty() && filter_s != "best" && filter_s != "auto") {
        if (std::all_of(filter_s.begin(), filter_s.end(), ::isdigit)) {
            // Specific itag format code
            return filter_s + "+bestaudio/" + filter_s;
        }
        std::smatch hm;
        static const std::regex kHRe(R"((\d{3,4})p?)");
        if (std::regex_search(filter_s, hm, kHRe)) {
            int th = std::stoi(hm[1].str());
            return BuildYtDlpFormatSelector(requested_filter, th, false, preferred_container);
        }
    }

    if (cont == "mkv" || cont == "webm") {
        return "bestvideo+bestaudio/best";
    }
    return "bestvideo[ext=mp4]+bestaudio[ext=m4a]/bestvideo+bestaudio/best[ext=mp4]/best";
}

Result<MediaQualityResult> Nm3u8dlReAdapter::VerifyMediaQuality(
    const std::filesystem::path& file_path,
    const MediaQualityExpectations& expectations,
    const std::filesystem::path& ffprobe_path) {

    std::error_code ec;
    if (!std::filesystem::exists(file_path, ec)) {
        return Result<MediaQualityResult>::Err("ERR_STREAM_FILE_MISSING", "Output media file does not exist: " + file_path.string());
    }
    uint64_t disk_sz = std::filesystem::file_size(file_path, ec);
    if (disk_sz == 0) {
        return Result<MediaQualityResult>::Err("ERR_STREAM_EMPTY_FILE", "Output media file is empty (0 bytes)");
    }
    if (expectations.expected_min_bytes > 0 && disk_sz < expectations.expected_min_bytes) {
        return Result<MediaQualityResult>::Err(
            "ERR_STREAM_TRUNCATED",
            "Output file size (" + std::to_string(disk_sz) + " bytes) is smaller than expected minimum (" +
            std::to_string(expectations.expected_min_bytes) + " bytes)");
    }

    std::string fmt_hint = expectations.expected_container;
    if (fmt_hint.empty()) {
        std::string ext = file_path.extension().string();
        if (!ext.empty() && ext.front() == '.') ext = ext.substr(1);
        fmt_hint = ToLowerStr(ext);
    }
    if (!VerifyContainerMagicBytes(file_path, fmt_hint)) {
        bool alt_magic = false;
        for (const auto& alt : {"mp4", "mkv", "webm", "m4a", "mov", "ts"}) {
            if (VerifyContainerMagicBytes(file_path, alt)) {
                alt_magic = true;
                break;
            }
        }
        if (!alt_magic) {
            return Result<MediaQualityResult>::Err("ERR_STREAM_CORRUPT_CONTAINER", "File container header is corrupt or unrecognized magic bytes");
        }
    }

    std::filesystem::path probe_bin = ffprobe_path;
    if (probe_bin.empty() || !std::filesystem::exists(probe_bin, ec)) {
        probe_bin = PathSanitizer::FindWorkerBinary("ffprobe", PathSanitizer::GetExecutableDir());
    }

    std::filesystem::path ffmpeg_bin;
    if (probe_bin.empty() || !std::filesystem::exists(probe_bin, ec)) {
        ffmpeg_bin = PathSanitizer::FindWorkerBinary("ffmpeg", PathSanitizer::GetExecutableDir());
    }

    MediaQualityResult qr;
    qr.probed_file_size = disk_sz;

    if (!probe_bin.empty() && std::filesystem::exists(probe_bin, ec)) {
        std::vector<std::string> args = {
            "-v", "error",
            "-show_entries", "stream=codec_type,codec_name,width,height,duration",
            "-show_entries", "format=duration,size",
            "-of", "json",
            file_path.string()
        };

        std::string json_output;
#ifdef _WIN32
        SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE hRead = nullptr, hWrite = nullptr;
        if (CreatePipe(&hRead, &hWrite, &sa, 0)) {
            SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdOutput = hWrite;
            si.hStdError = hWrite;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

            std::wostringstream wss;
            wss << L"\"" << probe_bin.wstring() << L"\"";
            for (const auto& a : args) {
                int sz = MultiByteToWideChar(CP_UTF8, 0, a.data(), static_cast<int>(a.size()), nullptr, 0);
                std::wstring w(sz, 0);
                MultiByteToWideChar(CP_UTF8, 0, a.data(), static_cast<int>(a.size()), w.data(), sz);
                wss << L" \"" << w << L"\"";
            }
            std::wstring wcmd = wss.str();
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                CloseHandle(hWrite);
                char buf[4096];
                DWORD got = 0;
                while (ReadFile(hRead, buf, sizeof(buf), &got, nullptr) && got > 0) {
                    json_output.append(buf, got);
                }
                WaitForSingleObject(pi.hProcess, 15000);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            } else {
                CloseHandle(hWrite);
            }
            CloseHandle(hRead);
        }
#else
        int pfd[2];
        if (pipe(pfd) == 0) {
            pid_t p = fork();
            if (p == 0) {
                ::close(pfd[0]);
                dup2(pfd[1], STDOUT_FILENO);
                dup2(pfd[1], STDERR_FILENO);
                std::string bin_s = probe_bin.string();
                std::vector<char*> argv_ptrs;
                argv_ptrs.push_back(const_cast<char*>(bin_s.c_str()));
                for (const auto& a : args) argv_ptrs.push_back(const_cast<char*>(a.c_str()));
                argv_ptrs.push_back(nullptr);
                execvp(bin_s.c_str(), argv_ptrs.data());
                _exit(127);
            }
            ::close(pfd[1]);
            char buf[4096];
            ssize_t n = 0;
            while ((n = ::read(pfd[0], buf, sizeof(buf))) > 0) {
                json_output.append(buf, static_cast<size_t>(n));
            }
            ::close(pfd[0]);
            int st = 0;
            waitpid(p, &st, 0);
        }
#endif
        if (json_output.size() >= 3 && static_cast<unsigned char>(json_output[0]) == 0xEF &&
            static_cast<unsigned char>(json_output[1]) == 0xBB && static_cast<unsigned char>(json_output[2]) == 0xBF) {
            json_output.erase(0, 3);
        }

        auto fbrace = json_output.find('{');
        auto lbrace = json_output.rfind('}');
        if (fbrace != std::string::npos && lbrace != std::string::npos && lbrace > fbrace) {
            std::string clean_json = json_output.substr(fbrace, lbrace - fbrace + 1);
            try {
                auto root = json::Parse(clean_json);
                if (root.is_object()) {
                    if (root["format"].is_object()) {
                        std::string dur_s = root["format"]["duration"].as_string();
                        if (!dur_s.empty()) {
                            try { qr.probed_duration_sec = std::stod(dur_s); } catch (...) {}
                        }
                    }
                    if (root["streams"].is_array()) {
                        for (const auto& s : root["streams"].as_array()) {
                            std::string ctype = ToLowerStr(s["codec_type"].as_string());
                            std::string cname = ToLowerStr(s["codec_name"].as_string());
                            if (ctype == "video" && qr.probed_video_codec.empty()) {
                                qr.probed_video_codec = cname;
                                qr.probed_width = static_cast<int>(s["width"].as_int64(0));
                                qr.probed_height = static_cast<int>(s["height"].as_int64(0));
                                if (qr.probed_duration_sec <= 0.0 && !s["duration"].as_string().empty()) {
                                    try { qr.probed_duration_sec = std::stod(s["duration"].as_string()); } catch (...) {}
                                }
                            } else if (ctype == "audio" && qr.probed_audio_codec.empty()) {
                                qr.probed_audio_codec = cname;
                                if (qr.probed_duration_sec <= 0.0 && !s["duration"].as_string().empty()) {
                                    try { qr.probed_duration_sec = std::stod(s["duration"].as_string()); } catch (...) {}
                                }
                            }
                        }
                    }
                }
            } catch (...) {}
        }
    } else if (!ffmpeg_bin.empty() && std::filesystem::exists(ffmpeg_bin, ec)) {
        // Fallback: Probe container metadata, dimensions, and codecs via ffmpeg -i <file>
        std::vector<std::string> args = {
            "-i", file_path.string()
        };

        std::string ffmpeg_output;
#ifdef _WIN32
        SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE hRead = nullptr, hWrite = nullptr;
        if (CreatePipe(&hRead, &hWrite, &sa, 0)) {
            SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdOutput = hWrite;
            si.hStdError = hWrite;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

            std::wostringstream wss;
            wss << L"\"" << ffmpeg_bin.wstring() << L"\"";
            for (const auto& a : args) {
                int sz = MultiByteToWideChar(CP_UTF8, 0, a.data(), static_cast<int>(a.size()), nullptr, 0);
                std::wstring w(sz, 0);
                MultiByteToWideChar(CP_UTF8, 0, a.data(), static_cast<int>(a.size()), w.data(), sz);
                wss << L" \"" << w << L"\"";
            }
            std::wstring wcmd = wss.str();
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                CloseHandle(hWrite);
                char buf[4096];
                DWORD got = 0;
                while (ReadFile(hRead, buf, sizeof(buf), &got, nullptr) && got > 0) {
                    ffmpeg_output.append(buf, got);
                }
                WaitForSingleObject(pi.hProcess, 15000);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            } else {
                CloseHandle(hWrite);
            }
            CloseHandle(hRead);
        }
#else
        int pfd[2];
        if (pipe(pfd) == 0) {
            pid_t p = fork();
            if (p == 0) {
                ::close(pfd[0]);
                dup2(pfd[1], STDOUT_FILENO);
                dup2(pfd[1], STDERR_FILENO);
                std::string bin_s = ffmpeg_bin.string();
                std::vector<char*> argv_ptrs;
                argv_ptrs.push_back(const_cast<char*>(bin_s.c_str()));
                for (const auto& a : args) argv_ptrs.push_back(const_cast<char*>(a.c_str()));
                argv_ptrs.push_back(nullptr);
                execvp(bin_s.c_str(), argv_ptrs.data());
                _exit(127);
            }
            ::close(pfd[1]);
            char buf[4096];
            ssize_t n = 0;
            while ((n = ::read(pfd[0], buf, sizeof(buf))) > 0) {
                ffmpeg_output.append(buf, static_cast<size_t>(n));
            }
            ::close(pfd[0]);
            int st = 0;
            waitpid(p, &st, 0);
        }
#endif

        // Parse ffmpeg -i textual stderr output:
        // Duration: 00:00:04.00, start: 0.000000, bitrate: 144 kb/s
        // Stream #0:0[0x1](und): Video: h264 (avc1 / 0x31637661), yuv420p(progressive), 640x360 [SAR 1:1 DAR 16:9], ...
        // Stream #0:1[0x2](und): Audio: aac (mp4a / 0x6134706D), 44100 Hz, mono, fltp, 63 kb/s (default)
        std::istringstream iss(ffmpeg_output);
        std::string line;
        static const std::regex kDurRe(R"(Duration:\s*(\d+):(\d+):(\d+(?:\.\d+)?))");
        static const std::regex kVideoRe(R"(Stream\s*#\d+:\d+.*?: Video:\s*([a-zA-Z0-9_-]+).*?,\s*(\d{2,5})x(\d{2,5}))");
        static const std::regex kAudioRe(R"(Stream\s*#\d+:\d+.*?: Audio:\s*([a-zA-Z0-9_-]+))");

        while (std::getline(iss, line)) {
            std::smatch m;
            if (qr.probed_duration_sec <= 0.0 && std::regex_search(line, m, kDurRe)) {
                try {
                    double hours = std::stod(m[1].str());
                    double mins = std::stod(m[2].str());
                    double secs = std::stod(m[3].str());
                    qr.probed_duration_sec = hours * 3600.0 + mins * 60.0 + secs;
                } catch (...) {}
            }
            if (qr.probed_video_codec.empty() && std::regex_search(line, m, kVideoRe)) {
                qr.probed_video_codec = ToLowerStr(m[1].str());
                try {
                    qr.probed_width = std::stoi(m[2].str());
                    qr.probed_height = std::stoi(m[3].str());
                } catch (...) {}
            }
            if (qr.probed_audio_codec.empty() && std::regex_search(line, m, kAudioRe)) {
                qr.probed_audio_codec = ToLowerStr(m[1].str());
            }
        }
    }

    // Evaluate probed metadata against expectations
    if (expectations.expect_video) {
        if (!qr.probed_video_codec.empty() && qr.probed_width > 0 && qr.probed_height > 0) {
            if (expectations.expected_height > 0) {
                bool matches = false;
                if (expectations.expected_height == 1080) {
                    matches = (qr.probed_height >= 1050) || (qr.probed_width >= 1800) ||
                             (qr.probed_width >= 1050 && qr.probed_height >= 1050);
                } else if (expectations.expected_height == 720) {
                    matches = (qr.probed_height >= 690) || (qr.probed_width >= 1200) ||
                             (qr.probed_width >= 690 && qr.probed_height >= 690);
                } else if (expectations.expected_height == 480) {
                    matches = (qr.probed_height >= 460) || (qr.probed_width >= 800) ||
                             (qr.probed_width >= 460 && qr.probed_height >= 460);
                } else if (expectations.expected_height == 1440) {
                    matches = (qr.probed_height >= 1400) || (qr.probed_width >= 2400) ||
                             (qr.probed_width >= 1400 && qr.probed_height >= 1400);
                } else if (expectations.expected_height == 2160) {
                    matches = (qr.probed_height >= 2100) || (qr.probed_width >= 3600) ||
                             (qr.probed_width >= 2100 && qr.probed_height >= 2100);
                } else {
                    int min_dim = std::min(qr.probed_width, qr.probed_height);
                    matches = (qr.probed_height >= expectations.expected_height - 30) ||
                             (min_dim >= expectations.expected_height - 30);
                }
                if (!matches) {
                    std::string err_msg = "Downloaded video quality mismatch: selected " +
                        std::to_string(expectations.expected_height) + "p, but output file is " +
                        std::to_string(qr.probed_width) + "x" + std::to_string(qr.probed_height) +
                        " (" + std::to_string(qr.probed_height) + "p)";
                    return Result<MediaQualityResult>::Err("ERR_STREAM_QUALITY_MISMATCH", err_msg);
                }
            }
        }
    }

    if (expectations.expect_audio) {
        if (!probe_bin.empty() && qr.probed_audio_codec.empty() && !qr.probed_video_codec.empty()) {
            return Result<MediaQualityResult>::Err("ERR_STREAM_NO_AUDIO", "Verified output media file is silent (missing audio stream)");
        }
    }

    if (expectations.expected_duration_sec > 5.0 && qr.probed_duration_sec > 0.0) {
        if (qr.probed_duration_sec < expectations.expected_duration_sec * 0.85) {
            std::string err_msg = "Downloaded media is severely truncated: duration is " +
                std::to_string(static_cast<int>(qr.probed_duration_sec)) + "s (expected " +
                std::to_string(static_cast<int>(expectations.expected_duration_sec)) + "s)";
            return Result<MediaQualityResult>::Err("ERR_STREAM_TRUNCATED_DURATION", err_msg);
        }
    }

    qr.ok = true;
    return Result<MediaQualityResult>::Ok(std::move(qr));
}

namespace {

int RunExternalFfmpegMux(const std::filesystem::path& ffmpeg_bin, const std::vector<std::string>& args) {
#ifdef _WIN32
    auto to_wide = [](std::string_view sv) {
        if (sv.empty()) return std::wstring();
        int sz = MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), nullptr, 0);
        std::wstring w(sz, 0);
        MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), w.data(), sz);
        return w;
    };
    std::wostringstream cmd;
    cmd << L"\"" << ffmpeg_bin.wstring() << L"\"";
    for (const auto& a : args) {
        cmd << L" \"" << to_wide(a) << L"\"";
    }
    std::wstring wcmd = cmd.str();
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(exit_code);
#else
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) ::close(devnull);
        }
        std::string bin_s = ffmpeg_bin.string();
        std::vector<char*> argv_ptrs;
        argv_ptrs.push_back(const_cast<char*>(bin_s.c_str()));
        for (const auto& a : args) {
            argv_ptrs.push_back(const_cast<char*>(a.c_str()));
        }
        argv_ptrs.push_back(nullptr);
        execvp(bin_s.c_str(), argv_ptrs.data());
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

} // namespace

Nm3u8dlReAdapter::Nm3u8dlReAdapter(StreamAdapterConfig cfg) : m_cfg(std::move(cfg)) {}

Result<std::string> Nm3u8dlReAdapter::VerifyWorkerBinary() const {
    std::error_code ec;
    auto test_bin = [&](const std::filesystem::path& bin_path) -> Result<std::string> {
        if (bin_path.empty() || !std::filesystem::exists(bin_path, ec)) {
            return Result<std::string>::Err(
                "ERR_DEPENDENCY_MISSING",
                "Stream worker binary not found: " + bin_path.string());
        }
#ifndef _WIN32
        int pipefd[2];
        if (pipe(pipefd) != 0) {
            return Result<std::string>::Err("ERR_OS_PIPE", "pipe() failed during worker check");
        }
        pid_t pid = fork();
        if (pid == 0) {
            ::close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            if (pipefd[1] > 2) ::close(pipefd[1]);
            execl(bin_path.c_str(), bin_path.c_str(), "--version", nullptr);
            _exit(127);
        }
        ::close(pipefd[1]);
        std::string out;
        char buf[512];
        ssize_t n = 0;
        while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0) {
            out.append(buf, static_cast<size_t>(n));
        }
        ::close(pipefd[0]);
        int status = 0;
        waitpid(pid, &status, 0);
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (code != 0 || out.empty()) {
            return Result<std::string>::Err(
                "ERR_WORKER_INCOMPATIBLE",
                "Stream worker binary failed --version compatibility check (exit " + std::to_string(code) + ")");
        }
        return Result<std::string>::Ok(out);
#else
        SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE hReadPipe = nullptr;
        HANDLE hWritePipe = nullptr;
        if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
            return Result<std::string>::Err("ERR_OS_PIPE", "CreatePipe failed during worker check");
        }
        SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = hWritePipe;
        si.hStdError = hWritePipe;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

        std::wstring cmd = L"\"" + bin_path.wstring() + L"\" --version";
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            CloseHandle(hReadPipe);
            CloseHandle(hWritePipe);
            return Result<std::string>::Err("ERR_WORKER_EXEC", "Failed to launch stream worker --version");
        }
        CloseHandle(hWritePipe);

        std::string out;
        char buf[512];
        DWORD got = 0;
        while (ReadFile(hReadPipe, buf, sizeof(buf), &got, nullptr) && got > 0) {
            out.append(buf, got);
        }
        CloseHandle(hReadPipe);

        WaitForSingleObject(pi.hProcess, 5000);
        DWORD exit_code = 1;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        if (exit_code != 0 || out.empty()) {
            return Result<std::string>::Err(
                "ERR_WORKER_INCOMPATIBLE",
                "Stream worker binary failed --version compatibility check (exit " + std::to_string(exit_code) + ")");
        }
        return Result<std::string>::Ok(out);
#endif
    };

    auto res = test_bin(m_cfg.nm3u8dl_binary_path);
    if (res.ok()) return res;

    std::filesystem::path fb = m_cfg.fallback_worker_path;
    if (!fb.empty() && std::filesystem::exists(fb, ec)) {
        auto fb_res = test_bin(fb);
        if (fb_res.ok()) {
            const_cast<Nm3u8dlReAdapter*>(this)->m_cfg.nm3u8dl_binary_path = fb;
            return fb_res;
        }
    }
    return res;
}

Result<void> Nm3u8dlReAdapter::ValidateManifestSafety(std::string_view url, std::string_view body) const {
    if (!url.empty()) {
        if (url.front() == '-' || url.front() == '@') {
            return Result<void>::Err("ERR_SEC_FLAG_INJECTION", "Manifest URL cannot begin with '-' or '@'");
        }
        for (char c : url) {
            if (static_cast<unsigned char>(c) <= 32 || c == '"' || c == '\\' || c == '\'' || c == '`') {
                return Result<void>::Err("ERR_SEC_FLAG_INJECTION", "Invalid or dangerous characters in manifest URL");
            }
        }
        auto pu = brisk::HttpClient::ParseUrl(url);
        if (!pu.ok()) {
            return Result<void>::Err(pu.error->code, pu.error->message);
        }
        if (pu.value->is_loopback_or_private && !m_cfg.allow_loopback_manifests) {
            return Result<void>::Err("ERR_SEC_SSRF_BLOCKED", "Loopback/private manifest URLs are blocked");
        }
    }

    if (body.size() > 10 * 1024 * 1024) {
        return Result<void>::Err("ERR_STREAM_MANIFEST_TOO_LARGE", "Manifest exceeds 10 MB maximum size limit");
    }

    // Defect B Fix: Reject JSON descriptors or non-manifest bodies passed as cached_manifest_body
    std::string trimmed = TrimView(body);
    if (!trimmed.empty()) {
        if (trimmed.front() == '{' || trimmed.front() == '[') {
            return Result<void>::Err(
                "ERR_STREAM_INVALID_MANIFEST",
                "Nm3u8dlReAdapter expects actual HLS (#EXTM3U) or DASH (<MPD) manifest text, not a JSON descriptor");
        }
        bool is_hls = (trimmed.rfind("#EXTM3U", 0) == 0 ||
                       trimmed.find("#EXT-X-STREAM-INF") != std::string::npos ||
                       trimmed.find("#EXT-X-TARGETDURATION") != std::string::npos);
        bool is_dash = (trimmed.find("<MPD") != std::string::npos);
        bool is_mss = (trimmed.find("<SmoothStreamingMedia") != std::string::npos);
        if (!is_hls && !is_dash && !is_mss) {
            return Result<void>::Err(
                "ERR_STREAM_INVALID_MANIFEST",
                "Cached manifest body is not a valid HLS (#EXTM3U) or MPEG-DASH (<MPD) manifest");
        }
    } else if (!url.empty() && PathSanitizer::IsWebpageWatchUrl(url)) {
        return Result<void>::Err(
            "ERR_STREAM_INVALID_MANIFEST",
            "Webpage watch URL cannot be passed directly to N_m3u8DL-RE as an HLS/DASH manifest");
    }

    std::string lower(body);
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // Line-by-line structural check for URI lines and key tags
    std::istringstream stream(lower);
    std::string line;
    while (std::getline(stream, line)) {
        line = TrimView(line);
        if (line.empty()) continue;
        if (line.find("file://") != std::string::npos ||
            line.find("smb://") != std::string::npos ||
            line.find("\\\\") != std::string::npos) {
            return Result<void>::Err("ERR_SEC_HOSTILE_MANIFEST", "Manifest contains forbidden local file/UNC URI");
        }
        if (!m_cfg.allow_loopback_manifests &&
            (line.find("http://127.0.0.1") != std::string::npos ||
             line.find("http://localhost") != std::string::npos ||
             line.find("http://169.254.") != std::string::npos ||
             line.find("http://10.") != std::string::npos ||
             line.find("http://192.168.") != std::string::npos)) {
            return Result<void>::Err("ERR_SEC_HOSTILE_MANIFEST", "Manifest contains loopback/link-local SSRF segment URI");
        }
    }
    return Result<void>::Ok();
}

Result<std::filesystem::path> Nm3u8dlReAdapter::WriteRestrictedResponseFile(
    const std::filesystem::path& task_tmp_dir,
    const std::string& filename,
    const std::vector<std::string>& lines) {

    std::error_code ec;
    std::filesystem::create_directories(task_tmp_dir, ec);
    if (ec) {
        return Result<std::filesystem::path>::Err("ERR_STORAGE_TMP", "Cannot create task temp dir");
    }
    std::filesystem::path rsp_path = task_tmp_dir / filename;

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    PSECURITY_DESCRIPTOR pSD = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;SY)(A;;GA;;;OW)",
            SDDL_REVISION_1,
            &pSD,
            nullptr)) {
        sa.lpSecurityDescriptor = pSD;
    } else {
        sa.lpSecurityDescriptor = nullptr;
    }

    HANDLE hFile = CreateFileW(
        rsp_path.wstring().c_str(),
        GENERIC_WRITE,
        0, // Exclusive access (no sharing while writing credentials)
        &sa,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY,
        nullptr);

    if (pSD) {
        LocalFree(pSD);
    }

    if (hFile == INVALID_HANDLE_VALUE) {
        return Result<std::filesystem::path>::Err("ERR_STORAGE_RSP", "Cannot create restricted response file");
    }
    std::ostringstream content;
    for (const auto& line : lines) content << line << "\n";
    std::string data = content.str();
    DWORD written = 0;
    BOOL ok = WriteFile(hFile, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    FlushFileBuffers(hFile);
    CloseHandle(hFile);
    if (!ok || written != static_cast<DWORD>(data.size())) {
        return Result<std::filesystem::path>::Err("ERR_STORAGE_RSP", "Incomplete write to response file");
    }
    return Result<std::filesystem::path>::Ok(rsp_path);
#else
    ::chmod(task_tmp_dir.c_str(), 0700);
    int fd = ::open(rsp_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0) {
        return Result<std::filesystem::path>::Err("ERR_STORAGE_RSP", "Cannot create restricted response file");
    }

    std::ostringstream content;
    for (const auto& line : lines) {
        content << line << "\n";
    }
    std::string data = content.str();
    ssize_t w = ::write(fd, data.data(), data.size());
    ::fsync(fd);
    ::close(fd);
    if (w != static_cast<ssize_t>(data.size())) {
        return Result<std::filesystem::path>::Err("ERR_STORAGE_RSP", "Incomplete write to response file");
    }
    return Result<std::filesystem::path>::Ok(rsp_path);
#endif
}

Result<std::filesystem::path> Nm3u8dlReAdapter::WriteNetscapeCookieFile(
    const std::filesystem::path& task_tmp_dir,
    const std::string& filename,
    const std::string& cookie_str,
    const std::string& domain_hint) {
    if (cookie_str.empty()) {
        return Result<std::filesystem::path>::Err("ERR_COOKIE_EMPTY", "Cookie string is empty");
    }

    std::string primary_domain = ".youtube.com";
    if (!domain_hint.empty()) {
        std::string d = domain_hint;
        auto ppos = d.find("://");
        if (ppos != std::string::npos) d = d.substr(ppos + 3);
        auto spos = d.find_first_of("/:?#");
        if (spos != std::string::npos) d = d.substr(0, spos);
        if (d.find("youtube.com") != std::string::npos || d.find("youtu.be") != std::string::npos) {
            primary_domain = ".youtube.com";
        } else if (!d.empty()) {
            primary_domain = (d.front() == '.') ? d : ("." + d);
        }
    }

    std::vector<std::string> lines;
    lines.push_back("# Netscape HTTP Cookie File");
    lines.push_back("# https://curl.se/docs/http-cookies.html");
    lines.push_back("# This file is generated by OpenIDM. Do not edit.");

    std::string_view sv = cookie_str;
    while (!sv.empty()) {
        auto semi = sv.find(';');
        std::string_view pair = (semi != std::string_view::npos) ? sv.substr(0, semi) : sv;
        sv = (semi != std::string_view::npos) ? sv.substr(semi + 1) : std::string_view();

        while (!pair.empty() && (pair.front() == ' ' || pair.front() == '\t' || pair.front() == '\r' || pair.front() == '\n')) {
            pair.remove_prefix(1);
        }
        while (!pair.empty() && (pair.back() == ' ' || pair.back() == '\t' || pair.back() == '\r' || pair.back() == '\n')) {
            pair.remove_suffix(1);
        }
        if (pair.empty()) continue;

        auto eq = pair.find('=');
        if (eq == std::string_view::npos || eq == 0) continue;

        std::string key(pair.substr(0, eq));
        std::string val(pair.substr(eq + 1));

        std::string entry = primary_domain + "\tTRUE\t/\tTRUE\t2147483647\t" + key + "\t" + val;
        lines.push_back(entry);

        if (primary_domain == ".youtube.com") {
            std::string gentry = ".google.com\tTRUE\t/\tTRUE\t2147483647\t" + key + "\t" + val;
            lines.push_back(gentry);
        }
    }

    return WriteRestrictedResponseFile(task_tmp_dir, filename, lines);
}

Result<StreamProbeResult> Nm3u8dlReAdapter::ParseMetaJson(const std::filesystem::path& meta_json_path) {
    std::ifstream ifs(meta_json_path);
    if (!ifs) {
        return Result<StreamProbeResult>::Err("ERR_STREAM_INVALID_MANIFEST", "meta.json was not generated by worker");
    }
    std::stringstream buf;
    buf << ifs.rdbuf();
    std::string raw = buf.str();
    if (raw.empty()) {
        return Result<StreamProbeResult>::Err("ERR_STREAM_INVALID_MANIFEST", "Empty meta.json output");
    }

    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
        static_cast<unsigned char>(raw[1]) == 0xBB && static_cast<unsigned char>(raw[2]) == 0xBF) {
        raw = raw.substr(3);
    }

    json::Value root;
    try {
        root = json::Parse(raw);
    } catch (const std::exception& e) {
        return Result<StreamProbeResult>::Err("ERR_STREAM_INVALID_MANIFEST", std::string("Invalid meta.json: ") + e.what());
    }

    StreamProbeResult out;
    if (root.is_array()) {
        const auto& arr = root.as_array();
        for (size_t idx = 0; idx < arr.size(); ++idx) {
            const auto& item = arr[idx];
            StreamTrack tr;
            std::string id = item["Id"].as_string();
            if (id.empty()) id = item["id"].as_string();
            if (id.empty()) id = std::to_string(idx);
            tr.id = id;

            tr.resolution = item["Resolution"].as_string();
            if (tr.resolution.empty()) tr.resolution = item["resolution"].as_string();

            tr.codecs = item["Codecs"].as_string();
            if (tr.codecs.empty()) tr.codecs = item["codecs"].as_string();

            tr.group_id = item["GroupId"].as_string();
            if (tr.group_id.empty()) tr.group_id = item["groupId"].as_string();

            tr.language = item["Language"].as_string();
            if (tr.language.empty()) tr.language = item["language"].as_string("und");

            tr.name = item["Name"].as_string();
            if (tr.name.empty()) tr.name = item["name"].as_string();

            tr.bandwidth = static_cast<uint64_t>(item["Bandwidth"].as_int64(item["bandwidth"].as_int64(0)));
            tr.segments_count = static_cast<uint32_t>(item["SegmentsCount"].as_int64(item["segmentsCount"].as_int64(0)));

            std::string mtype = item["MediaType"].as_string(item["mediaType"].as_string(""));
            if (mtype.empty()) {
                if (!tr.resolution.empty()) {
                    mtype = "VIDEO";
                } else if (!tr.group_id.empty() && (tr.group_id.find("sub") != std::string::npos || tr.group_id.find("Sub") != std::string::npos)) {
                    mtype = "SUBTITLES";
                } else if (!tr.group_id.empty() && (tr.group_id.find("aud") != std::string::npos || tr.group_id.find("Aud") != std::string::npos)) {
                    mtype = "AUDIO";
                } else if (tr.codecs.find("mp4a") != std::string::npos || tr.codecs.find("aac") != std::string::npos || tr.codecs.find("opus") != std::string::npos) {
                    mtype = "AUDIO";
                } else {
                    mtype = "VIDEO";
                }
            }
            tr.media_type = mtype;

            std::string enc = item["EncryptionMethod"].as_string(item["encryptionMethod"].as_string("NONE"));
            tr.encryption_method = enc;
            tr.encrypted = (enc != "NONE" && !enc.empty());

            if (tr.encryption_method == "SAMPLE_AES_CTR" || tr.encryption_method == "CENC" ||
                tr.encryption_method == "WIDEVINE" || tr.encryption_method == "PLAYREADY" ||
                tr.encryption_method == "FAIRPLAY") {
                out.drm_protected = true;
                if (out.drm_system.empty()) out.drm_system = tr.encryption_method;
            }

            out.tracks.push_back(std::move(tr));
        }

        if (out.manifest_url.empty() && !arr.empty()) {
            std::string orig = arr[0]["OriginalUrl"].as_string();
            if (orig.empty()) orig = arr[0]["Url"].as_string();
            out.manifest_url = orig;
        }
    } else {
        out.manifest_url = root["url"].as_string();
        out.is_live = root["isLive"].as_bool(false);
        out.drm_protected = root["drmProtected"].as_bool(false);
        out.drm_system = root["drmSystem"].as_string("");

        const auto& tracks_arr = root["tracks"].as_array();
        for (const auto& item : tracks_arr) {
            StreamTrack tr;
            tr.id = item["id"].as_string();
            tr.media_type = item["mediaType"].as_string("VIDEO");
            tr.group_id = item["groupId"].as_string();
            tr.language = item["language"].as_string("und");
            tr.name = item["name"].as_string();
            tr.codecs = item["codecs"].as_string();
            tr.resolution = item["resolution"].as_string();
            tr.bandwidth = static_cast<uint64_t>(item["bandwidth"].as_int64(0));
            tr.frame_rate = item["frameRate"].as_double(0.0);
            tr.segments_count = static_cast<uint32_t>(item["segmentsCount"].as_int64(0));
            tr.duration_sec = item["durationSec"].as_double(0.0);
            tr.encrypted = item["encrypted"].as_bool(false);
            tr.encryption_method = item["encryptionMethod"].as_string("NONE");

            if (tr.encryption_method == "SAMPLE_AES_CTR" || tr.encryption_method == "CENC" ||
                tr.encryption_method == "WIDEVINE" || tr.encryption_method == "PLAYREADY" ||
                tr.encryption_method == "FAIRPLAY") {
                out.drm_protected = true;
                if (out.drm_system.empty()) out.drm_system = tr.encryption_method;
            }
            out.tracks.push_back(std::move(tr));
        }
    }

    if (out.drm_protected) {
        return Result<StreamProbeResult>::Err(
            "ERR_STREAM_DRM_PROTECTED",
            "Stream uses unsupported DRM protection (" + out.drm_system + "). Flux does not circumvent DRM.");
    }
    if (out.tracks.empty()) {
        return Result<StreamProbeResult>::Err("ERR_STREAM_INVALID_MANIFEST", "No downloadable tracks found in manifest");
    }
    return Result<StreamProbeResult>::Ok(std::move(out));
}

Result<StreamProbeResult> Nm3u8dlReAdapter::ProbeManifest(
    const std::string& manifest_url,
    const std::map<std::string, std::string>& headers,
    const std::map<std::string, std::string>& auth_headers,
    const std::string& cached_manifest_body) {

    std::error_code ec;
    if (!std::filesystem::exists(m_cfg.nm3u8dl_binary_path, ec)) {
        if (!m_cfg.fallback_worker_path.empty() && std::filesystem::exists(m_cfg.fallback_worker_path, ec)) {
            m_cfg.nm3u8dl_binary_path = m_cfg.fallback_worker_path;
        } else if (m_cfg.nm3u8dl_binary_path.empty()) {
            auto discovered = PathSanitizer::FindWorkerBinary("N_m3u8DL-RE", PathSanitizer::GetExecutableDir());
            if (std::filesystem::exists(discovered, ec)) {
                m_cfg.nm3u8dl_binary_path = discovered;
            } else {
                return Result<StreamProbeResult>::Err(
                    "ERR_DEPENDENCY_MISSING", "Stream worker binary not found", false, true);
            }
        } else {
            return Result<StreamProbeResult>::Err(
                "ERR_DEPENDENCY_MISSING", "Stream worker binary not found: " + m_cfg.nm3u8dl_binary_path.string(), false, true);
        }
    }

    auto safety = ValidateManifestSafety(manifest_url, cached_manifest_body);
    if (!safety.ok()) {
        return Result<StreamProbeResult>::Err(safety.error->code, safety.error->message);
    }

    std::string probe_id = "probe_" + GenerateUuidV4();
    std::filesystem::path probe_dir = m_cfg.temp_root_dir / probe_id;
    std::filesystem::create_directories(probe_dir, ec);

    std::string input_target = manifest_url;
    if (!cached_manifest_body.empty()) {
        bool is_dash = (cached_manifest_body.find("<MPD") != std::string::npos);
        std::filesystem::path local_manifest = probe_dir / (is_dash ? "cached_manifest.mpd" : "cached_manifest.m3u8");
        std::ofstream mofs(local_manifest, std::ios::binary);
        mofs << cached_manifest_body;
        mofs.close();
        input_target = local_manifest.string();
    }

    std::vector<std::string> rsp_lines;
    rsp_lines.push_back("\"" + EscapeRspArg(input_target) + "\"");
    if (!cached_manifest_body.empty() && !manifest_url.empty()) {
        rsp_lines.push_back("--base-url \"" + EscapeRspArg(manifest_url) + "\"");
    }
    rsp_lines.push_back("--skip-download");
    rsp_lines.push_back("--write-meta-json");
    rsp_lines.push_back("--auto-select");
    rsp_lines.push_back("--tmp-dir \"" + EscapeRspArg(probe_dir.string()) + "\"");
    rsp_lines.push_back("--save-dir \"" + EscapeRspArg(probe_dir.string()) + "\"");
    rsp_lines.push_back("--no-log");
    rsp_lines.push_back("--http-request-timeout 30");

    for (const auto& [k, v] : headers) {
        rsp_lines.push_back("-H \"" + EscapeRspArg(k + ": " + v) + "\"");
    }
    for (const auto& [k, v] : auth_headers) {
        rsp_lines.push_back("-H \"" + EscapeRspArg(k + ": " + v) + "\"");
    }

    auto rsp_res = WriteRestrictedResponseFile(probe_dir, "probe.rsp", rsp_lines);
    if (!rsp_res.ok()) {
        std::filesystem::remove_all(probe_dir, ec);
        return Result<StreamProbeResult>::Err(rsp_res.error->code, rsp_res.error->message);
    }

#ifdef _WIN32
    std::wstring cmd = L"\"" + m_cfg.nm3u8dl_binary_path.wstring() + L"\" "
                       L"--no-ansi-color --disable-update-check "
                       L"--ui-language en-US "
                       L"\"@" + rsp_res.value->wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
#else
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) ::close(devnull);
        }
        std::string rsp_arg = "@" + rsp_res.value->string();
        execl(m_cfg.nm3u8dl_binary_path.c_str(),
              m_cfg.nm3u8dl_binary_path.c_str(),
              "--no-ansi-color",
              "--disable-update-check",
              "--ui-language", "en-US",
              rsp_arg.c_str(),
              nullptr);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
#endif

    {
        std::ofstream wipe(*rsp_res.value, std::ios::binary | std::ios::trunc);
        wipe << std::string(256, '\0');
    }
    std::filesystem::remove(*rsp_res.value, ec);

    std::filesystem::path meta_path = probe_dir / "meta.json";
    if (!std::filesystem::exists(meta_path, ec)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(probe_dir, ec)) {
            if (entry.is_regular_file() && entry.path().filename() == "meta.json") {
                meta_path = entry.path();
                break;
            }
        }
    }
    auto parsed = ParseMetaJson(meta_path);
    std::filesystem::remove_all(probe_dir, ec);
    return parsed;
}

Result<std::filesystem::path> Nm3u8dlReAdapter::ExecuteStreamDownload(
    TaskModel& task,
    std::atomic<bool>& pause_requested,
    std::atomic<bool>& cancel_requested,
    uint64_t speed_limit_bps,
    ProgressCallback on_progress) {

    std::error_code ec;
    bool is_yt = (task.url.find("googlevideo.com") != std::string::npos ||
                  task.url.find("youtube.com/watch") != std::string::npos ||
                  task.url.find("youtu.be/") != std::string::npos ||
                  task.url.find("youtube.com/shorts/") != std::string::npos ||
                  task.stream_options.cached_manifest_body.find("\"fluxType\":\"YOUTUBE_STREAM\"") != std::string::npos ||
                  task.stream_options.cached_manifest_body.find("\"fluxType\": \"YOUTUBE_STREAM\"") != std::string::npos);
    if (is_yt) {
        return ExecuteYouTubeExtraction(task, pause_requested, cancel_requested, speed_limit_bps, on_progress);
    }

    if (!std::filesystem::exists(m_cfg.nm3u8dl_binary_path, ec)) {
        if (!m_cfg.fallback_worker_path.empty() && std::filesystem::exists(m_cfg.fallback_worker_path, ec)) {
            m_cfg.nm3u8dl_binary_path = m_cfg.fallback_worker_path;
        } else if (m_cfg.nm3u8dl_binary_path.empty()) {
            auto discovered = PathSanitizer::FindWorkerBinary("N_m3u8DL-RE", PathSanitizer::GetExecutableDir());
            if (std::filesystem::exists(discovered, ec)) {
                m_cfg.nm3u8dl_binary_path = discovered;
            } else {
                task.state = TaskState::kFailed;
                task.error_code = "ERR_DEPENDENCY_MISSING";
                task.error_message = "N_m3u8DL-RE binary missing";
                task.status_detail = task.error_message;
                return Result<std::filesystem::path>::Err(task.error_code, task.error_message, false, true);
            }
        } else {
            task.state = TaskState::kFailed;
            task.error_code = "ERR_DEPENDENCY_MISSING";
            task.error_message = "N_m3u8DL-RE binary missing: " + m_cfg.nm3u8dl_binary_path.string();
            task.status_detail = task.error_message;
            return Result<std::filesystem::path>::Err(task.error_code, task.error_message, false, true);
        }
    }
    if (!std::filesystem::exists(m_cfg.ffmpeg_binary_path, ec)) {
        if (m_cfg.ffmpeg_binary_path.empty()) {
            auto discovered_ff = PathSanitizer::FindWorkerBinary("ffmpeg", PathSanitizer::GetExecutableDir());
            if (std::filesystem::exists(discovered_ff, ec)) {
                m_cfg.ffmpeg_binary_path = discovered_ff;
            } else {
                task.state = TaskState::kFailed;
                task.error_code = "ERR_DEPENDENCY_MISSING";
                task.error_message = "FFmpeg binary missing";
                task.status_detail = task.error_message;
                return Result<std::filesystem::path>::Err(task.error_code, task.error_message, false, true);
            }
        } else {
            task.state = TaskState::kFailed;
            task.error_code = "ERR_DEPENDENCY_MISSING";
            task.error_message = "FFmpeg binary missing: " + m_cfg.ffmpeg_binary_path.string();
            task.status_detail = task.error_message;
            return Result<std::filesystem::path>::Err(task.error_code, task.error_message, false, true);
        }
    }

    auto safety = ValidateManifestSafety(task.url, task.stream_options.cached_manifest_body);
    if (!safety.ok()) {
        task.state = TaskState::kFailed;
        task.error_code = safety.error->code;
        task.error_message = safety.error->message;
        task.status_detail = task.error_message;
        return Result<std::filesystem::path>::Err(task.error_code, task.error_message);
    }

    auto space_res = PathSanitizer::CheckAvailableDiskSpace(task.save_dir, 16ULL * 1048576ULL);
    if (!space_res.ok()) {
        task.state = TaskState::kFailed;
        task.error_code = space_res.error->code;
        task.error_message = space_res.error->message;
        task.status_detail = task.error_message;
        return Result<std::filesystem::path>::Err(task.error_code, task.error_message, false, true);
    }

    std::filesystem::path task_tmp_dir = m_cfg.temp_root_dir / task.task_id;
    std::filesystem::create_directories(task_tmp_dir, ec);
    std::filesystem::create_directories(task.save_dir, ec);
    task.part_path = task_tmp_dir.string();

    if (task.headers.find("User-Agent") == task.headers.end() && task.headers.find("user-agent") == task.headers.end()) {
        task.headers["User-Agent"] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";
    }
    if (task.headers.find("Referer") == task.headers.end() && task.headers.find("referer") == task.headers.end() && !task.page_url.empty()) {
        task.headers["Referer"] = task.page_url;
    }

    std::string input_target = task.url;
    if (task.stream_options.cached_manifest_body.empty() && !task.url.empty() &&
        (task.url.rfind("http://", 0) == 0 || task.url.rfind("https://", 0) == 0)) {
        auto probe = brisk::HttpClient::ProbeResource(task.url, task.headers, task.auth_headers, {}, m_cfg.allow_loopback_manifests);
        if (probe.ok()) {
            if (probe.value->status_code == 403 || probe.value->status_code == 401) {
                OPENIDM_LOG_WARN("StreamAdapter", "Probe for " + task.url + " returned HTTP " + std::to_string(probe.value->status_code) + "; delegating to stream worker with configured headers");
            } else if (!probe.value->final_url.empty() && probe.value->final_url != task.url) {
                OPENIDM_LOG_INFO("StreamAdapter", "Manifest redirected: " + task.url + " -> " + probe.value->final_url);
                task.url = probe.value->final_url;
                input_target = task.url;
            }
        }
    }

    if (!task.stream_options.cached_manifest_body.empty()) {
        bool is_dash = (task.task_type == TaskType::kStreamDash ||
                        task.stream_options.cached_manifest_body.find("<MPD") != std::string::npos);
        std::filesystem::path local_manifest = task_tmp_dir / (is_dash ? "cached_manifest.mpd" : "cached_manifest.m3u8");
        std::ofstream mofs(local_manifest, std::ios::binary);
        mofs << task.stream_options.cached_manifest_body;
        mofs.close();
        input_target = local_manifest.string();
    }

    std::string req_fmt = ToLowerStr(task.stream_options.output_format);
    std::string fmt = "mp4";
    if (req_fmt == "mkv" || req_fmt == "webm" || req_fmt == "m4a" || req_fmt == "mp3" || req_fmt == "ts") {
        fmt = req_fmt;
    }
    std::string base_stem = PathSanitizer::SanitizeFileName(task.file_name.empty() ? "stream_video" : task.file_name);
    auto dot = base_stem.find_last_of('.');
    if (dot != std::string::npos) {
        base_stem = base_stem.substr(0, dot);
    }
    std::string temp_save_name = base_stem + ".flux_mux";

    std::vector<std::string> rsp_lines;
    rsp_lines.push_back("\"" + EscapeRspArg(input_target) + "\"");
    if (!task.stream_options.cached_manifest_body.empty() && !task.url.empty()) {
        rsp_lines.push_back("--base-url \"" + EscapeRspArg(task.url) + "\"");
    }
    rsp_lines.push_back("--tmp-dir \"" + EscapeRspArg(task_tmp_dir.string()) + "\"");
    rsp_lines.push_back("--save-dir \"" + EscapeRspArg(task.save_dir) + "\"");
    rsp_lines.push_back("--save-name \"" + EscapeRspArg(temp_save_name) + "\"");
    rsp_lines.push_back("--thread-count " + std::to_string(std::clamp(task.stream_options.thread_count, 1u, 32u)));
    rsp_lines.push_back("--download-retry-count 3");
    rsp_lines.push_back("--http-request-timeout 15");
    if (task.stream_options.concurrent_download) {
        rsp_lines.push_back("-mt");
    }
    rsp_lines.push_back("--check-segments-count true");
    rsp_lines.push_back("--del-after-done true");
    rsp_lines.push_back("--auto-subtitle-fix true");
    rsp_lines.push_back("--sub-format SRT");
    rsp_lines.push_back("--no-log");
    rsp_lines.push_back("--ffmpeg-binary-path \"" + EscapeRspArg(m_cfg.ffmpeg_binary_path.string()) + "\"");

    bool is_audio_only = (fmt == "m4a" || fmt == "mp3" || task.stream_options.video_filter == "none");
    bool is_video_only = (task.stream_options.audio_filter == "none");

    if (is_audio_only) {
        rsp_lines.push_back("--drop-video");
        rsp_lines.push_back("-sa " + EscapeRspArg(task.stream_options.audio_filter.empty() || task.stream_options.audio_filter == "all" ? "best" : task.stream_options.audio_filter));
    } else if (is_video_only) {
        rsp_lines.push_back("--drop-audio");
        rsp_lines.push_back("-sv " + EscapeRspArg(task.stream_options.video_filter.empty() ? "best" : task.stream_options.video_filter));
    } else {
        rsp_lines.push_back("--auto-select");
        rsp_lines.push_back("-sv " + EscapeRspArg(task.stream_options.video_filter.empty() ? "best" : task.stream_options.video_filter));
        rsp_lines.push_back("-sa " + EscapeRspArg(task.stream_options.audio_filter.empty() ? "best" : task.stream_options.audio_filter));
    }
    rsp_lines.push_back("-ss " + EscapeRspArg(task.stream_options.subtitle_filter.empty() ? "all" : task.stream_options.subtitle_filter));
    rsp_lines.push_back("-M format=" + fmt + ":muxer=ffmpeg:bin_path=\"" + EscapeMuxPath(m_cfg.ffmpeg_binary_path) + "\":keep=false");

    if (!task.stream_options.custom_hls_key_hex.empty()) {
        rsp_lines.push_back("--custom-hls-method AES_128");
        rsp_lines.push_back("--custom-hls-key \"" + EscapeRspArg(task.stream_options.custom_hls_key_hex) + "\"");
    }
    if (speed_limit_bps > 0) {
        uint64_t kbps = std::max<uint64_t>(1, (speed_limit_bps * 8) / 1000);
        rsp_lines.push_back("-R " + std::to_string(kbps) + "K");
    }
    for (const auto& [k, v] : task.headers) {
        if (k.find('\r') != std::string::npos || k.find('\n') != std::string::npos ||
            v.find('\r') != std::string::npos || v.find('\n') != std::string::npos) {
            continue;
        }
        rsp_lines.push_back("-H \"" + EscapeRspArg(k + ": " + v) + "\"");
    }
    for (const auto& [k, v] : task.auth_headers) {
        if (k.find('\r') != std::string::npos || k.find('\n') != std::string::npos ||
            v.find('\r') != std::string::npos || v.find('\n') != std::string::npos) {
            continue;
        }
        rsp_lines.push_back("-H \"" + EscapeRspArg(k + ": " + v) + "\"");
    }

    auto rsp_res = WriteRestrictedResponseFile(task_tmp_dir, "job.rsp", rsp_lines);
    if (!rsp_res.ok()) {
        task.state = TaskState::kFailed;
        task.error_code = rsp_res.error->code;
        task.error_message = rsp_res.error->message;
        task.status_detail = task.error_message;
        return Result<std::filesystem::path>::Err(task.error_code, task.error_message);
    }

    task.state = TaskState::kProbing;
    task.status_detail = "Probing stream manifest...";

    static const std::regex kProgressRe(R"((\d+)/(\d+)\s+(\d+(?:\.\d+)?)%\s*\|\s*([0-9.]+)MB/([0-9.]+)MB\s*\|\s*([0-9.]+)MBps)");
    std::string last_error_line;

    struct StreamTrackDlState {
        uint64_t dl_bytes{0};
        uint64_t tot_bytes{0};
        uint32_t done_segs{0};
        uint32_t tot_segs{0};
    };
    std::map<std::string, StreamTrackDlState> stream_tracks;

    auto process_line = [&](const std::string& line) {
        if (line.empty()) return;
        OPENIDM_LOG_INFO("StreamWorker", line);
        if (line.find("ERROR") != std::string::npos ||
            line.find("Exception") != std::string::npos ||
            line.find("403") != std::string::npos ||
            line.find("Forbidden") != std::string::npos ||
            line.find("401") != std::string::npos ||
            line.find("Unauthorized") != std::string::npos) {
            last_error_line = line;
        }
        if (line.find("Muxing") != std::string::npos || line.find("Merging") != std::string::npos) {
            task.state = TaskState::kMuxing;
            task.status_detail = "Merging audio/video tracks (FFmpeg)...";
            task.stream_percent = 99.0;
            task.speed_bps = 0;
            if (on_progress) {
                StreamProgress sp;
                sp.task_id = task.task_id;
                sp.phase = TaskState::kMuxing;
                sp.percent = 99.0;
                sp.downloaded_bytes = task.downloaded_bytes;
                sp.total_bytes = task.total_bytes;
                sp.completed_segments = task.completed_segments;
                sp.total_segments = task.total_segments;
                sp.speed_bps = 0;
                sp.status_line = task.status_detail;
                on_progress(sp);
            }
            return;
        }
        std::smatch m;
        if (std::regex_search(line, m, kProgressRe)) {
            uint32_t done_seg = static_cast<uint32_t>(std::stoul(m[1].str()));
            uint32_t tot_seg = static_cast<uint32_t>(std::stoul(m[2].str()));
            double pct = std::stod(m[3].str());
            uint64_t dl_b = static_cast<uint64_t>(std::stod(m[4].str()) * 1048576.0);
            uint64_t tot_b = static_cast<uint64_t>(std::stod(m[5].str()) * 1048576.0);
            uint64_t spd_b = static_cast<uint64_t>(std::stod(m[6].str()) * 1048576.0);

            std::string media_key = "video";
            std::string media_label = "video segments";
            if (line.rfind("Aud", 0) == 0) {
                media_key = "audio";
                media_label = "audio segments";
            } else if (line.rfind("Sub", 0) == 0) {
                media_key = "subtitle";
                media_label = "subtitle tracks";
            }
            stream_tracks[media_key] = {dl_b, tot_b, done_seg, tot_seg};

            uint64_t agg_dl = 0;
            uint64_t agg_tot = 0;
            uint32_t agg_done_seg = 0;
            uint32_t agg_tot_seg = 0;
            for (const auto& [k, st] : stream_tracks) {
                agg_dl += st.dl_bytes;
                agg_tot += st.tot_bytes;
                agg_done_seg += st.done_segs;
                agg_tot_seg += st.tot_segs;
            }

            double agg_pct = (agg_tot > 0)
                ? std::min(99.0, (static_cast<double>(agg_dl) / static_cast<double>(agg_tot)) * 100.0)
                : std::min(99.0, pct);

            task.state = TaskState::kDownloading;
            task.downloaded_bytes = agg_dl;
            task.total_bytes = agg_tot;
            task.speed_bps = spd_b;
            task.stream_percent = agg_pct;
            task.completed_segments = agg_done_seg;
            task.total_segments = agg_tot_seg;
            task.status_detail = "Downloading " + media_label + " (" + std::to_string(done_seg) + "/" + std::to_string(tot_seg) + ")";

            if (on_progress) {
                StreamProgress sp;
                sp.task_id = task.task_id;
                sp.phase = TaskState::kDownloading;
                sp.percent = agg_pct;
                sp.downloaded_bytes = agg_dl;
                sp.total_bytes = agg_tot;
                sp.speed_bps = spd_b;
                sp.completed_segments = agg_done_seg;
                sp.total_segments = agg_tot_seg;
                sp.status_line = task.status_detail;
                on_progress(sp);
            }
        }
    };

    int exit_code = -1;

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE hReadPipe = nullptr;
    HANDLE hWritePipe = nullptr;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        return Result<std::filesystem::path>::Err("ERR_OS_PIPE", "CreatePipe failed");
    }
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    std::wstring cmd = L"\"" + m_cfg.nm3u8dl_binary_path.wstring() + L"\" "
                       L"--no-ansi-color --disable-update-check "
                       L"--ui-language en-US "
                       L"\"@" + rsp_res.value->wstring() + L"\"";

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        CloseHandle(hJob);
        return Result<std::filesystem::path>::Err("ERR_OS_SPAWN", "CreateProcessW failed for N_m3u8DL-RE");
    }
    AssignProcessToJobObject(hJob, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(hWritePipe);

    std::string line_buf;
    char rbuf[4096];
    bool child_exited = false;
    while (!child_exited) {
        if (pause_requested.load() || cancel_requested.load()) {
            TerminateJobObject(hJob, 1);
            WaitForSingleObject(pi.hProcess, 3000);
            child_exited = true;
            break;
        }
        DWORD avail = 0;
        if (PeekNamedPipe(hReadPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            DWORD got = 0;
            if (ReadFile(hReadPipe, rbuf, std::min<DWORD>(avail, sizeof(rbuf)), &got, nullptr) && got > 0) {
                line_buf.append(rbuf, got);
                size_t nl;
                while ((nl = line_buf.find_first_of("\r\n")) != std::string::npos) {
                    std::string one = line_buf.substr(0, nl);
                    line_buf.erase(0, nl + 1);
                    process_line(one);
                }
            }
        } else if (WaitForSingleObject(pi.hProcess, 25) == WAIT_OBJECT_0) {
            child_exited = true;
        }
    }
    DWORD rem_avail = 0;
    while (PeekNamedPipe(hReadPipe, nullptr, 0, nullptr, &rem_avail, nullptr) && rem_avail > 0) {
        DWORD got = 0;
        if (ReadFile(hReadPipe, rbuf, std::min<DWORD>(rem_avail, sizeof(rbuf)), &got, nullptr) && got > 0) {
            line_buf.append(rbuf, got);
            size_t nl;
            while ((nl = line_buf.find_first_of("\r\n")) != std::string::npos) {
                std::string one = line_buf.substr(0, nl);
                line_buf.erase(0, nl + 1);
                process_line(one);
            }
        } else {
            break;
        }
    }
    if (!line_buf.empty()) {
        process_line(line_buf);
    }
    DWORD wcode = 1;
    GetExitCodeProcess(pi.hProcess, &wcode);
    exit_code = static_cast<int>(wcode);
    CloseHandle(hReadPipe);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(hJob);
#else
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return Result<std::filesystem::path>::Err("ERR_OS_PIPE", "pipe() failed");
    }

    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        ::close(pipefd[0]);
        int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            if (devnull > 2) ::close(devnull);
        }
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        if (pipefd[1] > 2) ::close(pipefd[1]);

        std::string rsp_arg = "@" + rsp_res.value->string();
        execl(m_cfg.nm3u8dl_binary_path.c_str(),
              m_cfg.nm3u8dl_binary_path.c_str(),
              "--no-ansi-color",
              "--disable-update-check",
              "--ui-language", "en-US",
              rsp_arg.c_str(),
              nullptr);
        _exit(127);
    }

    ::close(pipefd[1]);
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    std::string line_buf;
    char rbuf[4096];
    int exit_status = 0;
    bool child_exited = false;

    while (!child_exited) {
        if (pause_requested.load() || cancel_requested.load()) {
            kill(-pid, SIGTERM);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            kill(-pid, SIGKILL);
            waitpid(pid, &exit_status, 0);
            child_exited = true;
            break;
        }

        ssize_t n = ::read(pipefd[0], rbuf, sizeof(rbuf));
        if (n > 0) {
            line_buf.append(rbuf, static_cast<size_t>(n));
            size_t nl;
            while ((nl = line_buf.find_first_of("\r\n")) != std::string::npos) {
                std::string one = line_buf.substr(0, nl);
                line_buf.erase(0, nl + 1);
                process_line(one);
            }
        } else {
            pid_t w = waitpid(pid, &exit_status, WNOHANG);
            if (w == pid) {
                child_exited = true;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        }
    }

    while (true) {
        ssize_t n = ::read(pipefd[0], rbuf, sizeof(rbuf));
        if (n <= 0) break;
        line_buf.append(rbuf, static_cast<size_t>(n));
    }
    ::close(pipefd[0]);
    if (!line_buf.empty()) process_line(line_buf);
    exit_code = WIFEXITED(exit_status) ? WEXITSTATUS(exit_status) : -1;
#endif

    // Wipe @job.rsp immediately so credentials never linger on disk
    {
        std::ofstream wipe(*rsp_res.value, std::ios::binary | std::ios::trunc);
        wipe << std::string(256, '\0');
    }
    std::filesystem::remove(*rsp_res.value, ec);

    if (cancel_requested.load()) {
        std::filesystem::remove_all(task_tmp_dir, ec);
        task.state = TaskState::kCanceled;
        task.status_detail = "Canceled";
        return Result<std::filesystem::path>::Err("ERR_CANCELED", "Stream task canceled by user");
    }
    if (pause_requested.load()) {
        task.state = TaskState::kPaused;
        task.status_detail = "Paused (segments cached)";
        return Result<std::filesystem::path>::Err("ERR_PAUSED", "Stream task paused by user");
    }

    std::filesystem::path muxed_output = std::filesystem::path(task.save_dir) / (temp_save_name + "." + fmt);

    if (exit_code != 0 || !std::filesystem::exists(muxed_output, ec) ||
        std::filesystem::file_size(muxed_output, ec) == 0 ||
        !VerifyContainerMagicBytes(muxed_output, fmt)) {
        std::filesystem::remove(muxed_output, ec);
        task.state = TaskState::kFailed;
        if (last_error_line.find("DRM") != std::string::npos) {
            std::filesystem::remove_all(task_tmp_dir, ec);
            task.error_code = "ERR_STREAM_DRM_PROTECTED";
            task.error_message = last_error_line;
        } else if (last_error_line.find("403") != std::string::npos ||
                   last_error_line.find("Forbidden") != std::string::npos ||
                   last_error_line.find("401") != std::string::npos ||
                   last_error_line.find("Unauthorized") != std::string::npos) {
            task.error_code = "ERR_STREAM_URL_EXPIRED";
            task.error_message = "Stream manifest URL expired (HTTP 403 Forbidden). Please refresh the URL in Flux to resume.";
        } else {
            task.error_code = "ERR_STREAM_WORKER_CRASH";
            task.error_message = last_error_line.empty() ? ("Worker exited with code " + std::to_string(exit_code)) : last_error_line;
        }
        task.status_detail = task.error_message;
        return Result<std::filesystem::path>::Err(task.error_code, task.error_message, false, true);
    }

    task.state = TaskState::kVerifying;
    task.status_detail = "Verifying output container...";
    uint64_t final_size = std::filesystem::file_size(muxed_output, ec);
    task.downloaded_bytes = final_size;
    task.total_bytes = final_size;
    task.stream_percent = 99.5;
    if (on_progress) {
        StreamProgress sp;
        sp.task_id = task.task_id;
        sp.phase = TaskState::kVerifying;
        sp.percent = 99.5;
        sp.downloaded_bytes = final_size;
        sp.total_bytes = final_size;
        sp.status_line = task.status_detail;
        on_progress(sp);
    }

    std::string req_filter = ToLowerStr(TrimView(task.stream_options.video_filter));
    bool wants_audio_only = (req_filter == "audio" || req_filter == "audio-only");
    int target_height = 0;
    {
        std::smatch hm;
        static const std::regex kFilterHeightRe(R"((\d{3,4})p?)");
        if (!wants_audio_only && req_filter != "best" && std::regex_search(req_filter, hm, kFilterHeightRe)) {
            target_height = std::stoi(hm[1].str());
        }
    }

    MediaQualityExpectations expectations;
    expectations.expected_height = target_height;
    expectations.expected_container = fmt;
    expectations.expect_video = !wants_audio_only;
    expectations.expect_audio = true;
    expectations.expected_min_bytes = 1024;

    auto quality_res = VerifyMediaQuality(muxed_output, expectations, m_cfg.ffprobe_binary_path);
    if (!quality_res.ok()) {
        std::filesystem::remove(muxed_output, ec);
        std::filesystem::remove_all(task_tmp_dir, ec);
        task.state = TaskState::kFailed;
        task.error_code = quality_res.error->code;
        task.error_message = quality_res.error->message;
        task.status_detail = task.error_message;
        return Result<std::filesystem::path>::Err(task.error_code, task.error_message, false, true);
    }

    task.state = TaskState::kFinalizing;
    task.status_detail = "Finalizing file...";
    task.stream_percent = 99.9;
    if (on_progress) {
        StreamProgress sp;
        sp.task_id = task.task_id;
        sp.phase = TaskState::kFinalizing;
        sp.percent = 99.9;
        sp.downloaded_bytes = quality_res.value->probed_file_size;
        sp.total_bytes = quality_res.value->probed_file_size;
        sp.status_line = task.status_detail;
        on_progress(sp);
    }

    std::string desired_final_name = base_stem + "." + fmt;
    auto commit_res = PathSanitizer::CommitPartFile(muxed_output, task.save_dir, desired_final_name, task.conflict_policy);
    std::filesystem::remove_all(task_tmp_dir, ec);

    if (!commit_res.ok()) {
        std::filesystem::remove(muxed_output, ec);
        task.state = TaskState::kFailed;
        task.error_code = commit_res.error->code;
        task.error_message = commit_res.error->message;
        task.status_detail = task.error_message;
        return commit_res;
    }

    uint64_t disk_sz = std::filesystem::file_size(*commit_res.value, ec);
    if (!ec && disk_sz > 0) {
        task.downloaded_bytes = disk_sz;
        task.total_bytes = disk_sz;
    }
    task.file_name = commit_res.value->filename().string();
    task.part_path.clear();
    task.stream_percent = 100.0;
    task.state = TaskState::kCompleted;
    task.status_detail = "Completed";
    task.completed_at_ms = NowEpochMs();
    task.auth_headers.clear();
    if (on_progress) {
        StreamProgress sp;
        sp.task_id = task.task_id;
        sp.phase = TaskState::kCompleted;
        sp.percent = 100.0;
        sp.downloaded_bytes = task.downloaded_bytes;
        sp.total_bytes = task.total_bytes;
        sp.status_line = task.status_detail;
        on_progress(sp);
    }
    return commit_res;
}

// -----------------------------------------------------------------------------
// Phase 3: Explicit YouTube / Site Extraction & Adaptive Track Muxing (Defect B)
// -----------------------------------------------------------------------------
Result<std::filesystem::path> Nm3u8dlReAdapter::ExecuteYouTubeExtraction(
    TaskModel& task,
    std::atomic<bool>& pause_requested,
    std::atomic<bool>& cancel_requested,
    uint64_t speed_limit_bps,
    ProgressCallback on_progress) {

    std::error_code ec;
    if (!std::filesystem::exists(m_cfg.ffmpeg_binary_path, ec)) {
        task.state = TaskState::kFailed;
        task.error_code = "ERR_DEPENDENCY_MISSING";
        task.error_message = "FFmpeg binary missing: " + m_cfg.ffmpeg_binary_path.string();
        task.status_detail = task.error_message;
        return Result<std::filesystem::path>::Err(task.error_code, task.error_message, false, true);
    }

    auto space_res = PathSanitizer::CheckAvailableDiskSpace(task.save_dir, 16ULL * 1048576ULL);
    if (!space_res.ok()) {
        task.state = TaskState::kFailed;
        task.error_code = space_res.error->code;
        task.error_message = space_res.error->message;
        task.status_detail = task.error_message;
        return Result<std::filesystem::path>::Err(task.error_code, task.error_message, false, true);
    }

    std::filesystem::path task_tmp_dir = m_cfg.temp_root_dir / task.task_id;
    std::filesystem::create_directories(task_tmp_dir, ec);
    std::filesystem::create_directories(task.save_dir, ec);
    task.part_path = task_tmp_dir.string();

    std::string fmt = (task.stream_options.output_format == "mkv") ? "mkv" : "mp4";
    std::string base_stem = PathSanitizer::SanitizeFileName(task.file_name.empty() ? "youtube_video" : task.file_name);
    auto dot = base_stem.find_last_of('.');
    if (dot != std::string::npos) {
        base_stem = base_stem.substr(0, dot);
    }
    std::filesystem::path muxed_output = std::filesystem::path(task.save_dir) / (base_stem + ".flux_yt_mux." + fmt);

    std::string watch_url = task.url;
    auto is_raw_googlevideo = [](std::string_view u) {
        return u.find("googlevideo.com") != std::string_view::npos || u.find("videoplayback") != std::string_view::npos;
    };
    if (is_raw_googlevideo(watch_url) || watch_url.empty()) {
        if (!task.page_url.empty() && !is_raw_googlevideo(task.page_url)) {
            watch_url = task.page_url;
        } else {
            watch_url.clear();
        }
    }
    std::string video_url;
    std::string audio_url;
    std::string hls_manifest_url;
    std::string dash_manifest_url;
    bool requires_cipher = false;
    bool requires_audio_mux = false;
    auto log_state_transition = [&](TaskState next_state, std::string_view reason, std::string_view path) {
        OPENIDM_LOG_INFO("Nm3u8dlAdapter", "task=" + task.task_id + " state=" + TaskStateToString(task.state) +
                         " -> " + TaskStateToString(next_state) + " reason=" + std::string(reason) +
                         " path=" + std::string(path));
        task.state = next_state;
    };

    log_state_transition(TaskState::kProbing, "youtube extraction started", "youtube");
    task.status_detail = "Extracting YouTube stream metadata...";
    if (on_progress) {
        StreamProgress sp;
        sp.task_id = task.task_id;
        sp.phase = TaskState::kProbing;
        sp.percent = 0.0;
        sp.status_line = task.status_detail;
        on_progress(sp);
    }

    std::string req_filter = ToLowerStr(TrimView(task.stream_options.video_filter));
    bool wants_audio_only = (req_filter == "audio" || req_filter == "audio-only");
    int target_height = 0;
    {
        std::smatch hm;
        static const std::regex kFilterHeightRe(R"((\d{3,4})p?)");
        if (!wants_audio_only && req_filter != "best" && std::regex_search(req_filter, hm, kFilterHeightRe)) {
            target_height = std::stoi(hm[1].str());
        }
    }

    // 1. Parse structured manifest body if provided as JSON
    int selected_itag = 0;
    int selected_width = 0;
    int selected_height = 0;
    std::string selected_resolution;

    if (!task.stream_options.cached_manifest_body.empty() &&
        (task.stream_options.cached_manifest_body.front() == '{' || task.stream_options.cached_manifest_body.front() == '[')) {
        try {
            auto root = json::Parse(task.stream_options.cached_manifest_body);
            if (root.is_object()) {
                if (root["selectedItag"].as_int64(0) > 0) {
                    selected_itag = static_cast<int>(root["selectedItag"].as_int64(0));
                } else if (root["itag"].as_int64(0) > 0) {
                    selected_itag = static_cast<int>(root["itag"].as_int64(0));
                }
                if (root["resolution"].is_string() && !root["resolution"].as_string().empty() && root["resolution"].as_string() != "Auto") {
                    selected_resolution = root["resolution"].as_string();
                }

                if (root["qualities"].is_array() && !req_filter.empty() && req_filter != "best") {
                    for (const auto& q : root["qualities"].as_array()) {
                        std::string qid = ToLowerStr(q["id"].as_string());
                        std::string qres = ToLowerStr(q["resolution"].as_string());
                        std::string qvf = ToLowerStr(q["videoFilter"].as_string());
                        int qh = static_cast<int>(q["height"].as_int64(0));
                        int qw = static_cast<int>(q["width"].as_int64(0));
                        int itag = static_cast<int>(q["itag"].as_int64(0));
                        if (qid == req_filter || qres == req_filter || qvf == req_filter ||
                            (target_height > 0 && qh == target_height) ||
                            (wants_audio_only && q["isAudioOnly"].as_bool(false))) {
                            selected_itag = itag;
                            selected_width = qw;
                            selected_height = qh;
                            selected_resolution = q["resolution"].as_string();
                            std::string qv = StripGooglevideoRangeParams(q["videoUrl"].as_string());
                            std::string qa = StripGooglevideoRangeParams(q["pairedAudioUrl"].as_string());
                            if (!qv.empty()) {
                                video_url = qv;
                                audio_url = qa;
                                requires_audio_mux = !audio_url.empty();
                                requires_cipher = q["requiresCipherExtraction"].as_bool(false);
                            } else {
                                requires_cipher = true;
                            }
                            break;
                        }
                    }
                }

                // Normalize target_height based on nominal selected resolution or dimensions:
                // For widescreen / scope videos (e.g. 1920x960 2:1 or 1920x800 2.39:1),
                // YouTube labels the quality as "1080p", but vertical pixels are 960 or 800.
                if (!wants_audio_only) {
                    std::string res_low = ToLowerStr(selected_resolution);
                    if (res_low.find("2160") != std::string::npos || (res_low.empty() && selected_width >= 3840)) {
                        target_height = 2160;
                    } else if (res_low.find("1440") != std::string::npos || (res_low.empty() && selected_width >= 2560)) {
                        target_height = 1440;
                    } else if (res_low.find("1080") != std::string::npos || (res_low.empty() && selected_width >= 1800)) {
                        target_height = 1080;
                    } else if (res_low.find("720") != std::string::npos || (res_low.empty() && selected_width >= 1200)) {
                        target_height = 720;
                    } else if (res_low.find("480") != std::string::npos || (res_low.empty() && selected_width >= 800)) {
                        target_height = 480;
                    }
                }

                // Never fall back to lower-resolution progressive root["videoUrl"] when a specific quality was requested!
                if (video_url.empty() && target_height == 0 && !requires_cipher) {
                    video_url = StripGooglevideoRangeParams(root["videoUrl"].as_string());
                }
                if (audio_url.empty() && !requires_cipher) {
                    audio_url = StripGooglevideoRangeParams(root["audioUrl"].as_string());
                }
                if (wants_audio_only && !audio_url.empty() && video_url != audio_url) {
                    video_url = audio_url;
                    audio_url.clear();
                    requires_audio_mux = false;
                }
                if (!root["videoId"].as_string().empty() && (watch_url.empty() || is_raw_googlevideo(watch_url))) {
                    watch_url = "https://www.youtube.com/watch?v=" + root["videoId"].as_string();
                } else if (watch_url.empty() || is_raw_googlevideo(watch_url)) {
                    std::string w = root["watchUrl"].as_string();
                    if (!w.empty() && !is_raw_googlevideo(w)) {
                        watch_url = w;
                    }
                }
                hls_manifest_url = root["hlsManifestUrl"].as_string();
                dash_manifest_url = root["dashManifestUrl"].as_string();
                if (root["requiresCipherExtraction"].as_bool(false) && video_url.empty()) requires_cipher = true;
                if (root["requiresAudioMux"].as_bool(false) && !wants_audio_only) requires_audio_mux = true;
            }
        } catch (...) {}
    }

    if (video_url.empty() && (req_filter.empty() || req_filter == "best") && task.url.find("googlevideo.com") != std::string::npos) {
        video_url = StripGooglevideoRangeParams(task.url);
    }

    if (watch_url.empty() || is_raw_googlevideo(watch_url)) {
        std::string src_for_id = (!task.url.empty() && is_raw_googlevideo(task.url)) ? task.url : video_url;
        std::smatch id_m;
        static const std::regex kYtIdParamRe(R"([?&](?:id|docid)=([a-zA-Z0-9_-]{11}))");
        if (std::regex_search(src_for_id, id_m, kYtIdParamRe)) {
            watch_url = "https://www.youtube.com/watch?v=" + id_m[1].str();
        } else if (!task.page_url.empty() && !is_raw_googlevideo(task.page_url)) {
            watch_url = task.page_url;
        }
    }

    if (!video_url.empty() && (video_url == watch_url || PathSanitizer::IsWebpageWatchUrl(video_url))) {
        if (watch_url.empty()) watch_url = video_url;
        video_url.clear();
    }

    bool is_youtube_host = (watch_url.find("youtube.com") != std::string::npos || watch_url.find("youtu.be") != std::string::npos);
    if (is_youtube_host) {
        std::smatch vid_m;
        static const std::regex kVidRe(R"((?:v=|\/shorts\/|\/embed\/|\/live\/|youtu\.be\/)([a-zA-Z0-9_-]{11}))");
        if (std::regex_search(watch_url, vid_m, kVidRe)) {
            watch_url = "https://www.youtube.com/watch?v=" + vid_m[1].str();
        } else if (std::regex_search(task.url, vid_m, kVidRe)) {
            watch_url = "https://www.youtube.com/watch?v=" + vid_m[1].str();
        } else if (std::regex_search(task.page_url, vid_m, kVidRe)) {
            watch_url = "https://www.youtube.com/watch?v=" + vid_m[1].str();
        }
    }

    std::map<std::string, std::string> combined_headers = task.headers;
    for (const auto& [k, v] : task.auth_headers) {
        combined_headers[k] = v;
    }
    if (combined_headers.find("User-Agent") == combined_headers.end()) {
        combined_headers["User-Agent"] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";
    }
    if (combined_headers.find("Referer") == combined_headers.end()) {
        combined_headers["Referer"] = (!watch_url.empty() && watch_url.find("youtube.com") != std::string::npos) ? watch_url : "https://www.youtube.com/";
    }

    // 2. If watch_url is a webpage or player endpoint and video_url is not yet resolved, inspect page/player JSON
    auto extract_from_player_json = [&](const json::Value& root) -> Result<void> {
        if (!root.is_object()) {
            return Result<void>::Err("ERR_YOUTUBE_PARSE", "Invalid YouTube player response object");
        }
        const auto& play_status = root["playabilityStatus"];
        if (play_status.is_object()) {
            std::string st = play_status["status"].as_string("OK");
            if (st != "OK" && !root["streamingData"].is_object()) {
                std::string reason = play_status["reason"].as_string("YouTube stream status: " + st);
                return Result<void>::Err("ERR_YOUTUBE_UNPLAYABLE", reason);
            }
        }
        const auto& sdata = root["streamingData"];
        if (!sdata.is_object()) {
            return Result<void>::Err("ERR_YOUTUBE_NO_STREAMS", "No streamingData found in YouTube player response");
        }

        if (hls_manifest_url.empty()) hls_manifest_url = sdata["hlsManifestUrl"].as_string();
        if (dash_manifest_url.empty()) dash_manifest_url = sdata["dashManifestUrl"].as_string();

        uint64_t best_v_bw = 0;
        uint64_t matched_v_bw = 0;
        std::string matched_v_url;
        uint64_t best_a_bw = 0;
        bool saw_cipher = false;
        bool saw_drm = false;

        if (sdata["adaptiveFormats"].is_array()) {
            for (const auto& item : sdata["adaptiveFormats"].as_array()) {
                if (item["drmFamilies"].is_array()) {
                    saw_drm = true;
                    continue;
                }
                std::string u = item["url"].as_string();
                if (u.empty()) {
                    if (!item["signatureCipher"].as_string().empty() || !item["cipher"].as_string().empty()) {
                        saw_cipher = true;
                    }
                    continue;
                }
                std::string mime = ToLowerStr(item["mimeType"].as_string());
                uint64_t bw = static_cast<uint64_t>(item["bitrate"].as_int64(item["averageBitrate"].as_int64(500000)));
                int h = static_cast<int>(item["height"].as_int64(0));
                std::string qlabel = ToLowerStr(item["qualityLabel"].as_string());
                if (mime.rfind("video/", 0) == 0) {
                    if (bw >= best_v_bw) {
                        best_v_bw = bw;
                        video_url = StripGooglevideoRangeParams(u);
                    }
                    if (target_height > 0 && (h == target_height || qlabel == req_filter || qlabel.rfind(std::to_string(target_height) + "p", 0) == 0)) {
                        if (bw >= matched_v_bw) {
                            matched_v_bw = bw;
                            matched_v_url = StripGooglevideoRangeParams(u);
                        }
                    }
                } else if (mime.rfind("audio/", 0) == 0 && bw >= best_a_bw) {
                    best_a_bw = bw;
                    audio_url = StripGooglevideoRangeParams(u);
                }
            }
        }

        if (wants_audio_only && !audio_url.empty()) {
            video_url = audio_url;
            audio_url.clear();
            requires_audio_mux = false;
            requires_cipher = false;
            return Result<void>::Ok();
        }

        if (!matched_v_url.empty()) {
            video_url = matched_v_url;
        }

        if (!video_url.empty() && !audio_url.empty()) {
            requires_audio_mux = true;
            requires_cipher = false;
            return Result<void>::Ok();
        }

        // If adaptive video had no paired audio, check progressive formats (combined audio+video)
        // BUT ONLY IF target_height <= 0 (best/auto) OR progressive format matches target_height!
        std::string best_prog_url;
        uint64_t best_prog_bw = 0;
        int best_prog_height = 0;
        if (sdata["formats"].is_array()) {
            for (const auto& item : sdata["formats"].as_array()) {
                if (item["drmFamilies"].is_array()) {
                    saw_drm = true;
                    continue;
                }
                std::string u = item["url"].as_string();
                if (u.empty()) {
                    if (!item["signatureCipher"].as_string().empty() || !item["cipher"].as_string().empty()) {
                        saw_cipher = true;
                    }
                    continue;
                }
                uint64_t bw = static_cast<uint64_t>(item["bitrate"].as_int64(1000000));
                int ph = static_cast<int>(item["height"].as_int64(0));
                if (target_height > 0) {
                    if (ph == target_height && (bw >= best_prog_bw || best_prog_url.empty())) {
                        best_prog_bw = bw;
                        best_prog_height = ph;
                        best_prog_url = StripGooglevideoRangeParams(u);
                    }
                } else if (bw >= best_prog_bw || best_prog_url.empty()) {
                    best_prog_bw = bw;
                    best_prog_height = ph;
                    best_prog_url = StripGooglevideoRangeParams(u);
                }
            }
        }

        if (!best_prog_url.empty() && (target_height == 0 || best_prog_height == target_height)) {
            video_url = best_prog_url;
            audio_url.clear();
            requires_audio_mux = false;
            requires_cipher = false;
            return Result<void>::Ok();
        }

        // If a specific resolution was requested (e.g. 1080p) and adaptive pairing was incomplete or requires cipher:
        if (target_height > 0 && (video_url.empty() || audio_url.empty())) {
            video_url.clear();
            audio_url.clear();
            requires_cipher = true; // Force external yt-dlp worker with exact format selector
            return Result<void>::Ok();
        }

        if (saw_drm && video_url.empty()) {
            return Result<void>::Err("ERR_STREAM_DRM_PROTECTED", "YouTube stream uses unsupported DRM protection");
        }
        if (saw_cipher) {
            requires_cipher = true;
        }
        return Result<void>::Ok();
    };

    if (video_url.empty() && !watch_url.empty() && hls_manifest_url.empty() && dash_manifest_url.empty() && !is_youtube_host) {
        std::string page_body;
        auto get_res = brisk::HttpClient::PerformGet(
            watch_url,
            combined_headers,
            std::nullopt,
            std::nullopt,
            20000,
            m_cfg.allow_loopback_manifests,
            cancel_requested,
            [](const brisk::HttpResponseMeta&) { return true; },
            [&](const uint8_t* data, size_t len) {
                if (page_body.size() + len > 8 * 1048576) return false;
                page_body.append(reinterpret_cast<const char*>(data), len);
                return true;
            });
        if (get_res.ok() && !page_body.empty()) {
            std::string trimmed_page = TrimView(page_body);
            if (!trimmed_page.empty() && trimmed_page.front() == '{') {
                try {
                    auto parsed_j = json::Parse(trimmed_page);
                    auto ex_res = extract_from_player_json(parsed_j);
                    if (!ex_res.ok()) {
                        std::filesystem::remove_all(task_tmp_dir, ec);
                        task.state = TaskState::kFailed;
                        task.error_code = ex_res.error->code;
                        task.error_message = ex_res.error->message;
                        task.status_detail = task.error_message;
                        return Result<std::filesystem::path>::Err(task.error_code, task.error_message);
                    }
                } catch (...) {}
            } else {
                auto yt_pos = page_body.find("ytInitialPlayerResponse");
                if (yt_pos != std::string::npos) {
                    std::string json_str = ExtractBalancedJsonObject(page_body, yt_pos);
                    if (!json_str.empty()) {
                        try {
                            auto parsed_j = json::Parse(json_str);
                            auto ex_res = extract_from_player_json(parsed_j);
                            if (!ex_res.ok()) {
                                std::filesystem::remove_all(task_tmp_dir, ec);
                                task.state = TaskState::kFailed;
                                task.error_code = ex_res.error->code;
                                task.error_message = ex_res.error->message;
                                task.status_detail = task.error_message;
                                return Result<std::filesystem::path>::Err(task.error_code, task.error_message);
                            }
                        } catch (...) {}
                    }
                }
            }
        }
    }

    // 3. If an actual HLS or DASH manifest URL was discovered in streamingData, delegate to N_m3u8DL-RE
    if (video_url.empty() && (!hls_manifest_url.empty() || !dash_manifest_url.empty())) {
        TaskModel delegated = task;
        if (!hls_manifest_url.empty()) {
            delegated.url = hls_manifest_url;
            delegated.task_type = TaskType::kStreamHls;
        } else {
            delegated.url = dash_manifest_url;
            delegated.task_type = TaskType::kStreamDash;
        }
        delegated.stream_options.cached_manifest_body.clear();
        auto res = ExecuteStreamDownload(delegated, pause_requested, cancel_requested, speed_limit_bps, on_progress);
        task.state = delegated.state;
        task.file_name = delegated.file_name;
        task.downloaded_bytes = delegated.downloaded_bytes;
        task.total_bytes = delegated.total_bytes;
        task.stream_percent = delegated.stream_percent;
        task.status_detail = delegated.status_detail;
        task.error_code = delegated.error_code;
        task.error_message = delegated.error_message;
        task.completed_at_ms = delegated.completed_at_ms;
        return res;
    }

    auto execute_direct_download = [&]() -> Result<std::filesystem::path> {
        // 5. Validate resolved video_url & audio_url requirements
        if (video_url.empty()) {
            std::filesystem::remove_all(task_tmp_dir, ec);
            task.state = TaskState::kFailed;
            if (is_youtube_host) {
                task.error_code = requires_cipher ? "ERR_YOUTUBE_EXTRACTOR_REQUIRED" : "ERR_YOUTUBE_NO_PLAYABLE_STREAM";
                task.error_message = requires_cipher
                    ? "YouTube stream uses signatureCipher encryption and requires yt-dlp (install yt-dlp in tools/ or PATH)"
                    : "No playable video stream URL could be resolved from YouTube player response";
            } else {
                task.error_code = "ERR_MANIFEST_PARSE_FAILED";
                task.error_message = "No playable media stream found for video source";
            }
            task.status_detail = task.error_message;
            return Result<std::filesystem::path>::Err(task.error_code, task.error_message);
        }

        if (requires_audio_mux && audio_url.empty()) {
            std::filesystem::remove_all(task_tmp_dir, ec);
            task.state = TaskState::kFailed;
            task.error_code = "ERR_YOUTUBE_INCOMPLETE_ADAPTIVE";
            task.error_message = "Adaptive YouTube video stream has no paired audio track; refusing to save incomplete silent video";
            task.status_detail = task.error_message;
            return Result<std::filesystem::path>::Err(task.error_code, task.error_message);
        }

        // 6. Download video track (and paired audio track if adaptive) and mux via FFmpeg
        std::filesystem::path video_tmp = task_tmp_dir / "yt_video.mp4";
        std::filesystem::path audio_tmp = task_tmp_dir / "yt_audio.m4a";
        bool has_separate_audio = (!audio_url.empty() && audio_url != video_url);
        uint32_t total_tracks = has_separate_audio ? 2u : 1u;
        uint64_t cumulative_bytes = 0;

        auto download_track = [&](const std::string& track_url, const std::filesystem::path& dest_path, uint32_t track_idx, const std::string& label) -> Result<void> {
            std::ofstream ofs(dest_path, std::ios::binary | std::ios::trunc);
            if (!ofs) {
                return Result<void>::Err("ERR_STORAGE_WRITE", "Failed to open temporary track file: " + dest_path.string());
            }
            auto track_start = std::chrono::steady_clock::now();
            uint64_t track_total = 0;
            uint64_t track_dl = 0;
            auto res = brisk::HttpClient::PerformGet(
                track_url,
                combined_headers,
                std::nullopt,
                std::nullopt,
                30000,
                m_cfg.allow_loopback_manifests,
                cancel_requested,
                [&](const brisk::HttpResponseMeta& meta) -> brisk::HeaderAction {
                    if (meta.status_code != 200 && meta.status_code != 206) {
                        return brisk::HeaderAction::kAbort;
                    }
                    if (meta.content_length > 0) {
                        track_total = meta.content_length;
                    }
                    return brisk::HeaderAction::kContinue;
                },
                [&](const uint8_t* data, size_t len) -> bool {
                    if (pause_requested.load() || cancel_requested.load()) return false;
                    ofs.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
                    if (!ofs.good()) return false;
                    track_dl += len;
                    cumulative_bytes += len;

                    auto now = std::chrono::steady_clock::now();
                    auto elapsed_ms = std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::milliseconds>(now - track_start).count());
                    task.speed_bps = (track_dl * 1000ULL) / static_cast<uint64_t>(elapsed_ms);

                    double track_pct = (track_total > 0) ? ((static_cast<double>(track_dl) * 100.0) / static_cast<double>(track_total)) : 50.0;
                    double overall_pct = ((static_cast<double>(track_idx - 1) * 100.0) + track_pct) / static_cast<double>(total_tracks);

                    task.state = TaskState::kDownloading;
                    task.downloaded_bytes = cumulative_bytes;
                    if (track_total > 0) {
                        task.total_bytes = std::max(task.total_bytes, cumulative_bytes);
                    }
                    task.stream_percent = std::clamp(overall_pct, 0.0, 98.0);
                    task.completed_segments = track_idx - 1;
                    task.total_segments = total_tracks;
                    task.status_detail = "Downloading YouTube " + label + " (" + std::to_string(track_idx) + "/" + std::to_string(total_tracks) + ")";

                    if (on_progress) {
                        StreamProgress sp;
                        sp.task_id = task.task_id;
                        sp.phase = TaskState::kDownloading;
                        sp.percent = task.stream_percent;
                        sp.downloaded_bytes = task.downloaded_bytes;
                        sp.total_bytes = task.total_bytes;
                        sp.speed_bps = task.speed_bps;
                        sp.completed_segments = task.completed_segments;
                        sp.total_segments = task.total_segments;
                        sp.status_line = task.status_detail;
                        on_progress(sp);
                    }
                    return true;
                });

            ofs.flush();
            if (!ofs.good()) {
                return Result<void>::Err("ERR_STORAGE_WRITE", "Failed to write or flush track file");
            }
            ofs.close();
            if (!res.ok()) {
                return Result<void>::Err(res.error->code, res.error->message);
            }
            if (cancel_requested.load()) {
                return Result<void>::Err("ERR_CANCELED", "Download canceled");
            }
            if (pause_requested.load()) {
                return Result<void>::Err("ERR_PAUSED", "Download paused");
            }
            if (!res.ok()) {
                return Result<void>::Err(res.error->code, "Failed to download YouTube " + label + ": " + res.error->message);
            }
            if (track_total > 0 && track_dl != track_total) {
                return Result<void>::Err("ERR_YOUTUBE_TRUNCATED_TRACK", "Downloaded YouTube " + label + " is truncated");
            }
            if (std::filesystem::file_size(dest_path, ec) == 0) {
                return Result<void>::Err("ERR_YOUTUBE_EMPTY_TRACK", "Downloaded YouTube " + label + " is empty (0 bytes)");
            }
            return Result<void>::Ok();
        };

        auto v_res = download_track(video_url, video_tmp, 1, "video track");
        if (!v_res.ok()) {
            if (cancel_requested.load()) {
                std::filesystem::remove_all(task_tmp_dir, ec);
            }
            task.state = cancel_requested.load() ? TaskState::kCanceled : (pause_requested.load() ? TaskState::kPaused : TaskState::kFailed);
            task.error_code = v_res.error->code;
            task.error_message = v_res.error->message;
            task.status_detail = task.error_message;
            return Result<std::filesystem::path>::Err(task.error_code, task.error_message);
        }

        if (has_separate_audio) {
            auto a_res = download_track(audio_url, audio_tmp, 2, "audio track");
            if (!a_res.ok()) {
                if (cancel_requested.load()) {
                    std::filesystem::remove_all(task_tmp_dir, ec);
                }
                task.state = cancel_requested.load() ? TaskState::kCanceled : (pause_requested.load() ? TaskState::kPaused : TaskState::kFailed);
                task.error_code = a_res.error->code;
                task.error_message = a_res.error->message;
                task.status_detail = task.error_message;
                return Result<std::filesystem::path>::Err(task.error_code, task.error_message);
            }
        }

        // 7. Mux via FFmpeg into clean output container
        task.state = TaskState::kMuxing;
        task.completed_segments = total_tracks;
        task.total_segments = total_tracks;
        task.status_detail = "Merging YouTube audio/video tracks (FFmpeg)...";
        if (on_progress) {
            StreamProgress sp;
            sp.task_id = task.task_id;
            sp.phase = TaskState::kMuxing;
            sp.percent = 99.0;
            sp.downloaded_bytes = task.downloaded_bytes;
            sp.total_bytes = task.downloaded_bytes;
            sp.completed_segments = total_tracks;
            sp.total_segments = total_tracks;
            sp.status_line = task.status_detail;
            on_progress(sp);
        }

        std::vector<std::string> ff_args = {"-y", "-loglevel", "error", "-i", video_tmp.string()};
        if (has_separate_audio) {
            ff_args.push_back("-i");
            ff_args.push_back(audio_tmp.string());
            ff_args.insert(ff_args.end(), {"-map", "0:v:0", "-map", "1:a:0"});
        }
        ff_args.insert(ff_args.end(), {"-c:v", "copy", "-c:a", "copy", muxed_output.string()});

        int mux_rc = RunExternalFfmpegMux(m_cfg.ffmpeg_binary_path, ff_args);
        if ((mux_rc != 0 || !std::filesystem::exists(muxed_output, ec) || std::filesystem::file_size(muxed_output, ec) == 0) &&
            has_separate_audio && fmt == "mp4") {
            std::filesystem::remove(muxed_output, ec);
            std::vector<std::string> ff_retry_args = {
                "-y", "-loglevel", "error",
                "-i", video_tmp.string(),
                "-i", audio_tmp.string(),
                "-map", "0:v:0", "-map", "1:a:0",
                "-c:v", "copy",
                "-c:a", "aac", "-b:a", "192k",
                muxed_output.string()
            };
            mux_rc = RunExternalFfmpegMux(m_cfg.ffmpeg_binary_path, ff_retry_args);
        }

        if (mux_rc != 0 || !std::filesystem::exists(muxed_output, ec) ||
            std::filesystem::file_size(muxed_output, ec) == 0 ||
            !VerifyContainerMagicBytes(muxed_output, fmt)) {
            std::filesystem::remove_all(task_tmp_dir, ec);
            std::filesystem::remove(muxed_output, ec);
            task.state = TaskState::kFailed;
            task.error_code = "ERR_YOUTUBE_MUX_FAILED";
            task.error_message = "FFmpeg failed to mux YouTube tracks (exit code " + std::to_string(mux_rc) + ")";
            task.status_detail = task.error_message;
            return Result<std::filesystem::path>::Err(task.error_code, task.error_message);
        }

        task.state = TaskState::kVerifying;
        task.status_detail = "Verifying output container and stream quality...";
        task.stream_percent = 99.5;
        if (on_progress) {
            StreamProgress sp;
            sp.task_id = task.task_id;
            sp.phase = TaskState::kVerifying;
            sp.percent = 99.5;
            sp.downloaded_bytes = task.downloaded_bytes;
            sp.total_bytes = task.downloaded_bytes;
            sp.status_line = task.status_detail;
            on_progress(sp);
        }

        MediaQualityExpectations expectations;
        expectations.expected_height = target_height;
        expectations.expected_container = fmt;
        expectations.expect_video = !wants_audio_only;
        expectations.expect_audio = has_separate_audio;
        expectations.expected_min_bytes = 1024;

        auto quality_res = VerifyMediaQuality(muxed_output, expectations, m_cfg.ffprobe_binary_path);
        if (!quality_res.ok()) {
            std::filesystem::remove_all(task_tmp_dir, ec);
            std::filesystem::remove(muxed_output, ec);
            task.state = TaskState::kFailed;
            task.error_code = quality_res.error->code;
            task.error_message = quality_res.error->message;
            task.status_detail = task.error_message;
            return Result<std::filesystem::path>::Err(task.error_code, task.error_message);
        }

        task.state = TaskState::kFinalizing;
        task.status_detail = "Finalizing file...";
        task.stream_percent = 99.9;
        if (on_progress) {
            StreamProgress sp;
            sp.task_id = task.task_id;
            sp.phase = TaskState::kFinalizing;
            sp.percent = 99.9;
            sp.downloaded_bytes = quality_res.value->probed_file_size;
            sp.total_bytes = quality_res.value->probed_file_size;
            sp.status_line = task.status_detail;
            on_progress(sp);
        }

        std::string desired_final_name = base_stem + "." + fmt;
        auto commit_res = PathSanitizer::CommitPartFile(muxed_output, task.save_dir, desired_final_name, task.conflict_policy);
        std::filesystem::remove_all(task_tmp_dir, ec);

        if (!commit_res.ok()) {
            task.state = TaskState::kFailed;
            task.error_code = commit_res.error->code;
            task.error_message = commit_res.error->message;
            task.status_detail = task.error_message;
            return commit_res;
        }

        uint64_t disk_sz = std::filesystem::file_size(*commit_res.value, ec);
        if (!ec && disk_sz > 0) {
            task.downloaded_bytes = disk_sz;
            task.total_bytes = disk_sz;
        } else {
            task.downloaded_bytes = quality_res.value->probed_file_size;
            task.total_bytes = quality_res.value->probed_file_size;
        }
        task.file_name = commit_res.value->filename().string();
        task.part_path.clear();
        task.stream_percent = 100.0;
        log_state_transition(TaskState::kCompleted, "verified output committed", "direct");
        task.status_detail = "Completed";
        task.completed_at_ms = NowEpochMs();
        task.error_code.clear();
        task.error_message.clear();
        task.auth_headers.clear();
        if (on_progress) {
            StreamProgress sp;
            sp.task_id = task.task_id;
            sp.phase = TaskState::kCompleted;
            sp.percent = 100.0;
            sp.downloaded_bytes = task.downloaded_bytes;
            sp.total_bytes = task.total_bytes;
            sp.status_line = task.status_detail;
            on_progress(sp);
        }
        return commit_res;
    };

    // 3.5. Direct stream fast-path (< 10ms latency): if direct unthrottled video & audio URLs are available, download directly!
    bool has_direct_urls = !video_url.empty() && !requires_cipher && (!requires_audio_mux || !audio_url.empty());
    if (has_direct_urls) {
        OPENIDM_LOG_INFO("Nm3u8dlAdapter", "Attempting direct stream fast-path (<10ms latency) for video_url: " + video_url);
        auto direct_res = execute_direct_download();
        if (direct_res.ok()) {
            return direct_res;
        }
        if (cancel_requested.load() || pause_requested.load()) {
            return direct_res;
        }
        OPENIDM_LOG_WARN("Nm3u8dlAdapter", "task=" + task.task_id + " direct path failed code=" + direct_res.error->code +
                         " message=" + direct_res.error->message + "; falling back to yt-dlp worker");
        std::filesystem::remove(muxed_output, ec);
        std::filesystem::remove(task_tmp_dir / "yt_video.mp4", ec);
        std::filesystem::remove(task_tmp_dir / "yt_audio.m4a", ec);
        task.error_code.clear();
        task.error_message.clear();
        task.downloaded_bytes = 0;
        task.total_bytes = 0;
        task.stream_percent = 0.0;
        log_state_transition(TaskState::kProbing, "retrying after direct path failure", "yt-dlp");
        task.status_detail = "Retrying with yt-dlp extraction...";
        if (on_progress) {
            StreamProgress sp;
            sp.task_id = task.task_id;
            sp.phase = TaskState::kProbing;
            sp.percent = 0.0;
            sp.downloaded_bytes = 0;
            sp.total_bytes = 0;
            sp.status_line = task.status_detail;
            on_progress(sp);
        }
    }

    // 4. If cipher extraction, YouTube host, or site extraction is required (or direct fast-path failed), use external yt-dlp worker
    std::filesystem::path ytdlp_bin = PathSanitizer::FindWorkerBinary("yt-dlp", PathSanitizer::GetExecutableDir());
    bool should_use_ytdlp = std::filesystem::exists(ytdlp_bin, ec);
    if (should_use_ytdlp) {
        log_state_transition(TaskState::kProbing, "yt-dlp worker launch", "yt-dlp");
        task.status_detail = "Extracting YouTube stream with yt-dlp...";

        std::filesystem::path yt_cookies_file = task_tmp_dir / "yt_cookies.txt";
        bool has_cookies = false;
        std::string cookie_str;
        if (task.auth_headers.count("Cookie")) cookie_str = task.auth_headers.at("Cookie");
        else if (task.headers.count("Cookie")) cookie_str = task.headers.at("Cookie");
        else if (task.headers.count("cookie")) cookie_str = task.headers.at("cookie");
        if (!cookie_str.empty()) {
            auto ck_res = WriteNetscapeCookieFile(task_tmp_dir, "yt_cookies.txt", cookie_str, !watch_url.empty() ? watch_url : task.url);
            has_cookies = ck_res.ok();
        }

        std::string out_tmpl = (task_tmp_dir / ("output.%(ext)s")).string();
        std::string ytdlp_fmt = BuildYtDlpFormatSelector(req_filter, target_height, wants_audio_only, fmt);
        if (selected_itag > 0) {
            if (wants_audio_only) {
                ytdlp_fmt = std::to_string(selected_itag) + "/" + ytdlp_fmt;
            } else {
                ytdlp_fmt = std::to_string(selected_itag) + "+140/" +
                            std::to_string(selected_itag) + "+ba/" +
                            std::to_string(selected_itag) + "+251/" +
                            ytdlp_fmt;
            }
        }
        std::filesystem::path ffmpeg_path = m_cfg.ffmpeg_binary_path;
        if (ffmpeg_path.empty() || !std::filesystem::exists(ffmpeg_path, ec)) {
            ffmpeg_path = PathSanitizer::FindWorkerBinary("ffmpeg", PathSanitizer::GetExecutableDir());
        }

        auto cache_dir = PathSanitizer::GetDefaultAppDataDir() / "cache" / "yt-dlp";
        std::filesystem::create_directories(cache_dir, ec);
        auto target_solver_dir = cache_dir / "challenge-solver";
        if (!std::filesystem::exists(target_solver_dir / "lib.json", ec)) {
            std::filesystem::path bundled_solver = PathSanitizer::GetExecutableDir() / "tools" / "win-x64" / "challenge-solver";
            if (!std::filesystem::exists(bundled_solver, ec)) {
                bundled_solver = PathSanitizer::GetExecutableDir() / "tools" / "challenge-solver";
            }
            if (std::filesystem::exists(bundled_solver, ec)) {
                std::filesystem::create_directories(target_solver_dir, ec);
                std::filesystem::copy(bundled_solver, target_solver_dir, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
            }
        }

        std::vector<std::string> yt_args = {
            "--newline",
            "--no-playlist",
            "--no-warnings",
            "--cache-dir", cache_dir.string(),
            "--remote-components", "ejs:github",
            "-N", "8",
            "--concurrent-fragments", "8",
            "--retries", "3",
            "--fragment-retries", "3",
            "--file-access-retries", "3",
            "--socket-timeout", "15",
            "--extractor-args", "youtube:player_client=web_embedded",
            "-f", ytdlp_fmt,
            "--merge-output-format", fmt,
            "-o", out_tmpl
        };
        if (speed_limit_bps > 0) {
            uint64_t rate_k = std::max<uint64_t>(1, speed_limit_bps / 1024);
            yt_args.push_back("--limit-rate");
            yt_args.push_back(std::to_string(rate_k) + "K");
        }
        if (std::filesystem::exists(ffmpeg_path, ec)) {
            yt_args.push_back("--ffmpeg-location");
            yt_args.push_back(ffmpeg_path.string());
        }
        if (has_cookies && std::filesystem::exists(yt_cookies_file, ec)) {
            yt_args.push_back("--cookies");
            yt_args.push_back(yt_cookies_file.string());
        }
        std::filesystem::path node_bin = PathSanitizer::FindWorkerBinary("node", PathSanitizer::GetExecutableDir());
        if (std::filesystem::exists(node_bin, ec)) {
            yt_args.push_back("--js-runtimes");
            yt_args.push_back("node:" + node_bin.string());
        }
        yt_args.push_back(watch_url);

        // Immediate responsive status notification before process launch
        task.status_detail = "Extracting YouTube stream...";
        if (on_progress) {
            StreamProgress sp;
            sp.task_id = task.task_id;
            sp.phase = TaskState::kProbing;
            sp.percent = 0.0;
            sp.downloaded_bytes = 0;
            sp.total_bytes = task.total_bytes;
            sp.status_line = task.status_detail;
            on_progress(sp);
        }

        static const std::regex kYtDlpProgRe(R"(\[download\]\s+(\d+(?:\.\d+)?)%\s+of\s+~?\s*([0-9.]+)(KiB|MiB|GiB))");
        static const std::regex kYtDlpSpeedRe(R"(at\s+([0-9.]+)(B|KiB|MiB|GiB)/s)");
        static const std::regex kYtDlpDestRe(R"(\[download\]\s+Destination:\s+(.+))");

        struct YtTrackProgress {
            uint64_t dl_bytes{0};
            uint64_t tot_bytes{0};
            double pct{0.0};
        };
        std::map<std::string, YtTrackProgress> yt_tracks;
        std::string current_dest = "primary";
        std::string last_yt_err;

        auto parse_yt_line = [&](const std::string& line) {
            if (line.empty()) return;
            OPENIDM_LOG_INFO("YtDlpWorker", line);
            if (line.find("ERROR:") != std::string::npos ||
                line.find("error:") != std::string::npos ||
                (line.find("Sign in") != std::string::npos && line.find("bot") != std::string::npos) ||
                line.find("confirm you're not a bot") != std::string::npos ||
                line.find("Private video") != std::string::npos ||
                line.find("unavailable") != std::string::npos) {
                last_yt_err = line;
            } else if (last_yt_err.empty() && line.find("WARNING:") != std::string::npos) {
                last_yt_err = line;
            }

            // Real-time live status updates during extraction so UI never freezes on "Connecting..."
            if (line.find("Extracting URL") != std::string::npos || line.find("Downloading webpage") != std::string::npos) {
                task.state = TaskState::kProbing;
                task.status_detail = "Extracting YouTube stream...";
                if (on_progress) {
                    StreamProgress sp;
                    sp.task_id = task.task_id;
                    sp.phase = TaskState::kProbing;
                    sp.percent = 0.0;
                    sp.downloaded_bytes = 0;
                    sp.total_bytes = task.total_bytes;
                    sp.status_line = task.status_detail;
                    on_progress(sp);
                }
            } else if (line.find("Downloading player") != std::string::npos || line.find("client config") != std::string::npos) {
                task.state = TaskState::kProbing;
                task.status_detail = "Loading YouTube player config...";
                if (on_progress) {
                    StreamProgress sp;
                    sp.task_id = task.task_id;
                    sp.phase = TaskState::kProbing;
                    sp.percent = 0.0;
                    sp.downloaded_bytes = 0;
                    sp.total_bytes = task.total_bytes;
                    sp.status_line = task.status_detail;
                    on_progress(sp);
                }
            } else if (line.find("Solving JS") != std::string::npos || line.find("[jsc:") != std::string::npos) {
                task.state = TaskState::kProbing;
                task.status_detail = "Solving YouTube stream tokens...";
                if (on_progress) {
                    StreamProgress sp;
                    sp.task_id = task.task_id;
                    sp.phase = TaskState::kProbing;
                    sp.percent = 0.0;
                    sp.downloaded_bytes = 0;
                    sp.total_bytes = task.total_bytes;
                    sp.status_line = task.status_detail;
                    on_progress(sp);
                }
            } else if (line.find("player API JSON") != std::string::npos || line.find("Downloading m3u8") != std::string::npos) {
                task.state = TaskState::kProbing;
                task.status_detail = "Resolving video & audio streams...";
                if (on_progress) {
                    StreamProgress sp;
                    sp.task_id = task.task_id;
                    sp.phase = TaskState::kProbing;
                    sp.percent = 0.0;
                    sp.downloaded_bytes = 0;
                    sp.total_bytes = task.total_bytes;
                    sp.status_line = task.status_detail;
                    on_progress(sp);
                }
            }

            std::smatch dest_m;
            if (std::regex_search(line, dest_m, kYtDlpDestRe)) {
                std::string dest_fn = std::filesystem::path(dest_m[1].str()).filename().string();
                if (!dest_fn.empty()) {
                    current_dest = PathSanitizer::SanitizeFileName(dest_fn);
                }
                std::string track_label = (current_dest.find(".m4a") != std::string::npos ||
                                           current_dest.find(".opus") != std::string::npos ||
                                           current_dest.find(".f140") != std::string::npos ||
                                           current_dest.find(".f251") != std::string::npos)
                                           ? "audio stream (2/2)"
                                           : "video stream (1/2)";
                task.state = TaskState::kDownloading;
                task.status_detail = "Downloading YouTube " + track_label + "...";
                if (on_progress) {
                    StreamProgress sp;
                    sp.task_id = task.task_id;
                    sp.phase = TaskState::kDownloading;
                    sp.percent = task.stream_percent;
                    sp.downloaded_bytes = task.downloaded_bytes;
                    sp.total_bytes = task.total_bytes;
                    sp.speed_bps = task.speed_bps;
                    sp.status_line = task.status_detail;
                    on_progress(sp);
                }
            } else if (line.find("has already been downloaded") != std::string::npos) {
                if (!current_dest.empty() && yt_tracks.count(current_dest)) {
                    yt_tracks[current_dest].dl_bytes = yt_tracks[current_dest].tot_bytes;
                    yt_tracks[current_dest].pct = 100.0;
                }
            }

            if (line.find("[Merger]") != std::string::npos || line.find("Merging formats") != std::string::npos) {
                task.state = TaskState::kMuxing;
                task.status_detail = "Merging audio/video tracks (FFmpeg)...";
                task.stream_percent = 99.0;
                task.speed_bps = 0;
                if (on_progress) {
                    StreamProgress sp;
                    sp.task_id = task.task_id;
                    sp.phase = TaskState::kMuxing;
                    sp.percent = 99.0;
                    sp.downloaded_bytes = task.downloaded_bytes;
                    sp.total_bytes = task.total_bytes;
                    sp.speed_bps = 0;
                    sp.status_line = task.status_detail;
                    on_progress(sp);
                }
                return;
            }
            std::smatch m;
            if (std::regex_search(line, m, kYtDlpProgRe)) {
                double pct = std::stod(m[1].str());
                double sz_val = std::stod(m[2].str());
                std::string unit = m[3].str();
                double mult = 1048576.0;
                if (unit == "KiB") mult = 1024.0;
                else if (unit == "GiB") mult = 1073741824.0;
                uint64_t tot_b = static_cast<uint64_t>(sz_val * mult);
                uint64_t dl_b = static_cast<uint64_t>((pct / 100.0) * tot_b);

                yt_tracks[current_dest] = {dl_b, tot_b, pct};

                std::smatch sm;
                if (std::regex_search(line, sm, kYtDlpSpeedRe)) {
                    double sp_val = std::stod(sm[1].str());
                    std::string sp_unit = sm[2].str();
                    double sp_mult = 1.0;
                    if (sp_unit == "KiB") sp_mult = 1024.0;
                    else if (sp_unit == "MiB") sp_mult = 1048576.0;
                    else if (sp_unit == "GiB") sp_mult = 1073741824.0;
                    task.speed_bps = static_cast<uint64_t>(sp_val * sp_mult);
                }

                uint64_t aggregate_tot = 0;
                uint64_t aggregate_dl = 0;
                for (const auto& [dst, tr] : yt_tracks) {
                    aggregate_tot += tr.tot_bytes;
                    aggregate_dl += tr.dl_bytes;
                }

                uint64_t effective_tot = (yt_tracks.size() >= 2 || task.total_bytes == 0)
                    ? aggregate_tot
                    : std::max(task.total_bytes, aggregate_tot);

                double aggregate_pct = (effective_tot > 0)
                    ? std::min(99.0, (static_cast<double>(aggregate_dl) / static_cast<double>(effective_tot)) * 100.0)
                    : std::min(99.0, pct);

                std::string track_label = (current_dest.find(".m4a") != std::string::npos ||
                                           current_dest.find(".opus") != std::string::npos ||
                                           current_dest.find(".f140") != std::string::npos ||
                                           current_dest.find(".f251") != std::string::npos)
                                           ? "audio (2/2)"
                                           : "video (1/2)";

                task.state = TaskState::kDownloading;
                task.stream_percent = aggregate_pct;
                task.downloaded_bytes = aggregate_dl;
                task.total_bytes = effective_tot;
                task.status_detail = "Downloading YouTube " + track_label + " (" +
                                     std::to_string(static_cast<int>(aggregate_pct)) + "%)";

                if (on_progress) {
                    StreamProgress sp;
                    sp.task_id = task.task_id;
                    sp.phase = TaskState::kDownloading;
                    sp.percent = aggregate_pct;
                    sp.downloaded_bytes = task.downloaded_bytes;
                    sp.total_bytes = task.total_bytes;
                    sp.speed_bps = task.speed_bps;
                    sp.status_line = task.status_detail;
                    on_progress(sp);
                }
            }
        };

        auto run_ytdlp_proc = [&](const std::vector<std::string>& current_args) -> int {
            int exit_code = -1;
#ifndef _WIN32
            int pipefd[2];
            if (pipe(pipefd) == 0) {
                pid_t pid = fork();
                if (pid == 0) {
                    setpgid(0, 0);
                    ::close(pipefd[0]);
                    dup2(pipefd[1], STDOUT_FILENO);
                    dup2(pipefd[1], STDERR_FILENO);
                    if (pipefd[1] > 2) ::close(pipefd[1]);
                    std::string bin_s = ytdlp_bin.string();
                    std::vector<char*> argv_ptrs;
                    argv_ptrs.push_back(const_cast<char*>(bin_s.c_str()));
                    for (const auto& a : current_args) argv_ptrs.push_back(const_cast<char*>(a.c_str()));
                    argv_ptrs.push_back(nullptr);
                    execvp(bin_s.c_str(), argv_ptrs.data());
                    _exit(127);
                }
                ::close(pipefd[1]);
                std::string line_buf;
                char rbuf[4096];
                ssize_t n = 0;
                while ((n = ::read(pipefd[0], rbuf, sizeof(rbuf))) > 0) {
                    if (cancel_requested.load() || pause_requested.load()) {
                        kill(pid, SIGTERM);
                        break;
                    }
                    line_buf.append(rbuf, static_cast<size_t>(n));
                    size_t nl;
                    while ((nl = line_buf.find_first_of("\r\n")) != std::string::npos) {
                        parse_yt_line(line_buf.substr(0, nl));
                        line_buf.erase(0, nl + 1);
                    }
                }
                if (!line_buf.empty()) parse_yt_line(line_buf);
                ::close(pipefd[0]);
                int st = 0;
                waitpid(pid, &st, 0);
                exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
            }
#else
            auto to_wide = [](std::string_view sv) {
                if (sv.empty()) return std::wstring();
                int sz = MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), nullptr, 0);
                std::wstring w(sz, 0);
                MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), w.data(), sz);
                return w;
            };
            auto to_utf8 = [](std::wstring_view wv) {
                if (wv.empty()) return std::string();
                int sz = WideCharToMultiByte(CP_UTF8, 0, wv.data(), static_cast<int>(wv.size()), nullptr, 0, nullptr, nullptr);
                std::string s(sz, 0);
                WideCharToMultiByte(CP_UTF8, 0, wv.data(), static_cast<int>(wv.size()), s.data(), sz, nullptr, nullptr);
                return s;
            };
            SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
            HANDLE hReadPipe = nullptr, hWritePipe = nullptr;
            if (CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
                SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);
                STARTUPINFOW si{};
                si.cb = sizeof(si);
                si.dwFlags = STARTF_USESTDHANDLES;
                si.hStdOutput = hWritePipe;
                si.hStdError = hWritePipe;
                si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

                std::wostringstream wcmd_ss;
                wcmd_ss << L"\"" << ytdlp_bin.wstring() << L"\"";
                for (const auto& a : current_args) wcmd_ss << L" \"" << to_wide(a) << L"\"";
                std::wstring wcmd = wcmd_ss.str();
                OPENIDM_LOG_INFO("Nm3u8dlAdapter", "Launching yt-dlp: " + to_utf8(wcmd));
                PROCESS_INFORMATION pi{};
                if (CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                    CloseHandle(hWritePipe);
                    std::string line_buf;
                    char rbuf[4096];
                    DWORD got = 0;
                    while (ReadFile(hReadPipe, rbuf, sizeof(rbuf), &got, nullptr) && got > 0) {
                        if (cancel_requested.load() || pause_requested.load()) {
                            TerminateProcess(pi.hProcess, 1);
                            break;
                        }
                        line_buf.append(rbuf, got);
                        size_t nl;
                        while ((nl = line_buf.find_first_of("\r\n")) != std::string::npos) {
                            parse_yt_line(line_buf.substr(0, nl));
                            line_buf.erase(0, nl + 1);
                        }
                    }
                    WaitForSingleObject(pi.hProcess, 300000);
                    DWORD wcode = 1;
                    GetExitCodeProcess(pi.hProcess, &wcode);
                    if (wcode == STILL_ACTIVE) {
                        TerminateProcess(pi.hProcess, 1);
                        wcode = 259;
                    }
                    exit_code = static_cast<int>(wcode);
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                } else {
                    CloseHandle(hWritePipe);
                }
                CloseHandle(hReadPipe);
            }
#endif
            return exit_code;
        };

        auto find_output_file = [&]() -> std::filesystem::path {
            std::filesystem::path p = task_tmp_dir / ("output." + fmt);
            if (std::filesystem::exists(p, ec) && std::filesystem::file_size(p, ec) > 0) return p;
            for (const auto& entry : std::filesystem::directory_iterator(task_tmp_dir, ec)) {
                if (entry.is_regular_file() && entry.path().stem() == "output" && std::filesystem::file_size(entry.path(), ec) > 0) {
                    return entry.path();
                }
            }
            for (const auto& entry : std::filesystem::directory_iterator(task_tmp_dir, ec)) {
                if (entry.is_regular_file() && entry.path().filename() != "yt_config.txt" &&
                    entry.path().filename() != "yt_cookies.txt" &&
                    !entry.path().extension().string().ends_with(".part") &&
                    !entry.path().extension().string().ends_with(".ytdl") &&
                    std::filesystem::file_size(entry.path(), ec) > 0) {
                    return entry.path();
                }
            }
            return {};
        };

        int yt_exit = run_ytdlp_proc(yt_args);
        std::filesystem::path actual_output = find_output_file();
        bool has_output = !actual_output.empty();

        // Fallback retry 1: If extraction failed and cookies were used, retry without cookies
        if ((yt_exit != 0 || !has_output) && !cancel_requested.load() && !pause_requested.load() && has_cookies) {
            OPENIDM_LOG_WARN("Nm3u8dlAdapter", "yt-dlp failed with cookies (exit=" + std::to_string(yt_exit) +
                             "), retrying without cookies: " + last_yt_err);
            std::vector<std::string> no_cookie_args;
            for (size_t i = 0; i < yt_args.size(); ++i) {
                if (yt_args[i] == "--cookies") {
                    ++i;
                    continue;
                }
                no_cookie_args.push_back(yt_args[i]);
            }
            yt_tracks.clear();
            last_yt_err.clear();
            yt_exit = run_ytdlp_proc(no_cookie_args);
            actual_output = find_output_file();
            has_output = !actual_output.empty();
        }

        // Fallback retry 2: If requested format failed or was throttled, retry with clean arguments
        // BUT NEVER downgrade to 360p (format 18) when a specific resolution was requested!
        if ((yt_exit != 0 || !has_output) && !cancel_requested.load() && !pause_requested.load()) {
            std::string retry_fmt = ytdlp_fmt;

            OPENIDM_LOG_WARN("Nm3u8dlAdapter", "yt-dlp initial attempt failed (exit=" + std::to_string(yt_exit) +
                             "), retrying YouTube download for format: " + retry_fmt);
            std::vector<std::string> fallback_args = {
                "--newline",
                "--no-playlist",
                "--no-warnings",
                "--cache-dir", cache_dir.string(),
                "--remote-components", "ejs:github",
                "-N", "4",
                "--concurrent-fragments", "4",
                "--retries", "3",
                "--fragment-retries", "3",
                "--file-access-retries", "3",
                "--socket-timeout", "20",
                "--extractor-args", "youtube:player_client=web_embedded,mweb",
                "-f", retry_fmt,
                "--merge-output-format", fmt,
                "-o", out_tmpl
            };
            if (std::filesystem::exists(ffmpeg_path, ec)) {
                fallback_args.push_back("--ffmpeg-location");
                fallback_args.push_back(ffmpeg_path.string());
            }
            if (std::filesystem::exists(node_bin, ec)) {
                fallback_args.push_back("--js-runtimes");
                fallback_args.push_back("node:" + node_bin.string());
            }
            fallback_args.push_back(watch_url);

            yt_tracks.clear();
            last_yt_err.clear();
            yt_exit = run_ytdlp_proc(fallback_args);
            actual_output = find_output_file();
            has_output = !actual_output.empty();
        }

        if (std::filesystem::exists(yt_cookies_file, ec)) {
            std::ofstream cfs(yt_cookies_file, std::ios::binary | std::ios::trunc);
            cfs << std::string(512, '\0');
            cfs.close();
            std::filesystem::remove(yt_cookies_file, ec);
        }

        if (cancel_requested.load()) {
            std::filesystem::remove_all(task_tmp_dir, ec);
            task.state = TaskState::kCanceled;
            task.status_detail = "Canceled";
            return Result<std::filesystem::path>::Err("ERR_CANCELED", "YouTube download canceled by user");
        }
        if (pause_requested.load()) {
            task.state = TaskState::kPaused;
            task.status_detail = "Paused";
            return Result<std::filesystem::path>::Err("ERR_PAUSED", "YouTube download paused by user");
        }

        std::string detected_fmt = fmt;
        std::string ext_str = actual_output.extension().string();
        if (!ext_str.empty() && ext_str.front() == '.') ext_str = ext_str.substr(1);
        std::string lower_ext = ToLowerStr(ext_str);
        if (lower_ext == "mp4" || lower_ext == "mkv" || lower_ext == "webm" || lower_ext == "m4a" || lower_ext == "mov") {
            detected_fmt = lower_ext;
        }

        has_output = std::filesystem::exists(actual_output, ec) && std::filesystem::file_size(actual_output, ec) > 0;
        bool valid_container = false;
        if (has_output) {
            valid_container = VerifyContainerMagicBytes(actual_output, detected_fmt);
            if (!valid_container) {
                for (const auto& alt_fmt : {"mp4", "mkv", "webm", "m4a"}) {
                    if (VerifyContainerMagicBytes(actual_output, alt_fmt)) {
                        detected_fmt = alt_fmt;
                        valid_container = true;
                        break;
                    }
                }
            }
        }

        if ((yt_exit == 0 || has_output) && has_output && valid_container) {
            task.state = TaskState::kVerifying;
            task.status_detail = "Verifying downloaded media quality...";
            task.stream_percent = 99.5;
            if (on_progress) {
                StreamProgress sp;
                sp.task_id = task.task_id;
                sp.phase = TaskState::kVerifying;
                sp.percent = 99.5;
                sp.downloaded_bytes = task.downloaded_bytes;
                sp.total_bytes = task.total_bytes;
                sp.status_line = task.status_detail;
                on_progress(sp);
            }

            MediaQualityExpectations expectations;
            expectations.expected_height = target_height;
            expectations.expected_container = detected_fmt;
            expectations.expect_video = !wants_audio_only;
            expectations.expect_audio = true;
            expectations.expected_min_bytes = 1024;

            auto quality_res = VerifyMediaQuality(actual_output, expectations, m_cfg.ffprobe_binary_path);
            if (!quality_res.ok()) {
                std::filesystem::remove(actual_output, ec);
                std::filesystem::remove_all(task_tmp_dir, ec);
                task.state = TaskState::kFailed;
                task.error_code = quality_res.error->code;
                task.error_message = quality_res.error->message;
                task.status_detail = task.error_message;
                return Result<std::filesystem::path>::Err(task.error_code, task.error_message, false, true);
            }

            task.state = TaskState::kFinalizing;
            task.status_detail = "Finalizing output file...";
            task.stream_percent = 99.9;
            if (on_progress) {
                StreamProgress sp;
                sp.task_id = task.task_id;
                sp.phase = TaskState::kFinalizing;
                sp.percent = 99.9;
                sp.downloaded_bytes = quality_res.value->probed_file_size;
                sp.total_bytes = quality_res.value->probed_file_size;
                sp.status_line = task.status_detail;
                on_progress(sp);
            }

            std::string desired_final_name = base_stem + "." + detected_fmt;
            auto commit_res = PathSanitizer::CommitPartFile(actual_output, task.save_dir, desired_final_name, task.conflict_policy);
            std::filesystem::remove_all(task_tmp_dir, ec);
            if (!commit_res.ok()) {
                task.state = TaskState::kFailed;
                task.error_code = commit_res.error->code;
                task.error_message = commit_res.error->message;
                task.status_detail = task.error_message;
                return commit_res;
            }
            uint64_t disk_sz = std::filesystem::file_size(*commit_res.value, ec);
            if (!ec && disk_sz > 0) {
                task.downloaded_bytes = disk_sz;
                task.total_bytes = disk_sz;
            } else {
                task.downloaded_bytes = quality_res.value->probed_file_size;
                task.total_bytes = quality_res.value->probed_file_size;
            }
            task.file_name = commit_res.value->filename().string();
            task.part_path.clear();
            task.stream_percent = 100.0;
            log_state_transition(TaskState::kCompleted, "verified output committed", "yt-dlp");
            task.status_detail = "Completed";
            task.completed_at_ms = NowEpochMs();
            task.error_code.clear();
            task.error_message.clear();
            task.auth_headers.clear();
            if (on_progress) {
                StreamProgress sp;
                sp.task_id = task.task_id;
                sp.phase = TaskState::kCompleted;
                sp.percent = 100.0;
                sp.downloaded_bytes = task.downloaded_bytes;
                sp.total_bytes = task.total_bytes;
                sp.status_line = task.status_detail;
                on_progress(sp);
            }
            return commit_res;
        }

        std::filesystem::remove_all(task_tmp_dir, ec);
        task.state = TaskState::kFailed;
        if (yt_exit == 0 && !has_output) {
            task.error_code = "ERR_STREAM_OUTPUT_MISSING";
            task.error_message = "yt-dlp exited with code 0 but no output file was created in temporary directory";
        } else if (yt_exit == 0 && has_output && !valid_container) {
            task.error_code = "ERR_STREAM_CONTAINER_INVALID";
            task.error_message = "Output container validation failed for " + actual_output.filename().string() + ": invalid or unrecognized container magic bytes";
        } else if (last_yt_err.find("DRM") != std::string::npos || last_yt_err.find("protected") != std::string::npos) {
            task.error_code = "ERR_STREAM_DRM_PROTECTED";
            task.error_message = "Stream uses unsupported DRM protection. Flux does not circumvent DRM.";
        } else if (last_yt_err.find("Requested format is not available") != std::string::npos ||
                   last_yt_err.find("format is not available") != std::string::npos) {
            task.error_code = "ERR_STREAM_FORMAT_UNAVAILABLE";
            task.error_message = "Requested video quality (" + (target_height > 0 ? std::to_string(target_height) + "p" : req_filter) +
                                 ") is not available for this stream";
        } else if (last_yt_err.find("Sign in") != std::string::npos || last_yt_err.find("bot") != std::string::npos) {
            task.error_code = "ERR_YOUTUBE_BOT_DETECTION";
            task.error_message = "YouTube bot detection triggered. Try exporting cookies or signing in.";
        } else if (last_yt_err.find("Private video") != std::string::npos || last_yt_err.find("members-only") != std::string::npos) {
            task.error_code = "ERR_YOUTUBE_PRIVATE_VIDEO";
            task.error_message = "This YouTube video is private or members-only.";
        } else if (last_yt_err.find("403") != std::string::npos || last_yt_err.find("Forbidden") != std::string::npos) {
            task.error_code = "ERR_YOUTUBE_EXTRACTION_FAILED";
            task.error_message = "YouTube extraction failed: 403 Forbidden. " + last_yt_err;
        } else if (is_youtube_host) {
            task.error_code = "ERR_YOUTUBE_EXTRACTION_FAILED";
            task.error_message = last_yt_err.empty() ? ("yt-dlp exited with code " + std::to_string(yt_exit)) : last_yt_err;
        } else {
            task.error_code = "ERR_STREAM_WORKER_FAILED";
            task.error_message = last_yt_err.empty() ? ("Stream worker exited with code " + std::to_string(yt_exit)) : last_yt_err;
        }
        task.status_detail = task.error_message;
        return Result<std::filesystem::path>::Err(task.error_code, task.error_message);
    }

    return execute_direct_download();
}

} // namespace openidm::stream
