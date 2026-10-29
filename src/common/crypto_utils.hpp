// SPDX-License-Identifier: Apache-2.0
// OpenIDM — File Checksum Verification (SHA-256 / MD5) & Credential Protection (DPAPI / AES-256-GCM)
// Implements FR-007, NFR-009, ADR-0007.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "common/types.hpp"

namespace openidm {

class CryptoUtils {
public:
    static Result<std::string> ComputeFileSha256Hex(const std::filesystem::path& file_path);
    static std::string ComputeBufferSha256Hex(std::string_view data);
    static std::string ComputeBufferSha1Hex(std::string_view data);
    static std::vector<uint8_t> ComputeBufferSha1Raw(const uint8_t* data, size_t len);

    // Encrypts sensitive session headers (Cookie, Authorization) at rest.
    // Uses Windows DPAPI (CryptProtectData) on Windows 11, and OpenSSL AES-256-GCM with per-user OS key on POSIX.
    static Result<std::vector<uint8_t>> ProtectSecret(std::string_view plaintext);
    static Result<std::string> UnprotectSecret(const std::vector<uint8_t>& ciphertext);

    // RFC 4648 Base64 encoding and decoding
    static std::string Base64Encode(std::string_view data);
    static std::vector<uint8_t> Base64Decode(std::string_view data);
};

} // namespace openidm
