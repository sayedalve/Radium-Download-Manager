// SPDX-License-Identifier: Apache-2.0
// OpenIDM / Flux — RTSP, MMS & Live Media Stream Adapter
#include "engine_stream/rtsp_mms_adapter.hpp"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "common/logger.hpp"
#include "common/path_sanitizer.hpp"

namespace openidm::stream {

RtspMmsAdapter::RtspMmsAdapter(std::filesystem::path ffmpeg_path, bool allow_loopback)
    : m_ffmpeg_path(std::move(ffmpeg_path)), m_allow_loopback(allow_loopback) {
    if (m_ffmpeg_path.empty()) {
        m_ffmpeg_path = PathSanitizer::FindWorkerBinary("ffmpeg", PathSanitizer::GetExecutableDir());
    }
}

bool RtspMmsAdapter::IsStreamUrl(std::string_view url) {
    static const std::string_view kProtocols[] = {
        "rtsp://", "rtsps://", "mms://", "mmst://", "rtmp://", "rtmpe://"
    };
    for (auto proto : kProtocols) {
        if (url.size() >= proto.size() && url.substr(0, proto.size()) == proto) {
            return true;
        }
    }
    return false;
}

bool RtspMmsAdapter::ValidateStreamUrl(std::string_view url) {
    if (!IsStreamUrl(url)) return false;
    for (char c : url) {
        if (static_cast<unsigned char>(c) <= 32 || c == '"' || c == '\\' || c == '\'' || c == '`') {
            return false;
        }
    }
    return true;
}

Result<std::filesystem::path> RtspMmsAdapter::ExecuteStreamCapture(
    TaskModel& task,
    std::atomic<bool>& pause_requested,
    std::atomic<bool>& cancel_requested,
    RtspProgressCallback on_progress) {

    if (!ValidateStreamUrl(task.url)) {
        task.state = TaskState::kFailed;
        task.error_code = "ERR_INVALID_URL";
        task.error_message = "Invalid or dangerous stream URL";
        return Result<std::filesystem::path>::Err(task.error_code, task.error_message);
    }

    std::error_code ec;
    if (!std::filesystem::exists(m_ffmpeg_path, ec)) {
        m_ffmpeg_path = PathSanitizer::FindWorkerBinary("ffmpeg", PathSanitizer::GetExecutableDir());
        if (!std::filesystem::exists(m_ffmpeg_path, ec)) {
            task.state = TaskState::kFailed;
            task.error_code = "ERR_FFMPEG_MISSING";
            task.error_message = "FFmpeg executable not found in PATH or tools/";
            return Result<std::filesystem::path>::Err(task.error_code, task.error_message);
        }
    }

    std::filesystem::path save_dir(task.save_dir);
    std::filesystem::create_directories(save_dir, ec);

    std::string out_name = task.file_name;
    if (out_name.empty()) {
        out_name = "stream_" + task.task_id.substr(0, 8) + ".mp4";
    }
    std::filesystem::path final_output = save_dir / out_name;
    std::filesystem::path temp_part = save_dir / (out_name + "." + task.task_id + ".part");

    task.part_path = temp_part.string();
    task.state = TaskState::kDownloading;
    task.status_detail = "Capturing live stream via FFmpeg";

#ifdef _WIN32
    // Windows Job Object for process group containment
    HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
    if (hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {0};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }

    // Pipes for stderr capture
    HANDLE hStdErrRead = nullptr, hStdErrWrite = nullptr;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    CreatePipe(&hStdErrRead, &hStdErrWrite, &sa, 0);
    SetHandleInformation(hStdErrRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = hStdErrWrite;

    PROCESS_INFORMATION pi = {0};

    // Argument-safe command line construction
    std::wstring cmd = L"\"" + m_ffmpeg_path.wstring() + L"\" -nostdin -y -i \"" +
                       std::wstring(task.url.begin(), task.url.end()) + L"\" -c copy \"" +
                       temp_part.wstring() + L"\"";

    std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back(L'\0');

    BOOL ok = CreateProcessW(
        nullptr, cmd_buf.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

    CloseHandle(hStdErrWrite);

    if (!ok) {
        if (hStdErrRead) CloseHandle(hStdErrRead);
        if (hJob) CloseHandle(hJob);
        task.state = TaskState::kFailed;
        task.error_code = "ERR_PROCESS_SPAWN";
        task.error_message = "Failed to spawn FFmpeg process";
        return Result<std::filesystem::path>::Err(task.error_code, task.error_message);
    }

    if (hJob) {
        AssignProcessToJobObject(hJob, pi.hProcess);
    }

    char buffer[1024];
    DWORD bytesRead = 0;
    std::string line_accum;

    DWORD exit_code = 0;
    while (true) {
        if (pause_requested.load() || cancel_requested.load()) {
            TerminateProcess(pi.hProcess, 1);
            break;
        }

        DWORD avail = 0;
        if (PeekNamedPipe(hStdErrRead, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            if (ReadFile(hStdErrRead, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) && bytesRead > 0) {
                buffer[bytesRead] = '\0';
                line_accum += buffer;

                size_t nl;
                while ((nl = line_accum.find('\n')) != std::string::npos || (nl = line_accum.find('\r')) != std::string::npos) {
                    std::string line = line_accum.substr(0, nl);
                    line_accum.erase(0, nl + 1);

                    // Parse time=00:01:23.45 and size
                    auto time_pos = line.find("time=");
                    if (time_pos != std::string::npos) {
                        std::string time_str = line.substr(time_pos + 5, 11);
                        int h = 0, m = 0;
                        double s = 0.0;
                        if (sscanf(time_str.c_str(), "%d:%d:%lf", &h, &m, &s) == 3) {
                            double dur = h * 3600.0 + m * 60.0 + s;
                            task.downloaded_bytes = std::filesystem::exists(temp_part, ec) ? std::filesystem::file_size(temp_part, ec) : 0;
                            task.status_detail = "Capturing stream (" + time_str + ")";

                            if (on_progress) {
                                RtspProgress prog;
                                prog.task_id = task.task_id;
                                prog.downloaded_bytes = task.downloaded_bytes;
                                prog.duration_sec = dur;
                                prog.speed_bps = 0;
                                prog.status_detail = task.status_detail;
                                on_progress(prog);
                            }
                        }
                    }
                }
            }
        }

        if (GetExitCodeProcess(pi.hProcess, &exit_code) && exit_code != STILL_ACTIVE) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    WaitForSingleObject(pi.hProcess, 1000);
    GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(hStdErrRead);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (hJob) CloseHandle(hJob);

    if (cancel_requested.load()) {
        std::filesystem::remove(temp_part, ec);
        task.state = TaskState::kCanceled;
        task.status_detail = "Canceled";
        return Result<std::filesystem::path>::Err("ERR_CANCELED", "Stream capture canceled");
    }

    if (pause_requested.load()) {
        task.state = TaskState::kPaused;
        task.status_detail = "Paused";
        return Result<std::filesystem::path>::Err("ERR_PAUSED", "Stream capture paused");
    }

    if (exit_code != 0) {
        std::error_code ec_rem;
        std::filesystem::remove(temp_part, ec_rem);
        task.state = TaskState::kFailed;
        task.error_code = "ERR_FFMPEG_FAILED";
        task.error_message = "FFmpeg process failed with exit code " + std::to_string(exit_code);
        task.status_detail = task.error_message;
        return Result<std::filesystem::path>::Err(task.error_code, task.error_message);
    }

    if (std::filesystem::exists(temp_part, ec) && std::filesystem::file_size(temp_part, ec) > 0) {
        std::filesystem::rename(temp_part, final_output, ec);
        task.state = TaskState::kCompleted;
        task.downloaded_bytes = std::filesystem::file_size(final_output, ec);
        task.total_bytes = task.downloaded_bytes;
        task.status_detail = "Stream Capture Completed";
        return Result<std::filesystem::path>::Ok(final_output);
    }

    task.state = TaskState::kFailed;
    task.error_code = "ERR_STREAM_EMPTY";
    task.error_message = "Stream capture produced empty file";
    task.status_detail = task.error_message;
    return Result<std::filesystem::path>::Err(task.error_code, task.error_message);

#else
    (void)pause_requested;
    (void)cancel_requested;
    (void)on_progress;
    (void)final_output;
    (void)temp_part;
    task.state = TaskState::kFailed;
    task.error_code = "ERR_UNSUPPORTED_PLATFORM";
    task.error_message = "RTSP/MMS stream capture is unsupported on this platform";
    task.status_detail = task.error_message;
    return Result<std::filesystem::path>::Err(task.error_code, task.error_message);
#endif
}

} // namespace openidm::stream
