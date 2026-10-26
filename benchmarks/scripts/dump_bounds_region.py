import re

with open(r'C:\Program Files (x86)\Internet Download Manager\IDMan.exe', 'rb') as f:
    data = f.read()

snippet = data[0x2C9800:0x2CA800]
strings = re.findall(b'[\x20-\x7e\r\n\t]{3,}', snippet)
for s in strings:
    print(repr(s.decode('latin1')))
