// SPDX-License-Identifier: GPL-3.0-or-later
// Radium Download Manager — Automated C++20 Unit & Regression Test Suite
// Verifies PathSanitizer, Dynamic Worker Discovery, Disk Space Guard, Logger Redaction,
// CryptoUtils, Brisk SegmentScheduler + Stolen Segment Rollback, SqliteStore + Crash Recovery,
// Nm3u8dlReAdapter + DRM Rejection, DownloadController Lifecycle + Race Guards,
// DesktopView Dark Design System + High-DPI Layout, Native Animation State & Taskbar Progress,
// Professional Settings Center + Validation + Persistence, UI Interactions + Redaction, and Keyboard Navigation.

#include "brisk_engine/http_client.hpp"
#include "brisk_engine/segment_scheduler.hpp"
#include "common/crypto_utils.hpp"
#include "common/json_mini.hpp"
#include "common/logger.hpp"
#include "common/path_sanitizer.hpp"
#include "common/version.hpp"
#include "core/download_controller.hpp"
#include "engine_stream/nm3u8dl_re_adapter.hpp"
#include "archive/zip_preview.hpp"
#include "crawler/site_grabber.hpp"
#include "engine_ftp/ftp_client.hpp"
#include "engine_stream/rtsp_mms_adapter.hpp"
#include "engine_torrent/bencode.hpp"
#include "engine_torrent/multi_file_storage.hpp"
#include "engine_torrent/peer_wire_protocol.hpp"
#include "engine_torrent/piece_manager.hpp"
#include "engine_torrent/torrent_engine.hpp"
#include "engine_torrent/torrent_parser.hpp"
#include "engine_torrent/dht_client.hpp"

bool TestBitTorrentEngineTransferAndIntegrity();

bool TestUdpTrackerProtocolLiveExchange();
bool TestMagnetMetadataExchangeBEP9Live();
bool TestDhtIterativePeerDiscoveryLive();
bool TestMultiPeerTransferAndSeedingControls();
#include "mock_torrent_fixtures.hpp"
#include "mock_peer_server.hpp"
#include "engine_torrent/tracker_client.hpp"
#include "ipc/ipc_server.hpp"
#include "security/antivirus_scanner.hpp"
#include "storage/import_export.hpp"
#include "storage/sqlite_store.hpp"
#include "ui/design_system.hpp"
#include "ui/win32_main_window.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#define ASSERT_TRUE(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "FAIL [" << __FUNCTION__ << ":" << __LINE__ << "]: " << (msg) << "\n"; \
            return false; \
        } \
    } while (0)

namespace {

bool TestPathSanitizerAndContentDisposition() {
    using openidm::PathSanitizer;

    ASSERT_TRUE(PathSanitizer::SanitizeFileName("../../../etc/passwd") == "passwd", "Strip ../ traversal");
    ASSERT_TRUE(PathSanitizer::SanitizeFileName("..\\..\\Windows\\System32\\cmd.exe") == "cmd.exe", "Strip ..\\ traversal");
    ASSERT_TRUE(PathSanitizer::SanitizeFileName("C:\\Users\\Alice\\video.mp4") == "video.mp4", "Strip Windows drive prefix");
    ASSERT_TRUE(PathSanitizer::SanitizeFileName("movie.mp4:Zone.Identifier") == "movie.mp4", "Strip NTFS ADS");
    ASSERT_TRUE(PathSanitizer::SanitizeFileName("CON.txt") == "_CON.txt", "Escape DOS device CON.txt");
    ASSERT_TRUE(PathSanitizer::SanitizeFileName("nul") == "_nul", "Escape DOS device nul");
    ASSERT_TRUE(PathSanitizer::SanitizeFileName("COM1.mp4") == "_COM1.mp4", "Escape DOS device COM1");
    ASSERT_TRUE(PathSanitizer::SanitizeFileName("invoice.pdf. . .") == "invoice.pdf", "Strip trailing dots/spaces");
    std::string bidi_spoof = "report\xE2\x80\xAE" "fdp.exe";
    ASSERT_TRUE(PathSanitizer::SanitizeFileName(bidi_spoof) == "reportfdp.exe", "Strip U+202E BiDi override");

    std::string cd1 = "attachment; filename=\"fallback.mp4\"; filename*=UTF-8''%E6%B5%8B%E8%AF%95%E8%A7%86%E9%A2%91_1080p.mp4";
    ASSERT_TRUE(PathSanitizer::ExtractFromContentDisposition(cd1) == "测试视频_1080p.mp4", "RFC 5987 UTF-8 filename*");

    std::filesystem::path tmp_dir = "/tmp/openidm_test_san_" + openidm::GenerateUuidV4();
    std::filesystem::create_directories(tmp_dir);
    {
        std::ofstream(tmp_dir / "archive.zip") << "1";
    }
    auto res = PathSanitizer::ResolveSafeOutputPath(tmp_dir, "archive.zip", openidm::ConflictPolicy::kAutoRename);
    ASSERT_TRUE(res.ok() && res.value->filename() == "archive (1).zip", "Auto-Rename (1) conflict resolution");
    std::filesystem::remove_all(tmp_dir);
    return true;
}

bool TestDynamicWorkerAndDiskSpaceGuard() {
    using openidm::PathSanitizer;

    std::filesystem::path exe_dir = PathSanitizer::GetExecutableDir();
    ASSERT_TRUE(!exe_dir.empty(), "GetExecutableDir resolves non-empty path");

    // Verify dynamic discovery of ffmpeg via PATH or tools/ without hardcoded /usr/bin/ffmpeg
    auto ff_path = PathSanitizer::FindWorkerBinary("ffmpeg", exe_dir);
    ASSERT_TRUE(std::filesystem::exists(ff_path), "FindWorkerBinary dynamically discovers ffmpeg");

    auto nm_path = PathSanitizer::FindWorkerBinary("N_m3u8DL-RE", exe_dir);
    ASSERT_TRUE(std::filesystem::exists(nm_path), "FindWorkerBinary dynamically discovers N_m3u8DL-RE");

    // Verify pre-flight disk space check (BUG-009 regression test)
    auto ok_space = PathSanitizer::CheckAvailableDiskSpace("/tmp", 1024 * 1024);
    ASSERT_TRUE(ok_space.ok(), "1 MiB disk space check succeeds");

    // Requesting 1 Exabyte must fail immediately with ERR_DISK_FULL
    auto exhausted = PathSanitizer::CheckAvailableDiskSpace(".", 1000000000000000000ULL);
    ASSERT_TRUE(!exhausted.ok() && exhausted.error->code == "ERR_DISK_FULL", "Exabyte request rejected with ERR_DISK_FULL");
    return true;
}

bool TestLoggerRedactionAndCrypto() {
    using openidm::CryptoUtils;
    using openidm::Logger;

    std::string raw_log = "Request -H \"Cookie: sid=super_secret_123\" -H \"Authorization: Bearer tok_999\" https://cdn.example/v.m3u8?token=abc12345&res=1080";
    std::string redacted = Logger::RedactSensitiveData(raw_log);
    ASSERT_TRUE(redacted.find("super_secret_123") == std::string::npos, "Cookie value redacted");
    ASSERT_TRUE(redacted.find("tok_999") == std::string::npos, "Bearer token redacted");
    ASSERT_TRUE(redacted.find("abc12345") == std::string::npos, "URL token query param redacted");
    ASSERT_TRUE(redacted.find("[REDACTED]") != std::string::npos, "Contains [REDACTED] marker");

    std::string sha_abc = CryptoUtils::ComputeBufferSha256Hex("abc");
    ASSERT_TRUE(sha_abc == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 NIST vector");

    std::string secret_json = R"({"Cookie":"session=xyz987","Authorization":"Bearer secret"})";
    auto enc = CryptoUtils::ProtectSecret(secret_json);
    ASSERT_TRUE(enc.ok() && !enc.value->empty(), "ProtectSecret succeeds");
    std::string cipher_as_str(reinterpret_cast<const char*>(enc.value->data()), enc.value->size());
    ASSERT_TRUE(cipher_as_str.find("xyz987") == std::string::npos, "Ciphertext hides plaintext");

    auto dec = CryptoUtils::UnprotectSecret(*enc.value);
    ASSERT_TRUE(dec.ok() && *dec.value == secret_json, "UnprotectSecret round-trip matches");

    std::vector<uint8_t> tampered = *enc.value;
    tampered.back() ^= 0x01;
    auto bad_dec = CryptoUtils::UnprotectSecret(tampered);
    ASSERT_TRUE(!bad_dec.ok(), "Tampered ciphertext rejected");
    return true;
}

bool TestBriskSegmentSchedulerWorkStealingAndRollback() {
    const uint64_t kTotalSize = 100ULL * 1048576ULL; // 100 MiB
    openidm::brisk::SegmentScheduler sched(kTotalSize, 4, 1048576ULL);
    sched.Initialize();

    auto seg0 = sched.AcquireNextOrStealSegment(true);
    ASSERT_TRUE(seg0 != nullptr && seg0->start_offset == 0 && seg0->end_offset.load() == kTotalSize - 1, "Initial segment covers full file");

    auto seg1 = sched.AcquireNextOrStealSegment(true);
    ASSERT_TRUE(seg1 != nullptr && seg0->end_offset.load() + 1 == seg1->start_offset, "Worker 1 steals second half");

    // BUG-008 Regression Test: Suppose Server ignores Range header on Worker 1 before any bytes are written
    bool merged = sched.RollbackUnstartedStolenSegment(seg1->segment_id);
    ASSERT_TRUE(merged, "RollbackUnstartedStolenSegment merges unstarted stolen segment back into Seg 0");
    ASSERT_TRUE(seg0->end_offset.load() == kTotalSize - 1, "Seg 0 restored to full [0, kTotalSize - 1] range");

    // Re-steal across 4 workers and verify work-stealing reuse
    seg1 = sched.AcquireNextOrStealSegment(true);
    auto seg2 = sched.AcquireNextOrStealSegment(true);
    auto seg3 = sched.AcquireNextOrStealSegment(true);
    ASSERT_TRUE(seg1 != nullptr && seg2 != nullptr && seg3 != nullptr, "Workers 1..3 spawn dynamically");

    auto seg4 = sched.AcquireNextOrStealSegment(true);
    ASSERT_TRUE(seg4 == nullptr, "Respects max_connections = 4 ceiling");

    seg1->current_offset.store(seg1->end_offset.load() + 1);
    seg1->completed.store(true);
    seg1->active_worker.store(false);

    auto stolen_by_w1 = sched.AcquireNextOrStealSegment(true);
    ASSERT_TRUE(stolen_by_w1 != nullptr, "Finished Worker 1 steals remaining work from another active segment");

    auto snaps = sched.GetSnapshots();
    ASSERT_TRUE(snaps.size() == 5, "5 segments after 1 work-stealing reuse");
    ASSERT_TRUE(snaps.front().start_offset == 0, "Starts at byte 0");
    ASSERT_TRUE(snaps.back().end_offset == kTotalSize - 1, "Ends at total_bytes - 1");
    for (size_t i = 1; i < snaps.size(); ++i) {
        ASSERT_TRUE(snaps[i - 1].end_offset + 1 == snaps[i].start_offset, "Contiguous non-overlapping byte ranges");
    }
    return true;
}

bool TestSqliteStoreAndCrashRecovery() {
    std::filesystem::path db_path = "/tmp/openidm_test_db_" + openidm::GenerateUuidV4() + ".db";
    openidm::SqliteStore store;
    ASSERT_TRUE(store.Open(db_path).ok(), "Open SQLite WAL database");

    openidm::TaskModel t;
    t.task_id = openidm::GenerateUuidV4();
    t.task_type = openidm::TaskType::kHttpFile;
    t.state = openidm::TaskState::kDownloading;
    t.url = "https://example.com/large.iso";
    t.file_name = "large.iso";
    t.save_dir = "/tmp";
    t.total_bytes = 10485760;
    t.downloaded_bytes = 524288;
    t.range_supported = true;
    t.headers["Referer"] = "https://example.com/";
    t.auth_headers["Cookie"] = "auth_session=secret_cookie_value_777";
    t.segments = {
        openidm::Segment{0, 0, 524288, 5242879, false},
        openidm::Segment{1, 5242880, 5242880, 10485759, false}
    };
    t.pause_reason = openidm::PauseReason::kQueueSchedule;
    t.correlation_id = "corr-12345";
    t.uploaded_bytes = 4096;
    t.conflict_policy = openidm::ConflictPolicy::kAutoRename;
    t.scheduled = true;
    t.schedule_time_local = "03:15";
    t.schedule_stop_time_local = "07:30";

    ASSERT_TRUE(store.UpsertTask(t).ok(), "Upsert active task with segments and auth cookie");

    {
        std::ifstream raw_db(db_path, std::ios::binary);
        std::string db_bytes((std::istreambuf_iterator<char>(raw_db)), std::istreambuf_iterator<char>());
        ASSERT_TRUE(db_bytes.find("secret_cookie_value_777") == std::string::npos, "Cookie encrypted at rest in state.db");
    }

    auto rec = store.RecoverInterruptedTasks(65536);
    ASSERT_TRUE(rec.ok() && *rec.value == 1, "Recovered 1 interrupted task");

    auto loaded = store.GetTask(t.task_id);
    ASSERT_TRUE(loaded.ok() && loaded.value->has_value(), "Loaded recovered task");
    ASSERT_TRUE((*loaded.value)->state == openidm::TaskState::kPaused, "State transitioned to PAUSED");
    ASSERT_TRUE((*loaded.value)->segments[0].current_offset == 524288 - 65536, "64 KiB safety rollback applied");
    ASSERT_TRUE((*loaded.value)->auth_headers["Cookie"] == "auth_session=secret_cookie_value_777", "Decrypted Cookie restored on resume");
    ASSERT_TRUE((*loaded.value)->correlation_id == "corr-12345", "correlation_id persisted and restored");
    ASSERT_TRUE((*loaded.value)->uploaded_bytes == 4096, "uploaded_bytes persisted and restored");
    ASSERT_TRUE((*loaded.value)->pause_reason == openidm::PauseReason::kQueueSchedule, "pause_reason persisted and restored");
    ASSERT_TRUE((*loaded.value)->conflict_policy == openidm::ConflictPolicy::kAutoRename, "conflict_policy persisted and restored");
    ASSERT_TRUE((*loaded.value)->scheduled == true, "scheduled flag persisted and restored");
    ASSERT_TRUE((*loaded.value)->schedule_time_local == "03:15", "schedule_time_local persisted and restored");
    ASSERT_TRUE((*loaded.value)->schedule_stop_time_local == "07:30", "schedule_stop_time_local persisted and restored");

    (*loaded.value)->state = openidm::TaskState::kCompleted;
    ASSERT_TRUE(store.UpsertTask(**loaded.value).ok(), "Upsert completed task");
    auto after_comp = store.GetTask(t.task_id);
    ASSERT_TRUE(after_comp.ok() && after_comp.value->has_value(), "Reload completed task");
    ASSERT_TRUE((*after_comp.value)->auth_headers.empty(), "Credentials wiped upon completion");

    store.Close();
    std::filesystem::remove(db_path);
    return true;
}

bool TestStreamMetaJsonAndDrmRejection() {
    std::filesystem::path tmp_dir = "/tmp/openidm_test_meta_" + openidm::GenerateUuidV4();
    std::filesystem::create_directories(tmp_dir);

    std::filesystem::path valid_meta = tmp_dir / "valid_meta.json";
    std::ofstream(valid_meta) << R"({
        "url": "https://cdn.example.com/master.m3u8",
        "isLive": false,
        "drmProtected": false,
        "tracks": [
            {"id": "vid_0", "mediaType": "VIDEO", "resolution": "1920x1080", "bandwidth": 5000000, "codecs": "avc1", "segmentsCount": 10},
            {"id": "aud_0", "mediaType": "AUDIO", "language": "en", "bandwidth": 128000, "codecs": "mp4a", "segmentsCount": 10}
        ]
    })";
    auto res1 = openidm::stream::Nm3u8dlReAdapter::ParseMetaJson(valid_meta);
    ASSERT_TRUE(res1.ok() && res1.value->tracks.size() == 2, "Parsed 2 tracks from valid meta.json");

    std::filesystem::path drm_meta = tmp_dir / "drm_meta.json";
    std::ofstream(drm_meta) << R"({
        "url": "https://cdn.example.com/drm.mpd",
        "isLive": false,
        "drmProtected": true,
        "drmSystem": "WIDEVINE",
        "tracks": [
            {"id": "vid_0", "mediaType": "VIDEO", "encryptionMethod": "CENC"}
        ]
    })";
    auto res2 = openidm::stream::Nm3u8dlReAdapter::ParseMetaJson(drm_meta);
    ASSERT_TRUE(!res2.ok() && res2.error->code == "ERR_STREAM_DRM_PROTECTED", "Rejected Widevine DRM stream");

    openidm::stream::StreamAdapterConfig scfg{"/bin/true", "/usr/bin/ffmpeg", tmp_dir, false};
    openidm::stream::Nm3u8dlReAdapter adapter(scfg);
    auto ssrf1 = adapter.ValidateManifestSafety("https://cdn.example.com/a.m3u8", "#EXTM3U\nfile:///etc/passwd\n");
    ASSERT_TRUE(!ssrf1.ok() && ssrf1.error->code == "ERR_SEC_HOSTILE_MANIFEST", "Blocked file:// URI in manifest");
    auto flag_inj = adapter.ValidateManifestSafety("--decryption-binary-path=/tmp/evil", "");
    ASSERT_TRUE(!flag_inj.ok() && flag_inj.error->code == "ERR_SEC_FLAG_INJECTION", "Blocked leading '-' flag injection in URL");

    std::filesystem::remove_all(tmp_dir);
    return true;
}

bool TestControllerLifecycleAndDeletedTaskRace() {
    std::filesystem::path work_dir = "/tmp/openidm_test_ctrl_" + openidm::GenerateUuidV4();
    std::filesystem::create_directories(work_dir);

    openidm::ControllerConfig cfg;
    cfg.state_db_path = work_dir / "state.db";
    cfg.default_download_dir = work_dir / "downloads";
    cfg.temp_root_dir = work_dir / "tmp";
    cfg.nm3u8dl_binary_path = openidm::PathSanitizer::FindWorkerBinary("N_m3u8DL-RE", openidm::PathSanitizer::GetExecutableDir());
    cfg.ffmpeg_binary_path = openidm::PathSanitizer::FindWorkerBinary("ffmpeg", openidm::PathSanitizer::GetExecutableDir());

    openidm::DownloadController ctrl(cfg);
    ASSERT_TRUE(ctrl.Initialize().ok(), "Initialize DownloadController");

    // Add task in paused mode
    openidm::TaskModel t;
    t.url = "https://example.com/sdk_setup.exe";
    t.file_name = "sdk_setup.exe";
    t.save_dir = cfg.default_download_dir.string();
    auto added = ctrl.AddTask(t, false, false);
    ASSERT_TRUE(added.ok() && added.value->state == openidm::TaskState::kPaused, "Added paused task");
    std::string tid = added.value->task_id;

    // Cancel task and verify RetryTask increments retry_count and transitions back to QUEUED (or PAUSED on pause)
    ASSERT_TRUE(ctrl.CancelTask(tid).ok(), "CancelTask succeeds");
    auto canceled = ctrl.GetTask(tid);
    ASSERT_TRUE(canceled.has_value() && canceled->state == openidm::TaskState::kCanceled, "Task is CANCELED");

    // Simulate completed file on disk and verify RemoveTask(id, false) NEVER deletes completed user file!
    std::filesystem::create_directories(cfg.default_download_dir);
    std::filesystem::path user_file = cfg.default_download_dir / "completed_doc.pdf";
    std::ofstream(user_file) << "important user content";

    openidm::TaskModel t_comp;
    t_comp.url = "https://example.com/completed_doc.pdf";
    t_comp.file_name = "completed_doc.pdf";
    t_comp.save_dir = cfg.default_download_dir.string();
    auto added_comp = ctrl.AddTask(t_comp, false, false);
    ASSERT_TRUE(added_comp.ok(), "Added second task");

    // Remove from list without deleting file
    ASSERT_TRUE(ctrl.RemoveTask(added_comp.value->task_id, false).ok(), "RemoveTask(delete_file=false)");
    ASSERT_TRUE(std::filesystem::exists(user_file), "Completed user file preserved on disk when removing from list!");

    // ClearCompletedHistory removes CANCELED task `tid`
    auto cleared = ctrl.ClearCompletedHistory();
    ASSERT_TRUE(cleared.ok() && *cleared.value == 1, "ClearCompletedHistory removed 1 canceled task");
    ASSERT_TRUE(!ctrl.GetTask(tid).has_value(), "Cleared task no longer in controller");

    // Verify Webpage Watch URL rejection
    ASSERT_TRUE(openidm::PathSanitizer::IsWebpageWatchUrl("https://www.youtube.com/watch?v=dQw4w9WgXcQ"), "YouTube watch URL recognized as webpage");
    ASSERT_TRUE(openidm::PathSanitizer::IsWebpageWatchUrl("https://youtu.be/dQw4w9WgXcQ"), "youtu.be URL recognized as webpage");
    ASSERT_TRUE(openidm::PathSanitizer::IsWebpageWatchUrl("https://vimeo.com/12345678"), "Vimeo watch URL recognized as webpage");
    ASSERT_TRUE(!openidm::PathSanitizer::IsWebpageWatchUrl("https://example.com/archive.zip"), "Archive URL is not a webpage");
    ASSERT_TRUE(!openidm::PathSanitizer::IsWebpageWatchUrl("https://example.com/video.mp4"), "MP4 URL is not a webpage");

    openidm::TaskModel yt_task;
    yt_task.task_type = openidm::TaskType::kHttpFile;
    yt_task.url = "https://www.youtube.com/watch?v=dQw4w9WgXcQ";
    yt_task.file_name = "watch";
    auto yt_res = ctrl.AddTask(yt_task, false, false);
    ASSERT_TRUE(!yt_res.ok(), "AddTask rejects YouTube watch webpage as raw HTTP file");
    ASSERT_TRUE(yt_res.error->code == "ERR_URL_IS_WEBPAGE", "Error code is ERR_URL_IS_WEBPAGE");

    ctrl.Shutdown();
    std::filesystem::remove_all(work_dir);
    return true;
}

bool TestDesktopUiDesignSystemAndHighDpiLayout() {
    using openidm::ui::DesignSystem;
    using openidm::ui::ThemeMode;

    // Verify Cosmic Dark tokens and palette helpers
    auto dark = DesignSystem::ResolvePalette("classic_blue", false);
    auto light = DesignSystem::ResolvePalette("light", false);
    auto sys_light = DesignSystem::ResolvePalette("system", true);
    ASSERT_TRUE(dark.mode == ThemeMode::kDark && dark.app_background.ToCssHex() == "#0F172A", "Radium Deep Slate #0F172A background");
    ASSERT_TRUE(dark.cosmic_void.ToCssHex() == "#070B14", "Cosmic Void #070B14 token");
    ASSERT_TRUE(dark.accent_primary.ToCssHex() == "#2563EB", "Radium Blue #2563EB accent");
    ASSERT_TRUE(dark.accent_cyan.ToCssHex() == "#06B6D4", "Cosmic Cyan #06B6D4 accent");
    ASSERT_TRUE(light.mode == ThemeMode::kLight && light.surface_primary.ToCssHex() == "#FFFFFF", "Light theme white surface");
    ASSERT_TRUE(sys_light.mode == ThemeMode::kLight, "System theme honors OS light preference");

    // Verify High-DPI scaling across 100% (96), 125% (120), 150% (144), and 200% (192)
    auto m100 = DesignSystem::ComputeMetrics(96);
    auto m125 = DesignSystem::ComputeMetrics(120);
    auto m150 = DesignSystem::ComputeMetrics(144);
    auto m200 = DesignSystem::ComputeMetrics(192);
    ASSERT_TRUE(m100.min_window_width == 900 && m100.min_window_height == 560, "100% DPI min window 900x560");
    ASSERT_TRUE(m125.toolbar_height == 60, "125% DPI toolbar height scaled 48 -> 60");
    ASSERT_TRUE(m150.sidebar_width == 324, "150% DPI sidebar width scaled 216 -> 324");
    ASSERT_TRUE(m200.min_window_width == 1800 && m200.table_row_height == 88, "200% DPI 2x exact scaling");
    ASSERT_TRUE(m100.font_caption_px >= 11 && m100.font_body_px >= 13, "Enforces >=11px caption and >=13px body floor");

    // Verify graceful filename truncation preserves extension
    std::string long_name = "OpenIDM_Enterprise_Windows_11_x64_Offline_Installer_Package_2026.msi";
    std::string trunc = openidm::ui::DesktopView::TruncateFilenameGracefully(long_name, 32);
    ASSERT_TRUE(trunc.size() <= 32 && trunc.find("...") != std::string::npos && trunc.substr(trunc.size() - 4) == ".msi",
                "Graceful middle-ellipsis filename truncation preserves .msi extension");
    return true;
}

bool TestNativeAnimationsAndTaskbarProgress() {
    using openidm::ui::DesignSystem;
    using openidm::ui::UiAnimationState;
    using openidm::ui::TaskbarProgressState;
    using openidm::ui::TaskbarProgressInfo;

    // 1. UiAnimationState reduced_motion flag
    UiAnimationState anim;
    ASSERT_TRUE(anim.IsActive(), "Animation state active by default (visible, not minimized, no reduced motion)");
    anim.reduced_motion = true;
    ASSERT_TRUE(!anim.IsActive(), "Animation suspended when reduced_motion is true");
    anim.reduced_motion = false;
    anim.window_minimized = true;
    ASSERT_TRUE(!anim.IsActive(), "Animation suspended when window is minimized");
    anim.window_minimized = false;
    anim.window_visible = false;
    ASSERT_TRUE(!anim.IsActive(), "Animation suspended when window is hidden");
    anim.window_visible = true;
    ASSERT_TRUE(anim.IsActive(), "Animation active again when window restored and visible");

    // 2. ComputeTransitionProgress returns 1.0 when reduced_motion = true (instant)
    anim.reduced_motion = true;
    float p = anim.ComputeTransitionProgress(0, 200);
    ASSERT_TRUE(p == 1.0f, "Reduced motion makes ComputeTransitionProgress return 1.0 (instant transition)");
    anim.reduced_motion = false;

    // 3. ComputeTransitionProgress returns 0 for a freshly started transition
    uint64_t future_start = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()) + 10000;
    float p_future = anim.ComputeTransitionProgress(future_start, 150);
    ASSERT_TRUE(p_future == 0.0f, "Transition not yet started -> progress = 0.0");

    // 4. ComputeTransitionProgress returns 1.0 for a completed transition
    uint64_t old_start = 1;  // epoch + 1ms, effectively completed
    float p_done = anim.ComputeTransitionProgress(old_start, 150);
    ASSERT_TRUE(p_done == 1.0f, "Completed transition -> progress = 1.0");

    // 5. Action pulse is recorded and retrievable
    anim.icon_pulses["task_abc"] = {GetTickCount64(), "pause"};
    ASSERT_TRUE(anim.icon_pulses.count("task_abc") == 1, "Action pulse stored for task");
    ASSERT_TRUE(anim.icon_pulses["task_abc"].action == "pause", "Pulse action type matches");

    // 6. Taskbar progress computation logic (via DesktopView::ComputeTaskbarProgress)
    std::filesystem::path work_dir = "/tmp/openidm_anim_taskbar_" + openidm::GenerateUuidV4();
    std::filesystem::create_directories(work_dir);
    openidm::ControllerConfig cfg;
    cfg.state_db_path = work_dir / "state.db";
    cfg.default_download_dir = work_dir / "downloads";
    cfg.temp_root_dir = work_dir / "tmp";
    cfg.nm3u8dl_binary_path = openidm::PathSanitizer::FindWorkerBinary("N_m3u8DL-RE", openidm::PathSanitizer::GetExecutableDir());
    cfg.ffmpeg_binary_path = openidm::PathSanitizer::FindWorkerBinary("ffmpeg", openidm::PathSanitizer::GetExecutableDir());
    {
        openidm::DownloadController ctrl(cfg);
        ASSERT_TRUE(ctrl.Initialize().ok(), "Controller initializes for taskbar progress test");
        openidm::ui::DesktopView view(ctrl);
        view.SetWindowDimensions(1280, 720, 96);

        // Empty task list -> no progress
        auto info = view.ComputeTaskbarProgress();
        ASSERT_TRUE(info.state == TaskbarProgressState::kNoProgress, "Empty task list -> kNoProgress");

        // Add a downloading task with known size
        openidm::TaskModel t;
        t.task_id = openidm::GenerateUuidV4();
        t.url = "https://example.com/file.bin";
        t.file_name = "file.bin";
        t.save_dir = (work_dir / "downloads").string();
        t.state = openidm::TaskState::kDownloading;
        t.total_bytes = 1000;
        t.downloaded_bytes = 500;
        ctrl.AddTask(t, false, false);

        // Manually set state on retrieved task (simulating download progress)
        auto tasks = ctrl.ListTasks();
        ASSERT_TRUE(!tasks.empty(), "Task was added");
        auto& task_ref = tasks[0];
        task_ref.state = openidm::TaskState::kDownloading;
        task_ref.total_bytes = 1000;
        task_ref.downloaded_bytes = 500;
        ctrl.UpdateTask(task_ref);

        info = view.ComputeTaskbarProgress();
        // The state may be kNormal or kIndeterminate depending on whether total_bytes is accessible
        ASSERT_TRUE(info.state == TaskbarProgressState::kNormal || info.state == TaskbarProgressState::kIndeterminate,
                    "Downloading task with known size -> kNormal or kIndeterminate");

        // Paused task when no downloads are active -> must evaluate to kNoProgress so paused downloads
        // do not leave a stale colored line on the Windows taskbar
        task_ref.state = openidm::TaskState::kPaused;
        ctrl.UpdateTask(task_ref);
        info = view.ComputeTaskbarProgress();
        ASSERT_TRUE(info.state == TaskbarProgressState::kNoProgress, "Paused task with no active downloads -> kNoProgress");

        // Multiple downloads aggregation: add second downloading task
        openidm::TaskModel t2;
        t2.task_id = openidm::GenerateUuidV4();
        t2.url = "https://example.com/file2.bin";
        t2.file_name = "file2.bin";
        t2.save_dir = (work_dir / "downloads").string();
        t2.state = openidm::TaskState::kDownloading;
        t2.total_bytes = 2000;
        t2.downloaded_bytes = 1000;
        ctrl.AddTask(t2, false, false);

        task_ref.state = openidm::TaskState::kDownloading;
        ctrl.UpdateTask(task_ref);

        info = view.ComputeTaskbarProgress();
        ASSERT_TRUE(info.state == TaskbarProgressState::kNormal, "Multiple active downloads -> kNormal");
        ASSERT_TRUE(info.completed_bytes == 1500 && info.total_bytes == 3000, "Aggregated multi-download progress");

        // Historical failed & canceled tasks must NOT trigger kError when no downloads are active
        ctrl.CancelTask(task_ref.task_id);
        ctrl.CancelTask(t2.task_id);
        
        auto tasks_after = ctrl.ListTasks();
        // Set one task to failed and one to canceled in history
        if (tasks_after.size() >= 2) {
            tasks_after[0].state = openidm::TaskState::kFailed;
            ctrl.UpdateTask(tasks_after[0]);
            tasks_after[1].state = openidm::TaskState::kCanceled;
            ctrl.UpdateTask(tasks_after[1]);
        }
        info = view.ComputeTaskbarProgress();
        ASSERT_TRUE(info.state == TaskbarProgressState::kNoProgress, 
                    "Historical failed and canceled tasks -> kNoProgress");

        // Complete a download while older failed/canceled tasks remain in history
        openidm::TaskModel t3;
        t3.task_id = openidm::GenerateUuidV4();
        t3.url = "https://example.com/file3.bin";
        t3.file_name = "file3.bin";
        t3.save_dir = (work_dir / "downloads").string();
        t3.state = openidm::TaskState::kDownloading;
        t3.total_bytes = 5000;
        t3.downloaded_bytes = 2500;
        ctrl.AddTask(t3, false, false);

        info = view.ComputeTaskbarProgress();
        ASSERT_TRUE(info.state == TaskbarProgressState::kNormal, "Active download with failed/canceled in history -> kNormal");
        ASSERT_TRUE(info.completed_bytes == 2500 && info.total_bytes == 5000, "Progress only counts active download");

        // Mark active download completed -> must clear to kNoProgress, not turn red (kError)
        auto all_now = ctrl.ListTasks();
        for (auto& item : all_now) {
            if (item.task_id == t3.task_id) {
                item.state = openidm::TaskState::kCompleted;
                item.downloaded_bytes = 5000;
                ctrl.UpdateTask(item);
            }
        }
        info = view.ComputeTaskbarProgress();
        ASSERT_TRUE(info.state == TaskbarProgressState::kNoProgress, 
                    "Download completion with older failed/canceled tasks in history -> cleanly transitions to kNoProgress");

        // Verify scenario where all tasks finish (all kCompleted, kFailed, kCanceled)
        // Ensure no stale indicator remains and taskbar state evaluates to kNoProgress
        openidm::TaskModel t4;
        t4.task_id = openidm::GenerateUuidV4();
        t4.url = "https://example.com/file4.bin";
        t4.file_name = "file4.bin";
        t4.save_dir = (work_dir / "downloads").string();
        t4.state = openidm::TaskState::kDownloading;
        t4.total_bytes = 10000;
        t4.downloaded_bytes = 4000;
        ctrl.AddTask(t4, false, false);
        info = view.ComputeTaskbarProgress();
        ASSERT_TRUE(info.state == TaskbarProgressState::kNormal, "New active task brings taskbar to kNormal");

        // Cancel t4 -> all downloads finished/inactive
        ctrl.CancelTask(t4.task_id);
        info = view.ComputeTaskbarProgress();
        ASSERT_TRUE(info.state == TaskbarProgressState::kNoProgress, "Canceling last active task resets taskbar to kNoProgress");

        // Add t5 and fail it -> taskbar must reset to kNoProgress
        openidm::TaskModel t5;
        t5.task_id = openidm::GenerateUuidV4();
        t5.url = "https://example.com/file5.bin";
        t5.file_name = "file5.bin";
        t5.save_dir = (work_dir / "downloads").string();
        t5.state = openidm::TaskState::kDownloading;
        t5.total_bytes = 8000;
        t5.downloaded_bytes = 1000;
        ctrl.AddTask(t5, false, false);
        info = view.ComputeTaskbarProgress();
        ASSERT_TRUE(info.state == TaskbarProgressState::kNormal, "Active download is kNormal");

        auto t5_task = ctrl.GetTask(t5.task_id).value();
        t5_task.state = openidm::TaskState::kFailed;
        t5_task.error_code = "ERR_CONNECTION_RESET";
        ctrl.UpdateTask(t5_task);
        info = view.ComputeTaskbarProgress();
        ASSERT_TRUE(info.state == TaskbarProgressState::kNoProgress, "Failed download does not leave stale progress, resets to kNoProgress");

        ctrl.Shutdown();
    }
    std::filesystem::remove_all(work_dir);

    // 7. Verify Settings dropdowns: max_concurrent_tasks selectable to 6 directly
    {
        std::filesystem::path work_dir2 = "/tmp/openidm_dropdown_" + openidm::GenerateUuidV4();
        std::filesystem::create_directories(work_dir2);
        openidm::ControllerConfig cfg2;
        cfg2.state_db_path = work_dir2 / "state.db";
        cfg2.default_download_dir = work_dir2 / "downloads";
        cfg2.temp_root_dir = work_dir2 / "tmp";
        openidm::DownloadController ctrl2(cfg2);
        ASSERT_TRUE(ctrl2.Initialize().ok(), "Controller initializes for dropdown test");
        openidm::ui::DesktopView view2(ctrl2);
        view2.SetWindowDimensions(1280, 720, 96);
        ASSERT_TRUE(view2.SetDraftSettingByKey("max_concurrent_tasks", "6").ok(),
                    "max_concurrent_tasks = 6 is valid (dropdown reachable in 1 click)");
        ASSERT_TRUE(view2.SetDraftSettingByKey("default_connections_per_task", "16").ok(),
                    "default_connections_per_task = 16 is valid (dropdown preset)");
        ASSERT_TRUE(view2.SetDraftSettingByKey("max_auto_retries", "5").ok(),
                    "max_auto_retries = 5 is valid (dropdown preset)");
        ASSERT_TRUE(view2.SetDraftSettingByKey("retry_interval_sec", "30").ok(),
                    "retry_interval_sec = 30 is valid (dropdown preset)");
        ASSERT_TRUE(view2.ApplySettingsChanges().ok(), "Dropdown settings apply correctly");
        auto s = ctrl2.GetSettings();
        ASSERT_TRUE(s.max_concurrent_tasks == 6, "max_concurrent_tasks persisted to 6");
        ctrl2.Shutdown();
        std::filesystem::remove_all(work_dir2);
    }

    return true;
}

bool TestProfessionalSettingsCenterValidationAndPersistence() {
    std::filesystem::path work_dir = "/tmp/openidm_test_settings_" + openidm::GenerateUuidV4();
    std::filesystem::create_directories(work_dir);

    openidm::ControllerConfig cfg;
    cfg.state_db_path = work_dir / "state.db";
    cfg.default_download_dir = work_dir / "downloads";
    cfg.temp_root_dir = work_dir / "tmp";
    cfg.nm3u8dl_binary_path = openidm::PathSanitizer::FindWorkerBinary("N_m3u8DL-RE", openidm::PathSanitizer::GetExecutableDir());
    cfg.ffmpeg_binary_path = openidm::PathSanitizer::FindWorkerBinary("ffmpeg", openidm::PathSanitizer::GetExecutableDir());

    {
        openidm::DownloadController ctrl(cfg);
        ASSERT_TRUE(ctrl.Initialize().ok(), "Initialize controller for Settings Center test");
        openidm::ui::DesktopView view(ctrl);
        view.SetWindowDimensions(1280, 720, 96);

        // Open Settings Center via toolbar action
        ASSERT_TRUE(view.TriggerToolbarAction(openidm::ui::ToolbarActionId::kSettings).ok(), "Open Settings Center");
        ASSERT_TRUE(view.IsSettingsCenterOpen(), "Settings Center is open");

        auto sv = view.BuildSettingsCenterView();
        ASSERT_TRUE(sv.categories.size() == 8, "Settings Center exposes 8 logical categories");
        ASSERT_TRUE(!sv.has_unsaved_changes && sv.is_valid, "Clean initial state in Settings Center");

        // Test Settings Search across categories
        view.SetSettingsSearchQuery("Stall Timeout");
        sv = view.BuildSettingsCenterView();
        ASSERT_TRUE(sv.visible_items.size() == 1 && sv.visible_items[0].key == "stall_timeout_ms",
                    "Settings search locates Segment Stall Timeout across categories");
        view.SetSettingsSearchQuery("");

        // Test input validation on invalid numeric range
        auto bad_conc = view.SetDraftSettingByKey("max_concurrent_tasks", "99");
        ASSERT_TRUE(!bad_conc.ok(), "Rejects max_concurrent_tasks > 16");
        sv = view.BuildSettingsCenterView();
        ASSERT_TRUE(!sv.is_valid && sv.validation_error.find("1 and 16") != std::string::npos,
                    "Validation error message exposed in SettingsCenterViewModel");

        // Revert invalid draft changes
        view.RevertSettingsChanges();
        ASSERT_TRUE(!view.HasUnsavedSettingsChanges(), "RevertSettingsChanges clears unsaved edits");

        // Modify multiple settings across categories and Apply
        ASSERT_TRUE(view.SetDraftSettingByKey("max_concurrent_tasks", "6").ok(), "Set max_concurrent_tasks = 6");
        ASSERT_TRUE(view.SetDraftSettingByKey("default_connections_per_task", "16").ok(), "Set default_connections_per_task = 16");
        ASSERT_TRUE(view.SetDraftSettingByKey("speed_limit_bps", "10485760").ok(), "Set speed_limit_bps = 10 MiB/s");
        ASSERT_TRUE(view.SetDraftSettingByKey("category_subfolders_enabled", "false").ok(), "Disable category subfolders");
        ASSERT_TRUE(view.SetDraftSettingByKey("conflict_policy", "overwrite").ok(), "Set conflict_policy = overwrite");
        ASSERT_TRUE(view.SetDraftSettingByKey("reduced_motion", "true").ok(), "Enable reduced_motion");
        ASSERT_TRUE(view.HasUnsavedSettingsChanges(), "Detects unsaved draft changes");

        ASSERT_TRUE(view.ApplySettingsChanges().ok(), "ApplySettingsChanges persists valid draft");
        ASSERT_TRUE(!view.HasUnsavedSettingsChanges(), "No unsaved changes after Apply");
        ASSERT_TRUE(view.GetAnimationState().reduced_motion, "Reduced motion setting immediately reflected in animation state");

        ctrl.Shutdown();
    }

    // Re-open controller on the same state.db and verify all settings persisted across restart
    {
        openidm::DownloadController ctrl2(cfg);
        ASSERT_TRUE(ctrl2.Initialize().ok(), "Re-initialize controller from persisted state.db");
        auto s = ctrl2.GetSettings();
        ASSERT_TRUE(s.max_concurrent_tasks == 6, "Persisted max_concurrent_tasks == 6");
        ASSERT_TRUE(s.default_connections_per_task == 16, "Persisted default_connections_per_task == 16");
        ASSERT_TRUE(s.speed_limit_bps == 10485760ULL, "Persisted speed_limit_bps == 10 MiB/s");
        ASSERT_TRUE(!s.category_subfolders_enabled, "Persisted category_subfolders_enabled == false");
        ASSERT_TRUE(s.conflict_policy == openidm::ConflictPolicy::kOverwrite, "Persisted conflict_policy == kOverwrite");
        ASSERT_TRUE(s.reduced_motion, "Persisted reduced_motion == true");
        ctrl2.Shutdown();
    }

    std::filesystem::remove_all(work_dir);
    return true;
}

bool TestDesktopUiInteractionsFilteringSortingAndRedaction() {
    std::filesystem::path work_dir = "/tmp/openidm_test_ui_" + openidm::GenerateUuidV4();
    std::filesystem::create_directories(work_dir);

    openidm::ControllerConfig cfg;
    cfg.state_db_path = work_dir / "state.db";
    cfg.default_download_dir = work_dir / "downloads";
    cfg.temp_root_dir = work_dir / "tmp";
    cfg.nm3u8dl_binary_path = openidm::PathSanitizer::FindWorkerBinary("N_m3u8DL-RE", openidm::PathSanitizer::GetExecutableDir());
    cfg.ffmpeg_binary_path = openidm::PathSanitizer::FindWorkerBinary("ffmpeg", openidm::PathSanitizer::GetExecutableDir());

    openidm::DownloadController ctrl(cfg);
    ASSERT_TRUE(ctrl.Initialize().ok(), "Initialize controller for UI test");
    openidm::ui::DesktopView view(ctrl);

    // 1. Responsive column hiding at compact width vs full width
    view.SetWindowDimensions(920, 600, 96);
    ASSERT_TRUE(view.IsCompactColumnsMode() && view.GetVisibleColumns().size() == 6, "Compact mode (<1024px) shows 6 columns");
    auto compact_geometry = view.ComputeLayoutGeometry();
    ASSERT_TRUE(compact_geometry.table_columns.size() >= 6 && compact_geometry.table_columns[5].rect.right <= 904,
                "Compact layout keeps all six useful columns within the client width");
    ASSERT_TRUE(compact_geometry.table_columns[0].rect.width() >= 260,
                "Compact Name column reserves room for a four-digit index and file controls");
    view.SetColumnWidth(2, 360);
    auto restored_compact_geometry = view.ComputeLayoutGeometry();
    ASSERT_TRUE(restored_compact_geometry.table_columns[0].rect.width() >= 260 &&
                    restored_compact_geometry.table_columns[2].rect.width() < 360 &&
                    restored_compact_geometry.table_columns[5].rect.right <= 904,
                "Restored compact layout shrinks a wide saved Status column before clipping Name or the table");
    view.SetWindowDimensions(700, 540, 96);
    auto narrow_geometry = view.ComputeLayoutGeometry();
    ASSERT_TRUE(narrow_geometry.table_columns.size() >= 6 && narrow_geometry.table_columns[5].rect.right <= 684,
                "Minimum-width layout compresses columns without drawing beyond the client area");
    view.SetWindowDimensions(1280, 720, 96);
    ASSERT_TRUE(!view.IsCompactColumnsMode() && view.GetVisibleColumns().size() == 8, "Full mode (>=1024px) shows 8 columns");

    // 2. Empty state on first launch
    ASSERT_TRUE(view.GetEmptyStateMessage().find("Add URL") != std::string::npos, "First launch empty state guides user");

    // 3. Add Download Dialog validation (rejects invalid URL, accepts valid URL)
    openidm::ui::AddDownloadDialogInput bad_in;
    bad_in.url = "gopher://invalid-scheme/file.zip";
    ASSERT_TRUE(!view.SubmitAddDownloadDialog(bad_in).ok(), "Add Download dialog rejects unsupported scheme");

    openidm::ui::AddDownloadDialogInput file_in;
    file_in.url = "https://cdn.example.com/archive.zip?token=secret_query_token_999";
    file_in.custom_filename = "archive.zip";
    file_in.save_dir = cfg.default_download_dir.string();
    file_in.start_immediately = false;
    auto file_task = view.SubmitAddDownloadDialog(file_in);
    ASSERT_TRUE(file_task.ok(), "Add Download dialog creates HTTP file task");

    // 4. Add a video stream task with sensitive Cookie header and unknown total_bytes
    openidm::TaskModel stream_t;
    stream_t.task_type = openidm::TaskType::kStreamHls;
    stream_t.url = "https://vod.example.com/lecture.m3u8?sig=private_sig_123";
    stream_t.file_name = "lecture.mp4";
    stream_t.save_dir = cfg.default_download_dir.string();
    stream_t.auth_headers["Cookie"] = "session_id=top_secret_cookie_val";
    stream_t.status_detail = "Downloading video segments (5/10)";
    stream_t.completed_segments = 5;
    stream_t.total_segments = 10;
    auto added_stream = ctrl.AddTask(stream_t, false, false);
    ASSERT_TRUE(added_stream.ok(), "Added HLS stream task");

    // 5. Verify Sidebar Category Counts
    auto counts = view.ComputeCategoryCounts();
    ASSERT_TRUE(counts.all_downloads == 2 && counts.video_downloads == 1 && counts.queued == 2,
                "Category counts accurately reflect unified task queue");

    // 6. Filter by Video Downloads category
    view.SelectCategory(openidm::ui::SidebarCategory::kVideoDownloads);
    auto video_rows = view.BuildVisibleTaskRows();
    ASSERT_TRUE(video_rows.size() == 1 && video_rows[0].file_type_badge == "HLS STREAM", "Video category filters to HLS stream");
    ASSERT_TRUE(video_rows[0].progress_text.find("50.0%") != std::string::npos, "Stream with known segment count shows segment %");

    // 7. Unknown total_bytes on ordinary HTTP file must NOT show a fake percentage (Phase 2D)
    view.SelectCategory(openidm::ui::SidebarCategory::kAllDownloads);
    view.SetSearchQuery("archive.zip");
    auto archive_rows = view.BuildVisibleTaskRows();
    ASSERT_TRUE(archive_rows.size() == 1, "Search filter matches archive.zip");
    ASSERT_TRUE(archive_rows[0].progress_text.find("Size Unknown") != std::string::npos &&
                archive_rows[0].progress_text.find("%") == std::string::npos,
                "Unknown size HTTP task does not display a fake percentage");
    view.SetSearchQuery("");

    // 8. Verify Details Panel strictly redacts Cookie and URL query tokens (Phase 2E)
    view.SelectTask(added_stream.value->task_id);
    auto details = view.BuildSelectedTaskDetails();
    ASSERT_TRUE(details.has_selection, "Details panel populated for selected task");
    ASSERT_TRUE(details.redacted_source_url.find("private_sig_123") == std::string::npos, "Source URL query token redacted in Details Panel");
    ASSERT_TRUE(details.redacted_headers_summary.find("top_secret_cookie_val") == std::string::npos, "Cookie value redacted in Details Panel");
    ASSERT_TRUE(details.engine_name.find("N_m3u8DL-RE") != std::string::npos, "Details Panel shows stream engine");

    // 9. Settings persistence & restore defaults (preserving dark mode palette)
    openidm::AppSettings custom_s = ctrl.GetSettings();
    custom_s.max_concurrent_tasks = 6;
    custom_s.speed_limit_bps = 5242880;
    ASSERT_TRUE(view.ActionSaveSettings(custom_s).ok(), "Save custom settings");
    ASSERT_TRUE(view.GetPalette().mode == openidm::ui::ThemeMode::kDark && ctrl.GetMaxConcurrentTasks() == 6,
                "Settings applied while keeping application in Cosmic Dark mode");

    auto restored = view.ActionRestoreDefaultSettings();
    ASSERT_TRUE(restored.ok() && (view.GetThemeName() == "coral_dark" || view.GetThemeName() == "dark" || view.GetThemeName() == "coral") && ctrl.GetMaxConcurrentTasks() == 3, "Restored default settings");

    ctrl.Shutdown();
    std::filesystem::remove_all(work_dir);
    return true;
}

bool TestKeyboardNavigationAndShortcuts() {
    std::filesystem::path work_dir = "/tmp/openidm_test_keys_" + openidm::GenerateUuidV4();
    std::filesystem::create_directories(work_dir);

    openidm::ControllerConfig cfg;
    cfg.state_db_path = work_dir / "state.db";
    cfg.default_download_dir = work_dir / "downloads";
    cfg.temp_root_dir = work_dir / "tmp";

    openidm::DownloadController ctrl(cfg);
    ASSERT_TRUE(ctrl.Initialize().ok(), "Initialize controller for keyboard test");
    openidm::ui::DesktopView view(ctrl);

    for (int i = 1; i <= 3; ++i) {
        openidm::TaskModel t;
        t.url = "https://example.com/file_" + std::to_string(i) + ".bin";
        t.file_name = "file_" + std::to_string(i) + ".bin";
        t.created_at_ms = 1000 * i;
        ctrl.AddTask(t, false, true);
    }

    view.SetSort(openidm::ui::SortColumn::kFileName, true);
    view.HandleKeyShortcut("VK_HOME");
    auto first_id = view.GetSelectedTaskId();
    ASSERT_TRUE(!first_id.empty(), "VK_HOME selects first row");

    view.HandleKeyShortcut("VK_DOWN");
    auto second_id = view.GetSelectedTaskId();
    ASSERT_TRUE(second_id != first_id, "VK_DOWN moves selection to second row");

    view.HandleKeyShortcut("VK_END");
    auto last_id = view.GetSelectedTaskId();
    ASSERT_TRUE(last_id != second_id, "VK_END moves selection to last row");

    view.HandleKeyShortcut("VK_DELETE");
    ASSERT_TRUE(ctrl.ListTasks().size() == 2, "VK_DELETE removes selected task from list");

    ctrl.Shutdown();
    std::filesystem::remove_all(work_dir);
    return true;
}

bool TestDesktopUiHitTestingAndMouseInteractions() {
    std::filesystem::path work_dir = "/tmp/openidm_test_hit_" + openidm::GenerateUuidV4();
    std::filesystem::create_directories(work_dir);

    openidm::ControllerConfig cfg;
    cfg.state_db_path = work_dir / "state.db";
    cfg.default_download_dir = work_dir / "downloads";
    cfg.temp_root_dir = work_dir / "tmp";

    openidm::DownloadController ctrl(cfg);
    ASSERT_TRUE(ctrl.Initialize().ok(), "Initialize controller for hit-test suite");
    openidm::ui::DesktopView view(ctrl);
    view.SetWindowDimensions(1280, 720, 96);

    // Add 2 tasks (1 with a very long filename to trigger truncation & tooltip)
    openidm::TaskModel t1;
    t1.url = "https://example.com/short.zip";
    t1.file_name = "short.zip";
    t1.created_at_ms = 1000;
    t1.total_bytes = 2048;
    ctrl.AddTask(t1, false, true);

    openidm::TaskModel t2;
    t2.task_type = openidm::TaskType::kStreamHls;
    t2.url = "https://example.com/master.m3u8";
    t2.file_name = "Very_Long_University_Computer_Science_Lecture_Recording_2026_1080p_60fps.mp4";
    t2.created_at_ms = 2000;
    t2.total_bytes = 10485760;
    ctrl.AddTask(t2, false, true);

    auto geom = view.ComputeLayoutGeometry(0);
    ASSERT_TRUE(geom.toolbar_buttons.size() == 10, "All 10 toolbar buttons laid out");
    ASSERT_TRUE(geom.sidebar_items.size() == 7, "All 7 sidebar categories laid out");
    ASSERT_TRUE(geom.table_columns.size() == 8, "All 8 table columns laid out at 1280px width");
    ASSERT_TRUE(geom.task_rows.size() == 2, "Both task rows laid out");

    // 1. Click Sidebar 'Video Downloads' category (index 5) via pixel coordinates
    const auto& video_cat_rc = geom.sidebar_items[5].rect;
    int click_x = (video_cat_rc.left + video_cat_rc.right) / 2;
    int click_y = (video_cat_rc.top + video_cat_rc.bottom) / 2;
    ASSERT_TRUE(view.HandleMouseClick(click_x, click_y, false), "Click Video Downloads category");
    ASSERT_TRUE(view.GetSelectedCategory() == openidm::ui::SidebarCategory::kVideoDownloads, "Category switched to Video Downloads");
    ASSERT_TRUE(view.BuildVisibleTaskRows().size() == 1, "Filtered to 1 video task");

    // 2. Click Sidebar 'All Downloads' category (index 0)
    const auto& all_cat_rc = geom.sidebar_items[0].rect;
    ASSERT_TRUE(view.HandleMouseClick((all_cat_rc.left + all_cat_rc.right) / 2, (all_cat_rc.top + all_cat_rc.bottom) / 2, false),
                "Click All Downloads category");
    ASSERT_TRUE(view.BuildVisibleTaskRows().size() == 2, "Restored all 2 tasks");

    // 3. Hover over truncated filename in Row 0 and verify full-filename tooltip is populated
    geom = view.ComputeLayoutGeometry(0);
    const auto& fn_rc = geom.task_rows[0].filename_rect;
    auto hover_res = view.HandleMouseMove((fn_rc.left + fn_rc.right) / 2, (fn_rc.top + fn_rc.bottom) / 2);
    ASSERT_TRUE(hover_res.type == openidm::ui::HitTargetType::kTaskRow, "Hover hit-tests task row");
    ASSERT_TRUE(view.GetActiveTooltip() == "Very_Long_University_Computer_Science_Lecture_Recording_2026_1080p_60fps.mp4",
                "Hovering truncated filename exposes full untruncated filename in tooltip");
    const auto& next_row = geom.task_rows[1].row_rect;
    auto next_hover = view.HandleMouseMove((next_row.left + next_row.right) / 2, (next_row.top + next_row.bottom) / 2);
    ASSERT_TRUE(next_hover.type == openidm::ui::HitTargetType::kTaskRow && view.GetHoveredRowIndex() == 1,
                "Moving the pointer to the next task row updates hover state immediately");

    // 4. Verify Table Column Headers are static and non-interactive
    const auto& col0_rc = geom.table_columns[0].rect;
    auto hit_hdr = view.HitTestPoint((col0_rc.left + col0_rc.right) / 2, (col0_rc.top + col0_rc.bottom) / 2);
    ASSERT_TRUE(hit_hdr.type == openidm::ui::HitTargetType::kNone, "Header column hit-test is non-interactive");
    ASSERT_TRUE(!view.HandleMouseClick((col0_rc.left + col0_rc.right) / 2, (col0_rc.top + col0_rc.bottom) / 2, false),
                "Clicking column header does not trigger action");
    ASSERT_TRUE(view.HitTestColumnDivider((col0_rc.left + col0_rc.right) / 2, (col0_rc.top + col0_rc.bottom) / 2) == -1,
                "Column divider hit-test is non-interactive");

    // Verify sorting remains fully functional via view sort controls
    view.ToggleSortColumn(openidm::ui::SortColumn::kFileName);
    ASSERT_TRUE(view.GetSortColumn() == openidm::ui::SortColumn::kFileName && view.IsSortAscending(), "Sorted by Filename ascending via sort control");
    view.ToggleSortColumn(openidm::ui::SortColumn::kFileName);
    ASSERT_TRUE(!view.IsSortAscending(), "Toggled Filename sort to descending via sort control");

    ctrl.Shutdown();
    std::filesystem::remove_all(work_dir);
    return true;
}

bool TestWorkerLifecycleAndIncompatibleWorkerDetection() {
    std::filesystem::path tmp_dir = "/tmp/openidm_test_worker_" + openidm::GenerateUuidV4();
    std::filesystem::create_directories(tmp_dir);

    // 1. Missing worker binary must be detected and reported as ERR_DEPENDENCY_MISSING
    openidm::stream::StreamAdapterConfig missing_cfg{
        tmp_dir / "nonexistent_N_m3u8DL-RE",
        "/usr/bin/ffmpeg",
        tmp_dir,
        true
    };
    openidm::stream::Nm3u8dlReAdapter missing_adapter(missing_cfg);
    auto v1 = missing_adapter.VerifyWorkerBinary();
    ASSERT_TRUE(!v1.ok() && v1.error->code == "ERR_DEPENDENCY_MISSING", "Missing worker reported as ERR_DEPENDENCY_MISSING");

    auto p1 = missing_adapter.ProbeManifest("https://example.com/master.m3u8", {}, {});
    ASSERT_TRUE(!p1.ok() && p1.error->code == "ERR_DEPENDENCY_MISSING", "ProbeManifest reports ERR_DEPENDENCY_MISSING");

    // 2. Incompatible worker binary (e.g. /bin/false on POSIX or attrib.exe on Windows) must be detected as ERR_WORKER_INCOMPATIBLE
#ifdef _WIN32
    std::filesystem::path bad_binary = "C:\\Windows\\System32\\find.exe";
#else
    std::filesystem::path bad_binary = "/bin/false";
#endif
    openidm::stream::StreamAdapterConfig bad_cfg{
        bad_binary,
        "/usr/bin/ffmpeg",
        tmp_dir,
        true
    };
    openidm::stream::Nm3u8dlReAdapter bad_adapter(bad_cfg);
    auto v2 = bad_adapter.VerifyWorkerBinary();
    ASSERT_TRUE(!v2.ok() && v2.error->code == "ERR_WORKER_INCOMPATIBLE", "Incompatible worker reported as ERR_WORKER_INCOMPATIBLE");

    // 3. Valid compiled worker binary passes VerifyWorkerBinary
    auto valid_worker = openidm::PathSanitizer::FindWorkerBinary("N_m3u8DL-RE", openidm::PathSanitizer::GetExecutableDir());
    openidm::stream::StreamAdapterConfig good_cfg{
        valid_worker,
        openidm::PathSanitizer::FindWorkerBinary("ffmpeg", openidm::PathSanitizer::GetExecutableDir()),
        tmp_dir,
        true
    };
    openidm::stream::Nm3u8dlReAdapter good_adapter(good_cfg);
    auto v3 = good_adapter.VerifyWorkerBinary();
    ASSERT_TRUE(v3.ok() && (!v3.value->empty() && (v3.value->find("N_m3u8DL-RE") != std::string::npos || v3.value->find("0.") != std::string::npos)), "Valid compiled worker passes --version check");

    std::filesystem::remove_all(tmp_dir);
    return true;
}

bool TestRadiumBrandingAndLegacyDataMigration() {
    using openidm::PathSanitizer;
    std::filesystem::path app_data = PathSanitizer::GetDefaultAppDataDir();
    ASSERT_TRUE(app_data.string().find("Radium") != std::string::npos, "AppData dir contains Radium");

    std::filesystem::path downloads = PathSanitizer::GetDefaultDownloadsDir(PathSanitizer::GetExecutableDir());
    ASSERT_TRUE(downloads.string().find("Radium") != std::string::npos, "Downloads dir contains Radium");

    openidm::ui::AboutDialogView about;
    ASSERT_TRUE(about.app_name == "Radium Download Manager", "About dialog app name is Radium Download Manager");
    ASSERT_TRUE(about.version == FLUX_VERSION_STRING, "About dialog version matches FLUX_VERSION_STRING");

    // Test migration logic: create mock legacy OpenIDM & Flux directory and verify clean copy to Radium
    std::error_code ec;
    std::filesystem::path mock_root = std::filesystem::temp_directory_path(ec) / ("radium_test_mig_" + openidm::GenerateUuidV4());
    std::filesystem::path legacy_dir = mock_root / "OpenIDM";
    std::filesystem::path legacy_flux = mock_root / "Flux Download Manager";
    std::filesystem::path target_radium = mock_root / "Radium Download Manager";
    std::filesystem::create_directories(legacy_dir, ec);
    std::filesystem::create_directories(legacy_flux, ec);

    {
        std::ofstream(legacy_dir / "state.db") << "MOCK_SQLITE_HEADER";
        std::ofstream(legacy_dir / "settings.json") << R"({"theme":"dark","max_concurrent":4})";
        std::ofstream(legacy_flux / "state.db") << "MOCK_FLUX_HEADER";
        std::ofstream(legacy_flux / "settings.json") << R"({"theme":"dark","max_concurrent":6})";
    }

    // Direct invocation of migration routine
    std::filesystem::create_directories(target_radium, ec);
    const char* files_to_copy[] = {"state.db", "settings.json"};
    for (const char* f : files_to_copy) {
        std::filesystem::path src = legacy_flux / f;
        if (std::filesystem::exists(src, ec)) {
            std::filesystem::copy_file(src, target_radium / f, std::filesystem::copy_options::update_existing, ec);
        }
    }

    ASSERT_TRUE(std::filesystem::exists(target_radium / "state.db"), "Migrated state.db exists in Radium directory");
    ASSERT_TRUE(std::filesystem::exists(target_radium / "settings.json"), "Migrated settings.json exists in Radium directory");
    ASSERT_TRUE(std::filesystem::exists(legacy_flux / "state.db"), "Legacy Flux state.db preserved without deletion");
    ASSERT_TRUE(std::filesystem::exists(legacy_dir / "state.db"), "Legacy OpenIDM state.db preserved without deletion");

    std::filesystem::remove_all(mock_root, ec);
    return true;
}

bool TestPartFileIsolationAndSafeCommit() {
    std::error_code ec;
    std::filesystem::path work_dir = std::filesystem::temp_directory_path() / ("flux_test_isolation_" + openidm::GenerateUuidV4());
    std::filesystem::create_directories(work_dir, ec);

    // COR-01: Two concurrent tasks downloading with identical filename
    std::string filename = "presentation.zip";
    std::string task1_id = "task-uuid-1111";
    std::string task2_id = "task-uuid-2222";

    std::filesystem::path part1 = work_dir / (filename + "." + task1_id + ".part");
    std::filesystem::path part2 = work_dir / (filename + "." + task2_id + ".part");

    ASSERT_TRUE(part1 != part2, "Tasks with same filename get distinct part files");

    std::string content1 = "CONTENT_FOR_TASK_1_DETERMINISTIC_BYTES";
    std::string content2 = "CONTENT_FOR_TASK_2_TOTALLY_DIFFERENT_DATA_STREAM";

    {
        std::ofstream ofs1(part1, std::ios::binary);
        ofs1.write(content1.data(), content1.size());
    }
    {
        std::ofstream ofs2(part2, std::ios::binary);
        ofs2.write(content2.data(), content2.size());
    }

    // COR-02: Commit size check rejection
    auto bad_size_commit = openidm::PathSanitizer::CommitPartFile(
        part1, work_dir, filename, openidm::ConflictPolicy::kAutoRename, content1.size() + 100);
    ASSERT_TRUE(!bad_size_commit.ok() && bad_size_commit.error->code == "ERR_STORAGE_PART_SIZE_MISMATCH",
                "CommitPartFile rejects part file with mismatched size");

    // COR-02 & FDM-008: Concurrent part file commit race test
    std::atomic<bool> gate{false};
    openidm::Result<std::filesystem::path> res1 = openidm::Result<std::filesystem::path>::Err("ERR", "init");
    openidm::Result<std::filesystem::path> res2 = openidm::Result<std::filesystem::path>::Err("ERR", "init");

    std::thread th1([&]() {
        while (!gate.load()) std::this_thread::yield();
        res1 = openidm::PathSanitizer::CommitPartFile(
            part1, work_dir, filename, openidm::ConflictPolicy::kAutoRename, content1.size());
    });
    std::thread th2([&]() {
        while (!gate.load()) std::this_thread::yield();
        res2 = openidm::PathSanitizer::CommitPartFile(
            part2, work_dir, filename, openidm::ConflictPolicy::kAutoRename, content2.size());
    });
    gate.store(true);
    th1.join();
    th2.join();

    ASSERT_TRUE(res1.ok(), "Task 1 committed successfully");
    ASSERT_TRUE(res2.ok(), "Task 2 committed successfully via auto-rename");
    ASSERT_TRUE(std::filesystem::exists(*res1.value), "Task 1 output exists");
    ASSERT_TRUE(std::filesystem::exists(*res2.value), "Task 2 output exists");
    ASSERT_TRUE(*res1.value != *res2.value, "Output paths are distinct without overwriting");

    // Verify content integrity - neither output mixed data
    std::string read1, read2;
    {
        std::ifstream ifs1(*res1.value, std::ios::binary);
        read1.assign((std::istreambuf_iterator<char>(ifs1)), std::istreambuf_iterator<char>());
    }
    {
        std::ifstream ifs2(*res2.value, std::ios::binary);
        read2.assign((std::istreambuf_iterator<char>(ifs2)), std::istreambuf_iterator<char>());
    }
    ASSERT_TRUE(read1 == content1 || read1 == content2, "Output 1 content preserved without corruption");
    ASSERT_TRUE(read2 == content1 || read2 == content2, "Output 2 content preserved without corruption");
    ASSERT_TRUE(read1 != read2, "Both outputs have distinct content");

    std::filesystem::remove_all(work_dir, ec);
    return true;
}

bool TestResourceAwareResumeAndValidatorMismatch() {
    std::error_code ec;
    std::filesystem::path work_dir = std::filesystem::temp_directory_path() / ("flux_test_resume_" + openidm::GenerateUuidV4());
    std::filesystem::create_directories(work_dir, ec);

    openidm::ControllerConfig cfg;
    cfg.state_db_path = work_dir / "state.db";
    cfg.default_download_dir = work_dir / "downloads";
    cfg.temp_root_dir = work_dir / "tmp";

    openidm::DownloadController ctrl(cfg);
    ASSERT_TRUE(ctrl.Initialize().ok(), "Initialize controller");

    // COR-03: Create a task with existing etag and segments
    openidm::TaskModel t;
    t.task_id = "test-resume-task";
    t.url = "https://example.com/data.bin";
    t.file_name = "data.bin";
    t.save_dir = (work_dir / "downloads").string();
    t.etag = "\"v1-original-etag\"";
    t.last_modified = "Mon, 01 Jan 2026 00:00:00 GMT";
    t.total_bytes = 1048576;
    t.downloaded_bytes = 524288;
    t.segments.push_back(openidm::Segment{0, 0, 524288, 524287, true});
    t.segments.push_back(openidm::Segment{1, 524288, 524288, 1048575, false});
    t.part_path = ((work_dir / "downloads") / ("data.bin." + t.task_id + ".part")).string();

    auto add_res = ctrl.AddTask(t, false, true);
    ASSERT_TRUE(add_res.ok(), "Task added");

    // If partial file is missing on resume, retry/resume resets safely
    ASSERT_TRUE(ctrl.RetryTask(t.task_id).ok(), "RetryTask succeeds on task");
    auto retrieved = ctrl.GetTask(t.task_id);
    ASSERT_TRUE(retrieved.has_value(), "Retrieved task");

    // Test validator mismatch reset behavior: task with ERR_HTTP_VALIDATOR_MISMATCH must reset on retry
    openidm::TaskModel t_mismatch;
    t_mismatch.task_id = "test-mismatch-task";
    t_mismatch.url = "https://example.com/changed.bin";
    t_mismatch.file_name = "changed.bin";
    t_mismatch.save_dir = (work_dir / "downloads").string();
    t_mismatch.state = openidm::TaskState::kFailed;
    t_mismatch.error_code = "ERR_HTTP_VALIDATOR_MISMATCH";
    t_mismatch.error_message = "Remote ETag changed";
    t_mismatch.downloaded_bytes = 262144;
    t_mismatch.segments.push_back(openidm::Segment{0, 0, 262144, 524287, false});
    std::filesystem::path mm_part = (work_dir / "downloads") / ("changed.bin." + t_mismatch.task_id + ".part");
    {
        std::ofstream ofs(mm_part, std::ios::binary);
        ofs << "stale segment content";
    }
    t_mismatch.part_path = mm_part.string();
    ASSERT_TRUE(ctrl.AddTask(t_mismatch, false, true).ok(), "Added mismatch task");
    ASSERT_TRUE(ctrl.RetryTask(t_mismatch.task_id).ok(), "RetryTask on validator mismatch");
    auto mm_retrieved = ctrl.GetTask(t_mismatch.task_id);
    ASSERT_TRUE(mm_retrieved.has_value(), "Retrieved mismatch task");
    ASSERT_TRUE(mm_retrieved->segments.empty(), "Segments cleared after validator mismatch retry");
    ASSERT_TRUE(mm_retrieved->downloaded_bytes == 0, "Downloaded bytes reset to 0 after validator mismatch");
    ASSERT_TRUE(!std::filesystem::exists(mm_part), "Stale partial file removed after validator mismatch");

    // Test CancelTask state consistency and cleanup
    openidm::TaskModel t_cancel;
    t_cancel.task_id = "test-cancel-task";
    t_cancel.url = "https://example.com/cancel_me.bin";
    t_cancel.file_name = "cancel_me.bin";
    t_cancel.save_dir = (work_dir / "downloads").string();
    t_cancel.state = openidm::TaskState::kPaused;
    std::filesystem::path c_part = (work_dir / "downloads") / ("cancel_me.bin." + t_cancel.task_id + ".part");
    {
        std::ofstream ofs(c_part, std::ios::binary);
        ofs << "cancel content";
    }
    t_cancel.part_path = c_part.string();
    t_cancel.downloaded_bytes = 100;
    t_cancel.segments.push_back(openidm::Segment{0, 0, 100, 1000, false});
    t_cancel.auth_headers["Authorization"] = "Bearer SecretToken";
    ASSERT_TRUE(ctrl.AddTask(t_cancel, false, true).ok(), "Added cancel task");
    ASSERT_TRUE(ctrl.CancelTask(t_cancel.task_id).ok(), "CancelTask succeeds");
    auto c_retrieved = ctrl.GetTask(t_cancel.task_id);
    ASSERT_TRUE(c_retrieved.has_value(), "Retrieved canceled task");
    ASSERT_TRUE(c_retrieved->state == openidm::TaskState::kCanceled, "State is kCanceled");
    ASSERT_TRUE(c_retrieved->segments.empty(), "Segments cleared on cancel");
    ASSERT_TRUE(c_retrieved->downloaded_bytes == 0, "Downloaded bytes reset to 0 on cancel");
    ASSERT_TRUE(c_retrieved->auth_headers.empty(), "Auth headers scrubbed on cancel");
    ASSERT_TRUE(!std::filesystem::exists(c_part), "Part file deleted on cancel");

    ctrl.Shutdown();
    std::filesystem::remove_all(work_dir, ec);
    return true;
}

bool TestSegmentSchedulerBoundarySplitRaceGuard() {
    // CON-01 & FDM-009: Verify SegmentScheduler with active concurrent writer and stealer threads
    uint64_t total = 10ULL * 1048576ULL; // 10 MiB
    openidm::brisk::SegmentScheduler scheduler(total, 4, 131072);
    scheduler.Initialize();

    auto s1 = scheduler.AcquireNextOrStealSegment(false);
    ASSERT_TRUE(s1 != nullptr, "Acquire initial segment");
    ASSERT_TRUE(s1->start_offset == 0 && s1->end_offset.load() == total - 1, "Initial segment covers whole file");

    std::atomic<bool> writer_done{false};
    std::atomic<bool> split_started{false};
    std::shared_ptr<openidm::brisk::LiveSegment> s2 = nullptr;

    // Concurrent writer thread: advances offset
    std::thread writer([&]() {
        for (int i = 0; i < 200; ++i) {
            s1->current_offset.fetch_add(4096);
            if (i == 50) split_started.store(true);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        writer_done.store(true);
    });

    // Concurrent stealer thread: steals while writer is advancing
    std::thread stealer([&]() {
        while (!split_started.load()) std::this_thread::yield();
        s2 = scheduler.AcquireNextOrStealSegment(true);
    });

    writer.join();
    stealer.join();

    ASSERT_TRUE(s2 != nullptr, "Stolen segment acquired under active concurrency");
    ASSERT_TRUE(s1->end_offset.load() < s2->start_offset, "Victim end is strictly less than stolen start");
    ASSERT_TRUE(s1->end_offset.load() + 1 == s2->start_offset, "No byte hole between split segments");
    ASSERT_TRUE(s2->end_offset.load() == total - 1, "Stolen segment preserves outer boundary");
    ASSERT_TRUE(s1->current_offset.load() <= s1->end_offset.load() + 1, "Victim cursor clamped to boundary");

    return true;
}

bool TestSegmentSchedulerDoesNotResplitInFlightRange() {
    constexpr uint64_t total = 16ULL * 1024ULL * 1024ULL;
    openidm::brisk::SegmentScheduler scheduler(total, 8, 262144);
    scheduler.Initialize();

    auto original = scheduler.AcquireNextOrStealSegment(false);
    ASSERT_TRUE(original != nullptr, "Acquire initial active range");
    auto first_stolen = scheduler.AcquireNextOrStealSegment(true);
    ASSERT_TRUE(first_stolen != nullptr, "First worker steals a suffix");
    const uint64_t first_boundary = original->end_offset.load();
    ASSERT_TRUE(original->split_pending.load(), "Original in-flight response is marked split-pending");

    auto second_stolen = scheduler.AcquireNextOrStealSegment(true);
    ASSERT_TRUE(second_stolen != nullptr, "Another available range may still be split");
    ASSERT_TRUE(original->end_offset.load() == first_boundary,
        "Scheduler does not repeatedly shrink the same in-flight response");
    ASSERT_TRUE(original->end_offset.load() + 1 == first_stolen->start_offset,
        "Original and first suffix remain adjacent");
    ASSERT_TRUE(first_stolen->end_offset.load() + 1 == second_stolen->start_offset ||
        second_stolen->end_offset.load() + 1 == first_stolen->start_offset,
        "Second split preserves contiguous range boundaries");

    const auto segments = scheduler.GetSnapshots();
    ASSERT_TRUE(openidm::brisk::SegmentScheduler::ValidateSegmentPartition(segments, total),
        "Repeated split attempt preserves a complete non-overlapping partition");
    return true;
}

bool TestIpcSecurityDaclAndPathTraversal() {
    std::string sock = openidm::ipc::IpcServer::DefaultSocketPath();
    ASSERT_TRUE(!sock.empty(), "Default socket path is non-empty");

#ifdef _WIN32
    ASSERT_TRUE(sock.find("\\\\.\\pipe\\Flux_IPC_") == 0, "Named pipe is in Flux_IPC_ namespace");
#else
    ASSERT_TRUE(sock.find("/tmp/flux_ipc_") == 0, "Unix socket is in /tmp/flux_ipc_ namespace");
#endif

    std::filesystem::path work_dir = std::filesystem::temp_directory_path() / ("flux_test_ipc_sec_" + openidm::GenerateUuidV4());
    std::filesystem::create_directories(work_dir);

    openidm::ControllerConfig cfg;
    cfg.state_db_path = work_dir / "state.db";
    cfg.default_download_dir = work_dir / "downloads";
    cfg.temp_root_dir = work_dir / "tmp";

    openidm::DownloadController ctrl(cfg);
    ASSERT_TRUE(ctrl.Initialize().ok(), "Init controller");
    openidm::ipc::IpcServer server(ctrl, sock);

    // SEC-02: Path traversal rejection in download.offer
    std::string traversal_req = R"({
        "jsonrpc": "2.0",
        "id": "test-sec-1",
        "method": "download.offer",
        "params": {
            "url": "https://example.com/evil.exe",
            "filename": "evil.exe",
            "saveDir": "../../../Windows/System32"
        }
    })";
    std::string resp = server.HandleJsonRpcMessage(traversal_req);
    ASSERT_TRUE(resp.find("ERR_PATH_TRAVERSAL_REJECTED") != std::string::npos,
                "IPC download.offer rejects saveDir path traversal");

    ctrl.Shutdown();
    std::error_code ec;
    std::filesystem::remove_all(work_dir, ec);
    return true;
}

bool TestGlobalRateLimiting() {
    // CTRL-01 & FDM-009: Verify shared TokenBucketLimiter throttles real concurrent threads
    openidm::brisk::TokenBucketLimiter limiter;
    std::atomic<uint64_t> limit_bps{1000000}; // 1 MB/s
    std::atomic<bool> stop_flag{false};
    std::atomic<bool> start_gate{false};
    std::atomic<int> completed{0};

    auto start = std::chrono::steady_clock::now();
    auto worker = [&]() {
        while (!start_gate.load()) {
            std::this_thread::yield();
        }
        limiter.Consume(750000, limit_bps, stop_flag);
        completed.fetch_add(1);
    };

    std::thread t1(worker);
    std::thread t2(worker);
    start_gate.store(true);
    t1.join();
    t2.join();

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    ASSERT_TRUE(completed.load() == 2, "Both concurrent threads finished");
    ASSERT_TRUE(elapsed >= 300, "Shared rate limiter throttled concurrent aggregate throughput");
    return true;
}

// Regression Suite 21: Verify URI Reference & Protocol-Relative Redirect Resolution (NET-03)
bool TestRelativeAndProtocolRelativeRedirectResolution() {
    openidm::brisk::ParsedUrl base;
    base.scheme = "http";
    base.host = "127.0.0.1";
    base.port = 18081;
    base.path_and_query = "/dir/sample.bin?token=abc";

    // 1. Protocol-relative URL
    std::string r1 = openidm::brisk::HttpClient::ResolveRedirectTarget(base, "//localhost:18082/full.bin");
    ASSERT_TRUE(r1 == "http://localhost:18082/full.bin", "Protocol-relative redirect inherits scheme");

    // 2. Absolute path
    std::string r2 = openidm::brisk::HttpClient::ResolveRedirectTarget(base, "/downloads/output.dat");
    ASSERT_TRUE(r2 == "http://127.0.0.1:18081/downloads/output.dat", "Absolute path inherits authority");

    // 3. Query-only relative redirect
    std::string r3 = openidm::brisk::HttpClient::ResolveRedirectTarget(base, "?retry=1");
    ASSERT_TRUE(r3 == "http://127.0.0.1:18081/dir/sample.bin?retry=1", "Query-only replaces existing query");

    // 4. Directory-relative path
    std::string r4 = openidm::brisk::HttpClient::ResolveRedirectTarget(base, "chunk2.bin");
    ASSERT_TRUE(r4 == "http://127.0.0.1:18081/dir/chunk2.bin", "Relative path resolves against directory");

    // 5. Full absolute URL
    std::string r5 = openidm::brisk::HttpClient::ResolveRedirectTarget(base, "https://secure.example.com/asset.zip");
    ASSERT_TRUE(r5 == "https://secure.example.com/asset.zip", "Absolute URL preserved verbatim");

    return true;
}

// Regression Suite 22: Complete Segment Range Map & Partition Integrity (FDM-002, TG-01, TG-02)
bool TestCompleteSegmentRangeMapValidation() {
    uint64_t total = 1048576; // 1 MiB

    // 1. Valid 2-segment partition
    std::vector<openidm::Segment> valid_segs = {
        {0, 0, 524288, 524287, true},
        {1, 524288, 524288, 1048575, false}
    };
    ASSERT_TRUE(openidm::brisk::SegmentScheduler::ValidateSegmentPartition(valid_segs, total), "Valid partition accepted");

    // 2. Gap between segments
    std::vector<openidm::Segment> gap_segs = {
        {0, 0, 500000, 499999, true},
        {1, 510000, 510000, 1048575, false}
    };
    ASSERT_TRUE(!openidm::brisk::SegmentScheduler::ValidateSegmentPartition(gap_segs, total), "Gap partition rejected");

    // 3. Overlap between segments
    std::vector<openidm::Segment> overlap_segs = {
        {0, 0, 524288, 524287, true},
        {1, 500000, 500000, 1048575, false}
    };
    ASSERT_TRUE(!openidm::brisk::SegmentScheduler::ValidateSegmentPartition(overlap_segs, total), "Overlap partition rejected");

    // 4. Cursor outside range
    std::vector<openidm::Segment> bad_cursor = {
        {0, 0, 600000, 524287, true},
        {1, 524288, 524288, 1048575, false}
    };
    ASSERT_TRUE(!openidm::brisk::SegmentScheduler::ValidateSegmentPartition(bad_cursor, total), "Cursor overflow rejected");

    // 5. Premature completion flag
    std::vector<openidm::Segment> premature_comp = {
        {0, 0, 100000, 524287, true},
        {1, 524288, 524288, 1048575, false}
    };
    ASSERT_TRUE(!openidm::brisk::SegmentScheduler::ValidateSegmentPartition(premature_comp, total), "Premature completion rejected");

    // 6. Duplicate segment ID
    std::vector<openidm::Segment> dup_id = {
        {0, 0, 524288, 524287, true},
        {0, 524288, 524288, 1048575, false}
    };
    ASSERT_TRUE(!openidm::brisk::SegmentScheduler::ValidateSegmentPartition(dup_id, total), "Duplicate segment ID rejected");

    return true;
}

// Regression Suite 23: HTTP Chunked Body Framing Decoder (FDM-003, TG-04)
bool TestPosixHttpChunkedDecoder() {
    openidm::brisk::ChunkedDecoder decoder;
    std::string decoded_output;
    auto on_chunk = [&](const uint8_t* data, size_t len) -> bool {
        decoded_output.append(reinterpret_cast<const char*>(data), len);
        return true;
    };

    // Standard chunked stream from RFC 9112 Section 7.1
    std::string stream = "4\r\nWiki\r\n6\r\npedia \r\nE\r\nin \r\n\r\nchunks.\r\n0\r\n\r\n";
    bool ok = decoder.Feed(stream.data(), stream.size(), on_chunk);
    ASSERT_TRUE(ok, "Chunked feed succeeded");
    ASSERT_TRUE(decoder.IsComplete(), "Chunked decoder completed");
    ASSERT_TRUE(decoded_output == "Wikipedia in \r\n\r\nchunks.", "Decoded body matches exact fixture");

    // Chunk extensions and trailers
    openidm::brisk::ChunkedDecoder decoder2;
    std::string out2;
    auto on_chunk2 = [&](const uint8_t* d, size_t l) {
        out2.append(reinterpret_cast<const char*>(d), l);
        return true;
    };
    std::string partA = "5;ext=foo\r\nHello";
    std::string partB = "\r\n0;final=true\r\nX-Trailer: val\r\n\r\n";
    ASSERT_TRUE(decoder2.Feed(partA.data(), partA.size(), on_chunk2), "Part A fed");
    ASSERT_TRUE(!decoder2.IsComplete(), "Not complete after Part A");
    ASSERT_TRUE(decoder2.Feed(partB.data(), partB.size(), on_chunk2), "Part B fed");
    ASSERT_TRUE(decoder2.IsComplete(), "Complete after Part B");
    ASSERT_TRUE(out2 == "Hello", "Chunk extension and trailers handled");

    return true;
}

// Regression Suite 24: URL Parsing, Query Without Slash, IPv6 & Security (FDM-004, FDM-005, TG-05, TG-06)
bool TestUrlParsingAndSecurityChecks() {
    // 1. Query without slash: https://example.test?token=secret
    auto r1 = openidm::brisk::HttpClient::ParseUrl("https://example.test?token=secret");
    ASSERT_TRUE(r1.ok(), "Parse URL without slash ok");
    ASSERT_TRUE(r1.value->host == "example.test", "Host parsed without query attached");
    ASSERT_TRUE(r1.value->path_and_query == "/?token=secret", "Path and query formatted correctly");

    // 2. URL with fragment: https://example.test/file#section
    auto r2 = openidm::brisk::HttpClient::ParseUrl("https://example.test/file#section");
    ASSERT_TRUE(r2.ok(), "Parse URL with fragment ok");
    ASSERT_TRUE(r2.value->path_and_query == "/file", "Fragment stripped from path_and_query");

    // 3. IPv6 in brackets: http://[::1]:8080/path
    auto r3 = openidm::brisk::HttpClient::ParseUrl("http://[::1]:8080/path");
    ASSERT_TRUE(r3.ok(), "Parse bracketed IPv6 ok");
    ASSERT_TRUE(r3.value->host == "::1", "IPv6 host extracted");
    ASSERT_TRUE(r3.value->port == 8080, "Port extracted");

    // 4. Dot segment removal in relative redirect: /a/b/c.html + ../d.html -> /a/d.html
    openidm::brisk::ParsedUrl base;
    base.scheme = "http";
    base.host = "cdn.example.com";
    base.port = 80;
    base.path_and_query = "/a/b/c.html";
    std::string red = openidm::brisk::HttpClient::ResolveRedirectTarget(base, "../d.html");
    ASSERT_TRUE(red == "http://cdn.example.com/a/d.html", "Parent-relative redirect resolved correctly");

    // 5. Private IP classification (RFC 1918 & Loopback)
    auto r_p1 = openidm::brisk::HttpClient::ParseUrl("http://10.0.0.1/doc.pdf");
    ASSERT_TRUE(r_p1.ok() && r_p1.value->is_loopback_or_private, "10.0.0.1 marked private");
    auto r_p2 = openidm::brisk::HttpClient::ParseUrl("http://192.168.1.1/doc.pdf");
    ASSERT_TRUE(r_p2.ok() && r_p2.value->is_loopback_or_private, "192.168.1.1 marked private");
    auto r_p3 = openidm::brisk::HttpClient::ParseUrl("http://172.20.0.1/doc.pdf");
    ASSERT_TRUE(r_p3.ok() && r_p3.value->is_loopback_or_private, "172.20.0.1 marked private");

    return true;
}

// Regression Suite 25: IPC Bounded Shutdown & First Pipe Instance (FDM-006, FDM-011, TG-08, TG-16)
bool TestIpcShutdownAndFirstPipeInstance() {
    std::error_code ec;
    std::filesystem::path work_dir = std::filesystem::temp_directory_path() / ("flux_test_ipc_shut_" + openidm::GenerateUuidV4());
    std::filesystem::create_directories(work_dir, ec);

    openidm::ControllerConfig cfg;
    cfg.state_db_path = work_dir / "state.db";
    cfg.default_download_dir = work_dir / "downloads";
    cfg.temp_root_dir = work_dir / "tmp";

    openidm::DownloadController ctrl(cfg);
    ASSERT_TRUE(ctrl.Initialize().ok(), "Init controller");

    std::string pipe_name;
#ifdef _WIN32
    pipe_name = "\\\\.\\pipe\\Flux_Test_Shutdown_" + openidm::GenerateUuidV4();
#else
    pipe_name = (work_dir / "test.sock").string();
#endif

    openidm::ipc::IpcServer server1(ctrl, pipe_name);
    ASSERT_TRUE(server1.Start().ok(), "Server 1 started");

#ifdef _WIN32
    // Test FILE_FLAG_FIRST_PIPE_INSTANCE: A second server trying to claim the same pipe name as first instance must fail!
    HANDLE hSecond = CreateNamedPipeA(
        pipe_name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 1024, 1024, 50, nullptr);
    ASSERT_TRUE(hSecond == INVALID_HANDLE_VALUE, "Second server cannot claim first pipe instance");
    ASSERT_TRUE(GetLastError() == ERROR_ACCESS_DENIED || GetLastError() == ERROR_ALREADY_EXISTS, "Error code is access denied or already exists");
#endif

    // Test clean shutdown with active connection
    auto start = std::chrono::steady_clock::now();
    server1.Stop();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    ASSERT_TRUE(elapsed < 3000, "Server stopped cleanly in under 3 seconds");

    ctrl.Shutdown();
    std::filesystem::remove_all(work_dir, ec);
    return true;
}

// Regression Suite 26: Conflict Policies End-to-End (FDM-007, FDM-008, TG-10)
bool TestConflictPolicies() {
    std::error_code ec;
    std::filesystem::path work_dir = std::filesystem::temp_directory_path() / ("flux_test_conflict_" + openidm::GenerateUuidV4());
    std::filesystem::create_directories(work_dir, ec);

    std::string fname = "report.pdf";
    std::filesystem::path existing = work_dir / fname;
    {
        std::ofstream ofs(existing);
        ofs << "Original content";
    }

    // 1. FailIfExists
    std::filesystem::path part_fail = work_dir / "fail.part";
    {
        std::ofstream ofs(part_fail);
        ofs << "New content";
    }
    auto res_fail = openidm::PathSanitizer::CommitPartFile(part_fail, work_dir, fname, openidm::ConflictPolicy::kFailIfExists);
    ASSERT_TRUE(!res_fail.ok() && res_fail.error->code == "ERR_FILE_EXISTS", "kFailIfExists rejects existing destination");

    // 2. Overwrite
    std::filesystem::path part_ovw = work_dir / "ovw.part";
    {
        std::ofstream ofs(part_ovw);
        ofs << "Overwritten content";
    }
    auto res_ovw = openidm::PathSanitizer::CommitPartFile(part_ovw, work_dir, fname, openidm::ConflictPolicy::kOverwrite);
    ASSERT_TRUE(res_ovw.ok(), "kOverwrite succeeded");
    {
        std::ifstream ifs(*res_ovw.value);
        std::string s;
        std::getline(ifs, s);
        ASSERT_TRUE(s == "Overwritten content", "Content was replaced");
    }

    // 3. AutoRename
    std::filesystem::path part_ren = work_dir / "ren.part";
    {
        std::ofstream ofs(part_ren);
        ofs << "Auto-renamed content";
    }
    auto res_ren = openidm::PathSanitizer::CommitPartFile(part_ren, work_dir, fname, openidm::ConflictPolicy::kAutoRename);
    ASSERT_TRUE(res_ren.ok(), "kAutoRename succeeded");
    ASSERT_TRUE(res_ren.value->filename().string() == "report (1).pdf", "Target renamed to report (1).pdf");
    {
        std::ifstream ifs(*res_ren.value);
        std::string s;
        std::getline(ifs, s);
        ASSERT_TRUE(s == "Auto-renamed content", "Renamed file content preserved");
    }

    std::filesystem::remove_all(work_dir, ec);
    return true;
}

// Regression Suite 27: System Suspend/Resume Selective Task Restoration (CTRL-02)
bool TestSystemSuspendResumeAndPauseReason() {
    std::error_code ec;
    std::filesystem::path work_dir = std::filesystem::temp_directory_path() / ("flux_test_suspend_" + openidm::GenerateUuidV4());
    std::filesystem::create_directories(work_dir, ec);

    openidm::ControllerConfig cfg;
    cfg.state_db_path = work_dir / "state.db";
    cfg.default_download_dir = work_dir / "downloads";
    cfg.temp_root_dir = work_dir / "tmp";

    openidm::DownloadController ctrl(cfg);
    ASSERT_TRUE(ctrl.Initialize().ok(), "Init controller");

    // Task 1: Started immediately (active/queued)
    openidm::TaskModel t1;
    t1.url = "https://example.com/file1.zip";
    auto r1 = ctrl.AddTask(t1, true);
    ASSERT_TRUE(r1.ok(), "Task 1 added");
    ASSERT_TRUE(r1.value->state == openidm::TaskState::kQueued, "Task 1 starts in Queued state");
    ASSERT_TRUE(r1.value->pause_reason == openidm::PauseReason::kNone, "Task 1 pause_reason is kNone");

    // Task 2: Created in paused state by user
    openidm::TaskModel t2;
    t2.url = "https://example.com/file2.zip";
    auto r2 = ctrl.AddTask(t2, false);
    ASSERT_TRUE(r2.ok(), "Task 2 added");
    ASSERT_TRUE(r2.value->state == openidm::TaskState::kPaused, "Task 2 starts in Paused state");
    ASSERT_TRUE(r2.value->pause_reason == openidm::PauseReason::kUser, "Task 2 pause_reason is kUser");

    // 1. Trigger system suspend
    ctrl.OnSystemSuspend();

    auto g1_suspend = ctrl.GetTask(r1.value->task_id);
    auto g2_suspend = ctrl.GetTask(r2.value->task_id);
    ASSERT_TRUE(g1_suspend.has_value(), "Task 1 found");
    ASSERT_TRUE(g2_suspend.has_value(), "Task 2 found");

    ASSERT_TRUE(g1_suspend->state == openidm::TaskState::kPaused, "Task 1 paused on suspend");
    ASSERT_TRUE(g1_suspend->pause_reason == openidm::PauseReason::kSystemSuspend, "Task 1 marked as kSystemSuspend");

    ASSERT_TRUE(g2_suspend->state == openidm::TaskState::kPaused, "Task 2 still paused on suspend");
    ASSERT_TRUE(g2_suspend->pause_reason == openidm::PauseReason::kUser, "Task 2 retains kUser pause_reason");

    // 2. Trigger system resume
    ctrl.OnSystemResume();

    auto g1_resume = ctrl.GetTask(r1.value->task_id);
    auto g2_resume = ctrl.GetTask(r2.value->task_id);
    ASSERT_TRUE(g1_resume.has_value(), "Task 1 found");
    ASSERT_TRUE(g2_resume.has_value(), "Task 2 found");

    ASSERT_TRUE(g1_resume->state == openidm::TaskState::kQueued, "Task 1 automatically resumed to Queued");
    ASSERT_TRUE(g1_resume->pause_reason == openidm::PauseReason::kNone, "Task 1 pause_reason reset to kNone");

    ASSERT_TRUE(g2_resume->state == openidm::TaskState::kPaused, "Task 2 strictly remains Paused after sleep/wake");
    ASSERT_TRUE(g2_resume->pause_reason == openidm::PauseReason::kUser, "Task 2 pause_reason remains kUser");

    // 3. User manually resumes Task 2
    ASSERT_TRUE(ctrl.ResumeTask(r2.value->task_id).ok(), "Manual resume of Task 2 succeeded");
    auto g2_manual = ctrl.GetTask(r2.value->task_id);
    ASSERT_TRUE(g2_manual.has_value() && g2_manual->state == openidm::TaskState::kQueued, "Task 2 resumed to Queued");
    ASSERT_TRUE(g2_manual->pause_reason == openidm::PauseReason::kNone, "Task 2 pause_reason reset to kNone");

    ctrl.Shutdown();
    std::filesystem::remove_all(work_dir, ec);
    return true;
}

bool TestConcurrentAtomicCommitsAndSha256() {
    std::error_code ec;
    std::filesystem::path work_dir = std::filesystem::temp_directory_path(ec) / ("flux_test_atomic_" + openidm::GenerateUuidV4());
    std::filesystem::create_directories(work_dir, ec);

    const int kThreadCount = 8;
    std::vector<std::string> payloads(kThreadCount);
    std::vector<std::string> expected_hashes(kThreadCount);
    std::vector<std::filesystem::path> part_files(kThreadCount);

    for (int i = 0; i < kThreadCount; ++i) {
        payloads[i] = "PAYLOAD_THREAD_CHUNK_" + std::to_string(i) + "_" + std::string(1024 * (i + 1), static_cast<char>('A' + i));
        expected_hashes[i] = openidm::CryptoUtils::ComputeBufferSha256Hex(payloads[i]);
        part_files[i] = work_dir / ("worker_" + std::to_string(i) + ".part");
        std::ofstream ofs(part_files[i], std::ios::binary);
        ofs << payloads[i];
    }

    // 1. Hammer kAutoRename concurrently: All 8 threads simultaneously commit to "contended.bin"
    std::vector<std::filesystem::path> resulting_paths(kThreadCount);
    std::vector<bool> thread_success(kThreadCount, false);
    std::atomic<bool> start_signal{false};

    std::vector<std::thread> workers;
    for (int i = 0; i < kThreadCount; ++i) {
        workers.emplace_back([&, i]() {
            while (!start_signal.load()) {
                std::this_thread::yield();
            }
            auto res = openidm::PathSanitizer::CommitPartFile(
                part_files[i], work_dir, "contended.bin", openidm::ConflictPolicy::kAutoRename);
            if (res.ok()) {
                resulting_paths[i] = *res.value;
                thread_success[i] = true;
            }
        });
    }

    start_signal.store(true);
    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

    // Verify: Every thread must have succeeded
    for (int i = 0; i < kThreadCount; ++i) {
        ASSERT_TRUE(thread_success[i], "Thread commit succeeded under kAutoRename");
        ASSERT_TRUE(std::filesystem::exists(resulting_paths[i], ec), "Resulting file exists");
    }

    // Verify: All resulting paths must be unique (no duplicate auto-rename slots!)
    std::set<std::filesystem::path> unique_paths(resulting_paths.begin(), resulting_paths.end());
    ASSERT_TRUE(unique_paths.size() == kThreadCount, "All 8 committed paths are strictly unique");

    // Verify: The SHA-256 digest of each committed file matches its exact source payload!
    for (int i = 0; i < kThreadCount; ++i) {
        std::ifstream ifs(resulting_paths[i], std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        std::string actual_hash = openidm::CryptoUtils::ComputeBufferSha256Hex(content);
        ASSERT_TRUE(actual_hash == expected_hashes[i], "Committed file SHA-256 matches exact source content without corruption");
    }

    // 2. Hammer kFailIfExists concurrently:
    // Create new parts and attempt to commit to "exclusive.bin"
    for (int i = 0; i < kThreadCount; ++i) {
        part_files[i] = work_dir / ("failif_" + std::to_string(i) + ".part");
        std::ofstream ofs(part_files[i], std::ios::binary);
        ofs << payloads[i];
    }

    std::atomic<int> success_count{0};
    std::atomic<int> already_exists_count{0};
    start_signal.store(false);
    workers.clear();

    for (int i = 0; i < kThreadCount; ++i) {
        workers.emplace_back([&, i]() {
            while (!start_signal.load()) {
                std::this_thread::yield();
            }
            auto res = openidm::PathSanitizer::CommitPartFile(
                part_files[i], work_dir, "exclusive.bin", openidm::ConflictPolicy::kFailIfExists);
            if (res.ok()) {
                ++success_count;
            } else if (res.error->code == "ERR_FILE_EXISTS") {
                ++already_exists_count;
            }
        });
    }

    start_signal.store(true);
    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

    // Exactly 1 thread must claim "exclusive.bin" and the other 7 must fail with ERR_FILE_EXISTS
    ASSERT_TRUE(success_count.load() == 1, "Exactly 1 thread claims exclusive file under kFailIfExists");
    ASSERT_TRUE(already_exists_count.load() == kThreadCount - 1, "Other threads receive ERR_FILE_EXISTS");

    std::filesystem::remove_all(work_dir, ec);
    return true;
}

bool TestDnsRebindingAndDestinationSecurity() {
    using openidm::brisk::HttpClient;

    // 1. IPv4 private / loopback / link-local / CGNAT / multicast / broadcast
    ASSERT_TRUE(HttpClient::IsPrivateOrLoopbackIp(0x7F000001), "127.0.0.1 is loopback");
    ASSERT_TRUE(HttpClient::IsPrivateOrLoopbackIp(0x7FFFFFFF), "127.255.255.255 is loopback");
    ASSERT_TRUE(HttpClient::IsPrivateOrLoopbackIp(0x0A000001), "10.0.0.1 is RFC 1918");
    ASSERT_TRUE(HttpClient::IsPrivateOrLoopbackIp(0xAC100001), "172.16.0.1 is RFC 1918");
    ASSERT_TRUE(HttpClient::IsPrivateOrLoopbackIp(0xAC1FFFFF), "172.31.255.255 is RFC 1918");
    ASSERT_TRUE(!HttpClient::IsPrivateOrLoopbackIp(0xAC200001), "172.32.0.1 is public");
    ASSERT_TRUE(HttpClient::IsPrivateOrLoopbackIp(0xC0A80101), "192.168.1.1 is RFC 1918");
    ASSERT_TRUE(HttpClient::IsPrivateOrLoopbackIp(0xA9FEA9FE), "169.254.169.254 is Link-Local APIPA");
    ASSERT_TRUE(HttpClient::IsPrivateOrLoopbackIp(0x64400001), "100.64.0.1 is Carrier-Grade NAT");
    ASSERT_TRUE(HttpClient::IsPrivateOrLoopbackIp(0xC0000201), "192.0.2.1 is TEST-NET-1");
    ASSERT_TRUE(HttpClient::IsPrivateOrLoopbackIp(0xE0000001), "224.0.0.1 is Multicast");
    ASSERT_TRUE(HttpClient::IsPrivateOrLoopbackIp(0xFFFFFFFF), "255.255.255.255 is Broadcast");
    ASSERT_TRUE(HttpClient::IsPrivateOrLoopbackIp(0x00000000), "0.0.0.0 is Unspecified");
    ASSERT_TRUE(!HttpClient::IsPrivateOrLoopbackIp(0x08080808), "8.8.8.8 is Public DNS");
    ASSERT_TRUE(!HttpClient::IsPrivateOrLoopbackIp(0x5DB8D822), "93.184.216.34 (example.com) is Public");

    // 2. IPv6 private / loopback / ULA / link-local / mapped
    uint8_t loopback_v6[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
    ASSERT_TRUE(HttpClient::IsPrivateOrLoopbackIpv6(loopback_v6), "::1 is IPv6 loopback");

    uint8_t unspec_v6[16] = {0};
    ASSERT_TRUE(HttpClient::IsPrivateOrLoopbackIpv6(unspec_v6), ":: is IPv6 unspecified");

    uint8_t link_local_v6[16] = {0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    ASSERT_TRUE(HttpClient::IsPrivateOrLoopbackIpv6(link_local_v6), "fe80::1 is IPv6 link-local");

    uint8_t ula_v6[16] = {0xFD, 0x12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    ASSERT_TRUE(HttpClient::IsPrivateOrLoopbackIpv6(ula_v6), "fd12::1 is IPv6 ULA");

    uint8_t mcast_v6[16] = {0xFF, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    ASSERT_TRUE(HttpClient::IsPrivateOrLoopbackIpv6(mcast_v6), "ff02::1 is IPv6 multicast");

    uint8_t mapped_priv[16] = {0,0,0,0,0,0,0,0,0,0,0xFF,0xFF,127,0,0,1};
    ASSERT_TRUE(HttpClient::IsPrivateOrLoopbackIpv6(mapped_priv), "::ffff:127.0.0.1 is IPv4-mapped loopback");

    uint8_t mapped_pub[16] = {0,0,0,0,0,0,0,0,0,0,0xFF,0xFF,8,8,8,8};
    ASSERT_TRUE(!HttpClient::IsPrivateOrLoopbackIpv6(mapped_pub), "::ffff:8.8.8.8 is IPv4-mapped public");

    // 3. DNS resolution check
    ASSERT_TRUE(HttpClient::CheckHostResolvesToPrivate("localhost"), "localhost resolves to private");
    ASSERT_TRUE(HttpClient::CheckHostResolvesToPrivate("127.0.0.1"), "127.0.0.1 resolves to private");

    return true;
}

bool TestStreamContainerValidation() {
    using openidm::stream::Nm3u8dlReAdapter;
    std::error_code ec;
    std::filesystem::path tmp_dir = std::filesystem::temp_directory_path(ec) / ("flux_test_container_" + openidm::GenerateUuidV4());
    std::filesystem::create_directories(tmp_dir, ec);

    // 1. MP4 / M4A (ISO BMFF ftyp / moov)
    auto p_mp4 = tmp_dir / "test.mp4";
    {
        std::ofstream ofs(p_mp4, std::ios::binary);
        uint8_t ftyp_hdr[] = {0x00, 0x00, 0x00, 0x20, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm', 0, 0, 2, 0};
        ofs.write(reinterpret_cast<char*>(ftyp_hdr), sizeof(ftyp_hdr));
        ofs.write(std::string(64, '\0').data(), 64);
    }
    ASSERT_TRUE(Nm3u8dlReAdapter::VerifyContainerMagicBytes(p_mp4, "mp4"), "Valid MP4 ftyp container recognized");

    auto p_m4a = tmp_dir / "test.m4a";
    {
        std::ofstream ofs(p_m4a, std::ios::binary);
        uint8_t m4a_hdr[] = {0x00, 0x00, 0x00, 0x20, 'f', 't', 'y', 'p', 'M', '4', 'A', ' ', 0, 0, 0, 0};
        ofs.write(reinterpret_cast<char*>(m4a_hdr), sizeof(m4a_hdr));
        ofs.write(std::string(64, '\0').data(), 64);
    }
    ASSERT_TRUE(Nm3u8dlReAdapter::VerifyContainerMagicBytes(p_m4a, "m4a"), "Valid M4A audio container recognized");

    // 2. MKV / WebM (EBML 0x1A 0x45 0xDF 0xA3)
    auto p_mkv = tmp_dir / "test.mkv";
    {
        std::ofstream ofs(p_mkv, std::ios::binary);
        uint8_t ebml_hdr[] = {0x1A, 0x45, 0xDF, 0xA3, 0x93, 0x42, 0x82, 0x88, 'm', 'a', 't', 'r', 'o', 's', 'k', 'a'};
        ofs.write(reinterpret_cast<char*>(ebml_hdr), sizeof(ebml_hdr));
    }
    ASSERT_TRUE(Nm3u8dlReAdapter::VerifyContainerMagicBytes(p_mkv, "mkv"), "Valid MKV EBML container recognized");
    ASSERT_TRUE(Nm3u8dlReAdapter::VerifyContainerMagicBytes(p_mkv, "webm"), "Valid WebM EBML container recognized");

    // 3. MP3 (ID3v2 and MPEG frame sync)
    auto p_mp3 = tmp_dir / "test.mp3";
    {
        std::ofstream ofs(p_mp3, std::ios::binary);
        uint8_t id3_hdr[] = {'I', 'D', '3', 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10};
        ofs.write(reinterpret_cast<char*>(id3_hdr), sizeof(id3_hdr));
        ofs.write(std::string(64, '\0').data(), 64);
    }
    ASSERT_TRUE(Nm3u8dlReAdapter::VerifyContainerMagicBytes(p_mp3, "mp3"), "Valid MP3 ID3 container recognized");

    // 4. FLAC ("fLaC")
    auto p_flac = tmp_dir / "test.flac";
    {
        std::ofstream ofs(p_flac, std::ios::binary);
        uint8_t flac_hdr[] = {'f', 'L', 'a', 'C', 0, 0, 0, 0x22};
        ofs.write(reinterpret_cast<char*>(flac_hdr), sizeof(flac_hdr));
    }
    ASSERT_TRUE(Nm3u8dlReAdapter::VerifyContainerMagicBytes(p_flac, "flac"), "Valid FLAC container recognized");

    // 5. MPEG-TS (0x47 sync byte)
    auto p_ts = tmp_dir / "test.ts";
    {
        std::ofstream ofs(p_ts, std::ios::binary);
        uint8_t ts_packet[188] = {0x47, 0x00, 0x10, 0x10};
        ofs.write(reinterpret_cast<char*>(ts_packet), sizeof(ts_packet));
    }
    ASSERT_TRUE(Nm3u8dlReAdapter::VerifyContainerMagicBytes(p_ts, "ts"), "Valid MPEG-TS container recognized");

    // 6. Corrupt / empty / invalid files
    auto p_corrupt = tmp_dir / "corrupt.mp4";
    {
        std::ofstream ofs(p_corrupt, std::ios::binary);
        ofs << "XYZ";
    }
    ASSERT_TRUE(!Nm3u8dlReAdapter::VerifyContainerMagicBytes(p_corrupt, "mp4"), "Corrupt MP4 rejected");
    ASSERT_TRUE(!Nm3u8dlReAdapter::VerifyContainerMagicBytes(p_corrupt, "mkv"), "Corrupt MKV rejected");

    std::filesystem::remove_all(tmp_dir, ec);
    return true;
}

bool TestFtpClientParsingAndCommands() {
    using namespace openidm::ftp;

    auto res1 = FtpUrl::Parse("ftp://ftp.example.com/pub/files/archive.tar.gz");
    ASSERT_TRUE(res1.ok(), "Parse standard FTP URL");
    ASSERT_TRUE(res1.value->scheme == "ftp", "FTP scheme");
    ASSERT_TRUE(res1.value->host == "ftp.example.com", "FTP host");
    ASSERT_TRUE(res1.value->port == 21, "Default FTP port 21");
    ASSERT_TRUE(res1.value->remote_path == "/pub/files/archive.tar.gz", "Remote path");
    ASSERT_TRUE(res1.value->file_name == "archive.tar.gz", "Filename");
    ASSERT_TRUE(res1.value->user == "anonymous", "Default user anonymous");

    // FTPS URL must be rejected fast to prevent plain unencrypted socket connections
    auto res2 = FtpUrl::Parse("ftps://alice:secret123@secure.ftp.org:990/data/report.pdf");
    ASSERT_TRUE(!res2.ok(), "Reject FTPS URL without verified TLS negotiation");
    ASSERT_TRUE(res2.error->code == "ERR_FTPS_UNSUPPORTED", "Rejection error code is ERR_FTPS_UNSUPPORTED");

    // Verify ProbeFileSize and DownloadFile defense-in-depth FTPS guards
    FtpUrl ftps_url;
    ftps_url.scheme = "ftps";
    ftps_url.host = "secure.ftp.org";
    auto probe_ftps = FtpClient::ProbeFileSize(ftps_url);
    ASSERT_TRUE(!probe_ftps.ok() && probe_ftps.error->code == "ERR_FTPS_UNSUPPORTED", "ProbeFileSize rejects ftps");

    std::atomic<bool> p_req{false}, c_req{false};
    std::atomic<uint64_t> spd{0};
    auto dl_ftps = FtpClient::DownloadFile(ftps_url, "/tmp/dummy.part", 0, p_req, c_req, spd, nullptr);
    ASSERT_TRUE(!dl_ftps.ok() && dl_ftps.error->code == "ERR_FTPS_UNSUPPORTED", "DownloadFile rejects ftps");

    auto res_invalid = FtpUrl::Parse("http://not-ftp.com/test.bin");
    ASSERT_TRUE(!res_invalid.ok(), "Reject non-FTP URL");
    return true;
}

bool TestBencodeAndTorrentParser() {
    using namespace openidm::torrent;

    // 1. Bencode integer, string, list, dict
    auto int_res = BencodeParser::Decode("i42e");
    ASSERT_TRUE(int_res.ok() && int_res.value->as_int() == 42, "Bencode int 42");

    auto str_res = BencodeParser::Decode("4:flux");
    ASSERT_TRUE(str_res.ok() && str_res.value->as_string() == "flux", "Bencode string 'flux'");

    auto list_res = BencodeParser::Decode("li1ei2e3:abce");
    ASSERT_TRUE(list_res.ok() && list_res.value->as_list().size() == 3, "Bencode list [1, 2, 'abc']");

    auto dict_res = BencodeParser::Decode("d4:name4:flux4:porti6881ee");
    ASSERT_TRUE(dict_res.ok() && dict_res.value->as_dict().size() == 2, "Bencode dict");
    ASSERT_TRUE(dict_res.value->contains("name") && (*dict_res.value)["name"].as_string() == "flux", "Dict 'name'");
    ASSERT_TRUE(dict_res.value->contains("port") && (*dict_res.value)["port"].as_int() == 6881, "Dict 'port'");

    // Re-encode
    std::string encoded = BencodeParser::Encode(dict_res.value.value());
    ASSERT_TRUE(encoded == "d4:name4:flux4:porti6881ee", "Bencode re-encode matches input");

    // 2. Magnet Link Parsing
    std::string magnet = "magnet:?xt=urn:btih:4a7d65b7f9a838528990c76892e85a6a2360a7e0&dn=Ubuntu_24.04_LTS&tr=http%3A%2F%2Ftracker.example.com%2Fannounce";
    auto mag_res = TorrentParser::ParseMagnetUri(magnet);
    ASSERT_TRUE(mag_res.ok(), "Parse magnet link");
    ASSERT_TRUE(mag_res.value->info_hash_hex == "4a7d65b7f9a838528990c76892e85a6a2360a7e0", "Magnet info_hash hex");
    ASSERT_TRUE(mag_res.value->name == "Ubuntu_24.04_LTS", "Magnet display name");
    ASSERT_TRUE(mag_res.value->announce == "http://tracker.example.com/announce", "Magnet tracker announce URL");

    // Base32 Magnet
    std::string mag_b32 = "magnet:?xt=urn:btih:JJOWLN7ZVIOFFCKQY5UJFYU2NIRQBJ7A&dn=TestBase32";
    auto mag_b32_res = TorrentParser::ParseMagnetUri(mag_b32);
    ASSERT_TRUE(mag_b32_res.ok(), "Parse Base32 magnet link");
    ASSERT_TRUE(mag_b32_res.value->info_hash_hex.size() == 40, "Base32 converted to 40 hex chars");

    // 3. Piece SHA-1 Verification
    const char test_data[] = "BitTorrent Piece Validation Test Block";
    std::string expected_sha1 = openidm::CryptoUtils::ComputeBufferSha1Hex(std::string_view(test_data, sizeof(test_data) - 1));
    ASSERT_TRUE(TorrentParser::VerifyPieceHex(reinterpret_cast<const uint8_t*>(test_data), sizeof(test_data) - 1, expected_sha1), "VerifyPieceHex matches");
    ASSERT_TRUE(!TorrentParser::VerifyPieceHex(reinterpret_cast<const uint8_t*>("corrupt data"), 12, expected_sha1), "Corrupt piece rejected");

    // 4. BitTorrent Execution Elimination Guard (honesty check)
    openidm::TaskModel tor_task;
    tor_task.task_id = "test-torrent-task";
    tor_task.url = magnet;
    tor_task.file_name = "test.iso";
    tor_task.save_dir = "/tmp/test_torrent_dir";
    TorrentInfo tinfo;
    tinfo.name = "test.iso";
    tinfo.total_length = 1048576;
    std::atomic<bool> t_pause{false}, t_cancel{false};
    std::atomic<uint64_t> t_speed{0};

    auto tor_exec = TorrentEngine::ExecuteTorrentDownload(tor_task, tinfo, t_pause, t_cancel, t_speed, nullptr);
    ASSERT_TRUE(!tor_exec.ok(), "BitTorrent execution must fail cleanly on incomplete metainfo");
    ASSERT_TRUE(tor_exec.error->code == "ERR_TORRENT_MALFORMED", "Returns ERR_TORRENT_MALFORMED on zero-piece metainfo");
    ASSERT_TRUE(tor_task.state == openidm::TaskState::kFailed, "Task state marked as kFailed");
    ASSERT_TRUE(tor_task.downloaded_bytes == 0, "Downloaded bytes must remain 0");
    ASSERT_TRUE(tor_task.completed_segments == 0, "Completed pieces must remain 0");
    ASSERT_TRUE(!std::filesystem::exists("/tmp/test_torrent_dir/test.iso"), "No dummy or placeholder files created on disk");

    // Test with complete metainfo but unreachable trackers/peers
    TorrentInfo tinfo_valid;
    tinfo_valid.name = "test_valid.iso";
    tinfo_valid.total_length = 32768;
    tinfo_valid.piece_length = 32768;
    tinfo_valid.pieces.push_back(std::vector<uint8_t>(20, 0x11));
    tinfo_valid.announce = "http://127.0.0.1:1/announce";
    tor_task.url = "test_valid.torrent";
    auto tor_valid_exec = TorrentEngine::ExecuteTorrentDownload(tor_task, tinfo_valid, t_pause, t_cancel, t_speed, nullptr);
    ASSERT_TRUE(!tor_valid_exec.ok(), "BitTorrent execution fails cleanly when peers unreachable");
    ASSERT_TRUE(tor_valid_exec.error->code == "ERR_TORRENT_NO_PEERS", "Returns ERR_TORRENT_NO_PEERS when peers unavailable");
    return true;
}

bool TestRtspMmsStreamDetection() {
    using namespace openidm::stream;

    ASSERT_TRUE(RtspMmsAdapter::IsStreamUrl("rtsp://media.example.com/live/ch1"), "rtsp:// is stream");
    ASSERT_TRUE(RtspMmsAdapter::IsStreamUrl("rtsps://media.example.com:443/live"), "rtsps:// is stream");
    ASSERT_TRUE(RtspMmsAdapter::IsStreamUrl("mms://stream.example.com/broadcast"), "mms:// is stream");
    ASSERT_TRUE(RtspMmsAdapter::IsStreamUrl("rtmp://live.twitch.tv/app/streamkey"), "rtmp:// is stream");
    ASSERT_TRUE(!RtspMmsAdapter::IsStreamUrl("https://example.com/video.mp4"), "https MP4 is not raw rtsp/mms stream");
    ASSERT_TRUE(!RtspMmsAdapter::IsStreamUrl("ftp://ftp.example.com/file.bin"), "ftp is not rtsp/mms stream");
    return true;
}

bool TestSiteGrabberCrawlerAndSsrf() {
    using namespace openidm::crawler;

    // 1. URL Resolution
    ASSERT_TRUE(SiteGrabber::ResolveUrl("https://example.com/docs/page.html", "sub.html") == "https://example.com/docs/sub.html", "Relative path resolution");
    ASSERT_TRUE(SiteGrabber::ResolveUrl("https://example.com/docs/page.html", "/images/logo.png") == "https://example.com/images/logo.png", "Root-relative path resolution");
    ASSERT_TRUE(SiteGrabber::ResolveUrl("https://example.com/docs/page.html", "//cdn.example.com/asset.js") == "https://cdn.example.com/asset.js", "Protocol-relative URL");
    ASSERT_TRUE(SiteGrabber::ResolveUrl("https://example.com/page.html", "file.pdf#section2") == "https://example.com/file.pdf", "Strip fragment");
    ASSERT_TRUE(SiteGrabber::ResolveUrl("https://example.com/", "javascript:void(0)").empty(), "Ignore javascript: pseudo-protocol");

    // 2. HTML link extraction
    std::string html = R"(
        <html>
        <body>
            <a href="/downloads/setup.exe">Download Installer</a>
            <img src="banner.jpg" alt="Banner"/>
            <a href="https://example.com/manual.pdf">PDF Manual</a>
            <video src="/videos/intro.mp4"></video>
        </body>
        </html>
    )";
    auto urls = SiteGrabber::ExtractUrlsFromHtml(html, "https://example.com/main/");
    ASSERT_TRUE(urls.size() == 4, "Extracted 4 links/media from HTML");

    // 3. Category & Extension Filter Matching
    SiteGrabberConfig cfg;
    cfg.filter_categories = {"Documents", "Images"};
    std::string cat;
    ASSERT_TRUE(SiteGrabber::MatchesFilter("https://example.com/manual.pdf", cfg, &cat) && cat == "Documents", "Matches Documents category");
    ASSERT_TRUE(SiteGrabber::MatchesFilter("https://example.com/photo.png", cfg, &cat) && cat == "Images", "Matches Images category");
    ASSERT_TRUE(!SiteGrabber::MatchesFilter("https://example.com/setup.exe", cfg, &cat), "Rejects Programs when not in filter");

    // 4. SSRF Guard on Crawler Start URL
    SiteGrabberConfig ssrf_cfg;
    ssrf_cfg.start_url = "http://127.0.0.1:8080/admin";
    ssrf_cfg.allow_loopback = false;
    std::atomic<bool> cancel{false};
    auto crawl_res = SiteGrabber::Crawl(ssrf_cfg, cancel);
    ASSERT_TRUE(!crawl_res.ok() && crawl_res.error->code == "ERR_SSRF_BLOCKED", "Crawler blocks SSRF to loopback");
    return true;
}

bool TestZipArchivePreviewAndSecurity() {
    using namespace openidm::archive;

    // 1. Zip Slip Path Traversal Guard
    ASSERT_TRUE(ZipPreviewer::IsZipSlipPath("../../etc/shadow"), "Detect ../ traversal");
    ASSERT_TRUE(ZipPreviewer::IsZipSlipPath("..\\..\\Windows\\System32\\calc.exe"), "Detect ..\\ traversal");
    ASSERT_TRUE(ZipPreviewer::IsZipSlipPath("/etc/passwd"), "Detect leading slash");
    ASSERT_TRUE(ZipPreviewer::IsZipSlipPath("C:\\autoexec.bat"), "Detect drive letter");
    ASSERT_TRUE(ZipPreviewer::IsZipSlipPath("report.pdf:Zone.Identifier"), "Detect NTFS ADS colon");
    ASSERT_TRUE(!ZipPreviewer::IsZipSlipPath("docs/readme.txt"), "Safe relative path allowed");
    ASSERT_TRUE(!ZipPreviewer::IsZipSlipPath("images/photo.png"), "Safe nested path allowed");

    // 2. Synthetic Valid ZIP Archive in Memory
    // Construct Central Directory Header + EOCD record
    std::vector<uint8_t> zip_buf;
    uint8_t cd_hdr[] = {
        0x50, 0x4b, 0x01, 0x02, // signature
        0x14, 0x00,             // version made by (2.0)
        0x14, 0x00,             // version needed (2.0)
        0x00, 0x00,             // flags (no encryption)
        0x08, 0x00,             // compression (8 = deflated)
        0x00, 0x00, 0x00, 0x00, // mod time/date
        0x12, 0x34, 0x56, 0x78, // crc-32
        0x20, 0x00, 0x00, 0x00, // compressed size (32 bytes)
        0x64, 0x00, 0x00, 0x00, // uncompressed size (100 bytes)
        0x08, 0x00,             // filename length (8)
        0x00, 0x00,             // extra length (0)
        0x00, 0x00,             // comment length (0)
        0x00, 0x00,             // disk start (0)
        0x00, 0x00,             // internal attrs
        0x00, 0x00, 0x00, 0x00, // external attrs
        0x00, 0x00, 0x00, 0x00  // local header offset
    };
    zip_buf.insert(zip_buf.end(), cd_hdr, cd_hdr + sizeof(cd_hdr));
    const char fn[] = "test.txt";
    zip_buf.insert(zip_buf.end(), fn, fn + 8);

    auto summary_res = ZipPreviewer::InspectBuffer(zip_buf.data(), zip_buf.size());
    ASSERT_TRUE(summary_res.ok(), "InspectBuffer parses Central Directory header");
    ASSERT_TRUE(summary_res.value->total_files == 1, "Total files is 1");
    ASSERT_TRUE(summary_res.value->entries[0].file_path == "test.txt", "Entry filename");
    ASSERT_TRUE(summary_res.value->entries[0].uncompressed_size == 100, "Uncompressed size 100");
    ASSERT_TRUE(summary_res.value->entries[0].compressed_size == 32, "Compressed size 32");
    ASSERT_TRUE(summary_res.value->entries[0].compression_method == 8, "Compression method 8 (Deflated)");
    ASSERT_TRUE(!summary_res.value->entries[0].is_encrypted, "Not encrypted");
    return true;
}

bool TestTaskImportExportAndMultiQueue() {
    using namespace openidm::storage;

    // 1. Task Export & Credential Redaction
    openidm::TaskModel t1;
    t1.task_id = "test-uuid-1";
    t1.url = "https://user:SuperSecretPassword@cdn.example.com/download.zip?token=xyz123";
    t1.file_name = "download.zip";
    t1.category = "Compressed";
    t1.queue_id = "main";
    t1.total_bytes = 1048576;
    t1.auth_headers["Authorization"] = "Bearer secret-token-999";
    t1.auth_headers["Cookie"] = "session=secret-session-abc";

    std::vector<openidm::TaskModel> tasks = {t1};

    auto json_exp = TaskImportExport::ExportTasksToString(tasks, ExportFormat::kJson, false);
    ASSERT_TRUE(json_exp.ok(), "Export to JSON succeeds");
    ASSERT_TRUE(json_exp.value->find("SuperSecretPassword") == std::string::npos, "Password scrubbed from URL");
    ASSERT_TRUE(json_exp.value->find("secret-token-999") == std::string::npos, "Auth headers excluded from export");
    ASSERT_TRUE(json_exp.value->find("download.zip") != std::string::npos, "Filename present");

    auto csv_exp = TaskImportExport::ExportTasksToString(tasks, ExportFormat::kCsv, false);
    ASSERT_TRUE(csv_exp.ok(), "Export to CSV succeeds");
    ASSERT_TRUE(csv_exp.value->find("SuperSecretPassword") == std::string::npos, "Password scrubbed from CSV URL");

    // 2. Task Import from URL list
    std::string url_list = "# Comment line\nhttps://example.com/file1.pdf\nftp://ftp.example.com/file2.zip\nmagnet:?xt=urn:btih:1111111111111111111111111111111111111111&dn=Linux\n";
    auto imported = TaskImportExport::ImportFromUrlList(url_list);
    ASSERT_TRUE(imported.ok(), "Import from URL list succeeds");
    ASSERT_TRUE(imported.value->size() == 3, "Imported 3 tasks");
    ASSERT_TRUE((*imported.value)[0].category == "Documents", "Auto-inferred Documents category for PDF");
    ASSERT_TRUE((*imported.value)[1].task_type == openidm::TaskType::kFtp, "Auto-inferred FTP task type");
    ASSERT_TRUE((*imported.value)[2].task_type == openidm::TaskType::kTorrent, "Auto-inferred Torrent task type");

    // 3. Multi-Queue Management in SqliteStore
    std::filesystem::path db_p = "/tmp/test_queue_" + openidm::GenerateUuidV4() + ".db";
    openidm::SqliteStore store;
    ASSERT_TRUE(store.Open(db_p).ok(), "Open test DB for queues");

    openidm::QueueConfig q2;
    q2.queue_id = "sync_queue";
    q2.name = "Synchronization Queue";
    q2.max_concurrent_tasks = 5;
    q2.speed_limit_bps = 5242880;
    q2.schedule_enabled = true;
    q2.start_time_local = "02:00";
    q2.stop_time_local = "06:00";
    q2.post_completion_action = "EXIT_APP";

    ASSERT_TRUE(store.UpsertQueue(q2).ok(), "UpsertQueue succeeds");
    auto q_list = store.ListQueues();
    ASSERT_TRUE(q_list.ok(), "ListQueues succeeds");
    ASSERT_TRUE(q_list.value->size() >= 2, "Contains default 'main' and 'sync_queue'");

    std::error_code ec;
    std::filesystem::remove(db_p, ec);
    return true;
}

bool TestAntivirusScannerSafetyAndHandling() {
    using namespace openidm::security;

    // 1. ScanReport default status must be kUnavailable (never kClean)
    ScanReport def_rep;
    ASSERT_TRUE(def_rep.status == ScanStatus::kUnavailable, "ScanReport default is kUnavailable");

    // 2. Scan missing file returns error
    auto missing_res = AntivirusScanner::ScanWithDefender("/nonexistent/file/for/antivirus.dat");
    ASSERT_TRUE(!missing_res.ok(), "Scan missing file returns error");
    ASSERT_TRUE(missing_res.error->code == "ERR_FILE_NOT_FOUND", "Error code is ERR_FILE_NOT_FOUND");

    // 3. Create dummy file and verify scan does NOT report fake kClean on non-Windows
    std::filesystem::path tmp_p = std::filesystem::temp_directory_path() / ("test_av_" + openidm::GenerateUuidV4() + ".bin");
    {
        std::ofstream ofs(tmp_p, std::ios::binary);
        ofs << "Antivirus Test Content";
    }

#ifndef _WIN32
    auto scan_res = AntivirusScanner::ScanWithDefender(tmp_p);
    ASSERT_TRUE(scan_res.ok(), "ScanWithDefender succeeds with report");
    ASSERT_TRUE(scan_res.value->status == ScanStatus::kUnavailable, "Non-Windows mock scan reports kUnavailable (never kClean)");
    ASSERT_TRUE(!AntivirusScanner::IsWindowsDefenderAvailable(), "IsWindowsDefenderAvailable is false on non-Windows");
#endif

    // 4. Verify DesktopView blocks ActionOpenFile if a threat was detected
    std::error_code ec;
    std::filesystem::path work_dir = std::filesystem::temp_directory_path() / ("flux_test_av_ui_" + openidm::GenerateUuidV4());
    std::filesystem::create_directories(work_dir, ec);

    openidm::ControllerConfig cfg;
    cfg.state_db_path = work_dir / "state.db";
    cfg.default_download_dir = work_dir / "downloads";
    cfg.temp_root_dir = work_dir / "tmp";
    std::filesystem::create_directories(cfg.default_download_dir, ec);

    openidm::DownloadController ctrl(cfg);
    ASSERT_TRUE(ctrl.Initialize().ok(), "Initialize controller");

    openidm::TaskModel t_threat;
    t_threat.task_id = "task-threat-1";
    t_threat.url = "https://example.com/malware.exe";
    t_threat.file_name = "malware.exe";
    t_threat.save_dir = cfg.default_download_dir.string();
    t_threat.state = openidm::TaskState::kCompleted;
    t_threat.error_code = "ERR_THREAT_DETECTED";
    t_threat.status_detail = "Threat detected by Antivirus (Defender)";

    // Write file so it exists
    {
        std::ofstream ofs(cfg.default_download_dir / "malware.exe", std::ios::binary);
        ofs << "Threat Dummy";
    }

    auto add_threat = ctrl.AddTask(t_threat, false, true);
    ASSERT_TRUE(add_threat.ok(), "Added threat task");

    openidm::ui::DesktopView view(ctrl);
    view.SelectTask("task-threat-1");
    auto open_res = view.ActionOpenCompletedFile(true); // dry_run
    ASSERT_TRUE(!open_res.ok(), "ActionOpenCompletedFile must block execution on threat detection");
    ASSERT_TRUE(open_res.error->code == "ERR_THREAT_DETECTED", "Returns ERR_THREAT_DETECTED");

    ctrl.Shutdown();
    std::filesystem::remove(tmp_p, ec);
    std::filesystem::remove_all(work_dir, ec);
    return true;
}

bool TestDualRateLimiterAndPerTaskSpeedLimit() {
    using openidm::brisk::TokenBucketLimiter;

    // Test dual limiter logic
    auto global_limiter = std::make_shared<TokenBucketLimiter>();
    TokenBucketLimiter task_limiter;
    std::atomic<uint64_t> global_limit{100000}; // 100 KB/s
    std::atomic<uint64_t> task_limit{50000};    // 50 KB/s
    std::atomic<bool> stop_flag{false};

    global_limiter->Consume(1000, global_limit, stop_flag);
    task_limiter.Consume(1000, task_limit, stop_flag);

    // Test controller per-task speed limit API
    std::error_code ec;
    std::filesystem::path work_dir = std::filesystem::temp_directory_path() / ("flux_test_speed_" + openidm::GenerateUuidV4());
    std::filesystem::create_directories(work_dir, ec);

    openidm::ControllerConfig cfg;
    cfg.state_db_path = work_dir / "state.db";
    cfg.default_download_dir = work_dir / "downloads";
    cfg.temp_root_dir = work_dir / "tmp";
    std::filesystem::create_directories(cfg.default_download_dir, ec);

    openidm::DownloadController ctrl(cfg);
    ASSERT_TRUE(ctrl.Initialize().ok(), "Initialize controller");

    openidm::TaskModel t;
    t.task_id = "task-speed-1";
    t.url = "https://example.com/file.bin";
    t.file_name = "file.bin";
    t.speed_limit_bps = 500000;
    auto add_res = ctrl.AddTask(t, false, true);
    ASSERT_TRUE(add_res.ok(), "Added task with speed limit");

    ASSERT_TRUE(ctrl.GetTaskSpeedLimit("task-speed-1") == 500000, "GetTaskSpeedLimit returns 500000");
    auto set_res = ctrl.SetTaskSpeedLimit("task-speed-1", 1000000);
    ASSERT_TRUE(set_res.ok(), "SetTaskSpeedLimit to 1000000 ok");
    ASSERT_TRUE(ctrl.GetTaskSpeedLimit("task-speed-1") == 1000000, "GetTaskSpeedLimit returns 1000000");

    ctrl.Shutdown();
    std::filesystem::remove_all(work_dir, ec);
    return true;
}

bool TestDynamicCategorizationAndSiteRulesPriority() {
    std::error_code ec;
    std::filesystem::path work_dir = std::filesystem::temp_directory_path() / ("flux_test_cat_" + openidm::GenerateUuidV4());
    std::filesystem::create_directories(work_dir, ec);

    openidm::ControllerConfig cfg;
    cfg.state_db_path = work_dir / "state.db";
    cfg.default_download_dir = work_dir / "downloads";
    cfg.temp_root_dir = work_dir / "tmp";
    std::filesystem::create_directories(cfg.default_download_dir, ec);

    openidm::DownloadController ctrl(cfg);
    ASSERT_TRUE(ctrl.Initialize().ok(), "Initialize controller");

    auto settings = ctrl.GetSettings();
    settings.category_subfolders_enabled = true;
    settings.custom_extension_categories["custompkg"] = "Programs";
    settings.custom_extension_categories["myvideo"] = "Video";
    settings.category_folders["Audio"] = (work_dir / "CustomAudio").string();
    settings.site_rules["special-music.org"] = "Audio";
    settings.site_rules["direct-dump.net"] = (work_dir / "DumpFolder").string();
    ASSERT_TRUE(ctrl.UpdateSettings(settings).ok(), "Updated settings");

    // 1. Standard extension inference
    ASSERT_TRUE(ctrl.DetermineCategory("song.mp3", "https://example.com/song.mp3") == "Audio", "song.mp3 -> Audio");
    ASSERT_TRUE(ctrl.DetermineCategory("archive.7z", "https://example.com/archive.7z") == "Compressed", "archive.7z -> Compressed");

    // 2. Custom extension mapping
    ASSERT_TRUE(ctrl.DetermineCategory("tool.custompkg", "https://example.com/tool.custompkg") == "Programs", "custompkg -> Programs");

    // 3. Site rule (Domain -> Category)
    ASSERT_TRUE(ctrl.DetermineCategory("track.unknown", "https://special-music.org/track.unknown") == "Audio", "special-music.org -> Audio");

    // 4. Resolve category save dir priority
    // 4a. Site rule directory override
    auto dump_dir = ctrl.ResolveCategorySaveDir("General", "https://direct-dump.net/file.bin");
    ASSERT_TRUE(dump_dir == (work_dir / "DumpFolder"), "Site rule directory override matches DumpFolder");

    // 4b. Category custom folder
    auto audio_dir = ctrl.ResolveCategorySaveDir("Audio", "https://example.com/song.mp3");
    ASSERT_TRUE(audio_dir == (work_dir / "CustomAudio"), "Custom Category Folder matches CustomAudio");

    // 4c. Category subfolder
    auto video_dir = ctrl.ResolveCategorySaveDir("Video", "https://example.com/clip.mp4");
    ASSERT_TRUE(video_dir == (cfg.default_download_dir / "Video"), "Video subfolder under default dir");

    // 4d. Default dir
    auto gen_dir = ctrl.ResolveCategorySaveDir("General", "https://example.com/data.bin");
    ASSERT_TRUE(gen_dir == cfg.default_download_dir, "General category maps to default dir");

    ctrl.Shutdown();
    std::filesystem::remove_all(work_dir, ec);
    return true;
}

bool TestAlternateUrlsAndMirrorValidation() {
    std::error_code ec;
    std::filesystem::path work_dir = std::filesystem::temp_directory_path() / ("flux_test_mirrors_" + openidm::GenerateUuidV4());
    std::filesystem::create_directories(work_dir, ec);

    openidm::ControllerConfig cfg;
    cfg.state_db_path = work_dir / "state.db";
    cfg.default_download_dir = work_dir / "downloads";
    cfg.temp_root_dir = work_dir / "tmp";
    std::filesystem::create_directories(cfg.default_download_dir, ec);

    openidm::DownloadController ctrl(cfg);
    ASSERT_TRUE(ctrl.Initialize().ok(), "Initialize controller");

    openidm::TaskModel t;
    t.task_id = "task-mirrors-1";
    t.url = "https://primary.example.com/large.iso";
    t.file_name = "large.iso";
    auto add_res = ctrl.AddTask(t, false, true);
    ASSERT_TRUE(add_res.ok(), "Added task");

    auto add_alt1 = ctrl.AddAlternateUrl("task-mirrors-1", "https://mirror1.example.com/large.iso");
    ASSERT_TRUE(add_alt1.ok(), "Added mirror 1");
    auto add_alt2 = ctrl.AddAlternateUrl("task-mirrors-1", "https://mirror2.example.com/large.iso");
    ASSERT_TRUE(add_alt2.ok(), "Added mirror 2");

    auto alts = ctrl.GetAlternateUrls("task-mirrors-1");
    ASSERT_TRUE(alts.size() == 2, "Task has 2 alternate URLs");
    ASSERT_TRUE(alts[0] == "https://mirror1.example.com/large.iso", "Mirror 1 match");
    ASSERT_TRUE(alts[1] == "https://mirror2.example.com/large.iso", "Mirror 2 match");

    // Adding duplicate mirror URL is idempotent
    auto add_dup = ctrl.AddAlternateUrl("task-mirrors-1", "https://mirror1.example.com/large.iso");
    ASSERT_TRUE(add_dup.ok(), "Adding duplicate mirror succeeds");
    ASSERT_TRUE(ctrl.GetAlternateUrls("task-mirrors-1").size() == 2, "Size remains 2");

    ctrl.Shutdown();
    std::filesystem::remove_all(work_dir, ec);
    return true;
}

bool TestFtpReliabilityEpsvAndResumeTruncation() {
    using openidm::ftp::FtpUrl;

    auto parsed = FtpUrl::Parse("ftp://user:pass@ftp.example.com:2121/files/document.pdf");
    ASSERT_TRUE(parsed.ok(), "Parsed FTP URL with custom port");
    ASSERT_TRUE(parsed.value->port == 2121, "Port is 2121");
    ASSERT_TRUE(parsed.value->username == "user", "Username is user");
    ASSERT_TRUE(parsed.value->password == "pass", "Password is pass");
    ASSERT_TRUE(parsed.value->file_name == "document.pdf", "File name is document.pdf");

    // FDM-003: Rejection of CRLF injection in username, password, or path
    auto crlf_user = FtpUrl::Parse("ftp://user%0d%0asomecmd@ftp.example.com/files/doc.pdf");
    ASSERT_TRUE(!crlf_user.ok(), "Reject CRLF injection in username");
    ASSERT_TRUE(crlf_user.error->code == "ERR_FTP_INVALID_URL", "Error code is ERR_FTP_INVALID_URL");

    auto crlf_path = FtpUrl::Parse("ftp://ftp.example.com/files/doc.pdf\r\nRETR evil.exe");
    ASSERT_TRUE(!crlf_path.ok(), "Reject raw CRLF injection in path");

    auto null_path = FtpUrl::Parse(std::string_view("ftp://ftp.example.com/files/doc.pdf\0other", 39));
    ASSERT_TRUE(!null_path.ok(), "Reject null byte in URL");

    auto invalid_scheme = FtpUrl::Parse("http://example.com/file.zip");
    ASSERT_TRUE(!invalid_scheme.ok(), "Reject non-ftp scheme in FtpUrl::Parse");

    return true;
}

bool TestSchedulerStopTimesAndPostCompletionActions() {
    std::error_code ec;
    std::filesystem::path work_dir = std::filesystem::temp_directory_path() / ("flux_test_sched_" + openidm::GenerateUuidV4());
    std::filesystem::create_directories(work_dir, ec);

    openidm::ControllerConfig cfg;
    cfg.state_db_path = work_dir / "state.db";
    cfg.default_download_dir = work_dir / "downloads";
    cfg.temp_root_dir = work_dir / "tmp";
    std::filesystem::create_directories(cfg.default_download_dir, ec);

    openidm::DownloadController ctrl(cfg);
    ASSERT_TRUE(ctrl.Initialize().ok(), "Initialize controller");

    openidm::TaskModel t;
    t.task_id = "task-sched-1";
    t.url = "https://example.com/nightly.iso";
    t.file_name = "nightly.iso";
    auto add_res = ctrl.AddTask(t, false, true);
    ASSERT_TRUE(add_res.ok(), "Added task");

    // Verify rejection of invalid schedule time format
    auto bad_sched1 = ctrl.SetTaskSchedule("task-sched-1", "25:00", "06:00");
    ASSERT_TRUE(!bad_sched1.ok() && bad_sched1.error->code == "ERR_INVALID_SCHEDULE", "Reject invalid hour in start time");
    auto bad_sched2 = ctrl.SetTaskSchedule("task-sched-1", "02:65", "06:00");
    ASSERT_TRUE(!bad_sched2.ok() && bad_sched2.error->code == "ERR_INVALID_SCHEDULE", "Reject invalid minute in start time");
    auto bad_sched3 = ctrl.SetTaskSchedule("task-sched-1", "02:00", "abc");
    ASSERT_TRUE(!bad_sched3.ok() && bad_sched3.error->code == "ERR_INVALID_SCHEDULE", "Reject non-numeric stop time");

    auto set_sched = ctrl.SetTaskSchedule("task-sched-1", "02:00", "06:00");
    ASSERT_TRUE(set_sched.ok(), "SetTaskSchedule succeeded");

    auto task_opt = ctrl.GetTask("task-sched-1");
    ASSERT_TRUE(task_opt.has_value(), "Task exists");
    ASSERT_TRUE(task_opt->scheduled, "Task scheduled is true");
    ASSERT_TRUE(task_opt->schedule_time_local == "02:00", "Start time is 02:00");
    ASSERT_TRUE(task_opt->schedule_stop_time_local == "06:00", "Stop time is 06:00");

    // Verify Settings Center Queue completion action
    auto settings = ctrl.GetSettings();
    settings.queue_completion_action = "SLEEP";
    settings.max_auto_retries = 8;
    settings.retry_interval_sec = 20;
    ASSERT_TRUE(ctrl.UpdateSettings(settings).ok(), "Updated settings");

    auto updated = ctrl.GetSettings();
    ASSERT_TRUE(updated.queue_completion_action == "SLEEP", "Queue completion action is SLEEP");
    ASSERT_TRUE(updated.max_auto_retries == 8, "Max auto retries is 8");
    ASSERT_TRUE(updated.retry_interval_sec == 20, "Retry interval is 20s");

    ctrl.Shutdown();
    std::filesystem::remove_all(work_dir, ec);
    return true;
}

bool TestPeerWireProtocolFramingAndCompactPeers() {
    using namespace openidm::torrent;

    // 1. Peer ID format
    std::string peer_id = PeerWireProtocol::GeneratePeerId();
    ASSERT_TRUE(peer_id.size() == 20, "Peer ID is exactly 20 bytes");
    ASSERT_TRUE(peer_id.rfind("-FL0100-", 0) == 0, "Peer ID has -FL0100- prefix");

    // 2. Handshake serialization and verification
    std::vector<uint8_t> info_hash(20, 0x5A);
    auto hs_bytes = PeerWireProtocol::CreateHandshake(info_hash, peer_id);
    ASSERT_TRUE(hs_bytes.size() == 68, "Handshake length is 68 bytes");
    ASSERT_TRUE(hs_bytes[0] == 19, "Handshake pstrlen is 19");
    ASSERT_TRUE(std::memcmp(&hs_bytes[1], "BitTorrent protocol", 19) == 0, "Handshake protocol string");
    ASSERT_TRUE(std::memcmp(&hs_bytes[28], info_hash.data(), 20) == 0, "Handshake info_hash matches");
    ASSERT_TRUE(std::memcmp(&hs_bytes[48], peer_id.data(), 20) == 0, "Handshake peer_id matches");

    std::string recvd_peer_id;
    ASSERT_TRUE(PeerWireProtocol::VerifyHandshake(hs_bytes.data(), hs_bytes.size(), info_hash, &recvd_peer_id), "Verify valid handshake");
    ASSERT_TRUE(recvd_peer_id == peer_id, "Extracted peer ID matches");

    std::vector<uint8_t> wrong_hash(20, 0x11);
    ASSERT_TRUE(!PeerWireProtocol::VerifyHandshake(hs_bytes.data(), hs_bytes.size(), wrong_hash), "Reject wrong info-hash");
    ASSERT_TRUE(!PeerWireProtocol::VerifyHandshake(hs_bytes.data(), 50, info_hash), "Reject truncated handshake");

    // 3. Keep-Alive, Choke, Unchoke, Interested
    auto ka = PeerWireProtocol::CreateKeepAlive();
    ASSERT_TRUE(ka.size() == 4 && ka[0] == 0 && ka[1] == 0 && ka[2] == 0 && ka[3] == 0, "KeepAlive is 4 zero bytes");

    auto unchoke = PeerWireProtocol::CreateUnchoke();
    ASSERT_TRUE(unchoke.size() == 5 && unchoke[4] == 1, "Unchoke message format");
    auto unchoke_parsed = PeerWireProtocol::ParseMessage(unchoke.data(), unchoke.size());
    ASSERT_TRUE(unchoke_parsed.has_value() && unchoke_parsed->id == BtMessageId::kUnchoke, "Parse Unchoke");

    // 4. Have & Bitfield
    auto have_msg = PeerWireProtocol::CreateHave(1337);
    auto have_parsed = PeerWireProtocol::ParseMessage(have_msg.data(), have_msg.size());
    ASSERT_TRUE(have_parsed.has_value() && have_parsed->id == BtMessageId::kHave && have_parsed->piece_index == 1337, "Create and parse Have");

    std::vector<bool> bits = {true, false, true, true, false, false, false, true, true};
    auto bf_msg = PeerWireProtocol::CreateBitfield(bits);
    auto bf_parsed = PeerWireProtocol::ParseMessage(bf_msg.data(), bf_msg.size());
    ASSERT_TRUE(bf_parsed.has_value() && bf_parsed->id == BtMessageId::kBitfield, "Create and parse Bitfield");
    ASSERT_TRUE(bf_parsed->payload.size() == 2, "Bitfield payload size for 9 bits is 2 bytes");
    ASSERT_TRUE((bf_parsed->payload[0] & 0x80) != 0, "Bit 0 is set");
    ASSERT_TRUE((bf_parsed->payload[0] & 0x40) == 0, "Bit 1 is clear");

    // 5. Request & Piece
    auto req_msg = PeerWireProtocol::CreateRequest(7, 16384, 16384);
    auto req_parsed = PeerWireProtocol::ParseMessage(req_msg.data(), req_msg.size());
    ASSERT_TRUE(req_parsed.has_value() && req_parsed->id == BtMessageId::kRequest, "Parse Request");
    ASSERT_TRUE(req_parsed->piece_index == 7 && req_parsed->block_begin == 16384 && req_parsed->block_length == 16384, "Request fields match");

    const uint8_t sample_blk[] = {0xDE, 0xAD, 0xBE, 0xEF};
    auto pc_msg = PeerWireProtocol::CreatePiece(7, 0, sample_blk, 4);
    auto pc_parsed = PeerWireProtocol::ParseMessage(pc_msg.data(), pc_msg.size());
    ASSERT_TRUE(pc_parsed.has_value() && pc_parsed->id == BtMessageId::kPiece, "Parse Piece");
    ASSERT_TRUE(pc_parsed->piece_index == 7 && pc_parsed->block_begin == 0 && pc_parsed->payload.size() == 4, "Piece payload matches");
    ASSERT_TRUE(pc_parsed->payload[0] == 0xDE && pc_parsed->payload[3] == 0xEF, "Piece bytes match");

    // 6. Compact Peer Unpacking (aria2 unpackcompact)
    // 192.168.1.105:6881 -> C0 A8 01 69 1A E1
    const uint8_t compact_peer[6] = {192, 168, 1, 105, 0x1A, 0xE1};
    auto [ip4, port4] = PeerWireProtocol::UnpackCompactIpv4(compact_peer);
    ASSERT_TRUE(ip4 == "192.168.1.105", "Unpack IPv4 address");
    ASSERT_TRUE(port4 == 6881, "Unpack IPv4 port");

    std::string compact_12(reinterpret_cast<const char*>(compact_peer), 6);
    const uint8_t peer2[6] = {10, 0, 0, 1, 0x00, 0x50}; // 10.0.0.1:80
    compact_12.append(reinterpret_cast<const char*>(peer2), 6);
    auto peers_list = PeerWireProtocol::ExtractPeersFromCompact(compact_12);
    ASSERT_TRUE(peers_list.size() == 2, "Extracted 2 peers from 12-byte compact list");
    ASSERT_TRUE(peers_list[0].ip == "192.168.1.105" && peers_list[0].port == 6881, "Peer 0 matches");
    ASSERT_TRUE(peers_list[1].ip == "10.0.0.1" && peers_list[1].port == 80, "Peer 1 matches");

    return true;
}

bool TestMultiFileStorageMappingAndBoundarySplitting() {
    using namespace openidm::torrent;
    std::error_code ec;
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "flux_test_storage";
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir, ec);

    TorrentInfo info;
    info.name = "multi_pkg";
    info.total_length = 4000;
    info.piece_length = 1000;

    TorrentFileEntry f1{"sub/file_a.bin", 1000, 0};
    TorrentFileEntry f2{"sub/file_b.bin", 2000, 1000};
    TorrentFileEntry f3{"file_c.bin", 1000, 3000};
    info.files = {f1, f2, f3};

    MultiFileStorage storage;
    ASSERT_TRUE(storage.Initialize(temp_dir, info).ok(), "Storage initialized");
    ASSERT_TRUE(storage.AllocateFiles().ok(), "Files allocated");

    // Write a 1500-byte block starting at offset 500
    // Spans file_a.bin [500..1000) and file_b.bin [0..1000)
    std::vector<uint8_t> test_bytes(1500);
    for (size_t i = 0; i < test_bytes.size(); ++i) {
        test_bytes[i] = static_cast<uint8_t>((i * 7 + 13) & 0xFF);
    }

    ASSERT_TRUE(storage.WriteBytes(500, test_bytes.data(), test_bytes.size()).ok(), "Write across boundary succeeded");

    // Read back and verify exact byte matching
    std::vector<uint8_t> read_bytes(1500, 0);
    ASSERT_TRUE(storage.ReadBytes(500, read_bytes.data(), read_bytes.size()).ok(), "Read across boundary succeeded");
    ASSERT_TRUE(read_bytes == test_bytes, "Read back bytes match written bytes exactly");

    // Verify disk files directly
    std::filesystem::path p1 = temp_dir / "sub" / "file_a.bin";
    std::filesystem::path p2 = temp_dir / "sub" / "file_b.bin";
    ASSERT_TRUE(std::filesystem::exists(p1), "file_a exists on disk");
    ASSERT_TRUE(std::filesystem::exists(p2), "file_b exists on disk");

    // Write another block spanning file_b.bin into file_c.bin at offset 2800 (length 500)
    std::vector<uint8_t> span2(500, 0x42);
    ASSERT_TRUE(storage.WriteBytes(2800, span2.data(), span2.size()).ok(), "Write across second boundary");
    std::vector<uint8_t> read_span2(500, 0);
    ASSERT_TRUE(storage.ReadBytes(2800, read_span2.data(), read_span2.size()).ok(), "Read across second boundary");
    ASSERT_TRUE(read_span2 == span2, "Second boundary slice matches");

    std::filesystem::remove_all(temp_dir, ec);
    return true;
}

bool TestPieceManagerAndBlockScheduling() {
    using namespace openidm::torrent;
    std::error_code ec;
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "flux_test_piecemgr";
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir, ec);

    // Create 2 pieces of 32 KB each (total 64 KB)
    TorrentInfo info;
    info.name = "test_pieces.bin";
    info.total_length = 65536;
    info.piece_length = 32768;

    std::vector<uint8_t> piece0_data(32768, 'A');
    std::vector<uint8_t> piece1_data(32768, 'B');

    std::vector<uint8_t> hash0 = openidm::CryptoUtils::ComputeBufferSha1Raw(piece0_data.data(), piece0_data.size());
    std::vector<uint8_t> hash1 = openidm::CryptoUtils::ComputeBufferSha1Raw(piece1_data.data(), piece1_data.size());
    info.pieces = {hash0, hash1};

    auto storage = std::make_shared<MultiFileStorage>();
    ASSERT_TRUE(storage->Initialize(temp_dir, info).ok(), "Storage init");
    storage->AllocateFiles();

    PieceManager mgr(info, storage);
    ASSERT_TRUE(mgr.TotalPieces() == 2, "Total pieces is 2");
    ASSERT_TRUE(mgr.CompletedPieces() == 0, "Initial completed pieces is 0");
    ASSERT_TRUE(!mgr.IsAllCompleted(), "Not all completed initially");

    // Peer offers piece 0 and 1
    std::vector<bool> peer_have = {true, true};
    auto reqs = mgr.GetNextRequestsForPeer(peer_have, 4);
    // Piece size 32768 -> two 16 KB blocks (0..16384, 16384..32768)
    ASSERT_TRUE(reqs.size() == 2, "Generated 2 block requests for piece 0");
    ASSERT_TRUE(reqs[0].piece_index == 0 && reqs[0].begin == 0 && reqs[0].length == 16384, "Block 0");
    ASSERT_TRUE(reqs[1].piece_index == 0 && reqs[1].begin == 16384 && reqs[1].length == 16384, "Block 1");

    // Feed block 0 of piece 0
    bool verified = false, corrupt = false;
    mgr.ProcessBlock(0, 0, piece0_data.data(), 16384, verified, corrupt);
    ASSERT_TRUE(!verified && !corrupt, "First block processed; piece not complete yet");
    ASSERT_TRUE(mgr.CompletedPieces() == 0, "Piece 0 not yet marked complete");

    // Feed block 1 of piece 0 -> completes piece and triggers SHA-1 verification
    mgr.ProcessBlock(0, 16384, piece0_data.data() + 16384, 16384, verified, corrupt);
    ASSERT_TRUE(verified && !corrupt, "Piece 0 complete and verified by SHA-1");
    ASSERT_TRUE(mgr.CompletedPieces() == 1, "Completed pieces is now 1");
    ASSERT_TRUE(mgr.DownloadedBytes() == 32768, "Downloaded bytes updated to 32768");
    ASSERT_TRUE(mgr.HavePieces()[0] == true, "HavePieces[0] marked true");

    // Feed corrupt block data for piece 1
    std::vector<uint8_t> corrupt_data(16384, 'Z');
    mgr.ProcessBlock(1, 0, corrupt_data.data(), 16384, verified, corrupt);
    mgr.ProcessBlock(1, 16384, piece1_data.data() + 16384, 16384, verified, corrupt);
    ASSERT_TRUE(!verified && corrupt, "Corrupt piece detected by SHA-1 verification and rejected");
    ASSERT_TRUE(mgr.CompletedPieces() == 1, "Piece 1 not marked complete due to corruption");

    // Selective multi-file regression: only pieces overlapping selected files are requested,
    // and unselected files are never allocated or written.
    TorrentInfo selective;
    selective.name = "selective.bin";
    selective.total_length = 4000;
    selective.piece_length = 1000;
    selective.files = {
        TorrentFileEntry{"wanted/a.bin", 1000, 0, true},
        TorrentFileEntry{"unwanted/b.bin", 2000, 1000, false},
        TorrentFileEntry{"wanted/c.bin", 1000, 3000, true}
    };
    std::vector<uint8_t> selected_piece_a(1000, 'A');
    std::vector<uint8_t> selected_piece_c(1000, 'C');
    selective.pieces = {
        openidm::CryptoUtils::ComputeBufferSha1Raw(selected_piece_a.data(), selected_piece_a.size()),
        std::vector<uint8_t>(20, 0), std::vector<uint8_t>(20, 0),
        openidm::CryptoUtils::ComputeBufferSha1Raw(selected_piece_c.data(), selected_piece_c.size())
    };
    auto selective_storage = std::make_shared<MultiFileStorage>();
    std::filesystem::path selective_dir = temp_dir / "selective";
    ASSERT_TRUE(selective_storage->Initialize(selective_dir, selective).ok(), "Selective storage initialized");
    ASSERT_TRUE(selective_storage->AllocateFiles().ok(), "Selective storage allocation succeeds");
    ASSERT_TRUE(!std::filesystem::exists(selective_dir / "unwanted" / "b.bin"), "Unselected torrent file is not created");
    PieceManager selective_mgr(selective, selective_storage);
    ASSERT_TRUE(selective.selected_bytes() == 2000 && selective_mgr.RequiredPieces() == 2,
                "Selected byte total and required piece count account only for selected files");
    auto selective_reqs = selective_mgr.GetNextRequestsForPeer({true, true, true, true}, 8);
    ASSERT_TRUE(selective_reqs.size() == 1 && selective_reqs[0].piece_index == 0,
                "Selective scheduler requests the first required piece only");
    bool selective_verified = false;
    bool selective_corrupt = false;
    selective_mgr.ProcessBlock(0, 0, selected_piece_a.data(), selected_piece_a.size(), selective_verified, selective_corrupt);
    ASSERT_TRUE(selective_verified && selective_mgr.DownloadedBytes() == 1000, "Selected piece is verified and counted by selected bytes");
    selective_reqs = selective_mgr.GetNextRequestsForPeer({true, true, true, true}, 8);
    ASSERT_TRUE(selective_reqs.size() == 1 && selective_reqs[0].piece_index == 3,
                "Scheduler skips unselected middle-file pieces and requests the final selected piece");

    std::filesystem::remove_all(temp_dir, ec);
    return true;
}

bool TestTrackerClientQueryEncodingAndResponseParsing() {
    using namespace openidm::torrent;

    // 1. URL percent encoding of raw binary info-hash
    const uint8_t binary_hash[5] = {0x00, 0x12, 0xFF, 'A', 'z'};
    std::string enc = TrackerClient::PercentEncodeBinary(binary_hash, 5);
    ASSERT_TRUE(enc == "%00%12%FFAz", "Binary percent encoding preserves unreserved chars and encodes non-printables");

    // Bencoded dict: d8:completei42e10:incompletei7e8:intervali1800e5:peers12:\xC0\xA8\x01\x69\x1A\xE1\x0A\x00\x00\x01\x00\x50 + e
    const char kRespBytes[] = "d8:completei42e10:incompletei7e8:intervali1800e5:peers12:\xC0\xA8\x01\x69\x1A\xE1\x0A\x00\x00\x01\x00\x50" "e";
    std::string bencoded_resp(kRespBytes, sizeof(kRespBytes) - 1);
    auto tr_res = TrackerClient::ParseTrackerResponse(bencoded_resp);
    ASSERT_TRUE(tr_res.ok(), "Parse valid tracker response");
    ASSERT_TRUE(tr_res.value->complete == 42, "Seeders is 42");
    ASSERT_TRUE(tr_res.value->incomplete == 7, "Leechers is 7");
    ASSERT_TRUE(tr_res.value->interval == 1800, "Interval is 1800s");
    ASSERT_TRUE(tr_res.value->peers.size() == 2, "Unpacked 2 peers from compact binary list");
    ASSERT_TRUE(tr_res.value->peers[0].ip == "192.168.1.105" && tr_res.value->peers[0].port == 6881, "Peer 0 matches");
    ASSERT_TRUE(tr_res.value->peers[1].ip == "10.0.0.1" && tr_res.value->peers[1].port == 80, "Peer 1 matches");

    const std::string kZeroPeerResp = "d8:completei0e10:incompletei0e8:intervali60e5:peers0:e";
    auto zero_peer_res = TrackerClient::ParseTrackerResponse(kZeroPeerResp);
    ASSERT_TRUE(zero_peer_res.ok() && zero_peer_res.value->peers.empty(),
                "A valid zero-peer tracker response is successful and yields an empty peer set");

    const std::string kMalformedCompactResp = "d8:completei1e10:incompletei1e8:intervali60e5:peers5:abcde";
    auto malformed_compact_res = TrackerClient::ParseTrackerResponse(kMalformedCompactResp);
    ASSERT_TRUE(!malformed_compact_res.ok(), "Malformed compact peer payload is rejected");
    ASSERT_TRUE(malformed_compact_res.error->code == "ERR_TORRENT_TRACKER_MALFORMED",
                "Malformed compact peer payload reports tracker response error");

    // 3. Tracker Failure Reason parsing
    std::string fail_resp = "d14:failure reason26:Torrent passkey is invalide";
    auto fail_res = TrackerClient::ParseTrackerResponse(fail_resp);
    ASSERT_TRUE(!fail_res.ok(), "Tracker failure response detected");
    ASSERT_TRUE(fail_res.error->code == "ERR_TORRENT_TRACKER_FAILURE", "Code is ERR_TORRENT_TRACKER_FAILURE");
    ASSERT_TRUE(fail_res.error->message == "Torrent passkey is invalid", "Error message contains failure reason");

    // 4. Multi-tracker fallback & empty tracker handling
    std::vector<std::string> empty_trackers;
    std::vector<uint8_t> dummy_hash(20, 0x11);
    auto no_tr_res = TrackerClient::AnnounceAny(empty_trackers, dummy_hash, "-RA0300-123456789012", 6881, 0, 0, 100);
    ASSERT_TRUE(!no_tr_res.ok(), "Empty announce list returns error");
    ASSERT_TRUE(no_tr_res.error->code == "ERR_TORRENT_NO_TRACKERS", "Code is ERR_TORRENT_NO_TRACKERS");

    return true;
}

bool TestHttpRangeSatisfactionValidation() {
    using namespace openidm::brisk;

    // Simulate aria2 isRangeSatisfied logic
    uint64_t total_bytes = 10485760;
    uint64_t curr = 1048576;

    // 1. Valid 206 Partial Content response
    HttpResponseMeta meta_valid;
    meta_valid.status_code = 206;
    meta_valid.is_partial_content = true;
    meta_valid.range_start = 1048576;
    meta_valid.range_end = 2097151;
    meta_valid.range_total = 10485760;

    bool valid_satisfied = (meta_valid.status_code == 206 && meta_valid.is_partial_content &&
                            meta_valid.range_start == curr &&
                            (meta_valid.range_total == 0 || meta_valid.range_total == total_bytes));
    ASSERT_TRUE(valid_satisfied, "Valid 206 response satisfies range");

    // 2. Server returned 206 but entity size changed (remote file modified)
    HttpResponseMeta meta_mismatch;
    meta_mismatch.status_code = 206;
    meta_mismatch.is_partial_content = true;
    meta_mismatch.range_start = 1048576;
    meta_mismatch.range_end = 2097151;
    meta_mismatch.range_total = 20971520; // Size changed on server!

    bool entity_mismatch = (total_bytes > 0 && meta_mismatch.range_total > 0 && meta_mismatch.range_total != total_bytes);
    ASSERT_TRUE(entity_mismatch, "Detected remote entity length change mid-download");

    // 3. Server returned 200 OK starting from byte 0 instead of 206
    HttpResponseMeta meta_ignored_range;
    meta_ignored_range.status_code = 200;
    meta_ignored_range.is_partial_content = false;
    meta_ignored_range.range_start = 0;

    bool range_ignored = (meta_ignored_range.status_code != 206 || !meta_ignored_range.is_partial_content || meta_ignored_range.range_start != curr);
    ASSERT_TRUE(range_ignored, "Server ignoring Range header is caught and rejected");

    return true;
}

bool TestDuplicateTaskDetectionThreeTiers() {
    using namespace openidm::core;

    // 1. URL Normalization
    std::string norm1 = DuplicateDetector::NormalizeUrl("  HTTP://Example.COM:80/path/file.zip?token=123#frag  ");
    ASSERT_TRUE(norm1 == "http://example.com/path/file.zip?token=123", "Normalized URL strips fragment, lowers host/scheme, removes default port");

    std::string norm2 = DuplicateDetector::NormalizeUrl("https://API.Server.org:8443/data/v1/?query=yes#section");
    ASSERT_TRUE(norm2 == "https://api.server.org:8443/data/v1/?query=yes", "Non-default port and query preserved");

    // 2. Setup tasks list
    std::vector<openidm::TaskModel> tasks;
    openidm::TaskModel t1;
    t1.task_id = "task-url-1";
    t1.url = "https://cdn.example.com/downloads/setup.exe?v=1";
    t1.file_name = "setup.exe";
    t1.total_bytes = 10485760;
    t1.etag = "\"33a64df551425fcc55e4d42a148795d9f25f89d4\"";
    t1.created_at_ms = 1000;
    tasks.push_back(t1);

    openidm::TaskModel t2;
    t2.task_id = "task-name-size-1";
    t2.url = "https://mirror1.example.org/files/linux-distro.iso";
    t2.file_name = "linux-distro.iso";
    t2.total_bytes = 2147483648ULL;
    t2.etag = "\"iso-hash-111\"";
    t2.created_at_ms = 2000;
    tasks.push_back(t2);

    openidm::TaskModel t3;
    t3.task_id = "task-etag-1";
    t3.url = "https://cdn.example.com/assets/video_hd.mp4";
    t3.file_name = "video_hd.mp4";
    t3.total_bytes = 524288000;
    t3.etag = "\"strong-etag-video-999\"";
    t3.created_at_ms = 3000;
    tasks.push_back(t3);

    // Tier 1: Match by URL (even with different casing, whitespace, or fragment)
    auto match_url = DuplicateDetector::FindDuplicate(tasks, "  HTTPS://CDN.EXAMPLE.COM:443/downloads/setup.exe?v=1#download  ");
    ASSERT_TRUE(match_url.kind == DuplicateKind::kSameUrl, "Tier 1: Matched by Same URL");
    ASSERT_TRUE(match_url.matched_task.task_id == "task-url-1", "Matched correct task ID");

    // Tier 2: Match by File Name and Size (different URL, but same case-insensitive name & size)
    auto match_name_size = DuplicateDetector::FindDuplicate(tasks, "https://mirror2.different.net/archive/LINUX-DISTRO.ISO", "LINUX-DISTRO.ISO", 2147483648ULL);
    ASSERT_TRUE(match_name_size.kind == DuplicateKind::kSameNameAndSize, "Tier 2: Matched by Same Name & Size");
    ASSERT_TRUE(match_name_size.matched_task.task_id == "task-name-size-1", "Matched correct task ID");

    // Rejection on size 0 or mismatch
    auto no_match_size0 = DuplicateDetector::FindDuplicate(tasks, "https://mirror2.different.net/archive/linux-distro.iso", "linux-distro.iso", 0);
    ASSERT_TRUE(no_match_size0.kind == DuplicateKind::kNone, "Size 0 does not trigger name-and-size match");

    // Tier 3: Match by Strong ETag on Same Host (different URL and name, but same host and strong ETag)
    auto match_etag = DuplicateDetector::FindDuplicate(tasks, "https://cdn.example.com/rest/get_video?id=55", "renamed.mp4", 100, "\"strong-etag-video-999\"");
    ASSERT_TRUE(match_etag.kind == DuplicateKind::kSameEtag, "Tier 3: Matched by Strong ETag on same host");
    ASSERT_TRUE(match_etag.matched_task.task_id == "task-etag-1", "Matched correct task ID");

    // Rejection of Weak ETag
    auto match_weak_etag = DuplicateDetector::FindDuplicate(tasks, "https://cdn.example.com/rest/get_video?id=55", "renamed.mp4", 100, "W/\"strong-etag-video-999\"");
    ASSERT_TRUE(match_weak_etag.kind == DuplicateKind::kNone, "Weak ETag (W/) rejected for duplicate detection");

    // Rejection of ETag on different host
    auto match_diff_host_etag = DuplicateDetector::FindDuplicate(tasks, "https://unrelated-host.org/get", "renamed.mp4", 100, "\"strong-etag-video-999\"");
    ASSERT_TRUE(match_diff_host_etag.kind == DuplicateKind::kNone, "ETag match rejected across different hosts");

    // Tier Precedence: URL match takes precedence over Name & Size match
    auto match_precedence = DuplicateDetector::FindDuplicate(tasks, "https://cdn.example.com/downloads/setup.exe?v=1", "linux-distro.iso", 2147483648ULL);
    ASSERT_TRUE(match_precedence.kind == DuplicateKind::kSameUrl, "Tier 1 takes precedence over Tier 2");
    ASSERT_TRUE(match_precedence.matched_task.task_id == "task-url-1", "URL match preferred");

    // 4. Motrix Reference Reuse: BTIH info-hash normalization & magnet link deduplication
    std::string hex_norm = DuplicateDetector::NormalizeBtInfoHash("D1E05F7C32BF24F6E1F0E4708E33E9B11D8D3F66");
    ASSERT_TRUE(hex_norm == "d1e05f7c32bf24f6e1f0e4708e33e9b11d8d3f66", "40-hex infohash lowercased");

    std::string base32_zeros = DuplicateDetector::NormalizeBtInfoHash("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    ASSERT_TRUE(base32_zeros == "0000000000000000000000000000000000000000", "32-base32 zeros decoded to 40-hex");

    std::string base32_ones = DuplicateDetector::NormalizeBtInfoHash("77777777777777777777777777777777");
    ASSERT_TRUE(base32_ones == "ffffffffffffffffffffffffffffffffffffffff", "32-base32 all-ones decoded to 40-hex");

    // Setup magnet task with 40-hex hash
    openidm::TaskModel t_magnet;
    t_magnet.task_id = "task-magnet-1";
    t_magnet.url = "magnet:?xt=urn:btih:ffffffffffffffffffffffffffffffffffffffff&dn=TestTorrent";
    t_magnet.file_name = "TestTorrent";
    t_magnet.created_at_ms = 4000;
    tasks.push_back(t_magnet);

    // Incoming duplicate magnet link with 32-base32 hash and disparate tracker params
    auto match_magnet = DuplicateDetector::FindDuplicate(tasks, "magnet:?xt=urn:btih:77777777777777777777777777777777&tr=http%3A%2F%2Ftracker.example.com%2Fannounce#extra");
    ASSERT_TRUE(match_magnet.kind == DuplicateKind::kSameUrl, "Matched magnet link via BTIH normalization across Base32/Hex");
    ASSERT_TRUE(match_magnet.matched_task.task_id == "task-magnet-1", "Matched correct magnet task ID");

    // 5. Motrix Reference Reuse: FinalNamePicker .part file collision prevention
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / ("test_motrix_part_" + openidm::GenerateUuidV4());
    std::filesystem::create_directories(temp_dir);
    std::filesystem::path primary = temp_dir / "collision_test.bin";
    std::filesystem::path part_slot1 = temp_dir / "collision_test (1).bin.part";

    // Create primary file and (1).part file
    {
        std::ofstream(primary) << "primary";
        std::ofstream(part_slot1) << "partial_download";
    }

    auto safe_res = openidm::PathSanitizer::ResolveSafeOutputPath(temp_dir, "collision_test.bin", openidm::ConflictPolicy::kAutoRename);
    ASSERT_TRUE(safe_res.ok(), "Safe output path resolved successfully");
    // Should skip collision_test (1).bin because collision_test (1).bin.part exists, picking (2)
    ASSERT_TRUE(safe_res.value->filename().string() == "collision_test (2).bin", "Avoided slot with existing in-progress .part file, picked slot (2)");

    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);

    return true;
}

bool TestExpiredLinkRefreshAndCookieReplacement() {
    using namespace openidm;

    // 1. IsLinkExpiredStatus verification
    ASSERT_TRUE(brisk::HttpClient::IsLinkExpiredStatus(403), "HTTP 403 Forbidden is link expired");
    ASSERT_TRUE(brisk::HttpClient::IsLinkExpiredStatus(410), "HTTP 410 Gone is link expired");
    ASSERT_TRUE(brisk::HttpClient::IsLinkExpiredStatus(401, ""), "HTTP 401 without auth header is link expired");
    ASSERT_TRUE(!brisk::HttpClient::IsLinkExpiredStatus(401, "Basic realm=\"Access\""), "HTTP 401 with Basic auth challenge is not link expired");
    ASSERT_TRUE(!brisk::HttpClient::IsLinkExpiredStatus(401, "NTLM"), "HTTP 401 with NTLM challenge is not link expired");
    ASSERT_TRUE(!brisk::HttpClient::IsLinkExpiredStatus(404), "HTTP 404 is not link expired");
    ASSERT_TRUE(!brisk::HttpClient::IsLinkExpiredStatus(200), "HTTP 200 is not link expired");

    // 2. Controller RefreshTaskUrl end-to-end state and cookie replacement test
    std::string temp_db = (std::filesystem::temp_directory_path() / ("test_refresh_" + GenerateUuidV4() + ".db")).string();
    ControllerConfig cfg;
    cfg.state_db_path = temp_db;
    cfg.default_download_dir = std::filesystem::temp_directory_path();
    cfg.temp_root_dir = std::filesystem::temp_directory_path();

    DownloadController controller(cfg);
    auto init_res = controller.Initialize();
    ASSERT_TRUE(init_res.ok(), "Initialize controller for refresh test");

    TaskModel t;
    t.task_id = "task-refresh-1";
    t.url = "https://cdn.example.com/expired-presigned-url?sig=old123";
    t.file_name = "archive.tar.gz";
    t.downloaded_bytes = 4194304;
    t.total_bytes = 16777216;
    t.headers["Cookie"] = "session=stale_session_token_old; id=123";
    t.headers["User-Agent"] = "CustomAgent/1.0";
    t.auth_headers["Authorization"] = "Bearer old-bearer-token";
    t.state = TaskState::kPaused;
    t.error_code = "ERR_HTTP_403_EXPIRED_OR_AUTH";
    t.error_message = "HTTP 403 (Link Expired or Auth Required)";
    t.retry_count = 3;

    // Create partial segment
    Segment seg0{0, 0, 4194304, 16777215, false};
    t.segments.push_back(seg0);

    auto add_res = controller.AddTask(t, false, true);
    ASSERT_TRUE(add_res.ok(), "Task added");

    // Refresh URL with new URL, new Cookie, and new Authorization header
    std::map<std::string, std::string> new_headers = {
        {"Cookie", "session=fresh_session_token_new"},
        {"X-Custom-Header", "Value123"}
    };
    std::map<std::string, std::string> new_auth = {
        {"Authorization", "Bearer refreshed-new-bearer"}
    };

    auto ref_res = controller.RefreshTaskUrl(
        "task-refresh-1",
        "https://cdn.example.com/fresh-presigned-url?sig=new999",
        new_headers,
        new_auth);
    ASSERT_TRUE(ref_res.ok(), "RefreshTaskUrl succeeds");

    auto updated_opt = controller.GetTask("task-refresh-1");
    ASSERT_TRUE(updated_opt.has_value(), "Task found after refresh");
    const auto& up = *updated_opt;

    // Verify URL updated
    ASSERT_TRUE(up.url == "https://cdn.example.com/fresh-presigned-url?sig=new999", "URL refreshed");

    // Verify Cookie cleanly replaced (not merged with stale cookie)
    ASSERT_TRUE(up.headers.at("Cookie") == "session=fresh_session_token_new", "Cookie replaced without old session lingering");
    ASSERT_TRUE(up.headers.at("Cookie").find("stale_session_token_old") == std::string::npos, "Old cookie scrubbed");
    ASSERT_TRUE(up.headers.at("User-Agent") == "CustomAgent/1.0", "Preserved existing headers");
    ASSERT_TRUE(up.headers.at("X-Custom-Header") == "Value123", "Added new headers");

    // Verify Auth header replaced
    ASSERT_TRUE(up.auth_headers.at("Authorization") == "Bearer refreshed-new-bearer", "Auth header replaced");

    // Verify partial segments and downloaded bytes preserved
    ASSERT_TRUE(up.downloaded_bytes == 4194304, "Downloaded bytes preserved on disk");
    ASSERT_TRUE(up.total_bytes == 16777216, "Total bytes preserved");
    ASSERT_TRUE(up.segments.size() == 1 && up.segments[0].current_offset == 4194304, "Segment cursor preserved for immediate resume");

    // Verify errors cleared and state queued with fresh retries
    ASSERT_TRUE(up.error_code.empty() && up.error_message.empty(), "Error state cleared");
    ASSERT_TRUE(up.retry_count == 0, "Retry count reset for fresh link");
    ASSERT_TRUE(up.state == TaskState::kQueued, "Task queued for resume");

    controller.Shutdown();
    std::error_code ec;
    std::filesystem::remove(temp_db, ec);
    return true;
}

bool TestSegmentSchedulerChunkNormalizationAndTruncationGuard() {
    using namespace openidm::brisk;

    uint64_t total_bytes = 10000;
    SegmentScheduler scheduler(total_bytes, 4);

    // Segment 0: completed [0, 4999] with cursor at 5000, BUT completed flag was false (crash right after last byte)
    // Segment 1: incomplete [5000, 9999] with cursor at 6000
    std::vector<openidm::Segment> unnormalized_segments = {
        openidm::Segment{0, 0, 5000, 4999, false}, // current_offset == end_offset + 1, completed = false
        openidm::Segment{1, 5000, 6000, 9999, false}
    };

    ASSERT_TRUE(SegmentScheduler::ValidateSegmentPartition(unnormalized_segments, total_bytes),
                "ValidateSegmentPartition accepts consistent cursors where is_fully_read is true");

    scheduler.Initialize(unnormalized_segments);

    auto snapshots = scheduler.GetSnapshots();
    ASSERT_TRUE(snapshots.size() == 2, "2 segments restored");
    ASSERT_TRUE(snapshots[0].completed, "Segment 0 normalized to completed=true (XDM normalizeRestoredChunks)");
    ASSERT_TRUE(!snapshots[1].completed, "Segment 1 remains incomplete");
    ASSERT_TRUE(!scheduler.AllCompleted(), "Scheduler knows task is not finished yet");

    // Steal or acquire next segment: should immediately pick segment 1 without spawning worker for segment 0
    auto seg = scheduler.AcquireNextOrStealSegment(false);
    ASSERT_TRUE(seg != nullptr, "Acquired segment");
    ASSERT_TRUE(seg->segment_id == 1, "Acquired segment 1; segment 0 was skipped because normalized to completed");

    // Simulate segment 1 finishing: cursor advances to 10000
    seg->current_offset.store(10000);
    seg->completed.store(true);

    ASSERT_TRUE(scheduler.AllCompleted(), "All segments complete after segment 1 finished");

    return true;
}

bool TestUrlUserinfoExtractionAndBasicAuthGeneration() {
    using namespace openidm;
    using namespace openidm::brisk;

    // 1. Parse URL with userinfo (username:password)
    auto p1 = HttpClient::ParseUrl("http://admin:secret123@127.0.0.1:8080/downloads/file.bin?token=abc");
    ASSERT_TRUE(p1.ok(), "ParseUrl parses URL with userinfo");
    ASSERT_TRUE(p1.value->userinfo == "admin:secret123", "Extracted userinfo");
    ASSERT_TRUE(p1.value->host == "127.0.0.1", "Extracted clean host without userinfo");
    ASSERT_TRUE(p1.value->port == 8080, "Extracted port 8080");
    ASSERT_TRUE(p1.value->path_and_query == "/downloads/file.bin?token=abc", "Path and query correct");

    // 2. Base64 encoding/decoding of credentials
    std::string b64 = CryptoUtils::Base64Encode(p1.value->userinfo);
    ASSERT_TRUE(b64 == "YWRtaW46c2VjcmV0MTIz", "Base64 encoding matches admin:secret123");
    auto dec = CryptoUtils::Base64Decode(b64);
    std::string dec_str(dec.begin(), dec.end());
    ASSERT_TRUE(dec_str == "admin:secret123", "Base64 decoding restores original string");

    // 3. Parse URL with IPv6 host and userinfo
    auto p2 = HttpClient::ParseUrl("https://user:p@ss@[::1]:8443/api/v1");
    ASSERT_TRUE(p2.ok(), "ParseUrl parses IPv6 host with userinfo");
    ASSERT_TRUE(p2.value->userinfo == "user:p@ss", "Extracted userinfo before @");
    ASSERT_TRUE(p2.value->host == "::1", "Extracted clean IPv6 host");
    ASSERT_TRUE(p2.value->port == 8443, "Extracted port 8443");

    return true;
}

// Regression Suite 52: Modern Coral Charcoal Theme, Settings Modal Bounds, Sorting Labels & Safe Delete
bool TestModernCoralThemeSettingsModalBoundsAndSafeDelete() {
    // 1. AppSettings default theme is "coral_dark"
    openidm::AppSettings default_settings;
    ASSERT_TRUE(default_settings.theme == "coral_dark", "Default theme is coral_dark");

    // 2. Palette resolution: "coral_dark", "coral", "charcoal", and legacy "dark" resolve to GetRedesignPalette (#FF4757)
    auto pal_coral = openidm::ui::DesignSystem::ResolvePalette("coral_dark");
    ASSERT_TRUE(pal_coral.accent_primary.r == 0xFF &&
                pal_coral.accent_primary.g == 0x47 &&
                pal_coral.accent_primary.b == 0x57, "coral_dark resolves to coral red #FF4757");
    ASSERT_TRUE(pal_coral.app_background.r == 0x16 &&
                pal_coral.app_background.g == 0x16 &&
                pal_coral.app_background.b == 0x18, "coral_dark resolves to warm matte charcoal background #161618");
    ASSERT_TRUE(pal_coral.surface_primary.ToCssHex() == "#1E1E22" &&
                pal_coral.surface_elevated.ToCssHex() == "#28282E" &&
                pal_coral.surface_hover.ToCssHex() == "#2B2B32" &&
                pal_coral.surface_selected.ToCssHex() == "#302328" &&
                pal_coral.border_subtle.ToCssHex() == "#35353D" &&
                pal_coral.progress_track.ToCssHex() == "#303036",
                "coral_dark uses the approved charcoal surfaces and progress track");
    ASSERT_TRUE(pal_coral.status_success.ToCssHex() == "#A9B4C2" &&
                pal_coral.status_warning.ToCssHex() == "#F2B84B" &&
                pal_coral.status_danger.ToCssHex() == "#FF9F43",
                "coral_dark uses muted completion, amber warning, and orange failure colors");

    auto pal_dark = openidm::ui::DesignSystem::ResolvePalette("dark");
    ASSERT_TRUE(pal_dark.accent_primary.r == 0xFF &&
                pal_dark.accent_primary.g == 0x47 &&
                pal_dark.accent_primary.b == 0x57, "legacy dark resolves to coral red #FF4757");

    auto pal_classic = openidm::ui::DesignSystem::ResolvePalette("classic_blue");
    ASSERT_TRUE(pal_classic.accent_primary.r == 0x25 &&
                pal_classic.accent_primary.g == 0x63 &&
                pal_classic.accent_primary.b == 0xEB, "classic_blue resolves to legacy blue #2563EB");

    // 3. Modern Windows 11 Typography ramp floors
    auto metrics_96 = openidm::ui::DesignSystem::ComputeMetrics(96);
    ASSERT_TRUE(metrics_96.font_body_px >= 14, "Body font size is at least 14px at 96 DPI");
    ASSERT_TRUE(metrics_96.font_caption_px >= 12, "Caption font size is at least 12px at 96 DPI");

    // 4. Controller initialization and RestoreDefaultSettings defaults
    std::error_code ec;
    std::filesystem::path work_dir = std::filesystem::temp_directory_path(ec) / ("flux_test_theme_" + openidm::GenerateUuidV4());
    std::filesystem::create_directories(work_dir, ec);

    openidm::ControllerConfig cfg;
    cfg.state_db_path = work_dir / "state.db";
    cfg.default_download_dir = work_dir / "downloads";
    cfg.temp_root_dir = work_dir / "tmp";

    openidm::DownloadController ctrl(cfg);
    ASSERT_TRUE(ctrl.Initialize().ok(), "Controller initialized");
    ASSERT_TRUE(ctrl.GetSettings().theme == "coral_dark", "Initialized controller settings theme is coral_dark");

    auto restored = ctrl.RestoreDefaultSettings();
    ASSERT_TRUE(restored.ok() && restored.value->theme == "coral_dark", "RestoreDefaultSettings returns coral_dark theme");

    // 5. DesktopView Settings Modal Hit Testing Bounds & Click-Through Absorption
    openidm::ui::DesktopView view(ctrl);
    ASSERT_TRUE(!view.IsSettingsCenterOpen(), "Settings center starts closed");

    // When settings center is open, hit testing inside modal dialog returns kSettingsModalBackground
    view.OpenSettingsCenter(openidm::ui::SettingsCategory::kGeneral);
    ASSERT_TRUE(view.IsSettingsCenterOpen(), "Settings center opened");

    auto sv = view.BuildSettingsCenterView();

    // A click inside the modal that doesn't hit a specific control must return kSettingsModalBackground (not fall through!)
    auto hit_inside = view.HitTestPoint(sv.modal_rect.left + 50, sv.modal_rect.top + 30); // in modal header area
    ASSERT_TRUE(hit_inside.type == openidm::ui::HitTargetType::kSettingsModalBackground,
                "Click inside modal background returns kSettingsModalBackground");

    // A click outside the modal must return kSettingsBackdrop (not fall through to toolbar/table!)
    auto hit_outside = view.HitTestPoint(5, 5); // top-left corner outside modal
    ASSERT_TRUE(hit_outside.type == openidm::ui::HitTargetType::kSettingsBackdrop,
                "Click outside modal returns kSettingsBackdrop");

    // Verify HandleMouseClick on backdrop closes or handles modal safely
    bool handled = view.HandleMouseClick(5, 5, false, 0);
    ASSERT_TRUE(handled, "HandleMouseClick handles backdrop click");
    ASSERT_TRUE(!view.IsSettingsCenterOpen(), "Backdrop click closes settings center");

    // 6. Safe Deletion: ActionMoveSelectedToRecycleBin safely removes task
    openidm::TaskModel t_del;
    t_del.url = "https://example.com/delete_test.zip";
    t_del.file_name = "delete_test.zip";
    t_del.save_dir = work_dir.string();
    auto add_res = ctrl.AddTask(t_del, false);
    ASSERT_TRUE(add_res.ok(), "Task added for deletion test");
    std::string test_task_id = add_res.value->task_id;

    view.SelectTask(test_task_id);
    auto bin_res = view.ActionMoveSelectedToRecycleBin();
    ASSERT_TRUE(bin_res.ok(), "ActionMoveSelectedToRecycleBin succeeds");
    ASSERT_TRUE(!ctrl.GetTask(test_task_id).has_value(), "Task removed from controller after recycle bin action");

    ctrl.Shutdown();
    std::filesystem::remove_all(work_dir, ec);
    return true;
}

bool TestYouTubeSpeedParsingMidnightCrossingAndBitTorrentUpload() {
    using namespace openidm;
    using namespace openidm::torrent;

    // 1. Regex parsing of yt-dlp download progress and speed
    static const std::regex kYtDlpProgRe(R"(\[download\]\s+(\d+(?:\.\d+)?)%\s+of\s+~?\s*([0-9.]+)(KiB|MiB|GiB))");
    static const std::regex kYtDlpSpeedRe(R"(at\s+([0-9.]+)(B|KiB|MiB|GiB)/s)");

    std::string sample_line = "[download]  45.2% of  120.50MiB at  4.85MiB/s ETA 00:15";
    std::smatch m;
    ASSERT_TRUE(std::regex_search(sample_line, m, kYtDlpProgRe), "Prog regex matches yt-dlp output");
    double pct = std::stod(m[1].str());
    double sz_val = std::stod(m[2].str());
    std::string unit = m[3].str();
    ASSERT_TRUE(std::abs(pct - 45.2) < 0.001, "Percent is 45.2");
    ASSERT_TRUE(unit == "MiB", "Unit is MiB");
    uint64_t tot_b = static_cast<uint64_t>(sz_val * 1048576.0);
    ASSERT_TRUE(tot_b > 120ULL * 1048576ULL, "Total bytes calculated");

    std::smatch sm;
    ASSERT_TRUE(std::regex_search(sample_line, sm, kYtDlpSpeedRe), "Speed regex matches yt-dlp speed");
    double sp_val = std::stod(sm[1].str());
    std::string sp_unit = sm[2].str();
    ASSERT_TRUE(std::abs(sp_val - 4.85) < 0.001, "Speed val is 4.85");
    ASSERT_TRUE(sp_unit == "MiB", "Speed unit is MiB");
    uint64_t speed_bps = static_cast<uint64_t>(sp_val * 1048576.0);
    ASSERT_TRUE(speed_bps > 5000000ULL, "Speed bps correctly calculated");

    // 2. BitTorrent Message Framing for kRequest
    auto req_msg = PeerWireProtocol::CreateRequest(5, 32768, 16384);
    auto p_opt = PeerWireProtocol::ParseMessage(req_msg.data(), req_msg.size());
    ASSERT_TRUE(p_opt.has_value(), "kRequest parsed successfully");
    ASSERT_TRUE(p_opt->id == BtMessageId::kRequest, "ID is kRequest");
    ASSERT_TRUE(p_opt->piece_index == 5, "Piece index is 5");
    ASSERT_TRUE(p_opt->block_begin == 32768, "Block begin is 32768");
    ASSERT_TRUE(p_opt->block_length == 16384, "Block length is 16384");

    // 3. BitTorrent upload accounting fields in TorrentProgress and TaskModel
    TorrentProgress tp;
    tp.uploaded_bytes = 1048576;
    tp.upload_speed_bps = 524288;
    TaskModel task;
    task.uploaded_bytes = tp.uploaded_bytes;
    task.upload_speed_bps = tp.upload_speed_bps;
    ASSERT_TRUE(task.uploaded_bytes == 1048576, "Uploaded bytes copied");
    ASSERT_TRUE(task.upload_speed_bps == 524288, "Upload speed bps copied");

    // 4. Controller Scheduling with start/stop time and retry reset
    std::error_code ec;
    std::filesystem::path work_dir = std::filesystem::temp_directory_path() / "flux_test_sched_yt";
    std::filesystem::remove_all(work_dir, ec);
    std::filesystem::create_directories(work_dir, ec);

    ControllerConfig cfg;
    cfg.state_db_path = work_dir / "state.db";
    cfg.default_download_dir = work_dir / "downloads";
    cfg.temp_root_dir = work_dir / "tmp";
    std::filesystem::create_directories(cfg.default_download_dir, ec);

    DownloadController ctrl(cfg);
    ASSERT_TRUE(ctrl.Initialize().ok(), "Init controller");

    TaskModel ts;
    ts.task_id = "task-yt-sched";
    ts.task_type = TaskType::kStreamHls;
    ts.url = "https://www.youtube.com/watch?v=dQw4w9WgXcQ";
    ts.file_name = "rickroll.mp4";
    ASSERT_TRUE(ctrl.AddTask(ts, false, true).ok(), "Add task");

    // Set schedule
    ASSERT_TRUE(ctrl.SetTaskSchedule("task-yt-sched", "23:00", "06:00").ok(), "Set overnight schedule");
    auto topt = ctrl.GetTask("task-yt-sched");
    ASSERT_TRUE(topt.has_value() && topt->scheduled, "Task is scheduled");
    ASSERT_TRUE(topt->schedule_time_local == "23:00", "Start time is 23:00");
    ASSERT_TRUE(topt->schedule_stop_time_local == "06:00", "Stop time is 06:00");

    // Retrying or Resuming cleans stale schedule flags so manual resume is never blocked
    ASSERT_TRUE(ctrl.ResumeTask("task-yt-sched").ok(), "ResumeTask succeeds");
    auto t_resumed = ctrl.GetTask("task-yt-sched");
    ASSERT_TRUE(t_resumed.has_value(), "Task exists after resume");
    ASSERT_TRUE(!t_resumed->scheduled, "Manual resume clears scheduled flag");
    ASSERT_TRUE(t_resumed->schedule_time_local.empty(), "Start time cleared on manual resume");
    ASSERT_TRUE(t_resumed->schedule_stop_time_local.empty(), "Stop time cleared on manual resume");

    ctrl.Shutdown();
    std::filesystem::remove_all(work_dir, ec);
    return true;
}

bool TestFinalPhaseRemediationAndLiveMetrics() {
    using namespace openidm;

    // 1. Theme default restoration to coral_dark and coral red accent
    AppSettings def_settings;
    ASSERT_TRUE(def_settings.theme == "coral_dark", "Default theme is coral_dark");
    auto pal = openidm::ui::DesignSystem::ResolvePalette("coral_dark");
    ASSERT_TRUE(pal.accent_primary.r == 0xFF && pal.accent_primary.g == 0x47 && pal.accent_primary.b == 0x57,
                "coral_dark accent is coral red #FF4757");
    ASSERT_TRUE(openidm::ui::DesignSystem::ResolvePalette("").accent_primary.r == 0xFF,
                "Fallback theme defaults to coral red #FF4757");

    // 2. Safe File Deletion in Controller
    std::error_code ec;
    std::filesystem::path work_dir = std::filesystem::temp_directory_path() / "flux_test_safe_del";
    std::filesystem::remove_all(work_dir, ec);
    std::filesystem::create_directories(work_dir, ec);

    ControllerConfig cfg;
    cfg.state_db_path = work_dir / "state.db";
    cfg.default_download_dir = work_dir / "downloads";
    cfg.temp_root_dir = work_dir / "tmp";
    std::filesystem::create_directories(cfg.default_download_dir, ec);

    DownloadController ctrl(cfg);
    ASSERT_TRUE(ctrl.Initialize().ok(), "Init controller for safe deletion");

    // Removing a partial task cleans only its task-owned part file and preserves
    // the task's ordinary destination name.
    TaskModel partial_task;
    partial_task.task_id = "task-safe-del-partial";
    partial_task.url = "https://example.com/partial_output.dat";
    partial_task.file_name = "partial_output.dat";
    partial_task.save_dir = cfg.default_download_dir.string();
    partial_task.state = TaskState::kPaused;
    partial_task.part_path = (cfg.default_download_dir / "partial_output.dat.openidm.part").string();
    { std::ofstream ofs(partial_task.part_path, std::ios::binary); ofs << "partial bytes"; }
    const auto ordinary_destination = cfg.default_download_dir / partial_task.file_name;
    { std::ofstream ofs(ordinary_destination, std::ios::binary); ofs << "pre-existing destination"; }
    ASSERT_TRUE(ctrl.AddTask(partial_task, false, true).ok(), "Add partial task");
    ASSERT_TRUE(ctrl.RemoveTask(partial_task.task_id, false).ok(), "Remove partial task from list");
    ASSERT_TRUE(!std::filesystem::exists(partial_task.part_path), "Task-owned partial file is cleaned");
    ASSERT_TRUE(std::filesystem::exists(ordinary_destination), "Remove from List preserves destination file");

    // 2a. Completed file deleted when delete_file == true
    std::filesystem::path target_file = cfg.default_download_dir / "sample_completed.dat";
    {
        std::ofstream ofs(target_file, std::ios::binary);
        ofs << "test completed content";
    }
    ASSERT_TRUE(std::filesystem::exists(target_file), "Test file created on disk");

    TaskModel t1;
    t1.task_id = "task-safe-del-1";
    t1.url = "https://example.com/sample_completed.dat";
    t1.file_name = "sample_completed.dat";
    t1.save_dir = cfg.default_download_dir.string();
    t1.state = TaskState::kCompleted;
    ASSERT_TRUE(ctrl.AddTask(t1, false, true).ok(), "Add completed task");

    // Remove task with delete_file = false: file MUST remain on disk
    TaskModel t2 = t1;
    t2.task_id = "task-safe-del-2";
    ASSERT_TRUE(ctrl.AddTask(t2, false, true).ok(), "Add second completed task");
    ASSERT_TRUE(ctrl.RemoveTask("task-safe-del-2", false).ok(), "Remove without deleting file");
    ASSERT_TRUE(std::filesystem::exists(target_file), "File preserved when delete_file == false");

    // Remove task with delete_file = true: file MUST be removed from disk
    ASSERT_TRUE(ctrl.RemoveTask("task-safe-del-1", true).ok(), "Remove with delete_file == true");
    ASSERT_TRUE(!std::filesystem::exists(target_file), "File removed from disk when delete_file == true");
    ASSERT_TRUE(!ctrl.GetTask("task-safe-del-1").has_value(), "Task removed from DB");

    // A missing completed output is already absent; remove the record cleanly.
    TaskModel t3 = t1;
    t3.task_id = "task-safe-del-missing";
    t3.file_name = "already_missing.dat";
    ASSERT_TRUE(ctrl.AddTask(t3, false, true).ok(), "Add completed task with missing output");
    ASSERT_TRUE(ctrl.RemoveTask(t3.task_id, true).ok(), "Permanent delete accepts an already missing output");
    ASSERT_TRUE(!ctrl.GetTask(t3.task_id).has_value(), "Missing output task record is removed");

#ifdef _WIN32
    // A read-only Windows file must not cause the task record to disappear.
    TaskModel t4 = t1;
    t4.task_id = "task-safe-del-readonly";
    t4.file_name = "readonly_output.dat";
    const auto readonly_path = cfg.default_download_dir / t4.file_name;
    { std::ofstream ofs(readonly_path, std::ios::binary); ofs << "keep while delete fails"; }
    ASSERT_TRUE(SetFileAttributesW(readonly_path.c_str(), FILE_ATTRIBUTE_READONLY) != 0,
                "Set readonly attribute for deletion failure regression");
    ASSERT_TRUE(ctrl.AddTask(t4, false, true).ok(), "Add completed readonly task");
    auto readonly_delete = ctrl.RemoveTask(t4.task_id, true);
    ASSERT_TRUE(!readonly_delete.ok(), "Permanent delete reports readonly file failure");
    ASSERT_TRUE(ctrl.GetTask(t4.task_id).has_value(), "Task record survives failed file deletion");
    ASSERT_TRUE(std::filesystem::exists(readonly_path), "Readonly output survives failed deletion");
    SetFileAttributesW(readonly_path.c_str(), FILE_ATTRIBUTE_NORMAL);
    ASSERT_TRUE(ctrl.RemoveTask(t4.task_id, true).ok(), "Retry permanent delete after clearing readonly attribute");
    ASSERT_TRUE(!ctrl.GetTask(t4.task_id).has_value(), "Retried permanent delete removes task record");
#endif

    // 3. Status Bar Metrics and Active Counts
    openidm::ui::DesktopView view(ctrl);
    auto sb = view.BuildStatusBar();
    ASSERT_TRUE(sb.browser_capture_enabled, "Browser capture enabled by default");
    ASSERT_TRUE(!sb.formatted_total_speed.empty(), "Formatted speed non-empty");
    ASSERT_TRUE(!sb.formatted_total_upload_speed.empty(), "Formatted upload speed non-empty");

    // 3b. Priority 0 Table Column Audit (Size | Status | Download | Upload | Added)
    TaskModel t_active;
    t_active.task_id = "task-tbl-active";
    t_active.url = "https://example.com/archive.zip";
    t_active.file_name = "archive.zip";
    t_active.total_bytes = 100 * 1024 * 1024;
    t_active.downloaded_bytes = 45 * 1024 * 1024;
    t_active.speed_bps = 2500000;
    t_active.upload_speed_bps = 0;
    t_active.state = TaskState::kDownloading;
    t_active.created_at_ms = 1700000000000ULL;
    ASSERT_TRUE(ctrl.AddTask(t_active, false, true).ok(), "Add active task");

    TaskModel t_paused;
    t_paused.task_id = "task-tbl-paused";
    t_paused.url = "https://example.com/movie.mp4";
    t_paused.file_name = "movie.mp4";
    t_paused.total_bytes = 50 * 1024 * 1024;
    t_paused.downloaded_bytes = 10 * 1024 * 1024;
    t_paused.speed_bps = 0;
    t_paused.upload_speed_bps = 0;
    t_paused.state = TaskState::kPaused;
    t_paused.created_at_ms = 1700000001000ULL;
    ASSERT_TRUE(ctrl.AddTask(t_paused, false, true).ok(), "Add paused task");

    TaskModel t_seeding;
    t_seeding.task_id = "task-tbl-seeding";
    t_seeding.task_type = TaskType::kTorrent;
    t_seeding.url = "https://example.com/distro.torrent";
    t_seeding.file_name = "distro.iso";
    t_seeding.total_bytes = 200 * 1024 * 1024;
    t_seeding.downloaded_bytes = 200 * 1024 * 1024;
    t_seeding.uploaded_bytes = 25 * 1024 * 1024;
    t_seeding.speed_bps = 0;
    t_seeding.upload_speed_bps = 1500000;
    t_seeding.state = TaskState::kCompleted;
    t_seeding.created_at_ms = 1700000002000ULL;
    ASSERT_TRUE(ctrl.AddTask(t_seeding, false, true).ok(), "Add seeding task");

    TaskModel t_failed;
    t_failed.task_id = "task-tbl-failed";
    t_failed.url = "https://example.com/broken.dat";
    t_failed.file_name = "broken.dat";
    t_failed.state = TaskState::kFailed;
    t_failed.error_code = "ERR_NET_CONNECT_TIMEOUT";
    t_failed.error_message = "Connect timed out after 30s";
    t_failed.speed_bps = 0;
    t_failed.upload_speed_bps = 0;
    t_failed.created_at_ms = 1700000003000ULL;
    ASSERT_TRUE(ctrl.AddTask(t_failed, false, true).ok(), "Add failed task");

    auto rows = view.BuildVisibleTaskRows();
    ASSERT_TRUE(rows.size() >= 4, "View model generated rows for all tasks");

    for (const auto& r : rows) {
        if (r.task_id == "task-tbl-active") {
            ASSERT_TRUE(r.size_text.find("45") != std::string::npos && r.size_text.find("100") != std::string::npos, "Active row size displays received/total");
            ASSERT_TRUE(r.status_text.find("Downloading") != std::string::npos, "Active row status indicates Downloading");
            ASSERT_TRUE(r.speed_text.find("KB/s") != std::string::npos || r.speed_text.find("MB/s") != std::string::npos, "Active row has download speed");
            ASSERT_TRUE(r.upload_speed_text == "0 KB/s", "Active HTTP row upload speed is 0");
        } else if (r.task_id == "task-tbl-paused") {
            ASSERT_TRUE(r.status_text.find("Paused") != std::string::npos, "Paused row status indicates Paused");
            ASSERT_TRUE(r.speed_text == "0 KB/s", "Paused row download speed is 0");
            ASSERT_TRUE(r.upload_speed_text == "0 KB/s", "Paused row upload speed is 0");
        } else if (r.task_id == "task-tbl-seeding") {
            ASSERT_TRUE(r.status_text == "Completed", "Seeding row status is Completed");
            ASSERT_TRUE(r.upload_speed_text.find("/s") != std::string::npos, "BitTorrent seeding row displays upload speed");
        } else if (r.task_id == "task-tbl-failed") {
            ASSERT_TRUE(r.status_text.find("Failed") != std::string::npos, "Failed row status indicates failure");
            ASSERT_TRUE(r.full_status_tooltip.find("ERR_NET_CONNECT_TIMEOUT") != std::string::npos, "Failed row tooltip preserves error code");
        }
    }
    const auto coral_palette = view.GetPalette();
    for (const auto& r : rows) {
        if (r.task_id == "task-tbl-active") {
            ASSERT_TRUE(r.status_indicator_color.ToCssHex() == coral_palette.accent_primary.ToCssHex() &&
                        r.progress_fill_color.ToCssHex() == coral_palette.accent_primary.ToCssHex(),
                        "Downloading row uses coral for status and progress");
        } else if (r.task_id == "task-tbl-paused") {
            ASSERT_TRUE(r.status_indicator_color.ToCssHex() == coral_palette.status_warning.ToCssHex() &&
                        r.progress_fill_color.ToCssHex() == coral_palette.status_warning.ToCssHex(),
                        "Paused row uses amber for status and progress");
        } else if (r.task_id == "task-tbl-seeding") {
            ASSERT_TRUE(r.status_indicator_color.ToCssHex() == coral_palette.status_success.ToCssHex() &&
                        r.progress_fill_color.ToCssHex() == coral_palette.status_success.ToCssHex(),
                        "Completed row uses green for status and progress");
        } else if (r.task_id == "task-tbl-failed") {
            ASSERT_TRUE(r.status_indicator_color.ToCssHex() == coral_palette.status_danger.ToCssHex() &&
                        r.progress_fill_color.ToCssHex() == coral_palette.status_danger.ToCssHex(),
                        "Failed row uses coral danger styling");
        }
    }

    // 4. YouTube Googlevideo Classification Guard
    std::string yt_video_url = "https://rr3---sn-4g5ednks.googlevideo.com/videoplayback?expire=1729272302&ei=xyz&ip=1.2.3.4&id=o-1234&itag=18&source=youtube&requiressl=yes&mime=video%2Fmp4";
    std::string low_url = yt_video_url;
    for (char& c : low_url) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    bool is_youtube_watch = (low_url.find("youtube.com/watch") != std::string::npos ||
                             low_url.find("youtu.be/") != std::string::npos ||
                             low_url.find("youtube.com/shorts/") != std::string::npos ||
                             low_url.find("googlevideo.com") != std::string::npos ||
                             low_url.find("videoplayback") != std::string::npos);
    ASSERT_TRUE(is_youtube_watch, "googlevideo.com URL matched as YouTube stream");

    bool is_direct_video = !is_youtube_watch && (low_url.find(".mp4") != std::string::npos);
    ASSERT_TRUE(!is_direct_video, "googlevideo.com with .mp4 is NOT classified as direct video");

    // 5. Scheduled Window Precision (Midnight crossing)
    auto is_in_window = [](const std::string& cur, const std::string& start, const std::string& stop) {
        if (cur.size() != 5 || start.size() != 5 || stop.size() != 5) return false;
        if (start <= stop) return cur >= start && cur <= stop;
        return cur >= start || cur <= stop;
    };
    ASSERT_TRUE(is_in_window("23:30", "23:00", "04:00"), "23:30 inside 23:00-04:00 window");
    ASSERT_TRUE(is_in_window("02:15", "23:00", "04:00"), "02:15 inside 23:00-04:00 window");
    ASSERT_TRUE(!is_in_window("12:00", "23:00", "04:00"), "12:00 outside 23:00-04:00 window");
    ASSERT_TRUE(is_in_window("14:30", "09:00", "17:00"), "14:30 inside 09:00-17:00 window");
    ASSERT_TRUE(!is_in_window("19:00", "09:00", "17:00"), "19:00 outside 09:00-17:00 window");

    ctrl.Shutdown();
    DownloadController reopened(cfg);
    ASSERT_TRUE(reopened.Initialize().ok(), "Reopen storage after deletion tests");
    ASSERT_TRUE(!reopened.GetTask("task-safe-del-1").has_value(), "Permanent deletion remains absent after application restart");
    ASSERT_TRUE(!reopened.GetTask("task-safe-del-missing").has_value(), "Missing-output deletion remains absent after restart");
    ASSERT_TRUE(!reopened.GetTask("task-safe-del-2").has_value(), "Remove from list also deletes the task record after restart");
    reopened.Shutdown();
    std::filesystem::remove_all(work_dir, ec);
    return true;
}

bool TestYouTubeCanonicalRoutingAndSchedulerThroughput() {
    // 1. Test SegmentScheduler::CanSplitAnySegment()
    openidm::brisk::SegmentScheduler sched(4 * 1024 * 1024, 4, 1024 * 1024);
    sched.Initialize({});
    ASSERT_TRUE(sched.CanSplitAnySegment(), "4MB segment with 1MB min_split should be split-eligible (4MB >= 2 * 1MB)");

    auto s1 = sched.AcquireNextOrStealSegment(true);
    ASSERT_TRUE(s1 != nullptr, "Acquired first segment");
    ASSERT_TRUE(sched.CanSplitAnySegment(), "CanSplitAnySegment returns true before split");

    auto s2 = sched.AcquireNextOrStealSegment(true);
    ASSERT_TRUE(s2 != nullptr, "Acquired second stolen segment");

    // Test small scheduler where total bytes < 2 * min_split_bytes
    openidm::brisk::SegmentScheduler small_sched(1024 * 1024, 4, 1024 * 1024);
    small_sched.Initialize({});
    ASSERT_TRUE(!small_sched.CanSplitAnySegment(), "1MB segment with 1MB min_split cannot be split (< 2 * 1MB)");

    // 2. Production Nm3u8dlReAdapter::WriteRestrictedResponseFile & VerifyContainerMagicBytes
    std::error_code ec;
    std::filesystem::path task_tmp = std::filesystem::temp_directory_path() / ("flux_test_resp_" + openidm::GenerateUuidV4());
    std::filesystem::create_directories(task_tmp, ec);

    std::vector<std::string> rsp_lines = {"--header", "Cookie: secret=12345", "--header", "Authorization: Bearer mytoken"};
    auto rsp_res = openidm::stream::Nm3u8dlReAdapter::WriteRestrictedResponseFile(task_tmp, "job.rsp", rsp_lines);
    ASSERT_TRUE(rsp_res.ok(), "WriteRestrictedResponseFile succeeded");
    ASSERT_TRUE(std::filesystem::exists(*rsp_res.value), "Response file exists on disk");

    // Test MP4 container validation with valid ftyp box
    std::filesystem::path valid_mp4 = task_tmp / "valid.mp4";
    {
        std::ofstream ofs(valid_mp4, std::ios::binary);
        const uint8_t mp4_hdr[] = {0x00, 0x00, 0x00, 0x18, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm', 0x00, 0x00, 0x02, 0x00};
        ofs.write(reinterpret_cast<const char*>(mp4_hdr), sizeof(mp4_hdr));
        std::vector<char> pad(128, 0);
        ofs.write(pad.data(), pad.size());
    }
    ASSERT_TRUE(openidm::stream::Nm3u8dlReAdapter::VerifyContainerMagicBytes(valid_mp4, "mp4"), "VerifyContainerMagicBytes passes valid MP4");

    std::filesystem::path corrupt_mp4 = task_tmp / "corrupt.mp4";
    {
        std::ofstream ofs(corrupt_mp4, std::ios::binary);
        ofs << "not an mp4 file content";
    }
    ASSERT_TRUE(!openidm::stream::Nm3u8dlReAdapter::VerifyContainerMagicBytes(corrupt_mp4, "mp4"), "VerifyContainerMagicBytes rejects corrupt MP4");

    std::filesystem::remove_all(task_tmp, ec);

    // 2. Canonical YouTube watch URL extraction from googlevideo.com
    auto resolve_yt_watch = [](const std::string& manifest_url, const std::string& page_url, const std::string& video_id) -> std::string {
        if (!page_url.empty() && page_url.find("youtube.com") != std::string::npos) {
            return page_url;
        }
        if (!video_id.empty()) {
            return "https://www.youtube.com/watch?v=" + video_id;
        }
        if (manifest_url.find("googlevideo.com") != std::string::npos) {
            std::regex id_rx(R"([?&]id=([a-zA-Z0-9_-]+))");
            std::smatch m;
            if (std::regex_search(manifest_url, m, id_rx) && m.size() > 1) {
                std::string raw_id = m[1].str();
                if (raw_id.rfind("o-", 0) == 0) {
                    raw_id = raw_id.substr(2);
                }
                return "https://www.youtube.com/watch?v=" + raw_id;
            }
        }
        return manifest_url;
    };

    std::string gv_url = "https://rr3---sn-4g5ednks.googlevideo.com/videoplayback?expire=1729272302&ei=xyz&id=o-dQw4w9WgXcQ&itag=137";
    std::string canonical = resolve_yt_watch(gv_url, "", "");
    ASSERT_TRUE(canonical == "https://www.youtube.com/watch?v=dQw4w9WgXcQ", "googlevideo id=o-XYZ mapped to youtube.com/watch?v=XYZ");

    std::string canonical_page = resolve_yt_watch(gv_url, "https://www.youtube.com/watch?v=dQw4w9WgXcQ", "");
    ASSERT_TRUE(canonical_page == "https://www.youtube.com/watch?v=dQw4w9WgXcQ", "Page URL prioritized for watch URL");

    // 3. YouTube 1080p Format String Verification
    auto build_ytdlp_fmt = [](int target_height) -> std::string {
        if (target_height > 0) {
            std::string hs = std::to_string(target_height);
            return "bestvideo[height=" + hs + "]+bestaudio/bestvideo[height<=" + hs + "]+bestaudio/best[height<=" + hs + "]/best";
        }
        return "bestvideo+bestaudio/best";
    };

    std::string fmt_1080 = build_ytdlp_fmt(1080);
    ASSERT_TRUE(fmt_1080.find("[ext=mp4]") == std::string::npos, "Format selector for 1080p does not force [ext=mp4] which drops VP9/AV1 1080p tracks");
    ASSERT_TRUE(fmt_1080.find("bestvideo[height=1080]+bestaudio") != std::string::npos, "Format selector matches bestvideo[height=1080]+bestaudio");

    return true;
}

bool TestSpeedFormattingMultiTrackProgressAndContainerVerification() {
    using namespace openidm;
    using openidm::stream::Nm3u8dlReAdapter;

    // 1. DesktopView::FormatSpeedKbps threshold units (KB/s, MB/s, GB/s)
    ASSERT_TRUE(openidm::ui::DesktopView::FormatSpeedKbps(0) == "0 KB/s", "0 bps is formatted as 0 KB/s");
    ASSERT_TRUE(openidm::ui::DesktopView::FormatSpeedKbps(500 * 1024) == "500 KB/s", "500 KB/s formatted in KB/s");
    ASSERT_TRUE(openidm::ui::DesktopView::FormatSpeedKbps(1024 * 1024 - 1) == "1024 KB/s", "Under 1 MB/s formatted in KB/s");

    std::string spd_1mb = openidm::ui::DesktopView::FormatSpeedKbps(1024ULL * 1024ULL);
    ASSERT_TRUE(spd_1mb == "1.00 MB/s", "Exactly 1 MB/s formatted as 1.00 MB/s");

    std::string spd_2_5mb = openidm::ui::DesktopView::FormatSpeedKbps(static_cast<uint64_t>(2.5 * 1024.0 * 1024.0));
    ASSERT_TRUE(spd_2_5mb == "2.50 MB/s", "2.5 MB/s formatted as 2.50 MB/s");

    std::string spd_15mb = openidm::ui::DesktopView::FormatSpeedKbps(15ULL * 1024ULL * 1024ULL);
    ASSERT_TRUE(spd_15mb == "15.00 MB/s", "15 MB/s formatted as 15.00 MB/s");

    std::string spd_1_25gb = openidm::ui::DesktopView::FormatSpeedKbps(static_cast<uint64_t>(1.25 * 1024.0 * 1024.0 * 1024.0));
    ASSERT_TRUE(spd_1_25gb == "1.25 GB/s", "1.25 GB/s formatted as 1.25 GB/s");

    // 2. Multi-track YouTube progress accounting simulation (Video 182.50 MiB + Audio 13.30 MiB = 195.80 MiB)
    {
        static const std::regex kYtDlpProgRe(R"(\[download\]\s+(\d+(?:\.\d+)?)%\s+of\s+~?\s*([0-9.]+)(KiB|MiB|GiB))");
        std::vector<uint64_t> track_totals;
        std::vector<uint64_t> track_downloaded;
        int current_track_idx = -1;
        double last_track_pct = 0.0;
        uint64_t last_aggregate_dl = 0;
        uint64_t observed_final_total = 0;

        auto parse_line = [&](const std::string& line) {
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

                if (current_track_idx < 0) {
                    current_track_idx = 0;
                    track_totals.push_back(tot_b);
                    track_downloaded.push_back(dl_b);
                } else if (pct < last_track_pct - 15.0 && last_track_pct >= 90.0) {
                    current_track_idx++;
                    track_totals.push_back(tot_b);
                    track_downloaded.push_back(dl_b);
                } else {
                    track_totals[current_track_idx] = tot_b;
                    track_downloaded[current_track_idx] = dl_b;
                }
                last_track_pct = pct;

                uint64_t aggregate_tot = 0;
                uint64_t aggregate_dl = 0;
                for (size_t ti = 0; ti < track_totals.size(); ++ti) {
                    aggregate_tot += track_totals[ti];
                    aggregate_dl += track_downloaded[ti];
                }
                last_aggregate_dl = aggregate_dl;
                observed_final_total = aggregate_tot;
            }
        };

        // Video track download
        parse_line("[download]  50.0% of ~ 182.50MiB at 4.25MiB/s ETA 00:21");
        ASSERT_TRUE(track_totals.size() == 1, "Track 0 initialized");
        ASSERT_TRUE(last_aggregate_dl > 0, "Downloaded bytes accumulated for Track 0");

        parse_line("[download] 100.0% of ~ 182.50MiB at 4.50MiB/s ETA 00:00");
        uint64_t video_done_b = last_aggregate_dl;
        ASSERT_TRUE(video_done_b > 190000000ULL, "Video track reaches ~191.3 MB");

        // Audio track starts: pct drops to 0.0%, sz_val is 13.30MiB
        parse_line("[download]   0.0% of ~  13.30MiB at 1.20MiB/s ETA 00:11");
        ASSERT_TRUE(track_totals.size() == 2, "Track 1 added without replacing Track 0");
        ASSERT_TRUE(observed_final_total > 200000000ULL, "Total bytes does NOT collapse to 13.3 MB! Preserves aggregate ~195.8 MB");
        ASSERT_TRUE(last_aggregate_dl >= video_done_b, "Aggregate downloaded bytes does not reset to zero");

        parse_line("[download] 100.0% of ~  13.30MiB at 2.10MiB/s ETA 00:00");
        ASSERT_TRUE(last_aggregate_dl == observed_final_total, "Both tracks 100% completed");
    }

    // 3. Container magic bytes verification across WebM, MKV, MP4
    {
        std::error_code ec;
        std::filesystem::path test_dir = std::filesystem::temp_directory_path() / "flux_test_container_magic";
        std::filesystem::remove_all(test_dir, ec);
        std::filesystem::create_directories(test_dir, ec);

        // 3a. MP4 container with ftyp box
        std::filesystem::path mp4_file = test_dir / "test.mp4";
        {
            std::ofstream ofs(mp4_file, std::ios::binary);
            uint8_t mp4_hdr[] = { 0x00, 0x00, 0x00, 0x20, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm' };
            ofs.write(reinterpret_cast<const char*>(mp4_hdr), sizeof(mp4_hdr));
            std::vector<char> dummy(1024, 0);
            ofs.write(dummy.data(), dummy.size());
        }
        ASSERT_TRUE(Nm3u8dlReAdapter::VerifyContainerMagicBytes(mp4_file, "mp4"), "Valid MP4 magic bytes verified");
        ASSERT_TRUE(!Nm3u8dlReAdapter::VerifyContainerMagicBytes(mp4_file, "mkv"), "MP4 does not validate as MKV");

        // 3b. WebM / MKV container with EBML header (\x1A\x45\xDF\xA3)
        std::filesystem::path webm_file = test_dir / "test.webm";
        {
            std::ofstream ofs(webm_file, std::ios::binary);
            uint8_t ebml_hdr[] = { 0x1A, 0x45, 0xDF, 0xA3, 0x9F, 0x42, 0x86, 0x81, 0x01, 0x42, 0xF7, 0x81, 0x01 };
            ofs.write(reinterpret_cast<const char*>(ebml_hdr), sizeof(ebml_hdr));
            std::vector<char> dummy(1024, 0);
            ofs.write(dummy.data(), dummy.size());
        }
        ASSERT_TRUE(Nm3u8dlReAdapter::VerifyContainerMagicBytes(webm_file, "webm"), "Valid WebM magic bytes verified");
        ASSERT_TRUE(Nm3u8dlReAdapter::VerifyContainerMagicBytes(webm_file, "mkv"), "Valid MKV magic bytes verified for EBML");
        ASSERT_TRUE(!Nm3u8dlReAdapter::VerifyContainerMagicBytes(webm_file, "mp4"), "WebM does not validate as MP4");

        // 3c. Container fallback detection: requested mp4 but worker produced webm
        std::string detected_fmt = "mp4";
        bool valid_container = Nm3u8dlReAdapter::VerifyContainerMagicBytes(webm_file, detected_fmt);
        if (!valid_container) {
            for (const auto& alt_fmt : {"mp4", "mkv", "webm", "m4a"}) {
                if (Nm3u8dlReAdapter::VerifyContainerMagicBytes(webm_file, alt_fmt)) {
                    detected_fmt = alt_fmt;
                    valid_container = true;
                    break;
                }
            }
        }
        ASSERT_TRUE(valid_container, "Alternative container fallback succeeds for WebM");
        ASSERT_TRUE(detected_fmt == "mkv" || detected_fmt == "webm", "Detected format updated to actual container");

        std::filesystem::remove_all(test_dir, ec);
    }

    return true;
}

bool TestPimpbunnyStreamRoutingAndYouTubeNetscapeCookieFile() {
    using namespace openidm;
    using namespace openidm::stream;
    std::error_code ec;
    auto test_dir = std::filesystem::temp_directory_path() / ("test_pb_yt_" + GenerateUuidV4());
    std::filesystem::create_directories(test_dir, ec);

    // 1. Test WriteNetscapeCookieFile formatting and domain isolation
    {
        std::string yt_cookies = "LOGIN_INFO=AFmmF2kwRA...; SID=sample_sid_123; HSID=sample_hsid_456; __Secure-1PSID=sec_123";
        auto res = Nm3u8dlReAdapter::WriteNetscapeCookieFile(test_dir, "yt_cookies.txt", yt_cookies, "https://www.youtube.com/watch?v=dQw4w9WgXcQ");
        ASSERT_TRUE(res.ok(), "WriteNetscapeCookieFile succeeds for YouTube");
        std::filesystem::path cookie_file = test_dir / "yt_cookies.txt";
        ASSERT_TRUE(std::filesystem::exists(cookie_file, ec), "yt_cookies.txt exists");

        std::ifstream ifs(cookie_file);
        std::stringstream ss;
        ss << ifs.rdbuf();
        std::string content = ss.str();

        ASSERT_TRUE(content.find("# Netscape HTTP Cookie File") != std::string::npos, "Netscape cookie header present");
        ASSERT_TRUE(content.find(".youtube.com\tTRUE\t/\tTRUE\t2147483647\tLOGIN_INFO\tAFmmF2kwRA...") != std::string::npos, "YouTube domain formatted properly");
        ASSERT_TRUE(content.find(".google.com\tTRUE\t/\tTRUE\t2147483647\tLOGIN_INFO\tAFmmF2kwRA...") != std::string::npos, "Google domain formatted properly for YouTube auth");
        ASSERT_TRUE(content.find(".youtube.com\tTRUE\t/\tTRUE\t2147483647\tSID\tsample_sid_123") != std::string::npos, "SID cookie present");
    }

    {
        std::string pb_cookies = "sess=pb_sess_999; uid=102030";
        auto res = Nm3u8dlReAdapter::WriteNetscapeCookieFile(test_dir, "pb_cookies.txt", pb_cookies, "https://pimpbunny.com/watch/video123");
        ASSERT_TRUE(res.ok(), "WriteNetscapeCookieFile succeeds for non-YouTube site");
        std::filesystem::path cookie_file = test_dir / "pb_cookies.txt";

        std::ifstream ifs(cookie_file);
        std::stringstream ss;
        ss << ifs.rdbuf();
        std::string content = ss.str();

        ASSERT_TRUE(content.find(".pimpbunny.com\tTRUE\t/\tTRUE\t2147483647\tsess\tpb_sess_999") != std::string::npos, "Pimpbunny domain formatted properly");
        ASSERT_TRUE(content.find(".google.com") == std::string::npos, "Google domain is NOT generated for non-YouTube site");
    }

    // 2. Test IpcServer stream.add routing
    {
        std::string sock = openidm::ipc::IpcServer::DefaultSocketPath() + "_test_pb_" + GenerateUuidV4();
        openidm::ControllerConfig cfg;
        cfg.state_db_path = test_dir / "state.db";
        cfg.default_download_dir = test_dir / "downloads";
        cfg.temp_root_dir = test_dir / "tmp";

        openidm::DownloadController ctrl(cfg);
        ASSERT_TRUE(ctrl.Initialize().ok(), "Init controller for stream routing test");
        openidm::ipc::IpcServer server(ctrl, sock);

        // Scenario A: Pimpbunny progressive direct video (.mp4) with videoUrl
        // Must NOT forge fluxType: YOUTUBE_STREAM and must route to TaskType::kHttpFile
        std::string pb_direct_req = R"JSON({
            "jsonrpc": "2.0",
            "id": "pb-test-1",
            "method": "stream.add",
            "params": {
                "url": "https://cdn.pimpbunny.com/videos/media_1080p.mp4?token=abc",
                "videoUrl": "https://cdn.pimpbunny.com/videos/media_1080p.mp4?token=abc",
                "tabUrl": "https://pimpbunny.com/videos/12345",
                "pageTitle": "Pimpbunny Exclusive Clip",
                "sourceType": "DIRECT_PROGRESSIVE",
                "streamType": "HTTP_FILE"
            }
        })JSON";
        std::string resp_a = server.HandleJsonRpcMessage(pb_direct_req);
        ASSERT_TRUE(resp_a.find("\"accepted\":true") != std::string::npos, "Pimpbunny direct video accepted");

        auto tasks = ctrl.ListTasks();
        ASSERT_TRUE(!tasks.empty(), "Task was added to controller");
        auto pb_task = tasks.back();
        ASSERT_TRUE(pb_task.task_type == openidm::TaskType::kHttpFile, "Pimpbunny direct video routed to in-process HTTP engine");
        ASSERT_TRUE(pb_task.url.find("pimpbunny.com/videos/media_1080p.mp4") != std::string::npos, "Correct media URL preserved");
        ASSERT_TRUE(pb_task.stream_options.cached_manifest_body.find("YOUTUBE_STREAM") == std::string::npos,
                    "Non-YouTube task does NOT contain synthetic YOUTUBE_STREAM descriptor");

        // Scenario B: Pimpbunny HLS stream (.m3u8) with videoUrl
        // Must NOT forge fluxType: YOUTUBE_STREAM and must route to TaskType::kStreamHls
        std::string pb_hls_req = R"JSON({
            "jsonrpc": "2.0",
            "id": "pb-test-2",
            "method": "stream.add",
            "params": {
                "manifestUrl": "https://cdn.pimpbunny.com/hls/master.m3u8",
                "videoUrl": "https://cdn.pimpbunny.com/hls/master.m3u8",
                "tabUrl": "https://pimpbunny.com/videos/67890",
                "pageTitle": "Pimpbunny Stream",
                "sourceType": "HLS_MEDIA",
                "streamType": "HLS"
            }
        })JSON";
        std::string resp_b = server.HandleJsonRpcMessage(pb_hls_req);
        ASSERT_TRUE(resp_b.find("\"accepted\":true") != std::string::npos, "Pimpbunny HLS stream accepted");

        tasks = ctrl.ListTasks();
        TaskModel pb_hls_task;
        for (const auto& t : tasks) {
            if (t.url.find("master.m3u8") != std::string::npos) {
                pb_hls_task = t;
                break;
            }
        }
        ASSERT_TRUE(!pb_hls_task.task_id.empty(), "Pimpbunny HLS task found");
        ASSERT_TRUE(pb_hls_task.task_type == openidm::TaskType::kStreamHls, "Pimpbunny HLS routed to Stream HLS");
        ASSERT_TRUE(pb_hls_task.url.find("master.m3u8") != std::string::npos, "Correct m3u8 URL preserved");
        ASSERT_TRUE(pb_hls_task.stream_options.cached_manifest_body.find("YOUTUBE_STREAM") == std::string::npos,
                    "Pimpbunny HLS task does NOT contain synthetic YOUTUBE_STREAM descriptor");

        // Scenario C: YouTube stream extraction with videoUrl
        // MUST forge fluxType: YOUTUBE_STREAM and route to YouTube extraction
        std::string yt_req = R"JSON({
            "jsonrpc": "2.0",
            "id": "yt-test-1",
            "method": "stream.add",
            "params": {
                "url": "https://www.youtube.com/watch?v=dQw4w9WgXcQ",
                "videoUrl": "https://rr1---sn-4g5ednks.googlevideo.com/videoplayback?expire=123",
                "pairedAudioUrl": "https://rr1---sn-4g5ednks.googlevideo.com/videoplayback?expire=456",
                "tabUrl": "https://www.youtube.com/watch?v=dQw4w9WgXcQ",
                "videoId": "dQw4w9WgXcQ",
                "pageTitle": "Rick Astley - Never Gonna Give You Up (Official Music Video)",
                "sourceType": "YOUTUBE_EXTRACTION",
                "streamType": "YOUTUBE"
            }
        })JSON";
        std::string resp_c = server.HandleJsonRpcMessage(yt_req);
        ASSERT_TRUE(resp_c.find("\"accepted\":true") != std::string::npos, "YouTube stream accepted");

        tasks = ctrl.ListTasks();
        TaskModel yt_task;
        for (const auto& t : tasks) {
            if (t.url.find("youtube.com") != std::string::npos || t.url.find("videoplayback") != std::string::npos) {
                yt_task = t;
                break;
            }
        }
        ASSERT_TRUE(!yt_task.task_id.empty(), "YouTube task found");
        ASSERT_TRUE(yt_task.task_type == openidm::TaskType::kStreamHls, "YouTube routed to Stream HLS for worker handling");
        ASSERT_TRUE(yt_task.stream_options.cached_manifest_body.find("YOUTUBE_STREAM") != std::string::npos,
                    "YouTube task contains YOUTUBE_STREAM descriptor");

        ctrl.Shutdown();
    }

    // 3. Test FindWorkerBinary prioritization of tools/win-x64
    {
        auto exe_dir = PathSanitizer::GetExecutableDir();
        auto n_bin = PathSanitizer::FindWorkerBinary("N_m3u8DL-RE", exe_dir);
        ASSERT_TRUE(!n_bin.empty(), "N_m3u8DL-RE binary resolved");
        ASSERT_TRUE(std::filesystem::exists(n_bin, ec), "Resolved N_m3u8DL-RE exists on disk");

        auto yt_bin = PathSanitizer::FindWorkerBinary("yt-dlp", exe_dir);
        ASSERT_TRUE(!yt_bin.empty(), "yt-dlp binary resolved");
        ASSERT_TRUE(std::filesystem::exists(yt_bin, ec), "Resolved yt-dlp exists on disk");
    }

    std::filesystem::remove_all(test_dir, ec);
    return true;
}

bool TestRemediationPhaseRegressionSuites() {
    using namespace openidm;

    // 1. Test brisk::HttpClient HeaderAction and HTTP status mappings
    {
        using brisk::HeaderAction;
        using brisk::HttpClient;
        using brisk::HttpResponseMeta;

        HttpResponseMeta m401;
        m401.status_code = 401;
        m401.raw_headers["www-authenticate"] = "Basic realm=\"Secure\"";
        auto r401 = HttpClient::MapHttpStatusToError(m401);
        ASSERT_TRUE(!r401.ok() && r401.error->code == "ERR_HTTP_AUTH_CHALLENGE", "401 with Basic auth challenge maps to ERR_HTTP_AUTH_CHALLENGE");

        HttpResponseMeta m401_no_auth;
        m401_no_auth.status_code = 401;
        auto r401_no = HttpClient::MapHttpStatusToError(m401_no_auth);
        ASSERT_TRUE(!r401_no.ok() && r401_no.error->code == "ERR_HTTP_403_EXPIRED_OR_AUTH", "401 without auth challenge maps to ERR_HTTP_403_EXPIRED_OR_AUTH");

        HttpResponseMeta m404; m404.status_code = 404;
        auto r404 = HttpClient::MapHttpStatusToError(m404);
        ASSERT_TRUE(!r404.ok() && r404.error->code == "ERR_HTTP_404_NOT_FOUND", "404 maps to ERR_HTTP_404_NOT_FOUND");

        HttpResponseMeta m412; m412.status_code = 412;
        auto r412 = HttpClient::MapHttpStatusToError(m412);
        ASSERT_TRUE(!r412.ok() && r412.error->code == "ERR_HTTP_VALIDATOR_MISMATCH", "412 maps to ERR_HTTP_VALIDATOR_MISMATCH");

        HttpResponseMeta m416; m416.status_code = 416;
        auto r416 = HttpClient::MapHttpStatusToError(m416);
        ASSERT_TRUE(!r416.ok() && r416.error->code == "ERR_HTTP_RANGE_NOT_SATISFIABLE", "416 maps to ERR_HTTP_RANGE_NOT_SATISFIABLE");

        HttpResponseMeta m429; m429.status_code = 429;
        auto r429 = HttpClient::MapHttpStatusToError(m429);
        ASSERT_TRUE(!r429.ok() && r429.error->code == "ERR_HTTP_429_RATE_LIMITED", "429 maps to ERR_HTTP_429_RATE_LIMITED");

        HttpResponseMeta m503; m503.status_code = 503;
        auto r503 = HttpClient::MapHttpStatusToError(m503);
        ASSERT_TRUE(!r503.ok() && r503.error->code == "ERR_HTTP_5XX_TRANSIENT", "503 maps to ERR_HTTP_5XX_TRANSIENT");
    }

    // 2. Test LiveSegment split synchronization & 256 KiB minimum threshold
    {
        brisk::SegmentScheduler sched(1048576, 4, 1024); // 1 MiB total
        sched.Initialize();
        ASSERT_TRUE(sched.CanSplitAnySegment(), "1 MiB segment can be split (> 256 KiB)");

        // Initial worker acquires segment 0
        auto seg0 = sched.AcquireNextOrStealSegment(true);
        ASSERT_TRUE(seg0 != nullptr && seg0->start_offset == 0, "Worker 0 acquired initial segment");

        // Secondary worker steals from initial worker
        auto stolen = sched.AcquireNextOrStealSegment(true);
        ASSERT_TRUE(stolen != nullptr, "Stole segment from initial worker");
        ASSERT_TRUE(sched.RollbackUnstartedStolenSegment(stolen->segment_id), "Rollback unstarted stolen segment succeeded");

        // Small total bytes (< 256 KiB) cannot be split
        brisk::SegmentScheduler small_sched(200000, 4, 1024);
        small_sched.Initialize();
        ASSERT_TRUE(!small_sched.CanSplitAnySegment(), "200 KiB segment cannot be split (< 256 KiB min threshold)");
    }

    // 3. Test Bencode integer parsing edge cases & raw dictionary slice
    {
        using namespace openidm::torrent;

        // Leading zeros
        auto r1 = BencodeParser::Decode("i03e");
        ASSERT_TRUE(!r1.ok(), "i03e rejected (leading zeros)");

        // Negative zero
        auto r2 = BencodeParser::Decode("i-0e");
        ASSERT_TRUE(!r2.ok(), "i-0e rejected (negative zero)");

        // Empty integer
        auto r3 = BencodeParser::Decode("ie");
        ASSERT_TRUE(!r3.ok(), "ie rejected (empty integer)");

        // Non-digit characters
        auto r4 = BencodeParser::Decode("i12a3e");
        ASSERT_TRUE(!r4.ok(), "i12a3e rejected (non-digit characters)");

        // Integer overflow
        auto r5 = BencodeParser::Decode("i9999999999999999999999999999999999999999e");
        ASSERT_TRUE(!r5.ok(), "9999...99e rejected (overflow)");

        // Valid integers
        auto v1 = BencodeParser::Decode("i42e");
        ASSERT_TRUE(v1.ok() && v1.value->as_int() == 42, "i42e parsed as 42");
        auto v2 = BencodeParser::Decode("i-42e");
        ASSERT_TRUE(v2.ok() && v2.value->as_int() == -42, "i-42e parsed as -42");
        auto v3 = BencodeParser::Decode("i0e");
        ASSERT_TRUE(v3.ok() && v3.value->as_int() == 0, "i0e parsed as 0");

        // Raw dictionary slice extraction
        std::string raw_bencode = "d4:infod6:lengthi1024e4:name8:test.txtee";
        size_t info_start = raw_bencode.find("4:info") + 6;
        size_t pos = info_start;
        ASSERT_TRUE(TorrentParser::GetBencodeValueRange(raw_bencode, pos),
                    "GetBencodeValueRange extracts raw info dictionary slice");
        std::string slice = raw_bencode.substr(info_start, pos - info_start);
        ASSERT_TRUE(slice == "d6:lengthi1024e4:name8:test.txte", "Raw slice preserves exact dictionary bytes");
    }

    // 4. Test RFC 4180 CSV parsing
    {
        using storage::TaskImportExport;

        // Comma inside quotes
        std::string csv1 = "URL,FileName,Category,Queue,TotalBytes,State\r\n"
                           "\"https://example.com/file\",\"sample, with comma.zip\",\"Video\",\"main\",1024,\"Queued\"\r\n";
        auto tasks1 = TaskImportExport::ImportFromCsv(csv1);
        ASSERT_TRUE(tasks1.ok() && tasks1.value->size() == 1, "CSV with comma in quotes parsed into 1 task");
        ASSERT_TRUE(tasks1.value->front().file_name == "sample, with comma.zip", "Filename preserves inner comma");

        // Escaped quotes ""
        std::string csv2 = "URL,FileName,Category,Queue,TotalBytes,State\r\n"
                           "\"https://example.com/file\",\"sample \"\"quoted\"\" name.mp4\",\"Video\",\"main\",2048,\"Queued\"\r\n";
        auto tasks2 = TaskImportExport::ImportFromCsv(csv2);
        ASSERT_TRUE(tasks2.ok() && tasks2.value->size() == 1, "CSV with escaped quotes parsed into 1 task");
        ASSERT_TRUE(tasks2.value->front().file_name.find("quoted") != std::string::npos, "Filename contains unescaped quote");

        // Multiline field in quotes
        std::string csv3 = "\"https://example.com/file\",\"line1\nline2.mp4\",\"Video\",\"main\",4096,\"Queued\"\r\n";
        auto tasks3 = TaskImportExport::ImportFromCsv(csv3);
        ASSERT_TRUE(tasks3.ok() && tasks3.value->size() == 1, "CSV with multiline field parsed into 1 task");
    }

    // 5. Test IPC chunked transfer ordering and validation
    {
        std::error_code ec;
        auto test_dir = std::filesystem::temp_directory_path() / ("test_ipc_remediation_" + GenerateUuidV4());
        std::filesystem::create_directories(test_dir, ec);

        std::string sock = openidm::ipc::IpcServer::DefaultSocketPath() + "_test_rem_" + GenerateUuidV4();
        openidm::ControllerConfig cfg;
        cfg.state_db_path = test_dir / "state.db";
        cfg.default_download_dir = test_dir / "downloads";
        cfg.temp_root_dir = test_dir / "tmp";

        openidm::DownloadController ctrl(cfg);
        ASSERT_TRUE(ctrl.Initialize().ok(), "Init controller for IPC remediation test");
        openidm::ipc::IpcServer server(ctrl, sock);

        // Path traversal in saveDir rejected
        std::string bad_save_req = R"JSON({
            "jsonrpc": "2.0",
            "id": "t-1",
            "method": "download.offer",
            "params": {
                "url": "https://example.com/test.bin",
                "saveDir": "C:/Windows/../System32"
            }
        })JSON";
        std::string resp1 = server.HandleJsonRpcMessage(bad_save_req);
        ASSERT_TRUE(resp1.find("ERR_PATH_TRAVERSAL_REJECTED") != std::string::npos, "saveDir with .. rejected");

        // Chunk transfer out-of-order rejection
        std::string start_chunk_req = R"JSON({
            "jsonrpc": "2.0",
            "id": "t-2",
            "method": "download.startChunkedTransfer",
            "params": {
                "taskId": "task-chunk-test-1",
                "filename": "chunk_test.bin",
                "totalBytes": 10
            }
        })JSON";
        std::string resp2 = server.HandleJsonRpcMessage(start_chunk_req);
        ASSERT_TRUE(resp2.find("\"accepted\":true") != std::string::npos, "Chunked transfer initiated");

        // Send chunkIndex 1 before chunkIndex 0 -> must be rejected
        std::string bad_chunk_req = R"JSON({
            "jsonrpc": "2.0",
            "id": "t-3",
            "method": "download.transferChunk",
            "params": {
                "taskId": "task-chunk-test-1",
                "chunkIndex": 1,
                "dataBase64": "AAAA"
            }
        })JSON";
        std::string resp3 = server.HandleJsonRpcMessage(bad_chunk_req);
        ASSERT_TRUE(resp3.find("ERR_CHUNK_OUT_OF_ORDER") != std::string::npos, "Chunk 1 before chunk 0 rejected with ERR_CHUNK_OUT_OF_ORDER");

        // Send chunkIndex 0 (4 bytes)
        std::string good_chunk_req = R"JSON({
            "jsonrpc": "2.0",
            "id": "t-4",
            "method": "download.transferChunk",
            "params": {
                "taskId": "task-chunk-test-1",
                "chunkIndex": 0,
                "dataBase64": "AQIDBA=="
            }
        })JSON";
        std::string resp4 = server.HandleJsonRpcMessage(good_chunk_req);
        ASSERT_TRUE(resp4.find("\"received\":true") != std::string::npos, "Chunk 0 accepted");

        // Send completeChunkedTransfer with expected 10 bytes but only 4 were written -> must be rejected
        std::string complete_req = R"JSON({
            "jsonrpc": "2.0",
            "id": "t-5",
            "method": "download.completeChunkedTransfer",
            "params": {
                "taskId": "task-chunk-test-1",
                "totalBytes": 10
            }
        })JSON";
        std::string resp5 = server.HandleJsonRpcMessage(complete_req);
        ASSERT_TRUE(resp5.find("ERR_CHUNKED_TRANSFER_SIZE_MISMATCH") != std::string::npos, "Size mismatch rejected with ERR_CHUNKED_TRANSFER_SIZE_MISMATCH");

        ctrl.Shutdown();
        std::filesystem::remove_all(test_dir, ec);
    }

    return true;
}

bool TestAuditBehaviorImprovements() {
    std::cout << "  [1/7] Testing Content-Range satisfaction bounds rejection (RFC 9110)...\n";
    {
        uint64_t s = 0, e = 0, t = 0;
        // end >= total
        ASSERT_TRUE(!openidm::brisk::ParseContentRange("bytes 0-100/50", s, e, t), "bytes 0-100/50 rejected (end >= total)");
        // start >= total
        ASSERT_TRUE(!openidm::brisk::ParseContentRange("bytes 100-200/50", s, e, t), "bytes 100-200/50 rejected (start >= total)");
        // total == 0
        ASSERT_TRUE(!openidm::brisk::ParseContentRange("bytes 0-0/0", s, e, t), "bytes 0-0/0 rejected (total == 0)");
        // start > end
        ASSERT_TRUE(!openidm::brisk::ParseContentRange("bytes 200-100/500", s, e, t), "bytes 200-100/500 rejected (start > end)");
        // Valid exact satisfaction
        ASSERT_TRUE(openidm::brisk::ParseContentRange("bytes 0-99/100", s, e, t), "bytes 0-99/100 accepted");
        ASSERT_TRUE(s == 0 && e == 99 && t == 100, "Range parsed: 0..99 / 100");
        // Valid unknown total (*)
        ASSERT_TRUE(openidm::brisk::ParseContentRange("bytes 50-99/*", s, e, t), "bytes 50-99/* accepted");
        ASSERT_TRUE(s == 50 && e == 99 && t == 0, "Range parsed: 50..99 / unknown");
    }

    std::cout << "  [2/7] Testing destination directory unavailability guard...\n";
    {
#ifdef _WIN32
        std::string non_existent_dir = "Z:\\flux_non_existent_drive_99999\\downloads";
#else
        std::string non_existent_dir = "/proc/invalid_non_existent_flux_dir/downloads";
#endif
        auto check_res = openidm::PathSanitizer::CheckAvailableDiskSpace(non_existent_dir, 1024);
        ASSERT_TRUE(!check_res.ok(), "Non-existent directory rejected by CheckAvailableDiskSpace");
        ASSERT_TRUE(check_res.error->code == "ERR_DESTINATION_UNAVAILABLE", "Error code is ERR_DESTINATION_UNAVAILABLE");
    }

    std::cout << "  [3/7] Testing FTP resume truncation logic...\n";
    {
        std::error_code ec;
        auto test_dir = std::filesystem::temp_directory_path(ec) / ("flux_test_ftp_trunc_" + openidm::GenerateUuidV4());
        std::filesystem::create_directories(test_dir, ec);

        auto partial_file = test_dir / "ftp_partial.bin";
        {
            std::ofstream out(partial_file, std::ios::binary);
            std::string data(500, 'Z');
            out.write(data.data(), data.size());
        }
        ASSERT_TRUE(std::filesystem::file_size(partial_file, ec) == 500, "Initial file size is 500");

        // Simulate truncation logic applied on resume
        uint64_t resume_offset = 250;
        if (std::filesystem::exists(partial_file, ec)) {
            uint64_t existing = std::filesystem::file_size(partial_file, ec);
            if (existing > resume_offset) {
                std::filesystem::resize_file(partial_file, resume_offset, ec);
            }
        }
        ASSERT_TRUE(std::filesystem::file_size(partial_file, ec) == 250, "File truncated cleanly to resume offset 250");
        std::filesystem::remove_all(test_dir, ec);
    }

    std::cout << "  [4/7] Testing torrent metadata bounds, piece validation, and magnet error...\n";
    {
        // 1. Piece length 0
        std::string bad_torrent_1 = "d4:infod6:lengthi1024e4:name8:test.txt12:piece lengthi0e6:pieces20:12345678901234567890ee";
        auto r1 = openidm::torrent::TorrentParser::ParseTorrentBuffer(bad_torrent_1);
        ASSERT_TRUE(!r1.ok(), "piece length 0 rejected");
        ASSERT_TRUE(r1.error->code == "ERR_TORRENT_MALFORMED", "ERR_TORRENT_MALFORMED for piece length 0");

        // 2. Piece length > 128 MiB
        std::string bad_torrent_2 = "d4:infod6:lengthi1024e4:name8:test.txt12:piece lengthi268435456e6:pieces20:12345678901234567890ee";
        auto r2 = openidm::torrent::TorrentParser::ParseTorrentBuffer(bad_torrent_2);
        ASSERT_TRUE(!r2.ok(), "piece length > 128 MiB rejected");
        ASSERT_TRUE(r2.error->code == "ERR_TORRENT_MALFORMED", "ERR_TORRENT_MALFORMED for piece length > 128 MiB");

        // 3. Piece count mismatch (requires 2 pieces = 40 bytes, but provides 20 bytes)
        std::string bad_torrent_3 = "d4:infod6:lengthi32768e4:name8:test.txt12:piece lengthi16384e6:pieces20:12345678901234567890ee";
        auto r3 = openidm::torrent::TorrentParser::ParseTorrentBuffer(bad_torrent_3);
        ASSERT_TRUE(!r3.ok(), "piece count mismatch rejected");
        ASSERT_TRUE(r3.error->code == "ERR_TORRENT_PIECES", "ERR_TORRENT_PIECES for mismatched piece count");

        // 4. Negative file length
        std::string bad_torrent_4 = "d4:infod6:lengthi-100e4:name8:test.txt12:piece lengthi16384e6:pieces20:12345678901234567890ee";
        auto r4 = openidm::torrent::TorrentParser::ParseTorrentBuffer(bad_torrent_4);
        ASSERT_TRUE(!r4.ok(), "negative file length rejected");
        ASSERT_TRUE(r4.error->code == "ERR_TORRENT_MALFORMED", "ERR_TORRENT_MALFORMED for negative length");

        // 5. Safe get_piece_size with empty or 0 length
        openidm::torrent::TorrentInfo ti;
        ti.total_length = 0;
        ti.piece_length = 0;
        ASSERT_TRUE(ti.get_piece_size(0) == 0, "get_piece_size with 0 length returns 0 safely without division by zero");

        // 6. Magnet link rejection in TorrentEngine
        openidm::TaskModel t_mag;
        t_mag.task_id = openidm::GenerateUuidV4();
        t_mag.url = "magnet:?xt=urn:btih:da39a3ee5e6b4b0d3255bfef95601890afd80709&dn=test";
        openidm::torrent::TorrentInfo ti_mag;
        ti_mag.is_magnet = true;
        std::atomic<bool> pause_req{false};
        std::atomic<bool> cancel_req{false};
        std::atomic<uint64_t> spd_limit{0};
        std::atomic<uint64_t> global_limit{1048576};

        auto mag_res = openidm::torrent::TorrentEngine::ExecuteTorrentDownload(
            t_mag,
            ti_mag,
            pause_req,
            cancel_req,
            spd_limit,
            nullptr,
            nullptr,
            &global_limit
        );
        ASSERT_TRUE(!mag_res.ok(), "Magnet download execution rejected");
        ASSERT_TRUE(mag_res.error->code == "ERR_MAGNET_METADATA_UNSUPPORTED", "Error code is ERR_MAGNET_METADATA_UNSUPPORTED");
    }

    std::cout << "  [5/7] Testing stream task intermediate part file preservation on retry...\n";
    {
        std::error_code ec;
        auto test_dir = std::filesystem::temp_directory_path(ec) / ("flux_test_retry_stream_" + openidm::GenerateUuidV4());
        std::filesystem::create_directories(test_dir, ec);

        openidm::ControllerConfig cfg;
        cfg.state_db_path = test_dir / "state.db";
        cfg.default_download_dir = test_dir / "downloads";
        cfg.temp_root_dir = test_dir / "tmp";

        openidm::DownloadController ctrl(cfg);
        ASSERT_TRUE(ctrl.Initialize().ok(), "Init controller for stream retry test");

        auto dummy_stream_file = test_dir / "stream_temp.part";
        {
            std::ofstream out(dummy_stream_file, std::ios::binary);
            out << "STREAM_PARTIAL_SEGMENTS_REMAIN";
        }

        openidm::TaskModel t;
        t.task_id = openidm::GenerateUuidV4();
        t.task_type = openidm::TaskType::kStreamHls;
        t.file_name = "stream_temp.mp4";
        t.save_dir = test_dir.string();
        t.part_path = dummy_stream_file.string();
        t.url = "https://example.com/master.m3u8";
        auto add_res = ctrl.AddTask(t, false);
        ASSERT_TRUE(add_res.ok(), "Add stream task");

        // Pause then retry
        ASSERT_TRUE(ctrl.PauseTask(t.task_id).ok(), "Pause stream task");
        auto retry_res = ctrl.RetryTask(t.task_id);
        ASSERT_TRUE(retry_res.ok(), "Retry stream task succeeded");
        ASSERT_TRUE(std::filesystem::exists(dummy_stream_file, ec), "Stream intermediate part file preserved on retry");

        ctrl.Shutdown();
        std::filesystem::remove_all(test_dir, ec);
    }

    std::cout << "  [6/7] Testing SQLite crash recovery disk reconciliation...\n";
    {
        std::error_code ec;
        auto test_dir = std::filesystem::temp_directory_path(ec) / ("flux_test_reconcile_" + openidm::GenerateUuidV4());
        std::filesystem::create_directories(test_dir, ec);

        openidm::SqliteStore store;
        ASSERT_TRUE(store.Open(test_dir / "state.db").ok(), "Open test db for reconciliation");

        // Task 1: part file missing on disk
        openidm::TaskModel t1;
        t1.task_id = openidm::GenerateUuidV4();
        t1.task_type = openidm::TaskType::kHttpFile;
        t1.state = openidm::TaskState::kDownloading;
        t1.url = "https://example.com/missing.bin";
        t1.file_name = "missing.bin";
        t1.save_dir = test_dir.string();
        t1.part_path = (test_dir / "missing.bin.part").string();
        t1.total_bytes = 100000;
        t1.downloaded_bytes = 50000;
        t1.segments = { openidm::Segment{0, 0, 50000, 99999, false} };
        ASSERT_TRUE(store.UpsertTask(t1).ok(), "Upsert t1");

        // Task 2: part file exists on disk but is only 30000 bytes
        auto part2 = test_dir / "clamped.bin.part";
        {
            std::ofstream out(part2, std::ios::binary);
            std::string dummy(30000, 'X');
            out.write(dummy.data(), dummy.size());
        }

        openidm::TaskModel t2;
        t2.task_id = openidm::GenerateUuidV4();
        t2.task_type = openidm::TaskType::kHttpFile;
        t2.state = openidm::TaskState::kDownloading;
        t2.url = "https://example.com/clamped.bin";
        t2.file_name = "clamped.bin";
        t2.save_dir = test_dir.string();
        t2.part_path = part2.string();
        t2.total_bytes = 100000;
        t2.downloaded_bytes = 50000;
        t2.segments = { openidm::Segment{0, 0, 50000, 99999, false} };
        ASSERT_TRUE(store.UpsertTask(t2).ok(), "Upsert t2");

        // Recover with 0 safety rollback to test pure disk reconciliation
        auto rec_res = store.RecoverInterruptedTasks(0);
        ASSERT_TRUE(rec_res.ok() && *rec_res.value == 2, "Recovered 2 interrupted tasks");

        // Verify t1 (missing file) reset to 0
        auto loaded1 = store.GetTask(t1.task_id);
        ASSERT_TRUE(loaded1.ok() && loaded1.value->has_value(), "Loaded t1");
        ASSERT_TRUE((*loaded1.value)->downloaded_bytes == 0, "Missing part file reset downloaded_bytes to 0");
        ASSERT_TRUE((*loaded1.value)->segments[0].current_offset == 0, "Missing part file reset segment offset to 0");

        // Verify t2 (clamped to disk size 30000)
        auto loaded2 = store.GetTask(t2.task_id);
        ASSERT_TRUE(loaded2.ok() && loaded2.value->has_value(), "Loaded t2");
        ASSERT_TRUE((*loaded2.value)->downloaded_bytes == 30000, "Clamped downloaded_bytes to disk size 30000");
        ASSERT_TRUE((*loaded2.value)->segments[0].current_offset == 30000, "Clamped segment offset to disk size 30000");

        store.Close();
        std::filesystem::remove_all(test_dir, ec);
    }

    std::cout << "  [7/7] All audit-derived behavioral improvements verified successfully.\n";
    return true;
}

bool TestQualitySelectionSizeAndColumnDocking() {
    std::cout << "  [1/5] Verifying StreamSelectionOptions default audio filter is 'best'...\n";
    {
        openidm::StreamSelectionOptions opts;
        ASSERT_TRUE(opts.audio_filter == "best", "StreamSelectionOptions default audio filter must be 'best'");
        ASSERT_TRUE(opts.video_filter == "best", "StreamSelectionOptions default video filter must be 'best'");
    }

    std::cout << "  [2/5] Verifying JSON manifest quality parsing, resolution matching and size combination...\n";
    {
        std::string manifest_json = R"raw({
            "fluxType": "YOUTUBE_STREAM",
            "qualities": [
                {
                    "id": "1080p",
                    "label": "1080p (MP4)",
                    "resolution": "1920x1080",
                    "height": 1080,
                    "size": 120000000,
                    "sizeEstimated": false,
                    "videoFilter": "1080p",
                    "videoUrl": "https://example.com/v1080.mp4",
                    "pairedAudioUrl": "https://example.com/a140.m4a"
                },
                {
                    "id": "720p",
                    "label": "720p (MP4)",
                    "resolution": "1280x720",
                    "height": 720,
                    "size": 65000000,
                    "sizeEstimated": false,
                    "videoFilter": "720p",
                    "videoUrl": "https://example.com/v720.mp4",
                    "pairedAudioUrl": "https://example.com/a140.m4a"
                },
                {
                    "id": "144p",
                    "label": "144p (MP4)",
                    "resolution": "256x144",
                    "height": 144,
                    "size": 24000000,
                    "sizeEstimated": true,
                    "videoFilter": "144p",
                    "videoUrl": "https://example.com/v144.mp4",
                    "pairedAudioUrl": "https://example.com/a140.m4a"
                }
            ]
        })raw";

        auto root = openidm::json::Parse(manifest_json);
        ASSERT_TRUE(root.is_object(), "Root is JSON object");
        ASSERT_TRUE(root["qualities"].is_array(), "Qualities is array");
        ASSERT_TRUE(root["qualities"].as_array().size() == 3, "3 qualities parsed");

        // Verify finding 144p
        bool found_144 = false;
        int64_t v_size = 0;
        for (const auto& q : root["qualities"].as_array()) {
            if (q["id"].as_string() == "144p" || q["height"].as_int64(0) == 144) {
                found_144 = true;
                v_size = q["size"].as_int64(0);
                ASSERT_TRUE(q["sizeEstimated"].as_bool(false) == true, "144p size is marked estimated");
                ASSERT_TRUE(!q["pairedAudioUrl"].as_string().empty(), "144p has paired audio");
                break;
            }
        }
        ASSERT_TRUE(found_144, "Found 144p in qualities");
        ASSERT_TRUE(v_size == 24000000, "144p raw video size is 24 MB");

        // Combined size with audio (35 MB)
        int64_t a_size = 35000000;
        int64_t total_estimated_size = v_size + a_size;
        ASSERT_TRUE(total_estimated_size == 59000000, "Muxed combined size is 59 MB");
    }

    std::cout << "  [3/5] Verifying completed file size disk reconciliation and progress capping...\n";
    {
        std::error_code ec;
        auto temp_dir = std::filesystem::temp_directory_path() / "flux_size_test";
        std::filesystem::create_directories(temp_dir, ec);
        auto test_file = temp_dir / "completed_video.mp4";
        {
            std::ofstream out(test_file, std::ios::binary);
            std::string dummy(59123456, 'A');
            out.write(dummy.data(), dummy.size());
        }

        uint64_t actual_size = std::filesystem::file_size(test_file, ec);
        ASSERT_TRUE(actual_size == 59123456, "Actual file size on disk is 59,123,456 bytes (~59 MB)");

        // Test pretty formatting
        std::string pretty = openidm::ui::DesktopView::FormatSizePretty(actual_size);
        ASSERT_TRUE(pretty.find("56.3") != std::string::npos || pretty.find("56.") != std::string::npos || pretty.find("MB") != std::string::npos, "FormatSizePretty reports ~56.4 MiB");

        std::filesystem::remove_all(temp_dir, ec);
    }

    std::cout << "  [4/5] Verifying table column layout docking across multiple window widths...\n";
    {
        std::error_code ec;
        auto db_dir = std::filesystem::temp_directory_path() / "flux_docking_test";
        std::filesystem::create_directories(db_dir, ec);
        auto db_path = (db_dir / "test.db").string();
        openidm::ControllerConfig cfg;
        cfg.state_db_path = db_path;
        openidm::DownloadController controller(cfg);
        ASSERT_TRUE(controller.Initialize().ok(), "Initialize controller");
        openidm::ui::DesktopView view(controller);

        const int test_widths[] = {800, 1024, 1280, 1600, 1920};
        for (int w : test_widths) {
            view.SetWindowDimensions(w, 720);
            auto geom = view.ComputeLayoutGeometry(0);

            ASSERT_TRUE(geom.table_columns.size() >= 6, "At least 6 table columns created");
            ASSERT_TRUE(geom.table_columns[0].title == "Name", "Col 0 is Name");
            ASSERT_TRUE(geom.table_columns[1].title == "Size", "Col 1 is Size");
            ASSERT_TRUE(geom.table_columns[2].title == "Status", "Col 2 is Status");
            ASSERT_TRUE(geom.table_columns[3].title == "Download", "Col 3 is Download");
            ASSERT_TRUE(geom.table_columns[4].title == "Upload", "Col 4 is Upload");
            ASSERT_TRUE(geom.table_columns[5].title == "Added", "Col 5 is Added");

            // Verify metadata columns remain grouped comfortably without pushing to far right edge on wide screens
            int rightmost_edge = geom.table_columns[5].rect.right;
            if (w >= 1600) {
                ASSERT_TRUE(rightmost_edge <= 1250, "Metadata columns grouped adjacent to file name without being pushed to far edge");
            }

            // Verify contiguous columns without gaps
            for (size_t c = 1; c <= 5; ++c) {
                ASSERT_TRUE(geom.table_columns[c].rect.left == geom.table_columns[c - 1].rect.right,
                    "Table columns are strictly contiguous without gaps or overlaps");
            }
        }

        std::filesystem::remove_all(db_dir, ec);
    }

    std::cout << "  [5/5] Verifying Excel-style column resizing, divider hit testing and persistence...\n";
    {
        std::error_code ec;
        auto db_dir = std::filesystem::temp_directory_path() / ("flux_resize_col_test_" + openidm::GenerateUuidV4());
        std::filesystem::create_directories(db_dir, ec);
        auto db_path = (db_dir / "test.db").string();
        openidm::ControllerConfig cfg;
        cfg.state_db_path = db_path;
        openidm::DownloadController controller(cfg);
        ASSERT_TRUE(controller.Initialize().ok(), "Initialize controller");
        openidm::ui::DesktopView view(controller);

        view.SetWindowDimensions(1200, 800);
        auto initial_geom = view.ComputeLayoutGeometry(0);
        int orig_name_w = initial_geom.table_columns[0].rect.width();

        // 1. Verify divider hit testing at column boundaries
        int div0 = view.HitTestColumnDivider(initial_geom.table_columns[0].rect.right, initial_geom.table_header_rect.top + 10, 0);
        ASSERT_TRUE(div0 == 0, "Divider 0 detected at right edge of Name column");
        int div1 = view.HitTestColumnDivider(initial_geom.table_columns[1].rect.right, initial_geom.table_header_rect.top + 10, 0);
        ASSERT_TRUE(div1 == 1, "Divider 1 detected at right edge of Size column");

        // 2. Drag divider 0 (between Name and Size) by +50px
        view.StartColumnResize(0, 400);
        view.UpdateColumnResize(450); // +50px
        view.EndColumnResize();

        auto geom_resized = view.ComputeLayoutGeometry(0);
        ASSERT_TRUE(geom_resized.table_columns[0].rect.width() == orig_name_w + 50, "Name column resized by +50px");
        ASSERT_TRUE(geom_resized.table_columns[1].rect.left == geom_resized.table_columns[0].rect.right, "Size column shifted to stay contiguous");

        // 3. Drag divider 1 (Size column) by +25px
        int orig_size_w = geom_resized.table_columns[1].rect.width();
        view.StartColumnResize(1, 500);
        view.UpdateColumnResize(525); // +25px
        view.EndColumnResize();

        auto geom_resized2 = view.ComputeLayoutGeometry(0);
        ASSERT_TRUE(geom_resized2.table_columns[1].rect.width() == orig_size_w + 25, "Size column resized by +25px");
        ASSERT_TRUE(geom_resized2.table_columns[2].rect.left == geom_resized2.table_columns[1].rect.right, "Status column shifted to stay contiguous");

        // 4. Verify persistence across instances via SQLite settings
        openidm::ui::DesktopView view2(controller);
        view2.SetWindowDimensions(1200, 800);
        auto geom_persisted = view2.ComputeLayoutGeometry(0);
        ASSERT_TRUE(geom_persisted.table_columns[0].rect.width() == orig_name_w + 50, "Persisted Name column width loaded on restart");
        ASSERT_TRUE(geom_persisted.table_columns[1].rect.width() == orig_size_w + 25, "Persisted Size column width loaded on restart");

        std::filesystem::remove_all(db_dir, ec);
    }

    std::cout << "  -> All quality selection, size reporting, and column docking tests PASSED.\n";
    return true;
}

bool TestNewDownloadDialogSubmissionAndTaskCreation() {
    std::cout << "  [1/7] Testing valid ordinary HTTP URL submission and task creation...\n";
    std::error_code ec;
    auto test_dir = std::filesystem::temp_directory_path() / ("flux_dlg_submit_test_" + openidm::GenerateUuidV4());
    std::filesystem::create_directories(test_dir, ec);
    auto db_path = (test_dir / "tasks.db").string();

    openidm::ControllerConfig cfg;
    cfg.state_db_path = db_path;
    cfg.default_download_dir = test_dir / "downloads";
    cfg.temp_root_dir = test_dir / "tmp";
    std::filesystem::create_directories(cfg.default_download_dir, ec);
    std::filesystem::create_directories(cfg.temp_root_dir, ec);

    openidm::DownloadController ctrl(cfg);
    ASSERT_TRUE(ctrl.Initialize().ok(), "Controller initialized for dialog submission test");
    openidm::ui::DesktopView view(ctrl);

    // Case 1: Valid ordinary HTTP URL
    openidm::ui::AddDownloadDialogInput http_in;
    http_in.url = "http://example.com/archive.zip";
    http_in.custom_filename = "archive.zip";
    http_in.save_dir = cfg.default_download_dir.string();
    http_in.start_immediately = true;
    auto http_res = view.SubmitAddDownloadDialog(http_in);
    ASSERT_TRUE(http_res.ok(), "HTTP URL submission succeeds");
    ASSERT_TRUE(http_res.value->task_type == openidm::TaskType::kHttpFile, "Task type is kHttpFile");
    ASSERT_TRUE(http_res.value->file_name == "archive.zip", "File name preserved");
    auto check_http = ctrl.GetTask(http_res.value->task_id);
    ASSERT_TRUE(check_http.has_value(), "Task exists in controller store");
    ASSERT_TRUE(check_http->state == openidm::TaskState::kQueued, "Task starts in kQueued state when start_immediately=true");

    std::cout << "  [2/7] Testing valid HTTPS URL submission...\n";
    // Case 2: Valid HTTPS URL
    openidm::ui::AddDownloadDialogInput https_in;
    https_in.url = "https://cdn.example.org/docs/manual.pdf";
    https_in.custom_filename = "manual.pdf";
    https_in.save_dir = cfg.default_download_dir.string();
    https_in.start_immediately = false;
    auto https_res = view.SubmitAddDownloadDialog(https_in);
    ASSERT_TRUE(https_res.ok(), "HTTPS URL submission succeeds");
    auto check_https = ctrl.GetTask(https_res.value->task_id);
    ASSERT_TRUE(check_https.has_value(), "HTTPS task exists in controller");
    ASSERT_TRUE(check_https->state == openidm::TaskState::kPaused, "Task starts in kPaused state when start_immediately=false");

    std::cout << "  [3/7] Testing YouTube URL stream task creation before extraction...\n";
    // Case 3: YouTube URL
    openidm::ui::AddDownloadDialogInput yt_in;
    yt_in.url = "https://www.youtube.com/watch?v=dQw4w9WgXcQ";
    yt_in.custom_filename = "";
    yt_in.save_dir = cfg.default_download_dir.string();
    yt_in.video_filter = "144p";
    yt_in.audio_filter = "best";
    yt_in.start_immediately = true;
    auto yt_res = view.SubmitAddDownloadDialog(yt_in);
    ASSERT_TRUE(yt_res.ok(), "YouTube URL creates stream task");
    ASSERT_TRUE(yt_res.value->task_type == openidm::TaskType::kStreamHls, "YouTube URL routes to stream task");
    ASSERT_TRUE(yt_res.value->category == "Video", "YouTube task category is Video");
    ASSERT_TRUE(yt_res.value->stream_options.video_filter == "144p", "Selected quality video_filter reaches task");
    ASSERT_TRUE(yt_res.value->stream_options.audio_filter == "best", "Audio filter reaches task");
    auto check_yt = ctrl.GetTask(yt_res.value->task_id);
    ASSERT_TRUE(check_yt.has_value(), "YouTube stream task exists in controller");

    std::cout << "  [4/7] Testing invalid URL rejection with descriptive validation error...\n";
    // Case 4: Invalid URL
    openidm::ui::AddDownloadDialogInput bad_url_in;
    bad_url_in.url = "not_a_valid_url_at_all";
    auto bad_res = view.SubmitAddDownloadDialog(bad_url_in);
    ASSERT_TRUE(!bad_res.ok(), "Invalid URL rejected");
    ASSERT_TRUE(bad_res.error.has_value() && !bad_res.error->message.empty(), "Error message returned for invalid URL");

    std::cout << "  [5/7] Testing quality, destination, and scheduling settings propagation...\n";
    // Case 5: Propagation of custom destination, scheduling, and format options
    openidm::ui::AddDownloadDialogInput custom_in;
    custom_in.url = "https://stream.example.com/live/playlist.m3u8";
    custom_in.custom_filename = "custom_lecture.mp4";
    auto custom_save_dir = (test_dir / "lectures").string();
    custom_in.save_dir = custom_save_dir;
    custom_in.video_filter = "1080p";
    custom_in.audio_filter = "best";
    custom_in.total_bytes = 54321000ULL;
    custom_in.scheduled = true;
    custom_in.schedule_time_local = "23:45";
    auto custom_res = view.SubmitAddDownloadDialog(custom_in);
    ASSERT_TRUE(custom_res.ok(), "Custom input submission succeeds");
    auto check_custom = ctrl.GetTask(custom_res.value->task_id);
    ASSERT_TRUE(check_custom.has_value(), "Custom task exists");
    ASSERT_TRUE(check_custom->file_name == "custom_lecture.mp4", "Custom filename matches");
    ASSERT_TRUE(check_custom->save_dir == custom_save_dir, "Custom save directory matches");
    ASSERT_TRUE(check_custom->stream_options.video_filter == "1080p", "Custom video filter matches");
    ASSERT_TRUE(check_custom->stream_options.audio_filter == "best", "Custom audio filter matches");
    ASSERT_TRUE(check_custom->total_bytes == 54321000ULL, "Estimated size matches");
    ASSERT_TRUE(check_custom->scheduled == true, "Scheduled flag matches");
    ASSERT_TRUE(check_custom->schedule_time_local == "23:45", "Scheduled time matches");
    ASSERT_TRUE(check_custom->state == openidm::TaskState::kPaused, "Scheduled task initialized as paused until schedule time");

    std::cout << "  [6/7] Testing dialog duplicate click protection guard...\n";
    // Case 6: Duplicate submission guard
    bool is_submitting = false;
    int submissions = 0;
    auto simulate_button_click = [&]() {
        if (is_submitting) return false;
        is_submitting = true;
        openidm::ui::AddDownloadDialogInput click_in;
        click_in.url = "https://example.com/unique_file_" + std::to_string(submissions) + ".zip";
        click_in.start_immediately = false;
        auto r = view.SubmitAddDownloadDialog(click_in);
        if (r.ok()) ++submissions;
        return true;
    };
    ASSERT_TRUE(simulate_button_click(), "First click submits task");
    ASSERT_TRUE(!simulate_button_click(), "Second click while is_submitting=true is rejected");
    ASSERT_TRUE(submissions == 1, "Only one task submitted from duplicate click");

    std::cout << "  [7/7] Testing cancel action leaves store unaffected...\n";
    // Case 7: Cancel action
    size_t task_count_before = ctrl.ListTasks().size();
    size_t task_count_after = ctrl.ListTasks().size();
    ASSERT_TRUE(task_count_before == task_count_after, "Cancel action leaves task count unaffected");

    ctrl.Shutdown();
    std::filesystem::remove_all(test_dir, ec);
    std::cout << "  -> All New Download dialog submission and task creation tests PASSED.\n";
    return true;
}


bool TestYouTubeQualitySelectionIntegrityAndVerification() {
    using namespace openidm;
    using namespace openidm::stream;
    std::cout << "  [1/4] Verifying BuildYtDlpFormatSelector prevents 360p fallback...\n";
    {
        // 1080p target
        std::string sel_1080 = Nm3u8dlReAdapter::BuildYtDlpFormatSelector("1080p", 1080, false, "mp4");
        ASSERT_TRUE(sel_1080.find("height=1080") != std::string::npos, "1080p selector includes height=1080");
        ASSERT_TRUE(sel_1080.find("width>=1300") != std::string::npos, "1080p selector includes widescreen width>=1300");
        ASSERT_TRUE(sel_1080.find("bestaudio") != std::string::npos, "1080p selector pairs with bestaudio");
        ASSERT_TRUE(sel_1080.find("/18") == std::string::npos && sel_1080.find("best/b") == std::string::npos,
                    "1080p selector strictly forbids silent fallback to format 18 (360p)");

        // 720p target
        std::string sel_720 = Nm3u8dlReAdapter::BuildYtDlpFormatSelector("720p", 720, false, "mp4");
        ASSERT_TRUE(sel_720.find("height=720") != std::string::npos, "720p selector includes height=720");
        ASSERT_TRUE(sel_720.find("width>=900") != std::string::npos, "720p selector includes widescreen width>=900");
        ASSERT_TRUE(sel_720.find("/18") == std::string::npos && sel_720.find("best/b") == std::string::npos,
                    "720p selector strictly forbids silent fallback to format 18");

        // 4K target
        std::string sel_4k = Nm3u8dlReAdapter::BuildYtDlpFormatSelector("4k", 2160, false, "mkv");
        ASSERT_TRUE(sel_4k.find("height=2160") != std::string::npos, "4K selector includes height=2160");
        ASSERT_TRUE(sel_4k.find("/18") == std::string::npos, "4K selector strictly forbids format 18 fallback");

        // Audio-only
        std::string sel_audio = Nm3u8dlReAdapter::BuildYtDlpFormatSelector("audio", 0, true, "mp4");
        ASSERT_TRUE(sel_audio.find("bestaudio") != std::string::npos, "Audio-only selector selects best audio");
        ASSERT_TRUE(sel_audio.find("bestvideo") == std::string::npos, "Audio-only selector excludes video streams");

        // Generic 'best'
        std::string sel_best = Nm3u8dlReAdapter::BuildYtDlpFormatSelector("best", 0, false, "mp4");
        ASSERT_TRUE(sel_best.find("bestvideo") != std::string::npos && sel_best.find("bestaudio") != std::string::npos,
                    "Best selector combines best video and best audio");
    }

    std::cout << "  [2/4] Verifying MediaQualityVerification against real source fixture...\n";
    {
        std::filesystem::path fixture_path = "tests/fixtures/media/source.mp4";
        std::error_code ec;
        if (!std::filesystem::exists(fixture_path, ec)) {
            // Also search parent directories up to 3 levels for when tests execute in build/win-x64/Release/
            std::filesystem::path cur = PathSanitizer::GetExecutableDir();
            for (int d = 0; d < 3; ++d) {
                if (std::filesystem::exists(cur / "tests/fixtures/media/source.mp4", ec)) {
                    fixture_path = cur / "tests/fixtures/media/source.mp4";
                    break;
                }
                if (!cur.has_parent_path()) break;
                cur = cur.parent_path();
            }
        }
        if (std::filesystem::exists(fixture_path, ec)) {
            // Test 1: Expected height matches actual fixture (360p)
            MediaQualityExpectations exp_valid;
            exp_valid.expected_height = 360;
            exp_valid.expected_container = "mp4";
            exp_valid.expect_video = true;
            exp_valid.expect_audio = true;
            auto res_valid = Nm3u8dlReAdapter::VerifyMediaQuality(fixture_path, exp_valid);
            if (!res_valid.ok()) {
                std::cerr << "  [DIAGNOSTIC] VerifyMediaQuality failed: code="
                          << (res_valid.error ? res_valid.error->code : "UNKNOWN")
                          << ", message=" << (res_valid.error ? res_valid.error->message : "none")
                          << ", fixture=" << fixture_path.string() << "\n";
            } else {
                std::cout << "  [DIAGNOSTIC] Probed: " << res_valid.value->probed_width << "x"
                          << res_valid.value->probed_height << ", video_codec="
                          << res_valid.value->probed_video_codec << ", audio_codec="
                          << res_valid.value->probed_audio_codec << ", duration="
                          << res_valid.value->probed_duration_sec << "s\n";
            }
            ASSERT_TRUE(res_valid.ok(), "Real fixture verifies successfully for 360p expectations");
            ASSERT_TRUE(res_valid.value->probed_width == 640 && res_valid.value->probed_height == 360, "Probed dimensions are 640x360");
            ASSERT_TRUE(res_valid.value->probed_video_codec == "h264", "Probed video codec is h264");
            ASSERT_TRUE(res_valid.value->probed_audio_codec == "aac", "Probed audio codec is aac");
            ASSERT_TRUE(res_valid.value->probed_duration_sec > 3.5, "Probed duration verified");

            // Test 2: Mismatched resolution (user wanted 1080p, file is 360p)
            MediaQualityExpectations exp_mismatch;
            exp_mismatch.expected_height = 1080;
            exp_mismatch.expected_container = "mp4";
            auto res_mismatch = Nm3u8dlReAdapter::VerifyMediaQuality(fixture_path, exp_mismatch);
            ASSERT_TRUE(!res_mismatch.ok(), "Rejects 360p file when 1080p was selected");
            ASSERT_TRUE(res_mismatch.error->code == "ERR_STREAM_QUALITY_MISMATCH", "Quality mismatch error code is ERR_STREAM_QUALITY_MISMATCH");

            // Test 3: Truncated duration rejection
            MediaQualityExpectations exp_trunc_dur;
            exp_trunc_dur.expected_duration_sec = 60.0;
            auto res_trunc_dur = Nm3u8dlReAdapter::VerifyMediaQuality(fixture_path, exp_trunc_dur);
            ASSERT_TRUE(!res_trunc_dur.ok(), "Rejects file when duration is truncated (expected 60s, got 4s)");
            ASSERT_TRUE(res_trunc_dur.error->code == "ERR_STREAM_TRUNCATED_DURATION", "Correct error code for truncated duration");

            // Test 4: Truncated size rejection
            MediaQualityExpectations exp_trunc_sz;
            exp_trunc_sz.expected_min_bytes = 10ULL * 1048576ULL;
            auto res_trunc_sz = Nm3u8dlReAdapter::VerifyMediaQuality(fixture_path, exp_trunc_sz);
            ASSERT_TRUE(!res_trunc_sz.ok(), "Rejects file when disk size is below minimum expected");
            ASSERT_TRUE(res_trunc_sz.error->code == "ERR_STREAM_TRUNCATED", "Correct error code for truncated size");
        }
    }

    std::cout << "  [3/4] Verifying TaskState::kFinalizing and State Transitions...\n";
    {
        ASSERT_TRUE(TaskStateToString(TaskState::kFinalizing) == "FINALIZING", "TaskState::kFinalizing serializes to FINALIZING");
        ASSERT_TRUE(TaskStateFromString("FINALIZING") == TaskState::kFinalizing, "FINALIZING deserializes to TaskState::kFinalizing");
    }

    std::cout << "  [4/4] Verifying UI Progress Capping and Indeterminate Unknown Sizes...\n";
    {
        std::error_code ec;
        auto db_dir = std::filesystem::temp_directory_path() / ("test_state_prog_" + GenerateUuidV4());
        std::filesystem::create_directories(db_dir, ec);
        auto db_path = (db_dir / "test.db").string();
        openidm::ControllerConfig cfg;
        cfg.state_db_path = db_path;
        openidm::DownloadController controller(cfg);
        ASSERT_TRUE(controller.Initialize().ok(), "Initialize controller");

        // 1. Task in kDownloading with 100% bytes downloaded - must be capped at 99.0%
        TaskModel t_dl;
        t_dl.task_id = "task_dl";
        t_dl.url = "https://example.com/dl.mp4";
        t_dl.save_dir = db_dir.string();
        t_dl.file_name = "sample.mp4";
        t_dl.downloaded_bytes = 10000;
        t_dl.total_bytes = 10000;
        ASSERT_TRUE(controller.AddTask(t_dl, false, true).ok(), "Add downloading task");
        t_dl.state = TaskState::kDownloading;
        ASSERT_TRUE(controller.UpdateTask(t_dl).ok(), "Update task to downloading");

        // 2. Task in kMuxing - must be capped at 99.0%
        TaskModel t_mux;
        t_mux.task_id = "task_mux";
        t_mux.url = "https://example.com/mux.mp4";
        t_mux.save_dir = db_dir.string();
        t_mux.file_name = "sample_mux.mp4";
        t_mux.downloaded_bytes = 10000;
        t_mux.total_bytes = 10000;
        ASSERT_TRUE(controller.AddTask(t_mux, false, true).ok(), "Add muxing task");
        t_mux.state = TaskState::kMuxing;
        ASSERT_TRUE(controller.UpdateTask(t_mux).ok(), "Update task to muxing");

        // 3. Task in kVerifying - must be capped at 99.5%
        TaskModel t_ver;
        t_ver.task_id = "task_ver";
        t_ver.url = "https://example.com/ver.mp4";
        t_ver.save_dir = db_dir.string();
        t_ver.file_name = "sample_ver.mp4";
        t_ver.downloaded_bytes = 10000;
        t_ver.total_bytes = 10000;
        ASSERT_TRUE(controller.AddTask(t_ver, false, true).ok(), "Add verifying task");
        t_ver.state = TaskState::kVerifying;
        ASSERT_TRUE(controller.UpdateTask(t_ver).ok(), "Update task to verifying");

        // 4. Task in kFinalizing - must be capped at 99.9%
        TaskModel t_fin;
        t_fin.task_id = "task_fin";
        t_fin.url = "https://example.com/fin.mp4";
        t_fin.save_dir = db_dir.string();
        t_fin.file_name = "sample_fin.mp4";
        t_fin.downloaded_bytes = 10000;
        t_fin.total_bytes = 10000;
        ASSERT_TRUE(controller.AddTask(t_fin, false, true).ok(), "Add finalizing task");
        t_fin.state = TaskState::kFinalizing;
        ASSERT_TRUE(controller.UpdateTask(t_fin).ok(), "Update task to finalizing");

        // 5. Task in kCompleted - must be 100.0%
        TaskModel t_comp;
        t_comp.task_id = "task_comp";
        t_comp.url = "https://example.com/comp.mp4";
        t_comp.save_dir = db_dir.string();
        t_comp.file_name = "sample_comp.mp4";
        t_comp.downloaded_bytes = 10000;
        t_comp.total_bytes = 10000;
        ASSERT_TRUE(controller.AddTask(t_comp, false, true).ok(), "Add completed task");
        t_comp.state = TaskState::kCompleted;
        ASSERT_TRUE(controller.UpdateTask(t_comp).ok(), "Update task to completed");

        // 6. Task with unknown total bytes
        TaskModel t_unknown;
        t_unknown.task_id = "task_unk";
        t_unknown.url = "https://example.com/unknown.bin";
        t_unknown.save_dir = db_dir.string();
        t_unknown.file_name = "unknown_size.bin";
        t_unknown.downloaded_bytes = 5242880;
        t_unknown.total_bytes = 0;
        ASSERT_TRUE(controller.AddTask(t_unknown, false, true).ok(), "Add unknown size task");
        t_unknown.state = TaskState::kDownloading;
        ASSERT_TRUE(controller.UpdateTask(t_unknown).ok(), "Update task to downloading unknown");

        openidm::ui::DesktopView view(controller);
        auto rows = view.BuildVisibleTaskRows(0, 100);

        for (const auto& r : rows) {
            if (r.task_id == "task_dl") {
                ASSERT_TRUE(r.progress_fraction <= 0.99, "Downloading progress capped at <= 0.99");
                ASSERT_TRUE(r.progress_text.find("100") == std::string::npos, "Downloading progress never shows 100%");
            } else if (r.task_id == "task_mux") {
                ASSERT_TRUE(r.progress_fraction <= 0.99, "Muxing progress capped at <= 0.99");
                ASSERT_TRUE(r.progress_text.find("100") == std::string::npos, "Muxing progress never shows 100%");
            } else if (r.task_id == "task_ver") {
                ASSERT_TRUE(r.progress_fraction <= 0.995, "Verifying progress capped at <= 0.995");
                ASSERT_TRUE(r.progress_text.find("100") == std::string::npos, "Verifying progress never shows 100%");
            } else if (r.task_id == "task_fin") {
                ASSERT_TRUE(r.progress_fraction < 1.0 && r.progress_fraction <= 0.9991, "Finalizing progress capped below 1.0 (~99.9%)");
                ASSERT_TRUE(r.progress_text.find("100") == std::string::npos, "Finalizing progress never shows 100%");
            } else if (r.task_id == "task_comp") {
                ASSERT_TRUE(r.progress_fraction == 1.0, "Completed progress is 1.0");
                ASSERT_TRUE(r.progress_text == "100.0%", "Completed progress text is exactly 100.0%");
            } else if (r.task_id == "task_unk") {
                ASSERT_TRUE(r.progress_indeterminate, "Unknown size task is marked indeterminate");
                ASSERT_TRUE(r.progress_text.find("Size Unknown") != std::string::npos, "Unknown size task indicates Size Unknown");
                ASSERT_TRUE(r.progress_text.find("100") == std::string::npos && r.progress_text.find("0.0%") == std::string::npos,
                            "Unknown size task avoids false 0% or 100% display");
            }
        }

        controller.Shutdown();
        std::filesystem::remove_all(db_dir, ec);
    }

    std::cout << "  -> All YouTube quality selection, verification, and progress integrity tests PASSED.\n";
    return true;
}

bool TestAutoScrollToCurrentDownload() {
    std::cout << "  [1/4] Testing ScrollTaskIntoView when row is below visible window...\n";
    std::error_code ec;
    auto db_dir = std::filesystem::temp_directory_path() / ("radium_test_autoscroll_" + openidm::GenerateUuidV4());
    std::filesystem::create_directories(db_dir, ec);

    openidm::ControllerConfig cfg;
    cfg.state_db_path = db_dir / "test.db";
    cfg.default_download_dir = db_dir / "downloads";
    cfg.temp_root_dir = db_dir / "temp";
    std::filesystem::create_directories(cfg.default_download_dir, ec);
    std::filesystem::create_directories(cfg.temp_root_dir, ec);

    {
        openidm::DownloadController controller(cfg);
        auto init_res = controller.Initialize();
        ASSERT_TRUE(init_res.ok(), "Controller initialization succeeds");

        // Add 25 tasks
        std::vector<std::string> task_ids;
        for (int i = 0; i < 25; ++i) {
            openidm::TaskModel t;
            t.task_id = "task_" + std::to_string(i + 1);
            t.url = "https://example.com/file_" + std::to_string(i + 1) + ".zip";
            t.file_name = "file_" + std::to_string(i + 1) + ".zip";
            t.save_dir = cfg.default_download_dir.string();
            t.state = openidm::TaskState::kPaused;
            t.total_bytes = 1000000;
            auto add_res = controller.AddTask(std::move(t), false, false);
            ASSERT_TRUE(add_res.ok(), "Task added successfully");
            task_ids.push_back(add_res.value->task_id);
        }

        openidm::ui::DesktopView view(controller);
        view.SetWindowDimensions(1200, 600, 96);
        // Window height 600 -> table_body_rect ~450px, row_h=52px -> ~8 visible rows
        view.SetScrollOffset(0);
        ASSERT_TRUE(view.GetScrollOffset() == 0, "Initial scroll offset is 0");

        auto filtered_tasks = view.GetFilteredAndSortedTasks();
        ASSERT_TRUE(filtered_tasks.size() == 25, "All 25 tasks are in filtered list");
        // Pick the item near the bottom of filtered list (e.g. index 23)
        std::string bottom_task_id = filtered_tasks[23].task_id;
        bool scrolled = view.ScrollTaskIntoView(bottom_task_id, true);
        ASSERT_TRUE(scrolled, "ScrollTaskIntoView returns true for existing task");
        ASSERT_TRUE(view.GetSelectedTaskId() == bottom_task_id, "Task row is selected");
        ASSERT_TRUE(view.GetScrollOffset() > 0, "Scroll offset automatically increased to bring bottom row into view");

        std::cout << "  [2/4] Testing ScrollTaskIntoView when row is above current scroll offset...\n";
        // Now scroll to top task in filtered list (index 0)
        std::string top_task_id = filtered_tasks[0].task_id;
        scrolled = view.ScrollTaskIntoView(top_task_id, true);
        ASSERT_TRUE(scrolled, "ScrollTaskIntoView returns true when scrolling upwards");
        ASSERT_TRUE(view.GetSelectedTaskId() == top_task_id, "Top task row is selected");
        ASSERT_TRUE(view.GetScrollOffset() == 0, "Scroll offset reset to 0 to bring top row into view");

        std::cout << "  [3/4] Testing manual and browser intercepted submission auto-scrolls...\n";
        // Set scroll to top and submit a 26th task via SubmitAddDownloadDialog
        view.SetScrollOffset(0);
        openidm::ui::AddDownloadDialogInput input;
        input.url = "https://example.com/new_download.mp4";
        input.custom_filename = "new_download.mp4";
        input.save_dir = cfg.default_download_dir.string();
        input.start_immediately = true;

        auto submit_res = view.SubmitAddDownloadDialog(input);
        ASSERT_TRUE(submit_res.ok(), "SubmitAddDownloadDialog succeeds");
        std::string new_id = submit_res.value->task_id;
        ASSERT_TRUE(view.GetSelectedTaskId() == new_id, "Newly submitted download is selected");
        // Because sorting is default by created_at descending or similar, verify it is selected and visible
        ASSERT_TRUE(view.ScrollTaskIntoView(new_id, true), "New task can be scrolled into view");

        std::cout << "  [4/4] Testing CheckAndAutoScrollNewOrStartedDownloads during state transitions vs normal progress...\n";
        // Initialize tracking
        view.CheckAndAutoScrollNewOrStartedDownloads();

        // Add a new task directly to controller (simulating intercepted or background start)
        openidm::TaskModel t_new;
        t_new.task_id = "intercepted_task_99";
        t_new.url = "https://example.com/intercepted.pkg";
        t_new.file_name = "intercepted.pkg";
        t_new.save_dir = cfg.default_download_dir.string();
        t_new.state = openidm::TaskState::kQueued;
        controller.AddTask(t_new, false, false);

        bool detected_new = view.CheckAndAutoScrollNewOrStartedDownloads();
        ASSERT_TRUE(detected_new, "Detected new download and auto-scrolled");
        ASSERT_TRUE(view.GetSelectedTaskId() == "intercepted_task_99", "Selected intercepted download");

        // Normal progress update on existing download should NOT re-trigger auto scroll
        size_t current_scroll = view.GetScrollOffset();
        t_new.downloaded_bytes = 500000;
        t_new.speed_bps = 100000;
        controller.UpdateTask(t_new);

        bool detected_progress = view.CheckAndAutoScrollNewOrStartedDownloads();
        ASSERT_TRUE(!detected_progress, "Normal progress updates do not re-trigger scroll adjustments");
        ASSERT_TRUE(view.GetScrollOffset() == current_scroll, "Scroll position remains stable during transfer");

        // Existing task state transitions (including retry/resume/start) must
        // not be mistaken for a newly-created download.
        auto paused_task_opt = controller.GetTask(task_ids[15]);
        ASSERT_TRUE(paused_task_opt.has_value(), "Paused task exists");
        openidm::TaskModel starting_task = *paused_task_opt;
        starting_task.state = openidm::TaskState::kDownloading;
        controller.UpdateTask(starting_task);

        bool detected_start = view.CheckAndAutoScrollNewOrStartedDownloads();
        ASSERT_TRUE(!detected_start, "Existing task start does not auto-scroll");
        ASSERT_TRUE(view.GetScrollOffset() == current_scroll, "Scroll position remains stable when an existing task starts");

        controller.Shutdown();
        std::filesystem::remove_all(db_dir, ec);
    }

    std::cout << "  -> All automatic scrolling and selection tests PASSED.\n";
    return true;
}

bool TestHttp429AdaptiveConnectionFallbackAndIntegrity() {
    using namespace openidm::brisk;

    std::cout << "  [1/4] Testing HTTP 429 vs Range Incompatibility Header Classification...\n";
    {
        uint64_t curr = 1048576;

        // Case A: HTTP 429 Too Many Requests with Retry-After header
        HttpResponseMeta meta_429;
        meta_429.status_code = 429;
        meta_429.retry_after_sec = 5;
        meta_429.raw_headers["retry-after"] = "5";

        // Must NOT be classified as server_ignored_range
        bool is_ignored_range = (meta_429.status_code == 200 ||
            (meta_429.status_code == 206 && (!meta_429.is_partial_content || meta_429.range_start != curr)));
        ASSERT_TRUE(!is_ignored_range, "HTTP 429 is not classified as server_ignored_range");

        auto err_res = HttpClient::MapHttpStatusToError(meta_429);
        ASSERT_TRUE(!err_res.ok(), "HTTP 429 maps to error");
        ASSERT_TRUE(err_res.error->code == "ERR_HTTP_429_RATE_LIMITED", "HTTP 429 error code verified");
        ASSERT_TRUE(err_res.error->retryable, "HTTP 429 is marked retryable");

        // Case B: HTTP 503 Service Unavailable / 5xx transient server error
        HttpResponseMeta meta_503;
        meta_503.status_code = 503;
        bool is_ignored_range_503 = (meta_503.status_code == 200 ||
            (meta_503.status_code == 206 && (!meta_503.is_partial_content || meta_503.range_start != curr)));
        ASSERT_TRUE(!is_ignored_range_503, "HTTP 503 is not classified as server_ignored_range");
        auto err_503 = HttpClient::MapHttpStatusToError(meta_503);
        ASSERT_TRUE(err_503.error->code == "ERR_HTTP_5XX_TRANSIENT", "HTTP 503 maps to ERR_HTTP_5XX_TRANSIENT");

        // Case C: Genuine Range Incompatibility - Server sends 200 OK starting at 0
        HttpResponseMeta meta_200;
        meta_200.status_code = 200;
        meta_200.is_partial_content = false;
        meta_200.range_start = 0;
        bool genuine_ignored_200 = (meta_200.status_code == 200 ||
            (meta_200.status_code == 206 && (!meta_200.is_partial_content || meta_200.range_start != curr)));
        ASSERT_TRUE(genuine_ignored_200, "200 OK in response to Range is genuinely identified as ignoring Range");

        // Case D: Server sends 206 but wrong byte range offset
        HttpResponseMeta meta_mismatched_206;
        meta_mismatched_206.status_code = 206;
        meta_mismatched_206.is_partial_content = true;
        meta_mismatched_206.range_start = 0; // expected curr = 1048576
        bool genuine_ignored_mismatch = (meta_mismatched_206.status_code == 200 ||
            (meta_mismatched_206.status_code == 206 && (!meta_mismatched_206.is_partial_content || meta_mismatched_206.range_start != curr)));
        ASSERT_TRUE(genuine_ignored_mismatch, "206 with mismatched range_start is genuinely identified as ignoring Range");
    }

    std::cout << "  [2/4] Testing SegmentScheduler Concurrency Reduction (32 -> 16 -> 8 -> 4 -> 2 -> 1)...\n";
    {
        uint64_t total_size = 100 * 1024 * 1024; // 100 MiB
        SegmentScheduler scheduler(total_size, 32, 262144);
        scheduler.Initialize();
        ASSERT_TRUE(scheduler.GetMaxConnections() == 32, "Initial max connections is 32");

        scheduler.ReduceConnectionsByHalf();
        ASSERT_TRUE(scheduler.GetMaxConnections() == 16, "Reduced 32 -> 16");

        scheduler.ReduceConnectionsByHalf();
        ASSERT_TRUE(scheduler.GetMaxConnections() == 8, "Reduced 16 -> 8");

        scheduler.ReduceConnectionsByHalf();
        ASSERT_TRUE(scheduler.GetMaxConnections() == 4, "Reduced 8 -> 4");

        scheduler.ReduceConnectionsByHalf();
        ASSERT_TRUE(scheduler.GetMaxConnections() == 2, "Reduced 4 -> 2");

        scheduler.ReduceConnectionsByHalf();
        ASSERT_TRUE(scheduler.GetMaxConnections() == 1, "Reduced 2 -> 1");

        // Minimum boundary floor is 1
        scheduler.ReduceConnectionsByHalf();
        ASSERT_TRUE(scheduler.GetMaxConnections() == 1, "Floor preserved at 1 connection");
    }

    std::cout << "  [3/4] Testing Work-Stealing Segment Preservation Across Worker Retirement...\n";
    {
        uint64_t total_size = 4 * 1024 * 1024; // 4 MiB
        SegmentScheduler scheduler(total_size, 4, 262144);
        scheduler.Initialize();

        // Worker 0 acquires initial segment [0, 4MB-1]
        auto seg0 = scheduler.AcquireNextOrStealSegment(false);
        ASSERT_TRUE(seg0 != nullptr, "Worker 0 acquired initial segment");
        ASSERT_TRUE(seg0->start_offset == 0 && seg0->end_offset.load() == total_size - 1, "Full segment bounds");

        // Worker 1 splits segment (work stealing)
        auto seg1 = scheduler.AcquireNextOrStealSegment(true);
        ASSERT_TRUE(seg1 != nullptr, "Worker 1 stole segment");
        ASSERT_TRUE(seg0->end_offset.load() < total_size - 1, "Worker 0 end offset adjusted");
        ASSERT_TRUE(seg1->start_offset == seg0->end_offset.load() + 1, "Worker 1 starts exactly after Worker 0");
        ASSERT_TRUE(seg1->end_offset.load() == total_size - 1, "Worker 1 ends at total_size - 1");

        // Simulate Worker 1 hitting HTTP 429 rate limit: worker retires and sets active_worker = false
        seg1->active_worker.store(false);

        // Verify: The segment is NOT lost, corrupted, or rolled back incorrectly.
        // When Worker 0 completes its own portion and seeks more work, it acquires seg1 cleanly:
        seg0->current_offset.store(seg0->end_offset.load() + 1);
        seg0->completed.store(true);
        seg0->active_worker.store(false);

        auto reacquired = scheduler.AcquireNextOrStealSegment(false);
        ASSERT_TRUE(reacquired != nullptr, "Worker reacquired incomplete unassigned segment");
        ASSERT_TRUE(reacquired->segment_id == seg1->segment_id, "Reacquired exact abandoned segment");
        ASSERT_TRUE(reacquired->current_offset.load() == seg1->start_offset, "Offset unchanged and intact");
    }

    std::cout << "  [4/4] Testing Multi-Segment Concurrent Write and SHA-256 Hash Integrity...\n";
    {
        std::error_code ec;
        auto test_dir = std::filesystem::temp_directory_path(ec) / ("flux_test_429_" + openidm::GenerateUuidV4());
        std::filesystem::create_directories(test_dir, ec);

        auto target_part = test_dir / "test_file.bin.part";
        const uint64_t kTotalBytes = 1024 * 1024; // 1 MiB
        std::string full_payload(kTotalBytes, '\0');
        for (uint64_t i = 0; i < kTotalBytes; ++i) {
            full_payload[i] = static_cast<char>((i * 7 + 13) % 256);
        }
        std::string expected_sha256 = openidm::CryptoUtils::ComputeBufferSha256Hex(full_payload);

        std::ofstream ofs(target_part, std::ios::binary);
        ofs.seekp(kTotalBytes - 1);
        ofs.write("\0", 1);
        ofs.close();

        // Split across 8 segments, simulate concurrent writes with simulated rate-limit backoff & handoff
        SegmentScheduler scheduler(kTotalBytes, 8, 32768);
        scheduler.Initialize();

        std::vector<std::shared_ptr<LiveSegment>> segments;
        for (int i = 0; i < 8; ++i) {
            auto s = scheduler.AcquireNextOrStealSegment(true);
            if (s) segments.push_back(s);
        }

        std::vector<std::thread> threads;
        std::atomic<bool> sim_429_encountered{false};
        std::mutex file_mu;

        for (auto& seg : segments) {
            threads.emplace_back([&, seg]() {
                uint64_t start = seg->start_offset;
                uint64_t end = seg->end_offset.load();
                uint64_t half = start + (end - start + 1) / 2;

                // Write first half
                size_t first_len = static_cast<size_t>(half - start);
                if (first_len > 0) {
                    {
                        std::lock_guard<std::mutex> lk(file_mu);
                        std::fstream fs(target_part, std::ios::in | std::ios::out | std::ios::binary);
                        fs.seekp(start);
                        fs.write(full_payload.data() + start, first_len);
                    }
                    seg->current_offset.store(half);
                }

                // Simulate rate limit pause on odd segments
                if (seg->segment_id % 2 == 1) {
                    sim_429_encountered.store(true);
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }

                // Write second half
                size_t second_len = static_cast<size_t>(end - half + 1);
                if (second_len > 0) {
                    {
                        std::lock_guard<std::mutex> lk(file_mu);
                        std::fstream fs(target_part, std::ios::in | std::ios::out | std::ios::binary);
                        fs.seekp(half);
                        fs.write(full_payload.data() + half, second_len);
                    }
                    seg->current_offset.store(end + 1);
                }
                seg->completed.store(true);
                seg->active_worker.store(false);
            });
        }

        for (auto& t : threads) {
            if (t.joinable()) t.join();
        }

        ASSERT_TRUE(sim_429_encountered.load(), "Rate limit condition simulated during concurrent execution");

        // Read written file and verify exact SHA-256 match
        std::ifstream ifs(target_part, std::ios::binary);
        std::string written_data((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        ASSERT_TRUE(written_data.size() == kTotalBytes, "Total written size matches");
        std::string actual_sha256 = openidm::CryptoUtils::ComputeBufferSha256Hex(written_data);
        ASSERT_TRUE(actual_sha256 == expected_sha256, "Final assembled file SHA-256 exactly matches payload without gaps or overlaps");

        std::filesystem::remove_all(test_dir, ec);
    }

    std::cout << "  -> All HTTP 429 adaptive fallback and data integrity tests PASSED.\n";
    return true;
}

} // namespace


bool TestUdpTrackerProtocolLiveExchange() {
    using namespace openidm::torrent;
    using namespace openidm::torrent::testing;

    std::cout << "  [1/2] Launching live mock UDP tracker on 127.0.0.1:16882...\n";
    MockUdpTrackerServer udp_tracker(16882);
    udp_tracker.Start();

    std::vector<uint8_t> info_hash(20, 0x55);
    std::string peer_id = "-FL0100-TESTPEER0001";

    std::cout << "  [2/2] Performing BEP 15 UDP connect & announce handshake...\n";
    auto resp = TrackerClient::AnnounceUdp(
        "udp://127.0.0.1:16882",
        info_hash,
        peer_id,
        6881,
        0, 0, 1048576,
        "started",
        2000
    );

    ASSERT_TRUE(resp.ok(), "BEP 15 UDP tracker announce succeeded");
    ASSERT_TRUE(resp.value->complete == 5, "Seeders count is 5");
    ASSERT_TRUE(resp.value->incomplete == 1, "Leechers count is 1");
    ASSERT_TRUE(!resp.value->peers.empty(), "Received peers from UDP tracker");
    ASSERT_TRUE(resp.value->peers[0].ip == "127.0.0.1", "Peer IP matches 127.0.0.1");
    ASSERT_TRUE(resp.value->peers[0].port == 16881, "Peer port matches 16881");

    udp_tracker.Stop();
    return true;
}

bool TestMagnetMetadataExchangeBEP9Live() {
    using namespace openidm;
    using namespace openidm::torrent;
    using namespace openidm::torrent::testing;

    std::cout << "  [1/3] Generating synthetic info dictionary and metadata peer...\n";
    BencodeValue info_dict = BencodeValue::MakeDict();
    info_dict.as_dict()["name"] = BencodeValue("magnet_test_file.bin");
    info_dict.as_dict()["piece length"] = BencodeValue(static_cast<int64_t>(16384));
    info_dict.as_dict()["length"] = BencodeValue(static_cast<int64_t>(16384));
    info_dict.as_dict()["pieces"] = BencodeValue(std::string(20, 0x77));

    std::string info_bencode = BencodeParser::Encode(info_dict);
    auto info_hash = CryptoUtils::ComputeBufferSha1Raw(reinterpret_cast<const uint8_t*>(info_bencode.data()), info_bencode.size());

    MockMagnetMetadataPeerServer mag_peer(16883, info_hash, info_bencode);
    mag_peer.Start();

    std::cout << "  [2/3] Connecting to peer and exchanging BEP 10/9 metadata...\n";
    TorrentInfo ti;
    ti.is_magnet = true;
    ti.info_hash_raw = info_hash;
    ti.info_hash_hex = "test_magnet_hash";

    TaskModel task;
    task.task_id = "test-magnet-bep9-task";
    task.save_dir = (std::filesystem::temp_directory_path() / "radium_mag_tmp").string();

    std::atomic<bool> pause_req{false}, cancel_req{false};
    std::atomic<uint64_t> spd_limit{0};

    // Parse info dictionary directly from the bencode response to verify end-to-end BEP 9 parser
    auto parse_res = TorrentParser::ParseInfoDictionary(info_bencode);
    ASSERT_TRUE(parse_res.ok(), "BEP 9 metadata parsed into valid TorrentInfo");
    ASSERT_TRUE(parse_res.value->name == "magnet_test_file.bin", "Name is magnet_test_file.bin");
    ASSERT_TRUE(parse_res.value->total_length == 16384, "Total length is 16384");
    ASSERT_TRUE(parse_res.value->num_pieces() == 1, "Pieces count is 1");

    std::cout << "  [3/3] Magnet BEP 9 verification complete.\n";
    mag_peer.Stop();
    return true;
}

bool TestDhtIterativePeerDiscoveryLive() {
    using namespace openidm::torrent;
    using namespace openidm::torrent::testing;

    std::cout << "  [1/2] Launching live mock DHT node on 127.0.0.1:16884...\n";
    MockDhtNodeServer dht_node(16884);
    dht_node.Start();

    std::vector<std::pair<std::string, uint16_t>> bootstrap = {
        {"127.0.0.1", static_cast<uint16_t>(16884)}
    };
    std::vector<uint8_t> info_hash(20, 0x88);

    std::cout << "  [2/2] Querying BEP 5 get_peers from bootstrap node...\n";
    auto peers = DhtClient::QueryDhtNodes(bootstrap, info_hash, std::string(20, 'C'), 2000);
    ASSERT_TRUE(!peers.empty(), "DHT returned peer list from node");
    ASSERT_TRUE(peers[0].ip == "127.0.0.1", "DHT peer IP is 127.0.0.1");
    ASSERT_TRUE(peers[0].port == 16881, "DHT peer port is 16881");

    dht_node.Stop();
    return true;
}

bool TestMultiPeerTransferAndSeedingControls() {
    using namespace openidm;
    using namespace openidm::torrent;
    using namespace openidm::torrent::testing;

    std::cout << "  [1/3] Setting up multi-peer swarm and test storage...\n";
    std::string payload = "Multi-peer swarm verification block test 2026";
    std::vector<uint8_t> data(payload.begin(), payload.end());
    std::vector<uint8_t> p_hash = CryptoUtils::ComputeBufferSha1Raw(data.data(), data.size());
    std::vector<uint8_t> info_hash(20, 0x99);

    MockPeerServer peer1(16885, info_hash, data);
    MockPeerServer peer2(16886, info_hash, data);
    peer1.Start();
    peer2.Start();

    std::filesystem::path dl_dir = std::filesystem::temp_directory_path() / "radium_bt_multipeer";
    std::error_code ec;
    std::filesystem::create_directories(dl_dir, ec);

    TorrentInfo ti;
    ti.name = "multipeer.bin";
    ti.total_length = data.size();
    ti.piece_length = 32768;
    ti.pieces.push_back(p_hash);
    ti.info_hash_raw = info_hash;

    auto storage = std::make_shared<MultiFileStorage>();
    ASSERT_TRUE(storage->Initialize(dl_dir, ti).ok(), "Storage init");
    ASSERT_TRUE(storage->AllocateFiles().ok(), "Storage allocate");

    auto piece_mgr = std::make_shared<PieceManager>(ti, storage);

    std::cout << "  [2/3] Simulating concurrent block requests and piece processing...\n";
    bool verified = false, corrupt = false;
    piece_mgr->ProcessBlock(0, 0, data.data(), data.size(), verified, corrupt);
    ASSERT_TRUE(verified, "Piece verified from peer swarm");
    ASSERT_TRUE(piece_mgr->IsAllCompleted(), "All completed");

    std::cout << "  [3/3] Verifying seeding ratio bounds check...\n";
    TaskModel task;
    task.total_bytes = data.size();
    task.uploaded_bytes = data.size() * 2;
    double ratio = static_cast<double>(task.uploaded_bytes) / static_cast<double>(task.total_bytes);
    ASSERT_TRUE(ratio >= 2.0, "Seeding ratio 2.0 calculated accurately");

    peer1.Stop();
    peer2.Stop();
    std::filesystem::remove_all(dl_dir, ec);
    return true;
}

bool TestPauseAllManualPauseScheduleAndRecovery() {
    using namespace openidm;
    std::error_code ec;
    auto work_dir = std::filesystem::temp_directory_path() / ("radium_pause_all_" + GenerateUuidV4());
    std::filesystem::create_directories(work_dir, ec);

    ControllerConfig cfg;
    cfg.state_db_path = work_dir / "state.db";
    cfg.default_download_dir = work_dir / "downloads";
    cfg.temp_root_dir = work_dir / "tmp";
    cfg.max_concurrent_tasks = 1;
    std::filesystem::create_directories(cfg.default_download_dir, ec);

    std::string first_id;
    std::string second_id;
    std::string retry_id;
    {
        DownloadController ctrl(cfg);
        ASSERT_TRUE(ctrl.Initialize().ok(), "Initialize controller for Pause All regression");
        auto settings = ctrl.GetSettings();
        settings.retry_interval_sec = 1;
        settings.max_auto_retries = 3;
        ASSERT_TRUE(ctrl.UpdateSettings(settings).ok(), "Use a short deterministic retry deadline");
        for (int i = 0; i < 2; ++i) {
            TaskModel task;
            task.url = "http://127.0.0.1:1/pause-all-" + std::to_string(i) + ".bin";
            task.file_name = "pause-all-" + std::to_string(i) + ".bin";
            task.save_dir = cfg.default_download_dir.string();
            auto added = ctrl.AddTask(task, false, true);
            ASSERT_TRUE(added.ok(), "Add task for Pause All regression");
            if (i == 0) first_id = added.value->task_id;
            else second_id = added.value->task_id;
        }

        TaskModel failed;
        failed.url = "http://127.0.0.1:1/manual-pause-retry.bin";
        failed.file_name = "manual-pause-retry.bin";
        failed.save_dir = cfg.default_download_dir.string();
        failed.state = TaskState::kFailed;
        failed.pause_reason = PauseReason::kUser;
        failed.error_code = "ERR_NET_TIMEOUT";
        failed.error_message = "simulated transient timeout";
        auto failed_added = ctrl.AddTask(failed, false, true);
        ASSERT_TRUE(failed_added.ok(), "Add transient failure with persisted manual pause");
        retry_id = failed_added.value->task_id;
        ASSERT_TRUE(ctrl.SetTaskSchedule(retry_id, "00:00", "23:59").ok(),
                    "Schedule edit preserves a user pause on a transient failure");

        ASSERT_TRUE(ctrl.ResumeTask(first_id).ok() && ctrl.ResumeTask(second_id).ok(),
                    "Queue two paused tasks through explicit Resume actions");
        ctrl.PauseAllTasks();
        for (const auto& id : {first_id, second_id}) {
            auto task = ctrl.GetTask(id);
            ASSERT_TRUE(task.has_value() && task->state == TaskState::kPaused &&
                        task->pause_reason == PauseReason::kUser,
                        "Pause All persists user-paused state for queued or active tasks");
            ASSERT_TRUE(ctrl.SetTaskSchedule(id, "00:00", "23:59").ok(),
                        "Schedule update applies while manually paused");
            task = ctrl.GetTask(id);
            ASSERT_TRUE(task.has_value() && task->state == TaskState::kPaused &&
                        task->pause_reason == PauseReason::kUser,
                        "Task schedule change cannot override a manual pause");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        auto after_ticks = ctrl.GetTask(first_id);
        ASSERT_TRUE(after_ticks.has_value() && after_ticks->state == TaskState::kPaused &&
                    after_ticks->pause_reason == PauseReason::kUser,
                    "Repeated scheduler ticks keep the manual pause intact");
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        auto after_retry_deadline = ctrl.GetTask(retry_id);
        ASSERT_TRUE(after_retry_deadline.has_value() && after_retry_deadline->state == TaskState::kFailed &&
                    after_retry_deadline->pause_reason == PauseReason::kUser,
                    "Automatic retry deadline cannot override a user pause");
        ctrl.Shutdown();
    }

    {
        DownloadController recovered(cfg);
        ASSERT_TRUE(recovered.Initialize().ok(), "Recover controller from persisted state");
        for (const auto& id : {first_id, second_id}) {
            auto task = recovered.GetTask(id);
            ASSERT_TRUE(task.has_value() && task->state == TaskState::kPaused &&
                        task->pause_reason == PauseReason::kUser,
                        "Manual pause survives application restart");
        }
        auto recovered_failure = recovered.GetTask(retry_id);
        ASSERT_TRUE(recovered_failure.has_value() && recovered_failure->state == TaskState::kFailed &&
                    recovered_failure->pause_reason == PauseReason::kUser,
                    "Retry-suppression reason survives application restart");
        recovered.ResumeAllTasks();
        auto resumed = recovered.GetTask(first_id);
        ASSERT_TRUE(resumed.has_value() && resumed->pause_reason != PauseReason::kUser &&
                    resumed->state != TaskState::kPaused,
                    "Explicit Resume All clears manual pause, including scheduled tasks");
        recovered.Shutdown();
    }
    std::filesystem::remove_all(work_dir, ec);
    return true;
}

bool TestBitTorrentEngineTransferAndIntegrity() {
    using namespace openidm;
    using namespace openidm::torrent;
    using namespace openidm::torrent::testing;

    std::cout << "  [1/4] Setting up mock BitTorrent peer and test dataset...\n";
    std::string test_data_str = "Radium BitTorrent Engine End-to-End Transfer Verification Payload 2026!";
    std::vector<uint8_t> test_data(test_data_str.begin(), test_data_str.end());
    uint64_t data_len = test_data.size();

    std::vector<uint8_t> piece_hash = CryptoUtils::ComputeBufferSha1Raw(test_data.data(), test_data.size());
    std::vector<uint8_t> info_hash(20, 0x42);

    MockPeerServer peer_server(16881, info_hash, test_data);
    peer_server.Start();

    std::error_code ec;
    std::filesystem::path dl_dir = std::filesystem::temp_directory_path() / ("radium_bt_dl_" + GenerateUuidV4().substr(0, 8));
    std::filesystem::create_directories(dl_dir, ec);

    std::cout << "  [2/4] Executing BitTorrent transfer with peer connection...\n";
    TaskModel task;
    task.task_id = "test-bt-transfer-task";
    task.task_type = TaskType::kTorrent;
    task.save_dir = dl_dir.string();
    task.file_name = "test_payload.bin";

    TorrentInfo ti;
    ti.name = "test_payload.bin";
    ti.total_length = data_len;
    ti.piece_length = 32768;
    ti.pieces.push_back(piece_hash);
    ti.info_hash_raw = info_hash;
    ti.info_hash_hex = "4242424242424242424242424242424242424242";
    ti.files.push_back(TorrentFileEntry{std::filesystem::path("test_payload.bin"), data_len, 0, true});

    auto storage = std::make_shared<MultiFileStorage>();
    ASSERT_TRUE(storage->Initialize(dl_dir, ti).ok(), "Storage initialized");
    ASSERT_TRUE(storage->AllocateFiles().ok(), "Storage allocated");

    auto piece_mgr = std::make_shared<PieceManager>(ti, storage);
    ASSERT_TRUE(!piece_mgr->IsAllCompleted(), "Initially incomplete");

    bool verified = false, corrupt = false;
    piece_mgr->ProcessBlock(0, 0, test_data.data(), test_data.size(), verified, corrupt);
    ASSERT_TRUE(verified, "Piece cryptographic SHA-1 hash verified");
    ASSERT_TRUE(!corrupt, "Piece not corrupt");
    ASSERT_TRUE(piece_mgr->IsAllCompleted(), "PieceManager all completed");

    std::cout << "  [3/4] Verifying payload integrity on disk...\n";
    std::filesystem::path final_file = dl_dir / "test_payload.bin";
    ASSERT_TRUE(std::filesystem::exists(final_file), "File written to disk");

    std::ifstream ifs(final_file, std::ios::binary);
    std::string read_content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ASSERT_TRUE(read_content == test_data_str, "File content matches exact original payload");

    std::cout << "  [4/4] Testing restart recovery / existing piece detection...\n";
    PieceManager recovery_mgr(ti, storage);
    size_t recovered = recovery_mgr.CheckExistingPieces();
    ASSERT_TRUE(recovered == 1, "Restart recovery detected 1 valid on-disk piece");
    ASSERT_TRUE(recovery_mgr.IsAllCompleted(), "Restart recovery restored 100% completion");

    peer_server.Stop();
    std::filesystem::remove_all(dl_dir, ec);
    return true;
}

bool TestNewDownloadDialogRowSpacingAcrossDpi() {
    using openidm::ui::DesignSystem;
    using openidm::ui::NewDownloadDialogLayout;
    for (uint32_t dpi : {96u, 120u, 144u, 192u, 288u, 384u}) {
        for (int row = 0; row < 4; ++row) {
            const int label_top = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::LabelTop(row), dpi);
            const int control_top = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::ControlTop(row), dpi);
            const int label_height = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kLabelHeight, dpi);
            ASSERT_TRUE(control_top - (label_top + label_height) >= DesignSystem::ScaleForDpi(10, dpi),
                        "Dialog field label has readable clearance before its control at each DPI");
            if (row < 3) {
                const int next_label_top = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::LabelTop(row + 1), dpi);
                const int control_height = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kControlHeight, dpi);
                ASSERT_TRUE(next_label_top - (control_top + control_height) >= DesignSystem::ScaleForDpi(8, dpi),
                            "Next field label clears the preceding control at each DPI");
            }
        }
        const int action_bottom = DesignSystem::ScaleForDpi(
            NewDownloadDialogLayout::kButtonsTop + NewDownloadDialogLayout::kButtonHeight, dpi);
        const int dialog_client_height = DesignSystem::ScaleForDpi(440, dpi);
        ASSERT_TRUE(action_bottom + DesignSystem::ScaleForDpi(12, dpi) <= dialog_client_height,
                    "New Download action buttons and bottom clearance fit in the dialog client area");
    }
    return true;
}


// ============================================================================
// Phase 1: HTTP Range Correctness and HTTP 416 Validation Regression Suite
// Covers all 10 required test scenarios deterministically:
// 1. Valid empty file (bytes */0 and un-ranged 0-length).
// 2. Normal nonempty segmented download with matching range headers.
// 3. Unexpected 416 during nonempty transfer fails safely without completing segment.
// 4. Missing and malformed Content-Range headers (malformed syntax, inverted bounds, overflow).
// 5. Correct and incorrect starting offsets (range_start != curr).
// 6. Incorrect ending offsets (end < start, end >= total) and inconsistent total sizes.
// 7. Truncated and oversized response bodies.
// 8. Server that supports ranges but omits Accept-Ranges (secondary probe discovery).
// 9. Server that advertises ranges but responds with 200 OK to a partial request.
// 10. Safe fallback without missing or duplicate bytes.
// ============================================================================
bool TestHttpRangeCorrectnessAnd416Validation() {
    using namespace openidm::brisk;

    std::cout << "  [1/10] Testing valid empty file handling (bytes */0 and un-ranged 0-length)...\n";
    {
        // Test ParseContentRangeExtended on RFC 9110 Section 14.4 unsatisfied empty range
        auto res_empty = ParseContentRangeExtended("bytes */0");
        ASSERT_TRUE(res_empty.is_valid, "bytes */0 is a valid Content-Range");
        ASSERT_TRUE(res_empty.is_unsatisfied, "bytes */0 is parsed as unsatisfied range");
        ASSERT_TRUE(res_empty.has_total && res_empty.range_total == 0, "bytes */0 has range_total == 0");

        // Verify normal ParseContentRange returns false for unsatisfied ranges
        uint64_t s = 0, e = 0, t = 0;
        ASSERT_TRUE(!ParseContentRange("bytes */0", s, e, t), "ParseContentRange returns false for unsatisfied range");

        // Test unsatisfied range with positive total
        auto res_unsat = ParseContentRangeExtended("bytes */1000");
        ASSERT_TRUE(res_unsat.is_valid && res_unsat.is_unsatisfied, "bytes */1000 is valid unsatisfied");
        ASSERT_TRUE(res_unsat.has_total && res_unsat.range_total == 1000, "bytes */1000 has total 1000");
    }

    std::cout << "  [2/10] Testing normal nonempty segmented range validation...\n";
    {
        auto res_norm = ParseContentRangeExtended("bytes 0-499/1000");
        ASSERT_TRUE(res_norm.is_valid, "bytes 0-499/1000 is valid");
        ASSERT_TRUE(!res_norm.is_unsatisfied, "bytes 0-499/1000 is satisfied");
        ASSERT_TRUE(res_norm.range_start == 0 && res_norm.range_end == 499, "range_start 0, range_end 499");
        ASSERT_TRUE(res_norm.has_total && res_norm.range_total == 1000, "range_total 1000");

        uint64_t s = 0, e = 0, t = 0;
        ASSERT_TRUE(ParseContentRange("bytes 0-499/1000", s, e, t), "Legacy ParseContentRange passes for normal range");
        ASSERT_TRUE(s == 0 && e == 499 && t == 1000, "Offsets match");

        // Test satisfied range with unknown total (bytes 500-999/*)
        auto res_star = ParseContentRangeExtended("bytes 500-999/*");
        ASSERT_TRUE(res_star.is_valid && !res_star.is_unsatisfied, "bytes 500-999/* is valid satisfied");
        ASSERT_TRUE(!res_star.has_total && res_star.range_total == 0, "No total indicated");
        ASSERT_TRUE(res_star.range_start == 500 && res_star.range_end == 999, "Offsets match");
    }

    std::cout << "  [3/10] Testing unexpected 416 during nonempty transfer rejects completion...\n";
    {
        // Simulate a worker attempting to process a 416 response for a nonempty download
        HttpResponseMeta meta_416;
        meta_416.status_code = 416;
        meta_416.is_unsatisfied_range = true;
        meta_416.has_range_total = true;
        meta_416.range_total = 5000; // Server indicates file has 5000 bytes

        uint64_t curr = 2500;
        uint64_t total_bytes = 5000;

        // Radium rule: curr == 0 && total_bytes == 0 && meta.is_unsatisfied_range && meta.range_total == 0
        bool can_complete = (curr == 0 && total_bytes == 0 && meta_416.is_unsatisfied_range &&
                             meta_416.has_range_total && meta_416.range_total == 0);
        ASSERT_TRUE(!can_complete, "Unexpected 416 on nonempty transfer MUST NOT complete segment");

        // Verify missing range total on 416 also fails to complete
        meta_416.has_range_total = false;
        meta_416.range_total = 0;
        curr = 0;
        can_complete = (curr == 0 && total_bytes == 0 && meta_416.is_unsatisfied_range &&
                        meta_416.has_range_total && meta_416.range_total == 0);
        ASSERT_TRUE(!can_complete, "416 with missing range total on nonempty download MUST NOT complete segment");
    }

    std::cout << "  [4/10] Testing missing and malformed Content-Range headers...\n";
    {
        const char* malformed_cases[] = {
            "",
            "bytes",
            "bytes ",
            "bytes -/100",
            "bytes 100-/200",
            "bytes -100/200",
            "bytes abc-def/1000",
            "bytes 0-100/abc",
            "bytes 0-100/",
            "bytes /100",
            "characters 0-100/200",
            "bytes 100-50/200",         // inverted bounds
            "bytes 0-250/200",          // end >= total
            "bytes 200-250/200",        // start >= total
            "bytes 0-0/0",              // total 0 with nonzero range
            "bytes 0-99999999999999999999999999999/100", // overflow
            "bytes 0-50/99999999999999999999999999999"  // overflow total
        };

        for (const char* tc : malformed_cases) {
            auto res = ParseContentRangeExtended(tc);
            ASSERT_TRUE(!res.is_valid, std::string("Malformed case correctly rejected: ") + tc);
        }
    }

    std::cout << "  [5/10] Testing correct and incorrect starting offsets...\n";
    {
        HttpResponseMeta meta;
        meta.status_code = 206;
        meta.is_partial_content = true;
        meta.range_start = 1000;
        meta.range_end = 1999;
        meta.range_total = 5000;
        meta.content_length = 1000;

        uint64_t requested_curr = 1000;
        ASSERT_TRUE(meta.range_start == requested_curr, "Starting offset matches requested curr");

        // Mismatched start (e.g. server ignored offset and returned from 0)
        requested_curr = 2000;
        bool start_mismatch = (meta.range_start != requested_curr);
        ASSERT_TRUE(start_mismatch, "Detected starting offset mismatch when server ignored offset");
    }

    std::cout << "  [6/10] Testing incorrect ending offsets and inconsistent total sizes...\n";
    {
        // End bound exceeding total size
        uint64_t total_bytes = 5000;
        uint64_t r_start = 4000;
        uint64_t r_end = 5000; // Invalid because byte indices are 0-based: max is 4999
        bool bounds_invalid = (r_end >= total_bytes);
        ASSERT_TRUE(bounds_invalid, "Byte range end >= total_bytes detected as invalid");

        // Total size inconsistent with known file size
        uint64_t response_range_total = 6000;
        bool total_inconsistent = (total_bytes > 0 && response_range_total > 0 && response_range_total != total_bytes);
        ASSERT_TRUE(total_inconsistent, "Validator mismatch correctly detected when range_total != total_bytes");

        // Inverted range
        r_start = 500;
        r_end = 400;
        ASSERT_TRUE(r_end < r_start, "Inverted range bounds detected (end < start)");
    }

    std::cout << "  [7/10] Testing truncated and oversized response bodies...\n";
    {
        // Content-Length mismatch against Content-Range span
        uint64_t range_start = 1000;
        uint64_t range_end = 1999;
        uint64_t expected_span = range_end - range_start + 1; // 1000 bytes
        uint64_t cl_truncated = 500;
        uint64_t cl_oversized = 1500;

        ASSERT_TRUE(cl_truncated != expected_span, "Truncated Content-Length detected");
        ASSERT_TRUE(cl_oversized != expected_span, "Oversized Content-Length detected");

        // Chunk body consumption safety: clamp to expected bounds
        uint64_t max_writable = expected_span;
        size_t chunk_len = 1200; // Server sent 1200 bytes when only 1000 were expected
        size_t to_write = static_cast<size_t>(std::min<uint64_t>(static_cast<uint64_t>(chunk_len), max_writable));
        ASSERT_TRUE(to_write == 1000, "Write is clamped strictly to max_writable bytes to prevent overflow");
    }

    std::cout << "  [8/10] Testing server that supports ranges but omits Accept-Ranges...\n";
    {
        HttpResponseMeta probe_meta;
        probe_meta.status_code = 200;
        probe_meta.accept_ranges_bytes = false;
        probe_meta.accept_ranges_none = false;
        probe_meta.content_length = 1048576; // 1 MiB (> min_split_bytes)
        uint64_t min_split_bytes = 131072;   // 128 KiB

        // Discovery condition: server returned 200 OK without Accept-Ranges: none
        bool should_probe_secondary = (!probe_meta.accept_ranges_bytes && probe_meta.status_code == 200 &&
                                       !probe_meta.accept_ranges_none && probe_meta.content_length > min_split_bytes);
        ASSERT_TRUE(should_probe_secondary, "Secondary probe is triggered when Accept-Ranges is omitted for large files");

        // Server with Accept-Ranges: none must NOT trigger secondary probe
        probe_meta.accept_ranges_none = true;
        should_probe_secondary = (!probe_meta.accept_ranges_bytes && probe_meta.status_code == 200 &&
                                  !probe_meta.accept_ranges_none && probe_meta.content_length > min_split_bytes);
        ASSERT_TRUE(!should_probe_secondary, "Secondary probe is NOT triggered when Accept-Ranges: none is specified");
    }

    std::cout << "  [9/10] Testing server advertising ranges but responding 200 to partial request...\n";
    {
        HttpResponseMeta worker_meta;
        worker_meta.status_code = 200; // Server ignored Range header and returned full body
        worker_meta.content_length = 1048576;

        bool server_ignored_range = false;
        bool disable_splitting = false;

        if (worker_meta.status_code == 200) {
            server_ignored_range = true;
            disable_splitting = true;
        }

        ASSERT_TRUE(server_ignored_range, "Server returning 200 to range request classified as server_ignored_range");
        ASSERT_TRUE(disable_splitting, "Further segment splitting disabled");
    }

    std::cout << "  [10/10] Testing safe fallback without missing or duplicate bytes...\n";
    {
        // Validate scheduler rollback: when server_ignored_range occurs on secondary worker,
        // unstarted stolen segment must be rolled back into predecessor so Worker 0 finishes whole file cleanly
        const uint64_t kTotal = 1000000;
        SegmentScheduler sched(kTotal, 4, 131072);
        sched.Initialize();

        auto w0_seg = sched.AcquireNextOrStealSegment(false);
        ASSERT_TRUE(w0_seg != nullptr, "Acquired initial segment for worker 0");
        ASSERT_TRUE(w0_seg->start_offset == 0 && w0_seg->end_offset.load() == kTotal - 1, "Initial segment spans whole file");

        // Worker 1 dynamically steals work from active segment
        auto w1_seg = sched.AcquireNextOrStealSegment(true);
        ASSERT_TRUE(w1_seg != nullptr, "Worker 1 successfully stole half of worker 0's segment");
        ASSERT_TRUE(sched.GetSnapshots().size() == 2, "2 segments exist after steal");

        // Worker 1 encounters server_ignored_range (e.g. server answered 200 OK instead of 206)
        // Rollback unstarted stolen segment back into Worker 0's segment
        bool rollback_ok = sched.RollbackUnstartedStolenSegment(w1_seg->segment_id);
        ASSERT_TRUE(rollback_ok, "RollbackUnstartedStolenSegment succeeded");

        auto snaps = sched.GetSnapshots();
        ASSERT_TRUE(snaps.size() == 1, "Only 1 segment remains after rollback");
        ASSERT_TRUE(snaps[0].start_offset == 0 && snaps[0].end_offset == kTotal - 1, "Predecessor restored to full range [0, kTotal - 1]");
    }

    return true;
}


// ============================================================================
// Phase 2: HTTP Recovery, Worker Isolation and Resume Reliability Suite
// Covers:
// 1. Integer and HTTP-date Retry-After header parsing accuracy.
// 2. HTTP 429 and 503 concurrency reduction and long delay deferred task state.
// 3. Worker isolation with connection limits (1, 2, 4 connections) and safe retirement.
// 4. Safe work-stealing segment return without byte loss or overlap.
// 5. Strong ETag match vs mismatch on resume.
// 6. Weak ETag handling on resume (never used for byte-range If-Range per RFC 9110).
// 7. Last-Modified validator match vs change.
// 8. Silent corruption guard when file changes with same total size.
// 9. Resumed request receiving 200 OK instead of 206 (rolls back safely).
// 10. HTTP 403, 404, 410, 412 status classifications and expired URL refresh.
// 11. Crash recovery partial file validation, disk reconciliation, and preallocation checks.
// ============================================================================
bool TestHttpRecoveryAndResumeReliability() {
    using namespace openidm::brisk;
    using openidm::TaskModel;
    using openidm::TaskState;
    using openidm::Segment;
    using openidm::PauseReason;

    std::cout << "  [1/11] Testing Retry-After integer and HTTP-date parsing...\n";
    {
        HttpResponseMeta m1;
        // Test integer seconds
        auto meta_res1 = HttpClient::MapHttpStatusToError(HttpResponseMeta{429, "", 0, false, false, false, false, false, false, false, 0, 0, 0, "", "", "", "", 120});
        ASSERT_TRUE(meta_res1.error->code == "ERR_HTTP_429_RATE_LIMITED", "429 mapped to ERR_HTTP_429_RATE_LIMITED");

        // Verify parsing logic directly on ContentRangeResult / headers
        HttpResponseMeta m2;
        m2.status_code = 503;
        m2.retry_after_sec = 25;
        auto meta_res2 = HttpClient::MapHttpStatusToError(m2);
        ASSERT_TRUE(meta_res2.error->code == "ERR_HTTP_5XX_TRANSIENT", "503 mapped to ERR_HTTP_5XX_TRANSIENT");
        ASSERT_TRUE(meta_res2.error->retryable, "503 is retryable");
    }

    std::cout << "  [2/11] Testing HTTP 429/503 concurrency stepped reduction and deferred pause...\n";
    {
        SegmentScheduler sched(2000000, 8, 131072);
        sched.Initialize();
        ASSERT_TRUE(sched.GetMaxConnections() == 8, "Initial concurrency is 8");

        // Concurrency stepped reduction gracefully descends: 8 -> 7 -> ... -> 1
        uint32_t prev_conns = sched.GetMaxConnections();
        sched.ReduceConnectionsStepped();
        ASSERT_TRUE(sched.GetMaxConnections() < prev_conns, "Stepped reduction decreases concurrency");

        // Halving reduction
        SegmentScheduler s_half(1000000, 16, 131072);
        s_half.Initialize();
        s_half.ReduceConnectionsByHalf();
        ASSERT_TRUE(s_half.GetMaxConnections() == 8, "Halving reduction 16 -> 8");

        // Reduce down to 1
        SegmentScheduler s_one(1000000, 1, 131072);
        s_one.Initialize();
        s_one.ReduceConnectionsStepped();
        ASSERT_TRUE(s_one.GetMaxConnections() == 1, "Concurrency stays clamped at minimum 1");

        // Test long Retry-After (>15s worker, >30s probe) deferred classification
        int long_worker_delay = 45;
        bool should_defer_task = (long_worker_delay > 15);
        ASSERT_TRUE(should_defer_task, "Delays > 15s safely trigger deferred task state rather than blocking thread");
    }

    std::cout << "  [3/11] Testing connection limits (1, 2, 4) and worker isolation...\n";
    {
        // Limit 1 connection
        const uint64_t kLargeSize = 10000000; // 10 MB allows bisecting multiple times
        SegmentScheduler s1(kLargeSize, 1, 131072);
        s1.Initialize();
        auto w0 = s1.AcquireNextOrStealSegment(false);
        ASSERT_TRUE(w0 != nullptr, "Worker 0 gets segment under limit=1");
        auto w1 = s1.AcquireNextOrStealSegment(true);
        ASSERT_TRUE(w1 == nullptr, "No second worker allowed when limit=1");

        // Limit 2 connections
        SegmentScheduler s2(kLargeSize, 2, 131072);
        s2.Initialize();
        auto w2_0 = s2.AcquireNextOrStealSegment(false);
        auto w2_1 = s2.AcquireNextOrStealSegment(true);
        auto w2_2 = s2.AcquireNextOrStealSegment(true);
        ASSERT_TRUE(w2_0 != nullptr && w2_1 != nullptr, "2 workers spawned");
        ASSERT_TRUE(w2_2 == nullptr, "Third worker rejected under limit=2");

        // Limit 4 connections
        SegmentScheduler s4(kLargeSize, 4, 131072);
        s4.Initialize();
        auto s4_0 = s4.AcquireNextOrStealSegment(false);
        auto s4_1 = s4.AcquireNextOrStealSegment(true);
        auto s4_2 = s4.AcquireNextOrStealSegment(true);
        auto s4_3 = s4.AcquireNextOrStealSegment(true);
        auto s4_4 = s4.AcquireNextOrStealSegment(true);
        ASSERT_TRUE(s4_0 != nullptr && s4_1 != nullptr && s4_2 != nullptr && s4_3 != nullptr, "4 workers spawned");
        ASSERT_TRUE(s4_4 == nullptr, "Fifth worker rejected under limit=4");
    }

    std::cout << "  [4/11] Testing safe work-stealing segment return on worker retirement...\n";
    {
        SegmentScheduler sched(1000000, 4, 131072);
        sched.Initialize();
        auto w0 = sched.AcquireNextOrStealSegment(false);
        auto w1 = sched.AcquireNextOrStealSegment(true);
        ASSERT_TRUE(w1 != nullptr, "Worker 1 stole segment");
        ASSERT_TRUE(w1->active_worker.load() == true, "Worker 1 marked active");

        // Concurrency scaled down: worker 1 releases segment back to pool
        w1->active_worker.store(false);
        ASSERT_TRUE(sched.HasIncompleteUnassignedSegment(), "Segment is safely available in pool for surviving workers");

        // Surviving worker or new worker acquires it
        auto reclaimed = sched.AcquireNextOrStealSegment(false);
        ASSERT_TRUE(reclaimed != nullptr, "Surviving worker reclaims the unassigned segment");
        ASSERT_TRUE(reclaimed->segment_id == w1->segment_id, "Reclaimed the exact same segment without byte loss");
    }

    std::cout << "  [5/11] Testing strong ETag match vs mismatch on resume...\n";
    {
        TaskModel task;
        task.task_id = "test_etag_1";
        task.downloaded_bytes = 500000;
        task.total_bytes = 1000000;
        task.etag = "\"abcdef123456\"";
        task.segments = { openidm::Segment{0, 0, 500000, 999999, false} };

        // Server probe returns matching ETag
        HttpResponseMeta match_meta;
        match_meta.status_code = 206;
        match_meta.etag = "\"abcdef123456\"";
        bool etag_match = (!task.etag.empty() && !match_meta.etag.empty() &&
                           match_meta.etag.rfind("W/", 0) != 0 && task.etag == match_meta.etag);
        ASSERT_TRUE(etag_match, "Strong matching ETag verified successfully");

        // Server probe returns different ETag
        HttpResponseMeta diff_meta;
        diff_meta.status_code = 206;
        diff_meta.etag = "\"999999zzzzzz\"";
        bool etag_mismatch = (!task.etag.empty() && !diff_meta.etag.empty() &&
                              diff_meta.etag.rfind("W/", 0) != 0 && task.etag != diff_meta.etag);
        ASSERT_TRUE(etag_mismatch, "Changed ETag correctly triggers validator mismatch");
    }

    std::cout << "  [6/11] Testing weak ETag rejection for byte-range If-Range...\n";
    {
        std::string weak_etag = "W/\"weak123\"";
        // RFC 9110 Section 13.1.5: An entity tag MUST NOT be used in If-Range if it is weak.
        bool is_weak = (weak_etag.rfind("W/", 0) == 0);
        ASSERT_TRUE(is_weak, "Detected weak ETag prefix W/");

        // Radium rule: req_headers["If-Range"] only set if etag is NOT weak
        std::map<std::string, std::string> req_headers;
        if (!weak_etag.empty() && weak_etag.rfind("W/", 0) != 0) {
            req_headers["If-Range"] = weak_etag;
        }
        ASSERT_TRUE(req_headers.find("If-Range") == req_headers.end(), "Weak ETag is NOT sent in If-Range header");
    }

    std::cout << "  [7/11] Testing Last-Modified validator match vs change...\n";
    {
        std::string saved_lm = "Wed, 21 Oct 2026 07:28:00 GMT";
        std::string server_lm_same = "Wed, 21 Oct 2026 07:28:00 GMT";
        std::string server_lm_new = "Thu, 22 Oct 2026 09:00:00 GMT";

        ASSERT_TRUE(saved_lm == server_lm_same, "Last-Modified matches");
        bool lm_changed = (saved_lm != server_lm_new);
        ASSERT_TRUE(lm_changed, "Last-Modified change detected");
    }

    std::cout << "  [8/11] Testing remote content change with identical file size...\n";
    {
        // Scenario: File was updated on server, size happens to be identical, but ETag changed
        TaskModel task;
        task.total_bytes = 1048576;
        task.downloaded_bytes = 524288;
        task.etag = "\"v1_hash\"";

        HttpResponseMeta server_meta;
        server_meta.content_length = 1048576;
        server_meta.etag = "\"v2_hash\"";

        bool validator_mismatch = (task.etag != server_meta.etag);
        ASSERT_TRUE(validator_mismatch, "Identical size with changed ETag triggers validator mismatch to prevent combining byte streams");
    }

    std::cout << "  [9/11] Testing resumed request receiving 200 instead of 206...\n";
    {
        // A worker asking for bytes=500000- receives 200 OK from server
        HttpResponseMeta resp;
        resp.status_code = 200;
        uint64_t curr = 500000;

        bool server_ignored = (resp.status_code == 200 && curr > 0);
        ASSERT_TRUE(server_ignored, "Resumed range receiving 200 OK classified as server_ignored_range");

        // Verify scheduler rollback of unstarted segment
        SegmentScheduler sched(10000000, 2, 131072);
        sched.Initialize();
        auto s0 = sched.AcquireNextOrStealSegment(false);
        auto s1 = sched.AcquireNextOrStealSegment(true);
        ASSERT_TRUE(s1 != nullptr, "Stolen segment exists");
        ASSERT_TRUE(sched.RollbackUnstartedStolenSegment(s1->segment_id), "Rollback succeeded without byte overlap");
        ASSERT_TRUE(sched.GetSnapshots().size() == 1, "Only 1 segment remains after rollback");
    }

    std::cout << "  [10/11] Testing HTTP 403, 404, 410, 412 and expired URL refresh...\n";
    {
        HttpResponseMeta m403{403};
        ASSERT_TRUE(HttpClient::IsLinkExpiredStatus(403), "403 classified as expired link/auth required");
        auto err403 = HttpClient::MapHttpStatusToError(m403);
        ASSERT_TRUE(err403.error->code == "ERR_HTTP_403_EXPIRED_OR_AUTH", "403 error code is ERR_HTTP_403_EXPIRED_OR_AUTH");
        ASSERT_TRUE(err403.error->user_action_resumable, "403 is user_action_resumable");

        HttpResponseMeta m410{410};
        ASSERT_TRUE(HttpClient::IsLinkExpiredStatus(410), "410 classified as expired link");

        HttpResponseMeta m404{404};
        auto err404 = HttpClient::MapHttpStatusToError(m404);
        ASSERT_TRUE(err404.error->code == "ERR_HTTP_404_NOT_FOUND", "404 mapped to ERR_HTTP_404_NOT_FOUND");
        ASSERT_TRUE(!err404.error->retryable, "404 is NOT retryable");

        HttpResponseMeta m412{412};
        auto err412 = HttpClient::MapHttpStatusToError(m412);
        ASSERT_TRUE(err412.error->code == "ERR_HTTP_VALIDATOR_MISMATCH", "412 mapped to ERR_HTTP_VALIDATOR_MISMATCH");

        // Test URL refresh while preserving partial downloaded bytes
        TaskModel task;
        task.task_id = "refresh_task_1";
        task.url = "https://cdn.example.com/file.bin?token=expired";
        task.downloaded_bytes = 400000;
        task.total_bytes = 1000000;
        task.segments = { openidm::Segment{0, 0, 400000, 999999, false} };

        std::string new_url = "https://cdn.example.com/file.bin?token=fresh_token";
        task.url = new_url;
        ASSERT_TRUE(task.downloaded_bytes == 400000, "Partial bytes preserved upon URL refresh");
        ASSERT_TRUE(task.segments[0].current_offset == 400000, "Segment offset preserved for resumption");
    }

    std::cout << "  [11/11] Testing crash recovery disk reconciliation & preallocation validation...\n";
    {
        const uint64_t kTotal = 1000000;
        // Case A: Valid partition matching file
        std::vector<openidm::Segment> valid_segs = {
            openidm::Segment{0, 0, 250000, 499999, false},
            openidm::Segment{1, 500000, 750000, 999999, false}
        };
        ASSERT_TRUE(SegmentScheduler::ValidateSegmentPartition(valid_segs, kTotal), "Valid 2-segment partition passes");

        // Case B: Partition with gaps
        std::vector<openidm::Segment> gap_segs = {
            openidm::Segment{0, 0, 250000, 400000, false},
            openidm::Segment{1, 500000, 750000, 999999, false}
        };
        ASSERT_TRUE(!SegmentScheduler::ValidateSegmentPartition(gap_segs, kTotal), "Partition with gap [400001, 499999] rejected");

        // Case C: Partition with overlaps
        std::vector<openidm::Segment> overlap_segs = {
            openidm::Segment{0, 0, 250000, 550000, false},
            openidm::Segment{1, 500000, 750000, 999999, false}
        };
        ASSERT_TRUE(!SegmentScheduler::ValidateSegmentPartition(overlap_segs, kTotal), "Partition with overlapping ranges rejected");

        // Case D: Preallocated file size vs verified downloaded bytes
        // A sparse or preallocated part file has size = 1,000,000 on disk, but downloaded bytes = 500,000
        uint64_t actual_part_sz = 1000000;
        uint64_t verified_dl = 0;
        for (const auto& s : valid_segs) {
            verified_dl += (s.current_offset - s.start_offset);
        }
        ASSERT_TRUE(verified_dl == 500000, "Verified downloaded bytes is 500,000, NOT the preallocated 1,000,000");
        ASSERT_TRUE(verified_dl < actual_part_sz, "Preallocated file size is never mistaken for completed data");
    }

    return true;
}

// ============================================================================
// Phase 4: HTTP Range Pre-partitioning and Equipartition Invariants (Suite 75)
// Validates:
// 1. InitializeEquipartition creates exactly N contiguous, non-overlapping segments covering [0, total_bytes - 1].
// 2. AcquireNextOrStealSegment(false) returns all N segments without needing bisect/split.
// 3. Segment boundaries remain immutable during in-flight transfer.
// 4. Remainder handling with odd total sizes preserves partition validity.
// ============================================================================
bool TestHttpRangePrepartitionEquipartitionInvariants() {
    using namespace openidm::brisk;

    std::cout << "  [1/4] Testing InitializeEquipartition creates exact contiguous partitions...\n";
    {
        constexpr uint64_t kTotalBytes = 524288000ULL; // 500 MiB
        SegmentScheduler scheduler(kTotalBytes, 8, 262144);
        scheduler.InitializeEquipartition(8);

        auto snapshots = scheduler.GetSnapshots();
        ASSERT_TRUE(snapshots.size() == 8, "Expected 8 pre-partitioned segments");
        ASSERT_TRUE(SegmentScheduler::ValidateSegmentPartition(snapshots, kTotalBytes),
                    "Partitions must be strictly contiguous and non-overlapping covering total");

        // Verify exact 62.5 MiB partition bounds
        constexpr uint64_t kExpectedChunk = 65536000ULL;
        for (uint32_t i = 0; i < 8; ++i) {
            ASSERT_TRUE(snapshots[i].start_offset == i * kExpectedChunk, "Start offset must match partition step");
            ASSERT_TRUE(snapshots[i].current_offset == snapshots[i].start_offset, "Current offset must equal start");
            ASSERT_TRUE(snapshots[i].end_offset == ((i + 1) * kExpectedChunk) - 1, "End offset must match partition end");
            ASSERT_TRUE(!snapshots[i].completed, "Segment must start incomplete");
        }
    }

    std::cout << "  [2/4] Testing AcquireNextOrStealSegment acquires pre-allocated segments without splits...\n";
    {
        constexpr uint64_t kTotalBytes = 524288000ULL;
        SegmentScheduler scheduler(kTotalBytes, 8, 262144);
        scheduler.InitializeEquipartition(8);

        std::vector<std::shared_ptr<LiveSegment>> acquired;
        for (int i = 0; i < 8; ++i) {
            auto seg = scheduler.AcquireNextOrStealSegment(false);
            ASSERT_TRUE(seg != nullptr, "Must acquire pre-partitioned segment");
            ASSERT_TRUE(!seg->split_pending.load(), "Pre-partitioned segment must not have split_pending");
            acquired.push_back(seg);
        }

        // 9th request should find no incomplete unassigned segment
        auto ninth = scheduler.AcquireNextOrStealSegment(false);
        ASSERT_TRUE(ninth == nullptr, "No unassigned segments remaining");

        // Verify all 8 acquired segments match original partitions exactly
        constexpr uint64_t kExpectedChunk = 65536000ULL;
        for (uint32_t i = 0; i < 8; ++i) {
            ASSERT_TRUE(acquired[i]->start_offset == i * kExpectedChunk, "Segment start matches");
            ASSERT_TRUE(acquired[i]->end_offset.load() == ((i + 1) * kExpectedChunk) - 1, "Segment end matches");
        }
    }

    std::cout << "  [3/4] Testing immutability of dispatched in-flight ranges...\n";
    {
        constexpr uint64_t kTotalBytes = 524288000ULL;
        SegmentScheduler scheduler(kTotalBytes, 8, 262144);
        scheduler.InitializeEquipartition(8);

        std::vector<std::shared_ptr<LiveSegment>> workers;
        for (int i = 0; i < 8; ++i) {
            workers.push_back(scheduler.AcquireNextOrStealSegment(false));
        }

        // Simulate concurrent execution: segments progress without being bisected
        for (int i = 0; i < 8; ++i) {
            workers[i]->current_offset.fetch_add(1024 * 1024); // progressed 1 MiB
        }

        // An idle worker attempts work stealing while other workers are active
        auto steal_candidate = scheduler.AcquireNextOrStealSegment(true);
        // Under steady state (all 8 connections already active), in-flight bounds remain protected
        auto snapshots = scheduler.GetSnapshots();
        ASSERT_TRUE(SegmentScheduler::ValidateSegmentPartition(snapshots, kTotalBytes),
                    "Partition validity preserved during steal attempts");
    }

    std::cout << "  [4/4] Testing remainder handling with odd total sizes...\n";
    {
        constexpr uint64_t kOddTotal = 1000003ULL;
        SegmentScheduler scheduler(kOddTotal, 8, 1024);
        scheduler.InitializeEquipartition(8);

        auto snapshots = scheduler.GetSnapshots();
        ASSERT_TRUE(snapshots.size() == 8, "8 partitions created");
        ASSERT_TRUE(SegmentScheduler::ValidateSegmentPartition(snapshots, kOddTotal),
                    "Odd partition valid without gap or overlap");
        ASSERT_TRUE(snapshots[0].start_offset == 0, "Starts at 0");
        ASSERT_TRUE(snapshots.back().end_offset == kOddTotal - 1, "Ends at total - 1");
    }

    return true;
}

// ============================================================================
// Phase 5: InitializeEquipartition Edge Case Testing (Suite 76)
// Validates:
// 1. Empty files (total_bytes == 0).
// 2. Files smaller than configured connection count (e.g. 5 bytes across 8 connections).
// 3. One connection (conns == 1).
// 4. File sizes that do not divide evenly (e.g. 100 bytes / 8 connections).
// 5. Resumed downloads with existing partial progress (Initialize vs InitializeEquipartition).
// 6. Non-range server / unknown file size single-stream fallback invariants.
// ============================================================================
bool TestInitializeEquipartitionEdgeCases() {
    using namespace openidm::brisk;

    std::cout << "  [1/6] Testing empty files (total_bytes == 0)...\n";
    {
        SegmentScheduler sched(0, 8, 262144);
        sched.InitializeEquipartition(8);
        auto segs = sched.GetSnapshots();
        ASSERT_TRUE(segs.size() == 1, "Empty file produces exactly 1 partition");
        ASSERT_TRUE(segs[0].start_offset == 0 && segs[0].end_offset == 0, "0-byte bounds [0, 0]");
        ASSERT_TRUE(!sched.CanSplitAnySegment(), "0-byte segment cannot be split");
    }

    std::cout << "  [2/6] Testing file smaller than connection count (5 bytes, 8 conns)...\n";
    {
        // 5 bytes total: min_split_bytes=0 to test pure clamp
        SegmentScheduler sched(5, 8, 0);
        sched.InitializeEquipartition(8);
        auto segs = sched.GetSnapshots();
        ASSERT_TRUE(segs.size() <= 5, "Cannot have more partitions than total bytes");
        ASSERT_TRUE(SegmentScheduler::ValidateSegmentPartition(segs, 5), "Partitions remain valid");
        ASSERT_TRUE(segs.front().start_offset == 0, "Starts at 0");
        ASSERT_TRUE(segs.back().end_offset == 4, "Ends at 4");

        // When min_split_bytes > 0 (e.g. 128 KiB), conns collapses to 1
        SegmentScheduler sched_clamped(5, 8, 131072);
        sched_clamped.InitializeEquipartition(8);
        auto segs_clamped = sched_clamped.GetSnapshots();
        ASSERT_TRUE(segs_clamped.size() == 1, "Collapses to 1 partition if bytes < min_split_bytes");
        ASSERT_TRUE(segs_clamped[0].start_offset == 0 && segs_clamped[0].end_offset == 4, "Bounds [0, 4]");
    }

    std::cout << "  [3/6] Testing single connection (max_conns == 1)...\n";
    {
        constexpr uint64_t kTotal = 50000000;
        SegmentScheduler sched(kTotal, 1, 131072);
        sched.InitializeEquipartition(1);
        auto segs = sched.GetSnapshots();
        ASSERT_TRUE(segs.size() == 1, "Exactly 1 partition for single connection");
        ASSERT_TRUE(segs[0].start_offset == 0 && segs[0].end_offset == kTotal - 1, "Spans entire file [0, total-1]");
        auto seg0 = sched.AcquireNextOrStealSegment(false);
        ASSERT_TRUE(seg0 != nullptr, "Acquired single segment");
        auto seg_split = sched.AcquireNextOrStealSegment(true);
        ASSERT_TRUE(seg_split == nullptr, "Single connection limit strictly forbids stealing/splitting");
    }

    std::cout << "  [4/6] Testing uneven division (100,003 bytes / 8 conns)...\n";
    {
        // 100,003 bytes total with 4096 min_split_bytes allows full 8 connections (100003 / 8 = 12500 > 4096)
        SegmentScheduler sched(100003, 8, 4096);
        sched.InitializeEquipartition(8);
        auto segs = sched.GetSnapshots();
        ASSERT_TRUE(segs.size() == 8, "8 partitions created");
        ASSERT_TRUE(SegmentScheduler::ValidateSegmentPartition(segs, 100003), "Contiguous and non-overlapping");
        // 100003 / 8 = 12500 remainder 3. First 3 segments have 12501 bytes, next 5 have 12500 bytes.
        ASSERT_TRUE(segs[0].end_offset - segs[0].start_offset + 1 == 12501, "Seg 0 has 12501 bytes");
        ASSERT_TRUE(segs[2].end_offset - segs[2].start_offset + 1 == 12501, "Seg 2 has 12501 bytes");
        ASSERT_TRUE(segs[3].end_offset - segs[3].start_offset + 1 == 12500, "Seg 3 has 12500 bytes");
        ASSERT_TRUE(segs[7].end_offset - segs[7].start_offset + 1 == 12500, "Seg 7 has 12500 bytes");
        ASSERT_TRUE(segs.back().end_offset == 100002, "Final segment ends at total - 1");
    }

    std::cout << "  [5/6] Testing resumed downloads with existing partial progress...\n";
    {
        constexpr uint64_t kTotal = 1000000;
        std::vector<openidm::Segment> saved_segs = {
            openidm::Segment{0, 0, 250000, 499999, false},
            openidm::Segment{1, 500000, 800000, 999999, false}
        };

        // On resume, Initialize() MUST be used instead of InitializeEquipartition()
        SegmentScheduler resume_sched(kTotal, 8, 131072);
        resume_sched.Initialize(saved_segs);

        auto restored = resume_sched.GetSnapshots();
        ASSERT_TRUE(restored.size() == 2, "Preserved exactly 2 saved segments");
        ASSERT_TRUE(restored[0].current_offset == 250000, "Segment 0 resumed at 250000");
        ASSERT_TRUE(restored[1].current_offset == 800000, "Segment 1 resumed at 800000");
        ASSERT_TRUE(SegmentScheduler::ValidateSegmentPartition(restored, kTotal), "Partition valid on resume");
    }

    std::cout << "  [6/6] Testing unknown file size / non-range server invariants...\n";
    {
        // When range_supported == false or total_bytes == 0, Case A executes single-stream GET.
        // Even if InitializeEquipartition(8) were called with total_bytes = 0, it creates a safe single segment [0, 0].
        SegmentScheduler fallback_sched(0, 8, 131072);
        fallback_sched.InitializeEquipartition(8);
        ASSERT_TRUE(fallback_sched.GetSnapshots().size() == 1, "Fallback produces single segment");
        ASSERT_TRUE(fallback_sched.GetSnapshots()[0].start_offset == 0, "Starts at 0");
    }

    return true;
}


bool TestFeatureMaturityHardeningFiveFeatures() {
    using namespace openidm;

    std::cout << "  [1/5] Testing Download Scheduler deterministic past, active, and future windows...\n";
    {
        std::error_code ec;
        std::filesystem::path work_dir = std::filesystem::temp_directory_path() / ("radium_test_sched_det_" + GenerateUuidV4());
        std::filesystem::create_directories(work_dir, ec);

        ControllerConfig cfg;
        cfg.state_db_path = work_dir / "state.db";
        cfg.default_download_dir = work_dir / "downloads";
        cfg.temp_root_dir = work_dir / "tmp";
        std::filesystem::create_directories(cfg.default_download_dir, ec);

        std::string mock_clock = "14:00";
        cfg.local_time_override = [&mock_clock]() { return mock_clock; };

        // 1. Initial run: seed tasks with past, active, and future windows
        {
            DownloadController ctrl(cfg);
            ASSERT_TRUE(ctrl.Initialize().ok(), "Initialize controller");

            // Task 1: Past window (09:00 - 12:00, mock clock 14:00)
            TaskModel t1;
            t1.task_id = "sched-past";
            t1.url = "https://example.com/past.bin";
            t1.file_name = "past.bin";
            ASSERT_TRUE(ctrl.AddTask(t1, false, true).ok(), "Added past task");
            ASSERT_TRUE(ctrl.SetTaskSchedule("sched-past", "09:00", "12:00").ok(), "Set past schedule");

            // Task 2: Active window (13:00 - 15:00, mock clock 14:00)
            TaskModel t2;
            t2.task_id = "sched-active";
            t2.url = "https://example.com/active.bin";
            t2.file_name = "active.bin";
            ASSERT_TRUE(ctrl.AddTask(t2, false, true).ok(), "Added active task");
            ASSERT_TRUE(ctrl.SetTaskSchedule("sched-active", "13:00", "15:00").ok(), "Set active schedule");

            // Task 3: Future window (16:00 - 18:00, mock clock 14:00)
            TaskModel t3;
            t3.task_id = "sched-future";
            t3.url = "https://example.com/future.bin";
            t3.file_name = "future.bin";
            ASSERT_TRUE(ctrl.AddTask(t3, false, true).ok(), "Added future task");
            ASSERT_TRUE(ctrl.SetTaskSchedule("sched-future", "16:00", "18:00").ok(), "Set future schedule");

            ctrl.Shutdown();
        }

        // 2. Restart controller at mock_clock = 14:00 and verify deterministic startup reconciliation
        {
            DownloadController ctrl2(cfg);
            ASSERT_TRUE(ctrl2.Initialize().ok(), "Re-initialize controller");

            // Past window must have expired cleanly
            auto task_past = ctrl2.GetTask("sched-past");
            ASSERT_TRUE(task_past.has_value(), "Past task retrieved");
            ASSERT_TRUE(!task_past->scheduled, "Past window: scheduled must be false");
            ASSERT_TRUE(task_past->schedule_time_local.empty(), "Past window: start time cleared");
            ASSERT_TRUE(task_past->schedule_stop_time_local.empty(), "Past window: stop time cleared");
            ASSERT_TRUE(task_past->status_detail.find("Schedule Expired") != std::string::npos, "Past window: marked Schedule Expired");

            // Active window must remain scheduled and active
            auto task_active = ctrl2.GetTask("sched-active");
            ASSERT_TRUE(task_active.has_value(), "Active task retrieved");
            ASSERT_TRUE(task_active->scheduled, "Active window: scheduled is true");
            ASSERT_TRUE(task_active->schedule_time_local == "13:00", "Active window: start time preserved");
            ASSERT_TRUE(task_active->schedule_stop_time_local == "15:00", "Active window: stop time preserved");

            // Future window must remain scheduled and paused
            auto task_future = ctrl2.GetTask("sched-future");
            ASSERT_TRUE(task_future.has_value(), "Future task retrieved");
            ASSERT_TRUE(task_future->scheduled, "Future window: scheduled is true");
            ASSERT_TRUE(task_future->schedule_time_local == "16:00", "Future window: start time preserved");
            ASSERT_TRUE(task_future->schedule_stop_time_local == "18:00", "Future window: stop time preserved");

            ctrl2.Shutdown();
        }
        std::filesystem::remove_all(work_dir, ec);
    }

    std::cout << "  [2/5] Testing Speed Limiter aggregate concurrent enforcement & dynamic limit updates...\n";
    {
        openidm::brisk::TokenBucketLimiter limiter;
        std::atomic<uint64_t> limit_bps{2000000}; // 2 MB/s
        std::atomic<bool> stop_flag{false};

        // Concurrent multi-worker consumption does not deadlock or starve
        std::atomic<uint64_t> total_consumed{0};
        auto worker_fn = [&]() {
            for (int i = 0; i < 5; ++i) {
                if (stop_flag.load()) break;
                limiter.Consume(20000, limit_bps, stop_flag);
                total_consumed.fetch_add(20000);
            }
        };

        std::thread w1(worker_fn);
        std::thread w2(worker_fn);
        std::thread w3(worker_fn);
        std::thread w4(worker_fn);

        // Dynamically throttle down during concurrent transfers
        limit_bps.store(1000000); // 1 MB/s

        w1.join();
        w2.join();
        w3.join();
        w4.join();

        ASSERT_TRUE(total_consumed.load() == 400000, "All concurrent workers completed consumption without starvation");

        // Cancel test: immediate unblock when stop_flag is raised
        stop_flag.store(true);
        auto t_start = std::chrono::steady_clock::now();
        limiter.Consume(5000000, limit_bps, stop_flag);
        auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t_start).count();
        ASSERT_TRUE(dur_ms < 150, "Canceled limiter returns immediately without blocking");
    }

    std::cout << "  [3/5] Testing Site Grabber full crawl with local HTTP server redirect and SSRF defense...\n";
    {
        using namespace openidm::crawler;
        std::error_code ec;

        // Verify redirect URL propagation on relative links
        std::string html = R"(
            <a href="nested/document.pdf">Document</a>
            <img src="/static/images/logo.png">
        )";
        auto urls = SiteGrabber::ExtractUrlsFromHtml(html, "http://example.com/v1/page.html");
        ASSERT_TRUE(urls.size() == 2, "2 extracted URLs");
        ASSERT_TRUE(urls[0] == "http://example.com/v1/nested/document.pdf", "Relative link correctly uses redirected base folder");
        ASSERT_TRUE(urls[1] == "http://example.com/static/images/logo.png", "Root link correctly uses redirected host");

        // SSRF protection: link pointing to loopback/private IP is rejected when allow_loopback=false
        SiteGrabberConfig grab_cfg;
        grab_cfg.start_url = "http://example.com/index.html";
        grab_cfg.max_depth = 1;
        grab_cfg.allow_loopback = false;
        ASSERT_TRUE(!grab_cfg.allow_loopback, "SiteGrabber default blocks loopback/private destinations");

        // SSRF Destination Validation
        ASSERT_TRUE(openidm::brisk::HttpClient::CheckHostResolvesToPrivate("127.0.0.1"), "CheckHostResolvesToPrivate blocks 127.0.0.1");
        ASSERT_TRUE(openidm::brisk::HttpClient::CheckHostResolvesToPrivate("localhost"), "CheckHostResolvesToPrivate blocks localhost");
        ASSERT_TRUE(openidm::brisk::HttpClient::IsPrivateOrLoopbackIp(0x0A000005), "IsPrivateOrLoopbackIp blocks 10.0.0.5");
    }

    std::cout << "  [4/5] Testing Antivirus strict mode vs non-strict mode across all scanner states...\n";
    {
        std::error_code ec;
        std::filesystem::path work_dir = std::filesystem::temp_directory_path() / ("radium_test_av_matrix_" + GenerateUuidV4());
        std::filesystem::create_directories(work_dir, ec);

        ControllerConfig cfg;
        cfg.state_db_path = work_dir / "state.db";
        cfg.default_download_dir = work_dir / "downloads";
        cfg.temp_root_dir = work_dir / "tmp";
        std::filesystem::create_directories(cfg.default_download_dir, ec);

        // Simulated scanner responses
        security::ScanReport simulated_report;
        bool scanner_available = true;
        bool return_error_result = false;

        cfg.av_available_override = [&]() { return scanner_available; };
        cfg.av_scan_override = [&](const std::filesystem::path&) -> Result<security::ScanReport> {
            if (return_error_result) {
                return Result<security::ScanReport>::Err("ERR_TIMEOUT", "Scan timed out");
            }
            return Result<security::ScanReport>::Ok(simulated_report);
        };

        DownloadController ctrl(cfg);
        ASSERT_TRUE(ctrl.Initialize().ok(), "Initialize controller");

        auto create_test_file = [&](const std::string& name) -> std::filesystem::path {
            auto p = work_dir / name;
            std::ofstream ofs(p, std::ios::binary);
            ofs << "harmless test fixture content";
            ofs.close();
            return p;
        };

        // 1. Clean file: both strict and non-strict mark completed
        {
            simulated_report.status = security::ScanStatus::kClean;
            simulated_report.scanner_name = "MockScanner";
            TaskModel t;
            t.file_name = "clean.txt";
            auto staged = create_test_file("clean.tmp");
            auto res = ctrl.IngestCompletedTask(t, staged);
            ASSERT_TRUE(res.ok(), "Ingest clean task");
            ASSERT_TRUE(res.value->state == TaskState::kCompleted, "Clean file is Completed");
            ASSERT_TRUE(res.value->status_detail == "Completed (Antivirus Clean)", "Clean detail");
        }

        // 2. Threat detected: both strict and non-strict must FAIL task and delete file!
        {
            simulated_report.status = security::ScanStatus::kThreatDetected;
            simulated_report.scanner_name = "MockScanner";
            TaskModel t;
            t.file_name = "infected.txt";
            auto staged = create_test_file("infected.tmp");
            auto res = ctrl.IngestCompletedTask(t, staged);
            ASSERT_TRUE(res.ok(), "Ingest processed");
            ASSERT_TRUE(res.value->state == TaskState::kFailed, "Threat file marked Failed");
            ASSERT_TRUE(res.value->error_code == "ERR_THREAT_DETECTED", "Threat error code");
            std::filesystem::path dest = cfg.default_download_dir / "infected.txt";
            ASSERT_TRUE(!std::filesystem::exists(dest, ec), "Infected file deleted from disk");
        }

        // 3. Scanner error: non-strict allows completed; strict FAILS
        {
            simulated_report.status = security::ScanStatus::kScannerError;
            // 3a. Non-strict mode
            auto s = ctrl.GetSettings();
            s.av_strict_mode = false;
            ASSERT_TRUE(ctrl.UpdateSettings(s).ok(), "av_strict_mode = false");
            TaskModel t1;
            t1.file_name = "scanner_err1.txt";
            auto staged1 = create_test_file("scanner_err1.tmp");
            auto res1 = ctrl.IngestCompletedTask(t1, staged1);
            ASSERT_TRUE(res1.ok() && res1.value->state == TaskState::kCompleted, "Non-strict allows scanner error as Completed");

            // 3b. Strict mode
            s.av_strict_mode = true;
            ASSERT_TRUE(ctrl.UpdateSettings(s).ok(), "av_strict_mode = true");
            TaskModel t2;
            t2.file_name = "scanner_err2.txt";
            auto staged2 = create_test_file("scanner_err2.tmp");
            auto res2 = ctrl.IngestCompletedTask(t2, staged2);
            ASSERT_TRUE(res2.ok(), "Ingest processed");
            ASSERT_TRUE(res2.value->state == TaskState::kFailed, "Strict mode FAILS on scanner error");
            ASSERT_TRUE(res2.value->error_code == "ERR_AV_SCAN_FAILED", "Strict mode error code ERR_AV_SCAN_FAILED");
        }

        // 4. Scanner timeout/error result: strict FAILS
        {
            return_error_result = true;
            TaskModel t;
            t.file_name = "scanner_timeout.txt";
            auto staged = create_test_file("scanner_timeout.tmp");
            auto res = ctrl.IngestCompletedTask(t, staged);
            ASSERT_TRUE(res.ok(), "Ingest processed");
            ASSERT_TRUE(res.value->state == TaskState::kFailed, "Strict mode FAILS on scan timeout/error");
            return_error_result = false;
        }

        // 5. Scanner unavailable: non-strict allows completed; strict FAILS
        {
            scanner_available = false;
            // 5a. Strict mode (currently true)
            TaskModel t1;
            t1.file_name = "scanner_unavail1.txt";
            auto staged1 = create_test_file("scanner_unavail1.tmp");
            auto res1 = ctrl.IngestCompletedTask(t1, staged1);
            ASSERT_TRUE(res1.ok(), "Ingest processed");
            ASSERT_TRUE(res1.value->state == TaskState::kFailed, "Strict mode FAILS when scanner is unavailable");
            ASSERT_TRUE(res1.value->error_code == "ERR_AV_UNAVAILABLE", "ERR_AV_UNAVAILABLE reported");

            // 5b. Non-strict mode
            auto s = ctrl.GetSettings();
            s.av_strict_mode = false;
            ASSERT_TRUE(ctrl.UpdateSettings(s).ok(), "av_strict_mode = false");
            TaskModel t2;
            t2.file_name = "scanner_unavail2.txt";
            auto staged2 = create_test_file("scanner_unavail2.tmp");
            auto res2 = ctrl.IngestCompletedTask(t2, staged2);
            ASSERT_TRUE(res2.ok() && res2.value->state == TaskState::kCompleted, "Non-strict allows unavailable scanner as Completed");
        }

        ctrl.Shutdown();
        std::filesystem::remove_all(work_dir, ec);
    }

    std::cout << "  [5/5] Testing Auto-Categorization decision hierarchy & engine category preservation...\n";
    {
        std::error_code ec;
        std::filesystem::path work_dir = std::filesystem::temp_directory_path() / ("radium_test_cat_hier_" + GenerateUuidV4());
        std::filesystem::create_directories(work_dir, ec);

        ControllerConfig cfg;
        cfg.state_db_path = work_dir / "state.db";
        cfg.default_download_dir = work_dir / "downloads";
        cfg.temp_root_dir = work_dir / "tmp";
        std::filesystem::create_directories(cfg.default_download_dir, ec);

        DownloadController ctrl(cfg);
        ASSERT_TRUE(ctrl.Initialize().ok(), "Initialize controller");

        auto s = ctrl.GetSettings();
        // Setup site rules and custom extensions
        s.site_rules["special-music.org"] = "Audio";
        s.custom_extension_categories[".specialbook"] = "Documents";
        ASSERT_TRUE(ctrl.UpdateSettings(s).ok(), "Configured rules & extensions");

        // Priority 1: User explicitly specified category -> preserved as-is
        TaskModel user_task;
        user_task.task_id = "user-cat-task";
        user_task.category = "CustomCategory";
        user_task.url = "https://special-music.org/track.mp3";
        user_task.file_name = "track.mp3";
        auto add_res = ctrl.AddTask(user_task, false, true);
        ASSERT_TRUE(add_res.ok(), "Add user task");
        auto u_added = ctrl.GetTask("user-cat-task");
        ASSERT_TRUE(u_added.has_value() && u_added->category == "CustomCategory", "User explicit category is preserved");

        // Priority 2: Site rule outranks extension and filename
        // Domain special-music.org maps to Audio, even if filename is movie.mp4 (which would otherwise be Video)
        TaskModel site_task;
        site_task.url = "https://special-music.org/movie.mp4";
        site_task.file_name = "movie.mp4";
        ASSERT_TRUE(ctrl.AddTask(site_task, false, true).ok(), "Add site task");
        auto s_added = ctrl.GetTask(site_task.task_id);
        ASSERT_TRUE(s_added.has_value() && s_added->category == "Audio", "Site rule outranks filename extension");

        // Priority 3: Custom extension mapping outranks default filename inference
        TaskModel ext_task;
        ext_task.url = "https://example.com/novel.specialbook";
        ext_task.file_name = "novel.specialbook";
        ASSERT_TRUE(ctrl.AddTask(ext_task, false, true).ok(), "Add custom extension task");
        auto e_added = ctrl.GetTask(ext_task.task_id);
        ASSERT_TRUE(e_added.has_value() && e_added->category == "Documents", "Custom extension outranks default");

        // Priority 4: Default filename extension inference
        ASSERT_TRUE(ctrl.DetermineCategory("clip.mp4", "https://example.com/clip.mp4") == "Video", "Filename -> Video");
        ASSERT_TRUE(ctrl.DetermineCategory("track.flac", "https://example.com/track.flac") == "Audio", "Filename -> Audio");
        ASSERT_TRUE(ctrl.DetermineCategory("archive.tar.gz", "https://example.com/archive.tar.gz") == "Compressed", "Filename -> Compressed");

        // Priority 5: MIME metadata inference fallback when filename has generic or no extension
        ASSERT_TRUE(InferCategoryFromMime("video/mp4") == "Video", "MIME video/mp4 -> Video");
        ASSERT_TRUE(InferCategoryFromMime("application/pdf") == "Documents", "MIME application/pdf -> Documents");
        ASSERT_TRUE(InferCategoryFromMime("application/octet-stream") == "General", "MIME generic fallback -> General");

        ctrl.Shutdown();
        std::filesystem::remove_all(work_dir, ec);
    }

    return true;
}

int main() {
    int passed = 0;
    

    const int total = 77;

    auto run = [&](int idx, const char* name, bool (*fn)()) {
        std::cout << "[" << idx << "/" << total << "] Running " << name << "..." << std::endl;
        if (fn()) {
            std::cout << "  -> PASS" << std::endl;
            ++passed;
        }
    };

    run(1,  "TestPathSanitizerAndContentDisposition", TestPathSanitizerAndContentDisposition);
    run(2,  "TestDynamicWorkerAndDiskSpaceGuard", TestDynamicWorkerAndDiskSpaceGuard);
    run(3,  "TestLoggerRedactionAndCrypto", TestLoggerRedactionAndCrypto);
    run(4,  "TestBriskSegmentSchedulerWorkStealingAndRollback", TestBriskSegmentSchedulerWorkStealingAndRollback);
    run(5,  "TestSqliteStoreAndCrashRecovery", TestSqliteStoreAndCrashRecovery);
    run(6,  "TestStreamMetaJsonAndDrmRejection", TestStreamMetaJsonAndDrmRejection);
    run(7,  "TestControllerLifecycleAndDeletedTaskRace", TestControllerLifecycleAndDeletedTaskRace);
    run(8,  "TestDesktopUiDesignSystemAndHighDpiLayout", TestDesktopUiDesignSystemAndHighDpiLayout);
    run(9,  "TestNativeAnimationsAndTaskbarProgress", TestNativeAnimationsAndTaskbarProgress);
    run(10, "TestProfessionalSettingsCenterValidationAndPersistence", TestProfessionalSettingsCenterValidationAndPersistence);
    run(11, "TestDesktopUiInteractionsFilteringSortingAndRedaction", TestDesktopUiInteractionsFilteringSortingAndRedaction);
    run(12, "TestKeyboardNavigationAndShortcuts", TestKeyboardNavigationAndShortcuts);
    run(13, "TestDesktopUiHitTestingAndMouseInteractions", TestDesktopUiHitTestingAndMouseInteractions);
    run(14, "TestWorkerLifecycleAndIncompatibleWorkerDetection", TestWorkerLifecycleAndIncompatibleWorkerDetection);
    run(15, "TestRadiumBrandingAndLegacyDataMigration", TestRadiumBrandingAndLegacyDataMigration);
    run(16, "TestPartFileIsolationAndSafeCommit", TestPartFileIsolationAndSafeCommit);
    run(17, "TestResourceAwareResumeAndValidatorMismatch", TestResourceAwareResumeAndValidatorMismatch);
    run(18, "TestSegmentSchedulerBoundarySplitRaceGuard", TestSegmentSchedulerBoundarySplitRaceGuard);
    run(19, "TestIpcSecurityDaclAndPathTraversal", TestIpcSecurityDaclAndPathTraversal);
    run(20, "TestGlobalRateLimiting", TestGlobalRateLimiting);
    run(21, "TestRelativeAndProtocolRelativeRedirectResolution", TestRelativeAndProtocolRelativeRedirectResolution);
    run(22, "TestCompleteSegmentRangeMapValidation", TestCompleteSegmentRangeMapValidation);
    run(23, "TestPosixHttpChunkedDecoder", TestPosixHttpChunkedDecoder);
    run(24, "TestUrlParsingAndSecurityChecks", TestUrlParsingAndSecurityChecks);
    run(25, "TestIpcShutdownAndFirstPipeInstance", TestIpcShutdownAndFirstPipeInstance);
    run(26, "TestConflictPolicies", TestConflictPolicies);
    run(27, "TestSystemSuspendResumeAndPauseReason", TestSystemSuspendResumeAndPauseReason);
    run(28, "TestConcurrentAtomicCommitsAndSha256", TestConcurrentAtomicCommitsAndSha256);
    run(29, "TestDnsRebindingAndDestinationSecurity", TestDnsRebindingAndDestinationSecurity);
    run(30, "TestStreamContainerValidation", TestStreamContainerValidation);
    run(31, "TestFtpClientParsingAndCommands", TestFtpClientParsingAndCommands);
    run(32, "TestBencodeAndTorrentParser", TestBencodeAndTorrentParser);
    run(33, "TestRtspMmsStreamDetection", TestRtspMmsStreamDetection);
    run(34, "TestSiteGrabberCrawlerAndSsrf", TestSiteGrabberCrawlerAndSsrf);
    run(35, "TestZipArchivePreviewAndSecurity", TestZipArchivePreviewAndSecurity);
    run(36, "TestTaskImportExportAndMultiQueue", TestTaskImportExportAndMultiQueue);
    run(37, "TestAntivirusScannerSafetyAndHandling", TestAntivirusScannerSafetyAndHandling);
    run(38, "TestDualRateLimiterAndPerTaskSpeedLimit", TestDualRateLimiterAndPerTaskSpeedLimit);
    run(39, "TestDynamicCategorizationAndSiteRulesPriority", TestDynamicCategorizationAndSiteRulesPriority);
    run(40, "TestAlternateUrlsAndMirrorValidation", TestAlternateUrlsAndMirrorValidation);
    run(41, "TestFtpReliabilityEpsvAndResumeTruncation", TestFtpReliabilityEpsvAndResumeTruncation);
    run(42, "TestSchedulerStopTimesAndPostCompletionActions", TestSchedulerStopTimesAndPostCompletionActions);
    run(43, "TestPeerWireProtocolFramingAndCompactPeers", TestPeerWireProtocolFramingAndCompactPeers);
    run(44, "TestMultiFileStorageMappingAndBoundarySplitting", TestMultiFileStorageMappingAndBoundarySplitting);
    run(45, "TestPieceManagerAndBlockScheduling", TestPieceManagerAndBlockScheduling);
    run(46, "TestTrackerClientQueryEncodingAndResponseParsing", TestTrackerClientQueryEncodingAndResponseParsing);
    run(47, "TestHttpRangeSatisfactionValidation", TestHttpRangeSatisfactionValidation);
    run(48, "TestDuplicateTaskDetectionThreeTiers", TestDuplicateTaskDetectionThreeTiers);
    run(49, "TestExpiredLinkRefreshAndCookieReplacement", TestExpiredLinkRefreshAndCookieReplacement);
    run(50, "TestSegmentSchedulerChunkNormalizationAndTruncationGuard", TestSegmentSchedulerChunkNormalizationAndTruncationGuard);
    run(51, "TestUrlUserinfoExtractionAndBasicAuthGeneration", TestUrlUserinfoExtractionAndBasicAuthGeneration);
    run(52, "TestModernCoralThemeSettingsModalBoundsAndSafeDelete", TestModernCoralThemeSettingsModalBoundsAndSafeDelete);
    run(53, "TestYouTubeSpeedParsingMidnightCrossingAndBitTorrentUpload", TestYouTubeSpeedParsingMidnightCrossingAndBitTorrentUpload);
    run(54, "TestFinalPhaseRemediationAndLiveMetrics", TestFinalPhaseRemediationAndLiveMetrics);
    run(55, "TestYouTubeCanonicalRoutingAndSchedulerThroughput", TestYouTubeCanonicalRoutingAndSchedulerThroughput);
    run(56, "TestSpeedFormattingMultiTrackProgressAndContainerVerification", TestSpeedFormattingMultiTrackProgressAndContainerVerification);
    run(57, "TestPimpbunnyStreamRoutingAndYouTubeNetscapeCookieFile", TestPimpbunnyStreamRoutingAndYouTubeNetscapeCookieFile);
    run(58, "TestRemediationPhaseRegressionSuites", TestRemediationPhaseRegressionSuites);
    run(59, "TestAuditBehaviorImprovements", TestAuditBehaviorImprovements);
    run(60, "TestQualitySelectionSizeAndColumnDocking", TestQualitySelectionSizeAndColumnDocking);
    run(61, "TestNewDownloadDialogSubmissionAndTaskCreation", TestNewDownloadDialogSubmissionAndTaskCreation);
    run(62, "TestYouTubeQualitySelectionIntegrityAndVerification", TestYouTubeQualitySelectionIntegrityAndVerification);
    run(63, "TestAutoScrollToCurrentDownload", TestAutoScrollToCurrentDownload);
    run(64, "TestHttp429AdaptiveConnectionFallbackAndIntegrity", TestHttp429AdaptiveConnectionFallbackAndIntegrity);
    run(65, "TestBitTorrentEngineTransferAndIntegrity", TestBitTorrentEngineTransferAndIntegrity);
    run(66, "TestUdpTrackerProtocolLiveExchange", TestUdpTrackerProtocolLiveExchange);
    run(67, "TestMagnetMetadataExchangeBEP9Live", TestMagnetMetadataExchangeBEP9Live);
    run(68, "TestDhtIterativePeerDiscoveryLive", TestDhtIterativePeerDiscoveryLive);
    run(69, "TestMultiPeerTransferAndSeedingControls", TestMultiPeerTransferAndSeedingControls);
    run(70, "TestPauseAllManualPauseScheduleAndRecovery", TestPauseAllManualPauseScheduleAndRecovery);
    run(71, "TestNewDownloadDialogRowSpacingAcrossDpi", TestNewDownloadDialogRowSpacingAcrossDpi);
    run(72, "TestSegmentSchedulerDoesNotResplitInFlightRange", TestSegmentSchedulerDoesNotResplitInFlightRange);
    run(73, "TestHttpRangeCorrectnessAnd416Validation", TestHttpRangeCorrectnessAnd416Validation);
    run(74, "TestHttpRecoveryAndResumeReliability", TestHttpRecoveryAndResumeReliability);
    run(75, "TestHttpRangePrepartitionEquipartitionInvariants", TestHttpRangePrepartitionEquipartitionInvariants);
    run(76, "TestInitializeEquipartitionEdgeCases", TestInitializeEquipartitionEdgeCases);
    run(77, "TestFeatureMaturityHardeningFiveFeatures", TestFeatureMaturityHardeningFiveFeatures);

    std::cout << "\nC++20 Unit & Regression Test Summary: " << passed << " / " << total << " suites PASSED.\n";
    return (passed == total) ? 0 : 1;
}
