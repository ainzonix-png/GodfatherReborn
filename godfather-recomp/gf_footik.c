/* gf_footik.c -- v1078F (Claude): procedural FOOT IK for the player (stairs, kerbs, slopes).
 *
 * User (2026-11-06 21:46): "procedural animations for the legs/feet, e.g. walking up stairs". The game moves the player
 * up stairs like a ramp while the walk cycle stays a flat-ground walk -> one foot floats above its step, the other sinks
 * into the next one. This module corrects the drawn pose after the game's own animation, every frame:
 *   1. a ray down under the heel and the ball of each foot (assets/gf_collision.bin, the game's own Havok collision, via
 *      gf_ragdoll.c col_ray) -> support height under that foot relative to the root (entity position = the animation's floor);
 *   2. the whole body drops by the lower foot's offset (so the leg on the higher step can bend instead of floating);
 *   3. two-bone IK hip -> knee -> ankle puts each ankle at (animation ankle + its ground offset) -- swing-phase lift is kept;
 *      the knee stays in the plane the animation gave it;
 *   4. the foot keeps its animated orientation, tilted towards the surface normal when planted (<= 25 deg).
 * Palette = model-space bone matrices (gf_ragdoll.c header); every slot is assigned once per rig to body / thigh / shin /
 * foot of one leg (capsule membership, arms tested first so hanging hands stay with the body) and moved rigidly.
 * Fades out (0.2 s) in a car, in cutscenes, when dead, airborne (> 0.3 m above ground) or faster than 5-7 m/s.
 * Hook: gf_rag_post_skel (after every skeleton update, func_003CC128 wrapper), player entity only.
 * Env: GF_FOOTIK=0 off, GF_FOOTIK_DBG=1 a state line every 0.25 s, GF_FOOTIK_MAX (0.45 m) largest correction.
 * Log tag [footik] (play-fast.out.log).
 *
 * v1108S (user 21:06: "procedural legs are ass right now, I really want it to work on stairs even though it's a ramp
 * collider"). Session logs: "bad-collision" 143-149 / 10 s and "fast" 650-788 / 800 -- the IK was OFF exactly on stairs and
 * while jogging; the left toe was the root bone at the model origin (rig "L foot: 0 13 14") so the L ball probe looked at
 * the wrong spot ([gpb] L ball off 600/600). Changes:
 *   - toe slots re-picked here (not within 5 cm of the model origin, ahead of / below the ankle, nearest-ankle side);
 *   - the root-vs-ground trust gate only for the COLLISION fallback; drawn ground may differ from the ramp root by a step;
 *   - speed fade GF_FOOTIK_VMAX (8.5) .. +2 m/s (was 5..7);
 *   - planted feet probe where they ARE (no velocity lead); swing feet lead;
 *   - SWING CLEARANCE: a 6th probe column ahead of the swing foot (GF_FOOTIK_AHEAD 0.22 m); the swinging foot is lifted to
 *     the higher of its own ground and the ground ahead, so it steps over the next stair edge instead of into the riser;
 *   - rsx_ui_overlay.c: probe columns shift SIDEWAYS (along the step) instead of towards the camera (previous tread). */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern uint8_t* vm_base;
extern int gf_rag_bone_slots(uint32_t ent, uint32_t anim, int* slot21, int* nslot);
extern int gf_rag_ray(const float* o, const float* d, float maxd, float* t, float* n);
extern int gf_rag_ent_frame(uint32_t ent, float* M9, float* T3);   /* model = M (w - T) */
extern int gf_cutscene_active(void);
extern int gf_rag_rig_groups(uint32_t anim, const int* key, int nk, uint8_t* out, float* var_out, int nslot);

enum { PEL, LUM, THO, NEC, HED, SHL, ELL, WRL, SHR, ELR, WRR, HIL, KNL, ANL, HIR, KNR, ANR, TOL, TOR, FIL, FIR, NB };
enum { G_BODY, G_TH, G_SH, G_FT };   /* group = leg * 3 + part (1..3 left, 4..6 right) */
#define MAXS 128

typedef struct { float x, y, z; } v3;
static v3 V(float x, float y, float z) { v3 r = { x, y, z }; return r; }
static v3 add(v3 a, v3 b) { return V(a.x + b.x, a.y + b.y, a.z + b.z); }
static v3 sub(v3 a, v3 b) { return V(a.x - b.x, a.y - b.y, a.z - b.z); }
static v3 mul(v3 a, float k) { return V(a.x * k, a.y * k, a.z * k); }
static float dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static v3 cross(v3 a, v3 b) { return V(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static float len(v3 a) { return sqrtf(dot(a, a)); }
static v3 nrm(v3 a) { const float l = len(a); return l > 1e-6f ? mul(a, 1.0f / l) : V(0, 0, 0); }
static float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static int fok(float f) { return f == f && f > -1e6f && f < 1e6f; }
typedef struct { float m[9]; } m3;   /* y = M x, m[r*3+c] */
static v3 mv(const m3* M, v3 v) { return V(M->m[0] * v.x + M->m[1] * v.y + M->m[2] * v.z, M->m[3] * v.x + M->m[4] * v.y + M->m[5] * v.z, M->m[6] * v.x + M->m[7] * v.y + M->m[8] * v.z); }
static v3 mtv(const m3* M, v3 v) { return V(M->m[0] * v.x + M->m[3] * v.y + M->m[6] * v.z, M->m[1] * v.x + M->m[4] * v.y + M->m[7] * v.z, M->m[2] * v.x + M->m[5] * v.y + M->m[8] * v.z); }
static m3 mm(const m3* A, const m3* B) { m3 r; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { float s = 0; for (int k = 0; k < 3; k++) s += A->m[i * 3 + k] * B->m[k * 3 + j]; r.m[i * 3 + j] = s; } return r; }
static m3 ident(void) { m3 r = { { 1, 0, 0, 0, 1, 0, 0, 0, 1 } }; return r; }
static m3 axis_angle(v3 ax, float a)
{
    ax = nrm(ax); const float c = cosf(a), s = sinf(a), t = 1 - c;
    m3 r = { { t * ax.x * ax.x + c, t * ax.x * ax.y - s * ax.z, t * ax.x * ax.z + s * ax.y,
               t * ax.x * ax.y + s * ax.z, t * ax.y * ax.y + c, t * ax.y * ax.z - s * ax.x,
               t * ax.x * ax.z - s * ax.y, t * ax.y * ax.z + s * ax.x, t * ax.z * ax.z + c } };
    return r;
}
static m3 rot_onto(v3 from, v3 to, float max_ang)   /* shortest rotation from -> to (angle capped) */
{
    from = nrm(from); to = nrm(to);
    const v3 ax = cross(from, to); const float s = len(ax), c = dot(from, to);
    if (s < 1e-6f) return ident();
    float a = atan2f(s, c); if (a > max_ang) a = max_ang;
    return axis_angle(ax, a);
}

static int ok(uint32_t ea) { return ea >= 0x10000u && ea < 0xF0000000u; }
static uint32_t rd32(uint32_t ea) { const uint8_t* p = vm_base + ea; return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void wr32(uint32_t ea, uint32_t v) { uint8_t* p = vm_base + ea; p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static float rdf(uint32_t ea) { uint32_t u = rd32(ea); float f; memcpy(&f, &u, 4); return f; }
static void wrf(uint32_t ea, float f) { uint32_t u; memcpy(&u, &f, 4); wr32(ea, u); }
static double qpc_s(void) { static LARGE_INTEGER fq; LARGE_INTEGER t; if (!fq.QuadPart) QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t); return (double)t.QuadPart / (double)fq.QuadPart; }

static uint32_t player_ent(void)
{
    uint32_t a = rd32(0x10181290u); if (!ok(a)) return 0; a = rd32(a + 4); if (!ok(a)) return 0; a = rd32(a);
    return (ok(a) && a >= 0x17FCu) ? a - 0x17FCu : 0;
}
static int player_in_car(uint32_t pl)
{
    const uint32_t car = rd32(pl + 0x1718u);
    return ok(car) && car == rd32(pl + 0x4Cu) && (rd32(pl + 0x5C4u) & 0x8u);
}

static int s_on = -1, s_dbg = 0, s_nlog = 0; static float s_max = 0.45f, s_trust = 0.08f, s_down = 0.24f;   /* v1094F; v1108S down 0.22 -> 0.24 */
static float s_vmax = 3.2f, s_ahead = 0.22f;   /* v1108S 8.5; v1112K 3.2 (user: "small chop when running, extremely frequent" -- the IK
                                                 * hovered at its fade edge while running 7-8 m/s on noisy probes: pelvis bob every step) */
static float s_settle = 0.12f;   /* v1108V */
#define FLOG(...) do { if (s_nlog < 400) { s_nlog++; printf(__VA_ARGS__); fflush(stdout); } } while (0)
static struct {
    uint32_t ent, anim, pal; int nslot, slot[NB]; uint8_t grp[MAXS]; int ready; double t_retry;
    float w, off_s[2], pel_s, spd_s; v3 T_prev; double t_prev; int have_prev;
    float mem_y[2], mem_x[2], mem_z[2]; double mem_t[2]; int feet_ok_prev; long n_mem;   /* v1108T */
    int lk_on[2]; float lk_y[2], lk_x[2], lk_z[2]; long n_lock; double lk_t[2]; float raw_off[2]; int raw_have[2]; int raw_drawn[2];   /* v1108U foot lock; v1108V settle */ double lk_dis_t[2]; long n_relock;   /* v1110W */
    uint32_t sig_ea, sig_val;   /* a word we wrote: unchanged next call = the palette was not refreshed -> do not apply twice */
    /* stats */
    double t_stat; long n_call, n_act, n_skip, n_miss; float min_pel, max_off, min_off;
    long g_car, g_cs, g_dead, g_fast, g_noground, g_air, g_bad, n_probe, n_fb, n_ffb, n_body, n_ahead; v3 miss_pos; int have_miss;   /* gate counters (calls) */
    int waiting_logged;
} S;

static float seg_t(v3 p, v3 a, v3 b, float* dist)
{
    const v3 ab = sub(b, a); const float L2 = dot(ab, ab); if (L2 < 1e-8f) { *dist = len(sub(p, a)); return 0; }
    const float t = dot(sub(p, a), ab) / L2; const v3 q = add(a, mul(ab, clampf(t, 0, 1)));
    *dist = len(sub(p, q)); return t;
}
static v3 slot_pos(uint32_t pal, int s) { const uint32_t m = pal + (uint32_t)s * 64u; return V(rdf(m + 0x30), rdf(m + 0x34), rdf(m + 0x38)); }

/* every palette slot -> body / thigh / shin / foot of a leg.
 * v1078F-3 (user screenshots 22:00: hands dragged + stretched with the legs): ELIGIBILITY first -- only slots below the
 * pelvis (+5 cm) and outside both arm capsules (upper arm, forearm + hand extended to 1.6 x past the wrist) may join a leg;
 * everything else stays with the body. Among the eligible slots, the LEARNED rig (gf_ragdoll.c v1055B: positional variance
 * of each slot in each key bone's frame) decides, once it has >= 600 samples and the slot is rigid (< 5 cm spread) with a
 * leg bone; the 209-sample rig of the first try (player mostly standing) called fingers "rigid with the thigh".
 * Returns 1 ok, 0 failed, -1 rig not learned yet. */
static int arm_slot(v3 q, const v3* P)
{
    const int seg[4][2] = { { ELL, WRL }, { ELR, WRR }, { SHL, ELL }, { SHR, ELR } };
    for (int a = 0; a < 4; a++) {
        const v3 A = P[seg[a][0]], B = P[seg[a][1]], ab = sub(B, A); const float L2 = dot(ab, ab); if (L2 < 1e-6f) continue;
        const float t = dot(sub(q, A), ab) / L2, tmax = a < 2 ? 1.6f : 1.0f;
        if (t < -0.05f || t > tmax) continue;
        if (len(sub(q, add(A, mul(ab, t)))) < (a < 2 ? 0.14f : 0.13f)) return 1;
    }
    return 0;
}
static int build_groups(void)
{
    const int key[7] = { S.slot[PEL], S.slot[HIL], S.slot[KNL], S.slot[ANL], S.slot[HIR], S.slot[KNR], S.slot[ANR] };
    for (int i = 0; i < 7; i++) if (key[i] < 0 || key[i] >= S.nslot) return 0;
    v3 P[NB];
    for (int b = 0; b < NB; b++) { P[b] = S.slot[b] >= 0 ? slot_pos(S.pal, S.slot[b]) : V(0, 0, 0); if (!fok(P[b].x)) return 0; }
    const v3 up = nrm(sub(P[NEC], P[PEL])); if (len(up) < 0.5f) return 0;
    uint8_t g[MAXS]; float var[MAXS];
    const int n = gf_rag_rig_groups(S.anim, key, 7, g, var, S.nslot);
    if (n < 600) return -1;
    int cnt[7] = { 0 }, n_arm = 0, n_high = 0, n_loose = 0; char lst[7][200]; for (int i = 0; i < 7; i++) lst[i][0] = 0;
    for (int s = 0; s < S.nslot; s++) {
        const v3 q = slot_pos(S.pal, s); int gg = g[s] < 7 ? g[s] : 0;
        if (gg) {
            if (dot(sub(q, P[PEL]), up) > 0.05f) { gg = G_BODY; n_high++; }
            else if (arm_slot(q, P)) { gg = G_BODY; n_arm++; }
            else if (var[s] > 0.0025f) { gg = G_BODY; n_loose++; }
        }
        S.grp[s] = (uint8_t)gg;
    }
    for (int leg = 0; leg < 2; leg++) {   /* the joint bones themselves */
        S.grp[S.slot[leg ? HIR : HIL]] = (uint8_t)(leg * 3 + G_TH); S.grp[S.slot[leg ? KNR : KNL]] = (uint8_t)(leg * 3 + G_SH);
        S.grp[S.slot[leg ? ANR : ANL]] = (uint8_t)(leg * 3 + G_FT);
        if (S.slot[leg ? TOR : TOL] >= 0 && !arm_slot(slot_pos(S.pal, S.slot[leg ? TOR : TOL]), P)) S.grp[S.slot[leg ? TOR : TOL]] = (uint8_t)(leg * 3 + G_FT);
    }
    S.grp[S.slot[PEL]] = G_BODY;
    for (int s = 0; s < S.nslot; s++) { const int gg = S.grp[s]; cnt[gg]++;
        if (gg) { char b[12]; snprintf(b, sizeof b, " %d", s); if (strlen(lst[gg]) + strlen(b) < sizeof lst[gg]) strcat(lst[gg], b); } }
    FLOG("[footik] v1078F-3 player rig (learned, %d samples): %d slots -> body %d | L thigh %d shin %d foot %d | R thigh %d shin %d foot %d (kept on the body: %d above the hips, %d arm, %d not rigid)\n",
         n, S.nslot, cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], cnt[5], cnt[6], n_high, n_arm, n_loose);
    FLOG("[footik]   slots L thigh:%s | shin:%s | foot:%s || R thigh:%s | shin:%s | foot:%s\n", lst[1], lst[2], lst[3], lst[4], lst[5], lst[6]);
    if (cnt[3] > 12 || cnt[6] > 12 || cnt[1] > 16 || cnt[4] > 16) { FLOG("[footik] grouping looks wrong (too many leg slots) -> foot IK off for this rig\n"); return 0; }
    return 1;
}

/* v1108S: toe tips. gf_rag_bone_slots (rag_bones) takes the farthest slot within 28 cm below the ankle -- on the left foot
 * that was the ROOT bone at the model origin (rig log "L foot: 0 13 14"). Re-picked: not within 5 cm of the origin, not a
 * key joint, 5..28 cm from this ankle, nearer this ankle than the other, not above the ankle + 2 cm, and in FRONT of the ankle
 * (model +z or, if the anim faces -z, the side the knee points to). */
static void fix_toes(void)
{
    for (int leg = 0; leg < 2; leg++) {
        const int an = S.slot[leg ? ANR : ANL], ao = S.slot[leg ? ANL : ANR], kn = S.slot[leg ? KNR : KNL];
        if (an < 0 || ao < 0 || kn < 0) continue;
        const v3 A = slot_pos(S.pal, an), O = slot_pos(S.pal, ao), K = slot_pos(S.pal, kn);
        v3 fwd = sub(K, A); fwd.y = 0; fwd = len(fwd) > 0.03f ? nrm(fwd) : V(0, 0, 0);   /* knees point forward of the ankle (unknown when straight) */
        int best = -1; float bd = 0.05f;
        for (int s = 0; s < S.nslot; s++) {
            int key = 0; for (int b = 0; b < NB; b++) if (b != TOL && b != TOR && S.slot[b] == s) key = 1;
            if (key) continue;
            const v3 q = slot_pos(S.pal, s); if (!fok(q.x)) continue;
            if (len(q) < 0.05f) continue;                       /* root / motion bones at the model origin */
            const float d = len(sub(q, A));
            if (d <= bd || d > 0.28f || len(sub(q, O)) < d || q.y > A.y + 0.02f) continue;
            if (len(fwd) > 0.5f && dot(sub(q, A), fwd) < -0.02f) continue;   /* behind the ankle = not a toe */
            bd = d; best = s;
        }
        const int was = S.slot[leg ? TOR : TOL];
        S.slot[leg ? TOR : TOL] = best;   /* -1 = none: the ball point falls back to the ankle below */
        FLOG("[footik] v1108S %s toe slot %d -> %d (%.2f m from the ankle)\n", leg ? "R" : "L", was, best, best >= 0 ? bd : 0.0f);
    }
}

/* support height (world y) under a world point, rays from up to 0.6 m above the root down to 0.75 m below it */
static int ground_at(v3 p, float root_y, float* gy, v3* n)
{
    const float o[3] = { p.x, root_y + 0.6f, p.z }, d[3] = { 0, -1, 0 }; float t, nn[3];
    if (!gf_rag_ray(o, d, 1.35f, &t, nn) || nn[1] < 0.5f) return 0;
    *gy = o[1] - t; *n = V(nn[0], nn[1], nn[2]); return 1;
}

/* v1095G (user: "a lot of the colliders seem horribly placed"): GF_FOOTIK_SRC=depth (default) reads the floor height as DRAWN
 * -- the renderer's ground probes (rsx_ui_overlay.c gpb_*): 5 vertical columns (heel / ball of each foot, the root), walked in
 * the world-only depth buffer right after the world is drawn. Results come back 2-3 frames later, so the columns lead by the
 * walking velocity x GF_FOOTIK_LEAD (0.05 s). GF_FOOTIK_SRC=coll = the old gf_collision.bin rays. */
extern void gpb_request(const float* cols, int n);
extern int gpb_result(int i, float* y, unsigned* age);
static int s_src = 1; static float s_lead = 0.05f; static float s_cols[6][4]; static int s_have_cols = 0;   /* v1108S: 6th = ahead of the swing foot */
static void col_set(int i, v3 p, float root_y) { s_cols[i][0] = p.x; s_cols[i][1] = p.z; s_cols[i][2] = root_y + 0.35f; s_cols[i][3] = root_y - 0.35f; }
static float s_age_avg = 2.5f;   /* v1108U: probe results arrive this many frames late (measured) */
static int probe_get(int i, float* gy) { float y; unsigned age; const int r = gpb_result(i, &y, &age); if (r == 2 && age < 10 && fok(y)) { *gy = y; if (i == 4) s_age_avg += ((float)age - s_age_avg) * 0.05f; return 1; } return 0; }
static void footik_apply(uint32_t pl, uint32_t anim)
{
    const double now = qpc_s();
    float dt = S.have_prev ? (float)(now - S.t_prev) : 1.0f / 60.0f; if (!(dt > 0.001f)) return; if (dt > 0.1f) dt = 0.1f;
    float Mf[9], Tf[3]; if (!gf_rag_ent_frame(pl, Mf, Tf)) return;
    m3 M; memcpy(M.m, Mf, 36); const v3 T = V(Tf[0], Tf[1], Tf[2]);
    const v3 upm = nrm(mv(&M, V(0, 1, 0)));   /* world up in model space */
    /* gates */
    v3 vel0 = V(0, 0, 0);   /* v1095G */
    float spd = 0; if (S.have_prev) { const v3 dv = sub(T, S.T_prev); vel0 = V(dv.x / dt, 0, dv.z / dt); spd = sqrtf(dv.x * dv.x + dv.z * dv.z) / dt; if (len(dv) > 3.0f) spd = 99.0f; else if (spd > 15.0f) spd = S.spd_s; }   /* v1112K: a run lurch (0.74 m in one frame) is not speed */
    S.spd_s += (spd - S.spd_s) * clampf(dt / 0.15f, 0, 1);
    S.T_prev = T; S.t_prev = now; S.have_prev = 1;
    float wt = 1.0f;
    const float hp = rdf(pl + 0x14C8u);
    if (player_in_car(pl)) { wt = 0.0f; S.g_car++; }
    if (gf_cutscene_active()) { wt = 0.0f; S.g_cs++; }
    if (!(fok(hp) && hp > 0.0f)) { wt = 0.0f; S.g_dead++; }
    if (S.spd_s > s_vmax) S.g_fast++;
    wt *= 1.0f - clampf((S.spd_s - s_vmax) / 1.5f, 0, 1);   /* v1108S: was 5..7 m/s; v1112K: 3.2..4.7 m/s -- off while running */
    float groot = T.y; v3 nroot;
    v3 vel = vel0; if (len(vel) > 8.0f) vel = V(0, 0, 0);
    /* v1108U: lead = the MEASURED probe latency (frames late x frame time), not a fixed 50 ms -- jogging at 5.5 m/s at 170 FPS
     * put every column ~0.2 m too far ahead (past the kerb edge / onto the next step). GF_FOOTIK_LEAD = upper limit. */
    const float lat = clampf(s_age_avg * dt, 0.0f, s_lead);
    const v3 lead = mul(vel, lat);
    if (s_src) { col_set(4, add(T, lead), T.y); s_have_cols = 1; gpb_request(&s_cols[0][0], 6); }   /* v1095G: ask every frame (results come later); v1108S: 6 columns */
    /* v1095G-5 (e6: the close third-person camera often has the feet BELOW the bottom of the screen -> no drawn ground there):
     * the drawn depth wins where it can be seen; elsewhere the collision file, but only where it agrees with the root (the
     * v1094F trust check) -- the badly placed colliders still never pull a foot. coll_ok also lets each foot fall back. */
    int coll_ok = -1;   /* -1 = not asked yet */
    #define COLL_OK() (coll_ok >= 0 ? coll_ok : (coll_ok = (ground_at(T, T.y, &gc_y, &nroot) && fabsf(gc_y - T.y) <= s_trust)))
    float gc_y = T.y;
    int root_ok = s_src ? probe_get(4, &groot) : ground_at(T, T.y, &groot, &nroot);
    int root_drawn = s_src && root_ok;   /* v1108S */
    if (root_drawn && fabsf(groot - T.y) > s_max) { root_ok = 0; root_drawn = 0; }   /* a probe on something else entirely */
    if (s_src) { if (root_ok) S.n_probe++; else if (COLL_OK()) { groot = gc_y; root_ok = 1; S.n_fb++; } }
    /* v1100L (user: the legs "very rarely work"): the user's 4K session had the root column hidden / off screen nearly always
     * ([gpb] flag 2 / 0) and the strict collision check failing -> no-ground -> IK off. The game's movement keeps the root ON the
     * floor, so with nothing seen the root's own height IS the ground; the feet still need their own drawn / trusted ground. */
    if (!root_ok) { groot = T.y; root_ok = 1; S.g_noground++; S.miss_pos = T; S.have_miss = 1; }
    if (T.y - groot > 0.3f && !S.feet_ok_prev) { wt = 0.0f; S.g_air++; }   /* airborne -- v1108T: only if the feet saw no ground either (the root
                                                                              * column beside narrow stairs looked down a drop: "airborne 95") */
    /* v1094F (user screenshot: legs sunk into a flat interior floor, "some of the colliders on the map are fucked"): the game's own
     * movement keeps the root ON the real floor, so the collision file must agree there. If the ground it gives under the root is
     * more than GF_FOOTIK_TRUST (0.08 m) away from the root, gf_collision.bin is wrong / missing here -> foot IK off. */
    else if (!root_drawn && fabsf(groot - T.y) > s_trust) { wt = 0.0f; S.g_bad++; }   /* v1108S: only the collision file must agree; drawn
                                                                                          * stairs under a ramp root differ by up to a step */
    S.w += clampf(wt - S.w, -dt / 0.2f, dt / 0.2f);
    S.n_call++;
    if (S.w < 0.002f) { S.off_s[0] = S.off_s[1] = S.pel_s = 0; return; }
    const uint32_t pal = S.pal;
    if (S.sig_ea && rd32(S.sig_ea) == S.sig_val) { S.n_skip++; return; }   /* not refreshed since our write */
    /* read the pose */
    static float R[MAXS][12];
    for (int s = 0; s < S.nslot; s++) { const uint32_t m = pal + (uint32_t)s * 64u;
        for (int k = 0; k < 3; k++) { R[s][k] = rdf(m + (uint32_t)k * 4); R[s][3 + k] = rdf(m + 16 + (uint32_t)k * 4); R[s][6 + k] = rdf(m + 32 + (uint32_t)k * 4); R[s][9 + k] = rdf(m + 48 + (uint32_t)k * 4); }
        if (!fok(R[s][9]) || !fok(R[s][0])) return; }
    #define SP(b) V(R[S.slot[b]][9], R[S.slot[b]][10], R[S.slot[b]][11])
    const m3 Mt = { { M.m[0], M.m[3], M.m[6], M.m[1], M.m[4], M.m[7], M.m[2], M.m[5], M.m[8] } };
    #define TOW(p) add(T, mv(&Mt, (p)))
    /* ground offset per foot: highest support under heel / ball, relative to the root */
    float off[2]; v3 gn[2]; int have[2]; float plant[2]; int from_coll[2] = { 0, 0 };   /* v1108V */
    for (int leg = 0; leg < 2; leg++) { const float ah0 = dot(SP(leg ? ANR : ANL), upm); plant[leg] = 1.0f - clampf((ah0 - 0.10f) / 0.12f, 0, 1); }
    const int swing = plant[0] < plant[1] ? 0 : 1;   /* v1108S: the less planted foot */
    for (int leg = 0; leg < 2; leg++) {
        const v3 an = SP(leg ? ANR : ANL);
        const v3 to = S.slot[leg ? TOR : TOL] >= 0 ? SP(leg ? TOR : TOL) : an;   /* v1108S: no toe found -> heel only */
        const v3 hw = TOW(an), bw = TOW(add(an, mul(sub(to, an), 0.8f)));
        const v3 lead_l = mul(lead, 1.0f - plant[leg]);   /* v1108S: a planted foot does not move -> no lead (it popped onto the next step) */
        float g1, g2; v3 n1 = V(0, 1, 0), n2 = V(0, 1, 0); int h1, h2;
        if (s_src) {   /* v1095G: drawn ground under heel / ball (columns lead by the walking velocity) */
            col_set(leg * 2, add(hw, lead_l), T.y); col_set(leg * 2 + 1, add(bw, lead_l), T.y);
            if (leg == swing) {   /* v1108S: the ground AHEAD of the swing foot (toe + GF_FOOTIK_AHEAD along the walk / foot) */
                v3 fw = V(vel.x, 0, vel.z); if (len(fw) < 0.3f) { const v3 tw = sub(TOW(to), hw); fw = V(tw.x, 0, tw.z); }
                fw = nrm(fw); col_set(5, add(add(TOW(to), mul(fw, s_ahead)), lead), T.y); }
            h1 = probe_get(leg * 2, &g1); h2 = probe_get(leg * 2 + 1, &g2);
            if ((!h1 || !h2) && COLL_OK()) {   /* v1095G-5: off screen / hidden -> the collision file (it agrees with the root here) */
                if (!h1) { h1 = ground_at(hw, T.y, &g1, &n1); if (h1) { S.n_ffb++; from_coll[leg] = 1; } }
                if (!h2) { h2 = ground_at(bw, T.y, &g2, &n2); if (h2) { S.n_ffb++; from_coll[leg] = 1; } } }
            if (h1 && h2) {   /* surface tilt along the foot from the two heights */
                const v3 dir = V(bw.x - hw.x, g2 - g1, bw.z - hw.z), side = cross(dir, V(0, 1, 0));
                v3 nn = nrm(cross(side, dir)); if (nn.y < 0) nn = mul(nn, -1.0f); if (nn.y > 0.6f) n1 = n2 = nn; }
        } else { h1 = ground_at(hw, T.y, &g1, &n1); h2 = ground_at(bw, T.y, &g2, &n2); from_coll[leg] = 1; }
        {   /* v1100L feet on dead bodies (user): a body under the heel / ball lifts that foot onto it (its top, up to GF_FOOTIK_MAX) */
            extern int gf_rag_body_top(float x, float z, float ylo, float yhi, float* ytop);
            float bt; const float lo = T.y - 0.05f, hi = T.y + s_max;
            if (gf_rag_body_top(hw.x, hw.z, lo, hi, &bt) && (!h1 || bt > g1)) { g1 = bt; n1 = V(0, 1, 0); h1 = 1; S.n_body++; }
            if (gf_rag_body_top(bw.x, bw.z, lo, hi, &bt) && (!h2 || bt > g2)) { g2 = bt; n2 = V(0, 1, 0); h2 = 1; S.n_body++; }
        }
        have[leg] = h1 || h2;
        if (!have[leg]) { off[leg] = 0; gn[leg] = V(0, 1, 0); S.n_miss++; }
        else { const int use2 = h2 && (!h1 || g2 > g1); off[leg] = (use2 ? g2 : g1) - T.y; gn[leg] = use2 ? n2 : n1;
               if (h1 && h2 && fabsf(g1 - g2) < 0.05f) gn[leg] = nrm(add(n1, n2)); }
        if (fabsf(off[leg]) > s_max || off[leg] < -s_down) { off[leg] = 0; have[leg] = 0; }
        /* v1108U FOOT LOCK (user: a foot on the road "gets pushed up in the air"; soak m24: planted feet re-read the ground every
         * frame and picked up the kerb / the next step): when a foot plants (plant >= 0.7) its ground is taken once and held
         * until it lifts (plant < 0.4) or slides > 12 cm; only a swinging foot follows the probes. */
        { const int was = S.lk_on[leg];
          if (!was && plant[leg] >= 0.7f && have[leg]) { S.lk_on[leg] = 1; S.lk_y[leg] = T.y + off[leg]; S.lk_x[leg] = hw.x; S.lk_z[leg] = hw.z; S.lk_t[leg] = now; }
          /* v1108V (user: "hard to get a foot up on the sidewalk"): the probes answer ~5 frames late, so the height taken at
           * plant time was the ground the foot was still swinging over (the road). For GF_FOOTIK_SETTLE (0.12 s) after planting
           * the lock keeps taking the newest reading under the planted heel, then holds. */
          else if (was && have[leg] && now - S.lk_t[leg] < s_settle && hypotf(hw.x - S.lk_x[leg], hw.z - S.lk_z[leg]) <= 0.12f) S.lk_y[leg] = T.y + off[leg];
          else if (was && (plant[leg] < 0.4f || hypotf(hw.x - S.lk_x[leg], hw.z - S.lk_z[leg]) > 0.12f)) S.lk_on[leg] = 0;
          /* v1110W (user screenshot, safehouse: flat floor, right foot held ~16 cm up while standing; log: "under the feet -0.004
           * +0.162", locked 1500 / 10 s, no collision there): the settle window (0.12 s) froze a reading taken while the ~5-frame-late
           * probes still answered for the swing, and a standing player never lifts the foot to release it. Now a lock that the live
           * reading under the planted heel keeps contradicting (> GF_FOOTIK_RELOCK_D 0.04 m for GF_FOOTIK_RELOCK_S 0.5 s) while the
           * player is slow (< 1 m/s) takes the live reading. Brief kerb / step readings while walking still cannot move it. */
          else if (was && have[leg] && S.spd_s < 1.0f) {
              static float rd = -1.0f, rs; if (rd < 0.0f) { const char* e = getenv("GF_FOOTIK_RELOCK_D"); rd = e && atof(e) > 0 ? (float)atof(e) : 0.04f;
                  e = getenv("GF_FOOTIK_RELOCK_S"); rs = e && atof(e) > 0 ? (float)atof(e) : 0.5f; }
              const float live = T.y + off[leg];
              if (fabsf(live - S.lk_y[leg]) > rd) { if (S.lk_dis_t[leg] <= 0.0) S.lk_dis_t[leg] = now;
                  else if (now - S.lk_dis_t[leg] >= rs) { static int nl; if (nl++ < 40) FLOG("[footik] v1110W %s foot lock %+.3f -> live %+.3f m (held against the drawn ground %.2f s)\n",
                          leg ? "R" : "L", S.lk_y[leg] - T.y, live - T.y, now - S.lk_dis_t[leg]);
                      S.lk_y[leg] = live; S.lk_dis_t[leg] = 0.0; S.n_relock++; } }
              else S.lk_dis_t[leg] = 0.0; }
          if (!S.lk_on[leg] || !have[leg]) S.lk_dis_t[leg] = 0.0;
          if (S.lk_on[leg] && was) { const float o = S.lk_y[leg] - T.y;
              if (o <= s_max && o >= -s_down) { off[leg] = o; have[leg] = 1; S.n_lock++; } else S.lk_on[leg] = 0; } }
        /* v1108T foot memory: a PLANTED foot whose ground is hidden now (kerb / step edge in front of it, the v1108T occluder test)
         * keeps the ground it saw at that spot (< 8 cm away, < 0.5 s ago) instead of dropping to the ramp root */
        if (have[leg]) { S.mem_y[leg] = T.y + off[leg]; S.mem_x[leg] = hw.x; S.mem_z[leg] = hw.z; S.mem_t[leg] = now; }
        else if (plant[leg] > 0.5f && now - S.mem_t[leg] < 0.5 && hypotf(hw.x - S.mem_x[leg], hw.z - S.mem_z[leg]) < 0.08f) {
            const float o = S.mem_y[leg] - T.y;
            if (o <= s_max && o >= -s_down) { off[leg] = o; have[leg] = 1; S.n_mem++; } }   /* a gap / ledge (v1094F: > one step down = a hole in the collision): leave that foot alone */
        /* planted = the animation holds this ankle low (within 15 cm of its floor) -- computed above (v1108S) */
        const float ah = dot(an, upm);
        /* v1108S swing clearance: the swinging foot rises to the ground ahead of it (next stair) so its toe clears the edge */
        if (s_src && leg == swing && plant[leg] < 0.6f && S.spd_s > 0.5f) { float ga;   /* v1108S-2: only while moving */
            const float base = have[leg] ? off[leg] : 0.0f;
            if (probe_get(5, &ga) && ga - T.y > base && ga - T.y - base <= 0.25f && ga - T.y <= s_max && ga - T.y > -s_down) {   /* <= one stair step up (m21: furniture) */
                const float k5 = 1.0f - plant[leg] / 0.6f; const float o5 = ga - T.y;
                off[leg] = have[leg] ? off[leg] + (o5 - off[leg]) * k5 : o5 * k5; have[leg] = 1; S.n_ahead++; } }
        if (S.n_act == 0 && leg == 1) FLOG("[footik] first frame: ankle heights above the root L %.3f R %.3f, pelvis %.3f | ground under the root %+.3f, under the feet %+.3f %+.3f (hit %d %d)\n",
                                           dot(SP(ANL), upm), ah, dot(SP(PEL), upm), groot - T.y, off[0], off[1], have[0], have[1]);
    }
    S.feet_ok_prev = have[0] || have[1];   /* v1108T airborne gate */
    if (have[0] && have[1] && from_coll[0] && from_coll[1] && off[0] < -0.18f && off[1] < -0.18f) { off[0] = off[1] = 0; S.g_bad++; }   /* v1108V: collision only (a ramp kerb root rises early) */   /* v1108S: -0.12 -> -0.18 (going down stairs) */   /* v1094F: both feet well below a root that stands on the floor = bad collision, not stairs */
    const float k = clampf(dt / 0.07f, 0, 1);
    for (int leg = 0; leg < 2; leg++) S.off_s[leg] += (off[leg] - S.off_s[leg]) * k;
    float pel = fminf(S.off_s[0], S.off_s[1]); pel = clampf(pel, -s_down, 0.10f);   /* v1094F: the hips drop at most one step */
    S.pel_s += (pel - S.pel_s) * clampf(dt / 0.10f, 0, 1);
    const float w = S.w; const v3 dP = mul(upm, S.pel_s * w);
    /* legs */
    m3 Rth[2], Rsh[2], Rft[2]; v3 hipN[2], kneN[2], ankN[2], hipO[2], kneO[2], ankO[2];
    for (int leg = 0; leg < 2; leg++) {
        hipO[leg] = SP(leg ? HIR : HIL); kneO[leg] = SP(leg ? KNR : KNL); ankO[leg] = SP(leg ? ANR : ANL);
        const float L1 = len(sub(kneO[leg], hipO[leg])), L2 = len(sub(ankO[leg], kneO[leg]));
        hipN[leg] = add(hipO[leg], dP);
        v3 at = add(ankO[leg], mul(upm, S.off_s[leg] * w));
        v3 d = sub(at, hipN[leg]); float D = len(d); const v3 dn = nrm(d);
        const float D0 = len(sub(ankO[leg], hipO[leg])), Dmin = fminf(fabsf(L1 - L2) + 0.01f, D0), Dmax = fmaxf((L1 + L2) * 0.995f, D0);   /* never tighter than the animation's own pose */
        if (D > Dmax) { D = Dmax; at = add(hipN[leg], mul(dn, D)); } if (D < Dmin) { D = Dmin; at = add(hipN[leg], mul(dn, D)); }
        v3 bend = sub(add(kneO[leg], dP), hipN[leg]); bend = sub(bend, mul(dn, dot(bend, dn)));
        if (len(bend) < 1e-3f) bend = sub(kneO[leg], add(hipO[leg], mul(nrm(sub(ankO[leg], hipO[leg])), dot(sub(kneO[leg], hipO[leg]), nrm(sub(ankO[leg], hipO[leg]))))));
        bend = nrm(bend); if (len(bend) < 0.5f) { Rth[leg] = Rsh[leg] = Rft[leg] = ident(); hipN[leg] = hipO[leg]; kneN[leg] = kneO[leg]; ankN[leg] = ankO[leg]; continue; }
        const float ca = clampf((L1 * L1 + D * D - L2 * L2) / (2 * L1 * D), -1, 1), sa = sqrtf(1 - ca * ca);
        kneN[leg] = add(hipN[leg], add(mul(dn, L1 * ca), mul(bend, L1 * sa))); ankN[leg] = at;
        Rth[leg] = rot_onto(sub(kneO[leg], hipO[leg]), sub(kneN[leg], hipN[leg]), 1.2f);
        const v3 shin1 = mv(&Rth[leg], sub(ankO[leg], kneO[leg]));
        const m3 r2 = rot_onto(shin1, sub(ankN[leg], kneN[leg]), 1.2f); Rsh[leg] = mm(&r2, &Rth[leg]);
        /* foot: keeps its animated orientation, tilted to the surface when planted */
        const v3 nm = nrm(mv(&M, gn[leg]));
        Rft[leg] = have[leg] ? rot_onto(upm, add(upm, mul(sub(nm, upm), plant[leg] * w)), 0.44f) : ident();
        /* re-solve the ankle from the actual chain so thigh + shin + foot stay joined */
        kneN[leg] = add(hipN[leg], mv(&Rth[leg], sub(kneO[leg], hipO[leg])));
        ankN[leg] = add(kneN[leg], mv(&Rsh[leg], sub(ankO[leg], kneO[leg])));
    }
    /* write every slot */
    for (int s = 0; s < S.nslot; s++) {
        const int g = S.grp[s]; v3 p = V(R[s][9], R[s][10], R[s][11]); const m3* Q = NULL;
        if (g == G_BODY) p = add(p, dP);
        else { const int leg = (g - 1) / 3, part = (g - 1) % 3 + 1;
            if (part == G_TH) { Q = &Rth[leg]; p = add(hipN[leg], mv(Q, sub(p, hipO[leg]))); }
            else if (part == G_SH) { Q = &Rsh[leg]; p = add(kneN[leg], mv(Q, sub(p, kneO[leg]))); }
            else { Q = &Rft[leg]; p = add(ankN[leg], mv(Q, sub(p, ankO[leg]))); } }
        const uint32_t m = pal + (uint32_t)s * 64u;
        if (Q) for (int r = 0; r < 3; r++) { const v3 ax = mv(Q, V(R[s][r * 3], R[s][r * 3 + 1], R[s][r * 3 + 2]));
            wrf(m + (uint32_t)r * 16, ax.x); wrf(m + (uint32_t)r * 16 + 4, ax.y); wrf(m + (uint32_t)r * 16 + 8, ax.z); }
        wrf(m + 48, p.x); wrf(m + 52, p.y); wrf(m + 56, p.z);
    }
    S.sig_ea = pal + (uint32_t)S.slot[ANL] * 64u + 52u; S.sig_val = rd32(S.sig_ea);
    S.n_act++;
    if (S.pel_s < S.min_pel) S.min_pel = S.pel_s;
    for (int leg = 0; leg < 2; leg++) { if (S.off_s[leg] > S.max_off) S.max_off = S.off_s[leg]; if (S.off_s[leg] < S.min_off) S.min_off = S.off_s[leg]; }
    if (s_dbg) { static double tl = 0; if (now - tl > 0.25) { tl = now;
        FLOG("[footik] w %.2f spd %.1f | off L %+.3f R %+.3f (raw %+.3f %+.3f, ground %d%d) pelvis %+.3f | plant %.1f %.1f\n",
             w, S.spd_s, S.off_s[0], S.off_s[1], off[0], off[1], have[0], have[1], S.pel_s, plant[0], plant[1]); } }
    #undef SP
    #undef TOW
}

void gf_footik_post_skel(uint32_t anim, uint32_t ent)
{
    if (s_on < 0) {
        const char* e = getenv("GF_FOOTIK"); s_on = (e && *e == '0') ? 0 : 1;
        e = getenv("GF_FOOTIK_DBG"); s_dbg = e && *e == '1';
        e = getenv("GF_FOOTIK_MAX"); if (e) s_max = clampf((float)atof(e), 0.05f, 0.8f);
        e = getenv("GF_FOOTIK_SRC"); if (e && (*e == 'c' || *e == '0')) s_src = 0;
        e = getenv("GF_FOOTIK_LEAD"); if (e) s_lead = clampf((float)atof(e), 0.0f, 0.3f);
        e = getenv("GF_FOOTIK_TRUST"); if (e) s_trust = clampf((float)atof(e), 0.02f, 0.5f);
        e = getenv("GF_FOOTIK_DOWN"); if (e) s_down = clampf((float)atof(e), 0.05f, 0.6f);
        e = getenv("GF_FOOTIK_VMAX"); if (e) s_vmax = clampf((float)atof(e), 2.0f, 30.0f);
        e = getenv("GF_FOOTIK_AHEAD"); if (e) s_ahead = clampf((float)atof(e), 0.0f, 0.6f);
        e = getenv("GF_FOOTIK_SETTLE"); if (e) s_settle = clampf((float)atof(e), 0.0f, 0.5f);
        printf("[footik] v1078F player foot IK %s (max %.2f m%s), ground from %s\n", s_on ? "ON" : "OFF (GF_FOOTIK=0)", s_max, s_dbg ? ", debug" : "", s_src ? "the DRAWN depth (v1095G probes)" : "gf_collision.bin"); fflush(stdout);
    }
    if (!s_on || !ent || !ok(anim)) return;
    { extern volatile int g_gf_sitting; if (g_gf_sitting) return; }   /* v1109C seated: no foot IK (gf_sit.c) */
    const uint32_t pl = player_ent(); if (!pl || ent != pl) return;
    __try {
        const uint32_t h = rd32(anim + 0x74); const uint32_t pal = ok(h) ? rd32(h + 8) : 0; if (!ok(pal)) return;
        if (S.ent != pl || S.anim != anim || S.pal != pal) {
            const double now = qpc_s();
            if (S.t_retry > now && S.ent == pl && S.anim == anim && S.pal == pal) return;
            memset(&S, 0, sizeof S); S.ent = pl; S.anim = anim; S.pal = pal; S.min_pel = 0; S.t_stat = now;
            int ns = 0;
            if (!gf_rag_bone_slots(pl, anim, S.slot, &ns) || ns > MAXS || ns <= 0) { S.t_retry = now + 3.0; S.ready = 0; FLOG("[footik] player bones not found (retry in 3 s)\n"); }
            else { S.nslot = ns; fix_toes();   /* v1108S */
                   const int gr = build_groups(); S.ready = gr == 1;
                   if (gr == -1) { S.t_retry = now + 3.0; static int lg = 0; if (!lg) { lg = 1; FLOG("[footik] waiting for the learned player rig (gf_ragdoll v1055B, >= 600 samples)\n"); } }
                   else if (!S.ready) { S.t_retry = now + 3.0; FLOG("[footik] player rig grouping failed (retry in 3 s)\n"); } }
        }
        if (!S.ready) { if (qpc_s() > S.t_retry) S.anim = 0; return; }   /* force a retry */
        footik_apply(pl, anim);
        const double now = qpc_s();
        if (now - S.t_stat > 10.0) {
            FLOG("[footik] 10s: %ld updates, %ld corrected, %ld not refreshed, %ld foot rays missed | pelvis down to %.3f m, foot offsets %+.3f..%+.3f m | off: car %ld cutscene %ld dead %ld fast %ld no-ground %ld airborne %ld bad-collision %ld\n",
                 S.n_call, S.n_act, S.n_skip, S.n_miss, S.min_pel, S.min_off, S.max_off, S.g_car, S.g_cs, S.g_dead, S.g_fast, S.g_noground, S.g_air, S.g_bad);
            if (s_src) FLOG("[footik]   ground from: drawn depth %ld, collision fallback %ld (feet %ld), feet on bodies %ld, swing lifted by the step ahead %ld, hidden ground remembered %ld, locked %ld (re-taken %ld), probe latency %.1f frames\n", S.n_probe, S.n_fb, S.n_ffb, S.n_body, S.n_ahead, S.n_mem, S.n_lock, S.n_relock, s_age_avg);
            S.n_probe = S.n_fb = S.n_ffb = S.n_body = S.n_ahead = S.n_mem = S.n_lock = 0;
            if (S.have_miss) FLOG("[footik]   no collision under the player at world (%.2f, %.2f, %.2f)\n", S.miss_pos.x, S.miss_pos.y, S.miss_pos.z);
            S.t_stat = now; S.n_call = S.n_act = S.n_skip = S.n_miss = 0; S.min_pel = 0; S.min_off = S.max_off = 0;
            S.g_car = S.g_cs = S.g_dead = S.g_fast = S.g_noground = S.g_air = S.g_bad = 0; S.have_miss = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { s_on = 0; printf("[footik] fault -> off\n"); fflush(stdout); }
}
