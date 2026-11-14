"""Inspect compressed IGC stream blocks without changing game assets."""
import pathlib
import re
import sys
from refpack import unrefpack

data = pathlib.Path(sys.argv[1]).read_bytes()
for match in re.finditer(b'\x10\xfb|\x90\xfb', data):
    offset = match.start()
    try:
        decoded, size, end = unrefpack(data, offset)
        if len(decoded) != size:
            continue
    except (IndexError, ValueError):
        continue
    print(f'BLOCK {offset:x} size={size:x} end={end:x}')
    if len(sys.argv) > 2:
        needle = sys.argv[2].encode('ascii')
        pos = decoded.find(needle)
        if pos >= 0:
            print(decoded[max(0, pos-64):pos+128].hex(' '))
        continue
    for text in re.finditer(rb'[\x20-\x7e]{6,}', decoded):
        value = text.group().decode('ascii')
        if re.search(r'[a-zA-Z]{4}', value):
            print(f'  {text.start():08x} {value}')
