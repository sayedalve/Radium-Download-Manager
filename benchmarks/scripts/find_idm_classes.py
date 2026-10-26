import re

with open(r'C:\Program Files (x86)\Internet Download Manager\IDMan.exe', 'rb') as f:
    data = f.read()

# Find strings starting with C or mentioning Classes
classes = set(re.findall(rb'\bC[A-Z][a-zA-Z0-9_]{3,30}::[a-zA-Z0-9_~]{3,35}', data))
for c in sorted(classes):
    print(c.decode('latin1'))
