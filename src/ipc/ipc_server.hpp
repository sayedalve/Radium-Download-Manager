// SPDX-License-Identifier: Apache-2.0
// OpenIDM — Local IPC Server & JSON-RPC 2.0 Dispatcher
// Uses Windows Named Pipe (\\.\pipe\OpenIDM_IPC_<UserSID>) on Windows 11 and
// user-ACL-restricted UNIX Domain Socket (/tmp/openidm_ipc_<uid>.sock, chmod 0600) on POSIX.
// Implements NATIVE_MESSAGING_AND_IPC.md, ADR-0003, NFR-007, NFR-008, Phase 6.
#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "core/download_controller.hpp"

namespace openidm::ipc {

class IpcServer {
public:
    IpcServer(DownloadController& controller, std::string socket_path = "");
    ~IpcServer();

    Result<void> Start();
    void Stop();

    std::string GetEndpointPath() const { return m_socket_path; }

    // Direct JSON-RPC 2.0 request handler (used both over socket/pipe and in unit tests)
    std::string HandleJsonRpcMessage(std::string_view raw_json);

    void SetCaptureEnabled(bool enabled) { m_capture_enabled.store(enabled); }
    bool IsCaptureEnabled() const { return m_capture_enabled.load(); }

    void SetHeadlessDaemonMode(bool headless) { m_is_headless.store(headless); }
    bool IsHeadlessDaemonMode() const { return m_is_headless.load(); }

    using OfferNotificationCallback = std::function<void(const TaskModel& task)>;
    void SetOfferNotificationCallback(OfferNotificationCallback cb) {
        std::vector<TaskModel> pending;
        OfferNotificationCallback active_cb;
        {
            std::lock_guard<std::mutex> lock(m_offer_mutex);
            m_offer_cb = std::move(cb);
            active_cb = m_offer_cb;
            if (active_cb && !m_pending_offers.empty()) {
                pending = std::move(m_pending_offers);
                m_pending_offers.clear();
            }
        }
        if (active_cb) {
            for (const auto& task : pending) {
                active_cb(task);
            }
        }
    }

    static std::string DefaultSocketPath();

private:
    void AcceptLoop();
    void HandleClientConnection(int client_fd);

    DownloadController& m_controller;
    std::string m_socket_path;
    int m_listen_fd{-1};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_capture_enabled{true};
    std::atomic<bool> m_is_headless{false};
    std::mutex m_offer_mutex;
    OfferNotificationCallback m_offer_cb;
    std::vector<TaskModel> m_pending_offers;
    std::thread m_accept_thread;

    struct ClientWorkerEntry {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done{std::make_shared<std::atomic<bool>>(false)};
    };

    std::mutex m_clients_mutex;
    std::vector<ClientWorkerEntry> m_client_threads;
#ifdef _WIN32
    HANDLE m_first_pipe_handle{INVALID_HANDLE_VALUE};
    std::vector<HANDLE> m_active_pipe_handles;
#else
    std::vector<int> m_active_client_fds;
#endif

    struct StagingSession {
        std::string task_id;
        std::string filename;
        std::string page_url;
        std::string correlation_id;
        std::string save_dir;
        uint64_t expected_total_bytes{0};
        uint64_t bytes_written{0};
        uint32_t next_chunk_index{0};
        std::filesystem::path staging_path;
        std::shared_ptr<std::ofstream> file_stream;
        int64_t created_at_ms{0};
    };

    std::mutex m_staging_mutex;
    std::map<std::string, StagingSession> m_staging_sessions;
};

} // namespace openidm::ipc
