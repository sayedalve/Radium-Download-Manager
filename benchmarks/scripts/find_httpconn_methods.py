import re

with open(r'C:\Program Files (x86)\Internet Download Manager\IDMan.exe', 'rb') as f:
    data = f.read()

methods = set(re.findall(rb'HTTPConnection::[a-zA-Z0-9_]{3,35}', data))
for m in sorted(methods):
    print(m.decode('latin1'))
