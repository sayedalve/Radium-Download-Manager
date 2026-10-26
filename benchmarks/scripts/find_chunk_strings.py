import re

with open(r'C:\Program Files (x86)\Internet Download Manager\IDMan.exe', 'rb') as f:
    data = f.read()

keywords = [b'InsInChunksList', b'OnChangeBounds', b'StartGetChunk', b'RollBack', b'AuditForRestart', b'SendInAddition']
for kw in keywords:
    matches = [m.start() for m in re.finditer(re.escape(kw), data)]
    print(f"Keyword {kw}: {len(matches)} occurrences")
    for off in matches:
        start = max(0, off - 40)
        end = min(len(data), off + 80)
        snippet = data[start:end]
        print(f"  Offset 0x{off:X}: {repr(snippet)}")
