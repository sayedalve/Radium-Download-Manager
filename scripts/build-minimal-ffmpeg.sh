#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Radium Download Manager -- Minimal Windows x64 FFmpeg Cross-Compilation Script
#
# Builds a deterministic minimal Windows x64 FFmpeg executable (ffmpeg.exe)
# from pinned sources and configurations on Linux / POSIX cross-compilation environments.
#
# Pinned Source: FFmpeg 7.1 (Tag: n7.1, Commit: 04706f3, Tarball: ffmpeg-7.1.tar.xz)
# Pinned Source SHA-256: 40973d44d00a8999824ff4ec27549efdbdc7667a488e14603954625b5a79667
# Pinned Toolchain: LLVM-MinGW or MinGW-w64 (x86_64-w64-mingw32)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
CONFIG_FILE="${SCRIPT_DIR}/ffmpeg-minimal.config"
WORK_DIR="${REPO_ROOT}/build/ffmpeg-minimal"
OUTPUT_DIR="${REPO_ROOT}/tools/win-x64"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

FFMPEG_VERSION="7.1"
FFMPEG_TARBALL="ffmpeg-${FFMPEG_VERSION}.tar.xz"
FFMPEG_URL="https://ffmpeg.org/releases/${FFMPEG_TARBALL}"
FFMPEG_EXPECTED_SHA256="40973d44d00a8999824ff4ec27549efdbdc7667a488e14603954625b5a79667"

echo "================================================================="
echo "   Flux Download Manager -- Minimal FFmpeg Cross-Compilation    "
echo "================================================================="
echo "Configuration: ${CONFIG_FILE}"
echo "Work Dir     : ${WORK_DIR}"
echo "Output Dir   : ${OUTPUT_DIR}"
echo "Jobs         : ${JOBS}"

mkdir -p "${WORK_DIR}" "${OUTPUT_DIR}"

# 1. Download and Verify FFmpeg Source
if [ ! -d "${WORK_DIR}/ffmpeg-${FFMPEG_VERSION}" ]; then
    if [ ! -f "${WORK_DIR}/${FFMPEG_TARBALL}" ]; then
        echo "==> Downloading FFmpeg ${FFMPEG_VERSION} source tarball..."
        curl -fsSL -o "${WORK_DIR}/${FFMPEG_TARBALL}" "${FFMPEG_URL}"
    fi

    echo "==> Verifying SHA-256 checksum..."
    ACTUAL_SHA256="$(sha256sum "${WORK_DIR}/${FFMPEG_TARBALL}" | awk '{print $1}')"
    if [ "${ACTUAL_SHA256}" != "${FFMPEG_EXPECTED_SHA256}" ]; then
        echo "ERROR: SHA-256 mismatch for ${FFMPEG_TARBALL}!"
        echo "Expected: ${FFMPEG_EXPECTED_SHA256}"
        echo "Actual  : ${ACTUAL_SHA256}"
        exit 1
    fi

    echo "==> Extracting source archive..."
    tar -xf "${WORK_DIR}/${FFMPEG_TARBALL}" -C "${WORK_DIR}"
fi

cd "${WORK_DIR}/ffmpeg-${FFMPEG_VERSION}"

# 2. Extract Flags from Config File
FLAGS=()
while IFS= read -r line || [ -n "$line" ]; do
    # Trim leading/trailing whitespace
    trimmed="$(echo "$line" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"
    # Skip empty lines and comments
    if [ -z "$trimmed" ] || [[ "$trimmed" =~ ^# ]]; then
        continue
    fi
    # Strip trailing comments
    trimmed="$(echo "$trimmed" | sed -e 's/[[:space:]]*#.*//')"
    FLAGS+=("$trimmed")
done < "${CONFIG_FILE}"

# 3. Configure
echo "==> Running ./configure with minimal flags (${#FLAGS[@]} flags)..."
./configure "${FLAGS[@]}"

# 4. Compile
echo "==> Compiling ffmpeg.exe..."
sed -i 's/LDFLAGS= */LDFLAGS=-static /' ffbuild/config.mak
make -j"${JOBS}" ffmpeg.exe

# 5. Strip and Stage
echo "==> Stripping executable and copying to output..."
STRIP_BIN="x86_64-w64-mingw32-strip"
if ! command -v "${STRIP_BIN}" >/dev/null 2>&1; then
    STRIP_BIN="llvm-strip"
fi

if command -v "${STRIP_BIN}" >/dev/null 2>&1; then
    "${STRIP_BIN}" --strip-all ffmpeg.exe
fi

cp -f ffmpeg.exe "${OUTPUT_DIR}/ffmpeg.exe"

SIZE_BYTES="$(wc -c < "${OUTPUT_DIR}/ffmpeg.exe")"
SIZE_MIB="$(echo "scale=2; ${SIZE_BYTES} / 1048576" | bc 2>/dev/null || echo "N/A")"

echo "================================================================="
echo "   Build Succeeded!                                              "
echo "   Output: ${OUTPUT_DIR}/ffmpeg.exe"
echo "   Size  : ${SIZE_BYTES} bytes (${SIZE_MIB} MiB)"
echo "================================================================="
