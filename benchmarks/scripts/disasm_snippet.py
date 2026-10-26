import pefile

pe = pefile.PE(r'C:\Program Files (x86)\Internet Download Manager\IDMan.exe')
image_base = pe.OPTIONAL_HEADER.ImageBase

def disasm_range(start_va, count_bytes):
    # Find section
    sec = None
    for s in pe.sections:
        if s.VirtualAddress <= start_va - image_base < s.VirtualAddress + s.Misc_VirtualSize:
            sec = s
            break
    if not sec:
        return
    offset = (start_va - image_base) - sec.VirtualAddress
    raw = sec.get_data()[offset:offset+count_bytes]
    print(f"Bytes at 0x{start_va:X}: {raw.hex()}")

disasm_range(0x5A6000, 64)
disasm_range(0x5A6400, 64)
