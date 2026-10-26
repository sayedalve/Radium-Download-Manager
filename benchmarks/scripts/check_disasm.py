import pefile
import subprocess

pe = pefile.PE(r'C:\Program Files (x86)\Internet Download Manager\IDMan.exe')
image_base = pe.OPTIONAL_HEADER.ImageBase

# Dump text around 0x5A6000 to a temporary .obj/.bin or run dumpbin /disasm
# In dumpbin, /disasm displays disassembly of the entire section or object.
# But we can extract bytes around 0x5A5F00 to 0x5A6500 and write a python x86 disassembler or use objdump if available
# Let's check if llvm-objdump or objdump exists in VS directory!
