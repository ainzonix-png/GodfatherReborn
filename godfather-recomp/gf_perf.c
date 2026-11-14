/*
 * gf_perf.c -- v1113Q host-side performance helpers (no visual effect).
 *
 * v1113Q GF_QWAIT: the main thread's "post a command and wait until the worker drained the queue" loop
 * (func_005844F0, ppu_recomp_002.cpp loc_00584638) re-takes TWO guest sys_mutexes on every check:
 *     do { lock(B); lock(A); done = A.count(+0x6800) == 0 && busy(+0xC270) == 0; unlock(A); unlock(B); } while (!done);
 * The worker (func_00583684, guest tid 3) needs mutex A to pop every record. On lv2 each lock/unlock was a ~1 us
 * syscall and the kernel queued the waiter; here the uncontended path (PS3_SC_FAST) is ~20 ns, so the main thread
 * re-acquires A ~100 ns after releasing it and the woken worker keeps finding it taken (unfair CRITICAL_SECTION) --
 * a lock convoy: pf02 soak, 4.5-8.6 MILLION main-thread lock+unlock pairs per second, main CPU 47 % -> 70 %.
 * Fix: after a failed check, read the two words WITHOUT the locks until they look done, then let the guest loop
 * re-lock and re-check exactly as before (the locked check still decides; the unlocked read is only a hint).
 * Pause-spin first (~20 us), then SwitchToThread, capped at GF_QWAIT_MAX_US (default 3000 us) per wait.
 *   GF_QWAIT=1 on (launchers), 0/unset off, ab = only in PS3_ABX phases with bit 0x20000 set (in-run A/B).
 *   Log "[qwait] v1113Q 10 s: ..." every 10 s while it is used.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <immintrin.h>

extern uint8_t* vm_base;
extern volatile int g_abx;

/* v1113M: the ONE definition of gf_memq.h's per-thread region cache (was a static copy in each of 8 TUs). */
#include "gf_memq.h"
__declspec(thread) gf_memq_ent g_memq[GF_MEMQ_N];
__declspec(thread) unsigned g_memq_i;
__declspec(thread) LONG g_memq_hits, g_memq_miss;
int g_memq_on = -1;
unsigned g_memq_ttl = GF_MEMQ_TTL_MS;

static uint32_t qw_rd32(uint32_t ea)
{
    const volatile uint8_t* p = (const volatile uint8_t*)(vm_base + ea);
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int s_qw_mode = -1;          /* 0 off, 1 on, 2 A/B on ABX bit 0x20000 */
static int s_qw_max_us = 3000;
static LARGE_INTEGER s_qw_f;
/* stats (main thread only calls this; plain counters are fine) */
static uint64_t s_qw_calls, s_qw_ready, s_qw_timeouts, s_qw_ticks, s_qw_t_last;

static void qw_init(void)
{
    const char* e = getenv("GF_QWAIT");
    s_qw_mode = !e || !*e || *e == '0' ? 0 : (e[0] == 'a' || e[0] == 'A') ? 2 : 1;
    const char* m = getenv("GF_QWAIT_MAX_US");
    if (m && *m) { int v = atoi(m); if (v >= 50 && v <= 100000) s_qw_max_us = v; }
    QueryPerformanceFrequency(&s_qw_f);
    if (s_qw_mode)
        fprintf(stderr, "[qwait] v1113Q queue-drain wait polls without the guest mutexes: %s (max %d us per wait)\n",
                s_qw_mode == 2 ? "A/B on PS3_ABX bit 0x20000" : "ON", s_qw_max_us);
}

static void qw_report(uint64_t now)
{
    if (!s_qw_t_last) { s_qw_t_last = now; return; }
    if ((double)(now - s_qw_t_last) < 10.0 * (double)s_qw_f.QuadPart) return;
    const double secs = (double)(now - s_qw_t_last) / (double)s_qw_f.QuadPart;
    fprintf(stderr, "[qwait] v1113Q %.0f s: %llu waits (%.0f/s), ready %llu, timed out %llu, mean %.1f us per wait%s\n",
            secs, (unsigned long long)s_qw_calls, (double)s_qw_calls / secs, (unsigned long long)s_qw_ready,
            (unsigned long long)s_qw_timeouts,
            s_qw_calls ? 1e6 * (double)s_qw_ticks / (double)s_qw_f.QuadPart / (double)s_qw_calls : 0.0,
            s_qw_mode == 2 ? ((g_abx & 0x20000) ? " [A/B on]" : " [A/B off]") : "");
    s_qw_calls = s_qw_ready = s_qw_timeouts = s_qw_ticks = 0;
    s_qw_t_last = now;
}

/* Called from the guest wait loop after it released both mutexes because the queue was not drained yet. */
void gf_qwait_poll(uint32_t count_ea, uint32_t busy_ea)
{
    if (s_qw_mode < 0) qw_init();
    if (!s_qw_mode || (s_qw_mode == 2 && !(g_abx & 0x20000))) return;
    LARGE_INTEGER t0, t;
    QueryPerformanceCounter(&t0);
    const int64_t spin_end = t0.QuadPart + s_qw_f.QuadPart / 50000;               /* ~20 us of pause-spin */
    const int64_t end = t0.QuadPart + s_qw_f.QuadPart * (int64_t)s_qw_max_us / 1000000;
    int ready = 0;
    for (unsigned i = 0;; i++) {
        if (qw_rd32(count_ea) == 0 && qw_rd32(busy_ea) == 0) { ready = 1; break; }
        if ((i & 7) != 7) { _mm_pause(); _mm_pause(); _mm_pause(); _mm_pause(); continue; }
        QueryPerformanceCounter(&t);
        if (t.QuadPart >= end) break;
        if (t.QuadPart >= spin_end) SwitchToThread();
    }
    QueryPerformanceCounter(&t);
    s_qw_calls++; s_qw_ticks += (uint64_t)(t.QuadPart - t0.QuadPart);
    if (ready) s_qw_ready++; else s_qw_timeouts++;
    qw_report((uint64_t)t.QuadPart);
}
