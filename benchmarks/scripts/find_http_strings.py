import re

with open(r'C:\Program Files (x86)\Internet Download Manager\IDMan.exe', 'rb') as f:
    data = f.read()

keywords = [b'Range: bytes=', b'Content-Range', b'206 Partial', b'Accept-Ranges', b'bytes=%I64', b'bytes %I64', b'Range:']
for kw in keywords:
    matches = [m.start() for m in re.finditer(re.escape(kw), data, re.IGNORECASE)]
    print(f"Keyword {kw}: {len(matches)} occurrences")
    for off in matches:
        start = max(0, off - 30)
        end = min(len(data), off + 80)
        snippet = data[start:end]
        print(f"  Offset 0x{off:X}: {repr(snippet)}")
