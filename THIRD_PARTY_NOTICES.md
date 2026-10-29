# Third-Party Software Notices and Licenses

Radium Download Manager incorporates or integrates with the following open-source components:

---

## 1. N_m3u8DL-RE
- **Repository:** https://github.com/nilaoda/N_m3u8DL-RE
- **License:** MIT License
- **Copyright:** (C) 2021-2024 nilaoda
- **Usage:** Out-of-process stream probing and segment downloading engine.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

---

## 2. FFmpeg (Minimal Stream Muxer)
- **Website:** https://ffmpeg.org/
- **Pinned Version:** 7.1 (`n7.1`, Commit: `04706f3`, Tarball: `ffmpeg-7.1.tar.xz`)
- **License:** GNU Lesser General Public License v2.1 or later (LGPL-2.1-or-later) / GNU Lesser General Public License v3.0 (LGPL-3.0) via `--enable-version3`
- **Copyright:** (C) 2000-2026 the FFmpeg developers
- **Usage:** Out-of-process audio/video container remuxing, bitstream filtering (`aac_adtstoasc`, `h264_mp4toannexb`), subtitle conversion (`mov_text`, `subrip`), and duration probing.
- **Architectural Isolation:** Executed strictly as an isolated worker child process via Win32 `CreateProcessW` inside a restricted Windows Job Object. There is zero static or dynamic linking against `avcodec`, `avformat`, or `avutil` within `flux.exe` / `openidm.exe`.
- **Minimal Configuration & Licensing Implications:**
  - Configured without `--enable-gpl` and without `--enable-nonfree`, ensuring the binary operates strictly under the terms of the LGPL v2.1+ / v3.0.
  - Network protocols are completely disabled (`--disable-network`) so FFmpeg has zero socket creation or network attack surface; all network I/O is handled securely in-process by Radium's WinHTTP engine.
  - Heavy video encoders (`libx264`, `libx265`, `libvpx`, `svtav1`) are omitted, favoring fast stream copying (`-c copy`) without quality loss or excessive CPU utilization.
- **Source Code Offer:**
  - FFmpeg source code is available at https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz (SHA-256: `40973D44970DBC83EF302B0609F2E74982BE2D85916DD2EE7472D30678A7ABE6`).
  - Full reproducible build instructions and configuration flags are provided in this repository in `scripts/ffmpeg-minimal.config`, `scripts/build-minimal-ffmpeg.ps1`, and `scripts/build-minimal-ffmpeg.sh`.

---

## 3. SQLite
- **Website:** https://www.sqlite.org/
- **License:** Public Domain
- **Usage:** Embedded transactional task state persistence and queue metadata storage.

---

## 4. Microsoft Windows Implementation Libraries (WIL)
- **Repository:** https://github.com/microsoft/wil
- **License:** MIT License
- **Copyright:** (C) Microsoft Corporation
- **Usage:** Exception-safe RAII wrappers for Win32 handles and COM resources.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

---

## 5. JSON for Modern C++ (nlohmann/json)
- **Repository:** https://github.com/nlohmann/json
- **License:** MIT License
- **Copyright:** (C) 2013-2025 Niels Lohmann
- **Usage:** High-performance JSON serialization for IPC and task import/export.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

---

## 8. yt-dlp (Optional Auxiliary Worker)
- **Repository:** https://github.com/yt-dlp/yt-dlp
- **License:** The Unlicense (Public Domain dedication)
- **Copyright:** (C) yt-dlp contributors
- **Usage:** Auxiliary metadata probing and fallback stream extraction.

This is free and unencumbered software released into the public domain.

Anyone is free to copy, modify, publish, use, compile, sell, or
distribute this software, either in source code form or as a compiled
binary, for any purpose, commercial or non-commercial, and by any
means.
