// SPDX-License-Identifier: Apache-2.0
#include "common/crypto_utils.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <dpapi.h>
#include <bcrypt.h>
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")
#else
#include <unistd.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#endif

namespace openidm {

namespace {
std::string BytesToHex(const unsigned char* buf, size_t len) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) {
        oss << std::setw(2) << static_cast<unsigned int>(buf[i]);
    }
    return oss.str();
}

#ifndef _WIN32
void DeriveUserLocalKey(unsigned char key_out[32]) {
    std::string seed = "OpenIDM_DPAPI_Equivalent_UserKey_v1_UID_" + std::to_string(geteuid());
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    unsigned int len = 32;
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, seed.data(), seed.size());
    EVP_DigestFinal_ex(ctx, key_out, &len);
    EVP_MD_CTX_free(ctx);
}
#endif
} // namespace

std::string CryptoUtils::ComputeBufferSha256Hex(std::string_view data) {
#ifdef _WIN32
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    unsigned char hash[32]{};
    BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0);
    BCryptHashData(hHash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), 0);
    BCryptFinishHash(hHash, hash, sizeof(hash), 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return BytesToHex(hash, 32);
#else
    unsigned char hash[32]{};
    unsigned int len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, data.data(), data.size());
    EVP_DigestFinal_ex(ctx, hash, &len);
    EVP_MD_CTX_free(ctx);
    return BytesToHex(hash, 32);
#endif
}

std::string CryptoUtils::ComputeBufferSha1Hex(std::string_view data) {
    auto raw = ComputeBufferSha1Raw(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    return BytesToHex(raw.data(), raw.size());
}

std::vector<uint8_t> CryptoUtils::ComputeBufferSha1Raw(const uint8_t* data, size_t len) {
    std::vector<uint8_t> out(20, 0);
#ifdef _WIN32
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) == 0) {
            BCryptHashData(hHash, reinterpret_cast<PUCHAR>(const_cast<uint8_t*>(data)), static_cast<ULONG>(len), 0);
            BCryptFinishHash(hHash, out.data(), static_cast<ULONG>(out.size()), 0);
            BCryptDestroyHash(hHash);
        }
        BCryptCloseAlgorithmProvider(hAlg, 0);
    }
#else
    unsigned int out_len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx) {
        EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr);
        EVP_DigestUpdate(ctx, data, len);
        EVP_DigestFinal_ex(ctx, out.data(), &out_len);
        EVP_MD_CTX_free(ctx);
    }
#endif
    return out;
}

Result<std::string> CryptoUtils::ComputeFileSha256Hex(const std::filesystem::path& file_path) {
    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs) {
        return Result<std::string>::Err("ERR_STORAGE_READ", "Cannot open file for SHA-256: " + file_path.string());
    }
#ifdef _WIN32
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    unsigned char hash[32]{};
    BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0);
    std::vector<char> buf(65536);
    while (ifs) {
        ifs.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        std::streamsize got = ifs.gcount();
        if (got > 0) {
            BCryptHashData(hHash, reinterpret_cast<PUCHAR>(buf.data()), static_cast<ULONG>(got), 0);
        }
    }
    BCryptFinishHash(hHash, hash, sizeof(hash), 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return Result<std::string>::Ok(BytesToHex(hash, 32));
#else
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    std::vector<char> buf(65536);
    while (ifs) {
        ifs.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        std::streamsize got = ifs.gcount();
        if (got > 0) {
            EVP_DigestUpdate(ctx, buf.data(), static_cast<size_t>(got));
        }
    }
    unsigned char hash[32]{};
    unsigned int len = 0;
    EVP_DigestFinal_ex(ctx, hash, &len);
    EVP_MD_CTX_free(ctx);
    return Result<std::string>::Ok(BytesToHex(hash, 32));
#endif
}

Result<std::vector<uint8_t>> CryptoUtils::ProtectSecret(std::string_view plaintext) {
    if (plaintext.empty()) return Result<std::vector<uint8_t>>::Ok({});
#ifdef _WIN32
    DATA_BLOB in_blob{static_cast<DWORD>(plaintext.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()))};
    DATA_BLOB out_blob{};
    if (!CryptProtectData(&in_blob, L"OpenIDM_Credential", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out_blob)) {
        return Result<std::vector<uint8_t>>::Err("ERR_CRYPTO_DPAPI", "CryptProtectData failed");
    }
    std::vector<uint8_t> res(out_blob.pbData, out_blob.pbData + out_blob.cbData);
    LocalFree(out_blob.pbData);
    return Result<std::vector<uint8_t>>::Ok(std::move(res));
#else
    // AES-256-GCM authenticated encryption: [12-byte IV][16-byte Tag][Ciphertext]
    unsigned char key[32]{};
    DeriveUserLocalKey(key);
    unsigned char iv[12]{};
    if (RAND_bytes(iv, sizeof(iv)) != 1) {
        return Result<std::vector<uint8_t>>::Err("ERR_CRYPTO_RAND", "RAND_bytes failed");
    }
    std::vector<uint8_t> out(12 + 16 + plaintext.size());
    std::memcpy(out.data(), iv, 12);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int len = 0;
    int ciphertext_len = 0;
    EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr);
    EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, iv);
    EVP_EncryptUpdate(ctx, out.data() + 28, &len, reinterpret_cast<const unsigned char*>(plaintext.data()), static_cast<int>(plaintext.size()));
    ciphertext_len = len;
    EVP_EncryptFinal_ex(ctx, out.data() + 28 + len, &len);
    ciphertext_len += len;
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, out.data() + 12);
    EVP_CIPHER_CTX_free(ctx);
    out.resize(28 + ciphertext_len);
    return Result<std::vector<uint8_t>>::Ok(std::move(out));
#endif
}

Result<std::string> CryptoUtils::UnprotectSecret(const std::vector<uint8_t>& ciphertext) {
    if (ciphertext.empty()) return Result<std::string>::Ok("");
#ifdef _WIN32
    DATA_BLOB in_blob{static_cast<DWORD>(ciphertext.size()), const_cast<BYTE*>(ciphertext.data())};
    DATA_BLOB out_blob{};
    if (!CryptUnprotectData(&in_blob, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out_blob)) {
        return Result<std::string>::Err("ERR_CRYPTO_DPAPI", "CryptUnprotectData failed");
    }
    std::string res(reinterpret_cast<const char*>(out_blob.pbData), out_blob.cbData);
    SecureZeroMemory(out_blob.pbData, out_blob.cbData);
    LocalFree(out_blob.pbData);
    return Result<std::string>::Ok(std::move(res));
#else
    if (ciphertext.size() < 28) {
        return Result<std::string>::Err("ERR_CRYPTO_INVALID_BLOB", "Ciphertext too short");
    }
    unsigned char key[32]{};
    DeriveUserLocalKey(key);
    const unsigned char* iv = ciphertext.data();
    const unsigned char* tag = ciphertext.data() + 12;
    const unsigned char* enc = ciphertext.data() + 28;
    int enc_len = static_cast<int>(ciphertext.size() - 28);

    std::string plain(enc_len, '\0');
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int len = 0;
    EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr);
    EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, iv);
    EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(plain.data()), &len, enc, enc_len);
    int total_len = len;
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, const_cast<unsigned char*>(tag));
    int ret = EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(plain.data()) + len, &len);
    EVP_CIPHER_CTX_free(ctx);
    if (ret <= 0) {
        return Result<std::string>::Err("ERR_CRYPTO_AUTH_FAILED", "AES-256-GCM tag verification failed");
    }
    total_len += len;
    plain.resize(total_len);
    return Result<std::string>::Ok(std::move(plain));
#endif
}

std::string CryptoUtils::Base64Encode(std::string_view data) {
    static const char kChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    size_t i = 0;
    while (i < data.size()) {
        uint32_t octet_a = static_cast<uint8_t>(data[i++]);
        uint32_t octet_b = (i < data.size()) ? static_cast<uint8_t>(data[i++]) : 0;
        uint32_t octet_c = (i < data.size()) ? static_cast<uint8_t>(data[i++]) : 0;

        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        out.push_back(kChars[(triple >> 18) & 0x3F]);
        out.push_back(kChars[(triple >> 12) & 0x3F]);
        out.push_back((i > data.size() + 1) ? '=' : kChars[(triple >> 6) & 0x3F]);
        out.push_back((i > data.size()) ? '=' : kChars[triple & 0x3F]);
    }
    return out;
}

std::vector<uint8_t> CryptoUtils::Base64Decode(std::string_view data) {
    static const int8_t kLookup[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    std::vector<uint8_t> out;
    out.reserve((data.size() * 3) / 4);
    uint32_t val = 0;
    int valb = -8;
    for (char c : data) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ') continue;
        int8_t d = kLookup[static_cast<uint8_t>(c)];
        if (d == -1) continue;
        val = (val << 6) | static_cast<uint32_t>(d);
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

} // namespace openidm
