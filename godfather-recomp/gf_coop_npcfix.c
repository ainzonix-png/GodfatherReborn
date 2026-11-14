/* gf_coop_npcfix.c -- v1067 (Claude, 2026-11-05 evening): three NPC co-op fixes after the user's 17:3x coop-local test
 * ("NPC animations still desync when the host is not looking at them / when the joiner fights or grabs an NPC, especially
 * throwing them around, it can't decide which animation to use; the joiner walks away from the host -> ghost town").
 *
 * L  HOST LOD COUNTS THE JOINER (v1067L). The host's character LOD (func_00140238 per managed ped, called from the manager
 *    at 0x0034D16C; reload func_00136DC0 from func_0034E380) measures ONE squared distance: func_00282850(&ped_pos) = distance^2
 *    to the host's focus point (func_0046C5F8(*(TOC+0x93C))). Beyond 40 m (80 m with +0x1818 bit 0x40000000) func_001370A4 saves
 *    the model GUID to +0x1C60 and drops the render model AND the anim controller (+0xC4 = 0); it reloads below 30 m / 60 m.
 *    So every host ped near the JOINER but away from the host had no animation at all on the host: the user's 17:3x host log
 *    "no rig 11-13 of 25-27 selected", "fanim skip: no anim ctl 980" -> no pose stream -> the joiner's proxies played donors /
 *    their own idle = "desync when the host is not looking". Hand edit (apply-pending.ps1 block v1067L) right after both
 *    func_00282850 calls (0x00140520 unload test, 0x00136E28 reload test): f1 = gf_coop_lod_d2(pos, f1) = min(f1, distance^2 to
 *    the joiner's puppet). Host only. GF_COOP_LOD_PARTNER=1 enables it (default off). Log "[coop-npcfix] v1067L host LOD".
 *
 * G  GRAB / THROW: THE JOINER OWNS THE NPC'S POSE (v1067G). v1065G pinned only the host NPC's POSITION to the joiner's hands
 *    (its own anim / hit reactions kept playing on the host), and handed the proxy back to the host stream 1.5 s after the
 *    joiner's player stopped being busy -- mid-throw the proxy snapped to the host NPC's standing / reaction pose. Now:
 *    - guest: while a proxy is held, its own (local, paired / thrown) pose is captured after its anim update and sent to the
 *      host on blob channel 7 ([u16 netid][pose wire], 30 Hz); the last one is kept;
 *    - host: the followed NPC gets that pose in its post-skeleton hook (+ the re-apply after the anim update, + forced bone eval
 *      when culled) -> both screens show the same grab / throw;
 *    - the hold now ends only when the thrown proxy has SETTLED (root < 0.5 m/s for GF_COOP_GRAB_SETTLE_MS = 500, at most
 *      GF_COOP_GRAB_SETTLE_MAX_MS = 6000 after the player's grab ended; gf_coop_combat.c);
 *    - then the proxy cross-fades from its last local pose into the host stream over GF_COOP_GRAB_XFADE_MS (500) instead of
 *      snapping (gf_coop_wpose.c wp_apply).
 *    GF_COOP_GRAB_UPPOSE=1 enables pose upload (default off), GF_COOP_GRAB_XFADE_MS > 0 enables the cross-fade (default 0),
 *    GF_COOP_GRAB_SETTLE_MS=0 = old 1.5 s end rule.
 *
 * R  (gf_coop_worldre.c v1067R) ghost town: no factory refusal any more (phantom population budget entries), see there.
 *
 * Threads: gf_coop_lod_d2 / post_skel / after_anim / force_eval = main PPU thread; gf_npcfix_on_blob = co-op thread. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "gf_coop_pose.h"
#include "gf_memq.h"
#include "gf_coop_remote.h"   /* v1072W */

extern uint8_t* vm_base;
extern int gf_coop_connected(void);
extern int gf_coop_mode(void);                 /* 0 off, 1 host, 2 join, 3 echo */
extern int gf_coop_partner_pose(float out_m[16], float* hp);
extern int gf_coop_send_blob(uint8_t channel, const void* data, int len);
extern uint32_t gf_coop_pose_now_ms(void);
extern int gf_coop_pose_capture_any(uint32_t ent, uint8_t* out, int cap);
extern int gf_coop_pose_rewrite(uint32_t ent, uint32_t anim);
extern void gf_coop_pose_cache_forget(uint32_t ent);
extern uint32_t gf_rag_cur_ent(void);
extern uint16_t gf_proxy_netid_any(uint32_t ent);      /* gf_coop_proxy.c v1065G */
extern int gf_coop_grab_holds(uint32_t ent);           /* gf_coop_combat.c v1065G (guest) */
extern uint16_t gf_coop_grab_followed(uint32_t ent);   /* gf_coop_combat.c v1067G (host) */

enum { NF_UP_CH = 7, NF_UP = 4, NF_BUF = 4, NF_PKT = GF_COOP_POSE_MAX_PACKET + 2 };

static int is_host_side(void) { const int m = gf_coop_mode(); return m == 1 || m == 3; }
static int is_guest_side(void) { const int m = gf_coop_mode(); return m == 2 || m == 3; }
static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

/* ================================================================ L: host LOD */
static int s_lod = -1; static float s_lod_scale = 1.0f;
static volatile long st_lod_calls, st_lod_partner, st_lod_kept, st_lod_reload, st_lod_nopos, st_lod_fault;
static void lod_cfg(void)
{
    const char* e = getenv("GF_COOP_LOD_PARTNER"); s_lod = !(e && *e == '0');   /* v1068T: default ON (root cause of "no rig" host peds near the joiner); =0 old LOD */
    if ((e = getenv("GF_COOP_LOD_PARTNER_SCALE")) && atof(e) > 0.01) s_lod_scale = (float)atof(e);   /* distance factor for the joiner (< 1 = keeps more) */
    printf("[coop-npcfix] v1067L host LOD: NPC model/anim kept loaded near the JOINER too (min distance to host focus / joiner puppet, joiner distance x%.2f) %s (GF_COOP_LOD_PARTNER)\n",
           (double)s_lod_scale, s_lod ? "ON" : "off"); fflush(stdout);
}
/* hand edit right after func_00282850 at 0x00140520 (unload test, kind 0) and 0x00136E28 (reload test, kind 1) */
extern int gf_coop_world_guest_auth(void);   /* weak default in gf_coop_wpose.c (v1068T) */
static volatile long st_lod_proxy;
float gf_coop_lod_d2e(uint32_t ent, uint32_t pos, float d2, int kind);
float gf_coop_lod_d2(uint32_t pos, float d2, int kind) { return gf_coop_lod_d2e(0, pos, d2, kind); }   /* v1067L entry (no entity) */
/* v1068T: ent = the character (r30 at both call sites). Proxies of the OTHER peer's peds are never kept loaded for the
 * partner (the partner animates its own copy); on the JOINER the same keep-alive runs for its own natives near the HOST
 * when the world sync is two-way (agent R), so joiner-owned peds near the host keep their anims for the pose stream. */
float gf_coop_lod_d2e(uint32_t ent, uint32_t pos, float d2, int kind)
{
    static ULONGLONG s_t, s_log; static int s_ok; static float s_pp[3]; static gf_remote s_lr[GF_REMOTE_MAX]; static int s_lrn;   /* v1072W */
    ULONGLONG now; float p[3], dx, dy, dz, d2p;
    if (s_lod < 0) lod_cfg();
    { const int m = gf_coop_mode();
      if (!s_lod || !gf_coop_connected() || !(m == 1 || (m == 2 && gf_coop_world_guest_auth()))) return d2; }   /* host (+ joiner when two-way); in echo the partner is the local player */
    if (ent && gf_proxy_netid_any(ent)) { st_lod_proxy++; return d2; }   /* v1068T: a proxy = the other peer's ped */
    now = GetTickCount64();
    if (now - s_t >= 20) {
        s_t = now;
        s_lrn = gf_world_remotes(s_lr, GF_REMOTE_MAX);   /* v1072W: every remote player (validated there, v1067X) */
        s_ok = s_lrn > 0;
        if (s_ok) { s_pp[0] = s_lr[0].p[0]; s_pp[1] = s_lr[0].p[1]; s_pp[2] = s_lr[0].p[2]; }
    }
    st_lod_calls++;
    if (now - s_log >= 10000) { s_log = now;
        printf("[coop-npcfix] v1067L host LOD: %ld checks | joiner nearer %ld | kept loaded by the joiner (host focus > 40 m) %ld, reload checks passed by the joiner %ld | no joiner pos %ld, faults %ld | v1068T proxies skipped %ld (mode %d)\n",
               st_lod_calls, st_lod_partner, st_lod_kept, st_lod_reload, st_lod_nopos, st_lod_fault, st_lod_proxy, gf_coop_mode()); fflush(stdout); }
    if (!(d2 == d2)) return d2;   /* v1067X: preserve a bad original result; never turn NaN into a load decision */
    if (!s_ok) { st_lod_nopos++; return d2; }
    if (!gf_memq_access(vm_base, pos, 12, 0)) { st_lod_fault++; return d2; }
    __try { uint32_t u; const uint8_t* q = vm_base + pos;
        u = be32(q); memcpy(&p[0], &u, 4); u = be32(q + 4); memcpy(&p[1], &u, 4); u = be32(q + 8); memcpy(&p[2], &u, 4); }
    __except (EXCEPTION_EXECUTE_HANDLER) { st_lod_fault++; return d2; }
    if (!(p[0] == p[0]) || !(p[1] == p[1]) || !(p[2] == p[2]) ||
        fabsf(p[0]) >= 1e5f || fabsf(p[1]) >= 1e5f || fabsf(p[2]) >= 1e5f) { st_lod_fault++; return d2; }   /* v1067X */
    dx = p[0] - s_pp[0]; dy = p[1] - s_pp[1]; dz = p[2] - s_pp[2];
    d2p = dx * dx + dy * dy + dz * dz;
    for (int i = 1; i < s_lrn; i++) { const float ax = p[0] - s_lr[i].p[0], ay = p[1] - s_lr[i].p[1], az = p[2] - s_lr[i].p[2], a2 = ax * ax + ay * ay + az * az; if (a2 < d2p) d2p = a2; }   /* v1072W nearest remote */
    d2p *= s_lod_scale * s_lod_scale;
    if (!(d2p == d2p) || d2p >= d2) return d2;
    st_lod_partner++;
    if (kind == 0 && d2 > 1600.0f && d2p <= 1600.0f) st_lod_kept++;
    if (kind == 1 && d2 > 900.0f && d2p <= 900.0f) st_lod_reload++;
    return d2p;
}

/* ================================================================ D: v1070D PARTNER KEEP (agent D, 2026-11-06)
 * "Host walks away -> the NPCs standing next to the joiner vanish on the joiner's screen" (v1070B open item). The NPCs the
 * joiner sees there are the HOST's natives (proxies on the joiner); the host's population despawns them because its own
 * checks only know the host. RE (lifted code, ppu_recomp_001.cpp):
 *  - func_00284EE4(requester, 1, x) = the regular despawn sweep (called by the population manager 0x0034F04C/0x0034F084/
 *    0x00352680/0x003526B8/0x00353AF8/0x00353B30 with r4 = 1): every entity of the requester list (+0xEC + 8*i, handle-0x38)
 *    is removed (func_00284D88 -> func_00455F58) when it is OUT OF THE HOST CAMERA VIEW (func_00283DA4(pos, 1, ..) != 0) AND
 *    dist^2 to the host focus (func_0046C5F8(*(TOC+0x93C))) >= (*(requester+0xA8))^2. (r4 = 0 = "remove all": dtor / zone off.)
 *  - func_00287124(requester, n, ..) = over-budget cull (called by the slot append func_00287414 when the requester list is
 *    full, and by the density reduction 0x002885A4 / 0x0028891C / 0x002CEF9C): removes the first OUT-OF-VIEW entities -- with
 *    the host walking on, very often a ped next to the joiner.
 * Fix (hand edits, _patches/apply-pending.ps1 block v1070D): both sites ask gf_coop_pop_keep(ent, site) first; 1 = this
 * native is SHOWN on the partner (gf_coop_world_src_kind: tracked + spawn sent) and stands within GF_COOP_POP_KEEP_R (45 m,
 * peds) / GF_COOP_POP_KEEP_CAR_R (80 m, cars) of the partner's player -> the sweep treats it as "inside the radius" and the
 * cull skips it. The NPC keeps its real brain/AI on the owner, so the partner keeps seeing it walk on; once it is beyond the
 * radius (it walked off or the partner left) the game removes it as before -- at that distance the partner's character LOD
 * (40 m) has already stripped its model, so the removal is invisible. At most GF_COOP_POP_KEEP_MAX (24) entities are held at
 * a time (they use the owner's population budget). Runs on the HOST for its natives and on the JOINER for the natives it
 * sources in the zone-authority mode (mirror case: the joiner walks away from its NPCs standing next to the host).
 * GF_COOP_POP_KEEP=0 = old (hand edits inert), =2 also in echo (partner = the local player; test aid only).
 * Thread: main PPU thread (population update), same as host_tick. */
extern int gf_coop_world_src_kind(uint32_t ent);   /* gf_coop_world.c v1070D */
static int s_pk = -1, s_pk_max = 24; static float s_pk_r = 45.0f, s_pk_car_r = 80.0f;
typedef struct { uint32_t ent; ULONGLONG first, last; } pk_slot;
static pk_slot s_pks[64];
static volatile long st_pk_seen[2], st_pk_calls[2], st_pk_kept[2], st_pk_new, st_pk_cap, st_pk_unsynced, st_pk_expired, st_pk_fault, st_pk_nopos;
static long st_pk_held_max; static ULONGLONG st_pk_hold_ms_max;
static void pk_cfg(void)
{
    const char* e = getenv("GF_COOP_POP_KEEP"); s_pk = e && *e ? atoi(e) : 1;
    if ((e = getenv("GF_COOP_POP_KEEP_R")) && atof(e) > 1.0) s_pk_r = (float)atof(e);
    if ((e = getenv("GF_COOP_POP_KEEP_CAR_R")) && atof(e) > 1.0) s_pk_car_r = (float)atof(e);
    if ((e = getenv("GF_COOP_POP_KEEP_MAX")) && atoi(e) > 0) s_pk_max = atoi(e) > 64 ? 64 : atoi(e);
    printf("[coop-npcfix] v1070D partner keep: the game's population does not despawn our synced NPCs/cars within %.0f m (cars %.0f m) of the PARTNER's player (max %d held) %s (GF_COOP_POP_KEEP)\n",
           (double)s_pk_r, (double)s_pk_car_r, s_pk_max, s_pk == 1 ? "ON" : s_pk == 2 ? "ON (+ echo test)" : "off"); fflush(stdout);
}
static int pk_partner(float p[3])   /* partner player position, cached 20 ms */
{
    static ULONGLONG s_t; static int s_ok; static float s_p[3];
    const ULONGLONG now = GetTickCount64();
    if (now - s_t >= 20) {
        float m[16]; s_t = now;
        s_ok = gf_coop_partner_pose(m, NULL) && m[12] == m[12] && m[13] == m[13] && m[14] == m[14] &&
               fabsf(m[12]) < 1e5f && fabsf(m[13]) < 1e5f && fabsf(m[14]) < 1e5f;
        if (s_ok) { s_p[0] = m[12]; s_p[1] = m[13]; s_p[2] = m[14]; }
    }
    if (s_ok) { p[0] = s_p[0]; p[1] = s_p[1]; p[2] = s_p[2]; }
    return s_ok;
}
static void pk_log(void)
{
    static ULONGLONG s_log; const ULONGLONG now = GetTickCount64(); int held = 0;
    if (now - s_log < 10000) return;
    s_log = now;
    if (!st_pk_calls[0] && !st_pk_calls[1] && !gf_coop_connected()) return;   /* single player / not connected yet: quiet */
    for (int i = 0; i < 64; i++) if (s_pks[i].ent && now - s_pks[i].last < 3000) held++;
    printf("[coop-npcfix] v1070D partner keep: despawn checks seen sweep %ld / cull %ld (active %ld / %ld) | kept sweep %ld, cull %ld | held now %d (max %ld, cap %d, longest %llu s), new %ld, let go %ld, at cap %ld | near partner but not synced %ld | no partner pos %ld, faults %ld\n",
           st_pk_seen[0], st_pk_seen[1], st_pk_calls[0], st_pk_calls[1], st_pk_kept[0], st_pk_kept[1], held, st_pk_held_max, s_pk_max,
           (unsigned long long)(st_pk_hold_ms_max / 1000), st_pk_new, st_pk_expired, st_pk_cap, st_pk_unsynced, st_pk_nopos, st_pk_fault); fflush(stdout);
}
/* v1071D (Claude) ZONE SPLIT, GF_COOP_ZONE_SPLIT (1 default, 0 = v1071C behaviour). User clip 15:06 (joiner 155-180 m from the host):
 * the host kept its own NPCs alive next to the far joiner (v1071B shared view + v1070D keep) but cannot animate them there (host
 * "no rig 6 of 6 selected", "captured fresh" flat; joiner "rx" flat, "proxy without stream" growing) -> sliding statues, on top of
 * the joiner's own street life (v1068R zone authority beyond 130 m). With the partner beyond the zone radius, points nearer to the
 * PARTNER than to us belong to the partner's population: we no longer keep / un-cull ours there (they despawn; the receiver's
 * v1071D despawn hold hides that), and our spawn placement treats them as visible (no new spawns in the partner's half). */
extern float gf_world_release_radius(void);
extern int gf_coop_local_pos(float out[3]);
static int s_zsn = -1; static volatile long st_zs_pk, st_zs_spawn, st_zs_let;
static int zsn_on(void) { if (s_zsn < 0) { const char* e = getenv("GF_COOP_ZONE_SPLIT"); s_zsn = !(e && *e == '0');
    printf("[coop-npcfix] v1071D zone split: partner beyond the zone radius -> its half of the street is its own (no keep / un-cull, no spawns there) %s (GF_COOP_ZONE_SPLIT)\n", s_zsn ? "ON" : "off"); fflush(stdout); } return s_zsn; }
static int zsn_far(const float me[3], const float pp[3])
{
    extern float gf_world_far_radius(void);
    float r = 60.0f; __try { r = gf_world_far_radius(); } __except (EXCEPTION_EXECUTE_HANDLER) { r = 60.0f; }   /* v1072Z (was the 130 m release radius) */
    { const float dx = pp[0] - me[0], dz = pp[2] - me[2]; return dx * dx + dz * dz > r * r; }
}
/* v1101P solo keep (user: NPCs "despawn so quickly when I turn my back"): the game's sweep removes a street NPC / car as soon as
 * it is out of the camera view and beyond the requester radius. Out of view but within GF_POP_KEEP_R (60 m) of the local player
 * = kept, any mode (site 0 only: the over-budget cull still makes room, so spawning never stalls). GF_POP_KEEP_R=0 off. */
static float s_pop_keep_r = -1.0f; static volatile long st_pop_keep, st_pop_seen;
static int pos_of(uint32_t ent, float p[3])
{
    uint32_t xf = 0;
    if (!ent || !gf_memq_access(vm_base, ent + 0x68u, 4, 0)) return 0;
    __try { xf = be32(vm_base + ent + 0x68u); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (xf < 0x10000u || xf >= 0xF0000000u || !gf_memq_access(vm_base, xf + 0x40u, 12, 0)) return 0;
    __try { uint32_t u; const uint8_t* q = vm_base + xf + 0x40u;
        u = be32(q); memcpy(&p[0], &u, 4); u = be32(q + 4); memcpy(&p[1], &u, 4); u = be32(q + 8); memcpy(&p[2], &u, 4); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return p[0] == p[0] && p[1] == p[1] && p[2] == p[2] && fabsf(p[0]) < 1e5f && fabsf(p[1]) < 1e5f && fabsf(p[2]) < 1e5f;
}
static int solo_keep(uint32_t ent)
{
    if (s_pop_keep_r < 0.0f) { const char* e = getenv("GF_POP_KEEP_R"); s_pop_keep_r = e && *e ? (float)atof(e) : 60.0f;
        printf("[pop] v1101P out-of-view street NPCs / cars within %.0f m of the player are not despawned (GF_POP_KEEP_R)\n", (double)s_pop_keep_r); fflush(stdout); }
    if (!(s_pop_keep_r > 1.0f)) return 0;
    st_pop_seen++;
    static ULONGLONG t_pl; static float pl[3]; static int pl_ok; const ULONGLONG now = GetTickCount64();
    if (now - t_pl > 50) { t_pl = now; pl_ok = 0;
        uint32_t a = 0;
        if (gf_memq_access(vm_base, 0x10181290u, 4, 0)) { __try { a = be32(vm_base + 0x10181290u); if (a >= 0x10000u && a < 0xF0000000u) a = be32(vm_base + a + 4u); else a = 0;
            if (a >= 0x10000u && a < 0xF0000000u) a = be32(vm_base + a); else a = 0; } __except (EXCEPTION_EXECUTE_HANDLER) { a = 0; } }
        if (a >= 0x17FCu + 0x10000u && a < 0xF0000000u) pl_ok = pos_of(a - 0x17FCu, pl); }
    float p[3]; if (!pl_ok || !pos_of(ent, p)) return 0;
    const float dx = p[0] - pl[0], dy = p[1] - pl[1], dz = p[2] - pl[2];
    if (dx * dx + dy * dy + dz * dz >= s_pop_keep_r * s_pop_keep_r) return 0;
    st_pop_keep++;
    { static ULONGLONG lt; if (now - lt > 10000) { lt = now; printf("[pop] v1101P despawn sweep: %ld checks, %ld kept near the player\n", st_pop_seen, st_pop_keep); fflush(stdout); } }
    return 1;
}
/* hand edits in func_00284EE4 (site 0, right before the radius compare) and func_00287124 (site 1, at loc_0028733C). 1 = keep. */
static int pop_seen_ent(uint32_t ent, int site);   /* v1109W-2, below */
/* v1127G3 (lead): combat keep. c27-23: B took net 32817 by a combat control request and B's population culled it at once
 * ("cull (culled here)") -> handed straight back. An NPC taken for a fight is kept from the sweep / over-budget cull for
 * GF_COOP_COMBAT_KEEP_MS (default 20000, 0 off); combat code may refresh it. */
static struct { uint32_t ent; ULONGLONG until; } s_ck[64]; static long st_ck_add, st_ck_kept, st_ck_evict, st_ck_drop;   /* v1127G6 32 -> 64 (world syncs up to 64) */
void gf_coop_pop_combat_drop(uint32_t ent)   /* v1127G6: ped died / handed away / became a corpse -> a reused pointer must not stay protected */
{
    if (!ent) return;
    for (int i = 0; i < 64; i++) if (s_ck[i].ent == ent) { s_ck[i].ent = 0; s_ck[i].until = 0; st_ck_drop++;
        { static int nl; if (nl++ < 20) { printf("[pop] v1127G6 combat keep: ent %08X dropped (%ld drops, %ld active evictions)\n", ent, st_ck_drop, st_ck_evict); fflush(stdout); } } }
}
void gf_coop_pop_combat_keep(uint32_t ent, unsigned ms)
{
    static int def = -1; if (def < 0) { const char* e = getenv("GF_COOP_COMBAT_KEEP_MS"); def = e ? atoi(e) : 20000; if (def < 0) def = 0; }
    if (!ent || !def) return;
    if (!ms) ms = (unsigned)def;
    const ULONGLONG now = GetTickCount64(); int fr = -1, old = 0;
    for (int i = 0; i < 64; i++) { if (s_ck[i].ent == ent) { s_ck[i].until = now + ms; return; } if (fr < 0 && (!s_ck[i].ent || s_ck[i].until <= now)) fr = i; if (s_ck[i].until < s_ck[old].until) old = i; }
    if (fr < 0) { fr = old; st_ck_evict++; }   /* v1127G6 counted */
    s_ck[fr].ent = ent; s_ck[fr].until = now + ms; st_ck_add++;
    { static int nl; if (nl++ < 20) { printf("[pop] v1127G3 combat keep: ent %08X protected from culling for %u ms\n", ent, ms); fflush(stdout); } }
}
int gf_coop_pop_combat_kept(uint32_t ent)   /* v1127G4: query only (world migration) */
{
    const ULONGLONG now = GetTickCount64();
    for (int i = 0; i < 64; i++) if (s_ck[i].ent == ent && ent) return s_ck[i].until > now;
    return 0;
}
static int combat_kept(uint32_t ent)
{
    const ULONGLONG now = GetTickCount64();
    for (int i = 0; i < 64; i++) if (s_ck[i].ent == ent) { if (s_ck[i].until > now) { st_ck_kept++; { static ULONGLONG lt; if (now - lt > 5000) { lt = now; printf("[pop] v1127G3 combat keep: cull of %08X refused (%ld refusals, %ld keeps)\n", ent, st_ck_kept, st_ck_add); fflush(stdout); } } return 1; } s_ck[i].ent = 0; return 0; }
    return 0;
}
int gf_coop_pop_keep(uint32_t ent, int site)
{
    if ((site == 0 || site == 1) && combat_kept(ent)) return 1;   /* v1127G3 */
    if ((site == 0 || site == 1) && pop_seen_ent(ent, site)) return 1;   /* v1109W-2: nothing the player can see goes (sweep + over-budget cull) */
    if (site == 0 && solo_keep(ent)) return 1;   /* v1101P */
    float pp[3], p[3], dx, dy, dz, d2, r; int kind, m, active = 0, fr = -1; ULONGLONG now;
    if (s_pk < 0) pk_cfg();
    if (site < 0 || site > 1) return 0;
    st_pk_seen[site]++;
    pk_log();
    if (s_pk <= 0 || !ent || !gf_coop_connected()) return 0;
    m = gf_coop_mode();
    if (!(m == 1 || (m == 2 && gf_coop_world_guest_auth()) || (m == 3 && s_pk == 2))) return 0;
    st_pk_calls[site]++;
    (void)pk_partner;   /* v1072W: the nearest remote player is picked below, after the entity position */
    {   uint32_t xf = 0;
        if (!gf_memq_access(vm_base, ent + 0x68u, 4, 0)) { st_pk_fault++; return 0; }
        __try { xf = be32(vm_base + ent + 0x68u); } __except (EXCEPTION_EXECUTE_HANDLER) { st_pk_fault++; return 0; }
        if (xf < 0x10000u || xf >= 0xF0000000u || !gf_memq_access(vm_base, xf + 0x40u, 12, 0)) { st_pk_fault++; return 0; }
        __try { uint32_t u; const uint8_t* q = vm_base + xf + 0x40u;
            u = be32(q); memcpy(&p[0], &u, 4); u = be32(q + 4); memcpy(&p[1], &u, 4); u = be32(q + 8); memcpy(&p[2], &u, 4); }
        __except (EXCEPTION_EXECUTE_HANDLER) { st_pk_fault++; return 0; } }
    if (!(p[0] == p[0]) || !(p[1] == p[1]) || !(p[2] == p[2]) || fabsf(p[0]) >= 1e5f || fabsf(p[1]) >= 1e5f || fabsf(p[2]) >= 1e5f) { st_pk_fault++; return 0; }
    {   gf_remote rr[GF_REMOTE_MAX]; const int nr = gf_world_remotes(rr, GF_REMOTE_MAX); int bi = -1; float bd = 0.0f;   /* v1072W: nearest remote player */
        for (int i = 0; i < nr; i++) { const float ax = p[0] - rr[i].p[0], ay = p[1] - rr[i].p[1], az = p[2] - rr[i].p[2], a2 = ax * ax + ay * ay + az * az; if (bi < 0 || a2 < bd) { bi = i; bd = a2; } }
        if (bi < 0) { st_pk_nopos++; return 0; }
        pp[0] = rr[bi].p[0]; pp[1] = rr[bi].p[1]; pp[2] = rr[bi].p[2];
        if (zsn_on()) { float me[3]; if (gf_coop_local_pos(me) && zsn_far(me, pp)) { InterlockedIncrement(&st_zs_pk); return 0; } } }   /* v1071D */
    dx = p[0] - pp[0]; dy = p[1] - pp[1]; dz = p[2] - pp[2]; d2 = dx * dx + dy * dy + dz * dz;
    r = s_pk_r > s_pk_car_r ? s_pk_r : s_pk_car_r;
    if (!(d2 < r * r)) return 0;
    kind = gf_coop_world_src_kind(ent);
    if (!kind) { if (d2 < s_pk_r * s_pk_r) st_pk_unsynced++; return 0; }
    r = kind == 2 /* GF_WK_CAR */ ? s_pk_car_r : s_pk_r;
    if (!(d2 < r * r)) return 0;
    now = GetTickCount64();
    for (int i = 0; i < 64; i++) {
        if (s_pks[i].ent == ent && now - s_pks[i].last < 3000) {
            s_pks[i].last = now; st_pk_kept[site]++;
            if (now - s_pks[i].first > st_pk_hold_ms_max) st_pk_hold_ms_max = now - s_pks[i].first;
            return 1; }
        if (s_pks[i].ent && now - s_pks[i].last >= 3000) { s_pks[i].ent = 0; st_pk_expired++; }   /* not asked for 3 s: in view / near the owner again, or gone */
        if (s_pks[i].ent) active++; else if (fr < 0) fr = i;
    }
    if (active >= s_pk_max || fr < 0) { st_pk_cap++; return 0; }
    s_pks[fr].ent = ent; s_pks[fr].first = s_pks[fr].last = now; st_pk_new++; st_pk_kept[site]++;
    if (active + 1 > st_pk_held_max) st_pk_held_max = active + 1;
    { static int nl; if (nl++ < 12) { printf("[coop-npcfix] v1070D partner keep: %s %08X %.0f m from the partner -> not despawned (%s)\n",
        kind == 2 ? "car" : "ped", ent, (double)sqrtf(d2), site ? "over-budget cull" : "out-of-view sweep"); fflush(stdout); } }
    return 1;
}

/* ================================================================ G: grab pose upload / host apply / guest cross-fade */
static int s_up = -1, s_xfade_ms = 500;   /* v1068T: defaults ON (GF_COOP_GRAB_UPPOSE=0 / GF_COOP_GRAB_XFADE_MS=0 = old) */
static void up_cfg(void)
{
    const char* e = getenv("GF_COOP_GRAB_UPPOSE"); s_up = e && *e == '1';   /* v1068T2: opt-in again -- t68-1-echo (=1) aborted at 0x00ABBF14 ~3 s after a throw, t68-2 (=0) ran clean; =1 to test */
    if ((e = getenv("GF_COOP_GRAB_XFADE_MS")) && atoi(e) >= 0) s_xfade_ms = atoi(e);
    printf("[coop-npcfix] v1067G grabbed NPC: joiner's pose uploaded + applied on the host NPC %s (GF_COOP_GRAB_UPPOSE), hand-back cross-fade %d ms (GF_COOP_GRAB_XFADE_MS)\n",
           s_up ? "ON" : "off", s_xfade_ms); fflush(stdout);
}
/* guest: last local pose per held proxy (for the upload and the cross-fade at hold end) */
typedef struct { uint16_t netid; uint32_t ent, last_tx, t_end; ULONGLONG mv_tick; int len; uint8_t d[GF_COOP_POSE_MAX_PACKET]; } nf_hx;
static nf_hx s_hx[NF_UP]; static unsigned s_hx_i;
static volatile long st_up_tx, st_up_capfail, st_xf_apply, st_xf_local_only;
/* host: upstream streams */
typedef struct { uint32_t ms; int len; uint8_t d[GF_COOP_POSE_MAX_PACKET]; } nf_pkt;
typedef struct { uint16_t netid; int n; ULONGLONG last_rx; double off; int off_ok; nf_pkt b[NF_BUF]; } nf_stream;
static nf_stream s_us[NF_UP]; static SRWLOCK s_ulock = SRWLOCK_INIT;
static volatile long st_up_rx, st_up_apply, st_up_rig, st_up_none, st_up_reapply;
static uint32_t s_ap_ent[NF_UP], s_ap_anim[NF_UP];   /* host: anim of the followed NPC seen in post-skel (for the re-apply) */

static nf_hx* hx_get(uint16_t id, int create)
{
    for (int i = 0; i < NF_UP; i++) if (s_hx[i].netid == id) return &s_hx[i];
    if (!create) return 0;
    { nf_hx* h = &s_hx[s_hx_i++ % NF_UP]; memset(h, 0, sizeof *h); h->netid = id; return h; }
}
static void log_tick(void)
{
    static ULONGLONG s_log; const ULONGLONG now = GetTickCount64();
    if (now - s_log < 10000) return;
    s_log = now;
    if (!st_up_tx && !st_up_rx && !st_xf_apply && !st_up_capfail) return;
    printf("[coop-npcfix] v1067G grab pose: guest tx %ld (capture failed %ld), hand-back cross-fades %ld (local pose only %ld) | host rx %ld, applied %ld (rig mismatch %ld, nothing fresh %ld), re-applied %ld\n",
           st_up_tx, st_up_capfail, st_xf_apply, st_xf_local_only, st_up_rx, st_up_apply, st_up_rig, st_up_none, st_up_reapply); fflush(stdout);
}

/* max bone displacement (model space, m) between two pose wires of the same rig; -1 = not comparable */
static float pose_delta(const uint8_t* a, int la, const uint8_t* b, int lb)
{
    float mx = 0.0f; int slots;
    if (la != lb || la < 16 + 13) return -1.0f;
    slots = (la - 16) / 13;
    for (int i = 0; i < slots; i++) {
        const uint8_t* x = a + 16 + i * 13 + 7; const uint8_t* y = b + 16 + i * 13 + 7; float d2 = 0.0f;
        for (int k = 0; k < 3; k++) { const float d = (float)((int16_t)(x[k * 2] | (x[k * 2 + 1] << 8)) - (int16_t)(y[k * 2] | (y[k * 2 + 1] << 8))) / 4096.0f; d2 += d * d; }
        if (d2 > mx) mx = d2;
    }
    return sqrtf(mx);
}
/* gf_coop_combat.c settle rule: GetTickCount64 of the last time the held proxy's limbs moved > 0.6 m/s (0 = unknown) */
unsigned long long gf_npcfix_last_motion(uint16_t netid) { const nf_hx* h = hx_get(netid, 0); return h ? h->mv_tick : 0; }

/* gf_coop_combat.c HOLD OFF (main thread) */
void gf_npcfix_hold_end(uint16_t netid)
{
    nf_hx* h = hx_get(netid, 0);
    if (h && h->len) { h->t_end = gf_coop_pose_now_ms(); if (!h->t_end) h->t_end = 1; }
}

/* main PPU thread, right after func_0046316C(ent) (gf_wpose_after_anim) */
void gf_npcfix_after_anim(uint32_t ent)
{
    if (s_up < 0) up_cfg();
    log_tick();
    if (!ent || !gf_coop_connected()) return;
    if (is_host_side() && s_up) {   /* re-apply the joiner's pose after the anim update (look-at / IK must not win) */
        for (int k = 0; k < NF_UP; k++) if (s_ap_ent[k] == ent) {
            if (gf_coop_grab_followed(ent) && gf_coop_pose_rewrite(ent, s_ap_anim[k])) st_up_reapply++;
            s_ap_ent[k] = 0; break; }
    }
    if (is_guest_side() && gf_coop_grab_holds(ent)) {
        const uint16_t id = gf_proxy_netid_any(ent); const uint32_t now = gf_coop_pose_now_ms(); nf_hx* h; uint8_t pkt[NF_PKT]; int n;
        if (!id) return;
        h = hx_get(id, 1);
        if (h->ent != ent) { h->ent = ent; h->len = 0; h->last_tx = 0; }
        h->t_end = 0;
        if (h->last_tx && now - h->last_tx < 33u) return;
        n = gf_coop_pose_capture_any(ent, pkt + 2, (int)sizeof pkt - 2);
        if (n <= 0) { st_up_capfail++; h->last_tx = now; return; }
        if (h->len && h->last_tx && now > h->last_tx) {   /* limb motion for the settle rule */
            const float d = pose_delta(pkt + 2, n, h->d, h->len);
            if (d < 0.0f || d * 1000.0f / (float)(now - h->last_tx) > 0.6f) h->mv_tick = GetTickCount64(); }
        else h->mv_tick = GetTickCount64();
        memcpy(h->d, pkt + 2, (size_t)n); h->len = n; h->last_tx = now;
        if (s_up) { pkt[0] = (uint8_t)id; pkt[1] = (uint8_t)(id >> 8); if (gf_coop_send_blob(NF_UP_CH, pkt, n + 2)) st_up_tx++; }
    }
}

/* co-op thread (gf_coop_glue.c on_blob, channel 7): copy only */
void gf_npcfix_on_blob(uint8_t ch, const void* data, int len)
{
    const uint8_t* p = (const uint8_t*)data; const int pl = len - 2; uint16_t id; uint32_t ms; int slot = -1;
    if (ch != NF_UP_CH || !p || pl < 16 || pl > GF_COOP_POSE_MAX_PACKET || !is_host_side()) return;   /* v1067X */
    id = (uint16_t)(p[0] | (p[1] << 8));
    if (!id) return;   /* v1067X: net id 0 is never a proxy */
    ms = (uint32_t)p[14] | ((uint32_t)p[15] << 8) | ((uint32_t)p[16] << 16) | ((uint32_t)p[17] << 24);   /* pose header +12 (LE, as gf_coop_wpose.c) */
    AcquireSRWLockExclusive(&s_ulock);
    for (int i = 0; i < NF_UP; i++) if (s_us[i].n && s_us[i].netid == id) { slot = i; break; }
    if (slot < 0) { ULONGLONG oldest = ~0ull;
        for (int i = 0; i < NF_UP; i++) { if (!s_us[i].n) { slot = i; break; } if (s_us[i].last_rx < oldest) { oldest = s_us[i].last_rx; slot = i; } }
        memset(&s_us[slot], 0, sizeof s_us[slot]); s_us[slot].netid = id; }
    {
        nf_stream* s = &s_us[slot]; const double now = (double)gf_coop_pose_now_ms(), off = now - (double)ms;
        if (s->n && (int32_t)(ms - s->b[s->n - 1].ms) <= 0) { ReleaseSRWLockExclusive(&s_ulock); return; }
        if (!s->off_ok || off < s->off || off - s->off > 1000.0) { s->off = off; s->off_ok = 1; } else s->off += 0.02;
        if (s->n == NF_BUF) { memmove(&s->b[0], &s->b[1], sizeof(nf_pkt) * (NF_BUF - 1)); s->n--; }
        s->b[s->n].ms = ms; s->b[s->n].len = pl; memcpy(s->b[s->n].d, p + 2, (size_t)pl); s->n++;
        s->last_rx = GetTickCount64();
    }
    ReleaseSRWLockExclusive(&s_ulock);
    InterlockedIncrement(&st_up_rx);
}

/* host: main PPU thread, post-skeleton hook (gf_wpose_post_skel) */
void gf_npcfix_post_skel(uint32_t anim)
{
    static uint8_t pa[GF_COOP_POSE_MAX_PACKET], pb[GF_COOP_POSE_MAX_PACKET];
    uint32_t ent; uint16_t id; int la = 0, lb = 0, r; float u = 1.0f;
    if (s_up < 0) up_cfg();
    if (!s_up || !is_host_side() || !gf_coop_connected()) return;
    ent = gf_rag_cur_ent();
    if (!ent || !(id = gf_coop_grab_followed(ent))) return;
    AcquireSRWLockShared(&s_ulock);
    for (int i = 0; i < NF_UP; i++) {
        const nf_stream* s = &s_us[i];
        if (!s->n || s->netid != id || GetTickCount64() - s->last_rx > 600) continue;
        {   /* 50 ms behind the newest upstream pose, interpolated */
            const double t = (double)gf_coop_pose_now_ms() - s->off - 50.0; int k = 0;
            while (k + 1 < s->n && (double)s->b[k + 1].ms <= t) k++;
            if (k + 1 < s->n && (double)s->b[k].ms <= t) {
                la = s->b[k].len; memcpy(pa, s->b[k].d, (size_t)la); lb = s->b[k + 1].len; memcpy(pb, s->b[k + 1].d, (size_t)lb);
                u = (float)((t - (double)s->b[k].ms) / (double)(int32_t)(s->b[k + 1].ms - s->b[k].ms));
                if (!(u >= 0.0f)) u = 0.0f; if (u > 1.0f) u = 1.0f;
            } else { const nf_pkt* q = (double)s->b[0].ms > t ? &s->b[0] : &s->b[s->n - 1]; lb = q->len; memcpy(pb, q->d, (size_t)lb); }
        }
        break;
    }
    ReleaseSRWLockShared(&s_ulock);
    if (!lb) { st_up_none++; return; }
    { gf_pose_pair S; S.pa = la ? pa : NULL; S.la = la; S.pb = pb; S.lb = lb; S.u = u;
      r = gf_coop_pose_apply_mix(ent, anim, &S, NULL, 0.0f, 0); }
    if (r > 0) { int k, f = -1; st_up_apply++;
        for (k = 0; k < NF_UP; k++) { if (s_ap_ent[k] == ent) break; if (!s_ap_ent[k] && f < 0) f = k; }
        if (k == NF_UP) k = f >= 0 ? f : 0;
        s_ap_ent[k] = ent; s_ap_anim[k] = anim; }
    else { gf_coop_pose_cache_forget(ent); if (r < 0) st_up_rig++; else st_up_none++; }
}

/* host: gf_wpose_force_eval -- the followed NPC always gets the full bone eval (culled on the host screen) */
int gf_npcfix_force_eval(uint32_t ent)
{
    if (s_up < 0) up_cfg();   /* v1067X: this hook runs before after_anim on the first relevant update */
    return s_up == 1 && ent && is_host_side() && gf_coop_grab_followed(ent) != 0;
}

/* guest: gf_coop_wpose.c wp_apply asks for the hand-back cross-fade of proxy id. Returns the weight of the LOCAL (last held)
 * pose in (0,1], 0 = none. *pose / *len = that pose. */
float gf_npcfix_handback(uint16_t id, const uint8_t** pose, int* len)
{
    nf_hx* h; uint32_t dt;
    if (s_up < 0) up_cfg();
    if (s_xfade_ms <= 0 || !(h = hx_get(id, 0)) || !h->t_end || !h->len) return 0.0f;
    dt = gf_coop_pose_now_ms() - h->t_end;
    if (dt >= (uint32_t)s_xfade_ms) { h->t_end = 0; return 0.0f; }
    *pose = h->d; *len = h->len;
    {   const float x = 1.0f - (float)dt / (float)s_xfade_ms; return x * x * (3.0f - 2.0f * x); }   /* smoothstep */
}
void gf_npcfix_note_handback(int local_only) { st_xf_apply++; if (local_only) st_xf_local_only++; }

/* ================================================================ v1071B SHARED VIEW (Claude, 2026-11-06)
 * User 13:51: "a lot of NPCs popping in/out when the joiner has gone away from the host". The owner's population decides what to
 * despawn and where to spawn with ONE view test, func_00283DA4(pos, ..) != 0 = "out of the camera view" (host camera only): the
 * despawn sweep func_00284EE4, the over-budget cull func_00287124, the ambient spawn placement (0x00338FA4 / 0x00339164 /
 * 0x0033C068), the corpse cleanup func_002FDAA0 and ~15 more "only when the player can't see it" decisions. So NPCs in front of the
 * joiner were removed and new ones spawned there (v1070D only patched 2 removal sites with a cap of 24 -- the host log showed it at
 * the cap 43 times). Now func_00283DA4 is wrapped (apply-pending.ps1 block v1071B): when the game answers "out of view", it is
 * "in view" if the PARTNER can see the point -- within GF_COOP_SHARED_VIEW_NEAR (22 m) of the partner's player in any direction, or
 * within GF_COOP_SHARED_VIEW_FAR (160 m) inside the partner camera's cone (+-65 deg; camera direction from the partner's state packet,
 * v1071B gf_coop.c; without it the partner's body facing +-90 deg). No line of sight test (walls) -> errs on keeping / not spawning.
 * Host and joiner (each for its own population). GF_COOP_SHARED_VIEW=0 = old, =2 = also in echo (partner = the echo twin; test aid).
 * Any PPU thread: only cached reads + atomics. */
static int s_sv = -1; static float s_sv_near = 22.0f, s_sv_far = 160.0f, s_sv_cos = 0.42f, s_sv_body_cos = 0.0f;
static volatile long st_sv_calls, st_sv_out, st_sv_near, st_sv_cone, st_sv_body, st_sv_site[4], st_sv_nopos, st_sv_nocam;
static void sv_cfg(void)
{
    const char* e = getenv("GF_COOP_SHARED_VIEW"); s_sv = e && *e ? atoi(e) : 1;
    if ((e = getenv("GF_COOP_SHARED_VIEW_NEAR")) && atof(e) >= 0.0) s_sv_near = (float)atof(e);
    if ((e = getenv("GF_COOP_SHARED_VIEW_FAR")) && atof(e) > 1.0) s_sv_far = (float)atof(e);
    printf("[coop-npcfix] v1071B shared view: the game's 'out of the player's view' test (spawn / despawn / cull / corpse cleanup) also counts the PARTNER's view (%.0f m around, %.0f m in the camera cone) %s (GF_COOP_SHARED_VIEW)\n",
           (double)s_sv_near, (double)s_sv_far, s_sv == 1 ? "ON" : s_sv == 2 ? "ON (+ echo test)" : "off"); fflush(stdout);
}
extern int gf_coop_partner_cam(float* fx, float* fz);   /* gf_coop.c v1071B */
/* v1101P-2 solo spawn guard (user, after v1101P: "I can see NPCs spawning in"). The game's view test func_00283DA4 is a cone around
 * the camera sized for the PS3's 16:9 picture; on the user's 21:9 screen (2560x1080) the sides are "out of view" to it, so street
 * NPCs spawn (and despawn) in plain sight there -- the doubled capacity made it frequent. When the game answers "out of view" for
 * the SPAWN placement (lr 0x00338FA4 / 0x00339164 / 0x0033C068) or the despawn sweep (0x0028504C), the point still counts as seen if
 * it is within GF_POP_VIEW_NEAR (30 m) of the player, or within GF_POP_VIEW_FAR (150 m) inside +-GF_POP_VIEW_DEG (75) of the camera
 * forward (from 3.5 m behind the player: third-person camera). No line of sight (walls) -> errs on not spawning. GF_POP_VIEW=0 off. */
extern int gf_coop_local_cam(float* fx, float* fz);   /* gf_coop.c v1071D: the game's view-test camera forward */
extern int (*volatile g_pad_menu_fn)(void);   /* gf_combat.c gf_pad_menu (v1043Y): the pause menu / map is open */
static int paused(void) { return g_pad_menu_fn && g_pad_menu_fn() > 0; }
/* v1109W: s_pv_far = how far IN VIEW nothing is despawned (250 m, was 150 -- people at the far end of a street popped out);
 * s_psv_seen_far = how far a visible spot is refused for a SPAWN (80 m; v1109V refused 150 m -> a straight street ahead could never
 * be filled: "ghost town in front of me"). */
static float s_psv_seen_far = 80.0f;
static int s_pv = -1; static float s_pv_near = 30.0f, s_pv_far = 110.0f, s_pv_cos = 0.26f;   /* v1109X: keep range 250 -> 110 m (far "seen" people filled the budget: 705 of 945 culls refused, ghost town ahead) */ static volatile long st_pv_spawn, st_pv_sweep, st_pv_q;
static int player_pos(float pl[3])
{
    static ULONGLONG t; static float c[3]; static int okc; const ULONGLONG now = GetTickCount64();
    if (now - t > 30) { t = now; okc = 0; uint32_t a = 0;
        if (gf_memq_access(vm_base, 0x10181290u, 4, 0)) { __try { a = be32(vm_base + 0x10181290u); if (a >= 0x10000u && a < 0xF0000000u) a = be32(vm_base + a + 4u); else a = 0;
            if (a >= 0x10000u && a < 0xF0000000u) a = be32(vm_base + a); else a = 0; } __except (EXCEPTION_EXECUTE_HANDLER) { a = 0; } }
        if (a >= 0x17FCu + 0x10000u && a < 0xF0000000u) okc = pos_of(a - 0x17FCu, c); }
    if (okc) { pl[0] = c[0]; pl[1] = c[1]; pl[2] = c[2]; }
    return okc;
}
static int solo_view(uint32_t pos, uint32_t lr)
{
    if (s_pv < 0) { const char* e = getenv("GF_POP_VIEW"); s_pv = !(e && *e == '0');
        if ((e = getenv("GF_POP_VIEW_NEAR")) && atof(e) >= 0.0) s_pv_near = (float)atof(e);
        if ((e = getenv("GF_POP_VIEW_FAR")) && atof(e) > 1.0) s_pv_far = (float)atof(e);
        if ((e = getenv("GF_POP_VIEW_DEG")) && atof(e) > 5.0 && atof(e) < 180.0) s_pv_cos = cosf((float)atof(e) * 0.0174533f);
        if ((e = getenv("GF_POP_SPAWN_SEEN_FAR")) && atof(e) >= 0.0) s_psv_seen_far = (float)atof(e);
        printf("[pop] v1101P-2 spawn guard: no street spawns / sweep despawns within %.0f m of the player or %.0f m in the camera cone (cos %.2f, wide screens) %s (GF_POP_VIEW)\n",
               (double)s_pv_near, (double)s_pv_far, (double)s_pv_cos, s_pv ? "ON" : "off"); fflush(stdout); }
    if (!s_pv) return 0;
    const int spawn = 0, sweep = lr == 0x0028504Cu || lr == 0x0028738Cu;   /* v1109V: + the over-budget cull; spawns -> gf_pop_view_override */
    if (!spawn && !sweep) return 0;
    if (paused()) { static volatile long n; n++; return 1; }   /* v1109W: the pause menu / map points the camera elsewhere -> nothing goes */
    float q[3], pl[3], fx, fz;
    if (!gf_memq_access(vm_base, pos, 12, 0)) return 0;
    __try { uint32_t u; const uint8_t* b = vm_base + pos; u = be32(b); memcpy(&q[0], &u, 4); u = be32(b + 4); memcpy(&q[1], &u, 4); u = be32(b + 8); memcpy(&q[2], &u, 4); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (!(q[0] == q[0]) || !(q[2] == q[2]) || fabsf(q[0]) >= 1e5f || fabsf(q[2]) >= 1e5f || !player_pos(pl)) return 0;
    st_pv_q++;
    const float dx = q[0] - pl[0], dz = q[2] - pl[2], d2 = dx * dx + dz * dz;
    int seen = d2 <= s_pv_near * s_pv_near;
    if (!seen && d2 <= s_pv_far * s_pv_far && gf_coop_local_cam(&fx, &fz)) {
        const float ox = dx + fx * 3.5f, oz = dz + fz * 3.5f, l = sqrtf(ox * ox + oz * oz);
        seen = l > 1e-3f && (ox * fx + oz * fz) / l >= s_pv_cos; }
    if (seen) { if (spawn) st_pv_spawn++; else st_pv_sweep++; }
    { static ULONGLONG lt; const ULONGLONG now = GetTickCount64(); if (now - lt > 10000) { lt = now;
        printf("[pop] v1101P-2 spawn guard: %ld 'out of view' answers checked, spawn points refused (seen) %ld, sweep despawns kept (on screen) %ld\n", st_pv_q, st_pv_spawn, st_pv_sweep); fflush(stdout); } }
    return seen;
}
int gf_coop_view_partner(uint32_t pos, uint32_t lr)   /* 1 = a REMOTE player sees pos (the wrapper then answers "in view"); v1072W: any of them */
{
    if (solo_view(pos, lr)) return 1;   /* v1101P-2 */
    static ULONGLONG s_t, s_log; static int s_nr, s_meok, s_ncam, s_nfar; static gf_remote s_r[GF_REMOTE_MAX]; static int s_far[GF_REMOTE_MAX]; static float s_me[3];
    ULONGLONG now; float q[3], d2[GF_REMOTE_MAX]; int near_i = -1;
    if (s_sv < 0) sv_cfg();
    if (!s_sv || !gf_coop_connected()) return 0;
    { const int m = gf_coop_mode(); if (!(m == 1 || m == 2 || (m == 3 && s_sv == 2))) return 0; }
    InterlockedIncrement(&st_sv_out);
    now = GetTickCount64();
    if (now - s_t >= 20) {   /* remote players / cameras / our position, cached 20 ms */
        int nr, nc = 0, nf = 0; s_t = now;
        nr = gf_world_remotes(s_r, GF_REMOTE_MAX);
        s_meok = gf_coop_local_pos(s_me);
        for (int i = 0; i < nr; i++) { s_far[i] = s_meok && zsn_on() && zsn_far(s_me, s_r[i].p); nc += s_r[i].cam != 0; nf += s_far[i]; }
        s_ncam = nc; s_nfar = nf; s_nr = nr;
    }
    if (now - s_log >= 10000) { s_log = now;
        printf("[coop-npcfix] v1071B shared view: 'out of view' answers %ld, turned visible because a remote player sees the point: within %.0f m %ld, in a remote camera cone %ld (body-facing fallback %ld) | by caller: despawn sweep %ld, budget cull %ld, spawn placement %ld, other %ld | remote players %d, camera dir received %d, no remote pos %ld\n",
               st_sv_out, (double)s_sv_near, st_sv_near, st_sv_cone, st_sv_body, st_sv_site[0], st_sv_site[1], st_sv_site[2], st_sv_site[3],
               s_nr, s_ncam, st_sv_nopos);
        printf("[coop-npcfix] v1071D zone split: %d of %d remote players BEYOND the zone | in their half: despawn / cull let through %ld, spawns refused %ld | partner keep skipped %ld\n",
               s_nfar, s_nr, st_zs_let, st_zs_spawn, st_zs_pk); fflush(stdout); }
    if (s_nr <= 0) { InterlockedIncrement(&st_sv_nopos); return 0; }
    if (!gf_memq_access(vm_base, pos, 12, 0)) return 0;
    __try { uint32_t u; const uint8_t* b = vm_base + pos; u = be32(b); memcpy(&q[0], &u, 4); u = be32(b + 4); memcpy(&q[1], &u, 4); u = be32(b + 8); memcpy(&q[2], &u, 4); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (!(q[0] == q[0]) || !(q[2] == q[2]) || fabsf(q[0]) >= 1e5f || fabsf(q[2]) >= 1e5f) return 0;
    {   const int nr = s_nr > GF_REMOTE_MAX ? GF_REMOTE_MAX : s_nr;
        const int site = lr == 0x0028504Cu ? 0 : lr == 0x0028738Cu ? 1 : (lr == 0x00338FA4u || lr == 0x00339164u || lr == 0x0033C068u) ? 2 : 3;
        for (int i = 0; i < nr; i++) { const float dx = q[0] - s_r[i].p[0], dz = q[2] - s_r[i].p[2]; d2[i] = dx * dx + dz * dz; if (near_i < 0 || d2[i] < d2[near_i]) near_i = i; }
        if (site == 2 && near_i >= 0 && s_meok && !s_far[near_i]) {   /* v1127W (M's request): with ownership migration every player spawns only in its OWN half */
            extern int gf_coop_world_migrate_on(void);
            const float mx = q[0] - s_me[0], mz = q[2] - s_me[2];
            if (d2[near_i] < mx * mx + mz * mz && gf_coop_world_migrate_on()) { InterlockedIncrement(&st_zs_spawn); return 1; } }
        if (near_i >= 0 && s_far[near_i] && s_meok) {   /* v1071D: the point is in a far remote player's half -> its population, not ours */
            const float mx = q[0] - s_me[0], mz = q[2] - s_me[2];
            if (d2[near_i] < mx * mx + mz * mz) {
                if (site == 2) { InterlockedIncrement(&st_zs_spawn); return 1; }   /* spawn placement: "visible" = no spawn */
                InterlockedIncrement(&st_zs_let); return 0; }
        }
        for (int i = 0; i < nr; i++) {
            const float dx = q[0] - s_r[i].p[0], dz = q[2] - s_r[i].p[2]; int hit = 0;
            if (d2[i] <= s_sv_near * s_sv_near) { hit = 1; InterlockedIncrement(&st_sv_near); }
            else if (d2[i] <= s_sv_far * s_sv_far) {
                if (s_r[i].cam) {   /* from a point 3.5 m behind the player along the camera direction (third-person camera) */
                    const float ox = dx + s_r[i].fwd[0] * 3.5f, oz = dz + s_r[i].fwd[1] * 3.5f, l = sqrtf(ox * ox + oz * oz);
                    if (l > 1e-3f && (ox * s_r[i].fwd[0] + oz * s_r[i].fwd[1]) / l >= s_sv_cos) { hit = 1; InterlockedIncrement(&st_sv_cone); }
                } else {
                    const float l = sqrtf(d2[i]);
                    InterlockedIncrement(&st_sv_nocam);
                    if (l > 1e-3f && (dx * s_r[i].bf[0] + dz * s_r[i].bf[1]) / l >= s_sv_body_cos) { hit = 1; InterlockedIncrement(&st_sv_body); }
                }
            }
            if (hit) { InterlockedIncrement(&st_sv_site[site]); return 1; }
        }
        return 0;
    }
}


/* ================================================================ v1109V spawn placement view (Claude, 2026-11-08)
 * User: "horrible pop in/out when I walk down a street -- no re/despawning when they're visible; when I turn a corner there should
 * already be NPCs walking". VERIFIED (lifted code): func_00283DA4(pos, ..) returns 1 = OUT of view, 0 = in view (cone test vs the
 * camera forward, TOC+0x93C focus). The three ambient spawn-point scorers (func_00338DE0 lr 338FA4, func_003390D0 lr 339164,
 * func_0033BCA8 lr 33C068) score  (r3 == 0 ? 0 : -500) - distance  and keep the MAX -> the game PREFERS spawn points IN VIEW.
 * v1101P-2's guard (and v1071B / v1071D's "spawn refused" = "visible") turned seen points into r3 = 0 = even MORE preferred.
 * Now at those three sites the answer is ours: 1 (= the -500 penalty) when the point can be seen -- within GF_POP_VIEW_NEAR (30 m) of
 * the player, or within GF_POP_VIEW_FAR (150 m) in the +-GF_POP_VIEW_DEG (75) camera cone with a clear line of sight from the camera
 * eye (3.5 m behind, 1.7 m up) to the point + 1 m through gf_collision.bin (buildings block it: corners and side streets are hidden) --
 * or a remote co-op player sees it (gf_coop_view_partner: remote part only, spawns no longer go through solo_view); else 0. LOS in
 * 12 m segments (few grid cells each), cached per spawn point 300 ms. Despawn sweep + budget cull keep anything in the cone
 * (solo_view, no LOS = conservative). GF_POP_SPAWN_VIS=0 = old behaviour. Log [pop]. */
extern int gf_rag_ray(const float* o, const float* d, float maxd, float* t, float* n);
static int s_psv = -1; static volatile long st_psv_q, st_psv_near, st_psv_cone_vis, st_psv_cone_occ, st_psv_behind, st_psv_far, st_psv_remote, st_psv_cache;
static struct { uint32_t pos; ULONGLONG t; int v; } s_psv_c[128];
static int los_clear(const float* a, const float* b)
{
    float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }; const float L = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (L < 0.5f) return 1; d[0] /= L; d[1] /= L; d[2] /= L;
    for (float s0 = 0.0f; s0 < L - 0.5f; s0 += 12.0f) {
        const float seg = (L - 0.5f - s0) < 12.0f ? (L - 0.5f - s0) : 12.0f; float o[3] = { a[0] + d[0] * s0, a[1] + d[1] * s0, a[2] + d[2] * s0 }, t = 0, n[3];
        if (seg > 0.01f && gf_rag_ray(o, d, seg, &t, n)) return 0;
    }
    return 1;
}
/* v1110A (user: "cars right next to me just disappear suddenly"). VERIFIED (lifted code + v1109Z release tracer: every on-screen
 * car release = func_00287610 spawn request -> func_0033EE8C -> func_0033DFD0 -> func_0033BCA8 -> func_00455F58 at 0x33C59C):
 * the "3 spawn-point scorers" func_00338DE0 / func_003390D0 / func_0033BCA8 score EXISTING entities (ent+0x34 bit 0, position
 * *(ent+0x68)+0x40) to pick a VICTIM to steal when the pool is full: score = prio*1000 - (view test says OUT of view ? 500 : 0)
 * - min(dist, 400), LOWEST = released (its driver dismissed) so the new request can have its slot. The game steals far, unseen
 * cars. v1109V answered "out of view" (1) for things the player CAN see and v1109S remapped near (< 45 m) to 330+ -> visible
 * and near cars were stolen first. Now at the car victim sites (33C068 view, 33C084 / 33C260 distance): seen (<= 30 m, or in
 * the camera cone <= GF_POP_CAR_VIEW_FAR 230 m, or paused) -> "in view" + distance 0 = never stolen by a same-priority request;
 * else "out of view" + the REAL distance. GF_POP_STEAL_FIX=0 old, 1 cars (default), 2 cars + peds (338FA4/339164, 338FC4/339094/
 * 339184/3391AC). Log [pop] v1110A. */
static float s_pv_car_far = -1.0f;   /* v1109Y (moved up for v1110A) */
static int s_stf = -1; static volatile long st_stf_seen, st_stf_hidden, st_stf_q;
int gf_pop_steal_fix_site(uint32_t lr)   /* 1 = victim-score site handled by v1110A */
{
    if (s_stf < 0) { const char* e = getenv("GF_POP_STEAL_FIX"); s_stf = e && *e ? atoi(e) : 1;
        printf("[pop] v1110A pool-steal victim scoring: entities the player can see are never stolen, hidden ones by real distance -- %s (GF_POP_STEAL_FIX)\n",
               s_stf <= 0 ? "off" : s_stf == 1 ? "cars" : "cars + peds"); fflush(stdout); }
    if (s_stf <= 0) return 0;
    if (lr == 0x0033C068u || lr == 0x0033C084u || lr == 0x0033C260u) return 1;
    if (s_stf >= 2 && (lr == 0x00338FA4u || lr == 0x00339164u || lr == 0x00338FC4u || lr == 0x00339094u || lr == 0x00339184u || lr == 0x003391ACu)) return 1;
    return 0;
}
int gf_pop_victim_seen(uint32_t pos)   /* 1 = the player can see this entity position (no LOS: errs on protecting) */
{
    float q[3], pl[3], fx, fz;
    if (!gf_memq_access(vm_base, pos, 12, 0)) return 0;
    __try { uint32_t u; const uint8_t* b = vm_base + pos; u = be32(b); memcpy(&q[0], &u, 4); u = be32(b + 4); memcpy(&q[1], &u, 4); u = be32(b + 8); memcpy(&q[2], &u, 4); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (!(q[0] == q[0]) || !(q[2] == q[2]) || fabsf(q[0]) >= 1e5f || fabsf(q[2]) >= 1e5f || !player_pos(pl)) return 0;
    if (s_pv < 0) solo_view(0, 0);
    if (s_pv_car_far < 0.0f) { const char* e = getenv("GF_POP_CAR_VIEW_FAR"); s_pv_car_far = e && atof(e) > 1.0 ? (float)atof(e) : 230.0f; }
    st_stf_q++;
    int seen = paused();
    const float dx = q[0] - pl[0], dz = q[2] - pl[2], d2 = dx * dx + dz * dz;
    if (!seen && d2 <= s_pv_near * s_pv_near) seen = 1;
    if (!seen && d2 <= s_pv_car_far * s_pv_car_far && gf_coop_local_cam(&fx, &fz)) {
        const float ox = dx + fx * 3.5f, oz = dz + fz * 3.5f, l = sqrtf(ox * ox + oz * oz);
        seen = l > 1e-3f && (ox * fx + oz * fz) / l >= s_pv_cos; }
    if (seen) st_stf_seen++; else st_stf_hidden++;
    { static ULONGLONG lt; const ULONGLONG now = GetTickCount64(); if (now - lt > 10000) { lt = now;
        printf("[pop] v1110A pool-steal victim checks %ld: protected (player sees them) %ld, stealable (hidden) %ld\n", st_stf_q, st_stf_seen, st_stf_hidden); fflush(stdout); } }
    return seen;
}
int gf_pop_view_override(uint32_t pos, uint32_t lr, int r3)
{
    (void)r3;
    if (gf_pop_steal_fix_site(lr)) return gf_pop_victim_seen(pos) ? 0 : 1;   /* v1110A: 0 = in view = no -500 = kept */
    if (!(lr == 0x00338FA4u || lr == 0x00339164u || lr == 0x0033C068u)) return -1;
    if (s_psv < 0) { const char* e = getenv("GF_POP_SPAWN_VIS"); s_psv = !(e && *e == '0');
        if (s_pv < 0) solo_view(0, 0);   /* reads GF_POP_VIEW_* */
        printf("[pop] v1109W spawn placement: points the player can see (%.0f m around, %.0f m in the camera cone with line of sight) are AVOIDED (the game preferred them); nothing in view despawns within %.0f m; nothing despawns while paused %s (GF_POP_SPAWN_VIS, GF_POP_SPAWN_SEEN_FAR)\n",
               (double)s_pv_near, (double)s_psv_seen_far, (double)s_pv_far, s_psv ? "ON" : "off"); fflush(stdout); }
    if (!s_psv) return -1;
    const ULONGLONG now = GetTickCount64(); const int slot = (int)((pos >> 4) * 2654435761u >> 25) & 127;
    InterlockedIncrement(&st_psv_q);
    { static ULONGLONG lt; if (now - lt > 10000) { lt = now;
        printf("[pop] v1109V spawn placement view: %ld answers -- seen: near %ld, in the cone with line of sight %ld, remote player %ld | hidden: in the cone but blocked %ld, outside the cone %ld, beyond %.0f m %ld | cached %ld\n",
               st_psv_q, st_psv_near, st_psv_cone_vis, st_psv_remote, st_psv_cone_occ, st_psv_behind, (double)s_psv_seen_far, st_psv_far, st_psv_cache);
        { extern void gf_pop_census_log(void); gf_pop_census_log(); } fflush(stdout); } }
    if (s_psv_c[slot].pos == pos && now - s_psv_c[slot].t < 300) { InterlockedIncrement(&st_psv_cache); return s_psv_c[slot].v; }
    float q[3], pl[3], fx = 0, fz = 1; int seen = 0;
    if (!gf_memq_access(vm_base, pos, 12, 0)) return -1;
    __try { uint32_t u; const uint8_t* b = vm_base + pos; u = be32(b); memcpy(&q[0], &u, 4); u = be32(b + 4); memcpy(&q[1], &u, 4); u = be32(b + 8); memcpy(&q[2], &u, 4); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    if (!(q[0] == q[0]) || !(q[2] == q[2]) || fabsf(q[0]) >= 1e5f || fabsf(q[2]) >= 1e5f || !player_pos(pl)) return -1;
    const float dx = q[0] - pl[0], dz = q[2] - pl[2], d2 = dx * dx + dz * dz;
    if (d2 <= s_pv_near * s_pv_near) { seen = 1; InterlockedIncrement(&st_psv_near); }
    else if (paused()) { }   /* v1109W: behind the pause menu nothing can be seen */
    else if (d2 > s_psv_seen_far * s_psv_seen_far) InterlockedIncrement(&st_psv_far);
    else if (gf_coop_local_cam(&fx, &fz)) {
        const float ox = dx + fx * 3.5f, oz = dz + fz * 3.5f, l = sqrtf(ox * ox + oz * oz);
        if (l > 1e-3f && (ox * fx + oz * fz) / l >= s_pv_cos) {
            /* v1109V-2: seen if ANY of feet / waist / head can be seen (one waist ray was stopped by lamp posts, parked cars, kerbs:
             * m56 soak 879 blocked vs 186 clear) */
            const float eye[3] = { pl[0] - fx * 3.5f, pl[1] + 1.9f, pl[2] - fz * 3.5f };
            const float hgt[3] = { 1.0f, 1.8f, 0.35f }; int vis = 0;
            for (int h = 0; h < 3 && !vis; h++) { const float tg[3] = { q[0], q[1] + hgt[h], q[2] }; vis = los_clear(eye, tg); }
            if (vis) { seen = 1; InterlockedIncrement(&st_psv_cone_vis); } else InterlockedIncrement(&st_psv_cone_occ);
        } else InterlockedIncrement(&st_psv_behind);
    } else { seen = 1; }   /* no camera direction yet: careful */
    if (!seen && gf_coop_connected() && gf_coop_view_partner(pos, lr)) { seen = 1; InterlockedIncrement(&st_psv_remote); }
    s_psv_c[slot].pos = pos; s_psv_c[slot].t = now; s_psv_c[slot].v = seen;
    return seen;   /* 1 = "out of view" to the scorer = the -500 penalty */
}

/* v1109W-2 (user video 01:17: a taxi + a man 10-20 m ahead vanished in plain view). VERIFIED (lifted code, func_00287124 = the
 * over-budget cull, run when a requester is over the game's capacity -- census: 24 alive vs capacity 3): its view-aware pass removes
 * IN-VIEW entities too (beyond a distance threshold, or when a flag test at ent+0x1818 fails), so answering "in view" (v1109V) does
 * not protect anyone. Every candidate first goes through gf_coop_pop_keep(ent, 1) (v1070D hand edit; keep = skip it). Now: keep when
 * the game is paused, or the entity is within GF_POP_VIEW_NEAR (30 m) of the player, or within GF_POP_VIEW_FAR (250 m) in the camera
 * cone (no line of sight: errs on keeping). Same for the sweep (site 0). */
static volatile long st_seen_keep[2], st_seen_q[2];
/* v1109Y (user, after v1109X: "NPCs fixed, but cars: I look down a street and they all disappear one by one"). Cars live out to
 * GF_POP_CAR_REACH (200 m, v1109S) but the in-view keep range was the peds' 110 m (v1109X) -> the full traffic requester's
 * over-budget cull took the far cars down the street, farthest first. Cars now keep to GF_POP_CAR_VIEW_FAR (230 m) in the camera
 * cone; peds unchanged (110 m). Cars drive out of the cone on their own, so this does not freeze the budget the way far peds did. */
extern int gf_world_is_car(uint32_t ent);   /* gf_coop_worldre.c v1109Y */
static volatile long st_car_q, st_car_keep, st_car_far_keep, st_car_go_cone;
static volatile long st_cf_n, st_cf_cone, st_cf_cone_far, st_cf_near, st_cf_nopos; static float st_cf_maxd;
static int car_in_cone(const float p[3], float* dist)
{
    float pl[3], fx, fz; if (!player_pos(pl)) return -1;
    const float dx = p[0] - pl[0], dz = p[2] - pl[2]; *dist = sqrtf(dx * dx + dz * dz);
    if (!gf_coop_local_cam(&fx, &fz)) return -1;
    const float ox = dx + fx * 3.5f, oz = dz + fz * 3.5f, l = sqrtf(ox * ox + oz * oz);
    return l > 1e-3f && (ox * fx + oz * fz) / l >= s_pv_cos;
}
/* called from gf_world_free_note (operator delete) for a factory car: where was it when it went? Log every 10 s. */
void gf_pop_car_freed(uint32_t ent)
{
    float p[3], d = 0.0f; st_cf_n++;
    if (!pos_of(ent, p)) st_cf_nopos++;
    else { const int c = car_in_cone(p, &d);
        if (d <= s_pv_near) st_cf_near++;
        else if (c == 1) { st_cf_cone++; if (d > s_pv_far) st_cf_cone_far++; if (d > st_cf_maxd) st_cf_maxd = d; } }
    { static ULONGLONG lt; const ULONGLONG now = GetTickCount64(); if (now - lt > 10000) { lt = now;
        printf("[pop] v1109Y cars deleted %ld: within %.0f m %ld, IN THE CAMERA CONE %ld (beyond %.0f m %ld, farthest %.0f m), no position %ld | cull/sweep: cars asked %ld, kept %ld (beyond the ped range %ld), let go in the cone %ld\n",
               st_cf_n, (double)s_pv_near, st_cf_near, st_cf_cone, (double)s_pv_far, st_cf_cone_far, (double)st_cf_maxd, st_cf_nopos,
               st_car_q, st_car_keep, st_car_far_keep, st_car_go_cone); fflush(stdout); } }
}
/* v1109Y car watch (main_tick, 10 Hz): cars are RELEASED (bit 0 of ent+0x34 cleared, func_00455F58) and pooled, rarely deleted,
 * so watch them directly: a car that was alive and on screen (<= 30 m, or <= 250 m in the camera cone) at the last check and is now
 * released / gone / moved > 25 m in 100 ms (slot re-used elsewhere) = "vanished in view", counted by distance. If the user still
 * sees cars vanish and these counters stay ~0, it is not the population code (render LOD / streaming). GF_POP_CARWATCH=0 off. */
extern int gf_world_cars(uint32_t* out, int max);
typedef struct { uint32_t ent; float p[3]; float d; int vis; } cw_ent;
static cw_ent s_cw[128]; static int s_cw_n, s_cw_on = -1;
static volatile long st_cw_van[4], st_cw_jump, st_cw_checks, st_cw_mass; static int st_cw_logged;
void gf_pop_car_watch(void)
{
    static ULONGLONG t; const ULONGLONG now = GetTickCount64();
    if (s_cw_on < 0) { const char* e = getenv("GF_POP_CARWATCH"); s_cw_on = !(e && *e == '0');
        printf("[pop] v1109Y car watch (cars vanishing on screen) %s (GF_POP_CARWATCH)\n", s_cw_on ? "ON" : "off"); fflush(stdout); }
    if (!s_cw_on || now - t < 100) return;
    t = now;
    if (s_pv < 0) solo_view(0, 0);
    uint32_t cars[128]; int nc = 0;
    __try { nc = gf_world_cars(cars, 128); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    const int paused_now = paused();
    cw_ent nw[128]; int nn = 0;
    for (int i = 0; i < nc && nn < 128; i++) {
        uint32_t f = 0; float p[3], d = 0.0f;
        if (!gf_memq_access(vm_base, cars[i] + 0x34u, 4, 0)) continue;
        __try { f = be32(vm_base + cars[i] + 0x34u); } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (!(f & 1u) || !pos_of(cars[i], p)) continue;   /* released / no matrix: not alive */
        const int c = car_in_cone(p, &d); if (c < 0) continue;
        nw[nn].ent = cars[i]; memcpy(nw[nn].p, p, sizeof p); nw[nn].d = d; nw[nn].vis = d <= s_pv_near || (c == 1 && d <= 250.0f); nn++;
    }
    st_cw_checks++;
    int nvan = 0;   /* >= 5 visible cars in one 100 ms check = a mass clear (cutscene start / stream unload), counted apart */
    if (!paused_now) for (int i = 0; i < s_cw_n; i++) {
        if (!s_cw[i].vis) continue;
        int j; for (j = 0; j < nn; j++) if (nw[j].ent == s_cw[i].ent) break;
        if (j == nn) nvan++; }
    if (nvan >= 5) { st_cw_mass++; printf("[pop] v1109Y car watch: %d on-screen cars released at once (cutscene / stream unload, not counted)\n", nvan); fflush(stdout); }
    else if (!paused_now) for (int i = 0; i < s_cw_n; i++) {
        if (!s_cw[i].vis) continue;
        int j; for (j = 0; j < nn; j++) if (nw[j].ent == s_cw[i].ent) break;
        int gone = j == nn, jump = 0;
        if (!gone) { const float ax = nw[j].p[0] - s_cw[i].p[0], az = nw[j].p[2] - s_cw[i].p[2]; jump = ax * ax + az * az > 25.0f * 25.0f; }
        if (!gone && !jump) continue;
        const float d = s_cw[i].d; const int b = d < 50.0f ? 0 : d < 110.0f ? 1 : d < 200.0f ? 2 : 3;
        st_cw_van[b]++; if (jump) st_cw_jump++;
        if (st_cw_logged < 30) { st_cw_logged++; printf("[pop] v1109Y car %08X VANISHED ON SCREEN at %.0f m (%s)\n", s_cw[i].ent, (double)d, jump ? "slot re-used elsewhere" : "released"); fflush(stdout); }
    }
    memcpy(s_cw, nw, sizeof(cw_ent) * (size_t)nn); s_cw_n = nn;
    { static ULONGLONG lt; if (now - lt > 10000) { lt = now; int vis = 0; for (int i = 0; i < nn; i++) vis += nw[i].vis;
        printf("[pop] v1109Y car watch: %d live cars (%d on screen) | vanished on screen: < 50 m %ld, 50-110 m %ld, 110-200 m %ld, > 200 m %ld (slot re-used %ld, mass clears %ld) | cull/sweep: cars asked %ld, kept %ld (beyond the ped range %ld), let go in the cone %ld\n",
               nn, vis, st_cw_van[0], st_cw_van[1], st_cw_van[2], st_cw_van[3], st_cw_jump, st_cw_mass, st_car_q, st_car_keep, st_car_far_keep, st_car_go_cone); fflush(stdout); } }
}
/* v1109Z release tracer (user: "cars right next to me just disappear suddenly"; v1109Y watch: 14 released on screen at 6-70 m,
 * cull let 0 go in view -> another caller). func_00455F58 (generic release, r3 = ent+0x30) is wrapped (apply-pending.ps1 block
 * v1109Z); for a live factory car the player can see, log the PPU call chain (saved LRs up the back chain). GF_POP_RELTRACE=0 off. */
typedef struct { uint32_t k[3]; long n; } rt_key;
static rt_key s_rt[24]; static int s_rt_n, s_rt_on = -1, s_rt_logged; static volatile long st_rt_car, st_rt_vis;
static uint32_t rd32(uint32_t a) { uint32_t v = 0; if (a < 0x10000u || a >= 0xF0000000u || !gf_memq_access(vm_base, a, 4, 0)) return 0;
    __try { v = be32(vm_base + a); } __except (EXCEPTION_EXECUTE_HANDLER) { v = 0; } return v; }
void gf_pop_car_release(uint32_t obj, uint32_t lr, uint32_t sp)
{
    if (s_rt_on < 0) { const char* e = getenv("GF_POP_RELTRACE"); s_rt_on = !(e && *e == '0');
        printf("[pop] v1109Z car release tracer %s (GF_POP_RELTRACE)\n", s_rt_on ? "ON" : "off"); fflush(stdout); }
    if (!s_rt_on || obj < 0x30u) return;
    const uint32_t ent = obj - 0x30u;
    if (!gf_world_is_car(ent)) return;
    if (!(rd32(obj + 4u) & 1u)) return;   /* already released */
    st_rt_car++;
    float p[3], d = 0.0f; if (!pos_of(ent, p)) return;
    if (s_pv < 0) solo_view(0, 0);
    const int c = car_in_cone(p, &d);
    if (!(d <= s_pv_near || (c == 1 && d <= 250.0f))) return;
    if (paused()) return;
    st_rt_vis++;
    uint32_t ch[8]; int n = 0; ch[n++] = lr;
    { uint32_t f = sp; for (int k = 0; k < 7; k++) { const uint32_t b = rd32(f + 4u); if (!b || b <= f || b - f > 0x10000u) break; const uint32_t l = rd32(b + 0x10u + 4u); if (!l) break; ch[n++] = l; f = b; } }
    while (n < 3) ch[n++] = 0;
    int i; for (i = 0; i < s_rt_n; i++) if (s_rt[i].k[0] == ch[0] && s_rt[i].k[1] == ch[1] && s_rt[i].k[2] == ch[2]) break;
    if (i == s_rt_n && s_rt_n < 24) { s_rt[i].k[0] = ch[0]; s_rt[i].k[1] = ch[1]; s_rt[i].k[2] = ch[2]; s_rt[i].n = 0; s_rt_n++; }
    if (i < 24) s_rt[i].n++;
    if (s_rt_logged < 40) { s_rt_logged++;
        printf("[pop] v1109Z car %08X released ON SCREEN at %.0f m by", ent, (double)d);
        for (int k = 0; k < n; k++) printf(" %08X", ch[k]);
        printf("\n"); fflush(stdout); }
    { static ULONGLONG lt; const ULONGLONG now = GetTickCount64(); if (now - lt > 10000) { lt = now;
        printf("[pop] v1109Z car releases %ld, on screen %ld; by caller:", st_rt_car, st_rt_vis);
        for (int k = 0; k < s_rt_n; k++) printf(" [%08X<%08X<%08X x%ld]", s_rt[k].k[0], s_rt[k].k[1], s_rt[k].k[2], s_rt[k].n);
        printf("\n"); fflush(stdout); } }
}
static int pop_seen_ent(uint32_t ent, int site)
{
    float p[3], pl[3], fx, fz;
    if (s_pv < 0) solo_view(0, 0);
    if (!s_pv || !ent) return 0;
    st_seen_q[site]++;
    { static ULONGLONG lt; const ULONGLONG now = GetTickCount64(); if (now - lt > 10000) { lt = now;
        printf("[pop] v1109W-2 kept because the player can see them: despawn sweep %ld of %ld, over-budget cull %ld of %ld\n", st_seen_keep[0], st_seen_q[0], st_seen_keep[1], st_seen_q[1]); fflush(stdout); } }
    if (paused()) { st_seen_keep[site]++; return 1; }
    if (!pos_of(ent, p) || !player_pos(pl)) return 0;
    if (s_pv_car_far < 0.0f) { const char* e = getenv("GF_POP_CAR_VIEW_FAR"); s_pv_car_far = e && atof(e) > 1.0 ? (float)atof(e) : 230.0f;
        printf("[pop] v1109Y cars in the camera cone are kept to %.0f m (peds %.0f m) (GF_POP_CAR_VIEW_FAR)\n", (double)s_pv_car_far, (double)s_pv_far); fflush(stdout); }
    const int car = gf_world_is_car(ent); const float keep_r = car && s_pv_car_far > s_pv_far ? s_pv_car_far : s_pv_far;
    const float dx = p[0] - pl[0], dz = p[2] - pl[2], d2 = dx * dx + dz * dz;
    int seen = d2 <= s_pv_near * s_pv_near, cone = 0;
    if (!seen && gf_coop_local_cam(&fx, &fz)) {
        const float ox = dx + fx * 3.5f, oz = dz + fz * 3.5f, l = sqrtf(ox * ox + oz * oz);
        cone = l > 1e-3f && (ox * fx + oz * fz) / l >= s_pv_cos;
        seen = cone && d2 <= keep_r * keep_r; }
    if (car) { st_car_q++; if (seen) { st_car_keep++; if (d2 > s_pv_far * s_pv_far) st_car_far_keep++; } else if (cone) st_car_go_cone++; }
    if (seen && site == 1) {   /* v1112L (user 18:45: "a shitload of hostile NPCs and taxis clumped up"; log: over-budget cull 1462 of 1860
                                * KEPT): the over-budget cull may take an in-view entity after GF_POP_CULL_GRACE (20 s) of being asked for,
                                * unless it is within 15 m -- bounds the pile-up the W-2 protection caused. The sweep (site 0) is unchanged. */
        static float grace = -1.0f; if (grace < 0.0f) { const char* e = getenv("GF_POP_CULL_GRACE"); grace = e && atof(e) > 0 ? (float)atof(e) : 20.0f;
            printf("[pop] v1112L in-view entities are protected from the over-budget cull for %.0f s (then they may go if beyond 100 m)\n", (double)grace); fflush(stdout); }
        static struct { uint32_t ent; ULONGLONG first, last; } g[256]; static long st_let;
        const ULONGLONG now = GetTickCount64(); int k = -1, fr = -1, old = 0;
        for (int i = 0; i < 256; i++) { if (g[i].ent == ent) { k = i; break; } if (g[i].ent && now - g[i].last > 3000) g[i].ent = 0; if (!g[i].ent && fr < 0) fr = i; if (g[i].ent && g[old].ent && g[i].last < g[old].last) old = i; }
        if (k < 0) { k = fr >= 0 ? fr : old; g[k].ent = ent; g[k].first = now; }
        g[k].last = now;
        if ((float)(now - g[k].first) > grace * 1000.0f && d2 > 100.0f * 100.0f) {   /* v1112R: was 15 m -- 126 in-view people / 10 s popped out close to the player */
            st_let++; { static ULONGLONG lt; if (now - lt > 10000) { lt = now; printf("[pop] v1112L over-budget cull allowed for %ld long-protected in-view entities\n", st_let); fflush(stdout); } }
            return 0; }
    }
    if (seen) st_seen_keep[site]++;
    return seen;
}
