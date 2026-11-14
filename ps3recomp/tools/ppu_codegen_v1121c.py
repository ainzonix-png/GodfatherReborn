#!/usr/bin/env python3
"""v1121C ppu_codegen_v1121c.py -- codegen rewrites for lifted PPU TUs (runs after ppu_memacc_v243.py --all-functions).

usage: python ppu_codegen_v1121c.py <in.cpp> <out.cpp> --hdr=<ppu_codegen_v1121c.h> [--vmb-local] [--fpfast] [--restrict] [--stats]
       (in.cpp may equal out.cpp: the file is read completely before it is written)

--vmb-local  every 'void func_X(ppu_context* ctx) {' line gets GF_PPU_FN_ENTRY (a local copy of vm_base) right after the
             brace and the lean accessors use it (ppu_codegen_v1121c.h). Needs the ppu_memacc_v243.py lean build.
--fpfast     the TU's PPC FP helpers ppu_fadd/fsub/fmul/fdiv and ppu_fmadd_core compute the IEEE result first and run the
             NaN-priority selection only when that result is a NaN. Bit-identical: for + - * / and fma a NaN operand
             always gives a NaN result, so a non-NaN result means no operand was a NaN and the old code returned the same
             value; a NaN result takes the unchanged old code.
--restrict   (experimental) the same signature lines take ctx as GF_PPU_CTX_RESTRICT.
Every other line is copied unchanged. Without any option the output equals the input.
"""
import sys, re

SIG = re.compile(r'^(void func_[0-9A-Fa-z_]+\(ppu_context\* )(ctx\) \{)')

FP_OPS = (('fadd', '+'), ('fsub', '-'), ('fmul', '*'), ('fdiv', '/'))
FMA_OLD_HEAD = 'static inline double ppu_fmadd_core(double a, double c, double b, int neg_b, int neg_res)\n{'
FMA_NAN_HEAD = 'static inline double ppu_fmadd_core_nan(double a, double c, double b, int neg_b, int neg_res)\n{'
FMA_FAST = ('static inline double ppu_fmadd_core(double a, double c, double b, int neg_b, int neg_res)   /* v1121C */\n'
            '{\n'
            '    double r = fma(a, c, neg_b ? -b : b);\n'
            '    if (__builtin_expect(r == r, 1)) return neg_res ? -r : r;\n'
            '    return ppu_fmadd_core_nan(a, c, b, neg_b, neg_res);\n'
            '}\n')

def fpfast(s):
    n = 0
    for op, sym in FP_OPS:
        old = 'static inline double ppu_%s(double a, double b) { return ppu_fp_bin(a, b, a %s b); }' % (op, sym)
        new = ('static inline double ppu_%s(double a, double b) { double r = a %s b; '
               'if (__builtin_expect(r == r, 1)) return r; return ppu_fp_bin(a, b, r); }   /* v1121C */' % (op, sym))
        if s.count(old) == 1:
            s = s.replace(old, new); n += 1
    if s.count(FMA_OLD_HEAD) == 1:
        i = s.index(FMA_OLD_HEAD)
        j = s.find('\n}\n', i)
        if j > 0:
            j += 3
            s = s[:i] + FMA_NAN_HEAD + s[i + len(FMA_OLD_HEAD):j] + FMA_FAST + s[j:]
            n += 1
    return s, n

def main():
    a = sys.argv[1:]
    src, dst = a[0], a[1]
    opts = dict(x.split('=', 1) if '=' in x else (x, '1') for x in a[2:])
    vmb, fpf, res = '--vmb-local' in opts, '--fpfast' in opts, '--restrict' in opts
    s = open(src, encoding='utf-8', errors='replace').read()
    nfp = 0
    if fpf:
        s, nfp = fpfast(s)
    nfn = 0
    if vmb or res:
        hdr = opts['--hdr'].replace('\\', '/')
        L = s.split('\n')
        o = []
        inc_done = False
        for l in L:
            m = SIG.match(l)
            if m:
                l = m.group(1) + ('GF_PPU_CTX_RESTRICT ' if res else '') + m.group(2) + \
                    (' GF_PPU_FN_ENTRY' if vmb else '') + l[m.end():]
                nfn += 1
            o.append(l)
            if not inc_done and (l.startswith('#include') and 'ppu_memory_v243.h' in l):
                if vmb: o.append('#define GF_PPU_VMB_LOCAL 1')
                if res: o.append('#define GF_PPU_RESTRICT 1')
                o.append('#include "%s"   /* v1121C codegen switches */' % hdr)
                inc_done = True
        if not inc_done:
            raise SystemExit('ppu_codegen_v1121c: no #include of ppu_memory_v243.h in %s (run ppu_memacc_v243.py --all-functions first)' % src)
        s = '\n'.join(o)
    open(dst, 'w', encoding='utf-8', newline='\n').write(s)
    if '--stats' in opts:
        print('%s: v1121C codegen: %d function entries rewritten (vmb-local=%d restrict=%d), %d FP helpers (fpfast=%d)'
              % (src, nfn, vmb, res, nfp, fpf))

if __name__ == '__main__':
    main()
