/* gf_fncount.c -- v1057T call-count tracker (only in the GF_FNCOUNT build, build-fncount\, launcher play-fncount.cmd).
 * Every lifted function bumps g_gf_fnc[(addr - 0x10000) >> 2] on entry (tools/gf_fncount.py). F2 writes a snapshot of
 * the whole array to fncount\snap_NN.bin (u32 count per 4-byte code address from 0x10000) and beeps; the difference of
 * two snapshots = what ran between the presses. Used to find the game's collision ray cast and decal spawner
 * (shoot a wall N times between two presses -> functions that ran ~N times more). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#define GF_FNC_N ((0xAE0000u - 0x10000u) >> 2)
unsigned int g_gf_fnc[GF_FNC_N];
/* v1057P probes: GF_PROBE=hex,hex,... (function addresses) -> the first GF_PROBE_MAX (6) entries of each log r3..r10,
 * f1..f8, 64 bytes at r3..r6 (hex + float) and the guest call stack (LR, then back chain saved LRs). Log [probe]. */
unsigned char g_gf_prb[GF_FNC_N];
extern unsigned char* vm_base;
static int rd_ok(unsigned a, unsigned n) { return vm_base && a >= 0x10000u && a < 0xF0000000u && a + n > a; }
#ifdef _WIN32
static CRITICAL_SECTION s_pcs; static int s_pcs_ok;
static unsigned be32(unsigned a) { unsigned v = 0; __try { const unsigned char* p = vm_base + a; v = (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3]; } __except (1) { v = 0xDEADDEADu; } return v; }
void gf_fnc_probe(unsigned a, const unsigned long long* gpr, const double* fpr, unsigned long long lr)
{
    const unsigned i = (a - 0x10000u) >> 2;
    if (!s_pcs_ok) return;
    EnterCriticalSection(&s_pcs);
    if (!g_gf_prb[i]) { LeaveCriticalSection(&s_pcs); return; }
    g_gf_prb[i]--;
    char b[4096]; int o = 0;
    o += snprintf(b + o, sizeof b - o, "[probe] func_%08X tick %llu lr %08X\n[probe]   r3-r10", a, (unsigned long long)GetTickCount64(), (unsigned)lr);
    for (int r = 3; r <= 10; r++) o += snprintf(b + o, sizeof b - o, " %08X", (unsigned)gpr[r]);
    o += snprintf(b + o, sizeof b - o, "\n[probe]   f1-f8");
    for (int r = 1; r <= 8; r++) o += snprintf(b + o, sizeof b - o, " %.4g", fpr[r]);
    for (int r = 3; r <= 6; r++) {
        const unsigned p = (unsigned)gpr[r];
        if (!rd_ok(p, 64)) continue;
        o += snprintf(b + o, sizeof b - o, "\n[probe]   @r%d", r);
        for (int k = 0; k < 16; k++) { unsigned w = be32(p + 4 * k); float f; memcpy(&f, &w, 4);
            o += snprintf(b + o, sizeof b - o, " %08X(%.4g)", w, (f == f && f > -1e7f && f < 1e7f) ? f : 0.0f); }
    }
    o += snprintf(b + o, sizeof b - o, "\n[probe]   stack %08X", (unsigned)lr);
    unsigned sp = (unsigned)gpr[1];
    for (int k = 0; k < 18 && rd_ok(sp, 32); k++) {
        const unsigned nsp = be32(sp + 4);           /* back chain (64-bit slot, low word) */
        if (!rd_ok(nsp, 32) || nsp <= sp) break;
        o += snprintf(b + o, sizeof b - o, " %08X", be32(nsp + 0x14));   /* saved LR (64-bit slot at +0x10, low word) */
        sp = nsp;
    }
    o += snprintf(b + o, sizeof b - o, "\n");
    fputs(b, stderr); fflush(stderr);
    LeaveCriticalSection(&s_pcs);
}
#else
void gf_fnc_probe(unsigned a, const unsigned long long* gpr, const double* fpr, unsigned long long lr) { (void)a; (void)gpr; (void)fpr; (void)lr; }
#endif
#ifdef _WIN32
static DWORD WINAPI fnc_thread(LPVOID p)
{
    (void)p; int prev = 0, n = 0;
    CreateDirectoryA("fncount", NULL);
    for (;;) {
        Sleep(15);
        const int down = (GetAsyncKeyState(VK_F2) & 0x8000) ? 1 : 0;
        if (down && !prev) {
            char fn[64]; snprintf(fn, sizeof fn, "fncount\\snap_%02d.bin", n);
            FILE* f = fopen(fn, "wb");
            if (f) { fwrite(g_gf_fnc, 4, GF_FNC_N, f); fclose(f); }
            fprintf(stderr, "[fncount] F2: snapshot %d -> %s (tick %llu)\n", n, fn, (unsigned long long)GetTickCount64());
            MessageBeep(MB_OK); n++;
        }
        prev = down;
    }
    return 0;
}
static void __cdecl fnc_start(void)
{
    InitializeCriticalSection(&s_pcs); s_pcs_ok = 1;
    { const char* e = getenv("GF_PROBE"); const char* m = getenv("GF_PROBE_MAX"); int mx = m && *m ? atoi(m) : 6; int n = 0;
      if (mx < 1) mx = 1; if (mx > 250) mx = 250;
      while (e && *e) { char* end; unsigned long v = strtoul(e, &end, 16); if (end == e) break;
          if (v >= 0x10000u && v < 0xAE0000u) { g_gf_prb[(v - 0x10000u) >> 2] = (unsigned char)mx; n++; }
          e = end; while (*e == ',' || *e == ' ') e++; }
      if (n) fprintf(stderr, "[probe] v1057P %d function probe(s), %d hits each (GF_PROBE)\n", n, mx); }
    CreateThread(NULL, 0, fnc_thread, NULL, 0, NULL);
    fprintf(stderr, "[fncount] v1057T call-count tracker ON: F2 = snapshot (fncount\\snap_NN.bin)\n");
}
/* v1057T-2: a static initializer pointer was dropped as unused (no [fncount] line in the first run) -> exported and
 * forced into the link with /include. */
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) void (__cdecl* gf_fnc_init_ptr)(void) = fnc_start;
#pragma comment(linker, "/include:gf_fnc_init_ptr")
#endif
