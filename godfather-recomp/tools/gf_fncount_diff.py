"""v1057T: compare call-count snapshots (fncount/snap_NN.bin from play-fncount.cmd, F2).
usage: python tools/gf_fncount_diff.py [shots=10]
Step k = snap k -> snap k+1. Step 0 is the idle baseline (stand still). For each later step, lists the functions that
(a) did not run at all while idle and (b) ran a multiple of `shots` times (bullet ray cast / impact / decal candidates),
then the biggest idle-free risers."""
import sys, glob, numpy as np, re
shots = int(sys.argv[1]) if len(sys.argv) > 1 else 10
S = [np.fromfile(f, '<u4').astype(np.int64) for f in sorted(glob.glob('fncount/snap_*.bin'))]
if len(S) < 3: sys.exit("need at least 3 snapshots (idle start, idle end / test start, test end)")
ticks = [int(m) for m in re.findall(r'\[fncount\] F2: snapshot \d+ -> \S+ \(tick (\d+)\)', open('play-fncount.err.log', errors='ignore').read())]
idle = S[1] - S[0]
addr = lambda i: 0x10000 + 4 * i
for k in range(1, len(S) - 1):
    d = S[k + 1] - S[k]
    secs = (ticks[k + 1] - ticks[k]) / 1000 if len(ticks) > k + 1 else 0
    print(f"\n=== step {k}: snap {k} -> {k+1}  ({secs:.1f} s)  {int((d > 0).sum())} functions ran")
    fresh = np.nonzero((idle == 0) & (d > 0))[0]
    mult = [i for i in fresh if d[i] % shots == 0 and d[i] <= 40 * shots]
    print(f"  idle-free functions run a multiple of {shots} times: {len(mult)}")
    for i in sorted(mult, key=lambda i: d[i])[:80]: print(f"    func_{addr(i):08X}  x{d[i]}")
    print("  biggest idle-free:")
    for i in fresh[np.argsort(-d[fresh])][:30]: print(f"    func_{addr(i):08X}  x{d[i]}")
