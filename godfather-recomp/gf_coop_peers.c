/* gf_coop_peers.c -- v1072S (agent S): puppet pool for N-peer sessions.
 * The PRIMARY remote peer (host: first joiner; joiner: the host) keeps its puppet in gf_coop_puppet.c (unchanged). Every
 * other remote peer gets an extra puppet: spawned with the same managed-factory path (gf_coop_puppet_spawn_extra = the
 * primary's spawn_char: func_003388AC crew class + player model via func_0046348C SetModel), root driven every frame
 * (vtable +0x30 SetMatrix) + late frame set after its anim update (+0x2C), bone pose from that peer's player-pose packets
 * (gf_coop_peer_pose_packets -> gf_coop_pose_apply_mix, the world-NPC mixer) right after its anim update.
 * Kill switch GF_COOP_MULTIPUPPET=0. Only acts when a non-primary peer exists, so a 2-player session never runs it.
 * Echo self test: GF_COOP_ECHO_PEERS=k (gf_coop.c) -> k fake relayed peers -> k extra puppets next to the echo twin. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "gf_coop_pose.h"
#include "gf_coop_peers.h"

extern uint8_t* vm_base;
extern int gf_coop_connected(void);
extern uint32_t gf_coop_puppet_entity(void);
extern uint32_t gf_coop_puppet_spawn_extra(const float m[16]);
extern int gf_coop_puppet_drive_extra(uint32_t ent, const float m[16]);
extern int gf_coop_puppet_late_extra(uint32_t ent, const float m[16]);
extern int gf_coop_puppet_valid(uint32_t ent);
extern void gf_coop_puppet_release_extra(uint32_t ent);
extern int gf_coop_char_hold(void);
extern uint32_t gf_coop_pose_now_ms(void);

typedef struct { int used, id; uint32_t ent; ULONGLONG seen, next_try, last30; int tries; float m[16]; int have_m; float p30[3], plast[3]; int have_plast; } xpup;
static xpup s_X[GF_COOP_MAX_PEERS];
static int s_mp = -1;
static volatile long st_jb_interp, st_jb_hold;
static volatile long st_spawn, st_spawn_fail, st_release, st_drive_fail, st_pose_ok, st_pose_fail, st_pose_none, st_late, st_set30, st_jump, st_wait_prim;
static ULONGLONG s_prim_since; static uint32_t s_prim_ent;

static uint32_t rd32(uint32_t ea) { const uint8_t* p = vm_base + ea; return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void cfg(void)
{
    const char* e; if (s_mp >= 0) return;
    e = getenv("GF_COOP_MULTIPUPPET"); s_mp = !(e && *e == '0');
    printf("[coop-peers] v1072S puppet pool: one puppet per extra remote peer (the primary keeps the classic puppet) %s (GF_COOP_MULTIPUPPET)\n", s_mp ? "ON" : "off"); fflush(stdout);
}
static xpup* x_find(int id) { for (int i = 0; i < GF_COOP_MAX_PEERS; i++) if (s_X[i].used && s_X[i].id == id) return &s_X[i]; return NULL; }
static void x_release(xpup* x, const char* why)
{
    if (x->ent) { printf("[coop-peers] v1072S release puppet of peer %d ent %08X (%s)\n", x->id, x->ent, why); fflush(stdout);
        __try { gf_coop_puppet_release_extra(x->ent); } __except (EXCEPTION_EXECUTE_HANDLER) { } st_release++; }
    memset(x, 0, sizeof *x);
}
int gf_coop_is_puppet(uint32_t ent)   /* v1072S: 1 = the primary puppet or one of the extra peer puppets (world enumeration must skip these) */
{
    if (!ent) return 0;
    if (ent == gf_coop_puppet_entity()) return 1;
    for (int i = 0; i < GF_COOP_MAX_PEERS; i++) if (s_X[i].used && s_X[i].ent == ent) return 1;
    return 0;
}
int gf_coop_extra_puppets(uint32_t* out, int max)   /* v1072X: live extra puppets (for gf_coop_look.c) */
{
    int n = 0; for (int i = 0; i < GF_COOP_MAX_PEERS && n < max; i++) if (s_X[i].used && s_X[i].ent) out[n++] = s_X[i].ent;
    return n;
}
uint32_t gf_coop_peer_puppet(int peer_id)
{
    const xpup* x = x_find(peer_id);
    if (gf_coop_peer_is_primary_id(peer_id)) return gf_coop_puppet_entity();
    return x ? x->ent : 0;
}
void gf_coop_peers_tick(void)   /* main PPU thread */
{
    static ULONGLONG s_log; const ULONGLONG now = GetTickCount64(); int n, want = 0;
    cfg();
    if (!s_mp) return;
    if (!gf_coop_connected()) { for (int i = 0; i < GF_COOP_MAX_PEERS; i++) if (s_X[i].used) x_release(&s_X[i], "disconnected"); return; }
    {   /* v1072S s72-3 abort 0x00ABBF14 (game placement path vt+0x30 during the intro igc): extras spawned while the world was still
         * loading (before the primary puppet existed). Extras now wait until the primary puppet has been live for 3 s. */
        const uint32_t pe = gf_coop_puppet_entity();
        if (pe != s_prim_ent) { s_prim_ent = pe; s_prim_since = now; } }
    n = gf_coop_peer_count();
    for (int i = 0; i < n; i++) {
        const int id = gf_coop_peer_id(i); float m[16], hp = 0; xpup* x;
        if (id < 0 || gf_coop_peer_is_primary_id(id)) continue;
        if (!gf_coop_peer_pose_id(id, m, &hp)) continue;
        want++;
        if (!(x = x_find(id))) { for (int k = 0; k < GF_COOP_MAX_PEERS; k++) if (!s_X[k].used) { x = &s_X[k]; memset(x, 0, sizeof *x); x->used = 1; x->id = id; break; } }
        if (!x) continue;
        x->seen = now; memcpy(x->m, m, sizeof m); x->have_m = 1;
        if (x->ent && !gf_coop_puppet_valid(x->ent)) { printf("[coop-peers] v1072S puppet of peer %d (%08X) no longer valid -> forget\n", id, x->ent); fflush(stdout); x->ent = 0; }
        if (!x->ent) {
            if (now < x->next_try) continue;
            if (!s_prim_ent || now - s_prim_since < 3000) { st_wait_prim++; continue; }
            { uint32_t e = 0; __try { e = gf_coop_puppet_spawn_extra(m); } __except (EXCEPTION_EXECUTE_HANDLER) { e = 0; }
              if (e) { x->ent = e; x->tries = 0; st_spawn++; { char nm[24]; gf_coop_peer_name_id(id, nm); printf("[coop-peers] v1072S puppet for peer %d (%s): ent %08X\n", id, nm, e); fflush(stdout); } }
              else { const int t = ++x->tries; st_spawn_fail++; x->next_try = now + (t >= 4 ? 10000u : (1000u << (t - 1))); } }
            continue;
        }
        if (gf_coop_char_hold()) continue;   /* v1068R2 world reload after a hitch: no placement */
        {   /* jump > 8 m in one tick (teleport / scene change) -> respawn instead of placing (the placement path aborts there) */
            if (x->have_plast) { const float dx = m[12] - x->plast[0], dy = m[13] - x->plast[1], dz = m[14] - x->plast[2];
                if (dx * dx + dy * dy + dz * dz > 64.0f) { st_jump++; x_release(x, "peer jumped > 8 m (teleport / area change)"); x->used = 1; x->id = id; x->seen = now; x->next_try = now + 2000; continue; } }
            x->plast[0] = m[12]; x->plast[1] = m[13]; x->plast[2] = m[14]; x->have_plast = 1; }
        {   /* frame-only setter (+0x2C) every frame; the full SetMatrix (+0x30: world partition + physics) at most every 500 ms and
             * only for a small move since the last one */
            /* v1072X: the throttle left the entity's world / physics position up to 2 s behind while the peer ran (user 3-player
             * test: "hella desync"). The s72-3 abort it guarded against was caused by spawning before the primary existed (fixed
             * by the 3 s wait above), so extras are now driven exactly like the primary: full SetMatrix every tick.
             * GF_COOP_XDRIVE_THROTTLE=1 restores the v1072S throttle. */
            static int s_thr = -1; if (s_thr < 0) { const char* e = getenv("GF_COOP_XDRIVE_THROTTLE"); s_thr = (e && *e == '1');
                printf("[coop-peers] v1072X extra puppets: full SetMatrix %s (GF_COOP_XDRIVE_THROTTLE)\n", s_thr ? "throttled (v1072S)" : "every tick like the primary"); fflush(stdout); }
            int ok = 0; const float dx = m[12] - x->p30[0], dz = m[14] - x->p30[2]; const int full = !s_thr || !x->last30 || (now - x->last30 >= 500 && dx * dx + dz * dz < 4.0f) || (now - x->last30 >= 500 && dx * dx + dz * dz >= 4.0f && now - x->last30 >= 2000);
            __try { ok = full ? gf_coop_puppet_drive_extra(x->ent, m) : gf_coop_puppet_late_extra(x->ent, m); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
            if (ok && full) { x->last30 = now; x->p30[0] = m[12]; x->p30[1] = m[13]; x->p30[2] = m[14]; st_set30++; }
            if (!ok) { st_drive_fail++; x_release(x, "drive transform failed"); } }
    }
    for (int i = 0; i < GF_COOP_MAX_PEERS; i++) if (s_X[i].used && now - s_X[i].seen > 1500) x_release(&s_X[i], "peer gone / primary now");
    if (now - s_log >= 10000 && (want || st_spawn || st_spawn_fail)) { int live = 0; s_log = now; for (int i = 0; i < GF_COOP_MAX_PEERS; i++) if (s_X[i].used && s_X[i].ent) live++;
        printf("[coop-peers] v1072S puppet pool: extra peers %d, puppets live %d | spawned %ld (failed tries %ld, waited for the primary %ld), released %ld (jumps %ld), drive failures %ld | full SetMatrix %ld, late sets %ld | pose applied %ld (failed %ld, no packets %ld) | v1072X4 jb interp %ld hold %ld\n",
               want, live, st_spawn, st_spawn_fail, st_wait_prim, st_release, st_jump, st_drive_fail, st_set30, st_late, st_pose_ok, st_pose_fail, st_pose_none, st_jb_interp, st_jb_hold); fflush(stdout); }
}
void gf_coop_peers_after_anim(uint32_t ent)   /* main PPU thread, right after the entity's anim update */
{
    xpup* x = NULL; static uint8_t pa[GF_COOP_POSE_MAX_PACKET], pb[GF_COOP_POSE_MAX_PACKET];
    if (s_mp != 1 || !ent) return;
    for (int i = 0; i < GF_COOP_MAX_PEERS; i++) if (s_X[i].used && s_X[i].ent == ent) { x = &s_X[i]; break; }
    if (!x) return;
    if (gf_coop_char_hold()) return;   /* loading-sized frame: no placement of any kind */
    if (x->have_m) { int ok = 0; __try { ok = gf_coop_puppet_late_extra(ent, x->m); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; } if (ok) st_late++; }
    {   /* v1072X4 (Claude): per-peer jitter buffer like the primary's v1059G. User 19:05: player 3 "dragged back, animations playing
         * in chunks" while player 2 is smooth. Before: only the 2 newest packets, played a->b over one interval starting when b
         * ARRIVED -> every late packet = a hold, then a jump. Now: the last 8 packets by sender time, clock offset = min(arrival -
         * sender ms) relaxed +1 ms/s, playback GF_COOP_POSE_DELAY (80) ms behind, interpolated. GF_COOP_XPOSE_JB=0 = old path. */
        static int jb_on = -1; static double dly = -1.0;
        if (jb_on < 0) { const char* e = getenv("GF_COOP_XPOSE_JB"); jb_on = !(e && *e == '0'); e = getenv("GF_COOP_POSE_DELAY"); dly = (e && atoi(e) >= 0) ? (double)atoi(e) : 80.0;
            printf("[coop-peers] v1072X4 extra-puppet pose jitter buffer %s (%.0f ms, GF_COOP_XPOSE_JB / GF_COOP_POSE_DELAY)\n", jb_on ? "ON" : "off", dly); fflush(stdout); }
        int la = 0, lb = 0, k; uint32_t ma = 0, mb = 0, anim = 0; unsigned long long age = 0; gf_pose_pair P;
        k = gf_coop_peer_pose_packets(x->id, pa, &la, pb, &lb, &ma, &mb, &age);
        if (k <= 0 || lb <= 0 || age > 1000) { st_pose_none++; return; }
        __try { anim = rd32(ent + 0xC4u); } __except (EXCEPTION_EXECUTE_HANDLER) { anim = 0; }
        if (!anim) { st_pose_fail++; return; }
        if (jb_on && x->id >= 0 && x->id < GF_COOP_MAX_PEERS) {
            typedef struct { uint32_t ms; int len; uint8_t d[GF_COOP_POSE_MAX_PACKET]; } jpk;
            typedef struct { jpk b[8]; int n; double off; int off_ok; ULONGLONG relax; } jbuf;
            static jbuf s_jb[GF_COOP_MAX_PEERS]; static uint8_t qa[GF_COOP_POSE_MAX_PACKET], qb[GF_COOP_POSE_MAX_PACKET];
            jbuf* J = &s_jb[x->id]; const double nowm = (double)gf_coop_pose_now_ms(); const ULONGLONG t64 = GetTickCount64();
            if (J->n && (int32_t)(mb - J->b[J->n - 1].ms) < -2000) J->n = 0;   /* sender restarted / clock jumped back */
            for (int pass = 0; pass < 2; pass++) {   /* insert a (if newer than ours) then b, kept sorted by sender ms */
                const uint8_t* d = pass ? pb : pa; const int l = pass ? lb : la; const uint32_t ms = pass ? mb : ma;
                if (l <= 0 || l > GF_COOP_POSE_MAX_PACKET) continue;
                if (J->n && (int32_t)(ms - J->b[J->n - 1].ms) <= 0) continue;
                if (J->n == 8) { memmove(&J->b[0], &J->b[1], sizeof(jpk) * 7); J->n = 7; }
                J->b[J->n].ms = ms; J->b[J->n].len = l; memcpy(J->b[J->n].d, d, (size_t)l); J->n++;
                if (pass) { const double o = (nowm - (double)age) - (double)ms; if (!J->off_ok || o < J->off) { J->off = o; J->off_ok = 1; } } }
            if (t64 - J->relax >= 1000) { J->relax = t64; if (J->off_ok) J->off += 1.0; }
            {   const double t = nowm - J->off - dly; int i = 0;
                la = 0; lb = 0; P.u = 1.0f;
                if (J->n == 1 || t >= (double)J->b[J->n - 1].ms) { lb = J->b[J->n - 1].len; memcpy(qb, J->b[J->n - 1].d, (size_t)lb); st_jb_hold++; }
                else { while (i + 1 < J->n && (double)J->b[i + 1].ms <= t) i++;
                    if ((double)J->b[i].ms > t) { lb = J->b[0].len; memcpy(qb, J->b[0].d, (size_t)lb); st_jb_hold++; }
                    else { la = J->b[i].len; memcpy(qa, J->b[i].d, (size_t)la); lb = J->b[i + 1].len; memcpy(qb, J->b[i + 1].d, (size_t)lb);
                        { const double f = (t - (double)J->b[i].ms) / (double)(int32_t)(J->b[i + 1].ms - J->b[i].ms); P.u = (float)(f < 0 ? 0 : f > 1 ? 1 : f); } st_jb_interp++; } }
                P.pa = la > 0 ? qa : NULL; P.la = la; P.pb = qb; P.lb = lb; }
        } else {
            P.pa = (k >= 2 && la > 0) ? pa : NULL; P.la = P.pa ? la : 0; P.pb = pb; P.lb = lb; P.u = 1.0f;
            if (P.pa && mb > ma) { const float iv = (float)(mb - ma); float u = (float)age / iv; if (u > 1.0f) u = 1.0f; if (u < 0.0f) u = 0.0f; P.u = u; }
        }
        { int r = 0; __try { { static int wf = -1; if (wf < 0) { const char* e = getenv("GF_COOP_XPOSE_WIREFACE"); wf = !(e && *e == '0'); }
                  r = gf_coop_pose_apply_mix(ent, anim, &P, NULL, 0.0f, wf ? 2 : 0); } } __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; }
          if (r > 0) st_pose_ok++; else st_pose_fail++; }
    }
}
