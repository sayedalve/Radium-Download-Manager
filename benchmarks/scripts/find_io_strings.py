import re

with open(r'C:\Program Files (x86)\Internet Download Manager\IDMan.exe', 'rb') as f:
    data = f.read()

keywords = [b'WriteFile', b'ReadFile', b'CreateFile', b'SetFilePointer', b'flush', b'buffer', b'tmpBuff', b'CmpBuff']
strings = re.findall(rb'[\x20-\x7e\r\n\t]{4,}', data)
file_strings = [s.decode('latin1') for s in strings if any(k.lower() in s.lower() for k in [b'file', b'chunk', b'part', b'buff', b'write', b'read', b'disk', b'tmp'])]
print(f"Total file/chunk strings: {len(file_strings)}")
for s in file_strings[:50]:
    print(repr(s))
