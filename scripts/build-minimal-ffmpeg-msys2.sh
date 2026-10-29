#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Radium Download Manager -- Minimal Windows x64 FFmpeg Build (MSYS2/MinGW-w64)
#
# Run inside MSYS2 MINGW64 shell:
#   bash scripts/build-minimal-ffmpeg-msys2.sh
#
# Pinned Source: FFmpeg 7.1 (Commit: 04706f3)
# Pinned Source SHA-256: 40973d44970dbc83ef302b0609f2e74982be2d85916dd2ee7472d30678a7abe6
# License Profile: LGPL-2.1-or-later / LGPL-3.0 (No GPL codecs)
# Expected Output: tools/win-x64/ffmpeg.exe (~12-16 MB)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WORK_DIR="$REPO_ROOT/build/ffmpeg-minimal"
OUTPUT_DIR="$REPO_ROOT/tools/win-x64"
FFMPEG_VERSION="7.1"
FFMPEG_TARBALL="ffmpeg-$FFMPEG_VERSION.tar.xz"
FFMPEG_URL="https://ffmpeg.org/releases/$FFMPEG_TARBALL"
FFMPEG_SHA256="40973d44970dbc83ef302b0609f2e74982be2d85916dd2ee7472d30678a7abe6"
JOBS="${JOBS:-$(nproc)}"

echo "============================================================"
echo "  Flux Minimal FFmpeg Build (MSYS2/MinGW-w64)"
echo "  Work Dir  : $WORK_DIR"
echo "  Output    : $OUTPUT_DIR/ffmpeg.exe"
echo "  Jobs      : $JOBS"
echo "============================================================"

mkdir -p "$WORK_DIR" "$OUTPUT_DIR"

# 1. Verify toolchain
echo ""
echo "==> [1/6] Verifying toolchain..."
if ! command -v x86_64-w64-mingw32-gcc &>/dev/null; then
    echo "ERROR: x86_64-w64-mingw32-gcc not found. Install mingw-w64-x86_64-gcc."
    exit 1
fi
GCC_VERSION=$(x86_64-w64-mingw32-gcc --version | head -1)
echo "  GCC: $GCC_VERSION"
if ! command -v nasm &>/dev/null; then
    echo "WARNING: nasm not found - x264/x265 encoders won't build (not needed for Flux)."
fi

# 2. Download and verify source
echo ""
echo "==> [2/6] Provisioning FFmpeg $FFMPEG_VERSION source..."
cd "$WORK_DIR"

if [ ! -d "ffmpeg-$FFMPEG_VERSION" ]; then
    if [ ! -f "$FFMPEG_TARBALL" ]; then
        echo "  Downloading $FFMPEG_URL..."
        if command -v curl &>/dev/null; then
            curl -L -o "$FFMPEG_TARBALL" "$FFMPEG_URL"
        else
            wget -O "$FFMPEG_TARBALL" "$FFMPEG_URL"
        fi
    fi
    
    echo "  Verifying SHA-256..."
    ACTUAL_HASH=$(sha256sum "$FFMPEG_TARBALL" | awk '{print $1}')
    if [ "${ACTUAL_HASH,,}" != "${FFMPEG_SHA256,,}" ]; then
        echo "ERROR: SHA-256 mismatch!"
        echo "  Expected: $FFMPEG_SHA256"
        echo "  Actual  : $ACTUAL_HASH"
        exit 1
    fi
    echo "  SHA-256 verified OK."
    
    echo "  Extracting..."
    tar -xf "$FFMPEG_TARBALL"
fi

# 3. Configure
echo ""
echo "==> [3/6] Configuring minimal FFmpeg..."
cd "$WORK_DIR/ffmpeg-$FFMPEG_VERSION"

./configure \
    --arch=x86_64 \
    --target-os=mingw32 \
    --cross-prefix=x86_64-w64-mingw32- \
    --pkg-config=pkg-config \
    --pkg-config-flags="--static" \
    --enable-version3 \
    --enable-small \
    --enable-lto \
    --enable-pic \
    --disable-shared \
    --enable-static \
    --disable-debug \
    --disable-doc \
    --disable-htmlpages \
    --disable-manpages \
    --disable-podpages \
    --disable-txtpages \
    --disable-autodetect \
    --extra-cflags="-Os -flto -DNDEBUG -DWIN32_LEAN_AND_MEAN" \
    --extra-ldflags="-Wl,--gc-sections -flto" \
    --disable-all \
    --enable-avcodec \
    --enable-avformat \
    --enable-avutil \
    --enable-swresample \
    --enable-avfilter \
    --enable-ffmpeg \
    --disable-network \
    --disable-devices \
    --disable-indevs \
    --disable-outdevs \
    --enable-protocol=file \
    --enable-protocol=pipe \
    --enable-protocol=concat \
    --enable-protocol=subfile \
    --enable-protocol=cache \
    --enable-demuxer=mpegts \
    --enable-demuxer=mov \
    --enable-demuxer=matroska \
    --enable-demuxer=aac \
    --enable-demuxer=mp3 \
    --enable-demuxer=flac \
    --enable-demuxer=ogg \
    --enable-demuxer=wav \
    --enable-demuxer=webvtt \
    --enable-demuxer=srt \
    --enable-demuxer=concat \
    --enable-demuxer=rawvideo \
    --enable-muxer=mp4 \
    --enable-muxer=matroska \
    --enable-muxer=webm \
    --enable-muxer=ipod \
    --enable-muxer=mp3 \
    --enable-muxer=mpegts \
    --enable-muxer=ogg \
    --enable-muxer=opus \
    --enable-muxer=flac \
    --enable-muxer=wav \
    --enable-muxer=srt \
    --enable-muxer=webvtt \
    --enable-muxer=null \
    --enable-parser=h264 \
    --enable-parser=hevc \
    --enable-parser=av1 \
    --enable-parser=vp9 \
    --enable-parser=vp8 \
    --enable-parser=aac \
    --enable-parser=ac3 \
    --enable-parser=mpegaudio \
    --enable-parser=opus \
    --enable-parser=vorbis \
    --enable-parser=flac \
    --enable-decoder=h264 \
    --enable-decoder=hevc \
    --enable-decoder=av1 \
    --enable-decoder=vp9 \
    --enable-decoder=vp8 \
    --enable-decoder=aac \
    --enable-decoder=ac3 \
    --enable-decoder=eac3 \
    --enable-decoder=mp3 \
    --enable-decoder=mp3float \
    --enable-decoder=opus \
    --enable-decoder=vorbis \
    --enable-decoder=flac \
    --enable-decoder=pcm_s16le \
    --enable-decoder=pcm_s24le \
    --enable-decoder=pcm_s32le \
    --enable-decoder=pcm_f32le \
    --enable-decoder=movtext \
    --enable-decoder=subrip \
    --enable-decoder=webvtt \
    --enable-encoder=movtext \
    --enable-encoder=subrip \
    --enable-encoder=webvtt \
    --enable-encoder=aac \
    --enable-encoder=pcm_s16le \
    --enable-bsf=aac_adtstoasc \
    --enable-bsf=h264_mp4toannexb \
    --enable-bsf=hevc_mp4toannexb \
    --enable-bsf=extract_extradata \
    --enable-bsf=vp9_superframe \
    --enable-bsf=av1_frame_merge \
    --enable-bsf=null \
    --enable-filter=aformat \
    --enable-filter=anull \
    --enable-filter=null \
    --enable-filter=concat \
    --enable-filter=trim \
    --enable-filter=atrim

echo "==> Configure complete."

# 4. Compile
echo ""
echo "==> [4/6] Compiling (jobs=$JOBS)..."
make -j"$JOBS"

if [ ! -f "ffmpeg.exe" ]; then
    echo "ERROR: ffmpeg.exe not found after build!"
    exit 1
fi

echo "==> Compilation complete."

# 5. Strip
echo ""
echo "==> [5/6] Stripping debug symbols..."
if command -v x86_64-w64-mingw32-strip &>/dev/null; then
    x86_64-w64-mingw32-strip --strip-all ffmpeg.exe
    echo "  Stripped successfully with x86_64-w64-mingw32-strip."
elif command -v strip &>/dev/null; then
    strip --strip-all ffmpeg.exe
    echo "  Stripped successfully with strip."
else
    echo "  WARNING: strip not found, skipping."
fi

# 6. Stage output
echo ""
echo "==> [6/6] Staging output..."

# Backup existing if present
if [ -f "$OUTPUT_DIR/ffmpeg.exe" ]; then
    EXISTING_SIZE=$(stat -c%s "$OUTPUT_DIR/ffmpeg.exe")
    echo "  Backing up existing ffmpeg.exe ($EXISTING_SIZE bytes) -> ffmpeg.exe.pre-minimal-backup"
    cp "$OUTPUT_DIR/ffmpeg.exe" "$OUTPUT_DIR/ffmpeg.exe.pre-minimal-backup"
fi

cp ffmpeg.exe "$OUTPUT_DIR/ffmpeg.exe"

FINAL_SIZE=$(stat -c%s "$OUTPUT_DIR/ffmpeg.exe")
FINAL_SHA=$(sha256sum "$OUTPUT_DIR/ffmpeg.exe" | awk '{print $1}')
SIZE_MB=$(awk "BEGIN {printf \"%.2f\", $FINAL_SIZE/1048576}")

echo ""
echo "============================================================"
echo "  SUCCESS: Minimal FFmpeg built and staged"
echo "  Output : $OUTPUT_DIR/ffmpeg.exe"
echo "  Size   : $FINAL_SIZE bytes ($SIZE_MB MiB)"
echo "  SHA256 : $FINAL_SHA"
echo "============================================================"
