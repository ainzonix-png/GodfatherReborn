/* gf_coop_wpose.c -- v1061F world NPC pose stream: the guest's proxies play the host NPCs' REAL animation (Claude).
 *
 * WHY: proxies are live game NPCs that we only re-place every frame. Their own AI keeps playing idle / its own anims,
 * so on the guest they stood, slid and never walked like the host's peds ("nothing synced", 2026-11-05 01:00).
 * The partner puppet already solved this with the final-bone-palette stream (gf_coop_pose.c): the pose written
 * after the skeleton update is what gets skinned, whatever the entity's AI / anim graph did. Same thing per NPC here.
 *
 * HOST (host + echo): every 250 ms pick the GF_COOP_WPOSE_N (default 10) synced peds nearest to the guest player
 *   (gf_coop_partner_pose; echo: the local player) within GF_COOP_WPOSE_R (60 m). Right after such an NPC's anim update
 *   (gf_coop_after_anim) capture its palette (gf_coop_pose_capture_any) at GF_COOP_WPOSE_HZ (default 10) and send
 *   [u16 netid][pose wire v1] as an unreliable blob on channel 12..15. Blobs are latest-wins per queue slot, flushed at
 *   GF_COOP_RATE (30 Hz) -> a slot is only reused 34 ms after its last use, so no pose is overwritten before it is sent.
 *   ~1 KB per pose: 10 NPCs x 10 Hz ~ 100 KB/s.
 * GUEST (join + echo): co-op thread copies into a per-netid jitter buffer (4 poses). In the post-skel hook (main
 *   thread, after the proxy's own skeleton update) the proxy entity -> netid (gf_proxy_netid_of) -> the two poses
 *   around now - GF_COOP_WPOSE_DELAY (100 ms, like the world snapshots) on the host clock -> nlerp -> palette
 *   (gf_coop_pose_apply_interp: same rig check + mesh-scale rule as the puppet). Poses older than 1 s are not applied
 *   (the proxy falls back to its own animation). Rig mismatch (substituted class) is counted and skipped.
 * Env: GF_COOP_WPOSE=0 off (default ON whenever co-op world sync runs). Log [coop-wpose] every 10 s. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "gf_coop_pose.h"
#include "gf_memq.h"   /* v1062W2 corpse link reads */
#include "recompiled/ppu_recomp.h"   /* v1065A forced anim update (guest call with f1 = dt) */

extern uint8_t* vm_base;
extern int gf_coop_connected(void);
extern int gf_coop_mode(void);                 /* 0 off, 1 host, 2 join, 3 echo */
extern int gf_coop_local_pos(float out[3]);
extern int gf_coop_partner_pose(float out_m[16], float* hp);
extern int gf_coop_send_blob(uint8_t channel, const void* data, int len);
extern uint32_t gf_coop_pose_now_ms(void);
extern int gf_coop_pose_capture_any(uint32_t ent, uint8_t* out, int cap);
extern int gf_coop_pose_apply_interp(uint32_t ent, uint32_t anim, const uint8_t* pa, int la, const uint8_t* pb, int lb, float u);
extern int gf_coop_world_host_peds(uint32_t* ents, uint16_t* ids, float (*pos)[3], int max);
extern uint16_t gf_proxy_netid_of(uint32_t ent);
extern uint32_t gf_rag_cur_ent(void);
extern void gf_npcfix_after_anim(uint32_t ent);   /* v1067X */
extern void gf_npcfix_post_skel(uint32_t anim);   /* v1067X */
extern int gf_npcfix_force_eval(uint32_t ent);   /* v1067X */
extern float gf_npcfix_handback(uint16_t id, const uint8_t** pose, int* len);   /* v1067X */
extern void gf_npcfix_note_handback(int local_only);   /* v1067X */
/* v1070A (agent A): pose guard -- see gf_coop_pose.c "v1070A". Here: (1) the after-anim re-apply only writes through the cached anim
 * controller while it is still the entity's live one (ent+0xC4): a proxy LOD-stripped on this peer (anim controller freed) kept
 * writing its cached palette into freed memory every frame = other NPCs near a far joiner corrupted / collapsed; (2) a remote pose is
 * never applied to an entity this peer SOURCES itself (GF_COOP_WPOSE_OWNGUARD=0 = old); (3) GF_COOP_POSE_TEST_COLLAPSE=1 (echo/test
 * only) sends every 4th net id a collapsed pose (all bones at the root) 6 s of every 12 s -> the receiver's validator must reject them. */
extern int gf_coop_pose_anim_live(uint32_t ent, uint32_t anim, int count);
extern int gf_coop_world_src_has(uint32_t ent);
extern volatile long g_v70_rw_noctl, g_v70_rw_changed, g_v70_cap_coll, g_v70_cap_expl, g_v70_cap_nonfin, g_v70_app_coll, g_v70_app_expl, g_v70_app_nonfin;
extern volatile long g_v70_scale_fix, g_v70_scale_noref, g_v70_face_skip, g_v70_live_face_skip, g_v70_recv_bad;
static volatile long st_v70_reapply_block, st_v70_own_block, st_v70_c4_ok, st_v70_c4_bad, st_v70_test_coll;
static int s_v70_own = -1, s_v70_test = -1;
static void v70_wcfg(void)
{
    const char* e;
    if (s_v70_own >= 0) return;
    e = getenv("GF_COOP_WPOSE_OWNGUARD"); s_v70_own = !(e && *e == '0');
    e = getenv("GF_COOP_POSE_TEST_COLLAPSE"); s_v70_test = e && *e == '1';
    printf("[coop-wpose] v1070A re-apply only through the live anim controller (GF_COOP_POSE_ANIMCHK), own-source guard %s (GF_COOP_WPOSE_OWNGUARD)%s\n",
           s_v70_own ? "ON" : "off", s_v70_test ? " | TEST_COLLAPSE: every 4th net id gets collapsed poses 6 s of 12 s" : ""); fflush(stdout);
}
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;   /* v1065A */
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
typedef void (*gfn)(ppu_context*);

enum { WP_CH0 = 12, WP_NCH = 8 /* v1066N: 4 -> 8 queue slots */, WP_SEL_MAX = 64, WP_STREAMS = 64, WP_BUF = 4, WP_PKT = GF_COOP_POSE_MAX_PACKET + 2 };   /* v1063P4: 32 -> 64 */

static int s_on = -1, s_n = 48, s_hz = 15, s_hz_far = 8, s_near = 8; static float s_r = 120.0f; static double s_delay = 130.0;   /* v1061H; v1063P4 N 24 -> 48, R 80 -> 120 (= H's world radius) */
static int s_mid = 24, s_hz_far2 = 5, s_hold_ms = 15000;   /* v1064Q: hold 3000 -> 15000 (with the host idle capture a synced ped's stream never goes silent; never fall back to local anims) */
/* v1065A off-screen NPCs (user 15:40: "NPC animations stop working ... if the host stops looking at them").
 * ROOT CAUSE: func_0046316C (entity anim update) asks vt+0x178(ent, &cullA, &cullB); a set byte ORs 0x100000 / 0x4000 into
 * the anim node flags (func_003BE758) -> func_003CC128 does not rebuild the bone palette for a ped the HOST camera does not
 * see. Our capture after the anim update then sent the same frozen palette (and the guest showed a statue sliding along).
 * Fix 1 (GF_COOP_WPOSE_FORCE_EVAL=1 default): hand edit in func_0046316C_orig (_patches block v1065A) calls
 *   gf_wpose_force_eval() -> for a SELECTED synced ped whose capture is due, the cull bytes are cleared = on-screen eval.
 * Fix 2 (GF_COOP_WPOSE_FORCE_ANIM=1 default): a selected ped the game did not anim-update at all for GF_COOP_WPOSE_IDLE_MS
 *   gets func_0046316C(ent, 0, dt) called from the tick (dt = time since its last update, <= 0.25 s), so its anim graph
 *   advances + evaluates and the normal after-anim capture sends a live pose. Cost: only due peds, at their tier rate. */
static int s_force_eval = -1, s_force_anim = 1;
static uint32_t s_sel_anim[WP_SEL_MAX], s_sel_vt[WP_SEL_MAX];   /* v1065A: last anim update (pose clock) / vtable at selection */
static volatile long st_feval, st_feval_seen, st_fanim, st_fanim_fail, st_fanim_skip;
static int s_in_fanim;
/* v1066N (agent N) host fair scheduler (GF_COOP_WPOSE_SCHED=1 default, 0 = old capture inside the anim update).
 * coop-local 16:3x: 35 selected but only 27 live streams, "slot waits 15856": the blob queue has 4 wpose slots (6 ms guard)
 * = ~4-5 poses per FRAME, handed out in anim-update order -> the peds updated late in the frame lost every time and starved.
 * Now: every tick grants the GF_COOP_WPOSE_CH (8: channels 12..15 + 18..21) most overdue peds (overdue = time since the last
 * capture / the ped's tier period); a granted ped gets the full bone eval in its next anim update (v1065A hook, only the
 * granted ones = cheap) and is captured from the next tick (not anim-updated for 3 ticks -> forced anim update first).
 * Selection rank (= tier rate) also prefers peds in front of the guest (GF_COOP_WPOSE_VIEW=1). */
static int s_sched = 1, s_nch = 8, s_view = 1;
static const uint8_t s_chmap[WP_NCH] = { 12, 13, 14, 15, 18, 19, 20, 21 };   /* queue slot = channel & 15 -> 12..15, 2..5 */
static uint32_t s_grant_at[WP_SEL_MAX], s_sel_fail[WP_SEL_MAX]; static int s_grant_ticks[WP_SEL_MAX];
static volatile long st_g_grant, st_g_fresh, st_g_forced, st_g_unanim, st_g_wait, st_g_late_sum, st_g_late_n, st_g_noslot, st_g_failed;
static volatile long st_fskip_fn, st_fskip_vt, st_fskip_alive, st_fskip_noanim, st_fskip_mem;
static int s_idle_ms = 300, s_idle_per_tick = 12;   /* v1064Q GF_COOP_WPOSE_IDLE_MS: host captures a selected ped from the tick when its anim update did not capture it for this long (0 = off, old) */   /* v1063P4: selection rank >= _MID at _HZ_FAR2; guest holds the last pose up to _HOLD_MS */

/* ---------------------------------------------------------------- host */
static void wp_reapply(uint32_t ent);
static int wp_capture_send(int i, uint32_t ent, uint32_t now);   /* v1064Q */
static void host_idle_capture(void);   /* v1064Q */
static uint32_t s_sel_ent[WP_SEL_MAX]; static uint16_t s_sel_id[WP_SEL_MAX]; static uint32_t s_sel_next[WP_SEL_MAX]; static int s_sel_n;
static float s_sel_d[WP_SEL_MAX];   /* v1119F distance to the nearest player (host_select) */
static uint32_t s_sel_last[WP_SEL_MAX];   /* v1064Q: last capture time per selected ped (pose clock) */
static volatile long st_idlecap, st_idle_peds;   /* v1064Q */
static uint32_t s_ch_free[WP_NCH]; static unsigned s_ch_rr;
static ULONGLONG s_sel_at;
/* v1062W2 corpse follow (host): a dying NPC is replaced by the game's corpse entity (func_002FC3BC: corpse+0x10C =
 * npc+0x38, same palette, also animated through func_0046316C -- gf_ragdoll.c follows it the same way). Without this
 * the NPC vanished from gf_world_enum -> despawn -> the guest's proxy disappeared mid-death and no body stayed.
 * after_anim notes every entity whose +0x10C links to a synced ped; gf_coop_world.c (host_tick) moves the net id over
 * to the corpse when the NPC goes missing, and keeps it while the corpse is animated. */
static uint32_t s_all_ent[256]; static int s_all_n;
enum { WP_CORPSES = 64 /* v1071A: 32 -> 64 */ };
typedef struct { uint32_t npc, corpse, vt; ULONGLONG seen, t0; int abit; } wp_corpse;   /* v1071A: vtable / first seen / alive bit for the existence check */
static int s_cp_keep_ms = -1; static volatile long st_cp_exist, st_cp_gone, st_cp_cap;   /* v1071A */
static int cp_rd32(uint32_t ea, uint32_t* out)
{
    if (!gf_memq_access(vm_base, ea, 4, 0)) return 0;
    __try { const uint8_t* p = vm_base + ea; *out = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}
static wp_corpse s_cp[WP_CORPSES]; static unsigned s_cp_i; static volatile long st_corpse_note;
static void corpse_note(uint32_t ent)
{
    uint32_t v = 0, npc; int i;
    if (!s_all_n || !gf_memq_access(vm_base, ent + 0x10Cu, 4, 0)) return;
    __try { const uint8_t* p = vm_base + ent + 0x10Cu; v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (v < 0x10038u || v >= 0xF0000000u) return;
    npc = v - 0x38u;
    if (npc == ent) return;
    for (i = 0; i < s_all_n; i++) if (s_all_ent[i] == npc) break;
    if (i == s_all_n) {   /* already following: refresh the corpse's timestamp */
        for (int k = 0; k < WP_CORPSES; k++) if (s_cp[k].corpse == ent) { s_cp[k].seen = GetTickCount64(); return; }
        return;
    }
    for (int k = 0; k < WP_CORPSES; k++) if (s_cp[k].corpse == ent && s_cp[k].npc == npc) { s_cp[k].seen = GetTickCount64(); return; }
    {   /* v1071A: reuse a free / finished slot first (kept bodies must not be overwritten by new deaths), else the oldest */
        const ULONGLONG now = GetTickCount64(); int k, pick = -1; ULONGLONG oldest = ~0ull;
        for (k = 0; k < WP_CORPSES; k++) { if (!s_cp[k].corpse) { pick = k; break; } if (s_cp[k].t0 < oldest) { oldest = s_cp[k].t0; pick = k; } }
        if (pick < 0) pick = (int)(s_cp_i++ % WP_CORPSES);
        { wp_corpse* c = &s_cp[pick]; uint32_t f = 0; c->npc = npc; c->corpse = ent; c->seen = now; c->t0 = now; c->vt = 0; cp_rd32(ent, &c->vt);
          c->abit = cp_rd32(ent + 0x34u, &f) && (f & 1u); st_corpse_note++; }
    }
}
/* v1071A (user 13:18: "bodies despawn on the other screen before they do on the killer's"): the follow used to end 1.5 s after the
 * corpse's last ANIM UPDATE -> a settled / culled / LOD-stripped body was despawned on the partner while it stayed on the owner.
 * Now it is kept while the corpse entity still exists here: same vtable, still linked to its NPC (+0x10C = npc + 0x38), alive
 * bit +0x34 still set (when it was set), transform valid -- at most GF_COOP_CORPSE_KEEP_S (120 s; 0 = old 1.5 s rule). */
static int cp_exists(const wp_corpse* c)
{
    uint32_t v = 0, l = 0, f = 0, xf = 0;
    if (!c->corpse || !cp_rd32(c->corpse, &v) || v != c->vt || !v) return 0;
    if (!cp_rd32(c->corpse + 0x10Cu, &l) || l != c->npc + 0x38u) return 0;
    if (c->abit && (!cp_rd32(c->corpse + 0x34u, &f) || !(f & 1u))) return 0;
    if (!cp_rd32(c->corpse + 0x68u, &xf) || !xf || !gf_memq_access(vm_base, xf + 0x10u, 0x40, 0)) return 0;
    return 1;
}
/* gf_coop_world.c: corpse entity of a (missing) synced NPC, or the age of a followed corpse's last anim update */
uint32_t gf_wpose_corpse_of(uint32_t npc)
{
    const ULONGLONG now = GetTickCount64();
    for (int k = 0; k < WP_CORPSES; k++) if (s_cp[k].npc == npc && s_cp[k].corpse && now - s_cp[k].seen < 1500) return s_cp[k].corpse;
    return 0;
}
int gf_wpose_corpse_alive(uint32_t corpse)
{
    const ULONGLONG now = GetTickCount64();
    if (s_cp_keep_ms < 0) { const char* e = getenv("GF_COOP_CORPSE_KEEP_S"); s_cp_keep_ms = (e && *e) ? atoi(e) * 1000 : 120000; if (s_cp_keep_ms < 0) s_cp_keep_ms = 0;
        printf("[coop-wpose] v1071A corpses stay synced while the body exists on its owner (max %d s; GF_COOP_CORPSE_KEEP_S, 0 = old: only while animated)\n", s_cp_keep_ms / 1000); fflush(stdout); }
    if (!corpse) return 0;   /* v1071A: config / startup line only (free slots have corpse 0) */
    for (int k = 0; k < WP_CORPSES; k++) {
        wp_corpse* c = &s_cp[k];
        if (c->corpse != corpse) continue;
        if (now - c->seen < 1500) return 1;
        if (!s_cp_keep_ms) return 0;
        if (now - c->t0 > (ULONGLONG)s_cp_keep_ms) { st_cp_cap++; c->corpse = 0; return 0; }
        { int ex = 0; __try { ex = cp_exists(c); } __except (EXCEPTION_EXECUTE_HANDLER) { ex = 0; }
          if (ex) { st_cp_exist++; return 1; }
          st_cp_gone++; c->corpse = 0; return 0; }
    }
    return 0;
}
static volatile long st_cap, st_capfail, st_sent, st_noslot;

static int wp_enabled(void)
{
    if (s_on < 0) {
        const char* e = getenv("GF_COOP_WPOSE"); s_on = !(e && *e == '0');
        if ((e = getenv("GF_COOP_WPOSE_N")) && atoi(e) >= 0) s_n = atoi(e) > WP_SEL_MAX ? WP_SEL_MAX : atoi(e);
        if ((e = getenv("GF_COOP_WPOSE_HZ")) && atoi(e) > 0) s_hz = atoi(e) > 30 ? 30 : atoi(e);
        if ((e = getenv("GF_COOP_WPOSE_R")) && atof(e) > 0) s_r = (float)atof(e);
        if ((e = getenv("GF_COOP_WPOSE_HZ_FAR")) && atoi(e) > 0) s_hz_far = atoi(e) > 30 ? 30 : atoi(e);
        if ((e = getenv("GF_COOP_WPOSE_NEAR")) && atoi(e) >= 0) s_near = atoi(e);
        if ((e = getenv("GF_COOP_WPOSE_DELAY")) && atoi(e) >= 0) s_delay = (double)atoi(e);
        if ((e = getenv("GF_COOP_WPOSE_MID")) && atoi(e) >= 0) s_mid = atoi(e);   /* v1063P4 */
        if ((e = getenv("GF_COOP_WPOSE_HZ_FAR2")) && atoi(e) > 0) s_hz_far2 = atoi(e) > 30 ? 30 : atoi(e);
        if ((e = getenv("GF_COOP_WPOSE_HOLD_MS")) && atoi(e) >= 0) s_hold_ms = atoi(e);
        if ((e = getenv("GF_COOP_WPOSE_IDLE_MS")) && atoi(e) >= 0) s_idle_ms = atoi(e);   /* v1064Q */
        if ((e = getenv("GF_COOP_WPOSE_IDLE_PER_TICK")) && atoi(e) > 0) s_idle_per_tick = atoi(e);
        e = getenv("GF_COOP_WPOSE_FORCE_EVAL"); s_force_eval = !(e && *e == '0');   /* v1065A */
        e = getenv("GF_COOP_WPOSE_FORCE_ANIM"); s_force_anim = !(e && *e == '0');
        if ((e = getenv("GF_COOP_WPOSE_FORCE_EVAL")) && *e == '2') s_force_eval = 2;   /* v1066N: 2 = every selected ped every anim update */
        e = getenv("GF_COOP_WPOSE_SCHED"); s_sched = !(e && *e == '0');   /* v1066N */
        if ((e = getenv("GF_COOP_WPOSE_CH")) && atoi(e) > 0) s_nch = atoi(e) > WP_NCH ? WP_NCH : atoi(e);
        e = getenv("GF_COOP_WPOSE_VIEW"); s_view = !(e && *e == '0');
        { const char* w = getenv("GF_COOP_WORLD"); if (w && *w == '0') s_on = 0; }
        printf("[coop-wpose] v1063P4 world NPC pose stream %s: %d nearest peds within %.0f m (nearest %d at %d Hz, up to rank %d at %d Hz, rest %d Hz), playback delay %.0f ms, hold %d ms\n",
               s_on ? "ON" : "off", s_n, (double)s_r, s_near, s_hz, s_mid, s_hz_far, s_hz_far2, s_delay, s_hold_ms);
        printf("[coop-wpose] v1064Q host idle capture %s: a selected ped not captured by its anim update for %d ms is captured from the tick (up to %d per tick) -> every synced ped near the guest has a stream; guest hold %d ms\n",
               s_idle_ms > 0 ? "ON" : "off", s_idle_ms, s_idle_per_tick, s_hold_ms);
        printf("[coop-wpose] v1065A off-screen NPC fix: force bone eval %s (GF_COOP_WPOSE_FORCE_EVAL), force anim update of non-updated peds %s (GF_COOP_WPOSE_FORCE_ANIM)\n",
               s_force_eval ? "ON" : "off", s_force_anim ? "ON" : "off");
        printf("[coop-wpose] v1066N host scheduler %s (most overdue first, %d blob slots, force eval %s), guest-view priority %s\n",
               s_sched ? "ON" : "off (old)", s_nch, s_force_eval == 2 ? "ALL selected" : s_force_eval ? "granted peds" : "off", s_view ? "ON" : "off"); fflush(stdout);
    }
    return s_on;
}
/* v1068T (agent T): pose stream in BOTH directions with agent R's zone spawn authority (gf_coop_world_guest_auth() = 1 on both
 * peers): every peer is a SOURCE for the peds it owns (gf_coop_world_host_peds = local sources on either side, net ids
 * 1..0x7FFF host / 0x8000.. joiner) and a SINK for the proxies of the other peer's peds (gf_proxy_netid_of != 0). Each peer
 * receives blobs only from the other, so channels 12..15/18..21 are reused per direction and the stream table never mixes
 * senders. GF_COOP_WPOSE_TWOWAY=0 = old (host sends, guest applies). */
int gf_coop_world_guest_auth_t0(void) { return 0; }   /* weak default until gf_coop_world.c defines it */
#pragma comment(linker, "/alternatename:gf_coop_world_guest_auth=gf_coop_world_guest_auth_t0")
extern int gf_coop_world_guest_auth(void);
static int s_twoway = -1;
static volatile long st_pose_bytes, st_sel_refs;   /* v1072W */
/* v1072W2 (agent W) COMPACT POSE WIRE on the world-NPC network path only (GF_COOP_WPOSE_COMPACT=0 = old v1 13 B/bone).
 * v1 slot = 3 x int16 smallest-three quaternion (x 46339.5) + omit byte + 3 x int16 position (1/4096 m) = 13 B.
 * v2 slot = u32 (omit:2 | 3 x 10-bit quaternion, step 64 of the v1 int16) + the same 3 x int16 position = 10 B (-23 %).
 * Model-space palette bones are independent (no chain accumulation); 10-bit = ~0.08 deg. The receiver expands v2 back to v1 before
 * anything else sees it, so every decoder / mixer is unchanged. Version byte (+4) 2 = compact. BOTH exes must be v1072W2+. */
static int s_cmp = -1; static volatile long st_cmp_tx, st_cmp_rx, st_cmp_bad, st_cmp_saved;
static int cmp_on(void) { if (s_cmp < 0) { const char* e = getenv("GF_COOP_WPOSE_COMPACT"); s_cmp = !(e && *e == '0');
    printf("[coop-wpose] v1072W2 compact pose wire (10 B per bone instead of 13) %s (GF_COOP_WPOSE_COMPACT; both exes must be v1072W2)\n", s_cmp ? "ON" : "off"); fflush(stdout); } return s_cmp; }
static int wp_v1_to_v2(const uint8_t* in, int len, uint8_t* out, int cap)
{
    int n;
    if (len < 16 || in[4] != 1 || (len - 16) % 13) return 0;
    n = (len - 16) / 13; if (16 + n * 10 > cap) return 0;
    memcpy(out, in, 16); out[4] = 2;
    for (int i = 0; i < n; i++) { const uint8_t* a = in + 16 + i * 13; uint8_t* b = out + 16 + i * 10; uint32_t w = (uint32_t)(a[6] & 3) << 30;
        if (a[6] & 0xFC) return 0;
        for (int k = 0; k < 3; k++) { int v = (int)(int16_t)(uint16_t)(a[k * 2] | (a[k * 2 + 1] << 8)); int q = v >= 0 ? (v + 32) / 64 : -((-v + 32) / 64);
            if (q > 511) q = 511; if (q < -511) q = -511; w |= ((uint32_t)q & 1023u) << (20 - 10 * k); }
        b[0] = (uint8_t)w; b[1] = (uint8_t)(w >> 8); b[2] = (uint8_t)(w >> 16); b[3] = (uint8_t)(w >> 24);
        memcpy(b + 4, a + 7, 6); }
    return 16 + n * 10;
}
static int wp_v2_to_v1(const uint8_t* in, int len, uint8_t* out, int cap)
{
    int n;
    if (len < 16 || in[4] != 2 || (len - 16) % 10) return 0;
    n = (len - 16) / 10; if (16 + n * 13 > cap) return 0;
    memcpy(out, in, 16); out[4] = 1;
    for (int i = 0; i < n; i++) { const uint8_t* b = in + 16 + i * 10; uint8_t* a = out + 16 + i * 13;
        const uint32_t w = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
        for (int k = 0; k < 3; k++) { int q = (int)((w >> (20 - 10 * k)) & 1023u); if (q & 512) q -= 1024; { int v = q * 64; if (v > 32767) v = 32767; if (v < -32767) v = -32767;
            a[k * 2] = (uint8_t)(v & 0xFF); a[k * 2 + 1] = (uint8_t)(((unsigned)v >> 8) & 0xFF); } }
        a[6] = (uint8_t)(w >> 30); memcpy(a + 7, b + 4, 6); }
    return 16 + n * 13;
}
#include "gf_coop_remote.h"   /* v1072W */
static int twoway(void)
{
    if (s_twoway < 0) { const char* e = getenv("GF_COOP_WPOSE_TWOWAY"); s_twoway = !(e && *e == '0');
        printf("[coop-wpose] v1068T two-way pose stream %s (each peer streams the peds it owns, applies the other's; active when the world sync runs two-way)\n", s_twoway ? "ON" : "off"); fflush(stdout); }
    return s_twoway && gf_coop_world_guest_auth();
}
static int is_host_side(void) { const int m = gf_coop_mode(); return m == 1 || m == 3 || (m == 2 && twoway()); }
static int is_guest_side(void) { const int m = gf_coop_mode(); return m == 2 || m == 3 || (m == 1 && twoway()); }

static void host_select(void)
{
    uint32_t ents[256]; uint16_t ids[256]; float pos[256][3]; float ref[3]; int n, k = 0;
    float m16[16], hp, fwd[3] = {0, 0, 0}; int have_fwd = 0;
    const int mode = gf_coop_mode();
    gf_remote rr[GF_REMOTE_MAX]; float refs[GF_REMOTE_MAX][3], rfw[GF_REMOTE_MAX][2]; int nref = 0, rhf[GF_REMOTE_MAX];   /* v1072W: rank by the nearest REMOTE player */
    (void)m16; (void)hp; (void)fwd; (void)have_fwd;
    if (mode == 1 || mode == 2) { const int nr = gf_world_remotes(rr, GF_REMOTE_MAX);
        for (int i = 0; i < nr; i++) { refs[nref][0] = rr[i].p[0]; refs[nref][1] = rr[i].p[1]; refs[nref][2] = rr[i].p[2];
            rfw[nref][0] = rr[i].bf[0]; rfw[nref][1] = rr[i].bf[1]; rhf[nref] = s_view; nref++; } }
    if (mode == 2 && !nref) { s_sel_n = 0; return; }   /* v1068T joiner as source: only peds a remote player can see */
    if (!nref) { if (!gf_coop_local_pos(refs[0])) { s_sel_n = 0; return; } rhf[0] = 0; rfw[0][0] = 0; rfw[0][1] = 1; nref = 1; }
    st_sel_refs = nref; ref[0] = refs[0][0]; ref[1] = refs[0][1]; ref[2] = refs[0][2];
#if 0
    if ((mode == 1 || mode == 2) && gf_coop_partner_pose(m16, &hp)) {   /* v1068T: mode 2 = joiner-owned peds ranked by the host's view */ ref[0] = m16[12]; ref[1] = m16[13]; ref[2] = m16[14];
        {   /* v1066N: guest facing (at row, horizontal) */
            const float l = sqrtf(m16[8] * m16[8] + m16[10] * m16[10]);
            if (s_view && l > 0.1f && l < 10.0f) { fwd[0] = m16[8] / l; fwd[2] = m16[10] / l; have_fwd = 1; } } }
    else if (!gf_coop_local_pos(ref)) { s_sel_n = 0; return; }
#endif
    n = gf_coop_world_host_peds(ents, ids, pos, 256);
    memcpy(s_all_ent, ents, sizeof(uint32_t) * (size_t)n); s_all_n = n;   /* v1062W2 corpse links */
    {   /* partial selection sort of the s_n nearest within range */
        float d2[256]; const float r2 = s_r * s_r;
        float rk[256];   /* v1066N: rank key = distance, x1.6 for peds > 8 m BEHIND the guest (its camera looks forward) */
        for (int i = 0; i < n; i++) { d2[i] = 3.4e38f; rk[i] = 3.4e38f;
            for (int r = 0; r < nref; r++) { const float dx = pos[i][0] - refs[r][0], dy = pos[i][1] - refs[r][1], dz = pos[i][2] - refs[r][2]; const float dd = dx*dx + dy*dy + dz*dz; float kk = dd;
                if (rhf[r] && dd > 64.0f) { const float hl = sqrtf(dx*dx + dz*dz); if (hl > 0.1f && (dx * rfw[r][0] + dz * rfw[r][1]) / hl < -0.25f) kk = dd * 2.56f; }   /* v1066N behind the player's view */
                if (dd < d2[i]) d2[i] = dd; if (kk < rk[i]) rk[i] = kk; } }
        uint32_t keep_ent[WP_SEL_MAX]; uint16_t keep_id[WP_SEL_MAX]; uint32_t keep_next[WP_SEL_MAX]; uint32_t keep_last[WP_SEL_MAX]; uint32_t keep_anim[WP_SEL_MAX], keep_vt[WP_SEL_MAX];
        uint32_t keep_grant[WP_SEL_MAX], keep_fail[WP_SEL_MAX]; int keep_gt[WP_SEL_MAX];   /* v1066N */
        float keep_d[WP_SEL_MAX], lp[3]; const int have_lp = gf_coop_local_pos(lp);   /* v1119F: distance to the nearest player incl. the local one */
        while (k < s_n) {
            int best = -1;
            for (int i = 0; i < n; i++) if (d2[i] <= r2 && (best < 0 || rk[i] < rk[best])) best = i;
            if (best < 0) break;
            keep_ent[k] = ents[best]; keep_id[k] = ids[best]; keep_next[k] = 0; keep_last[k] = 0; keep_anim[k] = 0; keep_vt[k] = 0; keep_grant[k] = 0; keep_fail[k] = 0; keep_gt[k] = 0;
            if (gf_memq_access(vm_base, ents[best], 4, 0)) __try { const uint8_t* q = vm_base + ents[best]; keep_vt[k] = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3]; } __except (EXCEPTION_EXECUTE_HANDLER) { keep_vt[k] = 0; }   /* v1065A */
            for (int j = 0; j < s_sel_n; j++) if (s_sel_ent[j] == ents[best] && s_sel_id[j] == ids[best]) { keep_next[k] = s_sel_next[j]; keep_last[k] = s_sel_last[j]; keep_anim[k] = s_sel_anim[j]; keep_grant[k] = s_grant_at[j]; keep_fail[k] = s_sel_fail[j]; keep_gt[k] = s_grant_ticks[j]; }   /* keep schedule (v1064Q + last capture) */
            { float dm = d2[best]; if (have_lp) { const float dx = pos[best][0] - lp[0], dy = pos[best][1] - lp[1], dz = pos[best][2] - lp[2]; const float dd = dx*dx + dy*dy + dz*dz; if (dd < dm) dm = dd; }
              keep_d[k] = dm < 3.0e38f ? sqrtf(dm) : 1.0e9f; }   /* v1119F */
            d2[best] = 3.4e38f; rk[best] = 3.4e38f; k++;
        }
        memcpy(s_sel_ent, keep_ent, sizeof(uint32_t) * (size_t)k); memcpy(s_sel_id, keep_id, sizeof(uint16_t) * (size_t)k);
        memcpy(s_sel_next, keep_next, sizeof(uint32_t) * (size_t)k); memcpy(s_sel_last, keep_last, sizeof(uint32_t) * (size_t)k);
        memcpy(s_sel_anim, keep_anim, sizeof(uint32_t) * (size_t)k); memcpy(s_sel_vt, keep_vt, sizeof(uint32_t) * (size_t)k);   /* v1065A */
        memcpy(s_grant_at, keep_grant, sizeof(uint32_t) * (size_t)k); memcpy(s_sel_fail, keep_fail, sizeof(uint32_t) * (size_t)k); memcpy(s_grant_ticks, keep_gt, sizeof(int) * (size_t)k); s_sel_n = k;   /* v1066N */
        memcpy(s_sel_d, keep_d, sizeof(float) * (size_t)k);   /* v1119F */
    }
}
void gf_wpose_after_anim(uint32_t ent)   /* main PPU thread, right after func_0046316C(ent) */
{
    int i; uint32_t now;
    gf_npcfix_after_anim(ent);   /* v1067X: capture/re-apply a joiner-owned grabbed NPC before normal wpose work */
    if (s_on == 1 && ent) wp_reapply(ent);   /* v1061H guest */
    if (s_on == 1 && ent && is_host_side() && gf_coop_connected()) __try { corpse_note(ent); } __except (EXCEPTION_EXECUTE_HANDLER) { }   /* v1062W2 */
    if (s_on != 1 || !s_sel_n || !ent || !is_host_side() || !gf_coop_connected()) return;
    for (i = 0; i < s_sel_n; i++) if (s_sel_ent[i] == ent) break;
    if (i == s_sel_n) return;
    now = gf_coop_pose_now_ms();
    s_sel_anim[i] = now ? now : 1;   /* v1065A: the game (or our forced call) anim-updated this ped */
    if (s_sched == 1) return;   /* v1066N: captured by host_sched_tick */
    if (s_sel_next[i] && (int32_t)(now - s_sel_next[i]) < 0) return;
    wp_capture_send(i, ent, now);   /* v1064Q: shared with the host idle capture (gf_wpose_tick) */
}
static int wp_capture_send(int i, uint32_t ent, uint32_t now)   /* v1064Q (was the body of gf_wpose_after_anim); 1 = sent */
{
    uint8_t pkt[WP_PKT]; int n, ch = -1;
    const int nch = s_sched == 1 ? s_nch : 4;   /* v1066N: old mode keeps the 4 old slots */
    for (int t = 0; t < nch; t++) { const int c = (int)((s_ch_rr + (unsigned)t) % (unsigned)nch); if ((int32_t)(now - s_ch_free[c]) >= 0) { ch = c; break; } }
    if (ch < 0) { st_noslot++; return -2; }   /* all slots still waiting for a flush: try on the next anim update (v1066N: -2) */
    n = gf_coop_pose_capture_any(ent, pkt + 2, (int)sizeof pkt - 2);
    if (n <= 0) { st_capfail++; s_sel_next[i] = now + 500; s_sel_last[i] = now; s_sel_fail[i] = (now + 1000) | 1u; return 0; }   /* v1064Q: last = now -> the idle path does not retry a failing ped every tick */
    st_cap++;
    v70_wcfg();
    if (s_v70_test && (s_sel_id[i] % 4u) == 1u && (GetTickCount64() / 6000u) % 2u == 1u && n > 16) {   /* v1070A test: all bone positions -> 0 */
        const int ns = (n - 16) / 13;
        for (int k = 0; k < ns; k++) memset(pkt + 2 + 16 + k * 13 + 7, 0, 6);
        st_v70_test_coll++;
    }
    pkt[0] = (uint8_t)s_sel_id[i]; pkt[1] = (uint8_t)(s_sel_id[i] >> 8);
    {   int sl = n + 2; const uint8_t* sp = pkt; uint8_t cp[WP_PKT];   /* v1072W2 compact wire */
        if (cmp_on()) { const int cl = wp_v1_to_v2(pkt + 2, n, cp + 2, (int)sizeof cp - 2); if (cl > 0) { cp[0] = pkt[0]; cp[1] = pkt[1]; sp = cp; sl = cl + 2; st_cmp_tx++; st_cmp_saved += n - cl; } }
        if (gf_coop_send_blob(s_chmap[ch], sp, sl)) { st_sent++; st_pose_bytes += sl; } }   /* v1072W bytes */   /* v1066N channel map (12..15, 18..21) */
    s_sel_fail[i] = 0;
    s_ch_free[ch] = now + 6; s_ch_rr = (unsigned)ch + 1;   /* v1061H: the co-op thread flushes every ~2 ms (Sleep(2)); 34 ms throttled to ~90 poses/s */
    s_sel_last[i] = now;
    {   /* fixed schedule per NPC, no drift */
        const uint32_t per = 1000u / (uint32_t)(i < s_near ? s_hz : i < s_mid ? s_hz_far : s_hz_far2);   /* v1061H: selection is sorted nearest first; v1063P4 third tier */
        s_sel_next[i] = (s_sel_next[i] && now - s_sel_next[i] < per) ? s_sel_next[i] + per : now + per;
    }
    return 1;
}
/* v1064Q: the host game does not anim-update every synced ped every frame (peds far from the HOST's camera / player are
 * LOD'd: no func_0046316C -> no gf_coop_after_anim -> no capture). 13:55 coop-local: host 28 selected, guest only 17 live
 * streams, "stale 11066" (stream silent > hold -> guest proxy fell back to its own anim), "proxy without stream 2026".
 * Fix: from the main-thread tick capture every selected ped whose last capture is older than GF_COOP_WPOSE_IDLE_MS (its
 * palette = exactly what the host shows, frozen if the host does not animate it) at the ped's tier rate. */
/* v1065A hand edit in func_0046316C_orig right after vt+0x178(ent, &a, &b): returns 1 = clear both cull bytes (full eval). */
int gf_wpose_force_eval(uint32_t ent, int a, int b)
{
    uint32_t now;
    if ((a || b) && gf_npcfix_force_eval(ent)) return 1;   /* v1067X: followed host NPC needs a palette for the joiner's pose */
    if ((!a && !b) || s_on != 1 || s_force_eval != 1 || !s_sel_n || !ent || !is_host_side() || !gf_coop_connected()) return 0;
    for (int i = 0; i < s_sel_n; i++) if (s_sel_ent[i] == ent) {
        st_feval_seen++;
        now = gf_coop_pose_now_ms();
        if (s_force_eval == 2) { st_feval++; return 1; }   /* v1066N GF_COOP_WPOSE_FORCE_EVAL=2: always */
        if (s_sched == 1) { if (s_in_fanim || s_grant_at[i]) { st_feval++; return 1; } return 0; }   /* v1066N: granted = captured on the next tick */
        if (!s_in_fanim && s_sel_next[i] && (int32_t)(now - s_sel_next[i]) < 0) return 0;   /* not due for a capture: keep the game's cheap path */
        st_feval++; return 1;
    }
    return 0;
}
static int wp_fanim(int i, uint32_t now)   /* v1065A: 1 = called func_0046316C(ent, 0, dt) */
{
    static gfn s_fn; static int s_fn_ok; uint32_t ent = s_sel_ent[i], vt = 0, c4 = 0, fl = 0; float dt;
    if (!s_fn_ok) { s_fn_ok = 1; for (uint64_t k = 0; k < function_table_count; k++) if ((uint32_t)function_table[k].addr == 0x0046316Cu) { s_fn = function_table[k].func; break; }
        if (!s_fn) { printf("[coop-wpose] v1065A func_0046316C not in the function table -> forced anim update off\n"); fflush(stdout); } }
    if (!s_fn || !g_active_ctx || !ent || !s_sel_vt[i] || !gf_memq_access(vm_base, ent, 0xC8, 0)) { st_fanim_skip++; if (!s_fn || !g_active_ctx) st_fskip_fn++; else st_fskip_mem++; return 0; }
    __try { const uint8_t* q = vm_base + ent;
        vt = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];
        fl = ((uint32_t)q[0x34] << 24) | ((uint32_t)q[0x35] << 16) | ((uint32_t)q[0x36] << 8) | q[0x37];
        c4 = ((uint32_t)q[0xC4] << 24) | ((uint32_t)q[0xC5] << 16) | ((uint32_t)q[0xC6] << 8) | q[0xC7]; } __except (EXCEPTION_EXECUTE_HANDLER) { st_fanim_skip++; return 0; }
    if (vt != s_sel_vt[i] || !(fl & 1u) || c4 < 0x10000u || c4 >= 0xF0000000u || !gf_memq_access(vm_base, c4, 0x20, 0)) { st_fanim_skip++;   /* freed / reused / no anim controller */
        if (vt != s_sel_vt[i]) st_fskip_vt++; else if (!(fl & 1u)) st_fskip_alive++; else st_fskip_noanim++;   /* v1066N reasons */
        return 0; }
    dt = s_sel_anim[i] ? (float)(uint32_t)(now - s_sel_anim[i]) * 0.001f : 0.033f;
    if (dt < 0.005f) dt = 0.005f; if (dt > 0.25f) dt = 0.25f;
    {
        ppu_context* cur = g_active_ctx; static ppu_context c;
        memcpy(&c, cur, sizeof c);
        c.gpr[1] = ((uint32_t)cur->gpr[1] - 0x1000u) & ~0xFull; c.gpr[2] = 0x00B4BF60u;
        c.gpr[3] = ent; c.gpr[4] = 0; c.gpr[5] = 0; c.fpr[1] = (double)dt; c.lr = 0;
        s_sel_next[i] = 0; s_in_fanim = 1;
        __try {
            g_active_ctx = &c; s_fn(&c);
            while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&c); }
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_trampoline_fn = 0; st_fanim_fail++; s_force_anim = 0;
            printf("[coop-wpose] v1065A forced anim update of ent %08X FAULTED -> forced anim update disabled\n", ent); fflush(stdout); }
        g_active_ctx = cur; s_in_fanim = 0;
    }
    st_fanim++;
    return 1;
}
static void host_idle_capture(void)
{
    const uint32_t now = gf_coop_pose_now_ms(); int done = 0, idle = 0;
    if (s_idle_ms <= 0 || !s_sel_n || !gf_coop_connected()) return;
    for (int i = 0; i < s_sel_n && done < s_idle_per_tick; i++) {
        const uint32_t per = 1000u / (uint32_t)(i < s_near ? s_hz : i < s_mid ? s_hz_far : s_hz_far2);
        const uint32_t gap = (uint32_t)s_idle_ms > per ? (uint32_t)s_idle_ms : per;
        if (!s_sel_ent[i] || (s_sel_last[i] && now - s_sel_last[i] < gap)) continue;
        idle++;
        if (s_force_anim == 1 && g_active_ctx && (!s_sel_anim[i] || now - s_sel_anim[i] >= gap)) {   /* v1065A: the game did not anim-update it */
            const uint32_t before = s_sel_last[i];
            if (wp_fanim(i, now) && s_sel_last[i] != before) { st_idlecap++; done++; continue; }   /* live pose captured by gf_wpose_after_anim */
        }
        {
            int ok = 0;
            __try { ok = wp_capture_send(i, s_sel_ent[i], now); } __except (EXCEPTION_EXECUTE_HANDLER) { s_sel_last[i] = now; ok = -1; }
            if (ok > 0) { st_idlecap++; done++; } else if (ok == -2) break;   /* no free slot: next tick (v1066N: a failed capture no longer stops the loop) */
        }
    }
    st_idle_peds = idle;
}

/* v1066N host scheduler tick (main thread, once per frame from gf_wpose_tick). */
/* v1071A (user 13:18 clip: NPCs near the joiner walk with a flipping / backwards stride): the tiers gave every ped 15 / 8 / 5 Hz by
 * rank alone, and with the anim-update wait the effective rate was ~4-8 poses/s (host 13:19: 153 captures/s for 36 peds) -- a
 * ~1.1 s walk cycle sampled that slowly and interpolated aliases (legs swing backwards). Now the rate follows motion: a WALKING ped
 * (source speed > 0.6 m/s, incl. a falling corpse) among the GF_COOP_WPOSE_MOVE_RANK (32) nearest the partner gets at least
 * GF_COOP_WPOSE_HZ_MOVE (15) Hz; a STANDING ped at most GF_COOP_WPOSE_HZ_IDLE (6) Hz -- that budget goes to the walkers.
 * GF_COOP_WPOSE_HZ_MOVE=0 = old tiers. */
static int s_hz_move = -1, s_hz_idle = 6, s_move_rank = 32; static float s_sel_spd[WP_SEL_MAX];
/* v1119F (user 20:16 "animation desync on the other player AND the NPCs, especially when fighting"): a fighting ped barely moves
 * its root, so v1071A classed it STANDING -> <= 6 Hz -> punches / hit reactions (100-300 ms) were smeared or lost. Now the
 * GF_COOP_WPOSE_FIGHT_N (6) nearest peds within GF_COOP_WPOSE_FIGHT_R (6 m) of ANY player (local or remote) get
 * GF_COOP_WPOSE_FIGHT_HZ (30) Hz whatever their speed. s_sel_d = distance to the nearest player, set in host_select. */
static float s_fight_r = -1.0f; static int s_fight_hz = 30, s_fight_n = 6; static volatile long st_m_fight;
static volatile long st_m_walk, st_m_stand, st_m_unk;
static void move_cfg(void)
{
    const char* e;
    if (s_hz_move >= 0) return;
    s_hz_move = 15;
    if ((e = getenv("GF_COOP_WPOSE_HZ_MOVE")) && *e) s_hz_move = atoi(e) < 0 ? 0 : atoi(e) > 30 ? 30 : atoi(e);
    if ((e = getenv("GF_COOP_WPOSE_HZ_IDLE")) && *e) s_hz_idle = atoi(e) < 0 ? 0 : atoi(e) > 30 ? 30 : atoi(e);
    if ((e = getenv("GF_COOP_WPOSE_MOVE_RANK")) && atoi(e) > 0) s_move_rank = atoi(e);
    printf("[coop-wpose] v1071A pose rate by motion: walking peds (nearest %d) >= %d Hz, standing <= %d Hz %s (GF_COOP_WPOSE_HZ_MOVE)\n",
           s_move_rank, s_hz_move, s_hz_idle, s_hz_move ? "ON" : "off (old rank tiers)"); fflush(stdout);
    s_fight_r = 6.0f;
    if ((e = getenv("GF_COOP_WPOSE_FIGHT_R")) && *e) s_fight_r = (float)atof(e);
    if ((e = getenv("GF_COOP_WPOSE_FIGHT_HZ")) && atoi(e) > 0) s_fight_hz = atoi(e) > 30 ? 30 : atoi(e);
    if ((e = getenv("GF_COOP_WPOSE_FIGHT_N")) && atoi(e) >= 0) s_fight_n = atoi(e);
    printf("[coop-wpose] v1119F fight range: the %d nearest peds within %.1f m of any player get %d Hz (GF_COOP_WPOSE_FIGHT_R / _HZ / _N)\n", s_fight_n, (double)s_fight_r, s_fight_hz); fflush(stdout);
}
static int is_fight(int i)   /* i among the s_fight_n nearest-to-a-player peds that are within s_fight_r */
{
    int closer = 0;
    if (s_fight_r <= 0.0f || s_fight_n <= 0 || i >= WP_SEL_MAX || !(s_sel_d[i] < s_fight_r)) return 0;
    for (int j = 0; j < s_sel_n && j < WP_SEL_MAX; j++) if (j != i && s_sel_d[j] < s_sel_d[i]) closer++;
    return closer < s_fight_n;
}
static uint32_t per_of(int i)
{
    uint32_t per = 1000u / (uint32_t)(i < s_near ? s_hz : i < s_mid ? s_hz_far : s_hz_far2);
    if (s_hz_move > 0 && i < WP_SEL_MAX) {
        const float v = s_sel_spd[i];
        if (v > 0.6f && i < s_move_rank) { const uint32_t pm = 1000u / (uint32_t)s_hz_move; if (pm < per) per = pm; }
        else if (v >= 0.0f && v <= 0.6f && s_hz_idle > 0) { const uint32_t pi = 1000u / (uint32_t)s_hz_idle; if (pi > per) per = pi; }
    }
    if (s_fight_hz > 0 && is_fight(i)) { const uint32_t pf = 1000u / (uint32_t)s_fight_hz; if (pf < per) per = pf; }   /* v1119F */
    return per;
}
static void host_sched_tick(void)
{
    const uint32_t now = gf_coop_pose_now_ms(); int sent = 0, pending = 0;
    if (!s_sel_n || !gf_coop_connected()) return;
    move_cfg();
    if (s_hz_move > 0) { extern float gf_coop_world_src_speed(uint16_t netid);   /* v1071A */
        for (int i = 0; i < s_sel_n && i < WP_SEL_MAX; i++) { float v = -1.0f; __try { v = gf_coop_world_src_speed(s_sel_id[i]); } __except (EXCEPTION_EXECUTE_HANDLER) { v = -1.0f; }
            s_sel_spd[i] = v; if (v > 0.6f) st_m_walk++; else if (v >= 0.0f) st_m_stand++; else st_m_unk++; if (is_fight(i)) st_m_fight++; } }
    for (int i = 0; i < s_sel_n; i++) {   /* phase A: capture the peds granted on an earlier tick */
        int updated, ok = 0;
        if (!s_grant_at[i]) continue;
        if (!s_sel_ent[i]) { s_grant_at[i] = 0; continue; }
        if (sent >= s_nch) { pending++; continue; }
        updated = s_sel_anim[i] && (int32_t)(s_sel_anim[i] - s_grant_at[i]) >= 0;
        if (!updated) {   /* LOD peds are anim-updated every few frames: wait for it; forced update only when it is really idle */
            const int idle = !s_sel_anim[i] || now - s_sel_anim[i] >= (uint32_t)(s_idle_ms > 0 ? s_idle_ms : 300);
            if (!idle && ++s_grant_ticks[i] < 30) { pending++; st_g_wait++; continue; }
            if (idle && s_force_anim == 1 && g_active_ctx && wp_fanim(i, now)) st_g_forced++;   /* -> after_anim -> s_sel_anim (forced eval: s_in_fanim) */
            else st_g_unanim++;   /* no anim at all on the host: send the palette as the host shows it */
        } else st_g_fresh++;
        __try { ok = wp_capture_send(i, s_sel_ent[i], now); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; s_sel_last[i] = now; s_sel_fail[i] = (now + 1000) | 1u; }
        if (ok == -2) { st_g_noslot++; pending++; break; }
        s_grant_at[i] = 0; s_grant_ticks[i] = 0;
        if (ok > 0) sent++; else st_g_failed++;
    }
    {   /* phase B: grant the most overdue peds (score = time since last capture / tier period, due at >= 1) */
        int freeslots = s_nch + s_nch / 2 - pending;   /* outstanding grants <= 1.5 x slots */
        while (freeslots > 0) {
            int best = -1; float bs = 0.0f;
            for (int i = 0; i < s_sel_n; i++) {
                float sc; uint32_t per;
                if (s_grant_at[i] || !s_sel_ent[i]) continue;
                if (s_sel_fail[i] && (int32_t)(now - s_sel_fail[i]) < 0) continue;   /* rig failed < 1 s ago */
                per = per_of(i);
                sc = s_sel_last[i] ? (float)(uint32_t)(now - s_sel_last[i] + 17u) / (float)per : 1000.0f - (float)i;   /* +17: one frame until the capture */
                if (sc >= 1.0f && sc > bs) { bs = sc; best = i; }
            }
            if (best < 0) break;
            s_grant_at[best] = now ? now : 1; s_grant_ticks[best] = 0; st_g_grant++; freeslots--;
            if (s_sel_last[best]) { const uint32_t late = now - s_sel_last[best]; st_g_late_sum += (long)(late > per_of(best) ? late - per_of(best) : 0); st_g_late_n++; }
        }
    }
}
static void host_sched_log(void)
{
    const uint32_t now = gf_coop_pose_now_ms(); int starved = 0, norig = 0, ns = 0; char lst[220]; int lp = 0;
    lst[0] = 0;
    for (int i = 0; i < s_sel_n; i++) {
        const int st = !s_sel_last[i] || now - s_sel_last[i] > 2000u, nr = s_sel_fail[i] && (int32_t)(now - s_sel_fail[i]) < 0;
        if (st) starved++; if (nr) norig++;
        if ((st || nr) && ns < 10 && lp < 190) { lp += snprintf(lst + lp, sizeof lst - (size_t)lp, " %u%s@%.0fm", s_sel_id[i], nr ? "(no rig)" : "", (double)(s_sel_d[i] < 1e8f ? s_sel_d[i] : -1.0f)); ns++; }   /* v1127W: + distance to the nearest player */
    }
    printf("[coop-wpose] v1066N host sched: grants %ld | captured fresh %ld, after forced anim %ld, unanimated %ld, failed %ld | waits %ld, no slot %ld | mean late %.0f ms | now: starved (> 2 s) %d, no rig %d of %d selected%s%s | fanim skip: fn %ld, vt %ld, alive %ld, no anim ctl %ld, mem %ld\n",
           st_g_grant, st_g_fresh, st_g_forced, st_g_unanim, st_g_failed, st_g_wait, st_g_noslot, st_g_late_n ? (double)st_g_late_sum / (double)st_g_late_n : 0.0,
           starved, norig, s_sel_n, ns ? " ->" : "", lst, st_fskip_fn, st_fskip_vt, st_fskip_alive, st_fskip_noanim, st_fskip_mem);
}

/* ---------------------------------------------------------------- guest */
typedef struct { uint32_t ms; int len; uint8_t d[GF_COOP_POSE_MAX_PACKET]; } wp_pkt;
typedef struct { uint16_t netid; int n; ULONGLONG last_rx; wp_pkt b[WP_BUF]; } wp_stream;
static wp_stream s_st[WP_STREAMS];
static SRWLOCK s_lock = SRWLOCK_INIT;
static double s_clk_off; static int s_clk_ok;
static volatile long st_test_drop;   /* v1066N */
static volatile long st_rx, st_apply, st_interp, st_hold, st_badrig, st_norig, st_old, st_nostream;

void gf_wpose_on_blob(uint8_t ch, const void* data, int len)   /* co-op thread: copy only */
{
    const uint8_t* p = (const uint8_t*)data; uint16_t id; uint32_t ms; int pl = len - 2, slot = -1;
    static uint8_t s_xp[WP_PKT];   /* v1072W2 expanded copy (co-op thread only) */
    (void)ch;
    if (s_on != 1 || !p || pl < 16) return;
    if (p[2 + 4] == 2) {   /* v1072W2 compact -> v1 */
        const int xl = wp_v2_to_v1(p + 2, pl, s_xp + 2, (int)sizeof s_xp - 2);
        if (xl <= 0) { st_cmp_bad++; return; }
        s_xp[0] = p[0]; s_xp[1] = p[1]; p = s_xp; pl = xl; st_cmp_rx++; }
    if (pl > GF_COOP_POSE_MAX_PACKET) return;
    id = (uint16_t)(p[0] | (p[1] << 8));
    {   /* v1066N echo/test aid GF_COOP_WPOSE_TEST_STARVE=1: drop every 3rd net id's poses 6 s of every 12 s (donor / cross-fade test) */
        static int starve = -1; if (starve < 0) { const char* e = getenv("GF_COOP_WPOSE_TEST_STARVE"); starve = e && *e == '1'; }
        if (starve && (id % 3u) == 0 && (GetTickCount64() / 6000u) % 2u == 1) { st_test_drop++; return; }
    }
    ms = (uint32_t)p[14] | ((uint32_t)p[15] << 8) | ((uint32_t)p[16] << 16) | ((uint32_t)p[17] << 24);   /* pose header +12 */
    AcquireSRWLockExclusive(&s_lock);
    {
        const double now = (double)gf_coop_pose_now_ms(), off = now - (double)ms;
        static double s_clk_t;   /* v1063P4: relax by wall time (1 ms per s), not per packet (+0.03 ms/packet = +11 ms/s at 48 peds) */
        if (!s_clk_ok || off < s_clk_off || off - s_clk_off > 2000.0) { s_clk_off = off; s_clk_ok = 1; }
        else if (s_clk_t > 0.0 && now > s_clk_t) s_clk_off += (now - s_clk_t) * 0.001;
        s_clk_t = now;
    }
    for (int i = 0; i < WP_STREAMS; i++) if (s_st[i].n && s_st[i].netid == id) { slot = i; break; }
    if (slot < 0) {   /* free stream, else the one silent longest */
        ULONGLONG oldest = ~0ull;
        for (int i = 0; i < WP_STREAMS; i++) { if (!s_st[i].n) { slot = i; break; } if (s_st[i].last_rx < oldest) { oldest = s_st[i].last_rx; slot = i; } }
        s_st[slot].n = 0; s_st[slot].netid = id;
    }
    {
        wp_stream* s = &s_st[slot];
        if (s->n && (int32_t)(ms - s->b[s->n - 1].ms) <= 0) { ReleaseSRWLockExclusive(&s_lock); return; }   /* late / duplicate: drop */
        if (s->n == WP_BUF) { memmove(&s->b[0], &s->b[1], sizeof(wp_pkt) * (WP_BUF - 1)); s->n--; }
        s->b[s->n].ms = ms; s->b[s->n].len = pl; memcpy(s->b[s->n].d, p + 2, (size_t)pl); s->n++;
        s->last_rx = GetTickCount64();
    }
    st_rx++;
    ReleaseSRWLockExclusive(&s_lock);
}
static void wp_apply(uint32_t ent, uint32_t anim, uint16_t id);
static uint32_t s_aent[64], s_aanim[64]; static unsigned s_ai;   /* v1061H: ent -> anim controller seen in post-skel */
static volatile long st_reapply, st_stale_anim, st_recopy;   /* v1063P9; v1063P11 recopy */
void gf_wpose_post_skel(uint32_t anim)   /* main PPU thread, after an entity's skeleton update (gf_coop_pose_post_skel) */
{
    uint32_t ent; uint16_t id; int k;
    gf_npcfix_post_skel(anim);   /* v1067X: host-side grabbed-NPC pose apply */
    if (s_on != 1 || !is_guest_side()) return;
    ent = gf_rag_cur_ent();
    if (!ent || !(id = gf_proxy_netid_of(ent))) return;
    v70_wcfg();
    if (gf_coop_pose_anim_live(ent, anim, 0)) st_v70_c4_ok++; else st_v70_c4_bad++;   /* v1070A diag: post-skel controller == ent+0xC4 */
    if (s_v70_own && gf_coop_world_src_has(ent)) { st_v70_own_block++;   /* v1070A: never a remote pose on our own source */
        { static int nl; if (nl < 6) { nl++; printf("[coop-wpose] v1070A own-source guard: ent %08X is proxy net %u AND a local source -> remote pose not applied\n", ent, id); fflush(stdout); } }
        return; }
    for (k = 0; k < 64; k++) if (s_aent[k] == ent) { s_aanim[k] = anim; break; }
    if (k == 64) { s_aent[s_ai & 63] = ent; s_aanim[s_ai & 63] = anim; s_ai++; }
    wp_apply(ent, anim, id);
}
/* v1061H: same pose again right after the whole anim update (func_0046316C) -- anything the game runs after the skeleton
 * update (look-at / IK toward a player) must not win over the host's pose. */
static void wp_reapply(uint32_t ent)
{
    uint16_t id;
    if (!is_guest_side() || !(id = gf_proxy_netid_of(ent))) return;
    for (int k = 0; k < 64; k++) if (s_aent[k] == ent) {
        if (!gf_coop_pose_anim_live(ent, s_aanim[k], 1)) {   /* v1070A: controller freed (LOD strip) / replaced -> nothing to re-apply, drop the caches */
            extern void gf_coop_pose_cache_forget(uint32_t e);
            s_aent[k] = 0; gf_coop_pose_cache_forget(ent); st_v70_reapply_block++; return; }
        v70_wcfg();
        if (s_v70_own && gf_coop_world_src_has(ent)) { st_v70_own_block++; return; }
        {   /* v1063P11 GF_COOP_WPOSE_REAPPLY: 2 (default) = copy back the palette written in post-skel, 1 = full decode again (old), 0 = off */
            static int ra = -1; if (ra < 0) { const char* e = getenv("GF_COOP_WPOSE_REAPPLY"); ra = e && *e ? atoi(e) : 2; }
            extern int gf_coop_pose_rewrite(uint32_t ent, uint32_t anim);
            if (ra == 0) return;
            if (ra == 2 && gf_coop_pose_rewrite(ent, s_aanim[k])) { st_reapply++; st_recopy++; return; }
        }
        st_reapply++; wp_apply(ent, s_aanim[k], id); return;
    }
}
void gf_wpose_forget(uint32_t ent)   /* v1063P9 (Codex J): ent -> anim cache is keyed by address; kill_ent drops it so a reused body never gets an older body's anim */
{
    for (int k = 0; k < 64; k++) if (s_aent[k] == ent) { s_aent[k] = 0; st_stale_anim++; }
    { extern void gf_coop_pose_cache_forget(uint32_t e); gf_coop_pose_cache_forget(ent); }   /* v1063P11 */
}
/* v1066N (agent N) guest: no frozen statues.
 * coop-local 16:3x: "applied 117041 (interpolated 88746, held 28295)" + "proxy without stream 4062": when the playback time
 * ran past the newest pose (late / starved stream) the proxy HELD that pose while the world snapshots kept moving its root
 * (statue sliding), and proxies without any stream played their own idle while sliding. Now:
 *  - short gaps: extrapolate the last two poses (up to GF_COOP_WPOSE_EXTRAP = 0.5 intervals, <= 150 ms) instead of holding;
 *  - no stream / stale (> GF_COOP_WPOSE_STALE_MS 250 ms past the newest pose) while the proxy MOVES (root speed from the
 *    snapshots > 0.45 m/s), or stale > 1.5 s while standing: DONOR fallback (GF_COOP_WPOSE_DONOR=1) = play the live stream of
 *    another proxy with the same rig and a similar speed (walk / run / idle class), the proxy's own face bones kept, and
 *    cross-faded in / out over GF_COOP_WPOSE_XFADE_MS (250) when its own stream stops / comes back.
 * The vel "for locomotion anim speed" of gf_proxy_drive never reached the game's anim system (only the optional smoothing
 * extrapolation used it) -- driving the proxy's own locomotion graph would need its speed input RE'd; the donor gives a
 * real walk cycle at about the right speed with the existing pose path. */
static int s_donor = -1, s_stale_ms = 250, s_xfade_ms = 250; static float s_extrap = 0.5f, s_move_v = 0.45f;
typedef struct { uint16_t netid, donor; float w; uint32_t t_ms, pick_ms; } wp_gst;
static wp_gst s_gs[WP_STREAMS]; static unsigned s_gs_i;
static volatile long st_extrap, st_frozen, st_donor_apply, st_donor_blend, st_donor_pick, st_donor_none, st_donor_nostream, st_donor_stale;
extern float gf_proxy_speed_of(uint16_t netid);
extern double gf_coop_world_play_ms(void);   /* v1066C (agent C): world playback time on the host clock, -1 = none */
static int s_wclock = 1; static double st_wclk_diff; static long st_wclk_n, st_wclk_used;
static void donor_cfg(void)
{
    const char* e;
    if (s_donor >= 0) return;
    e = getenv("GF_COOP_WPOSE_DONOR"); s_donor = !(e && *e == '0');
    e = getenv("GF_COOP_WPOSE_WORLDCLOCK"); s_wclock = !(e && *e == '0');   /* v1066N: play poses at C's world playback time (root and limbs in step) */
    if ((e = getenv("GF_COOP_WPOSE_STALE_MS")) && atoi(e) > 0) s_stale_ms = atoi(e);
    if ((e = getenv("GF_COOP_WPOSE_XFADE_MS")) && atoi(e) > 0) s_xfade_ms = atoi(e);
    if ((e = getenv("GF_COOP_WPOSE_EXTRAP"))) { s_extrap = (float)atof(e); if (s_extrap < 0.0f) s_extrap = 0.0f; if (s_extrap > 1.0f) s_extrap = 1.0f; }
    printf("[coop-wpose] v1066N guest: extrapolation %.2f intervals (<= 150 ms), donor fallback %s (stale > %d ms or no stream while moving > %.2f m/s), cross-fade %d ms\n",
           (double)s_extrap, s_donor ? "ON" : "off", s_stale_ms, (double)s_move_v, s_xfade_ms); fflush(stdout);
}
static wp_gst* gst_get(uint16_t id, int create)
{
    for (int i = 0; i < WP_STREAMS; i++) if (s_gs[i].netid == id) return &s_gs[i];
    if (!create) return 0;
    { wp_gst* g = &s_gs[s_gs_i++ % WP_STREAMS]; memset(g, 0, sizeof *g); g->netid = id; return g; }
}
/* the two poses of s around host time t (caller holds s_lock); *beyond = t - newest pose (ms) */
static double s_wp_iv;   /* v1071A: interval (ms) between the two poses wp_pair picked (0 = one pose) */
static void wp_pair(const wp_stream* s, double t, uint8_t* pa, int* la, uint8_t* pb, int* lb, float* u, double* beyond, int* ex)
{
    const wp_pkt* nb = &s->b[s->n - 1];
    *la = 0; *u = 1.0f; *ex = 0; *beyond = t - (double)nb->ms; s_wp_iv = 0.0;
    if (t >= (double)nb->ms || s->n == 1) {
        if (s->n >= 2 && s_extrap > 0.0f && t > (double)nb->ms) {
            const wp_pkt* na = &s->b[s->n - 2]; const double iv = (double)(int32_t)(nb->ms - na->ms);
            if (iv > 0.0 && iv < 1000.0) {
                double uu = (t - (double)na->ms) / iv; const double um = 1.0 + (double)s_extrap;
                if (uu > um) uu = um;
                if ((uu - 1.0) * iv > 150.0) uu = 1.0 + 150.0 / iv;
                *la = na->len; memcpy(pa, na->d, (size_t)na->len); *lb = nb->len; memcpy(pb, nb->d, (size_t)nb->len); *u = (float)uu; *ex = 1; s_wp_iv = iv;
                return;
            }
        }
        *lb = nb->len; memcpy(pb, nb->d, (size_t)nb->len);
        return;
    }
    {
        int k = 0;
        while (k + 1 < s->n && (double)s->b[k + 1].ms <= t) k++;
        if ((double)s->b[k].ms > t) { *lb = s->b[0].len; memcpy(pb, s->b[0].d, (size_t)*lb); }
        else {
            *la = s->b[k].len; memcpy(pa, s->b[k].d, (size_t)*la); *lb = s->b[k + 1].len; memcpy(pb, s->b[k + 1].d, (size_t)*lb);
            *u = (float)((t - (double)s->b[k].ms) / (double)(int32_t)(s->b[k + 1].ms - s->b[k].ms)); s_wp_iv = (double)(int32_t)(s->b[k + 1].ms - s->b[k].ms);
        }
    }
}
/* caller holds s_lock: a fresh stream of another proxy with the same rig key and a similar root speed (-1 = none) */
static int wp_find_donor(uint16_t self, uint32_t key, float spd, double t, uint16_t cur)
{
    const ULONGLONG now = GetTickCount64(); const int mv = spd > s_move_v; int best = -1; float bc = 1e9f;
    for (int i = 0; i < WP_STREAMS; i++) {
        const wp_stream* s = &s_st[i]; float sd, c;
        if (!s->n || s->netid == self || now - s->last_rx > 1000) continue;
        if (t - (double)s->b[s->n - 1].ms > (double)s_stale_ms) continue;   /* the donor itself must be live */
        if (gf_coop_pose_wire_key(s->b[s->n - 1].d, s->b[s->n - 1].len) != key) continue;
        { const wp_gst* dg = gst_get(s->netid, 0); if (dg && dg->w > 0.0f) continue; }   /* never a donor that plays a donor */
        sd = gf_proxy_speed_of(s->netid);
        if (sd < 0.0f || (sd > s_move_v) != mv) continue;   /* walk/run needs a moving donor, standing a standing one */
        c = fabsf(sd - spd) - (s->netid == cur ? 0.5f : 0.0f);   /* hysteresis: keep the current donor */
        if (c < bc) { bc = c; best = i; }
    }
    return best;
}
/* v1071A receiver: proxies whose owner flags them as corpses (WSN2 flag 4, gf_coop_world.c guest_tick, main thread). A body never
 * gets a donor walk and never falls back to its own (live ped) animation: when its stream is silent / old it keeps its last
 * applied pose. GF_COOP_CORPSE_HOLD=0 = old. Diag: pose intervals used by WALKING proxies (the aliasing check). */
enum { WP_CPX = 64 };
static uint16_t s_cpx_id[WP_CPX]; static ULONGLONG s_cpx_ms[WP_CPX]; static int s_cp_hold = -1;
static volatile long st_cp_hold, st_cp_nodonor, st_cp_holdfail, st_iv_n, st_iv_150, st_iv_250, st_iv_ext; static double st_iv_sum;
void gf_wpose_set_corpse(uint16_t id, int on)
{
    const ULONGLONG now = GetTickCount64(); int free_i = -1;
    if (!id) return;
    for (int i = 0; i < WP_CPX; i++) {
        if (s_cpx_id[i] == id) { if (on) s_cpx_ms[i] = now; else s_cpx_id[i] = 0; return; }
        if (free_i < 0 && (!s_cpx_id[i] || now - s_cpx_ms[i] > 3000)) free_i = i;
    }
    if (on && free_i >= 0) { s_cpx_id[free_i] = id; s_cpx_ms[free_i] = now; }
}
static int wp_is_corpse(uint16_t id)
{
    const ULONGLONG now = GetTickCount64();
    if (s_cp_hold < 0) { const char* e = getenv("GF_COOP_CORPSE_HOLD"); s_cp_hold = !(e && *e == '0'); }
    if (!s_cp_hold || !id) return 0;
    for (int i = 0; i < WP_CPX; i++) if (s_cpx_id[i] == id) return now - s_cpx_ms[i] < 3000;
    return 0;
}
static int wp_corpse_count(void) { const ULONGLONG now = GetTickCount64(); int n = 0; for (int i = 0; i < WP_CPX; i++) if (s_cpx_id[i] && now - s_cpx_ms[i] < 3000) n++; return n; }
static void wp_apply(uint32_t ent, uint32_t anim, uint16_t id)
{
    static uint8_t pa[GF_COOP_POSE_MAX_PACKET], pb[GF_COOP_POSE_MAX_PACKET], da[GF_COOP_POSE_MAX_PACKET], db[GF_COOP_POSE_MAX_PACKET];
    int la = 0, lb = 0, lda = 0, ldb = 0, found = 0, silent = 0, r = 0, ex = 0, dex = 0, want = 0, di = -1; float u = 1.0f, du = 1.0f, spd = -1.0f, w = 0.0f;
    double t, beyond = 1e9, dbeyond = 0.0; uint32_t key = 0; int lslots = 0, havekey = 0; wp_gst* g = 0;
    const uint32_t nowc = gf_coop_pose_now_ms();
    donor_cfg();
    const int corpse = wp_is_corpse(id);   /* v1071A */
    if (s_donor) { spd = gf_proxy_speed_of(id); havekey = gf_coop_pose_rig_key(ent, anim, &key, &lslots); }
    if (!s_donor && !corpse) spd = gf_proxy_speed_of(id);   /* v1071A diag */
    AcquireSRWLockShared(&s_lock);
    t = (double)nowc - s_clk_off - s_delay;   /* playback time on the host clock */
    if (s_wclock) {   /* v1066N: same playback time as the world snapshots (C's adaptive clock) when it is sane */
        extern double gf_coop_world_play_ms_for(uint16_t id); extern int gf_coop_world_origin_is_primary(uint16_t id);
        const double tw = gf_coop_world_play_ms_for(id);   /* v1072W3: this id's origin clock */
        if (tw >= 0.0 && s_clk_ok && (fabs(tw - t) < 1000.0 || !gf_coop_world_origin_is_primary(id))) { st_wclk_diff += tw - t; st_wclk_n++; st_wclk_used++; t = tw; }
    }
    for (int i = 0; i < WP_STREAMS; i++) {
        const wp_stream* s = &s_st[i];
        if (!s->n || s->netid != id) continue;
        found = 1;
        if (GetTickCount64() - s->last_rx > (ULONGLONG)s_hold_ms) { silent = 1; break; }   /* stream silent > GF_COOP_WPOSE_HOLD_MS: leave the proxy's own animation (or a donor) */
        wp_pair(s, t, pa, &la, pb, &lb, &u, &beyond, &ex);
        if (spd > 0.6f && !corpse && s_wp_iv > 0.0) { st_iv_n++; st_iv_sum += s_wp_iv; if (s_wp_iv > 150.0) st_iv_150++; if (s_wp_iv > 250.0) st_iv_250++; if (ex) st_iv_ext++; }   /* v1071A */
        break;
    }
    if (corpse && s_donor) st_cp_nodonor++;
    if (s_donor && havekey && spd >= 0.0f && !corpse) {   /* v1071A: never a donor walk on a body */
        const int stale = !found || silent || beyond > (double)s_stale_ms;
        want = stale && (spd > s_move_v || (found && !silent && beyond > 1500.0));
        if (want) {
            g = gst_get(id, 1);
            if (g->donor && nowc - g->pick_ms < 1000u) {   /* keep the donor for 1 s while it stays live */
                for (int i = 0; i < WP_STREAMS; i++) if (s_st[i].n && s_st[i].netid == g->donor) {
                    if (GetTickCount64() - s_st[i].last_rx <= 1000 && t - (double)s_st[i].b[s_st[i].n - 1].ms <= (double)s_stale_ms) di = i;
                    break; }
            }
            if (di < 0) { di = wp_find_donor(id, key, spd, t, g->donor); g->pick_ms = nowc; if (di >= 0 && s_st[di].netid != g->donor) { g->donor = s_st[di].netid; st_donor_pick++; } else if (di < 0) g->donor = 0; }
            if (di >= 0) wp_pair(&s_st[di], t, da, &lda, db, &ldb, &du, &dbeyond, &dex);
        }
    }
    ReleaseSRWLockShared(&s_lock);
    {   /* v1067X: after a guest grab ends, fade its last local pose into the resumed host stream. */
        const uint8_t* lp = NULL; int ll = 0; const float lw = gf_npcfix_handback(id, &lp, &ll);
        if (lw > 0.0f && lp && ll > 0) {
            gf_pose_pair L; L.pa = NULL; L.la = 0; L.pb = lp; L.lb = ll; L.u = 1.0f;
            if (!lb) r = gf_coop_pose_apply_mix(ent, anim, &L, NULL, 0.0f, 0);
            else { gf_pose_pair S; S.pa = la ? pa : NULL; S.la = la; S.pb = pb; S.lb = lb; S.u = u;
                   r = gf_coop_pose_apply_mix(ent, anim, &S, &L, lw, 0); }
            gf_npcfix_note_handback(!lb);
            if (r > 0) st_apply++;
            else { extern void gf_coop_pose_cache_forget(uint32_t e); gf_coop_pose_cache_forget(ent); if (r < 0) st_badrig++; else st_norig++; }
            return;
        }
    }
    if (!g) g = gst_get(id, 0);
    if (g) {   /* donor share: cross-fade toward 1 (donor) / 0 (own stream) */
        float step = (float)(uint32_t)(nowc - g->t_ms) / (float)s_xfade_ms; const float target = (want && ldb) ? 1.0f : 0.0f;
        if (!(step >= 0.0f) || step > 0.5f) step = 0.5f;
        g->t_ms = nowc;
        if (!lb || !ldb) g->w = target;   /* nothing to fade with */
        else if (g->w < target) { g->w += step; if (g->w > target) g->w = target; }
        else if (g->w > target) { g->w -= step; if (g->w < target) g->w = target; }
        w = g->w;
    }
    if (w > 0.0f && ldb) {
        gf_pose_pair D; D.pa = lda ? da : NULL; D.la = lda; D.pb = db; D.lb = ldb; D.u = du;
        if (w >= 0.999f || !lb) r = gf_coop_pose_apply_mix(ent, anim, &D, NULL, 0.0f, 1);
        else { gf_pose_pair S; S.pa = la ? pa : NULL; S.la = la; S.pb = pb; S.lb = lb; S.u = u; r = gf_coop_pose_apply_mix(ent, anim, &S, &D, w, 1); }
        if (!found) st_nostream++;   /* still counted: "proxy without stream" keeps its meaning */
        if (r > 0) { st_apply++; st_donor_apply++; if (w < 0.999f && lb) st_donor_blend++; if (!found || silent) st_donor_nostream++; else st_donor_stale++; return; }
        { extern void gf_coop_pose_cache_forget(uint32_t e); gf_coop_pose_cache_forget(ent); }
        if (r < 0) st_badrig++; else st_norig++;
        return;
    }
    if (corpse && (!found || !lb)) {   /* v1071A: a body keeps its last applied pose instead of its own (standing) animation */
        extern int gf_coop_pose_rewrite(uint32_t ent, uint32_t anim);
        int rw = 0; __try { rw = gf_coop_pose_rewrite(ent, anim); } __except (EXCEPTION_EXECUTE_HANDLER) { rw = 0; }
        if (rw > 0) { st_cp_hold++; return; }
        st_cp_holdfail++;
    }
    if (want && !ldb) st_donor_none++;
    if (!found || !lb) { extern void gf_coop_pose_cache_forget(uint32_t e); gf_coop_pose_cache_forget(ent); }   /* v1063P11: nothing applied -> no stale re-copy */
    if (!found) { st_nostream++; return; }
    if (!lb) { st_old++; return; }
    {
        gf_pose_pair S; S.pa = la ? pa : NULL; S.la = la; S.pb = pb; S.lb = lb; S.u = u;
        r = gf_coop_pose_apply_mix(ent, anim, &S, NULL, 0.0f, 0);
    }
    if (r > 0) { st_apply++; if (ex) st_extrap++; else if (la) st_interp++; else { st_hold++; if (beyond > (double)s_stale_ms) st_frozen++; } }
    else { extern void gf_coop_pose_cache_forget(uint32_t e); gf_coop_pose_cache_forget(ent); if (r < 0) st_badrig++; else st_norig++; }
}

/* ---------------------------------------------------------------- tick (main PPU thread, gf_coop_glue_tick) */
static volatile long st_np_pose;
static void wp_peer_blob(int peer, uint8_t ch, const void* d, int len)   /* v1072W3 host: pose streams of joiners 2.. */
{
    extern int gf_coop_peer_is_primary_id(int id); extern int gf_coop_npeer_active(void); int ok = 0;
    if (gf_coop_mode() != 1 || peer <= 0) return;
    __try { ok = gf_coop_npeer_active() && !gf_coop_peer_is_primary_id(peer); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    if (!ok) return;
    for (int i = 0; i < WP_NCH; i++) if (s_chmap[i] == ch) { InterlockedIncrement(&st_np_pose); gf_wpose_on_blob(ch, d, len); return; }
}
void gf_wpose_tick(void)
{
    {   static int s_reg; if (!s_reg) { typedef void (*pbf)(int, uint8_t, const void*, int); extern void gf_coop_add_peer_blob_handler(pbf fn); s_reg = 1;
            __try { gf_coop_add_peer_blob_handler(wp_peer_blob); } __except (EXCEPTION_EXECUTE_HANDLER) { } } }   /* v1072W3 */
    static ULONGLONG s_log;
    const ULONGLONG now = GetTickCount64();
    if (!wp_enabled() || !gf_coop_mode()) return;
    if (is_host_side() && now - s_sel_at >= 250) { s_sel_at = now; __try { host_select(); } __except (EXCEPTION_EXECUTE_HANDLER) { s_sel_n = 0; } }
    if (is_host_side()) { if (s_sched == 1) host_sched_tick(); else host_idle_capture(); }   /* v1064Q; v1066N scheduler */
    if (now - s_log >= 10000) {
        s_log = now;
        if (is_host_side()) printf("[coop-wpose] host: %d selected | captured %ld, sent %ld, capture failures %ld, slot waits %ld | v1062W2 corpse links %ld | v1064Q idle captures %ld (peds the host did not anim-update, last tick %ld)\n", s_sel_n, st_cap, st_sent, st_capfail, st_noslot, st_corpse_note, st_idlecap, st_idle_peds);
        if (is_host_side()) printf("[coop-wpose] v1065A off-screen: forced bone evals %ld (of %ld culled selected anim updates) | forced anim updates %ld (skipped %ld, faulted %ld)\n", st_feval, st_feval_seen, st_fanim, st_fanim_skip, st_fanim_fail);
        if (is_host_side() && s_sched == 1) host_sched_log();   /* v1066N */
        if (is_host_side()) { printf("[coop-wpose] v1072W owner: pose stream %.1f kB/s (%ld players ranked), %d peds selected | v1072W2 compact sent %ld (saved %.1f kB/s)\n", (double)st_pose_bytes / 10240.0, st_sel_refs, s_sel_n, st_cmp_tx, (double)st_cmp_saved / 10240.0); st_pose_bytes = 0; st_cmp_saved = 0; fflush(stdout); }
        if (is_guest_side()) { printf("[coop-wpose] v1072W2 receiver: compact poses expanded %ld, malformed %ld | v1072W3 pose packets from joiners 2.. %ld\n", st_cmp_rx, st_cmp_bad, st_np_pose); fflush(stdout); }
        if (is_host_side()) { gf_wpose_corpse_alive(0); printf("[coop-wpose] v1071A owner: pose rate by motion %s -- selected-ped ticks walking %ld / standing %ld / unknown %ld / v1119F fight range %ld | corpses kept after their last anim update (body still exists) %ld checks, dropped: gone %ld, %d s cap %ld\n",
               s_hz_move > 0 ? "ON" : "off", st_m_walk, st_m_stand, st_m_unk, st_m_fight, st_cp_exist, st_cp_gone, s_cp_keep_ms > 0 ? s_cp_keep_ms / 1000 : 0, st_cp_cap); fflush(stdout); }
        printf("[coop-wpose] v1071A receiver: walking proxies interpolate poses %.0f ms apart on average (> 150 ms %.1f %%, > 250 ms %.1f %%, extrapolated %.1f %%; %ld frames) | corpse proxies now %d, held last pose %ld (failed %ld), donor refused %ld\n",
               st_iv_n ? st_iv_sum / (double)st_iv_n : 0.0, st_iv_n ? 100.0 * (double)st_iv_150 / (double)st_iv_n : 0.0, st_iv_n ? 100.0 * (double)st_iv_250 / (double)st_iv_n : 0.0,
               st_iv_n ? 100.0 * (double)st_iv_ext / (double)st_iv_n : 0.0, st_iv_n, wp_corpse_count(), st_cp_hold, st_cp_holdfail, st_cp_nodonor); fflush(stdout);
        if (is_guest_side()) {
            int ns = 0; AcquireSRWLockShared(&s_lock); for (int i = 0; i < WP_STREAMS; i++) if (s_st[i].n && now - s_st[i].last_rx < 2000) ns++; ReleaseSRWLockShared(&s_lock);
            { extern volatile long g_wpose_face_cap, g_wpose_face_apply; printf("[coop-wpose] v1061G own faces captured %ld, kept on %ld applies\n", g_wpose_face_cap, g_wpose_face_apply); }
            printf("[coop-wpose] guest: %d live streams | rx %ld | applied %ld (interpolated %ld, held %ld) | rig mismatch %ld, no rig %ld, stale %ld, proxy without stream %ld | re-applied after anim %ld (copied %ld, anim cache dropped on release %ld)\n",
                   ns, st_rx, st_apply, st_interp, st_hold, st_badrig, st_norig, st_old, st_nostream, st_reapply, st_recopy, st_stale_anim);
            {   /* v1066N */
                int nd = 0; const uint32_t nc = gf_coop_pose_now_ms(); extern volatile long g_wpose_face_live;
                for (int i = 0; i < WP_STREAMS; i++) if (s_gs[i].netid && s_gs[i].w > 0.0f && nc - s_gs[i].t_ms < 1000u) nd++;
                printf("[coop-wpose] v1066N guest: extrapolated %ld, frozen holds %ld (held > %d ms past the newest pose) | donor fallback: proxies on a donor now %d | donor applies %ld (no own stream %ld, stale own stream %ld, cross-fade %ld), donor picks %ld, wanted but no donor %ld | own live faces %ld | world clock used %ld (world - own pose clock %+.0f ms)%s\n",
                       st_extrap, st_frozen, s_stale_ms, nd, st_donor_apply, st_donor_nostream, st_donor_stale, st_donor_blend, st_donor_pick, st_donor_none, g_wpose_face_live, st_wclk_used, st_wclk_n ? st_wclk_diff / (double)st_wclk_n : 0.0, st_test_drop ? " | TEST_STARVE dropped poses" : "");
                if (st_test_drop) printf("[coop-wpose] v1066N TEST_STARVE: %ld poses dropped\n", st_test_drop);
                st_wclk_diff = 0.0; st_wclk_n = 0;
            }
            printf("[coop-wpose] v1070A guest pose guard: stale-controller writes blocked (anim controller gone %ld / replaced %ld; after-anim re-applies skipped %ld) | rejected wire poses: collapsed %ld, exploded %ld, non-finite %ld | receiver palette unevaluated %ld (row scale from rig reference %ld, no reference %ld; face caches deferred %ld, live faces skipped %ld) | own-source applies blocked %ld | post-skel ctl == ent+0xC4: %ld yes / %ld no%s\n",
                   g_v70_rw_noctl, g_v70_rw_changed, st_v70_reapply_block, g_v70_app_coll, g_v70_app_expl, g_v70_app_nonfin, g_v70_recv_bad, g_v70_scale_fix, g_v70_scale_noref,
                   g_v70_face_skip, g_v70_live_face_skip, st_v70_own_block, st_v70_c4_ok, st_v70_c4_bad, s_v70_test > 0 ? " | TEST_COLLAPSE sent" : "");
            if (s_v70_test > 0) printf("[coop-wpose] v1070A TEST_COLLAPSE: %ld collapsed poses sent\n", st_v70_test_coll);
        }
        if (is_host_side()) printf("[coop-wpose] v1070A host pose guard: captures refused (palette collapsed / unevaluated %ld, exploded %ld, non-finite %ld)\n", g_v70_cap_coll, g_v70_cap_expl, g_v70_cap_nonfin);
        fflush(stdout);
    }
}

/* v1071D: receiver -- ms since the last pose packet of a proxy's stream, -1 = no stream at all (gf_coop_world.c no-anim cull) */
int gf_wpose_stream_age_ms(uint16_t id)
{
    int r = -1; const ULONGLONG now = GetTickCount64();
    if (!id) return -1;
    AcquireSRWLockShared(&s_lock);
    for (int i = 0; i < WP_STREAMS; i++) if (s_st[i].n && s_st[i].netid == id) { r = now > s_st[i].last_rx ? (int)(now - s_st[i].last_rx) : 0; break; }
    ReleaseSRWLockShared(&s_lock);
    return r;
}
