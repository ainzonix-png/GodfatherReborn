#!/usr/bin/env python3
"""Installer pipeline helpers (installer/godfather.toml). Everything here reads the player's own EBOOT.elf.

  functions <fresh EBOOT.functions.json> <out.json>
      ppu_loader.py's OPD-derived list starts at 0x10210, but the executable has a live four-instruction TOC thunk at
      0x10200 that func_00010210 calls (HANDOFF "First boot finding"). Adds that one entry.

  spu-ls <EBOOT.elf> <outdir>
      Rebuilds the three SPU local-store images the port's SPU lifts were made from. The originals were captured from
      a running game (spu/live_01ee0_ls.bin, _tmp/lift1039/cap/ls_0D1C0.bin, ls_0F9D4.bin); all of their CODE is
      stored verbatim in EBOOT.elf, so the same images (code only, data areas zero) are assembled from it here:
        live_01ee0_ls.bin  the low-LS program of the movie / mixer SPU threads
        ls_0D1C0.bin       RWAC mixer modules, city layout
        ls_0F9D4.bin       RWAC mixer modules, fight layout
"""
import json, os, sys

# (file name, [(LS start, LS end, EBOOT.elf file offset of LS start)])  -- BLES00043 v1.01 EBOOT.elf
LS_IMAGES = [
    ("live_01ee0_ls.bin", [(0x00080, 0x02C80, 0xC6F780)]),
    ("ls_0D1C0.bin",      [(0x09D80, 0x0AD00, 0xC7E600), (0x0AD00, 0x0CD00, 0xC7C500), (0x0CD00, 0x23500, 0xC84A00)]),
    ("ls_0F9D4.bin",      [(0x09D80, 0x0EF80, 0xC9BE00), (0x0EF80, 0x0FF00, 0xC7E600), (0x0FF00, 0x11F00, 0xC7C500),
                           (0x11F00, 0x23C00, 0xC84A00), (0x28B80, 0x29500, 0xC9B300), (0x29500, 0x2B100, 0xC72480),
                           (0x2B100, 0x2E680, 0xC7F680), (0x2E680, 0x2EC80, 0xC74180), (0x2EC80, 0x30000, 0xC6E400)]),
]

def cmd_functions(src, dst):
    fns = json.load(open(src))
    if not any(int(f["start"], 16) == 0x10200 for f in fns):
        toc = fns[0]["toc"]
        fns.insert(0, {"start": "0x10200", "end": "0x10210", "toc": toc, "opd": None})
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    with open(dst, "w", newline="\n") as f:
        json.dump(fns, f, indent=2); f.write("\n")
    print(f"{len(fns)} functions -> {dst}")

def cmd_spu_ls(elf_path, outdir):
    elf = open(elf_path, "rb").read()
    os.makedirs(outdir, exist_ok=True)
    for name, parts in LS_IMAGES:
        ls = bytearray(0x40000)
        for a, b, off in parts:
            chunk = elf[off:off + (b - a)]
            if len(chunk) != b - a:
                sys.exit(f"{elf_path} is too short for {name} (wrong EBOOT.elf?)")
            ls[a:b] = chunk
        open(os.path.join(outdir, name), "wb").write(ls)
        print(f"{name}: {sum(b - a for a, b, _ in parts)} code bytes")

if __name__ == "__main__":
    a = sys.argv[1:]
    if len(a) == 3 and a[0] == "functions": cmd_functions(a[1], a[2])
    elif len(a) == 3 and a[0] == "spu-ls": cmd_spu_ls(a[1], a[2])
    else: sys.exit(__doc__)
