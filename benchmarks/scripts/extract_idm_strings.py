import re

with open(r'C:\Program Files (x86)\Internet Download Manager\IDMan.exe', 'rb') as f:
    data = f.read()

# Extract ASCII strings >= 4 chars
ascii_strings = re.findall(b'[\x20-\x7e]{4,}', data)
# Extract UTF-16LE strings >= 4 chars
unicode_strings = re.findall(b'(?:[\x20-\x7e]\x00){4,}', data)
unicode_decoded = [s.decode('utf-16le') for s in unicode_strings]

interesting = []
keywords = ['range', 'bytes=', 'socket', 'split', 'part', 'segment', 'connect', 'chunk', 'buffer', 'thread', 'resume', 'speed', 'limit', 'downl', 'content-range', 'accept-ranges', 'http/1']
for s in ascii_strings:
    s_dec = s.decode('latin1')
    if any(k in s_dec.lower() for k in keywords):
        interesting.append(s_dec)

for s_dec in unicode_decoded:
    if any(k in s_dec.lower() for k in keywords):
        interesting.append(s_dec)

print(f"Total interesting strings: {len(interesting)}")
for s in interesting[:60]:
    print(repr(s))
