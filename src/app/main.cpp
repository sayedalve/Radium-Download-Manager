// SPDX-License-Identifier: GPL-3.0-or-later
// Radium -- Main Desktop GUI, CLI & Daemon Application Entry Point (radium)
// Default launch (no arguments):
//   Starts the background IPC server and launches the Radium Desktop GUI application window.
// CLI flags:
//   --version                    Print version info
//   --gui-snapshot               Render full desktop UI snapshot (sidebar counts, table, details, status bar) and exit
//   --list --db <state.db>       Display the unified task queue table
//   --download <url> --out <dir> Download an ordinary HTTP/HTTPS file using Brisk engine
//   --stream <url> --out <dir>   Download & mux an HLS/DASH stream using N_m3u8DL-RE + FFmpeg
//   --probe-stream <url>         Probe an HLS/DASH manifest and print JSON tracks
//   --daemon --ipc-socket <path> Run background daemon with IPC Server listening for Browser Extension

#include "common/json_mini.hpp"
#include "common/logger.hpp"
#include "common/path_sanitizer.hpp"
#include "common/version.hpp"
#include "core/download_controller.hpp"
#include "ipc/ipc_server.hpp"
#include "ui/win32_main_window.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

namespace {
std::atomic<bool> g_terminate_daemon{false};
void SignalHandler(int) { g_terminate_daemon.store(true); }

void WriteConsoleOutput(const std::string& str, bool is_error = false) {
#ifdef _WIN32
    HANDLE hStd = GetStdHandle(is_error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (hStd && hStd != INVALID_HANDLE_VALUE) {
        DWORD outType = GetFileType(hStd);
        if (outType == FILE_TYPE_PIPE || outType == FILE_TYPE_DISK || outType == FILE_TYPE_CHAR) {
            DWORD written = 0;
            if (WriteFile(hStd, str.data(), static_cast<DWORD>(str.size()), &written, nullptr) && written > 0) {
                FlushFileBuffers(hStd);
                return;
            }
        }
    }
#endif
    if (is_error) {
        std::cerr << str;
        std::cerr.flush();
        std::fflush(stderr);
    } else {
        std::cout << str;
        std::cout.flush();
        std::fflush(stdout);
    }
}
} // namespace

int main(int argc, char* argv[]) {
#if defined(__GLIBC__)
    // Cap glibc malloc arenas to 2 so 8-connection worker threads do not allocate 8 x 64 MiB virtual/resident arenas
    mallopt(M_ARENA_MAX, 2);
#endif
    std::filesystem::path exe_dir = openidm::PathSanitizer::GetExecutableDir(argc > 0 ? argv[0] : nullptr);
    std::filesystem::path app_data_dir = openidm::PathSanitizer::GetDefaultAppDataDir();

    openidm::ControllerConfig cfg;
    cfg.state_db_path = app_data_dir / "state.db";
    cfg.default_download_dir = openidm::PathSanitizer::GetDefaultDownloadsDir(exe_dir);
    cfg.temp_root_dir = app_data_dir / "tmp";
    cfg.nm3u8dl_binary_path = openidm::PathSanitizer::FindWorkerBinary("N_m3u8DL-RE", exe_dir);
    cfg.ffmpeg_binary_path = openidm::PathSanitizer::FindWorkerBinary("ffmpeg", exe_dir);
    // SEC-04: Block public-to-loopback/private redirects by default unless explicitly configured
    cfg.allow_loopback = (std::getenv("RADIUM_ALLOW_LOOPBACK") != nullptr || std::getenv("FLUX_ALLOW_LOOPBACK") != nullptr || std::getenv("OPENIDM_ALLOW_LOOPBACK") != nullptr);

    std::string mode;
    std::string target_url;
    std::string ipc_socket_path = openidm::ipc::IpcServer::DefaultSocketPath();
    uint32_t connections = 8;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--version" || arg == "-v") {
            WriteConsoleOutput(FLUX_FULL_VERSION_STRING, false);
            return 0;
        } else if (arg == "--db" && i + 1 < argc) {
            cfg.state_db_path = argv[++i];
        } else if (arg == "--out" && i + 1 < argc) {
            cfg.default_download_dir = argv[++i];
        } else if (arg == "--connections" && i + 1 < argc) {
            connections = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--download" && i + 1 < argc) {
            mode = "download";
            target_url = argv[++i];
        } else if (arg == "--stream" && i + 1 < argc) {
            mode = "stream";
            target_url = argv[++i];
        } else if (arg == "--probe-stream" && i + 1 < argc) {
            mode = "probe";
            target_url = argv[++i];
        } else if (arg == "--list") {
            mode = "list";
        } else if (arg == "--gui-snapshot") {
            mode = "gui-snapshot";
        } else if (arg == "--daemon") {
            mode = "daemon";
        } else if (arg == "--minimized") {
            mode = "minimized";
        } else if (arg == "--ipc-socket" && i + 1 < argc) {
            ipc_socket_path = argv[++i];
        } else if (arg == "--worker-binary" && i + 1 < argc) {
            cfg.nm3u8dl_binary_path = argv[++i];
        } else if (arg == "--ffmpeg-binary" && i + 1 < argc) {
            cfg.ffmpeg_binary_path = argv[++i];
        } else if (arg == "--allow-loopback") {
            cfg.allow_loopback = true;
        }
    }

    openidm::Logger::Instance().SetLogFile(cfg.state_db_path.parent_path() / "radium.log");

#ifdef _WIN32
    std::wstring wpipe(ipc_socket_path.begin(), ipc_socket_path.end());

    // 1. Single-instance check for GUI mode (mode.empty() || mode == "minimized")
    if (mode.empty() || mode == "minimized") {
        HWND existing_hwnd = FindWindowW(L"Radium_MainWindowClass", nullptr);
        if (existing_hwnd) {
            if (mode.empty()) {
                ShowWindow(existing_hwnd, SW_SHOW);
                ShowWindow(existing_hwnd, SW_RESTORE);
                SetForegroundWindow(existing_hwnd);
                PostMessageW(existing_hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
            }
            return 0; // Seamless single instance: bring existing GUI window to front and exit
        }

        // If a background daemon is running, signal it to exit so the user's interactive GUI can take over the pipe & database
        HANDLE hDaemonShutdown = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, L"Local\\Radium_Daemon_Shutdown_Event");
        if (hDaemonShutdown) {
            SetEvent(hDaemonShutdown);
            CloseHandle(hDaemonShutdown);
            for (int i = 0; i < 30; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                HANDLE hTest = CreateFileW(wpipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                if (hTest == INVALID_HANDLE_VALUE) {
                    break;
                }
                CloseHandle(hTest);
            }
        }
    }

    // 2. Single-instance check for background daemon mode
    if (mode == "daemon") {
        HWND existing_hwnd = FindWindowW(L"Radium_MainWindowClass", nullptr);
        if (existing_hwnd) {
            // Desktop GUI is already running and providing the IPC server on this pipe
            return 0;
        }
        HANDLE hTest = CreateFileW(wpipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hTest != INVALID_HANDLE_VALUE) {
            CloseHandle(hTest);
            // Another background daemon or host is already actively serving this IPC pipe
            return 0;
        }
    }
#endif

    openidm::DownloadController controller(cfg);
    auto init_res = controller.Initialize();
    if (!init_res.ok()) {
        WriteConsoleOutput("ERROR: Failed to initialize Radium controller: " + init_res.error->message + "\n", true);
        return 1;
    }

    // Default launch path: Launch the Desktop GUI Application + IPC Server (BUG-002 Fix)
    if (mode.empty() || mode == "minimized") {
        openidm::ipc::IpcServer server(controller, ipc_socket_path);
        server.Start();
        openidm::ui::DesktopView view(controller, &server);
        if (mode == "minimized") {
            view.SetStartVisible(false);
        } else {
            view.SetStartVisible(true);
        }
        int rc = view.RunApplicationLoop(false);
        server.Stop();
        controller.Shutdown();
        return rc;
    }

    if (mode == "list") {
        openidm::ui::DesktopView view(controller);
        WriteConsoleOutput(view.RenderQueueTableText(), false);
        controller.Shutdown();
        return 0;
    }

    if (mode == "gui-snapshot") {
        openidm::ui::DesktopView view(controller, nullptr);
        view.SelectFirstRow();
        WriteConsoleOutput(view.RenderFullDesktopSnapshotText(), false);
        controller.Shutdown();
        return 0;
    }

    if (mode == "probe") {
        auto pres = controller.ProbeStream(target_url, {}, {});
        if (!pres.ok()) {
            std::string err_str = "ERROR: " + pres.error->code + ": " + pres.error->message + "\n";
            WriteConsoleOutput(err_str, true);
            controller.Shutdown();
            return 2;
        }
        std::string probe_summary = "Probed " + std::to_string(pres.value->tracks.size()) + " tracks from " + pres.value->manifest_url + "\n";
        for (const auto& tr : pres.value->tracks) {
            std::string upper_type = tr.media_type;
            for (char& c : upper_type) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            probe_summary += "  [" + upper_type + "] id=" + tr.id
                          + " res=" + tr.resolution + " bw=" + std::to_string(tr.bandwidth)
                          + " lang=" + tr.language + " segs=" + std::to_string(tr.segments_count) + "\n";
        }
        WriteConsoleOutput(probe_summary, false);
        controller.Shutdown();
        return 0;
    }

    if (mode == "download" || mode == "stream") {
        openidm::TaskModel t;
        t.url = target_url;
        t.max_connections = connections;
        t.save_dir = cfg.default_download_dir.string();
        if (mode == "stream") {
            t.task_type = (target_url.find(".mpd") != std::string::npos)
                              ? openidm::TaskType::kStreamDash
                              : openidm::TaskType::kStreamHls;
        } else {
            t.task_type = openidm::TaskType::kHttpFile;
        }
        auto added = controller.AddTask(std::move(t), true, true);
        if (!added.ok()) {
            WriteConsoleOutput("ERROR: " + added.error->message + "\n", true);
            controller.Shutdown();
            return 1;
        }
        auto done = controller.WaitForTaskCompletion(added.value->task_id, 300000);
        controller.Shutdown();
        if (!done.ok() || done.value->state != openidm::TaskState::kCompleted) {
            std::string err_msg = "ERROR: Task failed: " + (done.ok() ? done.value->error_message : done.error->message) + "\n";
            WriteConsoleOutput(err_msg, true);
            return 2;
        }
        std::string comp_msg = "COMPLETED: " + (std::filesystem::path(done.value->save_dir) / done.value->file_name).string()
                             + " (" + std::to_string(done.value->downloaded_bytes) + " bytes)\n";
        WriteConsoleOutput(comp_msg, false);
        return 0;
    }

    if (mode == "daemon") {
        std::signal(SIGINT, SignalHandler);
        std::signal(SIGTERM, SignalHandler);
#ifdef _WIN32
        HANDLE hShutdownEvent = CreateEventW(nullptr, FALSE, FALSE, L"Local\\Radium_Daemon_Shutdown_Event");
#endif
        openidm::ipc::IpcServer server(controller, ipc_socket_path);
        server.SetHeadlessDaemonMode(true);
        auto sres = server.Start();
        if (!sres.ok()) {
            WriteConsoleOutput("ERROR: Failed to start IPC server: " + sres.error->message + "\n", true);
            controller.Shutdown();
#ifdef _WIN32
            if (hShutdownEvent) CloseHandle(hShutdownEvent);
#endif
            return 1;
        }
        while (!g_terminate_daemon.load()) {
#ifdef _WIN32
            if (hShutdownEvent) {
                DWORD wr = WaitForSingleObject(hShutdownEvent, 50);
                if (wr == WAIT_OBJECT_0) {
                    g_terminate_daemon.store(true);
                    break;
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
#else
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
#endif
        }
        server.Stop();
        controller.Shutdown();
#ifdef _WIN32
        if (hShutdownEvent) CloseHandle(hShutdownEvent);
#endif
        return 0;
    }

    controller.Shutdown();
    return 0;
}

#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);

    // If CLI arguments were passed and we're not redirected to a pipe/file, attach to parent console
    if (argc > 1) {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD outType = (hOut && hOut != INVALID_HANDLE_VALUE) ? GetFileType(hOut) : FILE_TYPE_UNKNOWN;
        if (outType != FILE_TYPE_PIPE && outType != FILE_TYPE_DISK) {
            if (AttachConsole(ATTACH_PARENT_PROCESS)) {
                FILE* fp = nullptr;
                freopen_s(&fp, "CONOUT$", "w", stdout);
                freopen_s(&fp, "CONOUT$", "w", stderr);
            }
        }
    }

    std::ios::sync_with_stdio(true);

    std::vector<std::string> utf8_args;
    std::vector<char*> argv_ptrs;
    for (int i = 0; i < argc; ++i) {
        int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(std::max(0, len > 0 ? len - 1 : 0), '\0');
        if (len > 1) {
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), len, nullptr, nullptr);
        }
        utf8_args.push_back(std::move(s));
    }
    if (wargv) LocalFree(wargv);
    for (auto& s : utf8_args) {
        argv_ptrs.push_back(s.data());
    }
    int ret = main(static_cast<int>(argv_ptrs.size()), argv_ptrs.data());
    std::cout.flush();
    std::cerr.flush();
    std::fflush(stdout);
    std::fflush(stderr);
    return ret;
}
#endif
