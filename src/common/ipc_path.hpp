// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdlib>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>
#include <vector>
#else
#include <unistd.h>
#endif

namespace openidm::ipc {

#ifdef _WIN32
inline std::string GetCurrentUserSidString() {
    std::string sid_str;
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        DWORD len = 0;
        GetTokenInformation(hToken, TokenUser, NULL, 0, &len);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && len > 0) {
            std::vector<BYTE> buf(len);
            if (GetTokenInformation(hToken, TokenUser, buf.data(), len, &len)) {
                TOKEN_USER* pUser = reinterpret_cast<TOKEN_USER*>(buf.data());
                LPSTR str = nullptr;
                if (ConvertSidToStringSidA(pUser->User.Sid, &str) && str) {
                    sid_str = str;
                    LocalFree(str);
                }
            }
        }
        CloseHandle(hToken);
    }
    return sid_str;
}
#endif

inline std::string GetDefaultIpcSocketPath() {
    if (const char* env_radium = std::getenv("RADIUM_IPC_SOCKET")) {
        if (env_radium[0] != '\0') return env_radium;
    }
    if (const char* env_flux = std::getenv("FLUX_IPC_SOCKET")) {
        if (env_flux[0] != '\0') return env_flux;
    }
    if (const char* env_old = std::getenv("OPENIDM_IPC_SOCKET")) {
        if (env_old[0] != '\0') return env_old;
    }
#ifdef _WIN32
    std::string user_sid = GetCurrentUserSidString();
    if (!user_sid.empty()) {
        return "\\\\.\\pipe\\Flux_IPC_" + user_sid;
    }
    char uname[256] = {0};
    DWORD ulen = sizeof(uname);
    if (GetUserNameA(uname, &ulen) && uname[0] != '\0') {
        return "\\\\.\\pipe\\Flux_IPC_" + std::string(uname);
    }
    return "\\\\.\\pipe\\Flux_IPC_Default";
#else
    return "/tmp/flux_ipc_" + std::to_string(geteuid()) + ".sock";
#endif
}

} // namespace openidm::ipc
