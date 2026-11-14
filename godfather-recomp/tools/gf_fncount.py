"""v1057T call-count tracker: copy of a lifted TU with a counter at the top of every func_XXXXXXXX.
usage: python gf_fncount.py <in.cpp> <out.cpp>   (CMake GF_FNCOUNT=ON runs it into build-fncount/recompiled-fncount)"""
import re, sys
src, dst = sys.argv[1], sys.argv[2]
pat = re.compile(r'^((?:static )?void func_([0-9A-Fa-f]{8})\(ppu_context\* ctx\) \{)\s*$')
hdr = ('extern "C" unsigned int g_gf_fnc[]; extern "C" unsigned char g_gf_prb[];\n'
       'extern "C" void gf_fnc_probe(unsigned a, const unsigned long long* gpr, const double* fpr, unsigned long long lr);\n'
       '#define GF_FNC(a) do { const unsigned _gi = ((unsigned)(a) - 0x10000u) >> 2; g_gf_fnc[_gi]++; '
       'if (g_gf_prb[_gi]) gf_fnc_probe((a), ctx->gpr, ctx->fpr, ctx->lr); } while (0)\n')   # v1057P probes
out = []; n = 0; done_hdr = False
with open(src, encoding='latin1') as f:
    for line in f:
        if not done_hdr and line.startswith('#include'):
            out.append(line); out.append(hdr); done_hdr = True; continue
        out.append(line)
        m = pat.match(line.rstrip('\n'))
        if m:
            a = int(m.group(2), 16)
            if 0x10000 <= a < 0xAE0000: out.append(f'        GF_FNC(0x{a:08X}u);\n'); n += 1
if not done_hdr: out.insert(0, hdr)
with open(dst, 'w', encoding='latin1') as f: f.writelines(out)
print(f"{src}: {n} functions counted")
