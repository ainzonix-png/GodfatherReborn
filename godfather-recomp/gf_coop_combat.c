/* gf_coop_combat.c -- v1062C co-op combat sync (Claude, agent C): weapons on the puppet, gunfire, damage.
 *
 * RE (EBOOT, 2026-11-05; details in HANDOFF.md "v1062C"):
 *   character vt+0x22C = SetItemInHand(ch, item): same slot on the player class (10048550) and the crew/ped class
 *       (100463A0, func_001388D8): holster the old item (vt+0x244 -> func_00234C04 detach, clear +0x5C4 0x200,
 *       item vt+0x68), link the weak ref ch+0x468 -> item+0x38, then vt+0x240 -> func_00234DCC = attach the item model to
 *       the hand (item vt+0x138(item, ch, TOC[-0x594], 0)) and set +0x5C4 |= 0x200. item = 0 -> holster only.
 *   weapon item (vtable 10043FA0): template GUID +0x1C4 (gf_trainer_weapons.txt "off 1C4"), +0xD9 type, +0xD4 1 = gun,
 *       +0x134 weak ref to the holder, +0x1EC clip, +0x1D4 ammo table (reserve), created like the trainer's give:
 *       func_0029B9F0(&guid, *(player+0x18), 0, 0, 1).
 *   weapon vt+0x164 = func_000A4284(item) = FIRE ONE SHOT: clip > 0 -> func_000A3D44 (aim = holder+0x5D4 when the
 *       holder's vt+0x214 says "has aim point": crew/ped class = bit 0x400000 of +0x1818, player = !(+0x5C4 & 0x400);
 *       bullet func_0029ACE8 / func_0029CCE0, clip--, muzzle fx / sound) -> func_000E9D24. clip 0 + no ammo table ->
 *       nothing happens, so the puppet's own crew AI cannot shoot with our weapon (clip is kept at 0).
 *   iMsgDamage (type object 0x101DB968, id at +0) payload = DamageData (0x3C, ctor func_002951CC, vtable 10045248):
 *       +4 weak ref attacker (handle = ent+0x38), +0xC / +0x18 vec3 positions, +0x24 float damage, +0x28 int type,
 *       +0x2C/+0x30/+0x34 ints, +0x38 flags. Characters get it in func_00140FE4 (ch, msg) -> ch vt+0x1C8 TakeDamage.
 *       The game's own sender (func_00296A00): operator new func_000283D4(0x3C), ctor, fill, then
 *       func_00473FD4(*(TOC+0xD00), TOC[0xD04] = 0x101DB968, target+0x30, 0, dmg, 0, delay f1); delay 0 = synchronous
 *       send (func_00455B98) and the queue deletes the payload afterwards (vt+4).
 *
 * What this does (all game calls on the main PPU thread, from gf_coop_puppet_tick / gf_coop_combat_msg):
 *   1. Weapon state: local item in hand (drawn flag, type, template GUID) -> reliable event 0x130 on change. The other side
 *      gives the puppet the same item through vt+0x22C (items cached per GUID) and holsters it with vt+0x22C(0).
 *   2. Gunfire: local clip drops -> event 0x131 {n shots, aim point = player+0x5D4}. The other side sets the puppet's aim
 *      point + aim bit, clip = 1, fires through func_000A4284 (the game's own shot: flash, sound, tracer, impact).
 *   3. Damage (hook gf_coop_combat_msg at the entry of func_00140FE4, _patches/apply-pending.ps1 block v1062C):
 *        - damage to the co-op puppet is swallowed (no friendly fire / no dying puppet); on the HOST, damage an NPC does to the
 *          guest's puppet is forwarded to the guest's real player (event 0x133);
 *        - damage DEALT by the puppet is swallowed (its shots are visual; the real damage comes from the shooter's game);
 *        - GUEST: damage to a world proxy is swallowed (host authoritative); if the local player caused it, it is
 *          forwarded (event 0x132 {netid, DamageData fields}) and the host sends the same DamageData to the real NPC with
 *          the attacker = the guest's puppet, through the game's own damage message (reaction / death / AI aggro).
 * Env: GF_COOP_COMBAT=0 off. GF_COOP_COMBAT_TEST=1 (echo self test: puppet draws a weapon after 6 s, fires every
 * 1.5 s at a point 15 m in front of it). GF_COOP_COMBAT_ECHO_HURT=1 (echo: forward puppet damage to yourself).
 * Log [coop-combat]. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "recompiled/ppu_recomp.h"
#include "gf_memq.h"

extern uint8_t* vm_base;
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
extern int gf_coop_connected(void);
extern int gf_coop_mode(void);                 /* 0 off, 1 host, 2 join, 3 echo */
extern uint32_t gf_coop_puppet_entity(void);
extern int gf_coop_send_event(uint16_t type, const void* data, int len);
#include "gf_coop_peers.h"   /* v1072X3: gf_coop_npeer_active, gf_coop_send_event_relay, peer handlers */
typedef void (*gf_coop_event_fn)(uint16_t type, const void* data, int len);
extern void gf_coop_set_event_handler(gf_coop_event_fn fn);
extern uint16_t gf_proxy_netid_of(uint32_t ent);                                         /* gf_coop_proxy.c (W) */
extern uint32_t gf_proxy_ent_of(uint16_t netid);                                         /* gf_coop_proxy.c, v1062C append */
extern int gf_coop_world_host_peds(uint32_t* ents, uint16_t* ids, float (*pos)[3], int max);   /* gf_coop_world.c (W) */
extern uint16_t gf_proxy_netid_any(uint32_t ent);                                        /* gf_coop_proxy.c, v1065G (ignores the grab hold) */

enum { GAME_TOC = 0x00B4BF60u, VT_WEAPON = 0x10043FA0u, MSG_DAMAGE = 0x101DB968u,
       EV_WEAPON = 0x0130, EV_SHOT = 0x0131, EV_HIT = 0x0132, EV_PHURT = 0x0133,
       EV_WEAPON_R = 0x0135, EV_SHOT_R = 0x0136 };   /* v1072X3: relayed copies for joiners that are not our direct peer */
typedef void (*gfn)(ppu_context*);

#pragma pack(push, 1)
typedef struct { uint8_t ver, drawn, type, isgun; uint8_t guid[16]; } ev_weapon;
typedef struct { uint8_t ver, n, type, flags; float aim[3], from[3]; } ev_shot;
typedef struct { uint8_t ver, pad; uint16_t netid; uint32_t dtype; float dmg; float p1[3], p2[3]; uint32_t x2c, x30, x34; } ev_hit;
#pragma pack(pop)

static int s_on = -1, s_test = 0, s_echo_hurt = 0;
static int s_fail;
static CRITICAL_SECTION s_cs; static int s_cs_ok;

/* ---------------------------------------------------------------- memory / guest calls */
static int ea_ok(uint32_t ea, uint32_t n) { return ea >= 0x10000u && ea < 0xF0000000u && n <= 0xF0000000u - ea; }
static int acc(uint32_t ea, uint32_t n, int w) { return ea_ok(ea, n) && gf_memq_access(vm_base, ea, n, w); }
static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint32_t rd32(uint32_t ea) { return be32(vm_base + ea); }
static void wr32(uint32_t ea, uint32_t v) { uint8_t* p = vm_base + ea; p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static float rdf(uint32_t ea) { uint32_t u = rd32(ea); float f; memcpy(&f, &u, 4); return f; }
static void wrf(uint32_t ea, float f) { uint32_t u; memcpy(&u, &f, 4); wr32(ea, u); }
static int r32(uint32_t ea, uint32_t* out) { if (!acc(ea, 4, 0)) return 0; __try { *out = rd32(ea); return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static uint32_t player(void)
{
    uint32_t a;
    if (!r32(0x10181290u, &a) || !ea_ok(a, 8)) return 0;
    if (!r32(a + 4, &a) || !ea_ok(a, 4)) return 0;
    if (!r32(a, &a) || a < 0x17FCu) return 0;
    a -= 0x17FCu; return ea_ok(a, 0x2000) ? a : 0;
}
static uint32_t weak(uint32_t ref) { uint32_t h; if (!r32(ref, &h) || h == 0 || h == 0x38u) return 0; return h - 0x38u; }
static gfn lookup(uint32_t addr)
{
    static uint32_t ca[32]; static gfn cf[32]; static int cn;
    for (int i = 0; i < cn; i++) if (ca[i] == addr) return cf[i];
    gfn f = 0;
    for (uint64_t i = 0; i < function_table_count; i++) if ((uint32_t)function_table[i].addr == addr) { f = function_table[i].func; break; }
    if (cn < 32) { ca[cn] = addr; cf[cn] = f; cn++; }
    if (!f) { printf("[coop-combat] func_%08X is not in the function table\n", addr); fflush(stdout); }
    return f;
}
static uint64_t gcall_toc(uint32_t addr, uint32_t toc, const uint64_t* a, int na, double f1)
{
    ppu_context* cur = g_active_ctx; gfn f;
    static ppu_context c[2]; static int depth = 0;   /* depth 1 = a call made from inside the message hook */
    if (!cur || depth >= 2 || !(f = lookup(addr))) return 0;
    ppu_context* x = &c[depth];
    __try {
        memcpy(x, cur, sizeof *x);
        x->gpr[1] = ((uint32_t)cur->gpr[1] - 0x1000u) & ~0xFull;
        x->gpr[2] = toc;
        for (int i = 0; i < 8; i++) x->gpr[3 + i] = i < na ? a[i] : 0;
        x->fpr[1] = f1;
        x->lr = 0;
        depth++; g_active_ctx = x;
        f(x);
        while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(x); }
        g_active_ctx = cur; depth--;
        return x->gpr[3];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_active_ctx = cur; g_trampoline_fn = 0; depth = 0; s_fail++;
        printf("[coop-combat] guest call %08X faulted\n", addr); fflush(stdout); return 0;
    }
}
static uint64_t gcall(uint32_t addr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{ const uint64_t a[5] = { a0, a1, a2, a3, a4 }; return gcall_toc(addr, GAME_TOC, a, 5, 0.0); }
static uint64_t vcall(uint32_t obj, uint32_t slot, uint64_t a1, uint64_t a2)
{
    uint32_t vt, opd, fn, toc;
    if (!r32(obj, &vt) || !r32(vt + slot, &opd) || !r32(opd, &fn) || !r32(opd + 4, &toc) || !fn) return 0;
    if (vt == 0x1005F6C8u) { static int lg; if (lg++ < 10) { printf("[coop-combat] v1127C refused vt+0x%X on DESTROYED object %08X (base vtable 1005F6C8)\n", slot, obj); fflush(stdout); } return 0; }   /* v1127C */
    const uint64_t a[3] = { obj, a1, a2 }; return gcall_toc(fn, toc, a, 3, 0.0);
}
static uint32_t scratch(uint32_t off)   /* guest stack area above our call frames (callee frames start 0x1000 below sp) */
{
    if (!g_active_ctx) return 0;
    const uint32_t ea = (((uint32_t)g_active_ctx->gpr[1] - 0x800u) & ~0xFu) + off;
    return acc(ea, 0x40, 1) ? ea : 0;
}
static int pos_of(uint32_t ent, float out[3])
{
    uint32_t xf; if (!r32(ent + 0x68u, &xf) || !acc(xf + 0x10u, 0x40, 0)) return 0;
    __try { out[0] = rdf(xf + 0x40); out[1] = rdf(xf + 0x44); out[2] = rdf(xf + 0x48); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return out[0] == out[0];
}
static int pos_sane(const float p[3]) { for (int k = 0; k < 3; k++) if (!(p[k] == p[k]) || p[k] > 1e5f || p[k] < -1e5f) return 0; return 1; }   /* v1127C */
static int fwd_of(uint32_t ent, float out[3])
{
    uint32_t xf; if (!r32(ent + 0x68u, &xf) || !acc(xf + 0x10u, 0x40, 0)) return 0;
    __try { out[0] = rdf(xf + 0x30); out[1] = rdf(xf + 0x34); out[2] = rdf(xf + 0x38); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}

/* ---------------------------------------------------------------- local weapon state (sender) */
static ev_weapon s_tx_w; static int s_tx_valid; static ULONGLONG s_tx_at;
static uint32_t s_tx_item; static int32_t s_tx_clip; static int s_was_conn;
static long st_w_tx, st_shot_tx, st_shots_tx, st_hit_tx, st_phurt_tx;
static int local_weapon(uint32_t pl, ev_weapon* w, uint32_t* item_out)
{
    memset(w, 0, sizeof *w); w->ver = 1; *item_out = 0;
    uint32_t fl, h; if (!r32(pl + 0x5C4u, &fl) || !r32(pl + 0x468u, &h)) return 0;
    if (h && h != 0x38u) {
        const uint32_t it = h - 0x38u; uint32_t vt;
        if (r32(it, &vt) && acc(it, 0x1E0, 0)) {
            __try { w->type = vm_base[it + 0xD9]; w->isgun = vm_base[it + 0xD4]; memcpy(w->guid, vm_base + it + 0x1C4, 16); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
            *item_out = it;
            w->drawn = (fl & 0x200u) ? 1 : 0;
        }
    }
    return 1;
}
static void sender_tick(ULONGLONG now)
{
    const int conn = gf_coop_connected();
    if (!conn) { s_was_conn = 0; return; }
    const uint32_t pl = player(); if (!pl) return;
    ev_weapon w; uint32_t item;
    if (!local_weapon(pl, &w, &item)) return;
    if (!s_was_conn || !s_tx_valid || memcmp(&w, &s_tx_w, sizeof w) || now - s_tx_at > 5000) {
        const int changed = !s_tx_valid || memcmp(&w, &s_tx_w, sizeof w);
        if (gf_coop_send_event(EV_WEAPON, &w, sizeof w)) {
            if (gf_coop_mode() == 2 && gf_coop_npeer_active()) gf_coop_send_event_relay(EV_WEAPON_R, &w, sizeof w);   /* v1072X3 */
            if (changed) { printf("[coop-combat] local weapon: %s type 0x%02X gun %d item %08X guid %08X%08X..\n", w.drawn ? "DRAWN" : "holstered",
                                  w.type, w.isgun, item, be32(w.guid), be32(w.guid + 4)); fflush(stdout); }
            s_tx_w = w; s_tx_valid = 1; s_tx_at = now; s_was_conn = 1; st_w_tx++;
        }
    }
    /* shots: the clip of the same item in hand went down */
    int32_t clip = 0;
    if (item && acc(item + 0x1EC, 4, 0)) { __try { clip = (int32_t)rd32(item + 0x1EC); } __except (EXCEPTION_EXECUTE_HANDLER) { clip = 0; } }
    if (item && item == s_tx_item && w.isgun == 1 && clip < s_tx_clip) {   /* v1127C: no 'drawn' (+0x5C4 0x200) test -- a gun given + put in hand by
                                                                           * vt+0x22C fires without that bit (c27-2: B's shots were never sent) */
        int n = s_tx_clip - clip; if (n > 8) n = 8;
        ev_shot s; memset(&s, 0, sizeof s); s.ver = 1; s.n = (uint8_t)n; s.type = w.type;
        float me[3] = { 0, 0, 0 }; pos_of(pl, me); memcpy(s.from, me, sizeof me);
        uint32_t au = 0; int aim_ok = 0;
        if (acc(pl + 0x5D4u, 12, 0)) __try { s.aim[0] = rdf(pl + 0x5D4); s.aim[1] = rdf(pl + 0x5D8); s.aim[2] = rdf(pl + 0x5DC); au = 1; } __except (EXCEPTION_EXECUTE_HANDLER) { au = 0; }
        if (au) { const float dx = s.aim[0] - me[0], dy = s.aim[1] - me[1], dz = s.aim[2] - me[2], d2 = dx*dx + dy*dy + dz*dz;
                  aim_ok = d2 == d2 && d2 > 0.25f && d2 < 400.0f * 400.0f; }
        if (!aim_ok) { float f[3] = { 0, 0, 1 }; fwd_of(pl, f); for (int k = 0; k < 3; k++) s.aim[k] = me[k] + f[k] * 30.0f; s.aim[1] += 1.3f; s.flags |= 1; }
        if (gf_coop_send_event(EV_SHOT, &s, sizeof s)) { st_shot_tx++; st_shots_tx += n; }
        if (gf_coop_mode() == 2 && gf_coop_npeer_active()) gf_coop_send_event_relay(EV_SHOT_R, &s, sizeof s);   /* v1072X3 */
        static int logs; if (logs < 12) { logs++; printf("[coop-combat] local shot x%d type 0x%02X clip %d -> %d, aim %.2f %.2f %.2f (%s), me %.2f %.2f %.2f\n", n, w.type, s_tx_clip, clip,
            (double)s.aim[0], (double)s.aim[1], (double)s.aim[2], aim_ok ? "player+0x5D4" : "fallback forward", (double)me[0], (double)me[1], (double)me[2]); fflush(stdout); }
    }
    s_tx_item = item; s_tx_clip = clip;
}

/* ---------------------------------------------------------------- partner weapon on the puppet (receiver) */
typedef struct { uint8_t guid[16]; uint32_t item, vt; } witem;
static witem s_items[8]; static int s_nitems;
static ev_weapon s_rx_w; static int s_rx_have;    /* guarded by s_cs */
typedef struct { ev_shot s; } shotq; static shotq s_shots[32]; static int s_shot_h, s_shot_t;   /* guarded by s_cs */
static ev_hit s_hits[32]; static uint16_t s_hit_type[32]; static int s_hit_h, s_hit_t;          /* guarded by s_cs */
static int s_hit_org[32];   /* v1072X3: origin peer of a queued hit (-1 = primary / legacy) */
extern uint32_t gf_coop_peer_puppet(int peer_id);
static uint32_t s_eq_pup, s_eq_item; static int s_eq_drawn; static ULONGLONG s_eq_check; static uint8_t s_eq_guid[16];
static long st_equip, st_holster, st_reassert, st_fired, st_fire_real, st_fire_skip, st_create, st_create_fail;
static int guid_set(const uint8_t g[16]) { for (int i = 0; i < 16; i++) if (g[i]) return 1; return 0; }
static int vt_plausible(uint32_t vt) { return vt >= 0x10040000u && vt < 0x10300000u; }
static int item_alive(uint32_t it, uint32_t want_vt) { uint32_t vt; return it && r32(it, &vt) && vt_plausible(vt) && (!want_vt || vt == want_vt) && acc(it, 0x200, 1); }
static uint32_t get_item(const uint8_t guid[16], uint32_t pup)
{
    for (int i = 0; i < s_nitems; i++) if (!memcmp(s_items[i].guid, guid, 16)) {
        if (item_alive(s_items[i].item, s_items[i].vt)) return s_items[i].item;
        s_items[i] = s_items[--s_nitems]; break; }
    {   extern uint32_t create_item_v72(const uint8_t guid[16]); const uint32_t it = create_item_v72(guid); if (!it) return 0;
        if (s_nitems == 8) s_nitems--;   /* drop the last; the game keeps the old item (no safe free) */
        memcpy(s_items[s_nitems].guid, guid, 16); s_items[s_nitems].item = it; s_items[s_nitems].vt = rd32(it); s_nitems++;
        return it; }
}
uint32_t create_item_v72(const uint8_t guid[16])   /* v1072X3: creation part of get_item (extra puppets keep their own items) */
{
    const uint32_t pl = player(), buf = scratch(0); uint32_t owner;
    if (!pl || !buf || !r32(pl + 0x18u, &owner) || !owner) return 0;
    __try { memcpy(vm_base + buf, guid, 16); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    const uint32_t reg = rd32(0x10181088u);
    if (!ea_ok(reg, 4) || !(uint32_t)gcall(0x004AD7F0u, reg, buf, 0, 0, 0)) { st_create_fail++; printf("[coop-combat] weapon template %08X%08X.. unknown to the registry\n", be32(guid), be32(guid + 4)); fflush(stdout); return 0; }
    const uint32_t it = (uint32_t)gcall(0x0029B9F0u, buf, owner, 0, 0, 1);
    if (!item_alive(it, 0)) { st_create_fail++; printf("[coop-combat] creating weapon %08X%08X.. failed (got %08X)\n", be32(guid), be32(guid + 4), it); fflush(stdout); return 0; }
    gcall(0x000E8EC4u, it, 1, 0, 0, 0);   /* same flag the trainer's give leaves set (+0xF0 |= 0x40) */
    const uint32_t ivt = rd32(it);
    __try { printf("[coop-combat] created weapon item %08X vt %08X%s type 0x%02X gun %d clip %d ammo table %08X holder %08X\n", it, ivt, ivt == VT_WEAPON ? "" : " (not the gun class)",
                   vm_base[it + 0xD9], vm_base[it + 0xD4], (int)rd32(it + 0x1EC), rd32(it + 0x1D4), weak(it + 0x134)); if (ivt == VT_WEAPON) wr32(it + 0x1EC, 0); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    fflush(stdout); st_create++;
    return it;
}
/* v1062C-2: the weapon's holder weak ref item+0x134 is set by the inventory add (the player), NOT by vt+0x22C; fire
 * (func_000A4284) returns at once without it (c2-2-echo: "item holder 00000000", 22 shots did nothing). Link it to the
 * puppet's handle exactly like the game links weak refs: [ref] = handle, [ref+4] = old head, [handle+4] = ref. */
static void link_holder(uint32_t it, uint32_t pup)
{
    const uint32_t ref = it + 0x134u, h = pup + 0x38u; uint32_t old;
    if (!r32(ref, &old) || old == h || !acc(h, 8, 1)) return;
    if (old) gcall(0x00405360u, old, ref, 0, 0, 0);   /* unlink from the previous holder's list */
    __try { wr32(ref, h); wr32(ref + 4, rd32(h + 4)); wr32(h + 4, ref); } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
static void set_in_hand(uint32_t pup, uint32_t it)
{
    if (it) link_holder(it, pup);
    vcall(pup, 0x22Cu, it, 0);
}
static int ent_destroyed(uint32_t e) { uint32_t v = 0; return e && (!r32(e, &v) || v == 0x1005F6C8u); }   /* v1127C */
static void receiver_tick(ULONGLONG now)
{
    const uint32_t pup = ent_destroyed(gf_coop_puppet_entity()) ? 0 : gf_coop_puppet_entity();
    ev_weapon w; int have = 0;
    if (s_cs_ok) { EnterCriticalSection(&s_cs); w = s_rx_w; have = s_rx_have; LeaveCriticalSection(&s_cs); }
    if (s_test && pup) {   /* self test: draw the first known gun 6 s after the puppet appeared */
        static uint32_t tp; static ULONGLONG t0; static uint8_t tg[16]; static int tg_ok = -1;
        if (tg_ok < 0) { tg_ok = 0; FILE* f = fopen("gf_trainer_weapons.txt", "r"); char line[256];
            while (f && fgets(line, sizeof line, f)) { unsigned t; char gs[64]; if (sscanf(line, "%X %40s", &t, gs) == 2 && strlen(gs) >= 32 && (t == 0x16 || !tg_ok)) {
                for (int j = 0; j < 16; j++) { unsigned v; sscanf(gs + 2*j, "%2X", &v); tg[j] = (uint8_t)v; } tg_ok = 1; if (t == 0x16) break; } }
            if (f) fclose(f); printf("[coop-combat] self test: weapon template %s\n", tg_ok ? "loaded" : "MISSING (gf_trainer_weapons.txt)"); fflush(stdout); }
        if (pup != tp) { tp = pup; t0 = now; }
        if (tg_ok && now - t0 > 6000) { memset(&w, 0, sizeof w); w.ver = 1; w.drawn = 1; w.isgun = 1; w.type = 0x16; memcpy(w.guid, tg, 16); have = 1;
            static ULONGLONG nf; static int tms = -1; if (tms < 0) { const char* e = getenv("GF_COOP_COMBAT_TEST_MS"); tms = e && atoi(e) > 50 ? atoi(e) : 1500; }
            if (now >= nf && s_eq_item && s_eq_pup == pup) { nf = now + (ULONGLONG)tms; float p[3], f[3] = { 0, 0, 1 }, me[3]; const uint32_t pl = player();
                if (pos_of(pup, p)) { ev_shot s; memset(&s, 0, sizeof s); s.ver = 1; s.n = 1; s.type = 0x16;
                    /* v1062C-3: aim 4 m in front of the LOCAL player (screen centre), so the tracer crosses the view */
                    if (pl && pos_of(pl, me)) { fwd_of(pl, f); for (int k = 0; k < 3; k++) s.aim[k] = me[k] + f[k] * 4.0f; }
                    else { fwd_of(pup, f); for (int k = 0; k < 3; k++) s.aim[k] = p[k] + f[k] * 15.0f; }
                    for (int k = 0; k < 3; k++) s.from[k] = p[k]; s.aim[1] += 1.2f;
                    EnterCriticalSection(&s_cs); if (((s_shot_h + 1) & 31) != s_shot_t) { s_shots[s_shot_h].s = s; s_shot_h = (s_shot_h + 1) & 31; } LeaveCriticalSection(&s_cs); } } }
    }
    if (!pup) { s_eq_pup = 0; s_eq_item = 0; s_eq_drawn = 0; return; }
    if (pup != s_eq_pup) { s_eq_pup = pup; s_eq_item = 0; s_eq_drawn = 0; s_eq_check = 0; }
    const int want = have && w.drawn && guid_set(w.guid);
    uint32_t cur = 0; { uint32_t h; if (r32(pup + 0x468u, &h) && h && h != 0x38u) cur = h - 0x38u; }
    if (want) {
        uint32_t it = s_eq_item;
        if (!it || !item_alive(it, 0) || memcmp(s_eq_guid, w.guid, 16)) { it = get_item(w.guid, pup); s_eq_item = it; memcpy(s_eq_guid, w.guid, 16); }
        if (it && (cur != it || !s_eq_drawn)) {
            if (now >= s_eq_check) {
                if (s_eq_drawn && cur != it) st_reassert++;
                set_in_hand(pup, it); st_equip++;
                uint32_t h = 0, fl = 0; r32(pup + 0x468u, &h); r32(pup + 0x5C4u, &fl);
                static int logs; if (logs < 16) { logs++; printf("[coop-combat] puppet %08X takes item %08X (type 0x%02X): +0x468 %08X, +0x5C4 %08X, item holder %08X\n",
                    pup, it, w.type, h, fl, weak(it + 0x134)); fflush(stdout); }
                s_eq_drawn = 1; s_eq_check = now + 400;
            }
        }
    } else if ((s_eq_drawn || (cur && have)) && now >= s_eq_check) {
        if (cur) { set_in_hand(pup, 0); st_holster++; static int logs; if (logs < 16) { logs++; printf("[coop-combat] puppet %08X holsters (item was %08X)\n", pup, cur); fflush(stdout); } }
        s_eq_drawn = 0; s_eq_check = now + 1000;
    }
    /* queued shots */
    for (int guard = 0; guard < 8; guard++) {
        ev_shot s; int got = 0;
        EnterCriticalSection(&s_cs); if (s_shot_t != s_shot_h) { s = s_shots[s_shot_t].s; s_shot_t = (s_shot_t + 1) & 31; got = 1; } LeaveCriticalSection(&s_cs);
        if (!got) break;
        const uint32_t it = s_eq_item;
        if (!it || cur != it || rd32(it) != VT_WEAPON) { st_fire_skip++; continue; }
        if (weak(it + 0x134) != pup) link_holder(it, pup);
        uint32_t f1818 = 0; if (!r32(pup + 0x1818u, &f1818) || !acc(pup + 0x5D4u, 12, 1) || !acc(it + 0x1EC, 4, 1)) { st_fire_skip++; continue; }
        for (int k = 0; k < (int)s.n && k < 8; k++) {
            __try { wrf(pup + 0x5D4, s.aim[0]); wrf(pup + 0x5D8, s.aim[1]); wrf(pup + 0x5DC, s.aim[2]); wr32(pup + 0x1818, f1818 | 0x400000u); wr32(it + 0x1EC, 1); }
            __except (EXCEPTION_EXECUTE_HANDLER) { break; }
            gcall(0x000A4284u, it, 0, 0, 0, 0); st_fired++;
            __try { if (rd32(it + 0x1EC) == 0) st_fire_real++; wr32(it + 0x1EC, 0); } __except (EXCEPTION_EXECUTE_HANDLER) { }   /* clip 1 -> 0 = the game's shot ran */
        }
        __try { wr32(pup + 0x1818, (rd32(pup + 0x1818) & ~0x400000u) | (f1818 & 0x400000u)); } __except (EXCEPTION_EXECUTE_HANDLER) { }
        static int logs; if (logs < 10) { logs++; float p[3] = { 0, 0, 0 }; pos_of(pup, p); printf("[coop-combat] puppet fired x%d at %.2f %.2f %.2f (puppet at %.2f %.2f %.2f, shooter at %.2f %.2f %.2f)%s\n", s.n,
            (double)s.aim[0], (double)s.aim[1], (double)s.aim[2], (double)p[0], (double)p[1], (double)p[2], (double)s.from[0], (double)s.from[1], (double)s.from[2], (s.flags & 1) ? " [fallback aim]" : ""); fflush(stdout); }
    }
}

/* ---------------------------------------------------------------- damage (hook + apply) */
static volatile int s_applying;
static long st_dmg_seen, st_sw_puppet, st_sw_bypuppet, st_sw_proxy, st_hit_rx, st_hit_applied, st_hit_nonpc, st_phurt_rx, st_phurt_applied;
static int is_host(int m) { return m == 1 || m == 3; }
static int is_guest(int m) { return m == 2 || m == 3; }
/* v1068R zone spawn authority: the joiner owns its street life -> damage flows both ways. Host: hits on a proxy of a joiner
 * NPC are swallowed + forwarded (EV_HIT) and applied by the joiner to its native; joiner: its natives hurting the host's
 * puppet are forwarded (EV_PHURT) to the host's player. GF_COOP_COMBAT_TWOWAY=0 = old one-way rules. */
extern int gf_coop_world_guest_auth(void);
static int s_twoway = -1;
static int twoway(void) { if (s_twoway < 0) { const char* e = getenv("GF_COOP_COMBAT_TWOWAY"); s_twoway = !(e && *e == '0'); }
    int ga = 0; __try { ga = gf_coop_world_guest_auth(); } __except (EXCEPTION_EXECUTE_HANDLER) { ga = 0; } return s_twoway && ga; }
static int owns_world(int m) { return is_host(m) || (m == 2 && twoway()); }    /* applies EV_HIT to its own natives */
static int shows_world(int m) { return is_guest(m) || (m == 1 && twoway()); }  /* swallows damage to proxies, forwards own hits */
static void read_dmg(uint32_t d, ev_hit* h)
{
    memset(h, 0, sizeof *h); h->ver = 1;
    h->p1[0] = rdf(d + 0xC); h->p1[1] = rdf(d + 0x10); h->p1[2] = rdf(d + 0x14);
    h->p2[0] = rdf(d + 0x18); h->p2[1] = rdf(d + 0x1C); h->p2[2] = rdf(d + 0x20);
    h->dmg = rdf(d + 0x24); h->dtype = rd32(d + 0x28); h->x2c = rd32(d + 0x2C); h->x30 = rd32(d + 0x30); h->x34 = rd32(d + 0x34);
}
static uint16_t host_netid_of(uint32_t ent)
{
    static uint32_t ents[256]; static uint16_t ids[256]; static float pos[256][3];
    const int n = gf_coop_world_host_peds(ents, ids, pos, 256);
    for (int i = 0; i < n; i++) if (ents[i] == ent) return ids[i];
    return 0;
}
static uint32_t host_ent_of(uint16_t id)
{
    static uint32_t ents[256]; static uint16_t ids[256]; static float pos[256][3];
    const int n = gf_coop_world_host_peds(ents, ids, pos, 256);
    for (int i = 0; i < n; i++) if (ids[i] == id) return ents[i];
    return 0;
}
static volatile int s_gt_on; static void grab_note(uint32_t ent, uint32_t msg);   /* v1065G (grab section below) */
static int s_hold_on; static volatile uint32_t s_id_paired; static void pair_note(uint32_t ent, uint32_t msg);   /* v1065G */
/* ---- v1127C combat control: attacking / grabbing a partner-owned NPC asks its owner for control (gf_coop_world.c, only with
 * GF_COOP_MIGRATE=1; -1 = n/a otherwise). Queued from the hook (any thread), sent from the main thread. GF_COOP_COMBAT_REQCTL=0 off. */
extern int gf_world_request_control(uint16_t netid);
static volatile uint16_t s_rc_q[16]; static volatile long s_rc_h, s_rc_t; static long st_rc_req, st_rc_mine, st_rc_na;
static long st_rc_drop;
static void rc_push(uint16_t nid)   /* v1127G6: under s_cs (several hook threads may push), full ring counted */
{ if (s_cs_ok) EnterCriticalSection(&s_cs); { const long h = s_rc_h; if (h - s_rc_t >= 16) st_rc_drop++; else { s_rc_q[h & 15] = nid; s_rc_h = h + 1; } } if (s_cs_ok) LeaveCriticalSection(&s_cs); }
static void rc_tick(void)   /* main thread */
{
    static int on = -1; if (on < 0) { const char* e = getenv("GF_COOP_COMBAT_REQCTL"); on = !(e && *e == '0'); }
    static uint16_t last; static ULONGLONG last_t;
    while (s_rc_t != s_rc_h) { const uint16_t nid = s_rc_q[s_rc_t & 15]; s_rc_t++;
        if (!on || !nid) continue; const ULONGLONG now = GetTickCount64();
        if (nid == last && now - last_t < 1000) continue; last = nid; last_t = now;
        int r = -1; __try { r = gf_world_request_control(nid); } __except (EXCEPTION_EXECUTE_HANDLER) { r = -1; }
        if (r == 0) st_rc_req++; else if (r == 1) st_rc_mine++; else st_rc_na++;
        static int lg; if (r >= 0 && lg++ < 30) { printf("[coop-combat] v1127C combat control: net %u -> %s\n", nid, r == 1 ? "already ours" : "asked the owner for control"); fflush(stdout); } }
}
/* hook: entry of func_00140FE4(ent, msg) (character message handler). 1 = swallow the message. Any PPU thread. */
static volatile long st_apply_seen;
int gf_coop_combat_msg(uint32_t ent, uint32_t msg)
{
    if (s_gt_on) grab_note(ent, msg);   /* v1065G test window */
    if (s_hold_on == 1 && s_id_paired) __try { pair_note(ent, msg); } __except (EXCEPTION_EXECUTE_HANDLER) { }   /* v1065G grab detection */
    if (s_applying) { st_apply_seen++; return 0; }
    if (s_on != 1 || !gf_coop_connected()) return 0;
    const int m = gf_coop_mode(); if (!m) return 0;
    __try {
        if (!acc(msg, 8, 0) || rd32(msg) != rd32(MSG_DAMAGE)) return 0;
        const uint32_t d = rd32(msg + 4); if (!acc(d, 0x3C, 0)) return 0;
        st_dmg_seen++;
        const uint32_t att = weak(d + 4), pup = gf_coop_puppet_entity(), me = player();
        static int logs; if (logs < 24) { logs++; ev_hit h; read_dmg(d, &h);
            printf("[coop-combat] iMsgDamage to %08X from %08X (me %08X, puppet %08X, proxy %u): dmg %.2f type %u vt %08X x2c %08X x30 %08X x34 %08X flags %08X p1 %.1f %.1f %.1f\n",
                   ent, att, me, pup, (unsigned)gf_proxy_netid_any(ent), (double)h.dmg, h.dtype, rd32(d), h.x2c, h.x30, h.x34, rd32(d + 0x38), (double)h.p1[0], (double)h.p1[1], (double)h.p1[2]); fflush(stdout); }
        if (pup && ent == pup) {
            st_sw_puppet++;
            if ((m == 1 || (m == 3 && s_echo_hurt) || (m == 2 && twoway())) && att && att != me && att != pup) {
                ev_hit h; read_dmg(d, &h); h.netid = host_netid_of(att);
                if ((m != 2 || h.netid) && gf_coop_send_event(EV_PHURT, &h, sizeof h)) st_phurt_tx++;   /* v1068R joiner: only its OWN natives (proxies are the host's business) */
            }
            return 1;
        }
        if (pup && att == pup) { st_sw_bypuppet++; return 1; }
        if (shows_world(m)) {   /* v1068R: the host too, for proxies of the joiner's NPCs */
            const uint16_t nid = gf_proxy_netid_any(ent);   /* v1065G: also while grab-held */
            if (nid) {
                st_sw_proxy++;
                if (att && att == me) { ev_hit h; read_dmg(d, &h); h.netid = nid; if (gf_coop_send_event(EV_HIT, &h, sizeof h)) st_hit_tx++; rc_push(nid); }   /* v1127C: + ask for control */
                return 1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 0;
}
static int send_damage(uint32_t target, uint32_t attacker, const ev_hit* h)
{
    const uint32_t d = (uint32_t)gcall(0x000283D4u, 0x3C, 0, 0, 0, 0);
    if (!acc(d, 0x3C, 1)) return 0;
    gcall(0x002951CCu, d, 0, 0, 0, 0);
    __try {
        wrf(d + 0xC, h->p1[0]); wrf(d + 0x10, h->p1[1]); wrf(d + 0x14, h->p1[2]);
        wrf(d + 0x18, h->p2[0]); wrf(d + 0x1C, h->p2[1]); wrf(d + 0x20, h->p2[2]);
        wrf(d + 0x24, h->dmg); wr32(d + 0x28, h->dtype);
        /* +0x2C..+0x34: copied only when they are small values (pointers would be the sender's memory) */
        if (h->x2c < 0x10000u) wr32(d + 0x2C, h->x2c);
        if (h->x30 < 0x10000u) wr32(d + 0x30, h->x30);
        if (h->x34 < 0x10000u) wr32(d + 0x34, h->x34);
        if (attacker) { const uint32_t hd = attacker + 0x38u; wr32(d + 4, hd); wr32(d + 8, rd32(hd + 4)); wr32(hd + 4, d + 4); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    const uint32_t q = rd32(rd32(GAME_TOC + 0xD00u)), ty = rd32(GAME_TOC + 0xD04u);
    const uint64_t a[6] = { q, ty, target + 0x30u, 0, d, 0 };
    s_applying = 1; gcall_toc(0x00473FD4u, GAME_TOC, a, 6, 0.0); s_applying = 0;
    return 1;
}
static int s_test_att = 0;   /* self test: 0 = puppet, 1 = local player, 2 = none */
static void damage_selftest(void)   /* GF_COOP_COMBAT_TEST=1: every 5 s the host half of a forwarded hit on a nearby synced ped */
{
    static ULONGLONG next; static uint32_t last_npc; static float last_hp; static int round; const ULONGLONG now = GetTickCount64();
    if (!is_host(gf_coop_mode()) || now < next) return;
    next = now + 5000;
    if (last_npc) { uint32_t u = 0; r32(last_npc + 0x14C8u, &u); float f; memcpy(&f, &u, 4);
        printf("[coop-combat] self test: npc %08X health %.2f -> %.2f 5 s after the hit\n", last_npc, (double)last_hp, (double)f); fflush(stdout); last_npc = 0; }
    s_test_att = round % 3; round++;
    if (is_guest(gf_coop_mode()) && round % 4 == 1) {   /* guest half of "an NPC hurt your puppet on the host": 5 points to us */
        ev_hit q; memset(&q, 0, sizeof q); q.ver = 1; q.dmg = 5.0f; q.dtype = 2; q.x2c = 1; q.x30 = 0x16; float mp[3];
        const uint32_t pl0 = player(); if (pl0 && pos_of(pl0, mp)) { for (int k = 0; k < 3; k++) { q.p1[k] = mp[k]; q.p2[k] = mp[k]; } q.p1[1] += 1.2f; q.p2[1] += 1.2f;
            uint32_t u0 = 0, u1 = 0; r32(pl0 + 0x14C8u, &u0); const int okd = send_damage(pl0, 0, &q); r32(pl0 + 0x14C8u, &u1); float a0, a1; memcpy(&a0, &u0, 4); memcpy(&a1, &u1, 4);
            printf("[coop-combat] self test: our player takes a forwarded 5-point hit (sent %d): health %.2f -> %.2f\n", okd, (double)a0, (double)a1); fflush(stdout); }
    }
    static uint32_t ents[256]; static uint16_t ids[256]; static float pos[256][3];
    const int n = gf_coop_world_host_peds(ents, ids, pos, 256); float me[3]; const uint32_t pl = player();
    if (!n || !pl || !pos_of(pl, me)) return;
    int best = -1; float bd = 1e30f; const int skip = (round / 3) % 3;   /* a different one of the 3 nearest peds every 3 rounds */
    for (int pass = 0; pass <= skip; pass++) { int b = -1; float bdd = 1e30f;
        for (int i = 0; i < n; i++) { const float dx = pos[i][0] - me[0], dz = pos[i][2] - me[2], d = dx*dx + dz*dz;
            if (d < bdd && (pass == 0 || d > bd)) { bdd = d; b = i; } }
        if (b < 0) break; best = b; bd = bdd; }
    if (best < 0 || bd > 40.0f * 40.0f) return;
    ev_hit h; memset(&h, 0, sizeof h); h.ver = 1; h.netid = ids[best]; h.dmg = 25.0f; h.dtype = 2; h.x2c = 1; h.x30 = 0x16;
    for (int k = 0; k < 3; k++) { h.p1[k] = pos[best][k]; h.p2[k] = pos[best][k]; } h.p1[1] += 1.2f; h.p2[1] += 1.2f;
    uint32_t u = 0; r32(ents[best] + 0x14C8u, &u); memcpy(&last_hp, &u, 4); last_npc = ents[best];
    EnterCriticalSection(&s_cs); if (((s_hit_h + 1) & 31) != s_hit_t) { s_hits[s_hit_h] = h; s_hit_type[s_hit_h] = EV_HIT; s_hit_h = (s_hit_h + 1) & 31; } LeaveCriticalSection(&s_cs);
}
static void damage_tick(void)
{
    if (s_test) damage_selftest();
    for (int guard = 0; guard < 16; guard++) {
        ev_hit h; uint16_t ty = 0; int got = 0, org = -1;
        EnterCriticalSection(&s_cs); if (s_hit_t != s_hit_h) { h = s_hits[s_hit_t]; ty = s_hit_type[s_hit_t]; org = s_hit_org[s_hit_t]; s_hit_t = (s_hit_t + 1) & 31; got = 1; } LeaveCriticalSection(&s_cs);
        if (!got) break;
        const int m = gf_coop_mode();
        if (ty == EV_HIT && owns_world(m)) {   /* v1068R joiner applies hits on its own natives */
            const uint32_t npc = host_ent_of(h.netid);
            if (!npc) { st_hit_nonpc++; continue; }
            uint32_t hp0 = 0; r32(npc + 0x14C8u, &hp0); const long seen0 = st_apply_seen;
            uint32_t att = gf_coop_puppet_entity();
            if (org >= 0) { const uint32_t xp = gf_coop_peer_puppet(org); if (xp) att = xp; }   /* v1072X3 */
            if (s_test && s_test_att == 1) att = player(); else if (s_test && s_test_att == 2) att = 0;
            if (send_damage(npc, att, &h)) st_hit_applied++;
            { extern void gf_coop_pop_combat_keep(uint32_t ent, unsigned ms); extern void gf_coop_pop_combat_drop(uint32_t ent); uint32_t hb = 0; float hf = 1.0f; if (r32(npc + 0x14C8u, &hb)) memcpy(&hf, &hb, 4);
              if (hf > 0.0f) gf_coop_pop_combat_keep(npc, 0); else gf_coop_pop_combat_drop(npc); }   /* v1127G3 being fought: not culled; v1127G6 not after a lethal hit */
            static int logs; if (logs < 24) { logs++; uint32_t hp = 0, vt = 0; r32(npc + 0x14C8u, &hp); r32(npc, &vt); float f, f0; memcpy(&f, &hp, 4); memcpy(&f0, &hp0, 4);
                printf("[coop-combat] host: partner hit netid %u -> npc %08X (vt %08X) attacker %08X dmg %.2f type %u: health %.2f -> %.2f, handler reached %ld\n", h.netid, npc, vt, att,
                       (double)h.dmg, h.dtype, (double)f0, (double)f, st_apply_seen - seen0); fflush(stdout); }
        } else if (ty == EV_PHURT && shows_world(m)) {
            const uint32_t me = player(); if (!me) continue;
            uint32_t att = 0; if (h.netid && (m == 2 || m == 1)) att = gf_proxy_ent_of(h.netid);   /* v1068R host: proxy of the joiner's NPC */
            if (send_damage(me, att, &h)) st_phurt_applied++;
            static int logs; if (logs < 12) { logs++; printf("[coop-combat] guest: host NPC netid %u hurt our puppet on the host -> our player takes %.2f (type %u, attacker proxy %08X)\n",
                h.netid, (double)h.dmg, h.dtype, att); fflush(stdout); }
        }
    }
}

/* ---------------------------------------------------------------- v1065G grab / melee on world proxies (agent G)
 * RE (v1065G, g65-2-echo): a ped proxy CAN be locked on (player+0x2A88) and the grab handshake reaches it exactly like a
 * native ped (same paired-animation messages, +0x1814 |= 0x40000000, +0x1818 |= 0x20). What broke the grab: every frame
 * gf_proxy_tick/drive_one (vt+0x30 + late vt+0x2C to the host's position) and the wpose stream (host bones) overwrite the
 * grabbed proxy -> it is yanked out of the grabber's hands (~2.8 m jump ~1 s into the grab, native <= 0.5 m).
 * Fix: GRAB HOLD (GF_COOP_GRAB_HOLD=0 off). While the local player is grabbing a proxy (player+0x5C4 bit 0x4 with the
 * lock target = the proxy) and GF_COOP_GRAB_HOLD_MS (1500) after it, gf_coop_grab_holds(ent) = 1: drive_one skips it and
 * gf_proxy_netid_of() returns 0 (no pose stream / reapply), so the game's paired grab animation owns it. The guest sends
 * EV_GRAB 0x0134 {netid, on, matrix} at 20 Hz; the host places the REAL NPC at that matrix every frame (it follows the
 * guest's grab; throws / slams included) and lets go on release. Damage during the grab is still swallowed + forwarded
 * (gf_proxy_netid_any).
 * Diagnostics (always on, cheap): the local player's lock-on target (player+0x2A88 = weak handle, target = h - 0x38,
 * read by the target scorer func_00260398) and the grab / melee messages that reach a proxy.
 * Echo self test GF_COOP_GRAB_TEST=1 (needs PAD_FILE): every 25 s the local player is put 1 m in front of a native ped
 * (even rounds) or of its proxy twin (odd rounds; rnd % 4 == 3 first registers the proxy as a target candidate
 * through func_00135d04, the call the game makes in the iMsgManagedEntityCreated branch of func_00140FE4), then grab
 * (L1+R1) and a lock-on punch (L1 + right stick up) are pressed through PAD_FILE and the messages / lock target logged. */
#include "gf_coop_msgnames.h"
extern uint16_t gf_proxy_netid_any(uint32_t ent);   /* gf_coop_proxy.c v1065G: like gf_proxy_netid_of, ignores the grab hold */
enum { EV_GRAB = 0x0134, EV_GRAB_END = 0x0137 };   /* v1125A EV_GRAB_END: owner -> holder, the held NPC died / is gone */
#pragma pack(push, 1)
typedef struct { uint8_t ver, on; uint16_t netid; float m[12]; } ev_grab;
typedef struct { uint8_t ver, why; uint16_t netid; } ev_grab_end;   /* why 1 = died (health <= 0), 2 = gone (corpse swap / despawn) */
#pragma pack(pop)
enum { MSGN = (int)(sizeof k_msgn / sizeof k_msgn[0]) };
static uint32_t s_mn_id[MSGN]; static uint32_t s_mn_pl; static int s_mn_ok;
static uint32_t s_id_grabF, s_id_grabB, s_id_grabRel, s_id_dmg; static volatile uint32_t s_id_paired;
static void msg_ids(void)   /* main PPU thread */
{
    const uint32_t pl = player();
    if (pl == s_mn_pl && s_mn_ok) return;
    s_mn_pl = pl; s_mn_ok = pl != 0;
    for (int i = 0; i < MSGN; i++) { uint32_t a = k_msgn[i].member ? (pl ? pl + k_msgn[i].a : 0) : k_msgn[i].a, v = 0;
        s_mn_id[i] = a && r32(a, &v) ? v : 0;
        if (!strcmp(k_msgn[i].n, "iMsgGrabbedFront")) s_id_grabF = s_mn_id[i];
        else if (!strcmp(k_msgn[i].n, "iMsgGrabbedBack")) s_id_grabB = s_mn_id[i];
        else if (!strcmp(k_msgn[i].n, "iMsgGrabReleased")) s_id_grabRel = s_mn_id[i];
        else if (!strcmp(k_msgn[i].n, "iMsgDamage")) s_id_dmg = s_mn_id[i];
        else if (!strncmp(k_msgn[i].n, "Sent to an entity when paired animation change", 46)) s_id_paired = s_mn_id[i]; }
}
static const char* msg_name(uint32_t id)
{
    if (!id) return "0";
    for (int i = 0; i < MSGN; i++) if (s_mn_id[i] == id) return k_msgn[i].n;
    return "?";
}
static volatile uint32_t s_gt_ent; static volatile int s_gt_on;
static uint32_t s_gt_ids[64]; static uint8_t s_gt_who[64]; static long s_gt_cnt[64]; static volatile long s_gt_n;
static volatile long st_px_grabmsg, st_px_msgs_test;
static void grab_note(uint32_t ent, uint32_t msg)   /* hook, any PPU thread; only while a test window is open */
{
    const uint32_t pl = s_mn_pl; int who = 0; uint32_t id;
    if (ent && ent == s_gt_ent) who = 1; else if (ent && ent == pl) who = 2; else return;
    if (!acc(msg, 4, 0)) return;
    id = rd32(msg);
    for (long i = 0; i < s_gt_n; i++) if (s_gt_ids[i] == id && s_gt_who[i] == who) { s_gt_cnt[i]++; return; }
    if (s_gt_n < 64) { const long k = s_gt_n; s_gt_ids[k] = id; s_gt_who[k] = (uint8_t)who; s_gt_cnt[k] = 1; s_gt_n = k + 1; }
}
static uint32_t lock_target(uint32_t pl) { uint32_t h = 0; if (!pl || !r32(pl + 0x2A88u, &h) || !h || h == 0x38u) return 0; return h - 0x38u; }
static void ent_flags(const char* tag, uint32_t e)
{
    uint32_t vt = 0, f34 = 0, f1814 = 0, f1818 = 0, f14b4 = 0, c4 = 0, inst = 0, f1864 = 0, f508 = 0, f1d9c = 0; float hp = 0, p[3] = { 0, 0, 0 };
    r32(e, &vt); r32(e + 0x34u, &f34); r32(e + 0x1814u, &f1814); r32(e + 0x1818u, &f1818); r32(e + 0x14B4u, &f14b4); r32(e + 0xC4u, &c4); r32(e + 0x64u, &inst);
    r32(e + 0x1864u, &f1864); r32(e + 0x508u, &f508); r32(e + 0x1D9Cu, &f1d9c);
    { uint32_t u = 0; r32(e + 0x14C8u, &u); memcpy(&hp, &u, 4); } pos_of(e, p);
    printf("[coop-grab] %s ent %08X vt %08X proxy %u | +34 %08X +1814 %08X +1818 %08X +14B4 %08X +1864 %08X +508 %08X +1D9C %08X | anim %08X inst %08X hp %.0f at %.2f %.2f %.2f\n",
           tag, e, vt, (unsigned)gf_proxy_netid_any(e), f34, f1814, f1818, f14b4, f1864, f508, f1d9c, c4, inst, (double)hp, (double)p[0], (double)p[1], (double)p[2]);
}
static int place_char(uint32_t ent, const float m[12])   /* vt+0x30 SetMatrix + vt+0x24 position block (as gf_coop_proxy.c place) */
{
    uint32_t xf = 0; const uint32_t buf = scratch(0);
    if (!buf || !acc(buf, 0x60, 1) || !r32(ent + 0x68u, &xf) || !acc(xf + 0x10u, 0x40, 0)) return 0;
    __try { for (int r = 0; r < 4; r++) { for (int k = 0; k < 3; k++) wrf(buf + (uint32_t)(r * 16 + k * 4), m[r * 3 + k]); wr32(buf + (uint32_t)(r * 16 + 12), rd32(xf + 0x10u + (uint32_t)(r * 16 + 12))); }
            wrf(buf + 0x40, m[9]); wrf(buf + 0x44, m[10]); wrf(buf + 0x48, m[11]); wr32(buf + 0x4C, 2u); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    vcall(ent, 0x30u, buf, 0); vcall(ent, 0x24u, buf + 0x40u, 0);
    return 1;
}
int gf_coop_place_char(uint32_t ent, const float m12[12]) { int r = 0; __try { r = place_char(ent, m12); } __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; } return r; }   /* v1073H warp (main PPU thread) */
uint32_t gf_coop_player_ent(void) { return player(); }   /* v1073H */
static int pad_file(const char* line)
{
    static char path[512]; static int ok = -1;
    if (ok < 0) { const char* e = getenv("PAD_FILE"); ok = e && *e; if (ok) { strncpy(path, e, sizeof path - 1); path[sizeof path - 1] = 0; } }
    if (!ok) return 0;
    FILE* f = fopen(path, "wb"); if (!f) return 0; fputs(line, f); fputc('\n', f); fclose(f); return 1;
}
/* ---- grab hold (guest / echo): proxies the local player is grabbing */
typedef struct { uint32_t ent; uint16_t netid; ULONGLONG until, sent, since, mv_ms, lt; float lp[3]; } ghold;   /* v1067G: + settle tracking */
static volatile uint16_t s_gend_id[8]; static volatile uint8_t s_gend_why[8]; static volatile long s_gend_n, st_gend_tx, st_gend_rx, st_gend_cut;   /* v1125A */   /* v1067G: + settle tracking */
static ghold s_hold[4]; static int s_hold_on = -1, s_hold_ms = 1500;
/* v1067G: a thrown / slammed NPC keeps flying after the player's grab ends -> the hold ends only when the proxy has SETTLED
 * (root < 0.5 m/s for GF_COOP_GRAB_SETTLE_MS, 0 = old rule), at most GF_COOP_GRAB_SETTLE_MAX_MS after the grab ended. */
static int s_settle_ms = 500, s_settle_max_ms = 6000; static long st_settle_wait, st_settle_cap;
extern void gf_npcfix_hold_end(uint16_t netid);   /* gf_coop_npcfix.c: hand-back cross-fade */
extern unsigned long long gf_npcfix_last_motion(uint16_t netid);   /* gf_coop_npcfix.c: limb motion of the held proxy */
static long st_hold_on, st_hold_frames, st_grab_tx, st_grab_rx, st_grab_apply, st_grab_noent;
int gf_coop_grab_holds(uint32_t ent)   /* gf_coop_proxy.c drive_one / gf_proxy_netid_of; main PPU thread mostly, read-only */
{
    if (!ent || s_hold_on != 1) return 0;
    for (int i = 0; i < 4; i++) if (s_hold[i].ent == ent) return 1;
    return 0;
}
static int read_m12(uint32_t ent, float m[12])
{
    uint32_t xf; if (!r32(ent + 0x68u, &xf) || !acc(xf + 0x10u, 0x40, 0)) return 0;
    __try { for (int r = 0; r < 4; r++) for (int k = 0; k < 3; k++) m[r * 3 + k] = rdf(xf + 0x10u + (uint32_t)(r * 16 + k * 4)); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return m[9] == m[9];
}
static void hold_send(ghold* h, int on, ULONGLONG now)
{
    ev_grab g; memset(&g, 0, sizeof g); g.ver = 1; g.on = (uint8_t)on; g.netid = h->netid;
    if (!read_m12(h->ent, g.m)) { if (on) return; }
    if (gf_coop_mode() == 3) { static float ofs = -1; if (ofs < 0) { const char* e = getenv("GF_COOP_WORLD_ECHO_OFS"); ofs = e ? (float)atof(e) : 2.5f; } g.m[9] -= ofs; }   /* echo: undo the twin offset */
    if (gf_coop_send_event(EV_GRAB, &g, sizeof g)) st_grab_tx++;
    h->sent = now;
}
static volatile uint32_t s_pair_ent; static volatile ULONGLONG s_pair_ms; static volatile long st_pair_msgs;
static void pair_note(uint32_t ent, uint32_t msg)   /* hook, any PPU thread: a paired (grab) animation starts / changes on ent */
{
    uint32_t id; if (!s_id_paired || !ent || !acc(msg, 4, 0)) return;
    id = rd32(msg); if (id != s_id_paired) return;
    s_pair_ent = ent; s_pair_ms = GetTickCount64(); st_pair_msgs++;
}
static void hold_tick(uint32_t pl, ULONGLONG now)
{
    if (s_hold_on < 0) { const char* e = getenv("GF_COOP_GRAB_HOLD"); s_hold_on = !(e && *e == '0');
        if ((e = getenv("GF_COOP_GRAB_HOLD_MS")) && atoi(e) >= 0) s_hold_ms = atoi(e);
        printf("[coop-grab] v1065G grab hold %s (a proxy the local player grabs leaves the host stream until %d ms after the grab; host NPC follows)\n", s_hold_on ? "ON" : "off", s_hold_ms);
        if ((e = getenv("GF_COOP_GRAB_SETTLE_MS")) && atoi(e) >= 0) s_settle_ms = atoi(e);
        if ((e = getenv("GF_COOP_GRAB_SETTLE_MAX_MS")) && atoi(e) >= 0) s_settle_max_ms = atoi(e);
        printf("[coop-grab] v1067G hold end: after the grab + until the NPC has settled (< 0.5 m/s for %d ms, at most %d ms more; GF_COOP_GRAB_SETTLE_MS / _MAX_MS)\n", s_settle_ms, s_settle_max_ms);
        fflush(stdout); }
    if (!s_hold_on) return;
    const int m = gf_coop_mode(); if (!shows_world(m)) return;   /* v1068R: the host too (grabbing a proxy of a joiner NPC) */
    uint32_t fl = 0; r32(pl + 0x5C4u, &fl);
    const uint32_t t = lock_target(pl); const uint16_t nid = t ? gf_proxy_netid_any(t) : 0;
    /* grab = a paired-animation message reached that proxy (punches never send one: g65-4) while the player is busy (+0x5C4 & 4);
     * then held while the player stays busy with it (lock target or the paired entity), + GF_COOP_GRAB_HOLD_MS */
    const uint32_t pe = s_pair_ent; const int pair_new = pe && now - s_pair_ms < 1500 && gf_proxy_netid_any(pe);
    int already = 0; for (int i = 0; i < 4; i++) if (s_hold[i].ent && (s_hold[i].ent == t || s_hold[i].ent == pe)) already = 1;
    if ((fl & 4u) && (pair_new || (already && nid))) {   /* grabbing a proxy */
        const uint32_t g = pair_new ? pe : t; const uint16_t gid = gf_proxy_netid_any(g);
        if (gid) {
        int k = -1; for (int i = 0; i < 4; i++) if (s_hold[i].ent == g) { k = i; break; }
        if (k < 0) for (int i = 0; i < 4; i++) if (!s_hold[i].ent) { k = i; break; }
        if (k >= 0) { ghold* h = &s_hold[k];
            if (h->ent != g) { h->ent = g; h->netid = gid; h->since = now; h->sent = 0; h->mv_ms = now; h->lt = 0; st_hold_on++; rc_push(gid);   /* v1127C */
                static int logs; if (logs++ < 30) { printf("[coop-grab] v1065G HOLD ON: local player grabs proxy net %u ent %08X (lock %08X) -> host stream off, host NPC follows\n", gid, g, t); fflush(stdout); } }
            h->until = now + (ULONGLONG)s_hold_ms; }
        }
    }
    for (int i = 0; i < 4; i++) { ghold* h = &s_hold[i]; if (!h->ent) continue;
        uint32_t v = 0; const int alive = gf_proxy_netid_any(h->ent) == h->netid && r32(h->ent, &v);
        int settled = 1;
        if (alive && s_settle_ms > 0) {   /* v1067G settle tracking (root speed of the held proxy) */
            float m[12];
            if (read_m12(h->ent, m)) {
                if (h->lt && now > h->lt) { const float dx = m[9] - h->lp[0], dy = m[10] - h->lp[1], dz = m[11] - h->lp[2];
                    if (sqrtf(dx * dx + dy * dy + dz * dz) * 1000.0f / (float)(now - h->lt) > 0.5f) h->mv_ms = now; }
                if (!h->lt || now > h->lt) { h->lp[0] = m[9]; h->lp[1] = m[10]; h->lp[2] = m[11]; h->lt = now; } }
            {   const ULONGLONG lm = gf_npcfix_last_motion(h->netid);   /* limbs (pose) moving too: ragdoll / thrown anim with a still root */
                if (lm > h->mv_ms && lm <= now) h->mv_ms = lm; }
            settled = now - h->mv_ms >= (ULONGLONG)s_settle_ms || now >= h->until + (ULONGLONG)s_settle_max_ms;
            if (now >= h->until && !settled) st_settle_wait++;
            if (now >= h->until + (ULONGLONG)s_settle_max_ms && now - h->mv_ms < (ULONGLONG)s_settle_ms) st_settle_cap++;
        }
        int owner_end = 0;   /* v1125A: the owner says this NPC died / is gone -> end the hold NOW, so its death (corpse stream) shows here */
        for (int q = 0; q < 8; q++) if (s_gend_id[q] && s_gend_id[q] == h->netid) { owner_end = s_gend_why[q] ? s_gend_why[q] : 1; s_gend_id[q] = 0; }
        if (owner_end) { st_gend_cut++; static int lg; if (lg++ < 20) { printf("[coop-grab] v1125A owner: held NPC net %u %s -> hold ends now (death / corpse comes from the owner's stream)\n", h->netid, owner_end == 1 ? "DIED" : "is gone"); fflush(stdout); } }
        if (owner_end || !alive || (now >= h->until && settled) || now - h->since > 60000) {
            if (alive) { hold_send(h, 0, now); gf_npcfix_hold_end(h->netid); }
            static int logs; if (logs++ < 30) { printf("[coop-grab] v1065G HOLD OFF: proxy net %u ent %08X after %llu ms%s -> back to the host stream\n", h->netid, h->ent, now - h->since, alive ? "" : " (proxy gone)"); fflush(stdout); }
            memset(h, 0, sizeof *h); continue; }
        st_hold_frames++;
        if (now - h->sent >= 50) hold_send(h, 1, now);
    }
}
/* ---- host side: follow the guest's grab */
typedef struct { uint16_t netid; int on; float m[12]; ULONGLONG rx; } gfollow;
static gfollow s_fol[4];   /* guarded by s_cs */
static uint32_t s_fol_ent[4]; static uint16_t s_fol_id[4];   /* v1067G: host NPCs followed this frame (main thread) */
uint16_t gf_coop_grab_followed(uint32_t ent)   /* v1067G (gf_coop_npcfix.c): net id if ent is a host NPC the partner holds now */
{
    if (!ent) return 0;
    for (int i = 0; i < 4; i++) if (s_fol_ent[i] == ent) return s_fol_id[i];
    return 0;
}
static void follow_tick(ULONGLONG now)
{
    const int m = gf_coop_mode(); if (!owns_world(m)) { memset(s_fol_ent, 0, sizeof s_fol_ent); return; }   /* v1068R: the joiner follows host grabs of its natives */
    gfollow f[4]; EnterCriticalSection(&s_cs); memcpy(f, s_fol, sizeof f); LeaveCriticalSection(&s_cs);
    for (int i = 0; i < 4; i++) {
        s_fol_ent[i] = 0; s_fol_id[i] = 0;
        if (!f[i].netid || !f[i].on || now - f[i].rx > 600) continue;
        const uint32_t npc = host_ent_of(f[i].netid);
        {   /* v1125A: the NPC the partner holds died (our hits applied it, health <= 0) or is gone -> tell the holder, stop placing it */
            uint32_t u = 0; float hp = 1.0f; if (npc && r32(npc + 0x14C8u, &u)) memcpy(&hp, &u, 4);
            if (!npc || hp <= 0.0f) { ev_grab_end ge; ge.ver = 1; ge.why = npc ? 1 : 2; ge.netid = f[i].netid;
                if (gf_coop_send_event(EV_GRAB_END, &ge, sizeof ge)) st_gend_tx++;
                EnterCriticalSection(&s_cs); if (s_fol[i].netid == f[i].netid) s_fol[i].on = 0; LeaveCriticalSection(&s_cs);
                static int lg; if (lg++ < 20) { printf("[coop-grab] v1125A held NPC net %u %s -> told the holder, follow off\n", f[i].netid, npc ? "died" : "is gone"); fflush(stdout); }
                continue; } }
        if (!npc) { st_grab_noent++; continue; }
        { extern int gf_coop_char_hold(void); if (gf_coop_char_hold()) continue; }   /* v1068R2: no placement while the world reloads (Havok abort 0x00ABBF14, t68-1) */
        s_fol_ent[i] = npc; s_fol_id[i] = f[i].netid;
        if (place_char(npc, f[i].m)) st_grab_apply++;
    }
}
static void on_grab_event(const ev_grab* g)   /* co-op thread */
{
    const ULONGLONG now = GetTickCount64(); int k = -1;
    EnterCriticalSection(&s_cs);
    for (int i = 0; i < 4; i++) if (s_fol[i].netid == g->netid) { k = i; break; }
    if (k < 0) for (int i = 0; i < 4; i++) if (!s_fol[i].netid || !s_fol[i].on || now - s_fol[i].rx > 5000) { k = i; break; }
    if (k >= 0) { const int was = s_fol[k].netid == g->netid && s_fol[k].on; s_fol[k].netid = g->netid; s_fol[k].on = g->on; memcpy(s_fol[k].m, g->m, sizeof g->m); s_fol[k].rx = now;
        LeaveCriticalSection(&s_cs);
        if (was != (g->on != 0)) { static int logs; if (logs++ < 30) { printf("[coop-grab] v1065G host: partner %s NPC net %u\n", g->on ? "GRABS" : "released", g->netid); fflush(stdout); } }
    } else LeaveCriticalSection(&s_cs);
    InterlockedIncrement(&st_grab_rx);
}
static int s_gtest = -1;
static void grab_tick(ULONGLONG now)   /* main PPU thread */
{
    const int m = gf_coop_mode(); const uint32_t pl = player();
    if (!m) return;
    __try { follow_tick(now); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    if (!pl) return;
    msg_ids();
    __try { hold_tick(pl, now); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    {   /* lock target changes (guest / echo): proves whether a proxy can be locked on at all */
        static uint32_t last; static int logs; const uint32_t t = lock_target(pl);
        if (t != last) { last = t; if (t && logs < 60) { logs++; const uint16_t nid = gf_proxy_netid_any(t);
            printf("[coop-grab] v1065G lock target -> %08X (%s %u)\n", t, nid ? "PROXY net" : "native", (unsigned)nid); fflush(stdout); } }
    }
    { static ULONGLONG lg; if (now - lg >= 10000) { lg = now; if (st_hold_on || st_grab_rx || st_grab_tx || st_pair_msgs) {
        printf("[coop-grab] v1065G holds %ld (%ld held frames, paired-anim msgs %ld), grab tx %ld rx %ld, host follow applied %ld (no npc %ld) | v1067G frames waiting for the NPC to settle %ld (cap hits %ld)\n", st_hold_on, st_hold_frames, st_pair_msgs, st_grab_tx, st_grab_rx, st_grab_apply, st_grab_noent, st_settle_wait, st_settle_cap); fflush(stdout); } } }
    if (s_gtest < 0) { const char* e = getenv("GF_COOP_GRAB_TEST"); s_gtest = e && *e == '1' && m == 3;
        if (s_gtest) { printf("[coop-grab] v1065G grab self test ON (PAD_FILE %s)\n", getenv("PAD_FILE") ? getenv("PAD_FILE") : "MISSING"); fflush(stdout); } }
    if (!s_gtest) return;
    static ULONGLONG t_first, t_round; static int rnd = -1, phase; static uint32_t tgt; static float tgt_p0[3];
    if (!t_first) { t_first = now; }
    const ULONGLONG at = (ULONGLONG)(getenv("GF_COOP_GRAB_TEST_AT") ? atoi(getenv("GF_COOP_GRAB_TEST_AT")) : 80) * 1000ull;
    if (now - t_first < at) return;
    if (phase == 0 && (rnd < 0 || now - t_round >= 28000)) {
        static uint32_t ents[256]; static uint16_t ids[256]; static float pos[256][3]; float me[3];
        const int n = gf_coop_world_host_peds(ents, ids, pos, 256);
        if (!pos_of(pl, me)) return;
        int best = -1; float bd = 1e30f;
        for (int i = 0; i < n; i++) { const uint32_t px = gf_proxy_ent_of(ids[i]); float pp[3];
            if (!px || !pos_of(px, pp) || !pos_of(ents[i], pos[i])) continue;
            uint32_t vt = 0; r32(ents[i], &vt);
            const float dx = pos[i][0] - me[0], dz = pos[i][2] - me[2], d = dx * dx + dz * dz + (vt == 0x100463A0u ? 1e6f : 0.0f);   /* crew class last */
            if (d < bd) { bd = d; best = i; } }
        if (best < 0) { static int said; if (!said++) { printf("[coop-grab] test: no native with a live proxy twin yet (%d host peds)\n", n); fflush(stdout); } return; }
        rnd++; t_round = now;
        const int use_px = rnd & 1; const uint32_t px = gf_proxy_ent_of(ids[best]);
        tgt = use_px ? px : ents[best];
        float tp[3], f[3] = { 0, 0, 1 }; pos_of(tgt, tp); fwd_of(tgt, f);
        { float l = sqrtf(f[0] * f[0] + f[2] * f[2]); if (!(l > 1e-3f)) { f[0] = 0; f[2] = 1; l = 1; } f[0] /= l; f[2] /= l; f[1] = 0; }
        float mm[12] = { -f[2], 0, f[0],  0, 1, 0,  -f[0], 0, -f[2],  tp[0] + f[0] * 1.0f, tp[1], tp[2] + f[2] * 1.0f };   /* at = -f (face the target), right = (at.z, 0, -at.x) */
        printf("[coop-grab] ---- test round %d: %s net %u (native %08X, proxy %08X) %.1f m away -> player to 1 m in front of it\n", rnd, use_px ? "PROXY" : "NATIVE", (unsigned)ids[best], ents[best], px, (double)sqrtf(bd < 1e6f ? bd : bd - 1e6f));
        ent_flags("target", tgt);
        place_char(pl, mm); pos_of(tgt, tgt_p0);
        s_gt_n = 0; s_gt_ent = tgt; s_gt_on = 1; phase = 1; fflush(stdout);
        return;
    }
    if (phase == 1 && now - t_round >= 800) { pad_file("0x0C00 300"); phase = 2; printf("[coop-grab] test: GRAB pressed (L1+R1 300 polls), lock target %08X\n", lock_target(pl)); fflush(stdout); return; }
    if (phase >= 2 && phase <= 9) { static ULONGLONG ls; if (now - ls > 400) { ls = now; float p[3] = { 0, 0, 0 }, q[3] = { 0, 0, 0 }; uint32_t a = 0, hu = 0; float hp = 0; pos_of(tgt, p); pos_of(pl, q); r32(pl + 0x5C4u, &a); r32(tgt + 0x14C8u, &hu); memcpy(&hp, &hu, 4);
            printf("[coop-grab] test t+%llu ms: lock %08X, target moved %.2f m, target-player %.2f m, player +5C4 %08X, held %d, hp %.0f\n", now - t_round, lock_target(pl),
                   (double)sqrtf((p[0] - tgt_p0[0]) * (p[0] - tgt_p0[0]) + (p[2] - tgt_p0[2]) * (p[2] - tgt_p0[2])),
                   (double)sqrtf((p[0] - q[0]) * (p[0] - q[0]) + (p[2] - q[2]) * (p[2] - q[2])), a, gf_coop_grab_holds(tgt), (double)hp); fflush(stdout); } }
    /* punch: re-placed in front of the target, then L1 (centred stick) -> L1 + right stick up -> L1 centred, twice */
    if (phase == 2 && now - t_round >= 9000) { float tp[3], f[3] = { 0, 0, 1 }; pos_of(tgt, tp); fwd_of(tgt, f);
        { float l = sqrtf(f[0] * f[0] + f[2] * f[2]); if (!(l > 1e-3f)) { f[0] = 0; f[2] = 1; l = 1; } f[0] /= l; f[2] /= l; }
        float mm[12] = { -f[2], 0, f[0],  0, 1, 0,  -f[0], 0, -f[2],  tp[0] + f[0] * 1.1f, tp[1], tp[2] + f[2] * 1.1f }; place_char(pl, mm);
        pad_file("0x0400 40 128 128 128 128"); phase = 3; printf("[coop-grab] test: PUNCH sequence (L1, then right stick up x2), lock %08X\n", lock_target(pl)); fflush(stdout); return; }
    if (phase == 3 && now - t_round >= 10000) { pad_file("0x0400 12 128 128 128 0"); phase = 4; return; }
    if (phase == 4 && now - t_round >= 10400) { pad_file("0x0400 40 128 128 128 128"); phase = 5; return; }
    if (phase == 5 && now - t_round >= 11400) { pad_file("0x0400 12 128 128 128 0"); phase = 6; return; }
    if (phase == 6 && now - t_round >= 11800) { pad_file("0x0400 40 128 128 128 128"); phase = 7; return; }
    if (phase == 7 && now - t_round >= 13000) { pad_file("0 0 128 128 128 128"); phase = 8; return; }
    if (phase == 8 && now - t_round >= 15000) {
        printf("[coop-grab] test round %d summary: %ld distinct messages (1 = to target, 2 = to player):", rnd, s_gt_n);
        for (long i = 0; i < s_gt_n; i++) printf(" %d:%s(%08X)x%ld", s_gt_who[i], msg_name(s_gt_ids[i]), s_gt_ids[i], s_gt_cnt[i]);
        printf("\n"); ent_flags("target after", tgt);
        s_gt_on = 0; s_gt_ent = 0; phase = 0; fflush(stdout);
    }
}

/* ---------------------------------------------------------------- v1072X3 (Claude) extra peers (3+ players)
 * User 18:40: "the host can't see the gun on guest2 or any shots from him". Legacy handlers only get the PRIMARY peer's traffic
 * (v1072S), so joiner 2's EV_WEAPON / EV_SHOT / EV_HIT never reached combat. Now: an all-peer handler takes traffic from every
 * NON-primary origin into per-peer slots; the weapon goes on that peer's puppet (gf_coop_peer_puppet) with its OWN item, shots
 * fire through it. Joiners also send EV_WEAPON_R / EV_SHOT_R through the host relay so the OTHER joiners see them; a receiver
 * ignores the relayed copy from a direct peer (it already got the legacy event). Hits (EV_HIT) from any non-primary joiner join the
 * normal hit queue (attacker = that joiner's puppet). GF_COOP_COMBAT_NPEER=0 off. */
extern int gf_coop_peer_direct_id(int id); extern int gf_coop_peer_is_primary_id(int id);
typedef struct { int used, id; ev_weapon w; int have; uint32_t pup, item; uint8_t iguid[16]; int drawn; ULONGLONG check; } xrs;
static xrs s_xr[8]; typedef struct { int id; ev_shot s; } xshot; static xshot s_xsh[32]; static int s_xsh_h, s_xsh_t;   /* guarded by s_cs */
static long st_x_weapon, st_x_shot, st_x_fired, st_x_equip, st_x_hit, st_x_reldup;
static xrs* xr_get(int id) { for (int i = 0; i < 8; i++) if (s_xr[i].used && s_xr[i].id == id) return &s_xr[i];
    for (int i = 0; i < 8; i++) if (!s_xr[i].used) { memset(&s_xr[i], 0, sizeof s_xr[i]); s_xr[i].used = 1; s_xr[i].id = id; return &s_xr[i]; } return NULL; }
static void on_peer_event(int from, uint16_t type, const void* data, int len)   /* co-op thread */
{
    static int on = -1; if (on < 0) { const char* e = getenv("GF_COOP_COMBAT_NPEER"); on = !(e && *e == '0'); }
    if (!on || !s_cs_ok || from < 0) return;
    if (type == EV_WEAPON_R || type == EV_SHOT_R) { if (gf_coop_peer_direct_id(from)) { InterlockedIncrement(&st_x_reldup); return; } type = type == EV_WEAPON_R ? EV_WEAPON : EV_SHOT; }
    else if (gf_coop_peer_is_primary_id(from)) return;   /* legacy on_event has it */
    if (type == EV_WEAPON && len >= (int)sizeof(ev_weapon)) { EnterCriticalSection(&s_cs); { xrs* x = xr_get(from); if (x) { memcpy(&x->w, data, sizeof x->w); x->have = 1; } } LeaveCriticalSection(&s_cs); InterlockedIncrement(&st_x_weapon); }
    else if (type == EV_SHOT && len >= (int)sizeof(ev_shot)) { EnterCriticalSection(&s_cs); if (((s_xsh_h + 1) & 31) != s_xsh_t) { s_xsh[s_xsh_h].id = from; memcpy(&s_xsh[s_xsh_h].s, data, sizeof(ev_shot)); s_xsh_h = (s_xsh_h + 1) & 31; } LeaveCriticalSection(&s_cs); InterlockedIncrement(&st_x_shot); }
    else if (type == EV_HIT && len >= (int)sizeof(ev_hit)) {
        EnterCriticalSection(&s_cs); if (((s_hit_h + 1) & 31) != s_hit_t) { memcpy(&s_hits[s_hit_h], data, sizeof(ev_hit)); s_hit_type[s_hit_h] = EV_HIT; s_hit_org[s_hit_h] = from; s_hit_h = (s_hit_h + 1) & 31; } LeaveCriticalSection(&s_cs);
        InterlockedIncrement(&st_hit_rx); InterlockedIncrement(&st_x_hit); }
}
static void xr_tick(ULONGLONG now)   /* main PPU thread */
{
    for (int i = 0; i < 8; i++) {
        xrs* x = &s_xr[i]; ev_weapon w; int have; if (!x->used) continue;
        EnterCriticalSection(&s_cs); w = x->w; have = x->have; LeaveCriticalSection(&s_cs);
        uint32_t pup = gf_coop_peer_puppet(x->id); if (ent_destroyed(pup)) pup = 0;   /* v1127C */
        if (!pup) { x->pup = 0; x->item = 0; x->drawn = 0; continue; }
        if (pup != x->pup) { x->pup = pup; x->item = 0; x->drawn = 0; x->check = 0; }   /* new puppet (respawn): new item */
        const int want = have && w.drawn && guid_set(w.guid);
        uint32_t cur = 0; { uint32_t h; if (r32(pup + 0x468u, &h) && h && h != 0x38u) cur = h - 0x38u; }
        if (want) {
            if (!x->item || !item_alive(x->item, 0) || memcmp(x->iguid, w.guid, 16)) { x->item = create_item_v72(w.guid); memcpy(x->iguid, w.guid, 16); }
            if (x->item && (cur != x->item || !x->drawn) && now >= x->check) { set_in_hand(pup, x->item); st_x_equip++; x->drawn = 1; x->check = now + 400;
                static int logs; if (logs < 8) { logs++; printf("[coop-combat] v1072X3 peer %d puppet %08X takes item %08X (type 0x%02X)\n", x->id, pup, x->item, w.type); fflush(stdout); } }
        } else if ((x->drawn || (cur && have)) && now >= x->check) { if (cur) set_in_hand(pup, 0); x->drawn = 0; x->check = now + 1000; }
    }
    for (int guard = 0; guard < 8; guard++) {
        xshot q; int got = 0; xrs* x = NULL;
        EnterCriticalSection(&s_cs); if (s_xsh_t != s_xsh_h) { q = s_xsh[s_xsh_t]; s_xsh_t = (s_xsh_t + 1) & 31; got = 1; } LeaveCriticalSection(&s_cs);
        if (!got) break;
        for (int i = 0; i < 8; i++) if (s_xr[i].used && s_xr[i].id == q.id) x = &s_xr[i];
        if (!x || !x->pup || !x->item) { st_fire_skip++; continue; }
        const uint32_t pup = x->pup, it = x->item; uint32_t cur = 0; { uint32_t h; if (r32(pup + 0x468u, &h) && h && h != 0x38u) cur = h - 0x38u; }
        if (cur != it || rd32(it) != VT_WEAPON) { st_fire_skip++; continue; }
        if (weak(it + 0x134) != pup) link_holder(it, pup);
        uint32_t f1818 = 0; if (!r32(pup + 0x1818u, &f1818) || !acc(pup + 0x5D4u, 12, 1) || !acc(it + 0x1EC, 4, 1)) { st_fire_skip++; continue; }
        for (int k = 0; k < (int)q.s.n && k < 8; k++) {
            __try { wrf(pup + 0x5D4, q.s.aim[0]); wrf(pup + 0x5D8, q.s.aim[1]); wrf(pup + 0x5DC, q.s.aim[2]); wr32(pup + 0x1818, f1818 | 0x400000u); wr32(it + 0x1EC, 1); }
            __except (EXCEPTION_EXECUTE_HANDLER) { break; }
            gcall(0x000A4284u, it, 0, 0, 0, 0); st_x_fired++;
            __try { wr32(it + 0x1EC, 0); } __except (EXCEPTION_EXECUTE_HANDLER) { }
        }
        __try { wr32(pup + 0x1818, (rd32(pup + 0x1818) & ~0x400000u) | (f1818 & 0x400000u)); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
}

/* ---------------------------------------------------------------- events (co-op thread: copy only) */
static void on_event(uint16_t type, const void* data, int len)
{
    if (!s_cs_ok) return;
    if (type == EV_WEAPON && len >= (int)sizeof(ev_weapon)) { EnterCriticalSection(&s_cs); memcpy(&s_rx_w, data, sizeof s_rx_w); s_rx_have = 1; LeaveCriticalSection(&s_cs); }
    else if (type == EV_SHOT && len >= (int)sizeof(ev_shot)) { EnterCriticalSection(&s_cs); if (((s_shot_h + 1) & 31) != s_shot_t) { memcpy(&s_shots[s_shot_h].s, data, sizeof(ev_shot)); s_shot_h = (s_shot_h + 1) & 31; } LeaveCriticalSection(&s_cs); }
    else if ((type == EV_HIT || type == EV_PHURT) && len >= (int)sizeof(ev_hit)) {
        EnterCriticalSection(&s_cs); if (((s_hit_h + 1) & 31) != s_hit_t) { memcpy(&s_hits[s_hit_h], data, sizeof(ev_hit)); s_hit_type[s_hit_h] = type; s_hit_org[s_hit_h] = -1; s_hit_h = (s_hit_h + 1) & 31; } LeaveCriticalSection(&s_cs);
        if (type == EV_HIT) InterlockedIncrement(&st_hit_rx); else InterlockedIncrement(&st_phurt_rx);
    }
    else if (type == EV_GRAB && len >= (int)sizeof(ev_grab)) on_grab_event((const ev_grab*)data);   /* v1065G */
    else if (type == EV_GRAB_END && len >= (int)sizeof(ev_grab_end)) {   /* v1125A holder side */
        const ev_grab_end* ge = (const ev_grab_end*)data; const int k = (int)(InterlockedIncrement(&s_gend_n) & 7);
        s_gend_why[k] = ge->why; s_gend_id[k] = ge->netid; InterlockedIncrement(&st_gend_rx); }
}

void gf_coop_combat_init(void)
{
    const char* e = getenv("GF_COOP_COMBAT"); s_on = !(e && *e == '0');
    if ((e = getenv("GF_COOP_COMBAT_TEST")) && *e == '1') s_test = 1;
    if ((e = getenv("GF_COOP_COMBAT_ECHO_HURT")) && *e == '1') s_echo_hurt = 1;
    if (!s_on) { printf("[coop-combat] v1062C off (GF_COOP_COMBAT=0)\n"); fflush(stdout); return; }
    InitializeCriticalSection(&s_cs); s_cs_ok = 1;
    gf_coop_set_event_handler(on_event);
    gf_coop_add_peer_event_handler(on_peer_event);   /* v1072X3 */
    printf("[coop-combat] v1062C combat sync ON (weapon on puppet, gunfire replay, damage forwarding)%s\n", s_test ? " + SELF TEST" : ""); fflush(stdout);
}
void gf_coop_combat_tick(void)   /* main PPU thread (gf_coop_puppet_tick) */
{
    if (s_on != 1 || !g_active_ctx || !gf_coop_mode()) return;
    const ULONGLONG now = GetTickCount64();
    __try { sender_tick(now); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    __try { receiver_tick(now); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    __try { xr_tick(now); } __except (EXCEPTION_EXECUTE_HANDLER) { }   /* v1072X3 */
    __try { damage_tick(); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    __try { grab_tick(now); } __except (EXCEPTION_EXECUTE_HANDLER) { }   /* v1065G */
    __try { rc_tick(); } __except (EXCEPTION_EXECUTE_HANDLER) { }   /* v1127C combat control */
    static ULONGLONG last; if (now - last >= 10000) { last = now;
        printf("[coop-combat] tx weapon %ld shots %ld (%ld events) hits %ld phurt %ld | puppet equip %ld holster %ld reassert %ld, items %d created %ld failed %ld, fired %ld (real shots %ld) skipped %ld | calls faulted %d\n",
               st_w_tx, st_shots_tx, st_shot_tx, st_hit_tx, st_phurt_tx, st_equip, st_holster, st_reassert, s_nitems, st_create, st_create_fail, st_fired, st_fire_real, st_fire_skip, s_fail);
        printf("[coop-combat] damage msgs %ld | swallowed: to puppet %ld, by puppet %ld, to proxy %ld | hits rx %ld applied %ld no-npc %ld | phurt rx %ld applied %ld\n",
               st_dmg_seen, st_sw_puppet, st_sw_bypuppet, st_sw_proxy, st_hit_rx, st_hit_applied, st_hit_nonpc, st_phurt_rx, st_phurt_applied);
        if (gf_coop_npeer_active()) printf("[coop-combat] v1072X3 extra peers: weapon events %ld, shot events %ld (fired %ld), equips %ld, hits rx %ld, relayed dups ignored %ld\n", st_x_weapon, st_x_shot, st_x_fired, st_x_equip, st_x_hit, st_x_reldup);
        fflush(stdout); }
}

/* v1127G1 (lead): func_000FB9E4 (new 0x70 object + std::list push_back) hand edit -> called when the list allocator returned
 * NULL for the 12-byte node (c27-17 joiner hang: node 0 linked in -> endless list walk). Logs the list / allocator state and
 * returns 1 = skip the link (object returned unlinked). GF_LIST_GUARD=0 -> 0 = old behaviour. Any PPU thread. */
int gf_list_node_null(uint32_t fn, uint32_t list, uint32_t obj, uint32_t lr)
{
    static int s_on = -1; static volatile LONG s_n;
    LONG k;
    if (s_on < 0) { const char* e = getenv("GF_LIST_GUARD"); s_on = !(e && *e == '0'); }
    k = InterlockedIncrement(&s_n);
    if (k <= 20 || (k % 200) == 0) {
        uint32_t al = 0, alvt = 0, alfn = 0, cnt = 0, p4 = 0;
        __try { al = rd32(list); p4 = rd32(list + 4); cnt = rd32(list + 0x1C); if (al) { alvt = rd32(al); if (alvt) alfn = rd32(rd32(alvt)); } }
        __except (EXCEPTION_EXECUTE_HANDLER) { }
        printf("[guard] v1127G1 list node alloc FAILED #%ld in func_%08X (caller lr %08X): list %08X count %u, allocator %08X vt %08X fn %08X, +4 %08X, obj %08X -> %s\n",
               (long)k, fn, lr, list, cnt, al, alvt, alfn, p4, obj, s_on ? "returned UNLINKED (no hang)" : "linked as before (GF_LIST_GUARD=0)");
        fflush(stdout);
    }
    return s_on;
}

/* v1127G26 lock-on candidate hooks (lifted func_00261674 around its func_00260CD0 call, _patches/apply-pending.ps1): the score
 * passes through unchanged. */
void gf_lock_cand_begin(void) { }
double gf_lock_cand_score(uint32_t q, uint32_t rec, double f1) { (void)q; (void)rec; return f1; }
