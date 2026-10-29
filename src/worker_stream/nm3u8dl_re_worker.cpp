// SPDX-License-Identifier: Apache-2.0
// OpenIDM — Native C++20 Stream Worker Executable (N_m3u8DL-RE Compatible + YouTube/Site Extractor)
// Supports:
//   - @args.rsp response file parsing with quoted arguments
//   - HLS (#EXTM3U Master & Media playlists, #EXT-X-MEDIA Audio/Subtitles, #EXT-X-MAP, #EXT-X-KEY AES-128)
//   - MPEG-DASH (<MPD> AdaptationSet, Representation, SegmentTemplate, SegmentTimeline, BaseURL)
//   - YouTube & Site Extraction (YOUTUBE_STREAM descriptors, ytInitialPlayerResponse HTML/JSON extraction,
//     separate adaptive Video+Audio stream downloading & FFmpeg muxing, and external yt-dlp worker delegation)
//   - DRM detection (SAMPLE-AES, CENC, Widevine, PlayReady, FairPlay) & meta.json emission
//   - Multi-threaded HTTP/HTTPS segment downloading via OpenIDM's HttpClient
//   - Native in-process AES-128-CBC segment decryption (OpenSSL EVP / Windows CNG BCrypt)
//   - FFmpeg container muxing (-M format=mp4|mkv:muxer=ffmpeg)

#include "brisk_engine/http_client.hpp"
#include "common/json_mini.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <openssl/evp.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

struct WorkerOptions {
    std::string input;
    std::string base_url;
    std::filesystem::path tmp_dir{"./tmp"};
    std::filesystem::path save_dir{"."};
    std::string save_name{"output"};
    uint32_t thread_count{8};
    uint32_t download_retry_count{5};
    uint32_t http_request_timeout_ms{30000};
    uint64_t max_speed_kbps{0};
    bool skip_download{false};
    bool write_meta_json{true};
    bool del_after_done{true};
    std::string sub_format{"SRT"};
    std::string ffmpeg_binary_path{"ffmpeg"};
    std::string select_video{"best"};
    std::string select_audio{"all"};
    std::string select_subtitle{"all"};
    std::string mux_format{"mp4"};
    bool drop_video{false};
    bool drop_audio{false};
    bool allow_loopback{false};
    std::string custom_hls_method;
    std::string custom_hls_key;
    std::map<std::string, std::string> headers;
    uint32_t live_record_limit_sec{0};
    bool live_real_time_merge{false};
    bool live_keep_segments{false};
};

struct SegmentItem {
    uint32_t index{0};
    std::string url;
    double duration{2.0};
    bool encrypted{false};
    std::string method{"NONE"};
    std::string key_uri;
    std::string iv_hex;
};

struct TrackItem {
    std::string id;
    std::string media_type{"VIDEO"}; // VIDEO, AUDIO, SUBTITLES
    std::string group_id;
    std::string language{"und"};
    std::string name;
    std::string codecs{"avc1.64001f"};
    std::string resolution;
    uint64_t bandwidth{1000000};
    double frame_rate{30.0};
    double duration_sec{0.0};
    bool encrypted{false};
    std::string encryption_method{"NONE"};
    std::string playlist_url;
    std::string init_url;
    std::vector<SegmentItem> segments;
};

struct ManifestMeta {
    std::string url;
    bool is_live{false};
    bool drm_protected{false};
    std::string drm_system;
    std::vector<TrackItem> tracks;
};

std::string Trim(std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
        sv.remove_suffix(1);
    }
    return std::string(sv);
}

std::string ToLower(std::string_view sv) {
    std::string s(sv);
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string ToUpper(std::string_view sv) {
    std::string s(sv);
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
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

std::vector<std::string> SplitShellLine(const std::string& line) {
    std::vector<std::string> tokens;
    std::string cur;
    bool in_quotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '\\' && i + 1 < line.size() && (line[i + 1] == '"' || line[i + 1] == '\\')) {
            cur.push_back(line[++i]);
        } else if (c == '"') {
            in_quotes = !in_quotes;
        } else if (!in_quotes && (c == ' ' || c == '\t')) {
            if (!cur.empty()) {
                tokens.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

WorkerOptions ParseWorkerArgs(int argc, char* argv[]) {
    std::vector<std::string> expanded;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.size() > 1 && arg.front() == '@') {
            std::filesystem::path rsp_path(arg.substr(1));
            std::ifstream ifs(rsp_path);
            if (!ifs) {
                std::cerr << "ERROR: Response file not found: " << rsp_path.string() << "\n";
                std::exit(2);
            }
            std::string ln;
            while (std::getline(ifs, ln)) {
                ln = Trim(ln);
                if (ln.empty() || ln.front() == '#') continue;
                auto toks = SplitShellLine(ln);
                expanded.insert(expanded.end(), toks.begin(), toks.end());
            }
        } else {
            expanded.push_back(arg);
        }
    }

    for (const auto& tok : expanded) {
        if (tok == "--version" || tok == "-v") {
            std::cout << "N_m3u8DL-RE (Radium Stream Worker v0.3.0 / N_m3u8DL-RE v0.3.0-beta CLI contract)\n";
            std::exit(0);
        }
        if (tok == "--help" || tok == "-h") {
            std::cout << "N_m3u8DL-RE v0.3.0 --auto-select --ffmpeg-binary-path --skip-download --write-meta-json\n";
            std::exit(0);
        }
    }

    WorkerOptions opts;
    for (size_t i = 0; i < expanded.size();) {
        const std::string& tok = expanded[i];
        if (tok == "--no-config" || tok == "--no-ansi-color" || tok == "--disable-update-check" ||
            tok == "-mt" || tok == "--concurrent-download" || tok == "--no-log" || tok == "--auto-select") {
            ++i;
        } else if (tok == "--skip-download") {
            opts.skip_download = true;
            ++i;
        } else if (tok == "--write-meta-json") {
            opts.write_meta_json = true;
            ++i;
        } else if (tok == "--del-after-done") {
            if (i + 1 < expanded.size() && (ToLower(expanded[i + 1]) == "true" || ToLower(expanded[i + 1]) == "false")) {
                opts.del_after_done = (ToLower(expanded[i + 1]) == "true");
                i += 2;
            } else {
                opts.del_after_done = true;
                ++i;
            }
        } else if (tok == "--vod-select-parts" || tok == "--check-segments-count" || tok == "--auto-subtitle-fix") {
            if (i + 1 < expanded.size() && (ToLower(expanded[i + 1]) == "true" || ToLower(expanded[i + 1]) == "false")) {
                i += 2;
            } else {
                ++i;
            }
        } else if (tok == "--download-retry-count" && i + 1 < expanded.size()) {
            try { opts.download_retry_count = static_cast<uint32_t>(std::stoul(expanded[i + 1])); } catch (...) {}
            i += 2;
        } else if (tok == "--http-request-timeout" && i + 1 < expanded.size()) {
            try {
                uint32_t sec = static_cast<uint32_t>(std::stoul(expanded[i + 1]));
                opts.http_request_timeout_ms = sec > 0 ? (sec * 1000) : 30000;
            } catch (...) {}
            i += 2;
        } else if ((tok == "-R" || tok == "--max-speed") && i + 1 < expanded.size()) {
            std::string val = expanded[i + 1];
            uint64_t multiplier = 1;
            if (!val.empty() && (val.back() == 'k' || val.back() == 'K')) {
                multiplier = 1000;
                val.pop_back();
            } else if (!val.empty() && (val.back() == 'm' || val.back() == 'M')) {
                multiplier = 1000000;
                val.pop_back();
            }
            try { opts.max_speed_kbps = (std::stoull(val) * multiplier) / 1000; } catch (...) {}
            i += 2;
        } else if (tok == "--sub-format" && i + 1 < expanded.size()) {
            opts.sub_format = ToUpper(expanded[i + 1]);
            i += 2;
        } else if (tok == "--live-record-limit" && i + 1 < expanded.size()) {
            std::string val = expanded[i + 1];
            uint32_t limit_sec = 0;
            std::vector<std::string> parts;
            std::stringstream pss(val);
            std::string pitem;
            while (std::getline(pss, pitem, ':')) {
                parts.push_back(pitem);
            }
            if (parts.size() == 3) {
                try {
                    limit_sec = std::stoul(parts[0]) * 3600 + std::stoul(parts[1]) * 60 + std::stoul(parts[2]);
                } catch (...) {}
            } else if (parts.size() == 2) {
                try {
                    limit_sec = std::stoul(parts[0]) * 60 + std::stoul(parts[1]);
                } catch (...) {}
            } else if (!parts.empty()) {
                try {
                    limit_sec = std::stoul(parts[0]);
                } catch (...) {}
            }
            opts.live_record_limit_sec = limit_sec;
            i += 2;
        } else if (tok == "--live-real-time-merge") {
            opts.live_real_time_merge = true;
            ++i;
        } else if (tok == "--live-keep-segments") {
            opts.live_keep_segments = true;
            ++i;
        } else if (tok == "--live-pipe-mux") {
            ++i;
        } else if (tok == "--ui-language") {
            i += 2;
        } else if (tok == "--base-url" && i + 1 < expanded.size()) {
            opts.base_url = expanded[i + 1];
            i += 2;
        } else if (tok == "--tmp-dir" && i + 1 < expanded.size()) {
            opts.tmp_dir = expanded[i + 1];
            i += 2;
        } else if (tok == "--save-dir" && i + 1 < expanded.size()) {
            opts.save_dir = expanded[i + 1];
            i += 2;
        } else if (tok == "--save-name" && i + 1 < expanded.size()) {
            opts.save_name = expanded[i + 1];
            i += 2;
        } else if (tok == "--thread-count" && i + 1 < expanded.size()) {
            opts.thread_count = static_cast<uint32_t>(std::stoul(expanded[i + 1]));
            i += 2;
        } else if (tok == "--ffmpeg-binary-path" && i + 1 < expanded.size()) {
            opts.ffmpeg_binary_path = expanded[i + 1];
            i += 2;
        } else if ((tok == "-sv" || tok == "--select-video") && i + 1 < expanded.size()) {
            opts.select_video = expanded[i + 1];
            i += 2;
        } else if ((tok == "-sa" || tok == "--select-audio") && i + 1 < expanded.size()) {
            opts.select_audio = expanded[i + 1];
            i += 2;
        } else if ((tok == "-ss" || tok == "--select-subtitle") && i + 1 < expanded.size()) {
            opts.select_subtitle = expanded[i + 1];
            i += 2;
        } else if (tok == "--custom-hls-method" && i + 1 < expanded.size()) {
            opts.custom_hls_method = expanded[i + 1];
            i += 2;
        } else if (tok == "--custom-hls-key" && i + 1 < expanded.size()) {
            opts.custom_hls_key = expanded[i + 1];
            i += 2;
        } else if ((tok == "-M" || tok == "--mux-after-done") && i + 1 < expanded.size()) {
            std::string mux_s = expanded[i + 1];
            if (mux_s.find("format=mkv") != std::string::npos) opts.mux_format = "mkv";
            else if (mux_s.find("format=m4a") != std::string::npos) opts.mux_format = "m4a";
            else if (mux_s.find("format=mp3") != std::string::npos) opts.mux_format = "mp3";
            else if (mux_s.find("format=webm") != std::string::npos) opts.mux_format = "webm";
            else if (mux_s.find("format=ts") != std::string::npos) opts.mux_format = "ts";
            else opts.mux_format = "mp4";

            auto bp_pos = mux_s.find("bin_path=");
            if (bp_pos != std::string::npos) {
                std::string bp = mux_s.substr(bp_pos + 9);
                if (!bp.empty() && bp.front() == '"') {
                    bp.erase(0, 1);
                    auto qend = bp.find('"');
                    if (qend != std::string::npos) bp = bp.substr(0, qend);
                } else {
                    auto kpos = bp.find(":keep=");
                    if (kpos != std::string::npos) bp = bp.substr(0, kpos);
                }
                std::string unescaped;
                for (size_t k = 0; k < bp.size(); ++k) {
                    if (bp[k] == '\\' && k + 1 < bp.size() && (bp[k + 1] == ':' || bp[k + 1] == '\\')) {
                        unescaped.push_back(bp[++k]);
                    } else {
                        unescaped.push_back(bp[k]);
                    }
                }
                std::error_code ec;
                if (!unescaped.empty() && std::filesystem::exists(unescaped, ec)) {
                    opts.ffmpeg_binary_path = unescaped;
                }
            }
            i += 2;
        } else if (tok == "--drop-video") {
            opts.drop_video = true;
            ++i;
        } else if (tok == "--drop-audio") {
            opts.drop_audio = true;
            ++i;
        } else if (tok == "--allow-loopback") {
            opts.allow_loopback = true;
            ++i;
        } else if ((tok == "-H" || tok == "--header") && i + 1 < expanded.size()) {
            std::string hdr = expanded[i + 1];
            auto colon = hdr.find(':');
            if (colon != std::string::npos) {
                opts.headers[Trim(hdr.substr(0, colon))] = Trim(hdr.substr(colon + 1));
            }
            i += 2;
        } else if (!tok.empty() && tok.front() != '-' && opts.input.empty()) {
            opts.input = tok;
            ++i;
        } else {
            ++i;
        }
    }

    const char* env_radium = std::getenv("RADIUM_ALLOW_LOOPBACK");
    const char* env_lb1 = std::getenv("FLUX_ALLOW_LOOPBACK");
    const char* env_lb2 = std::getenv("OPENIDM_ALLOW_LOOPBACK");
    if ((env_radium && std::string(env_radium) == "1") ||
        (env_lb1 && std::string(env_lb1) == "1") ||
        (env_lb2 && std::string(env_lb2) == "1")) {
        opts.allow_loopback = true;
    }

    return opts;
}

std::string ResolveUrl(const std::string& base_url, const std::string& rel) {
    if (rel.rfind("http://", 0) == 0 || rel.rfind("https://", 0) == 0) {
        return rel;
    }
    auto qpos = base_url.find('?');
    std::string clean_base = (qpos == std::string::npos) ? base_url : base_url.substr(0, qpos);
    auto scheme_pos = clean_base.find("://");
    if (scheme_pos == std::string::npos) {
        std::filesystem::path bp(clean_base);
        return (bp.parent_path() / rel).string();
    }
    auto first_slash = clean_base.find('/', scheme_pos + 3);
    std::string origin = (first_slash == std::string::npos) ? clean_base : clean_base.substr(0, first_slash);
    if (!rel.empty() && rel.front() == '/') {
        return origin + rel;
    }
    auto last_slash = clean_base.find_last_of('/');
    if (last_slash != std::string::npos && last_slash >= scheme_pos + 3) {
        return clean_base.substr(0, last_slash + 1) + rel;
    }
    return origin + "/" + rel;
}

bool FetchBytes(
    const std::string& url_or_path,
    const std::map<std::string, std::string>& headers,
    std::vector<uint8_t>& out_bytes,
    std::string& out_resolved_url,
    bool allow_loopback = false,
    bool is_manifest_input = false,
    uint32_t timeout_ms = 30000) {

    std::error_code ec;
    std::filesystem::path p(url_or_path);
    if (is_manifest_input && std::filesystem::exists(p, ec) && std::filesystem::is_regular_file(p, ec)) {
        std::ifstream ifs(p, std::ios::binary);
        if (!ifs) return false;
        out_bytes.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        out_resolved_url = std::filesystem::weakly_canonical(p, ec).string();
        return true;
    }

    out_bytes.clear();
    std::atomic<bool> cancel{false};
    auto res = openidm::brisk::HttpClient::PerformGet(
        url_or_path,
        headers,
        std::nullopt,
        std::nullopt,
        timeout_ms,
        allow_loopback,
        cancel,
        [&](const openidm::brisk::HttpResponseMeta& meta) -> bool {
            out_resolved_url = meta.final_url;
            return true;
        },
        [&](const uint8_t* data, size_t len) -> bool {
            out_bytes.insert(out_bytes.end(), data, data + len);
            return true;
        });
    return res.ok();
}

bool FetchBytesWithRetry(
    const std::string& url_or_path,
    const std::map<std::string, std::string>& headers,
    std::vector<uint8_t>& out_bytes,
    std::string& out_resolved_url,
    uint32_t retry_count = 3,
    uint32_t timeout_ms = 30000,
    bool allow_loopback = false,
    bool is_manifest_input = false,
    const std::atomic<bool>* cancel_flag = nullptr) {

    for (uint32_t attempt = 0; attempt <= retry_count; ++attempt) {
        if (cancel_flag && cancel_flag->load()) return false;
        if (attempt > 0) {
            uint32_t backoff = std::min<uint32_t>(3000, 200 * (1 << (attempt - 1)));
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
        }
        if (FetchBytes(url_or_path, headers, out_bytes, out_resolved_url, allow_loopback, is_manifest_input, timeout_ms)) {
            return true;
        }
    }
    return false;
}

std::map<std::string, std::string> ParseM3u8Attributes(const std::string& attr_line) {
    std::map<std::string, std::string> attrs;
    static const std::regex kAttrRe(R"(([A-Z0-9-]+)=("[^"]*"|[^,]*))");
    auto begin = std::sregex_iterator(attr_line.begin(), attr_line.end(), kAttrRe);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        std::string k = (*it)[1].str();
        std::string v = (*it)[2].str();
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
            v = v.substr(1, v.size() - 2);
        }
        attrs[k] = v;
    }
    return attrs;
}

void ParseHlsMediaPlaylist(const std::string& text, const std::string& playlist_url, TrackItem& track, bool& out_is_live) {
    out_is_live = (text.find("#EXT-X-ENDLIST") == std::string::npos);
    bool encrypted = false;
    std::string enc_method = "NONE";
    std::string key_uri;
    std::string key_iv;
    uint64_t media_sequence = 0;
    double total_dur = 0.0;
    double cur_dur = 2.0;

    std::istringstream iss(text);
    std::string raw_line;
    while (std::getline(iss, raw_line)) {
        std::string ln = Trim(raw_line);
        if (ln.empty()) continue;

        if (ln.rfind("#EXT-X-MEDIA-SEQUENCE:", 0) == 0) {
            try {
                media_sequence = std::stoull(ln.substr(22));
            } catch (...) {
                media_sequence = 0;
            }
        } else if (ln.rfind("#EXT-X-MAP:", 0) == 0) {
            auto attrs = ParseM3u8Attributes(ln.substr(11));
            if (attrs.count("URI")) {
                track.init_url = ResolveUrl(playlist_url, attrs["URI"]);
            }
        } else if (ln.rfind("#EXT-X-KEY:", 0) == 0) {
            auto attrs = ParseM3u8Attributes(ln.substr(11));
            std::string m = ToUpper(attrs.count("METHOD") ? attrs["METHOD"] : "NONE");
            std::replace(m.begin(), m.end(), '-', '_');
            if (m != "NONE") {
                encrypted = true;
                enc_method = m;
                if (attrs.count("URI")) key_uri = ResolveUrl(playlist_url, attrs["URI"]);
                if (attrs.count("IV")) key_iv = attrs["IV"];
            } else {
                encrypted = false;
                enc_method = "NONE";
            }
        } else if (ln.rfind("#EXTINF:", 0) == 0) {
            std::string val = ln.substr(8);
            auto comma = val.find(',');
            if (comma != std::string::npos) val = val.substr(0, comma);
            try { cur_dur = std::stod(val); } catch (...) { cur_dur = 2.0; }
        } else if (ln.front() != '#') {
            SegmentItem seg;
            uint32_t seg_idx = static_cast<uint32_t>(track.segments.size());
            seg.index = seg_idx;
            seg.url = ResolveUrl(playlist_url, ln);
            seg.duration = cur_dur;
            seg.encrypted = encrypted;
            seg.method = enc_method;
            seg.key_uri = key_uri;
            if (encrypted && enc_method == "AES_128") {
                if (!key_iv.empty()) {
                    seg.iv_hex = key_iv;
                } else {
                    uint64_t seq_num = media_sequence + seg_idx;
                    char iv_buf[33];
                    std::snprintf(iv_buf, sizeof(iv_buf), "0000000000000000%016llX", static_cast<unsigned long long>(seq_num));
                    seg.iv_hex = iv_buf;
                }
            } else {
                seg.iv_hex = key_iv;
            }
            track.segments.push_back(std::move(seg));
            total_dur += cur_dur;
        }
    }
    track.duration_sec = total_dur;
    track.encrypted = encrypted;
    track.encryption_method = enc_method;
}

double ParseIso8601DurationSec(const std::string& str) {
    if (str.empty() || str[0] != 'P') return 0.0;
    double total_sec = 0.0;
    double cur_val = 0.0;
    bool in_time = false;
    for (size_t i = 1; i < str.size(); ++i) {
        char c = str[i];
        if (c == 'T') {
            in_time = true;
        } else if ((c >= '0' && c <= '9') || c == '.') {
            size_t end_idx = i;
            while (end_idx < str.size() && ((str[end_idx] >= '0' && str[end_idx] <= '9') || str[end_idx] == '.')) {
                ++end_idx;
            }
            try {
                cur_val = std::stod(str.substr(i, end_idx - i));
            } catch (...) {
                cur_val = 0.0;
            }
            i = end_idx - 1;
        } else if (c == 'H') {
            total_sec += cur_val * 3600.0;
            cur_val = 0.0;
        } else if (c == 'M') {
            total_sec += in_time ? (cur_val * 60.0) : (cur_val * 86400.0 * 30.0);
            cur_val = 0.0;
        } else if (c == 'S') {
            total_sec += cur_val;
            cur_val = 0.0;
        }
    }
    return total_sec;
}

std::string ExtractXmlAttr(const std::string& tag_text, const std::string& attr_name) {
    std::regex re(attr_name + R"re(=\s*"([^"]*)")re");
    std::smatch m;
    if (std::regex_search(tag_text, m, re)) {
        return m[1].str();
    }
    return "";
}

std::string ExpandDashTemplate(std::string tmpl, const std::string& rep_id, uint64_t number, uint64_t bandwidth, uint64_t time_val = 0) {
    size_t pos = 0;
    while ((pos = tmpl.find("$RepresentationID$", pos)) != std::string::npos) {
        tmpl.replace(pos, 18, rep_id);
        pos += rep_id.size();
    }
    pos = 0;
    std::string bw_str = std::to_string(bandwidth);
    while ((pos = tmpl.find("$Bandwidth$", pos)) != std::string::npos) {
        tmpl.replace(pos, 11, bw_str);
        pos += bw_str.size();
    }
    static const std::regex kNumRe(R"(\$Number(?:%0?(\d+)d)?\$)");
    std::smatch m;
    while (std::regex_search(tmpl, m, kNumRe)) {
        int width = 1;
        if (m[1].matched) {
            try { width = std::stoi(m[1].str()); } catch (...) {}
        }
        std::ostringstream ss;
        ss << std::setw(width) << std::setfill('0') << number;
        tmpl.replace(m.position(), m.length(), ss.str());
    }
    static const std::regex kTimeRe(R"(\$Time(?:%0?(\d+)d)?\$)");
    while (std::regex_search(tmpl, m, kTimeRe)) {
        int width = 1;
        if (m[1].matched) {
            try { width = std::stoi(m[1].str()); } catch (...) {}
        }
        std::ostringstream ss;
        ss << std::setw(width) << std::setfill('0') << time_val;
        tmpl.replace(m.position(), m.length(), ss.str());
    }
    return tmpl;
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

std::string FindYtDlpBinary() {
    std::error_code ec;
#ifdef _WIN32
    const std::string name = "yt-dlp.exe";
    const char sep = ';';
#else
    const std::string name = "yt-dlp";
    const char sep = ':';
#endif
    const std::filesystem::path local_candidates[] = {
        std::filesystem::current_path(ec) / "tools" / "win-x64" / name,
        std::filesystem::current_path(ec) / "tools" / name
    };
    for (const auto& c : local_candidates) {
        if (std::filesystem::exists(c, ec)) return c.string();
    }
    if (const char* path_env = std::getenv("PATH")) {
        std::stringstream ss(path_env);
        std::string dir;
        while (std::getline(ss, dir, sep)) {
            if (dir.empty()) continue;
            auto cand = std::filesystem::path(dir) / name;
            if (std::filesystem::exists(cand, ec)) return cand.string();
        }
    }
    return "";
}

bool TryExtractViaYtDlp(const std::string& url, ManifestMeta& meta) {
    std::string ytdlp_bin = FindYtDlpBinary();
    if (ytdlp_bin.empty()) return false;

    std::string out;
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hReadPipe = NULL, hWritePipe = NULL;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return false;
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    auto to_wide = [](std::string_view sv) {
        if (sv.empty()) return std::wstring();
        int sz = MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), nullptr, 0);
        std::wstring w(sz, 0);
        MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), w.data(), sz);
        return w;
    };

    std::wstring cmd = L"\"" + to_wide(ytdlp_bin) + L"\" --dump-single-json --no-warnings --no-playlist \"" + to_wide(url) + L"\"";
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return false;
    }
    CloseHandle(hWritePipe);

    char buf[4096];
    DWORD got = 0;
    while (ReadFile(hReadPipe, buf, sizeof(buf), &got, nullptr) && got > 0) {
        out.append(buf, got);
    }
    CloseHandle(hReadPipe);

    WaitForSingleObject(pi.hProcess, 15000);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (exit_code != 0 || out.empty()) return false;
#else
    int pipefd[2];
    if (pipe(pipefd) != 0) return false;
    pid_t pid = fork();
    if (pid == 0) {
        ::close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); ::close(devnull); }
        if (pipefd[1] > 2) ::close(pipefd[1]);
        execl(ytdlp_bin.c_str(), ytdlp_bin.c_str(), "--dump-single-json", "--no-warnings", "--no-playlist", url.c_str(), nullptr);
        _exit(127);
    }
    ::close(pipefd[1]);
    char buf[4096];
    ssize_t n = 0;
    while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0) {
        out.append(buf, static_cast<size_t>(n));
    }
    ::close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || out.empty()) return false;
#endif

    try {
        auto root = openidm::json::Parse(out);
        if (!root.is_object()) return false;
        std::string direct_url = root["url"].as_string();
        if (!direct_url.empty()) {
            TrackItem tr;
            tr.id = "vid_0";
            tr.media_type = "VIDEO";
            tr.group_id = "main";
            tr.name = root["title"].as_string("youtube_video");
            tr.resolution = root["resolution"].as_string("1280x720");
            tr.playlist_url = direct_url;
            SegmentItem seg;
            seg.index = 0;
            seg.url = StripGooglevideoRangeParams(direct_url);
            seg.duration = root["duration"].as_double(4.0);
            tr.segments.push_back(std::move(seg));
            tr.duration_sec = root["duration"].as_double(4.0);
            meta.tracks.push_back(std::move(tr));
            return true;
        }
    } catch (...) {}
    return false;
}

bool ExtractManifestInfo(const WorkerOptions& opts, ManifestMeta& meta);

bool ParseYouTubeJsonIntoMeta(const openidm::json::Value& root, const WorkerOptions& opts, ManifestMeta& meta) {
    if (!root.is_object()) return false;

    // Case A: Structured YOUTUBE_STREAM descriptor emitted by Flux extension catch.js
    if (root["fluxType"].as_string() == "YOUTUBE_STREAM") {
        std::string hls_url = root["hlsManifestUrl"].as_string();
        if (!hls_url.empty()) {
            WorkerOptions sub = opts;
            sub.input = hls_url;
            sub.base_url = hls_url;
            if (ExtractManifestInfo(sub, meta)) return true;
        }
        std::string dash_url = root["dashManifestUrl"].as_string();
        if (!dash_url.empty()) {
            WorkerOptions sub = opts;
            sub.input = dash_url;
            sub.base_url = dash_url;
            if (ExtractManifestInfo(sub, meta)) return true;
        }

        std::string video_url = StripGooglevideoRangeParams(root["videoUrl"].as_string());
        std::string audio_url = StripGooglevideoRangeParams(root["audioUrl"].as_string());
        std::string watch_url = root["watchUrl"].as_string(opts.base_url);

        if (!video_url.empty()) {
            meta.url = watch_url.empty() ? video_url : watch_url;
            TrackItem vtr;
            vtr.id = "vid_0";
            vtr.media_type = "VIDEO";
            vtr.group_id = "youtube_video";
            vtr.language = "und";
            vtr.name = root["title"].as_string("youtube_video");
            vtr.resolution = root["resolution"].as_string("1280x720");
            vtr.codecs = "avc1.64001f";
            vtr.bandwidth = 2500000;
            vtr.playlist_url = video_url;
            SegmentItem vseg;
            vseg.index = 0;
            vseg.url = video_url;
            vseg.duration = 4.0;
            vtr.segments.push_back(std::move(vseg));
            vtr.duration_sec = 4.0;
            meta.tracks.push_back(std::move(vtr));

            if (!audio_url.empty() && audio_url != video_url) {
                TrackItem atr;
                atr.id = "aud_0";
                atr.media_type = "AUDIO";
                atr.group_id = "youtube_audio";
                atr.language = "en";
                atr.name = "audio_main";
                atr.codecs = "mp4a.40.2";
                atr.bandwidth = 128000;
                atr.playlist_url = audio_url;
                SegmentItem aseg;
                aseg.index = 0;
                aseg.url = audio_url;
                aseg.duration = 4.0;
                atr.segments.push_back(std::move(aseg));
                atr.duration_sec = 4.0;
                meta.tracks.push_back(std::move(atr));
            }
            return true;
        }

        if (!watch_url.empty() && watch_url != opts.input) {
            if (TryExtractViaYtDlp(watch_url, meta)) return true;
            WorkerOptions sub = opts;
            sub.input = watch_url;
            sub.base_url = watch_url;
            return ExtractManifestInfo(sub, meta);
        }
        return false;
    }

    // Case B: Raw ytInitialPlayerResponse JSON object
    const auto& play_status = root["playabilityStatus"];
    if (play_status.is_object()) {
        std::string st = play_status["status"].as_string("OK");
        if (st != "OK" && !root["streamingData"].is_object()) {
            std::string reason = play_status["reason"].as_string("Stream status: " + st);
            std::cerr << "ERROR: YouTube stream unplayable: " << reason << "\n";
            return false;
        }
    }

    const auto& sdata = root["streamingData"];
    if (!sdata.is_object()) return false;

    std::string hls_url = sdata["hlsManifestUrl"].as_string();
    if (!hls_url.empty()) {
        WorkerOptions sub = opts;
        sub.input = hls_url;
        sub.base_url = hls_url;
        if (ExtractManifestInfo(sub, meta)) return true;
    }

    std::string dash_url = sdata["dashManifestUrl"].as_string();
    if (!dash_url.empty()) {
        WorkerOptions sub = opts;
        sub.input = dash_url;
        sub.base_url = dash_url;
        if (ExtractManifestInfo(sub, meta)) return true;
    }

    std::string best_video_url;
    std::string best_video_res = "640x360";
    uint64_t best_video_bw = 0;
    std::string best_audio_url;
    uint64_t best_audio_bw = 0;

    if (sdata["adaptiveFormats"].is_array()) {
        for (const auto& item : sdata["adaptiveFormats"].as_array()) {
            std::string u = item["url"].as_string();
            if (u.empty()) continue;
            std::string mime = ToLower(item["mimeType"].as_string());
            uint64_t bw = static_cast<uint64_t>(item["bitrate"].as_int64(item["averageBitrate"].as_int64(500000)));
            if (mime.rfind("video/", 0) == 0) {
                if (bw >= best_video_bw) {
                    best_video_bw = bw;
                    best_video_url = StripGooglevideoRangeParams(u);
                    std::string q = item["qualityLabel"].as_string();
                    if (!q.empty()) best_video_res = q;
                }
            } else if (mime.rfind("audio/", 0) == 0) {
                if (bw >= best_audio_bw) {
                    best_audio_bw = bw;
                    best_audio_url = StripGooglevideoRangeParams(u);
                }
            }
        }
    }

    if (best_video_url.empty() && sdata["formats"].is_array()) {
        for (const auto& item : sdata["formats"].as_array()) {
            std::string u = item["url"].as_string();
            if (u.empty()) continue;
            uint64_t bw = static_cast<uint64_t>(item["bitrate"].as_int64(1000000));
            if (bw >= best_video_bw || best_video_url.empty()) {
                best_video_bw = bw;
                best_video_url = StripGooglevideoRangeParams(u);
                std::string q = item["qualityLabel"].as_string();
                if (!q.empty()) best_video_res = q;
            }
        }
    }

    if (!best_video_url.empty()) {
        TrackItem vtr;
        vtr.id = "vid_0";
        vtr.media_type = "VIDEO";
        vtr.group_id = "youtube_video";
        vtr.name = root["videoDetails"]["title"].as_string("youtube_video");
        vtr.resolution = best_video_res;
        vtr.bandwidth = best_video_bw > 0 ? best_video_bw : 1500000;
        vtr.playlist_url = best_video_url;
        SegmentItem vseg;
        vseg.index = 0;
        vseg.url = best_video_url;
        vseg.duration = 4.0;
        vtr.segments.push_back(std::move(vseg));
        vtr.duration_sec = 4.0;
        meta.tracks.push_back(std::move(vtr));

        if (!best_audio_url.empty() && best_audio_url != best_video_url) {
            TrackItem atr;
            atr.id = "aud_0";
            atr.media_type = "AUDIO";
            atr.group_id = "youtube_audio";
            atr.language = "en";
            atr.name = "audio_main";
            atr.bandwidth = best_audio_bw > 0 ? best_audio_bw : 128000;
            atr.playlist_url = best_audio_url;
            SegmentItem aseg;
            aseg.index = 0;
            aseg.url = best_audio_url;
            aseg.duration = 4.0;
            atr.segments.push_back(std::move(aseg));
            atr.duration_sec = 4.0;
            meta.tracks.push_back(std::move(atr));
        }
        return true;
    }

    std::cerr << "ERROR: YouTube stream requires external signature deciphering (install yt-dlp in tools/ or PATH)\n";
    return false;
}

bool ExtractManifestInfo(const WorkerOptions& opts, ManifestMeta& meta) {
    std::vector<uint8_t> raw_bytes;
    std::string resolved_url;
    if (!FetchBytesWithRetry(opts.input, opts.headers, raw_bytes, resolved_url, opts.download_retry_count, opts.http_request_timeout_ms, opts.allow_loopback, true)) {
        return false;
    }
    std::string base_url = opts.base_url.empty() ? resolved_url : opts.base_url;
    std::string text(reinterpret_cast<const char*>(raw_bytes.data()), raw_bytes.size());
    text = Trim(text);
    meta.url = resolved_url;

    // 1. HLS (#EXTM3U)
    if (text.rfind("#EXTM3U", 0) == 0) {
        if (text.find("#EXT-X-STREAM-INF") != std::string::npos) {
            std::istringstream iss(text);
            std::vector<std::string> lines;
            std::string l;
            while (std::getline(iss, l)) {
                l = Trim(l);
                if (!l.empty()) lines.push_back(l);
            }
            uint32_t vid_idx = 0, aud_idx = 0, sub_idx = 0;
            for (size_t i = 0; i < lines.size(); ++i) {
                const std::string& ln = lines[i];
                if (ln.rfind("#EXT-X-KEY:", 0) == 0 || ln.rfind("#EXT-X-SESSION-KEY:", 0) == 0) {
                    auto attrs = ParseM3u8Attributes(ln.substr(ln.find(':') + 1));
                    std::string m = ToUpper(attrs["METHOD"]);
                    std::replace(m.begin(), m.end(), '-', '_');
                    if (m == "SAMPLE_AES_CTR" || m == "SAMPLE_AES" || m == "CENC") {
                        meta.drm_protected = true;
                        meta.drm_system = m;
                    }
                } else if (ln.rfind("#EXT-X-MEDIA:", 0) == 0) {
                    auto attrs = ParseM3u8Attributes(ln.substr(13));
                    std::string mtype = ToUpper(attrs.count("TYPE") ? attrs["TYPE"] : "AUDIO");
                    std::string uri = attrs["URI"];
                    if (uri.empty()) continue;
                    std::string sub_url = ResolveUrl(base_url, uri);
                    std::vector<uint8_t> sub_bytes;
                    std::string sub_res_url;
                    if (!FetchBytesWithRetry(sub_url, opts.headers, sub_bytes, sub_res_url, opts.download_retry_count, opts.http_request_timeout_ms, opts.allow_loopback, false)) continue;
                    TrackItem tr;
                    tr.id = (mtype == "AUDIO") ? ("aud_" + std::to_string(aud_idx++)) : ("sub_" + std::to_string(sub_idx++));
                    tr.media_type = (mtype == "AUDIO") ? "AUDIO" : "SUBTITLES";
                    tr.group_id = attrs["GROUP-ID"];
                    tr.language = attrs.count("LANGUAGE") ? attrs["LANGUAGE"] : "und";
                    tr.name = attrs.count("NAME") ? attrs["NAME"] : tr.id;
                    tr.codecs = (mtype == "AUDIO") ? "mp4a.40.2" : "wvtt";
                    tr.bandwidth = (mtype == "AUDIO") ? 128000 : 1000;
                    tr.frame_rate = 0.0;
                    tr.playlist_url = sub_url;
                    bool is_live = false;
                    ParseHlsMediaPlaylist(std::string(reinterpret_cast<const char*>(sub_bytes.data()), sub_bytes.size()), sub_url, tr, is_live);
                    meta.tracks.push_back(std::move(tr));
                } else if (ln.rfind("#EXT-X-STREAM-INF:", 0) == 0) {
                    auto attrs = ParseM3u8Attributes(ln.substr(18));
                    if (i + 1 < lines.size() && lines[i + 1].front() != '#') {
                        std::string var_url = ResolveUrl(base_url, lines[i + 1]);
                        std::vector<uint8_t> var_bytes;
                        std::string var_res_url;
                        if (!FetchBytesWithRetry(var_url, opts.headers, var_bytes, var_res_url, opts.download_retry_count, opts.http_request_timeout_ms, opts.allow_loopback, false)) continue;
                        TrackItem tr;
                        tr.id = "vid_" + std::to_string(vid_idx++);
                        tr.media_type = "VIDEO";
                        tr.group_id = attrs.count("AUDIO") ? attrs["AUDIO"] : "main";
                        tr.language = "und";
                        tr.resolution = attrs.count("RESOLUTION") ? attrs["RESOLUTION"] : "1280x720";
                        try { tr.bandwidth = std::stoull(attrs.count("BANDWIDTH") ? attrs["BANDWIDTH"] : "1500000"); } catch (...) { tr.bandwidth = 1500000; }
                        try { tr.frame_rate = std::stod(attrs.count("FRAME-RATE") ? attrs["FRAME-RATE"] : "30.0"); } catch (...) { tr.frame_rate = 30.0; }
                        tr.codecs = attrs.count("CODECS") ? attrs["CODECS"] : "avc1.64001f";
                        tr.name = tr.resolution + "_" + std::to_string(tr.bandwidth);
                        tr.playlist_url = var_url;
                        bool is_live = false;
                        ParseHlsMediaPlaylist(std::string(reinterpret_cast<const char*>(var_bytes.data()), var_bytes.size()), var_url, tr, is_live);
                        if (tr.encryption_method == "SAMPLE_AES_CTR" || tr.encryption_method == "SAMPLE_AES" || tr.encryption_method == "CENC") {
                            meta.drm_protected = true;
                            meta.drm_system = tr.encryption_method;
                        }
                        meta.tracks.push_back(std::move(tr));
                    }
                }
            }
        } else {
            TrackItem tr;
            tr.id = "vid_0";
            tr.media_type = "VIDEO";
            tr.group_id = "main";
            tr.language = "und";
            tr.name = "main";
            tr.codecs = "avc1.64001f";
            tr.resolution = "1280x720";
            tr.bandwidth = 2500000;
            tr.frame_rate = 30.0;
            tr.playlist_url = base_url;
            ParseHlsMediaPlaylist(text, base_url, tr, meta.is_live);
            if (tr.encryption_method == "SAMPLE_AES_CTR" || tr.encryption_method == "SAMPLE_AES" || tr.encryption_method == "CENC") {
                meta.drm_protected = true;
                meta.drm_system = tr.encryption_method;
            }
            meta.tracks.push_back(std::move(tr));
        }
        return true;
    }

    // 2. MPEG-DASH (<MPD)
    if (text.find("<MPD") != std::string::npos) {
        if (text.find("ContentProtection") != std::string::npos) {
            meta.drm_protected = true;
            std::string scheme = ExtractXmlAttr(text, "schemeIdUri");
            meta.drm_system = scheme.empty() ? "CENC" : scheme;
        }
        meta.is_live = (ToLower(ExtractXmlAttr(text, "type")) == "dynamic");

        std::string mpd_duration_str = ExtractXmlAttr(text, "mediaPresentationDuration");
        double mpd_duration_sec = ParseIso8601DurationSec(mpd_duration_str);

        static const std::regex kAdaptRe(R"(<AdaptationSet\b([^>]*)>([\s\S]*?)</AdaptationSet>)");
        static const std::regex kRepRe(R"(<Representation\b([^>]*)>([\s\S]*?)</Representation>)");
        static const std::regex kBaseUrlRe(R"(<BaseURL>([^<]+)</BaseURL>)");
        static const std::regex kSegTmplRe(R"(<SegmentTemplate\b([^>]*)(?:/>|>([\s\S]*?)</SegmentTemplate>))");
        static const std::regex kSTagRe(R"(<S\b([^>]*)/?>)");

        uint32_t vid_idx = 0, aud_idx = 0;
        auto abegin = std::sregex_iterator(text.begin(), text.end(), kAdaptRe);
        auto aend = std::sregex_iterator();
        for (auto ait = abegin; ait != aend; ++ait) {
            std::string adapt_attrs = (*ait)[1].str();
            std::string adapt_body = (*ait)[2].str();
            std::string mime = ExtractXmlAttr(adapt_attrs, "mimeType");
            std::string content_type = ExtractXmlAttr(adapt_attrs, "contentType");
            std::string lang = ExtractXmlAttr(adapt_attrs, "lang");
            if (lang.empty()) lang = "und";

            std::string adapt_tmpl_attrs;
            std::string adapt_tmpl_body;
            std::smatch tm_match;
            if (std::regex_search(adapt_body, tm_match, kSegTmplRe)) {
                adapt_tmpl_attrs = tm_match[1].str();
                if (tm_match.size() > 2) adapt_tmpl_body = tm_match[2].str();
            }

            auto rbegin = std::sregex_iterator(adapt_body.begin(), adapt_body.end(), kRepRe);
            auto rend = std::sregex_iterator();
            for (auto rit = rbegin; rit != rend; ++rit) {
                std::string rep_attrs = (*rit)[1].str();
                std::string rep_body = (*rit)[2].str();
                std::string rep_id = ExtractXmlAttr(rep_attrs, "id");
                if (rep_id.empty()) rep_id = "0";
                std::string rep_mime = ExtractXmlAttr(rep_attrs, "mimeType");
                if (rep_mime.empty()) rep_mime = mime;
                std::string codecs = ExtractXmlAttr(rep_attrs, "codecs");
                if (codecs.empty()) codecs = ExtractXmlAttr(adapt_attrs, "codecs");
                if (codecs.empty()) codecs = "avc1";
                uint64_t bw = 1000000;
                try { bw = std::stoull(ExtractXmlAttr(rep_attrs, "bandwidth")); } catch (...) {}
                std::string width = ExtractXmlAttr(rep_attrs, "width");
                std::string height = ExtractXmlAttr(rep_attrs, "height");
                std::string res = (!width.empty() && !height.empty()) ? (width + "x" + height) : "";
                bool is_video = (rep_mime.find("video") != std::string::npos || content_type.find("video") != std::string::npos || !res.empty());

                TrackItem tr;
                tr.id = is_video ? ("vid_" + std::to_string(vid_idx++)) : ("aud_" + std::to_string(aud_idx++));
                tr.media_type = is_video ? "VIDEO" : "AUDIO";
                tr.group_id = rep_id;
                tr.language = lang;
                tr.name = tr.media_type + "_" + rep_id;
                tr.codecs = codecs;
                tr.resolution = res;
                tr.bandwidth = bw;
                tr.frame_rate = is_video ? 30.0 : 0.0;
                tr.encrypted = meta.drm_protected;
                tr.encryption_method = meta.drm_protected ? "CENC" : "NONE";
                tr.playlist_url = base_url;

                std::string tmpl_attrs = adapt_tmpl_attrs;
                std::string tmpl_body = adapt_tmpl_body;
                std::smatch rtm;
                if (std::regex_search(rep_body, rtm, kSegTmplRe)) {
                    tmpl_attrs = rtm[1].str();
                    if (rtm.size() > 2) tmpl_body = rtm[2].str();
                }

                std::smatch bm;
                if (!tmpl_attrs.empty()) {
                    std::string init_pat = ExtractXmlAttr(tmpl_attrs, "initialization");
                    std::string media_pat = ExtractXmlAttr(tmpl_attrs, "media");
                    int start_num = 1;
                    int count = 0;
                    try {
                        std::string sn = ExtractXmlAttr(tmpl_attrs, "startNumber");
                        if (!sn.empty()) start_num = std::stoi(sn);
                        std::string sc = ExtractXmlAttr(tmpl_attrs, "segmentCount");
                        if (!sc.empty()) count = std::stoi(sc);
                    } catch (...) {}

                    uint64_t timescale = 1;
                    std::string ts_str = ExtractXmlAttr(tmpl_attrs, "timescale");
                    if (!ts_str.empty()) {
                        try { timescale = std::stoull(ts_str); } catch (...) {}
                    }
                    if (timescale == 0) timescale = 1;

                    struct TimelineEntry {
                        uint64_t number;
                        uint64_t time;
                        double duration_sec;
                    };
                    std::vector<TimelineEntry> timeline_segments;

                    if (!tmpl_body.empty()) {
                        uint64_t current_time = 0;
                        uint64_t cur_num = static_cast<uint64_t>(start_num);
                        for (auto sit = std::sregex_iterator(tmpl_body.begin(), tmpl_body.end(), kSTagRe); sit != std::sregex_iterator(); ++sit) {
                            std::string s_attrs = (*sit)[1].str();
                            std::string t_str = ExtractXmlAttr(s_attrs, "t");
                            if (!t_str.empty()) {
                                try { current_time = std::stoull(t_str); } catch (...) {}
                            }
                            uint64_t d = 0;
                            std::string d_str = ExtractXmlAttr(s_attrs, "d");
                            if (!d_str.empty()) {
                                try { d = std::stoull(d_str); } catch (...) {}
                            }
                            int r = 0;
                            std::string r_str = ExtractXmlAttr(s_attrs, "r");
                            if (!r_str.empty()) {
                                try { r = std::stoi(r_str); } catch (...) {}
                            }
                            int repeat_count = (r >= 0) ? r : 0;
                            double dur_sec = (d > 0) ? (static_cast<double>(d) / static_cast<double>(timescale)) : 2.0;

                            for (int k = 0; k <= repeat_count; ++k) {
                                timeline_segments.push_back({cur_num++, current_time, dur_sec});
                                current_time += d;
                            }
                        }
                    }

                    if (!init_pat.empty()) {
                        uint64_t init_time = timeline_segments.empty() ? 0 : timeline_segments.front().time;
                        tr.init_url = ResolveUrl(base_url, ExpandDashTemplate(init_pat, rep_id, start_num, bw, init_time));
                    }

                    if (!timeline_segments.empty()) {
                        double total_dur = 0.0;
                        for (const auto& ts : timeline_segments) {
                            std::string m_rel = ExpandDashTemplate(media_pat, rep_id, ts.number, bw, ts.time);
                            SegmentItem seg;
                            seg.index = static_cast<uint32_t>(tr.segments.size());
                            seg.url = ResolveUrl(base_url, m_rel);
                            seg.duration = ts.duration_sec;
                            seg.encrypted = meta.drm_protected;
                            seg.method = meta.drm_protected ? "CENC" : "NONE";
                            tr.segments.push_back(std::move(seg));
                            total_dur += ts.duration_sec;
                        }
                        if (mpd_duration_sec <= 0.0) {
                            tr.duration_sec = total_dur;
                        }
                    } else {
                        if (count <= 0) {
                            uint64_t seg_dur = 0;
                            std::string dur_str = ExtractXmlAttr(tmpl_attrs, "duration");
                            if (!dur_str.empty()) {
                                try { seg_dur = std::stoull(dur_str); } catch (...) {}
                            }

                            if (mpd_duration_sec > 0.0 && seg_dur > 0) {
                                double seg_duration_sec = static_cast<double>(seg_dur) / static_cast<double>(timescale);
                                if (seg_duration_sec > 0.0) {
                                    count = static_cast<int>(std::ceil(mpd_duration_sec / seg_duration_sec));
                                }
                            }
                        }
                        if (count <= 0) count = 3;

                        uint64_t seg_dur = 0;
                        std::string dur_str = ExtractXmlAttr(tmpl_attrs, "duration");
                        if (!dur_str.empty()) {
                            try { seg_dur = std::stoull(dur_str); } catch (...) {}
                        }
                        double seg_duration_sec = (seg_dur > 0) ? (static_cast<double>(seg_dur) / static_cast<double>(timescale)) : 2.0;

                        for (int n = start_num; n < start_num + count; ++n) {
                            uint64_t seg_time = static_cast<uint64_t>(n - start_num) * seg_dur;
                            std::string m_rel = ExpandDashTemplate(media_pat, rep_id, n, bw, seg_time);
                            SegmentItem seg;
                            seg.index = static_cast<uint32_t>(tr.segments.size());
                            seg.url = ResolveUrl(base_url, m_rel);
                            seg.duration = seg_duration_sec;
                            seg.encrypted = meta.drm_protected;
                            seg.method = meta.drm_protected ? "CENC" : "NONE";
                            tr.segments.push_back(std::move(seg));
                        }
                        if (mpd_duration_sec <= 0.0) {
                            tr.duration_sec = tr.segments.size() * seg_duration_sec;
                        }
                    }
                } else if (std::regex_search(rep_body, bm, kBaseUrlRe)) {
                    SegmentItem seg;
                    seg.index = 0;
                    seg.url = ResolveUrl(base_url, Trim(bm[1].str()));
                    seg.duration = 4.0;
                    seg.encrypted = meta.drm_protected;
                    seg.method = meta.drm_protected ? "CENC" : "NONE";
                    tr.segments.push_back(std::move(seg));
                }
                tr.duration_sec = (mpd_duration_sec > 0.0) ? mpd_duration_sec : (tr.segments.size() * 2.0);
                meta.tracks.push_back(std::move(tr));
            }
        }
        return true;
    }

    // 3. JSON Stream Descriptor (YOUTUBE_STREAM or ytInitialPlayerResponse JSON)
    if (!text.empty() && text.front() == '{') {
        try {
            auto root = openidm::json::Parse(text);
            if (ParseYouTubeJsonIntoMeta(root, opts, meta)) {
                return true;
            }
        } catch (...) {}
    }

    // 4. HTML Webpage containing ytInitialPlayerResponse (YouTube watch/shorts/embed page)
    auto yt_pos = text.find("ytInitialPlayerResponse");
    if (yt_pos != std::string::npos) {
        std::string json_str = ExtractBalancedJsonObject(text, yt_pos);
        if (!json_str.empty()) {
            try {
                auto root = openidm::json::Parse(json_str);
                if (ParseYouTubeJsonIntoMeta(root, opts, meta)) {
                    return true;
                }
            } catch (...) {}
        }
    }

    // 5. Fallback to external yt-dlp binary if input is a webpage URL
    if (TryExtractViaYtDlp(base_url, meta)) {
        return true;
    }

    return false;
}

std::vector<TrackItem> SelectTracks(const std::vector<TrackItem>& tracks, const std::string& media_type, const std::string& filter_expr) {
    std::vector<TrackItem> candidates;
    for (const auto& t : tracks) {
        if (t.media_type == media_type) candidates.push_back(t);
    }
    std::string fl = ToLower(Trim(filter_expr));
    if (candidates.empty() || fl == "none" || fl == "false" || fl == "drop") {
        return {};
    }

    std::sort(candidates.begin(), candidates.end(), [](const TrackItem& a, const TrackItem& b) {
        return a.bandwidth > b.bandwidth;
    });

    if (fl == "all") {
        return candidates;
    }
    if (fl == "worst") {
        return {candidates.back()};
    }
    if (fl == "best" || fl.empty()) {
        return {candidates.front()};
    }

    // Helper to parse width and height from a resolution string (e.g. "1920x1080", "256x144", "1080p")
    auto parse_wh = [](std::string_view r, int& out_w, int& out_h) {
        out_w = 0;
        out_h = 0;
        auto x_pos = r.find('x');
        if (x_pos == std::string_view::npos) x_pos = r.find('X');
        if (x_pos != std::string_view::npos) {
            try {
                out_w = std::stoi(std::string(r.substr(0, x_pos)));
                out_h = std::stoi(std::string(r.substr(x_pos + 1)));
            } catch (...) {}
        } else {
            std::smatch m;
            std::string s(r);
            static const std::regex kRe(R"((\d{3,4})p?)");
            if (std::regex_search(s, m, kRe)) {
                try { out_h = std::stoi(m[1].str()); } catch (...) {}
            }
        }
    };

    // Helper to extract height from a filter or selector (e.g. "144", "144p", "res=144*", "res='144p*'", "height=144")
    auto extract_target_height = [](std::string_view f) -> int {
        std::string s(f);
        std::smatch m;
        static const std::regex kRe(R"((?:height\s*=\s*|res\s*=\s*["']?|^)(\d{3,4})p?)");
        if (std::regex_search(s, m, kRe)) {
            try { return std::stoi(m[1].str()); } catch (...) {}
        }
        return 0;
    };

    int req_target_h = extract_target_height(fl);

    // Check if composite filter with colons or key-values (e.g. res=..., codecs=..., lang=..., for=...)
    bool is_composite = (fl.find(':') != std::string::npos || fl.find('=') != std::string::npos);
    if (!is_composite) {
        for (const auto& c : candidates) {
            std::string rl = ToLower(c.resolution);
            std::string idl = ToLower(c.id);
            std::string langl = ToLower(c.language);
            std::string namel = ToLower(c.name);
            if (rl == fl || idl == fl || langl == fl || namel == fl) {
                return {c};
            }
            if (req_target_h > 0) {
                int cw = 0, ch = 0;
                parse_wh(c.resolution, cw, ch);
                std::string suffix = "x" + std::to_string(req_target_h);
                std::string p_str = std::to_string(req_target_h) + "p";
                if (ch == req_target_h || rl.ends_with(suffix) || rl == p_str ||
                    rl.find(p_str) != std::string::npos || idl.find(p_str) != std::string::npos ||
                    namel.find(p_str) != std::string::npos) {
                    return {c};
                }
            }
        }
        // If a specific target height was requested and no exact match found, find closest resolution
        if (req_target_h > 0) {
            const TrackItem* best_closest = &candidates.front();
            int min_diff = 999999;
            for (const auto& c : candidates) {
                int cw = 0, ch = 0;
                parse_wh(c.resolution, cw, ch);
                if (ch > 0) {
                    int diff = std::abs(ch - req_target_h);
                    if (diff < min_diff) {
                        min_diff = diff;
                        best_closest = &c;
                    }
                }
            }
            return {*best_closest};
        }
        return {candidates.front()};
    }

    // Parse composite filter key-value pairs
    std::map<std::string, std::string> criteria;
    std::string for_target = (media_type == "AUDIO") ? "best" : "best";
    std::stringstream fss(fl);
    std::string item;
    while (std::getline(fss, item, ':')) {
        item = Trim(item);
        if (item.empty()) continue;
        auto eq = item.find('=');
        if (eq != std::string::npos) {
            std::string k = Trim(item.substr(0, eq));
            std::string v = Trim(item.substr(eq + 1));
            if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\''))) {
                v = v.substr(1, v.size() - 2);
            }
            if (k == "for") {
                for_target = v;
            } else {
                criteria[k] = v;
            }
        }
    }

    std::vector<TrackItem> matched;
    int crit_target_h = 0;
    if (criteria.count("res")) {
        crit_target_h = extract_target_height(criteria["res"]);
    } else if (criteria.count("height")) {
        try { crit_target_h = std::stoi(criteria["height"]); } catch (...) {}
    }

    for (const auto& c : candidates) {
        bool pass = true;
        std::string rl = ToLower(c.resolution);
        std::string idl = ToLower(c.id);
        std::string langl = ToLower(c.language);
        std::string codl = ToLower(c.codecs);
        std::string namel = ToLower(c.name);
        int cw = 0, ch = 0;
        parse_wh(c.resolution, cw, ch);

        for (const auto& [k, v] : criteria) {
            if (k == "res") {
                std::string target_res = v;
                bool res_matched = false;
                if (crit_target_h > 0 && ch == crit_target_h) {
                    res_matched = true;
                } else if (!target_res.empty() && target_res.back() == '*') {
                    std::string prefix = target_res.substr(0, target_res.size() - 1);
                    if (rl.rfind(prefix, 0) == 0 || rl.find(prefix) != std::string::npos ||
                        namel.find(prefix) != std::string::npos) {
                        res_matched = true;
                    }
                } else if (rl == target_res || rl.find(target_res) != std::string::npos) {
                    res_matched = true;
                }
                if (!res_matched) pass = false;
            } else if (k == "height") {
                try {
                    int h_val = std::stoi(v);
                    if (ch != h_val) pass = false;
                } catch (...) {
                    pass = false;
                }
            } else if (k == "codecs") {
                std::string target_c = v;
                if (!target_c.empty() && target_c.back() == '*') {
                    std::string prefix = target_c.substr(0, target_c.size() - 1);
                    if (codl.rfind(prefix, 0) != 0 && codl.find(prefix) == std::string::npos) pass = false;
                } else if (codl.find(target_c) == std::string::npos) {
                    pass = false;
                }
            } else if (k == "lang" || k == "language") {
                if (langl != v && langl.find(v) == std::string::npos && v.find(langl) == std::string::npos) {
                    pass = false;
                }
            } else if (k == "id") {
                if (idl != v && idl.rfind(v, 0) != 0) pass = false;
            } else if (k == "name" || k == "group") {
                if (namel.find(v) == std::string::npos) pass = false;
            } else if (k == "bw") {
                try {
                    uint64_t target_bw = std::stoull(v.front() == '>' || v.front() == '<' ? v.substr(1) : v);
                    if (v.rfind(">=", 0) == 0 && c.bandwidth < target_bw) pass = false;
                    else if (v.front() == '<' && c.bandwidth > target_bw) pass = false;
                } catch (...) {}
            }
            if (!pass) break;
        }
        if (pass) matched.push_back(c);
    }

    if (matched.empty()) {
        if (crit_target_h > 0) {
            const TrackItem* best_closest = &candidates.front();
            int min_diff = 999999;
            for (const auto& c : candidates) {
                int cw = 0, ch = 0;
                parse_wh(c.resolution, cw, ch);
                if (ch > 0) {
                    int diff = std::abs(ch - crit_target_h);
                    if (diff < min_diff) {
                        min_diff = diff;
                        best_closest = &c;
                    }
                }
            }
            return {*best_closest};
        }
        matched = candidates;
    }

    if (for_target == "all") {
        return matched;
    }
    if (for_target == "worst") {
        return {matched.back()};
    }
    return {matched.front()};
}

std::vector<uint8_t> HexToBytes(std::string_view hex_sv) {
    std::string h(hex_sv);
    if (h.rfind("0x", 0) == 0 || h.rfind("0X", 0) == 0) h = h.substr(2);
    if (h.size() % 2 != 0) h = "0" + h;
    std::vector<uint8_t> out;
    out.reserve(h.size() / 2);
    for (size_t i = 0; i + 1 < h.size(); i += 2) {
        char buf[3] = {h[i], h[i + 1], 0};
        out.push_back(static_cast<uint8_t>(std::strtoul(buf, nullptr, 16)));
    }
    return out;
}

bool DecryptAes128Cbc(
    const std::vector<uint8_t>& cipher,
    const std::vector<uint8_t>& key,
    const std::vector<uint8_t>& iv,
    std::vector<uint8_t>& plain_out) {
    if (key.size() != 16 || iv.size() != 16 || cipher.empty()) return false;

#ifdef _WIN32
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) return false;
    BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)), sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
    if (BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0, const_cast<PUCHAR>(key.data()), 16, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }
    std::vector<uint8_t> iv_copy = iv;
    ULONG out_len = 0;
    plain_out.resize(cipher.size());
    NTSTATUS st = BCryptDecrypt(
        hKey,
        const_cast<PUCHAR>(cipher.data()),
        static_cast<ULONG>(cipher.size()),
        nullptr,
        iv_copy.data(),
        16,
        plain_out.data(),
        static_cast<ULONG>(plain_out.size()),
        &out_len,
        BCRYPT_BLOCK_PADDING);
    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    if (st != 0) return false;
    plain_out.resize(out_len);
    return true;
#else
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    plain_out.resize(cipher.size() + 16);
    int len1 = 0, len2 = 0;
    if (EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr, key.data(), iv.data()) != 1 ||
        EVP_DecryptUpdate(ctx, plain_out.data(), &len1, cipher.data(), static_cast<int>(cipher.size())) != 1 ||
        EVP_DecryptFinal_ex(ctx, plain_out.data() + len1, &len2) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    EVP_CIPHER_CTX_free(ctx);
    plain_out.resize(static_cast<size_t>(len1 + len2));
    return true;
#endif
}

bool DownloadAndAssembleTrack(
    const TrackItem& track,
    const WorkerOptions& opts,
    const std::filesystem::path& track_dir,
    std::filesystem::path& out_assembled) {

    std::error_code ec;
    std::filesystem::create_directories(track_dir, ec);

    std::filesystem::path init_file;
    if (!track.init_url.empty()) {
        init_file = track_dir / "_init.mp4";
        if (!std::filesystem::exists(init_file, ec)) {
            std::vector<uint8_t> ibytes;
            std::string rurl;
            if (!FetchBytesWithRetry(track.init_url, opts.headers, ibytes, rurl, opts.download_retry_count, opts.http_request_timeout_ms, opts.allow_loopback)) return false;
            std::ofstream ofs(init_file, std::ios::binary);
            ofs.write(reinterpret_cast<const char*>(ibytes.data()), static_cast<std::streamsize>(ibytes.size()));
        }
    }

    size_t total_segs = track.segments.size();
    if (total_segs == 0) return false;

    std::mutex mu;
    std::map<std::string, std::vector<uint8_t>> key_cache;
    std::vector<std::filesystem::path> seg_files(total_segs);
    std::atomic<size_t> next_idx{0};
    std::atomic<size_t> done_segs{0};
    std::atomic<uint64_t> downloaded_bytes{0};
    std::atomic<bool> failed{false};

    uint32_t workers_count = std::clamp<uint32_t>(opts.thread_count, 1u, static_cast<uint32_t>(total_segs));
    std::vector<std::thread> workers;
    auto start_time = std::chrono::steady_clock::now();

    for (uint32_t w = 0; w < workers_count; ++w) {
        workers.emplace_back([&]() {
            while (!failed.load()) {
                size_t idx = next_idx.fetch_add(1);
                if (idx >= total_segs) break;

                const auto& seg = track.segments[idx];
                std::string ext = (track.media_type == "SUBTITLES") ? ".vtt" : ".ts";
                char name_buf[32];
                std::snprintf(name_buf, sizeof(name_buf), "seg_%05zu%s", idx, ext.c_str());
                std::filesystem::path seg_path = track_dir / name_buf;

                uint64_t seg_sz = 0;
                if (std::filesystem::exists(seg_path, ec) && std::filesystem::file_size(seg_path, ec) > 0) {
                    seg_sz = std::filesystem::file_size(seg_path, ec);
                } else {
                    std::vector<uint8_t> raw;
                    std::string rurl;
                    if (!FetchBytesWithRetry(seg.url, opts.headers, raw, rurl, opts.download_retry_count, opts.http_request_timeout_ms, opts.allow_loopback, false, &failed)) {
                        failed.store(true);
                        break;
                    }
                    if (seg.encrypted && seg.method == "AES_128") {
                        std::vector<uint8_t> key_bytes;
                        if (!opts.custom_hls_key.empty()) {
                            key_bytes = HexToBytes(opts.custom_hls_key);
                        } else {
                            {
                                std::lock_guard<std::mutex> lk(mu);
                                if (key_cache.count(seg.key_uri)) {
                                    key_bytes = key_cache[seg.key_uri];
                                }
                            }
                            if (key_bytes.empty()) {
                                std::string krurl;
                                if (!FetchBytesWithRetry(seg.key_uri, opts.headers, key_bytes, krurl, opts.download_retry_count, opts.http_request_timeout_ms, opts.allow_loopback, false, &failed) || key_bytes.size() != 16) {
                                    failed.store(true);
                                    break;
                                }
                                std::lock_guard<std::mutex> lk(mu);
                                key_cache[seg.key_uri] = key_bytes;
                            }
                        }

                        std::vector<uint8_t> iv_bytes(16, 0);
                        if (!seg.iv_hex.empty()) {
                            auto parsed_iv = HexToBytes(seg.iv_hex);
                            if (parsed_iv.size() >= 16) {
                                std::memcpy(iv_bytes.data(), parsed_iv.data() + (parsed_iv.size() - 16), 16);
                            } else if (!parsed_iv.empty()) {
                                std::memcpy(iv_bytes.data() + (16 - parsed_iv.size()), parsed_iv.data(), parsed_iv.size());
                            }
                        } else {
                            uint64_t seq = seg.index;
                            for (int b = 15; b >= 8; --b) {
                                iv_bytes[b] = static_cast<uint8_t>(seq & 0xFF);
                                seq >>= 8;
                            }
                        }

                        std::vector<uint8_t> decrypted;
                        if (!DecryptAes128Cbc(raw, key_bytes, iv_bytes, decrypted)) {
                            failed.store(true);
                            break;
                        }
                        raw = std::move(decrypted);
                    }

                    std::ofstream ofs(seg_path, std::ios::binary);
                    ofs.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
                    ofs.close();
                    seg_sz = raw.size();
                }

                if (opts.max_speed_kbps > 0) {
                    uint64_t max_bytes_per_sec = (opts.max_speed_kbps * 1000) / 8;
                    if (max_bytes_per_sec > 0) {
                        auto now_throttle = std::chrono::steady_clock::now();
                        double elapsed = std::chrono::duration<double>(now_throttle - start_time).count();
                        if (elapsed > 0.05) {
                            double current_rate = static_cast<double>(downloaded_bytes.load()) / elapsed;
                            if (current_rate > static_cast<double>(max_bytes_per_sec)) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                            }
                        }
                    }
                }

                {
                    std::lock_guard<std::mutex> lk(mu);
                    seg_files[idx] = seg_path;
                    size_t done = ++done_segs;
                    uint64_t tot_dl = (downloaded_bytes += seg_sz);
                    double pct = (static_cast<double>(done) * 100.0) / static_cast<double>(total_segs);
                    double dl_mb = static_cast<double>(tot_dl) / 1048576.0;
                    double est_tot_mb = dl_mb / std::max(0.01, pct / 100.0);
                    auto now = std::chrono::steady_clock::now();
                    double elapsed_sec = std::chrono::duration<double>(now - start_time).count();
                    double speed_mbps = (elapsed_sec > 0.1) ? (dl_mb / elapsed_sec) : 0.0;
                    std::string prefix = "Vid";
                    if (track.media_type == "AUDIO") prefix = "Aud";
                    else if (track.media_type == "SUBTITLES") prefix = "Sub";
                    std::cout << prefix << " " << (track.resolution.empty() ? track.language : track.resolution)
                              << " | " << (track.bandwidth / 1000) << " Kbps | "
                              << done << "/" << total_segs << " "
                              << std::fixed << std::setprecision(2) << pct << "% | "
                              << dl_mb << "MB/" << est_tot_mb << "MB | "
                              << speed_mbps << "MBps\n" << std::flush;
                }
            }
        });
    }

    for (auto& th : workers) {
        if (th.joinable()) th.join();
    }
    if (failed.load()) return false;

    std::string assembled_ext = (track.media_type == "SUBTITLES") ? ".vtt" : ".mp4";
    out_assembled = track_dir / ("assembled_" + track.id + assembled_ext);
    std::ofstream out_f(out_assembled, std::ios::binary);
    if (!out_f) return false;

    if (!init_file.empty() && std::filesystem::exists(init_file, ec)) {
        std::ifstream ifs(init_file, std::ios::binary);
        out_f << ifs.rdbuf();
    }

    if (track.media_type == "SUBTITLES") {
        bool first_sub = true;
        for (const auto& sf : seg_files) {
            std::ifstream ifs(sf, std::ios::binary);
            if (first_sub) {
                out_f << ifs.rdbuf();
                first_sub = false;
            } else {
                std::string sub_ln;
                bool skipped_hdr = false;
                while (std::getline(ifs, sub_ln)) {
                    std::string t = Trim(sub_ln);
                    if (!skipped_hdr && (t == "WEBVTT" || t.rfind("WEBVTT", 0) == 0 || t.rfind("X-TIMESTAMP-MAP", 0) == 0)) {
                        continue;
                    }
                    skipped_hdr = true;
                    out_f << sub_ln << "\n";
                }
            }
        }
    } else {
        for (const auto& sf : seg_files) {
            std::ifstream ifs(sf, std::ios::binary);
            out_f << ifs.rdbuf();
        }
    }
    out_f.close();
    return true;
}

static std::atomic<bool> g_live_stop_requested{false};

#ifdef _WIN32
static BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType) {
    if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_BREAK_EVENT || dwCtrlType == CTRL_CLOSE_EVENT) {
        g_live_stop_requested.store(true);
        return TRUE;
    }
    return FALSE;
}
#else
static void SignalHandler(int) {
    g_live_stop_requested.store(true);
}
#endif

bool DownloadAndAssembleLiveTrack(
    TrackItem& track,
    const WorkerOptions& opts,
    const std::filesystem::path& track_dir,
    std::filesystem::path& out_assembled) {

    std::error_code ec;
    std::filesystem::create_directories(track_dir, ec);

#ifdef _WIN32
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#else
    struct sigaction sa{};
    sa.sa_handler = SignalHandler;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
#endif

    std::filesystem::path init_file;
    if (!track.init_url.empty()) {
        init_file = track_dir / "_init.mp4";
        if (!std::filesystem::exists(init_file, ec)) {
            std::vector<uint8_t> ibytes;
            std::string rurl;
            if (FetchBytesWithRetry(track.init_url, opts.headers, ibytes, rurl, opts.download_retry_count, opts.http_request_timeout_ms, opts.allow_loopback)) {
                std::ofstream ofs(init_file, std::ios::binary);
                ofs.write(reinterpret_cast<const char*>(ibytes.data()), static_cast<std::streamsize>(ibytes.size()));
            }
        }
    }

    std::set<std::string> seen_urls;
    std::vector<std::filesystem::path> seg_files;
    double total_recorded_sec = 0.0;
    std::map<std::string, std::vector<uint8_t>> key_cache;
    size_t seg_file_counter = 0;

    std::cout << "[Live] Recording dynamic stream: " << track.id << " ("
              << (track.resolution.empty() ? track.language : track.resolution) << ")\n" << std::flush;

    while (!g_live_stop_requested.load()) {
        for (const auto& seg : track.segments) {
            if (seen_urls.count(seg.url)) continue;
            seen_urls.insert(seg.url);

            std::string ext = (track.media_type == "SUBTITLES") ? ".vtt" : ".ts";
            char name_buf[32];
            std::snprintf(name_buf, sizeof(name_buf), "live_seg_%05zu%s", seg_file_counter++, ext.c_str());
            std::filesystem::path seg_path = track_dir / name_buf;

            std::vector<uint8_t> raw;
            std::string rurl;
            if (!FetchBytesWithRetry(seg.url, opts.headers, raw, rurl, opts.download_retry_count, opts.http_request_timeout_ms, opts.allow_loopback, false, &g_live_stop_requested)) {
                continue;
            }

            if (seg.encrypted && seg.method == "AES_128") {
                std::vector<uint8_t> key_bytes;
                if (!opts.custom_hls_key.empty()) {
                    key_bytes = HexToBytes(opts.custom_hls_key);
                } else {
                    if (key_cache.count(seg.key_uri)) {
                        key_bytes = key_cache[seg.key_uri];
                    } else {
                        std::string krurl;
                        if (FetchBytesWithRetry(seg.key_uri, opts.headers, key_bytes, krurl, opts.download_retry_count, opts.http_request_timeout_ms, opts.allow_loopback, false, &g_live_stop_requested) && key_bytes.size() == 16) {
                            key_cache[seg.key_uri] = key_bytes;
                        }
                    }
                }
                if (key_bytes.size() == 16) {
                    std::vector<uint8_t> iv_bytes(16, 0);
                    if (!seg.iv_hex.empty()) {
                        auto parsed_iv = HexToBytes(seg.iv_hex);
                        if (parsed_iv.size() >= 16) {
                            std::memcpy(iv_bytes.data(), parsed_iv.data() + (parsed_iv.size() - 16), 16);
                        } else if (!parsed_iv.empty()) {
                            std::memcpy(iv_bytes.data() + (16 - parsed_iv.size()), parsed_iv.data(), parsed_iv.size());
                        }
                    } else {
                        uint64_t seq = seg.index;
                        for (int b = 15; b >= 8; --b) {
                            iv_bytes[b] = static_cast<uint8_t>(seq & 0xFF);
                            seq >>= 8;
                        }
                    }
                    std::vector<uint8_t> decrypted;
                    if (DecryptAes128Cbc(raw, key_bytes, iv_bytes, decrypted)) {
                        raw = std::move(decrypted);
                    }
                }
            }

            std::ofstream ofs(seg_path, std::ios::binary);
            ofs.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
            ofs.close();

            seg_files.push_back(seg_path);
            total_recorded_sec += seg.duration;

            std::cout << "[Live] Recorded " << std::fixed << std::setprecision(1) << total_recorded_sec
                      << "s (" << seg_files.size() << " segments)\n" << std::flush;

            if (opts.live_record_limit_sec > 0 && total_recorded_sec >= static_cast<double>(opts.live_record_limit_sec)) {
                break;
            }
        }

        if (opts.live_record_limit_sec > 0 && total_recorded_sec >= static_cast<double>(opts.live_record_limit_sec)) {
            break;
        }

        if (track.playlist_url.empty() || g_live_stop_requested.load()) {
            break;
        }

        // Poll interval: sleep up to 1.5s checking cancellation every 100ms
        for (int p = 0; p < 15; ++p) {
            if (g_live_stop_requested.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (g_live_stop_requested.load()) break;

        // Re-fetch playlist for newly arrived sliding window segments
        std::vector<uint8_t> pl_bytes;
        std::string pl_res_url;
        if (!FetchBytesWithRetry(track.playlist_url, opts.headers, pl_bytes, pl_res_url, opts.download_retry_count, opts.http_request_timeout_ms, opts.allow_loopback, false, &g_live_stop_requested)) {
            continue;
        }
        std::string pl_text(reinterpret_cast<const char*>(pl_bytes.data()), pl_bytes.size());
        bool still_live = true;
        TrackItem refreshed_track;
        refreshed_track.id = track.id;
        refreshed_track.media_type = track.media_type;
        refreshed_track.playlist_url = track.playlist_url;
        ParseHlsMediaPlaylist(pl_text, track.playlist_url, refreshed_track, still_live);
        track.segments = std::move(refreshed_track.segments);

        if (!still_live) {
            // #EXT-X-ENDLIST was reached; process any final segments then break
            for (const auto& seg : track.segments) {
                if (seen_urls.count(seg.url)) continue;
                seen_urls.insert(seg.url);

                std::string ext = (track.media_type == "SUBTITLES") ? ".vtt" : ".ts";
                char name_buf[32];
                std::snprintf(name_buf, sizeof(name_buf), "live_seg_%05zu%s", seg_file_counter++, ext.c_str());
                std::filesystem::path seg_path = track_dir / name_buf;

                std::vector<uint8_t> raw;
                std::string rurl;
                if (FetchBytesWithRetry(seg.url, opts.headers, raw, rurl, opts.download_retry_count, opts.http_request_timeout_ms, opts.allow_loopback, false, &g_live_stop_requested)) {
                    std::ofstream ofs(seg_path, std::ios::binary);
                    ofs.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
                    ofs.close();
                    seg_files.push_back(seg_path);
                    total_recorded_sec += seg.duration;
                }
            }
            break;
        }
    }

    if (seg_files.empty()) return false;

    std::string assembled_ext = (track.media_type == "SUBTITLES") ? ".vtt" : ".mp4";
    out_assembled = track_dir / ("assembled_" + track.id + assembled_ext);
    std::ofstream out_f(out_assembled, std::ios::binary);
    if (!out_f) return false;

    if (!init_file.empty() && std::filesystem::exists(init_file, ec)) {
        std::ifstream ifs(init_file, std::ios::binary);
        out_f << ifs.rdbuf();
    }

    for (const auto& sf : seg_files) {
        std::ifstream ifs(sf, std::ios::binary);
        out_f << ifs.rdbuf();
    }
    out_f.close();
    return true;
}

int RunFfmpegMux(const std::string& ffmpeg_bin, const std::vector<std::string>& args) {
#ifdef _WIN32
    std::ostringstream cmd;
    cmd << "\"" << ffmpeg_bin << "\"";
    for (const auto& a : args) {
        cmd << " \"" << a << "\"";
    }
    std::string cmd_s = cmd.str();
    std::wstring wcmd(cmd_s.begin(), cmd_s.end());
    STARTUPINFOW si{sizeof(STARTUPINFOW)};
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
        std::vector<char*> argv_ptrs;
        argv_ptrs.push_back(const_cast<char*>(ffmpeg_bin.c_str()));
        for (const auto& a : args) {
            argv_ptrs.push_back(const_cast<char*>(a.c_str()));
        }
        argv_ptrs.push_back(nullptr);
        execvp(ffmpeg_bin.c_str(), argv_ptrs.data());
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

} // namespace

int main(int argc, char* argv[]) {
#ifdef __GLIBC__
    mallopt(M_ARENA_MAX, 2);
#endif
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--version" || a == "-v") {
            std::cout << "N_m3u8DL-RE (Radium Stream Worker v0.3.0 / N_m3u8DL-RE v0.3.0-beta CLI contract)\n";
            return 0;
        }
        if (a == "--help" || a == "-h") {
            std::cout << "N_m3u8DL-RE v0.3.0 --auto-select --ffmpeg-binary-path --skip-download --write-meta-json\n";
            return 0;
        }
    }
    WorkerOptions opts = ParseWorkerArgs(argc, argv);
    if (opts.input.empty()) {
        std::cerr << "ERROR: Missing manifest input URL or file\n";
        return 1;
    }

    std::error_code ec;
    std::filesystem::create_directories(opts.tmp_dir, ec);

    ManifestMeta meta;
    if (!ExtractManifestInfo(opts, meta)) {
        std::cerr << "ERROR: Failed to parse manifest: " << opts.input << "\n";
        return 2;
    }

    if (opts.write_meta_json) {
        openidm::json::Value root = openidm::json::Object{};
        root["url"] = meta.url;
        root["isLive"] = meta.is_live;
        root["drmProtected"] = meta.drm_protected;
        root["drmSystem"] = meta.drm_system;
        openidm::json::Array tarr;
        for (const auto& tr : meta.tracks) {
            openidm::json::Value tobj = openidm::json::Object{};
            tobj["id"] = tr.id;
            tobj["mediaType"] = tr.media_type;
            tobj["groupId"] = tr.group_id;
            tobj["language"] = tr.language;
            tobj["name"] = tr.name;
            tobj["codecs"] = tr.codecs;
            tobj["resolution"] = tr.resolution;
            tobj["bandwidth"] = tr.bandwidth;
            tobj["frameRate"] = tr.frame_rate;
            tobj["segmentsCount"] = static_cast<int64_t>(tr.segments.size());
            tobj["durationSec"] = tr.duration_sec;
            tobj["encrypted"] = tr.encrypted;
            tobj["encryptionMethod"] = tr.encryption_method;
            tarr.push_back(std::move(tobj));
        }
        root["tracks"] = std::move(tarr);

        std::ofstream mofs(opts.tmp_dir / "meta.json", std::ios::binary);
        mofs << root.dump();
        mofs.close();
    }

    if (opts.skip_download) {
        return 0;
    }

    if (meta.drm_protected) {
        std::cerr << "ERROR: DRM protected stream (" << meta.drm_system << ") is not supported.\n";
        return 3;
    }

    auto sel_video = SelectTracks(meta.tracks, "VIDEO", opts.select_video);
    auto sel_audio = SelectTracks(meta.tracks, "AUDIO", opts.select_audio);
    auto sel_subs = SelectTracks(meta.tracks, "SUBTITLES", opts.select_subtitle);

    std::vector<TrackItem> chosen;
    if (!opts.drop_video) {
        chosen.insert(chosen.end(), sel_video.begin(), sel_video.end());
    }
    if (!opts.drop_audio) {
        chosen.insert(chosen.end(), sel_audio.begin(), sel_audio.end());
    }
    chosen.insert(chosen.end(), sel_subs.begin(), sel_subs.end());

    if (chosen.empty()) {
        std::cerr << "ERROR: No tracks matched selection criteria\n";
        return 4;
    }

    std::vector<std::pair<TrackItem, std::filesystem::path>> assembled_inputs;
    for (auto tr : chosen) {
        std::filesystem::path tdir = opts.tmp_dir / ("track_" + tr.id);
        std::filesystem::path assembled_path;
        bool ok = false;
        if (meta.is_live || opts.live_record_limit_sec > 0) {
            ok = DownloadAndAssembleLiveTrack(tr, opts, tdir, assembled_path);
        } else {
            ok = DownloadAndAssembleTrack(tr, opts, tdir, assembled_path);
        }
        if (!ok) {
            std::cerr << "ERROR: Failed to download segments for track " << tr.id << "\n";
            return 4;
        }
        assembled_inputs.push_back({tr, assembled_path});
    }

    std::cout << "Muxing selected tracks with FFmpeg...\n" << std::flush;
    std::filesystem::create_directories(opts.save_dir, ec);
    std::filesystem::path out_file = opts.save_dir / (opts.save_name + "." + opts.mux_format);

    std::vector<std::string> ff_args = {"-y", "-loglevel", "error"};
    for (const auto& [tr, apath] : assembled_inputs) {
        ff_args.push_back("-i");
        ff_args.push_back(apath.string());
    }
    for (size_t idx = 0; idx < assembled_inputs.size(); ++idx) {
        ff_args.push_back("-map");
        ff_args.push_back(std::to_string(idx));
    }

    size_t a_idx = 0;
    size_t s_idx = 0;
    bool has_subs = false;
    for (const auto& [tr, _] : assembled_inputs) {
        if (tr.media_type == "AUDIO") {
            if (!tr.language.empty() && tr.language != "und") {
                ff_args.push_back("-metadata:s:a:" + std::to_string(a_idx));
                ff_args.push_back("language=" + tr.language);
            }
            if (!tr.name.empty()) {
                ff_args.push_back("-metadata:s:a:" + std::to_string(a_idx));
                ff_args.push_back("title=" + tr.name);
            }
            ++a_idx;
        } else if (tr.media_type == "SUBTITLES") {
            has_subs = true;
            if (!tr.language.empty() && tr.language != "und") {
                ff_args.push_back("-metadata:s:s:" + std::to_string(s_idx));
                ff_args.push_back("language=" + tr.language);
            }
            if (!tr.name.empty()) {
                ff_args.push_back("-metadata:s:s:" + std::to_string(s_idx));
                ff_args.push_back("title=" + tr.name);
            }
            ++s_idx;
        }
    }

    if (opts.mux_format == "m4a") {
        ff_args.insert(ff_args.end(), {"-c:a", "copy", "-vn"});
    } else if (opts.mux_format == "mp3") {
        ff_args.insert(ff_args.end(), {"-c:a", "libmp3lame", "-q:a", "2", "-vn"});
    } else if (opts.mux_format == "mp4") {
        ff_args.insert(ff_args.end(), {"-c:v", "copy", "-c:a", "copy"});
        if (has_subs) {
            ff_args.insert(ff_args.end(), {"-c:s", "mov_text"});
        }
    } else if (opts.mux_format == "mkv") {
        ff_args.insert(ff_args.end(), {"-c:v", "copy", "-c:a", "copy"});
        if (has_subs) {
            ff_args.insert(ff_args.end(), {"-c:s", (opts.sub_format == "VTT" ? "webvtt" : "subrip")});
        }
    } else {
        ff_args.insert(ff_args.end(), {"-c", "copy"});
    }
    ff_args.push_back(out_file.string());

    int mux_rc = RunFfmpegMux(opts.ffmpeg_binary_path, ff_args);
    if (mux_rc != 0) {
        std::cerr << "ERROR: FFmpeg muxing failed with exit code " << mux_rc << "\n";
        return 5;
    }

    if (opts.del_after_done && !opts.live_keep_segments) {
        for (const auto& [tr, apath] : assembled_inputs) {
            std::filesystem::remove(apath, ec);
            std::filesystem::remove_all(opts.tmp_dir / ("track_" + tr.id), ec);
        }
    }

    std::cout << "Done: " << out_file.string() << "\n" << std::flush;
    return 0;
}
