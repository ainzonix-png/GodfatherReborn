/* gf_coop_promote.c -- v1126P (agent P): proxy -> native promotion (see gf_coop_promote.h for the RE summary).
 * gf_proxy_promote(ent, mat, health): place (vt+0x30 + vt+0x24 {x,y,z,2}, as the proxy spawn), health (+0x14C8), then register
 * the character for the game's per-frame updates exactly like the iMsgManagedEntityCreated handler does (vt+0x1C ->
 * func_0012D28C lists 2..5 + the class' brain list 6/7/8); the brain func_00241614 starts the HSM itself on its next tick.
 * Verified = the entity is in its brain list right after the call. Monitor (1 Hz, 30 s per promoted entity): lists, HSM state
 * (ent+0x10C), position drift, health, alive bit. Test: GF_PROMOTE_TEST=1 promotes the nearest ped proxy (<= GF_PROMOTE_TEST_R
 * 30 m) GF_PROMOTE_TEST_S (45) s after the first proxy drive, GF_PROMOTE_TEST_N (1) of them 20 s apart, and detaches it from the
 * proxy layer (gf_proxy_detach). GF_PROMOTE_WWATCH=1: arm the runtime store watch on its position 3 s later (needs PPU_WWATCH).
 * GF_PROMOTE_CENSUS=1 (test on: once at the promote time): update-list membership + HSM state of our natives vs. ped proxies.
 * Log prefix "[promote] v1126P". */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "recompiled/ppu_recomp.h"
#include "gf_coop_world.h"
#include "gf_coop_promote.h"
#include "gf_memq.h"

extern uint8_t* vm_base;
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
extern volatile uint32_t g_ww_live, g_ww_live_len; extern volatile long g_ww_live_gen; extern uint32_t g_ww_lo, g_ww_hi;   /* runtime store watch */

extern uint32_t gf_proxy_ent_of(uint16_t netid);        /* gf_coop_proxy.c */
extern uint32_t gf_proxy_detach(uint16_t netid);        /* gf_coop_proxy.c v1126M */
extern int      gf_proxy_is_adopted(uint16_t netid);    /* gf_coop_proxy.c v1126M */
extern int      gf_coop_local_pos(float out[3]);        /* gf_coop.c */
extern uint32_t gf_coop_puppet_entity(void);            /* gf_coop_puppet.c */

/* v1126P requester of a native (the factory's 4th argument), from gf_coop_worldre.c (agent M, requested); weak default 0 */
uint32_t gf_world_req_of_p0(uint32_t ent) { (void)ent; return 0; }
#pragma comment(linker, "/alternatename:gf_world_req_of=gf_world_req_of_p0")
extern uint32_t gf_world_req_of(uint32_t ent);
extern int gf_coop_world_host_peds(uint32_t* ents, uint16_t* ids, float (*pos)[3], int max);

typedef void (*gfn)(ppu_context*);
enum { GAME_TOC = 0x00B4BF60u };
enum { UPD_MGR_PTR = 0x101D9884u, MSG_CREATED = 0x101DBFD8u };

/* ---------------------------------------------------------------- guest memory + calls (as gf_coop_proxy.c) */
static int s_call_fail;
static int ea_range(uint32_t ea, size_t n) { return ea >= 0x10000u && ea < 0xF0000000u && n <= 0xF0000000u - ea; }
static int mem_access(uint32_t ea, size_t n, int write) { return gf_memq_access(vm_base, ea, n, write); }
static uint32_t rd32(uint32_t ea) { const uint8_t* p = vm_base + ea; return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void wr32(uint32_t ea, uint32_t v) { uint8_t* p = vm_base + ea; p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static int read_u32(uint32_t ea, uint32_t* out)
{
    if (!ea_range(ea, 4) || !mem_access(ea, 4, 0)) return 0;
    __try { *out = rd32(ea); return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int write_u32(uint32_t ea, uint32_t v)
{
    if (!ea_range(ea, 4) || !mem_access(ea, 4, 1)) return 0;
    __try { wr32(ea, v); return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int read_f(uint32_t ea, float* out) { uint32_t u; if (!read_u32(ea, &u)) return 0; memcpy(out, &u, 4); return 1; }
static gfn lookup(uint32_t addr)
{
    static uint32_t ca[32]; static gfn cf[32]; static int cn;
    gfn f = 0;
    for (int i = 0; i < cn; i++) if (ca[i] == addr) return cf[i];
    for (uint64_t i = 0; i < function_table_count; i++)
        if ((uint32_t)function_table[i].addr == addr) { f = function_table[i].func; break; }
    if (cn < 32) { ca[cn] = addr; cf[cn] = f; cn++; }
    if (!f) { printf("[promote] v1126P func_%08X is not in the function table\n", addr); fflush(stdout); }
    return f;
}
static uint64_t gcall_raw(gfn f, uint32_t toc, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    ppu_context* cur = g_active_ctx;
    static ppu_context c;
    if (!cur || !f) return 0;
    memcpy(&c, cur, sizeof c);
    c.gpr[1] = ((uint32_t)cur->gpr[1] - 0x1800u) & ~0xFull;   /* below the scratch block (sp - 0x800) */
    c.gpr[2] = toc;
    c.gpr[3] = a0; c.gpr[4] = a1; c.gpr[5] = a2; c.gpr[6] = a3;
    c.lr = 0;
    g_active_ctx = &c;
    f(&c);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&c); }
    g_active_ctx = cur;
    return c.gpr[3];
}
static uint64_t gcall_toc(uint32_t addr, uint32_t toc, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    ppu_context* cur = g_active_ctx; gfn f;
    if (!cur || !(f = lookup(addr))) { s_call_fail++; return 0; }
    __try { return gcall_raw(f, toc, a0, a1, a2, a3); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_active_ctx = cur; g_trampoline_fn = 0; s_call_fail++;
        printf("[promote] v1126P guest call %08X faulted\n", addr); fflush(stdout); return 0;
    }
}
static uint64_t gcall(uint32_t addr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) { return gcall_toc(addr, GAME_TOC, a0, a1, a2, a3); }
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
    if (!ea_range(ent, 0x1CC0u) || !read_u32(ent, &v) || !ea_range(v, 0x40u) || !v) return 0;
    if (!read_u32(ent + 0x68u, &x) || !ea_range(x, 0x50u) || !mem_access(x + 0x10u, 0x40, 0)) return 0;
    if (!read_u32(v + 0x2Cu, &opd) || !ea_range(opd, 8) || !mem_access(opd, 8, 0)) return 0;
    if (vt) *vt = v; if (xf) *xf = x;
    return 1;
}
static int vfn_of(uint32_t ent, uint32_t slot, uint32_t* fn, uint32_t* toc)
{
    uint32_t vt, opd;
    if (!read_u32(ent, &vt) || !ea_range(vt + slot, 4) || !read_u32(vt + slot, &opd) || !ea_range(opd, 8) ||
        !read_u32(opd, fn) || !read_u32(opd + 4, toc) || !*fn || !*toc) return 0;
    return 1;
}
static int call_virtual(uint32_t ent, uint32_t slot, uint32_t a1)
{
    uint32_t fn, toc; const int before = s_call_fail;
    if (!vfn_of(ent, slot, &fn, &toc) || !lookup(fn)) return 0;
    gcall_toc(fn, toc, ent, a1, 0, 0);
    return s_call_fail == before;
}
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
static int read_pos(uint32_t ent, float p[3])
{
    uint32_t xf;
    return read_u32(ent + 0x68u, &xf) && ea_range(xf, 0x50) && read_f(xf + 0x40u, &p[0]) && read_f(xf + 0x44u, &p[1]) && read_f(xf + 0x48u, &p[2]);
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

/* ---------------------------------------------------------------- update manager lists */
static uint32_t upd_lists(void)
{
    uint32_t mgr = 0, l = 0;
    if (!read_u32(UPD_MGR_PTR, &mgr) || !ea_range(mgr, 8) || !read_u32(mgr + 4u, &l) || !ea_range(l, 9 * 16)) return 0;
    return l;
}
static int list_has(uint32_t lists, int idx, uint32_t ent)
{
    uint32_t data = 0, n = 0, e = 0;
    if (!read_u32(lists + (uint32_t)idx * 16u, &data) || !read_u32(lists + (uint32_t)idx * 16u + 4u, &n)) return -1;
    if (!n) return 0;
    if (n > 4096 || !ea_range(data, n * 4u) || !mem_access(data, n * 4u, 0)) return -1;
    for (uint32_t i = 0; i < n; i++) if (read_u32(data + i * 4u, &e) && e == ent) return 1;
    return 0;
}
int gf_native_update_lists(uint32_t ent)
{
    const uint32_t l = upd_lists(); int m = 0;
    if (!l || !ent) return -1;
    __try { for (int i = 2; i <= 8; i++) { const int h = list_has(l, i, ent); if (h < 0) return -1; if (h) m |= 1 << i; } }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return m;
}
int gf_native_has_brain(uint32_t ent) { const int m = gf_native_update_lists(ent); return m > 0 && (m & 0x1C0) != 0; }
/* brain list of a character class from its vt+0x1C register function (0 = not a known character class) */
static int brain_list_of(uint32_t ent, uint32_t* reg_fn)
{
    uint32_t fn = 0, toc = 0;
    if (!vfn_of(ent, 0x1Cu, &fn, &toc)) return 0;
    if (reg_fn) *reg_fn = fn;
    if (fn == 0x001438B0u) return 6;   /* pedestrian 0x1CC0 (BA1F9E7B / 3E8BF7B3 / 5DA5581E) */
    if (fn == 0x0012A84Cu) return 7;   /* combat NPC 0x1F80 (BDA2CAC1 / D1812E99 / F48F270E) */
    if (fn == 0x0012AF68u) return 8;   /* A37F4AB4 0x1F90 */
    return 0;
}

/* ---------------------------------------------------------------- monitor */
enum { MON_MAX = 8 };
typedef struct { uint32_t ent, vt; ULONGLONG t0, next; float p0[3], plast[3]; float moved; uint32_t hsm0; int n, ww_done; uint16_t netid; } mon;
static mon s_mon[MON_MAX];
static int s_on = -1, s_mode = 0;   /* mode 0 lists, 1 created */
static unsigned st_ok, st_fail, st_already;
static int promote_on(void)
{
    if (s_on < 0) {
        const char* e = getenv("GF_PROMOTE"); s_on = !(e && *e == '0');
        e = getenv("GF_PROMOTE_MODE"); s_mode = e && (*e == 'c' || *e == 'C' || *e == '1') ? 1 : 0;
        printf("[promote] v1126P proxy -> native promotion %s (mode %s; GF_PROMOTE / GF_PROMOTE_MODE=lists|created)\n", s_on ? "ON" : "off", s_mode ? "created (iMsgManagedEntityCreated)" : "lists (vt+0x1C)"); fflush(stdout);
    }
    return s_on;
}
static void mon_add(uint32_t ent, uint16_t netid)
{
    mon* m = 0; uint32_t vt = 0;
    for (int i = 0; i < MON_MAX && !m; i++) if (!s_mon[i].ent || s_mon[i].ent == ent) m = &s_mon[i];
    if (!m) { int o = 0; for (int i = 1; i < MON_MAX; i++) if (s_mon[i].t0 < s_mon[o].t0) o = i; m = &s_mon[o]; }
    memset(m, 0, sizeof *m); read_u32(ent, &vt);
    m->ent = ent; m->vt = vt; m->netid = netid; m->t0 = GetTickCount64(); m->next = m->t0 + 250;
    read_pos(ent, m->p0); memcpy(m->plast, m->p0, sizeof m->p0); read_u32(ent + 0x10Cu, &m->hsm0);
}
static void mon_tick(ULONGLONG now)
{
    static int ww = -1;
    if (ww < 0) { const char* e = getenv("GF_PROMOTE_WWATCH"); ww = e && *e == '1'; }
    for (int i = 0; i < MON_MAX; i++) {
        mon* m = &s_mon[i]; uint32_t vt = 0, a34 = 0, hsm = 0, xf = 0; float p[3] = {0, 0, 0}, hp = 0; int lm;
        if (!m->ent || now < m->next) continue;
        m->next = now + (m->n < 4 ? 250 : 1000); m->n++;
        if (!read_u32(m->ent, &vt) || vt != m->vt) { printf("[promote] v1126P mon %08X: entity gone / reused (vt %08X) after %.1f s\n", m->ent, vt, (double)(now - m->t0) * 0.001); fflush(stdout); m->ent = 0; continue; }
        read_u32(m->ent + 0x34u, &a34); read_u32(m->ent + 0x10Cu, &hsm); read_f(m->ent + 0x14C8u, &hp); read_pos(m->ent, p);
        { float d = 0; for (int k = 0; k < 3; k++) { const float q = p[k] - m->plast[k]; d += q * q; } m->moved += sqrtf(d); memcpy(m->plast, p, sizeof p); }
        lm = gf_native_update_lists(m->ent);
        { const float dx = p[0] - m->p0[0], dz = p[2] - m->p0[2];
          printf("[promote] v1126P mon net %u ent %08X t+%.1f s: lists %03X brain %s, HSM state %08X (at promote %08X), alive %u, hp %.0f, pos %.2f %.2f %.2f, %.2f m from start, path %.2f m\n",
                 m->netid, m->ent, (double)(now - m->t0) * 0.001, lm < 0 ? 0xFFF : lm, lm > 0 && (lm & 0x1C0) ? "YES" : "no", hsm, m->hsm0, a34 & 1u, (double)hp,
                 (double)p[0], (double)p[1], (double)p[2], (double)sqrtf(dx * dx + dz * dz), (double)m->moved); fflush(stdout); }
        if (ww && !m->ww_done && now - m->t0 > 3000 && read_u32(m->ent + 0x68u, &xf) && ea_range(xf, 0x50)) {
            m->ww_done = 1;
            g_ww_live_len = 0x10; g_ww_lo = (xf + 0x40u) & ~15u; g_ww_hi = g_ww_lo + 0x10u; g_ww_live = xf + 0x40u; g_ww_live_gen++;
            printf("[promote] v1126P WWATCH armed on promoted ent %08X xf+0x40 %08X (stderr [ww] = writers; brain = func_00241614 chain)\n", m->ent, xf + 0x40u); fflush(stdout);
            fprintf(stderr, "[ww] ===== v1126P promoted ent %08X xf+0x40 = %08X =====\n", m->ent, xf + 0x40u); fflush(stderr);
        }
        if (now - m->t0 > 30000) { printf("[promote] v1126P mon %08X done: path %.2f m in 30 s\n", m->ent, (double)m->moved); fflush(stdout); m->ent = 0; }
    }
}

/* ---------------------------------------------------------------- requester link (GF_PROMOTE_LINK=1, experimental)
 * Street population ownership: requester slots req+0xEC (8 B: handle = ent+0x38, chained through handle+4), count req+0xE4;
 * append = func_00287414(req, ent, cull) (cull 0 = fail when full). Requester = the one of the nearest own native ped with a
 * street requester (vt 0x1004B5E8). Then the game's despawn sweep / over-budget cull own it like any street ped. */
static int s_link = -1;
static int link_requester(uint32_t ent)
{
    uint32_t ents[128]; uint16_t ids[128]; float pos[128][3], me[3]; int n = 0, best = -1; float bd = 1e18f; uint32_t req = 0, rvt = 0, c0 = 0, c1 = 0;
    if (s_link < 0) { const char* e = getenv("GF_PROMOTE_LINK"); s_link = e && *e == '1'; }
    if (!s_link) return 0;
    if (!read_pos(ent, me)) return 0;
    __try { n = gf_coop_world_host_peds(ents, ids, pos, 128); } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
    for (int i = 0; i < n; i++) {
        uint32_t r = gf_world_req_of(ents[i]), v = 0; float d;
        if (!r || ents[i] == ent || !read_u32(r, &v) || v != 0x1004B5E8u || brain_list_of(ents[i], 0) != 6) continue;
        d = (pos[i][0] - me[0]) * (pos[i][0] - me[0]) + (pos[i][2] - me[2]) * (pos[i][2] - me[2]);
        if (d < bd) { bd = d; best = i; req = r; }
    }
    if (best < 0) { printf("[promote] v1126P link ent %08X: no own street ped with a requester (%d natives, gf_world_req_of %s)\n", ent, n, gf_world_req_of == gf_world_req_of_p0 ? "missing" : "ok"); fflush(stdout); return 0; }
    read_u32(req, &rvt); read_u32(req + 0xE4u, &c0);
    { const int r = (int)gcall(0x00287414u, req, ent, 0, 0); read_u32(req + 0xE4u, &c1);
      printf("[promote] v1126P link ent %08X -> street requester %08X (vt %08X): %s, slots %u -> %u\n", ent, req, rvt, r == 1 ? "LINKED" : "refused (full?)", c0, c1); fflush(stdout);
      return r == 1; }
}

/* ---------------------------------------------------------------- API */
int gf_native_demote_ok(uint32_t ent)
{
    uint32_t a34 = 0; float hp = 0;
    if (!ent || ent == player() || ent == gf_coop_puppet_entity()) return 0;
    if (!entity_info(ent, 0, 0) || !brain_list_of(ent, 0)) return 0;
    if (!read_u32(ent + 0x34u, &a34) || !(a34 & 1u) || !read_f(ent + 0x14C8u, &hp) || !(hp > 0.0f)) return 0;
    return gf_native_has_brain(ent);
}
int gf_native_brain_off(uint32_t ent)
{
    uint32_t mgr = 0; int bl; const int before = s_call_fail;
    if (!g_active_ctx || !ent || !(bl = brain_list_of(ent, 0)) || !read_u32(UPD_MGR_PTR, &mgr) || !ea_range(mgr, 8)) return 0;
    if (!gf_native_has_brain(ent)) return 1;
    gcall(0x0034B58Cu, mgr, (uint64_t)bl, ent, 0);
    { const int ok = s_call_fail == before && !gf_native_has_brain(ent);
      printf("[promote] v1126P brain OFF ent %08X (list %d): %s\n", ent, bl, ok ? "removed" : "FAILED"); fflush(stdout); return ok; }
}
/* v1127W: undo a promotion before the entity is let go: vt+0x20 = unregister from every update list (P's RE: func_001436AC /
 * 0012A628 / 0012AE80). A promoted FACTORY proxy (owner = requester = 0) is not removed by the game's release (v1061G orphan bodies),
 * so without this it kept running its brain / anim as an unsynced ghost after gf_coop_world.c released it out of scope. 1 = not in
 * any list afterwards. Main PPU thread. */
int gf_proxy_unpromote(uint32_t ent)
{
    int lm0, lm1 = -1; const int before = s_call_fail;
    if (!promote_on() || !ent || !g_active_ctx) return 0;
    __try {
        if (ent == player() || ent == gf_coop_puppet_entity() || !entity_info(ent, 0, 0) || !brain_list_of(ent, 0)) return 0;
        lm0 = gf_native_update_lists(ent);
        if (lm0 <= 0) return lm0 == 0;
        call_virtual(ent, 0x20u, 0);
        lm1 = gf_native_update_lists(ent);
        printf("[promote] v1127W unpromote ent %08X: update lists %03X -> %03X%s\n", ent, lm0, lm1 < 0 ? 0xFFF : lm1, s_call_fail != before ? " (guest call faulted)" : ""); fflush(stdout);
        return s_call_fail == before && lm1 == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { printf("[promote] v1127W unpromote ent %08X: exception\n", ent); fflush(stdout); return 0; }
}
int gf_proxy_promote(uint32_t ent, const float* mat, float health)
{
    uint32_t vt = 0, xf = 0, reg_fn = 0, a34 = 0, buf, hsm = 0; int bl, lm0, lm1 = 0, before = s_call_fail; float hp0 = 0;
    if (!promote_on() || !ent || !g_active_ctx) return 0;
    __try {
        if (ent == player() || ent == gf_coop_puppet_entity() ) { printf("[promote] v1126P ent %08X refused: player / puppet\n", ent); fflush(stdout); st_fail++; return 0; }
        if (!entity_info(ent, &vt, &xf) || !(bl = brain_list_of(ent, &reg_fn))) {
            printf("[promote] v1126P ent %08X refused: not a known character class (vt %08X, vt+0x1C func %08X)\n", ent, vt, reg_fn); fflush(stdout); st_fail++; return 0; }
        if (!read_u32(ent + 0x34u, &a34) || !(a34 & 1u)) { printf("[promote] v1126P ent %08X refused: not alive (+0x34 %08X)\n", ent, a34); fflush(stdout); st_fail++; return 0; }
        if (!(buf = scratch())) { st_fail++; return 0; }
        lm0 = gf_native_update_lists(ent);
        if (lm0 < 0) { printf("[promote] v1126P ent %08X refused: update manager lists unreadable\n", ent); fflush(stdout); st_fail++; return 0; }
        read_f(ent + 0x14C8u, &hp0);
        if (mat) {   /* placement as the proxy spawn: vt+0x30 SetMatrix, vt+0x24 {x,y,z,2} (body / controller) */
            if (!write_matrix12(buf, xf, mat) || !call_virtual(ent, 0x30u, buf)) { printf("[promote] v1126P ent %08X: placement failed -> refused\n", ent); fflush(stdout); st_fail++; return 0; }
            { uint32_t u; const uint32_t blk = buf + 0x40u;
              memcpy(&u, &mat[9], 4); wr32(blk, u); memcpy(&u, &mat[10], 4); wr32(blk + 4, u); memcpy(&u, &mat[11], 4); wr32(blk + 8, u); wr32(blk + 12, 2u);
              call_virtual(ent, 0x24u, blk); }
        }
        if (health > 0.0f) { uint32_t u; memcpy(&u, &health, 4); write_u32(ent + 0x14C8u, u); }
        if (lm0 & (1 << bl)) {
            st_already++;
            printf("[promote] v1126P ent %08X already native (lists %03X, brain list %d) -> placed%s, ok\n", ent, lm0, bl, health > 0 ? " + health" : ""); fflush(stdout);
            mon_add(ent, 0); return 1;
        }
        before = s_call_fail;
        if (s_mode == 1) {   /* the game's full spawn-complete init (character handler func_00140FE4 loc_00141360) */
            uint32_t id = 0; read_u32(MSG_CREATED, &id);
            wr32(buf + 0x70, id); wr32(buf + 0x74, ent);
            gcall(0x00455A2Cu, buf + 0x70, ent + 0x30u, 0, 0);
        } else {
            call_virtual(ent, 0x1Cu, 0);   /* register: lists 2..5 + brain list */
        }
        lm1 = gf_native_update_lists(ent); read_u32(ent + 0x10Cu, &hsm);
        if (s_call_fail != before || lm1 < 0 || !(lm1 & (1 << bl))) {
            printf("[promote] v1126P ent %08X FAILED: lists %03X -> %03X (brain list %d)%s\n", ent, lm0, lm1, bl, s_call_fail != before ? ", guest call faulted" : ""); fflush(stdout);
            st_fail++; return 0;
        }
        st_ok++;
        printf("[promote] v1126P ent %08X vt %08X PROMOTED (%s): update lists %03X -> %03X (brain list %d), HSM %08X, hp %.0f -> %.0f%s | ok %u, failed %u\n", ent, vt,
               s_mode ? "created msg" : "vt+0x1C", lm0, lm1, bl, hsm, (double)hp0, (double)(health > 0 ? health : hp0), mat ? ", placed" : "", st_ok, st_fail); fflush(stdout);
        link_requester(ent);
        mon_add(ent, 0);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { printf("[promote] v1126P ent %08X: exception -> failed\n", ent); fflush(stdout); st_fail++; return 0; }
}

/* ---------------------------------------------------------------- test (GF_PROMOTE_TEST=1) + tick via the ped drive-info hook */
enum { SEEN_MAX = 128 };
static uint16_t s_seen_id[SEEN_MAX]; static float s_seen_m[SEEN_MAX][12]; static ULONGLONG s_seen_ms[SEEN_MAX]; static int s_seen_n;
static int s_test = -1, s_test_n = 1, s_test_done; static float s_test_r = 30.0f; static ULONGLONG s_test_t0, s_test_at, s_last_tick;
static void census(void)
{
    uint32_t ents[128]; uint16_t ids[128]; float pos[128][3]; int n = 0, nat = 0, natb = 0, px = 0, pxb = 0, pxl = 0;
    __try { n = gf_coop_world_host_peds(ents, ids, pos, 128); } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
    for (int i = 0; i < n; i++) { const int m = gf_native_update_lists(ents[i]); uint32_t h = 0; read_u32(ents[i] + 0x10Cu, &h); nat++; if (m > 0 && (m & 0x1C0)) natb++;
        if (i < 6) { printf("[promote] v1126P census native net %u ent %08X: lists %03X, HSM %08X, class list %d\n", ids[i], ents[i], m < 0 ? 0xFFF : m, h, brain_list_of(ents[i], 0)); } }
    for (int i = 0; i < s_seen_n; i++) { const uint32_t e = gf_proxy_ent_of(s_seen_id[i]); int m; uint32_t h = 0; if (!e) continue; m = gf_native_update_lists(e); read_u32(e + 0x10Cu, &h); px++;
        if (m > 0 && (m & 0x1C0)) pxb++; if (m > 0) pxl++;
        if (px <= 6) printf("[promote] v1126P census proxy net %u ent %08X: lists %03X, HSM %08X, class list %d, adopted %d\n", s_seen_id[i], e, m < 0 ? 0xFFF : m, h, brain_list_of(e, 0), gf_proxy_is_adopted(s_seen_id[i])); }
    printf("[promote] v1126P census: our natives %d (with brain %d) | ped proxies %d (in any update list %d, with brain %d)\n", nat, natb, px, pxl, pxb); fflush(stdout);
}
static void test_tick(ULONGLONG now)
{
    float me[3]; int best = -1; float bd = 1e9f;
    if (s_test < 0) { const char* e = getenv("GF_PROMOTE_TEST"); s_test = e && *e == '1';
        if ((e = getenv("GF_PROMOTE_TEST_N")) && atoi(e) > 0) s_test_n = atoi(e);
        if ((e = getenv("GF_PROMOTE_TEST_R")) && atof(e) > 0) s_test_r = (float)atof(e);
        { int s = 45; if ((e = getenv("GF_PROMOTE_TEST_S")) && atoi(e) >= 0) s = atoi(e); s_test_t0 = now; s_test_at = now + (ULONGLONG)s * 1000u; }
        if (s_test) { printf("[promote] v1126P TEST on: promote %d nearest ped prox%s (<= %.0f m) from t+%llu s\n", s_test_n, s_test_n == 1 ? "y" : "ies", (double)s_test_r, (s_test_at - s_test_t0) / 1000u); fflush(stdout); } }
    if (!s_test || s_test_done >= s_test_n || now < s_test_at) return;
    if (!gf_coop_local_pos(me)) return;
    if (s_test_done == 0) census();
    for (int i = 0; i < s_seen_n; i++) {
        const uint32_t e = gf_proxy_ent_of(s_seen_id[i]); float p[3], d;
        if (!e || now - s_seen_ms[i] > 1000 || gf_proxy_is_adopted(s_seen_id[i]) || !read_pos(e, p) || !brain_list_of(e, 0)) continue;
        d = (p[0] - me[0]) * (p[0] - me[0]) + (p[2] - me[2]) * (p[2] - me[2]);
        if (d < bd) { bd = d; best = i; }
    }
    s_test_at = now + 20000;
    if (best < 0 || bd > s_test_r * s_test_r) { printf("[promote] v1126P TEST: no ped proxy within %.0f m (nearest %.1f m, %d seen) -> retry in 20 s\n", (double)s_test_r, best < 0 ? -1.0 : (double)sqrtf(bd), s_seen_n); fflush(stdout); s_test_at = now + 5000; return; }
    {
        const uint16_t id = s_seen_id[best]; const uint32_t e = gf_proxy_ent_of(id); float hp = 0; int r; uint32_t d = 0;
        read_f(e + 0x14C8u, &hp);
        printf("[promote] v1126P TEST #%d: promoting proxy net %u ent %08X (%.1f m from us, hp %.0f)\n", s_test_done + 1, id, e, (double)sqrtf(bd), (double)hp); fflush(stdout);
        r = gf_proxy_promote(e, s_seen_m[best], hp > 0 ? hp : 0.0f);
        if (r == 1) { d = gf_proxy_detach(id); for (int i = 0; i < MON_MAX; i++) if (s_mon[i].ent == e) s_mon[i].netid = id; }
        printf("[promote] v1126P TEST #%d: net %u promote %s, detach %s\n", s_test_done + 1, id, r == 1 ? "OK" : "FAILED", r == 1 ? (d == e ? "OK" : "failed (proxy keeps driving it)") : "-"); fflush(stdout);
        s_test_done++;
    }
}
static void promote_tick(void)
{
    const ULONGLONG now = GetTickCount64();
    if (now == s_last_tick) return;   /* once per frame-ish (the hook runs per ped proxy) */
    s_last_tick = now;
    __try { mon_tick(now); test_tick(now); } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
/* v1126P: gf_coop_world.c calls this right before gf_proxy_drive for every ped proxy, every frame (guest side of any peer);
 * the header assigns the _ped hooks to agent P. Used as our tick + to know the live ped proxy ids. Main PPU thread. */
void gf_proxy_drive_info_ped(uint16_t netid, const gf_world_drive_info* di)
{
    int i;
    if (!netid || !di) return;
    for (i = 0; i < s_seen_n; i++) if (s_seen_id[i] == netid) break;
    if (i == s_seen_n) {
        if (s_seen_n < SEEN_MAX) s_seen_n++;
        else { int o = 0; for (int j = 1; j < SEEN_MAX; j++) if (s_seen_ms[j] < s_seen_ms[o]) o = j; i = o; }
    }
    s_seen_id[i] = netid; memcpy(s_seen_m[i], di->m, sizeof s_seen_m[i]); s_seen_ms[i] = GetTickCount64();
    if (promote_on()) promote_tick();
}
/* optional explicit tick (agent M may call it from gf_coop_world_tick; harmless twice per frame) */
void gf_promote_tick(void) { if (promote_on()) promote_tick(); }
