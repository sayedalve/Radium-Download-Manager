import pefile

pe = pefile.PE(r'C:\Program Files (x86)\Internet Download Manager\IDMan.exe')
image_base = pe.OPTIONAL_HEADER.ImageBase

# Search for calls or references to recv (IAT address = 0x78520C)
# In 32-bit x86, call dword ptr [0x78520C] is FF 15 0C 52 78 00
target_va = 0x78520C
target_bytes = target_va.to_bytes(4, 'little')

text_sec = None
for s in pe.sections:
    if b'.text' in s.Name:
        text_sec = s
        break

data = text_sec.get_data()
offset_base = text_sec.VirtualAddress + image_base

# Find FF 15 .. .. .. .. (call) or FF 25 .. .. .. .. (jmp)
refs = []
idx = 0
while True:
    pos = data.find(target_bytes, idx)
    if pos == -1:
        break
    va = offset_base + pos - 2
    opcode = data[pos-2:pos]
    print(f"Reference to recv at VA=0x{va:X}, opcode={opcode.hex()}")
    refs.append(va)
    idx = pos + 4
