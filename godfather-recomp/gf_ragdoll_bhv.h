/* gf_ragdoll_bhv.h -- v1075R (Claude): Euphoria / RDR2-style dying behaviours (gf_ragdoll_bhv.c). */
#ifndef GF_RAGDOLL_BHV_H
#define GF_RAGDOLL_BHV_H
#ifdef __cplusplus
extern "C" {
#endif
enum { BHV_Z_HEAD, BHV_Z_CHEST, BHV_Z_GUT, BHV_Z_ARM, BHV_Z_LEG, BHV_Z_OTHER };
enum { BHV_NONE, BHV_STAGGER, BHV_FALL };
enum { BHV_PH_NONE, BHV_PH_STAGGER, BHV_PH_KNEEL, BHV_PH_FALL, BHV_PH_AGONY, BHV_PH_DONE };
#define BHV_MAXF 12
typedef struct {
    int ext;            /* 1 = jr_assist (external: kp, kd, ff), 0 = jr_pull (internal muscle pull, k) */
    int part, pidx;     /* Jolt part, joint index of the point (gf_ragdoll.c particle) */
    float pt[3], tgt[3], kp, kd, ff[3], k;
} bhv_force;
typedef struct {
    int active, phase;
    int use_tone; float tone[3];       /* spine, arms, legs */
    int use_pose; float pose[63];      /* motor target pose (21 joints) */
    int hold_pose;                     /* motor target = the body's current pose (weak hold) */
    int clutch, no_brace;
    float yaw_damp;                    /* jr_yaw_damp per frame (0 = none) */
    float yaw_kp, yaw_err;             /* heading hold: torque about +y = yaw_kp * yaw_err - yaw_kd * spin rate (caller) */              /* hand on the wound allowed / brace suppressed */
    int nf; bhv_force f[BHV_MAXF];
} bhv_out;
typedef struct {
    float p0[63], ground, h0, hc0, Lch, dmg, front[3], lat[3], push[3], dir[3], dist, hunch, t_stag, t_kneel, t_agony;
    float pel_start[3], sw0[3], t_phase, t_swing, t_fall, wound[3];
    float pa[63]; int pa_set; float t_prone;          /* agony: pose snapshot when he hit the ground (targets are built from it, not from the live body) */
    int zone, prog, phase, started, kneel, steps, nstep, swing, last_swing, agony, prone, prone_tries, topple, side, have_wound;
    float yaw_ref, yaw_unw, yaw_prev; int yaw_init;
    unsigned seed;
} bhv_t;
/* p0: 21 joints at death (body frame, +y up); push_dir: bullet direction (NULL = reels back); side: 0 left / 1 right */
void bhv_init(bhv_t* b, const float* p0, float ground, int zone, float dmg, int low_start, const float* push_dir, unsigned seed);
void bhv_set_side(bhv_t* b, int side, const float* wound_local_pt);
void bhv_frame(bhv_t* b, float t, float dt, const float* p, const float* vpel, bhv_out* o);
const char* bhv_phase_name(int ph);
#ifdef __cplusplus
}
#endif
#endif
