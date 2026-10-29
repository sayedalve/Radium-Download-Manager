// SPDX-License-Identifier: Apache-2.0
#include "common/logger.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>

namespace openidm {

Logger& Logger::Instance() {
    static Logger inst;
    return inst;
}

void Logger::SetLogFile(const std::filesystem::path& log_path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_log_path = log_path;
    std::error_code ec;
    if (m_log_path.has_parent_path()) {
        std::filesystem::create_directories(m_log_path.parent_path(), ec);
    }
}

std::string Logger::RedactSensitiveData(std::string_view input) {
    std::string out(input);

    // 1. Redact HTTP headers: Cookie, Set-Cookie, Authorization, Proxy-Authorization, X-Auth-Token, X-Api-Key
    static const std::regex kHeaderRegex(
        R"(((?:Cookie|Set-Cookie|Authorization|Proxy-Authorization|X-Auth-Token|X-Api-Key)\s*:\s*)([^\r\n"]+))",
        std::regex_constants::icase);
    out = std::regex_replace(out, kHeaderRegex, "$1[REDACTED]");

    // 2. Redact JSON fields: "Cookie": "...", "cookie": "...", "Authorization": "..."
    static const std::regex kJsonCookieRegex(
        R"re(("(?:cookie|set-cookie|authorization|proxy-authorization)"\s*:\s*")([^"]*)("))re",
        std::regex_constants::icase);
    out = std::regex_replace(out, kJsonCookieRegex, "$1[REDACTED]$3");

    // 3. Redact sensitive URL query string tokens
    static const std::regex kQueryTokenRegex(
        R"(([?&](?:token|access_token|auth|sig|signature|hdnea|Policy|Signature|Key-Pair-Id|X-Amz-Signature|X-Amz-Credential|X-Amz-Security-Token)=)([^&\s"']+))",
        std::regex_constants::icase);
    out = std::regex_replace(out, kQueryTokenRegex, "$1[REDACTED]");

    return out;
}

void Logger::Log(LogLevel level, std::string_view component, std::string_view message) {
    std::string redacted = RedactSensitiveData(message);
    const char* lvl_str = "INFO";
    switch (level) {
        case LogLevel::kDebug: lvl_str = "DEBUG"; break;
        case LogLevel::kInfo:  lvl_str = "INFO";  break;
        case LogLevel::kWarn:  lvl_str = "WARN";  break;
        case LogLevel::kError: lvl_str = "ERROR"; break;
    }

    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &tt);
#else
    gmtime_r(&tt, &tm_buf);
#endif
    std::ostringstream line;
    line << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ")
         << " [" << lvl_str << "] [" << component << "] " << redacted;

    std::string formatted = line.str();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_ring_buffer.size() >= 500) {
        m_ring_buffer.erase(m_ring_buffer.begin());
    }
    m_ring_buffer.push_back(formatted);

    if (!m_log_path.empty()) {
        std::ofstream ofs(m_log_path, std::ios::app);
        if (ofs) {
            ofs << formatted << "\n";
        }
    }
}

std::vector<std::string> Logger::GetRecentLines(size_t max_lines) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_ring_buffer.size() <= max_lines) return m_ring_buffer;
    return std::vector<std::string>(m_ring_buffer.end() - max_lines, m_ring_buffer.end());
}

} // namespace openidm
