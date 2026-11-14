/* gf_coop_world.c -- v1060W2 world sync network layer (peds + traffic), host authoritative (Claude, agent W2).
 * Contract: gf_coop_world.h. W1 (gf_coop_worldre.c) enumerates / suppresses, W3 (gf_coop_proxy.c) spawns / drives.
 *
 * Host (GF_COOP=host, and the host half of echo):
 *   every 1/GF_COOP_WORLD_HZ s (default 15): gf_world_enum -> keep entities within GF_COOP_WORLD_R m (default 80) of
 *   the host OR the guest player, nearest first, cap GF_COOP_WORLD_MAX (default 64; 12 in echo). Stable 16-bit net ids
 *   per (entity address, class). New ones -> reliable EV_WORLD_SPAWN batches (13 records / event); gone for 3 ticks ->
 *   EV_WORLD_DESPAWN batches. Snapshot = unreliable blob fragments on channels 8..11 (gf_coop.c queue slot = ch & 15),
 *   self-contained (no reassembly): header 28 B + 21 B / entity (pos int16 1/64 m relative to the header origin,
 *   smallest-three quaternion 7 B, velocity int16 1/256 m/s). On every (re)connect all spawns are re-sent.
 * Guest (GF_COOP=join, and the guest half of echo):
 *   connected -> gf_world_suppress_ambient(1) (re-asserted every 2 s, must be idempotent; not in echo). Events are
 *   queued on the co-op thread and executed on the main PPU thread: spawn -> gf_proxy_spawn (retry every 1 s while it
 *   returns 0), despawn -> gf_proxy_release. Snapshot samples (4 per entity) are played back GF_COOP_WORLD_DELAY ms
 *   (default 100) behind the sender clock and interpolated (nlerp + lerp, short velocity extrapolation) ->
 *   gf_proxy_drive every frame. Disconnect -> gf_proxy_release_all + suppress(0).
 * Echo: proxies are offset by GF_COOP_WORLD_ECHO_OFS m along world X (default 2.5) so every ped gets a visible twin.
 * GF_COOP_WORLD=0 = off. Log [coop-world]. */
#include "gf_coop_world.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "gf_memq.h"   /* v1062W2 corpse matrix reads */
extern uint8_t* vm_base;

extern int gf_coop_connected(void);
extern int gf_coop_mode(void);                 /* 0 off, 1 host, 2 join, 3 echo */
extern int gf_coop_local_pos(float out[3]);
extern int gf_coop_partner_pose(float out_m[16], float* hp);
#include "gf_coop_remote.h"   /* v1072W */
#include "gf_coop_peers.h"    /* v1072W3 agent S API */
extern int gf_coop_send_event(uint16_t type, const void* data, int len);
extern int gf_coop_send_blob(uint8_t channel, const void* data, int len);
typedef void (*gf_coop_event_fn)(uint16_t type, const void* data, int len);
extern void gf_coop_set_event_handler(gf_coop_event_fn fn);
extern uint32_t gf_coop_pose_now_ms(void);

enum { W_HOST = 1, W_JOIN = 2, W_ECHO = 3 };

/* ---------------------------------------------------------------- v1063H weak defaults (agent H): P / V override by defining them */
/* per-kind hooks: P defines the *_ped ones (gf_coop_proxy.c), V the *_car ones (gf_coop_vehicle.c) */
#define H0_EXT(name) int name##_h0(uint32_t ent, uint8_t kind, uint8_t* out, int max) { (void)ent; (void)kind; (void)out; (void)max; return 0; }
H0_EXT(gf_world_snap_ext_ped) H0_EXT(gf_world_snap_ext_car) H0_EXT(gf_world_spawn_ext_ped) H0_EXT(gf_world_spawn_ext_car)
void gf_proxy_spawn_ext_ped_h0(uint16_t id, const uint8_t* ext, int len) { (void)id; (void)ext; (void)len; }
void gf_proxy_spawn_ext_car_h0(uint16_t id, const uint8_t* ext, int len) { (void)id; (void)ext; (void)len; }
void gf_proxy_drive_info_ped_h0(uint16_t id, const gf_world_drive_info* di) { (void)id; (void)di; }
void gf_proxy_drive_info_car_h0(uint16_t id, const gf_world_drive_info* di) { (void)id; (void)di; }
void gf_world_owned_apply_h0(uint32_t ent, uint8_t kind, const gf_world_drive_info* di) { (void)ent; (void)kind; (void)di; }
int  gf_proxy_adopt_h0(uint16_t id, uint8_t kind, uint32_t cls, const uint8_t guid[16], uint32_t ent) { (void)id; (void)kind; (void)cls; (void)guid; (void)ent; return -1; }   /* -1 = not implemented */
#pragma comment(linker, "/alternatename:gf_world_snap_ext_ped=gf_world_snap_ext_ped_h0")
#pragma comment(linker, "/alternatename:gf_world_snap_ext_car=gf_world_snap_ext_car_h0")
#pragma comment(linker, "/alternatename:gf_world_spawn_ext_ped=gf_world_spawn_ext_ped_h0")
#pragma comment(linker, "/alternatename:gf_world_spawn_ext_car=gf_world_spawn_ext_car_h0")
#pragma comment(linker, "/alternatename:gf_proxy_spawn_ext_ped=gf_proxy_spawn_ext_ped_h0")
#pragma comment(linker, "/alternatename:gf_proxy_spawn_ext_car=gf_proxy_spawn_ext_car_h0")
#pragma comment(linker, "/alternatename:gf_proxy_drive_info_ped=gf_proxy_drive_info_ped_h0")
#pragma comment(linker, "/alternatename:gf_proxy_drive_info_car=gf_proxy_drive_info_car_h0")
#pragma comment(linker, "/alternatename:gf_world_owned_apply=gf_world_owned_apply_h0")
#pragma comment(linker, "/alternatename:gf_proxy_adopt=gf_proxy_adopt_h0")
/* dispatchers (the generic names in gf_coop_world.h) */
int  gf_world_snap_ext(uint32_t ent, uint8_t kind, uint8_t* out, int max) { return kind == GF_WK_CAR ? gf_world_snap_ext_car(ent, kind, out, max) : gf_world_snap_ext_ped(ent, kind, out, max); }
int  gf_world_spawn_ext(uint32_t ent, uint8_t kind, uint8_t* out, int max) { return kind == GF_WK_CAR ? gf_world_spawn_ext_car(ent, kind, out, max) : gf_world_spawn_ext_ped(ent, kind, out, max); }
extern void gf_coop_pop_combat_drop(uint32_t ent);   /* v1127G6 (gf_coop_npcfix.c) */
/* v1127G20 CAP STICKINESS (agent analysis of c27-53): the joiner tracks up to 64 sources (its street + ~54 migrated in); ranked by
 * distance to the partner, the cut lands ~80 m from the partner and already-sent peds the partner SEES get evicted by newcomers ->
 * "DESPAWN SEEN .. ALIVE (left our selection)" at 80-82 m + POP after the 12 s hold, plus churn (same native under new net ids).
 * A tracked, sent ped the partner sees (cone, 90 m) keeps priority under the cap. GF_COOP_CAP_STICKY=0 off. */
static int remote_sees(const float p[3], float view_r);
static int s_cap_sticky = 1; static long st_cap_sticky; static float s_cap_sticky_m = 30.0f;   /* v1127G26 (Codex G24 #1): bounded advantage, GF_COOP_CAP_STICKY_M */
static int s_bound_skip = 1; static long st_g23_bound;   /* v1127G23 GF_COOP_BOUND_SKIP */
static int g23_bound_ents(uint32_t* out, int max);   /* v1127G23, defined after s_p */
/* v1127G18: despawn-hold age; dying_ms can be set by the receiver thread AFTER this tick read now -> unsigned now - dying_ms wrapped
 * (c27 logs "after 18446744073709551615 ms") and the 12 s hold was released at once, in view. Future stamps count as age 0. */
#define DH_AGE(now_, t_) ((now_) > (t_) ? (ULONGLONG)((now_) - (t_)) : (ULONGLONG)0)
static void proxy_spawn_ext(uint16_t id, uint8_t kind, const uint8_t* ext, int len) { if (kind == GF_WK_CAR) gf_proxy_spawn_ext_car(id, ext, len); else gf_proxy_spawn_ext_ped(id, ext, len); }
static void proxy_drive_info(uint16_t id, uint8_t kind, const gf_world_drive_info* di) { if (kind == GF_WK_CAR) gf_proxy_drive_info_car(id, di); else gf_proxy_drive_info_ped(id, di); }
extern void gf_world_v1063_status(char* buf, int n);
enum { SNAP2_MAGIC = 0x324E5357u /* "WSN2" */, SNAP_UP_CH = 6, SNAP2_BASE = 22 };
static int s_adopt = 1;            /* v1063H GF_COOP_WORLD_ADOPT=0: never pair guest natives (echo: off unless =2) */
static float s_adopt_r = 12.0f;    /* GF_COOP_WORLD_ADOPT_R */
static int s_far_m = 60;           /* v1063H entities farther than this from both players: every 2nd snapshot */
static volatile long st_adopt, st_adopt_fail, st_claim_tx, st_claim_rx, st_up_tx, st_up_rx, st_owned_apply, st_ext_bytes;
static int s_adopt_unsupported = 0;
static volatile long st_refresh;
static volatile long st_ext_redeliver; static int s_ped_ext_refresh = 1;   /* v1064Q (agent Q) GF_COOP_WORLD_PED_EXT_REFRESH=0: ped ext read once (old) */
/* v1064I coverage gauges (host: last enumeration; guest: proxies pending > 5 s) */
static int s_cov_enum, s_cov_valid, s_cov_in, s_cov_beyond, s_cov_capped, s_cov_peds_in, s_cov_cars_in, s_cov_nopos; static float s_cov_beyond_min = 0, s_cov_far_max = 0;
static volatile long st_pend_long; static int s_cov_log = 1;   /* GF_COOP_WORLD_COVLOG=0 off */
static int s_tod_probe = 0;   /* v1064I GF_COOP_TOD_PROBE=1: time-of-day / weather probe (diagnostic) */
static int s_tod_sync = 1; static volatile long st_tod_tx, st_tod_rx, st_tod_apply; static long s_tod_last_diff = 0;   /* v1064I3 GF_COOP_TOD_SYNC */
static uint8_t s_tod_rx[2 + 18 * 4]; static volatile long s_tod_rx_new = 0;   /* co-op thread -> main thread (under s_qcs) */
enum { SNAP_MAGIC = 0x314E5357u /* "WSN1" */, SNAP_HDR = 28, SNAP_ENT = 21, SNAP_MAX_BYTES = 1150,
       SNAP_PER_FRAG = (SNAP_MAX_BYTES - SNAP_HDR) / SNAP_ENT, SNAP_FRAGS = 4, SNAP_CH0 = 8 };
enum { SPAWN_REC = 71, SPAWN_PER_EV = 13, MAXT = 256, ENUM_CAP = 256, NSAMP = 4 };

static int s_on = -1, s_hz = 15, s_max = 64; static float s_r = 120.0f /* v1063H: 80 -> 120 */, s_echo_ofs = 2.5f; static double s_delay = 130.0;   /* v1061H: = pose playback delay (gf_coop_wpose.c), feet stay in step with the body */
/* v1066C (agent C) guest playback clock + interpolation:
 *  ADAPT (GF_COOP_WORLD_ADAPT=1, 0 = old fixed "now - min offset - delay" clock): one playback clock for all proxies that
 *    follows the ideal time with a rate-limited servo (0.85x .. 1.25x real time) -> a re-centred min offset or a host frame
 *    hitch no longer makes every proxy jump; the clock never runs more than GF_COOP_WORLD_HOLD_MS (40) past the newest
 *    snapshot (underrun = slow down instead of extrapolating 250 ms and snapping back); the playback delay adapts to the
 *    measured sender interval + arrival lateness (GF_COOP_WORLD_DELAY .. GF_COOP_WORLD_DELAY_MAX, 130 .. 220 ms).
 *  SPLINE (GF_COOP_WORLD_SPLINE=1, 0 = linear): position = non-uniform Catmull-Rom (Hermite with neighbour tangents),
 *    velocity = the derivative of the played trajectory (the car servo's feed-forward now matches the target exactly; the
 *    host's EMA finite-difference velocity lagged ~1 sample) -> no per-sample velocity kinks on turning / braking cars.
 *  Extrapolation uses the last segment's velocity, capped 250 ms for cars, GF_COOP_WORLD_PED_EXTRAP_MS (150) for peds. */
static int s_adapt = 1, s_spline = 1; static double s_delay_max = 220.0, s_hold_ms = 40.0, s_ped_extrap = 150.0;
static double s_play_t = 0, s_play_wall = 0, s_play_delay = 130.0, s_play_rate = 1.0; static int s_play_ok = 0;
static uint32_t s_newest_ms = 0; static int s_newest_ok = 0; static uint32_t s_prev_seq_ms = 0; static uint32_t s_prev_seq = 0;   /* under s_plock */
static double s_gap_env = 70.0, s_late_env = 10.0, s_late_last = 0;   /* under s_plock */
static volatile long st_clk_rec, st_pf_frames, st_pf_hold, st_pf_fast, st_pf_slow, st_pf_jump, st_pe_interp, st_pe_extrap, st_pe_before, st_pe_gap, st_pe_spl_clamp;
static double st_lead_sum = 0, st_lead_min = 1e9, st_delay_sum = 0;
static int s_echo_pull = 0;   /* v1061B GF_COOP_WORLD_ECHO_PULL=1 (debug): echo proxies appear on a ring around the local player and then
                                 * move exactly like their source entity (for tests started inside an interior) */
static float s_echo_car_up = 3.0f;   /* v1061B: echo car twins float 3 m ABOVE the real car (a side offset overlaps the real car's Havok body / traffic) */
/* v1068R (agent R) ZONE SPAWN AUTHORITY, GF_COOP_WORLD_GUEST_AUTH (1 default, 0 = old host-only world, 2 = echo test aid):
 * each peer is authoritative for the ambient peds/cars its own game spawns. The joiner runs the SOURCE half (host_tick:
 * enumeration, spawn ext, WSN2 snapshots, despawns) for its AMBIENT natives that first appear more than s_auth_r
 * (= worldre release radius, 130 m) from the host -- exactly the ones the joiner's suppression would NOT release -- with
 * net ids 0x8000..0xFFFF; the host keeps 1..0x7FFF and runs the SINK half (guest_tick without suppression / adoption /
 * claims) and builds proxies of them with the normal factory. Same events + snapshot channels: every peer only receives
 * from the other one. Proxies are "ours" in gf_coop_worldre.c (owner = requester = 0) -> never enumerated as a source,
 * no echo loop. The joiner never releases a native it already sources (gf_coop_world_src_has), so the host walking up
 * to the joiner's street life sees it stay; new natives near the host are still released (host-owned wins).
 * Echo (=2): the echo source uses the 0x8000 id range + ambient-only enumeration (no zone gate: one process). */
static volatile long st_farown_skip;   /* v1072Z2 */
static int s_gauth = 1; static float s_auth_r = 130.0f; static volatile long st_gauth_new, st_gauth_near_skip, st_gauth_nohost_skip;
static int zs_far_now(void); static volatile long st_zs_own, st_zs_ho;   /* v1071D (defined after s_p) */
/* ================================================================ v1072W (agent W) remote players, N-peer ready (gf_coop_remote.h).
 * Agent S's API is bound weakly: until gf_coop.c defines gf_coop_peer_count/_pose/_cam the defaults below link in (count -1 =
 * not active) and the single partner is used, exactly the 2-player behaviour. GF_COOP_REMOTES_API=0 forces the partner path. */
int gf_coop_peer_count_w0(void) { return -1; }
int gf_coop_peer_pose_w0(int i, float m[16], uint16_t* id) { (void)i; (void)m; (void)id; return 0; }
int gf_coop_peer_cam_w0(int i, float* fx, float* fz) { (void)i; (void)fx; (void)fz; return 0; }
#pragma comment(linker, "/alternatename:gf_coop_peer_count=gf_coop_peer_count_w0")
#pragma comment(linker, "/alternatename:gf_coop_peer_pose=gf_coop_peer_pose_w0")
#pragma comment(linker, "/alternatename:gf_coop_peer_cam=gf_coop_peer_cam_w0")
extern int gf_coop_peer_count(void); extern int gf_coop_peer_pose(int i, float m16[16], uint16_t* peer_id); extern int gf_coop_peer_cam(int i, float* fx, float* fz);
extern int gf_coop_partner_cam(float* fx, float* fz);
static SRWLOCK s_rm_lock = SRWLOCK_INIT; static gf_remote s_rm[GF_REMOTE_MAX]; static int s_rm_n, s_rm_api = -1; static ULONGLONG s_rm_t;
static volatile long st_rm_api, st_rm_partner, st_rm_max;
static int rm_ok(const float* m) { return m[12] == m[12] && m[13] == m[13] && m[14] == m[14] && fabsf(m[12]) < 1e5f && fabsf(m[13]) < 1e5f && fabsf(m[14]) < 1e5f; }
static void rm_fill(gf_remote* r, const float* m)
{
    const float l = sqrtf(m[8] * m[8] + m[10] * m[10]);
    r->p[0] = m[12]; r->p[1] = m[13]; r->p[2] = m[14];
    if (l > 1e-3f && l < 10.0f) { r->bf[0] = m[8] / l; r->bf[1] = m[10] / l; } else { r->bf[0] = 0.0f; r->bf[1] = 1.0f; }
    r->fwd[0] = r->bf[0]; r->fwd[1] = r->bf[1]; r->cam = 0; r->peer = 0;
}
int gf_world_remotes(gf_remote* out, int max)
{
    const ULONGLONG now = GetTickCount64(); int n;
    if (!out || max <= 0) return 0;
    if (s_rm_api < 0) { const char* e = getenv("GF_COOP_REMOTES_API"); s_rm_api = !(e && *e == '0');
        printf("[coop-world] v1072W remote players: N-peer API %s (GF_COOP_REMOTES_API), else the 2-player partner; max %d\n", s_rm_api ? "used when active" : "OFF", GF_REMOTE_MAX); fflush(stdout); }
    AcquireSRWLockShared(&s_rm_lock);
    n = now - s_rm_t < 20 ? s_rm_n : -1;
    if (n >= 0) { if (n > max) n = max; memcpy(out, s_rm, sizeof(gf_remote) * (size_t)n); }
    ReleaseSRWLockShared(&s_rm_lock);
    if (n >= 0) return n;
    {   gf_remote t[GF_REMOTE_MAX]; int k = 0, c = -1;
        if (s_rm_api) { __try { c = gf_coop_peer_count(); } __except (EXCEPTION_EXECUTE_HANDLER) { c = -1; } }
        if (c >= 0) {
            for (int i = 0; i < c && k < GF_REMOTE_MAX; i++) { float m[16]; uint16_t id = 0; int ok = 0;
                __try { ok = gf_coop_peer_pose(i, m, &id); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
                if (!ok || !rm_ok(m)) continue;
                rm_fill(&t[k], m); t[k].peer = id;
                __try { t[k].cam = gf_coop_peer_cam(i, &t[k].fwd[0], &t[k].fwd[1]); } __except (EXCEPTION_EXECUTE_HANDLER) { t[k].cam = 0; }
                if (!t[k].cam) { t[k].fwd[0] = t[k].bf[0]; t[k].fwd[1] = t[k].bf[1]; }
                k++; }
            InterlockedIncrement(&st_rm_api);
        } else {
            float m[16], hp = 0;
            if (gf_coop_partner_pose(m, &hp) && rm_ok(m)) { rm_fill(&t[0], m);
                t[0].cam = gf_coop_partner_cam(&t[0].fwd[0], &t[0].fwd[1]); if (!t[0].cam) { t[0].fwd[0] = t[0].bf[0]; t[0].fwd[1] = t[0].bf[1]; } k = 1; }
            InterlockedIncrement(&st_rm_partner);
        }
        if (k > st_rm_max) st_rm_max = k;
        AcquireSRWLockExclusive(&s_rm_lock); memcpy(s_rm, t, sizeof(gf_remote) * (size_t)k); s_rm_n = k; s_rm_t = now; ReleaseSRWLockExclusive(&s_rm_lock);
        n = k > max ? max : k; memcpy(out, t, sizeof(gf_remote) * (size_t)n); return n; }
}
/* v1072W snapshot tiers (GF_COOP_WORLD_TIERS=1 opt-in): > GF_COOP_WORLD_FAR (60 m) from every player every 2nd snapshot (old rule),
 * > 2.5x that (150 m) every 4th; GF_COOP_WORLD_KBPS = budget: over it in the last 10 s window both tiers send half as often.
 * Staggered by net id so the skipped ones spread over the ticks. Default = old rule (far every 2nd). */
static int s_mprio = 1; static volatile long st_mprio_cand;   /* v1072W GF_COOP_WORLD_MISSION_PRIO=0 off */
static int s_tiers = -1, s_kbps = 0, s_tier_over = 0; static volatile long st_tier_mid, st_tier_far, st_tier_skip, st_snap_recs;
static void tiers_cfg(void)
{
    const char* e = getenv("GF_COOP_WORLD_TIERS"); s_tiers = e && *e == '1';
    e = getenv("GF_COOP_WORLD_MISSION_PRIO"); s_mprio = !(e && *e == '0');
    printf("[coop-world] v1072W host: mission / crew / police NPCs + cars ranked before street life under the sync cap %s (GF_COOP_WORLD_MISSION_PRIO)\n", s_mprio ? "ON" : "off");
    if ((e = getenv("GF_COOP_WORLD_KBPS")) && atoi(e) > 0) s_kbps = atoi(e);
    printf("[coop-world] v1072W snapshot rate tiers %s (GF_COOP_WORLD_TIERS), budget %d kB/s (GF_COOP_WORLD_KBPS, 0 = none)\n", s_tiers ? "ON" : "off (old: far every 2nd)", s_kbps); fflush(stdout);
}
int gf_coop_world_guest_auth(void) { const int m = gf_coop_mode(); return s_on == 1 && s_gauth > 0 && (m == W_HOST || m == W_JOIN || (m == W_ECHO && s_gauth == 2)); }
static int src_is_joiner(void) { const int m = gf_coop_mode(); return gf_coop_world_guest_auth() && (m == W_JOIN || m == W_ECHO); }   /* local source uses the joiner rules */
/* v1126M (agent M) OWNERSHIP MIGRATION, GF_COOP_MIGRATE (v1127W: 1 = DEFAULT ON, 0 = v1125 behaviour; needs GF_COOP_WORLD_GUEST_AUTH, not in
 * echo). Description at the v1126M block further down. */
static int s_mig = -1, s_mig_cars = 0; static float s_sc_ped = 150.0f, s_sc_car = 250.0f;
static void mig_cfg(void);
int gf_coop_world_migrate_on(void) { const int m = gf_coop_mode(); if (s_mig < 0) mig_cfg(); return s_on == 1 && s_mig == 1 && s_gauth > 0 && (m == W_HOST || m == W_JOIN); }
static int mig_id_busy(uint16_t id);   /* v1126M: an id lent to / owned by another peer (a proxy here, or migrated away recently) */
static void mig_host_tick(const gf_world_ent* e, int n, const float me[3], int have_me, const float pm[16], int have_p, ULONGLONG now);
static void desp_note(const void* tv);   /* v1127W despawn reasons */
/* v1126M (C request): gf_coop_session_gen() (gf_coop.c v1126C, +1 per new connection) -> a changed gen = a fresh connection even
 * when the 0->1 edge of gf_coop_connected() was missed (game thread stalled while loading). Weak default: 0 (= edge only). */
int gf_coop_session_gen_m0(void) { return 0; }
#pragma comment(linker, "/alternatename:gf_coop_session_gen=gf_coop_session_gen_m0")
extern int gf_coop_session_gen(void);
extern int gf_world_enum2(gf_world_ent* out, int max, int ambient_only);   /* v1068R gf_coop_worldre.c */
extern float gf_world_release_radius(void);                                 /* v1068R gf_coop_worldre.c (s_sup_rel_m) */
/* v1070B (agent B) ZONE OVERLAP -- "host walks over to the joiner": one owner per NPC/car on both screens (HANDOFF v1070B).
 * v1068R let the owner keep an entity until the game despawns it, so when the players meet, the host's own population
 * spawns on top of the joiner-owned natives it already shows as proxies (double crowd on BOTH screens; same-spot twins for
 * fixed-spot spawns: parked cars, street crew) and the joiner's owned set never shrinks. Three rules, each with a kill switch:
 *  HANDOVER (joiner, GF_COOP_WORLD_HANDOVER=0 off): a joiner-owned native that is inside the host's zone (host < _HANDOVER_R,
 *    default 100 m XZ; ownership is only ACQUIRED beyond the 130 m zone radius -> 30 m hysteresis, no flapping) and HIDDEN
 *    (>= _HANDOVER_HIDE_M 60 m peds / _HANDOVER_HIDE_CAR_M 90 m cars from BOTH players) is released by the joiner with the
 *    game's release + an immediate DESPAWN -> the host's own population covers that spot; nothing pops near a player.
 *  DEDUP (host, GF_COOP_WORLD_DEDUP=0 off): a host AMBIENT native of the same class within _DEDUP_R (1.5 m peds) /
 *    _DEDUP_CAR_R (3 m cars) of a joiner-owned proxy for >= _DEDUP_MS (1500) while both are slow or the native is < 5 s old
 *    = a same-spot twin -> the host releases ITS native (+ despawn if it was synced). The twin at the same spot stays on both
 *    screens, so the removal is invisible. A new native in that state is HELD (not synced) until decided.
 *  YIELD (host, GF_COOP_WORLD_YIELD=0 off): while joiner-owned peds stand inside the host's zone (s_r), the host releases its
 *    own NEW ambient ped spawns (< 3 s old, > 35 m from both players, never synced) when own + partner peds would exceed the
 *    host's learned ambient ped budget (learned while no partner peds are near; never below half of it).
 *  GF_COOP_WORLD_OVERLAP_TEST (echo aid, needs GUEST_AUTH=2): 1 = handover live in echo (partner = self) + dedup/yield
 *    count-only, 2 = all live. Counters: "[coop-world] v1070B overlap ..." every 10 s. */
static int s_ho = 1, s_dd = 1, s_yield = 1, s_ovl_test = 0, s_dd_ms = 1500, s_revclaim = 1;
static float s_ho_in = 100.0f, s_ho_hide_ped = 60.0f, s_ho_hide_car = 90.0f, s_dd_r_ped = 1.5f, s_dd_r_car = 3.0f, s_yb = 0.0f;
static volatile long st_ho, st_ho_fail, st_dd_seen, st_dd_res, st_dd_hold, st_dd_dry, st_dd_fail, st_yield, st_yield_dry, st_pop_near, st_pop_stale, st_ga_mismatch;
static int s_k_ped = 0, s_h_ped = 0;
extern int gf_world_native_info(uint32_t ent, int* ambient, unsigned* age_ms);   /* v1070B gf_coop_worldre.c */
extern int gf_world_release_native(uint32_t ent);                                /* v1070B gf_coop_worldre.c */
static int ovl_echo(void);
static int ho_role(void);
static int dd_role(void);
static void ovl_dd_begin(const float me[3], int have_me, ULONGLONG now);
static int  ovl_dd_check(const gf_world_ent* w, int slot, const float me[3], int have_me, const float pm[16], int have_p, ULONGLONG now);
static void ovl_handover(const float me[3], int have_me, const float pm[16], int have_p);

/* ------------------------------------------------------------------ helpers */
static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void putf(uint8_t* p, float f) { uint32_t u; memcpy(&u, &f, 4); put32(p, u); }
static float getf(const uint8_t* p) { uint32_t u = get32(p); float f; memcpy(&f, &u, 4); return f; }
static int fin(float f) { return f == f && fabsf(f) < 1e6f; }
static int16_t q16(float v, float scale) { long x = lroundf(v * scale); if (x < -32767) x = -32767; if (x > 32767) x = 32767; return (int16_t)x; }

typedef struct { float w, x, y, z; } quat;
static int vnorm3(float* v) { float d = v[0]*v[0] + v[1]*v[1] + v[2]*v[2]; if (!(d > 1e-8f) || !fin(d)) return 0; d = 1.0f / sqrtf(d); v[0] *= d; v[1] *= d; v[2] *= d; return 1; }
static quat mat_to_quat(const float* m)   /* rows r = m[0..2], u = m[3..5], a = m[6..8]; orthonormalised first */
{
    float r[3] = {m[0], m[1], m[2]}, u[3] = {m[3], m[4], m[5]}, a[3]; quat q = {1, 0, 0, 0};
    if (!vnorm3(r)) return q; { float d = r[0]*u[0] + r[1]*u[1] + r[2]*u[2]; for (int i = 0; i < 3; i++) u[i] -= r[i]*d; } if (!vnorm3(u)) return q;
    a[0] = r[1]*u[2] - r[2]*u[1]; a[1] = r[2]*u[0] - r[0]*u[2]; a[2] = r[0]*u[1] - r[1]*u[0];
    { const float m00=r[0],m01=r[1],m02=r[2],m10=u[0],m11=u[1],m12=u[2],m20=a[0],m21=a[1],m22=a[2]; float t = m00+m11+m22, s;
      if (t > 0) { s = sqrtf(t+1)*2; q.w=.25f*s; q.x=(m21-m12)/s; q.y=(m02-m20)/s; q.z=(m10-m01)/s; }
      else if (m00 > m11 && m00 > m22) { s = sqrtf(1+m00-m11-m22)*2; q.w=(m21-m12)/s; q.x=.25f*s; q.y=(m01+m10)/s; q.z=(m02+m20)/s; }
      else if (m11 > m22) { s = sqrtf(1+m11-m00-m22)*2; q.w=(m02-m20)/s; q.x=(m01+m10)/s; q.y=.25f*s; q.z=(m12+m21)/s; }
      else { s = sqrtf(1+m22-m00-m11)*2; q.w=(m10-m01)/s; q.x=(m02+m20)/s; q.y=(m12+m21)/s; q.z=.25f*s; } }
    return q;
}
static void quat_to_rows(quat q, float* m)   /* writes m[0..8] */
{
    float n = sqrtf(q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z); if (!(n > 1e-6f) || !fin(n)) { q.w = 1; q.x = q.y = q.z = 0; n = 1; }
    const float w = q.w/n, x = q.x/n, y = q.y/n, z = q.z/n;
    m[0]=1-2*(y*y+z*z); m[1]=2*(x*y-w*z); m[2]=2*(x*z+w*y);
    m[3]=2*(x*y+w*z); m[4]=1-2*(x*x+z*z); m[5]=2*(y*z-w*x);
    m[6]=2*(x*z-w*y); m[7]=2*(y*z+w*x); m[8]=1-2*(x*x+y*y);
}
static void enc_quat(uint8_t* d, quat q)
{
    float c[4] = {q.w, q.x, q.y, q.z}; int omit = 0, k = 0;
    for (int i = 1; i < 4; i++) if (fabsf(c[i]) > fabsf(c[omit])) omit = i;
    if (c[omit] < 0) for (int i = 0; i < 4; i++) c[i] = -c[i];
    for (int i = 0; i < 4; i++) if (i != omit) { put16(d + k*2, (uint16_t)q16(c[i], 46339.5358f)); k++; }
    d[6] = (uint8_t)omit;
}
static quat dec_quat(const uint8_t* s)
{
    float c[4] = {0, 0, 0, 0}, sum = 0; int omit = s[6] & 3, k = 0; quat q;
    for (int i = 0; i < 4; i++) if (i != omit) { c[i] = (float)(int16_t)get16(s + k*2) / 46339.5358f; sum += c[i]*c[i]; k++; }
    c[omit] = sqrtf(sum < 1.0f ? 1.0f - sum : 0.0f); q.w = c[0]; q.x = c[1]; q.y = c[2]; q.z = c[3]; return q;
}

/* ------------------------------------------------------------------ stats */
static volatile long st_spawn_tx, st_despawn_tx, st_spawn_rx, st_despawn_rx, st_snap_bytes, st_snap_rx, st_spawn_fail, st_proxies, st_tracked;
static volatile long st_frag_exp, st_frag_got, st_stale_rel, st_snap_n;   /* v1061B: fragment loss (per snapshot seq), stale proxies released, snapshots sent */
static uint32_t s_rx_seq; static int s_rx_nfrag, s_rx_got;   /* v1061B (under s_plock) */
enum { W_HYST_M = 15, W_GONE_TICKS = 8, W_GONE_MS = 1500, W_STALE_MS = 8000 /* v1063H 5 -> 8 s (spawns are refreshed every ~4 s) */ };   /* v1061B: selection hysteresis (m), despawn after N ticks absent, guest stale timeout */

/* ------------------------------------------------------------------ host side */
typedef struct { int used; uint32_t ent, cls; uint16_t netid; uint8_t kind, guid[16]; float m[12]; int missing, spawn_sent; ULONGLONG seen_ms; int corpse;
                 int claimed; uint8_t sext_len, sext[GF_WORLD_SPAWN_EXT_MAX]; float vel[3], avel[3]; float dist; int cluster; uint32_t gen; int refresh; ULONGLONG corpse_ms;
                 float hp; int8_t amb; uint8_t mig_in, mig_pend, mig_gone, mig_why, mig_want, mig_req, mig_ls; ULONGLONG mig_req_t; ULONGLONG mig_t, nearer_since, view_since; } tracked;   /* v1126M */   /* v1063H */   /* v1062W2 corpse: ent is the game's corpse of the original NPC */
static tracked s_t[MAXT];
static uint16_t s_next_id = 1;
static uint32_t s_snap_seq = 0;
static ULONGLONG s_host_next = 0;
static int s_host_conn = 0;

static int id_in_use(uint16_t id) { for (int i = 0; i < MAXT; i++) if (s_t[i].used && s_t[i].netid == id) return 1; return gf_coop_world_migrate_on() && mig_id_busy(id); }   /* v1126M */
/* v1072W3 (agent W) N-PEER WORLD IDS + ORIGINS (GF_COOP_WORLD_NPEER_IDS=0 off). Classic 2-player: host 1..0x7FFF, joiner 0x8001..0xFFEF
 * (unchanged). N-peer session (gf_coop_npeer_active): joiner with peer id k (1..15) owns 0x8000 + (k-1)*0x800 + 1 .. 0x8000 + k*0x800 - 1,
 * so every joiner's street life has its own ids. "Origin" of an id (= which peer's clock its samples use): 0 host, k joiner (classic: 1). */
static int s_npid = -1;
static int npeer_ids(void) { if (s_npid < 0) { const char* e = getenv("GF_COOP_WORLD_NPEER_IDS"); s_npid = !(e && *e == '0'); }
    { int a = 0; if (!s_npid) return 0; __try { a = gf_coop_npeer_active(); } __except (EXCEPTION_EXECUTE_HANDLER) { a = 0; } return a; } }
static int my_peer_k(void) { int k = 1; __try { k = gf_coop_self_id(); } __except (EXCEPTION_EXECUTE_HANDLER) { k = 1; } if (k < 1) k = 1; if (k > 15) k = 15; return k; }
static int org_of(uint16_t id) { int k; if (!npeer_ids() && gf_coop_world_migrate_on()) return 1;   /* v1126M: ids no longer name the sender -> one origin (2 players) */
    if (id < 0x8000u) return 0; if (!npeer_ids()) return 1; k = 1 + (int)((id - 0x8000u) / 0x800u); return k > 16 ? 16 : k; }
static uint16_t new_id(void)   /* v1068R: host 1..0x7FFF, joiner 0x8001..0xFFFF while two-way world sync is on (old: 1..0xFFFF) */
{
    const int ga = gf_coop_world_guest_auth(), hi = ga && src_is_joiner();
    uint32_t lo = hi ? 0x8001u : 1u, top = hi ? 0xFFEFu : (ga ? 0x7FFFu : 0xFFEFu);   /* >= 0xFFF0 = gf_coop_vehicle.c sentinels (agent Y) */
    if (hi && gf_coop_mode() == W_JOIN && npeer_ids()) { const uint32_t k = (uint32_t)my_peer_k(); lo = 0x8000u + (k - 1u) * 0x800u + 1u; top = 0x8000u + k * 0x800u - 1u; if (top > 0xFFEFu) top = 0xFFEFu; }   /* v1072W3 */
    for (int n = 0; n < 65535; n++) { if (s_next_id < lo || s_next_id > top) s_next_id = (uint16_t)lo;
        const uint16_t id = s_next_id; s_next_id = (uint16_t)(id >= top ? lo : id + 1u); if (id && !id_in_use(id)) return id; }
    return 0;
}

typedef struct { int idx; float d; } cand;
static int cand_cmp(const void* a, const void* b) { const float x = ((const cand*)a)->d, y = ((const cand*)b)->d; return x < y ? -1 : x > y; }

static void host_send_spawns(void)   /* v1063H: EV_WORLD_SPAWN2 = u8 count + records {u16 netid, u8 kind, u32 class, 16 B guid,
                                      * 12 floats m, u8 extlen, ext} (variable length, <= 1000 B per event) */
{
    uint8_t ev[1000]; int n = 0, pos = 1; int idx[64];
    for (int i = 0; i <= MAXT; i++) {
        const int last = i == MAXT;
        if (!last && !(s_t[i].used && (!s_t[i].spawn_sent || s_t[i].refresh) && !s_t[i].missing)) continue;   /* v1063H refresh keeps spawn_sent */
        const int rl = last ? 0 : 72 + s_t[i].sext_len;
        if (n && (last || pos + rl > (int)sizeof ev || n == 64)) {
            ev[0] = (uint8_t)n;
            if (!gf_coop_send_event(GF_COOP_EV_WORLD_SPAWN2, ev, pos)) return;   /* queue full: rest next tick */
            for (int k = 0; k < n; k++) { s_t[idx[k]].spawn_sent = 1; s_t[idx[k]].refresh = 0; } st_spawn_tx += n; n = 0; pos = 1;
        }
        if (last) break;
        { uint8_t* r = ev + pos; put16(r, s_t[i].netid); r[2] = s_t[i].kind; put32(r + 3, s_t[i].cls); memcpy(r + 7, s_t[i].guid, 16);
          for (int k = 0; k < 12; k++) putf(r + 23 + 4*k, s_t[i].m[k]);
          r[71] = s_t[i].sext_len; if (s_t[i].sext_len) memcpy(r + 72, s_t[i].sext, s_t[i].sext_len);
          pos += rl; idx[n++] = i; }
    }
}
static int host_send_despawn(const uint16_t* ids, int n)
{
    uint8_t ev[1 + 2*255]; int sent = 0;
    while (sent < n) { int k = n - sent > 255 ? 255 : n - sent; ev[0] = (uint8_t)k; for (int i = 0; i < k; i++) put16(ev + 1 + 2*i, ids[sent + i]);
        if (!gf_coop_send_event(GF_COOP_EV_WORLD_DESPAWN, ev, 1 + 2*k)) break; sent += k; st_despawn_tx += k; }
    return sent;
}

static uint16_t s_pending_despawn[MAXT * 2]; static int s_npd = 0;
static volatile long st_corpse_follow;   /* v1062W2 */
static volatile long st_pop_in40, st_pop_in80, st_pop_infar;   /* v1071B: new proxies by distance from the local player when they first appear */
static int read_ent_m12(uint32_t ent, float m[12])   /* v1062W2: world rows of *(ent+0x68) (+0x10 right, +0x20 up, +0x30 at, +0x40 pos) */
{
    uint32_t xf; const uint8_t* p;
    if (!gf_memq_access(vm_base, ent + 0x68u, 4, 0)) return 0;
    __try {
        p = vm_base + ent + 0x68u; xf = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
        if (!gf_memq_access(vm_base, xf + 0x10u, 0x40, 0)) return 0;
        for (int r = 0; r < 4; r++) for (int k = 0; k < 3; k++) {
            const uint8_t* q = vm_base + xf + 0x10u + (uint32_t)(r * 16 + k * 4);
            const uint32_t u = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];
            memcpy(&m[r * 3 + k], &u, 4); if (!fin(m[r * 3 + k])) return 0;
        }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static CRITICAL_SECTION s_qcs; static int s_qcs_ok;   /* defined with the guest event queue below */
static uint16_t s_claim_q[32]; static uint8_t s_claim_on[32]; static int s_claim_n = 0;   /* v1063H co-op thread -> host_tick (under s_qcs) */
static int s_guid_late = -1; static long st_guid_late;   /* v1127G29 GF_COOP_GUID_LATE=0 off */
static int guid_is_zero(const uint8_t g[16]) { for (int b = 0; b < 16; b++) if (g[b]) return 0; return 1; }
static int find_tracked_ent(const gf_world_ent* w)
{
    for (int i = 0; i < MAXT; i++) if (s_t[i].used && s_t[i].ent == w->ent && s_t[i].cls == w->class_hash && s_t[i].kind == w->kind && !s_t[i].corpse
                                       && s_t[i].gen == w->gen && !memcmp(s_t[i].guid, w->guid, 16)) return i;   /* v1063H: + spawn serial + GUID (address reuse) */
    /* v1127G29 (agent P27, c27-66): worldre resolves <= 8 GUIDs per enum and reports 0 until then -> a ped first tracked with a
     * zero GUID got a NEW net id once it resolved; the old id went "alive but left our selection" (G10 at 1 m) and was handed to the
     * partner -> same-spot twins -> twin resolver -> held despawn -> POP (c27-66: ~19 of B's 21 POPs). Same ent + cls + kind + gen
     * (= same spawn record; worldre bumps gen on address reuse) -> adopt the late GUID and keep the net id. */
    if (s_guid_late < 0) { const char* e = getenv("GF_COOP_GUID_LATE"); s_guid_late = !(e && *e == '0'); }
    if (s_guid_late && !guid_is_zero(w->guid))
        for (int i = 0; i < MAXT; i++) if (s_t[i].used && s_t[i].ent == w->ent && s_t[i].cls == w->class_hash && s_t[i].kind == w->kind && !s_t[i].corpse
                                           && s_t[i].gen == w->gen && guid_is_zero(s_t[i].guid)) {
            memcpy(s_t[i].guid, w->guid, 16); st_guid_late++;
            { static int nl; if (nl++ < 10) { printf("[coop-world] v1127G29 net %u ent %08X: GUID resolved late -> same net id kept (%ld)\n", (unsigned)s_t[i].netid, w->ent, st_guid_late); fflush(stdout); } }
            return i; }
    for (int i = 0; i < MAXT; i++) if (s_t[i].used && s_t[i].mig_in && s_t[i].ent == w->ent && s_t[i].kind == w->kind && !s_t[i].corpse) {   /* v1126M migrated in: the identity is the netid */
        if (!s_t[i].gen) s_t[i].gen = w->gen; if (s_t[i].gen == w->gen) return i; }
    return -1;
}
/* v1071A: corpse records carry flag 4 (no payload) -> the receiver never animates a body with its own (standing) anims or a
 * donor. GF_COOP_CORPSE_FLAG=0 = old records (a v1071A receiver accepts both; a pre-v1071A receiver drops flagged fragments). */
static int s_corpse_flag = -1;
static int snap_put_rec(uint8_t* r, const tracked* t, const float org[3], int room)   /* v1063H WSN2 record, returns bytes or 0 */
{
    uint8_t ext[GF_WORLD_SNAP_EXT_MAX]; int el = 0, len = SNAP2_BASE; uint8_t fl = 0;
    const float rp[3] = {t->m[9] - org[0], t->m[10] - org[1], t->m[11] - org[2]};
    if (fabsf(rp[0]) > 511.0f || fabsf(rp[1]) > 511.0f || fabsf(rp[2]) > 511.0f) return 0;
    if (t->kind == GF_WK_CAR || fabsf(t->avel[0]) + fabsf(t->avel[1]) + fabsf(t->avel[2]) > 0.02f) { fl |= 1; len += 6; }
    if (t->ent && !t->corpse) { __try { el = gf_world_snap_ext(t->ent, t->kind, ext, GF_WORLD_SNAP_EXT_MAX); } __except (EXCEPTION_EXECUTE_HANDLER) { el = 0; } }
    if (el < 0) el = 0; if (el > GF_WORLD_SNAP_EXT_MAX) el = GF_WORLD_SNAP_EXT_MAX;
    if (el) { fl |= 2; len += 1 + el; }
    if (s_corpse_flag < 0) { const char* e = getenv("GF_COOP_CORPSE_FLAG"); s_corpse_flag = !(e && *e == '0'); }
    if (t->corpse && s_corpse_flag) fl |= 4;   /* v1071A */
    if (len > room) return 0;
    put16(r, t->netid); for (int j = 0; j < 3; j++) put16(r + 2 + 2*j, (uint16_t)q16(rp[j], 64.0f));
    enc_quat(r + 8, mat_to_quat(t->m)); for (int j = 0; j < 3; j++) put16(r + 15 + 2*j, (uint16_t)q16(fin(t->vel[j]) ? t->vel[j] : 0.0f, 256.0f));
    r[21] = fl; { int o = SNAP2_BASE;
      if (fl & 1) { for (int j = 0; j < 3; j++) put16(r + o + 2*j, (uint16_t)q16(fin(t->avel[j]) ? t->avel[j] : 0.0f, 1024.0f)); o += 6; }
      if (fl & 2) { r[o] = (uint8_t)el; memcpy(r + o + 1, ext, (size_t)el); st_ext_bytes += el; } }
    return len;
}
/* v1063H: one WSN2 snapshot (fragments on channels ch0.., one origin per fragment: the player the entities are nearest to) */
static int send_snapshot(const int* order, int no, const float (*org_c)[3], int ch0, int maxfrag, uint32_t seq)
{
    static uint8_t pkt[SNAP_FRAGS][SNAP_MAX_BYTES]; int plen[SNAP_FRAGS]; int frag = 0, cnt = 0, pos = SNAP_HDR, cl = -1, sent = 0; const uint32_t ms = gf_coop_pose_now_ms();
    if (maxfrag > SNAP_FRAGS) maxfrag = SNAP_FRAGS;
    for (int k = 0; k <= no && frag < maxfrag; k++) {
        const tracked* t = k < no ? &s_t[order[k]] : 0; uint8_t rec[SNAP2_BASE + 7 + GF_WORLD_SNAP_EXT_MAX]; int rl = 0;
        const int c = t ? t->cluster : cl;
        if (t) rl = snap_put_rec(rec, t, org_c[c], (int)sizeof rec);
        if (cnt && (!t || c != cl || pos + rl > SNAP_MAX_BYTES || cnt == 255)) {
            uint8_t* q = pkt[frag];
            put32(q, SNAP2_MAGIC); put32(q + 4, seq); put32(q + 8, ms); q[12] = (uint8_t)frag; q[13] = 0; q[14] = (uint8_t)cnt; q[15] = 0;
            putf(q + 16, org_c[cl][0]); putf(q + 20, org_c[cl][1]); putf(q + 24, org_c[cl][2]);
            plen[frag++] = pos; cnt = 0; pos = SNAP_HDR; if (frag >= maxfrag) break;
        }
        if (!t || !rl) continue;
        cl = c; memcpy(pkt[frag] + pos, rec, (size_t)rl); pos += rl; cnt++;
    }
    for (int f = 0; f < frag; f++) { pkt[f][13] = (uint8_t)frag;   /* fragment count known now (guest loss stats) */
        if (gf_coop_send_blob((uint8_t)(ch0 + f), pkt[f], plen[f])) { st_snap_bytes += plen[f]; sent++; } }
    return sent;
}
static void host_tick(int conn)
{
    static gf_world_ent e[ENUM_CAP]; static cand c[ENUM_CAP];
    const ULONGLONG now = gf_coop_pose_now_ms();
    {   static int s_gen_h = -1; int g = 0; __try { g = gf_coop_session_gen(); } __except (EXCEPTION_EXECUTE_HANDLER) { g = 0; }   /* v1126M (C request) */
        if (conn && s_host_conn && s_gen_h >= 0 && g != s_gen_h) { s_host_conn = 0; printf("[coop-world] v1126M host_tick: session gen %d -> %d = a new connection\n", s_gen_h, g); fflush(stdout); }
        if (conn) s_gen_h = g; }
    if (conn && !s_host_conn) { for (int i = 0; i < MAXT; i++) { s_t[i].spawn_sent = 0; s_t[i].claimed = 0; s_t[i].mig_pend = 0; } s_npd = 0; printf("[coop-world] host: partner connected -> (re)sending all spawns\n"); fflush(stdout); }
    s_host_conn = conn;
    if (!conn) return;
    if (s_qcs_ok) {   /* v1063H guest claims (its player drives the car) */
        uint16_t q[32]; uint8_t o[32]; int n = 0;
        EnterCriticalSection(&s_qcs); n = s_claim_n; memcpy(q, s_claim_q, sizeof q); memcpy(o, s_claim_on, sizeof o); s_claim_n = 0; LeaveCriticalSection(&s_qcs);
        for (int k = 0; k < n; k++) for (int i = 0; i < MAXT; i++) if (s_t[i].used && s_t[i].netid == q[k]) {
            s_t[i].claimed = o[k]; printf("[coop-world] v1063H net %u %s by the guest (%s %08X)\n", q[k], o[k] ? "CLAIMED" : "released", s_t[i].kind == GF_WK_CAR ? "car" : "ped", s_t[i].ent); fflush(stdout); }
    }
    if (s_host_next && now < s_host_next && s_host_next - now < 2000) { host_send_spawns(); return; }
    s_host_next = now + (ULONGLONG)(1000 / s_hz);

    const int mig = gf_coop_world_migrate_on();   /* v1126M */
    const int jsrc = src_is_joiner(), jzone = jsrc && gf_coop_mode() == W_JOIN && !mig;   /* v1068R joiner source: ambient only + zone gate; v1126M: no zone gate (population per player) */
    int n = 0; __try { n = jsrc ? gf_world_enum2(e, ENUM_CAP, 1) : gf_world_enum(e, ENUM_CAP); } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
    if (n < 0) n = 0; if (n > ENUM_CAP) n = ENUM_CAP;
    float me[3] = {0, 0, 0}, pm[16], hp = 0; const int have_me = gf_coop_local_pos(me); const int have_p = gf_coop_partner_pose(pm, &hp);
    gf_remote rmt[GF_REMOTE_MAX]; const int nrm = gf_world_remotes(rmt, 7);   /* v1072W: clusters 1..7 = remote players */
    int nc = 0; static int cl_of[ENUM_CAP]; static float d_of[ENUM_CAP];
    int cv_valid = 0, cv_beyond = 0; float cv_bmin = 0;   /* v1064I coverage */
    /* v1127G23 (agent analysis c27-55): entities bound to OUR proxies (incl. a demoted native adopted after a hand-over) are never
     * local sources -- re-sourcing them under a new net id made same-spot twins on the partner, whose twin resolver then despawned
     * the old id in our view (B: ~17 of 29 POPs). GF_COOP_BOUND_SKIP=0 off. */
    static uint32_t pe_g23[256]; const int npe_g23 = s_bound_skip ? g23_bound_ents(pe_g23, 256) : 0;
    for (int i = 0; i < n; i++) {
        if (!e[i].ent || (e[i].kind != GF_WK_PED && e[i].kind != GF_WK_CAR)) continue;
        if (npe_g23) { int b = 0; for (int j = 0; j < npe_g23; j++) if (pe_g23[j] == e[i].ent) { b = 1; break; } if (b) { st_g23_bound++; continue; } }
        if (!fin(e[i].m[9]) || !fin(e[i].m[10]) || !fin(e[i].m[11])) continue;
        cv_valid++;
        float d = 1e9f; int cl = 0;
        if (have_me) { float dx = e[i].m[9]-me[0], dy = e[i].m[10]-me[1], dz = e[i].m[11]-me[2]; d = sqrtf(dx*dx+dy*dy+dz*dz); }
        if (nrm > 0) { for (int r = 0; r < nrm; r++) { float dx = e[i].m[9]-rmt[r].p[0], dy = e[i].m[10]-rmt[r].p[1], dz = e[i].m[11]-rmt[r].p[2]; float d2 = sqrtf(dx*dx+dy*dy+dz*dz); if (d2 < d) { d = d2; cl = 1 + r; } } }   /* v1072W */
        else if (have_p) { float dx = e[i].m[9]-pm[12], dy = e[i].m[10]-pm[13], dz = e[i].m[11]-pm[14]; float d2 = sqrtf(dx*dx+dy*dy+dz*dz); if (d2 < d) { d = d2; cl = 1; } }
        if (!have_me && !have_p && nrm <= 0) d = 0;
        float rad = s_r;
        if (mig) {   /* v1126M scope per receiver: distance to the nearest REMOTE player, 150 m peds / 250 m cars (not the host's sync radius) */
            float dr = 1e9f;
            if (nrm > 0) { for (int r = 0; r < nrm; r++) { const float dx = e[i].m[9]-rmt[r].p[0], dy = e[i].m[10]-rmt[r].p[1], dz = e[i].m[11]-rmt[r].p[2]; const float d2 = sqrtf(dx*dx+dy*dy+dz*dz); if (d2 < dr) dr = d2; } }
            else if (have_p) { const float dx = e[i].m[9]-pm[12], dy = e[i].m[10]-pm[13], dz = e[i].m[11]-pm[14]; dr = sqrtf(dx*dx+dy*dy+dz*dz); }
            if (dr < 1e8f) d = dr;
            rad = e[i].kind == GF_WK_CAR ? s_sc_car : s_sc_ped; }
        cl_of[i] = cl; d_of[i] = d;
        int trk_vis = 0;   /* v1127G20 */
        { const int ti = find_tracked_ent(&e[i]); const int tr = ti >= 0;
          if (tr) { if (d > rad + W_HYST_M) { cv_beyond++; if (!cv_bmin || d < cv_bmin) cv_bmin = d; continue; } d -= W_HYST_M;
              if (s_cap_sticky && mig && s_t[ti].spawn_sent && !s_t[ti].corpse) { __try { trk_vis = remote_sees(&e[i].m[9], 90.0f); } __except (EXCEPTION_EXECUTE_HANDLER) { trk_vis = 0; } } }
          else if (d > rad) { cv_beyond++; if (!cv_bmin || d < cv_bmin) cv_bmin = d; continue; } }   /* v1061B hysteresis: no flapping at the cap / radius edge */
        {   int amb = 1; unsigned age = 0; float dk = d;   /* v1072W mission priority: non-ambient (mission / crew / police) first under the cap */
            if (s_mprio && !jsrc) { __try { if (gf_world_native_info(e[i].ent, &amb, &age) && !amb) { dk = d * 0.25f; st_mprio_cand++; } } __except (EXCEPTION_EXECUTE_HANDLER) { } }
            /* v1072Z2 (Claude): a FAR remote player's half belongs to that player's own game for non-ambient natives too (guards,
             * crew, police). User 18:40 / 19:11: sunk + crumpled hostiles and an unhittable, face-broken gangster in a shop on
             * joiner 2 = joiner natives ADOPTED by our copies (gf_world_native_match 12 m), which we cannot animate that far out.
             * Not sent -> the joiner's proxy goes stale, an adopted native is unbound and runs on its own game.
             * GF_COOP_FAR_OWN=0 off (e.g. to share a mission's NPCs while apart). */
            if (!s_mprio && !jsrc && cl > 0) { __try { gf_world_native_info(e[i].ent, &amb, &age); } __except (EXCEPTION_EXECUTE_HANDLER) { amb = 1; } }
            if (!jsrc && !amb && cl > 0 && have_me && cl - 1 < nrm) {
                static int fo = -1; if (fo < 0) { const char* ev = getenv("GF_COOP_FAR_OWN"); fo = !(ev && *ev == '0');
                    printf("[coop-world] v1072Z2 far player's half: our non-ambient natives there are NOT synced (its own game runs them) %s (GF_COOP_FAR_OWN)\n", fo ? "ON" : "off"); fflush(stdout); }
                extern float gf_world_far_radius(void);
                const float hx = rmt[cl - 1].p[0] - me[0], hz = rmt[cl - 1].p[2] - me[2], fr = gf_world_far_radius();
                extern int gf_coop_host_mission_active(void);   /* v1073H: mission NPCs are shared while the host runs a mission */
                if (fo && hx * hx + hz * hz > fr * fr && !gf_coop_host_mission_active()) { st_farown_skip++; continue; } }
            if (trk_vis) { dk -= s_cap_sticky_m; st_cap_sticky++; }   /* v1127G20: a seen, already-sent ped is kept under the 64 cap; v1127G26 (Codex G24 #1): only a bounded
             * lead (default 30 m) -- the old -100000 let 64 seen peds starve every newcomer forever in a busy, stable view */
            c[nc].idx = i; c[nc].d = dk; nc++; }
    }
    qsort(c, (size_t)nc, sizeof c[0], cand_cmp);
    {   /* v1064I coverage gauges (printed with the 10 s status): what the host has vs what it sends and why not */
        int pi = 0, ci = 0; float fm = 0; const int keep = nc > s_max ? s_max : nc;
        for (int k = 0; k < keep; k++) { const gf_world_ent* w = &e[c[k].idx]; if (w->kind == GF_WK_CAR) ci++; else pi++; if (d_of[c[k].idx] > fm) fm = d_of[c[k].idx]; }
        s_cov_enum = n; s_cov_valid = cv_valid; s_cov_in = keep; s_cov_beyond = cv_beyond; s_cov_capped = nc - keep; s_cov_peds_in = pi; s_cov_cars_in = ci;
        s_cov_beyond_min = cv_bmin; s_cov_far_max = fm; s_cov_nopos = !have_me && !have_p; }
    if (nc > s_max) nc = s_max;
    const int dd_on = dd_role();   /* v1070B */
    if (dd_on) ovl_dd_begin(me, have_me, now);

    for (int i = 0; i < MAXT; i++) if (s_t[i].used) s_t[i].missing++;
    int live[MAXT]; int nl = 0;
    for (int k = 0; k < nc; k++) { const gf_world_ent* w = &e[c[k].idx]; int slot = find_tracked_ent(w), freei = -1;
        if (dd_on && ovl_dd_check(w, slot, me, have_me, pm, have_p, now)) continue;   /* v1070B same-spot twin / yield: released or held */
        if (slot < 0) {
            if (jzone) {   /* v1068R: the joiner owns only what its suppression keeps (first seen > s_auth_r from the host) */
                if (!have_p) { st_gauth_nohost_skip++; continue; }
                { const float dx = w->m[9] - pm[12], dz = w->m[11] - pm[14]; if (dx*dx + dz*dz <= s_auth_r * s_auth_r) {
                    int mine = 0; if (have_me && zs_far_now()) { const float jx = w->m[9] - me[0], jz = w->m[11] - me[2]; mine = jx*jx + jz*jz < dx*dx + dz*dz; }   /* v1071D zone split */
                    if (!mine) { st_gauth_near_skip++; continue; } st_zs_own++; } } }
            for (int i = 0; i < MAXT; i++) if (!s_t[i].used) { freei = i; break; }
            if (freei < 0) continue; uint16_t id = new_id(); if (!id) continue; slot = freei; memset(&s_t[slot], 0, sizeof s_t[slot]);
            if (jsrc) st_gauth_new++;
            s_t[slot].used = 1; s_t[slot].ent = w->ent; s_t[slot].cls = w->class_hash; s_t[slot].kind = w->kind; s_t[slot].netid = id; memcpy(s_t[slot].guid, w->guid, 16); s_t[slot].gen = w->gen;
            { int el = 0; __try { el = gf_world_spawn_ext(w->ent, w->kind, s_t[slot].sext, GF_WORLD_SPAWN_EXT_MAX); } __except (EXCEPTION_EXECUTE_HANDLER) { el = 0; }
              s_t[slot].sext_len = (uint8_t)(el > 0 ? (el > GF_WORLD_SPAWN_EXT_MAX ? GF_WORLD_SPAWN_EXT_MAX : el) : 0); } }
        memcpy(s_t[slot].m, w->m, sizeof s_t[slot].m); memcpy(s_t[slot].vel, w->vel, sizeof w->vel); memcpy(s_t[slot].avel, w->avel, sizeof w->avel);
        s_t[slot].missing = 0; s_t[slot].seen_ms = now; s_t[slot].dist = d_of[c[k].idx]; s_t[slot].cluster = cl_of[c[k].idx];
        if (nl < MAXT) live[nl++] = slot;
    }
    {   /* v1062W2 corpse follow: a synced ped that left gf_world_enum because the game replaced it by its corpse keeps its
         * net id on the corpse entity (pose stream + matrix) while the corpse is animated -> the guest sees the body fall
         * and stay instead of the NPC vanishing mid-death. */
        extern uint32_t gf_wpose_corpse_of(uint32_t npc); extern int gf_wpose_corpse_alive(uint32_t corpse);
        for (int i = 0; i < MAXT; i++) { tracked* t = &s_t[i]; float m[12]; uint32_t cp = 0;
            if (!t->used || !t->missing || t->kind != GF_WK_PED || !t->spawn_sent) continue;
            if (t->corpse) { if (!gf_wpose_corpse_alive(t->ent) || !read_ent_m12(t->ent, m)) continue; }
            else {
                if (!(cp = gf_wpose_corpse_of(t->ent)) || !read_ent_m12(cp, m)) continue;
                { const float dx = m[9] - t->m[9], dy = m[10] - t->m[10], dz = m[11] - t->m[11]; if (dx*dx + dy*dy + dz*dz > 16.0f) continue; }
                { static int nlog; if (nlog++ < 20) { printf("[coop-world] v1062W2 net %u: ped %08X -> corpse %08X (follow)\n", t->netid, t->ent, cp); fflush(stdout); } }
                gf_coop_pop_combat_drop(t->ent);   /* v1127G6 */
                t->ent = cp; t->corpse = 1; t->corpse_ms = now; st_corpse_follow++;
            }
            memcpy(t->m, m, sizeof t->m); memset(t->vel, 0, sizeof t->vel); memset(t->avel, 0, sizeof t->avel); t->missing = 0; t->seen_ms = now;
            if (nl < MAXT) live[nl++] = i;
        }
    }
    if (ho_role()) ovl_handover(me, have_me, pm, have_p);   /* v1070B joiner: hidden owned natives inside the host zone -> host */
    if (mig) { __try { mig_host_tick(e, n, me, have_me, pm, have_p, now); } __except (EXCEPTION_EXECUTE_HANDLER) { } }   /* v1126M triggers / offers */
    /* despawn: missing for >= 8 ticks AND >= 1.5 s (v1126M: not while a migration offer for it is out) */
    for (int i = 0; i < MAXT; i++) if (s_t[i].used && !s_t[i].mig_pend && s_t[i].missing >= W_GONE_TICKS && now - s_t[i].seen_ms >= W_GONE_MS) { if (s_t[i].spawn_sent && s_npd < (int)(sizeof s_pending_despawn / 2)) { s_pending_despawn[s_npd++] = s_t[i].netid; __try { desp_note(&s_t[i]); } __except (EXCEPTION_EXECUTE_HANDLER) { } } gf_coop_pop_combat_drop(s_t[i].ent); s_t[i].used = 0; }   /* v1127G6 drop */
    if (s_npd) { int sent = host_send_despawn(s_pending_despawn, s_npd); if (sent) { memmove(s_pending_despawn, s_pending_despawn + sent, (size_t)(s_npd - sent) * 2); s_npd -= sent; } }
    {   /* v1063H rolling spawn refresh: re-send one live spawn per tick (cycles all in ~4 s) -> a guest that dropped a proxy
         * (stale timeout, world change) gets it back; a known id is just re-targeted on the guest. */
        static int rr = 0;
        for (int k = 0; k < MAXT; k++) { rr = (rr + 1) % MAXT; if (s_t[rr].used && s_t[rr].spawn_sent && !s_t[rr].refresh && !s_t[rr].missing && !s_t[rr].corpse) { s_t[rr].refresh = 1; st_refresh++;
            if (s_t[rr].kind != GF_WK_CAR && s_t[rr].ent && s_ped_ext_refresh) {   /* v1064Q (agent Q): re-read the ped appearance ext -- it was read ONCE when tracking began; a ped
                                                                                   * first seen without its render model (LOD / still streaming) sent GUID 0 forever ("unknown 17") */
                int el = 0; __try { el = gf_world_spawn_ext(s_t[rr].ent, s_t[rr].kind, s_t[rr].sext, GF_WORLD_SPAWN_EXT_MAX); } __except (EXCEPTION_EXECUTE_HANDLER) { el = 0; }
                if (el > 0) s_t[rr].sext_len = (uint8_t)(el > GF_WORLD_SPAWN_EXT_MAX ? GF_WORLD_SPAWN_EXT_MAX : el); }
            break; } } }
    host_send_spawns();

    /* v1063H snapshot WSN2: entities whose spawn was queued, not claimed by the guest; cluster 0 (nearest the host) first,
     * then cluster 1 (nearest the guest) -- each fragment carries the origin of its cluster's player, so entities around a
     * guest far away from the host are not dropped by the +-511 m int16 range. Far ones (> s_far_m) every 2nd snapshot. */
    const uint32_t seq = ++s_snap_seq;
    float org_c[8][3]; memset(org_c, 0, sizeof org_c);   /* v1072W: one origin per player cluster (0 = us, 1.. = remotes) */
    if (have_me) memcpy(org_c[0], me, sizeof me);
    if (nrm > 0) { for (int r = 0; r < nrm; r++) memcpy(org_c[1 + r], rmt[r].p, sizeof org_c[0]); }
    else if (have_p) { org_c[1][0] = pm[12]; org_c[1][1] = pm[13]; org_c[1][2] = pm[14]; } else memcpy(org_c[1], org_c[0], sizeof org_c[0]);
    const int ncl = nrm > 0 ? 1 + nrm : 2;
    if (s_tiers < 0) tiers_cfg();
    int order[MAXT]; int no = 0;
    for (int pass = 0; pass < ncl; pass++) for (int k = 0; k < nl; k++) { const tracked* t = &s_t[live[k]];
        if (!t->spawn_sent || t->claimed || t->cluster != pass) continue;
        if (s_tiers) {   /* v1072W tiers */
            const int far_ = t->dist > 2.5f * (float)s_far_m, mid = !far_ && t->dist > (float)s_far_m;
            int every = far_ ? 4 : mid ? 2 : 1; if (s_tier_over && every > 1) every *= 2;
            if (far_) st_tier_far++; else if (mid) st_tier_mid++;
            if (every > 1 && (seq % (uint32_t)every) != (uint32_t)(t->netid % every)) { st_tier_skip++; continue; }
        } else if (t->dist > (float)s_far_m && (seq & 1)) continue;
        order[no++] = live[k]; }
    st_snap_recs += no;
    if (no && send_snapshot(order, no, (const float (*)[3])org_c, SNAP_CH0, SNAP_FRAGS, seq)) st_snap_n++;
    { long tr = 0; for (int i = 0; i < MAXT; i++) tr += s_t[i].used; st_tracked = tr; }
}

/* ------------------------------------------------------------------ guest side */
typedef struct { uint32_t ms; float p[3]; quat q; float v[3], av[3]; uint8_t el, ext[GF_WORLD_SNAP_EXT_MAX]; uint8_t fl; } sample;   /* v1071A fl = WSN2 record flags (4 = corpse) */   /* v1063H + avel / ext */
typedef struct { int used; uint16_t netid; uint8_t kind, guid[16]; uint32_t cls; float m[12]; uint32_t ent; ULONGLONG retry; int tries;
                 sample s[NSAMP]; int ns; ULONGLONG last_rx; float pull[3]; int pull_ok;
                 uint8_t sext_len, sext[GF_WORLD_SPAWN_EXT_MAX]; int sext_given;   /* v1063H spawn ext (appearance etc.) */
                 uint32_t adopted; ULONGLONG pair_next;                           /* v1063H guest native bound as this proxy */
                 int claimed; float up_m[12]; ULONGLONG up_ms; float up_v[3];
                 ULONGLONG pend_since; int pend_logged; int was_live; ULONGLONG dying_ms, noanim_ms; double dying_t; int dying_why; } proxy;   /* v1071D despawn hold / no-anim cull */   /* v1071B was_live: first entity seen (pop-in diag) */   /* v1064I pending diagnostics */   /* v1063H the guest player drives it */
static proxy s_p[MAXT];
/* v1071D (Claude) receiver: no pop-out in view, no sliding statues (user clip 15:06, joiner 155-180 m from the host).
 *  DESPAWN HOLD (GF_COOP_DESPAWN_HOLD=0 off): a proxy whose DESPAWN (or 8 s stale timeout) arrives while WE can see it (< 12 m, or
 *    < 90 m in our camera cone +-65 deg) stays where it is (frozen transform, its own idle anim) until it leaves our view, max
 *    GF_COOP_DESPAWN_HOLD_S (12 s); then it is released. Its later snapshots are ignored.
 *  NO-ANIM CULL (joiner, GF_COOP_NOANIM_CULL=0 off): beyond the host zone, a host ped proxy on OUR side of the street that moves
 *    (> 0.45 m/s) with no pose stream (none, or silent > 2 s) for 1.5 s cannot be animated (the host has no rig for it out there)
 *    -> it stops (despawn hold) and goes when out of view; its id is ignored while the host keeps re-sending it (120 s). */
extern int gf_coop_local_cam(float* fx, float* fz);   /* gf_coop.c v1071D */
extern int gf_wpose_stream_age_ms(uint16_t id);      /* gf_coop_wpose.c v1071D */
extern int gf_world_joiner_far(void);                 /* gf_coop_worldre.c v1071D */
static int s_dh = -1, s_na = 1, s_zsw = 1; static ULONGLONG s_dh_max_ms = 12000;
static volatile long st_dh_held, st_dh_rel_out, st_dh_rel_max, st_dh_revive, st_na_cull, st_na_blocked;
static uint16_t s_blk_id[64]; static ULONGLONG s_blk_ms[64];
static int dh_on(void)
{
    if (s_dh < 0) { const char* e = getenv("GF_COOP_DESPAWN_HOLD"); s_dh = !(e && *e == '0');
        if ((e = getenv("GF_COOP_DESPAWN_HOLD_S")) && atoi(e) > 0) s_dh_max_ms = (ULONGLONG)atoi(e) * 1000u;
        e = getenv("GF_COOP_NOANIM_CULL"); s_na = !(e && *e == '0');
        e = getenv("GF_COOP_ZONE_SPLIT"); s_zsw = !(e && *e == '0');
        printf("[coop-world] v1071D despawn hold while in our view (max %llu s) %s (GF_COOP_DESPAWN_HOLD) | joiner no-anim cull %s (GF_COOP_NOANIM_CULL) | zone split %s (GF_COOP_ZONE_SPLIT)\n",
               (unsigned long long)(s_dh_max_ms / 1000), s_dh ? "ON" : "off", s_na ? "ON" : "off", s_zsw ? "ON" : "off"); fflush(stdout); }
    return s_dh && (gf_coop_mode() == W_JOIN || gf_coop_mode() == W_HOST);
}
static int zs_far_now(void) { if (s_dh < 0) dh_on(); if (!s_zsw || gf_coop_mode() != W_JOIN || gf_coop_world_migrate_on()) return 0;   /* v1126M: no host zone */ { int r = 0; __try { r = gf_world_joiner_far(); } __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; } return r; } }
static int local_sees(const float p[3])   /* 1 = we (probably) see this point; no camera -> 1 (errs on holding) */
{
    float me[3], fx, fz, dx, dz, d2;
    if (!gf_coop_local_pos(me)) return 0;
    dx = p[0] - me[0]; dz = p[2] - me[2]; d2 = dx * dx + dz * dz;
    if (!(d2 == d2)) return 0;
    if (d2 < 144.0f) return 1;
    if (d2 > 8100.0f) return 0;
    if (!gf_coop_local_cam(&fx, &fz)) return 1;
    { const float ox = dx + fx * 3.5f, oz = dz + fz * 3.5f, l = sqrtf(ox * ox + oz * oz); return l > 1e-3f && (ox * fx + oz * fz) / l >= 0.42f; }
}
static int nearer_us(const float p[3])   /* 1 = p is nearer to our player than to the partner's */
{
    float me[3]; gf_remote rr[GF_REMOTE_MAX]; const int nr = gf_world_remotes(rr, GF_REMOTE_MAX);   /* v1072W: nearer than EVERY remote player */
    if (!gf_coop_local_pos(me) || nr <= 0) return 0;
    { const float ax = p[0] - me[0], az = p[2] - me[2], a2 = ax * ax + az * az;
      for (int i = 0; i < nr; i++) { const float bx = p[0] - rr[i].p[0], bz = p[2] - rr[i].p[2]; if (!(a2 < bx * bx + bz * bz)) return 0; }
      return 1; }
}
static int blk_has(uint16_t id, ULONGLONG now)
{
    if (!zs_far_now()) return 0;   /* players back together: everything may show again */
    for (int i = 0; i < 64; i++) if (s_blk_id[i] == id && now - s_blk_ms[i] < 120000) { s_blk_ms[i] = now; return 1; }
    return 0;
}
static void blk_add(uint16_t id, ULONGLONG now)
{
    int fr = 0; ULONGLONG old = ~0ull;
    for (int i = 0; i < 64; i++) { if (s_blk_id[i] == id) { s_blk_ms[i] = now; return; } if (s_blk_ms[i] < old) { old = s_blk_ms[i]; fr = i; } }
    s_blk_id[fr] = id; s_blk_ms[fr] = now;
}
static SRWLOCK s_plock = SRWLOCK_INIT;
static int g23_bound_ents(uint32_t* out, int max)   /* v1127G23: entities bound to our proxies (adopted natives or factory proxies) */
{
    int n = 0; AcquireSRWLockShared(&s_plock);
    for (int j = 0; j < MAXT && n < max; j++) if (s_p[j].used) { const uint32_t x = s_p[j].adopted ? s_p[j].adopted : s_p[j].ent; if (x) out[n++] = x; }
    ReleaseSRWLockShared(&s_plock); return n;
}
static double s_clk_off; static int s_clk_ok;
/* v1072W3 per-origin clocks: the adaptive playback clock follows the FIRST origin that sends (classic: the only one); every other
 * origin keeps its own min-filter offset and its proxies play at the same playback time shifted by (primary - own) offset. */
static int s_prim_o = -1; static double s_ocl_off[17]; static int s_ocl_ok[17]; static double s_ocl_t[17]; static volatile long st_ocl_frag[17];
static double org_delta(uint16_t id) { const int o = org_of(id); if (s_prim_o < 0 || o == s_prim_o || !s_ocl_ok[o]) return 0.0; return s_clk_off - s_ocl_off[o]; }
double gf_coop_world_play_ms_for(uint16_t id) { extern double gf_coop_world_play_ms(void); const double t = gf_coop_world_play_ms(); return t < 0.0 ? t : t + org_delta(id); }   /* v1072W3 for wpose */
int gf_coop_world_origin_is_primary(uint16_t id) { return s_prim_o < 0 || org_of(id) == s_prim_o; }
static int s_guest_conn = 0; static ULONGLONG s_suppress_next = 0;
/* v1063H host side: upstream samples of guest-claimed entities (blob channel SNAP_UP_CH) */
typedef struct { int used; uint16_t netid; sample s[NSAMP]; int ns; ULONGLONG last_rx; } owned;
static owned s_own[16]; static double s_up_clk_off; static int s_up_clk_ok;

/* events: co-op thread -> main thread */
typedef struct { uint16_t type, len; uint8_t data[1000]; } qev;
enum { QEV_N = 256, QEV_M = QEV_N - 1 };   /* v1127G30: was 64 (c28-1: B dropped 10k reliable spawn/despawn/migration events) */
static qev s_q[QEV_N]; static int s_qh = 0, s_qt = 0; static volatile long st_qev_drop; static CRITICAL_SECTION s_qcs; static int s_qcs_ok = 0;

void gf_coop_world_on_blob(uint8_t ch, const void* data, int len);
static void on_event(uint16_t type, const void* data, int len);
static volatile long st_np_ev, st_np_blob;
/* v1072W3: the HOST also sinks the world of joiners 2.. (their natives are in their own id ranges); the primary joiner already comes
 * through the legacy handlers. Relay to the other joiners: NOT yet (needs per-origin blob channels from agent S). */
static void w_peer_event(int peer, uint16_t type, const void* d, int len)
{
    int prim = 1; if (gf_coop_mode() != W_HOST || peer <= 0 || !npeer_ids()) return;
    __try { prim = gf_coop_peer_is_primary_id(peer); } __except (EXCEPTION_EXECUTE_HANDLER) { prim = 1; }
    if (prim) return;
    if (type == GF_COOP_EV_WORLD_SPAWN || type == GF_COOP_EV_WORLD_SPAWN2 || type == GF_COOP_EV_WORLD_DESPAWN) { InterlockedIncrement(&st_np_ev); on_event(type, d, len); }
}
static void w_peer_blob(int peer, uint8_t ch, const void* d, int len)
{
    int prim = 1; if (gf_coop_mode() != W_HOST || peer <= 0 || !npeer_ids()) return;
    __try { prim = gf_coop_peer_is_primary_id(peer); } __except (EXCEPTION_EXECUTE_HANDLER) { prim = 1; }
    if (prim || ch < SNAP_CH0 || ch >= SNAP_CH0 + SNAP_FRAGS) return;
    InterlockedIncrement(&st_np_blob); gf_coop_world_on_blob(ch, d, len);
}
static void on_event(uint16_t type, const void* data, int len)   /* co-op thread */
{
    if (!s_qcs_ok || len < 1 || len > 1000) return;
    if (type == GF_COOP_EV_WORLD_CLAIM) {   /* v1063H guest -> host: {u16 netid, u8 on} */
        const uint8_t* b = (const uint8_t*)data; if (len < 3) return;
        EnterCriticalSection(&s_qcs); if (s_claim_n < 32) { s_claim_q[s_claim_n] = get16(b); s_claim_on[s_claim_n] = b[2] ? 1 : 0; s_claim_n++; } LeaveCriticalSection(&s_qcs);
        InterlockedIncrement(&st_claim_rx); return;
    }
    if (type == 0x0126) {   /* v1064I3 GF_COOP_EV_WORLD_TOD (enum defined further down): keep only the newest */
        const uint8_t* b = (const uint8_t*)data; if (len < 2 + 18 * 4 || b[0] != 'T' || b[1] != 1) return;
        EnterCriticalSection(&s_qcs); memcpy(s_tod_rx, b, 2 + 18 * 4); s_tod_rx_new = 1; LeaveCriticalSection(&s_qcs);
        InterlockedIncrement(&st_tod_rx); return;
    }
    if (type != GF_COOP_EV_WORLD_SPAWN && type != GF_COOP_EV_WORLD_DESPAWN && type != GF_COOP_EV_WORLD_SPAWN2 && !(type >= 0x0127 && type <= 0x012B)) return;   /* v1126M 0x0127..0x012B migration */
    if (gf_coop_mode() == W_HOST && !gf_coop_world_guest_auth()) {   /* v1068R: a two-way joiner talking to a one-way host (nothing drains the queue) */
        if (type == GF_COOP_EV_WORLD_SPAWN2 && len >= 3 && (((const uint8_t*)data)[1] | (((const uint8_t*)data)[2] << 8)) >= 0x8000) {   /* v1070B mismatch detector */
            if (InterlockedIncrement(&st_ga_mismatch) == 1) { printf("[coop-world] v1070B WARNING: the joiner runs GF_COOP_WORLD_GUEST_AUTH=1 but this host has it OFF -> the joiner keeps NPCs/cars the host never shows. Use the SAME setting on both exes!\n"); fflush(stdout); } }
        return; }
    EnterCriticalSection(&s_qcs);
    if (((s_qt + 1) & QEV_M) != s_qh) { s_q[s_qt].type = type; s_q[s_qt].len = (uint16_t)len; memcpy(s_q[s_qt].data, data, (size_t)len); s_qt = (s_qt + 1) & QEV_M; }
    else { const long nd = InterlockedIncrement(&st_qev_drop); if (nd <= 20 || (nd & 1023) == 0) printf("[coop-world] event queue full, dropped type %04X (%ld dropped; v1127G30 log capped)\n", type, nd); }
    LeaveCriticalSection(&s_qcs);
}
static int find_proxy(uint16_t id) { for (int i = 0; i < MAXT; i++) if (s_p[i].used && s_p[i].netid == id) return i; return -1; }

/* v1063H record decoder (WSN1 21 B / WSN2 22 B + optional avel / ext). Returns the record length, 0 = malformed. */
static int dec_rec(const uint8_t* r, int room, int v2, const float org[3], sample* s, uint16_t* id)
{
    int len = v2 ? SNAP2_BASE : SNAP_ENT;
    if (room < len) return 0;
    *id = get16(r);
    for (int j = 0; j < 3; j++) { s->p[j] = org[j] + (float)(int16_t)get16(r + 2 + 2*j) / 64.0f; s->v[j] = (float)(int16_t)get16(r + 15 + 2*j) / 256.0f; s->av[j] = 0; }
    s->q = dec_quat(r + 8); s->el = 0; s->fl = 0;
    if (v2) { const uint8_t fl = r[21];
        if (fl & ~7u) return 0;   /* unknown flag -> unknown length: drop the rest of the fragment */
        s->fl = fl;   /* v1071A: bit 4 = corpse (no payload) */
        if (fl & 1) { if (room < len + 6) return 0; for (int j = 0; j < 3; j++) s->av[j] = (float)(int16_t)get16(r + len + 2*j) / 1024.0f; len += 6; }
        if (fl & 2) { if (room < len + 1) return 0; const int el = r[len]; if (el > GF_WORLD_SNAP_EXT_MAX || room < len + 1 + el) return 0;
                      memcpy(s->ext, r + len + 1, (size_t)el); s->el = (uint8_t)el; len += 1 + el; } }
    return len;
}
static void push_sample(sample* buf, int* ns, const sample* s)
{
    if (*ns && (int32_t)(s->ms - buf[*ns - 1].ms) <= 0) return;   /* old / duplicate */
    if (*ns == NSAMP) { memmove(&buf[0], &buf[1], sizeof(sample) * (NSAMP - 1)); (*ns)--; }
    buf[(*ns)++] = *s;
}

void gf_coop_world_on_blob(uint8_t ch, const void* data, int len)   /* co-op thread (gf_coop_glue.c on_blob) */
{
    const uint8_t* b = (const uint8_t*)data; int v2;
    if (s_on != 1 || len < SNAP_HDR) return;
    if (get32(b) == SNAP2_MAGIC) v2 = 1; else if (get32(b) == SNAP_MAGIC) v2 = 0; else return;
    const uint32_t ms = get32(b + 8); const int cnt = b[14];
    const float org[3] = {getf(b + 16), getf(b + 20), getf(b + 24)};
    const double now = (double)gf_coop_pose_now_ms();
    if (ch == SNAP_UP_CH) {   /* v1063H upstream (guest-claimed entities) -> host */
        const int mode = gf_coop_mode(); if (mode != W_HOST && mode != W_ECHO && !(mode == W_JOIN && s_revclaim && gf_coop_world_guest_auth())) return;   /* v1070B: + the joiner (host drives a joiner-owned car) */
        AcquireSRWLockExclusive(&s_plock);
        { double off = now - (double)ms; if (!s_up_clk_ok || off < s_up_clk_off || off - s_up_clk_off > 2000.0) { s_up_clk_off = off; s_up_clk_ok = 1; } else s_up_clk_off += 0.02; }
        for (int k = 0, pos = SNAP_HDR; k < cnt; k++) { sample sm; uint16_t id; const int rl = dec_rec(b + pos, len - pos, v2, org, &sm, &id); if (!rl) break; pos += rl; sm.ms = ms;
            int i = -1, fr = -1; for (int j = 0; j < 16; j++) { if (s_own[j].used && s_own[j].netid == id) { i = j; break; } if (!s_own[j].used && fr < 0) fr = j; }
            if (i < 0) { if (fr < 0) continue; i = fr; memset(&s_own[i], 0, sizeof s_own[i]); s_own[i].used = 1; s_own[i].netid = id; }
            push_sample(s_own[i].s, &s_own[i].ns, &sm); s_own[i].last_rx = (ULONGLONG)now; }
        ReleaseSRWLockExclusive(&s_plock);
        InterlockedIncrement(&st_up_rx); return;
    }
    if (ch < SNAP_CH0 || ch >= SNAP_CH0 + SNAP_FRAGS) return;
    { const int mode = gf_coop_mode(); if (mode != W_JOIN && mode != W_ECHO && !(mode == W_HOST && gf_coop_world_guest_auth())) return; }   /* v1068R host sink */
    int og = -1;   /* v1072W3 origin of this fragment = origin of its first record id */
    if (cnt > 0) { sample s0; uint16_t id0 = 0; if (dec_rec(b + SNAP_HDR, len - SNAP_HDR, v2, org, &s0, &id0)) og = org_of(id0); }
    AcquireSRWLockExclusive(&s_plock);
    if (og >= 0 && s_prim_o < 0) s_prim_o = og;
    if (og >= 0) st_ocl_frag[og]++;
    if (og >= 0 && og != s_prim_o) {   /* v1072W3: secondary origin -> own offset only, the shared clock / envelopes stay with the primary */
        const double off = now - (double)ms;
        if (!s_ocl_ok[og] || off < s_ocl_off[og] || off - s_ocl_off[og] > 2000.0) { s_ocl_off[og] = off; s_ocl_ok[og] = 1; }
        else if (s_ocl_t[og] > 0 && now > s_ocl_t[og]) s_ocl_off[og] += (now - s_ocl_t[og]) * 0.001;
        s_ocl_t[og] = now;
        for (int k = 0, pos = SNAP_HDR; k < cnt; k++) { sample sm; uint16_t id; const int rl = dec_rec(b + pos, len - pos, v2, org, &sm, &id); if (!rl) break; pos += rl; sm.ms = ms;
            const int i = find_proxy(id); if (i < 0 || s_p[i].dying_ms) continue;
            push_sample(s_p[i].s, &s_p[i].ns, &sm); s_p[i].last_rx = (ULONGLONG)now; }
        ReleaseSRWLockExclusive(&s_plock); InterlockedIncrement(&st_snap_rx); return; }
    {   /* min-filter offset; v1063H: relax by 1 ms per second of wall time (was +0.02 per fragment = depended on entity count) */
        static double s_clk_t = 0; const double off = now - (double)ms;
        if (s_clk_ok && (off < s_clk_off - 5.0 || off - s_clk_off > 2000.0)) st_clk_rec++;   /* v1066C min-offset re-centre (> 5 ms) */
        if (!s_clk_ok || off < s_clk_off || off - s_clk_off > 2000.0) { s_clk_off = off; s_clk_ok = 1; }
        else if (s_clk_t > 0 && now > s_clk_t) s_clk_off += (now - s_clk_t) * 0.001;
        s_clk_t = now;
        {   /* v1066C newest snapshot time + jitter envelopes (fast attack, slow release) for the adaptive delay */
            const double late = off - s_clk_off; s_late_last = late;
            s_late_env += (late > s_late_env ? 0.25 : 0.01) * ((late > 300.0 ? 300.0 : late) - s_late_env);
            if (!s_newest_ok || (int32_t)(ms - s_newest_ms) > 0) {
                const uint32_t seq0 = get32(b + 4);
                if (s_newest_ok && seq0 != s_prev_seq) { double gap = (double)(int32_t)(ms - s_prev_seq_ms); if (gap > 0) { if (gap > 300.0) gap = 300.0;
                    s_gap_env += (gap > s_gap_env ? 0.25 : 0.01) * (gap - s_gap_env); } }
                s_prev_seq = seq0; s_prev_seq_ms = ms; s_newest_ms = ms; s_newest_ok = 1; } } }
    { const uint32_t seq = get32(b + 4);   /* v1061B loss stats: fragments of a seq arrive together; count per seq */
      if (seq != s_rx_seq) { if (s_rx_nfrag) { st_frag_exp += s_rx_nfrag; st_frag_got += s_rx_got; } s_rx_seq = seq; s_rx_nfrag = b[13]; s_rx_got = 0; }
      s_rx_got++; }
    for (int k = 0, pos = SNAP_HDR; k < cnt; k++) { sample sm; uint16_t id; const int rl = dec_rec(b + pos, len - pos, v2, org, &sm, &id); if (!rl) break; pos += rl; sm.ms = ms;
        const int i = find_proxy(id); if (i < 0) continue;
        if (s_p[i].dying_ms) continue;   /* v1071D: held proxy stays where it is */
        push_sample(s_p[i].s, &s_p[i].ns, &sm); s_p[i].last_rx = (ULONGLONG)now; }
    ReleaseSRWLockExclusive(&s_plock);
    InterlockedIncrement(&st_snap_rx);
}

static void guest_release_all(const char* why)
{
    __try { gf_proxy_release_all(); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    AcquireSRWLockExclusive(&s_plock); memset(s_p, 0, sizeof s_p); s_clk_ok = 0; s_newest_ok = 0; s_play_ok = 0; s_prim_o = -1; memset(s_ocl_ok, 0, sizeof s_ocl_ok); ReleaseSRWLockExclusive(&s_plock);   /* v1072W3 origins reset */
    st_proxies = 0; printf("[coop-world] guest: released all proxies (%s)\n", why); fflush(stdout);
}

/* ------------------------------------------------------------------ v1070B zone overlap (see the v1070B block at the top) */
static int ovl_echo(void) { return gf_coop_mode() == W_ECHO && s_ovl_test > 0 && gf_coop_world_guest_auth(); }
static int ho_role(void) { return s_ho && !gf_coop_world_migrate_on() && ((gf_coop_mode() == W_JOIN && gf_coop_world_guest_auth()) || ovl_echo()); }   /* v1126M: migration replaces the handover */
static int dd_role(void) { return (s_dd || s_yield) && ((gf_coop_mode() == W_HOST && gf_coop_world_guest_auth()) || ovl_echo()); }
static int dd_live(void) { return gf_coop_mode() == W_HOST || s_ovl_test >= 2; }
typedef struct { uint16_t netid; uint8_t kind; uint32_t cls; float p[3], v[3]; } pprox;   /* partner-owned live proxy (sender position) */
static pprox s_pp[MAXT]; static int s_npp = 0;
typedef struct { uint32_t ent; ULONGLONG first, last; int done; } dupst;
static dupst s_dup[64];
static void src_drop(int i)   /* stop sourcing slot i now: despawn goes out with this tick (no 1.5 s missing wait) */
{
    if (s_t[i].spawn_sent && s_npd < (int)(sizeof s_pending_despawn / 2)) s_pending_despawn[s_npd++] = s_t[i].netid;
    gf_coop_pop_combat_drop(s_t[i].ent);   /* v1127G6 */
    s_t[i].used = 0;
}
static void ovl_dd_begin(const float me[3], int have_me, ULONGLONG now)   /* host_tick, before the selection loop */
{
    (void)now; s_npp = 0; s_k_ped = 0; s_h_ped = 0;
    AcquireSRWLockShared(&s_plock);
    for (int i = 0; i < MAXT && s_npp < MAXT; i++) { const proxy* p = &s_p[i];
        if (!p->used || !p->ent || !p->ns || (p->netid < 0x8000u && !gf_coop_world_migrate_on()) || p->claimed) continue;   /* v1126M: every proxy is partner-owned */
        pprox* q = &s_pp[s_npp++]; q->netid = p->netid; q->kind = p->kind; q->cls = p->cls; memcpy(q->p, p->s[p->ns - 1].p, sizeof q->p); memcpy(q->v, p->s[p->ns - 1].v, sizeof q->v); }
    ReleaseSRWLockShared(&s_plock);
    if (!have_me) return;
    { const float r2 = s_r * s_r;
      for (int k = 0; k < s_npp; k++) if (s_pp[k].kind == GF_WK_PED) { const float dx = s_pp[k].p[0] - me[0], dz = s_pp[k].p[2] - me[2]; if (dx*dx + dz*dz < r2) s_k_ped++; }
      for (int i = 0; i < MAXT; i++) { const tracked* t = &s_t[i]; int amb = 0;
          if (!t->used || t->corpse || t->missing || t->kind != GF_WK_PED || !t->ent) continue;
          { const float dx = t->m[9] - me[0], dz = t->m[11] - me[2]; if (dx*dx + dz*dz >= r2) continue; }
          if (gf_world_native_info(t->ent, &amb, 0) && amb) s_h_ped++; } }
    if (s_k_ped == 0 || ovl_echo()) { const float h = (float)s_h_ped; s_yb += (h - s_yb) * (h > s_yb ? 0.05f : 0.002f); }   /* budget: fast rise, ~30 s fall */
}
static dupst* dup_get(uint32_t ent, ULONGLONG now)
{
    int fr = -1, old = 0;
    for (int i = 0; i < 64; i++) {
        if (s_dup[i].ent == ent) { if (now - s_dup[i].last > 500) { s_dup[i].first = now; s_dup[i].done = 0; st_dd_seen++; } return &s_dup[i]; }
        if (!s_dup[i].ent || now - s_dup[i].last > 5000) { if (fr < 0) fr = i; }
        else if (s_dup[i].last < s_dup[old].last) old = i; }
    if (fr < 0) fr = old;
    s_dup[fr].ent = ent; s_dup[fr].first = s_dup[fr].last = now; s_dup[fr].done = 0; st_dd_seen++;
    return &s_dup[fr];
}
static int ovl_dd_check(const gf_world_ent* w, int slot, const float me[3], int have_me, const float pm[16], int have_p, ULONGLONG now)   /* 1 = do not track / update w */
{
    int amb = 0; unsigned age = 0xFFFFFFFFu;
    if (!gf_world_native_info(w->ent, &amb, &age) || !amb) return 0;   /* mission / crew-from-script natives: never touched */
    if (s_dd && s_npp) {
        const float r = w->kind == GF_WK_CAR ? s_dd_r_car : s_dd_r_ped; int hit = -1;
        for (int k = 0; k < s_npp; k++) { const pprox* q = &s_pp[k];
            if (q->kind != w->kind || q->cls != w->class_hash) continue;
            { const float dx = q->p[0] - w->m[9], dy = q->p[1] - w->m[10], dz = q->p[2] - w->m[11]; if (dx*dx + dz*dz >= r * r || fabsf(dy) >= 2.5f) continue; }
            if (gf_coop_world_ent_of(q->netid) == w->ent) continue;   /* echo: the proxy of this very native */
            { const float wv = sqrtf(w->vel[0]*w->vel[0] + w->vel[2]*w->vel[2]), qv = sqrtf(q->v[0]*q->v[0] + q->v[2]*q->v[2]);
              if (!((wv < 0.6f && qv < 0.6f) || age < 5000u)) continue; }   /* two peds walking past each other are not twins */
            hit = k; break; }
        if (hit >= 0) {
            dupst* d = dup_get(w->ent, now); d->last = now;
            if ((long long)(now - d->first) >= (long long)s_dd_ms) {
                if (d->done) return 0;
                d->done = 1;
                if (!dd_live()) { st_dd_dry++; return 0; }
                if (gf_world_release_native(w->ent)) { st_dd_res++;
                    { static int nl; if (nl++ < 30) { printf("[coop-world] v1070B same-spot twin: our %s %08X (class %08X, age %u ms%s) on partner net %u -> released ours (the twin stays)\n",
                        w->kind == GF_WK_CAR ? "car" : "ped", w->ent, w->class_hash, age, slot >= 0 ? ", was synced" : "", s_pp[hit].netid); fflush(stdout); } }
                    if (slot >= 0) { src_drop(slot); }
                    return 1; }
                st_dd_fail++; return 0;
            }
            if (slot < 0) { st_dd_hold++; return 1; }   /* do not sync a twin that is about to go */
            return 0;
        }
    }
    if (s_yield && slot < 0 && w->kind == GF_WK_PED && age < 3000u && have_me && s_k_ped > 0 && s_yb >= 4.0f
        && (float)(s_h_ped + 1 + s_k_ped) > s_yb + 1.5f && (float)s_h_ped >= s_yb * 0.5f) {
        const float dx = w->m[9] - me[0], dz = w->m[11] - me[2]; int far_ok = dx*dx + dz*dz > 1225.0f;
        if (have_p) { const float px = w->m[9] - pm[12], pz = w->m[11] - pm[14]; if (px*px + pz*pz <= 1225.0f) far_ok = 0; }
        if (far_ok) {
            if (!dd_live()) st_yield_dry++;
            else if (gf_world_release_native(w->ent)) { st_yield++; return 1; }
        }
    }
    if (slot < 0 && w->kind == GF_WK_PED && have_me) { const float dx = w->m[9] - me[0], dz = w->m[11] - me[2]; if (dx*dx + dz*dz < s_r * s_r) s_h_ped++; }
    return 0;
}
static void ovl_handover(const float me[3], int have_me, const float pm[16], int have_p)   /* joiner host_tick, after the selection */
{
    int n = 0;
    if (!have_p || !have_me) return;
    for (int i = 0; i < MAXT && n < 4; i++) { tracked* t = &s_t[i];
        if (!t->used || t->corpse || t->claimed || !t->ent || t->missing) continue;
        { const float hx = t->m[9] - pm[12], hz = t->m[11] - pm[14], jx = t->m[9] - me[0], jz = t->m[11] - me[2];
          const float dh2 = hx*hx + hz*hz, dj2 = jx*jx + jz*jz, hide = t->kind == GF_WK_CAR ? s_ho_hide_car : s_ho_hide_ped;
          if (dh2 >= s_ho_in * s_ho_in || dh2 < hide * hide || dj2 < hide * hide) continue;
          if (dj2 < dh2 && zs_far_now()) { st_zs_ho++; continue; }   /* v1071D: our half while we are out of the host zone */
          {   /* v1070B-2: a refused native (e.g. a car someone sits in) is retried every 3 s, not every tick (b70-3: refused 588 for 241 handovers) */
              static uint32_t s_rf_ent[16]; static ULONGLONG s_rf_until[16]; static int s_rf_i; const ULONGLONG tnow = gf_coop_pose_now_ms(); int skip = 0;
              for (int k = 0; k < 16; k++) if (s_rf_ent[k] == t->ent && tnow < s_rf_until[k]) { skip = 1; break; }
              if (skip) continue;
              if (!gf_world_release_native(t->ent)) { st_ho_fail++; s_rf_ent[s_rf_i] = t->ent; s_rf_until[s_rf_i] = tnow + 3000; s_rf_i = (s_rf_i + 1) & 15; continue; } }
          st_ho++; n++;
          { static int nl; if (nl++ < 30) { printf("[coop-world] v1070B handover net %u (%s %08X): %.0f m from the host, %.0f m from us -> released, the host's population covers it\n",
              t->netid, t->kind == GF_WK_CAR ? "car" : "ped", t->ent, (double)sqrtf(dh2), (double)sqrtf(dj2)); fflush(stdout); } }
          src_drop(i); }
    }
}

static volatile long st_wrong_range;   /* v1068R spawns for an id of OUR range (reflection / old peer), dropped */
static int id_is_local_range(uint16_t id) { if (!gf_coop_world_guest_auth() || gf_coop_mode() == W_ECHO) return 0; if (gf_coop_mode() == W_JOIN && npeer_ids()) return org_of(id) == my_peer_k(); return gf_coop_mode() == W_JOIN ? id >= 0x8000u : id < 0x8000u; }   /* v1072W3 */
static int id_in_use_src(uint16_t id) { for (int i = 0; i < MAXT; i++) if (s_t[i].used && s_t[i].netid == id) return 1; return 0; }   /* v1126M */
static void guest_add_spawn(const uint8_t* r, const uint8_t* ext, int el)   /* main thread */
{
    const uint16_t id = get16(r);
    if (gf_coop_world_migrate_on() ? id_in_use_src(id) : id_is_local_range(id)) { st_wrong_range++; return; }   /* v1068R: never build a proxy of one of our own sources (v1126M: = an id we source now) */
    if (blk_has(id, gf_coop_pose_now_ms())) { st_na_blocked++; return; }   /* v1071D: culled no-anim proxy, host still re-sends it */
    AcquireSRWLockExclusive(&s_plock);
    int i = find_proxy(id);
    if (s_guid_late < 0) { const char* e = getenv("GF_COOP_GUID_LATE"); s_guid_late = !(e && *e == '0'); }
    if (i >= 0 && s_guid_late && s_p[i].cls == get32(r + 3) && s_p[i].kind == r[2] && guid_is_zero(s_p[i].guid) && !guid_is_zero(r + 7)) {   /* v1128G (review R28 H1):
        * the owner's GUID resolved late (G29 keeps the net id) -> adopt it in place instead of releasing + respawning the proxy (= a POP) */
        memcpy(s_p[i].guid, r + 7, 16); st_guid_late++; }
    if (i >= 0 && (s_p[i].cls != get32(r + 3) || s_p[i].kind != r[2] || memcmp(s_p[i].guid, r + 7, 16))) {   /* v1061B/v1063H: same id, other entity -> respawn */
        ReleaseSRWLockExclusive(&s_plock); __try { gf_proxy_release(id); } __except (EXCEPTION_EXECUTE_HANDLER) { } AcquireSRWLockExclusive(&s_plock); i = find_proxy(id); if (i >= 0) s_p[i].used = 0; i = -1; }
    if (i >= 0) { proxy* p = &s_p[i]; for (int j = 0; j < 12; j++) p->m[j] = getf(r + 23 + 4*j); p->last_rx = gf_coop_pose_now_ms();   /* re-sent spawn of a known proxy: keep it */
      if (p->dying_ms && p->dying_why == 1) { p->dying_ms = 0; st_dh_revive++; }   /* v1071D: the owner has it again */
      if (el > 0 && (el != p->sext_len || memcmp(p->sext, ext, (size_t)el))) {   /* v1064Q (agent Q): refreshed ext (e.g. ped model GUID now known) -> hand it to the hooks again */
          p->sext_len = (uint8_t)(el > GF_WORLD_SPAWN_EXT_MAX ? GF_WORLD_SPAWN_EXT_MAX : el); memcpy(p->sext, ext, p->sext_len); p->sext_given = 0; st_ext_redeliver++; } }
    else { for (int j = 0; j < MAXT; j++) if (!s_p[j].used) { i = j; break; }
      if (i >= 0) { proxy* p = &s_p[i]; memset(p, 0, sizeof *p); p->used = 1; p->netid = id; p->kind = r[2]; p->cls = get32(r + 3); memcpy(p->guid, r + 7, 16);
        for (int j = 0; j < 12; j++) p->m[j] = getf(r + 23 + 4*j); p->last_rx = gf_coop_pose_now_ms();
        if (el > 0) { p->sext_len = (uint8_t)(el > GF_WORLD_SPAWN_EXT_MAX ? GF_WORLD_SPAWN_EXT_MAX : el); memcpy(p->sext, ext, p->sext_len); } } }
    ReleaseSRWLockExclusive(&s_plock);
    st_spawn_rx++;
}
/* ================================================================ v1126M (agent M) OWNERSHIP MIGRATION
 * GF_COOP_MIGRATE (v1127W default 1, =0 off; needs the two-way world GF_COOP_WORLD_GUEST_AUTH, host / joiner, not echo).
 * Owner of a net id = the peer that has it in its SOURCE table s_t (sends spawn + snapshots + pose stream); everyone else shows a
 * proxy. Net id ranges (host 1..0x7FFF, joiner 0x8000..) are only the ALLOCATOR now: a migrated id keeps its number on its new
 * owner, so every receiver keeps the same proxy (no pop). Two-phase hand-off, decided by the current owner (cars: GF_COOP_MIGRATE_CARS=1,
 * default peds only -- car claims in gf_coop_vehicle.c still read the ranges):
 *   OFFER 0x0127 owner -> partner {u8 ver 1, u8 why, u16 netid, u8 kind, u32 class, 16 B guid, 12 f matrix, 3 f vel, f health,
 *     u8 extlen, spawn ext}. The partner promotes its proxy: a demoted native of its own (adopted) = just detach it; a factory proxy =
 *     agent P's gf_proxy_promote(ent, matrix12, health) (weak stub -1 until P links it). Then it sources the entity under the SAME
 *     netid and answers ACK 0x0128 {1, 0, netid}; refused -> NAK 0x0129 {1, code, netid} (1 off, 2 no proxy, 3 claimed / held,
 *     4 proxy pending, 5 car, 6 promote failed, 7 promote not available, 8 no source slot).
 *   Old owner on ACK: drops its source slot WITHOUT a despawn; its native (alive, a ped within GF_COOP_MIGRATE_DEMOTE_R 90 m of our player,
 *     not a cull) is DEMOTED = bound as the proxy of the same netid (gf_proxy_adopt_mig: driven by the new owner's snapshots + pose;
 *     its later release = the game's release), else released. NAK / no answer in 3 s: keeps it (alive) or despawns it (culled),
 *     10 s cooldown for that id.
 *   REQ 0x012A {1, 0, netid}: gf_world_request_control(netid) (agent C, combat control) -> the owner offers it (why 4).
 *   TAKE 0x012B {1, 0, netid}: the owner went silent (trigger c) -> we promoted it ourselves; the old owner (if alive) demotes.
 * Triggers (owner side, host_tick, max GF_COOP_MIGRATE_RATE 4 offers / s, cooldown 10 s per id):
 *   (a) CULL: our population wants it gone (gf_coop_world_src_kind asked by the despawn sweep / cull) or culled it (native gone,
 *       not a corpse, health > 0) while the partner is within GF_COOP_MIGRATE_CULL_R (90 m) of it;
 *   (b) NEAR: the partner is nearer by > GF_COOP_MIGRATE_NEAR_M (30 m) for > GF_COOP_MIGRATE_NEAR_MS (3 s) (hysteresis: going back
 *       needs the same margin the other way);
 *   (c) TIMEOUT (receiver side): proxy silent > GF_COOP_MIGRATE_TAKE_MS (5 s) while connected, within 90 m of us and nearer to us
 *       than to every remote player -> promote + TAKE;
 *   (d) REQ: combat control (agent C).
 * Population per player (with MIGRATE): no ambient suppression / CAP0 / zone gate / handover / zone split / no-anim cull; spawn points
 * nearer to another player are refused (gf_coop_worldre.c); street capacity x0.5..1 by partner distance. Scope per receiver:
 * an entity is sent while within GF_COOP_SCOPE_PED_R (150 m) / GF_COOP_SCOPE_CAR_R (250 m) of the nearest remote player.
 * Promoted factory proxies that end up out of everybody's scope for 10 s are released (they may not be in a game requester).
 * Log "[migrate] v1126M ...", counters every 10 s. N-peer sessions: 2-player semantics only (offers go to "the partner"). */
enum { EV_MIG_OFFER = 0x0127, EV_MIG_ACK = 0x0128, EV_MIG_NAK = 0x0129, EV_MIG_REQ = 0x012A, EV_MIG_TAKE = 0x012B };
enum { MW_CULL = 1, MW_NEAR = 2, MW_TIMEOUT = 3, MW_REQ = 4, MW_VIEW = 5 };   /* v1127W: + VIEW */
int gf_proxy_promote_m0(uint32_t ent, const float* mat, float health) { (void)ent; (void)mat; (void)health; return -1; }   /* -1 = agent P's promotion not linked */
#pragma comment(linker, "/alternatename:gf_proxy_promote=gf_proxy_promote_m0")
extern int gf_proxy_promote(uint32_t ent, const float* mat, float health);   /* gf_coop_promote.c (agent P): 1 ok, 0 failed */
extern uint32_t gf_proxy_detach(uint16_t netid);                                /* gf_coop_proxy.c v1126M */
extern int gf_proxy_is_adopted(uint16_t netid);
extern int gf_proxy_adopt_mig(uint16_t netid, uint32_t cls, const uint8_t guid[16], uint32_t ent);
extern int gf_world_mark_src(uint32_t ent, int on);                             /* gf_coop_worldre.c v1126M */
extern int gf_world_native_alive(uint32_t ent);
int gf_native_brain_off_m0(uint32_t ent) { (void)ent; return 0; }   /* agent P (gf_coop_promote.c), EXPERIMENTAL: brain list entry off */
#pragma comment(linker, "/alternatename:gf_native_brain_off=gf_native_brain_off_m0")
extern int gf_native_brain_off(uint32_t ent);
static int s_mig_brainoff = 0;   /* GF_COOP_MIGRATE_BRAINOFF=1: a demoted native's brain is switched off (no fighting the pose stream); re-promotion then goes through gf_proxy_promote */
static int s_mig_leftsel = 1; static long st_g10_leftsel;   /* v1127G10 GF_COOP_MIGRATE_LEFTSEL */
static int s_mig_nonamb = 1; static float s_mig_keep_r = 45.0f;   /* v1127W GF_COOP_MIGRATE_NONAMB / _KEEP_R */
static int s_mig_view = 1, s_mig_view_ms = 1500; static float s_mig_view_r = 60.0f;   /* v1127W GF_COOP_MIGRATE_VIEW (host not looking -> the partner who sees it simulates it) */
static float s_mig_near = 30.0f, s_mig_cull_r = 90.0f, s_mig_dem_r = 90.0f; static int s_mig_near_ms = 3000, s_mig_take_ms = 5000, s_mig_rate = 4;
static long st_g5_skip, st_g5_refresh, st_g6_req_exp;   /* v1127G6 control requests that expired unoffered (2 s) */
/* v1127G5 combat keep: view/nearer hand-backs skipped, keeps extended by hp loss */
static volatile long st_mg_off[6], st_mg_ack_tx, st_mg_nak_tx[9], st_mg_ack_rx, st_mg_nak_rx, st_mg_tmo, st_mg_prom_ok, st_mg_prom_fail, st_mg_prom_na, st_mg_detach,
                     st_mg_dem_adopt, st_mg_dem_rel, st_mg_dem_gone, st_mg_take_tx, st_mg_take_rx, st_mg_req_tx, st_mg_req_rx, st_mg_gone_desp, st_mg_orphan, st_mg_ratecap;
static struct { uint16_t id; ULONGLONG until; } s_mcool[128];
static struct { uint16_t id; ULONGLONG t; } s_mlent[128]; static int s_mlent_i;
static struct { uint32_t ent; ULONGLONG far_since; } s_mown[64];
static void mig_cfg(void)
{
    const char* e = getenv("GF_COOP_MIGRATE"); s_mig = !(e && *e == '0');   /* v1127W: default ON (rig-verified w27-2/-3/-9); GF_COOP_MIGRATE=0 = old fixed ownership */
    if ((e = getenv("GF_COOP_MIGRATE_CARS"))) s_mig_cars = *e == '1';
    if ((e = getenv("GF_COOP_SCOPE_PED_R")) && atof(e) > 10.0) s_sc_ped = (float)atof(e);
    if ((e = getenv("GF_COOP_SCOPE_CAR_R")) && atof(e) > 10.0) s_sc_car = (float)atof(e);
    if ((e = getenv("GF_COOP_MIGRATE_NEAR_M")) && atof(e) > 1.0) s_mig_near = (float)atof(e);
    if ((e = getenv("GF_COOP_MIGRATE_NEAR_MS")) && atoi(e) >= 0) s_mig_near_ms = atoi(e);
    if ((e = getenv("GF_COOP_MIGRATE_CULL_R")) && atof(e) > 1.0) s_mig_cull_r = (float)atof(e);
    if ((e = getenv("GF_COOP_MIGRATE_DEMOTE_R")) && atof(e) >= 0.0) s_mig_dem_r = (float)atof(e);
    if ((e = getenv("GF_COOP_MIGRATE_TAKE_MS")) && atoi(e) > 0) s_mig_take_ms = atoi(e);
    if ((e = getenv("GF_COOP_MIGRATE_RATE")) && atoi(e) > 0) s_mig_rate = atoi(e);
    if ((e = getenv("GF_COOP_MIGRATE_BRAINOFF"))) s_mig_brainoff = *e == '1';
    if ((e = getenv("GF_COOP_MIGRATE_VIEW"))) s_mig_view = *e == '1';   /* v1127W */
    if ((e = getenv("GF_COOP_MIGRATE_NONAMB"))) s_mig_nonamb = *e == '1';
    if ((e = getenv("GF_COOP_MIGRATE_LEFTSEL"))) s_mig_leftsel = *e == '1';   /* v1127G10 */
    if ((e = getenv("GF_COOP_CAP_STICKY"))) s_cap_sticky = *e == '1';   /* v1127G20 */
    if ((e = getenv("GF_COOP_CAP_STICKY_M")) && *e) s_cap_sticky_m = (float)atof(e);   /* v1127G26 */
    if ((e = getenv("GF_COOP_BOUND_SKIP"))) s_bound_skip = *e == '1';   /* v1127G23 */
    if ((e = getenv("GF_COOP_MIGRATE_KEEP_R")) && atof(e) > 1.0) s_mig_keep_r = (float)atof(e);
    printf("[migrate] v1127W culled NON-ambient NPCs (mission / crew / police) the partner is within %.0f m of or sees -> handed over instead of despawned %s (GF_COOP_MIGRATE_NONAMB); culled ones bypass the per-id cooldown + rate cap\n", (double)s_mig_keep_r, s_mig_nonamb ? "ON" : "off");
    if ((e = getenv("GF_COOP_MIGRATE_VIEW_R")) && atof(e) > 5.0) s_mig_view_r = (float)atof(e);
    if ((e = getenv("GF_COOP_MIGRATE_VIEW_MS")) && atoi(e) >= 0) s_mig_view_ms = atoi(e);
    printf("[migrate] v1127W view trigger %s (GF_COOP_MIGRATE_VIEW): an NPC we own that we do NOT see but the partner sees (camera cone, <= %.0f m from it, not > 15 m farther than us) for %d ms is offered to the partner\n",
           s_mig_view ? "ON" : "off", (double)s_mig_view_r, s_mig_view_ms);
    printf("[migrate] v1126M ownership migration %s (GF_COOP_MIGRATE): peds%s | triggers: cull (partner < %.0f m), partner nearer by %.0f m for %d ms, owner silent %d ms, combat request | max %d offers/s | scope per receiver %.0f m peds / %.0f m cars | demote within %.0f m\n",
           s_mig ? "ON" : "off", s_mig_cars ? " + cars" : " only (GF_COOP_MIGRATE_CARS=1 adds cars)", (double)s_mig_cull_r, (double)s_mig_near, s_mig_near_ms, s_mig_take_ms, s_mig_rate,
           (double)s_sc_ped, (double)s_sc_car, (double)s_mig_dem_r); fflush(stdout);
}
static int mig_cool_ok(uint16_t id, ULONGLONG now) { for (int i = 0; i < 128; i++) if (s_mcool[i].id == id && now < s_mcool[i].until) return 0; return 1; }
static void mig_cool_set(uint16_t id, ULONGLONG now, ULONGLONG ms)
{
    int fr = 0; for (int i = 0; i < 128; i++) { if (s_mcool[i].id == id) { fr = i; break; } if (s_mcool[i].until < s_mcool[fr].until) fr = i; }
    s_mcool[fr].id = id; s_mcool[fr].until = now + ms;
}
static int mig_id_busy(uint16_t id)   /* main thread (new_id): a proxy of it lives here, or we lent it < 10 min ago */
{
    const ULONGLONG now = gf_coop_pose_now_ms();
    for (int i = 0; i < 128; i++) if (s_mlent[i].id == id && now - s_mlent[i].t < 600000) return 1;
    for (int i = 0; i < MAXT; i++) if (s_p[i].used && s_p[i].netid == id) return 1;   /* main thread writes s_p; a racy read only costs one id */
    return 0;
}
static void mig_lent(uint16_t id) { s_mlent[s_mlent_i & 127].id = id; s_mlent[s_mlent_i & 127].t = gf_coop_pose_now_ms(); s_mlent_i++; }
static int rd_hp(uint32_t ent, float* hp)
{
    uint32_t u; if (!ent || !gf_memq_access(vm_base, ent + 0x14C8u, 4, 0)) return 0;
    __try { const uint8_t* q = vm_base + ent + 0x14C8u; u = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3]; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    memcpy(hp, &u, 4); return fin(*hp);
}
static void wr_hp(uint32_t ent, float hp)
{
    uint32_t u; memcpy(&u, &hp, 4);
    if (!ent || !fin(hp) || hp <= 0.0f || !gf_memq_access(vm_base, ent + 0x14C8u, 4, 1)) return;
    __try { uint8_t* q = vm_base + ent + 0x14C8u; q[0] = (uint8_t)(u >> 24); q[1] = (uint8_t)(u >> 16); q[2] = (uint8_t)(u >> 8); q[3] = (uint8_t)u; } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
static const char* mw_name(int w) { return w == MW_CULL ? "cull" : w == MW_NEAR ? "partner nearer" : w == MW_TIMEOUT ? "owner silent" : w == MW_REQ ? "control request" : w == MW_VIEW ? "partner sees it, we do not" : "?"; }
static void mig_send3(uint16_t type, int code, uint16_t id) { uint8_t ev[4]; ev[0] = 1; ev[1] = (uint8_t)code; put16(ev + 2, id); gf_coop_send_event(type, ev, 4); }
static int mig_kind_ok(uint8_t kind) { return kind == GF_WK_PED || (kind == GF_WK_CAR && s_mig_cars); }
static float near_remote(const float p[3], const float pm[16], int have_p, int* idx)   /* XZ distance to the nearest remote player (1e9 = none) */
{
    gf_remote rr[GF_REMOTE_MAX]; const int nr = gf_world_remotes(rr, GF_REMOTE_MAX); float bd = 1e9f; if (idx) *idx = -1;
    for (int i = 0; i < nr; i++) { const float dx = p[0] - rr[i].p[0], dz = p[2] - rr[i].p[2], d = sqrtf(dx * dx + dz * dz); if (d < bd) { bd = d; if (idx) *idx = i; } }
    if (nr <= 0 && have_p && pm) { const float dx = p[0] - pm[12], dz = p[2] - pm[14]; bd = sqrtf(dx * dx + dz * dz); }
    return bd;
}
/* v1127W: 1 = a remote player probably SEES p (within 12 m, or <= view_r inside its camera cone, cos 0.42, cone apex 3.5 m behind
 * it = third-person camera). Camera forward from the peer's cam stream, else its body facing. */
static int remote_sees(const float p[3], float view_r)
{
    gf_remote rr[GF_REMOTE_MAX]; const int nr = gf_world_remotes(rr, GF_REMOTE_MAX);
    for (int i = 0; i < nr; i++) { const float dx = p[0] - rr[i].p[0], dz = p[2] - rr[i].p[2], d2 = dx * dx + dz * dz; float fx = rr[i].fwd[0], fz = rr[i].fwd[1];
        if (!(d2 == d2)) continue;
        if (d2 < 144.0f) return 1;
        if (d2 > view_r * view_r) continue;
        { const float fl = sqrtf(fx * fx + fz * fz); if (!(fl > 0.1f)) continue; fx /= fl; fz /= fl; }
        { const float ox = dx + fx * 3.5f, oz = dz + fz * 3.5f, l = sqrtf(ox * ox + oz * oz); if (l > 1e-3f && (ox * fx + oz * fz) / l >= 0.42f) return 1; } }
    return 0;
}
/* v1127W: why did we despawn a synced entity? (owner side; partner-visible ones = the pops the partner sees) */
static volatile long st_dsp_n, st_dsp_seen, st_dsp_gone_seen, st_dsp_alive_seen, st_dsp_nonamb_seen;
static void desp_note(const void* tv)
{
    const tracked* t = (const tracked*)tv; float dr; int seen, alive;
    if (!t || !t->used) return;
    dr = near_remote(&t->m[9], 0, 0, 0); seen = dr < 1e8f && remote_sees(&t->m[9], 90.0f); alive = t->ent && !t->corpse && gf_world_native_alive(t->ent);
    st_dsp_n++;
    if (!seen) return;
    st_dsp_seen++; if (alive) st_dsp_alive_seen++; else st_dsp_gone_seen++; if (t->amb != 1) st_dsp_nonamb_seen++;
    { static int nl; if (nl++ < 60) { printf("[migrate] v1127W DESPAWN SEEN by the partner: net %u %s %08X cls %08X %s, %s%s, %.0f m from the partner, hp %.0f, missing %d%s\n", t->netid,
        t->kind == GF_WK_CAR ? "car" : "ped", t->ent, t->cls, t->amb == 1 ? "ambient" : t->amb < 0 ? "NON-ambient" : "amb?", alive ? "native ALIVE (left our selection)" : "native GONE",
        t->corpse ? " (corpse)" : "", (double)dr, (double)t->hp, t->missing, mig_cool_ok(t->netid, gf_coop_pose_now_ms()) ? "" : ", in migration cooldown"); fflush(stdout); } }
}
static int mig_send_offer(int i, int why, ULONGLONG now)
{
    tracked* t = &s_t[i]; uint8_t ev[96 + GF_WORLD_SPAWN_EXT_MAX]; int o = 0;
    ev[o++] = 1; ev[o++] = (uint8_t)why; put16(ev + o, t->netid); o += 2; ev[o++] = t->kind; put32(ev + o, t->cls); o += 4; memcpy(ev + o, t->guid, 16); o += 16;
    for (int k = 0; k < 12; k++) { putf(ev + o, t->m[k]); o += 4; }
    for (int k = 0; k < 3; k++) { putf(ev + o, fin(t->vel[k]) ? t->vel[k] : 0.0f); o += 4; }
    putf(ev + o, t->kind == GF_WK_CAR ? 1.0f : t->hp); o += 4;
    ev[o++] = t->sext_len; if (t->sext_len) { memcpy(ev + o, t->sext, t->sext_len); o += t->sext_len; }
    if (!gf_coop_send_event(EV_MIG_OFFER, ev, o)) return 0;
    t->mig_pend = 1; t->mig_t = now; t->mig_why = (uint8_t)why; if (why >= 1 && why <= 5) st_mg_off[why]++;
    { static int nl; if (nl++ < 60) { printf("[migrate] v1126M OFFER net %u (%s %08X, hp %.0f) -> partner: %s%s\n", t->netid, t->kind == GF_WK_CAR ? "car" : "ped", t->ent, (double)t->hp, mw_name(why), t->mig_gone ? " (culled here)" : ""); fflush(stdout); } }
    return 1;
}
static void mig_host_tick(const gf_world_ent* e, int n, const float me[3], int have_me, const float pm[16], int have_p, ULONGLONG now)   /* host_tick, every world tick */
{
    static ULONGLONG win; static int nwin;
    if (now - win >= 1000) { win = now; nwin = 0; }
    for (int i = 0; i < MAXT; i++) { tracked* t = &s_t[i]; int why = 0, gone = 0, ls = 0; float dr, dm = 1e9f;
        if (!t->used) continue;
        if (t->mig_pend) {   /* answer overdue -> NAK */
            if (now - t->mig_t > 3000) { t->mig_pend = 0; st_mg_tmo++; mig_cool_set(t->netid, now, 10000);
                if (t->mig_gone) { st_mg_gone_desp++; src_drop(i); } }
            continue; }
        if (!t->ent || t->corpse || t->claimed || !t->spawn_sent || !mig_kind_ok(t->kind)) continue;
        if (!t->missing) {   /* fresh: health + ambient class */
            if (t->kind == GF_WK_PED) { float hp; if (rd_hp(t->ent, &hp)) {
                    if (hp <= 0.0f && t->hp > 0.0f) gf_coop_pop_combat_drop(t->ent);   /* v1127G6 died */
                    if (hp > 0.0f && hp < t->hp - 1.0f) { extern int gf_coop_pop_combat_kept(uint32_t ent); extern void gf_coop_pop_combat_keep(uint32_t ent, unsigned ms);
                        if (gf_coop_pop_combat_kept(t->ent)) { gf_coop_pop_combat_keep(t->ent, 0); st_g5_refresh++;   /* v1127G5 still being fought here (hp dropping): keep extended */
                            { static int nl; if (nl++ < 20) { printf("[migrate] v1127G5 combat keep: net %u hp %.0f -> %.0f, keep extended (%ld)\n", t->netid, (double)t->hp, (double)hp, st_g5_refresh); fflush(stdout); } } } }
                    t->hp = hp; } } else t->hp = 1.0f;
            if (!t->amb) { int a = 0; t->amb = (gf_world_native_info(t->ent, &a, 0) && a) ? 1 : -1; } }
        dr = near_remote(&t->m[9], pm, have_p, 0);
        if (dr >= 1e8f) continue;
        if (have_me) { const float dx = t->m[9] - me[0], dz = t->m[11] - me[2]; dm = sqrtf(dx * dx + dz * dz); }
        if (t->missing >= 2 && !gf_world_native_alive(t->ent)) {   /* (a) culled by our population (deaths became corpses above) */
            float hp = t->hp; if (t->kind == GF_WK_PED && rd_hp(t->ent, &hp) && hp <= 0.0f) continue;
            if (!(t->hp > 0.0f)) continue;   /* normal despawn */
            if (t->amb == 1 && dr > s_mig_cull_r) continue;
            if (t->amb != 1) {   /* v1127W: a NON-ambient (mission / crew / police) NPC our game removed while the partner is near / sees it -> hand it over instead of a pop */
                if (!s_mig_nonamb || !(dr < s_mig_keep_r || remote_sees(&t->m[9], s_mig_view_r))) continue; }
            gone = 1; why = MW_CULL; }
        else if (t->missing >= 2 && s_mig_leftsel && t->kind == GF_WK_PED && !t->corpse && t->amb >= 0 && gf_world_native_alive(t->ent)) {
            /* v1127G10: a native that is ALIVE but left our selection would be despawned in ~8 ticks; c27-33 showed ~70% of the
             * "DESPAWN SEEN by the partner" pops are these. If the partner is near / sees it -> hand it over (cooldown + rate cap
             * apply, gone = 0: we still have it and get demoted to a proxy on ACK). GF_COOP_MIGRATE_LEFTSEL=0 off. */
            float m[12], hp = 0.0f, dr2; int a = 0;
            if (!read_ent_m12(t->ent, m) || !rd_hp(t->ent, &hp) || !(hp > 0.0f)) continue;
            if (!t->amb) t->amb = (gf_world_native_info(t->ent, &a, 0) && a) ? 1 : -1;
            if (t->amb != 1) continue;
            dr2 = near_remote(&m[9], pm, have_p, 0);
            if (!(dr2 < s_mig_keep_r || (dr2 < s_mig_view_r && remote_sees(&m[9], s_mig_view_r)))) continue;
            memcpy(t->m, m, sizeof t->m); memset(t->vel, 0, sizeof t->vel); memset(t->avel, 0, sizeof t->avel); t->hp = hp; why = MW_CULL; ls = 1;   /* v1127G11 vel zeroed (stale) */
            if (t->missing >= W_GONE_TICKS) t->missing = W_GONE_TICKS - 1;   /* v1127G11 (Codex): not despawned while still eligible but held by cooldown / rate cap / send failure */
            { static int nl; if (nl++ < 30) { printf("[migrate] v1127G10 net %u ped %08X alive but left our selection, partner %.0f m%s -> hand-over candidate\n", t->netid, t->ent, (double)dr2, dr2 < s_mig_keep_r ? "" : " (sees it)"); fflush(stdout); } } }
        else if (t->amb != 1) continue;   /* mission / crew / police from scripts exist on every game: never migrated while alive here */
        else if (t->missing) continue;
        else if (t->mig_req && now - t->mig_req_t <= 2000) why = MW_REQ;   /* v1127G6: cleared only once the offer went out (cooldown / rate cap no longer eat it) */
        else if (t->mig_req) { t->mig_req = 0; st_g6_req_exp++; }
        else if (t->mig_want) { t->mig_want = 0; if (dr <= s_mig_cull_r) why = MW_CULL; }
        if (!why) {   /* (b) partner nearer by > 30 m for > 3 s */
            const float scope = t->kind == GF_WK_CAR ? s_sc_car : s_sc_ped;
            if (dm - dr > s_mig_near && dr < scope) { if (!t->nearer_since) t->nearer_since = now; else if (now - t->nearer_since >= (ULONGLONG)s_mig_near_ms) why = MW_NEAR; }
            else t->nearer_since = 0; }
        if (!why && s_mig_view && t->kind == GF_WK_PED && have_me && dr < s_mig_view_r && dr <= dm + 15.0f) {   /* v1127W (e) VIEW: we do not see it, the partner does */
            if (!local_sees(&t->m[9]) && remote_sees(&t->m[9], s_mig_view_r)) { if (!t->view_since) t->view_since = now; else if (now - t->view_since >= (ULONGLONG)s_mig_view_ms) why = MW_VIEW; }
            else t->view_since = 0; }
        else if (!why) t->view_since = 0;
        if (why == MW_VIEW || why == MW_NEAR) { extern int gf_coop_pop_combat_kept(uint32_t ent); if (gf_coop_pop_combat_kept(t->ent)) { st_g5_skip++;
                { static int nl; if (nl++ < 20) { printf("[migrate] v1127G5 combat keep: %s hand-back of net %u (ped %08X, hp %.0f) skipped (%ld skips)\n", why == MW_VIEW ? "VIEW" : "NEARER", t->netid, t->ent, (double)t->hp, st_g5_skip); fflush(stdout); } }
                t->view_since = 0; t->nearer_since = 0; why = 0; } }   /* v1127G4 being fought here: no view / nearer hand-back (G5: logged) */
        if (!why || !(t->hp > 0.0f)) continue;
        if (!gone && !mig_cool_ok(t->netid, now)) continue;   /* v1127W: a culled one cannot ping-pong (we no longer have it) -> no cooldown / rate cap */
        if (!gone && nwin >= s_mig_rate) { st_mg_ratecap++; continue; }
        t->mig_gone = (uint8_t)gone; t->mig_ls = (uint8_t)ls;
        if (mig_send_offer(i, why, now)) { nwin++; t->nearer_since = 0; t->view_since = 0; if (why == MW_REQ) t->mig_req = 0; if (ls) st_g10_leftsel++; }   /* v1127G11 counted only when sent */
    }
    /* promoted factory proxies out of everybody's scope for 10 s -> release (they may not belong to a game requester) */
    for (int k = 0; k < 64; k++) { uint32_t ent = s_mown[k].ent; const gf_world_ent* w = 0;
        if (!ent) continue;
        for (int j = 0; j < n; j++) if (e[j].ent == ent) { w = &e[j]; break; }
        if (!w || !gf_world_native_alive(ent)) { s_mown[k].ent = 0; continue; }
        { const float dr = near_remote(&w->m[9], pm, have_p, 0); float dm = 1e9f; const float sc = w->kind == GF_WK_CAR ? s_sc_car : s_sc_ped;
          if (have_me) { const float dx = w->m[9] - me[0], dz = w->m[11] - me[2]; dm = sqrtf(dx * dx + dz * dz); }
          if (dr < sc || dm < sc) { s_mown[k].far_since = 0; continue; }
          if (!s_mown[k].far_since) { s_mown[k].far_since = now; continue; }
          if (now - s_mown[k].far_since < 10000) continue;
          { int up = 0, ok; extern int gf_proxy_unpromote(uint32_t e);   /* v1127W: off the update lists first (else an unsynced ghost keeps its brain) */
            __try { up = gf_proxy_unpromote(ent); } __except (EXCEPTION_EXECUTE_HANDLER) { up = 0; }
            ok = gf_world_release_native(ent); st_mg_orphan++; gf_world_mark_src(ent, 0); s_mown[k].ent = 0;
            printf("[migrate] v1126M promoted ent %08X out of every scope for 10 s -> released (%s, v1127W unpromoted %d)\n", ent, ok ? "game release" : "release refused", up); fflush(stdout); } }
    }
}
/* new owner: proxy slot i of netid becomes our source. 1 = done, 0 = promote failed, -1 = promotion not available */
static int mig_promote(int pi, uint16_t id, uint8_t kind, uint32_t cls, const uint8_t guid[16], const float m[12], const float vel[3], float hp, const uint8_t* ext, int el, int why)
{
    proxy snap; int fr = -1, adopted; uint32_t ent, e2; const ULONGLONG now = gf_coop_pose_now_ms();
    AcquireSRWLockShared(&s_plock); snap = s_p[pi]; ReleaseSRWLockShared(&s_plock);
    if (!snap.used || snap.netid != id || !snap.ent) return 0;
    for (int i = 0; i < MAXT; i++) if (!s_t[i].used) { fr = i; break; }
    if (fr < 0) return -2;
    ent = snap.ent; adopted = gf_proxy_is_adopted(id);
    { extern uint32_t gf_proxy_any_ent_of(uint16_t netid); if (gf_proxy_any_ent_of(id) != ent) return 0; }   /* proxy entity not live (re-spawning) */
    if (!adopted || s_mig_brainoff) { int pr = 0; __try { pr = gf_proxy_promote(ent, m, hp); } __except (EXCEPTION_EXECUTE_HANDLER) { pr = 0; }
        if (pr < 0) { st_mg_prom_na++; return -1; }
        if (pr == 0) { st_mg_prom_fail++; { static int nl; if (nl++ < 20) { printf("[migrate] v1126M net %u: gf_proxy_promote(%08X) FAILED -> stays a proxy (%s)\n", id, ent, mw_name(why)); fflush(stdout); } } return 0; }
        st_mg_prom_ok++; }
    e2 = gf_proxy_detach(id); if (!e2) e2 = ent; else st_mg_detach++;
    if (adopted && kind == GF_WK_PED) wr_hp(e2, hp);
    gf_world_mark_src(e2, 1);
    AcquireSRWLockExclusive(&s_plock); if (s_p[pi].used && s_p[pi].netid == id) s_p[pi].used = 0; ReleaseSRWLockExclusive(&s_plock);
    { tracked* t = &s_t[fr]; memset(t, 0, sizeof *t);
      t->used = 1; t->ent = e2; t->cls = cls; t->kind = kind; t->netid = id; memcpy(t->guid, guid, 16); memcpy(t->m, m, sizeof t->m); memcpy(t->vel, vel, sizeof t->vel);
      t->seen_ms = now; t->mig_in = 1; t->hp = hp; t->amb = 1; t->spawn_sent = 0;   /* SPAWN2 to the old owner: it keeps its demoted proxy (same id / class / guid) or builds one */
      if (el > 0) { t->sext_len = (uint8_t)(el > GF_WORLD_SPAWN_EXT_MAX ? GF_WORLD_SPAWN_EXT_MAX : el); memcpy(t->sext, ext, t->sext_len); } }
    if (!adopted) for (int k = 0; k < 64; k++) if (!s_mown[k].ent) { s_mown[k].ent = e2; s_mown[k].far_since = 0; break; }
    if (why == MW_REQ && kind == GF_WK_PED) { extern void gf_coop_pop_combat_keep(uint32_t ent, unsigned ms); gf_coop_pop_combat_keep(e2, 0); }   /* v1127G3 taken for a fight: our population must not cull it at once */
    mig_cool_set(id, now, 10000);
    { static int nl; if (nl++ < 60) { float me[3] = {0, 0, 0}; const int hm = gf_coop_local_pos(me); const float dx = m[9] - me[0], dz = m[11] - me[2];
        printf("[migrate] v1126M net %u is OURS now (%s %08X, %s, hp %.0f, %.0f m from us): %s\n", id, kind == GF_WK_CAR ? "car" : "ped", e2, adopted ? "our own demoted native" : "promoted proxy",
               (double)hp, hm ? (double)sqrtf(dx * dx + dz * dz) : -1.0, mw_name(why)); fflush(stdout); } }
    return 1;
}
/* old owner: ownership of slot i moved away -> drop the source slot (no despawn), demote / release our native */
static void mig_give_up(int i, const char* why)
{
    tracked t = s_t[i]; int alive, done = 0; float me[3]; float dm = 1e9f;
    s_t[i].used = 0; mig_lent(t.netid); gf_coop_pop_combat_drop(t.ent);   /* v1127G6 handed away */
    alive = !t.corpse && t.ent && gf_world_native_alive(t.ent);
    if (gf_coop_local_pos(me)) { const float dx = t.m[9] - me[0], dz = t.m[11] - me[2]; dm = sqrtf(dx * dx + dz * dz); }
    if (!alive) { st_mg_dem_gone++; return; }
    gf_world_mark_src(t.ent, 0);
    if (t.kind == GF_WK_PED && (t.mig_why != MW_CULL || (t.mig_ls && !t.mig_gone)) && dm < s_mig_dem_r) {   /* v1127G11 (Codex): a G10 left-selection hand-over is DEMOTED (no pop), true culls still released */
        int pi = -1;
        AcquireSRWLockExclusive(&s_plock);
        if (find_proxy(t.netid) < 0) for (int j = 0; j < MAXT; j++) if (!s_p[j].used) { pi = j; break; }
        ReleaseSRWLockExclusive(&s_plock);
        if (pi >= 0) { int r = 0; __try { r = gf_proxy_adopt_mig(t.netid, t.cls, t.guid, t.ent); } __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; }
            if (r == 1) { const ULONGLONG now = gf_coop_pose_now_ms();
                AcquireSRWLockExclusive(&s_plock);
                { proxy* p = &s_p[pi]; memset(p, 0, sizeof *p); p->used = 1; p->netid = t.netid; p->kind = t.kind; p->cls = t.cls; memcpy(p->guid, t.guid, 16); memcpy(p->m, t.m, sizeof p->m);
                  p->ent = t.ent; p->adopted = t.ent; p->last_rx = now; p->was_live = 1; p->sext_given = 1; p->pair_next = now + 3600000;
                  p->sext_len = t.sext_len; memcpy(p->sext, t.sext, t.sext_len); }
                ReleaseSRWLockExclusive(&s_plock);
                if (s_mig_brainoff) { __try { gf_native_brain_off(t.ent); } __except (EXCEPTION_EXECUTE_HANDLER) { } }
                st_mg_dem_adopt++; done = 1; } } }
    if (!done) { gf_world_release_native(t.ent); st_mg_dem_rel++; }
    { static int nl; if (nl++ < 60) { printf("[migrate] v1126M net %u given away (%s): our native %08X %s (%.0f m from us)\n", t.netid, why, t.ent, done ? "DEMOTED to a proxy" : "released", (double)dm); fflush(stdout); } }
}
static int src_slot_of(uint16_t id) { for (int i = 0; i < MAXT; i++) if (s_t[i].used && s_t[i].netid == id) return i; return -1; }
static void mig_on_event(uint16_t type, const uint8_t* d, int len)   /* main thread (guest_exec_events) */
{
    const ULONGLONG now = gf_coop_pose_now_ms();
    if (len < 4 || d[0] != 1) return;
    if (type == EV_MIG_OFFER) {
        uint16_t id; uint8_t kind, guid[16]; uint32_t cls; float m[12], v[3], hp; int o = 2, el, code = 0, pi;
        if (len < 90) return;
        id = get16(d + o); o += 2; kind = d[o++]; cls = get32(d + o); o += 4; memcpy(guid, d + o, 16); o += 16;
        for (int k = 0; k < 12; k++) { m[k] = getf(d + o); o += 4; } for (int k = 0; k < 3; k++) { v[k] = getf(d + o); o += 4; } hp = getf(d + o); o += 4;
        el = d[o++]; if (o + el > len) el = 0;
        AcquireSRWLockShared(&s_plock); pi = find_proxy(id); { const proxy* p = pi >= 0 ? &s_p[pi] : 0;
            if (!gf_coop_world_migrate_on()) code = 1; else if (!p) code = 2; else if (p->claimed || p->dying_ms) code = 3; else if (!p->ent) code = 4; else if (!mig_kind_ok(kind)) code = 5; }
        ReleaseSRWLockShared(&s_plock);
        if (!code) { const int r = mig_promote(pi, id, kind, cls, guid, m, v, hp, d + o, el, d[1]); code = r == 1 ? 0 : r == -1 ? 7 : r == -2 ? 8 : 6; }
        if (!code) { mig_send3(EV_MIG_ACK, 0, id); st_mg_ack_tx++; }
        else { mig_send3(EV_MIG_NAK, code, id); st_mg_nak_tx[code]++; { static int nl; if (nl++ < 30) { printf("[migrate] v1126M NAK net %u (code %d: %s)\n", id, code,
            code == 1 ? "migration off here" : code == 2 ? "no proxy" : code == 3 ? "claimed / despawn hold" : code == 4 ? "proxy pending" : code == 5 ? "car" : code == 6 ? "promote failed" : code == 7 ? "promotion not available" : "no source slot"); fflush(stdout); } } }
        return; }
    { const uint16_t id = get16(d + 2); const int i = src_slot_of(id);
      if (type == EV_MIG_ACK) { st_mg_ack_rx++; if (i >= 0 && s_t[i].mig_pend) mig_give_up(i, mw_name(s_t[i].mig_why)); return; }
      if (type == EV_MIG_NAK) { st_mg_nak_rx++; if (i >= 0 && s_t[i].mig_pend) { s_t[i].mig_pend = 0; mig_cool_set(id, now, 10000); if (s_t[i].mig_gone) { st_mg_gone_desp++; src_drop(i); } } return; }
      if (type == EV_MIG_REQ) { st_mg_req_rx++; if (i >= 0 && !s_t[i].mig_pend) { s_t[i].mig_req = 1; s_t[i].mig_req_t = GetTickCount64(); } return; }   /* v1127G6 time-stamped */
      if (type == EV_MIG_TAKE) { st_mg_take_rx++; if (i >= 0) { s_t[i].mig_why = MW_TIMEOUT; mig_give_up(i, "partner took it (we were silent)"); } return; } }
}
/* (c) receiver: proxy silent > take ms while connected, near us and nearer to us than to every remote player -> promote + TAKE */
static int mig_try_take(int i, const proxy* snap, const float m[12], ULONGLONG now)
{
    float me[3], hp = 1500.0f; const float v0[3] = {0, 0, 0}; int r;
    if (now - snap->last_rx <= (ULONGLONG)s_mig_take_ms || !gf_coop_connected() || !snap->ent || snap->claimed || !mig_kind_ok(snap->kind)) return 0;
    if (snap->ns && (snap->s[snap->ns - 1].fl & 4)) return 0;   /* a corpse */
    if (!gf_coop_local_pos(me)) return 0;
    { const float dx = m[9] - me[0], dz = m[11] - me[2]; if (dx * dx + dz * dz > s_mig_cull_r * s_mig_cull_r) return 0; }
    { gf_remote rr[GF_REMOTE_MAX]; if (gf_world_remotes(rr, GF_REMOTE_MAX) > 0 && !nearer_us(&m[9])) return 0; }
    if (!mig_cool_ok(snap->netid, now)) return 0;
    if (snap->kind == GF_WK_PED && rd_hp(snap->ent, &hp) && hp <= 0.0f) return 0;
    r = mig_promote(i, snap->netid, snap->kind, snap->cls, snap->guid, m, v0, hp, snap->sext, snap->sext_len, MW_TIMEOUT);
    if (r != 1) { mig_cool_set(snap->netid, now, 10000); return 0; }
    mig_send3(EV_MIG_TAKE, 0, snap->netid); st_mg_take_tx++;
    return 1;
}
/* agent C (combat control, trigger d): ask the owner of netid to hand it to us. 1 = already ours, 0 = requested, -1 = n/a (off,
 * unknown id, cooldown). Main PPU thread. */
int gf_world_request_control(uint16_t netid)
{
    const ULONGLONG now = gf_coop_pose_now_ms(); int known;
    if (!netid || !gf_coop_world_migrate_on()) return -1;
    if (src_slot_of(netid) >= 0) return 1;
    AcquireSRWLockShared(&s_plock); known = find_proxy(netid) >= 0; ReleaseSRWLockShared(&s_plock);
    if (!known || !mig_cool_ok(netid, now)) return -1;
    mig_cool_set(netid, now, 2000);
    mig_send3(EV_MIG_REQ, 0, netid); st_mg_req_tx++;
    return 0;
}
static void mig_status(void)
{
    if (!gf_coop_world_migrate_on()) return;
    { float me[3]; if (gf_coop_local_pos(me)) { const float pd = near_remote(me, 0, 0, 0); long np = 0, nd = 0;
        AcquireSRWLockShared(&s_plock); for (int i = 0; i < MAXT; i++) if (s_p[i].used) { np++; nd += s_p[i].adopted != 0; } ReleaseSRWLockShared(&s_plock);
        printf("[migrate] v1126M partner %.0f m away (us at %.0f %.0f %.0f) | proxies here %ld (our demoted natives %ld)\n", pd < 1e8f ? (double)pd : -1.0, (double)me[0], (double)me[1], (double)me[2], np, nd); } }
    { long own = 0, min_ = 0, pend = 0; for (int i = 0; i < MAXT; i++) if (s_t[i].used) { own++; min_ += s_t[i].mig_in; pend += s_t[i].mig_pend; }
      printf("[migrate] v1126M offers sent: cull %ld, nearer %ld, view %ld (v1127W), request %ld (rate capped %ld) | answers: ACK %ld NAK %ld timeout %ld | given away: demoted %ld, released %ld, already gone %ld, culled+refused -> despawned %ld | "
             "received offers: ACK %ld, NAK no-proxy %ld claimed %ld pending %ld car %ld failed %ld n/a %ld full %ld | promote ok %ld fail %ld n/a %ld, detached %ld | take tx %ld rx %ld | req tx %ld rx %ld | orphans released %ld | sources %ld (migrated in %ld, offers out %ld)\n",
             st_mg_off[MW_CULL], st_mg_off[MW_NEAR], st_mg_off[MW_VIEW], st_mg_off[MW_REQ], st_mg_ratecap, st_mg_ack_rx, st_mg_nak_rx, st_mg_tmo, st_mg_dem_adopt, st_mg_dem_rel, st_mg_dem_gone, st_mg_gone_desp,
             st_mg_ack_tx, st_mg_nak_tx[2], st_mg_nak_tx[3], st_mg_nak_tx[4], st_mg_nak_tx[5], st_mg_nak_tx[6], st_mg_nak_tx[7], st_mg_nak_tx[8],
             st_mg_prom_ok, st_mg_prom_fail, st_mg_prom_na, st_mg_detach, st_mg_take_tx, st_mg_take_rx, st_mg_req_tx, st_mg_req_rx, st_mg_orphan, own, min_, pend);
      printf("[migrate] v1127G5 combat keep: view/nearer hand-backs skipped %ld, keeps extended by hp loss %ld | v1127G6 control requests expired unoffered %ld | v1127G10 left-selection hand-over offers %ld | v1127G20 cap-sticky ranks %ld | v1127G23 own-proxy ents skipped as sources %ld | v1127G29 late GUIDs adopted %ld\n", st_g5_skip, st_g5_refresh, st_g6_req_exp, st_g10_leftsel, st_cap_sticky, st_g23_bound, st_guid_late);
      printf("[migrate] v1127W despawns sent %ld, of them SEEN by the partner %ld (native gone %ld, alive but deselected %ld; non-ambient %ld)\n", st_dsp_n, st_dsp_seen, st_dsp_gone_seen, st_dsp_alive_seen, st_dsp_nonamb_seen); fflush(stdout); }
}
static void guest_exec_events(void)
{
    for (;;) { qev ev; int have = 0;
        EnterCriticalSection(&s_qcs); if (s_qh != s_qt) { ev = s_q[s_qh]; s_qh = (s_qh + 1) & QEV_M; have = 1; } LeaveCriticalSection(&s_qcs);
        if (!have) break;
        const int n = ev.data[0];
        if (ev.type == GF_COOP_EV_WORLD_SPAWN && ev.len >= 1 + n*SPAWN_REC) {
            for (int k = 0; k < n; k++) guest_add_spawn(ev.data + 1 + k*SPAWN_REC, 0, 0);
        } else if (ev.type == GF_COOP_EV_WORLD_SPAWN2) {   /* v1063H variable records */
            for (int k = 0, pos = 1; k < n; k++) { if (pos + 72 > ev.len) break; const int el = ev.data[pos + 71]; if (pos + 72 + el > ev.len) break;
                guest_add_spawn(ev.data + pos, ev.data + pos + 72, el); pos += 72 + el; }
        } else if (ev.type >= 0x0127 && ev.type <= 0x012B) { mig_on_event(ev.type, ev.data, ev.len);   /* v1126M */
        } else if (ev.type == GF_COOP_EV_WORLD_DESPAWN && ev.len >= 1 + 2*n) {
            for (int k = 0; k < n; k++) { const uint16_t id = get16(ev.data + 1 + 2*k); int had = 0;
                float lp[3] = {0, 0, 0}; int lpv = 0;
                int hold = 0; const int dh = dh_on();
                AcquireSRWLockExclusive(&s_plock); const int i = find_proxy(id); if (i >= 0) { had = 1; if (s_p[i].ent && s_p[i].ns) { memcpy(lp, s_p[i].s[s_p[i].ns - 1].p, sizeof lp); lpv = 1; }
                    if (s_p[i].dying_ms) hold = 2;   /* v1071D: already held (no-anim cull) */
                    else if (dh && lpv && !s_p[i].claimed && local_sees(lp)) { s_p[i].dying_ms = gf_coop_pose_now_ms(); s_p[i].dying_t = -1.0; s_p[i].dying_why = 1; hold = 1; }
                    if (!hold) s_p[i].used = 0; } ReleaseSRWLockExclusive(&s_plock);   /* v1061B: release even while ent is 0 */
                if (hold) { if (hold == 1) st_dh_held++; st_despawn_rx++; continue; }   /* v1071D: goes when out of our view */
                if (lpv) { float me[3]; if (gf_coop_local_pos(me)) { const float dx = lp[0] - me[0], dz = lp[2] - me[2]; if (dx*dx + dz*dz < 3600.0f) st_pop_near++; } }   /* v1070B visible pops */
                if (had) { __try { gf_proxy_release(id); } __except (EXCEPTION_EXECUTE_HANDLER) { } }
                st_despawn_rx++; }
        }
    }
}

/* v1063H shared playback: samples -> transform at playback time t (ms, sender clock). di gets avel + ext of the bracket. */
static int interp_samples_k(const sample* S, int ns, double t, float m[12], float v[3], gf_world_drive_info* di, double xcap_ms)
{
    if (ns <= 0) return 0;
    int a = 0; while (a + 1 < ns && (double)S[a + 1].ms <= t) a++;
    const sample* sa = &S[a]; quat q = sa->q; float p[3] = {sa->p[0], sa->p[1], sa->p[2]}; float av[3] = {sa->av[0], sa->av[1], sa->av[2]}; float f = 0;
    const sample* sb = sa;
    v[0] = sa->v[0]; v[1] = sa->v[1]; v[2] = sa->v[2];
    if (a + 1 < ns && (double)sa->ms <= t) { sb = &S[a + 1];
        const double h = (double)(int32_t)(sb->ms - sa->ms);
        f = (float)((t - (double)sa->ms) / h); if (f < 0) f = 0; if (f > 1) f = 1;
        quat qb = sb->q; if (q.w*qb.w + q.x*qb.x + q.y*qb.y + q.z*qb.z < 0) { qb.w = -qb.w; qb.x = -qb.x; qb.y = -qb.y; qb.z = -qb.z; }
        q.w += (qb.w - q.w)*f; q.x += (qb.x - q.x)*f; q.y += (qb.y - q.y)*f; q.z += (qb.z - q.z)*f;
        for (int j = 0; j < 3; j++) { p[j] += (sb->p[j] - p[j])*f; v[j] += (sb->v[j] - v[j])*f; av[j] += (sb->av[j] - av[j])*f; }
        if (di) { st_pe_interp++; if (h > 200.0) st_pe_gap++; }
        if (s_spline && h > 1.0 && h <= 400.0) {   /* v1066C Catmull-Rom position + trajectory velocity (m/s) */
            const float hs = (float)(h * 0.001), s1 = f, s2 = f * f, s3 = s2 * f;
            const float h00 = 2*s3 - 3*s2 + 1, h10 = s3 - 2*s2 + s1, h01 = -2*s3 + 3*s2, h11 = s3 - s2;
            const float d00 = 6*s2 - 6*s1, d10 = 3*s2 - 4*s1 + 1, d01 = -6*s2 + 6*s1, d11 = 3*s2 - 2*s1;
            float ma[3], mb[3], sp[3], sv[3], dev = 0;
            for (int j = 0; j < 3; j++) { const float lin = (sb->p[j] - sa->p[j]) / hs; ma[j] = mb[j] = lin; }
            if (a > 0) { const double h0 = (double)(int32_t)(sb->ms - S[a - 1].ms); if (h0 > 1.0 && h0 <= 600.0) for (int j = 0; j < 3; j++) ma[j] = (sb->p[j] - S[a - 1].p[j]) / (float)(h0 * 0.001); }
            if (a + 2 < ns) { const double h2 = (double)(int32_t)(S[a + 2].ms - sa->ms); if (h2 > 1.0 && h2 <= 600.0) for (int j = 0; j < 3; j++) mb[j] = (S[a + 2].p[j] - sa->p[j]) / (float)(h2 * 0.001); }
            for (int j = 0; j < 3; j++) { sp[j] = h00*sa->p[j] + h10*hs*ma[j] + h01*sb->p[j] + h11*hs*mb[j];
                                          sv[j] = (d00*sa->p[j] + d10*hs*ma[j] + d01*sb->p[j] + d11*hs*mb[j]) / hs;
                                          dev += (sp[j] - p[j]) * (sp[j] - p[j]); }
            if (dev < 0.25f && fin(sv[0]) && fin(sv[1]) && fin(sv[2])) { for (int j = 0; j < 3; j++) { p[j] = sp[j]; v[j] = sv[j]; } }
            else { if (di) st_pe_spl_clamp++; for (int j = 0; j < 3; j++) v[j] = (sb->p[j] - sa->p[j]) / hs; }   /* overshoot guard: linear + segment velocity */
        } }
    else if ((double)sa->ms < t) {   /* short extrapolation (position + v1063H rotation by avel) */
        float dt = (float)((t - (double)sa->ms) * 0.001); const float cap = (float)(xcap_ms * 0.001); if (dt > cap) dt = cap;
        if (s_spline && a > 0) { const double hl = (double)(int32_t)(sa->ms - S[a - 1].ms);   /* v1066C last segment velocity (the EMA lags) */
            if (hl > 1.0 && hl <= 400.0) for (int j = 0; j < 3; j++) v[j] = (sa->p[j] - S[a - 1].p[j]) / (float)(hl * 0.001); }
        if (di) st_pe_extrap++;
        for (int j = 0; j < 3; j++) p[j] += v[j]*dt;
        { const float hx = 0.5f*av[0]*dt, hy = 0.5f*av[1]*dt, hz = 0.5f*av[2]*dt; const quat dq = {1.0f, hx, hy, hz}, q0 = q;   /* q' = dq * q (world-space omega) */
          q.w = dq.w*q0.w - dq.x*q0.x - dq.y*q0.y - dq.z*q0.z; q.x = dq.w*q0.x + dq.x*q0.w + dq.y*q0.z - dq.z*q0.y;
          q.y = dq.w*q0.y - dq.x*q0.z + dq.y*q0.w + dq.z*q0.x; q.z = dq.w*q0.z + dq.x*q0.y - dq.y*q0.x + dq.z*q0.w; } }
    else if (di && ns > 1) st_pe_before++;   /* playback before the oldest sample (buffer too short for the delay) */
    quat_to_rows(q, m); m[9] = p[0]; m[10] = p[1]; m[11] = p[2];
    if (di) { memcpy(di->m, m, sizeof di->m); memcpy(di->vel, v, sizeof di->vel); memcpy(di->avel, av, sizeof di->avel);
              di->ext_a = sa->el ? sa->ext : 0; di->ext_b = sb->el ? sb->ext : di->ext_a; di->ext_len = sa->el < sb->el ? sa->el : sb->el; if (!sb->el) di->ext_len = sa->el;
              di->ext_f = f; { const double age = t - (double)S[ns - 1].ms; di->age_ms = age > 0 ? (uint32_t)age : 0; } }
    return 1;
}
static int interp_samples(const sample* S, int ns, double t, float m[12], float v[3], gf_world_drive_info* di) { return interp_samples_k(S, ns, t, m, v, di, 250.0); }
/* v1066C adaptive playback clock (guest main thread). ideal = now - min offset - delay; delay = clamp(gap env + lateness env
 * + 25 ms, DELAY .. DELAY_MAX). Returns the playback time on the sender clock. */
static double play_clock(double now, double clk)
{
    double gap, late, newest; int have;
    AcquireSRWLockShared(&s_plock); gap = s_gap_env; late = s_late_env; newest = (double)s_newest_ms; have = s_newest_ok; ReleaseSRWLockShared(&s_plock);
    if (!s_adapt) return now - clk - s_delay;
    { double want = gap + late + 25.0; if (want < s_delay) want = s_delay; if (want > s_delay_max) want = s_delay_max;
      s_play_delay += (want > s_play_delay ? 0.02 : 0.005) * (want - s_play_delay); }   /* slow drift of the delay itself */
    const double ideal = now - clk - s_play_delay;
    st_pf_frames++;
    if (!s_play_ok || fabs(ideal - s_play_t) > 700.0) { if (s_play_ok) st_pf_jump++; s_play_t = ideal; s_play_wall = now; s_play_ok = 1; s_play_rate = 1.0; return s_play_t; }
    double dt = now - s_play_wall; s_play_wall = now; if (dt < 0) dt = 0; if (dt > 250.0) dt = 250.0;
    { double rate = 1.0 + (ideal - s_play_t) * 0.004; if (rate < 0.85) rate = 0.85; if (rate > 1.25) rate = 1.25;   /* 50 ms behind -> 1.2x */
      s_play_rate += (rate - s_play_rate) * 0.2; }
    double nt = s_play_t + dt * s_play_rate;
    if (s_play_rate > 1.02) st_pf_fast++; else if (s_play_rate < 0.98) st_pf_slow++;
    if (have && nt > newest + s_hold_ms) { nt = newest + s_hold_ms; if (nt < s_play_t) nt = s_play_t; st_pf_hold++; }   /* underrun: wait for data */
    s_play_t = nt;
    if (have) { const double lead = newest - s_play_t; st_lead_sum += lead; if (lead < st_lead_min) st_lead_min = lead; }
    st_delay_sum += s_play_delay;
    return s_play_t;
}
double gf_coop_world_play_ms(void) { return s_play_ok ? s_play_t : -1.0; }   /* v1066C: the world playback time (sender clock), for N's pose stream */

static void guest_publish_claimed(const int* idx, int n, int echo)   /* v1063H: guest -> host state of claimed entities */
{
    static ULONGLONG next = 0; static tracked up[16]; int order[16]; int no = 0; float org_c[2][3];
    const ULONGLONG now = gf_coop_pose_now_ms();
    if (!n || (next && now < next && next - now < 1000)) return;
    next = now + 33;
    for (int k = 0; k < n && no < 16; k++) {
        proxy snap; AcquireSRWLockShared(&s_plock); snap = s_p[idx[k]]; ReleaseSRWLockShared(&s_plock);
        float m[12]; if (!snap.used || !snap.ent || !read_ent_m12(snap.ent, m)) continue;
        if (echo) { if (snap.kind == GF_WK_CAR && s_echo_car_up != 0.0f) m[10] -= s_echo_car_up; else m[9] -= s_echo_ofs; }   /* undo the echo twin offset */
        tracked* t = &up[no]; memset(t, 0, sizeof *t); t->used = 1; t->netid = snap.netid; t->kind = snap.kind; t->ent = snap.ent; memcpy(t->m, m, sizeof m);
        if (snap.up_ms && now > snap.up_ms && now - snap.up_ms < 500) { const float dt = (float)(now - snap.up_ms) * 0.001f;
            for (int j = 0; j < 3; j++) t->vel[j] = (m[9 + j] - snap.up_m[9 + j]) / dt; }
        AcquireSRWLockExclusive(&s_plock); if (s_p[idx[k]].used && s_p[idx[k]].netid == snap.netid) { memcpy(s_p[idx[k]].up_m, m, sizeof m); s_p[idx[k]].up_ms = now; } ReleaseSRWLockExclusive(&s_plock);
        order[no] = no; no++;
    }
    if (!no) return;
    memcpy(org_c[0], up[0].m + 9, 12); memcpy(org_c[1], org_c[0], 12);
    {   /* send_snapshot reads s_t[]; build the packet here with the same record writer (1 fragment, <= 16 entities) */
        uint8_t pkt[SNAP_MAX_BYTES]; int pos = SNAP_HDR, cnt = 0; static uint32_t seq;
        for (int k = 0; k < no; k++) { const int rl = snap_put_rec(pkt + pos, &up[order[k]], org_c[0], SNAP_MAX_BYTES - pos); if (rl) { pos += rl; cnt++; } }
        if (!cnt) return;
        put32(pkt, SNAP2_MAGIC); put32(pkt + 4, ++seq); put32(pkt + 8, (uint32_t)now); pkt[12] = 0; pkt[13] = 1; pkt[14] = (uint8_t)cnt; pkt[15] = 0;
        putf(pkt + 16, org_c[0][0]); putf(pkt + 20, org_c[0][1]); putf(pkt + 24, org_c[0][2]);
        if (gf_coop_send_blob(SNAP_UP_CH, pkt, pos)) InterlockedIncrement(&st_up_tx);
    }
}

static void guest_tick(int conn)
{
    const ULONGLONG now = gf_coop_pose_now_ms(); const int echo = gf_coop_mode() == W_ECHO;
    const int sink_only = gf_coop_mode() == W_HOST;   /* v1068R host = sink of the joiner's owned natives: no suppression / adoption / claims */
    if (!conn) { if (s_guest_conn) { guest_release_all("disconnected"); if (!echo && !sink_only) { __try { gf_world_suppress_ambient(0); } __except (EXCEPTION_EXECUTE_HANDLER) { } } } s_guest_conn = 0; return; }
    static ULONGLONG s_conn_ms = 0; static int s_sup_started = 0;
    {   static int s_gen_g = -1; int g = 0; __try { g = gf_coop_session_gen(); } __except (EXCEPTION_EXECUTE_HANDLER) { g = 0; }   /* v1126M (C request) */
        if (s_guest_conn && s_gen_g >= 0 && g != s_gen_g) { guest_release_all("v1126M new session gen"); s_guest_conn = 0; }
        s_gen_g = g; }
    if (!s_guest_conn) { s_conn_ms = now; s_sup_started = 0; }
    if (!s_guest_conn) { s_guest_conn = 1; s_suppress_next = 0;
        if (sink_only) printf("[coop-world] v1068R host: partner connected -> showing the joiner's own street life (net ids >= 0x8000)\n");
        else printf("[coop-world] guest: connected -> host owns peds/traffic%s%s\n", echo ? " (echo: twins, no suppression)" : "", gf_coop_world_guest_auth() ? " near the host, we own ours far from it (v1068R)" : "");
        fflush(stdout); }
    if (!echo && !sink_only) { float pm[16], hp = 0; const int hv = gf_coop_partner_pose(pm, &hp); float hpos[3] = {pm[12], pm[13], pm[14]};   /* v1063H spatial suppression */
                 __try { gf_world_set_host_pos(hpos, hv && fin(hpos[0]) && fin(hpos[2])); } __except (EXCEPTION_EXECUTE_HANDLER) { } }
    if (!s_sup_started && (st_proxies > 0 || now - s_conn_ms > 20000)) {   /* v1063H: keep the guest's own street life until the host's
                                                                          * replacements exist (proxy settle gate ~10 s) */
        s_sup_started = 1; printf("[coop-world] v1063H guest: %s -> ambient suppression starts\n", st_proxies > 0 ? "first proxies live" : "20 s after connect"); fflush(stdout); }
    if (!echo && !sink_only && s_sup_started && now >= s_suppress_next && !gf_coop_world_migrate_on()) {   /* v1126M: population per player, no suppression */ s_suppress_next = now + 2000; __try { gf_world_suppress_ambient(1); } __except (EXCEPTION_EXECUTE_HANDLER) { } }
    guest_exec_events();
    double clk; AcquireSRWLockShared(&s_plock); clk = s_clk_off; ReleaseSRWLockShared(&s_plock);
    const double t_g = play_clock((double)now, clk); long live = 0;   /* v1066C (old: now - clk - s_delay); v1072W3 t_g = primary origin */
    const int adopt_on = !sink_only && s_adopt && !s_adopt_unsupported && (!echo || s_adopt == 2);
    int claimed[16]; int ncl = 0;
    for (int i = 0; i < MAXT; i++) {
        proxy snap; AcquireSRWLockShared(&s_plock); snap = s_p[i]; ReleaseSRWLockShared(&s_plock);
        if (!snap.used) continue;
        const double t = t_g + org_delta(snap.netid);   /* v1072W3 per-origin playback time */
        float m[12]; float v[3] = {0, 0, 0}; gf_world_drive_info di; memset(&di, 0, sizeof di);
        if (!interp_samples_k(snap.s, snap.ns, t, m, v, &di, snap.kind == GF_WK_CAR ? 250.0 : s_ped_extrap)) { memcpy(m, snap.m, sizeof m); memcpy(di.m, m, sizeof m); }
        if (snap.dying_ms) {   /* v1071D despawn hold: frozen at the time it was marked; released once out of our view or after the max */
            double tf = snap.dying_t;
            if (tf < 0.0) { tf = t; AcquireSRWLockExclusive(&s_plock); if (s_p[i].used && s_p[i].netid == snap.netid) s_p[i].dying_t = t; ReleaseSRWLockExclusive(&s_plock); }
            memset(&di, 0, sizeof di);
            if (!interp_samples_k(snap.s, snap.ns, tf, m, v, &di, 0.0)) { memcpy(m, snap.m, sizeof m); memcpy(di.m, m, sizeof m); }
            v[0] = v[1] = v[2] = 0.0f;
            if (DH_AGE(now, snap.dying_ms) > s_dh_max_ms || !local_sees(&m[9])) {
                AcquireSRWLockExclusive(&s_plock); if (s_p[i].used && s_p[i].netid == snap.netid) s_p[i].used = 0; ReleaseSRWLockExclusive(&s_plock);
                if (DH_AGE(now, snap.dying_ms) > s_dh_max_ms) { st_dh_rel_max++; { static int nl; if (nl++ < 60) { float me[3] = {0, 0, 0}; const int hm = gf_coop_local_pos(me); const float dx = m[9] - me[0], dz = m[11] - me[2];
                    printf("[coop-world] v1127W POP: held despawn of net %u (%s cls %08X ent %08X, why %d) released IN VIEW after %llu ms, %.0f m from us\n", snap.netid, snap.kind == GF_WK_CAR ? "car" : "ped", snap.cls, snap.ent, snap.dying_why,
                           (unsigned long long)(DH_AGE(now, snap.dying_ms)), hm ? (double)sqrtf(dx * dx + dz * dz) : -1.0); fflush(stdout); } } }
                else st_dh_rel_out++;
                __try { gf_proxy_release(snap.netid); } __except (EXCEPTION_EXECUTE_HANDLER) { } continue; }
        }
        if (echo && s_echo_pull) {   /* v1061B debug pull: fixed per-proxy offset chosen at first drive */
            if (!snap.pull_ok) { float me[3]; if (gf_coop_local_pos(me)) { const float a = (float)(snap.netid % 12) * 0.5236f, r = snap.kind == GF_WK_CAR ? 8.0f : 3.0f;
                snap.pull[0] = me[0] + cosf(a) * r - m[9]; snap.pull[1] = me[1] - m[10]; snap.pull[2] = me[2] + sinf(a) * r - m[11]; snap.pull_ok = 1;
                AcquireSRWLockExclusive(&s_plock); if (s_p[i].used && s_p[i].netid == snap.netid) { memcpy(s_p[i].pull, snap.pull, sizeof snap.pull); s_p[i].pull_ok = 1; } ReleaseSRWLockExclusive(&s_plock); } }
            if (snap.pull_ok) { m[9] += snap.pull[0]; m[10] += snap.pull[1]; m[11] += snap.pull[2]; }
        } else if (echo) { if (snap.kind == GF_WK_CAR && s_echo_car_up != 0.0f) m[10] += s_echo_car_up; else m[9] += s_echo_ofs; }
        memcpy(di.m, m, sizeof m);
        if (snap.claimed) { if (ncl < 16) claimed[ncl++] = i; if (snap.ent) live++; continue; }   /* v1063H: the local player drives it -> no host transform */
        if (!snap.dying_ms && snap.last_rx && snap.last_rx < now && gf_coop_world_migrate_on() && mig_try_take(i, &snap, m, now)) continue;   /* v1126M (c) owner silent -> we take it */
        if (!snap.dying_ms && snap.last_rx && snap.last_rx < now && now - snap.last_rx > W_STALE_MS) {
            if (snap.ent && snap.ns && !snap.claimed && dh_on() && local_sees(snap.s[snap.ns - 1].p)) {   /* v1071D: stale in view -> hold */
                AcquireSRWLockExclusive(&s_plock); if (s_p[i].used && s_p[i].netid == snap.netid) { s_p[i].dying_ms = now; s_p[i].dying_t = -1.0; s_p[i].dying_why = 1; } ReleaseSRWLockExclusive(&s_plock);
                st_dh_held++; continue; }   /* v1063P8: last_rx is stamped on the co-op thread AFTER this tick read now -> unsigned underflow = false "stale" (24-31 releases per echo run) */   /* v1061B: no snapshot / spawn for 5 s (host gone, id dropped) -> local release */
            AcquireSRWLockExclusive(&s_plock); if (s_p[i].used && s_p[i].netid == snap.netid) s_p[i].used = 0; ReleaseSRWLockExclusive(&s_plock);
            { static int nl; if (nl++ < 24) { printf("[coop-world] v1063H stale release net %u (%s class %08X ent %08X): no snapshot/spawn for %llu ms, %d samples, last sample age %d ms\n",
                  snap.netid, snap.kind == GF_WK_CAR ? "car" : "ped", snap.cls, snap.ent, (unsigned long long)(now - snap.last_rx), snap.ns,
                  snap.ns ? (int)((double)now - clk - (double)snap.s[snap.ns - 1].ms) : -1); fflush(stdout); } }
            if (snap.ent && snap.ns) { float me[3]; if (gf_coop_local_pos(me)) { const float dx = snap.s[snap.ns - 1].p[0] - me[0], dz = snap.s[snap.ns - 1].p[2] - me[2]; if (dx*dx + dz*dz < 3600.0f) st_pop_stale++; } }   /* v1070B */
            __try { gf_proxy_release(snap.netid); } __except (EXCEPTION_EXECUTE_HANDLER) { } st_stale_rel++; continue; }
        if (!snap.sext_given) {   /* v1063H appearance / variant bytes before the first spawn call */
            __try { proxy_spawn_ext(snap.netid, snap.kind, snap.sext, snap.sext_len); } __except (EXCEPTION_EXECUTE_HANDLER) { }
            AcquireSRWLockExclusive(&s_plock); if (s_p[i].used && s_p[i].netid == snap.netid) s_p[i].sext_given = 1; ReleaseSRWLockExclusive(&s_plock); }
        if (adopt_on && !snap.adopted && now >= snap.pair_next) {   /* v1063H: pair with the guest's own copy (mission / crew / police) */
            uint32_t cand = 0; int r = 0;
            __try { cand = gf_world_native_match(snap.cls, snap.guid, m + 9, s_adopt_r); } __except (EXCEPTION_EXECUTE_HANDLER) { cand = 0; }
            if (cand) { __try { r = gf_proxy_adopt(snap.netid, snap.kind, snap.cls, snap.guid, cand); } __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; }
                if (r < 0) { s_adopt_unsupported = 1; printf("[coop-world] v1063H gf_proxy_adopt not implemented -> guest natives are not paired\n"); fflush(stdout); }
                else if (r > 0) { st_adopt++; { static int nl; if (nl++ < 30) { printf("[coop-world] v1063H net %u adopted guest native %08X (class %08X)\n", snap.netid, cand, snap.cls); fflush(stdout); } } }
                else st_adopt_fail++; }
            AcquireSRWLockExclusive(&s_plock); if (s_p[i].used && s_p[i].netid == snap.netid) { s_p[i].pair_next = now + 500 + (ULONGLONG)(snap.netid % 7) * 37; if (r > 0) s_p[i].adopted = cand; } ReleaseSRWLockExclusive(&s_plock);
        }
        {   /* v1061B: gf_proxy_spawn of a known id is a cheap lookup returning the current entity (0 while pending) */
            uint32_t ent = 0; __try { ent = gf_proxy_spawn(snap.netid, snap.kind, snap.cls, snap.guid, m); } __except (EXCEPTION_EXECUTE_HANDLER) { ent = 0; }
            int pend_log = 0, pop_new = 0; ULONGLONG pend_ms = 0;   /* v1071B pop_new */
            AcquireSRWLockExclusive(&s_plock); if (s_p[i].used && s_p[i].netid == snap.netid) { if (!ent && s_p[i].ent) s_p[i].tries++; if (ent && !s_p[i].was_live) { s_p[i].was_live = 1; pop_new = 1; } s_p[i].ent = ent;
                if (s_p[i].adopted && ent != s_p[i].adopted) s_p[i].adopted = 0;   /* adopted native gone -> may pair again */
                if (ent) { if (s_p[i].pend_logged) pend_log = 2, pend_ms = now - s_p[i].pend_since; s_p[i].pend_since = 0; s_p[i].pend_logged = 0; }   /* v1064I */
                else { if (!s_p[i].pend_since) s_p[i].pend_since = now;
                       else if (now - s_p[i].pend_since > 5000) { st_pend_long++; if (!s_p[i].pend_logged) { s_p[i].pend_logged = 1; pend_log = 1; pend_ms = now - s_p[i].pend_since; } } } }
            ReleaseSRWLockExclusive(&s_plock);
            if (pend_log && s_cov_log) { static int nl; if (nl++ < 40) { float me[3] = {0, 0, 0}; const int hm = gf_coop_local_pos(me);
                const float dx = m[9] - me[0], dy = m[10] - me[1], dz = m[11] - me[2];
                printf("[coop-world] v1064I net %u (%s class %08X guid %02X%02X%02X%02X ext %u B) %s after %.1f s, %.0f m from the local player\n",
                       snap.netid, snap.kind == GF_WK_CAR ? "car" : "ped", snap.cls, snap.guid[0], snap.guid[1], snap.guid[2], snap.guid[3], snap.sext_len,
                       pend_log == 1 ? "still PENDING (no proxy entity)" : "now live", (double)pend_ms / 1000.0, hm ? (double)sqrtf(dx*dx + dy*dy + dz*dz) : -1.0); fflush(stdout); } }
            if (pop_new) { float me[3]; if (gf_coop_local_pos(me)) { const float dx = m[9] - me[0], dz = m[11] - me[2], d2 = dx * dx + dz * dz;   /* v1071B */
                if (d2 < 1600.0f) st_pop_in40++; else if (d2 < 6400.0f) st_pop_in80++; else st_pop_infar++; } }
            if (!ent) st_spawn_fail++; else live++;
        }
        if (!snap.dying_ms && s_na && snap.kind == GF_WK_PED && snap.ent && !snap.adopted && !(snap.ns && (snap.s[snap.ns - 1].fl & 4)) && zs_far_now()) {   /* v1071D no-anim cull */
            const float sp = sqrtf(v[0] * v[0] + v[2] * v[2]); int age = -1, cond = 0; ULONGLONG na = snap.noanim_ms;
            if (sp > 0.45f) { __try { age = gf_wpose_stream_age_ms(snap.netid); } __except (EXCEPTION_EXECUTE_HANDLER) { age = 0; }
                cond = (age < 0 || age > 2000) && nearer_us(&m[9]); }
            if (!cond) na = 0; else if (!na) na = now;
            AcquireSRWLockExclusive(&s_plock); if (s_p[i].used && s_p[i].netid == snap.netid) { s_p[i].noanim_ms = na;
                if (cond && now - na > 1500) { s_p[i].dying_ms = now; s_p[i].dying_t = t; s_p[i].dying_why = 2; } } ReleaseSRWLockExclusive(&s_plock);
            if (cond && now - na > 1500) { blk_add(snap.netid, now); st_na_cull++;
                { static int nl; if (nl++ < 12) { printf("[coop-world] v1071D no-anim cull net %u (ped %08X): moving %.1f m/s with %s -> stops, goes when out of view\n",
                    snap.netid, snap.ent, (double)sp, age < 0 ? "no pose stream" : "a silent pose stream"); fflush(stdout); } } }
        }
        __try { proxy_drive_info(snap.netid, snap.kind, &di); } __except (EXCEPTION_EXECUTE_HANDLER) { }
        __try { gf_proxy_drive(snap.netid, m, v); } __except (EXCEPTION_EXECUTE_HANDLER) { }
        { extern void gf_wpose_set_corpse(uint16_t id, int on); gf_wpose_set_corpse(snap.netid, snap.ns ? (snap.s[snap.ns - 1].fl & 4) != 0 : 0); }   /* v1071A */
    }
    guest_publish_claimed(claimed, ncl, echo);
    st_proxies = live;   /* gf_proxy_tick() is called by gf_coop_glue.c (W3) */
}

/* ------------------------------------------------------------------ v1063H claims (guest player drives a synced car) */
void gf_coop_world_claim(uint16_t netid, int on)   /* guest main thread (V) */
{
    uint8_t ev[3]; int had = 0;
    if (s_on != 1 || !netid) return;
    AcquireSRWLockExclusive(&s_plock); { const int i = find_proxy(netid); if (i >= 0) { had = 1; s_p[i].claimed = on ? 1 : 0; s_p[i].up_ms = 0; } } ReleaseSRWLockExclusive(&s_plock);
    put16(ev, netid); ev[2] = on ? 1 : 0;
    if (gf_coop_send_event(GF_COOP_EV_WORLD_CLAIM, ev, 3)) InterlockedIncrement(&st_claim_tx);
    printf("[coop-world] v1063H guest %s net %u%s\n", on ? "claims" : "releases", netid, had ? "" : " (not a known proxy)"); fflush(stdout);
}
static void host_owned_tick(void)   /* host main thread, every frame: apply the guest's state to claimed entities */
{
    double clk; AcquireSRWLockShared(&s_plock); clk = s_up_clk_off; ReleaseSRWLockShared(&s_plock);
    const double t = (double)gf_coop_pose_now_ms() - clk - 60.0;   /* short delay: the guest drives it live */
    for (int i = 0; i < MAXT; i++) { tracked* tr = &s_t[i]; owned o; int k = -1;
        if (!tr->used || !tr->claimed || !tr->ent || tr->corpse) continue;
        AcquireSRWLockShared(&s_plock); for (int j = 0; j < 16; j++) if (s_own[j].used && s_own[j].netid == tr->netid) { k = j; o = s_own[j]; break; } ReleaseSRWLockShared(&s_plock);
        if (k < 0 || !o.ns) continue;
        float m[12], v[3]; gf_world_drive_info di; memset(&di, 0, sizeof di);
        if (!interp_samples(o.s, o.ns, t, m, v, &di)) continue;
        __try { gf_world_owned_apply(tr->ent, tr->kind, &di); } __except (EXCEPTION_EXECUTE_HANDLER) { }
        st_owned_apply++;
    }
}
int gf_coop_world_src_has(uint32_t ent)   /* v1068R: 1 = the local peer sources this native (joiner: never release it) */
{
    if (!ent) return 0;
    for (int i = 0; i < MAXT; i++) if (s_t[i].used && s_t[i].ent == ent) return 1;
    return 0;
}
uint32_t gf_coop_world_ent_of(uint16_t netid)   /* v1063H host: entity of a synced net id (0 = none); V uses it */
{
    if (!netid) return 0;
    for (int i = 0; i < MAXT; i++) if (s_t[i].used && s_t[i].netid == netid) return s_t[i].ent;
    return 0;
}
int gf_coop_world_netid_of(uint32_t ent)   /* host: net id of a synced entity */
{
    if (!ent) return 0;
    for (int i = 0; i < MAXT; i++) if (s_t[i].used && s_t[i].ent == ent) return s_t[i].netid;
    return 0;
}
/* v1070D (agent D): kind (GF_WK_PED / GF_WK_CAR, both != 0) of a native this peer sources AND the partner already got (spawn
 * sent), 0 = not shown on the partner (not tracked, not sent yet, corpse slot). For gf_coop_pop_keep (gf_coop_npcfix.c), called
 * from the game's population sweep on the main PPU thread (same thread as host_tick). */
int gf_coop_world_src_kind(uint32_t ent)
{
    if (!ent || s_on != 1) return 0;
    for (int i = 0; i < MAXT; i++) if (s_t[i].used && s_t[i].ent == ent) { const int k = (s_t[i].spawn_sent && !s_t[i].corpse && !s_t[i].missing) ? (int)s_t[i].kind : 0;
        if (k && !s_t[i].mig_pend && gf_coop_world_migrate_on()) s_t[i].mig_want = 1;   /* v1126M trigger (a): our population wants it gone while the partner is near -> offer it (host_tick) */
        return k; }
    return 0;
}

/* ------------------------------------------------------------------ module */
void gf_coop_world_init(void)
{
    const char* e = getenv("GF_COOP_WORLD"); const int mode = gf_coop_mode();
    if (s_on >= 0) return;   /* once */
    if (mode == 0) return;   /* v1119M: co-op not running (yet) -- retried from the tick, so hosting / joining from the pause menu turns it on */
    s_on = (mode == W_HOST || mode == W_JOIN || mode == W_ECHO) && !(e && atoi(e) == 0) ? 1 : 0;
    if (!s_on) return;
    if ((e = getenv("GF_COOP_WORLD_HZ")) && atoi(e) > 0) s_hz = atoi(e) > 60 ? 60 : atoi(e);
    if ((e = getenv("GF_COOP_WORLD_R")) && atof(e) > 1.0) s_r = (float)atof(e);
    s_max = mode == W_ECHO ? 12 : 64;
    if ((e = getenv("GF_COOP_WORLD_MAX")) && atoi(e) > 0) s_max = atoi(e);
    if (s_max > SNAP_FRAGS * SNAP_PER_FRAG) s_max = SNAP_FRAGS * SNAP_PER_FRAG;
    if ((e = getenv("GF_COOP_WORLD_DELAY")) && atoi(e) >= 0) s_delay = (double)atoi(e);
    if ((e = getenv("GF_COOP_WORLD_ADAPT"))) s_adapt = atoi(e) != 0;               /* v1066C */
    if ((e = getenv("GF_COOP_WORLD_SPLINE"))) s_spline = atoi(e) != 0;             /* v1066C */
    if ((e = getenv("GF_COOP_WORLD_DELAY_MAX")) && atoi(e) > 0) s_delay_max = (double)atoi(e);
    if ((e = getenv("GF_COOP_WORLD_HOLD_MS")) && atoi(e) >= 0) s_hold_ms = (double)atoi(e);
    if ((e = getenv("GF_COOP_WORLD_PED_EXTRAP_MS")) && atoi(e) >= 0) s_ped_extrap = (double)atoi(e);
    if (s_delay_max < s_delay) s_delay_max = s_delay;
    s_play_delay = s_delay;
    printf("[coop-world] v1066C playback: adaptive clock %s (delay %.0f..%.0f ms, hold %.0f ms past newest), spline %s, ped extrapolation <= %.0f ms (GF_COOP_WORLD_ADAPT / _SPLINE)\n",
           s_adapt ? "ON" : "off", s_delay, s_delay_max, s_hold_ms, s_spline ? "ON (Catmull-Rom + trajectory velocity)" : "off (linear)", s_ped_extrap);
    if ((e = getenv("GF_COOP_WORLD_ECHO_OFS"))) s_echo_ofs = (float)atof(e);
    if ((e = getenv("GF_COOP_WORLD_ECHO_CAR_UP"))) s_echo_car_up = (float)atof(e);
    if ((e = getenv("GF_COOP_WORLD_ECHO_PULL"))) s_echo_pull = atoi(e) != 0;   /* 0 = side offset like peds */
    if ((e = getenv("GF_COOP_WORLD_ADOPT"))) s_adopt = atoi(e);   /* v1063H 0 off, 1 on (join), 2 also in echo */
    if ((e = getenv("GF_COOP_WORLD_ADOPT_R")) && atof(e) > 0) s_adopt_r = (float)atof(e);
    if ((e = getenv("GF_COOP_WORLD_FAR")) && atoi(e) > 0) s_far_m = atoi(e);
    if ((e = getenv("GF_COOP_WORLD_PED_EXT_REFRESH"))) s_ped_ext_refresh = atoi(e) != 0;   /* v1064Q */
    printf("[coop-world] v1064Q ped appearance ext refresh %s (host re-reads it with every rolling spawn refresh, guest re-delivers a changed ext)\n", s_ped_ext_refresh ? "ON" : "off");
    if ((e = getenv("GF_COOP_WORLD_COVLOG"))) s_cov_log = atoi(e) != 0;   /* v1064I */
    if ((e = getenv("GF_COOP_TOD_PROBE"))) s_tod_probe = atoi(e) != 0;    /* v1064I diagnostic, default off */
    if ((e = getenv("GF_COOP_TOD_SYNC"))) s_tod_sync = atoi(e) != 0;      /* v1064I3 default ON */
    printf("[coop-tod] v1064I3 time-of-day sync %s (GF_COOP_TOD_SYNC; host clock *0x101D97FC every 2 s, guest corrects > 30 game s)\n", s_tod_sync ? "ON" : "off");
    if (s_tod_probe) printf("[coop-tod] v1064I time-of-day / weather probe ON (GF_COOP_TOD_PROBE=1): heap scan 45 s after start, dumps every 10 s\n");
    printf("[coop-world] v1064I coverage diagnostics %s (GF_COOP_WORLD_COVLOG), spawn ext max %d B, snapshot ext max %d B\n", s_cov_log ? "ON" : "off", (int)GF_WORLD_SPAWN_EXT_MAX, (int)GF_WORLD_SNAP_EXT_MAX);
    if ((e = getenv("GF_COOP_WORLD_GUEST_AUTH"))) s_gauth = atoi(e);   /* v1068R */
    __try { s_auth_r = gf_world_release_radius(); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    if ((e = getenv("GF_COOP_WORLD_AUTH_R")) && atof(e) > 0) s_auth_r = (float)atof(e);
    printf("[coop-world] v1068R zone spawn authority %s (GF_COOP_WORLD_GUEST_AUTH=%d): %s\n", gf_coop_world_guest_auth() ? "ON" : "off", s_gauth,
           !gf_coop_world_guest_auth() ? "host owns every synced ped/car (old)" :
           mode == W_HOST ? "host owns its spawns (ids < 0x8000) and shows the joiner's own street life" :
           mode == W_JOIN ? "joiner owns its ambient spawns first seen beyond the zone radius from the host (ids >= 0x8000)" : "echo test: source uses the joiner id range, ambient only");
    printf("[coop-world] v1068R zone radius %.0f m (= joiner release radius, GF_COOP_WORLD_AUTH_R)\n", (double)s_auth_r);
    {   /* v1070B */
        if ((e = getenv("GF_COOP_WORLD_HANDOVER"))) s_ho = atoi(e) != 0;
        if ((e = getenv("GF_COOP_WORLD_HANDOVER_R")) && atof(e) > 0) s_ho_in = (float)atof(e);
        if ((e = getenv("GF_COOP_WORLD_HANDOVER_HIDE_M")) && atof(e) > 0) s_ho_hide_ped = (float)atof(e);
        if ((e = getenv("GF_COOP_WORLD_HANDOVER_HIDE_CAR_M")) && atof(e) > 0) s_ho_hide_car = (float)atof(e);
        if ((e = getenv("GF_COOP_WORLD_DEDUP"))) s_dd = atoi(e) != 0;
        if ((e = getenv("GF_COOP_WORLD_DEDUP_R")) && atof(e) > 0) s_dd_r_ped = (float)atof(e);
        if ((e = getenv("GF_COOP_WORLD_DEDUP_CAR_R")) && atof(e) > 0) s_dd_r_car = (float)atof(e);
        if ((e = getenv("GF_COOP_WORLD_DEDUP_MS")) && atoi(e) >= 0) s_dd_ms = atoi(e);
        if ((e = getenv("GF_COOP_WORLD_YIELD"))) s_yield = atoi(e) != 0;
        if ((e = getenv("GF_COOP_WORLD_OVERLAP_TEST"))) s_ovl_test = atoi(e);
        if ((e = getenv("GF_COOP_WORLD_REVCLAIM"))) s_revclaim = atoi(e) != 0;
        if (s_ho_in > s_auth_r - 10.0f) s_ho_in = s_auth_r - 10.0f;   /* keep the hysteresis band */
        printf("[coop-world] v1070B zone overlap: handover %s (host < %.0f m, hidden >= %.0f m peds / %.0f m cars from both players; GF_COOP_WORLD_HANDOVER), same-spot dedup %s (%.1f / %.1f m, %d ms; GF_COOP_WORLD_DEDUP), ped yield %s (GF_COOP_WORLD_YIELD), reverse car claim %s (GF_COOP_WORLD_REVCLAIM)%s\n",
               s_ho ? "ON" : "off", (double)s_ho_in, (double)s_ho_hide_ped, (double)s_ho_hide_car, s_dd ? "ON" : "off", (double)s_dd_r_ped, (double)s_dd_r_car, s_dd_ms,
               s_yield ? "ON" : "off", s_revclaim ? "ON" : "off", s_ovl_test ? (s_ovl_test >= 2 ? " | ECHO OVERLAP TEST 2 (all live)" : " | ECHO OVERLAP TEST 1 (handover live, dedup/yield count-only)") : ""); }
    InitializeCriticalSection(&s_qcs); s_qcs_ok = 1;
    gf_coop_set_event_handler(on_event);
    __try { gf_coop_add_peer_event_handler(w_peer_event); gf_coop_add_peer_blob_handler(w_peer_blob); } __except (EXCEPTION_EXECUTE_HANDLER) { }   /* v1072W3 */
    printf("[coop-world] v1063H WSN2 snapshots (avel + ext), SPAWN2 (ext), guest native adoption %s (%.0f m), claims, far > %d m at half rate\n",
           s_adopt ? (mode == W_ECHO && s_adopt != 2 ? "off in echo" : "ON") : "off", (double)s_adopt_r, s_far_m);
    printf("[coop-world] v1060W2 world sync ON (%s): %d Hz, radius %.0f m, max %d, playback delay %.0f ms\n",
           mode == W_HOST ? "host owns peds/traffic" : mode == W_JOIN ? "guest shows the host's peds/traffic" : "echo twins",
           s_hz, (double)s_r, s_max, s_delay);
    fflush(stdout);
}

/* v1064I time-of-day / weather PROBE (diagnostic, GF_COOP_TOD_PROBE=1, default off): nothing syncs the game clock or the weather
 * yet. RE: name table 0xB44600 -> TimeOfDayManager component (creator func_00022858, alloc 0xE0, ctor func_0053DF94, vtable
 * 0x10069C08) and WeatherManager component (creator func_0001F640, alloc 0xC8, ctor func_002730C4, vtable 0x1004AFF0). 45 s after
 * start the guest heap 0x20000000..0x60000000 is scanned (16 MB per frame) for those vtables; every 10 s the instances are dumped
 * (first full, then only changed words) -> the next round can pick the time / rain fields and sync them. Log [coop-tod]. */
static int s_tod_state = 0, s_tod_n = 0; static uint32_t s_tod_scan = 0x20000000u, s_tod_ea[8], s_tod_vt[8], s_tod_prev[8][64]; static int s_tod_have[8];
static uint32_t tod_be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void tod_probe_tick(void)
{
    static ULONGLONG start = 0, next = 0; static int dumps = 0; const ULONGLONG now = GetTickCount64();
    if (!start) start = now;
    if (s_tod_state == 0) { if (now - start < 45000) return; s_tod_state = 1; printf("[coop-tod] v1064I scanning guest heap for TimeOfDayManager (vt 10069C08) / WeatherManager (vt 1004AFF0)\n"); fflush(stdout); }
    if (s_tod_state == 1) {
        for (int k = 0; k < 256 && s_tod_scan < 0x60000000u; k++, s_tod_scan += 0x10000u) {
            if (!gf_memq_access(vm_base, s_tod_scan, 0x10000, 0)) continue;
            __try { const uint8_t* p = vm_base + s_tod_scan;
                for (uint32_t o = 0; o < 0x10000u && s_tod_n < 8; o += 4) { if (p[o] != 0x10) continue; { const uint32_t v = tod_be32(p + o);
                    if (v == 0x10069C08u || v == 0x1004AFF0u) { s_tod_ea[s_tod_n] = s_tod_scan + o; s_tod_vt[s_tod_n] = v; s_tod_n++; } } } }
            __except (EXCEPTION_EXECUTE_HANDLER) { }
        }
        if (s_tod_scan >= 0x60000000u) { s_tod_state = 2; next = now;
            printf("[coop-tod] v1064I scan done: %d instance(s)", s_tod_n); for (int i = 0; i < s_tod_n; i++) printf(" %s@%08X", s_tod_vt[i] == 0x10069C08u ? "TOD" : "WEATHER", s_tod_ea[i]);
            printf("\n"); fflush(stdout); }
        return;
    }
    if (now < next || dumps >= 40) return;
    next = now + 10000; dumps++;
    for (int i = 0; i < s_tod_n && i < 8; i++) { uint32_t w[64]; const int nw = s_tod_vt[i] == 0x10069C08u ? 0xE0 / 4 : 0xC8 / 4; char line[900]; int pos = 0, nch = 0;
        if (!gf_memq_access(vm_base, s_tod_ea[i], (size_t)nw * 4, 0)) continue;
        __try { for (int k = 0; k < nw; k++) w[k] = tod_be32(vm_base + s_tod_ea[i] + 4u * (uint32_t)k); } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (w[0] != s_tod_vt[i]) { printf("[coop-tod] v1064I %08X no longer a %s (vt %08X)\n", s_tod_ea[i], s_tod_vt[i] == 0x10069C08u ? "TOD" : "WEATHER", w[0]); s_tod_vt[i] = 0; continue; }
        for (int k = 0; k < nw && pos < (int)sizeof line - 40; k++) {
            if (s_tod_have[i] && w[k] == s_tod_prev[i][k]) continue;
            if (s_tod_have[i]) { float f; memcpy(&f, &w[k], 4); pos += snprintf(line + pos, sizeof line - (size_t)pos, " +%02X:%08X(%g)", k * 4, w[k], (double)f); }
            else pos += snprintf(line + pos, sizeof line - (size_t)pos, " %08X", w[k]);
            nch++; }
        printf("[coop-tod] v1064I %s@%08X t=%llus %s:%s\n", s_tod_vt[i] == 0x10069C08u ? "TOD" : "WEATHER", s_tod_ea[i], (unsigned long long)((now - start) / 1000),
               s_tod_have[i] ? "changed" : "full", nch ? line : " (none)");
        memcpy(s_tod_prev[i], w, (size_t)nw * 4); s_tod_have[i] = 1; }
    fflush(stdout);
}

/* v1064I3 TIME-OF-DAY SYNC (GF_COOP_TOD_SYNC, default 1 = on; 0 = off). Nothing synced the game clock before: each instance ran
 * its own time of day (sky / lighting). RE (lifted code): game clock object = *(u32*)0x101D97FC (TOC 0xB4BF60 slot +0x328, read by
 * the TestTimeOfDay component func_0026A2EC); func_0026B9AC = SetTime(clock, year, day, hour, min, sec) writes +0x4C year, +0x50 day
 * (0..364), +0x54 hour, +0x58 minute, +0x5C second, clears +0x68 / +0x6C (ticks) and +0x74 (cache key) and fills the cache
 * +0x78..+0x88; func_0026B7A0 = GetTime: total = day*103680000 + hour*4320000 + min*72000 + sec*1200 + *(+0x68) (1200 ticks per
 * game second), decoded into +0x7C..+0x88 whenever +0x74 != +0x68. Host: EV 0x0126 {u8 'T', u8 ver 1, 18 BE words +0x4C..+0x90}
 * every 2 s. Guest: if |host - guest| > 30 game seconds -> copy +0x4C..+0x5C, +0x68, +0x6C and the cache +0x74..+0x88 (the
 * rate fields +0x8C / +0x90 stay local). Echo: decode + log only (one shared clock). Log "[coop-tod] v1064I3 ...". */
enum { GF_COOP_EV_WORLD_TOD = 0x0126, TOD_CLOCK_PTR = 0x101D97FCu, TOD_NW = 18 };
static uint32_t tod_clock(void)
{
    uint32_t c = 0;
    if (!gf_memq_access(vm_base, TOD_CLOCK_PTR, 4, 0)) return 0;
    __try { c = tod_be32(vm_base + TOD_CLOCK_PTR); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (c < 0x10000u || c >= 0xF0000000u || !gf_memq_access(vm_base, c + 0x4Cu, TOD_NW * 4, 1)) return 0;
    return c;
}
static int tod_read(uint32_t c, uint32_t w[TOD_NW])
{
    __try { for (int k = 0; k < TOD_NW; k++) w[k] = tod_be32(vm_base + c + 0x4Cu + 4u * (uint32_t)k); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    /* sanity: year <= 3000, day <= 364, hour <= 23, min / sec <= 59 (SetTime clamps) */
    return w[0] <= 3000u && w[1] <= 364u && w[2] <= 23u && w[3] <= 59u && w[4] <= 59u;
}
static long long tod_total(const uint32_t w[TOD_NW])   /* ticks (1200 per game second), as func_0026B7A0 */
{ return (long long)w[1] * 103680000LL + (long long)w[2] * 4320000LL + (long long)w[3] * 72000LL + (long long)w[4] * 1200LL + (long long)w[7]; }
static void tod_fmt(const uint32_t w[TOD_NW], char* b, int n)
{ long long t = tod_total(w); const long long day = t / 103680000LL; t -= day * 103680000LL; const int h = (int)(t / 4320000LL); t -= (long long)h * 4320000LL;
  const int mi = (int)(t / 72000LL); t -= (long long)mi * 72000LL; snprintf(b, (size_t)n, "day %lld %02d:%02d:%02d", day, h, mi, (int)(t / 1200LL)); }
static void tod_tick(int mode, int conn)
{
    static ULONGLONG next_tx = 0, next_log = 0; const ULONGLONG now = GetTickCount64(); uint32_t w[TOD_NW]; char b1[48], b2[48];
    if (!s_tod_sync || !conn) return;
    const uint32_t c = tod_clock();
    if ((mode == W_HOST || mode == W_ECHO) && now >= next_tx) { next_tx = now + 2000;
        if (c && tod_read(c, w)) { uint8_t ev[2 + TOD_NW * 4]; ev[0] = 'T'; ev[1] = 1; for (int k = 0; k < TOD_NW; k++) { ev[2 + 4*k] = (uint8_t)(w[k] >> 24); ev[3 + 4*k] = (uint8_t)(w[k] >> 16); ev[4 + 4*k] = (uint8_t)(w[k] >> 8); ev[5 + 4*k] = (uint8_t)w[k]; }
            if (gf_coop_send_event(GF_COOP_EV_WORLD_TOD, ev, (int)sizeof ev)) st_tod_tx++; } }
    if ((mode == W_JOIN || mode == W_ECHO) && s_tod_rx_new && s_qcs_ok) {
        uint8_t ev[2 + TOD_NW * 4]; uint32_t hw[TOD_NW];
        EnterCriticalSection(&s_qcs); memcpy(ev, s_tod_rx, sizeof ev); s_tod_rx_new = 0; LeaveCriticalSection(&s_qcs);
        for (int k = 0; k < TOD_NW; k++) hw[k] = tod_be32(ev + 2 + 4*k);
        if (c && tod_read(c, w) && hw[0] <= 3000u && hw[1] <= 364u && hw[2] <= 23u && hw[3] <= 59u && hw[4] <= 59u) {
            const long long d = tod_total(hw) - tod_total(w); s_tod_last_diff = (long)(d / 1200LL);
            if (mode == W_JOIN && (d > 36000LL || d < -36000LL)) {   /* > 30 game seconds apart */
                __try { for (int k = 0; k < TOD_NW; k++) { const uint32_t off = 0x4Cu + 4u * (uint32_t)k;
                            if (off == 0x60u || off == 0x64u || off == 0x70u || off >= 0x8Cu) continue;   /* unknown / rate fields stay local */
                            uint8_t* q = vm_base + c + off; q[0] = (uint8_t)(hw[k] >> 24); q[1] = (uint8_t)(hw[k] >> 16); q[2] = (uint8_t)(hw[k] >> 8); q[3] = (uint8_t)hw[k]; } }
                __except (EXCEPTION_EXECUTE_HANDLER) { }
                st_tod_apply++;
                { static int nl; if (nl++ < 12) { tod_fmt(w, b1, sizeof b1); tod_fmt(hw, b2, sizeof b2);
                    printf("[coop-tod] v1064I3 guest clock %s -> host %s (diff %+ld game s) APPLIED\n", b1, b2, s_tod_last_diff); fflush(stdout); } } } } }
    if (now >= next_log) { next_log = now + 10000;
        if (c && tod_read(c, w)) { tod_fmt(w, b1, sizeof b1);
            printf("[coop-tod] v1064I3 %s clock %08X: %s (year %u) | tx %ld rx %ld applied %ld | last host-guest diff %+ld game s\n", mode == W_HOST ? "host" : mode == W_JOIN ? "guest" : "echo",
                   c, b1, w[0], st_tod_tx, st_tod_rx, st_tod_apply, s_tod_last_diff); }
        else printf("[coop-tod] v1064I3 clock not readable (ptr %08X)\n", c);
        fflush(stdout); }
}

/* v1127W rig aid GF_COOP_WTEST_TP=<file> holding "x y z [fx fz]": whenever the file content changes, the local player is placed at
 * that absolute position facing (fx, fz) (walk-away tests without a pad route; uses gf_coop_place_char of gf_coop_combat.c). */
extern int gf_coop_place_char(uint32_t ent, const float m12[12]);
static uint32_t wt_rd(uint32_t ea) { const uint8_t* q; if (!gf_memq_access(vm_base, ea, 4, 0)) return 0; q = vm_base + ea; return ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3]; }
static void wtest_tp_tick(void)
{
    static int on = -1; static char path[400], last[128]; static ULONGLONG next;
    const ULONGLONG now = GetTickCount64();
    if (on < 0) { const char* e = getenv("GF_COOP_WTEST_TP"); on = e && *e; if (on) { snprintf(path, sizeof path, "%s", e); printf("[coop-world] v1127W test teleport file %s\n", path); fflush(stdout); } }
    {   static int god = -1; static ULONGLONG gt;   /* GF_COOP_WTEST_GOD=1 (rig aid): local player health kept at its max (rig spawns next to hostile gangsters) */
        if (god < 0) { const char* e = getenv("GF_COOP_WTEST_GOD"); god = e && *e == '1'; if (god) { printf("[coop-world] v1127W test god mode ON (GF_COOP_WTEST_GOD)\n"); fflush(stdout); } }
        if (god && now - gt >= 100) { uint32_t a = 0, pl = 0; float hp = 0, mx = 0; gt = now;
            __try { a = wt_rd(0x10181290u); a = a >= 0x10000u ? wt_rd(a + 4) : 0; a = a >= 0x10000u ? wt_rd(a) : 0; if (a >= 0x17FCu + 0x10000u) pl = a - 0x17FCu; } __except (EXCEPTION_EXECUTE_HANDLER) { pl = 0; }
            if (pl && rd_hp(pl, &hp) && hp > 0.0f) { uint32_t u = wt_rd(pl + 0x14CCu); memcpy(&mx, &u, 4); if (!(mx > 100.0f && mx < 1e5f)) mx = 1500.0f; if (hp < mx) wr_hp(pl, mx); } } }
    if (!on || now < next) return;
    next = now + 200;
    { FILE* f = fopen(path, "rb"); char bf[128] = {0}; float x = 0, y = 0, z = 0, fx = 0, fz = 1; uint32_t a = 0, pl = 0; int n;
      if (!f) return;
      if (!fgets(bf, sizeof bf, f)) bf[0] = 0;
      fclose(f);
      for (char* q = bf; *q; q++) if (*q == '\r' || *q == '\n') *q = 0;
      if (!strcmp(bf, last)) return;
      snprintf(last, sizeof last, "%s", bf);
      n = sscanf(bf, "%f %f %f %f %f", &x, &y, &z, &fx, &fz);
      if (n < 3) return;
      { const float l = sqrtf(fx * fx + fz * fz); if (l > 1e-3f) { fx /= l; fz /= l; } else { fx = 0; fz = 1; } }
      __try { a = wt_rd(0x10181290u); a = a >= 0x10000u ? wt_rd(a + 4) : 0; a = a >= 0x10000u ? wt_rd(a) : 0; if (a >= 0x17FCu + 0x10000u) pl = a - 0x17FCu; } __except (EXCEPTION_EXECUTE_HANDLER) { pl = 0; }
      { const float m12[12] = { -fz, 0, fx, 0, 1, 0, -fx, 0, -fz, x, y, z }; const int r = pl ? gf_coop_place_char(pl, m12) : -1;
        printf("[coop-world] v1127W test teleport '%s' -> player %08X placed at %.1f %.1f %.1f facing %.2f %.2f: %d\n", bf, pl, (double)x, (double)y, (double)z, (double)fx, (double)fz, r); fflush(stdout); } }
}
void gf_coop_world_tick(void)   /* main PPU thread, from gf_coop_glue_tick */
{
    if (s_on < 0) gf_coop_world_init();
    if (s_on != 1) return;
    __try { wtest_tp_tick(); } __except (EXCEPTION_EXECUTE_HANDLER) { }   /* v1127W rig aid */
    if (s_tod_probe) tod_probe_tick();   /* v1064I diagnostic */
    const int mode = gf_coop_mode(), conn = gf_coop_connected();
    __try { tod_tick(mode, conn); } __except (EXCEPTION_EXECUTE_HANDLER) { }   /* v1064I3 time-of-day sync */
    if ((mode == W_HOST || mode == W_ECHO || (mode == W_JOIN && s_revclaim && gf_coop_world_guest_auth())) && conn) host_owned_tick();   /* v1063H every frame; v1070B also the joiner (reverse car claim) */
    { const int ga = gf_coop_world_guest_auth();   /* v1068R: both peers are source AND sink */
      if (mode == W_HOST || mode == W_ECHO || (mode == W_JOIN && ga)) host_tick(conn);
      if (mode == W_JOIN || mode == W_ECHO || (mode == W_HOST && ga)) guest_tick(conn); }
    static ULONGLONG last = 0; const ULONGLONG now = GetTickCount64();
    if (now - last >= 10000) { last = now;
        printf("[coop-world] tracked %ld | spawns sent %ld recv %ld | despawns sent %ld recv %ld | snapshot %.1f KB/s out (%ld snaps), %ld frags in | proxies live %ld, pending-frames %ld\n",
               st_tracked, st_spawn_tx, st_spawn_rx, st_despawn_tx, st_despawn_rx, (double)st_snap_bytes / 10240.0, st_snap_n, st_snap_rx, st_proxies, st_spawn_fail);
        if (st_pop_in40 + st_pop_in80 + st_pop_infar) printf("[coop-world] v1071B new NPC/car proxies appearing (pop-in): < 40 m from us %ld, 40-80 m %ld, farther %ld\n", st_pop_in40, st_pop_in80, st_pop_infar);
        printf("[coop-world] v1061B fragment loss %ld of %ld (%.1f %%) | stale proxies released %ld | clock offset %.0f ms, playback delay %.0f ms | v1062W2 corpses followed %ld\n",
               st_frag_exp - st_frag_got, st_frag_exp, st_frag_exp ? 100.0 * (double)(st_frag_exp - st_frag_got) / (double)st_frag_exp : 0.0, st_stale_rel, s_clk_off, s_delay, st_corpse_follow);
        { char wb[256]; gf_world_v1063_status(wb, sizeof wb);
          printf("[coop-world] v1063H spawn refreshes %ld | adopted %ld (refused %ld%s) | claims tx %ld rx %ld, upstream tx %ld rx %ld, owned applies %ld | ext bytes %ld | %s\n",
                 st_refresh, st_adopt, st_adopt_fail, s_adopt_unsupported ? ", gf_proxy_adopt missing" : "", st_claim_tx, st_claim_rx, st_up_tx, st_up_rx, st_owned_apply, st_ext_bytes, wb);
          if (st_ext_redeliver) printf("[coop-world] v1064Q changed spawn ext re-delivered %ld\n", st_ext_redeliver);   /* v1064Q */
          st_ext_bytes = 0; }
        if (gf_coop_world_guest_auth() && conn) {   /* v1068R */
            long own = 0, sink = 0; for (int i = 0; i < MAXT; i++) if (s_t[i].used) own++;
            AcquireSRWLockShared(&s_plock); for (int i = 0; i < MAXT; i++) if (s_p[i].used && (s_p[i].netid >= 0x8000u) == (mode != W_JOIN)) sink++; ReleaseSRWLockShared(&s_plock);
            printf("[coop-world] v1068R zone authority (%s): we own %ld (new %ld, skipped near host %ld, no host pos %ld) | partner-owned shown here %ld | wrong-range spawns dropped %ld\n",
                   mode == W_HOST ? "host" : mode == W_JOIN ? "joiner" : "echo", own, st_gauth_new, st_gauth_near_skip, st_gauth_nohost_skip, sink, st_wrong_range);
            st_gauth_new = st_gauth_near_skip = st_gauth_nohost_skip = 0; }
        if (conn) {   /* v1072W */
            gf_remote rr[GF_REMOTE_MAX]; const int nr = gf_world_remotes(rr, GF_REMOTE_MAX); const double kbs = (double)st_snap_bytes / 10240.0;
            if (s_kbps > 0) s_tier_over = kbs > (double)s_kbps;
            printf("[coop-world] v1072W3 n-peer world ids %s (self peer %d) | primary origin %d | fragments by origin: host %ld, j1 %ld, j2 %ld, j3 %ld, j4+ %ld | joiners 2.. sunk: events %ld blobs %ld\n",
                   npeer_ids() ? "ON" : "classic", gf_coop_mode() == W_JOIN ? my_peer_k() : 0, s_prim_o, st_ocl_frag[0], st_ocl_frag[1], st_ocl_frag[2], st_ocl_frag[3],
                   st_ocl_frag[4] + st_ocl_frag[5] + st_ocl_frag[6] + st_ocl_frag[7] + st_ocl_frag[8] + st_ocl_frag[9] + st_ocl_frag[10] + st_ocl_frag[11] + st_ocl_frag[12] + st_ocl_frag[13] + st_ocl_frag[14] + st_ocl_frag[15] + st_ocl_frag[16], st_np_ev, st_np_blob);
            printf("[coop-world] v1072W remotes now %d (max seen %ld; via N-peer API %ld / partner %ld lookups) | snapshot %.1f kB/s, %ld records (%.0f B/record), tiers mid %ld far %ld skipped %ld%s | non-ambient candidates %ld | v1072Z2 far-half non-ambient skipped %ld\n",
                   nr, st_rm_max, st_rm_api, st_rm_partner, kbs, st_snap_recs, st_snap_recs ? (double)st_snap_bytes / (double)st_snap_recs : 0.0,
                   st_tier_mid, st_tier_far, st_tier_skip, s_tier_over ? " | OVER BUDGET: tiers halved" : "", st_mprio_cand, st_farown_skip); st_mprio_cand = 0;
            st_snap_recs = 0; st_tier_mid = st_tier_far = st_tier_skip = 0; }
        if (conn) mig_status();   /* v1126M */
        if (conn && (mode == W_HOST || mode == W_JOIN)) { long held = 0; AcquireSRWLockShared(&s_plock); for (int i = 0; i < MAXT; i++) if (s_p[i].used && s_p[i].dying_ms) held++; ReleaseSRWLockShared(&s_plock);   /* v1071D */
            printf("[coop-world] v1071D receiver: despawns held while in view %ld (held now %ld; released out of view %ld, after max %ld, revived %ld) | no-anim cull %ld, re-sends ignored %ld | zone split (joiner %s): owned on our side %ld, handovers skipped %ld\n",
                   st_dh_held, held, st_dh_rel_out, st_dh_rel_max, st_dh_revive, st_na_cull, st_na_blocked, zs_far_now() ? "BEYOND the host zone" : "in the host zone / host", st_zs_own, st_zs_ho); }
        if (gf_coop_world_guest_auth() && conn)   /* v1070B overlap counters (cumulative) */
            printf("[coop-world] v1070B overlap (%s): handovers to the host zone %ld (refused %ld) | same-spot twins seen %ld, resolved %ld, held unsynced %ld, count-only %ld, release refused %ld | ped yields %ld (count-only %ld; partner peds in our zone %d, own %d, budget %.1f) | proxy despawns < 60 m from us %ld, stale < 60 m %ld | GUEST_AUTH mismatch %ld\n",
                   mode == W_HOST ? "host" : mode == W_JOIN ? "joiner" : "echo", st_ho, st_ho_fail, st_dd_seen, st_dd_res, st_dd_hold, st_dd_dry, st_dd_fail,
                   st_yield, st_yield_dry, s_k_ped, s_h_ped, (double)s_yb, st_pop_near, st_pop_stale, st_ga_mismatch);
        if (s_cov_log && (mode == W_HOST || mode == W_ECHO) && conn)   /* v1064I: what the host has vs sends */
            printf("[coop-world] v1064I coverage host: enumerated %d (valid %d) | sent %d (peds %d, cars %d, farthest %.0f m) | beyond %.0f m: %d (nearest %.0f m) | over cap %d: %d%s\n",
                   s_cov_enum, s_cov_valid, s_cov_in, s_cov_peds_in, s_cov_cars_in, (double)s_cov_far_max, (double)s_r, s_cov_beyond, (double)s_cov_beyond_min,
                   s_max, s_cov_capped, s_cov_nopos ? " | NO player positions" : "");
        if (s_cov_log && (mode == W_JOIN || mode == W_ECHO) && conn) {
            long known = 0, pend = 0, pl5 = 0, cars = 0; ULONGLONG nw = gf_coop_pose_now_ms(); AcquireSRWLockShared(&s_plock);
            for (int i = 0; i < MAXT; i++) if (s_p[i].used) { known++; if (s_p[i].kind == GF_WK_CAR) cars++;
                if (!s_p[i].ent && !s_p[i].claimed) { pend++; if (s_p[i].pend_since && nw - s_p[i].pend_since > 5000) pl5++; } }
            ReleaseSRWLockShared(&s_plock);
            printf("[coop-world] v1064I coverage guest: host entities known %ld (cars %ld), proxies live %ld, pending now %ld (> 5 s: %ld), pending>5s proxy-frames %ld\n",
                   known, cars, st_proxies, pend, pl5, st_pend_long); }
        if ((mode == W_JOIN || mode == W_ECHO) && conn) {   /* v1066C playback health */
            const long fr = st_pf_frames ? st_pf_frames : 1; const long pe = st_pe_interp + st_pe_extrap + st_pe_before;
            printf("[coop-world] v1066C playback: delay %.0f ms (sender gap env %.0f, lateness env %.0f, last %.0f) | lead mean %.0f min %.0f ms | clock frames %ld: hold(underrun) %ld, fast %ld, slow %ld, jumps %ld, offset re-centres %ld | proxy-frames %ld: interp %ld (gap>200ms %ld, spline clamp %ld), extrap %ld, before-oldest %ld\n",
                   st_delay_sum / (double)fr, s_gap_env, s_late_env, s_late_last, st_lead_sum / (double)fr, st_lead_min < 1e8 ? st_lead_min : 0.0,
                   st_pf_frames, st_pf_hold, st_pf_fast, st_pf_slow, st_pf_jump, st_clk_rec, pe, st_pe_interp, st_pe_gap, st_pe_spl_clamp, st_pe_extrap, st_pe_before); }
        st_pf_frames = st_pf_hold = st_pf_fast = st_pf_slow = st_pf_jump = st_clk_rec = 0; st_pe_interp = st_pe_extrap = st_pe_before = st_pe_gap = st_pe_spl_clamp = 0;
        st_lead_sum = 0; st_lead_min = 1e9; st_delay_sum = 0;
        st_pend_long = 0;
        st_snap_bytes = 0; st_snap_rx = 0; st_snap_n = 0; st_spawn_fail = 0; st_frag_exp = 0; st_frag_got = 0; fflush(stdout); }
}

/* v1061F: host-side list for the world NPC pose stream (gf_coop_wpose.c): synced peds whose spawn was sent, with the
 * last enumerated world position. Main PPU thread (same thread as host_tick). */
/* v1071A: horizontal speed (m/s) of a ped we source, for the pose scheduler (gf_coop_wpose.c): a corpse counts as moving for
 * its first 6 s (ragdoll fall), then as still; -1 = unknown net id. Main PPU thread. */
float gf_coop_world_src_speed(uint16_t netid)
{
    for (int i = 0; i < MAXT; i++) { const tracked* t = &s_t[i];
        if (!t->used || t->netid != netid) continue;
        if (t->corpse) return GetTickCount64() - t->corpse_ms < 6000 ? 99.0f : 0.0f;
        { const float v = sqrtf(t->vel[0] * t->vel[0] + t->vel[2] * t->vel[2]); return v == v && v < 50.0f ? v : 0.0f; } }
    return -1.0f;
}
int gf_coop_world_host_peds(uint32_t* ents, uint16_t* ids, float (*pos)[3], int max)
{
    int n = 0;
    for (int i = 0; i < MAXT && n < max; i++) {
        const tracked* t = &s_t[i];
        if (!t->used || !t->spawn_sent || t->missing || t->kind != GF_WK_PED || !t->ent) continue;
        ents[n] = t->ent; ids[n] = t->netid; pos[n][0] = t->m[9]; pos[n][1] = t->m[10]; pos[n][2] = t->m[11]; n++;
    }
    return n;
}
