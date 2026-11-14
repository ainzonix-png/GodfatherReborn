/* gf_coop_look.c -- v1059L co-op partner appearance (MobFace created character) (Claude, agent LOOK).
 *
 * RE (see NOTES_mobface_RE.md):
 *   P = *(0x10181060)  player MobFace holder (ctor func_005E9550). P+0x1C = face object {+0 baked model (clump-like
 *                       template, shareable: the IGC stand-in "herc_igc" uses it while the player exists, func_0028E2E0),
 *                       +4 serialized MobFace blob, +8 blob size}. func_005E8E88(P) returns P+0x1C.
 *   func_001BC370(player, mode) = the player's model switch. mode 1 (MobFace, player+0x1A64 == 1):
 *       player->vt[0x108](player, face->model); func_00461E30(player); func_00463660(player, player+0x2234 skeleton GUID)
 *   vt+0x108 on the crew puppet's class (vtable 100463A0) is the generic character SetModel func_00464818.
 *
 * What this does:
 *   1. Capture: every 1 s on the main thread, hash the local face blob; on change send it to the partner on the reliable
 *      event channel (type 0x0110, <= 900-byte chunks).
 *   2. Look: once the puppet exists (and again after a respawn or when our face model changes) give it the MobFace model
 *      with the same 3 calls as func_001BC370 mode 1. The baked model is the LOCAL face: correct for coop-local.cmd and
 *      echo (both windows load the same save); a partner with a DIFFERENT face is detected by hash and logged -- building
 *      a second face needs a second holder (P's 2 MB bake buffer is shared), see the notes.
 * Env: GF_COOP_LOOK=0 off. GF_COOP_LOOK_TAIL=1 also runs the generic tail of func_001BC370 (model +0x64 vt+0x34(0),
 *      vt+0x6C, vt+0x130(1)). GF_COOP_LOOK_ANYFACE=1 applies our model even if the partner's face hash differs (default 1).
 * Threads: gf_coop_look_tick() main PPU thread (gf_coop_glue_tick); on_event() co-op thread (copies only).
 *
 * v1119A (user: "make the players' own models show instead of the same on everyone", "their character name when you target
 * them instead of the random NPC one"):
 *   OWN LOOK: when the partner's face blob differs from ours, build a SECOND face with a temporary gameplay MobFace editor (the
 *     gf_shop.c tailor path, proven in v1111V..v1112A): new 0x88 + ctor thunk 13ADC (alloc *(0x101EC778), 0x10012630), tick
 *     thunk 11C3C until E+0x38 == 4, point OUR face object's blob at a guest copy of the partner's blob only for the parse thunk
 *     10C9C(P) (= func_005E9340) and restore it at once, 10 more ticks, then face2 = thunk 132AC(E, *(P+0x18)) + thunk 12B7C(E,
 *     face2) (builds face2->model) WITHOUT releasing our own face (141AC is not called), shutdown 10EAC + ticks + delete (vt+4).
 *     The puppet gets face2->model through apply_look. v1119L: default ON (GF_COOP_LOOK_OWN=0 off) -- built in its own heap H2.
 *     q71 RESULT: bake ok, puppet got the model, then the WORLD GEOMETRY EXPLODED + freeze -> the second bake writes into memory
 *     our face / the world still use (P's 2 MB bake buffer). Needs its own holder / buffer before it can be turned on.
 *     GF_COOP_LOOK_TESTFACE=1 (echo test): partner blob = ours with another hat / shirt / trousers -> visible difference.
 *   NAME: the targeting HUD (func_000B7FE8) shows GetName(char) = func_0012D5BC: the string object at char+0x1824 {ptr, len,
 *     cap} (8-bit), generated once from lw_*_firstname/lastname when empty. We assign the peer's co-op name into it with the
 *     game's string assign func_0040F008 (source string in guest scratch below the stack). GF_COOP_NAMES=0 off. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "recompiled/ppu_recomp.h"

extern uint8_t* vm_base;
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
extern int gf_coop_connected(void);
extern uint32_t gf_coop_puppet_entity(void);
extern const char* gf_coop_partner_name(void);
#include "gf_coop_peers.h"
typedef void (*gf_coop_event_fn)(uint16_t type, const void* data, int len);
extern void gf_coop_set_event_handler(gf_coop_event_fn fn);
extern int gf_coop_send_event(uint16_t type, const void* data, int len);

enum { GAME_TOC = 0x00B4BF60u, EV_APPEARANCE = 0x0110, CHUNK = 900, MAX_BLOB = 256 * 1024 };
typedef void (*gfn)(ppu_context*);

static int s_on = 1, s_tail = 0, s_anyface = 1;
static uint32_t s_my_hash, s_my_size; static int s_my_valid;
static uint8_t* s_tx; static uint32_t s_tx_size, s_tx_hash; static int s_tx_next, s_tx_count;   /* pending chunked send */
static ULONGLONG s_next_cap, s_puppet_seen_at;
static uint32_t s_applied_ent, s_applied_model, s_last_puppet;
static int s_apply_fail;
static CRITICAL_SECTION s_cs; static int s_cs_ok;
static uint8_t* s_rx; static uint32_t s_rx_size, s_rx_hash; static uint8_t* s_rx_have; static int s_rx_count, s_rx_got;
static volatile LONG s_partner_hash_ok; static volatile uint32_t s_partner_hash, s_partner_size;
static int s_logged_partner;

static int ea_ok(uint32_t ea, uint32_t n) { return ea >= 0x10000u && ea < 0xF0000000u && n <= 0xF0000000u - ea; }
static uint32_t rd32(uint32_t ea) { const uint8_t* p = vm_base + ea; return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static int rd(uint32_t ea, uint32_t* out)
{
    if (!ea_ok(ea, 4)) return 0;
    __try { *out = rd32(ea); return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static uint32_t player(void)
{
    uint32_t a;
    if (!rd(0x10181290u, &a) || !ea_ok(a, 8) || !rd(a + 4, &a) || !ea_ok(a, 4) || !rd(a, &a) || a < 0x17FCu) return 0;
    a -= 0x17FCu; return ea_ok(a, 4) ? a : 0;
}
static uint32_t fnv(const uint8_t* p, uint32_t n) { uint32_t h = 2166136261u; for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; } return h ? h : 1u; }

static gfn lookup(uint32_t addr)
{
    static uint32_t ca[16]; static gfn cf[16]; static int cn;
    for (int i = 0; i < cn; i++) if (ca[i] == addr) return cf[i];
    gfn f = 0;
    for (uint64_t i = 0; i < function_table_count; i++) if ((uint32_t)function_table[i].addr == addr) { f = function_table[i].func; break; }
    if (cn < 16) { ca[cn] = addr; cf[cn] = f; cn++; }
    if (!f) { printf("[coop-look] func_%08X not in the function table\n", addr); fflush(stdout); }
    return f;
}
static int gcall_toc(uint32_t addr, uint32_t toc, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t* ret)
{
    ppu_context* cur = g_active_ctx; gfn f; static ppu_context c;
    if (!cur || !(f = lookup(addr))) return 0;
    __try {
        memcpy(&c, cur, sizeof c);
        c.gpr[1] = ((uint32_t)cur->gpr[1] - 0x1000u) & ~0xFull; c.gpr[2] = toc;
        c.gpr[3] = a0; c.gpr[4] = a1; c.gpr[5] = a2; c.lr = 0;
        g_active_ctx = &c; f(&c);
        while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&c); }
        g_active_ctx = cur; if (ret) *ret = c.gpr[3]; return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_active_ctx = cur; g_trampoline_fn = 0;
        printf("[coop-look] guest call %08X faulted\n", addr); fflush(stdout); return 0;
    }
}
static int vcall(uint32_t obj, uint32_t slot, uint64_t a1)
{
    uint32_t vt, opd, fn, toc;
    if (!rd(obj, &vt) || !rd(vt + slot, &opd) || !rd(opd, &fn) || !rd(opd + 4, &toc) || !fn || !toc) return 0;
    return gcall_toc(fn, toc, obj, a1, 0, NULL);
}

/* face object of the local player: {model, blob, size} */
static int local_face(uint32_t* model, uint32_t* blob, uint32_t* size)
{
    uint32_t P, F;
    if (!rd(0x10181060u, &P) || !ea_ok(P, 0x40) || !rd(P + 0x1C, &F) || !ea_ok(F, 12)) return 0;
    if (!rd(F, model) || !rd(F + 4, blob) || !rd(F + 8, size)) return 0;
    return 1;
}

static void on_event(uint16_t type, const void* data, int len)   /* co-op thread */
{
    const uint8_t* d = (const uint8_t*)data; uint32_t hash, total; uint16_t idx, cnt;
    if (type != EV_APPEARANCE || len < 12 || !s_cs_ok) return;
    memcpy(&hash, d, 4); memcpy(&total, d + 4, 4); memcpy(&idx, d + 8, 2); memcpy(&cnt, d + 10, 2);
    if (!total || total > MAX_BLOB || !cnt || idx >= cnt || (uint32_t)cnt != (total + CHUNK - 1) / CHUNK) return;
    EnterCriticalSection(&s_cs);
    if (!s_rx || s_rx_hash != hash || s_rx_size != total) {
        free(s_rx); free(s_rx_have); s_rx = (uint8_t*)malloc(total); s_rx_have = (uint8_t*)calloc(cnt, 1);
        s_rx_size = total; s_rx_hash = hash; s_rx_count = cnt; s_rx_got = 0;
    }
    if (s_rx && s_rx_have && !s_rx_have[idx]) {
        const uint32_t off = (uint32_t)idx * CHUNK, n = (total - off) < CHUNK ? (total - off) : CHUNK;
        if ((uint32_t)(len - 12) == n) { memcpy(s_rx + off, d + 12, n); s_rx_have[idx] = 1; s_rx_got++; }
        if (s_rx_got == s_rx_count) {
            const uint32_t h = fnv(s_rx, total);
            s_partner_hash = h; s_partner_size = total; InterlockedExchange(&s_partner_hash_ok, h == hash ? 1 : -1);
        }
    }
    LeaveCriticalSection(&s_cs);
}

/* Main thread. Returns 1 when the puppet got the model. */
static int apply_look(uint32_t ent, uint32_t model)
{
    const uint32_t pl = player(); uint32_t vt = 0, vt108 = 0;
    if (!pl || !ent || !model) return 0;
    rd(ent, &vt); rd(vt + 0x108, &vt108);
    printf("[coop-look] v1059L giving puppet %08X (vtable %08X, vt+108 OPD %08X) the MobFace model %08X, skeleton GUID player+2234\n", ent, vt, vt108, model); fflush(stdout);
    if (!vcall(ent, 0x108, model)) return 0;                                     /* SetModel (func_00464818 on characters) */
    if (!gcall_toc(0x00461E30u, GAME_TOC, ent, 0, 0, NULL)) return 0;           /* drop the old anim/skeleton binding */
    if (!gcall_toc(0x00463660u, GAME_TOC, ent, pl + 0x2234u, 0, NULL)) return 0; /* skeleton from the player's GUID + new anim controller */
    if (s_tail) {
        uint32_t m = 0;
        if (rd(ent + 0x64, &m) && ea_ok(m, 4)) { vcall(m, 0x34, 0); }
        vcall(ent, 0x6C, 0);
        vcall(ent, 0x130, 1);
    }
    { uint32_t anim = 0, mdl = 0; rd(ent + 0xC4, &anim); rd(ent + 0x64, &mdl);
      printf("[coop-look] v1059L puppet look applied: model inst +64=%08X anim +C4=%08X\n", mdl, anim); fflush(stdout); }
    return 1;
}

static void send_tick(void)
{
    if (!s_tx || !gf_coop_connected()) return;
    for (int k = 0; k < 4 && s_tx_next < s_tx_count; k++) {   /* a few chunks per frame: the reliable queue has 32 slots */
        uint8_t pkt[12 + CHUNK]; const uint32_t off = (uint32_t)s_tx_next * CHUNK, n = (s_tx_size - off) < CHUNK ? (s_tx_size - off) : CHUNK;
        uint16_t idx = (uint16_t)s_tx_next, cnt = (uint16_t)s_tx_count;
        memcpy(pkt, &s_tx_hash, 4); memcpy(pkt + 4, &s_tx_size, 4); memcpy(pkt + 8, &idx, 2); memcpy(pkt + 10, &cnt, 2); memcpy(pkt + 12, s_tx + off, n);
        if (!gf_coop_send_event(EV_APPEARANCE, pkt, (int)(12 + n))) break;   /* queue full: retry next frame */
        s_tx_next++;
    }
    if (s_tx_next >= s_tx_count) { printf("[coop-look] v1059L sent our MobFace (%u bytes, hash %08X, %d chunks)\n", s_tx_size, s_tx_hash, s_tx_count); fflush(stdout);
        free(s_tx); s_tx = NULL; }
}

/* ================================================================ v1119A partner's own face + names */
static void wr32(uint32_t ea, uint32_t v) { uint8_t* p = vm_base + ea; p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t rd32s(uint32_t ea) { uint32_t v = 0; return rd(ea, &v) ? v : 0; }
enum { FB_EGLOB = 0x10180F88u, FB_CGLOB = 0x10180F8Cu, FB_PGLOB = 0x10181060u, FB_ALLOC = 0x101EC778u, FB_ARG5 = 0x10012630u, FB_EVT = 0x10108DC8u };
static int s_own = -1, s_testface = 0, s_names = -1;
static int s_fb_st; static uint32_t s_fb_E, s_fb_buf, s_fb_bufsz, s_fb_hash, s_fb_model, s_fb_face; static int s_fb_frames, s_fb_fails; static ULONGLONG s_fb_ts;
static uint8_t* s_fb_blob; static uint32_t s_fb_size;   /* host copy of the blob to build */
static int g(uint32_t addr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t* ret) { return gcall_toc(addr, GAME_TOC, a0, a1, a2, ret); }
static int g4(uint32_t addr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    ppu_context* cur = g_active_ctx; gfn f; static ppu_context c;
    if (!cur || !(f = lookup(addr))) return 0;
    __try {
        memcpy(&c, cur, sizeof c); c.gpr[1] = ((uint32_t)cur->gpr[1] - 0x1000u) & ~0xFull; c.gpr[2] = GAME_TOC;
        c.gpr[3] = a0; c.gpr[4] = a1; c.gpr[5] = a2; c.gpr[6] = a3; c.lr = 0; g_active_ctx = &c; f(&c);
        while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&c); }
        g_active_ctx = cur; return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_active_ctx = cur; g_trampoline_fn = 0; printf("[coop-look] v1119A guest call %08X faulted\n", addr); fflush(stdout); return 0; }
}
#define FBLOG(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)
/* v1119L: the bake heap. The model build (func_005E3D70) and the face object (func_005DFF34) run inside a push of P's bake heap
 * H = *(P+0x14) (func_008A5DE8 / _5E5C swap the RW memory hooks); the hooks allocate from the CURRENT heap slot *(0x1028B6B4)
 * (fallback heap *(0x1028B6B0) for 0x10000-hinted requests). H (vtable 0x10113938, ctor func_008A6128(H, size, align)) is a BUMP
 * heap over one 2 MB block (+0x4 size, +0x10 live count, +0x14 used, +0x18 base, +0x1C cursor; free = count-- and reset at 0).
 * Our own face fills it -> v1119A's second bake ran out (alloc returned 0) and RW wrote through null pointers = the q71 world
 * corruption. Fix: the partner gets its OWN 2 MB bump heap H2; the current-heap slot points at H2 only around our bake / release
 * calls (both slots saved and restored; the H2 ctor writes them too). */
enum { FB_HSLOT = 0x1028B6B4u, FB_HSLOT2 = 0x1028B6B0u, FB_HVT = 0x10113938u, FB_HSIZE = 0x1E8480u, LIB_TOC = 0x00B5BF60u };
static uint32_t s_h2, s_p2;   /* v1119L-4: P2 = a copy of the holder P with +0x14 = H2 */
static int h2_ensure(void)
{
    if (s_h2) return 1;
    uint64_t m = 0; if (!g(0x000283D4u, 0x40, 0, 0, &m) || !ea_ok((uint32_t)m, 0x40)) { FBLOG("[coop-look] v1119L new(H2) failed\n"); return 0; }
    memset(vm_base + (uint32_t)m, 0, 0x40);
    const uint32_t a = rd32s(FB_HSLOT), b = rd32s(FB_HSLOT2);
    const int ok = gcall_toc(0x008A6128u, LIB_TOC, (uint32_t)m, FB_HSIZE, 4, NULL);
    wr32(FB_HSLOT, a); wr32(FB_HSLOT2, b);
    const uint32_t base = rd32s((uint32_t)m + 0x18), vt = rd32s((uint32_t)m);
    FBLOG("[coop-look] v1119L partner bake heap H2 %08X: ctor %s, vtable %08X, base %08X size %u | slots %08X %08X (P's heap %08X)\n", (uint32_t)m, ok ? "ok" : "FAILED",
          vt, base, rd32s((uint32_t)m + 4), a, b, rd32s(rd32s(FB_PGLOB) + 0x14));
    if (!ok || vt != FB_HVT || !ea_ok(base, FB_HSIZE)) return 0;
    if (!s_p2) {
        uint64_t q = 0; if (!g(0x000283D4u, 0x90, 0, 0, &q) || !ea_ok((uint32_t)q, 0x90)) { FBLOG("[coop-look] v1119L new(P2) failed\n"); return 0; }
        s_p2 = (uint32_t)q;
    }
    s_h2 = (uint32_t)m; return 1;
}
/* run a guest call with the current bake heap = H2 */
static int with_h2(uint32_t addr, uint32_t toc, uint64_t a0, uint64_t a1, uint64_t* ret)
{
    const uint32_t a = rd32s(FB_HSLOT), b = rd32s(FB_HSLOT2), P = rd32s(FB_PGLOB);
    /* v1119L-4 (l09 GSTACK): the model build passes the heap EXPLICITLY -- func_005DF7F8 -> func_008A3BF4(.., *(*(0x10181060)+0x14))
     * and func_005E3D70 pushes *(*(0x10181060)+0x14) -- so the holder global must point at a P copy whose +0x14 is H2, else the
     * geometry goes into P's full heap (null buffers -> AddressToOffset REFUSED ea 0x3C98.. -> stall / "no player"). */
    if (!s_p2 || !ea_ok(P, 0x88)) return 0;
    memcpy(vm_base + s_p2, vm_base + P, 0x88); wr32(s_p2 + 0x14, s_h2); wr32(FB_PGLOB, s_p2);
    wr32(FB_HSLOT, s_h2);   /* v1119L-3: the fallback slot (0x10000-hinted = raster / GPU memory) stays: zeroing it put textures in H2 -> AddressToOffset REFUSED + stall (l05) */
    const int ok = gcall_toc(addr, toc, a0, a1, 0, ret);
    wr32(FB_HSLOT, a); wr32(FB_HSLOT2, b); wr32(FB_PGLOB, P);
    return ok;
}
/* v1119P: HEAP OWNER REDIRECT. The bump free func_008A5F5C(heap, ptr) range-checks and forwards a pointer that is not in its own
 * block to the FALLBACK heap *(0x1028B6B0) (raster / general memory). The RW free hook func_008A5D90(ptr) frees through the CURRENT
 * heap slot. Some of the partner bake's H2 objects outlive the face (5E1700 left count 65 -> 23, l28) and get freed later (stream
 * unload / editor shutdown) while H2 is not current -> the fallback allocator receives a pointer it does not own = abort 0xABBF14
 * at the t1_rooftop stream-in (l14/l21/l26) and the l28 freeze. Fix: recompiled func_008A5D90 / func_008A5F5C ask us first; a
 * pointer inside H2 (or a retired H2, or P's own heap while H2 is current) is freed in the heap that owns it. Only active once a
 * partner heap exists. */
static uint32_t s_h2_old[4]; static int s_h2_nold; static volatile LONG s_redir_n;
static int in_bump(uint32_t h, uint32_t ptr) { return ptr - rd32(h + 0x18) < rd32(h + 4); }
uint32_t gf_coop_look_heap_owner(uint32_t ptr)   /* any thread; 0 = not ours, let the game's own path run */
{
    if (!s_h2 || ptr < 0x10000u) return 0;
    uint32_t o = 0;
    if (in_bump(s_h2, ptr)) o = s_h2;
    else { for (int i = 0; i < s_h2_nold && !o; i++) if (in_bump(s_h2_old[i], ptr)) o = s_h2_old[i]; }
    if (!o) {   /* P's own heap (also reached while the holder global points at P2, whose +0x14 is H2) */
        const uint32_t P = rd32(FB_PGLOB), Pr = (P == s_p2 && s_p2) ? 0 : P;
        if (Pr >= 0x10000u && Pr < 0xF0000000u) { const uint32_t h = rd32(Pr + 0x14); if (h >= 0x10000u && h < 0xF0000000u && rd32(h) == FB_HVT && in_bump(h, ptr)) o = h; }
        if (!o) return 0;
        if (rd32(FB_HSLOT) == o) return 0;   /* P's heap is current: the game's own path is right */
    }
    InterlockedIncrement(&s_redir_n);
    return o;
}
static void h2_retire(void)   /* H2 still has live objects: keep it (owner lookups) and bake into a fresh one */
{
    if (s_h2_nold >= 4) return;
    s_h2_old[s_h2_nold++] = s_h2; FBLOG("[coop-look] v1119P H2 %08X retired with %u live objects (%u bytes) -- new heap for the next partner face\n", s_h2, rd32s(s_h2 + 0x10), rd32s(s_h2 + 0x14));
    s_h2 = 0;
}
static uint32_t s_fb_puppet;   /* the puppet wearing s_fb_model */
/* v1119L-6 (l14 / l21: other clothes -> abort 0xABBF14 at the next IGC stream unload; an identical copy (l13) was fine): the
 * baked model still uses part resources the editor streamed in (released by its shutdown, E+0x34). So the editor that built the
 * partner's face is PARKED (kept alive, never ticked, global cleared) for as long as that face is in use, and only shut down
 * when a newer partner face replaces it. GF_COOP_LOOK_KEEPE=0 = shut it down at once (old behaviour). */
static uint32_t s_fb_keepE; static int s_fb_resume, s_keepE = -1;
static void fb_delete(const char* why)
{
    const uint32_t E = rd32s(FB_EGLOB);
    if (E && E == s_fb_E && rd32s(E) == FB_EVT) {
        const uint32_t opd = rd32s(FB_EVT + 4); const int r = gcall_toc(rd32s(opd), rd32s(opd + 4), E, 1, 0, NULL);
        if (rd32s(FB_EGLOB) == E) wr32(FB_EGLOB, 0);
        FBLOG("[coop-look] v1119A editor %08X deleted (%s) %s\n", E, why, r ? "ok" : "FAILED");
    } else FBLOG("[coop-look] v1119A editor not deleted (%s): global %08X ours %08X\n", why, E, s_fb_E);
    s_fb_E = 0; s_fb_st = 0;
}
static void fb_shutdown(const char* why)
{
    if (!s_fb_E || rd32s(FB_EGLOB) != s_fb_E) { FBLOG("[coop-look] v1119A shutdown (%s): editor not ours (%08X)\n", why, rd32s(FB_EGLOB)); s_fb_E = 0; s_fb_st = 0; return; }
    if (!g(0x00010EACu, s_fb_E, 0, 0, NULL)) { FBLOG("[coop-look] v1119A shutdown faulted -- editor left alive\n"); s_fb_E = 0; s_fb_st = 0; return; }
    FBLOG("[coop-look] v1119A editor shutdown (%s)\n", why); s_fb_st = 5; s_fb_frames = 0; s_fb_ts = GetTickCount64();
}
/* main thread, one step per frame. Returns the built model when done (s_fb_model). */
static void fb_tick(void)
{
    const ULONGLONG now = GetTickCount64();
    switch (s_fb_st) {
    case 0: return;
    case 1: {   /* create */
        const uint32_t P = rd32s(FB_PGLOB), face = P ? rd32s(P + 0x1C) : 0;
        if (rd32s(FB_EGLOB) || rd32s(FB_CGLOB)) return;   /* the tailor / MobFace screen owns the editor: wait */
        if (!ea_ok(P, 0x40) || !ea_ok(face, 12) || !player()) return;
        {   /* v1119L: only in gameplay as the created character (q71 ran during the prologue title card, model mode 0) */
            extern int gf_cutscene_active(void); uint32_t mode = 0; rd(player() + 0x1A64, &mode);
            /* v1119L-2: +0x1A64 is NOT the current mode (func_001BC370 only reads it; 0 in every run) -> gate on: no cutscene, our
             * baked face present, player alive (the H2 heap is what makes the build safe) */
            (void)mode;
            if (gf_cutscene_active() || !ea_ok(rd32s(face), 4)) { static ULONGLONG nl; if (now >= nl) { nl = now + 10000; FBLOG("[coop-look] v1119L partner face waits:%s%s\n", gf_cutscene_active() ? " cutscene" : "", ea_ok(rd32s(face), 4) ? "" : " no baked face yet"); } return; }
        }
        if (!h2_ensure()) { s_fb_fails = 3; s_fb_st = 0; FBLOG("[coop-look] v1119L no partner bake heap -- partner keeps our face\n"); return; }
        if (s_fb_face) {   /* v1119L: a previous partner face lives in H2: take it off the puppet and release it (H2 resets at count 0) */
            if (s_fb_puppet && s_fb_puppet == gf_coop_puppet_entity()) vcall(s_fb_puppet, 0x108, 0);
            const int r = with_h2(0x005E1700u, LIB_TOC, s_fb_face, 0, NULL);
            FBLOG("[coop-look] v1119L old partner face %08X released %s: H2 used %u count %u\n", s_fb_face, r ? "ok" : "FAILED", rd32s(s_h2 + 0x14), rd32s(s_h2 + 0x10));
            s_fb_face = 0; s_fb_model = 0; s_fb_puppet = 0; s_applied_ent = 0;
        }
        if (s_fb_keepE) {   /* v1119L-6: now shut down the parked editor of the old face, then come back here */
            s_fb_E = s_fb_keepE; s_fb_keepE = 0; wr32(FB_EGLOB, s_fb_E); s_fb_resume = 1;
            FBLOG("[coop-look] v1119L shutting down the parked editor %08X of the old partner face\n", s_fb_E);
            fb_shutdown("old partner face replaced"); return;
        }
        if (rd32s(s_h2 + 0x14) != 0 || rd32s(s_h2 + 0x10) != 0) {   /* H2 must be empty before a bake */
            /* v1119P: resetting the cursor overwrote objects still in use (count 23 after the release, l28) -> retire it instead */
            if (s_h2_nold >= 4) { s_fb_fails = 3; s_fb_st = 0; FBLOG("[coop-look] v1119P 4 retired partner heaps still in use -- partner keeps our face\n"); return; }
            h2_retire();
            if (!h2_ensure()) { s_fb_fails = 3; s_fb_st = 0; FBLOG("[coop-look] v1119P no fresh partner bake heap -- partner keeps our face\n"); return; }
        }
        uint64_t mem = 0;
        if (!g(0x000283D4u, 0x88, 0, 0, &mem) || !ea_ok((uint32_t)mem, 0x88)) { s_fb_fails++; s_fb_st = 0; FBLOG("[coop-look] v1119A new(0x88) failed\n"); return; }
        if (!g4(0x00013ADCu, mem, rd32s(FB_ALLOC), FB_ARG5, 0)) { s_fb_fails++; s_fb_st = 0; return; }
        s_fb_E = rd32s(FB_EGLOB);
        if (s_fb_E != (uint32_t)mem) { FBLOG("[coop-look] v1119A editor ctor did not publish (%08X)\n", s_fb_E); s_fb_E = 0; s_fb_fails++; s_fb_st = 0; return; }
        if (rd32s(s_fb_E + 0x38) == 0) wr32(s_fb_E + 0x38, 1);
        FBLOG("[coop-look] v1119A building the partner's face (blob %u bytes, hash %08X): editor %08X created\n", s_fb_size, s_fb_hash, s_fb_E);
        s_fb_st = 2; s_fb_ts = now; s_fb_frames = 0; return; }
    case 2: case 3: {   /* tick to 4, parse, settle */
        if (rd32s(FB_EGLOB) != s_fb_E) { FBLOG("[coop-look] v1119A editor global changed under us -- abort\n"); s_fb_E = 0; s_fb_st = 0; s_fb_fails++; return; }
        if (!g(0x00011C3Cu, s_fb_E, 0, 0, NULL)) { s_fb_fails++; fb_shutdown("update faulted"); return; }
        s_fb_frames++;
        const uint32_t es = rd32s(s_fb_E + 0x38);
        if (now - s_fb_ts > 20000) { s_fb_fails++; FBLOG("[coop-look] v1119A editor timeout (state %u)\n", es); fb_shutdown("timeout"); return; }
        if (s_fb_st == 2 && es == 4) {
            const uint32_t P = rd32s(FB_PGLOB), face = rd32s(P + 0x1C);
            if (!s_fb_buf || s_fb_bufsz < s_fb_size) { uint64_t m = 0; if (!g(0x000283D4u, s_fb_size + 16, 0, 0, &m) || !ea_ok((uint32_t)m, s_fb_size)) { s_fb_fails++; fb_shutdown("no blob buffer"); return; } s_fb_buf = (uint32_t)m; s_fb_bufsz = s_fb_size; }
            memcpy(vm_base + s_fb_buf, s_fb_blob, s_fb_size);
            const uint32_t ob = rd32s(face + 4), os = rd32s(face + 8);
            wr32(face + 4, s_fb_buf); wr32(face + 8, s_fb_size);
            uint64_t r = 0; const int ok = g(0x00010C9Cu, P, 0, 0, &r);   /* func_005E9340(P): parse the blob into E + generate */
            wr32(face + 4, ob); wr32(face + 8, os);
            FBLOG("[coop-look] v1119A parse of the partner's blob %s (%u)\n", ok ? "ok" : "FAILED", (uint32_t)r);
            if (!ok) { s_fb_fails++; fb_shutdown("parse faulted"); return; }
            s_fb_st = 3; s_fb_frames = 0; return;
        }
        if (s_fb_st == 3 && s_fb_frames <= 10) FBLOG("[coop-look] v1119L settle tick %d (state %u)\n", s_fb_frames, es);
        if (s_fb_st == 3 && es == 4 && s_fb_frames >= 10) {
            const uint32_t P = rd32s(FB_PGLOB); uint64_t face2 = 0;
            const uint32_t pf = rd32s(P + 0x1C), ph = rd32s(P + 0x14), pu0 = ph ? rd32s(ph + 0x14) : 0;
            FBLOG("[coop-look] v1119L bake step 1: face object (132AC)\n");
            int ok = with_h2(0x000132ACu, GAME_TOC, s_fb_E, rd32s(P + 0x18), &face2) && ea_ok((uint32_t)face2, 12);
            FBLOG("[coop-look] v1119L bake step 2: model (12B7C)\n");
            const uint32_t u1 = rd32s(s_h2 + 0x14);
            ok = ok && with_h2(0x00012B7Cu, GAME_TOC, s_fb_E, face2, NULL);
            const uint32_t mdl = ok ? rd32s((uint32_t)face2) : 0, u2 = rd32s(s_h2 + 0x14), base = rd32s(s_h2 + 0x18);
            const int in_h2 = ok && (uint32_t)face2 >= base && (uint32_t)face2 < base + FB_HSIZE && mdl >= base && mdl < base + FB_HSIZE;
            FBLOG("[coop-look] v1119L partner face %s: face %08X model %08X (in H2 %d) | H2 used %u -> %u -> %u of %u, count %u | our heap %08X used %u -> %u, our face %08X model %08X\n",
                  ok && ea_ok(mdl, 4) ? "BUILT" : "FAILED", (uint32_t)face2, mdl, in_h2, 0u, u1, u2, FB_HSIZE, rd32s(s_h2 + 0x10), ph, pu0, ph ? rd32s(ph + 0x14) : 0, pf, rd32s(pf));
            if (ok && u2 >= FB_HSIZE - 0x1000) { FBLOG("[coop-look] v1119L H2 FULL -- partner face not used\n"); ok = 0; }
            if (ok && ea_ok(mdl, 4)) { s_fb_face = (uint32_t)face2; s_fb_model = mdl; } else { s_fb_fails++; s_fb_face = ea_ok((uint32_t)face2, 12) ? (uint32_t)face2 : 0; s_fb_model = 0; }
            {   /* v1119P diagnostic (l26: parking the editor did NOT stop abort 0xABBF14 at the t1_rooftop stream-in): GF_COOP_LOOK_DISCARD=1
                 * = bake, then release the partner face at once and shut the editor down; the puppet keeps our face. Survives -> the LIVE
                 * partner model is what the stream-in trips on; still aborts -> the editor's extra part streaming itself is the cause. */
                static int s_discard = -1; if (s_discard < 0) { const char* e = getenv("GF_COOP_LOOK_DISCARD"); s_discard = (e && *e == '1'); }
                if (s_discard && ea_ok((uint32_t)face2, 12)) {
                    const int r = with_h2(0x005E1700u, LIB_TOC, (uint32_t)face2, 0, NULL);
                    FBLOG("[coop-look] v1119P DISCARD test: partner face %08X released %s (H2 used %u count %u) -- puppet keeps our face\n", (uint32_t)face2, r ? "ok" : "FAILED", rd32s(s_h2 + 0x14), rd32s(s_h2 + 0x10));
                    s_fb_face = 0; s_fb_model = 0; s_fb_fails = 3;
                    fb_shutdown("discard test"); return;
                }
            }
            if (s_keepE < 0) { const char* e = getenv("GF_COOP_LOOK_KEEPE"); s_keepE = !(e && *e == '0'); }
            if (ok && s_keepE && rd32s(FB_EGLOB) == s_fb_E) {   /* v1119L-6: park it */
                s_fb_keepE = s_fb_E; wr32(FB_EGLOB, 0); s_fb_E = 0; s_fb_st = 0;
                FBLOG("[coop-look] v1119L editor %08X parked (keeps the partner's part resources; global cleared)\n", s_fb_keepE); return;
            }
            fb_shutdown(ok ? "partner face built" : "bake failed"); return;
        }
        return; }
    case 5: {   /* shutdown: tick until released, then delete */
        if (!s_fb_E || rd32s(FB_EGLOB) != s_fb_E) { s_fb_E = 0; s_fb_st = 0; return; }
        uint32_t es = rd32s(s_fb_E + 0x38);
        if (es == 5) { if (!g(0x00011C3Cu, s_fb_E, 0, 0, NULL)) { s_fb_E = 0; s_fb_st = 0; return; } }
        s_fb_frames++; es = rd32s(s_fb_E + 0x38);
        if (es != 5 || s_fb_frames >= 30) {
            if (es == 6 || es == 0) { fb_delete("released"); if (s_fb_resume) { s_fb_resume = 0; s_fb_st = 1; } }
            else if (now - s_fb_ts > 10000) { FBLOG("[coop-look] v1119A editor stuck in state %u -- global cleared (leak)\n", es); if (rd32s(FB_EGLOB) == s_fb_E) wr32(FB_EGLOB, 0); s_fb_E = 0; s_fb_st = 0; }
        }
        return; }
    default: s_fb_st = 0; return;
    }
}
/* v1119N CHARACTER names: the name typed at the game's name entry is stored by the APT native SetPlayerName (func_00388108)
 * into the string object at *(0x10181064) + 8 {ptr, len, cap} (the player profile / clothing manager, TOC+0x443C). Every 1 s
 * we read it; on change, on (re)connect and every 10 s we send it to everyone on event 0x01A7 (direct + relay). The peer
 * handler stores it per peer id; names_tick prefers it over the co-op name. */
enum { EV_CHARNAME = 0x01A7 };
static char s_cn_peer[GF_COOP_MAX_PEERS][32]; static volatile LONG s_cn_peer_ok[GF_COOP_MAX_PEERS];
static char s_cn_mine[32]; static ULONGLONG s_cn_next_send;
static int local_charname(char out[32])
{
    const uint32_t mgr = rd32s(0x10181064u); if (!ea_ok(mgr, 0x20)) return 0;
    const uint32_t ptr = rd32s(mgr + 8), len = rd32s(mgr + 12);
    if (!len || len > 30 || !ea_ok(ptr, len)) return 0;
    __try { memcpy(out, vm_base + ptr, len); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    out[len] = 0; for (uint32_t i = 0; i < len; i++) if ((unsigned char)out[i] < 0x20) return 0;
    return 1;
}
static void on_peer_event(int peer_id, uint16_t type, const void* data, int len)   /* co-op thread */
{
    if (type != EV_CHARNAME || peer_id < 0 || peer_id >= GF_COOP_MAX_PEERS || len < 2 || len > 31) return;
    char nm[32]; memcpy(nm, data, (size_t)len); nm[len] = 0;
    for (int i = 0; i < len; i++) if ((unsigned char)nm[i] < 0x20) return;
    if (s_cs_ok) EnterCriticalSection(&s_cs);
    const int changed = strcmp(s_cn_peer[peer_id], nm) != 0;
    memcpy(s_cn_peer[peer_id], nm, (size_t)len + 1); InterlockedExchange(&s_cn_peer_ok[peer_id], 1);
    if (s_cs_ok) LeaveCriticalSection(&s_cs);
    if (changed) { printf("[coop-look] v1119N peer %d character name \"%s\"\n", peer_id, nm); fflush(stdout); }
}
static int peer_charname(int id, char out[24])
{
    if (id < 0 || id >= GF_COOP_MAX_PEERS || !s_cn_peer_ok[id]) return 0;
    if (s_cs_ok) EnterCriticalSection(&s_cs);
    snprintf(out, 24, "%s", s_cn_peer[id]);
    if (s_cs_ok) LeaveCriticalSection(&s_cs);
    return out[0] != 0;
}
static void charname_tick(void)
{
    static int reg = 0; if (!reg && s_cs_ok) { reg = 1; gf_coop_add_peer_event_handler(on_peer_event); }
    const ULONGLONG now = GetTickCount64();
    static ULONGLONG next_rd; static int was = 0; const int c = gf_coop_connected();
    if (c && !was) s_cn_next_send = 0;   /* (re)connect: send now */
    was = c;
    if (now < next_rd) return; next_rd = now + 1000;
    char nm[32]; if (!local_charname(nm)) return;
    if (strcmp(nm, s_cn_mine)) { snprintf(s_cn_mine, sizeof s_cn_mine, "%s", nm); s_cn_next_send = 0; printf("[coop-look] v1119N our character name \"%s\"\n", nm); fflush(stdout); }
    if (!c || now < s_cn_next_send) return;
    const int n = (int)strlen(s_cn_mine);
    const int a = gf_coop_send_event(EV_CHARNAME, s_cn_mine, n);
    const int b = gf_coop_npeer_active() ? gf_coop_send_event_relay(EV_CHARNAME, s_cn_mine, n) : 0;
    if (a || b) s_cn_next_send = now + 10000;
}
/* v1119A names: char+0x1824 = GetName string {ptr, len, cap} */
static int set_name(uint32_t ent, const char* nm)
{
    ppu_context* cur = g_active_ctx; if (!cur || !ent || !nm || !*nm) return 0;
    const uint32_t scr = ((uint32_t)cur->gpr[1] - 0x800u) & ~0xFu; size_t n = strlen(nm); if (n > 40) n = 40;
    __try { memcpy(vm_base + scr + 0x10, nm, n); vm_base[scr + 0x10 + n] = 0; wr32(scr, scr + 0x10); wr32(scr + 4, (uint32_t)n); wr32(scr + 8, (uint32_t)n + 1); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    const uint32_t str = ent + 0x1824u; char before[48] = { 0 };
    { const uint32_t bp = rd32s(str), bl = rd32s(str + 4); if (bp && bl && bl < 47 && ea_ok(bp, bl)) memcpy(before, vm_base + bp, bl); }
    if (!g(0x0040F008u, str, scr, 0, NULL)) return 0;
    uint64_t gn = 0; g(0x0012D5BCu, ent, 0, 0, &gn);   /* read it back through the game's GetName */
    char after[48] = { 0 }; { const uint32_t bp = rd32s((uint32_t)gn), bl = rd32s((uint32_t)gn + 4); if (bp && bl && bl < 47 && ea_ok(bp, bl)) memcpy(after, vm_base + bp, bl); }
    FBLOG("[coop-look] v1119A puppet %08X name \"%s\" -> \"%s\" (GetName %08X)\n", ent, before, after, (uint32_t)gn);
    return !strcmp(after, nm) || !strncmp(after, nm, n);
}
static void names_tick(void)
{
    if (s_names < 0) { const char* e = getenv("GF_COOP_NAMES"); s_names = !(e && *e == '0'); FBLOG("[coop-look] v1119A partner names on the targeting HUD %s (GF_COOP_NAMES)\n", s_names ? "ON" : "off"); }
    if (!s_names) return;
    charname_tick();   /* v1119N */
    static struct { uint32_t ent; char nm[24]; int fails; ULONGLONG seen; } nt[GF_COOP_MAX_PEERS + 1];
    const ULONGLONG now = GetTickCount64();
    uint32_t ents[GF_COOP_MAX_PEERS + 1]; char nms[GF_COOP_MAX_PEERS + 1][24]; int k = 0;
    { const uint32_t pe = gf_coop_puppet_entity(); if (pe) { ents[k] = pe; int got = 0;   /* v1119N: the partner's CHARACTER name, co-op name until it arrives */
        for (int id = 0; id < GF_COOP_MAX_PEERS && !got; id++) if (s_cn_peer_ok[id] && gf_coop_peer_is_primary_id(id)) got = peer_charname(id, nms[k]);
        if (!got) for (int id = 0; id < GF_COOP_MAX_PEERS && !got; id++) if (s_cn_peer_ok[id] && gf_coop_peer_count() < 0) got = peer_charname(id, nms[k]);   /* classic 2-player: the only peer */
        if (!got) snprintf(nms[k], 24, "%s", gf_coop_partner_name()); k++; } }
    { const int n = gf_coop_peer_count(); for (int i = 0; i < n && k <= GF_COOP_MAX_PEERS; i++) { const int id = gf_coop_peer_id(i); if (id < 0 || gf_coop_peer_is_primary_id(id)) continue;
        const uint32_t e = gf_coop_peer_puppet(id); if (!e) continue; ents[k] = e; if (!peer_charname(id, nms[k]) && !gf_coop_peer_name_id(id, nms[k])) nms[k][0] = 0; k++; } }
    for (int j = 0; j < k; j++) {
        if (!nms[j][0]) continue;
        int s; for (s = 0; s <= GF_COOP_MAX_PEERS; s++) if (nt[s].ent == ents[j]) break;
        if (s > GF_COOP_MAX_PEERS) { for (s = 0; s <= GF_COOP_MAX_PEERS; s++) if (!nt[s].ent) break; if (s > GF_COOP_MAX_PEERS) continue; memset(&nt[s], 0, sizeof nt[s]); nt[s].ent = ents[j]; nt[s].seen = now; }
        if (!strcmp(nt[s].nm, nms[j]) || nt[s].fails >= 3 || now - nt[s].seen < 2000) continue;
        if (set_name(ents[j], nms[j])) snprintf(nt[s].nm, 24, "%s", nms[j]); else { nt[s].fails++; nt[s].seen = now; }
    }
    for (int s = 0; s <= GF_COOP_MAX_PEERS; s++) { int alive = 0; for (int j = 0; j < k; j++) if (ents[j] == nt[s].ent) alive = 1; if (!alive) memset(&nt[s], 0, sizeof nt[s]); }
}

extern void gf_coop_npclook_init(void); extern void gf_coop_npclook_tick(void);   /* v1127L gf_coop_npclook.c */
void gf_coop_look_init(void)
{
    gf_coop_npclook_init();   /* v1127L NPC look sync (own flag GF_COOP_NPC_LOOK) */
    const char* e = getenv("GF_COOP_LOOK"); if (e && *e == '0') s_on = 0;
    if ((e = getenv("GF_COOP_LOOK_TAIL")) && *e == '1') s_tail = 1;
    if ((e = getenv("GF_COOP_LOOK_ANYFACE")) && *e == '0') s_anyface = 0;
    if (!s_on) { printf("[coop-look] v1059L off (GF_COOP_LOOK=0)\n"); fflush(stdout); return; }
    InitializeCriticalSection(&s_cs); s_cs_ok = 1;
    gf_coop_set_event_handler(on_event);
    printf("[coop-look] v1059L ON: puppet gets the MobFace model, face blob sync on event 0x%04X (tail %d, anyface %d)\n", EV_APPEARANCE, s_tail, s_anyface); fflush(stdout);
}

void gf_coop_look_tick(void)   /* main PPU thread */
{
    gf_coop_npclook_tick();   /* v1127L */
    if (!s_on) return;
    const ULONGLONG now = GetTickCount64();
    uint32_t model = 0, blob = 0, size = 0;
    const int have = local_face(&model, &blob, &size);
    /* 1. capture + send on change (1 s) */
    if (now >= s_next_cap) {
        s_next_cap = now + 1000;
        if (have && blob && size && size <= MAX_BLOB && ea_ok(blob, size)) {
            uint8_t* tmp = (uint8_t*)malloc(size); int ok = 0;
            if (tmp) { __try { memcpy(tmp, vm_base + blob, size); ok = 1; } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; } }
            if (ok) {
                const uint32_t h = fnv(tmp, size);
                if (!s_my_valid || h != s_my_hash || size != s_my_size) {
                    uint32_t pl = player(), mode = 0; if (pl) rd(pl + 0x1A64, &mode);
                    printf("[coop-look] v1059L our MobFace: blob %08X %u bytes hash %08X, baked model %08X, player model mode %u (1 = MobFace)\n", blob, size, h, model, mode); fflush(stdout);
                    s_my_hash = h; s_my_size = size; s_my_valid = 1;
                    free(s_tx); s_tx = tmp; tmp = NULL; s_tx_size = size; s_tx_hash = h; s_tx_next = 0; s_tx_count = (int)((size + CHUNK - 1) / CHUNK);
                }
            }
            free(tmp);
        }
        /* resend after a (re)connect: the partner may have joined after our first send */
        { static int was = 0; const int c = gf_coop_connected(); if (c && !was && s_my_valid && !s_tx) s_my_valid = 0; was = c; }
        { const LONG ph = s_partner_hash_ok;
          if (ph && !s_logged_partner) { s_logged_partner = 1;
            printf("[coop-look] v1059L partner MobFace received: %u bytes hash %08X %s -> %s\n", s_partner_size, s_partner_hash, ph > 0 ? "intact" : "CORRUPT",
                   s_partner_hash == s_my_hash ? "SAME face as ours (exact look)" : "DIFFERENT face (puppet shows OUR face for now; second-face build not done)"); fflush(stdout); } }
    }
    send_tick();
    /* v1119A */
    if (s_own < 0) { const char* e = getenv("GF_COOP_LOOK_OWN"); s_own = (e && *e == '1');   /* v1119P: OFF by default again -- different clothes still abort (uncaught C++ exception, l21/l26/l31; l30 passed by luck); GF_COOP_LOOK_OWN=1 to test */   /* v1119L: ON again (own bake heap H2 + holder copy P2, see v1119L) -- v1119A-2 had it OFF by default -- q71: the second bake
         * (no 141AC release) corrupted shared memory: world geometry exploded during the prologue title card, then a flip stall */ if ((e = getenv("GF_COOP_LOOK_TESTFACE")) && *e) s_testface = atoi(e);   /* v1119L: 1 other clothes, 2 identical copy */
        FBLOG("[coop-look] v1119A partner's own face %s (GF_COOP_LOOK_OWN; test face %d)\n", s_own ? "ON" : "off", s_testface); }
    names_tick();
    { static LONG last; static ULONGLONG nl; if (s_h2 && now >= nl) { nl = now + 10000; const LONG r = s_redir_n; if (r != last) { last = r;
        FBLOG("[coop-look] v1119P heap owner redirects so far %ld | H2 %08X live %u used %u | retired %d\n", r, s_h2, rd32s(s_h2 + 0x10), rd32s(s_h2 + 0x14), s_h2_nold); } } }
    if (s_own) {
        fb_tick();
        /* what should the partner look like? its blob when received and different, or (test) ours with other clothes */
        uint32_t want = 0;
        if (s_testface && s_my_valid) want = s_my_hash ^ 0x5EED5EEDu;
        else if (s_partner_hash_ok > 0 && s_partner_hash != s_my_hash) want = s_partner_hash;
        if (want && want != s_fb_hash && !s_fb_st && s_fb_fails < 3 && gf_coop_puppet_entity() && now - s_puppet_seen_at > 3000) {
            uint8_t* src = NULL; uint32_t sz = 0;
            if (s_testface) {
                if (have && blob && size && size <= MAX_BLOB && (src = (uint8_t*)malloc(size))) { __try { memcpy(src, vm_base + blob, size); sz = size; } __except (EXCEPTION_EXECUTE_HANDLER) { free(src); src = NULL; } }
                if (src && sz >= 0x60 && s_testface == 1) { static const uint16_t off[3] = { 0x44, 0x50, 0x54 }; static const uint32_t cnt[3] = { 9, 46, 7 };
                    for (int i = 0; i < 3; i++) { uint32_t v = ((uint32_t)src[off[i]] << 24) | ((uint32_t)src[off[i] + 1] << 16) | ((uint32_t)src[off[i] + 2] << 8) | src[off[i] + 3];
                        v = (v + (i == 1 ? 9 : 3)) % cnt[i]; src[off[i]] = (uint8_t)(v >> 24); src[off[i] + 1] = (uint8_t)(v >> 16); src[off[i] + 2] = (uint8_t)(v >> 8); src[off[i] + 3] = (uint8_t)v; } }
            } else if (s_cs_ok) {
                EnterCriticalSection(&s_cs);
                if (s_rx && s_rx_size == s_partner_size && (src = (uint8_t*)malloc(s_rx_size))) { memcpy(src, s_rx, s_rx_size); sz = s_rx_size; }
                LeaveCriticalSection(&s_cs);
            }
            if (src && sz) { free(s_fb_blob); s_fb_blob = src; s_fb_size = sz; s_fb_hash = want; s_fb_model = 0; s_fb_st = 1; s_fb_ts = now; }
            else free(src);
        }
    }
    /* 2b. v1072X: extra puppets (3+ players) get the same MobFace look; before this they kept the bare player model */
    static int s_lx = -1; if (s_lx < 0) { const char* e = getenv("GF_COOP_LOOK_EXTRAS"); s_lx = !(e && *e == '0');
        printf("[coop-look] v1072X MobFace look on extra puppets %s (GF_COOP_LOOK_EXTRAS; costs ~3 ms/frame per extra puppet in echo x72-2/x72-4)\n", s_lx ? "ON" : "off"); fflush(stdout); }
    if (s_lx && have && model && (s_anyface || !s_partner_hash_ok || s_partner_hash == s_my_hash)) {
        extern int gf_coop_extra_puppets(uint32_t* out, int max);
        static struct { uint32_t ent, model; ULONGLONG seen; int fails; } xl[8];
        uint32_t live[8]; const int n = gf_coop_extra_puppets(live, 8);
        for (int k = 0; k < 8; k++) { int alive = 0; for (int j = 0; j < n; j++) if (live[j] == xl[k].ent) alive = 1; if (!alive) memset(&xl[k], 0, sizeof xl[k]); }
        for (int j = 0; j < n; j++) {
            int k, f = -1; for (k = 0; k < 8; k++) if (xl[k].ent == live[j]) break;
            if (k == 8) { for (k = 0; k < 8; k++) if (!xl[k].ent) { f = k; break; } if (f < 0) continue; k = f; xl[k].ent = live[j]; xl[k].model = 0; xl[k].seen = now; xl[k].fails = 0; }
            if (xl[k].model == model || xl[k].fails >= 3 || now - xl[k].seen < 1500) continue;
            if (apply_look(live[j], model)) { xl[k].model = model; printf("[coop-look] v1072X extra puppet %08X got the MobFace look\n", live[j]); fflush(stdout); }
            else { xl[k].fails++; xl[k].seen = now; printf("[coop-look] v1072X extra puppet %08X look failed (%d/3)\n", live[j], xl[k].fails); fflush(stdout); }
        }
    }
    /* 2. puppet look */
    const uint32_t ent = gf_coop_puppet_entity();
    if (ent != s_last_puppet) { s_last_puppet = ent; s_puppet_seen_at = now; s_apply_fail = 0; }
    if (ent && (!have || !model)) { static int once; if (!once) { once = 1; printf("[coop-look] v1059L no baked MobFace model yet (face object %s, model %08X) -- puppet keeps its own look\n", have ? "found" : "missing", model); fflush(stdout); } }
    if (!ent || !have || !model) return;
    if (s_own && s_fb_model && ea_ok(s_fb_model, 4)) model = s_fb_model;   /* v1119A: the partner's own face once built */
    else if (s_own && s_fb_st) return;   /* building it: keep the current look until it is ready */
    if (!s_anyface && s_partner_hash_ok && s_partner_hash != s_my_hash) return;
    if (ent == s_applied_ent && model == s_applied_model) return;
    if (now - s_puppet_seen_at < 1500 || s_apply_fail >= 3) return;   /* let the spawn / post-init settle first */
    if (apply_look(ent, model)) {
        s_applied_ent = ent; s_applied_model = model;
        if (model == s_fb_model) {
            uint32_t inst = 0; rd(ent + 0x64, &inst);
            if (!ea_ok(inst, 4)) {   /* v1119L-5 (l14): SetModel made no instance (+0x64 = 0) -> later abort 0xABBF14 in an IGC: back to our face */
                FBLOG("[coop-look] v1119L partner model %08X gave NO model instance on puppet %08X -- puppet gets our face instead\n", model, ent);
                s_fb_model = 0; s_fb_fails = 3;
                if (have && local_face(&model, &blob, &size) && model && apply_look(ent, model)) { s_applied_model = model; }
            } else s_fb_puppet = ent;
        }
    }
    else { s_apply_fail++; s_puppet_seen_at = now; printf("[coop-look] v1059L apply failed (%d/3)\n", s_apply_fail); fflush(stdout); }
}
