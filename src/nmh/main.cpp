// SPDX-License-Identifier: Apache-2.0
// Radium -- Browser Native Messaging Host (radium-nmh)
// Bridges 32-bit length-prefixed JSON messages on stdin/stdout from Chrome/Edge/Brave/Firefox
// to the Radium IPC Server (Named Pipe on Windows 11, UNIX Domain Socket on POSIX),
// auto-launching radium --daemon if the server is not yet running.
// Implements NATIVE_MESSAGING_AND_IPC.md & BUG-004 Fix.

#include "common/ipc_path.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace {

bool ReadExact(void* buf, size_t count, FILE* stream) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t total = 0;
    while (total < count) {
        size_t n = std::fread(p + total, 1, count - total, stream);
        if (n == 0) {
            if (std::feof(stream) || std::ferror(stream)) return false;
        }
        total += n;
    }
    return true;
}

bool ReadExact(FILE* stream, void* buf, size_t count) {
    return ReadExact(buf, count, stream);
}

bool WriteExact(const void* buf, size_t count, FILE* stream) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t total = 0;
    while (total < count) {
        size_t n = std::fwrite(p + total, 1, count - total, stream);
        if (n == 0) return false;
        total += n;
    }
    std::fflush(stream);
    return true;
}

std::string ExtractJsonRpcId(const std::string& payload) {
    auto pos = payload.find("\"id\"");
    if (pos == std::string::npos) return "null";
    pos += 4;
    while (pos < payload.size() && (payload[pos] == ' ' || payload[pos] == ':')) ++pos;
    if (pos >= payload.size()) return "null";
    if (payload[pos] == '"') {
        auto end_pos = payload.find('"', pos + 1);
        if (end_pos != std::string::npos) {
            return payload.substr(pos, end_pos - pos + 1);
        }
    } else {
        auto end_pos = payload.find_first_of(",}\r\n \t", pos);
        if (end_pos != std::string::npos) {
            return payload.substr(pos, end_pos - pos);
        }
    }
    return "null";
}

void WriteStdoutFrame(const std::string& json_str) {
    if (json_str.size() > 1024 * 1024) {
        // Native messaging frame limit is 1 MiB (1,048,576 bytes). Send an RPC error frame instead of triggering browser host termination.
        std::string err_frame = "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"Response exceeds 1MB limit\"}}";
        uint32_t err_len = static_cast<uint32_t>(err_frame.size());
        WriteExact(&err_len, sizeof(err_len), stdout);
        WriteExact(err_frame.data(), err_frame.size(), stdout);
        return;
    }
    uint32_t len = static_cast<uint32_t>(json_str.size());
    WriteExact(&len, sizeof(len), stdout);
    WriteExact(json_str.data(), json_str.size(), stdout);
}

#ifdef _WIN32
bool WritePipeExact(HANDLE hPipe, const void* buf, DWORD count) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    DWORD total = 0;
    while (total < count) {
        DWORD written = 0;
        if (!WriteFile(hPipe, p + total, count - total, &written, nullptr) || written == 0) {
            return false;
        }
        total += written;
    }
    return true;
}

bool ReadPipeExact(HANDLE hPipe, void* buf, DWORD count) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    DWORD total = 0;
    while (total < count) {
        DWORD chunk = 0;
        if (!ReadFile(hPipe, p + total, count - total, &chunk, nullptr) || chunk == 0) {
            return false;
        }
        total += chunk;
    }
    return true;
}
#endif

#ifdef _WIN32
HANDLE ConnectToDesktopPipe(const std::string& pipe_path, const std::filesystem::path& daemon_bin_path) {
    std::wstring wpipe(pipe_path.begin(), pipe_path.end());
    auto try_connect = [&]() -> HANDLE {
        for (int retry = 0; retry < 5; ++retry) {
            HANDLE h = CreateFileW(wpipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) return h;
            DWORD err = GetLastError();
            if (err == ERROR_PIPE_BUSY) {
                if (WaitNamedPipeW(wpipe.c_str(), 3000)) {
                    continue;
                }
            }
            break;
        }
        return INVALID_HANDLE_VALUE;
    };

    HANDLE h = try_connect();
    if (h != INVALID_HANDLE_VALUE) return h;

    if (!daemon_bin_path.empty() && std::filesystem::exists(daemon_bin_path)) {
        // Prevent launch storms across concurrent NMH processes:
        // Use an interprocess named mutex so only one NMH process attempts to launch the background daemon.
        HANDLE hLaunchMutex = CreateMutexW(nullptr, FALSE, L"Local\\Radium_NMH_Launch_Mutex");
        DWORD wait_res = WAIT_TIMEOUT;
        if (hLaunchMutex != nullptr && hLaunchMutex != INVALID_HANDLE_VALUE) {
            wait_res = WaitForSingleObject(hLaunchMutex, 3000);
        }

        // Inside the mutex, re-check if another instance already started the daemon while we were waiting
        h = try_connect();
        if (h != INVALID_HANDLE_VALUE) {
            if (hLaunchMutex && (wait_res == WAIT_OBJECT_0 || wait_res == WAIT_ABANDONED)) {
                ReleaseMutex(hLaunchMutex);
                CloseHandle(hLaunchMutex);
            } else if (hLaunchMutex) {
                CloseHandle(hLaunchMutex);
            }
            return h;
        }

        // Auto-launch the desktop application in minimized tray mode so the Win32 message loop and New Download dialogs are active
        std::wstring cmd = L"\"" + daemon_bin_path.wstring() + L"\" --minimized --ipc-socket \"" + wpipe + L"\"";
        STARTUPINFOW si{sizeof(STARTUPINFOW)};
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        DWORD creation_flags = DETACHED_PROCESS;
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, creation_flags, nullptr, nullptr, &si, &pi)) {
            AllowSetForegroundWindow(pi.dwProcessId);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }

        for (int attempt = 0; attempt < 50; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            h = try_connect();
            if (h != INVALID_HANDLE_VALUE) break;
        }

        if (hLaunchMutex && (wait_res == WAIT_OBJECT_0 || wait_res == WAIT_ABANDONED)) {
            ReleaseMutex(hLaunchMutex);
            CloseHandle(hLaunchMutex);
        } else if (hLaunchMutex) {
            CloseHandle(hLaunchMutex);
        }
        return h;
    }
    return INVALID_HANDLE_VALUE;
}
#else
int ConnectToDesktopIpc(const std::string& sock_path, const std::string& daemon_bin_path) {
    auto try_connect = [&]() -> int {
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);
        if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
            return fd;
        }
        ::close(fd);
        return -1;
    };

    int fd = try_connect();
    if (fd >= 0) return fd;

    if (!daemon_bin_path.empty() && std::filesystem::exists(daemon_bin_path)) {
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            execl(daemon_bin_path.c_str(), daemon_bin_path.c_str(), "--daemon", "--ipc-socket", sock_path.c_str(), nullptr);
            _exit(127);
        }
        for (int attempt = 0; attempt < 25; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            fd = try_connect();
            if (fd >= 0) return fd;
        }
    }
    return -1;
}
#endif

} // namespace

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // Critical on Windows: prevent CRT from translating 0x0A (\n) <-> 0x0D 0x0A (\r\n) on Native Messaging stdio!
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    std::string sock_path = openidm::ipc::GetDefaultIpcSocketPath();
    if (const char* env_flux = std::getenv("FLUX_IPC_SOCKET")) {
        sock_path = env_flux;
    } else if (const char* env_old = std::getenv("OPENIDM_IPC_SOCKET")) {
        sock_path = env_old;
    }
    std::filesystem::path daemon_bin;
    if (argc > 0 && argv[0]) {
        std::filesystem::path self_path = std::filesystem::path(argv[0]);
#ifdef _WIN32
        std::filesystem::path candidate = self_path.parent_path() / "Radium.exe";
        if (!std::filesystem::exists(candidate)) {
            candidate = self_path.parent_path() / "radium.exe";
        }
        if (!std::filesystem::exists(candidate)) {
            candidate = self_path.parent_path() / "Flux.exe";
        }
        if (!std::filesystem::exists(candidate)) {
            candidate = self_path.parent_path() / "flux.exe";
        }
        if (!std::filesystem::exists(candidate)) {
            candidate = self_path.parent_path() / "openidm.exe";
        }
#else
        std::filesystem::path candidate = self_path.parent_path() / "radium";
        if (!std::filesystem::exists(candidate)) {
            candidate = self_path.parent_path() / "flux";
        }
        if (!std::filesystem::exists(candidate)) {
            candidate = self_path.parent_path() / "openidm";
        }
#endif
        if (std::filesystem::exists(candidate)) {
            daemon_bin = candidate;
        }
    }

#ifdef _WIN32
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    auto last_failed_launch = std::chrono::steady_clock::time_point{};

    while (true) {
        uint32_t in_len = 0;
        if (!ReadExact(stdin, &in_len, sizeof(in_len))) break;
        if (in_len == 0 || in_len > 1048576) {
            WriteStdoutFrame(R"({"jsonrpc":"2.0","id":null,"error":{"code":-32600,"message":"ERR_IPC_FRAME_SIZE"}})");
            break;
        }
        std::string payload(in_len, '\0');
        if (!ReadExact(stdin, payload.data(), in_len)) break;

        std::string json_id = ExtractJsonRpcId(payload);

        if (hPipe == INVALID_HANDLE_VALUE) {
            auto now = std::chrono::steady_clock::now();
            if (last_failed_launch.time_since_epoch().count() != 0 &&
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last_failed_launch).count() < 3000) {
                // Quickly try connect without re-launching process within 3s of failure
                std::wstring wpipe(sock_path.begin(), sock_path.end());
                hPipe = CreateFileW(wpipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            } else {
                hPipe = ConnectToDesktopPipe(sock_path, daemon_bin);
                if (hPipe == INVALID_HANDLE_VALUE) {
                    last_failed_launch = std::chrono::steady_clock::now();
                }
            }
        }
        if (hPipe == INVALID_HANDLE_VALUE) {
            std::string err = R"({"jsonrpc":"2.0","id":)" + json_id + R"(,"error":{"code":-32003,"message":"ERR_IPC_DESKTOP_UNAVAILABLE"}})";
            WriteStdoutFrame(err);
            continue;
        }

#ifdef _WIN32
        AllowSetForegroundWindow(ASFW_ANY);
#endif

        if (!WritePipeExact(hPipe, &in_len, sizeof(in_len)) ||
            !WritePipeExact(hPipe, payload.data(), in_len)) {
            CloseHandle(hPipe);
            hPipe = INVALID_HANDLE_VALUE;
            std::string err = R"({"jsonrpc":"2.0","id":)" + json_id + R"(,"error":{"code":-32003,"message":"ERR_IPC_DISCONNECTED"}})";
            WriteStdoutFrame(err);
            continue;
        }

        uint32_t out_len = 0;
        if (!ReadPipeExact(hPipe, &out_len, sizeof(out_len)) || out_len == 0 || out_len > 1048576) {
            CloseHandle(hPipe);
            hPipe = INVALID_HANDLE_VALUE;
            std::string err = R"({"jsonrpc":"2.0","id":)" + json_id + R"(,"error":{"code":-32003,"message":"ERR_IPC_BAD_RESPONSE"}})";
            WriteStdoutFrame(err);
            continue;
        }
        std::string resp(out_len, '\0');
        if (!ReadPipeExact(hPipe, resp.data(), out_len)) {
            CloseHandle(hPipe);
            hPipe = INVALID_HANDLE_VALUE;
            std::string err = R"({"jsonrpc":"2.0","id":)" + json_id + R"(,"error":{"code":-32003,"message":"ERR_IPC_BAD_RESPONSE"}})";
            WriteStdoutFrame(err);
            continue;
        }
        WriteStdoutFrame(resp);
    }
    if (hPipe != INVALID_HANDLE_VALUE) CloseHandle(hPipe);
#else
    int ipc_fd = -1;
    while (true) {
        uint32_t in_len = 0;
        if (!ReadExact(stdin, &in_len, sizeof(in_len))) {
            break;
        }
        if (in_len == 0 || in_len > 1048576) {
            WriteStdoutFrame(R"({"jsonrpc":"2.0","id":null,"error":{"code":-32600,"message":"ERR_IPC_FRAME_SIZE"}})");
            break;
        }
        std::string payload(in_len, '\0');
        if (!ReadExact(stdin, payload.data(), in_len)) {
            break;
        }

        std::string json_id = ExtractJsonRpcId(payload);

        if (ipc_fd < 0) {
            ipc_fd = ConnectToDesktopIpc(sock_path, daemon_bin.string());
        }
        if (ipc_fd < 0) {
            std::string err = R"({"jsonrpc":"2.0","id":)" + json_id + R"(,"error":{"code":-32003,"message":"ERR_IPC_DESKTOP_UNAVAILABLE"}})";
            WriteStdoutFrame(err);
            continue;
        }

#ifdef MSG_NOSIGNAL
        int send_flags = MSG_NOSIGNAL;
#else
        int send_flags = 0;
#endif
        if (::send(ipc_fd, &in_len, sizeof(in_len), send_flags) != sizeof(in_len) ||
            ::send(ipc_fd, payload.data(), in_len, send_flags) != static_cast<ssize_t>(in_len)) {
            ::close(ipc_fd);
            ipc_fd = -1;
            std::string err = R"({"jsonrpc":"2.0","id":)" + json_id + R"(,"error":{"code":-32003,"message":"ERR_IPC_DISCONNECTED"}})";
            WriteStdoutFrame(err);
            continue;
        }

        uint32_t out_len = 0;
        if (::recv(ipc_fd, &out_len, sizeof(out_len), MSG_WAITALL) != sizeof(out_len) ||
            out_len == 0 || out_len > 1048576) {
            ::close(ipc_fd);
            ipc_fd = -1;
            std::string err = R"({"jsonrpc":"2.0","id":)" + json_id + R"(,"error":{"code":-32003,"message":"ERR_IPC_BAD_RESPONSE"}})";
            WriteStdoutFrame(err);
            continue;
        }

        std::string resp(out_len, '\0');
        if (::recv(ipc_fd, resp.data(), out_len, MSG_WAITALL) != static_cast<ssize_t>(out_len)) {
            ::close(ipc_fd);
            ipc_fd = -1;
            break;
        }
        WriteStdoutFrame(resp);
    }

    if (ipc_fd >= 0) ::close(ipc_fd);
#endif
    return 0;
}
