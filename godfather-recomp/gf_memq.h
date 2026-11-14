/* gf_memq.h -- v1061E cached guest-memory validity check (Claude).
 * WHY: gf_coop_proxy.c / gf_coop_puppet.c called VirtualQuery() on EVERY guest read. VirtualQuery reports the run of
 * pages with the same attributes (RegionSize), so on the emulator's huge committed guest blocks each call walks a lot
 * of page tables: ~0.3-0.5 ms per call. ~20 reads per proxy per frame -> cellPadGetData (where the mod tick runs) took
 * 45 ms per frame with ~5 proxies (wb-14: "HLE 0x8B72CDA1 177x 7975 ms" per 10 s vs 1x in wb-8b without proxies;
 * main thread 85 % in ntdll). That was the whole "proxies cost 50 FPS" blocker.
 * NOW: per-thread cache of the last 8 regions VirtualQuery returned, valid for GF_MEMQ_TTL_MS. All callers
 * still read/write inside __try, so a region freed within the TTL only costs a caught fault, never a crash.
 * GF_MEMQ=0 -> old uncached behaviour (A/B).
 * v1113M: the cache was a `static` copy in EVERY including TU (8 of them: npcfix, combat, proxy, puppet, vehicle, world,
 * wpose, mission), so one thread re-queried the same regions once per TU every 250 ms -- VirtualQuery was still
 * 2-3.6 % of the main thread's samples in solo gameplay (pf02 soak). Now ONE per-thread cache shared by all TUs
 * (defined in gf_perf.c), TTL 1000 ms (GF_MEMQ_TTL_MS=50..10000 overrides; 250 = the old value), and the hit/miss
 * counters are per-thread too (no cache line bounced between threads on every call). */
#ifndef GF_MEMQ_H
#define GF_MEMQ_H
#include <stdint.h>
#include <stdlib.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define GF_MEMQ_N 8
#define GF_MEMQ_TTL_MS 1000
typedef struct { uintptr_t lo, hi; int wr; ULONGLONG t; } gf_memq_ent;
extern __declspec(thread) gf_memq_ent g_memq[GF_MEMQ_N];   /* v1113M: gf_perf.c */
extern __declspec(thread) unsigned g_memq_i;
extern __declspec(thread) LONG g_memq_hits, g_memq_miss;
extern int g_memq_on;          /* -1 = not read yet */
extern unsigned g_memq_ttl;    /* ms */
static int gf_memq_access(uint8_t* base, uint32_t ea, size_t n, int write)
{
    MEMORY_BASIC_INFORMATION mi; DWORD p; uintptr_t a, end; ULONGLONG now; int wr;
    if (!base || ea < 0x10000u || ea >= 0xF0000000u || n > 0xF0000000u - ea) return 0;
    a = (uintptr_t)(base + ea); end = a + n;
    if (end < a) return 0;
    if (g_memq_on < 0) {
        const char* t = getenv("GF_MEMQ_TTL_MS");
        unsigned v = t && *t ? (unsigned)atoi(t) : GF_MEMQ_TTL_MS;
        g_memq_ttl = v < 50 ? 50 : v > 10000 ? 10000 : v;
        const char* e = getenv("GF_MEMQ"); g_memq_on = !(e && *e == '0');
    }
    now = GetTickCount64();
    if (g_memq_on) {
        for (int i = 0; i < GF_MEMQ_N; i++) {
            const gf_memq_ent* c = &g_memq[i];
            if (c->hi && a >= c->lo && end <= c->hi && now - c->t < g_memq_ttl && (!write || c->wr)) { g_memq_hits++; return 1; }
        }
    }
    g_memq_miss++;
    if (!VirtualQuery((void*)a, &mi, sizeof mi) || mi.State != MEM_COMMIT) return 0;
    if (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return 0;
    p = mi.Protect & 0xFFu;
    wr = p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
    if (g_memq_on) {
        gf_memq_ent* c = &g_memq[g_memq_i++ % GF_MEMQ_N];
        c->lo = (uintptr_t)mi.BaseAddress; c->hi = (uintptr_t)mi.BaseAddress + mi.RegionSize; c->wr = wr; c->t = now;
    }
    if (write && !wr) return 0;
    return end <= (uintptr_t)mi.BaseAddress + mi.RegionSize;
}
#endif
