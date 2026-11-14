/* gf_ragdoll_bhv.c -- v1075R (Claude): Euphoria / RDR2-style DYING BEHAVIOURS for the death ragdoll.
 *
 * User (2026-11-06 20:40): "way more Euphoria like, as close to RDR2 as possible". Until now a dying NPC lost its muscle tone
 * in 0.1-1.4 s and collapsed; the only "behaviour" was a hand on the wound and a brace. RDR2's look is the fight in between:
 * the man tries to stay up, staggers, clutches the wound, his knees go, he goes down -- and on the ground he is not dead yet.
 *
 * This module only DECIDES; gf_ragdoll.c (game) and tools/ragsim (offline harness) apply its output to the Jolt body:
 *   tone[3]   muscle tone spine / arms / legs (replaces tone_of while the behaviour owns the body)
 *   pose      motor target pose (21 joints, the body's own frame) -- replaces the game's death animation as muscle target
 *   forces    jr_assist (EXTERNAL: balance support, the "invisible hand" Euphoria also uses) or jr_pull (internal muscle pull)
 * Programs (rolled per body from the hit zone, damage and a seed):
 *   STAGGER   1-3 lurching steps with the bullet, hips sag, chest hunches over the wound or reels back, hand on the wound;
 *             then either the legs give out (falls with its momentum) or the knees go first (KNEEL).
 *   KNEEL     drops to the knees, sways there hunched over the wound 0.4-1.6 s, then topples (forward / to the wound side).
 *   AGONY     on the ground, not dead yet (2-6 s): on the back -> knees draw up with the feet on the ground, hand presses the
 *             wound, head lifts, rolls half onto the wound side; face down -> tries to push up / crawl once or twice, fails.
 *             Weakens and stops. None for headshots / heart shots (those are already "lights out" in gf_ragdoll.c).
 * Joint / part numbering = gf_ragdoll.c / gf_ragdoll_jolt.cpp. All positions in the body's frame, +y up, metres. */
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "gf_ragdoll_bhv.h"

enum { PEL, LUM, THO, NEC, HED, SHL, ELL, WRL, SHR, ELR, WRR, HIL, KNL, ANL, HIR, KNR, ANR, TOL, TOR, FIL, FIR, NP };
enum { D_LT, D_UT, D_HEAD, D_UAL, D_LAL, D_UAR, D_LAR, D_ULL, D_LLL, D_ULR, D_LLR, ND };

typedef struct { float x, y, z; } v3;
static v3 V(float x, float y, float z) { v3 r = { x, y, z }; return r; }
static v3 P(const float* p, int i) { return V(p[3 * i], p[3 * i + 1], p[3 * i + 2]); }
static v3 add(v3 a, v3 b) { return V(a.x + b.x, a.y + b.y, a.z + b.z); }
static v3 sub(v3 a, v3 b) { return V(a.x - b.x, a.y - b.y, a.z - b.z); }
static v3 mul(v3 a, float k) { return V(a.x * k, a.y * k, a.z * k); }
static float dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static v3 cross(v3 a, v3 b) { return V(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static float len(v3 a) { return sqrtf(dot(a, a)); }
static v3 nrm(v3 a) { const float l = len(a); return l > 1e-6f ? mul(a, 1.0f / l) : V(0, 0, 0); }
static v3 flat(v3 a) { return V(a.x, 0, a.z); }
static float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static float smooth(float x) { x = clampf(x, 0, 1); return x * x * (3 - 2 * x); }
static float rnd(unsigned* s) { unsigned x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x ? x : 0x1234567u; return (float)(x >> 8) / 16777215.0f; }
static void put(float* p, int i, v3 v) { p[3 * i] = v.x; p[3 * i + 1] = v.y; p[3 * i + 2] = v.z; }
/* rotate point about an axis through c */
static v3 rot(v3 p, v3 c, v3 ax, float a)
{
    const float cs = cosf(a), sn = sinf(a); const v3 d = sub(p, c);
    return add(c, add(add(mul(d, cs), mul(cross(ax, d), sn)), mul(ax, dot(ax, d) * (1 - cs))));
}
static const int k_upper[] = { THO, NEC, HED, SHL, ELL, WRL, SHR, ELR, WRR, FIL, FIR };

/* chest assist without a yaw lever (v1075R: a sideways correction at the hunched chest, forward of the body's centre of
 * mass, twisted the body round at ~60 deg/s): forward/back + height act at the chest, the SIDEWAYS part at a point on the
 * same body straight above the hips. */
static void force(bhv_out* o, int ext, int part, int pidx, v3 pt, v3 tg, float kp, float kd, v3 ff, float k);
static v3 s_com;   /* body centre of mass (approx.), set per frame in bhv_frame */
static void chest_assist(bhv_out* o, v3 tho, v3 pel, v3 ct, v3 front, v3 lat, float kp, float kd, v3 ff)
{
    (void)pel; (void)front; (void)lat;
    /* height at the chest; the HORIZONTAL correction at a point on the vertical through the body's centre of mass:
     * no lever about the vertical -> it can lean / right the body but never spin it (v1075R) */
    force(o, 1, D_UT, -1, tho, V(tho.x, ct.y, tho.z), kp, kd, ff, 0);
    const v3 q = V(s_com.x, tho.y, s_com.z);
    force(o, 1, D_UT, -1, q, V(q.x + ct.x - tho.x, q.y, q.z + ct.z - tho.z), kp, kd, V(0, 0, 0), 0);
}
static void force(bhv_out* o, int ext, int part, int pidx, v3 pt, v3 tg, float kp, float kd, v3 ff, float k)
{
    if (o->nf >= BHV_MAXF) return;
    bhv_force* f = &o->f[o->nf++];
    f->ext = ext; f->part = part; f->pidx = pidx;
    f->pt[0] = pt.x; f->pt[1] = pt.y; f->pt[2] = pt.z; f->tgt[0] = tg.x; f->tgt[1] = tg.y; f->tgt[2] = tg.z;
    f->kp = kp; f->kd = kd; f->ff[0] = ff.x; f->ff[1] = ff.y; f->ff[2] = ff.z; f->k = k;
}

void bhv_init(bhv_t* b, const float* p0, float ground, int zone, float dmg, int low_start, const float* push_dir, unsigned seed)
{
    memset(b, 0, sizeof *b);
    memcpy(b->p0, p0, sizeof b->p0);
    b->ground = ground; b->zone = zone; b->dmg = dmg; { unsigned z = seed + 0x9E3779B9u; z = (z ^ (z >> 16)) * 0x85EBCA6Bu; z = (z ^ (z >> 13)) * 0xC2B2AE35u; b->seed = z ^ (z >> 16); }   /* well mixed: small seeds differ */
    b->h0 = P(p0, PEL).y - ground;
    { const v3 pc = mul(add(add(P(p0, PEL), P(p0, HIL)), P(p0, HIR)), 1.0f / 3.0f), cc = mul(add(add(P(p0, THO), P(p0, NEC)), add(P(p0, SHL), P(p0, SHR))), 0.25f);
      b->Lch = len(sub(cc, pc)); b->hc0 = pc.y - ground; }
    { const v3 lat = nrm(flat(sub(P(p0, HIR), P(p0, HIL)))); const v3 f = cross(V(0, 1, 0), lat);   /* anatomical front */
      b->front[0] = f.x; b->front[1] = 0; b->front[2] = f.z; b->lat[0] = lat.x; b->lat[2] = lat.z; }
    { v3 d = push_dir ? nrm(flat(V(push_dir[0], push_dir[1], push_dir[2]))) : V(0, 0, 0);
      if (len(d) < 0.5f) d = mul(V(b->front[0], 0, b->front[2]), -1.0f);   /* no direction: reels back */
      b->push[0] = d.x; b->push[2] = d.z; }
    b->prog = BHV_NONE;
    if (zone == BHV_Z_HEAD || low_start || b->h0 < 0.7f) { b->prog = BHV_NONE; return; }   /* headshot / kneeling: gf_ragdoll.c as before */
    {   unsigned* s = &b->seed; const float r = rnd(s);
        float p_st = 0.0f, p_kneel = 0.0f;
        switch (zone) {
        case BHV_Z_GUT:   p_st = 0.85f; p_kneel = 0.60f; break;
        case BHV_Z_CHEST: p_st = dmg >= 3000.0f ? 0.35f : 0.75f; p_kneel = 0.30f; break;
        case BHV_Z_ARM:   p_st = 0.70f; p_kneel = 0.20f; break;
        case BHV_Z_LEG:   p_st = 0.45f; p_kneel = 0.35f; break;
        default: p_st = 0.5f; p_kneel = 0.3f; break;
        }
        if (r < p_st) {
            b->prog = BHV_STAGGER;
            b->kneel = rnd(s) < p_kneel;
            const float big = dmg >= 3000.0f ? 1.0f : 0.0f;
            b->t_stag = (zone == BHV_Z_CHEST && big) ? 0.45f + 0.3f * rnd(s) : 0.75f + 0.95f * rnd(s);
            b->steps = b->t_stag < 0.7f ? 1 : 1 + (int)(rnd(s) * 2.6f);
            b->dist = (0.25f + 0.22f * b->steps) * (0.8f + 0.5f * rnd(s)) * (zone == BHV_Z_LEG ? 0.6f : 1.0f);
            /* the lurch: with the bullet, plus a sideways component (towards the wounded side for limb hits) */
            v3 d = V(b->push[0], 0, b->push[2]); const v3 lat = V(b->lat[0], 0, b->lat[2]);
            const float side = (b->zone == BHV_Z_ARM || b->zone == BHV_Z_LEG) ? (b->side ? 0.6f : -0.6f) : (rnd(s) - 0.5f) * 0.8f;
            (void)side; d = nrm(add(d, mul(lat, (rnd(s) - 0.5f) * 0.7f)));
            b->dir[0] = d.x; b->dir[2] = d.z;
            /* posture: gut -> doubled over the wound; chest -> reels back first, then hunches; arm / leg -> leans to the side */
            b->hunch = zone == BHV_Z_GUT ? 0.38f + 0.14f * rnd(s) : zone == BHV_Z_CHEST ? 0.18f + 0.12f * rnd(s) : 0.12f;   /* rad (~20-30 deg gut) */
            b->t_kneel = 0.4f + 1.2f * rnd(s);
        }
        b->agony = (zone == BHV_Z_GUT || zone == BHV_Z_LEG || zone == BHV_Z_ARM || (zone == BHV_Z_CHEST && dmg < 3000.0f)) && rnd(s) < 0.85f;
        b->t_agony = 2.0f + 4.0f * rnd(s);
        b->prone_tries = 1 + (int)(rnd(s) * 1.9f);
        if (b->prog == BHV_NONE && b->agony) b->prog = BHV_FALL;
    }
}
void bhv_set_side(bhv_t* b, int side, const float* wound_local_pt) { b->side = side; if (wound_local_pt) { b->wound[0] = wound_local_pt[0]; b->wound[1] = wound_local_pt[1]; b->wound[2] = wound_local_pt[2]; b->have_wound = 1; } }

/* pose helpers on a copy of the start pose */
static void hunch_pose(float* q, const float* p0, float ang, v3 axis)   /* axis = lat: positive ang = FORWARD (lat x up = back -> use -lat) */
{
    const v3 c = P(p0, LUM); axis = mul(axis, -1.0f);
    for (unsigned i = 0; i < sizeof k_upper / sizeof k_upper[0]; i++) put(q, k_upper[i], rot(P(q, k_upper[i]), c, axis, ang));
}
static void knee_pose(float* q, float bend)   /* bend both knees: the shins rotate back about the knees, hips stay */
{
    const v3 lat = nrm(sub(P(q, HIR), P(q, HIL)));
    for (int s = 0; s < 2; s++) {
        const int kn = s ? KNR : KNL, an = s ? ANR : ANL, to = s ? TOR : TOL, hi = s ? HIR : HIL;
        /* thigh forward by bend/2, shin back by bend */
        const v3 h = P(q, hi);
        put(q, kn, rot(P(q, kn), h, lat, bend * 0.5f)); put(q, an, rot(P(q, an), h, lat, bend * 0.5f)); put(q, to, rot(P(q, to), h, lat, bend * 0.5f));
        const v3 k = P(q, kn);
        put(q, an, rot(P(q, an), k, lat, -bend)); put(q, to, rot(P(q, to), k, lat, -bend));   /* signs checked: lat x down = front */
    }
}

static void kneel_pose(float* q, float thigh, float shin)   /* thighs rotate forward by `thigh`, shins back by `shin` (rad) */
{
    const v3 lat = nrm(sub(P(q, HIR), P(q, HIL)));
    for (int s = 0; s < 2; s++) {
        const int kn = s ? KNR : KNL, an = s ? ANR : ANL, to = s ? TOR : TOL, hi = s ? HIR : HIL;
        const v3 h = P(q, hi);
        put(q, kn, rot(P(q, kn), h, lat, thigh)); put(q, an, rot(P(q, an), h, lat, thigh)); put(q, to, rot(P(q, to), h, lat, thigh));
        const v3 k = P(q, kn);
        put(q, an, rot(P(q, an), k, lat, -shin)); put(q, to, rot(P(q, to), k, lat, -shin));
    }
}
static v3 rot_onto(v3 x, v3 from, v3 to)   /* rotate x by the rotation that takes direction `from` onto `to` */
{
    from = nrm(from); to = nrm(to); const v3 ax = cross(from, to); const float sn = len(ax), cs = dot(from, to);
    if (sn < 1e-5f) return x;
    return rot(x, V(0, 0, 0), mul(ax, 1.0f / sn), atan2f(sn, cs));
}
static void arms_hang(float* q, float fwd_amt)   /* dying: upper arms hang along the torso, elbows soft, forearms a little forward */
{
#ifdef BHV_DEBUG
    if (dbgm() & 128) return;
#endif
    const v3 up = nrm(sub(P(q, NEC), P(q, THO))), lat = nrm(sub(P(q, SHR), P(q, SHL)));
    const v3 front = nrm(cross(up, lat));
    for (int s = 0; s < 2; s++) {
        const int sh = s ? SHR : SHL, el = s ? ELR : ELL, wr = s ? WRR : WRL, fi = s ? FIR : FIL;
        const v3 S = P(q, sh), E = P(q, el), W = P(q, wr), F = P(q, fi);
        const v3 side = mul(lat, s ? 0.18f : -0.18f);
        const v3 du = nrm(add(add(mul(up, -1.0f), side), mul(front, 0.10f + fwd_amt)));
        const v3 df = nrm(add(add(mul(up, -1.0f), mul(front, 0.45f + fwd_amt)), side));
        /* reachable targets only (v1075R: unreachable arm targets kept the muscles pushing -> the body spun on its feet):
         * 1) the whole arm swings RIGIDLY about the shoulder until the upper arm hangs along du (no change at the elbow),
         * 2) the forearm + hand bend about the elbow's OWN hinge axis to a soft flexion (no twist about the forearm) */
        (void)df;
        const v3 u0 = sub(E, S);
        const v3 E2 = add(S, rot_onto(u0, u0, du)), W1 = add(S, rot_onto(sub(W, S), u0, du)), F1 = add(S, rot_onto(sub(F, S), u0, du));
        const v3 ud = nrm(sub(E2, S)), fd = nrm(sub(W1, E2));
        v3 hx = cross(ud, fd); if (len(hx) < 0.15f) hx = cross(ud, front); hx = nrm(hx);
        const float th = acosf(clampf(dot(ud, fd), -1, 1)), want = 0.55f + fwd_amt;   /* elbow flexion, rad */
        const v3 W2 = rot(W1, E2, hx, want - th), F2 = rot(F1, E2, hx, want - th);
        put(q, el, E2); put(q, wr, W2); put(q, fi, F2);
    }
}
static void leg_draw(float* q, int s, float thigh, float shin)   /* one leg: thigh forward (knee toward the chest), shin back */
{
    const v3 lat = nrm(sub(P(q, HIR), P(q, HIL)));
    const int kn = s ? KNR : KNL, an = s ? ANR : ANL, to = s ? TOR : TOL, hi = s ? HIR : HIL;
    const v3 h = P(q, hi);
    put(q, kn, rot(P(q, kn), h, lat, thigh)); put(q, an, rot(P(q, an), h, lat, thigh)); put(q, to, rot(P(q, to), h, lat, thigh));
    const v3 k = P(q, kn);
    put(q, an, rot(P(q, an), k, lat, -shin)); put(q, to, rot(P(q, to), k, lat, -shin));
}
static void head_flex(float* q, float ang)   /* chin toward the chest: head about the neck, forward */
{
    const v3 lat = nrm(sub(P(q, SHR), P(q, SHL)));
    put(q, HED, rot(P(q, HED), P(q, NEC), mul(lat, -1.0f), ang));
}
static void neck_flex(float* q, float ang)   /* neck + head about the top of the chest */
{
    const v3 lat = nrm(sub(P(q, SHR), P(q, SHL))), c = P(q, THO);
    put(q, NEC, rot(P(q, NEC), c, mul(lat, -1.0f), ang)); put(q, HED, rot(P(q, HED), c, mul(lat, -1.0f), ang));
}
const char* bhv_phase_name(int ph) { static const char* n[] = { "none", "stagger", "kneel", "fall", "agony", "done" }; return ph >= 0 && ph <= 5 ? n[ph] : "?"; }

void bhv_frame(bhv_t* b, float t, float dt, const float* p, const float* vpel, bhv_out* o)
{
    memset(o, 0, sizeof *o);
    if (b->prog == BHV_NONE || b->phase == BHV_PH_DONE) { o->phase = b->phase; return; }
    if (!b->started) { b->started = 1; b->phase = b->prog == BHV_STAGGER ? BHV_PH_STAGGER : BHV_PH_FALL; b->t_phase = 0; b->pel_start[0] = p[0]; b->pel_start[2] = p[2];
        b->swing = -1; b->last_swing = -1; b->t_fall = b->phase == BHV_PH_FALL ? 0.0f : -1.0f; }
    const float g = b->ground;
    /* assists act near each part's centre of mass (v1075R: at the spine joint the lever twisted the body into a spin) */
    const v3 pel = mul(add(add(P(p, PEL), P(p, HIL)), P(p, HIR)), 1.0f / 3.0f);
    const v3 tho = mul(add(add(P(p, THO), P(p, NEC)), add(P(p, SHL), P(p, SHR))), 0.25f);
    { v3 m = V(0, 0, 0); for (int i = 0; i < NP; i++) m = add(m, P(p, i)); m = mul(m, 1.0f / NP);
      s_com = add(mul(add(pel, tho), 0.35f), mul(m, 0.30f)); }
    const v3 dir = V(b->dir[0], 0, b->dir[2]);
    /* the body's CURRENT facing (it turns while it staggers): front = up x (left hip -> right hip) */
    /* the body's CURRENT facing (it turns while it staggers), from hips AND shoulders: front = up x (left -> right) */
    const v3 lat = nrm(flat(add(sub(P(p, HIR), P(p, HIL)), sub(P(p, SHR), P(p, SHL))))), front = cross(V(0, 1, 0), lat);
    const v3 lat0 = V(b->lat[0], 0, b->lat[2]);   /* pose targets are built on the START pose -> its own axis (v1075R spin fix) */
    const float M = 73.0f, G = 9.81f;
    o->active = 1; o->phase = b->phase;
    /* HEADING HOLD (v1075R): while the behaviour holds the body up, a pure torque about +y steers the hips' facing back to
     * a reference that may drift slowly (<= 35 deg/s) -- he can turn as he staggers, he cannot pirouette. The caller
     * applies o->yaw_tq split over pelvis / chest (jr_torque) with the measured spin rate (jr_yaw_rate) as damping. */
    {   const v3 lh = flat(sub(P(p, HIR), P(p, HIL)));
        float yw = atan2f(lh.x, lh.z);
        if (!b->yaw_init) { b->yaw_init = 1; b->yaw_ref = yw; b->yaw_unw = yw; b->yaw_prev = yw; }
        { float d = yw - b->yaw_prev; while (d > 3.14159f) d -= 6.28318f; while (d < -3.14159f) d += 6.28318f; b->yaw_unw += d; b->yaw_prev = yw; }
        float err = b->yaw_ref - b->yaw_unw;
        const float drift = 0.61f * dt;   /* 35 deg/s */
        if (err > drift) b->yaw_ref -= drift; else if (err < -drift) b->yaw_ref += drift; else b->yaw_ref = b->yaw_unw;
        err = b->yaw_ref - b->yaw_unw;
        o->yaw_kp = (b->phase == BHV_PH_STAGGER || b->phase == BHV_PH_KNEEL) ? 1700.0f : 0.0f;   /* N m / rad */
        o->yaw_err = err; }
    float tp = t - b->t_phase;

    if (b->phase == BHV_PH_STAGGER) {
        const float u = clampf(tp / b->t_stag, 0, 1);
        /* --- support: the legs carry most of the weight, sagging as the strength goes */
        const float sag = 0.04f + 0.10f * u * u + (b->kneel ? 0.05f * u : 0.0f);
        const float hy = g + b->hc0 * (1.0f - sag);
        const float sup = 0.92f - 0.25f * u;
        force(o, 1, D_LT, -1, pel, V(pel.x, hy, pel.z), 5200.0f, 650.0f, V(0, sup * M * G, 0), 0);
        /* --- lurch path: eased, with a wobble across it */
        const float e = 1.0f - powf(1.0f - u, 1.6f);
        const float wob = sinf(tp * 7.0f) * 0.05f * (1.0f - u);
        const v3 tgt = add(V(b->pel_start[0], 0, b->pel_start[2]), add(mul(dir, b->dist * e), mul(lat, wob)));
        { const v3 q = V(s_com.x, pel.y, s_com.z); force(o, 1, D_LT, -1, q, V(q.x + tgt.x - pel.x, q.y, q.z + tgt.z - pel.z), 2300.0f, 420.0f, V(0, 0, 0), 0); }
        /* --- chest: held at its height over the hips (3D: keeps the torso from pitching / rolling over), leaning as the
         * posture wants -- reels back early for chest hits, then hunches over the wound */
        {   const float reel = (b->zone == BHV_Z_CHEST && tp < 0.35f) ? -0.22f * (1.0f - tp / 0.35f) : 0.0f;
            const float lean = b->hunch * smooth(tp / 0.4f) + reel;                       /* rad, + = forward */
            const float L = b->Lch * (1.0f - 0.05f * u);                                  /* pelvis -> chest */
            const v3 ct = add(V(pel.x, pel.y, pel.z), add(mul(front, L * sinf(lean)), V(0, L * cosf(lean), 0)));
            chest_assist(o, tho, pel, ct, front, lat, 1700.0f, 220.0f, V(0, 0.14f * M * G * (1.0f - 0.5f * u), 0)); }
        /* --- steps (capture point, v1075R): when the body's centre of mass runs away from the feet, the foot on the side it
         * is falling to -- or the trailing one -- swings to where the body will be (CoM + velocity x 0.25 s), each foot
         * staying on ITS side of the body. (Stepping along the fixed lurch direction crossed the legs once the body had
         * turned a little, which turned it more: it walked in circles.) */
        {   const v3 fL = P(p, ANL), fR = P(p, ANR), mid = mul(add(fL, fR), 0.5f);
            const v3 vp = vpel ? flat(V(vpel[0], vpel[1], vpel[2])) : V(0, 0, 0);
            const v3 cap = add(flat(s_com), mul(vp, 0.25f));
            const v3 off = sub(cap, flat(mid));
            if (b->swing < 0 && b->nstep < b->steps + 2 && len(off) > 0.13f && tp > 0.06f) {
                const float sl = dot(off, lat);   /* falling to the right -> right foot, to the left -> left foot */
                int sw = fabsf(sl) > 0.06f ? (sl > 0 ? 1 : 0) : (dot(sub(flat(fL), cap), off) < dot(sub(flat(fR), cap), off) ? 0 : 1);
                if (sw == b->last_swing && fabsf(sl) < 0.10f) sw ^= 1;   /* alternate unless clearly falling to one side */
                b->swing = sw; b->last_swing = sw; b->t_swing = tp; b->nstep++;
                const v3 fs = sw ? fR : fL; b->sw0[0] = fs.x; b->sw0[1] = fs.y; b->sw0[2] = fs.z;
            }
            if (b->swing >= 0) {
                const float su = clampf((tp - b->t_swing) / 0.28f, 0, 1);
                v3 land = add(cap, mul(lat, b->swing ? 0.11f : -0.11f)); land.y = g + 0.09f;
                const v3 s0 = V(b->sw0[0], b->sw0[1], b->sw0[2]);
                v3 at = add(mul(s0, 1.0f - su), mul(land, su)); at.y = g + 0.09f + 0.15f * sinf(su * 3.14159f) * (1.0f - 0.5f * u);
                force(o, 0, b->swing ? D_LLR : D_LLL, b->swing ? ANR : ANL, P(p, b->swing ? ANR : ANL), at, 0, 0, V(0, 0, 0), 2.2f);
                if (su >= 1.0f) b->swing = -1;
            } }
        /* --- muscles: standing, hunched; knees soften */
        {   float* q = o->pose; memcpy(q, b->p0, sizeof b->p0);
            hunch_pose(q, b->p0, b->hunch * smooth(tp / 0.4f) * 0.9f, lat0);
            knee_pose(q, 0.15f + 0.35f * u);
            arms_hang(q, 0.15f);
            /* poses are relative per part: the start pose's absolute position does not matter */
            o->use_pose = 1; }
        o->tone[0] = 0.75f; o->tone[1] = 0.45f; o->tone[2] = 0.80f - 0.3f * u; o->use_tone = 1;
        o->yaw_damp = 0.10f;
        o->clutch = 1; o->no_brace = 1;
        if (u >= 1.0f) { b->phase = b->kneel ? BHV_PH_KNEEL : BHV_PH_FALL; b->t_phase = t; b->t_fall = b->kneel ? -1.0f : t;
            if (!b->kneel) b->topple = 1; }
        return;
    }
    if (b->phase == BHV_PH_KNEEL) {
        /* knees go: the hips come down to kneeling height (thighs about vertical, shins flat behind), the chest stays up over
         * the hips, hunched over the wound, swaying -- then the last strength goes and he topples */
        const float down = smooth(tp / 0.40f);
        const float hk = g + 0.52f;   /* kneeling hips */
        const float hy = (g + b->hc0 * 0.86f) * (1.0f - down) + hk * down;
        if (tp < 0.40f + b->t_kneel) {
            force(o, 1, D_LT, -1, pel, V(pel.x, hy, pel.z), 4500.0f, 650.0f, V(0, 0.80f * M * G, 0), 0);
            const v3 sw = mul(lat, sinf(tp * 3.1f) * 0.04f);
            const float lean = 0.10f + 0.12f * b->hunch;   /* m in front of the hips */
            const v3 ct = add(add(V(pel.x, 0, pel.z), mul(front, lean)), sw);
            const float lk = 0.30f + 0.25f * b->hunch;   /* rad forward */
            const v3 ck = add(add(V(pel.x, pel.y, pel.z), add(mul(front, b->Lch * sinf(lk)), V(0, b->Lch * cosf(lk), 0))), sw);
            chest_assist(o, tho, pel, ck, front, lat, 2400.0f, 260.0f, V(0, 0.25f * M * G, 0)); (void)ct; (void)lean;
            o->tone[0] = 0.65f; o->tone[1] = 0.40f; o->tone[2] = 0.25f; o->use_tone = 1;
            { float* q = o->pose; memcpy(q, b->p0, sizeof b->p0); hunch_pose(q, b->p0, 0.35f + 0.3f * b->hunch, lat0);
              kneel_pose(q, 0.10f * down, 1.30f * down); arms_hang(q, 0.25f); o->use_pose = 1; }
            /* the knees go down to the floor under the hips (a kneel, not a squat) */
            for (int sd = 0; sd < 2 && down > 0.2f; sd++) { const int kn = sd ? KNR : KNL, hi = sd ? HIR : HIL;
                const v3 kp_ = P(p, kn); (void)hi;
                force(o, 0, sd ? D_ULR : D_ULL, kn, kp_, V(kp_.x, g + 0.07f, kp_.z), 0, 0, V(0, 0, 0), 1.2f * down); }   /* straight down: no twist */
            o->yaw_damp = 0.35f;   /* a kneeling man does not turn */
            o->clutch = 1; o->no_brace = 1;
            return;
        }
        b->phase = BHV_PH_FALL; b->t_phase = t; b->t_fall = t; b->topple = 1; tp = 0;
    }
    if (b->phase == BHV_PH_FALL) {
        if (b->t_fall < 0) b->t_fall = t;
        const float tf = t - b->t_fall;
        if (b->topple && tf < 0.25f) {   /* the last of the strength tips him over: forward over a gut wound, else with the momentum */
            const v3 d = b->zone == BHV_Z_GUT || b->hunch > 0.5f ? front : add(mul(dir, 0.7f), mul(front, -0.3f));
            force(o, 1, D_UT, -1, tho, add(tho, mul(nrm(flat(d)), 0.3f)), 300.0f, 0.0f, V(0, 0, 0), 0);
        }
        o->tone[0] = 0.45f * expf(-tf / 0.5f); o->tone[1] = 0.35f * expf(-tf / 0.4f); o->tone[2] = 0.05f; o->use_tone = 1;
        o->clutch = 1;   /* the brace stays allowed (gf_ragdoll.c) */
        const float vp = sqrtf(vpel[0] * vpel[0] + vpel[1] * vpel[1] + vpel[2] * vpel[2]);
        const int lying = pel.y < g + 0.35f && tho.y < g + 0.38f && P(p, HED).y < g + 0.45f;
        if ((lying && vp < 1.2f && tf > 0.6f) || tf > 3.0f) {
            /* agony only from a body that is really down and has stopped (v1075R2: slumped/sitting bodies thrashed) */
            b->t_phase = t; b->phase = b->agony && lying ? BHV_PH_AGONY : BHV_PH_DONE;
            const v3 up_ch = nrm(cross(sub(P(p, SHR), P(p, SHL)), sub(P(p, NEC), P(p, THO))));   /* chest normal: front */
            b->prone = up_ch.y < -0.3f;
        }
        return;
    }
    if (b->phase == BHV_PH_AGONY) {
        const float ta = tp, T = b->t_agony;
        const float life = ta < 0.3f ? ta / 0.3f : ta > T - 1.5f ? clampf((T - ta) / 1.5f, 0, 1) : 1.0f;
        if (ta >= T) { b->phase = BHV_PH_DONE; o->active = 0; o->phase = b->phase; return; }
        if (!b->pa_set) { memcpy(b->pa, p, sizeof b->pa); b->pa_set = 1; }
        /* live orientation: chest normal (front) y. +1 on the back, 0 on the side, -1 face down. Rolling onto the face mid-agony
         * switches him to the face-down program for good (v1075R2: drawn-up legs on a body that rolled prone = scorpion) */
        const float ny = nrm(cross(sub(P(p, SHR), P(p, SHL)), sub(P(p, NEC), P(p, THO)))).y;
        if (!b->prone && ny < -0.55f) { b->prone = 1; b->t_prone = ta; }
        o->clutch = !b->prone; o->no_brace = 1;
        {   /* a writhing body skates on Jolt's low-friction contacts: soft horizontal anchor at the pelvis to where he landed */
            const v3 a0 = P(b->pa, PEL);
            force(o, 1, D_LT, PEL, pel, V(a0.x, pel.y, a0.z), 260.0f * life, 60.0f, V(0, 0, 0), 0); }
        if (!b->prone) {
            const float up_w = smooth((ny + 0.35f) / 0.5f);   /* legs/head work fade as he tips toward his front */
            /* on the back (v1075R2): the MUSCLES do it, Euphoria-style -- motor targets built from the pose he landed in:
             * the wounded side's knee draws up (foot sliding on the ground), the other follows later, the head and neck curl
             * up off the ground in waves, all fading as he weakens. External forces only nudge (shoulder roll). */
            float* q = o->pose; memcpy(q, b->pa, sizeof b->pa);
            for (int s = 0; s < 2; s++) {
                const float lag = s == b->side ? 0.0f : 1.4f, tt = ta - lag;
                if (tt <= 0) continue;
                const float br = (0.55f + 0.45f * (0.5f - 0.5f * cosf(tt * 2.0f * 3.14159f / 2.8f))) * smooth(tt / 0.9f) * life * up_w;
                const float th = (s == b->side ? 1.05f : 0.75f) * br;
                leg_draw(q, s, th, 2.0f * th);   /* shin back twice the thigh: the foot stays down by the bum */
            }
            {   const float ph = ta < 0.6f ? 9.0f : fmodf(ta - 0.6f, 2.4f);
                const float k = ph < 1.4f ? sinf(ph / 1.4f * 3.14159f) : 0.0f;
                neck_flex(q, (0.25f + 0.55f * k) * life); head_flex(q, (0.15f + 0.35f * k) * life);
                if (k > 0.05f) { const v3 hd = P(p, HED);   /* help lift the head's weight a little */
                    force(o, 1, D_HEAD, HED, hd, V(hd.x, g + 0.25f * k + 0.08f, hd.z), 300.0f * life, 30.0f, V(0, 45.0f * life * k, 0), 0); }
            }
            arms_hang(q, 0.35f);
            o->use_pose = 1; o->hold_pose = 0;
            o->tone[0] = 0.45f * life; o->tone[1] = 0.25f * life; o->tone[2] = 0.55f * life; o->use_tone = 1;
            if (ny > 0.6f) { const float rl = (sinf(ta * 0.9f) * 0.5f + 0.5f) * smooth((ny - 0.6f) / 0.3f); const int sh = b->side ? SHL : SHR;   /* on the back only: roll a little toward the wound, never over */
              const v3 s = P(p, sh); const v3 across = mul(nrm(flat(sub(P(p, b->side ? SHR : SHL), s))), 0.12f * rl);
              force(o, 1, D_UT, sh, s, add(s, add(across, V(0, 0.06f * rl, 0))), 650.0f * life, 60.0f, V(0, 0, 0), 0); }
        } else {
            o->tone[0] = 0.18f * life; o->tone[1] = 0.14f * life; o->tone[2] = 0.10f * life; o->use_tone = 1; o->use_pose = 0; o->hold_pose = 1;
            /* face down: tries to push himself up -- hands under the shoulders, chest lifts a little, gives up */
            const float tq = ta - b->t_prone; const float per = 2.4f; const int k = tq < 0 ? 99 : (int)(tq / per);
            if (k < b->prone_tries) {
                const float u = fmodf(tq, per) / per, lift = sinf(clampf(u, 0, 1) * 3.14159f);
                for (int s = 0; s < 2; s++) {
                    const int sh = s ? SHR : SHL, wr = s ? WRR : WRL;
                    v3 tg = P(p, sh); tg.y = g + 0.04f; tg = add(tg, mul(front, -0.05f));
                    force(o, 0, s ? D_LAR : D_LAL, wr, P(p, wr), tg, 0, 0, V(0, 0, 0), 0.8f * life);
                }
                force(o, 1, D_UT, -1, tho, V(tho.x, g + 0.16f + 0.14f * lift, tho.z), 900.0f * life * lift, 90.0f, V(0, 0.30f * M * G * lift * life, 0), 0);   /* absolute height: chest off the ground, never up to sitting */
                if (u > 0.35f && u < 0.75f) { const v3 hd = P(p, HED); force(o, 1, D_HEAD, HED, hd, add(hd, V(0, 0.10f, 0)), 250.0f, 25.0f, V(0, 20.0f, 0), 0); }
            }
        }
        (void)dt;
        return;
    }
    o->active = 0;
}
