/* gf_coop_proxy.c -- v1060W3 world sync, guest side: proxies of the host's pedestrians and cars (Claude).
 * Contract: gf_coop_world.h. W2 (gf_coop_world.c) calls gf_proxy_spawn / drive / release / release_all; this module
 * runs its own per-frame work from gf_coop_glue_tick (gf_proxy_tick) and gf_coop_after_anim (gf_proxy_late).
 *
 * Spawn: the managed factory func_003388AC(classHash, attrib, 0, 0) (same path as the partner puppet, v1058I).
 *   attrib = template registry func_004AD7F0(*(0x10181088), &guid) for the host's template GUID; if that archetype is
 *   not loaded here: a live (hash, attrib) noted by the factory hook this session for the same class (verified
 *   attrib -> func_004530A8 -> GUID -> registry -> same attrib); else any live template of the same kind (ped / car),
 *   logged as a substitute. Characters whose model was not resident get iMsgManagedEntityCreated (func_00455A2C) like
 *   the puppet. Placement: vtable +0x30 SetMatrix + vtable +0x24 {x,y,z,2}. Crew classes: func_001281AC (AI off).
 * Drive: every frame the shown transform moves toward the target (position extrapolated by the host velocity up to
 *   0.25 s, exponential smoothing, snap beyond 6 m; rotation nlerp) -> vt+0x30 (characters) / vt+0x2C (cars: writes the
 *   Havok body ent+0x138); characters also get the frame-only vt+0x2C right after their anim update (gf_proxy_late) so
 *   their own AI / collision cannot move them.
 * Budget: at most 2 factory spawns per frame, cap 64 proxies. Invalid entity or > 4 m from target for 120 frames ->
 *   release and respawn (area changes).
 * Env: GF_COOP_PROXY=0 off; GF_COOP_PROXY_TEST=1 = self test (3 peds + 1 car circling 4 m ahead of the player).
 * Log prefix [coop-proxy]. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "recompiled/ppu_recomp.h"
#include "gf_coop_world.h"
#include "gf_memq.h"   /* v1061E */

extern uint8_t* vm_base;
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);

typedef void (*gfn)(ppu_context*);
enum { GAME_TOC = 0x00B4BF60u };
enum { PX_MAX = 64, PX_SPAWN_PER_FRAME = 2, PX_NOTES = 256, PX_LIVE = 48 };
enum { PS_FREE = 0, PS_PENDING, PS_LIVE, PS_DEAD };

typedef struct {
    int state;
    uint16_t netid;
    uint8_t kind;
    uint32_t cls, used_cls;
    uint8_t guid[16];
    uint32_t ent;
    float tgt[12], vel[3];      /* last target from the host */
    ULONGLONG tgt_ms;
    float cur[12];              /* shown transform */
    int have_cur;
    int far_frames;
    int tries;
    ULONGLONG next_try;
    float late_m[16]; int late_ok;
    uint32_t vt;                /* v1061B: vtable at spawn (freed + reused memory check) */
    int alive_bit;              /* v1061B: *(ent+0x34) bit 0 was set at spawn (func_00455F58 clears it on release) */
    int warp_cool, nwarp;       /* v1061G: re-warp (vt+0x24) instead of release+respawn when the body drifts */
    uint32_t attrib;            /* v1061G: template attrib used (pool reuse key) */
    int adopted;                /* v1063P2: an existing guest entity bound by gf_proxy_adopt (H) -> release only unbinds */
    int look;                   /* v1063P5: 0 unknown, 1 model GUID == host NPC model (wire face), 2 different model */
    ULONGLONG look_at; int look_tries;   /* v1063P5 retry while the model is not bound yet */
    int nomodel_said;           /* v1063P6 */
    int exact_tmpl;             /* v1064Q: template resolved by the host's attrib GUID (same skeleton as the host ped) */
    float spd; double spd_t; float spd_p[3]; int spd_ok;   /* v1066N: horizontal root speed (m/s) from the driven target, for the wpose donor fallback */
    ULONGLONG spawn_ms;         /* v1070D: entity bound (factory / pool reuse) at, for the no-model census */
    ULONGLONG lk_next; int lk_state, lk_fails;   /* v1124H per-ped look (tints + head texture) applied from the host's ext v3 */
    int mig;                    /* v1126M: adopted = OUR native demoted by a migration (ownership moved away) -> release = game release too */
} proxy;

static proxy s_px[PX_MAX];
static int s_late = 1;   /* v1061B GF_COOP_PROXY_LATE=0: no late vt+0x2C (perf test) */
static int s_on = -1, s_test = 0, s_cars = 1, s_drive30 = 1; static uint32_t s_pedas = 0;   /* v1061B GF_COOP_PROXY_PEDAS=<hex class> / _DRIVE30=0 (perf tests) */   /* v1061B GF_COOP_PROXY_CARS=0: no car proxies (diagnostic) */
static int s_call_fail;
static int s_spawned_this_frame;
static unsigned s_n_spawn, s_n_fail, s_n_sub, s_n_respawn, s_n_late;
static ULONGLONG s_last_ms, s_log_at;
/* v1061C settle gate: no proxy spawns until the guest world has been stable (same player entity, no frame > 250 ms)
 * for GF_COOP_PROXY_SETTLE_MS (default 10000). Spawning through the managed factory while a save / area / mission is
 * still loading aborted the game at 0x00ABBF14 (coop-local join 2026-11-05 00:39, w3-3-test, wa-11). */
static ULONGLONG s_settle_until; static int s_settle_ms = -1; static unsigned s_n_settle_skips;
static uint32_t player(void);
static void calm_log(void);   /* v1062W1 */
/* v1063P1 diag GF_COOP_PROXY_WWATCH=<n arms>: aim the runtime's live store watch (ppu_loader.cpp g_ww_live, needs
 * PPU_WWATCH=<any> at launch) at the world position (xf+0x40) of a live ped proxy / its echo source ped, right after its
 * anim update -> stderr [ww] lines name every guest function that moves the body (first hit per arm: guest stack). */
extern volatile uint32_t g_ww_live, g_ww_live_len; extern volatile long g_ww_live_gen; extern uint32_t g_ww_lo, g_ww_hi;
extern int gf_coop_world_host_peds(uint32_t* ents, uint16_t* ids, float (*pos)[3], int max);
extern int gf_coop_mode(void);   /* v1063P5 */
static int s_ww = -1, s_ww_k, s_ww_pending, s_ww_pi; static ULONGLONG s_ww_next; static uint32_t s_ww_target; static uint16_t s_ww_net;
/* v1062W3: in-world loads (area / mission / interior streaming) show up as frames > 250 ms. Calling into live proxies
 * (vt+0x30 / +0x2C / +0x24, pool re-warps) while the game tears down / streams entities is the prime suspect for the
 * abort at 0x00ABBF14 seen with live proxies. Hold every proxy call for 2 s after such a frame. */
static ULONGLONG s_hold_until; static unsigned s_n_hold;
static int px_settled(ULONGLONG now) { return s_settle_ms >= 0 && now >= s_settle_until && player() != 0; }
static uint32_t s_dead_ent[64]; static ULONGLONG s_dead_ms[64]; static int s_dead_i;   /* v1061B: released proxies stay 'proxy' for 10 s */
static unsigned s_n_exit, s_n_vtgone, s_n_world;
static unsigned s_n_adopt, s_n_unbind;   /* v1063P2 */
static unsigned s_n_nomodel;   /* v1063P6 */
static int s_setmodel = -1; static unsigned s_n_setmodel, s_n_setmodel_fail, s_n_setmodel_nores;
static unsigned s_n_look_match, s_n_look_diff, s_n_look_unk, s_n_ext_tx, s_n_ext_rx;
static unsigned s_n_ext_inst, s_n_ext_saved, s_n_ext_none;   /* v1064Q host ext GUID source */
static int s_body_every = -1; static unsigned s_frame_no;   /* v1063P10 */
static int s_smooth = -1;   /* v1063P3 GF_COOP_PROXY_SMOOTH=1: old proxy-level exp smoothing (lagged ~80 ms behind the pose stream) */
static int s_lookdiag = -1; static ULONGLONG s_lookdiag_at;   /* v1063P3 GF_COOP_PROXY_LOOKDIAG=1 (echo): proxy vs source appearance fields */
static unsigned s_n_warp, s_n_orphan_anim; static uint32_t s_orphan_seen[64]; static int s_orphan_n;   /* v1061G */
/* v1061G proxy POOL: the game's release func_00455F58 does NOT remove our entities (they never entered the managed-
 * entity lists: owner = requester = 0) -- we-2-diag "ORPHAN: released ent still animated 7.9 s after release". Every
 * release+respawn left a body standing in the world = the user's NPC cluster. Now unused proxies are parked 400 m
 * below where they were (re-warped every 1 s so they cannot fall into a kill plane) and reused by the next spawn of
 * the same class + template. Only world changes / invalid entities still go through the old release. */
enum { POOL_MAX = 128 };   /* v1128G (agent F28b): was 64 -- every overflow went to func_00455F58 = a leaked live body (c28-1/c28-5 joiner freeze) */
typedef struct { uint32_t ent, vt, cls, attrib; uint8_t kind; int alive_bit; float park[12]; ULONGLONG warp_at; } pooled;
static pooled s_pool[POOL_MAX]; static int s_pool_on = -1; static unsigned s_n_park, s_n_reuse, s_n_pool_lost;
/* v1128G knobs: GF_COOP_PROXY_NOREL=0 -> old func_00455F58 release on world change / entity gone; GF_COOP_PROXY_CLASSREUSE=0 -> exact
 * template reuse only; GF_COOP_PROXY_REVIVE=0 -> drop game-released bodies as before. */
static int s_norel = -1, s_classreuse = -1, s_revive = -1; static unsigned s_n_revive, s_n_classreuse, s_n_full_rel;
static void g_knobs(void) { const char* e; if (s_norel >= 0) return;
    e = getenv("GF_COOP_PROXY_NOREL"); s_norel = !(e && *e == '0'); e = getenv("GF_COOP_PROXY_CLASSREUSE"); s_classreuse = !(e && *e == '0');
    e = getenv("GF_COOP_PROXY_REVIVE"); s_revive = !(e && *e == '0');
    printf("[coop-proxy] v1128G bounded bodies: no game release of our bodies %s, class reuse %s, revive game-released bodies %s, pool %d\n",
           s_norel ? "ON" : "off", s_classreuse ? "ON" : "off", s_revive ? "ON" : "off", (int)POOL_MAX); fflush(stdout); }

/* ---------------------------------------------------------------- guest memory + calls (as gf_coop_puppet.c) */
static int ea_range(uint32_t ea, size_t n) { return ea >= 0x10000u && ea < 0xF0000000u && n <= 0xF0000000u - ea; }
static int mem_access(uint32_t ea, size_t n, int write)   /* v1061E: cached (gf_memq.h) -- was one VirtualQuery per read */
{
    return gf_memq_access(vm_base, ea, n, write);
}
static uint32_t rd32(uint32_t ea) { const uint8_t* p = vm_base + ea; return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void wr32(uint32_t ea, uint32_t v) { uint8_t* p = vm_base + ea; p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static int read_u32(uint32_t ea, uint32_t* out)
{
    if (!mem_access(ea, 4, 0)) return 0;
    __try { *out = rd32(ea); return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
/* v1068Z (agent Z): func_004530A8(attrib) walks records p = attrib+0x14 while (*(p+4) != 0x40000000) p += *p (when byte +0x10
 * bit 0 is clear). An attrib whose block was freed / zeroed / reused (area change) makes that loop spin FOREVER on the main thread:
 * t68-6-echo FLIP_STALL ~165 s after "entity gone (area change)" -- every [HANG] RIP symbolized (build-perf pdb) to
 * func_004530A8 ppu_recomp_002.cpp:63433-63435 (+ vm_read32). Pre-walk it here, bounded and with checked reads; 0 = the game's
 * walk would not terminate -> the caller treats the attrib as invalid. GF_COOP_ATTRIB_GUARD=0 = old (no pre-walk). */
static int attrib_walk_ok(uint32_t at)
{
    static int s_ag = -1; uint32_t w = 0, p, step = 0, tag = 0; int i;
    if (s_ag < 0) { const char* e = getenv("GF_COOP_ATTRIB_GUARD"); s_ag = !(e && *e == '0');
        printf("[coop-attrib] v1068Z %s: template attrib walk (func_004530A8) pre-checked %s (GF_COOP_ATTRIB_GUARD)\n", __FILE__, s_ag ? "ON" : "off"); fflush(stdout); }
    if (!s_ag) return 1;
    if (!read_u32(at + 0x10u, &w)) return 0;
    if ((w >> 24) & 1u) return 1;   /* byte +0x10 bit 0 set: the game takes the other (non-looping) branch */
    p = at + 0x14u;
    for (i = 0; i < 16384; i++) {
        if (!read_u32(p + 4u, &tag)) return 0;
        if (tag == 0x40000000u) return 1;
        if (!read_u32(p, &step) || !step) { static int nl; if (nl++ < 10) { printf("[coop-attrib] v1068Z attrib %08X: record walk stuck at %08X (step 0) -> attrib rejected (would hang func_004530A8)\n", at, p); fflush(stdout); } return 0; }
        p += step;   /* 32-bit wrap like the game's rldicl */
    }
    { static int nl; if (nl++ < 10) { printf("[coop-attrib] v1068Z attrib %08X: record walk did not end after %d steps -> attrib rejected\n", at, i); fflush(stdout); } }
    return 0;
}
static int read_f(uint32_t ea, float* out) { uint32_t u; if (!read_u32(ea, &u)) return 0; memcpy(out, &u, 4); return 1; }
static int read_bytes(uint32_t ea, void* out, size_t n)
{
    if (!mem_access(ea, n, 0)) return 0;
    __try { memcpy(out, vm_base + ea, n); return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static uint32_t player(void)
{
    uint32_t a;
    if (!read_u32(0x10181290u, &a) || !ea_range(a, 8)) return 0;
    if (!read_u32(a + 4, &a) || !ea_range(a, 4)) return 0;
    if (!read_u32(a, &a) || a < 0x17FCu) return 0;
    a -= 0x17FCu;
    return ea_range(a, 4) ? a : 0;
}
static gfn lookup(uint32_t addr)
{
    static uint32_t ca[64]; static gfn cf[64]; static int cn;
    uint64_t i; gfn f = 0;
    for (i = 0; i < (uint64_t)cn; i++) if (ca[i] == addr) return cf[i];
    for (i = 0; i < function_table_count; i++)
        if ((uint32_t)function_table[i].addr == addr) { f = function_table[i].func; break; }
    if (cn < 64) { ca[cn] = addr; cf[cn] = f; cn++; }
    if (!f) { printf("[coop-proxy] func_%08X is not in the function table\n", addr); fflush(stdout); }
    return f;
}
static uint64_t gcall_raw(gfn f, uint32_t toc, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    ppu_context* cur = g_active_ctx;
    static ppu_context c;
    if (!cur || !f) return 0;
    memcpy(&c, cur, sizeof c);
    c.gpr[1] = ((uint32_t)cur->gpr[1] - 0x1800u) & ~0xFull;   /* below the scratch block (sp - 0x800) */
    c.gpr[2] = toc;
    c.gpr[3] = a0; c.gpr[4] = a1; c.gpr[5] = a2; c.gpr[6] = a3; c.gpr[7] = a4;
    c.lr = 0;
    g_active_ctx = &c;
    f(&c);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&c); }
    g_active_ctx = cur;
    return c.gpr[3];
}
static uint64_t gcall_toc(uint32_t addr, uint32_t toc, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    ppu_context* cur = g_active_ctx; gfn f;
    if (!cur || !(f = lookup(addr))) return 0;
    __try { return gcall_raw(f, toc, a0, a1, a2, a3, a4); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_active_ctx = cur; g_trampoline_fn = 0; s_call_fail++;
        printf("[coop-proxy] guest call %08X faulted\n", addr); fflush(stdout); return 0;
    }
}
static uint64_t gcall(uint32_t addr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    return gcall_toc(addr, GAME_TOC, a0, a1, a2, a3, a4);
}
static uint32_t scratch(void)
{
    uint32_t ea;
    if (!g_active_ctx) return 0;
    ea = ((uint32_t)g_active_ctx->gpr[1] - 0x800u) & ~0xFu;
    return mem_access(ea, 0x100, 1) ? ea : 0;
}
static int entity_info(uint32_t ent, uint32_t* vt, uint32_t* xf)
{
    uint32_t v, x, opd;
    if (!ea_range(ent, 0x6Cu) || !read_u32(ent, &v) || !ea_range(v, 0x40u) || !v) return 0;
    if (!read_u32(ent + 0x68u, &x) || !ea_range(x, 0x50u) || !mem_access(x + 0x10u, 0x40, 0)) return 0;
    if (!read_u32(v + 0x2Cu, &opd) || !ea_range(opd, 8) || !mem_access(opd, 8, 0)) return 0;
    if (vt) *vt = v; if (xf) *xf = x;
    return 1;
}
static int call_virtual(uint32_t ent, uint32_t slot, uint32_t a1)
{
    uint32_t vt, opd, fn, toc; int before = s_call_fail;
    if (!read_u32(ent, &vt) || !ea_range(vt + slot, 4) || !read_u32(vt + slot, &opd) ||
        !ea_range(opd, 8) || !read_u32(opd, &fn) || !read_u32(opd + 4, &toc) || !fn || !toc) return 0;
    if (!lookup(fn)) return 0;
    gcall_toc(fn, toc, ent, a1, 0, 0, 0);
    return s_call_fail == before;
}
static int call_virtual3(uint32_t ent, uint32_t slot, uint32_t a1, uint32_t a2, uint32_t a3)   /* v1061B */
{
    uint32_t vt, opd, fn, toc; int before = s_call_fail;
    if (!read_u32(ent, &vt) || !ea_range(vt + slot, 4) || !read_u32(vt + slot, &opd) ||
        !ea_range(opd, 8) || !read_u32(opd, &fn) || !read_u32(opd + 4, &toc) || !fn || !toc) return 0;
    if (!lookup(fn)) return 0;
    gcall_toc(fn, toc, ent, a1, a2, a3, 0);
    return s_call_fail == before;
}
/* v1061B car despawn (HANDOFF vehicle RE): everyone out first -- driver veh+0x104, occupants +0x10C / +0x110 hold
 * handles = character+0x38; exit = vt+0x1E0(veh, character, desc8). Proxies never get an AI driver (no 0x9B0 |= 0x400,
 * occupant GUIDs +0x79C/+0x7AC/+0x9CC untouched), so this only fires when a local character (the guest player) sits in it. */
static void car_exit_all(uint32_t veh)
{
    static const uint32_t seats[3] = {0x104u, 0x10Cu, 0x110u};
    uint32_t buf = scratch();
    for (int k = 0; k < 3; k++) {
        uint32_t h = 0, ch;
        if (!read_u32(veh + seats[k], &h) || !h || !ea_range(h, 4) || h < 0x38u) continue;
        ch = h - 0x38u;
        if (!buf || !entity_info(ch, 0, 0)) continue;
        { int okw = 0; __try { memset(vm_base + buf + 0xA0, 0, 8); okw = 1; } __except (EXCEPTION_EXECUTE_HANDLER) { okw = 0; } if (!okw) continue; }
        s_n_exit++;
        printf("[coop-proxy] v1061B car %08X seat +%03X occupied by %08X -> exit before release\n", veh, seats[k], ch); fflush(stdout);
        call_virtual3(veh, 0x1E0u, ch, buf + 0xA0, 0);
    }
}
/* m12 (rows r,u,a,p) -> RwMatrix at dst; padding words copied from the entity's own matrix */
static int write_matrix12(uint32_t dst, uint32_t source_xf, const float m[12])
{
    if (!mem_access(dst, 0x40, 1) || !mem_access(source_xf + 0x10u, 0x40, 0)) return 0;
    __try {
        for (int r = 0; r < 4; r++) {
            for (int k = 0; k < 3; k++) { uint32_t u; memcpy(&u, &m[r * 3 + k], 4); wr32(dst + (uint32_t)(r * 16 + k * 4), u); }
            wr32(dst + (uint32_t)(r * 16 + 12), rd32(source_xf + 0x10u + (uint32_t)(r * 16 + 12)));
        }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int read_xf12(uint32_t xf, float m[12])
{
    for (int r = 0; r < 4; r++) for (int k = 0; k < 3; k++)
        if (!read_f(xf + 0x10u + (uint32_t)(r * 16 + k * 4), &m[r * 3 + k])) return 0;
    return 1;
}

/* ---------------------------------------------------------------- template notes (factory hook) */
/* gf_proxy_factory_note is called from gf_coop_char_factory_note (entry of func_003388AC, every managed spawn of this
 * session, PPU thread): record only. The main tick validates and files them per class. */
static uint32_t s_fn_hash[PX_NOTES], s_fn_attrib[PX_NOTES]; static volatile LONG s_fn_head = 0; static LONG s_fn_done = 0; static volatile LONG s_fn_seq[PX_NOTES];
static uint32_t s_live_cls[PX_LIVE], s_live_attrib[PX_LIVE]; static uint8_t s_live_kind[PX_LIVE]; static int s_live_n = 0;
void gf_proxy_factory_note(uint32_t hash, uint32_t attrib)
{
    const LONG i = InterlockedIncrement(&s_fn_head) - 1;
    s_fn_hash[i & (PX_NOTES - 1)] = hash; s_fn_attrib[i & (PX_NOTES - 1)] = attrib;
    MemoryBarrier(); s_fn_seq[i & (PX_NOTES - 1)] = i + 1;   /* v1063P5: publish the slot AFTER its payload (Codex L #15) */
}
static uint8_t kind_of_class(uint32_t h)
{
    if (h == 0x10E5319Eu || h == 0xFE169CA3u) return GF_WK_CAR;
    return GF_WK_PED;   /* characters (crew, peds, cops) -- refined by the entity check after spawn */
}
static int is_crew(uint32_t h) { return h == 0xBDA2CAC1u || h == 0xD1812E99u || h == 0xF48F270Eu; }
static void live_add(uint32_t h, uint32_t at)
{
    int i;
    if (!h || !ea_range(at, 0x20)) return;
    for (i = 0; i < s_live_n; i++) if (s_live_cls[i] == h) { s_live_attrib[i] = at; return; }
    if (s_live_n < PX_LIVE) {
        s_live_cls[s_live_n] = h; s_live_attrib[s_live_n] = at; s_live_kind[s_live_n] = kind_of_class(h); s_live_n++;
        printf("[coop-proxy] live template class %08X attrib %08X (%s), %d known\n", h, at, kind_of_class(h) == GF_WK_CAR ? "car" : "char", s_live_n);
        fflush(stdout);
    }
}
static void notes_drain(void)
{
    const LONG head = s_fn_head;
    if (head - s_fn_done > PX_NOTES) s_fn_done = head - PX_NOTES;
    for (; s_fn_done < head; s_fn_done++) {
        { const LONG q = s_fn_seq[s_fn_done & (PX_NOTES - 1)];
          if (q < s_fn_done + 1) break;        /* v1063P5: payload not published yet -> next tick */
          if (q > s_fn_done + 1) continue; }   /* lapped by a newer note: skip */
        MemoryBarrier();
        live_add(s_fn_hash[s_fn_done & (PX_NOTES - 1)], s_fn_attrib[s_fn_done & (PX_NOTES - 1)]);
    }
}
/* attrib still registered? attrib -> GUID -> registry -> attrib */
static int attrib_valid(uint32_t reg, uint32_t at)
{
    int before = s_call_fail; uint32_t gp, back;
    if (!ea_range(at, 0x20)) return 0;
    if (!attrib_walk_ok(at)) return 0;   /* v1068Z */
    gp = (uint32_t)gcall(0x004530A8u, at, 0, 0, 0, 0);
    if (s_call_fail != before || !ea_range(gp, 16) || !mem_access(gp, 16, 0)) return 0;
    back = (uint32_t)gcall(0x004AD7F0u, reg, gp, 0, 0, 0);
    return s_call_fail == before && back == at;
}
static int s_resolve_exact;   /* v1064Q: last resolve_attrib found the host's own template (by GUID) */
static uint32_t resolve_attrib(proxy* p, uint32_t buf, uint32_t* cls_out)
{
    s_resolve_exact = 0;
    uint32_t reg, at = 0; int nz = 0, before = s_call_fail, i;
    if (!read_u32(0x10181088u, &reg) || !ea_range(reg, 4)) return 0;
    for (i = 0; i < 16; i++) nz |= p->guid[i];
    if (nz) {
        __try { memcpy(vm_base + buf + 0x60, p->guid, 16); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        at = (uint32_t)gcall(0x004AD7F0u, reg, buf + 0x60, 0, 0, 0);
        if (s_call_fail != before) at = 0;
        if (at) { *cls_out = p->cls; s_resolve_exact = 1; return at; }   /* v1064Q exact */
    }
    for (i = 0; i < s_live_n; i++) if (s_live_cls[i] == p->cls) {
        if (attrib_valid(reg, s_live_attrib[i])) { *cls_out = p->cls; return s_live_attrib[i]; }
        s_live_attrib[i] = 0;
    }
    /* substitute: any live template of the same kind, preferring non-crew for peds */
    for (int pass = 0; pass < 2; pass++)
        for (i = 0; i < s_live_n; i++) {
            if (!s_live_attrib[i] || s_live_kind[i] != p->kind) continue;
            if (pass == 0 && p->kind == GF_WK_PED && is_crew(s_live_cls[i])) continue;
            if (attrib_valid(reg, s_live_attrib[i])) {
                *cls_out = s_live_cls[i]; s_n_sub++;
                if (s_n_sub <= 20 || s_n_sub % 50 == 0) {
                    printf("[coop-proxy] net %u: class %08X not loaded here -> substitute class %08X\n", p->netid, p->cls, s_live_cls[i]); fflush(stdout); }
                return s_live_attrib[i];
            }
            s_live_attrib[i] = 0;
        }
    return 0;
}

/* ---------------------------------------------------------------- spawn / release */
static proxy* find(uint16_t netid)
{
    for (int i = 0; i < PX_MAX; i++) if (s_px[i].state != PS_FREE && s_px[i].netid == netid) return &s_px[i];
    return 0;
}
static int px_enabled(void)
{
    if (s_on < 0) {
        const char* e = getenv("GF_COOP_PROXY"); s_on = !(e && *e == '0');
        e = getenv("GF_COOP_PROXY_TEST"); s_test = e && *e ? atoi(e) : 0;
        e = getenv("GF_COOP_PROXY_CARS"); s_cars = !(e && *e == '0');
        e = getenv("GF_COOP_PROXY_PEDAS"); s_pedas = e && *e ? (uint32_t)strtoul(e, NULL, 16) : 0;   /* spawn every ped proxy as this class (crew BDA2CAC1 = AI off) */
        e = getenv("GF_COOP_PROXY_DRIVE30"); s_drive30 = !(e && *e == '0');
        e = getenv("GF_COOP_PROXY_SETTLE_MS"); s_settle_ms = e && *e ? atoi(e) : 10000; if (s_settle_ms < 0) s_settle_ms = 0;
        s_settle_until = GetTickCount64() + (ULONGLONG)s_settle_ms;
        e = getenv("GF_COOP_PROXY_LATE"); s_late = !(e && *e == '0');   /* 0: peds only get the late vt+0x2C set, no per-tick vt+0x30 */
        if (s_pedas || !s_drive30 || !s_cars || !s_late) { printf("[coop-proxy] v1061B perf options: ped class override %08X, per-tick SetMatrix %s, cars %s, late set %s\n", s_pedas, s_drive30 ? "on" : "OFF", s_cars ? "on" : "OFF", s_late ? "on" : "OFF"); fflush(stdout); }
        printf("[coop-proxy] v1060W3 world proxies %s%s (cap %d, %d spawns/frame)\n", s_on ? "ON" : "off", s_test ? ", SELF TEST" : "", PX_MAX, PX_SPAWN_PER_FRAME);
        fflush(stdout);
    }
    return s_on;
}
/* v1061B: is p->ent still OUR live entity? vtable unchanged since spawn and, if it had one, the managed-entity alive bit
 * (bit 0 of *(ent+0x34), cleared by the game's own release func_00455F58 -- e.g. when an area / the frontend scene
 * unloads). A failed check = the game owns / freed it: never call into it again (w3-3-test aborted inside a load). */
static int ent_ok(const proxy* p, uint32_t* vt_out, uint32_t* xf_out)
{
    uint32_t vt = 0, xf = 0, f = 0;
    if (!p->ent || !entity_info(p->ent, &vt, &xf)) return 0;
    if (p->vt && vt != p->vt) return 0;
    if (p->alive_bit && (!read_u32(p->ent + 0x34u, &f) || !(f & 1u))) return 0;
    if (vt_out) *vt_out = vt; if (xf_out) *xf_out = xf;
    return 1;
}
static int place(proxy* p, uint32_t ent, const float m[12], int with_block);   /* v1061G (pool parking) */
static void look_check(proxy* p);   /* v1063P5 */
static void look_apply(proxy* p, ULONGLONG now);   /* v1124H */
static void look_log(void);   /* v1124H */
static void nomodel_fix(proxy* p);   /* v1064Q */
static int preguid_write(proxy* p, uint32_t ent);   /* v1064Q */
static void look_census(void);   /* v1064Q */
static void look_dump2(ULONGLONG now);   /* v1124G */
static void kill_ent(proxy* p, const char* why)
{
    const uint32_t ent = p->ent;
    p->ent = 0; p->late_ok = 0; p->have_cur = 0; p->far_frames = 0;
    if (!ent) return;
    { extern void gf_coop_pose_face_forget(uint32_t e); gf_coop_pose_face_forget(ent); }   /* v1063P5 */
    { extern void gf_wpose_forget(uint32_t e); gf_wpose_forget(ent); }   /* v1063P9 */
    if (p->kind == GF_WK_CAR) { extern void gf_veh_forget(uint32_t e); gf_veh_forget(ent); }   /* v1063V */
    p->look = 0; p->look_tries = 0; p->nomodel_said = 0;
    if (p->adopted) {   /* v1063P2: the guest's own entity (mission scripts own it): never release / park it */
        p->adopted = 0; s_n_unbind++;
        printf("[coop-proxy] v1063P2 net %u unbind adopted ent=%08X (%s)\n", p->netid, ent, why); fflush(stdout);
        if (p->mig && !strcmp(why, "release all")) { p->mig = 0; printf("[migrate] v1126M net %u demoted native %08X handed back to our game (session over)\n", p->netid, ent); fflush(stdout); }
        else if (p->mig) {   /* v1126M: a native we demoted (the new owner simulates it): unbound it would run on unsynced here = a duplicate -> the game's release */
            extern int gf_world_release_native(uint32_t e); int ok = 0; p->mig = 0;
            __try { ok = gf_world_release_native(ent); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
            printf("[migrate] v1126M net %u demoted native %08X released with its proxy: %s\n", p->netid, ent, ok ? "game release" : "refused (gone / not ambient)"); fflush(stdout); }
        return;
    }
    { static unsigned nlog; if (++nlog <= 40 || nlog % 50 == 0 || strcmp(why, "release")) { printf("[coop-proxy] net %u release ent=%08X (%s)\n", p->netid, ent, why); fflush(stdout); } }
    s_dead_ent[s_dead_i & 63] = ent; s_dead_ms[s_dead_i & 63] = GetTickCount64(); s_dead_i++;   /* v1061B */
    {   /* v1061B: only release what is still OUR entity (same vtable as at spawn); freed + reused memory is left alone */
        proxy tmp = *p; tmp.ent = ent;
        g_knobs();
        if (s_pool_on < 0) { const char* e = getenv("GF_COOP_PROXY_POOL"); s_pool_on = !(e && *e == '0'); }
        if (!ent_ok(&tmp, 0, 0)) {   /* v1128G: alive bit cleared by the game's death / area reload but the body is still there (same vtable,
                                      * not the destroyed base) -> park it instead of leaking it (c28-5: most ORPHANs were "entity gone (area change)") */
            uint32_t v2 = 0, x2 = 0;
            if (s_revive && s_pool_on && p->attrib && entity_info(ent, &v2, &x2) && v2 == p->vt && v2 != 0x1005F6C8u) {
                for (int i = 0; i < POOL_MAX; i++) if (s_pool[i].ent == ent) { s_n_revive++; return; }
                for (int i = 0; i < POOL_MAX; i++) if (!s_pool[i].ent) {
                    pooled* q = &s_pool[i]; float m[12];
                    q->ent = ent; q->vt = p->vt; q->cls = p->used_cls; q->attrib = p->attrib; q->kind = p->kind; q->alive_bit = 0;
                    memcpy(m, p->have_cur ? p->cur : p->tgt, sizeof m); m[10] -= 400.0f; memcpy(q->park, m, sizeof m); q->warp_at = 0;
                    if (p->kind == GF_WK_CAR) car_exit_all(ent);
                    { proxy t2 = *p; t2.ent = ent; place(&t2, ent, q->park, 1); }
                    s_n_park++; s_n_revive++; return; } }
            s_n_vtgone++; return; }
        if (s_pool_on && (s_norel || (strncmp(why, "world change", 12) != 0 && strncmp(why, "entity gone", 11) != 0)) && p->attrib) {   /* v1128G: s_norel parks on world change too */
            for (int i = 0; i < POOL_MAX; i++) if (!s_pool[i].ent) {
                pooled* q = &s_pool[i]; float m[12];
                q->ent = ent; q->vt = p->vt; q->cls = p->used_cls; q->attrib = p->attrib; q->kind = p->kind; q->alive_bit = p->alive_bit;
                memcpy(m, p->have_cur ? p->cur : p->tgt, sizeof m); m[10] -= 400.0f;
                memcpy(q->park, m, sizeof m); q->warp_at = 0;
                if (p->kind == GF_WK_CAR) car_exit_all(ent);
                { proxy t2 = *p; t2.ent = ent; place(&t2, ent, q->park, 1); }
                s_n_park++;
                return;
            }
            if (s_norel) { s_n_full_rel++; static unsigned nl; if (++nl <= 10 || nl % 100 == 0) { printf("[coop-proxy] v1128G pool full (%d) -> body %08X released the old way (%u)\n", (int)POOL_MAX, ent, s_n_full_rel); fflush(stdout); } }
        }
        if (p->kind == GF_WK_CAR) car_exit_all(ent);
        gcall(0x00455F58u, ent + 0x30u, 0, 0, 0, 0);
    }
}
static int place(proxy* p, uint32_t ent, const float m[12], int with_block)
{
    uint32_t vt, xf, buf = scratch(); int before = s_call_fail;
    const uint32_t slot = p->kind == GF_WK_CAR ? 0x2Cu : 0x30u;
    if (!buf || !entity_info(ent, &vt, &xf) || !write_matrix12(buf, xf, m)) return 0;
    if (!call_virtual(ent, slot, buf) || s_call_fail != before) return 0;
    if (with_block) {
        const uint32_t blk = buf + 0x40u; uint32_t u;
        __try { memcpy(&u, &m[9], 4); wr32(blk, u); memcpy(&u, &m[10], 4); wr32(blk + 4, u); memcpy(&u, &m[11], 4); wr32(blk + 8, u); wr32(blk + 12, 2u); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        call_virtual(ent, 0x24u, blk);
    }
    if (p->kind != GF_WK_CAR) {
        for (int r = 0; r < 4; r++) { for (int k = 0; k < 3; k++) p->late_m[r * 4 + k] = m[r * 3 + k]; p->late_m[r * 4 + 3] = 0.0f; }
        p->late_ok = 1;
    }
    return s_call_fail == before;
}
static void try_spawn(proxy* p)
{
    uint32_t buf = scratch(), attrib, cls = p->cls, ent, vt, xf, c4 = 0; int before = s_call_fail;
    const ULONGLONG now = GetTickCount64();
    if (!buf) return;
    p->tries++;
    p->next_try = now + (p->tries < 4 ? 500u : p->tries < 10 ? 2000u : 8000u);
    attrib = 0;
    if (s_pedas && p->kind == GF_WK_PED) {   /* v1061B: ped class override -> a live template of that class */
        uint32_t reg = 0; read_u32(0x10181088u, &reg);
        for (int i = 0; i < s_live_n && !attrib; i++) if (s_live_cls[i] == s_pedas && s_live_attrib[i] && attrib_valid(reg, s_live_attrib[i])) { attrib = s_live_attrib[i]; cls = s_pedas; }
    }
    p->exact_tmpl = 0;   /* v1064Q */
    if (!attrib) { attrib = resolve_attrib(p, buf, &cls); p->exact_tmpl = s_resolve_exact; }
    if (!attrib || s_call_fail != before) {
        if (p->tries <= 2 || p->tries % 20 == 0) { printf("[coop-proxy] net %u class %08X: no loaded template yet (try %d)\n", p->netid, p->cls, p->tries); fflush(stdout); }
        s_n_fail++; return;
    }
    p->attrib = attrib;
    {   /* v1061G: reuse a parked proxy of the same class + template (no factory call, no new body) */
        g_knobs();
        for (int pass = 0; pass < 2; pass++) for (int i = 0; i < POOL_MAX; i++) {   /* v1128G pass 1: any parked body of the same class */
            pooled* q = &s_pool[i]; uint32_t v = 0, x = 0, f = 0;
            if (!q->ent || q->cls != cls || q->kind != p->kind || (pass == 0 && q->attrib != attrib) || (pass == 1 && (!s_classreuse || q->attrib == attrib))) continue;
            if (!entity_info(q->ent, &v, &x) || v != q->vt || v == 0x1005F6C8u) { memset(q, 0, sizeof *q); s_n_pool_lost++; continue; }
            if (q->alive_bit && (!read_u32(q->ent + 0x34u, &f) || !(f & 1u))) { if (s_revive) { q->alive_bit = 0; s_n_revive++; } else { memset(q, 0, sizeof *q); s_n_pool_lost++; continue; } }   /* v1128G */
            if (pass == 1) { s_n_classreuse++; p->look = 0; p->look_tries = 0; }   /* v1128G other template: look_check re-checks */
            p->ent = q->ent; p->vt = q->vt; p->used_cls = cls; p->alive_bit = q->alive_bit; p->state = PS_LIVE; p->far_frames = 0; p->warp_cool = 0; p->spawn_ms = GetTickCount64();   /* v1070D */
            memset(q, 0, sizeof *q);
            memcpy(p->cur, p->tgt, sizeof p->cur); p->have_cur = 1;
            if (!place(p, p->ent, p->tgt, 1)) { kill_ent(p, "entity gone (reuse placement failed)"); p->state = PS_PENDING; return; }
            s_n_reuse++; s_n_spawn++;
            look_check(p);   /* v1063P5 */
            return;
        }
    }
    {   /* v1128F (agent F28): every factory body lost to func_00455F58 stays alive and holds render / anim handle pools; c28-1 B reached
         * 331 of ours -> a handle alloc returned NULL-0x40 -> main-thread freeze. No NEW factory body while >= GF_COOP_PROXY_ENT_CAP (160,
         * 0 = off) of ours are alive; the proxy stays pending (the normal retry). Count cached 250 ms. */
        extern int gf_world_ours_alive(void); static int cap = -1; static ULONGLONG t_n; static int n_ours; static long n_capped;
        if (cap < 0) { const char* e = getenv("GF_COOP_PROXY_ENT_CAP"); cap = e && *e ? atoi(e) : 160; printf("[coop-proxy] v1128F factory body cap %d (GF_COOP_PROXY_ENT_CAP)\n", cap); fflush(stdout); }
        if (cap > 0) { const ULONGLONG tn = GetTickCount64(); if (tn - t_n >= 250) { t_n = tn; int live = 0, parked = 0;   /* v1128G: count from OUR tables (worldre "ours" records were never pruned) */
                for (int i = 0; i < PX_MAX; i++) if (s_px[i].ent && !s_px[i].adopted) live++;
                for (int i = 0; i < POOL_MAX; i++) if (s_pool[i].ent) parked++;
                n_ours = live + parked + (int)s_orphan_n; { const int w = gf_world_ours_alive(); if (w < n_ours) n_ours = w; } }
            if (n_ours >= cap) { n_capped++; if (n_capped <= 10 || (n_capped % 2000) == 0) { printf("[coop-proxy] v1128F net %u: %d of our bodies alive >= cap %d -> spawn deferred (%ld)\n", p->netid, n_ours, cap, n_capped); fflush(stdout); } return; } } }
    s_spawned_this_frame++;
    ent = (uint32_t)gcall(0x003388ACu, cls, attrib, 0, 0, 0);
    if (s_call_fail != before || !ent || !entity_info(ent, &vt, &xf)) {
        printf("[coop-proxy] net %u factory failed class %08X attrib %08X -> %08X\n", p->netid, cls, attrib, ent); fflush(stdout);
        s_n_fail++; return;
    }
    if (p->kind != GF_WK_CAR) {
        read_u32(ent + 0xC4u, &c4);
        if (!c4) {   /* model not resident at construction: NPC appearance init (iMsgManagedEntityCreated), as the puppet */
            uint32_t id = 0; read_u32(0x101DBFD8u, &id);
            preguid_write(p, ent);   /* v1064Q: the game builds the HOST ped's model, not the template's random pick */
            __try { wr32(buf + 0x70, id); wr32(buf + 0x74, ent); } __except (EXCEPTION_EXECUTE_HANDLER) { }
            gcall(0x00455A2Cu, buf + 0x70, ent + 0x30u, 0, 0, 0);
        }
    }
    for (int i = 0; i < POOL_MAX; i++) if (s_pool[i].ent == ent) { memset(&s_pool[i], 0, sizeof s_pool[i]); s_n_pool_lost++; }   /* v1064K: a fresh factory entity can reuse a freed parked body's address -> forget the stale pool entry */
    p->ent = ent; p->vt = vt; p->used_cls = cls; p->state = PS_LIVE; p->far_frames = 0; p->spawn_ms = GetTickCount64();   /* v1070D */
    { uint32_t f = 0; p->alive_bit = read_u32(ent + 0x34u, &f) && (f & 1u); }   /* v1061B */
    memcpy(p->cur, p->tgt, sizeof p->cur); p->have_cur = 1;
    if (!place(p, ent, p->tgt, 1)) { kill_ent(p, "initial placement failed"); p->state = PS_PENDING; s_n_fail++; return; }
    {   /* v1063P9 GF_COOP_PROXY_SAFE_SPAWN=1 (Codex J): skip func_001281AC -- it is a crew teardown (flags, msg 0xA6, manager
         * relink, vt+0x24 to y = -25), not an AI switch. Our proxies are not in the manager update lists anyway (p63-2 wwatch:
         * func_00241614 brain/HSM never runs for them), so it only matters for the puppet-freeze class of bugs. Default off (old). */
        static int safe = -1; if (safe < 0) { const char* e = getenv("GF_COOP_PROXY_SAFE_SPAWN"); safe = e && *e == '1'; if (safe) { printf("[coop-proxy] v1063P9 safe proxy spawn ON: func_001281AC suppressed\n"); fflush(stdout); } }
        if (!safe && p->kind != GF_WK_CAR && is_crew(cls)) gcall(0x001281ACu, ent, 0, 0, 0, 0);   /* crew AI off (v1059C: safe on characters) */
    }
    s_n_spawn++;
    look_check(p);   /* v1063P5 */
    if (s_n_spawn <= 40 || s_n_spawn % 50 == 0) {
        printf("[coop-proxy] net %u spawned %s class %08X%s ent=%08X vt=%08X at %.1f %.1f %.1f (try %d)\n", p->netid,
               p->kind == GF_WK_CAR ? "car" : "ped", cls, cls != p->cls ? " (substitute)" : "", ent, vt,
               (double)p->tgt[9], (double)p->tgt[10], (double)p->tgt[11], p->tries); fflush(stdout); }
}

uint32_t gf_proxy_spawn(uint16_t netid, uint8_t kind, uint32_t class_hash, const uint8_t guid[16], const float m[12])
{
    proxy* p;
    if (!px_enabled() || !m) return 0;
    if (!s_cars && (kind == GF_WK_CAR || (!kind && kind_of_class(class_hash) == GF_WK_CAR))) return 0;   /* v1061B */
    if ((p = find(netid)) != 0) {   /* respawn request for a known id: retarget, keep the entity if the class matches */
        if (p->cls == class_hash && p->kind == kind) { memcpy(p->tgt, m, sizeof p->tgt); p->tgt_ms = GetTickCount64(); return p->ent; }
        kill_ent(p, "class changed"); memset(p, 0, sizeof *p);
    } else {
        for (int i = 0; i < PX_MAX && !p; i++) if (s_px[i].state == PS_FREE) p = &s_px[i];
        if (!p) { static ULONGLONG said; if (GetTickCount64() - said > 5000) { said = GetTickCount64(); printf("[coop-proxy] cap %d reached, net %u ignored\n", PX_MAX, netid); fflush(stdout); } return 0; }
        memset(p, 0, sizeof *p);
    }
    p->state = PS_PENDING; p->netid = netid; p->kind = kind ? kind : kind_of_class(class_hash); p->cls = class_hash;
    if (guid) memcpy(p->guid, guid, 16);
    memcpy(p->tgt, m, sizeof p->tgt); p->tgt_ms = GetTickCount64();
    if (s_spawned_this_frame < PX_SPAWN_PER_FRAME && g_active_ctx && px_settled(GetTickCount64())) { __try { try_spawn(p); } __except (EXCEPTION_EXECUTE_HANDLER) { } }
    return p->ent;
}
void gf_proxy_drive(uint16_t netid, const float m[12], const float vel[3])
{
    proxy* p = find(netid);
    if (!p || !m) return;
    memcpy(p->tgt, m, sizeof p->tgt);
    if (vel) memcpy(p->vel, vel, sizeof p->vel); else memset(p->vel, 0, sizeof p->vel);
    p->tgt_ms = GetTickCount64();
    {   /* v1066N: root speed of the (already interpolated) target, QPC-timed EMA; host velocity when the snapshot has one */
        static double s_qf; LARGE_INTEGER c; double t, dt;
        if (s_qf <= 0.0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); s_qf = (double)f.QuadPart; }
        QueryPerformanceCounter(&c); t = (double)c.QuadPart / s_qf;
        dt = t - p->spd_t;
        if (p->spd_ok && dt >= 0.004 && dt < 0.5) {
            const float dx = m[9] - p->spd_p[0], dz = m[11] - p->spd_p[2];
            float v = sqrtf(dx * dx + dz * dz) / (float)dt;
            if (vel && (vel[0] != 0.0f || vel[2] != 0.0f)) v = sqrtf(vel[0] * vel[0] + vel[2] * vel[2]);
            if (!(v >= 0.0f) || v > 30.0f) v = p->spd;
            { const float a = (float)(dt / 0.15); p->spd += (v - p->spd) * (a > 1.0f ? 1.0f : a); }
        } else if (!p->spd_ok || dt >= 0.5) { p->spd = vel ? sqrtf(vel[0] * vel[0] + vel[2] * vel[2]) : 0.0f; if (!(p->spd >= 0.0f) || p->spd > 30.0f) p->spd = 0.0f; }
        if (dt >= 0.004 || !p->spd_ok) { p->spd_t = t; p->spd_p[0] = m[9]; p->spd_p[1] = m[10]; p->spd_p[2] = m[11]; p->spd_ok = 1; }
    }
}
/* v1066N (agent N): horizontal root speed of a live ped proxy (m/s), -1 = no such proxy. gf_coop_wpose.c donor fallback. */
float gf_proxy_speed_of(uint16_t netid)
{
    proxy* p = netid ? find(netid) : 0;
    if (!p || p->state != PS_LIVE || !p->ent || p->kind == GF_WK_CAR || !p->spd_ok) return -1.0f;
    return p->spd;
}
void gf_proxy_release(uint16_t netid)
{
    proxy* p = find(netid);
    if (!p) return;
    __try { kill_ent(p, "release"); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    memset(p, 0, sizeof *p);
}
void gf_proxy_release_all(void)
{
    int n = 0;
    for (int i = 0; i < PX_MAX; i++) if (s_px[i].state != PS_FREE) {
        __try { kill_ent(&s_px[i], "release all"); } __except (EXCEPTION_EXECUTE_HANDLER) { }   /* v1061B: vt check + car exit */
        memset(&s_px[i], 0, sizeof s_px[i]); n++;
    }
    if (n) { printf("[coop-proxy] released all (%d)\n", n); fflush(stdout); }
}
int gf_world_is_proxy(uint32_t ent)
{
    if (!ent) return 0;
    for (int i = 0; i < PX_MAX; i++) if (s_px[i].state != PS_FREE && s_px[i].ent == ent) return 1;
    for (int i = 0; i < POOL_MAX; i++) if (s_pool[i].ent == ent) return 1;   /* v1061G parked */
    { const ULONGLONG now = GetTickCount64(); for (int i = 0; i < 64; i++) if (s_dead_ent[i] == ent && now - s_dead_ms[i] < 10000) return 1; }   /* v1061B */
    return 0;
}

/* ---------------------------------------------------------------- per-frame drive */
static void nlerp_basis(float cur[9], const float tgt[9], float a)
{
    /* blend rows then re-orthonormalise (rows right/up/at) -- adequate for per-frame steps */
    float r[3], u[3], f[3], l;
    for (int k = 0; k < 3; k++) { r[k] = cur[k] + (tgt[k] - cur[k]) * a; u[k] = cur[3 + k] + (tgt[3 + k] - cur[3 + k]) * a; }
    l = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]); if (!(l > 1e-4f)) { memcpy(cur, tgt, 9 * sizeof(float)); return; }
    for (int k = 0; k < 3; k++) r[k] /= l;
    { const float d = r[0] * u[0] + r[1] * u[1] + r[2] * u[2]; for (int k = 0; k < 3; k++) u[k] -= r[k] * d; }
    l = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]); if (!(l > 1e-4f)) { memcpy(cur, tgt, 9 * sizeof(float)); return; }
    for (int k = 0; k < 3; k++) u[k] /= l;
    f[0] = r[1] * u[2] - r[2] * u[1]; f[1] = r[2] * u[0] - r[0] * u[2]; f[2] = r[0] * u[1] - r[1] * u[0];
    /* keep the handedness of the game's at-row */
    if (f[0] * tgt[6] + f[1] * tgt[7] + f[2] * tgt[8] < 0.0f) { f[0] = -f[0]; f[1] = -f[1]; f[2] = -f[2]; }
    memcpy(cur, r, sizeof r); memcpy(cur + 3, u, sizeof u); memcpy(cur + 6, f, sizeof f);
}
static void drive_one(proxy* p, float dt, ULONGLONG now)
{
    uint32_t vt, xf; float want[3], d2, shown[12];
    if (p->kind != GF_WK_CAR && now >= p->look_at && p->look_tries >= 10 && !p->nomodel_said) {   /* v1063P6 diag: a ped proxy without a render instance is invisible */
        uint32_t inst = 0; read_u32(p->ent + 0x64u, &inst);
        if (!inst) { p->nomodel_said = 1; s_n_nomodel++; if (s_n_nomodel <= 10) { printf("[coop-proxy] v1063P6 net %u ent %08X class %08X: still NO render model 10 s after spawn (invisible proxy)\n", p->netid, p->ent, p->used_cls); fflush(stdout); }
            nomodel_fix(p); }   /* v1064Q */
    }
    if (!p->look && p->kind != GF_WK_CAR && p->look_tries < 10 && now >= p->look_at) { p->look_tries++; p->look_at = now + 1000; s_n_look_unk--; look_check(p); }   /* v1063P5 (unknown counted once) */
    if (!ent_ok(p, &vt, &xf)) { kill_ent(p, "entity gone (area change)"); p->state = PS_PENDING; p->tries = 0; p->next_try = now + 500; s_n_respawn++; return; }
    if (p->kind != GF_WK_CAR) {   /* v1063V2: a ped proxy seated in a car proxy (host driver/passenger) rides with the car */
        extern int gf_veh_ped_seated(uint32_t ent);
        if (gf_veh_ped_seated(p->ent)) { p->late_ok = 0; p->far_frames = 0; p->warp_cool = 3; return; }
        {   /* v1065G (agent G): the local player is grabbing this proxy -> the game's paired grab owns it (gf_coop_combat.c) */
            extern int gf_coop_grab_holds(uint32_t ent);
            if (gf_coop_grab_holds(p->ent)) { p->late_ok = 0; p->far_frames = 0; p->warp_cool = 3; p->have_cur = 0; return; }
        }
    }
    if (p->kind == GF_WK_CAR) {   /* v1063V (agent V): cars are driven by gf_coop_vehicle.c (Havok body velocity servo) */
        extern int gf_veh_proxy_drive(uint16_t netid, uint32_t ent, const float tgt[12], const float vel[3], unsigned long long tgt_ms, float dt);
        const int vr = gf_veh_proxy_drive(p->netid, p->ent, p->tgt, p->vel, p->tgt_ms, dt);
        if (vr > 0) { memcpy(p->cur, p->tgt, sizeof p->cur); p->have_cur = 1; p->far_frames = 0; return; }
        if (vr < 0) { kill_ent(p, "drive call failed (v1063V)"); p->state = PS_PENDING; p->next_try = now + 1000; s_n_respawn++; return; }
    }
    if (s_smooth < 0) { const char* e = getenv("GF_COOP_PROXY_SMOOTH"); s_smooth = e && *e == '1'; printf("[coop-proxy] v1063P3 proxy transform = %s\n", s_smooth ? "smoothed (old v1060 exp filter + extrapolation)" : "the interpolated snapshot target as is (no second smoothing)"); fflush(stdout); }
    {
        float ahead = (float)(now - p->tgt_ms) * 0.001f; if (ahead > 0.25f) ahead = 0.25f;
        if (!s_smooth) ahead = 0.0f;   /* v1063P3: the world layer already interpolates at the pose stream's playback time */
        for (int k = 0; k < 3; k++) want[k] = p->tgt[9 + k] + p->vel[k] * ahead;
    }
    if (!p->have_cur) { memcpy(p->cur, p->tgt, sizeof p->cur); p->have_cur = 1; }
    d2 = 0; for (int k = 0; k < 3; k++) { const float d = want[k] - p->cur[9 + k]; d2 += d * d; }
    if (d2 > 36.0f || !s_smooth) { memcpy(p->cur, p->tgt, sizeof p->cur); for (int k = 0; k < 3; k++) p->cur[9 + k] = want[k]; }
    else {
        const float a = 1.0f - expf(-dt * 12.0f);
        for (int k = 0; k < 3; k++) p->cur[9 + k] += (want[k] - p->cur[9 + k]) * a;
        nlerp_basis(p->cur, p->tgt, 1.0f - expf(-dt * 10.0f));
    }
    if (s_body_every < 0) { const char* e = getenv("GF_COOP_PROXY_BODY_EVERY"); s_body_every = e && *e ? atoi(e) : 1;   /* v1063P12: default back to 1 -- every 4 frames gave 1148 warps (place + vt+0x24) and was SLOWER (p63-16c 445 vs p63-16b 600 frames / 10 s) */ if (s_body_every < 1) s_body_every = 1;
        printf("[coop-proxy] v1063P10 ped body (vt+0x30, Havok controller set) every %d frames, drawn matrix (late vt+0x2C) every frame\n", s_body_every); fflush(stdout); }
    /* v1063P10 perf: the per-frame vt+0x30 (RW frame + Havok character controller setter func_00441D40) was ~12 % of the echo
     * frame time (p63-13a DRIVE30=0: 590 vs 525 frames / 10 s). The drawn matrix comes from the late vt+0x2C anyway, so the
     * controller is only refreshed every GF_COOP_PROXY_BODY_EVERY frames (staggered per slot); the 0.3 m warp still catches up. */
    if (p->kind != GF_WK_CAR && s_drive30 && s_body_every > 1 && ((s_frame_no + (unsigned)(p - s_px)) % (unsigned)s_body_every) != 0) {
        for (int r = 0; r < 4; r++) { for (int k = 0; k < 3; k++) p->late_m[r * 4 + k] = p->cur[r * 3 + k]; p->late_m[r * 4 + 3] = 0.0f; }
        p->late_ok = 1;
    } else
    if (!s_drive30 && p->kind != GF_WK_CAR) {   /* v1061B: late vt+0x2C only */
        for (int r = 0; r < 4; r++) { for (int k = 0; k < 3; k++) p->late_m[r * 4 + k] = p->cur[r * 3 + k]; p->late_m[r * 4 + 3] = 0.0f; }
        p->late_ok = 1;
    } else if (!place(p, p->ent, p->cur, 0)) { kill_ent(p, "drive call failed"); p->state = PS_PENDING; p->next_try = now + 1000; s_n_respawn++; return; }
    /* stuck check: where is it really? */
    if (read_xf12(xf, shown)) {
        float e2 = 0; for (int k = 0; k < 3; k++) { const float d = shown[9 + k] - p->cur[9 + k]; e2 += d * d; }
        p->far_frames = e2 > 16.0f ? p->far_frames + 1 : 0;
        /* v1061G: the body drifted (its own AI / physics walked it away: 108 of 124 releases on the user's coop-local
         * 01:30 were "stuck > 4 m", and every release+respawn may leave an orphan body = the NPC cluster). Warp it back
         * with the spawn placement (vt+0x30 + vt+0x24 {x,y,z,2}) at most every 15 frames; release only after 10 s. */
        if (e2 > (p->kind == GF_WK_CAR ? 4.0f : 0.09f) && p->warp_cool <= 0) {   /* v1062W1 cars: 2 m (xf is one physics step behind the vt+0x2C body set) */   /* > 0.3 m: the per-tick vt+0x30 only moves the drawn matrix; vt+0x24 moves the body */
            p->warp_cool = 3; p->nwarp++; s_n_warp++;
            if (s_n_warp <= 10 || s_n_warp % 2000 == 0) { printf("[coop-proxy] v1061G net %u warp #%d: body at %.1f %.1f %.1f, shown target %.1f %.1f %.1f (%.1f m off)\n", p->netid, p->nwarp,
                (double)shown[9], (double)shown[10], (double)shown[11], (double)p->cur[9], (double)p->cur[10], (double)p->cur[11], (double)sqrtf(e2)); fflush(stdout); }
            place(p, p->ent, p->cur, 1);
        } else if (p->warp_cool > 0) p->warp_cool--;
        if (p->far_frames > 1200 && (s_drive30 || s_late)) { kill_ent(p, "stuck > 4 m from target"); p->state = PS_PENDING; p->tries = 0; p->next_try = now + 500; s_n_respawn++; }
    }
}
/* main PPU thread, right after an entity's animation update (gf_coop_after_anim) */
void gf_proxy_late(uint32_t ent)
{
    uint32_t vt, xf, buf; int before;
    if (s_on != 1 || !ent || !s_late) return;
    if (GetTickCount64() < s_hold_until) return;   /* v1062W3 */
    { extern int gf_veh_ped_seated(uint32_t e); if (gf_veh_ped_seated(ent)) return; }   /* v1063V2 seated in a car proxy */
    if (s_ww_pending && ent == s_ww_target) {   /* v1063P1 diag */
        uint32_t xf = 0;
        if (read_u32(ent + 0x68u, &xf) && ea_range(xf, 0x50)) {
            float pz[3] = {0, 0, 0}; read_f(xf + 0x40u, &pz[0]); read_f(xf + 0x44u, &pz[1]); read_f(xf + 0x48u, &pz[2]);
            g_ww_live_len = 0x10; g_ww_lo = (xf + 0x40u) & ~15u; g_ww_hi = g_ww_lo + 0x10u; g_ww_live = xf + 0x40u; g_ww_live_gen++;
            printf("[coop-proxy] v1063P1 WWATCH arm #%d: %s net %u ent %08X xf %08X pos %.2f %.2f %.2f (stderr [ww] = writers)\n", s_ww_k,
                   (s_ww_k & 1) ? "SOURCE ped" : "PROXY ped", s_ww_net, ent, xf, (double)pz[0], (double)pz[1], (double)pz[2]); fflush(stdout);
            fprintf(stderr, "[ww] ===== v1063P1 arm #%d %s net %u ent %08X xf+0x40 = %08X =====\n", s_ww_k, (s_ww_k & 1) ? "SOURCE" : "PROXY", s_ww_net, ent, xf + 0x40u); fflush(stderr);
        }
        s_ww_pending = 0; s_ww_k++; s_ww_next = GetTickCount64() + 1500;
    }
    {   /* v1061G: a proxy we released > 2 s ago that still gets animation updates = an orphan body left in the world */
        const ULONGLONG now = GetTickCount64();
        int parked = 0; for (int i = 0; i < POOL_MAX; i++) if (s_pool[i].ent == ent) { parked = 1; break; }
        if (!parked) for (int i = 0; i < PX_MAX; i++) if (s_px[i].state == PS_LIVE && s_px[i].ent == ent) { parked = 1; break; }   /* v1062W1: memory reused by a new proxy = not an orphan */
        if (!parked) for (int i = 0; i < 64; i++) if (s_dead_ent[i] == ent && now - s_dead_ms[i] > 2000) {
            int k; s_n_orphan_anim++;
            for (k = 0; k < s_orphan_n; k++) if (s_orphan_seen[k] == ent) break;
            if (k == s_orphan_n && s_orphan_n < 64) { s_orphan_seen[s_orphan_n++] = ent; printf("[coop-proxy] v1061G ORPHAN: released ent %08X still animated %.1f s after release\n", ent, (double)(now - s_dead_ms[i]) / 1000.0); fflush(stdout); }
            break;
        }
    }
    for (int i = 0; i < PX_MAX; i++) {
        proxy* p = &s_px[i];
        if (p->state != PS_LIVE || p->ent != ent || !p->late_ok) continue;
        buf = scratch(); before = s_call_fail;
        if (!buf || !ent_ok(p, &vt, &xf)) return;   /* v1061B */
        {
            float m12[12]; for (int r = 0; r < 4; r++) for (int k = 0; k < 3; k++) m12[r * 3 + k] = p->late_m[r * 4 + k];
            if (!write_matrix12(buf, xf, m12)) return;
        }
        if (call_virtual(ent, 0x2Cu, buf) && s_call_fail == before) s_n_late++; else p->late_ok = 0;
        return;
    }
}

/* ---------------------------------------------------------------- self test (GF_COOP_PROXY_TEST=1) */
static void self_test(ULONGLONG now)
{
    static ULONGLONG t0 = 0; static int started = 0;
    uint32_t pl = player(), xf; float pm[12];
    if (!pl || !read_u32(pl + 0x68u, &xf) || !ea_range(xf, 0x50) || !read_xf12(xf, pm)) { t0 = 0; return; }
    if (fabsf(pm[9]) + fabsf(pm[10]) + fabsf(pm[11]) < 0.01f) { t0 = 0; return; }
    if (!t0) t0 = now;
    if (now - t0 < 8000) return;
    if (!started) {
        int nped = 0, ncar = 0;
        for (int i = 0; i < s_live_n; i++) { if (s_live_kind[i] == GF_WK_CAR) ncar++; else nped++; }
        printf("[coop-proxy] SELF TEST start: %d char / %d car templates known\n", nped, ncar); fflush(stdout);
        started = 1;
    }
    {
        const float ang0 = (float)((now - t0) % 12000) / 12000.0f * 6.2831853f;
        const float cx = pm[9] + pm[6] * 4.0f, cz = pm[11] + pm[8] * 4.0f;
        static const uint8_t z16[16] = {0};
        for (int i = 0; i < (s_test >= 2 ? 4 : 3); i++) {   /* v1061B: car only with GF_COOP_PROXY_TEST=2 */
            const uint16_t id = (uint16_t)(0xFF00u + (unsigned)i);
            const uint8_t kind = i == 3 ? GF_WK_CAR : GF_WK_PED;
            const float rad = kind == GF_WK_CAR ? 6.0f : 2.5f, ang = ang0 + (float)i * 2.0944f;
            float m[12], vel[3];
            uint32_t cls = kind == GF_WK_CAR ? 0x10E5319Eu : 0xBA1F9E7Bu;
            for (int j = 0; j < s_live_n; j++) if (s_live_kind[j] == kind && s_live_attrib[j]) { cls = s_live_cls[j]; if (kind == GF_WK_CAR || !is_crew(cls)) break; }
            /* facing along the circle tangent: at = (-sin, 0, cos) rotated; right = up x at */
            {
                const float ax = -sinf(ang), az = cosf(ang);
                m[0] = az; m[1] = 0; m[2] = -ax;   /* right */
                m[3] = 0;  m[4] = 1; m[5] = 0;     /* up */
                m[6] = ax; m[7] = 0; m[8] = az;    /* at */
                m[9] = cx + cosf(ang) * rad; m[10] = pm[10]; m[11] = cz + sinf(ang) * rad;
                const float w = 6.2831853f / 12.0f * rad;
                vel[0] = ax * w; vel[1] = 0; vel[2] = az * w;
            }
            if (!find(id)) gf_proxy_spawn(id, kind, cls, z16, m);
            gf_proxy_drive(id, m, vel);
        }
    }
}

/* v1063P3 model registry (gf_trainer.c): *(0x101F3670) = {buckets, count}; node +0x8 resource, +0x1C GUID, +0x2C next */
static int mreg_lookup(uint32_t res, const uint8_t* want, uint8_t* guid_out, uint32_t* res_out)
{
    uint32_t tab = 0, bk = 0, n = 0; long budget = 300000;
    if (!read_u32(0x101F3670u, &tab) || !ea_range(tab, 8) || !read_u32(tab, &bk) || !read_u32(tab + 4, &n)) return 0;
    if (!ea_range(bk, 4) || n == 0 || n > (1u << 20) || !mem_access(bk, (size_t)n * 4u, 0)) return 0;
    __try {
        for (uint32_t b = 0; b < n; b++) {
            uint32_t node = rd32(bk + 4 * b);
            while (node && ea_range(node, 0x30) && budget-- > 0) {
                if (!mem_access(node, 0x30, 0)) break;
                {
                    const uint32_t r = rd32(node + 8);
                    if (r && (res ? r == res : !memcmp(vm_base + node + 0x1C, want, 16))) {
                        if (guid_out) memcpy(guid_out, vm_base + node + 0x1C, 16);
                        if (res_out) *res_out = r;
                        return 1;
                    }
                }
                node = rd32(node + 0x2C);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 0;
}
/* v1070D (agent D, from agent C's car finding v1070C): the game's own character SetModel is vt+0x104 = func_00464890(ent,
 * REGISTRY NODE) -- the appearance init (func_00140FE4) calls it with func_004596B8(GUID). It checks the node type word
 * *(*(node+4)+8) (0x9E3791CF / 0xF3F21004), picks the matching render-instance ctor (func_000139AC / func_0001207C) and copies
 * the GUID node+0x1C into the instance (func_000126AC). The v1063P7 path vt+0x108 = func_00464818(ent, *(node+8)) always
 * takes the func_000139AC ctor and never writes the instance GUID (wrong ctor for 0xF3F21004 models = model-less instance
 * for cars, c70-9-mtest; no GUID for the LOD save func_001370A4 -> bad +0x1C60 later). So the ped SetModel now passes the
 * NODE to vt+0x104 (GF_COOP_PROXY_SETMODEL_108=1 = old vt+0x108). SetModel itself stays OPT-IN (GF_COOP_PROXY_SETMODEL=1). */
static uint32_t mreg_node(const uint8_t want[16])   /* registry node of a GUID (type-checked), 0 = not registered */
{
    uint32_t tab = 0, bk = 0, n = 0; long budget = 300000;
    if (!read_u32(0x101F3670u, &tab) || !ea_range(tab, 8) || !read_u32(tab, &bk) || !read_u32(tab + 4, &n)) return 0;
    if (!ea_range(bk, 4) || n == 0 || n > (1u << 20) || !mem_access(bk, (size_t)n * 4u, 0)) return 0;
    __try {
        for (uint32_t b = 0; b < n; b++) {
            uint32_t node = rd32(bk + 4 * b);
            while (node && ea_range(node, 0x30) && budget-- > 0) {
                if (!mem_access(node, 0x30, 0)) break;
                if (rd32(node + 8) && !memcmp(vm_base + node + 0x1C, want, 16)) {
                    uint32_t t4 = rd32(node + 4), ty = 0;
                    if (!ea_range(t4, 0x10) || !mem_access(t4 + 8, 4, 0)) return 0;
                    ty = rd32(t4 + 8);
                    return (ty == 0x9E3791CFu || ty == 0xF3F21004u) ? node : 0;
                }
                node = rd32(node + 0x2C);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 0;
}
static int s_sm108 = -1;
static int setmodel_call(uint32_t ent, const uint8_t want[16], uint32_t res)   /* 1 = called OK */
{
    uint32_t vt = 0, opd = 0, fn = 0, node;
    if (s_sm108 < 0) { const char* e = getenv("GF_COOP_PROXY_SETMODEL_108"); s_sm108 = e && *e == '1';
        printf("[coop-proxy] v1070D ped SetModel path: %s (only used with GF_COOP_PROXY_SETMODEL=1)\n", s_sm108 ? "OLD vt+0x108(resource)" : "game's vt+0x104(registry node)"); fflush(stdout); }
    if (s_sm108) return call_virtual(ent, 0x108u, res);
    if (!(node = mreg_node(want))) return 0;
    if (!read_u32(ent, &vt) || !read_u32(vt + 0x104u, &opd) || !read_u32(opd, &fn) || fn != 0x00464890u) {
        static int said; if (!said++) { printf("[coop-proxy] v1070D SetModel skipped: vt %08X +0x104 -> %08X (not func_00464890)\n", vt, fn); fflush(stdout); }
        return 0; }
    return call_virtual(ent, 0x104u, node);
}
static void look_diag(ULONGLONG now)
{
    uint32_t ents[256]; uint16_t ids[256]; float pos[256][3]; int n = 0, done = 0;
    if (now - s_lookdiag_at < 10000) return;
    s_lookdiag_at = now;
    __try { n = gf_coop_world_host_peds(ents, ids, pos, 256); } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
    for (int i = 0; i < PX_MAX && done < 4; i++) {
        proxy* p = &s_px[i]; uint32_t src = 0, wp[48], ws[48], ip = 0, is = 0; char line[640]; int o = 0, nd = 0;
        if (p->state != PS_LIVE || p->kind != GF_WK_PED || !p->ent) continue;
        for (int t = 0; t < n; t++) if (ids[t] == p->netid) { src = ents[t]; break; }
        if (!src) continue;
        done++;
        /* entity words 0x9C..0x15C */
        for (int k = 0; k < 48; k++) { wp[k] = ws[k] = 0; read_u32(p->ent + 0x9Cu + (uint32_t)k * 4u, &wp[k]); read_u32(src + 0x9Cu + (uint32_t)k * 4u, &ws[k]); }
        o += snprintf(line + o, sizeof line - (size_t)o, "[coop-proxy] v1063P3 LOOK net %u proxy %08X src %08X | ent diffs:", p->netid, p->ent, src);
        for (int k = 0; k < 48 && o < 560; k++) if (wp[k] != ws[k]) { nd++; o += snprintf(line + o, sizeof line - (size_t)o, " +%03X %08X/%08X", 0x9C + k * 4, wp[k], ws[k]); }
        printf("%s (%d)\n", line, nd);
        read_u32(p->ent + 0x64u, &ip); read_u32(src + 0x64u, &is);
        o = 0; nd = 0;
        o += snprintf(line + o, sizeof line - (size_t)o, "[coop-proxy] v1063P3 LOOK net %u inst +64 %08X/%08X diffs:", p->netid, ip, is);
        if (ea_range(ip, 0x68) && ea_range(is, 0x68))
            for (int k = 0; k < 26 && o < 560; k++) { uint32_t a = 0, b = 0; read_u32(ip + (uint32_t)k * 4u, &a); read_u32(is + (uint32_t)k * 4u, &b);
                if (a != b) { nd++; o += snprintf(line + o, sizeof line - (size_t)o, " +%02X %08X/%08X", k * 4, a, b); } }
        printf("%s (%d)\n", line, nd);
        {   /* registry names of B8 and of every instance word that is a registered resource */
            uint8_t g[16]; uint32_t rb8p = 0, rb8s = 0;
            read_u32(p->ent + 0xB8u, &rb8p); read_u32(src + 0xB8u, &rb8s);
            o = 0; o += snprintf(line + o, sizeof line - (size_t)o, "[coop-proxy] v1063P3 LOOK net %u B8 %08X/%08X", p->netid, rb8p, rb8s);
            if (mreg_lookup(rb8s, 0, g, 0)) { o += snprintf(line + o, sizeof line - (size_t)o, " srcB8 guid %02X%02X%02X%02X..", g[0], g[1], g[2], g[3]); }
            for (int side = 0; side < 2; side++) {
                const uint32_t inst = side ? is : ip;
                if (!ea_range(inst, 0x68)) continue;
                for (int k = 0; k < 26 && o < 560; k++) { uint32_t w = 0; read_u32(inst + (uint32_t)k * 4u, &w);
                    if (ea_range(w, 0x20) && mreg_lookup(w, 0, g, 0)) o += snprintf(line + o, sizeof line - (size_t)o, " %s inst+%02X=%08X guid %02X%02X%02X%02X", side ? "SRC" : "PRX", k * 4, w, g[0], g[1], g[2], g[3]); }
            }
            printf("%s\n", line);
        }
        fflush(stdout);
    }
}
/* v1068R2 (agent R) CHARACTER HOLD after a loading-sized frame, for the modules that place characters OUTSIDE the proxy table
 * (co-op puppet apply_pose, host grab-follow place_char). Echo t68-1 / t68-3 aborted in Havok (abort 0x00ABBF14 <- 0x00ABB538)
 * from vt+0x30/vt+0x24 placement calls made right after a 6.7 s area-change hitch (symbolized: place_char <- follow_tick and
 * call_virtual <- apply_pose <- gf_coop_puppet_tick): the characters' physics are not back in a world yet. Proxies already
 * had this guard (v1062W3 s_hold_until). gf_coop_frame_begin() runs FIRST in gf_coop_glue_tick (before the puppet), so the
 * very first frame after the hitch is covered. GF_COOP_CHAR_HOLD=0 off, GF_COOP_CHAR_HOLD_MS (2000). */
static ULONGLONG s_fb_last, s_fb_hold_until; static int s_fb_on = -1, s_fb_ms = 2000; static unsigned s_fb_n;
void gf_coop_frame_begin(void)
{
    const ULONGLONG now = GetTickCount64();
    if (s_fb_on < 0) { const char* e = getenv("GF_COOP_CHAR_HOLD"); s_fb_on = !(e && *e == '0');
        if ((e = getenv("GF_COOP_CHAR_HOLD_MS")) && atoi(e) >= 0) s_fb_ms = atoi(e);
        printf("[coop-proxy] v1068R2 character hold after loading-sized frames %s (%d ms; puppet + grab-follow placement paused; GF_COOP_CHAR_HOLD)\n", s_fb_on ? "ON" : "off", s_fb_ms); fflush(stdout); }
    if (s_fb_last && now - s_fb_last > 250) { s_fb_hold_until = now + (ULONGLONG)s_fb_ms; s_fb_n++;
        if (s_fb_on && s_fb_n <= 40) { printf("[coop-proxy] v1068R2 frame %.2f s -> character placement paused %d ms (#%u)\n", (double)(now - s_fb_last) * 0.001, s_fb_ms, s_fb_n); fflush(stdout); } }
    s_fb_last = now;
}
int gf_coop_char_hold(void) { return s_fb_on == 1 && GetTickCount64() < s_fb_hold_until; }
void gf_proxy_tick(void)
{
    const ULONGLONG now = GetTickCount64();
    float dt;
    { extern void gf_veh_tick(void); __try { gf_veh_tick(); } __except (EXCEPTION_EXECUTE_HANDLER) { } }   /* v1063V car sync (agent V) */
    if (!px_enabled()) return;
    s_spawned_this_frame = 0;
    s_frame_no++;   /* v1063P10 */
    dt = s_last_ms ? (float)(now - s_last_ms) * 0.001f : 0.016f;
    {   /* v1061B world change: the local player entity changed (frontend scene -> new game, area / mission load) or a
         * frame took > 1 s (loading hitch) -> every proxy goes back to pending (entity released if it is still ours,
         * forgotten otherwise) and respawns in the new world 3 s later if W2 still drives it. */
        static uint32_t s_pl_last = 0; static int s_pl_init = 0; const uint32_t pl = player();
        const int pl_changed = s_pl_init && pl != s_pl_last; const int hitch = s_last_ms && dt > 1.0f;
        s_pl_last = pl; s_pl_init = 1;
        if (pl_changed || hitch) {
            int n = 0;
            for (int i = 0; i < PX_MAX; i++) if (s_px[i].state == PS_LIVE) {
                __try { kill_ent(&s_px[i], pl_changed ? "world change (player entity changed)" : "world change (frame > 1 s)"); } __except (EXCEPTION_EXECUTE_HANDLER) { s_px[i].ent = 0; }
                s_px[i].state = PS_PENDING; s_px[i].tries = 0; s_px[i].next_try = now + 3000; n++; }
            memset(s_pool, 0, sizeof s_pool);   /* v1061G: parked bodies belong to the old world */
            { extern void gf_coop_pose_face_reset(void); gf_coop_pose_face_reset(); }
            if (n || pl_changed) { printf("[coop-proxy] v1061B world change (%s, player %08X, frame %.2f s): %d proxies back to pending\n",
                                          pl_changed ? "player changed" : "hitch", pl, (double)dt, n); fflush(stdout); }
            s_n_world++;
        }
        /* v1061C: any world change or loading-sized frame (> 250 ms) restarts the settle window */
        if (pl_changed || (s_last_ms && dt > 0.25f) || pl == 0) {
            const ULONGLONG until = now + (ULONGLONG)s_settle_ms;
            if (until > s_settle_until) {
                if (now >= s_settle_until) { printf("[coop-proxy] v1061C world not settled (%s, frame %.2f s): proxy spawns held %d ms\n",
                                                    pl == 0 ? "no player" : pl_changed ? "player changed" : "slow frame", (double)dt, s_settle_ms); fflush(stdout); }
                s_settle_until = until;
            }
        }
    }
    if (s_last_ms && dt > 0.25f) { s_hold_until = now + 2000; s_n_hold++; }   /* v1062W3: loading-sized frame -> no calls into proxies for 2 s */
    if (dt > 0.1f) dt = 0.1f; if (dt < 0.001f) dt = 0.001f;
    s_last_ms = now;
    if (now < s_hold_until) return;
    __try { notes_drain(); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    for (int i = 0; i < POOL_MAX; i++) {   /* v1061G: parked proxies: drop invalid ones, re-warp to the park spot every 1 s */
        pooled* q = &s_pool[i]; uint32_t v = 0, x = 0, f = 0;
        if (!q->ent || now < q->warp_at) continue;
        q->warp_at = now + 1000;
        __try {
            if (!entity_info(q->ent, &v, &x) || v != q->vt || v == 0x1005F6C8u) { memset(q, 0, sizeof *q); s_n_pool_lost++; continue; }
            if (q->alive_bit && (!read_u32(q->ent + 0x34u, &f) || !(f & 1u))) { g_knobs(); if (s_revive) { q->alive_bit = 0; s_n_revive++; } else { memset(q, 0, sizeof *q); s_n_pool_lost++; continue; } }   /* v1128G keep game-released bodies */
            {   /* v1064K (cars: 400 m track errors): the game freed the parked body and the factory gave the SAME address (same class
                 * -> same vtable, alive) to a new live proxy -> this stale entry warped the live proxy 400 m down every second. */
                int alias = 0; for (int j = 0; j < PX_MAX; j++) if (s_px[j].state == PS_LIVE && s_px[j].ent == q->ent) { alias = 1; break; }
                if (alias) { static unsigned na; if (++na <= 10 || na % 100 == 0) { printf("[coop-proxy] v1064K pool entry %08X is now a LIVE proxy (address reused) -> dropped, not warped (#%u)\n", q->ent, na); fflush(stdout); }
                    memset(q, 0, sizeof *q); s_n_pool_lost++; continue; }
            }
            { proxy t; memset(&t, 0, sizeof t); t.kind = q->kind; t.ent = q->ent; place(&t, q->ent, q->park, 1); }
        } __except (EXCEPTION_EXECUTE_HANDLER) { memset(q, 0, sizeof *q); }
    }
    if (s_test) { __try { self_test(now); } __except (EXCEPTION_EXECUTE_HANDLER) { } }
    if (s_lookdiag < 0) { const char* e = getenv("GF_COOP_PROXY_LOOKDIAG"); s_lookdiag = e && *e == '1'; }
    if (s_lookdiag) { __try { look_diag(now); } __except (EXCEPTION_EXECUTE_HANDLER) { } }
    __try { look_dump2(now); } __except (EXCEPTION_EXECUTE_HANDLER) { }   /* v1124G */
    if (s_ww < 0) { const char* e = getenv("GF_COOP_PROXY_WWATCH"); s_ww = e && *e ? atoi(e) : 0; }
    if (s_ww > 0 && s_ww_k < s_ww && !s_ww_pending && now >= s_ww_next && s_n_spawn > 0) {   /* v1063P1 diag: next target */
        if (!(s_ww_k & 1)) {   /* a live ped proxy (rotating) */
            for (int t = 0; t < PX_MAX; t++) { proxy* q = &s_px[(s_ww_pi + t) % PX_MAX];
                if (q->state == PS_LIVE && q->kind == GF_WK_PED && q->ent) { s_ww_target = q->ent; s_ww_net = q->netid; s_ww_pending = 1; s_ww_pi = (s_ww_pi + t + 1) % PX_MAX; break; } }
        } else {               /* its source (host side; echo only) */
            uint32_t ents[256]; uint16_t ids[256]; float pos[256][3]; int n = 0;
            __try { n = gf_coop_world_host_peds(ents, ids, pos, 256); } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
            for (int t = 0; t < n; t++) if (ids[t] == s_ww_net) { s_ww_target = ents[t]; s_ww_pending = 1; break; }
            if (!s_ww_pending) s_ww_k++;
        }
        if (!s_ww_pending) s_ww_next = now + 500;
    }
    for (int i = 0; i < PX_MAX; i++) {
        proxy* p = &s_px[i];
        __try {
            if (p->state == PS_PENDING && s_spawned_this_frame < PX_SPAWN_PER_FRAME && now >= p->next_try) {
                if (px_settled(now)) try_spawn(p); else s_n_settle_skips++;
            }
            else if (p->state == PS_LIVE) { drive_one(p, dt, now); look_apply(p, now); }   /* v1124H look */
        } __except (EXCEPTION_EXECUTE_HANDLER) { p->ent = 0; p->state = PS_PENDING; p->next_try = now + 2000; }
    }
    if (now - s_log_at >= 10000) {
        int live = 0, pend = 0; s_log_at = now;
        for (int i = 0; i < PX_MAX; i++) { if (s_px[i].state == PS_LIVE) live++; else if (s_px[i].state == PS_PENDING) pend++; }
        if (!live && !pend && s_n_ext_tx) { printf("[coop-proxy] v1064Q host ped ext: sent %u (model GUID from render instance %u, from saved +0x1C60 %u, unknown %u)\n", s_n_ext_tx, s_n_ext_inst, s_n_ext_saved, s_n_ext_none); fflush(stdout); }   /* v1064Q host */
        if (live || pend || s_n_spawn || s_test) {
            printf("[coop-proxy] %d live, %d pending | spawned %u, failed %u, substitutes %u, respawns %u, late sets %u, templates %d | v1061B car exits %u, freed-before-release %u, world changes %u\n",
                   live, pend, s_n_spawn, s_n_fail, s_n_sub, s_n_respawn, s_n_late, s_live_n, s_n_exit, s_n_vtgone, s_n_world);
            calm_log();
            look_log();   /* v1124H */
            printf("[coop-proxy] v1063P5 look: model match %u (wire face), differs %u, unknown %u, no model %u | v1063P7 SetModel %u (failed %u, not loaded %u) | ped ext sent %u, received %u | v1063P2 adopted %u, unbound %u\n", s_n_look_match, s_n_look_diff, s_n_look_unk, s_n_nomodel, s_n_setmodel, s_n_setmodel_fail, s_n_setmodel_nores, s_n_ext_tx, s_n_ext_rx, s_n_adopt, s_n_unbind);
            look_census();   /* v1064Q */
            if (s_n_hold) printf("[coop-proxy] v1062W3 load holds %u (proxy calls paused 2 s after a frame > 250 ms)\n", s_n_hold);
            printf("[coop-proxy] v1061E memq cache: %ld hits, %ld VirtualQuery | v1061G warps %u, orphan bodies %d (%u anim updates), parked %u, reused %u, pool lost %u\n", (long)g_memq_hits, (long)g_memq_miss, s_n_warp, s_orphan_n, s_n_orphan_anim, s_n_park, s_n_reuse, s_n_pool_lost);
            { int np = 0; for (int k = 0; k < POOL_MAX; k++) if (s_pool[k].ent) np++; printf("[coop-proxy] v1128G bodies: pool %d/%d, revived %u, class reuses %u, pool-full releases %u\n", np, (int)POOL_MAX, s_n_revive, s_n_classreuse, s_n_full_rel); fflush(stdout); }
            fflush(stdout);
        }
    }
}

/* v1062W1: proxies must not REACT. func_00140FE4(ent, msg) is the character-level message handler (ped handler
 * func_001435FC and crew handler func_00129314 call it first). Hand edit at its entry (_patches/apply-pending.ps1
 * block v1062W1): return 1 -> the handler returns at once. Dropped for live proxies: the perception / reaction message
 * types below (msg type object addresses in .bss; runtime id = *(obj + 0), compared with *(msg + 0) like the game).
 * iMsgDamage / ManagedEntity* / PostSpawnInitialize and the tick pass. GF_COOP_PROXY_CALM=0 off. */
static const uint32_t k_calm_obj[] = {
    0x101D9F6Cu /* iMsgNPCActionRequest */, 0x101DB958u /* iMsgAISound */, 0x101D9E88u /* iMsgInterestingObjectAdvertisementEvent */,
    0x101D9F44u /* iMsgHearDamage */, 0x101D9F3Cu /* iMsgHearHijack */, 0x101D9F34u /* iMsgHitByCar */, 0x101D9F2Cu /* iMsgSeeDeath */,
    0x101D9F24u /* iMsgHearCar */, 0x101D9F4Cu /* iMsgApplyPlayerInViewPressure */, 0x101DA154u /* iMsgSeeCarBombPlanted */,
    0x101DBBCCu /* iMsgFuelTruckExploded */, 0x101D9F74u /* AvoidStreetEventData */, 0x101D9EA0u /* Stimulus_HearDanger */,
    0x101D9E98u /* Stimulus_HearDamage */, 0x101D9E90u /* Stimulus_? */, 0x101D9F14u /* Stimulus_HearPressure */ };
enum { CALM_N = (int)(sizeof k_calm_obj / sizeof k_calm_obj[0]) };
static int s_calm = -1; static volatile long st_calm_drop[CALM_N], st_calm_pass;
int gf_proxy_msg_filter(uint32_t ent, uint32_t msg)   /* PPU threads: every message to every character -> cheap */
{
    int i, live = 0; uint32_t id;
    if (s_calm == 0 || s_on != 1 || !ent) return 0;
    for (i = 0; i < PX_MAX; i++) if (s_px[i].ent == ent && s_px[i].state == PS_LIVE && s_px[i].kind != GF_WK_CAR) { live = 1; break; }
    if (!live) return 0;
    if (s_calm < 0) { const char* e = getenv("GF_COOP_PROXY_CALM"); s_calm = !(e && *e == '0'); if (!s_calm) return 0; }
    if (!ea_range(msg, 4)) return 0;
    __try {
        id = rd32(msg);
        for (i = 0; i < CALM_N; i++) if (rd32(k_calm_obj[i]) == id && id) { st_calm_drop[i]++; return 1; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    st_calm_pass++;
    return 0;
}
static void calm_log(void)
{
    long tot = 0; char b[512]; int o = 0;
    for (int i = 0; i < CALM_N; i++) { tot += st_calm_drop[i]; if (st_calm_drop[i]) o += snprintf(b + o, sizeof b - (size_t)o, " %08X:%ld", k_calm_obj[i], st_calm_drop[i]); if (o > 440) break; }
    b[o] = 0;
    printf("[coop-proxy] v1062W1 calm: %ld reaction messages dropped, %ld other messages passed |%s\n", tot, st_calm_pass, o ? b : " (none)");
}

/* v1061F: entity -> netid of a LIVE character proxy (0 = not one). World NPC pose stream, main PPU thread. */
uint16_t gf_proxy_netid_of(uint32_t ent)
{
    extern int gf_coop_grab_holds(uint32_t ent);   /* v1065G: a proxy the local player is grabbing gets no pose stream / reapply */
    if (!ent || s_on != 1) return 0;
    for (int i = 0; i < PX_MAX; i++) if (s_px[i].state == PS_LIVE && s_px[i].ent == ent && s_px[i].kind != GF_WK_CAR) return gf_coop_grab_holds(ent) ? 0 : s_px[i].netid;
    return 0;
}
/* v1065G (agent G): same as gf_proxy_netid_of but ignores the grab hold (damage forwarding, grab detection). */
uint16_t gf_proxy_netid_any(uint32_t ent)
{
    if (!ent || s_on != 1) return 0;
    for (int i = 0; i < PX_MAX; i++) if (s_px[i].state == PS_LIVE && s_px[i].ent == ent && s_px[i].kind != GF_WK_CAR) return s_px[i].netid;
    return 0;
}

/* v1062C (agent C, appended): net id -> LIVE character proxy entity (0 = none). gf_coop_combat.c uses it as the attacker
 * when a host NPC's hit on the guest's puppet is forwarded to the guest's player. Main PPU thread. */
uint32_t gf_proxy_ent_of(uint16_t netid)
{
    if (!netid || s_on != 1) return 0;
    for (int i = 0; i < PX_MAX; i++) if (s_px[i].state == PS_LIVE && s_px[i].netid == netid && s_px[i].kind != GF_WK_CAR) return s_px[i].ent;
    return 0;
}

/* v1063P2 (for H): bind an EXISTING guest entity (a native mission / crew ped the host also has) as the live proxy of
 * netid instead of spawning a duplicate. Any factory proxy the id had is parked / released first. Release / release_all /
 * world change on an adopted proxy only UNBIND it (kill_ent). Peds only (cars: V). Main PPU thread. 1 = adopted. */
int gf_proxy_adopt(uint16_t netid, uint8_t kind, uint32_t cls, const uint8_t guid[16], uint32_t ent)
{
    proxy* p; uint32_t vt = 0, xf = 0, f = 0;
    if (!px_enabled() || !netid || !ent) return 0;
    if (kind == GF_WK_CAR || (!kind && kind_of_class(cls) == GF_WK_CAR)) return 0;
    if (!entity_info(ent, &vt, &xf)) return 0;
    for (int i = 0; i < PX_MAX; i++) if (s_px[i].state != PS_FREE && s_px[i].ent == ent && s_px[i].netid != netid) return 0;   /* already bound elsewhere */
    for (int i = 0; i < POOL_MAX; i++) if (s_pool[i].ent == ent) return 0;
    if ((p = find(netid)) != 0) {
        if (p->ent == ent && p->state == PS_LIVE) return 1;
        __try { kill_ent(p, "replaced by adoption"); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    } else {
        for (int i = 0; i < PX_MAX && !p; i++) if (s_px[i].state == PS_FREE) p = &s_px[i];
        if (!p) return 0;
        memset(p, 0, sizeof *p);
        p->netid = netid; p->kind = GF_WK_PED; p->cls = cls;
        if (guid) memcpy(p->guid, guid, 16);
        if (!read_xf12(xf, p->tgt)) return 0;
        p->tgt_ms = GetTickCount64();
    }
    p->ent = ent; p->vt = vt; p->used_cls = cls; p->state = PS_LIVE; p->spawn_ms = GetTickCount64(); p->adopted = 1; p->far_frames = 0; p->warp_cool = 0; p->have_cur = 0;
    p->alive_bit = read_u32(ent + 0x34u, &f) && (f & 1u);
    s_n_adopt++;
    look_check(p);   /* v1063P5 */
    printf("[coop-proxy] v1063P2 net %u ADOPTED guest ent %08X class %08X vt %08X\n", netid, ent, cls, vt); fflush(stdout);
    return 1;
}
/* v1126M migration (agent M): demote = bind OUR native as the proxy of netid (gf_proxy_adopt) and remember that it is ours
 * (release / world change = the game's release of the native, kill_ent). detach = promotion: forget the proxy WITHOUT touching
 * the entity (no park / release / unbind side effects) and return it; the caller makes it a source. Main PPU thread. */
int gf_proxy_adopt_mig(uint16_t netid, uint32_t cls, const uint8_t guid[16], uint32_t ent)
{
    proxy* p; const int r = gf_proxy_adopt(netid, GF_WK_PED, cls, guid, ent);
    if (r == 1 && (p = find(netid)) != 0 && p->ent == ent) p->mig = 1;
    return r;
}
int gf_proxy_is_adopted(uint16_t netid) { proxy* p = netid ? find(netid) : 0; return p && p->state == PS_LIVE && p->adopted; }
uint32_t gf_proxy_detach(uint16_t netid)
{
    proxy* p = netid ? find(netid) : 0; uint32_t ent;
    if (!p || p->state != PS_LIVE || !p->ent) return 0;
    ent = p->ent;
    { extern void gf_coop_pose_face_forget(uint32_t e); gf_coop_pose_face_forget(ent); }
    { extern void gf_wpose_forget(uint32_t e); gf_wpose_forget(ent); }
    if (p->kind == GF_WK_CAR) { extern void gf_veh_forget(uint32_t e); gf_veh_forget(ent); }
    printf("[migrate] v1126M net %u proxy ent %08X detached (%s) -> local source\n", netid, ent, p->adopted ? "our own native" : "factory proxy"); fflush(stdout);
    memset(p, 0, sizeof *p);   /* PS_FREE: no drive / late set / calm for it from now on */
    return ent;
}
/* v1063P2: live proxy entity of netid, peds AND cars (0 = none). */
uint32_t gf_proxy_any_ent_of(uint16_t netid)
{
    if (!netid || s_on != 1) return 0;
    for (int i = 0; i < PX_MAX; i++) if (s_px[i].state == PS_LIVE && s_px[i].netid == netid) return s_px[i].ent;
    return 0;
}

/* ---------------------------------------------------------------- v1063P5 ped appearance (spawn ext, H's v1063H API)
 * Findings (p63-4-echo LOOKDIAG, echo proxy vs its source ped): same template -> same skeleton resource (+0xB8) and the
 * same render model (render instance *(ent+0x64), model resource at instance+0x10, registered in the model registry
 * *(0x101F3670) with a GUID). The per-NPC face is procedural (face-bone offsets in the palette, v1061G). So: the host
 * sends the render model GUID; the guest checks its proxy's model; same model -> the proxy takes the WIRE face bones
 * (host NPC's exact face), different / unknown -> its own face (v1061G rule, avoids the broken-face case).
 * PED ext layout v1 (20 B): [0] 'P' [1] version 1 [2..3] reserved [4..19] render model GUID (zero = unknown).
 * H's dispatcher (gf_coop_world.c) calls the _ped hooks for peds only; cars are V's _car hooks. */
enum { LOOK_CACHE = 64, EXT_TAB = 128 };
static uint32_t s_lk_res[LOOK_CACHE]; static uint8_t s_lk_guid[LOOK_CACHE][16]; static int s_lk_ok[LOOK_CACHE], s_lk_i;
static uint16_t s_ext_id[EXT_TAB]; static uint8_t s_ext_guid[EXT_TAB][16]; static int s_ext_i;
static uint8_t s_ext_src[EXT_TAB]; static uint32_t s_ext_fp[EXT_TAB];
/* v1124H ext v3 look: host ped's appearance block *(ent+0x6C) (header 0x02000000): +0x34 / +0x38 / +0x3C tint colours (RGBA),
 * +0x44 head texture-swap object (name at obj+0x10, "swap_<head>_lod1"). The joiner's proxy gets the right model but rolls
 * its OWN tints and head variant (v1124G probe, net 10: host swap_thugheadyng01 + 231911FF, proxy ..02 + 38281EFF).
 * +0x40 / +0x50 (other material-selection objects) are not synced yet. GF_COOP_PED_LOOK=1 on (default OFF, see ped_look_on). */
static uint8_t s_ext_lkf[EXT_TAB]; static uint32_t s_ext_lk[EXT_TAB][4];   /* flags bit0 tints, bit1 head | c34 c38 c3C head-name hash */
static int s_pedlook = -1;
static unsigned s_n_lk_tx, s_n_lk_ok, s_n_lk_head, s_n_lk_nohead, s_n_lk_revert, s_n_lk_noblk;
static int ped_look_on(void) { if (s_pedlook < 0) { const char* e = getenv("GF_COOP_PED_LOOK"); s_pedlook = e && *e == '1'; } return s_pedlook; }   /* default OFF: h20 run -- the game re-writes the tints (reverted 10 of 19) and the host head variant was never in the proxy's controller lists (0 of 10) -> research only */
static uint32_t swap_hash(uint32_t tex)   /* FNV-1a of a texture-swap object's name, 0 = not a "swap..." object */
{
    char nm[40]; uint32_t h = 2166136261u;
    if (tex < 0x30000000u || tex >= 0x50000000u || (tex & 3u) || !read_bytes(tex + 0x10u, (uint8_t*)nm, 39)) return 0;
    nm[39] = 0; if (strncmp(nm, "swap", 4)) return 0;
    for (const char* q = nm; *q; q++) { if ((uint8_t)*q < 0x20 || (uint8_t)*q > 0x7E) return 0; h ^= (uint8_t)*q; h *= 16777619u; }
    return h ? h : 1u;
}
static uint32_t look_blk(uint32_t ent) { uint32_t b = 0, h = 0; if (!read_u32(ent + 0x6Cu, &b) || b < 0x30000000u || b >= 0x50000000u || !read_u32(b, &h) || h != 0x02000000u) return 0; return b; }
static uint32_t find_swap(uint32_t ent, uint32_t want)   /* the proxy's own candidate head textures: render instance material controllers */
{
    uint32_t inst = 0, vd = 0, vn = 0, obj, v, w;
    if (!want || !read_u32(ent + 0x64u, &inst) || inst < 0x30000000u || inst >= 0x50000000u) return 0;
    if (!read_u32(inst + 0x44u, &vd) || !read_u32(inst + 0x48u, &vn) || vd < 0x30000000u || vd >= 0x50000000u || vn > 16) return 0;
    for (uint32_t k = 0; k < vn; k++) {
        if (!read_u32(vd + 8 * k, &obj) || obj < 0x30000000u || obj >= 0x50000000u) continue;
        for (uint32_t o = 0; o < 0x40; o += 4) {
            if (!read_u32(obj + o, &v) || v < 0x30000000u || v >= 0x50000000u || (v & 3u)) continue;
            if (swap_hash(v) == want) return v;
            for (uint32_t o2 = 0; o2 < 0x100; o2 += 4) if (read_u32(v + o2, &w) && swap_hash(w) == want) return w;
        }
    }
    return 0;
}   /* v1064Q: ext v2 (GUID source, render-instance fingerprint) */
static int model_res_of(uint32_t ent, uint32_t* res)
{
    uint32_t inst = 0, r = 0;
    if (!read_u32(ent + 0x64u, &inst) || !ea_range(inst, 0x20) || !read_u32(inst + 0x10u, &r) || !ea_range(r, 0x20)) return 0;
    *res = r; return 1;
}
static int model_guid(uint32_t res, uint8_t g[16])
{
    for (int i = 0; i < LOOK_CACHE; i++) if (s_lk_res[i] == res) { if (s_lk_ok[i]) memcpy(g, s_lk_guid[i], 16); return s_lk_ok[i]; }
    {
        const int k = s_lk_i++ % LOOK_CACHE; uint8_t t[16]; int ok = mreg_lookup(res, 0, t, 0);
        if (ok) { int nz = 0; for (int j = 0; j < 16; j++) nz |= t[j]; ok = nz != 0; }
        if (!ok) return 0;   /* v1064Q: a failed registry walk (budget / model still registering) is not cached as 'no GUID' forever */
        s_lk_res[k] = res; s_lk_ok[k] = ok; if (ok) { memcpy(s_lk_guid[k], t, 16); memcpy(g, t, 16); }
        return ok;
    }
}
/* v1064Q (agent Q) ped appearance, part 2. RE (lifted code, verified by reading):
 *  - Every character (all ped classes derive from the character base ctor func_00137DBC) keeps its APPEARANCE MODEL GUID at
 *    ent+0x1C60 (16 B, registry GUID format = node+0x1C of *(0x101F3670), the format func_004596B8 looks up) and its skeleton
 *    GUID at ent+0x1C70. The ctor fills both with the invalid pattern (xx, BEEFBEEF, EAC15A55, 91100911).
 *  - iMsgManagedEntityCreated (0x101DBFD8) handler = func_00140FE4 loc_00141360: func_00338C78(stream request of *(ent+0x1C60),
 *    priority *(ent+0x1C80)), func_00463660(ent, ent+0x1C70) skeleton, model = func_004596B8(ent+0x1C60), vt+0x104 SetModel,
 *    then anim restore func_00460CE8(ent, *(ent+0x1C84)) (+0x1C84 = saved anim index, NOT appearance).
 *  - Distance LOD (manager only: func_00140238 from 0x0034D16C, func_00136DC0 from func_0034E380): beyond ~80 m the game saves
 *    the instance's GUID into +0x1C60 (func_001370A4, flag 0x02000000 in +0x1818) and drops the render model; it reloads from
 *    +0x1C60 when the player comes back. => a host ped far from the HOST has NO render instance (old ext: GUID 0 = "unknown")
 *    but its GUID is still at +0x1C60. Our proxies are not in the manager lists, so they never unload / reload by themselves.
 *  - The render model GUID is the whole per-ped "variant": textures / colours come with the model resource. No per-instance
 *    tint field was found (shader semantics TINT_COLOR = engine type 10, TINT_COLOR_INDEX = 36 exist; where the renderer takes
 *    the 4 tint colours from is NOT found yet) -> the ext carries a fingerprint of the render instance's value fields so the
 *    next run tells whether two instances of the same model differ at all ("fp equal / differs" in the census).
 * PED ext v2 (28 B): [0] 'P' [1] 2 [2] GUID source (0 none, 1 render instance, 2 saved +0x1C60) [3] flags (bit0 instance
 *   present) [4..19] model GUID [20..23] instance fingerprint (LE, 0 = none) [24..27] reserved. v1 (20 B) still accepted.
 * GF_COOP_PED_EXT=1 (default) v2, 0 = old v1 20-byte ext. */
static int s_pedext = -1;
static int guid_valid(const uint8_t g[16])
{
    int nz = 0; for (int j = 0; j < 16; j++) nz |= g[j];
    if (!nz) return 0;
    if (g[4] == 0xBE && g[5] == 0xEF && g[6] == 0xBE && g[7] == 0xEF && g[8] == 0xEA && g[9] == 0xC1 && g[10] == 0x5A && g[11] == 0x55) return 0;   /* ctor pattern */
    return 1;
}
static uint32_t inst_fp(uint32_t ent)   /* render instance value fields (ctor func_005227C4: +0x14..+0x23, +0x30..+0x37, +0x50) */
{
    uint32_t inst = 0, h = 2166136261u; uint8_t b[16 + 8 + 2];
    if (!read_u32(ent + 0x64u, &inst) || !ea_range(inst, 0x68)) return 0;
    if (!read_bytes(inst + 0x14u, b, 16) || !read_bytes(inst + 0x30u, b + 16, 8) || !read_bytes(inst + 0x50u, b + 24, 2)) return 0;
    for (int i = 0; i < (int)sizeof b; i++) { h ^= b[i]; h *= 16777619u; }
    return h ? h : 1u;
}
static int ped_guid_of(uint32_t ent, uint8_t g[16], int* src)   /* 1 = ok; *src 1 instance, 2 saved +0x1C60 */
{
    uint32_t res = 0; uint8_t t[16];
    *src = 0;
    if (model_res_of(ent, &res) && model_guid(res, g) && guid_valid(g)) { *src = 1; return 1; }
    if (read_bytes(ent + 0x1C60u, t, 16) && guid_valid(t)) { memcpy(g, t, 16); *src = 2; return 1; }
    return 0;
}
int gf_world_spawn_ext_ped(uint32_t ent, uint8_t kind, uint8_t* out, int max)   /* host, main PPU thread, at spawn + every rolling refresh (v1064Q) */
{
    uint32_t res = 0; uint8_t g[16]; int src = 0;
    if (kind == GF_WK_CAR) return 0;
    if (!out || max < 20 || !ent) return 0;
    if (s_pedext < 0) { const char* e = getenv("GF_COOP_PED_EXT"); s_pedext = !(e && *e == '0'); printf("[coop-proxy] v1064Q ped spawn ext %s (model GUID from render instance OR saved ent+0x1C60, refreshed with every spawn refresh, instance fingerprint)\n", s_pedext ? "v2 ON" : "v1 (old)"); fflush(stdout); }
    if (!s_pedext || max < 28) {
        memset(out, 0, 20); out[0] = 'P'; out[1] = 1;
        __try { if (model_res_of(ent, &res) && model_guid(res, g)) memcpy(out + 4, g, 16); } __except (EXCEPTION_EXECUTE_HANDLER) { }
        s_n_ext_tx++;
        return 20;
    }
    memset(out, 0, 28); out[0] = 'P'; out[1] = 2;
    __try {
        uint32_t fp = 0, inst = 0;
        if (ped_guid_of(ent, g, &src)) memcpy(out + 4, g, 16);
        out[2] = (uint8_t)src;
        if (read_u32(ent + 0x64u, &inst) && ea_range(inst, 0x68)) out[3] |= 1;
        fp = inst_fp(ent); out[20] = (uint8_t)fp; out[21] = (uint8_t)(fp >> 8); out[22] = (uint8_t)(fp >> 16); out[23] = (uint8_t)(fp >> 24);
        if (src == 1) s_n_ext_inst++; else if (src == 2) s_n_ext_saved++; else s_n_ext_none++;
        {   /* host diag: the first peds' ext fields */
            static int nlog; if (nlog < 12) { uint8_t sv[16]; int same = read_bytes(ent + 0x1C60u, sv, 16) && !memcmp(sv, g, 16);
                nlog++; printf("[coop-proxy] v1064Q host ped ext ent %08X: GUID %02X%02X%02X%02X%02X%02X%02X%02X.. src %s, +0x1C60 %s, instance %s, fp %08X\n",
                    ent, out[4], out[5], out[6], out[7], out[8], out[9], out[10], out[11], src == 1 ? "instance" : src == 2 ? "saved+1C60" : "NONE",
                    src ? (same ? "same" : "differs") : "-", (out[3] & 1) ? "yes" : "no", fp); fflush(stdout); } }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    s_n_ext_tx++;
    if (max >= 49 && ped_look_on()) {   /* v1124H ext v3: + look */
        __try { const uint32_t b = look_blk(ent); uint32_t c[3] = { 0, 0, 0 }, tex = 0, hh = 0;
            if (b && read_u32(b + 0x34u, &c[0]) && read_u32(b + 0x38u, &c[1]) && read_u32(b + 0x3Cu, &c[2])) {
                out[1] = 3; out[48] = 1;
                for (int i = 0; i < 3; i++) { out[28 + 4*i] = (uint8_t)c[i]; out[29 + 4*i] = (uint8_t)(c[i] >> 8); out[30 + 4*i] = (uint8_t)(c[i] >> 16); out[31 + 4*i] = (uint8_t)(c[i] >> 24); }
                if (read_u32(b + 0x44u, &tex) && (hh = swap_hash(tex)) != 0) { out[48] |= 2; out[40] = (uint8_t)hh; out[41] = (uint8_t)(hh >> 8); out[42] = (uint8_t)(hh >> 16); out[43] = (uint8_t)(hh >> 24); }
                s_n_lk_tx++; return 49; } } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    return 28;
}
void gf_proxy_spawn_ext_ped(uint16_t netid, const uint8_t* ext, int len)   /* guest, main PPU thread, before gf_proxy_spawn (and again when a refresh changed it, v1064Q) */
{
    int changed = 0;
    if (!ext || len < 20 || ext[0] != 'P' || (ext[1] != 1 && ext[1] != 2 && ext[1] != 3)) return;
    if (ext[1] >= 2 && len < 28) return;
    if (ext[1] == 3 && len < 49) return;
    {
        int k = -1;
        for (int i = 0; i < EXT_TAB; i++) if (s_ext_id[i] == netid) { k = i; break; }
        if (k < 0) {   /* v1066N (agent N, C's find): the plain ring overwrote the ext of still-LIVE proxies (> 128 net ids per
                        * session) -> census "no ext 14". Take the next ring slot whose net id has no proxy any more. */
            for (int t = 0; t < EXT_TAB && k < 0; t++) { const int j = (s_ext_i + t) % EXT_TAB; if (!s_ext_id[j] || !find(s_ext_id[j])) { k = j; s_ext_i = j + 1; } }
            if (k < 0) k = s_ext_i++ % EXT_TAB;
            changed = 1;
        }
        else changed = memcmp(s_ext_guid[k], ext + 4, 16) != 0;
        s_ext_id[k] = netid; memcpy(s_ext_guid[k], ext + 4, 16);
        s_ext_src[k] = ext[1] >= 2 ? ext[2] : (guid_valid(ext + 4) ? 1 : 0);
        s_ext_fp[k] = ext[1] >= 2 ? ((uint32_t)ext[20] | ((uint32_t)ext[21] << 8) | ((uint32_t)ext[22] << 16) | ((uint32_t)ext[23] << 24)) : 0;
        {   const uint8_t f = ext[1] == 3 ? ext[48] : 0; uint32_t lk[4] = { 0, 0, 0, 0 };   /* v1124H look */
            if (f) for (int i = 0; i < 4; i++) lk[i] = (uint32_t)ext[28 + 4*i] | ((uint32_t)ext[29 + 4*i] << 8) | ((uint32_t)ext[30 + 4*i] << 16) | ((uint32_t)ext[31 + 4*i] << 24);
            if (f != s_ext_lkf[k] || memcmp(lk, s_ext_lk[k], sizeof lk)) { s_ext_lkf[k] = f; memcpy(s_ext_lk[k], lk, sizeof lk); proxy* q = find(netid); if (q) { q->lk_state = 0; q->lk_next = 0; q->lk_fails = 0; } } }
        s_n_ext_rx++;
    }
    if (changed) { proxy* p = find(netid); if (p && p->state == PS_LIVE) { p->look_tries = 0; look_check(p); } }   /* ext after the spawn (resend / reconnect / v1064Q refresh with a now-known GUID) */
}
static int ext_of(uint16_t netid, uint8_t g[16], int* src, uint32_t* fp)   /* v1064Q */
{
    for (int i = 0; i < EXT_TAB; i++) if (s_ext_id[i] == netid && netid) {
        if (g) memcpy(g, s_ext_guid[i], 16); if (src) *src = s_ext_src[i]; if (fp) *fp = s_ext_fp[i];
        return guid_valid(s_ext_guid[i]) ? 1 : 2;   /* 2 = ext known, host GUID unknown */
    }
    return 0;
}
/* v1064Q: a factory proxy whose model was not resident (c4 == 0) gets the HOST ped's GUID at +0x1C60 before we send
 * iMsgManagedEntityCreated, so the game's own appearance init (func_00140FE4 loc_00141360) streams + builds the host's model
 * instead of the template's random pick. GF_COOP_PROXY_PREGUID=0 = old. */
static int s_preguid = -1, s_nmfix = -1; static unsigned s_n_preguid, s_n_preguid_inexact, s_n_preguid_nores, s_n_nm_setmodel, s_n_nm_recreate, s_n_nm_wait;
static int preguid_write(proxy* p, uint32_t ent)
{
    uint8_t g[16]; uint8_t* d;
    if (s_preguid < 0) { const char* e = getenv("GF_COOP_PROXY_PREGUID"); s_preguid = e && *e == '1';   /* v1064Z1: default OFF -- froze the coop-local guest (null calls in func_00140FE4) */ printf("[coop-proxy] v1064Q pre-created host GUID %s (proxy+0x1C60 = host model before iMsgManagedEntityCreated)\n", s_preguid ? "ON" : "off"); fflush(stdout); }
    if (!s_preguid || p->kind == GF_WK_CAR || ext_of(p->netid, g, 0, 0) != 1) return 0;
    if (!p->exact_tmpl) { s_n_preguid_inexact++; return 0; }   /* other template (substitute / same class): its skeleton may not fit the host model */
    {   /* only a model that is registered (resident) here: the created handler does model = func_004596B8(+0x1C60) -> vt+0x104
         * right away; a GUID we do not have would give it a null model. Not resident -> keep the template's pick (v1063P7 later). */
        uint32_t res = 0;
        if (!mreg_lookup(0, g, 0, &res) || !ea_range(res, 0x20)) { s_n_preguid_nores++; return 0; }
    }
    if (!mem_access(ent + 0x1C60u, 16, 1)) return 0;
    __try { d = vm_base + ent + 0x1C60u; memcpy(d, g, 16); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    s_n_preguid++;
    return 1;
}
/* v1063P7: a template can carry SEVERAL render models (p63-7-echo: 9 of 77 proxies got another model than their host
 * NPC from the same attrib). Swap to the host NPC's model if it is loaded here: generic character SetModel vt+0x108
 * (func_00464818: release the old render instance, palette, new 0x68-byte instance at ent+0x64 -- the call gf_coop_look.c
 * makes on the puppet). Only when vt+0x108 really is func_00464818 (or GF_COOP_PROXY_SETMODEL=2). =0 off. */
static int set_model(proxy* p, const uint8_t want[16])
{
    uint32_t res = 0, vt = 0, opd = 0, fn = 0;
    if (s_setmodel < 0) { const char* e = getenv("GF_COOP_PROXY_SETMODEL"); s_setmodel = e && *e ? atoi(e) : 0;   /* v1064I6: default 0 -- every frozen echo (i64-2/3/4) froze seconds after SetModel bursts */
        printf("[coop-proxy] v1064I6 runtime SetModel on ped proxies %s (GF_COOP_PROXY_SETMODEL=1 re-enables; froze the game in i64-2/3/4 echo)\n", s_setmodel ? "ON" : "OFF"); fflush(stdout); }
    if (!s_setmodel || p->adopted || p->look_tries > 6) return 0;
    if (!mreg_lookup(0, want, 0, &res) || !ea_range(res, 0x20)) { s_n_setmodel_nores++; return 0; }
    if (!read_u32(p->ent, &vt) || !read_u32(vt + 0x108u, &opd) || !read_u32(opd, &fn)) return 0;
    if (fn != 0x00464818u && s_setmodel != 2) {
        static int said; if (!said++) { printf("[coop-proxy] v1063P7 SetModel skipped: vt %08X +0x108 -> %08X (not func_00464818)\n", vt, fn); fflush(stdout); }
        return 0;
    }
    { extern void gf_coop_pose_face_forget(uint32_t e); gf_coop_pose_face_forget(p->ent); }
    if (!setmodel_call(p->ent, want, res)) { s_n_setmodel_fail++; return 0; }   /* v1070D: vt+0x104(node) */
    s_n_setmodel++;
    if (s_n_setmodel <= 10) { printf("[coop-proxy] v1063P7 net %u ent %08X: SetModel -> host model %02X%02X%02X%02X.. (res %08X)\n", p->netid, p->ent, want[0], want[1], want[2], want[3], res); fflush(stdout); }
    return 1;
}
static void look_log(void)
{
    if (ped_look_on()) { printf("[coop-proxy] v1124H ped look: host sent %u | guest applied %u (head swapped %u, head variant not loaded here %u, game reverted %u, no look block %u)\n",
        s_n_lk_tx, s_n_lk_ok, s_n_lk_head, s_n_lk_nohead, s_n_lk_revert, s_n_lk_noblk); fflush(stdout); }
}
static void look_apply(proxy* p, ULONGLONG now)   /* v1124H guest, main PPU thread: host tints + head texture onto a live ped proxy */
{
    if (!ped_look_on() || p->kind == GF_WK_CAR || !p->ent || now < p->lk_next || p->lk_fails > 8) return;
    p->lk_next = now + (p->lk_state == 1 ? 2000 : 500);
    int k = -1; for (int i = 0; i < EXT_TAB; i++) if (s_ext_id[i] == p->netid && p->netid) { k = i; break; }
    if (k < 0 || !s_ext_lkf[k]) return;
    const uint32_t b = look_blk(p->ent); uint32_t c4 = 0;
    if (!b || !read_u32(p->ent + 0xC4u, &c4) || !c4) { if (++p->lk_fails > 8) s_n_lk_noblk++; return; }
    __try {
        int changed = 0;
        if (s_ext_lkf[k] & 1) for (int i = 0; i < 3; i++) { const uint32_t o = 0x34u + 4u * (uint32_t)i; if (rd32(b + o) != s_ext_lk[k][i]) { wr32(b + o, s_ext_lk[k][i]); changed = 1; } }
        if (s_ext_lkf[k] & 2) { uint32_t cur = 0; read_u32(b + 0x44u, &cur);
            if (swap_hash(cur) != s_ext_lk[k][3]) { const uint32_t t = find_swap(p->ent, s_ext_lk[k][3]);
                if (t) { wr32(b + 0x44u, t); changed = 1; s_n_lk_head++; } else if (p->lk_state == 0) s_n_lk_nohead++; } }
        if (p->lk_state == 1 && changed) s_n_lk_revert++;   /* the game wrote its own values back after we applied ours */
        if (p->lk_state == 0) { s_n_lk_ok++; p->lk_state = 1;
            static int nlog; if (nlog < 10) { nlog++; printf("[coop-proxy] v1124H look net %u ent %08X: tints %08X %08X %08X%s\n", p->netid, p->ent, s_ext_lk[k][0], s_ext_lk[k][1], s_ext_lk[k][2],
                (s_ext_lkf[k] & 2) ? (swap_hash(rd32(b + 0x44u)) == s_ext_lk[k][3] ? ", head = host's" : ", head NOT available here") : ""); fflush(stdout); } }
    } __except (EXCEPTION_EXECUTE_HANDLER) { p->lk_fails++; }
}
static void look_check(proxy* p)
{
    uint8_t want[16], have[16]; uint32_t res = 0; int k = -1, nz = 0;
    extern void gf_coop_pose_face_wire(uint32_t ent, int on);
    if (!p || !p->ent || p->kind == GF_WK_CAR) return;
    for (int i = 0; i < EXT_TAB; i++) if (s_ext_id[i] == p->netid) { k = i; break; }
    if (k < 0) {   /* echo: no ext path through the network -> compare with the source ped directly */
        if (gf_coop_mode() != 3) { p->look = 0; s_n_look_unk++; return; }
        {
            uint32_t ents[256]; uint16_t ids[256]; float pos[256][3]; int n = 0; uint32_t src = 0, sres = 0;
            __try { n = gf_coop_world_host_peds(ents, ids, pos, 256); } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
            for (int t = 0; t < n; t++) if (ids[t] == p->netid) { src = ents[t]; break; }
            if (!src || !model_res_of(src, &sres) || !model_guid(sres, want)) { p->look = 0; s_n_look_unk++; return; }
        }
    } else memcpy(want, s_ext_guid[k], 16);
    for (int j = 0; j < 16; j++) nz |= want[j];
    if (!nz || !model_res_of(p->ent, &res) || !model_guid(res, have)) { p->look = 0; s_n_look_unk++; return; }
    if (!memcmp(want, have, 16)) { p->look = 1; s_n_look_match++; gf_coop_pose_face_wire(p->ent, 1); }
    else {
        p->look = 2; s_n_look_diff++; gf_coop_pose_face_wire(p->ent, 0);
        if (set_model(p, want)) { p->look = 0; p->look_at = 0; s_n_look_diff--; s_n_look_unk++; return; }   /* v1063P7: model swapped -> re-check next frame (counted as unknown until then) */
        if (s_n_look_diff <= 20) { printf("[coop-proxy] v1063P5 net %u ent %08X: model differs from the host NPC (have %02X%02X%02X%02X.., host %02X%02X%02X%02X..) -> own face\n",
            p->netid, p->ent, have[0], have[1], have[2], have[3], want[0], want[1], want[2], want[3]); fflush(stdout); }
    }
}
/* v1064K (agent K, cars): render model GUID of any entity (ent+0x64 instance +0x10 resource -> model registry GUID), used by
 * gf_coop_vehicle.c for the car look ext / compare. 1 = ok. Main PPU thread. */
int gf_proxy_model_guid_of(uint32_t ent, uint8_t g[16])
{
    uint32_t res = 0;
    if (!ent || !g) return 0;
    __try { return model_res_of(ent, &res) && model_guid(res, g); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* v1064Q: a ped proxy still without a render instance 10 s after spawn (v1063P6 "invisible proxy"). Proxies are not in the
 * manager lists, so the game's own LOD reload (func_00136DC0) never runs for them. Fix (GF_COOP_PROXY_NOMODEL_FIX=1 default):
 *  - host model resident here -> generic SetModel vt+0x108 (func_00464818, as v1063P7) when it is safe (anim controller
 *    +0xC4 present or no skeleton +0xB8: func_0046461C calls func_003CAE04(*(ent+0xC4)) when +0xB8 != 0);
 *  - else, if the anim controller is missing too (= appearance init never completed): host GUID -> +0x1C60 and
 *    iMsgManagedEntityCreated again (the game's own init path, the same call try_spawn makes) -- once per proxy. */
static void nomodel_fix(proxy* p)
{
    uint8_t g[16]; uint32_t res = 0, c4 = 0, b8 = 0, vt = 0, opd = 0, fn = 0, buf;
    if (s_nmfix < 0) { const char* e = getenv("GF_COOP_PROXY_NOMODEL_FIX"); s_nmfix = e && *e == '1';   /* v1064Z1: default OFF (re-runs the same appearance init as PREGUID) */ printf("[coop-proxy] v1064Q no-model proxy fix %s\n", s_nmfix ? "ON" : "off"); fflush(stdout); }
    if (!s_nmfix || !p || !p->ent || p->kind == GF_WK_CAR || p->adopted) return;
    if (ext_of(p->netid, g, 0, 0) != 1) { s_n_nm_wait++; return; }
    read_u32(p->ent + 0xC4u, &c4); read_u32(p->ent + 0xB8u, &b8);
    if (s_setmodel < 0) { const char* e = getenv("GF_COOP_PROXY_SETMODEL"); s_setmodel = e && *e ? atoi(e) : 0; }   /* v1064I6 */
    if (s_setmodel > 0 && mreg_lookup(0, g, 0, &res) && ea_range(res, 0x20) && (c4 || !b8)
        && read_u32(p->ent, &vt) && read_u32(vt + 0x108u, &opd) && read_u32(opd, &fn) && fn == 0x00464818u) {
        extern void gf_coop_pose_face_forget(uint32_t e); gf_coop_pose_face_forget(p->ent);
        if (setmodel_call(p->ent, g, res)) { s_n_nm_setmodel++; p->look = 0; p->look_tries = 0; p->look_at = 0;
            if (s_n_nm_setmodel <= 10) { printf("[coop-proxy] v1064Q net %u ent %08X: no render model -> SetModel host model %02X%02X%02X%02X..\n", p->netid, p->ent, g[0], g[1], g[2], g[3]); fflush(stdout); }
            return; }
    }
    if (c4 || !(buf = scratch())) return;
    {
        uint32_t id = 0; read_u32(0x101DBFD8u, &id);
        if (!id || !preguid_write(p, p->ent)) return;
        __try { wr32(buf + 0x70, id); wr32(buf + 0x74, p->ent); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        gcall(0x00455A2Cu, buf + 0x70, p->ent + 0x30u, 0, 0, 0);
        s_n_nm_recreate++; p->look = 0; p->look_tries = 0; p->look_at = GetTickCount64() + 2000;
        if (s_n_nm_recreate <= 10) { printf("[coop-proxy] v1064Q net %u ent %08X: no render model / no anim controller -> host GUID + iMsgManagedEntityCreated again\n", p->netid, p->ent); fflush(stdout); }
    }
}
/* v1065M1 (agent M) diagnostic GF_COOP_LOOKDUMP=<dir> (echo only): per-NPC "variation" RE. For up to 8 model-matched ped
 * proxies, dump the HOST source ped and the PROXY (entity 0x2000 B + every heap pointer in it 0x80 B; render instance
 * 0x68 B + its pointers 0x100 B + their pointers 0x80 B) to <dir>/<tick>_<netid>_<h|p>.bin, 3 ticks (10 s apart).
 * Record: [u32 BE path tag][u32 BE ea][u32 BE len][bytes]. Tags: 0xE0000000 ent, 0xE1000000|off ent ptr, 0x10000000 inst,
 * 0x11000000|off inst ptr, 0x12000000|off<<12|off2 2nd level. */
static int s_ldump_tick;
static void ld_rec(FILE* f, uint32_t tag, uint32_t ea, uint32_t len)
{
    static uint8_t buf[0x2000]; uint8_t h[12];
    if (len > sizeof buf || !ea_range(ea, len) || !mem_access(ea, len, 0) || !read_bytes(ea, buf, len)) return;
    h[0] = (uint8_t)(tag >> 24); h[1] = (uint8_t)(tag >> 16); h[2] = (uint8_t)(tag >> 8); h[3] = (uint8_t)tag;
    h[4] = (uint8_t)(ea >> 24); h[5] = (uint8_t)(ea >> 16); h[6] = (uint8_t)(ea >> 8); h[7] = (uint8_t)ea;
    h[8] = (uint8_t)(len >> 24); h[9] = (uint8_t)(len >> 16); h[10] = (uint8_t)(len >> 8); h[11] = (uint8_t)len;
    fwrite(h, 1, 12, f); fwrite(buf, 1, len, f);
}
static int ld_ptr(uint32_t v) { return v >= 0x30000000u && v < 0x50000000u && !(v & 3u); }
static void ld_ent(const char* dir, uint16_t netid, char side, uint32_t ent)
{
    char path[512]; FILE* f; uint32_t inst = 0, v, w;
    snprintf(path, sizeof path, "%s\\%d_%u_%c.bin", dir, s_ldump_tick, netid, side);
    if (!(f = fopen(path, "wb"))) return;
    __try {
        ld_rec(f, 0xE0000000u, ent, 0x2000);
        for (uint32_t o = 0; o < 0x2000; o += 4) if (read_u32(ent + o, &v) && ld_ptr(v) && v != ent) ld_rec(f, 0xE1000000u | o, v, 0x80);
        {   /* v1065M2: anim controller ent+0xC4 (0x320) + its pointers (0x200) + their pointers (0x100) -- per-NPC face data? */
            uint32_t ac = 0;
            if (read_u32(ent + 0xC4u, &ac) && ld_ptr(ac)) {
                ld_rec(f, 0xC4000000u, ac, 0x320);
                for (uint32_t o = 0; o < 0x320; o += 4) if (read_u32(ac + o, &v) && ld_ptr(v) && v != ent) {
                    ld_rec(f, 0xC5000000u | o, v, 0x200);
                    for (uint32_t o2 = 0; o2 < 0x200; o2 += 4) if (read_u32(v + o2, &w) && ld_ptr(w) && w != ent) ld_rec(f, 0xC6000000u | (o << 12) | o2, w, 0x100);
                }
            }
        }
        if (read_u32(ent + 0x64u, &inst) && ea_range(inst, 0x68)) {
            ld_rec(f, 0x10000000u, inst, 0x68);
            for (uint32_t o = 0; o < 0x68; o += 4) if (read_u32(inst + o, &v) && ld_ptr(v)) {
                ld_rec(f, 0x11000000u | o, v, 0x100);
                for (uint32_t o2 = 0; o2 < 0x100; o2 += 4) if (read_u32(v + o2, &w) && ld_ptr(w)) ld_rec(f, 0x12000000u | (o << 12) | o2, w, 0x80);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    fclose(f);
}
static void look_dump(void)
{
    static const char* dir = (const char*)1; uint32_t ents[256]; uint16_t ids[256]; float pos[256][3]; int n = 0, done = 0;
    if (dir == (const char*)1) { dir = getenv("GF_COOP_LOOKDUMP"); if (dir && !*dir) dir = 0; if (dir) { printf("[coop-proxy] v1065M1 look dump ON -> %s\n", dir); fflush(stdout); } }
    if (!dir || s_ldump_tick >= 3 || gf_coop_mode() != 3) return;
    __try { n = gf_coop_world_host_peds(ents, ids, pos, 256); } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
    for (int i = 0; i < PX_MAX && done < 8; i++) {
        proxy* p = &s_px[i]; uint32_t src = 0;
        if (p->state != PS_LIVE || !p->ent || p->kind == GF_WK_CAR || p->look != 1) continue;
        for (int t = 0; t < n; t++) if (ids[t] == p->netid) { src = ents[t]; break; }
        if (!src) continue;
        ld_ent(dir, p->netid, 'h', src); ld_ent(dir, p->netid, 'p', p->ent); done++;
    }
    printf("[coop-proxy] v1065M1 look dump tick %d: %d pairs\n", s_ldump_tick, done); fflush(stdout);
    if (done) s_ldump_tick++;
}
/* v1124G (agent NPC) cross-process look probe GF_COOP_LOOKDUMP2=<dir> (any mode, default off): every 15 s (4 ticks) the
 * peds this peer SOURCES within 25 m of the local player ('s') and the live ped proxies within 25 m ('p') are dumped like
 * v1065M1 (+ the model resource inst+0x10 0x200 B and its pointers 0x100 B, tags 0x20000000 / 0x21000000|off) to
 * <dir>\<GF_COOP_LOGTAG>_<tick>_<netid>_<s|p>.bin -> compare the owner's 's' file with the other window's 'p' file. */
extern int gf_coop_local_pos(float out[3]);   /* gf_coop.c */
static void ld_ent2(const char* path, uint32_t ent)
{
    FILE* f; uint32_t inst = 0, res = 0, v;
    if (!(f = fopen(path, "wb"))) return;
    __try {
        ld_rec(f, 0xE0000000u, ent, 0x2000);
        for (uint32_t o = 0; o < 0x2000; o += 4) if (read_u32(ent + o, &v) && ld_ptr(v) && v != ent) ld_rec(f, 0xE1000000u | o, v, 0x80);
        if (read_u32(ent + 0x64u, &inst) && ea_range(inst, 0x68)) {
            uint32_t w;
            ld_rec(f, 0x10000000u, inst, 0x68);
            for (uint32_t o = 0; o < 0x68; o += 4) if (read_u32(inst + o, &v) && ld_ptr(v)) {
                ld_rec(f, 0x11000000u | o, v, 0x100);
                for (uint32_t o2 = 0; o2 < 0x100; o2 += 4) if (read_u32(v + o2, &w) && ld_ptr(w)) ld_rec(f, 0x12000000u | (o << 12) | o2, w, 0x80);
            }
            {   /* inst+0x44 = per-instance material param controllers {obj, type}: their objects 2 levels deeper */
                uint32_t vd = 0, vn = 0;
                if (read_u32(inst + 0x44u, &vd) && read_u32(inst + 0x48u, &vn) && ld_ptr(vd) && vn <= 16)
                    for (uint32_t k = 0; k < vn; k++) { uint32_t obj = 0;
                        if (!read_u32(vd + 8 * k, &obj) || !ld_ptr(obj)) continue;
                        for (uint32_t o = 0; o < 0x40; o += 4) if (read_u32(obj + o, &v) && ld_ptr(v) && v != ent && v != inst) {
                            ld_rec(f, 0x30000000u | (k << 16) | o, v, 0x100);
                            for (uint32_t o2 = 0; o2 < 0x100; o2 += 4) if (read_u32(v + o2, &w) && ld_ptr(w) && w != ent && w != inst) ld_rec(f, 0x31000000u | (k << 20) | (o << 10) | o2, w, 0x80);
                        } }
            }
            if (read_u32(inst + 0x10u, &res) && ea_range(res, 0x200)) {
                ld_rec(f, 0x20000000u, res, 0x200);
                for (uint32_t o = 0; o < 0x200; o += 4) if (read_u32(res + o, &v) && ld_ptr(v)) ld_rec(f, 0x21000000u | o, v, 0x100);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    fclose(f);
}
static void mreg_share(uint32_t res, int* direct, int* indirect)   /* v1124G: registry nodes whose resource is res */
{
    uint32_t tab = 0, bk = 0, n = 0; long budget = 300000;
    *direct = *indirect = 0;
    if (!res || !read_u32(0x101F3670u, &tab) || !ea_range(tab, 8) || !read_u32(tab, &bk) || !read_u32(tab + 4, &n)) return;
    if (!ea_range(bk, 4) || n == 0 || n > (1u << 20) || !mem_access(bk, (size_t)n * 4u, 0)) return;
    __try {
        for (uint32_t b = 0; b < n; b++) {
            uint32_t node = rd32(bk + 4 * b);
            while (node && ea_range(node, 0x30) && budget-- > 0) {
                uint32_t r, r2 = 0;
                if (!mem_access(node, 0x30, 0)) break;
                r = rd32(node + 8);
                if (r == res) (*direct)++;
                else if (r && ea_range(r, 0x10) && read_u32(r + 8, &r2) && r2 == res) (*indirect)++;
                node = rd32(node + 0x2C);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
static void look_line(const char* tag, int tick, const char* role, uint16_t netid, uint32_t ent)   /* v1124G: the GUID sources side by side */
{
    uint32_t inst = 0, res = 0, ig = 0, t4 = 0, ty = 0; uint8_t gi[16], gs[16], gm[16]; int dir = 0, ind = 0, okm = 0;
    memset(gi, 0, 16); memset(gs, 0, 16); memset(gm, 0, 16);
    __try {
        if (read_u32(ent + 0x64u, &inst) && ea_range(inst, 0x68)) {
            read_u32(inst + 0x10u, &res);
            if (read_u32(inst + 4u, &ig) && ig && ea_range(ig, 16)) read_bytes(ig, gi, 16);
            if (res) { okm = model_guid(res, gm); mreg_share(res, &dir, &ind); }
            { uint32_t node = okm ? mreg_node(gm) : 0; if (node && read_u32(node + 4, &t4) && ea_range(t4, 0x10)) read_u32(t4 + 8, &ty); }
        }
        read_bytes(ent + 0x1C60u, gs, 16);
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    printf("[coop-proxy] v1124G look %s tick %d %s net %u: inst+4 GUID %02X%02X%02X%02X%02X%02X%02X%02X | registry(res) GUID %02X%02X%02X%02X%02X%02X%02X%02X (%s, node type %08X, res shared by %d direct / %d indirect nodes) | +0x1C60 %02X%02X%02X%02X%02X%02X%02X%02X\n",
           tag, tick, role, netid, gi[0], gi[1], gi[2], gi[3], gi[4], gi[5], gi[6], gi[7], gm[0], gm[1], gm[2], gm[3], gm[4], gm[5], gm[6], gm[7], okm ? "ok" : "none", ty, dir, ind,
           gs[0], gs[1], gs[2], gs[3], gs[4], gs[5], gs[6], gs[7]);
}
static void look_dump2(ULONGLONG now)
{
    static const char* dir = (const char*)1; static ULONGLONG next; static int tick;
    uint32_t ents[256]; uint16_t ids[256]; float pos[256][3]; float me[3]; int n = 0, ns = 0, np = 0; char path[600]; const char* tag;
    if (dir == (const char*)1) { dir = getenv("GF_COOP_LOOKDUMP2"); if (dir && !*dir) dir = 0; if (dir) { printf("[coop-proxy] v1124G look probe ON -> %s\n", dir); fflush(stdout); } }
    if (!dir || tick >= 6 || now < next) return;
    next = now + 2000;
    { extern int gf_coop_connected(void); if (!gf_coop_connected() || !gf_coop_local_pos(me)) return; }
    tag = getenv("GF_COOP_LOGTAG"); if (!tag || !*tag) tag = "x";
    __try { n = gf_coop_world_host_peds(ents, ids, pos, 256); } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
    for (int t = 0; t < n && ns < 10; t++) {
        const float dx = pos[t][0] - me[0], dz = pos[t][2] - me[2]; uint8_t g[16]; int src = 0;
        if (dx * dx + dz * dz > 25.0f * 25.0f || !ents[t]) continue;
        memset(g, 0, 16); __try { ped_guid_of(ents[t], g, &src); } __except (EXCEPTION_EXECUTE_HANDLER) { }
        snprintf(path, sizeof path, "%s\\%s_%d_%u_s.bin", dir, tag, tick, ids[t]); ld_ent2(path, ents[t]); ns++; look_line(tag, tick, "SOURCE", ids[t], ents[t]);
        printf("[coop-proxy] v1124G probe %s tick %d SOURCE net %u ent %08X at (%.1f %.1f %.1f) d %.1f m, model %02X%02X%02X%02X%02X%02X%02X%02X (src %d)\n", tag, tick, ids[t], ents[t],
               pos[t][0], pos[t][1], pos[t][2], sqrtf(dx * dx + dz * dz), g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], src);
    }
    for (int i = 0; i < PX_MAX && np < 10; i++) {
        proxy* p = &s_px[i]; float m[12]; uint32_t vt = 0, xf = 0; uint8_t g[16]; int src = 0;
        if (p->state != PS_LIVE || !p->ent || p->kind == GF_WK_CAR) continue;
        if (!entity_info(p->ent, &vt, &xf) || !read_xf12(xf, m)) continue;
        { const float dx = m[9] - me[0], dz = m[11] - me[2];
          if (dx * dx + dz * dz > 25.0f * 25.0f) continue;
          memset(g, 0, 16); __try { ped_guid_of(p->ent, g, &src); } __except (EXCEPTION_EXECUTE_HANDLER) { }
          snprintf(path, sizeof path, "%s\\%s_%d_%u_p.bin", dir, tag, tick, p->netid); ld_ent2(path, p->ent); np++; look_line(tag, tick, "PROXY", p->netid, p->ent);
          printf("[coop-proxy] v1124G probe %s tick %d PROXY net %u ent %08X at (%.1f %.1f %.1f) d %.1f m, model %02X%02X%02X%02X%02X%02X%02X%02X (src %d) look %d exact %d attrib %08X cls %08X\n", tag, tick, p->netid, p->ent,
                 m[9], m[10], m[11], sqrtf(dx * dx + dz * dz), g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], src, p->look, p->exact_tmpl, p->attrib, p->used_cls); }
    }
    if (!ns && !np) return;   /* nothing near yet: look again in 2 s */
    printf("[coop-proxy] v1124G probe %s tick %d: me (%.1f %.1f %.1f), %d sources, %d proxies dumped\n", tag, tick, me[0], me[1], me[2], ns, np); fflush(stdout);
    tick++; next = now + 15000;
}
/* v1064Q: live census of the ped proxies (the v1063P5 counters are cumulative event counts, not proxies). */
static unsigned s_n_nm_diag;   /* v1070D */
static void look_census(void)
{ look_dump();   /* v1065M1 */
    unsigned n = 0, full = 0, fpd = 0, mo = 0, diff = 0, hunk = 0, noext = 0, noinst = 0, hsaved = 0;
    unsigned nm_near = 0, nm_near_lod = 0, nm_near_never = 0, nm_near_noanim = 0, nm_mid = 0, nm_far = 0;   /* v1070D */
    float me[3] = {0, 0, 0}; const int have_me = gf_coop_local_pos(me);
    for (int i = 0; i < PX_MAX; i++) {
        proxy* p = &s_px[i]; uint8_t want[16], have[16]; uint32_t res = 0, fp = 0, inst = 0; int src = 0, e;
        if (p->state != PS_LIVE || !p->ent || p->kind == GF_WK_CAR) continue;
        n++;
        e = ext_of(p->netid, want, &src, &fp);
        if (!e) { noext++; continue; }
        if (e == 2) { hunk++; continue; }
        if (src == 2) hsaved++;
        {
            int st = 0;   /* 0 no local model, 1 differs, 2 model match (host had no instance), 3 full, 4 fp differs */
            __try {
                if (read_u32(p->ent + 0x64u, &inst) && ea_range(inst, 0x20) && model_res_of(p->ent, &res) && model_guid(res, have))
                    st = memcmp(want, have, 16) ? 1 : !fp ? 2 : inst_fp(p->ent) == fp ? 3 : 4;
            } __except (EXCEPTION_EXECUTE_HANDLER) { st = 0; }
            if (st == 0) noinst++; else if (st == 1) diff++; else if (st == 2) mo++; else if (st == 3) full++; else fpd++;
            if (st == 0 && have_me) {   /* v1070D: where are the model-less proxies? (the receiver's character LOD strips > 40 m) */
                const float* q = p->have_cur ? p->cur : p->tgt; const float dx = q[9] - me[0], dy = q[10] - me[1], dz = q[11] - me[2];
                const float d = sqrtf(dx * dx + dy * dy + dz * dz); uint32_t fl = 0, c4 = 0;
                read_u32(p->ent + 0x1818u, &fl); read_u32(p->ent + 0xC4u, &c4);
                if (d < 25.0f) { nm_near++; if (fl & 0x02000000u) nm_near_lod++; else nm_near_never++; if (!c4) nm_near_noanim++;
                    if (s_n_nm_diag < 8) { uint8_t sg[16]; uint32_t sres = 0; int res_ok = 0; memset(sg, 0, sizeof sg);
                        if (read_bytes(p->ent + 0x1C60u, sg, 16) && guid_valid(sg)) res_ok = mreg_lookup(0, sg, 0, &sres) ? 1 : 2;
                        s_n_nm_diag++; printf("[coop-proxy] v1070D NEAR proxy without model: net %u ent %08X %.1f m from us, +0x1818 %08X (LOD-stripped flag %s), anim ctl %08X, +0x1C60 %02X%02X%02X%02X.. (%s), exact template %d, alive %d s\n",
                            p->netid, p->ent, (double)d, fl, (fl & 0x02000000u) ? "SET" : "clear", c4, sg[0], sg[1], sg[2], sg[3],
                            res_ok == 1 ? "model registered here" : res_ok == 2 ? "model NOT registered here" : "no GUID", p->exact_tmpl, (int)((GetTickCount64() - p->spawn_ms) / 1000)); fflush(stdout); } }
                else if (d < 40.0f) nm_mid++; else nm_far++;
            }
        }
    }
    printf("[coop-proxy] v1064Q look census: live ped proxies %u | full match %u (model + instance fp), model match %u (instance fp differs), model match %u (host had no instance), DIFFERS %u, host model unknown %u, no ext %u, no local model %u | host GUIDs from saved +0x1C60 %u | pre-created GUID writes %u (host model not resident here %u, other template %u), no-model fixes: SetModel %u, re-created %u, waiting for host GUID %u | host ext: from instance %u, saved %u, none %u\n",
           n, full, fpd, mo, diff, hunk, noext, noinst, hsaved, s_n_preguid, s_n_preguid_nores, s_n_preguid_inexact, s_n_nm_setmodel, s_n_nm_recreate, s_n_nm_wait, s_n_ext_inst, s_n_ext_saved, s_n_ext_none);
    if (noinst) printf("[coop-proxy] v1070D no-model ped proxies by distance from us: < 25 m %u (LOD-stripped flag set %u, never had a model %u; no anim ctl %u) | 25-40 m %u | >= 40 m %u (= beyond the character LOD, expected)%s\n",
           nm_near, nm_near_lod, nm_near_never, nm_near_noanim, nm_mid, nm_far, have_me ? "" : " | no local position");
}

/* v1128F (agent F28): called from the lifted func_00483FE0 (apply-pending v1128F) when its handle allocation func_000104FC returned an
 * invalid handle (pool empty: NULL-0x40). Returns 1 = leave the slot empty and return (the null-write guard would otherwise turn the
 * game's crash into an endless loop in func_000135FC / func_00483958 = c28-1 joiner freeze). GF_HANDLE_NULL_GUARD=0 off. */
int gf_handle_guard(uint32_t h, uint32_t name, uint32_t slot)
{
    static int on = -1; static volatile long n;
    if (on < 0) { const char* e = getenv("GF_HANDLE_NULL_GUARD"); on = !(e && *e == '0'); }
    if (!on) return 0;
    { const long k = InterlockedIncrement(&n); if (k <= 20 || k % 1000 == 0) { printf("[hguard] v1128F handle alloc FAILED (h %08X, name %08X, slot %08X) -> slot left empty (%ld)\n", h, name, slot, k); fflush(stdout); } }
    return 1;
}
