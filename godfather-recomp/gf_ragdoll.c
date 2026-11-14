/* gf_ragdoll.c -- v1054U first ragdoll ("Euphoria vibe" groundwork). NOTES_ragdoll_RE.md has the RE.
 *
 * Pose (found v1054R, func_00461478 -> func_003E78D4):  anim = *(entity+0xC4);  slot = byte[*(anim+0x78) + bone];
 *   palette = *(*(anim+0x74) + 8);  bone matrix = palette + slot*64 (RwMatrix rows right/up/at/pos, MODEL space).
 *   Bone count: skel = *(*(anim+4)+0x1C) + (s16)*(anim+0xA)*20, n = *(skel+4)  (func_003BE788 / func_003E7808).
 * Per-frame hook: func_003CC128(anim, dt) = skeleton update, only caller func_0046316C (entity anim update, also used
 *   by corpses). ppu_recomp_001.cpp renames it func_003CC128_orig; the wrapper calls gf_rag_post_skel(anim, dt) after.
 * Death: PainStateMachine action 29 (death_init, hook on func_001A4E1C). sm+0x4C entity, sm+0x60 shooter pos (world),
 *   sm+0x6C hit point (world), sm+0x78 damage. Bone indices via the entity's findBone (vtable +0x94, func_00461150).
 *
 * Ragdoll: 17 particles (pelvis, lumbar, thoracic, neck, head tip, shoulders/elbows/wrists, hips/knees/ankles),
 *   position-based dynamics: gravity, distance constraints (bones + torso braces), knee/elbow bend-direction limits,
 *   ground plane at the feet, friction. Start velocity = the animation's own motion over the last frame + a push from
 *   the shot (direction shooter -> hit point, scaled by damage) on the particle nearest the hit. Every palette slot
 *   follows one of 11 rigid drivers (lower/upper torso, head, upper/lower arms, thighs, shins) chosen by distance.
 *   The game's death animation keeps running underneath (the state machines advance normally); only the pose drawn
 *   is replaced. Settled bodies keep their pose. Safety: stops writing when the entity / palette changes or the
 *   entity has health again (reused); max GF_RAG_MAX (8) bodies, oldest settled one replaced.
 * v1054T: follows the game's corpse (func_002FC3BC: corpse+0x10C = npc+0x38) via the func_0046316C wrapper (ppu_recomp_002.cpp).
 * v1054U ("RDR2, brutal"): muscle tone -- the game's death animation (still running underneath) drives the limbs relative
 *   to the torso with a strength that fades (body shot: spine ~0.75 s, arms ~0.55 s, legs hold 0.12 s then give out;
 *   headshot: lights out); sharp local jolt at the hit; heavy friction, no bounce; stiff spine/neck (min distances);
 *   clothes / in-between bones blend between the two nearest drivers; real time between updates as dt.
 * Env: GF_RAG_TONE (1.0) muscle-tone scale, GF_RAG=0 all off, GF_RAG_PHYS=0 log only (no pose writes), GF_RAG_PUSH (1.0) push scale, GF_RAG_GRAV (9.81).
 * Log tag [rag] (play-fast.out.log).
 * v1069H ("as close to Euphoria as possible -- NPCs actually react to shots"):
 *   LIVE HIT REACTIONS: every non-lethal hit (PainStateMachine's damage record sm+0x54.., watched from its action AND
 *   event hooks -- a change of hit point / damage = a new bullet) gives that NPC a short powered ragdoll in model space
 *   (gf_ragdoll_jolt.cpp jr_hr_*): pelvis follows the game's animation (kinematically, swayed about the feet by the
 *   shot), every other part is a Jolt body driven to the animation by joint motors; the bullet's impulse lands on the
 *   part it hit, that part and its neighbours go slack for an instant and regain tone over ~0.5 s, then the physics
 *   offset blends out. The game's own pain animation keeps playing underneath -> anim + physics. More hits stack.
 *   Log tag [rag-hit]. GF_RAG_HIT=0 off, GF_RAG_HIT_SCALE (1.0) strength, GF_RAG_HIT_MAX (6) bodies at once.
 *   DYING BODIES: a further hit on a ragdolling NPC (the game still runs its pain machine while it dies) pushes the
 *   body at the hit point (wakes a settled one).
 *   BRACE: a body falling forward throws its free hand(s) out to the ground (GF_RAG_BRACE=0 off).
 *   JOINT FIX (gf_ragdoll_jolt.cpp): knee / elbow hinges and hip cones were mirrored for every death so far.
 * v1069J (user: "weird ragdoll thing with their limbs but go into their own animation"): the reaction is now ADDITIVE --
 *   the Jolt body simulates only the bullet's push on the pose frozen at the hit, and each joint's deviation from that
 *   pose is added on top of the live animation (the game's pain / stagger animation plays exactly as authored, the limbs
 *   never lag behind it). Gut / hip hits hunch the upper body forward over the wound.
 * v1069S leg / foot hits on a standing NPC: STUMBLE -- the NPC really moves 0.35-0.8 m towards the wounded side / with the
 *   bullet (vt+0x30 + vt+0x24 from the pain hooks, wall check on the level collision), the shot leg buckles and the hips
 *   drop, the other leg takes a catch step, arms go out, chest leans into it (~0.95 s). GF_RAG_STUMBLE=0 off.
 * v1069U (user: too subtle / quick; knee shots mostly shouldn't stumble): outcome rolled by zone (knee 15 %, thigh 30 % +
 *   dmg/3000, shin 35 % + dmg/4000, foot 55 % short hop, +25 % after a recent leg hit): buckle in place (0.85 s), one-step
 *   stumble (1.1 s, 0.45-0.85 m) or two steps (1.65 s, 0.8-1.4 m: catch step, then the shot leg limps and gives way again).
 * v1069V (user: dead NPCs stumble / move around): the pain machine sees a bullet BEFORE its damage is taken off the health,
 *   so a killing leg shot looked survivable and the dying NPC was carried off. Stumbles now start 90 ms later and only if
 *   the NPC is alive; any NPC at hp <= 0 stops moving and its hit reaction ends; bullets on a body push it less.
 * v1069C CAR HITS (user: NPCs just die instantly when driven into): a death within 4.5 m of the player's moving car (> 2 m/s)
 *   starts the ragdoll with the car's momentum (legs 100 %, hips 75 % + up, upper body 22 % -> folds onto the hood) and the
 *   car joins the body's Jolt world as a kinematic two-box hull that follows the real car every frame (6 s / 12 m):
 *   slow = knocked down and shoved, fast = legs swept, rolls onto the hood and is thrown. GF_RAG_CAR=0 off. Log [rag-car].
 * v1069F (user: many NPCs play their death animation instead of ragdolling): only 8 bodies were kept in all, so in a big
 *   fight every 9th death released the oldest body back to the game's dead pose. Settled bodies no longer count against
 *   GF_RAG_MAX (now 10 MOVING bodies); beyond the 6 newest settled ones they are frozen (Jolt world freed, final pose kept
 *   being written); 48 slots in all before the oldest is let go.
 *   Blood: non-lethal hits spray a smaller splat on the wall / ground behind (GF_BLOOD=0 off). */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "recompiled/ppu_recomp.h"

extern uint8_t* vm_base;
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
extern void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));

typedef void (*gfn)(ppu_context*);
/* v1056A: rigid-body core (gf_ragdoll_jolt.cpp, Jolt Physics); GF_RAG_ENGINE=pbd = the old particle solver */
void* jr_create(const float* p0, const float* pv, float dt, float ground, float kn_sign, float el_sign);
void jr_tone(void* h, const float* anim, float t_spine, float t_arms, float t_legs, int shot_leg);
void jr_pull(void* h, int part, const float* point_now, const float* target, float k);
void jr_step(void* h, float dt);
int jr_get(void* h, float* Q, float* X, float* p);
/* v1075R: Euphoria-style dying behaviours (gf_ragdoll_bhv.c) drive the Jolt body through these */
void jr_assist(void* h, int part, const float* point_now, const float* target, float kp, float kd, const float* ff);
void jr_part_vel(void* h, int part, const float* point, float* vout);
void jr_yaw_damp(void* h, float k);
void jr_torque(void* h, int part, const float* tq);
float jr_yaw_rate(void* h, int part);
#include "gf_ragdoll_bhv.h"
void jr_destroy(void* h);
void jr_coll_pair(void* h, int a, int b, int on);   /* v1091F */
void jr_add_mesh(void* h, const float* tri, int n, float drop_floor);   /* v1057C level collision */
/* v1069H live hit reactions + bullets on ragdolls */
void* jr_hr_create(const float* p0, float ground);
void jr_hr_target(void* h, const float* anim, const float* tone11, const float* sway, const float* pivot);
void jr_hr_step(void* h, float dt);
int jr_hr_get(void* h, float* C, float* Xp, float* Xa, float* p, float* maxdeg);
int jr_impulse(void* h, int part, const float* point, const float* J);
void jr_hr_extra(void* h, const float* ex11, const float* root_off);   /* v1069S */
void jr_hr_base(void* h, const float* base21);   /* v1069T */
void jr_car_add(void* h, const float* rot9, const float* pos, float oy);   /* v1069C */
void jr_car_move(void* h, const float* rot9, const float* pos);
void jr_car_remove(void* h);
enum { TOC = 0x00B4BF60u, NBN = 21, NP = 21, NBODY = 17, MAXR = 48, MAXS = 128, ND = 11 };   /* 17 body joints + toe / finger tips */

/* ------------------------------------------------------------------ guest memory */
static int ok(uint32_t ea) { return ea >= 0x10000u && ea < 0xF0000000u; }
static uint32_t rd32(uint32_t ea) { const uint8_t* p = vm_base + ea; return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint16_t rd16(uint32_t ea) { const uint8_t* p = vm_base + ea; return (uint16_t)((p[0] << 8) | p[1]); }
static void wr32(uint32_t ea, uint32_t v) { uint8_t* p = vm_base + ea; p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static float rdf(uint32_t ea) { uint32_t u = rd32(ea); float f; memcpy(&f, &u, 4); return f; }
static void wrf(uint32_t ea, float f) { uint32_t u; memcpy(&u, &f, 4); wr32(ea, u); }
static int fok(float f) { return f == f && f > -1e6f && f < 1e6f; }
static double now_ms(void) { return (double)GetTickCount64(); }

static gfn lookup(uint32_t addr)
{
    for (uint64_t k = 0; k < function_table_count; k++) if ((uint32_t)function_table[k].addr == addr) return function_table[k].func;
    return 0;
}
static uint64_t gcall_raw(gfn f, uint64_t a0, uint64_t a1, uint64_t a2)
{
    ppu_context* cur = g_active_ctx;
    static ppu_context c;   /* callers hold s_cs; calls do not nest */
    memcpy(&c, cur, sizeof c);
    c.gpr[1] = ((uint32_t)cur->gpr[1] - 0x1000u) & ~0xFull;
    c.gpr[2] = TOC; c.gpr[3] = a0; c.gpr[4] = a1; c.gpr[5] = a2; c.lr = 0;
    g_active_ctx = &c;
    f(&c);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&c); }
    g_active_ctx = cur;
    return c.gpr[3];
}
static int gcall(gfn f, uint64_t a0, uint64_t a1, uint64_t a2)
{
    ppu_context* cur = g_active_ctx;
    if (!cur || !f) return 0;
    __try { gcall_raw(f, a0, a1, a2); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_active_ctx = cur; g_trampoline_fn = 0; return 0; }
}
static uint32_t vfunc(uint32_t obj, uint32_t slot)
{
    if (!ok(obj)) return 0;
    const uint32_t vt = rd32(obj); if (!ok(vt)) return 0;
    const uint32_t opd = rd32(vt + slot); if (!ok(opd)) return 0;
    return rd32(opd);
}

/* ------------------------------------------------------------------ small vector math */
typedef struct { float x, y, z; } v3;
static v3 V(float x, float y, float z) { v3 r = { x, y, z }; return r; }
static v3 add(v3 a, v3 b) { return V(a.x + b.x, a.y + b.y, a.z + b.z); }
static v3 sub(v3 a, v3 b) { return V(a.x - b.x, a.y - b.y, a.z - b.z); }
static v3 mul(v3 a, float s) { return V(a.x * s, a.y * s, a.z * s); }
static float dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static v3 cross(v3 a, v3 b) { return V(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static float len(v3 a) { return sqrtf(dot(a, a)); }
static v3 nrm(v3 a) { const float l = len(a); return l > 1e-6f ? mul(a, 1.0f / l) : V(0, 1, 0); }
typedef struct { float m[9]; } m3;   /* column-vector convention: y = M x, m[r*3+c] */
static m3 I3(void) { m3 r = { { 1, 0, 0, 0, 1, 0, 0, 0, 1 } }; return r; }
static v3 mv(const m3* M, v3 v) { return V(M->m[0] * v.x + M->m[1] * v.y + M->m[2] * v.z, M->m[3] * v.x + M->m[4] * v.y + M->m[5] * v.z, M->m[6] * v.x + M->m[7] * v.y + M->m[8] * v.z); }
static m3 mm(const m3* A, const m3* B) { m3 r; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r.m[i * 3 + j] = A->m[i * 3] * B->m[j] + A->m[i * 3 + 1] * B->m[3 + j] + A->m[i * 3 + 2] * B->m[6 + j]; return r; }
static m3 tr(const m3* A) { m3 r; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r.m[i * 3 + j] = A->m[j * 3 + i]; return r; }
static m3 cols(v3 a, v3 b, v3 c) { m3 r = { { a.x, b.x, c.x, a.y, b.y, c.y, a.z, b.z, c.z } }; return r; }
static m3 frame(v3 a, v3 b)   /* orthonormal frame: e1 along a, e2 = b made orthogonal */
{
    const v3 e1 = nrm(a); v3 e2 = sub(b, mul(e1, dot(b, e1)));
    e2 = len(e2) > 1e-5f ? nrm(e2) : nrm(cross(e1, fabsf(e1.y) < 0.9f ? V(0, 1, 0) : V(1, 0, 0)));
    return cols(e1, e2, cross(e1, e2));
}
static m3 arc(v3 a, v3 b)     /* minimal rotation taking direction a to direction b */
{
    a = nrm(a); b = nrm(b);
    const v3 v = cross(a, b); const float c = dot(a, b);
    if (c < -0.9999f) { const v3 ax = nrm(cross(a, fabsf(a.x) < 0.9f ? V(1, 0, 0) : V(0, 1, 0))); m3 r; const float x = ax.x, y = ax.y, z = ax.z;
        r.m[0] = 2 * x * x - 1; r.m[1] = 2 * x * y; r.m[2] = 2 * x * z; r.m[3] = 2 * x * y; r.m[4] = 2 * y * y - 1; r.m[5] = 2 * y * z; r.m[6] = 2 * x * z; r.m[7] = 2 * y * z; r.m[8] = 2 * z * z - 1; return r; }
    const float k = 1.0f / (1.0f + c);
    m3 r = { { c + v.x * v.x * k, v.x * v.y * k - v.z, v.x * v.z * k + v.y,
               v.x * v.y * k + v.z, c + v.y * v.y * k, v.y * v.z * k - v.x,
               v.x * v.z * k - v.y, v.y * v.z * k + v.x, c + v.z * v.z * k } };
    return r;
}

typedef struct { float w, x, y, z; } qt;
static qt m2q(const m3* M)
{
    const float* m = M->m; qt q; const float t = m[0] + m[4] + m[8];
    if (t > 0) { const float s = sqrtf(t + 1.0f) * 2; q.w = .25f * s; q.x = (m[7] - m[5]) / s; q.y = (m[2] - m[6]) / s; q.z = (m[3] - m[1]) / s; }
    else if (m[0] > m[4] && m[0] > m[8]) { const float s = sqrtf(1.0f + m[0] - m[4] - m[8]) * 2; q.w = (m[7] - m[5]) / s; q.x = .25f * s; q.y = (m[1] + m[3]) / s; q.z = (m[2] + m[6]) / s; }
    else if (m[4] > m[8]) { const float s = sqrtf(1.0f + m[4] - m[0] - m[8]) * 2; q.w = (m[2] - m[6]) / s; q.x = (m[1] + m[3]) / s; q.y = .25f * s; q.z = (m[5] + m[7]) / s; }
    else { const float s = sqrtf(1.0f + m[8] - m[0] - m[4]) * 2; q.w = (m[3] - m[1]) / s; q.x = (m[2] + m[6]) / s; q.y = (m[5] + m[7]) / s; q.z = .25f * s; }
    return q;
}
static m3 q2m(qt q)
{
    const float n = sqrtf(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z); if (n > 1e-8f) { q.w /= n; q.x /= n; q.y /= n; q.z /= n; }
    const float w = q.w, x = q.x, y = q.y, z = q.z;
    m3 r = { { 1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y),
               2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
               2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y) } };
    return r;
}
static m3 mblend(const m3* A, const m3* B, float t)   /* nlerp between two rotations */
{
    const qt a = m2q(A); qt b = m2q(B);
    if (a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z < 0) { b.w = -b.w; b.x = -b.x; b.y = -b.y; b.z = -b.z; }
    const qt q = { a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
    return q2m(q);
}

/* ------------------------------------------------------------------ body model */
static const char* const k_bn[NBN] = { "pelvis", "lumbar", "thoracic", "neck", "(head)", "shoulder_L", "elbow_L", "wrist_L",
    "shoulder_R", "elbow_R", "wrist_R", "hip_L", "knee_L", "ankle_L", "hip_R", "knee_R", "ankle_R", "(toes_L)", "(toes_R)", "(fingers_L)", "(fingers_R)" };
static const uint32_t k_bs[NBN] = { 0x100000B0u, 0x1000B998u, 0x10000120u, 0x10001D30u, 0, 0x10000148u, 0x10000158u, 0x10000168u,
    0x100001B8u, 0x100001C8u, 0x100001D8u, 0x100000C0u, 0x100000D0u, 0x100000E0u, 0x100000F0u, 0x10000100u, 0x10000110u, 0, 0, 0, 0 };
enum { PEL, LUM, THO, NEC, HED, SHL, ELL, WRL, SHR, ELR, WRR, HIL, KNL, ANL, HIR, KNR, ANR, TOL, TOR, FIL, FIR };
static const float k_rad[NP] = { .12f, .12f, .14f, .07f, .11f, .07f, .05f, .04f, .07f, .05f, .04f, .09f, .07f, .06f, .09f, .07f, .06f, .035f, .035f, .03f, .03f };
static const float k_mass[NP] = { 4.f, 4.f, 5.f, 1.f, 2.f, 1.5f, 1.f, .5f, 1.5f, 1.f, .5f, 3.5f, 1.8f, .8f, 3.5f, 1.8f, .8f, .3f, .3f, .25f, .25f };   /* torso heavy, hands light */
typedef struct { uint8_t a, b; float stiff; } link_t;
static const link_t k_links[] = {
    { LUM, PEL, 1 }, { THO, LUM, 1 }, { NEC, THO, 1 }, { HED, NEC, 1 },
    { SHL, NEC, 1 }, { SHL, THO, 1 }, { SHR, NEC, 1 }, { SHR, THO, 1 }, { SHL, SHR, 1 },
    { ELL, SHL, 1 }, { WRL, ELL, 1 }, { ELR, SHR, 1 }, { WRR, ELR, 1 },
    { HIL, PEL, 1 }, { HIR, PEL, 1 }, { HIL, HIR, 1 }, { KNL, HIL, 1 }, { ANL, KNL, 1 }, { KNR, HIR, 1 }, { ANR, KNR, 1 },
    /* braces: keep the torso a torso, allow some spine bend and head flop */
    { HIL, LUM, 1 }, { HIR, LUM, 1 }, { SHL, LUM, .8f }, { SHR, LUM, .8f }, { THO, PEL, .5f }, { NEC, LUM, .6f }, { HED, THO, .35f },
    { HIL, THO, .3f }, { HIR, THO, .3f },
    /* feet and hands: rigid with the shin / forearm, so toes and fingers collide with the ground too */
    { TOL, ANL, 1 }, { TOL, KNL, 1 }, { TOR, ANR, 1 }, { TOR, KNR, 1 }, { FIL, WRL, 1 }, { FIL, ELL, 1 }, { FIR, WRR, 1 }, { FIR, ELR, 1 },
};
enum { NL = (int)(sizeof k_links / sizeof k_links[0]) };
/* drivers: joint particle, direction end particle, parent driver (-1 = frame from two vectors) */
enum { D_LT, D_UT, D_HEAD, D_UAL, D_LAL, D_UAR, D_LAR, D_ULL, D_LLL, D_ULR, D_LLR };
static const int k_dj[ND] = { PEL, THO, NEC, SHL, ELL, SHR, ELR, HIL, KNL, HIR, KNR };
static const int k_de[ND] = { LUM, NEC, HED, ELL, WRL, ELR, WRR, KNL, ANL, KNR, ANR };
static const int k_dp[ND] = { -1, -1, D_UT, D_UT, D_UAL, D_UT, D_UAR, D_LT, D_ULL, D_LT, D_ULR };
/* muscle tone: each child is pulled to where the game's animation has it relative to its parent, in the frame of its
 * torso half (0 lower, 1 upper); group: 0 spine/head, 1 arms, 2 legs */
typedef struct { uint8_t par, ch, half, grp; } mot_t;
static const mot_t k_mot[] = {
    { LUM, THO, 0, 0 }, { THO, NEC, 1, 0 }, { NEC, HED, 1, 0 },
    { THO, SHL, 1, 1 }, { THO, SHR, 1, 1 }, { SHL, ELL, 1, 1 }, { ELL, WRL, 1, 1 }, { SHR, ELR, 1, 1 }, { ELR, WRR, 1, 1 },
    { PEL, HIL, 0, 2 }, { PEL, HIR, 0, 2 }, { HIL, KNL, 0, 2 }, { KNL, ANL, 0, 2 }, { HIR, KNR, 0, 2 }, { KNR, ANR, 0, 2 },
    { ANL, TOL, 0, 2 }, { ANR, TOR, 0, 2 }, { WRL, FIL, 1, 1 }, { WRR, FIR, 1, 1 },
};
enum { NM = (int)(sizeof k_mot / sizeof k_mot[0]) };
/* stiff spine / neck: these pairs may not come closer than a fraction of their start distance */
typedef struct { uint8_t a, b; float frac; } mind_t;
static const mind_t k_mind[] = { { HED, THO, .85f }, { NEC, LUM, .9f }, { THO, PEL, .9f }, { HED, PEL, .88f }, { SHL, HIL, .85f }, { SHR, HIR, .85f },
                                 { HED, SHL, .8f }, { HED, SHR, .8f } };
enum { NMD = (int)(sizeof k_mind / sizeof k_mind[0]) };

typedef struct {
    int state;               /* 0 free, 1 pending (death seen), 2 snap1 taken, 3 simulating, 4 settled, 5 stopped,
                                6 lost its skeleton, waiting for the game's corpse (resume_state afterwards) */
    uint32_t ent, anim, pal, vt, src38;
    int corpse, resume_state, nbones;
    uint32_t anim_id; v3 last_T;  /* for the corpse match: same animation, same place */
    double wait_t0;
    int idx[NBN], slot[NBN], nslot;
    double t0, last_call, still_ms;
    v3 shooter_w, hit_w; float dmg;
    m3 Rd; v3 Td;            /* entity transform at the start (model -> world: w = Td + Rd^T p) */
    v3 p[NP], pp[NP], p0[NP], snap1[NP];
    float rest[NL], minkn[2], minel[2], knsgn[2], elsgn[2], ground;
    float M0[MAXS][12];      /* slot matrices at the start: rows right, up, at, pos */
    uint8_t drv[MAXS], drv2[MAXS]; float w2[MAXS];   /* clothes / in-between bones blend with a second driver */
    v3 a[NP]; int have_anim; v3 wrote_pel; float mind[NMD]; int headshot, core; double last_dt_ms, slow_ms;
    void* jh; float jq[11 * 9], jx[11 * 3]; int jasleep;
    /* v1056C coat / skirt: the ring of skirt bones around the hips hangs as pendulums (tip particles) from the pelvis */
    uint8_t clothy[MAXS]; int learned;   /* v1056C: slot swings against its best body part (learned) */
    int ncl, cl_slot[12], cl_nb[12][2]; v3 cl_A0[12], cl_T0[12], cl_T[12], cl_Tp[12]; float cl_L[12], cl_nbL[12][2], cl_wl[12]; float frame_dt; int bl_pool, bl_hits; float bl_vy[3];   /* v1056A Jolt body + its latest part rotations / joint positions */
    int kill_spin, conv, low_start;   /* low_start: kneeling / crouching when killed */   /* conv: the frame convention this body started with (never changes under it) */
    int pin_n; float pin_max;   /* v1081P */
    int cf_n; double cf_t; v3 col_c; int col_have;   /* v1110Z corpse hits in a row; where its level collision was last gathered */
    v3 pin_T; int pin_have;     /* v1108Y: where the root is held (follows a thrown body in 1 m hops) */
    uint32_t rest_car, rest_vt; v3 rest_T; int rest_have;   /* v1108Z: settled while a car was in its world -> wake when it leaves */
    bhv_t bh; bhv_out bo; int bhv_last, bhv_on, bhv_zone, bhv_side; float bhv_dir[3];   /* v1075R dying behaviour (stagger / kneel / fall / agony) */
    int wound_half, clutch_arm, writhe_leg, have_wound, hit_leg, hit_arm;   /* -1 none, 0 left, 1 right */ v3 wound_loc; float writhe_end, clutch_str;   /* dying */
    m3 F0[2];                /* lower / upper torso frames at the start */
    int frames;
    float fwd_sign;          /* v1069H: body_fwd() * fwd_sign = the body's front (model +z at the start) */
    double t_wake;           /* v1069H: last bullet on this ragdoll (settling counts from here) */
    /* v1069H live hit reaction (hr = 1): model-space powered ragdoll on a living NPC */
    int hr, hr_pending, hr_part, hr_leg, hr_nhits; double hr_t_hit, hr_fade_t0;
    v3 hr_hit_w, hr_sh_w, hr_sw, hr_sww; float hr_dmg;
    /* v1069S stumble (leg / foot hit): the NPC lurches st_D m along st_Lw (world), the shot leg buckles, the other leg
     * takes a catch step, arms out */
    int st_on, st_leg, st_moves; double st_t0, st_last_move, st_cool; v3 st_Lw, st_Lm; float st_D, st_done, st_s;
    v3 st_pose[NP]; int st_have_pose;   /* v1069T: pose frozen at the stumble's hit (model space) */
    int st_mode; float st_T, st_mT; double st_last_leg;
    uint32_t car; v3 car_vw; float car_oy; int car_on; double car_t0;   /* v1069C: the player's car hit this body */
    int frozen;
    int kd, kd_ph; double kd_set, kd_t1; float kd_bw;   /* v1097N punch knockdown: 0 falling/down, 1 flat->crawl, 2 crawl->stand */
    int cad_n; double cad_max, cad_t; int selfc;   /* v1091E update cadence after a corpse hit */              /* v1069F: settled, its Jolt world freed, the last part rotations (jq / jx) keep being written */   /* v1069U: 1 buckle in place, 2 one step, 3 two steps; length, move time */
} rag_t;
static rag_t s_rg[MAXR];
static CRITICAL_SECTION s_cs;
static int s_jolt = 1;

static int s_on = 1, s_phys = 1, s_max = 10, s_nlog = 0, s_ndeath = 0;
/* v1069F: settled bodies beyond the 6 newest give back their Jolt world (memory) and keep their final pose */
static void freeze_old(void)
{
    int n = 0;
    /* v1091E (user: corpses move "choppy, like it glides"): a frozen body woken by a shot gets a new Jolt world rebuilt from its
     * joint points -> the limbs' twist snapped on every wake, and with several corpses around they kept cycling frozen / woken.
     * Keep 12 (was 6) and never freeze a body that was hit / shoved in the last 20 s; the oldest by its last activity goes first. */
    const double nn = now_ms();
    #define FRZ_OK(q) ((q).state == 4 && (q).jh && !(q).hr && nn - (q).t_wake > 20000.0)
    for (int i = 0; i < MAXR; i++) if (s_rg[i].state == 4 && s_rg[i].jh && !s_rg[i].hr) n++;
    while (n > 12) {   /* v1091E: 12 (each world holds up to 60k level triangles) */
        int o = -1; for (int i = 0; i < MAXR; i++) if (FRZ_OK(s_rg[i]) && (o < 0 || (s_rg[i].t_wake > s_rg[i].t0 ? s_rg[i].t_wake : s_rg[i].t0) < (s_rg[o].t_wake > s_rg[o].t0 ? s_rg[o].t_wake : s_rg[o].t0))) o = i;
        if (o < 0) break;
        jr_destroy(s_rg[o].jh); s_rg[o].jh = NULL; s_rg[o].frozen = 1; s_rg[o].car_on = 0; n--;
    }
}
static float s_push = 1.0f, s_grav = 9.81f, s_tone = 1.0f;
/* entity matrix convention (v1054Z self-check): 0 = RenderWare rows (model = R (w - T)), 1 = transposed. Each death
 * maps the bullet's hit point both ways; the one that lands on the body's surface gets a vote; 2 net votes switch. */
static int s_conv = 0, s_vote[2] = { 0, 0 };
static m3 w2mc(const m3* R, int conv) { return conv ? tr(R) : *R; }
static gfn o_pain_act, o_npc_act, o_base_act;
/* v1055C recorder (GF_RAG_REC=0 off): rag_rec.bin next to the exe's working dir -- per body a 'RAGS' start record
 * (slots, drivers, rest pose, hit) and per frame a 'RAGF' record with WORLD positions of the simulated particles,
 * the game's own animation pose and every drawn palette slot. Rendered offline to see what goes wrong. */
static FILE* s_rec = 0; static long s_rec_bytes = 0; static int s_rec_on = 1;
static void rec_w(const void* p, size_t n) { if (s_rec && s_rec_bytes < (60L << 20)) { fwrite(p, 1, n, s_rec); s_rec_bytes += (long)n; } }
static void rec_u32(uint32_t v) { rec_w(&v, 4); }
static void rec_f(float v) { rec_w(&v, 4); }
static void rec_v3(v3 v) { rec_w(&v, 12); }
#define CLOG2(...) do { static int n2_ = 0; if (n2_ < 200) { n2_++; printf(__VA_ARGS__); fflush(stdout); } } while (0)
#define RLOG(...) do { if (s_nlog < 1500) { s_nlog++; printf(__VA_ARGS__); fflush(stdout); } } while (0)
/* v1057C level collision (user: bodies go through bins / walls / kerbs): assets/gf_collision.bin, built offline from the
 * game's own Havok collision by tools/gf_collision_build.py (street + building + prop collision, world space, XZ grid of
 * 16 m cells). At the ragdoll start every triangle within GF_RAG_COLL_R (10 m) of the pelvis goes into that body's Jolt
 * world as a static mesh, converted into its simulation frame (p = Md (w - Td)). GF_RAG_COLL=0 = flat floor only. */
static struct { int state; float cell, x0, z0; int nx, nz, ntri, nidx; float* tri; int* cells; int* idx; } s_col;
static void col_load(void)
{
    if (s_col.state) return;
    s_col.state = -1;
    const char* e = getenv("GF_RAG_COLL"); if (e && *e == '0') { RLOG("[rag] v1057C level collision off (GF_RAG_COLL=0)\n"); return; }
    char exe[MAX_PATH]; DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH); FILE* f = NULL;
    if (n && n < MAX_PATH) { char* sl = strrchr(exe, '\\'); if (sl) *sl = 0;
        static const char* rel[] = { "\\assets\\gf_collision.bin", "\\..\\assets\\gf_collision.bin", "\\..\\..\\assets\\gf_collision.bin" };
        for (int i = 0; i < 3 && !f; i++) { char p[MAX_PATH + 64]; snprintf(p, sizeof p, "%s%s", exe, rel[i]); f = fopen(p, "rb"); } }
    if (!f) { RLOG("[rag] v1057C no assets\\gf_collision.bin (tools/gf_collision_build.py) -> flat floor only\n"); return; }
    char mg[8]; float hf[3]; int hi[4];
    if (fread(mg, 1, 8, f) == 8 && !memcmp(mg, "GFCOL1", 6) && fread(hf, 4, 3, f) == 3 && fread(hi, 4, 4, f) == 4 &&
        hi[0] > 0 && hi[1] > 0 && hi[0] * hi[1] < 4000000 && hi[2] > 0 && hi[3] >= 0) {
        s_col.cell = hf[0]; s_col.x0 = hf[1]; s_col.z0 = hf[2]; s_col.nx = hi[0]; s_col.nz = hi[1]; s_col.ntri = hi[2]; s_col.nidx = hi[3];
        s_col.tri = (float*)malloc((size_t)s_col.ntri * 36); s_col.cells = (int*)malloc((size_t)s_col.nx * s_col.nz * 8); s_col.idx = (int*)malloc((size_t)s_col.nidx * 4 + 4);
        if (s_col.tri && s_col.cells && s_col.idx && fread(s_col.tri, 36, (size_t)s_col.ntri, f) == (size_t)s_col.ntri &&
            fread(s_col.cells, 8, (size_t)s_col.nx * s_col.nz, f) == (size_t)s_col.nx * s_col.nz && fread(s_col.idx, 4, (size_t)s_col.nidx, f) == (size_t)s_col.nidx)
            s_col.state = 1;
    }
    fclose(f);
    if (s_col.state == 1) RLOG("[rag] v1057C level collision: %d triangles, %dx%d cells of %.0f m\n", s_col.ntri, s_col.nx, s_col.nz, s_col.cell);
    else RLOG("[rag] v1057C assets\\gf_collision.bin unreadable -> flat floor only\n");
}
/* v1119S: is (px, pz) inside triangle w (XZ projection)? *y = its plane height there */
static int gf_intri_xz(const float* w, float px, float pz, float* y)
{
    const float ax = w[0], az = w[2], bx = w[3], bz = w[5], cx = w[6], cz = w[8];
    const float d = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz); if (fabsf(d) < 1e-9f) return 0;
    const float l1 = ((bz - cz) * (px - cx) + (cx - bx) * (pz - cz)) / d, l2 = ((cz - az) * (px - cx) + (ax - cx) * (pz - cz)) / d, l3 = 1.0f - l1 - l2;
    if (l1 < 0 || l2 < 0 || l3 < 0) return 0;
    *y = l1 * w[1] + l2 * w[4] + l3 * w[7]; return 1;
}
/* triangles within rad of world point c, into r's simulation frame; returns the count (out: 9 floats each) */
static int col_gather(const rag_t* r, v3 c, float rad, float* out, int maxn, int* nfloor)
{
    col_load(); *nfloor = 0;
    if (s_col.state != 1) return 0;
    static unsigned* stamp = NULL; static unsigned gen = 0;
    if (!stamp) { stamp = (unsigned*)calloc((size_t)s_col.ntri, 4); if (!stamp) return 0; }
    gen++;
    const m3 Md = w2mc(&r->Rd, r->conv);
    const int cx0 = (int)floorf((c.x - rad - s_col.x0) / s_col.cell), cx1 = (int)floorf((c.x + rad - s_col.x0) / s_col.cell);
    const int cz0 = (int)floorf((c.z - rad - s_col.z0) / s_col.cell), cz1 = (int)floorf((c.z + rad - s_col.z0) / s_col.cell);
    int n = 0;
    /* v1119S (user: indoor bodies "bugged" half-standing, one "fell through the floor"; [rag-rest] v1119R):
     * (1) gf_collision.bin has flat sheets ABOVE some floors (20 x 4 m / 20 x 17 m at +0.54 m over where the NPC stood): the
     *     body landed on air. A flat surface 0.15 .. 1.8 m directly above the dying NPC's feet cannot be a floor (he stood
     *     under it) -- that surface and every flat triangle at the same height (the rest of the sheet) are left out.
     * (2) the floor box was lowered 0.6 m whenever some floor triangle was within 1 m, also with none under the body
     *     (interior floors are often missing) -> it fell 0.5 m. Now only when a real floor lies right under the pelvis.
     * GF_RAG_GHOST=0 / GF_RAG_FLOORCHK=0 = old. */
    static int g_on = -1, f_on = -1;
    if (g_on < 0) { const char* e = getenv("GF_RAG_GHOST"); g_on = (e && *e == '0') ? 0 : 1; e = getenv("GF_RAG_FLOORCHK"); f_on = (e && *e == '0') ? 0 : 1; }
    float gh[8]; int ngh = 0, under = 0, ghost_n = 0;
    #define GF_INTRI_XZ(w, px, pz, yout) gf_intri_xz((w), (px), (pz), &(yout))
    {   const float fx = r->Td.x, fy = r->Td.y, fz = r->Td.z;
        const int near_start = (c.x - fx) * (c.x - fx) + (c.z - fz) * (c.z - fz) < 9.0f;
        for (int pass = 0; pass < 2; pass++) {
            const float px = pass ? c.x : fx, pz = pass ? c.z : fz;
            if (!pass && (!g_on || !near_start)) continue;
            const int qx = (int)floorf((px - s_col.x0) / s_col.cell), qz = (int)floorf((pz - s_col.z0) / s_col.cell);
            if (qx < 0 || qz < 0 || qx >= s_col.nx || qz >= s_col.nz) continue;
            const int st = s_col.cells[2 * (qz * s_col.nx + qx)], cnt = s_col.cells[2 * (qz * s_col.nx + qx) + 1];
            for (int k = 0; k < cnt; k++) {
                const int t = s_col.idx[st + k]; if (t < 0 || t >= s_col.ntri) continue;
                const float* w = s_col.tri + 9 * t; float y;
                if (!GF_INTRI_XZ(w, px, pz, y)) continue;
                const float e1x = w[3] - w[0], e1y = w[4] - w[1], e1z = w[5] - w[2], e2x = w[6] - w[0], e2y = w[7] - w[1], e2z = w[8] - w[2];
                const float nx = e1y * e2z - e1z * e2y, ny = e1z * e2x - e1x * e2z, nz = e1x * e2y - e1y * e2x;
                const float L = sqrtf(nx * nx + ny * ny + nz * nz); if (L < 1e-9f || fabsf(ny) < 0.95f * L) continue;
                if (!pass) {
                    if (y > fy + 0.15f && y < fy + 1.8f && ngh < 8) { int dup = 0; for (int i = 0; i < ngh; i++) if (fabsf(gh[i] - y) < 0.03f) dup = 1; if (!dup) gh[ngh++] = y; }
                } else {
                    int g = 0; for (int i = 0; i < ngh; i++) if (fabsf(gh[i] - y) < 0.03f) g = 1;
                    if (!g && y > c.y - 1.3f && y < c.y + 0.1f) under = 1;
                }
            }
        }
    }
    for (int cz = cz0; cz <= cz1; cz++) for (int cx = cx0; cx <= cx1; cx++) {
        if (cx < 0 || cz < 0 || cx >= s_col.nx || cz >= s_col.nz) continue;
        const int st = s_col.cells[2 * (cz * s_col.nx + cx)], cnt = s_col.cells[2 * (cz * s_col.nx + cx) + 1];
        for (int k = 0; k < cnt && n < maxn; k++) {
            const int t = s_col.idx[st + k]; if (t < 0 || t >= s_col.ntri || stamp[t] == gen) continue;
            stamp[t] = gen;
            const float* w = s_col.tri + 9 * t; v3 v[3]; int nr = 0;
            for (int j = 0; j < 3; j++) { v[j] = V(w[3 * j], w[3 * j + 1], w[3 * j + 2]);
                if (fabsf(v[j].x - c.x) < rad && fabsf(v[j].z - c.z) < rad) nr = 1; }
            if (!nr) { /* big triangle: keep it if its XZ box overlaps */ 
                const float mnx = fminf(v[0].x, fminf(v[1].x, v[2].x)), mxx = fmaxf(v[0].x, fmaxf(v[1].x, v[2].x));
                const float mnz = fminf(v[0].z, fminf(v[1].z, v[2].z)), mxz = fmaxf(v[0].z, fmaxf(v[1].z, v[2].z));
                if (mxx < c.x - rad || mnx > c.x + rad || mxz < c.z - rad || mnz > c.z + rad) continue; }
            if (ngh) { const v3 e1 = sub(v[1], v[0]), e2 = sub(v[2], v[0]); const v3 nn = V(e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x);
              const float L = len(nn), my = (v[0].y + v[1].y + v[2].y) / 3.0f;
              if (L > 1e-6f && fabsf(nn.y) / L > 0.95f && fmaxf(v[0].y, fmaxf(v[1].y, v[2].y)) - fminf(v[0].y, fminf(v[1].y, v[2].y)) < 0.06f) {
                  int g = 0; for (int i = 0; i < ngh; i++) if (fabsf(gh[i] - my) < 0.03f) g = 1;
                  if (g) { ghost_n++; continue; } } }
            { const v3 e1 = sub(v[1], v[0]), e2 = sub(v[2], v[0]); const v3 nn = V(e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x);
              const float L = len(nn); if (L > 1e-6f && fabsf(nn.y) / L > 0.7f) {   /* walkable surface under the body? */
                  const float my = (v[0].y + v[1].y + v[2].y) / 3.0f;
                  if (my < c.y + 0.5f && my > c.y - 2.0f && fminf(v[0].x, fminf(v[1].x, v[2].x)) <= c.x + 1.0f && fmaxf(v[0].x, fmaxf(v[1].x, v[2].x)) >= c.x - 1.0f &&
                      fminf(v[0].z, fminf(v[1].z, v[2].z)) <= c.z + 1.0f && fmaxf(v[0].z, fmaxf(v[1].z, v[2].z)) >= c.z - 1.0f) (*nfloor)++; } }
            for (int j = 0; j < 3; j++) { const v3 q = mv(&Md, sub(v[j], r->Td)); out[9 * n + 3 * j] = q.x; out[9 * n + 3 * j + 1] = q.y; out[9 * n + 3 * j + 2] = q.z; }
            n++;
        }
    }
    #undef GF_INTRI_XZ
    if (ngh) { static int nl = 0; if (nl < 40) { nl++; RLOG("[rag] v1119S ent=%08X: %d triangle(s) of %d flat sheet(s) above where it stood left out (first +%.2f m)\n", r->ent, ghost_n, ngh, gh[0] - r->Td.y); } }
    if (f_on && *nfloor && !under) { static int nl = 0; if (nl < 40) { nl++; RLOG("[rag] v1119S ent=%08X: no level floor right under the body -> floor box kept at the ground\n", r->ent); } *nfloor = 0; }
    return n;
}
/* v1057B blood: nearest hit of the world ray o + t d (d unit, t <= maxd) on the level collision; n = surface normal facing
 * the ray. */
static int col_ray(v3 o, v3 d, float maxd, float* t_out, v3* n_out)
{
    col_load();
    if (s_col.state != 1) return 0;
    const v3 e = add(o, mul(d, maxd));
    const int cx0 = (int)floorf((fminf(o.x, e.x) - s_col.x0) / s_col.cell), cx1 = (int)floorf((fmaxf(o.x, e.x) - s_col.x0) / s_col.cell);
    const int cz0 = (int)floorf((fminf(o.z, e.z) - s_col.z0) / s_col.cell), cz1 = (int)floorf((fmaxf(o.z, e.z) - s_col.z0) / s_col.cell);
    float best = maxd; int hit = 0; v3 bn = V(0, 1, 0);
    for (int cz = cz0; cz <= cz1; cz++) for (int cx = cx0; cx <= cx1; cx++) {
        if (cx < 0 || cz < 0 || cx >= s_col.nx || cz >= s_col.nz) continue;
        const int st = s_col.cells[2 * (cz * s_col.nx + cx)], cnt = s_col.cells[2 * (cz * s_col.nx + cx) + 1];
        for (int k = 0; k < cnt; k++) {
            const float* w = s_col.tri + 9 * s_col.idx[st + k];
            const v3 a = V(w[0], w[1], w[2]), b = V(w[3], w[4], w[5]), c = V(w[6], w[7], w[8]);
            const v3 e1 = sub(b, a), e2 = sub(c, a);
            const v3 pv = V(d.y * e2.z - d.z * e2.y, d.z * e2.x - d.x * e2.z, d.x * e2.y - d.y * e2.x);
            const float det = dot(e1, pv); if (fabsf(det) < 1e-9f) continue;
            const float inv = 1.0f / det; const v3 tv = sub(o, a); const float u = dot(tv, pv) * inv; if (u < 0 || u > 1) continue;
            const v3 qv = V(tv.y * e1.z - tv.z * e1.y, tv.z * e1.x - tv.x * e1.z, tv.x * e1.y - tv.y * e1.x);
            const float v = dot(d, qv) * inv; if (v < 0 || u + v > 1) continue;
            const float t = dot(e2, qv) * inv;
            if (t > 0.0f && t < best) { best = t; hit = 1; bn = nrm(V(e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x)); }
        }
    }
    if (!hit) return 0;
    if (dot(bn, d) > 0) bn = mul(bn, -1.0f);
    *t_out = best; *n_out = bn; return 1;
}
/* v1119R diagnostics (user: "ragdolls bugged" -- indoor bodies settle half-standing): when a body comes to rest, log what
 * level-collision surface is under its pelvis, chest and head -- height above the floor under the start, normal, size --
 * so the surface holding it up can be identified (a real floor, furniture, or a stray box / sheet in gf_collision.bin).
 * GF_RAG_REST_DIAG=0 = off. Log [rag-rest]. */
static int col_ray_down_i(v3 o, float maxd, float* t_out, int* idx_out)
{
    col_load();
    if (s_col.state != 1) return 0;
    const int cx = (int)floorf((o.x - s_col.x0) / s_col.cell), cz = (int)floorf((o.z - s_col.z0) / s_col.cell);
    if (cx < 0 || cz < 0 || cx >= s_col.nx || cz >= s_col.nz) return 0;
    const int st = s_col.cells[2 * (cz * s_col.nx + cx)], cnt = s_col.cells[2 * (cz * s_col.nx + cx) + 1];
    float best = maxd; int hit = -1;
    for (int k = 0; k < cnt; k++) {
        const int ti = s_col.idx[st + k]; if (ti < 0 || ti >= s_col.ntri) continue;
        const float* w = s_col.tri + 9 * ti;
        /* vertical ray: 2D point-in-triangle in XZ, then the plane height */
        const float ax = w[0], az = w[2], bx = w[3], bz = w[5], cx2 = w[6], cz2 = w[8];
        const float d = (bz - cz2) * (ax - cx2) + (cx2 - bx) * (az - cz2); if (fabsf(d) < 1e-9f) continue;
        const float l1 = ((bz - cz2) * (o.x - cx2) + (cx2 - bx) * (o.z - cz2)) / d;
        const float l2 = ((cz2 - az) * (o.x - cx2) + (ax - cx2) * (o.z - cz2)) / d, l3 = 1.0f - l1 - l2;
        if (l1 < 0 || l2 < 0 || l3 < 0) continue;
        const float y = l1 * w[1] + l2 * w[4] + l3 * w[7], t = o.y - y;
        if (t >= 0.0f && t < best) { best = t; hit = ti; }
    }
    if (hit < 0) return 0;
    *t_out = best; *idx_out = hit; return 1;
}
static void rag_rest_diag(const rag_t* r)
{
    static int on = -1; if (on < 0) { const char* e = getenv("GF_RAG_REST_DIAG"); on = (e && *e == '0') ? 0 : 1; }
    if (!on) return;
    const m3 Md = w2mc(&r->Rd, r->conv), MdT = tr(&Md);
    float tf; int fi; const v3 st0 = add(r->Td, V(0, 0.6f, 0));
    const float floor0 = col_ray_down_i(st0, 3.0f, &tf, &fi) ? st0.y - tf : r->Td.y;
    char line[700]; int o = snprintf(line, sizeof line, "[rag-rest] v1119R ent=%08X at (%.2f %.2f %.2f), floor under the start %.2f |",
                                     r->ent, r->Td.x, r->Td.y, r->Td.z, floor0);
    static const int parts[3] = { PEL, THO, HED }; static const char* nm[3] = { "pelvis", "chest", "head" };
    for (int i = 0; i < 3 && o < (int)sizeof line - 160; i++) {
        const v3 w = add(r->Td, mv(&MdT, r->p[parts[i]]));
        float t; int ti;
        if (col_ray_down_i(add(w, V(0, 0.05f, 0)), 3.0f, &t, &ti)) {
            const float* q = s_col.tri + 9 * ti;
            const float mnx = fminf(q[0], fminf(q[3], q[6])), mxx = fmaxf(q[0], fmaxf(q[3], q[6]));
            const float mny = fminf(q[1], fminf(q[4], q[7])), mxy = fmaxf(q[1], fmaxf(q[4], q[7]));
            const float mnz = fminf(q[2], fminf(q[5], q[8])), mxz = fmaxf(q[2], fmaxf(q[5], q[8]));
            const v3 e1 = V(q[3] - q[0], q[4] - q[1], q[5] - q[2]), e2 = V(q[6] - q[0], q[7] - q[1], q[8] - q[2]);
            const v3 nn = nrm(V(e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x));
            o += snprintf(line + o, sizeof line - o, " %s %+.2f, on tri %d at %+.2f (ny %+.2f, %.2fx%.2fx%.2f m) |", nm[i], w.y - floor0, ti,
                          (w.y + 0.05f - t) - floor0, nn.y, mxx - mnx, mxy - mny, mxz - mnz);
        } else o += snprintf(line + o, sizeof line - o, " %s %+.2f, nothing below |", nm[i], w.y - floor0);
    }
    RLOG("%s\n", line);
}
extern void gfd_add(const float* p, const float* n, const float* t, float w, float h, int sprite, float alpha, float grow);
static float frand(void) { return (float)rand() / (float)RAND_MAX; }
/* v1057B-3 (user: blood in the air): a decal only where the surface really is under all four corners (a 1 m spray on
 * a 20 cm kerb face stuck out into the air) -- rays back into the surface from each corner must hit it within 12 cm of
 * the plane; otherwise shrink (70 %, 45 %) or drop it. */
static int decal_fits(v3 p, v3 n, v3 t, float w, float h)
{
    const v3 b = V(n.y * t.z - n.z * t.y, n.z * t.x - n.x * t.z, n.x * t.y - n.y * t.x);
    static const float cs[4][2] = { { -0.45f, -0.45f }, { 0.45f, -0.45f }, { -0.45f, 0.45f }, { 0.45f, 0.45f } };
    for (int k = 0; k < 4; k++) {
        const v3 c = add(p, add(mul(t, cs[k][0] * w), mul(b, cs[k][1] * h)));
        float tt; v3 nn;
        if (!col_ray(add(c, mul(n, 0.25f)), mul(n, -1.0f), 0.5f, &tt, &nn)) return 0;
        if (fabsf(tt - 0.25f) > 0.12f || dot(nn, n) < 0.8f) return 0;
    }
    return 1;
}
static int blood_decal(v3 p, v3 n, v3 along, float w, float h, int sprite, float alpha, float grow)
{
    v3 t = sub(along, mul(n, dot(along, n)));
    if (len(t) < 0.1f) { t = fabsf(n.y) < 0.9f ? V(0, 1, 0) : V(1, 0, 0); t = sub(t, mul(n, dot(t, n))); }
    t = nrm(t);
    static const float sc[3] = { 1.0f, 0.7f, 0.45f };
    for (int i = 0; i < 3; i++)
        if (decal_fits(p, n, t, w * sc[i], h * sc[i])) { const float ww = w * sc[i], hh = h * sc[i]; gfd_add(&p.x, &n.x, &t.x, ww, hh, sprite, alpha, grow); return 1; }
    return 0;
}
/* a splat on the ground under the world point q (within 2.5 m below); sprite / size given */
static int blood_ground(v3 q, float size, int sprite, float alpha, float grow)
{
    float t; v3 n;
    if (!col_ray(add(q, V(0, 0.4f, 0)), V(0, -1, 0), 2.9f, &t, &n) || n.y < 0.55f) return 0;
    const float a = frand() * 6.2831853f;
    return blood_decal(V(q.x, q.y + 0.4f - t, q.z), n, V(cosf(a), 0, sinf(a)), size * (0.85f + 0.3f * frand()), size * (0.85f + 0.3f * frand()), sprite, alpha, grow);
}
/* at the death: spray where the bullet's line leaves the body and meets a wall / the ground behind it (<= 4.5 m), and
 * a few drops on the ground along the way */
static int blood_spray(v3 hit_w, v3 shooter_w, float dmg, float scale, int drops)   /* v1069H: shared by deaths and hits */
{
    static int on = -1; if (on < 0) { const char* e = getenv("GF_BLOOD"); on = (e && *e == '0') ? 0 : 1; }
    if (!on || !fok(hit_w.x) || !fok(shooter_w.x)) return 0;
    v3 d = sub(hit_w, shooter_w); const float L = len(d); if (L < 0.2f || L > 300.0f) return 0; d = mul(d, 1.0f / L);
    float t; v3 n; int nd = 0;
    const v3 o = add(hit_w, mul(d, 0.2f));
    if (col_ray(o, d, 4.5f, &t, &n)) {
        const v3 p = add(o, mul(d, t));
        const float sz = fminf(1.25f, 0.55f + 0.15f * t) * (dmg >= 3000.0f ? 1.2f : 1.0f) * scale;
        nd += blood_decal(p, n, d, sz * 1.35f, sz, 0, 0.92f, 0.10f);
        if (scale > 0.8f) nd += blood_decal(add(p, mul(sub(d, mul(n, dot(d, n))), 0.25f)), n, V(frand() - 0.5f, frand() - 0.5f, frand() - 0.5f), sz * 0.8f, sz * 0.8f, 1, 0.85f, 0.15f);
    }
    for (int k = 0; k < drops; k++) { const v3 q = add(hit_w, mul(d, 0.5f + 0.7f * k + 0.3f * frand())); nd += blood_ground(q, (0.30f + 0.12f * frand()) * scale, 1, 0.8f, 0.05f); }
    return nd;
}
static void blood_on_death(const rag_t* r)
{
    const int nd = blood_spray(r->hit_w, r->shooter_w, r->dmg, 1.0f, 3);
    RLOG("[rag] ent=%08X: blood on death -> %d decal(s)\n", r->ent, nd);
}

static int ent_xform(uint32_t ent, m3* R, v3* T)   /* RwMatrix rows at *(ent+0x68)+0x10 */
{
    const uint32_t m = rd32(ent + 0x68); if (!ok(m)) return 0;
    for (int i = 0; i < 9; i++) R->m[i] = rdf(m + 0x10 + (uint32_t)(i / 3) * 0x10 + (uint32_t)(i % 3) * 4);
    *T = V(rdf(m + 0x40), rdf(m + 0x44), rdf(m + 0x48));
    return fok(T->x) && fok(T->y) && fok(T->z) && fok(R->m[0]);
}
static uint32_t anim_pal(uint32_t anim) { const uint32_t h = rd32(anim + 0x74); return ok(h) ? rd32(h + 8) : 0; }
static int anim_nbones(uint32_t anim)
{
    const uint32_t a = rd32(anim + 4); if (!ok(a)) return 0;
    const int cnt = (int)rd32(a + 0x18); const uint32_t base = rd32(a + 0x1C); const int i = (int16_t)rd16(anim + 0xA);
    if (!ok(base) || i < 0 || i >= cnt) return 0;
    const int n = (int)rd32(base + (uint32_t)i * 20u + 4u);
    return (n > 0 && n <= 255) ? n : 0;
}
static v3 slot_pos(uint32_t pal, int s) { const uint32_t m = pal + (uint32_t)s * 64u; return V(rdf(m + 0x30), rdf(m + 0x34), rdf(m + 0x38)); }

/* ------------------------------------------------------------------ v1055B: learn the rig from the game's own animation
 * Every NPC skeleton the game animates (walking, running, idling -- dozens per frame) is sampled in the background:
 * for every pair of palette slots (b, p) the position of b in p's frame is tracked (running mean + variance). A bone
 * that keeps a constant offset to a body bone moves rigidly with it -- that is the part it belongs to. Replaces the
 * pose-based guessing that gave a mid-stride foot and coat tails to the pelvis (v1055A log, slot 14 / 68 / 69). */
enum { LS = 96, NTYPE = 16, LEARN_MAX = 900 };   /* v1100L: 16 rig types (was 4: once four NPC rigs had a slot the player's rig was never learned -> no foot IK all session) */
typedef struct { uint32_t key; int nslot, n; float mean[LS * LS * 3]; float m2[LS * LS]; double last; } skt_t;
static skt_t s_skt[NTYPE]; static int s_nskt = 0;
static volatile LONG s_learn_busy = 0; static double s_learn_last = 0;
/* v1100L: learned rigs persist in gf_rig_cache.bin next to the exe ("GFRIG1", count, then per rig: key, nslot, n, mean, m2 for
 * nslot x nslot) -> the player's foot IK is ready from the first frame of the next session. GF_RIG_CACHE=0 off. */
static int rig_cache_path(char* out, size_t cap)
{
    const char* e = getenv("GF_RIG_CACHE"); if (e && *e == '0') return 0;
    char exe[MAX_PATH]; const DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH); if (!n || n >= MAX_PATH) return 0;
    char* sl = strrchr(exe, '\\'); if (sl) *sl = 0;
    snprintf(out, cap, "%s\\gf_rig_cache.bin", exe); return 1;
}
static void rig_save(void)
{
    char path[MAX_PATH + 32]; if (!rig_cache_path(path, sizeof path)) return;
    int cnt = 0; for (int i = 0; i < s_nskt; i++) if (s_skt[i].n >= LEARN_MAX) cnt++;
    FILE* f = fopen(path, "wb"); if (!f) return;
    fwrite("GFRIG1\0\0", 1, 8, f); fwrite(&cnt, 4, 1, f);
    for (int i = 0; i < s_nskt; i++) { const skt_t* T = &s_skt[i]; if (T->n < LEARN_MAX) continue;
        fwrite(&T->key, 4, 1, f); fwrite(&T->nslot, 4, 1, f); fwrite(&T->n, 4, 1, f);
        for (int b = 0; b < T->nslot; b++) fwrite(&T->mean[(b * LS) * 3], 4, (size_t)T->nslot * 3, f);
        for (int b = 0; b < T->nslot; b++) fwrite(&T->m2[b * LS], 4, (size_t)T->nslot, f); }
    fclose(f); RLOG("[rag] v1100L rig cache saved: %d learned rig(s) -> %s\n", cnt, path);
}
static void rig_load(void)
{
    char path[MAX_PATH + 32]; if (!rig_cache_path(path, sizeof path)) return;
    FILE* f = fopen(path, "rb"); if (!f) { RLOG("[rag] v1100L no rig cache yet (%s)\n", path); return; }
    char mg[8]; int cnt = 0, got = 0;
    if (fread(mg, 1, 8, f) == 8 && !memcmp(mg, "GFRIG1", 6) && fread(&cnt, 4, 1, f) == 1 && cnt > 0 && cnt <= NTYPE) {
        for (int i = 0; i < cnt && s_nskt < NTYPE; i++) { skt_t* T = &s_skt[s_nskt]; memset(T, 0, sizeof *T);
            if (fread(&T->key, 4, 1, f) != 1 || fread(&T->nslot, 4, 1, f) != 1 || fread(&T->n, 4, 1, f) != 1 || T->nslot < 8 || T->nslot > LS) break;
            int okr = 1;
            for (int b = 0; okr && b < T->nslot; b++) okr = fread(&T->mean[(b * LS) * 3], 4, (size_t)T->nslot * 3, f) == (size_t)T->nslot * 3;
            for (int b = 0; okr && b < T->nslot; b++) okr = fread(&T->m2[b * LS], 4, (size_t)T->nslot, f) == (size_t)T->nslot;
            if (!okr) break;
            s_nskt++; got++; } }
    fclose(f); RLOG("[rag] v1100L rig cache: %d learned rig(s) loaded from %s\n", got, path);
}
static uint32_t rig_key(uint32_t anim, int* nslot)   /* hash of the bone -> slot table: same rig, same key */
{
    const uint32_t tab = rd32(anim + 0x78); const int nb = anim_nbones(anim);
    if (!ok(tab) || !nb) return 0;
    uint32_t h = 2166136261u ^ (uint32_t)nb; int mx = -1;
    for (int i = 0; i < nb; i++) { const int sl = vm_base[tab + (uint32_t)i]; h = (h ^ (uint32_t)sl) * 16777619u; if (sl != 0xFF && sl > mx) mx = sl; }
    *nslot = mx + 1; return h ? h : 1;
}
static skt_t* rig_find(uint32_t key) { for (int i = 0; i < s_nskt; i++) if (s_skt[i].key == key) return &s_skt[i]; return 0; }
static void rig_learn_one(uint32_t anim)
{
    int ns = 0; const uint32_t key = rig_key(anim, &ns); const uint32_t pal = anim_pal(anim);
    if (!key || ns < 8 || ns > LS || !ok(pal)) return;
    skt_t* T = rig_find(key);
    if (!T) { if (s_nskt >= NTYPE) return; T = &s_skt[s_nskt++]; memset(T, 0, sizeof *T); T->key = key; T->nslot = ns; }
    if (T->n >= LEARN_MAX || T->nslot != ns) return;
    { const double nw = now_ms(); if (nw - T->last < 8.0) return; T->last = nw; }   /* v1100L: each rig ~1 sample per frame (was one shared 6 ms budget: the player's rig took most of a session) */
    static v3 P[LS], R0[LS], R1[LS], R2[LS];
    for (int b = 0; b < ns; b++) {
        const uint32_t m = pal + (uint32_t)b * 64u;
        R0[b] = V(rdf(m), rdf(m + 4), rdf(m + 8)); R1[b] = V(rdf(m + 16), rdf(m + 20), rdf(m + 24)); R2[b] = V(rdf(m + 32), rdf(m + 36), rdf(m + 40));
        P[b] = V(rdf(m + 48), rdf(m + 52), rdf(m + 56));
        const float l = len(R0[b]); if (!(l > 0.5f && l < 2.0f) || !fok(P[b].x)) return;   /* not a clean pose */
    }
    const int n = ++T->n; const float inv = 1.0f / (float)n;
    for (int b = 0; b < ns; b++) for (int q = 0; q < ns; q++) {
        const v3 d = sub(P[b], P[q]); const float t0 = dot(R0[q], d), t1 = dot(R1[q], d), t2 = dot(R2[q], d);
        float* mu = &T->mean[(b * LS + q) * 3];
        const float e0 = t0 - mu[0], e1 = t1 - mu[1], e2 = t2 - mu[2];
        mu[0] += e0 * inv; mu[1] += e1 * inv; mu[2] += e2 * inv;
        T->m2[b * LS + q] += e0 * (t0 - mu[0]) + e1 * (t1 - mu[1]) + e2 * (t2 - mu[2]);
    }
    if (n == 60 || n == LEARN_MAX) RLOG("[rag] v1055B rig %08X (%d slots): %d animation samples learned\n", key, ns, n);
    if (n == LEARN_MAX) rig_save();   /* v1100L */
}
static void rig_learn(uint32_t anim)
{
    const double n0 = now_ms();   /* v1100L-2: no shared throttle (NPC updates in the same ms starved the player's rig); per-rig limit in rig_learn_one */
    if (InterlockedExchange(&s_learn_busy, 1)) return;
    __try { rig_learn_one(anim); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    s_learn_last = n0;
    InterlockedExchange(&s_learn_busy, 0);
}

/* ------------------------------------------------------------------ death -> pending ragdoll */
/* bone indices (findBone through the entity's vtable +0x94) + toe / finger tip slots; 0 = this skeleton cannot be posed */
static int rag_bones(rag_t* r, uint32_t ent, uint32_t anim)
{
    gfn fb = lookup(vfunc(ent, 0x94));
    ppu_context* cur = g_active_ctx; const uint32_t s = cur ? (((uint32_t)cur->gpr[1] - 0x800u) & ~0xFu) : 0;
    if (!fb || !s) { RLOG("[rag] ent=%08X: findBone missing -- skipped\n", ent); return 0; }
    const uint32_t tab = rd32(anim + 0x78); const int nb = anim_nbones(anim);
    if (!ok(tab) || !nb) { RLOG("[rag] ent=%08X: no bone table -- skipped\n", ent); return 0; }
    int maxslot = -1;
    for (int i = 0; i < nb; i++) { const int sl = vm_base[tab + (uint32_t)i]; if (sl != 0xFF && sl > maxslot) maxslot = sl; }
    r->nslot = maxslot + 1;
    if (r->nslot <= 0 || r->nslot > MAXS) { RLOG("[rag] ent=%08X: %d palette slots -- skipped\n", ent, r->nslot); return 0; }
    for (int b = 0; b < NBN; b++) {
        r->idx[b] = -1; r->slot[b] = -1;
        if (!k_bs[b]) continue;
        wr32(s, 0xFFFFFFFFu);
        if (!gcall(fb, ent, s, k_bs[b])) { RLOG("[rag] findBone faulted -- skipped\n"); return 0; }
        const int ix = (int)rd32(s);
        if (ix < 0 || ix >= nb) { RLOG("[rag] ent=%08X: bone %s missing (%d) -- skipped\n", ent, k_bn[b], ix); return 0; }
        const int sl = vm_base[tab + (uint32_t)ix];
        if (sl == 0xFF || sl >= r->nslot) { RLOG("[rag] ent=%08X: bone %s has no palette slot -- skipped\n", ent, k_bn[b]); return 0; }
        r->idx[b] = ix; r->slot[b] = sl;
    }
    /* toe / finger tips: the palette slot farthest out from the ankle (below it) / wrist (beyond it) on that side */
    { const int tip[4] = { TOL, TOR, FIL, FIR }, base[4] = { ANL, ANR, WRL, WRR }, other[4] = { ANR, ANL, WRR, WRL }, prox[4] = { KNL, KNR, ELL, ELR };
      for (int t = 0; t < 4; t++) {
        const v3 b0 = slot_pos(r->pal, r->slot[base[t]]), o0 = slot_pos(r->pal, r->slot[other[t]]), pr = slot_pos(r->pal, r->slot[prox[t]]);
        int best = r->slot[base[t]]; float bd = 0.05f;
        for (int sl = 0; sl < r->nslot; sl++) {
            const v3 q = slot_pos(r->pal, sl); const float d = len(sub(q, b0));
            if (d > 0.28f || d <= bd || len(sub(q, o0)) < d) continue;
            if (t < 2 ? (q.y > b0.y + 0.02f) : (dot(sub(q, b0), sub(b0, pr)) <= 0.0f)) continue;
            bd = d; best = sl;
        }
        r->slot[tip[t]] = best; r->idx[tip[t]] = -1;
      } }
    r->nbones = nb;
    return 1;
}
static void rag_release(rag_t* r) { if (r->jh) jr_destroy(r->jh); r->jh = NULL; r->state = 5; }
/* v1069C: the player's car (player+0x1718 == player+0x4C, +0x5C4 bit 0x8: gf_combat.c) and its Havok body velocity
 * (*(car+0x138) + 0x180: gf_coop_vehicle.c) */
static int s_carhit = 1;
static uint32_t player_car(v3* vel)
{
    uint32_t a = rd32(0x10181290u); if (!ok(a)) return 0; a = rd32(a + 4); if (!ok(a)) return 0; a = rd32(a);
    if (!ok(a) || a < 0x17FCu) return 0;
    const uint32_t pl = a - 0x17FCu; if (!ok(pl)) return 0;
    const uint32_t car = rd32(pl + 0x1718u);
    if (!ok(car) || car != rd32(pl + 0x4Cu) || !(rd32(pl + 0x5C4u) & 0x8u)) return 0;
    const uint32_t b = rd32(car + 0x138u); if (!ok(b)) return 0;
    *vel = V(rdf(b + 0x180u), rdf(b + 0x184u), rdf(b + 0x188u));
    return fok(vel->x) && fok(vel->y) && fok(vel->z) ? car : 0;
}
static int car_frame(const rag_t* r, uint32_t car, float* rot9, v3* pos)   /* car local -> this body's sim frame */
{
    m3 R; v3 T; if (!ent_xform(car, &R, &T)) return 0;
    const m3 Mc = w2mc(&R, r->conv), McT = tr(&Mc), Md = w2mc(&r->Rd, r->conv);
    const m3 Rs = mm(&Md, &McT);
    memcpy(rot9, Rs.m, 36); *pos = mv(&Md, sub(T, r->Td));
    return fok(pos->x) && fok(rot9[0]);
}
static void rag_death(uint32_t sm, uint32_t ent)
{
    for (int i = 0; i < MAXR; i++) if (s_rg[i].state && s_rg[i].state < 5 && s_rg[i].ent == ent) {
        if (s_rg[i].kd) {   /* v1097N: killed while knocked down -> the knockdown ragdoll becomes the death ragdoll; during the get-up anim: a new one */
            if (!s_rg[i].kd_ph) { s_rg[i].kd = 0; s_rg[i].state = s_rg[i].state == 4 ? 3 : s_rg[i].state; s_rg[i].t_wake = now_ms(); RLOG("[kd] ent=%08X died while down -> death ragdoll\n", ent); return; }
            rag_release(&s_rg[i]); continue; }
        if (!s_rg[i].hr) return;
        rag_release(&s_rg[i]);   /* v1069H: the hit reaction ends, the death ragdoll takes over */
    }
    const uint32_t anim = rd32(ent + 0xC4);
    if (!ok(anim) || !ok(anim_pal(anim))) { RLOG("[rag] death ent=%08X: no skeleton (anim %08X) -- skipped\n", ent, anim); return; }
    /* a free entry while fewer than GF_RAG_MAX bodies are posed; else replace the oldest settled one (it goes back to
     * the game's dead pose); else a live hit reaction; if every body is still moving, skip this death */
    /* v1069F (user: many NPCs play their death animation instead of ragdolling): 8 bodies in total were kept, so every 9th
     * death released the oldest body back to the game's dead pose. Now only MOVING bodies count against GF_RAG_MAX; settled
     * ones are frozen (no physics) and stay down -- up to MAXR (48) slots in all before the oldest is let go. */
    int pick = -1, active = 0;
    for (int i = 0; i < MAXR; i++) { if (s_rg[i].hr && s_rg[i].state >= 1 && s_rg[i].state < 5) continue;
        if ((s_rg[i].state >= 1 && s_rg[i].state <= 3) || s_rg[i].state == 6) active++;
        else if (s_rg[i].state != 4 && pick < 0) pick = i; }
    if (active >= s_max) { RLOG("[rag] death ent=%08X: %d bodies still moving -- skipped\n", ent, active); return; }
    if (pick < 0) {
        double oldest = 1e300; pick = -1;
        for (int i = 0; i < MAXR; i++) if (!s_rg[i].hr && s_rg[i].state == 4 && s_rg[i].t0 < oldest) { oldest = s_rg[i].t0; pick = i; }
        if (pick < 0 && active < s_max) for (int i = 0; i < MAXR; i++) if (s_rg[i].hr && s_rg[i].state >= 1 && s_rg[i].state < 5) { pick = i; break; }
        if (pick < 0) { RLOG("[rag] death ent=%08X: %d bodies still moving -- skipped\n", ent, active); return; }
        RLOG("[rag] body %08X released (oldest) for a new one\n", s_rg[pick].ent);
    }
    rag_t* r = &s_rg[pick]; if (r->jh) jr_destroy(r->jh); memset(r, 0, sizeof *r);
    r->ent = ent; r->anim = anim; r->pal = anim_pal(anim); r->vt = rd32(ent); r->t0 = now_ms(); r->src38 = ent + 0x38u;
    r->shooter_w = V(rdf(sm + 0x60), rdf(sm + 0x64), rdf(sm + 0x68));
    r->hit_w = V(rdf(sm + 0x6C), rdf(sm + 0x70), rdf(sm + 0x74));
    r->dmg = rdf(sm + 0x78); if (!fok(r->dmg)) r->dmg = 0;
    if (!rag_bones(r, ent, anim)) { r->state = 0; return; }
    r->state = 1; r->hit_leg = r->hit_arm = -1; r->bhv_zone = -1; r->bhv_on = 0; r->bhv_last = -1; r->bh.prog = BHV_NONE; s_ndeath++;
    if (s_carhit) { v3 cv; const uint32_t car = player_car(&cv); m3 Rc; v3 Tc, Te; m3 Re;   /* v1069C */
      if (car && ent_xform(car, &Rc, &Tc) && ent_xform(ent, &Re, &Te)) {
          const float dx = Te.x - Tc.x, dz = Te.z - Tc.z, d = sqrtf(dx * dx + dz * dz), sp = sqrtf(cv.x * cv.x + cv.z * cv.z);
          if (d < 4.5f && sp > 2.0f) { r->car = car; r->car_vw = cv; r->car_t0 = now_ms();
              RLOG("[rag-car] v1069C ent=%08X killed by the player's car at %.1f m/s (%.0f km/h), %.2f m away\n", ent, sp, sp * 3.6f, d); }
          else RLOG("[rag-car] ent=%08X died, player in a car %.2f m away at %.1f m/s -- not a car hit\n", ent, d, sp);
      } }
    RLOG("[rag] v1069H DEATH #%d ent=%08X anim=%08X palette=%08X slots=%d bones=%d dmg=%.0f hit (%.2f %.2f %.2f) from (%.2f %.2f %.2f)\n",
         s_ndeath, ent, anim, r->pal, r->nslot, r->nbones, r->dmg, r->hit_w.x, r->hit_w.y, r->hit_w.z, r->shooter_w.x, r->shooter_w.y, r->shooter_w.z);
}

/* ------------------------------------------------------------------ start: snapshot + drivers + push */
static void read_particles(rag_t* r, v3* out)
{
    for (int b = 0; b < NP; b++) if (b != HED) out[b] = slot_pos(r->pal, r->slot[b]);
    out[HED] = add(out[NEC], mul(nrm(sub(out[NEC], out[THO])), 0.22f));
}
static float seg_dist(v3 p, v3 a, v3 b)
{
    const v3 ab = sub(b, a); const float L = dot(ab, ab);
    float t = L > 1e-8f ? dot(sub(p, a), ab) / L : 0.0f; if (t < 0) t = 0; if (t > 1.25f) t = 1.25f;   /* a bit past the end: hands, feet, head */
    return len(sub(p, add(a, mul(ab, t))));
}
static v3 body_fwd(const v3* p) { return nrm(cross(sub(p[HIR], p[HIL]), sub(p[THO], p[PEL]))); }
static int rag_start(rag_t* r, float dt)
{
    for (int s = 0; s < r->nslot; s++) {
        const uint32_t m = r->pal + (uint32_t)s * 64u;
        for (int k = 0; k < 4; k++) for (int c = 0; c < 3; c++) r->M0[s][k * 3 + c] = rdf(m + (uint32_t)k * 16u + (uint32_t)c * 4u);
    }
    for (int b = 0; b < NBN; b++) if (b != HED) {   /* the body bones must be plain rotation + position */
        const float* M = r->M0[r->slot[b]];
        for (int k = 0; k < 3; k++) { const float l = sqrtf(M[k * 3] * M[k * 3] + M[k * 3 + 1] * M[k * 3 + 1] + M[k * 3 + 2] * M[k * 3 + 2]);
            if (!(l > 0.5f && l < 2.0f)) { RLOG("[rag] ent=%08X: bone %s matrix row %d length %.3f -- not a bone matrix, skipped\n", r->ent, k_bn[b], k, l); return 0; } }
    }
    read_particles(r, r->p0);
    for (int i = 0; i < NP; i++) { r->p[i] = r->p0[i];
        v3 v = mul(sub(r->p0[i], r->snap1[i]), 1.0f / dt); const float sp = len(v); if (sp > 2.5f) v = mul(v, 2.5f / sp);
        r->pp[i] = sub(r->p0[i], mul(v, dt)); }
    for (int l = 0; l < NL; l++) r->rest[l] = len(sub(r->p0[k_links[l].a], r->p0[k_links[l].b]));
    /* limbs may not fold flatter than this -- but never more open than they start (kneeling / crouching NPCs: v1054V
     * pushed hips and ankles apart on the first frame and flung the body up) */
    r->minkn[0] = fminf(0.55f * (len(sub(r->p0[KNL], r->p0[HIL])) + len(sub(r->p0[ANL], r->p0[KNL]))), 0.9f * len(sub(r->p0[ANL], r->p0[HIL])));
    r->minkn[1] = fminf(0.55f * (len(sub(r->p0[KNR], r->p0[HIR])) + len(sub(r->p0[ANR], r->p0[KNR]))), 0.9f * len(sub(r->p0[ANR], r->p0[HIR])));
    r->minel[0] = fminf(0.45f * (len(sub(r->p0[ELL], r->p0[SHL])) + len(sub(r->p0[WRL], r->p0[ELL]))), 0.9f * len(sub(r->p0[WRL], r->p0[SHL])));
    r->minel[1] = fminf(0.45f * (len(sub(r->p0[ELR], r->p0[SHR])) + len(sub(r->p0[WRR], r->p0[ELR]))), 0.9f * len(sub(r->p0[WRR], r->p0[SHR])));
    { const v3 f = body_fwd(r->p0);
      const float k0 = dot(sub(r->p0[KNL], mul(add(r->p0[HIL], r->p0[ANL]), .5f)), f), k1 = dot(sub(r->p0[KNR], mul(add(r->p0[HIR], r->p0[ANR]), .5f)), f);
      const float e0 = dot(sub(r->p0[ELL], mul(add(r->p0[SHL], r->p0[WRL]), .5f)), f), e1 = dot(sub(r->p0[ELR], mul(add(r->p0[SHR], r->p0[WRR]), .5f)), f);
      r->knsgn[0] = fabsf(k0) > .02f ? (k0 > 0 ? 1.f : -1.f) : 0.f; r->knsgn[1] = fabsf(k1) > .02f ? (k1 > 0 ? 1.f : -1.f) : 0.f;
      r->elsgn[0] = fabsf(e0) > .02f ? (e0 > 0 ? 1.f : -1.f) : 0.f; r->elsgn[1] = fabsf(e1) > .02f ? (e1 > 0 ? 1.f : -1.f) : 0.f;
      /* straight limbs: knees bend so the knee goes forward of the hip-ankle line; elbows the other way */
      if (r->knsgn[0] == 0.f) r->knsgn[0] = r->knsgn[1] ? r->knsgn[1] : 1.f;
      if (r->knsgn[1] == 0.f) r->knsgn[1] = r->knsgn[0];
      if (r->elsgn[0] == 0.f) r->elsgn[0] = -r->knsgn[0];
      if (r->elsgn[1] == 0.f) r->elsgn[1] = -r->knsgn[1]; }
    r->ground = fminf(fminf(r->p0[ANL].y, r->p0[ANR].y) - 0.08f, fminf(r->p0[KNL].y, r->p0[KNR].y) - 0.06f);   /* standing or kneeling */
    r->low_start = (r->p0[PEL].y - r->ground) < 0.75f;
    if (r->ground > 0.15f || r->ground < -0.6f) r->ground = 0.0f;   /* NPC not standing on its own origin: use the origin */
    /* dead weight: the knees buckle (towards their bend side) and the hips drop, so the body crumples instead of
     * toppling like a plank */
    { const v3 f = body_fwd(r->p0);
      (void)f; }
    /* (knee buckle / hip drop applied after the hit type is known, below) */
    r->F0[0] = frame(sub(r->p0[LUM], r->p0[PEL]), sub(r->p0[HIR], r->p0[HIL]));
    r->F0[1] = frame(sub(r->p0[NEC], r->p0[THO]), sub(r->p0[SHR], r->p0[SHL]));
    /* which body part does each palette slot (74 in the game, incl. twist / clavicle / skirt / coat / face bones)
     * belong to?  v1054X: membership, not nearest segment -- a bone inside a limb's capsule past its joint belongs to that
     * limb (checked from the hand / foot inwards), the rest to the torso halves; skirt / coat bones at the top of the
     * thighs blend pelvis -> thigh. (v1054W's nearest-segment rule gave the top of a sleeve to the torso: "stub" arms.) */
    for (int s = 0; s < r->nslot; s++) {
        const v3 q = V(r->M0[s][9], r->M0[s][10], r->M0[s][11]);
        int best = -1, second = -1; float bscore = 1e30f, sscore = 1e30f, bt = 0;
        static const struct { int d; float tmin, tmax, rad, rad_end; } lim[] = {
            { D_LAL, 0.0f, 1.6f, .12f, .14f }, { D_LAR, 0.0f, 1.6f, .12f, .14f },   /* forearm + hand + fingers */
            { D_UAL, .04f, 1.0f, .13f, .13f }, { D_UAR, .04f, 1.0f, .13f, .13f },   /* upper arm (twist bones, sleeve) */
            { D_LLL, 0.0f, 1.45f, .14f, .24f }, { D_LLR, 0.0f, 1.45f, .14f, .24f }, /* shin + foot / toes */
            { D_ULL, .04f, 1.0f, .17f, .17f }, { D_ULR, .04f, 1.0f, .17f, .17f },   /* thigh */
            { D_HEAD, .05f, 1.8f, .16f, .16f },                                     /* head, face, hat */
        };
        for (unsigned li = 0; li < sizeof lim / sizeof lim[0]; li++) {
            const v3 a = r->p0[k_dj[lim[li].d]], b = r->p0[k_de[lim[li].d]], ab = sub(b, a);
            const float L2 = dot(ab, ab); if (L2 < 1e-6f) continue;
            const float t = dot(sub(q, a), ab) / L2;
            if (t <= lim[li].tmin || t > lim[li].tmax) continue;
            const float perp = len(sub(q, add(a, mul(ab, t < 1.0f ? t : 1.0f))));
            const float rad = t > 0.9f ? lim[li].rad_end : lim[li].rad;
            if (perp > rad) continue;
            const float score = perp / rad;
            if (score < bscore) { sscore = bscore; second = best; bscore = score; best = lim[li].d; bt = t; }
            else if (score < sscore) { sscore = score; second = lim[li].d; }
        }
        /* equally inside the left and the right limb (root between the feet, a belt buckle between the thighs): torso */
        if (best >= 0 && second >= 0 && sscore - bscore < 0.2f &&
            ((best == D_LLL && second == D_LLR) || (best == D_LLR && second == D_LLL) || (best == D_ULL && second == D_ULR) || (best == D_ULR && second == D_ULL)))
            best = -1;
        if (len(q) < 0.03f) best = -1;   /* root / motion bones sit at the model origin -> pelvis (v1054Y's floor rule also took the toes) */
        r->w2[s] = 0.0f; r->drv2[s] = (uint8_t)(best < 0 ? D_LT : best);
        if (best < 0) {   /* torso: lower or upper half, blended near the waist */
            const float dl = fminf(seg_dist(q, r->p0[PEL], r->p0[LUM]), seg_dist(q, r->p0[HIL], r->p0[HIR]));
            const float du = fminf(fminf(seg_dist(q, r->p0[LUM], r->p0[THO]), seg_dist(q, r->p0[THO], r->p0[NEC])), seg_dist(q, r->p0[SHL], r->p0[SHR]));
            best = dl <= du ? D_LT : D_UT;
            const float gap = fabsf(dl - du);
            if (gap < 0.08f) { r->drv2[s] = (uint8_t)(best == D_LT ? D_UT : D_LT); r->w2[s] = 0.5f * (1.0f - gap / 0.08f); }
        } else if ((best == D_ULL || best == D_ULR) && bt < 0.6f) {    /* skirt / coat / crotch: hangs from the pelvis, */
            r->drv2[s] = (uint8_t)best; r->w2[s] = 0.15f + 0.45f * bt / 0.6f;   /* follows the leg a little (v1055A) */
            best = D_LT;
        }
        r->drv[s] = (uint8_t)best;
    }
    { /* v1055B: the learned rig overrides the guess for every non-body slot */
      static const int8_t bdrv[NBODY] = { D_LT, D_LT, D_UT, D_HEAD, -1, D_UAL, D_LAL, D_LAL, D_UAR, D_LAR, D_LAR, D_ULL, D_LLL, D_LLL, D_ULR, D_LLR, D_LLR };
      int nsl = 0; const skt_t* T = rig_find(rig_key(r->anim, &nsl));
      if (T && T->n >= 40 && T->nslot == r->nslot) {
          int isbody[MAXS] = { 0 }; for (int b = 0; b < NBODY; b++) if (b != HED) isbody[r->slot[b]] = 1;
          int nblend = 0, changed = 0;
          for (int sl = 0; sl < r->nslot; sl++) {
              if (isbody[sl]) continue;
              const v3 q = V(r->M0[sl][9], r->M0[sl][10], r->M0[sl][11]);
              int b1 = -1, b2 = -1; float v1 = 1e30f, v2 = 1e30f;
              for (int b = 0; b < NBODY; b++) {
                  if (b == HED) continue;
                  const int bs = r->slot[b];
                  const float var = T->m2[sl * LS + bs] / (float)T->n;
                  const float dd = len(sub(q, V(r->M0[bs][9], r->M0[bs][10], r->M0[bs][11])));
                  const float sc = var + 1e-5f * dd * dd;          /* never moved: nearest body bone */
                  if (sc < v1) { if (b1 < 0 || bdrv[b1] != bdrv[b]) { v2 = v1; b2 = b1; } v1 = sc; b1 = b; }
                  else if (sc < v2 && bdrv[b] != bdrv[b1]) { v2 = sc; b2 = b; }
              }
              if (b1 < 0) continue;
              const uint8_t nd = (uint8_t)bdrv[b1];
              r->clothy[sl] = (v1 > 4e-4f && (nd == D_LT || nd == D_ULL || nd == D_ULR)) ? 1 : 0;   /* coat / skirt: animated like cloth */
              if (nd != r->drv[sl]) changed++;
              r->drv[sl] = nd; r->drv2[sl] = nd; r->w2[sl] = 0.0f;
              static const uint16_t adj[ND] = {   /* parts that share a joint (bit masks) */
                  (1 << D_UT) | (1 << D_ULL) | (1 << D_ULR), (1 << D_LT) | (1 << D_HEAD) | (1 << D_UAL) | (1 << D_UAR), (1 << D_UT),
                  (1 << D_UT) | (1 << D_LAL), (1 << D_UAL), (1 << D_UT) | (1 << D_LAR), (1 << D_UAR),
                  (1 << D_LT) | (1 << D_LLL), (1 << D_ULL), (1 << D_LT) | (1 << D_LLR), (1 << D_ULR) };
              /* v1055B log: a left finger blended with the RIGHT upper arm -> only blend across a joint */
              /* v1056B: and only for skin near the joint the two parts share (a finger blended with the upper arm stretched
               * the hand up to 50x: recorder slots 31 / 42) */
              const int d2 = b2 >= 0 ? bdrv[b2] : -1;
              const int child = d2 < 0 ? -1 : (k_dp[nd] == d2 ? nd : k_dp[d2] == nd ? d2 : (nd == D_UT && d2 == D_LT ? D_UT : nd == D_LT && d2 == D_UT ? D_UT : -1));
              const float jd = child < 0 ? 9.0f : len(sub(q, r->p0[child == D_UT ? LUM : k_dj[child]]));
              if (v1 > 4e-4f && b2 >= 0 && v2 < 4.0f * v1 && (adj[nd] & (1 << bdrv[b2])) && len(q) > 0.03f && jd < 0.15f) {
                  r->drv2[sl] = (uint8_t)bdrv[b2]; r->w2[sl] = fminf(0.5f, v1 / (v1 + v2)); nblend++; r->clothy[sl] = 0; }   /* blended = waist skin at a joint, not a hanging tail */
          }
          r->learned = 1;
          if (!r->hr) RLOG("[rag] ent=%08X: learned rig (%d samples): %d slot(s) reassigned vs the guess, %d blended\n", r->ent, T->n, changed, nblend);
      } else if (!r->hr) RLOG("[rag] ent=%08X: rig not learned yet (%d samples) -- guessed bone assignment\n", r->ent, T ? T->n : 0);
    }
#ifdef GF_RAG_REPLAY   /* offline replay harness only: the recorded (learned) assignment */
    { extern const uint8_t* g_rp_drv; extern const uint8_t* g_rp_drv2; extern const float* g_rp_w2;
      { extern const uint8_t* g_rp_cloth; if (g_rp_cloth) { r->learned = 1; for (int sl = 0; sl < r->nslot; sl++) r->clothy[sl] = g_rp_cloth[sl];
          /* v1057B-4 (user: the dead body's leg looks broken): the learned flags also marked slots 5-10 (body bones of the
           * 74-slot rig) as cloth -> they swung as coat panels and bent the hip / leg skin. Coat / skirt = the ring 60.. */
          if (r->nslot > 62) for (int sl = 0; sl < 60 && sl < r->nslot; sl++) r->clothy[sl] = 0; } }   /* v1057B-5: any rig with the ring (not only 74 slots) */
      if (g_rp_drv) for (int sl = 0; sl < r->nslot; sl++) { r->drv[sl] = g_rp_drv[sl]; r->drv2[sl] = g_rp_drv2[sl]; r->w2[sl] = g_rp_w2[sl];
          if (r->w2[sl] > 0) { const int nd = r->drv[sl], d2 = r->drv2[sl]; const v3 q = V(r->M0[sl][9], r->M0[sl][10], r->M0[sl][11]);
              const int child = k_dp[nd] == d2 ? nd : k_dp[d2] == nd ? d2 : ((nd == D_UT && d2 == D_LT) || (nd == D_LT && d2 == D_UT)) ? D_UT : -1;
              const float jd = child < 0 ? 9.0f : len(sub(q, r->p0[child == D_UT ? LUM : k_dj[child]]));
              if (jd >= 0.15f) { r->w2[sl] = 0; r->drv2[sl] = (uint8_t)nd; } } } }
#endif
    for (int b = 0; b < NBODY; b++) if (b != HED) r->drv[r->slot[b]] = (uint8_t)(b == PEL || b == LUM || b == HIL || b == HIR ? D_LT : b == THO || b == NEC || b == SHL || b == SHR ? D_UT :
        b == ELL ? D_UAL : b == WRL ? D_LAL : b == ELR ? D_UAR : b == WRR ? D_LAR : b == KNL ? D_ULL : b == ANL ? D_LLL : b == KNR ? D_ULR : D_LLR);
    /* knee slot belongs to the thigh's end -> make the knee bone rotate with the shin, elbow with the forearm */
    r->drv[r->slot[KNL]] = D_LLL; r->drv[r->slot[KNR]] = D_LLR; r->drv[r->slot[ELL]] = D_LAL; r->drv[r->slot[ELR]] = D_LAR;
    r->drv[r->slot[SHL]] = D_UAL; r->drv[r->slot[SHR]] = D_UAR; r->drv[r->slot[HIL]] = D_ULL; r->drv[r->slot[HIR]] = D_ULR;
    r->drv[r->slot[NEC]] = D_HEAD;
    if (!r->hr) { /* v1056C: coat / skirt bones = non-body slots on the pelvis or thighs, between the knees and the hips, around
       * the body (not on its axis) -- in the game's 74-bone rig the lower ring 65..73 -- become cloth pendulums */
      int isb[MAXS] = { 0 }; for (int b = 0; b < NBN; b++) if (b != HED && r->slot[b] >= 0) isb[r->slot[b]] = 1;
      v3 up = nrm(sub(r->p0[LUM], r->p0[PEL]));
      { const float* Mp = r->M0[r->slot[PEL]]; float bd = 0; v3 bu = up;
        for (int k = 0; k < 3; k++) { v3 rw = V(Mp[k * 3], Mp[k * 3 + 1], Mp[k * 3 + 2]); const float dd = dot(rw, up);
            if (fabsf(dd) > bd) { bd = fabsf(dd); bu = nrm(mul(rw, dd < 0 ? -1.0f : 1.0f)); } }
        up = bu; }   /* the pelvis bone's own axis: steadier than pelvis->lumbar when the spine bends */
      float ang[12]; r->ncl = 0;
      for (int sl = 0; sl < r->nslot && r->ncl < 12; sl++) {
          if (isb[sl]) continue;
          const int d = r->drv[sl];
          if (!(d == D_LT || d == D_ULL || d == D_ULR)) continue;
          const v3 q = V(r->M0[sl][9], r->M0[sl][10], r->M0[sl][11]);
          const float hh = dot(sub(q, r->p0[PEL]), up);
          if (len(q) < 0.03f || hh < -0.30f || hh > 0.15f) continue;
          if (r->learned ? !r->clothy[sl] : (hh > -0.03f)) continue;   /* learned: swings like cloth; else: below the pelvis */
          if (r->nslot > 62 && sl < 60) continue;   /* v1057B-6: body bones (5-10, 31 learned as cloth on men / women) never swing as coat */
          v3 rad = sub(q, r->p0[PEL]); rad = sub(rad, mul(up, dot(rad, up)));
          if (len(rad) < 0.06f) continue;
          if (seg_dist(q, r->p0[HIL], r->p0[KNL]) < 0.05f || seg_dist(q, r->p0[HIR], r->p0[KNR]) < 0.05f) continue;   /* on a thigh's axis: a thigh bone */
          const int k = r->ncl++;
          r->cl_slot[k] = sl; r->drv[sl] = D_LT; r->w2[sl] = 0.0f;
          r->cl_A0[k] = add(q, mul(up, 0.12f)); r->cl_T0[k] = sub(q, mul(up, 0.25f));
          r->cl_L[k] = len(sub(r->cl_T0[k], r->cl_A0[k]));
          { const float dl = seg_dist(q, r->p0[HIL], r->p0[KNL]), dr = seg_dist(q, r->p0[HIR], r->p0[KNR]);   /* v1057R: share of the left thigh */
            r->cl_wl[k] = dl + dr > 1e-5f ? dr / (dl + dr) : 0.5f; }
          const v3 vel = mul(sub(r->p[PEL], r->pp[PEL]), 1.0f);
          r->cl_T[k] = r->cl_T0[k]; r->cl_Tp[k] = sub(r->cl_T0[k], vel);
          ang[k] = atan2f(dot(rad, nrm(sub(r->p0[HIR], r->p0[HIL]))), dot(rad, body_fwd(r->p0)));
      }
      /* ring neighbours by angle around the hips */
      int ord[12]; for (int k = 0; k < r->ncl; k++) ord[k] = k;
      for (int i = 0; i < r->ncl; i++) for (int j = i + 1; j < r->ncl; j++) if (ang[ord[j]] < ang[ord[i]]) { const int t = ord[i]; ord[i] = ord[j]; ord[j] = t; }
      for (int i = 0; i < r->ncl; i++) {
          const int k = ord[i], a = ord[(i + r->ncl - 1) % r->ncl], b = ord[(i + 1) % r->ncl];
          r->cl_nb[k][0] = r->ncl > 2 || i > 0 ? a : -1; r->cl_nb[k][1] = r->ncl > 2 || i + 1 < r->ncl ? b : -1;
          for (int e = 0; e < 2; e++) r->cl_nbL[k][e] = r->cl_nb[k][e] >= 0 ? len(sub(r->cl_T0[k], r->cl_T0[r->cl_nb[k][e]])) : 0;
      }
      if (r->ncl) { char sb[96]; int o = 0; for (int c = 0; c < r->ncl && o < 88; c++) o += snprintf(sb + o, sizeof sb - o, " %d", r->cl_slot[c]);
          RLOG("[rag] ent=%08X: %d coat/skirt bone(s) hang as cloth (slots%s, %s)\n", r->ent, r->ncl, sb, r->learned ? "learned" : "guessed"); } }
    /* v1055D: toe / finger tips = the slot of THAT foot / hand (by the assignment above) farthest out past the ankle /
     * wrist. v1055C picked by distance only and glued hands to the pelvis / knee and toes to the root at the origin
     * (recorder: slots 4, 5, 12, 0). No such slot -> the tip sits on the joint (links of length 0). */
    { const int tip[4] = { TOL, TOR, FIL, FIR }, base[4] = { ANL, ANR, WRL, WRR }, prox[4] = { KNL, KNR, ELL, ELR }, dv[4] = { D_LLL, D_LLR, D_LAL, D_LAR };
      for (int k = 0; k < 4; k++) {
          const v3 b0 = r->p0[base[k]], ax = sub(b0, r->p0[prox[k]]); const float L2 = dot(ax, ax);
          int best = r->slot[base[k]]; float bd = 0.04f;
          for (int sl = 0; sl < r->nslot && L2 > 1e-6f; sl++) {
              if (r->drv[sl] != dv[k] || sl == r->slot[base[k]]) continue;
              const v3 q = V(r->M0[sl][9], r->M0[sl][10], r->M0[sl][11]);
              const float t = dot(sub(q, r->p0[prox[k]]), ax) / L2, d = len(sub(q, b0));
              if (t < 0.85f || d > 0.30f || d <= bd) continue;
              bd = d; best = sl;
          }
          const int ti = tip[k];
          r->slot[ti] = best;
          const v3 vel = sub(r->p[base[k]], r->pp[base[k]]);   /* same start motion as its joint */
          r->p0[ti] = r->p[ti] = slot_pos(r->pal, best); r->pp[ti] = sub(r->p[ti], vel);
      }
      for (int l = 0; l < NL; l++) if (k_links[l].a >= NBODY || k_links[l].b >= NBODY) r->rest[l] = len(sub(r->p0[k_links[l].a], r->p0[k_links[l].b]));
      if (!r->hr) RLOG("[rag] ent=%08X tips: toes %d/%d fingers %d/%d\n", r->ent, r->slot[TOL], r->slot[TOR], r->slot[FIL], r->slot[FIR]); }
    for (int b = 0; b < NBN; b++) if (b != HED) r->w2[r->slot[b]] = 0.0f;   /* the body bones themselves stay rigid */
    { static int dumped = 0;   /* once per session: which body part every palette slot follows (for screenshots) */
      if (!dumped && !r->hr) { dumped = 1; static const char* const dn[ND] = { "LT", "UT", "HEAD", "UAL", "LAL", "UAR", "LAR", "ULL", "LLL", "ULR", "LLR" };
        RLOG("[rag] slot map (model pos -> part [+ blend]):");
        for (int s2 = 0; s2 < r->nslot; s2++) { if (s2 % 4 == 0) RLOG("\n[rag]  ");
            RLOG(" %2d(%5.2f %5.2f %5.2f)%s", s2, r->M0[s2][9], r->M0[s2][10], r->M0[s2][11], dn[r->drv[s2]]);
            if (r->w2[s2] > 0) RLOG("+%s%.1f", dn[r->drv2[s2]], r->w2[s2]); }
        RLOG("\n"); } }
    for (int i = 0; i < NMD; i++) r->mind[i] = k_mind[i].frac * len(sub(r->p0[k_mind[i].a], r->p0[k_mind[i].b]));
    r->fwd_sign = body_fwd(r->p0).z >= 0.0f ? 1.0f : -1.0f;   /* v1069H: the model faces +z */
    if (r->hr) return 1;   /* v1069H hit reaction: drivers + tips are all it needs */
    /* push from the shot: world direction shooter -> hit, into the model frame */
    if (!ent_xform(r->ent, &r->Rd, &r->Td)) return 0;
    r->conv = s_conv;
    v3 dw = sub(r->hit_w, r->shooter_w); dw.y = 0; const float hl = len(dw);
    const int have_hit = fok(r->hit_w.x) && hl > 0.05f && hl < 200.0f;
    if (have_hit) {
        dw = nrm(dw); dw.y = -0.05f;                           /* never lift the body: shots push back, slightly down */
        { /* convention vote: distance of the hit point to the nearest bone capsule, both ways */
          const m3 RdT = tr(&r->Rd); float dbest[2];
          for (int c = 0; c < 2; c++) {
              const v3 h = mv(c ? &RdT : &r->Rd, sub(r->hit_w, r->Td)); float bdc = 1e30f;
              for (int l = 0; l < NL; l++) if (k_links[l].stiff >= 1.0f && k_links[l].a < NBODY && k_links[l].b < NBODY)
                  bdc = fminf(bdc, seg_dist(h, r->p0[k_links[l].a], r->p0[k_links[l].b]));
              dbest[c] = bdc;
          }
          if (dbest[0] + 0.05f < dbest[1]) s_vote[0]++; else if (dbest[1] + 0.05f < dbest[0]) s_vote[1]++;
          const int was = s_conv; s_conv = (s_vote[1] >= s_vote[0] + 2) ? 1 : (s_vote[0] >= s_vote[1] + 2) ? 0 : s_conv;
          const v3 to_sh = nrm(V(r->shooter_w.x - r->Td.x, 0, r->shooter_w.z - r->Td.z));
          RLOG("[rag] frame check: hit-to-body %.2f m (rows) vs %.2f m (transposed); facing the shooter %+.2f (rows) %+.2f (transposed); votes %d:%d -> %s%s\n",
               dbest[0], dbest[1], r->Rd.m[6] * to_sh.x + r->Rd.m[8] * to_sh.z, r->Rd.m[2] * to_sh.x + r->Rd.m[8] * to_sh.z, s_vote[0], s_vote[1],
               s_conv ? "transposed" : "rows", was != s_conv ? " (SWITCHED)" : ""); }
        r->conv = s_conv;
        const m3 Md = w2mc(&r->Rd, r->conv);
        const v3 dm = mv(&Md, dw);                             /* model = M (w - T) */
        const v3 hm = mv(&Md, sub(r->hit_w, r->Td));
        int k = THO; float bd = 1e30f;
        for (int i = 0; i < NP; i++) { const float d = len(sub(r->p0[i], hm)); if (d < bd) { bd = d; k = i; } }
        const int head = (k == HED || k == NEC);
        r->headshot = head; r->core = !head && (k == THO || k == LUM || k == PEL || k == SHL || k == SHR);
        r->hit_leg = (k == HIL || k == KNL || k == ANL) ? 0 : (k == HIR || k == KNR || k == ANR) ? 1 : -1;
        r->hit_arm = (k == ELL || k == WRL) ? 0 : (k == ELR || k == WRR) ? 1 : -1;
        if (r->hit_arm >= 0) r->core = 1;   /* arm kill: the body crumples like a torso hit, the arm only jerks */
        /* a sharp jolt where the bullet lands; the heavy torso hardly travels (no flying bodies) */
        float sp = (2.6f + r->dmg / 4000.0f) * s_push; if (sp > 4.5f * s_push) sp = 4.5f * s_push;
        if (head) sp = 4.8f * s_push;                          /* head whips back */
        if (r->core) { sp = (1.5f + r->dmg / 5000.0f) * s_push; if (sp > 2.6f * s_push) sp = 2.6f * s_push; }   /* chest: a punch, not a launch */
        if (r->hit_leg >= 0 || r->hit_arm >= 0) { sp = 2.4f * s_push; }
        for (int i = 0; i < NP; i++) {
            float w;
            if (i == k) w = 1.0f;
            else if (head) w = (i == HED || i == NEC) ? 0.75f : (i == THO || i == SHL || i == SHR) ? 0.10f : 0.0f;
            else if (r->hit_leg >= 0) {   /* the shot leg is knocked out from under the body */
                const int sameleg = r->hit_leg ? (i == HIR || i == KNR || i == ANR) : (i == HIL || i == KNL || i == ANL);
                w = sameleg ? 0.7f : 0.03f; }
            else if (r->hit_arm >= 0) {   /* the arm jerks back, the body is not shoved */
                const int samearm = r->hit_arm ? (i == SHR || i == ELR || i == WRR) : (i == SHL || i == ELL || i == WRL);
                w = samearm ? (i == SHL || i == SHR ? 0.15f : 0.55f) : 0.0f; }
            else { const float dd = len(sub(r->p0[i], r->p0[k])); w = dd < 0.2f ? 0.45f : dd < 0.4f ? 0.15f : 0.02f; }
            r->pp[i] = sub(r->pp[i], mul(dm, sp * w * dt));     /* adds velocity sp*w along dm */
        }
        if (r->hit_leg >= 0) {   /* collapse towards the wounded side, a little forward and down */
            const int hs = r->hit_leg ? HIR : HIL, ho = r->hit_leg ? HIL : HIR;
            v3 side = sub(r->p0[hs], r->p0[ho]); side.y = 0; side = nrm(side);
            for (int i = 0; i < NP; i++) if (i == PEL || i == LUM || i == THO || i == NEC || i == HED || i == SHL || i == SHR) {
                const float up = (i == PEL || i == LUM) ? 0.0f : 1.0f;   /* the top tips over more than the hips */
                r->pp[i] = sub(r->pp[i], mul(add(mul(side, 0.6f + 0.6f * up), V(0, -0.5f, 0)), dt)); }
        }
        r->kill_spin = 1;
        RLOG("[rag] ent=%08X start: push %.1f m/s at %s%s (hit %.2f m from it), %d slots on 11 drivers, ground %.2f\n",
             r->ent, sp, k_bn[k], head ? " (headshot)" : r->hit_leg >= 0 ? " (leg: knocked out)" : r->hit_arm >= 0 ? " (arm: jerk + crumple)" : r->core ? " (core: crumple)" : "", bd, r->nslot, r->ground);
        /* dying: the wound is stored in its torso half's frame; the nearer hand goes to it, a leg draws up, a few weak
         * breaths -- short for heart shots, longer for gut / limb hits, none for headshots */
        if (!head) {
            const int upper = (k == THO || k == NEC || k == SHL || k == SHR || k == ELL || k == ELR || k == WRL || k == WRR);
            const int torso = (k == THO || k == LUM || k == PEL || k == SHL || k == SHR || k == HIL || k == HIR);
            v3 wpt = hm;   /* wound point on the body: for a limb hit, the limb joint itself */
            if (!torso) wpt = r->p0[k];
            r->wound_half = upper; const int j = upper ? THO : PEL; const m3 Ft = tr(&r->F0[upper]);
            r->wound_loc = mv(&Ft, sub(wpt, r->p0[j])); r->have_wound = 1;
            const float dL = len(sub(r->p0[WRL], wpt)), dR = len(sub(r->p0[WRR], wpt));
            r->clutch_arm = (k == WRL || k == ELL) ? 1 : (k == WRR || k == ELR) ? 0 : (dL < dR ? 0 : 1);   /* 0 = left hand, 1 = right */
            r->writhe_leg = (k == KNL || k == ANL || k == HIL) ? 0 : (k == KNR || k == ANR || k == HIR) ? 1 : (r->clutch_arm ^ 1);
            const int heart = upper && torso && r->dmg >= 3000.0f;
            r->writhe_end = heart ? 2.2f : r->dmg >= 3000.0f ? 4.0f : 5.5f;
            r->clutch_str = torso ? 1.0f : 0.6f;
        }
        /* v1075R: behaviour zone + side + push direction (model space, horizontal) */
        r->bhv_zone = head ? BHV_Z_HEAD : r->hit_leg >= 0 ? BHV_Z_LEG : r->hit_arm >= 0 ? BHV_Z_ARM
                    : (k == THO || k == NEC || k == SHL || k == SHR) ? BHV_Z_CHEST : (k == LUM || k == PEL) ? BHV_Z_GUT : BHV_Z_OTHER;
        r->bhv_side = r->hit_leg >= 0 ? r->hit_leg : r->hit_arm >= 0 ? r->hit_arm : r->have_wound ? r->clutch_arm : 0;
        { const v3 dh = nrm(V(dm.x, 0, dm.z)); r->bhv_dir[0] = dh.x; r->bhv_dir[1] = 0; r->bhv_dir[2] = dh.z; }
    } else RLOG("[rag] ent=%08X start: no usable hit direction (melee?) -- falls with the animation's motion only\n", r->ent);
    /* dead weight: knees buckle towards their bend side and the hips drop -- hardest for heart / gut shots, where the
     * chest also folds forward over the wound once the punch has landed */
    { const v3 f = body_fwd(r->p0);
      const float kb = r->core ? 1.4f : r->headshot ? 0.9f : 0.6f, hd = r->core ? 1.0f : r->headshot ? 0.7f : 0.4f;
      for (int j = 0; j < 2; j++) { const int kn = j ? KNR : KNL; r->pp[kn] = sub(r->pp[kn], mul(f, kb * r->knsgn[j] * dt)); }
      for (int i = 0; i < NP; i++) if (i == PEL || i == LUM || i == HIL || i == HIR) r->pp[i].y += hd * dt;
      if (r->core) { const v3 fold = mul(f, -0.6f * r->knsgn[0]);   /* knees bend forward -> chest folds the same way */
        for (int i = 0; i < NP; i++) if (i == NEC || i == HED) r->pp[i] = sub(r->pp[i], mul(fold, dt)); } }
    if (s_rec) {
        const m3 Md = w2mc(&r->Rd, r->conv), MdT = tr(&Md);
        rec_w("RAGS", 4); rec_u32(r->ent); rec_u32((uint32_t)r->nslot); rec_u32(NP);
        for (int i = 0; i < NP; i++) rec_u32((uint32_t)r->slot[i]);
        rec_w(r->drv, (size_t)r->nslot); rec_w(r->drv2, (size_t)r->nslot); rec_w(r->w2, sizeof(float) * (size_t)r->nslot);
        for (int sl = 0; sl < r->nslot; sl++) rec_v3(add(r->Td, mv(&MdT, V(r->M0[sl][9], r->M0[sl][10], r->M0[sl][11]))));
        rec_w("ROWS", 4); rec_w(r->M0, sizeof(float) * 12 * (size_t)r->nslot);   /* v1055D: model-space rest matrices, for offline replays */
        for (int i = 0; i < NP; i++) rec_v3(add(r->Td, mv(&MdT, r->p0[i])));
        rec_f(r->ground); rec_v3(r->Td); rec_v3(r->hit_w); rec_v3(r->shooter_w); rec_f(r->dmg);
        rec_u32((uint32_t)(r->headshot | (r->core << 1) | ((r->hit_leg + 1) << 2) | ((r->hit_arm + 1) << 4) | (r->low_start << 6)));
        fflush(s_rec);
    }
    return 1;
}

/* ------------------------------------------------------------------ simulation */
static void solve_links(rag_t* r, int strict_only)
{
    for (int l = 0; l < NL; l++) {
        if (strict_only && k_links[l].stiff < 1.0f) continue;
        const int a = k_links[l].a, b = k_links[l].b;
        const v3 d = sub(r->p[a], r->p[b]); const float L = len(d); if (L < 1e-6f) continue;
        const float wa = 1.0f / k_mass[a], wb = 1.0f / k_mass[b];
        const v3 c = mul(d, (L - r->rest[l]) / L * k_links[l].stiff / (wa + wb));
        r->p[a] = sub(r->p[a], mul(c, wa)); r->p[b] = add(r->p[b], mul(c, wb));
    }
}
static float tone_of(const rag_t* r, int grp, float t)   /* muscle tone 0..1 for a group, t seconds after death */
{
    float base, tau, hold = 0.0f;
    if (r->headshot) { base = 0.06f; tau = 0.12f; }                        /* lights out */
    else if (r->core) {                                                    /* heart / gut: the legs go at once, the body curls */
        if (grp == 0) { base = 0.40f; tau = 0.45f; } else if (grp == 1) { base = 0.30f; tau = 0.35f; } else { base = 0.25f; tau = 0.08f; } }
    else if (grp == 0) { base = 0.55f; tau = 0.60f; }                      /* limb hits: spine braces a while */
    else if (grp == 1) { base = 0.45f; tau = 0.45f; }
    else { base = 0.70f; tau = 0.12f; hold = 0.12f; }                      /* legs hold an instant, then give out */
    if (grp == 2 && r->low_start) { base = 0.06f; tau = 0.10f; hold = 0.0f; }   /* kneeling: the legs just fold, they hold nothing up */
    if (t > 1.4f) return 0.0f;                                             /* dead is dead: no twitching on the ground */
    float k = t < hold ? base : base * expf(-(t - hold) / tau);
    if (r->p[PEL].y < r->ground + 0.3f) k *= 0.3f;                          /* already down: let it go */
    k *= s_tone; return k > 0.95f ? 0.95f : k;
}
static void motors(rag_t* r, float h, float t)
{
    if (!r->have_anim || r->corpse) return;   /* the corpse restarts the death anim from the top: not a target */
    m3 Rrel[2];
    { const m3 Fs = frame(sub(r->p[LUM], r->p[PEL]), sub(r->p[HIR], r->p[HIL])), Fa = frame(sub(r->a[LUM], r->a[PEL]), sub(r->a[HIR], r->a[HIL]));
      const m3 t2 = tr(&Fa); Rrel[0] = mm(&Fs, &t2); }
    { const m3 Fs = frame(sub(r->p[NEC], r->p[THO]), sub(r->p[SHR], r->p[SHL])), Fa = frame(sub(r->a[NEC], r->a[THO]), sub(r->a[SHR], r->a[SHL]));
      const m3 t2 = tr(&Fa); Rrel[1] = mm(&Fs, &t2); }
    float kg[3]; for (int g = 0; g < 3; g++) { const float k = tone_of(r, g, t); kg[g] = 1.0f - powf(1.0f - k, h * 60.0f); }   /* per-frame tone -> this substep */
    for (int i = 0; i < NM; i++) {
        const int pa = k_mot[i].par, ch = k_mot[i].ch;
        const v3 tgt = add(r->p[pa], mv(&Rrel[k_mot[i].half], sub(r->a[ch], r->a[pa])));
        float kk = kg[k_mot[i].grp];
        if (k_mot[i].grp == 2 && r->hit_leg >= 0) {   /* shot leg: gone at once; other leg: tries to stand ~0.25 s */
            const int left = (ch == HIL || ch == KNL || ch == ANL || ch == TOL);
            const int shot = (r->hit_leg == 0) == left;
            if (shot) kk *= 0.05f; else if (t < 0.15f && !r->low_start) kk = 1.0f - powf(1.0f - 0.4f * s_tone, h * 60.0f);
        }
        const v3 e = mul(sub(tgt, r->p[ch]), kk);
        const float wp = 1.0f / k_mass[pa], wc = 1.0f / k_mass[ch];
        r->p[ch] = add(r->p[ch], mul(e, wc / (wp + wc))); r->p[pa] = sub(r->p[pa], mul(e, wp / (wp + wc)));   /* internal: the body still falls */
    }
}
static v3 wound_now(const rag_t* r)   /* the wound point, moving with its torso half */
{
    const int u = r->wound_half; const int j = u ? THO : PEL;
    const m3 F = u ? frame(sub(r->p[NEC], r->p[THO]), sub(r->p[SHR], r->p[SHL])) : frame(sub(r->p[LUM], r->p[PEL]), sub(r->p[HIR], r->p[HIL]));
    return add(r->p[j], mv(&F, r->wound_loc));
}
static void pull(rag_t* r, int i, v3 tgt, float k)   /* soft pull of one particle, balanced against the torso */
{
    const v3 e = mul(sub(tgt, r->p[i]), k);
    r->p[i] = add(r->p[i], e);
    const int anchor = (i == WRL || i == WRR || i == ELL || i == ELR) ? THO : PEL;
    r->p[anchor] = sub(r->p[anchor], mul(e, k_mass[i] / k_mass[anchor]));
}
static void dying(rag_t* r, float h, float t)
{
    if (!r->have_wound || r->headshot || t > r->writhe_end + 1.0f) return;
    const float fade = t < r->writhe_end ? 1.0f : 1.0f - (t - r->writhe_end);           /* last second: let go */
    const float per = h * 60.0f;                                                          /* per-frame strength -> substep */
    /* the hand goes to the wound and presses on it (from 0.08 s on, through the fall, while dying) */
    if (t > 0.08f) {
        const int wr = r->clutch_arm ? WRR : WRL, el = r->clutch_arm ? ELR : ELL;
        const v3 w = wound_now(r);
        const v3 out = nrm(sub(w, r->p[r->wound_half ? THO : PEL]));
        const v3 tgt = add(w, mul(out, 0.05f));                                           /* on the skin, not inside */
        const float k = (t < 1.0f ? 0.10f : 0.05f) * r->clutch_str * fade;
        pull(r, wr, tgt, 1.0f - powf(1.0f - k, per));
        pull(r, el, add(mul(add(r->p[r->clutch_arm ? SHR : SHL], tgt), 0.5f), mul(out, 0.08f)), 1.0f - powf(1.0f - k * 0.3f, per));
    }
    /* on the ground: one leg slowly draws up and slides back, and a few weak breaths lift the chest */
    if (t > 1.2f && r->p[PEL].y < r->ground + 0.35f) {
        /* (v1055A: the slow leg draw-up is gone -- on a body lying on its back it lifted the leg into the air) */
        const float br = sinf((t - 1.2f) * 4.2f); /* ~1.5 s breaths, weakening */
        if (br > 0.0f) { const float a = 0.0006f * br * fade * per; r->p[THO].y += a; r->p[NEC].y += a * 0.7f; }
    }
}
/* each torso half moves as one rigid piece (pelvis+lumbar+hips, chest+neck+shoulders), centre of mass kept: the
 * drawn legs / arms (chained from the torso) then sit exactly where the simulated ones are -- no limbs in the floor */
static __attribute__((unused)) void rigid_halves(rag_t* r, float k)   /* unused: both halves are already rigid by their 6 links each */
{
    for (int u = 0; u < 2; u++) {
        const int mem[4] = { u ? THO : PEL, u ? NEC : LUM, u ? SHL : HIL, u ? SHR : HIR };
        const m3 F = u ? frame(sub(r->p[NEC], r->p[THO]), sub(r->p[SHR], r->p[SHL])) : frame(sub(r->p[LUM], r->p[PEL]), sub(r->p[HIR], r->p[HIL]));
        const m3 t = tr(&r->F0[u]); const m3 Q = mm(&F, &t);
        /* shape matching about the centre of mass (v1054Z: about the joint it spun the body up) */
        v3 c = V(0, 0, 0), c0 = V(0, 0, 0); float M = 0;
        for (int i = 0; i < 4; i++) { const float m = k_mass[mem[i]]; c = add(c, mul(r->p[mem[i]], m)); c0 = add(c0, mul(r->p0[mem[i]], m)); M += m; }
        c = mul(c, 1.0f / M); c0 = mul(c0, 1.0f / M);
        for (int i = 0; i < 4; i++) {
            const v3 tg = add(c, mv(&Q, sub(r->p0[mem[i]], c0)));
            r->p[mem[i]] = add(r->p[mem[i]], mul(sub(tg, r->p[mem[i]]), k));
        }
    }
}
static void min_dists(rag_t* r)
{
    for (int i = 0; i < NMD; i++) {
        const int a = k_mind[i].a, b = k_mind[i].b; const v3 d = sub(r->p[a], r->p[b]); const float L = len(d);
        if (L >= r->mind[i] || L < 1e-6f) continue;
        const float wa = 1.0f / k_mass[a], wb = 1.0f / k_mass[b];
        const v3 c = mul(d, (L - r->mind[i]) / L / (wa + wb));
        r->p[a] = sub(r->p[a], mul(c, wa)); r->p[b] = add(r->p[b], mul(c, wb));
    }
}
/* the chest may twist at most +-30 deg against the hips around the spine (v1054X: torsos spun round while falling) */
static void rot_about(v3* p, v3 c, v3 ax, float ang) { const float cs = cosf(ang), sn = sinf(ang); const v3 d = sub(*p, c);
    *p = add(c, add(add(mul(d, cs), mul(cross(ax, d), sn)), mul(ax, dot(ax, d) * (1 - cs)))); }
static void twist_limit(rag_t* r)
{
    const v3 ax = nrm(sub(r->p[THO], r->p[PEL]));
    v3 h = sub(r->p[HIR], r->p[HIL]), sh = sub(r->p[SHR], r->p[SHL]);
    h = sub(h, mul(ax, dot(h, ax))); sh = sub(sh, mul(ax, dot(sh, ax)));
    if (len(h) < 1e-4f || len(sh) < 1e-4f) return;
    h = nrm(h); sh = nrm(sh);
    const float ang = atan2f(dot(cross(h, sh), ax), dot(h, sh)), lim = 0.52f;
    if (fabsf(ang) <= lim) return;
    const float ex = ang > 0 ? ang - lim : ang + lim;
    const v3 cu = r->p[THO], cl = r->p[PEL];
    const int up[] = { SHL, SHR, ELL, ELR, WRL, WRR, NEC, HED, FIL, FIR }, lo[] = { HIL, HIR, KNL, KNR, ANL, ANR, TOL, TOR };
    for (unsigned i = 0; i < sizeof up / sizeof up[0]; i++) { rot_about(&r->p[up[i]], cu, ax, -ex * 0.6f); }
    for (unsigned i = 0; i < sizeof lo / sizeof lo[0]; i++) { rot_about(&r->p[lo[i]], cl, ax, ex * 0.4f); }
}
/* spin about the vertical through the centre of mass: removed from the hit (kill_spin), damped while falling */
static void yaw_damp(rag_t* r, float keep, float h)
{
    float M = 0; v3 c = V(0, 0, 0);
    for (int i = 0; i < NP; i++) { M += k_mass[i]; c = add(c, mul(r->p[i], k_mass[i])); }
    c = mul(c, 1.0f / M);
    float L = 0, I = 0;
    for (int i = 0; i < NP; i++) {
        const v3 d = sub(r->p[i], c), v = mul(sub(r->p[i], r->pp[i]), 1.0f / h);
        L += k_mass[i] * (d.z * v.x - d.x * v.z); I += k_mass[i] * (d.x * d.x + d.z * d.z);
    }
    if (I < 1e-4f) return;
    const float w = L / I * (1.0f - keep);   /* angular velocity to remove (about +y) */
    for (int i = 0; i < NP; i++) {
        const v3 d = sub(r->p[i], c);
        const v3 dv = V(w * d.z, 0, -w * d.x);   /* omega x d for omega = (0, w, 0) */
        r->pp[i] = add(r->pp[i], mul(dv, h));    /* lowers the velocity by dv */
    }
}
static void sim_step(rag_t* r, float dt)
{
    const float t = (float)((now_ms() - r->t0) / 1000.0);
    int down = 0; for (int i = 0; i < NP; i++) if (r->p[i].y < r->ground + k_rad[i] + 0.03f) down++;
    /* more damping the longer it lies: limbs come to rest instead of sprawling */
    /* air: almost no damping (a raised leg falls back down, nothing freezes mid-air); resting contact is damped below */
    float damp = t < 3.0f ? 0.997f : 0.99f;
    (void)down;
    const float g = s_grav * dt * dt, vcap = 6.0f * dt;
    for (int i = 0; i < NP; i++) {
        v3 v = mul(sub(r->p[i], r->pp[i]), damp);
        { const float l = len(v); if (l > vcap) v = mul(v, vcap / l); }
        r->pp[i] = r->p[i];
        r->p[i] = add(r->p[i], add(v, V(0, -g, 0)));
    }
    if (r->kill_spin) { yaw_damp(r, 0.25f, dt); r->kill_spin = 0; }   /* the hit must not spin the body like a top */
    else if (t < 2.5f) yaw_damp(r, powf(0.88f, dt * 60.0f), dt);     /* and spin dies out while falling */
    motors(r, dt, t);
    dying(r, dt, t);
    for (int it = 0; it < 12; it++) {
        solve_links(r, 0);
        min_dists(r);
        twist_limit(r);
        /* limbs may not fold flat; knees / elbows bend one way */
        const v3 f = body_fwd(r->p);
        const int lim[4][3] = { { HIL, KNL, ANL }, { HIR, KNR, ANR }, { SHL, ELL, WRL }, { SHR, ELR, WRR } };
        for (int j = 0; j < 4; j++) {
            const int a = lim[j][0], m = lim[j][1], e = lim[j][2];
            const float mn = j < 2 ? r->minkn[j] : r->minel[j - 2], sg = j < 2 ? r->knsgn[j] : r->elsgn[j - 2];
            v3 d = sub(r->p[e], r->p[a]); float L = len(d);
            if (L < mn && L > 1e-5f) { const v3 c = mul(d, (mn - L) / L * 0.5f); r->p[e] = add(r->p[e], c); r->p[a] = sub(r->p[a], mul(c, 0.3f)); }
            const v3 mid = mul(add(r->p[a], r->p[e]), 0.5f); d = nrm(sub(r->p[e], r->p[a]));
            v3 ff = sub(f, mul(d, dot(f, d))); if (len(ff) < 1e-4f) continue; ff = nrm(ff);
            const float side = dot(sub(r->p[m], mid), ff) * sg;
            if (side < 0.0f) r->p[m] = add(r->p[m], mul(ff, -side * sg * 0.6f));
        }
        for (int i = 0; i < NP; i++) {
            const float gy = r->ground + k_rad[i];
            if (r->p[i].y < gy) {
                r->p[i].y = gy;
                r->pp[i].x = r->p[i].x - (r->p[i].x - r->pp[i].x) * 0.15f;   /* heavy friction: bodies do not slide */
                r->pp[i].z = r->p[i].z - (r->p[i].z - r->pp[i].z) * 0.15f;
                if (r->pp[i].y < gy) r->pp[i].y = gy;                       /* no bounce: dead weight */
            }
        }
    }
    for (int it = 0; it < 4; it++) {   /* bones end at their exact length (no stretched limbs), floor kept */
        solve_links(r, 1);
        for (int i = 0; i < NP; i++) { const float gy = r->ground + k_rad[i]; if (r->p[i].y < gy) r->p[i].y = gy; }
    }
}
/* rotations from the simulation; joint POSITIONS by forward kinematics from the parent (v1054X): the shoulder is
 * where the torso puts it, the elbow where the upper arm puts it ... -> no seams / detached limbs at the joints */
static void driver_rots(const rag_t* r, m3* Q, v3* X)
{
    { const m3 F = frame(sub(r->p[LUM], r->p[PEL]), sub(r->p[HIR], r->p[HIL])); const m3 t = tr(&r->F0[0]); Q[D_LT] = mm(&F, &t); }
    { const m3 F = frame(sub(r->p[NEC], r->p[THO]), sub(r->p[SHR], r->p[SHL])); const m3 t = tr(&r->F0[1]); Q[D_UT] = mm(&F, &t); }
    for (int d = 2; d < ND; d++) {
        const m3* P = &Q[k_dp[d]];
        const v3 d0 = mv(P, sub(r->p0[k_de[d]], r->p0[k_dj[d]]));
        const m3 A = arc(d0, sub(r->p[k_de[d]], r->p[k_dj[d]]));
        Q[d] = mm(&A, P);
    }
    /* shin + foot and forearm + hand: the full rotation incl. twist from the toe / finger tip, so the drawn foot and
     * hand are where the colliding ones are (v1054Z: toes drawn 6-9 cm in the floor) */
    { const int dd[4] = { D_LLL, D_LLR, D_LAL, D_LAR }, tip[4] = { TOL, TOR, FIL, FIR };
      for (int i = 0; i < 4; i++) {
          const int d = dd[i], j = k_dj[d], e = k_de[d], t = tip[i];
          if (len(sub(r->p0[t], r->p0[e])) < 0.03f) continue;
          const m3 F = frame(sub(r->p[e], r->p[j]), sub(r->p[t], r->p[e])), F0 = frame(sub(r->p0[e], r->p0[j]), sub(r->p0[t], r->p0[e]));
          const m3 F0t = tr(&F0); Q[d] = mm(&F, &F0t);
      } }
    X[D_LT] = r->p[PEL]; X[D_UT] = r->p[THO];
    for (int d = 2; d < ND; d++) {
        const int pd = k_dp[d];
        X[d] = add(X[pd], mv(&Q[pd], sub(r->p0[k_dj[d]], r->p0[k_dj[pd]])));
    }
}
/* v1097N get-up blend: while the game's get-up anim fades in, each palette row / position = the game's (already written this
 * frame) towards ours by kd_bw (rows nlerped, keeping the anim row's length) */
static void kd_wrow(const rag_t* r, uint32_t o, v3 row)
{
    if (r->kd_ph && r->kd_bw < 0.999f) { const v3 a = V(rdf(o), rdf(o + 4), rdf(o + 8)); const float la = len(a);
        if (fok(a.x) && la > 1e-4f) { v3 v = add(mul(a, 1.0f - r->kd_bw), mul(row, r->kd_bw)); const float lv = len(v); if (lv > 1e-4f) row = mul(v, la / lv); } }
    wrf(o, row.x); wrf(o + 4, row.y); wrf(o + 8, row.z);
}
static void kd_wpos(const rag_t* r, uint32_t o, v3 pos)
{
    if (r->kd_ph && r->kd_bw < 0.999f) { const v3 a = V(rdf(o), rdf(o + 4), rdf(o + 8)); if (fok(a.x) && fok(a.y) && fok(a.z)) pos = add(mul(a, 1.0f - r->kd_bw), mul(pos, r->kd_bw)); }
    wrf(o, pos.x); wrf(o + 4, pos.y); wrf(o + 8, pos.z);
}
static void write_pose(rag_t* r)
{
    m3 Q[ND]; v3 X[ND];
    if (r->jh || r->frozen) { for (int d = 0; d < ND; d++) { memcpy(Q[d].m, &r->jq[d * 9], 36); X[d] = V(r->jx[d * 3], r->jx[d * 3 + 1], r->jx[d * 3 + 2]); } }
    else driver_rots(r, Q, X);
    /* v1057F (user: bodies vanish depending on where you stand, come back when you get closer): the game culls a body
     * by its entity position, which stayed where the death started while the ragdoll slid / rolled away. The corpse's
     * entity (RwFrame at *(ent+0x68): modelling matrix +0x10, LTM +0x50) follows the pelvis on the ground plane; the
     * pose below is written relative to wherever the entity is, so the body does not move. GF_RAG_FOLLOW=0 = off. */
    /* v1091G (user video 01:52: a settled corpse "moves on its own, then teleports back to his original spot"): the follow hopped
     * the root 1.63 m to the body; the game later puts its corpse back where it stored it and the pose (relative to the root)
     * went with it. The pose is relative to the CURRENT root anyway, so the root now stays: OFF by default, GF_RAG_FOLLOW=1 = on. */
    { static int on = -1; if (on < 0) { const char* e = getenv("GF_RAG_FOLLOW"); on = (e && *e == '1') ? 1 : 0; }
      m3 R0; v3 T0; const uint32_t fm = rd32(r->ent + 0x68);
      if (on && r->corpse && r->state == 4 && ok(fm) && ent_xform(r->ent, &R0, &T0)) {   /* v1091F: only at rest (0.15 m hops while a shot corpse slid = one-frame jumps, the 'glide') */
          const m3 Md = w2mc(&r->Rd, r->conv), MdT = tr(&Md);
          const v3 pel = add(r->Td, mv(&MdT, X[D_LT]));
          const float dx = pel.x - T0.x, dz = pel.z - T0.z;
          if (fok(pel.x) && fok(pel.z) && dx * dx + dz * dz > 0.4f * 0.4f && dx * dx + dz * dz < 6.0f * 6.0f) {
              const int ltm = fabsf(rdf(fm + 0x80) - T0.x) < 0.02f && fabsf(rdf(fm + 0x84) - T0.y) < 0.02f && fabsf(rdf(fm + 0x88) - T0.z) < 0.02f;
              wrf(fm + 0x40, pel.x); wrf(fm + 0x48, pel.z);
              if (ltm) { wrf(fm + 0x80, pel.x); wrf(fm + 0x88, pel.z); }
              static int nlog = 0; if (nlog < 12) { nlog++; RLOG("[rag] v1057F ent=%08X: entity moved %.2f m to the body (LTM %s)\n", r->ent, sqrtf(dx * dx + dz * dz), ltm ? "too" : "differs, left"); }
          } } }
    /* v1081P (user 23:07: "NPCs glide ~90 % of the time when they ragdoll and hit the ground, then instantly go back to where
     * they hit the ground"): rag_rec.bin of that session -- the game's death animation keeps moving the DYING entity's root
     * (1.5 m in 1.5 s, up to 4.8 m) while the simulated body lies still; our pose is compensated against *(ent+0x68), but what is
     * drawn follows the moving root (the renderer's transform is out of step with the one we read), and the corpse hand-off
     * (placed at the body) snaps it back. PIN the dying entity's frame (modelling matrix +0x10..+0x48 and LTM +0x50..+0x88) to
     * where the ragdoll started (Rd, Td) every frame, so every transform agrees and the root cannot glide. Not for corpses
     * (v1057F follows the body), not for live hit reactions. GF_RAG_PIN=0 = off. */
    /* v1108Y (user 22:09: bodies "often become invisible when they're in the ragdoll state, and don't land the correct place when
     * driving into them"): the game culls by the ROOT, which stayed pinned where the death started while a car threw the body
     * metres away -> off-screen root = body not drawn. The pin target now follows the pelvis on the ground plane in >= 1 m hops
     * (pose is written relative to the current root, so nothing visible moves) -- also for CORPSES, re-asserted every frame so
     * the game cannot put the root back (the v1091G snap-back). GF_RAG_PIN_FOLLOW=0 = old fixed pin, dying entities only. */
    { static int pin = -1, pfol = -1; if (pin < 0) { const char* e = getenv("GF_RAG_PIN"); pin = (e && *e == '0') ? 0 : 1; e = getenv("GF_RAG_PIN_FOLLOW"); pfol = (e && *e == '0') ? 0 : 1; }
      const uint32_t fm = rd32(r->ent + 0x68); m3 R1; v3 T1;
      if (pin && (!r->corpse || pfol) && !r->hr && !r->kd_ph && ok(fm) && ent_xform(r->ent, &R1, &T1)) {
          if (!r->pin_have) { r->pin_T = r->Td; r->pin_have = 1; }
          if (pfol) { const m3 Mdp = w2mc(&r->Rd, r->conv), MdpT = tr(&Mdp); const v3 pel = add(r->Td, mv(&MdpT, X[D_LT]));
              const float hx = pel.x - r->pin_T.x, hz = pel.z - r->pin_T.z, hd = sqrtf(hx * hx + hz * hz);
              static float pstep = -1.0f; if (pstep < 0.0f) { const char* e = getenv("GF_RAG_PIN_STEP"); pstep = e && atof(e) > 0.0 ? (float)atof(e) : 0.05f; }   /* v1110Z: was 1 m hops -> one-frame flash back by the hop */
              if (fok(pel.x) && fok(pel.y) && hd > pstep && hd < 60.0f) { r->pin_T.x = pel.x; r->pin_T.z = pel.z;
                  if (pel.y - 0.25f < r->pin_T.y) r->pin_T.y = pel.y - 0.25f;   /* fell lower (kerb / slope): follow down, never up */
                  static int nf = 0; if (hd > 1.0f && nf < 30) { nf++; RLOG("[rag] v1108Y ent=%08X root follows the thrown body (%.1f m)\n", r->ent, hd); } } }
          const v3 PT = r->pin_T;
          const float dd = len(sub(T1, PT));
          if (dd > 0.002f && dd < 60.0f) {
              for (int i = 0; i < 9; i++) { const uint32_t o = (uint32_t)(i / 3) * 0x10u + (uint32_t)(i % 3) * 4u; wrf(fm + 0x10u + o, r->Rd.m[i]); wrf(fm + 0x50u + o, r->Rd.m[i]); }
              wrf(fm + 0x40, PT.x); wrf(fm + 0x44, PT.y); wrf(fm + 0x48, PT.z);
              wrf(fm + 0x80, PT.x); wrf(fm + 0x84, PT.y); wrf(fm + 0x88, PT.z);
              r->pin_n++; if (dd > r->pin_max) r->pin_max = dd;
              static int nl = 0; if (r->pin_n == 1 && nl < 20) { nl++; RLOG("[rag] v1081P ent=%08X: dying entity pinned where the ragdoll started (it had moved %.2f m)\n", r->ent, dd); }
          } } }
    /* the entity may have moved / turned since the start (root motion): current model = Rc (Td + Rd^T p - Tc) */
    m3 Rc; v3 Tc; if (!ent_xform(r->ent, &Rc, &Tc)) return;
    /* v1057F-2 (user: a body lay floating at kerb height): the renderer places the body with the frame's LTM (+0x50),
     * not the modelling matrix we read -- when the game moves one and not the other (it snaps a moved corpse onto the
     * floor under it), the pose must be written relative to the LTM. */
    { const uint32_t fm = rd32(r->ent + 0x68);
      if (ok(fm)) { const v3 lt = V(rdf(fm + 0x80), rdf(fm + 0x84), rdf(fm + 0x88)); const v3 dd = sub(lt, Tc);
          if (fok(lt.x) && fok(lt.y) && fok(lt.z) && len(dd) > 0.01f && len(dd) < 5.0f) {
              static int nl = 0; if (nl < 10) { nl++; RLOG("[rag] v1057F-2 ent=%08X: LTM differs from the modelling matrix by (%.2f %.2f %.2f) -> pose follows the LTM\n", r->ent, dd.x, dd.y, dd.z); }
              Tc = lt; } } }
    const m3 Mc = w2mc(&Rc, r->conv), Md = w2mc(&r->Rd, r->conv), MdT = tr(&Md);
    const m3 C = mm(&Mc, &MdT); const v3 off = mv(&Mc, sub(r->Td, Tc));
    m3 CQ[ND]; for (int d = 0; d < ND; d++) CQ[d] = mm(&C, &Q[d]);
    /* v1056C cloth: anchors ride the pelvis; tips fall with gravity, keep their length, stay off the floor, out of the
     * thighs and below the hips (never flipping up over the body), linked to their ring neighbours */
    v3 clA[12];
    if (r->ncl) {
        const float dt = r->frame_dt > 0.002f && r->frame_dt < 0.05f ? r->frame_dt : 1.0f / 60.0f, g = s_grav * dt * dt;
        for (int k = 0; k < r->ncl; k++) {
            clA[k] = add(X[D_LT], mv(&Q[D_LT], sub(r->cl_A0[k], r->p0[PEL])));
            const v3 v = mul(sub(r->cl_T[k], r->cl_Tp[k]), 0.94f);
            r->cl_Tp[k] = r->cl_T[k]; r->cl_T[k] = add(r->cl_T[k], add(v, V(0, -g, 0)));
        }
        const v3 hang_axis = mv(&Q[D_LT], nrm(sub(r->p0[PEL], r->p0[LUM])));   /* "down the legs" with the pelvis */
        /* v1057R (user: shot woman lay with her thighs coming out through the side of the skirt, hem flared like an upturned
         * cone): a skirt rides the thighs, it does not hang from the hips. Each panel's rest axis = its nearer thigh (by its
         * rest-pose share, cl_wl) 85 % + the pelvis axis 15 %, swing <= 25 deg off it (was 80 deg off the pelvis axis).
         * Offline on the recorded death (rag_rec 04:37): panels end 17 deg off the legs (standing: 11), was 59-110. */
        const v3 thL = nrm(sub(r->p[KNL], r->p[HIL])), thR = nrm(sub(r->p[KNR], r->p[HIR]));
        for (int it = 0; it < 6; it++) for (int k = 0; k < r->ncl; k++) {
            v3* T = &r->cl_T[k];
            for (int e = 0; e < 2; e++) { const int o = r->cl_nb[k][e]; if (o < 0) continue;
                const v3 d = sub(*T, r->cl_T[o]); const float L = len(d); if (L < 1e-5f) continue;
                const v3 c = mul(d, (L - r->cl_nbL[k][e]) / L * 0.15f); *T = sub(*T, c); r->cl_T[o] = add(r->cl_T[o], c); }
            for (int lg = 0; lg < 2; lg++) {   /* out of the thigh capsules */
                const v3 a = r->p[lg ? HIR : HIL], b = r->p[lg ? KNR : KNL], ab = sub(b, a); const float L2 = dot(ab, ab);
                float t = L2 > 1e-6f ? dot(sub(*T, a), ab) / L2 : 0; t = t < 0 ? 0 : t > 1 ? 1 : t;
                const v3 cp = add(a, mul(ab, t)), d = sub(*T, cp); const float L = len(d);
                if (L < 0.10f && L > 1e-5f) *T = add(cp, mul(d, 0.10f / L));
            }
            { const v3 d = sub(*T, clA[k]); const float L = len(d);   /* length + swing limit (<= 80 deg off "down the legs") */
              const v3 ax = nrm(add(mul(nrm(add(mul(thL, r->cl_wl[k]), mul(thR, 1.0f - r->cl_wl[k]))), 0.85f), mul(hang_axis, 0.15f)));
              v3 dir = L > 1e-5f ? mul(d, 1.0f / L) : ax;
              const float c = dot(dir, ax), cmin = 0.906f;   /* cos 25 deg */
              if (c < cmin) { dir = nrm(add(sub(dir, mul(ax, c)), mul(ax, cmin + 0.02f))); }
              *T = add(clA[k], mul(dir, r->cl_L[k])); }
            if (T->y < r->ground + 0.03f) { T->y = r->ground + 0.03f; r->cl_Tp[k].x = T->x - (T->x - r->cl_Tp[k].x) * 0.3f; r->cl_Tp[k].z = T->z - (T->z - r->cl_Tp[k].z) * 0.3f; }
        }
    }
    for (int s = 0; s < r->nslot; s++) {
        const int d = r->drv[s], j = k_dj[d];
        const float* M = r->M0[s]; const uint32_t m = r->pal + (uint32_t)s * 64u;
        const v3 x0 = V(M[9], M[10], M[11]);
        if (len(x0) < 0.03f) continue;   /* root / motion bones at the origin stay as the game sets them */
        { int ck = -1; for (int k = 0; k < r->ncl; k++) if (r->cl_slot[k] == s) { ck = k; break; }
          if (ck >= 0) {   /* cloth bone: pelvis rotation, then swung so its hang direction points at the tip */
              const v3 h0 = mv(&Q[D_LT], sub(r->cl_T0[ck], r->cl_A0[ck])); const m3 Aq = arc(h0, sub(r->cl_T[ck], clA[ck]));
              const m3 Rb = mm(&Aq, &Q[D_LT]); const m3 CR = mm(&C, &Rb);
              const v3 pos = add(mv(&C, add(clA[ck], mv(&Rb, sub(x0, r->cl_A0[ck])))), off);
              for (int k = 0; k < 3; k++) { const v3 row = mv(&CR, V(M[k * 3], M[k * 3 + 1], M[k * 3 + 2]));
                  kd_wrow(r, m + (uint32_t)k * 16u, row); }
              kd_wpos(r, m + 0x30u, pos);
              continue;
          } }
        v3 pos = add(mv(&C, add(X[d], mv(&Q[d], sub(x0, r->p0[j])))), off);
        m3 R = CQ[d];
        if (r->w2[s] > 0.0f) {
            const int d2 = r->drv2[s], j2 = k_dj[d2]; const float w = r->w2[s];
            const v3 pos2 = add(mv(&C, add(X[d2], mv(&Q[d2], sub(x0, r->p0[j2])))), off);
            pos = add(mul(pos, 1.0f - w), mul(pos2, w));
            R = mblend(&CQ[d], &CQ[d2], w);
        }
        for (int k = 0; k < 3; k++) { const v3 row = mv(&R, V(M[k * 3], M[k * 3 + 1], M[k * 3 + 2]));
            kd_wrow(r, m + (uint32_t)k * 16u, row); }
        kd_wpos(r, m + 0x30u, pos);
    }
    r->wrote_pel = slot_pos(r->pal, r->slot[PEL]);
    if (s_rec && r->frames <= 600) {
        const m3 McT = tr(&Mc), MdT2 = tr(&Md);
        rec_w("RAGF", 4); rec_u32(r->ent); rec_f((float)((now_ms() - r->t0) / 1000.0)); rec_u32((uint32_t)(r->state | (r->have_anim << 8) | (r->corpse << 9)));
        for (int i = 0; i < NP; i++) rec_v3(add(r->Td, mv(&MdT2, r->p[i])));
        for (int i = 0; i < NP; i++) rec_v3(add(r->Td, mv(&MdT2, r->a[i])));
        for (int sl = 0; sl < r->nslot; sl++) rec_v3(add(Tc, mv(&McT, slot_pos(r->pal, sl))));
        if ((r->frames & 63) == 0) fflush(s_rec);
    }
}
/* the game's fresh animation pose (read before we overwrite it), into the start frame: p = Rd (Tc + Rc^T a - Td) */
static void read_anim(rag_t* r)
{
    r->have_anim = 0;
    const v3 pel = slot_pos(r->pal, r->slot[PEL]);
    if (r->frames > 0 && fabsf(pel.x - r->wrote_pel.x) + fabsf(pel.y - r->wrote_pel.y) + fabsf(pel.z - r->wrote_pel.z) < 1e-6f) return;   /* not re-animated: our own pose */
    m3 Rc; v3 Tc; if (!ent_xform(r->ent, &Rc, &Tc)) return;
    v3 a[NP]; read_particles(r, a);
    const m3 Mc = w2mc(&Rc, r->conv), McT = tr(&Mc), Md = w2mc(&r->Rd, r->conv);
    for (int i = 0; i < NP; i++) { const v3 w = add(Tc, mv(&McT, a[i])); r->a[i] = mv(&Md, sub(w, r->Td)); if (!fok(r->a[i].x)) return; }
    r->have_anim = 1;
}
static int still_valid(const rag_t* r)
{
    if (rd32(r->ent) != r->vt || rd32(r->ent + 0xC4) != r->anim || anim_pal(r->anim) != r->pal) return 0;
    if (!r->corpse && !r->kd) { const float hp = rdf(r->ent + 0x14C8); if (fok(hp) && hp > 0.0f) return 0; }   /* NPC reused / revived (v1097N: knockdowns are alive) */
    return 1;
}

/* entity whose animation is being updated (ppu_recomp_002.cpp wraps func_0046316C(entity, dt)) */
static PPU_THREAD_LOCAL uint32_t s_cur_ent;
uint32_t gf_rag_ent_begin(uint32_t ent) { const uint32_t prev = s_cur_ent; s_cur_ent = ent; return prev; }
void gf_rag_ent_end(uint32_t prev) { s_cur_ent = prev; }
uint32_t gf_rag_cur_ent(void) { return s_cur_ent; }   /* v1058H co-op pose: entity whose skeleton is being updated */

/* the game's corpse (func_002FC3BC: corpse+0x10C = npc+0x38, same death anim restarted) or the NPC's skeleton moved to
 * another entity: keep the body going there */
static int try_handoff(uint32_t anim, uint32_t cur)
{
    if (!ok(cur)) return 0;
    for (int i = 0; i < MAXR; i++) {
        rag_t* r = &s_rg[i];
        if (!(r->state == 3 || r->state == 4 || r->state == 6) || cur == r->ent || r->hr || r->kd) continue;
        const int samepal = (anim_pal(anim) == r->pal);      /* v1054U log: the corpse reuses the NPC's palette */
        const int link = rd32(cur + 0x10C) == r->src38, moved = (anim == r->anim);
        if (!samepal && !link && !moved) continue;
        if (now_ms() - r->t0 > 15000.0) continue;   /* v1055A log: a reused palette went to new NPCs 35-44 s later */
        /* v1108Z2 (user 22:29: bodies "revert to the default death stance"; log "no corpse within 6 s -> released"): since v1108Y
         * the dying root FOLLOWS a thrown body, but the game spawns the corpse where the NPC died -> also accept the death spot */
        { m3 R; v3 T; if (!ent_xform(cur, &R, &T)) continue;
          const float d1 = len(sub(T, r->last_T)), d2 = len(sub(T, r->Td));
          if (d1 > 2.5f && d2 > 2.5f) continue; }       /* not where the body is / was */
        if (!samepal) {
            if (link && !moved && rd32(anim + 0x10) != r->anim_id) continue;                        /* not the same animation */
        }
        const uint32_t pal = anim_pal(anim);
        if (!ok(pal) || anim_nbones(anim) != r->nbones) continue;   /* loot / props linked to the body: not it */
        RLOG("[rag] body %08X -> %s %08X (anim %08X palette %08X%s) after %.1f s, %s\n", r->ent, link ? "corpse" : "entity", cur, anim, pal, samepal ? ", same palette" : "",
             (now_ms() - r->t0) / 1000.0, (r->state == 4 || r->resume_state == 4) ? "settled" : "still moving");
        if (r->state == 6) r->state = r->resume_state;
        r->ent = cur; r->anim = anim; r->pal = pal; r->vt = rd32(cur); r->corpse = 1;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ v1069H live hit reactions (+ bullets on ragdolls) */
static int s_bhv = 0;   /* v1075R dying behaviours; v1080B: OFF by default (user 22:55: falls less realistic than before, bodies "moved around when dead") -- GF_RAG_BHV=1 turns them on */
static int s_hit_on = 1, s_hit_max = 6, s_brace = 1, s_hlog = 0, s_stumble = 1;
static float s_hit_scale = 1.0f;
static int s_mblood = -1;   /* v1099B GF_MELEE_BLOOD */
#define HLOG(...) do { if (s_hlog < 600) { s_hlog++; printf(__VA_ARGS__); fflush(stdout); } } while (0)
static const float k_pmass[ND] = { 11.f, 20.f, 5.f, 2.5f, 2.f, 2.5f, 2.f, 9.f, 5.f, 9.f, 5.f };      /* = gf_ragdoll_jolt.cpp */
static const int k_dtip[ND] = { LUM, NEC, HED, ELL, FIL, ELR, FIR, KNL, ANL, KNR, ANR };           /* far end of each part */
static const float k_hrad[ND] = { .14f, .16f, .11f, .06f, .05f, .06f, .05f, .08f, .06f, .08f, .06f };
static const float k_hdv[ND] = { 0.f, 3.2f, 2.4f, 2.6f, 3.2f, 2.6f, 3.2f, 4.0f, 3.5f, 4.0f, 3.5f };   /* m/s the hit part gets (x damage scale) */
static const char* const k_pn[ND] = { "pelvis", "chest", "head", "upper arm L", "forearm L", "upper arm R", "forearm R", "thigh L", "shin L", "thigh R", "shin R" };
static float segcp(v3 p, v3 a, v3 b, v3* cp)
{
    const v3 ab = sub(b, a); const float L = dot(ab, ab);
    float t = L > 1e-8f ? dot(sub(p, a), ab) / L : 0.0f; t = t < 0 ? 0 : t > 1 ? 1 : t;
    *cp = add(a, mul(ab, t)); return len(sub(p, *cp));
}
/* which part a bullet at hm (same frame as p) hits; *pt = where to push it; *arm2 = upper arm next to a shoulder hit;
 * *miss = distance from the body (m) */
static int hit_part(const v3* p, v3 hm, v3* pt, int* arm2, float* miss)
{
    int best = D_UT; float bs = 1e30f; v3 bcp = hm;
    for (int d = 0; d < ND; d++) {
        v3 cp; float dd = segcp(hm, p[k_dj[d]], p[k_dtip[d]], &cp);
        if (d == D_LT) { v3 c2; const float d2 = segcp(hm, p[HIL], p[HIR], &c2); if (d2 < dd) { dd = d2; cp = c2; } }
        if (d == D_UT) { v3 c2; const float d2 = segcp(hm, p[SHL], p[SHR], &c2); if (d2 < dd) { dd = d2; cp = c2; } }
        const float sc = dd - k_hrad[d]; if (sc < bs) { bs = sc; best = d; bcp = cp; }
    }
    *miss = bs > 0 ? bs : 0; *arm2 = -1;
    if (best == D_UT || best == D_LT) {   /* lever from the spine axis, not the shoulder line (no 70 deg twists) */
        segcp(hm, p[PEL], p[NEC], &bcp);
        const float dl = len(sub(hm, p[SHL])), dr = len(sub(hm, p[SHR]));
        if (best == D_UT && (dl < 0.11f || dr < 0.11f)) *arm2 = dl < dr ? D_UAL : D_UAR;
    }
    v3 off = sub(hm, bcp); const float l = len(off), rmax = (best == D_UT || best == D_LT) ? 0.09f : k_hrad[best];
    if (l > rmax) off = mul(off, rmax / l);
    *pt = add(bcp, off); return best;
}
/* ------------------------------------------------------------------ v1069B blood spray + arterial spurts
 * (rsx_ui_overlay.c gfp_add: ballistic droplets drawn as streaks, a small splat where each lands). Every bullet: a puff
 * out of the exit side (along the bullet) + back-spatter towards the shooter. Arteries: neck (carotid), inner upper thigh
 * (femoral), upper arm (brachial), heavy torso hits (gusher) -> a pulsing spurt from the wound for several seconds, riding
 * the nearest palette bone (so it follows the body: alive, ragdolling, corpse). GF_BLOOD_SPRAY=0 off. */
extern void gfp_add(const float* p, const float* v, float size, float ground, float life);
static int s_spray = 1;
static float s_bmul = -1.0f;   /* v1091C GF_BLOOD_MUL (default 2): more spray particles + stronger spurts */
static float bmul(void) { if (s_bmul < 0.0f) { const char* e = getenv("GF_BLOOD_MUL"); s_bmul = (e && *e) ? (float)atof(e) : 2.0f; if (!(s_bmul >= 0.0f)) s_bmul = 2.0f; if (s_bmul > 5.0f) s_bmul = 5.0f; } return s_bmul; }
typedef struct { int on, kind, slot; uint32_t pal, ent; v3 loc, dirl; double t0, next_pulse, pulse_t0, last; float strength, tau, hz, dur, acc, ground; } artery_t;
enum { NART = 16 };   /* v1091C: was 8 */
static artery_t s_art[NART];
static volatile LONG s_art_live;
static float frand2(float a, float b) { return a + (b - a) * frand(); }
static v3 rand_cone(v3 d, float ang)
{
    const v3 a = fabsf(d.y) < 0.9f ? V(0, 1, 0) : V(1, 0, 0); const v3 u = nrm(cross(d, a)), w = cross(d, u);
    const float ph = frand() * 6.2831853f, ct = 1.0f - frand() * (1.0f - cosf(ang)), st = sqrtf(fmaxf(0.0f, 1.0f - ct * ct));
    return nrm(add(mul(d, ct), add(mul(u, st * cosf(ph)), mul(w, st * sinf(ph)))));
}
static float ground_at(v3 w) { float t; v3 n; if (col_ray(add(w, V(0, 0.3f, 0)), V(0, -1, 0), 3.0f, &t, &n)) return w.y + 0.3f - t; return w.y - 1.3f; }
/* v1069B-2 (GFD_DEBUG frames: droplet splats under the sidewalk): the ground where THIS droplet lands -- predict the
 * landing point on the first guess (same ballistic + drag as the renderer), then the level height there */
static float land_ground(v3 p, v3 v, float g0)
{
    const float h = p.y - g0; if (h < 0.0f) return g0;
    const float t = (v.y + sqrtf(v.y * v.y + 19.62f * h)) / 9.81f, fe = (1.0f - expf(-0.35f * t)) / 0.35f;
    return ground_at(V(p.x + v.x * fe, fmaxf(p.y, g0 + 0.6f), p.z + v.z * fe));
}
static void spray_puff(v3 hit, v3 sh, float dmg)
{
    if (!s_spray || !fok(hit.x) || !fok(sh.x)) return;
    v3 d = sub(hit, sh); if (len(d) < 0.05f) return; d = nrm(d);
    col_load(); const float g = ground_at(hit);
    int n = 10 + (int)(dmg / 150.0f); if (n > 40) n = 40; n = (int)(n * bmul());
    for (int i = 0; i < n; i++) {
        const int back = frand() < 0.3f;
        const v3 dir = back ? rand_cone(mul(d, -1.0f), 0.6f) : rand_cone(d, 0.42f);
        const float sp = back ? frand2(0.8f, 2.0f) : frand2(1.8f, 4.5f) * (dmg >= 2000.0f ? 1.3f : 1.0f);
        const v3 v = mul(dir, sp), p = add(hit, mul(dir, 0.03f));
        const float gl = land_ground(p, v, g);
        gfp_add(&p.x, &v.x, frand2(0.010f, 0.024f), gl, 2.5f);
    }
}
static const char* const k_artn[5] = { "", "carotid (neck)", "femoral (thigh)", "brachial (arm)", "gusher (torso)" };
static void artery_start(uint32_t ent, int kind, v3 hit_w, v3 sh_w, int dying)
{
    const uint32_t anim = rd32(ent + 0xC4); if (!ok(anim)) return;
    const uint32_t pal = anim_pal(anim); if (!ok(pal)) return;
    m3 R; v3 T; if (!ent_xform(ent, &R, &T)) return;
    const m3 Mc = w2mc(&R, s_conv);
    const v3 hm = mv(&Mc, sub(hit_w, T)), dm = mv(&Mc, nrm(sub(sh_w, hit_w)));
    int ns = 0; if (!rig_key(anim, &ns) || ns <= 0 || ns > MAXS) return;
    int best = -1; float bd = 1e9f;
    for (int sl = 0; sl < ns; sl++) { const v3 q = slot_pos(pal, sl); if (len(q) < 0.03f || !fok(q.x)) continue; const float d = len(sub(q, hm)); if (d < bd) { bd = d; best = sl; } }
    if (best < 0 || bd > 0.35f) return;
    const uint32_t m = pal + (uint32_t)best * 64u;
    const v3 r0 = V(rdf(m), rdf(m + 4), rdf(m + 8)), r1 = V(rdf(m + 16), rdf(m + 20), rdf(m + 24)), r2 = V(rdf(m + 32), rdf(m + 36), rdf(m + 40));
    const float l0 = dot(r0, r0), l1 = dot(r1, r1), l2 = dot(r2, r2); if (l0 < 0.25f || l1 < 0.25f || l2 < 0.25f) return;
    const v3 dd = sub(hm, slot_pos(pal, best));
    artery_t* a = 0; for (int i = 0; i < NART; i++) if (!s_art[i].on) { a = &s_art[i]; break; }
    if (!a) { a = &s_art[0]; for (int i = 1; i < NART; i++) if (s_art[i].t0 < a->t0) a = &s_art[i]; }
    memset(a, 0, sizeof *a);
    a->kind = kind; a->slot = best; a->pal = pal; a->ent = ent;
    a->loc = V(dot(r0, dd) / l0, dot(r1, dd) / l1, dot(r2, dd) / l2);
    a->dirl = V(dot(r0, dm) / l0, dot(r1, dm) / l1, dot(r2, dm) / l2);
    static const float hz[5] = { 0, 1.35f, 1.15f, 1.25f, 0.9f }, st[5] = { 0, 1.0f, 0.85f, 0.6f, 0.45f }, tau[5] = { 0, 3.0f, 3.5f, 2.5f, 2.0f }, du[5] = { 0, 6.0f, 7.0f, 4.5f, 3.5f };
    a->hz = hz[kind]; a->strength = st[kind] * frand2(0.85f, 1.15f); a->tau = tau[kind] * (dying ? 0.75f : 1.0f); a->dur = du[kind] * frand2(0.8f, 1.2f);
    a->t0 = a->next_pulse = now_ms(); a->pulse_t0 = 0; a->last = 0; col_load(); a->ground = ground_at(hit_w); a->on = 1;
    InterlockedExchange(&s_art_live, 1);
    HLOG("[rag-hit] ent=%08X ARTERY: %s spurting for %.1f s (bone slot %d, %.2f m from the wound)\n", ent, k_artn[kind], a->dur, best, bd);
}
static void wound_add(uint32_t ent, v3 hit_w, v3 sh_w, float size, int sprite);   /* v1117A, below */
/* every bullet that lands on a body: the puff, and maybe an artery. p = the 21 joints, hm = hit point (same frame) */
static void blood_hit(uint32_t ent, const v3* p, v3 hm, int part, v3 hit_w, v3 sh_w, float dmg, int dying)
{
    if (!s_spray) return;
    spray_puff(hit_w, sh_w, dmg);
    int kind = 0;
    if (len(sub(hm, p[NEC])) < 0.10f || (part == D_HEAD && hm.y < p[NEC].y + 0.06f)) kind = frand() < 0.85f ? 1 : 0;
    else if (part == D_ULL || part == D_ULR) {
        const int hi = part == D_ULL ? HIL : HIR, ho = part == D_ULL ? HIR : HIL;
        v3 in = sub(p[ho], p[hi]); in.y = 0; in = nrm(in);
        const float med = dot(sub(hm, p[hi]), in), below = p[hi].y - hm.y;
        if ((med > -0.02f && below < 0.40f && frand() < 0.65f) || frand() < 0.12f) kind = 2;
    }
    else if ((part == D_UAL || part == D_UAR) && frand() < 0.45f) kind = 3;   /* v1091C: 0.45 (was 0.25) */
    else if ((part == D_UT || part == D_LT) && frand() < (dmg >= 2500.0f ? 0.6f : 0.3f)) kind = 4;   /* v1091C: any damage */
    if (!kind && dying && part != D_HEAD && frand() < 0.75f) kind = 4;   /* v1091C: a killing shot / a shot corpse spurts from the wound */
    if (kind) artery_start(ent, kind, hit_w, sh_w, dying);
    wound_add(ent, hit_w, sh_w, 0.08f + fminf(dmg, 4000.0f) / 50000.0f, 2);   /* v1117A blood on the body */
}
/* per skeleton update of a body with a spurting wound: pulses (fast in, ~0.22 s) + a dribble in between */
static void artery_tick(uint32_t anim, uint32_t cur)
{
    if (!s_art_live) return;
    const uint32_t pal = anim_pal(anim); if (!ok(pal)) return;
    int any = 0;
    for (int i = 0; i < NART; i++) {
        artery_t* a = &s_art[i]; if (!a->on) continue; any = 1;
        if (a->pal != pal) continue;
        const double n = now_ms(); const float t = (float)((n - a->t0) / 1000.0);
        const float dt = a->last > 0 ? (float)((n - a->last) / 1000.0) : 0.0f; a->last = n;
        if (t > a->dur || dt > 0.25f) { if (t > a->dur) a->on = 0; continue; }
        const uint32_t e = ok(cur) ? cur : a->ent;
        m3 R; v3 T; if (!ent_xform(e, &R, &T)) continue;
        const m3 Mc = w2mc(&R, s_conv), McT = tr(&Mc);
        const float sn = a->strength * expf(-t / a->tau);
        if (sn < 0.12f) { a->on = 0; continue; }
        if (n >= a->next_pulse) { a->pulse_t0 = n; a->next_pulse = n + 1000.0 / a->hz * frand2(0.9f, 1.1f); }
        const float pt = (float)((n - a->pulse_t0) / 1000.0); const int pulse = pt < 0.22f;
        a->acc += (pulse ? 75.0f * sn * (1.0f - pt / 0.22f * 0.6f) : 5.0f * sn) * dt * bmul();
        if (a->acc < 1.0f) continue;
        const uint32_t m = pal + (uint32_t)a->slot * 64u;
        const v3 r0 = V(rdf(m), rdf(m + 4), rdf(m + 8)), r1 = V(rdf(m + 16), rdf(m + 20), rdf(m + 24)), r2 = V(rdf(m + 32), rdf(m + 36), rdf(m + 40));
        const v3 pm = add(slot_pos(pal, a->slot), add(add(mul(r0, a->loc.x), mul(r1, a->loc.y)), mul(r2, a->loc.z)));
        const v3 outl = add(mul(nrm(add(add(mul(r0, a->loc.x), mul(r1, a->loc.y)), mul(r2, a->loc.z))), 0.6f), mul(nrm(add(add(mul(r0, a->dirl.x), mul(r1, a->dirl.y)), mul(r2, a->dirl.z))), 0.4f));
        if (!fok(pm.x) || !fok(outl.x)) { a->on = 0; continue; }
        const v3 w = add(T, mv(&McT, pm)), dw = nrm(add(nrm(mv(&McT, outl)), V(0, 0.25f, 0)));
        while (a->acc >= 1.0f) {
            a->acc -= 1.0f;
            const v3 d = rand_cone(dw, pulse ? 0.16f : 0.5f);
            const float sp = pulse ? frand2(2.0f, 3.6f) * (0.55f + 0.45f * sn) : frand2(0.2f, 0.6f);
            const v3 v = mul(d, sp), p = add(w, mul(d, 0.02f));
            gfp_add(&p.x, &v.x, pulse ? frand2(0.014f, 0.030f) : frand2(0.010f, 0.018f), pulse ? land_ground(p, v, a->ground) : a->ground, 3.0f);
        }
    }
    if (!any) InterlockedExchange(&s_art_live, 0);
}

static float dmg_scale(float dmg) { float d = 0.75f + dmg / 2500.0f; return d < 0.75f ? 0.75f : d > 2.0f ? 2.0f : d; }
static float smooth01(float x) { x = x < 0 ? 0 : x > 1 ? 1 : x; return x * x * (3 - 2 * x); }

/* v1097K knockdown RE probe (GF_KD_TEST=1): a punch -> the game's own forceFall (PainSM action 31 = func_0023452C:
 * entity+0x14B4 |= 0x60000 + a message) on that NPC, then flags / anim / position logged every 100 ms for 4 s and
 * g_kd_dump_req asks the renderer for GF_KD_DUMP frames. Answers: does the game play a fall + get-up by itself? */
static int s_kd_test = 0;
volatile int g_kd_dump_req = 0;
static struct { uint32_t ent, anim; double t0, next; } s_kd; static double s_kd_cool;
static void kd_log(const char* tag)
{
    const uint32_t e = s_kd.ent, xf = rd32(e + 0x68);
    const float px = ok(xf) ? rdf(xf + 0x40) : 0, py = ok(xf) ? rdf(xf + 0x44) : 0, pz = ok(xf) ? rdf(xf + 0x48) : 0;
    const uint32_t an = rd32(e + 0xC4);
    printf("[kd] %s ent=%08X t=%.2f 14B4=%08X 181C=%08X anim=%08X hash=%08X hp=%.0f pos %.2f %.2f %.2f\n", tag, e, (now_ms() - s_kd.t0) / 1000.0,
           rd32(e + 0x14B4), rd32(e + 0x181C), an, ok(an) ? rd32(an + 0x10) : 0, rdf(e + 0x14C8), px, py, pz); fflush(stdout);
}
static void kd_try(uint32_t ent)
{
    if (!s_kd_test || s_kd.ent || now_ms() < s_kd_cool) return;
    s_kd.ent = ent; s_kd.t0 = now_ms(); s_kd.next = s_kd.t0 + 100.0;
    kd_log("before");
    gfn ff = lookup(0x0023452Cu);
    const int okc = s_kd_test == 1 ? gcall(ff, ent, 0, 0) : 0;
    if (s_kd_test == 1) { printf("[kd] v1097K forceFall(func_0023452C) on %08X: %s\n", ent, okc ? "called" : "FAULT/missing"); fflush(stdout); }
    if (s_kd_test == 3) {   /* v1097K-2: play a recovery-table anim (GF_KD_ANIM, default flatToCrawl 0xBF6796A9) on the main controller */
        static uint32_t ah = 0; if (!ah) { const char* e = getenv("GF_KD_ANIM"); ah = e && *e ? (uint32_t)strtoul(e, 0, 16) : 0xBF6796A9u; }
        gfn fp = lookup(0x00460CE8u); ppu_context* cur = g_active_ctx; uint64_t rv = 0; int okp = 0;
        if (fp && cur) {
            __try { static ppu_context c; memcpy(&c, cur, sizeof c); c.gpr[1] = ((uint32_t)cur->gpr[1] - 0x1000u) & ~0xFull;
                c.gpr[2] = TOC; c.gpr[3] = ent; c.gpr[4] = ah; c.gpr[5] = 1; c.gpr[6] = 0; c.lr = 0; g_active_ctx = &c; fp(&c);
                while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&c); }
                g_active_ctx = cur; rv = c.gpr[3]; okp = 1; }
            __except (EXCEPTION_EXECUTE_HANDLER) { g_active_ctx = cur; g_trampoline_fn = 0; }
        }
        printf("[kd] v1097K-2 play anim %08X on %08X: %s, returned %llu\n", ah, ent, okp ? "called" : "FAULT/missing", (unsigned long long)rv); fflush(stdout);
    }
    kd_log("after");
    g_kd_dump_req = 1;
}
static void kd_tick(void)
{
    if (!s_kd.ent) return;
    const double n = now_ms();
    if (n < s_kd.next) return;
    s_kd.next = n + 100.0;
    if (rd32(s_kd.ent + 0xC4) == 0 || !ok(rd32(s_kd.ent + 0x68))) { printf("[kd] entity gone\n"); s_kd.ent = 0; return; }
    kd_log("watch");
    if (n - s_kd.t0 > 4000.0) { s_kd.ent = 0; s_kd_cool = n + 6000.0; }
}

/* a new bullet on a live hit reaction: impulse on the part, sway, tone dip, blood behind */
static void hr_apply(rag_t* r)
{
    m3 Rc; v3 Tc; if (!ent_xform(r->ent, &Rc, &Tc)) return;
    const m3 Mc = w2mc(&Rc, s_conv);
    const v3 hm = mv(&Mc, sub(r->hr_hit_w, Tc));
    v3 dw = sub(r->hr_hit_w, r->hr_sh_w); dw.y *= 0.3f; if (len(dw) < 1e-3f) return; dw = nrm(dw);
    const v3 dm = mv(&Mc, dw);
    v3 pt; int arm2; float miss; const int part = hit_part(r->p, hm, &pt, &arm2, &miss);
    const float ds = dmg_scale(r->hr_dmg) * s_hit_scale, farf = miss > 0.35f ? 0.4f : 1.0f;   /* far off the body: weaker */
    if (s_kd_test && len(sub(r->hr_hit_w, r->hr_sh_w)) < 1.8f && r->hr_dmg < 600.0f) kd_try(r->ent);   /* v1097K probe */
    const int ip = part == D_LT ? D_UT : part;   /* pelvis follows the anim: a gut / hip shot folds the torso over it */
    v3 J = mul(dm, k_pmass[ip] * (part == D_LT ? 2.2f : k_hdv[part]) * ds * farf);
    if (arm2 >= 0) { const v3 Ja = mul(dm, k_pmass[arm2] * k_hdv[arm2] * ds * farf * 0.4f); jr_impulse(r->jh, arm2, &hm.x, &Ja.x); J = mul(J, 0.7f); }
    if (part == D_LT) {   /* v1069J gut / hip: the hips are pushed back (sway), the upper body hunches forward over the wound */
        const v3 up = add(r->p[THO], mul(sub(r->p[NEC], r->p[THO]), 0.6f)); const v3 Jf = mul(dm, -k_pmass[D_UT] * 2.2f * ds * farf);
        jr_impulse(r->jh, D_UT, &up.x, &Jf.x);
    } else jr_impulse(r->jh, ip, &pt.x, &J.x);
    /* whole-body sway about the feet: into the shot; leg hits tip towards the wounded side */
    r->hr_part = part; r->hr_leg = (part == D_ULL || part == D_LLL) ? 0 : (part == D_ULR || part == D_LLR) ? 1 : -1;
    float v0 = (part == D_UT || part == D_LT ? 1.6f : part == D_HEAD ? 0.9f : r->hr_leg >= 0 ? 1.4f : 0.5f) * ds * farf;
    v3 dirh = V(dm.x, 0, dm.z);
    if (r->hr_leg >= 0) { v3 side = sub(r->p[r->hr_leg ? HIR : HIL], r->p[PEL]); side.y = 0; dirh = add(mul(nrm(side), 0.8f), mul(dirh, 0.4f)); }
    if (len(dirh) > 1e-3f) { const v3 ax = nrm(cross(V(0, 1, 0), nrm(dirh))); r->hr_sww = add(r->hr_sww, mul(ax, v0)); }
    r->hr_t_hit = now_ms(); r->hr_fade_t0 = 0; r->hr_nhits++;
    if (r->hr_leg >= 0 && s_stumble && now_ms() > r->st_cool && r->p[PEL].y - r->ground > 0.70f) {   /* standing, leg hit */
        /* v1069U: what a leg hit does depends on where and how hard (user: "a knee shot mostly shouldn't stumble -- be
         * realistic"): knee -> the leg gives way in place (stumble 15 %); thigh 30 % + dmg/3000 (<= 80 %); shin 35 % +
         * dmg/4000 (<= 70 %); foot 55 % but a short hop; +25 % if that NPC took a leg hit in the last 3 s. Heavy hits mostly
         * stumble two steps, light ones one. Distance varies +-20 %. */
        const int kn = r->hr_leg ? KNR : KNL, an = r->hr_leg ? ANR : ANL;
        const float dk = len(sub(hm, r->p[kn])), da = fminf(len(sub(hm, r->p[an])), hm.y - r->ground);
        const int zone = dk < 0.10f ? 0 : (da < 0.12f) ? 3 : (part == D_ULL || part == D_ULR) ? 1 : 2;   /* 0 knee 1 thigh 2 shin 3 foot */
        static const char* const zn[4] = { "knee", "thigh", "shin", "foot" };
        const float dmg = r->hr_dmg;
        float pst = zone == 0 ? 0.15f : zone == 1 ? fminf(0.80f, 0.30f + dmg / 3000.0f) : zone == 2 ? fminf(0.70f, 0.35f + dmg / 4000.0f) : 0.55f;
        if (now_ms() - r->st_last_leg < 3000.0) pst = fminf(0.95f, pst + 0.25f);
        r->st_last_leg = now_ms();
        const float roll = frand();
        int mode;
        if (roll < pst) { const float p2 = zone == 3 ? 0.20f : zone == 0 ? 0.30f : dmg >= 1000.0f ? 0.70f : 0.35f; mode = frand() < p2 ? 3 : 2; }
        else mode = (zone == 0 || dmg >= 600.0f || frand() < 0.5f) ? 1 : 0;   /* no stumble: the leg buckles in place (knee: always) */
        const m3 McT = tr(&Mc);
        v3 side = sub(r->p[r->hr_leg ? HIR : HIL], r->p[PEL]); side.y = 0; side = nrm(side);
        v3 Lm = add(mul(side, 0.6f), mul(nrm(V(dm.x, 0, dm.z)), 0.7f)); Lm.y = 0;
        if (mode && len(Lm) > 0.05f) {
            Lm = nrm(Lm); v3 Lw = mv(&McT, Lm); Lw.y = 0; Lw = nrm(Lw);
            float st = 0.75f + dmg / 2000.0f; st = st > 1.35f ? 1.35f : st; st *= (0.85f + 0.3f * frand()) * s_hit_scale;
            float D = 0.0f;
            if (mode >= 2) {
                float dsc = 0.85f + dmg / 5000.0f; if (dsc > 1.3f) dsc = 1.3f;
                D = (mode == 3 ? 1.0f : 0.55f) * (0.8f + 0.4f * frand()) * dsc * s_hit_scale * (zone == 3 ? 0.6f : 1.0f);
                if (D > 1.4f) D = 1.4f;
                float tt; v3 nn; col_load(); const v3 o = add(Tc, V(0, 0.6f, 0));   /* no walking into walls */
                if (col_ray(o, Lw, D + 0.4f, &tt, &nn)) D = tt - 0.4f > 0.0f ? tt - 0.4f : 0.0f;
            }
            r->st_on = 1; r->st_mode = mode; r->st_leg = r->hr_leg; r->st_t0 = now_ms() + 90.0;   /* v1069V: starts only if the NPC survives the bullet */
            r->st_Lw = Lw; r->st_Lm = Lm; r->st_D = D; r->st_done = 0; r->st_s = st;
            r->st_T = mode == 3 ? 1.65f : mode == 2 ? 1.10f : 0.85f; r->st_mT = mode == 3 ? 1.05f : 0.62f;
            r->st_moves = 0; r->st_last_move = 0; r->st_cool = now_ms() + r->st_T * 1000.0 + 300.0;
            r->st_have_pose = 0;   /* frozen from the next fresh animation frame (stumble_pose) */
            HLOG("[rag-hit] ent=%08X %s leg, %s (roll %.2f vs %.2f): %s, %.2f m towards (%.2f %.2f), strength %.2f\n", r->ent, r->hr_leg ? "right" : "left", zn[zone], roll, pst,
                 mode == 3 ? "STUMBLE 2 steps" : mode == 2 ? "STUMBLE 1 step" : "buckles in place", D, Lw.x, Lw.z, st);
        } else HLOG("[rag-hit] ent=%08X %s leg, %s (roll %.2f vs %.2f): no stumble\n", r->ent, r->hr_leg ? "right" : "left", zn[zone], roll, pst);
    }
    const int melee_bl = s_mblood != 0 && len(sub(r->hr_hit_w, r->hr_sh_w)) < 1.8f && r->hr_dmg < 600.0f;   /* v1099B: melee bled in pain_hit_check */
    { static int bl = 0; if (!melee_bl && ((bl++ & 1) == 0 || r->hr_dmg >= 2000.0f)) blood_spray(r->hr_hit_w, r->hr_sh_w, r->hr_dmg, 0.55f, 1); }
    if (!melee_bl) blood_hit(r->ent, r->p, hm, part, r->hr_hit_w, r->hr_sh_w, r->hr_dmg, 0);   /* v1069B spray / artery */
    HLOG("[rag-hit] ent=%08X hit #%d: %s%s, dmg %.0f (x%.2f), %.2f m off the body, sway %.1f rad/s\n", r->ent, r->hr_nhits, k_pn[part],
         arm2 >= 0 ? " + shoulder" : "", r->hr_dmg, ds, miss, v0);
}
/* tone per part t s after the last hit: the hit part + its neighbours go slack and regain tone over 0.5 s */
static void hr_tones(const rag_t* r, float t, float* t11)
{
    for (int d = 0; d < ND; d++) {
        const int legp = d >= D_ULL, hp = r->hr_part;
        float m;
        if (d == hp) m = legp ? 0.02f : (d == D_UT ? 0.03f : 0.10f);
        else if (hp >= 0 && (k_dp[d] == hp || k_dp[hp] == d)) m = 0.22f;
        else m = legp ? 0.60f : (d == D_HEAD ? 0.90f : 0.45f);
        if (r->hr_leg >= 0 && legp && ((r->hr_leg == 0) == (d == D_ULL || d == D_LLL))) m = fminf(m, 0.05f);
        t11[d] = m + (1.0f - m) * smooth01(t / 0.5f);
    }
}
/* the corrected pose: every slot = the game's animated bone moved with its part's physics offset (weight w) */
static void hr_write(rag_t* r, const float* C, const float* Xp, const float* Xa, float w)
{
    m3 Cm[ND]; v3 P[ND], A[ND]; const m3 I = I3();
    for (int d = 0; d < ND; d++) { m3 c; memcpy(c.m, C + d * 9, 36); Cm[d] = w < 0.999f ? mblend(&I, &c, w) : c;
        P[d] = V(Xp[d * 3], Xp[d * 3 + 1], Xp[d * 3 + 2]); A[d] = V(Xa[d * 3], Xa[d * 3 + 1], Xa[d * 3 + 2]); }
    for (int sl = 0; sl < r->nslot; sl++) {
        const uint32_t m = r->pal + (uint32_t)sl * 64u;
        const v3 x = V(rdf(m + 0x30), rdf(m + 0x34), rdf(m + 0x38));
        if (len(x) < 0.03f || !fok(x.x)) continue;   /* root / motion bones */
        const int d = r->drv[sl];
        v3 pos = add(P[d], mv(&Cm[d], sub(x, A[d]))); m3 Rm = Cm[d];
        if (r->w2[sl] > 0.0f) { const int d2 = r->drv2[sl]; const float ww = r->w2[sl];
            const v3 p2 = add(P[d2], mv(&Cm[d2], sub(x, A[d2]))); pos = add(mul(pos, 1.0f - ww), mul(p2, ww)); Rm = mblend(&Cm[d], &Cm[d2], ww); }
        pos = add(x, mul(sub(pos, x), w));
        if (!fok(pos.x) || !fok(pos.y) || !fok(pos.z)) continue;
        for (int k = 0; k < 3; k++) { const uint32_t o = m + (uint32_t)k * 16u;
            const v3 row = mv(&Rm, V(rdf(o), rdf(o + 4), rdf(o + 8)));
            wrf(o, row.x); wrf(o + 4, row.y); wrf(o + 8, row.z); }
        wrf(m + 0x30u, pos.x); wrf(m + 0x34u, pos.y); wrf(m + 0x38u, pos.z);
    }
}
/* v1069S stumble pose program (model space, t s since the stumble started), on top of anim + physics */
static float bump(float t, float t0, float t1) { if (t <= t0 || t >= t1) return 0.0f; return sinf(3.14159265f * (t - t0) / (t1 - t0)); }
static void stumble_pose(rag_t* r, const v3* a)
{
    float ex[ND * 3]; memset(ex, 0, sizeof ex); v3 root = V(0, 0, 0);
    const float t = (float)((now_ms() - r->st_t0) / 1000.0), T = r->st_T;
    if (r->st_on) { const float hp = rdf(r->ent + 0x14C8); if (!(fok(hp) && hp > 0.0f)) {   /* v1069V: dead -> no stumble */
        HLOG("[rag-hit] ent=%08X: died -- stumble cancelled after %.2f m\n", r->ent, r->st_done); r->st_on = 0; r->st_D = r->st_done; } }
    if (r->st_on && t < 0.0f) { jr_hr_base(r->jh, NULL); jr_hr_extra(r->jh, NULL, NULL); return; }   /* waiting for the 90 ms */
    if (r->st_on && t < T) {
        /* v1069T: the stumble owns the legs + hips (pose frozen at the hit + the program), the upper body is a 50/50 mix
         * with the game's own pain animation; in over 0.06 s, out over the last 0.4 s */
        if (!r->st_have_pose) { memcpy(r->st_pose, a, sizeof r->st_pose); r->st_have_pose = 1; }
        { const float wv = t < 0.06f ? t / 0.06f : t < T - 0.4f ? 1.0f : (T - t) / 0.4f;
          v3 base[NP];
          for (int i = 0; i < NP; i++) {
              const int leg = (i == PEL || i == LUM || i == HIL || i == KNL || i == ANL || i == HIR || i == KNR || i == ANR || i == TOL || i == TOR);
              const float w = smooth01(wv) * (leg ? 1.0f : 0.5f);
              base[i] = add(mul(a[i], 1.0f - w), mul(r->st_pose[i], w));
          }
          jr_hr_base(r->jh, &base[0].x); }
        const v3 up = V(0, 1, 0), lat = nrm(sub(a[HIR], a[HIL])), fwd = nrm(cross(up, lat)), L = r->st_Lm;
        const float k = r->st_s; const int mode = r->st_mode;
        /* v1069U program: buckle (fast in, slow out); step 1 = the good leg catches; step 2 (two-step) = the shot leg limps
         * through and gives way again as the weight lands on it */
        float b1 = t < 0.12f ? t / 0.12f : 1.0f - (t - 0.12f) / (mode == 1 ? 0.65f : 0.50f); b1 = smooth01(b1);
        const float b2 = mode == 3 ? 0.75f * bump(t, 0.60f, 1.15f) : 0.0f, b = fmaxf(b1, b2);
        const float h1 = mode >= 2 ? bump(t, 0.05f, 0.60f) : 0.0f, k1 = mode >= 2 ? bump(t, 0.05f, 0.45f) : 0.0f;
        const float h2 = mode == 3 ? bump(t, 0.55f, 1.10f) : 0.0f, k2 = mode == 3 ? bump(t, 0.55f, 1.00f) : 0.0f;
        const float arms = bump(t, 0.03f, T - 0.15f), lean = bump(t, 0.0f, T - 0.25f);
        const int th = r->st_leg ? D_ULR : D_ULL, sh = r->st_leg ? D_LLR : D_LLL, tf = r->st_leg ? D_ULL : D_ULR, sf = r->st_leg ? D_LLL : D_LLR;
        /* steps: forward / back with the lurch, sideways only AWAY from the shot leg (never across it) */
        const v3 away = r->st_leg ? mul(lat, -1.0f) : lat;   /* lat points left -> right; shot left leg: step out to the right */
        v3 Ls = sub(L, mul(lat, dot(L, lat))); { const float aw = dot(L, away); if (aw > 0) Ls = add(Ls, mul(away, aw * 0.5f)); }
        if (len(Ls) < 0.2f) Ls = mul(fwd, -1.0f);
        const v3 step_ax = nrm(cross(nrm(Ls), up)), lurch_ax = nrm(cross(up, L));
        const float bk = mode == 1 ? 1.15f : 1.0f;   /* in place: the knee gives way harder */
        v3 e;
        e = add(mul(lat, 0.35f * b * k * bk), mul(step_ax, 0.25f * h2 * k));   memcpy(&ex[th * 3], &e, 12);   /* shot leg: hip flexes (+ low limp step) */
        e = mul(lat, -(0.95f * b * bk + 0.35f * k2) * k);                       memcpy(&ex[sh * 3], &e, 12);   /* ... the knee gives way */
        e = add(mul(step_ax, 0.45f * h1 * k), mul(lat, 0.30f * b2 * k));        memcpy(&ex[tf * 3], &e, 12);   /* good leg: catch step, then carries the weight */
        e = mul(lat, -(0.62f * k1 + 0.70f * b2) * k);                           memcpy(&ex[sf * 3], &e, 12);   /*   (knee lifts the foot / bends under the dip) */
        const float aw = mode == 1 ? 0.45f : 0.75f;
        e = mul(fwd, aw * arms * k);                                            memcpy(&ex[D_UAL * 3], &e, 12);   /* arms out for balance */
        e = mul(fwd, -aw * arms * k);                                           memcpy(&ex[D_UAR * 3], &e, 12);
        e = mul(mode == 1 ? mul(fwd, -1.0f) : lurch_ax, (mode == 1 ? 0.18f : 0.30f) * lean * k); memcpy(&ex[D_UT * 3], &e, 12);   /* chest into it / doubles over */
        root = V(0, -(mode == 1 ? 0.19f : 0.17f) * b * k, 0);                                                          /* hips drop */
        jr_hr_extra(r->jh, ex, &root.x);
    } else { jr_hr_base(r->jh, NULL); if (r->st_on) { r->st_on = 0; HLOG("[rag-hit] ent=%08X: stumble done -- moved %.2f of %.2f m in %d steps\n", r->ent, r->st_done, r->st_D, r->st_moves); }
             jr_hr_extra(r->jh, NULL, NULL); }
}
/* the NPC really moves (vt+0x30 SetMatrix + vt+0x24 position block, as gf_coop_combat.c place_char): called from the pain
 * machine's hooks (game thread, inside the NPC's own update), so the AI and the character controller take the new spot */
static void stumble_move(uint32_t ent)
{
    for (int i = 0; i < MAXR; i++) {
        rag_t* r = &s_rg[i];
        if (!r->hr || r->ent != ent || r->state < 1 || r->state >= 5 || !r->st_on || r->st_D <= 0.01f) continue;
        const double n = now_ms(); if (n - r->st_last_move < 4.0 || n < r->st_t0) return;
        { const float hp = rdf(ent + 0x14C8); if (!(fok(hp) && hp > 0.0f)) { r->st_D = r->st_done; return; } }   /* v1069V: never move a dead NPC */
        float x = (float)((n - r->st_t0) / (r->st_mT * 1000.0)); if (x > 1.0f) x = 1.0f;
        const float want = r->st_D * (1.0f - powf(1.0f - x, 1.7f)), dd = want - r->st_done;
        if (dd < 0.002f) return;
        const uint32_t xf = rd32(ent + 0x68); ppu_context* cur = g_active_ctx;
        gfn f30 = lookup(vfunc(ent, 0x30)), f24 = lookup(vfunc(ent, 0x24));
        if (!ok(xf) || !cur || !f30 || !f24) { r->st_D = 0; return; }
        const uint32_t buf = (((uint32_t)cur->gpr[1] - 0x800u) & ~0xFu);
        for (uint32_t k = 0; k < 64; k += 4) wr32(buf + k, rd32(xf + 0x10u + k));
        const float px = rdf(buf + 0x30) + r->st_Lw.x * dd, py = rdf(buf + 0x34), pz = rdf(buf + 0x38) + r->st_Lw.z * dd;
        if (!fok(px) || !fok(pz)) { r->st_D = 0; return; }
        wrf(buf + 0x30, px); wrf(buf + 0x38, pz);
        if (!gcall(f30, ent, buf, 0)) { r->st_D = 0; HLOG("[rag-hit] ent=%08X: stumble move faulted -- stopped\n", ent); return; }
        const uint32_t blk = buf + 0x40u; wrf(blk, px); wrf(blk + 4, py); wrf(blk + 8, pz); wr32(blk + 12, 2u);
        gcall(f24, ent, blk, 0);
        r->st_done += dd; r->st_moves++; r->st_last_move = n;
        return;
    }
}
/* one game update of a live hit reaction (called with the game's fresh animation in the palette) */
static void hr_frame(rag_t* r, float dt)
{
    const double n = now_ms();
    if (rd32(r->ent) != r->vt || rd32(r->ent + 0xC4) != r->anim || anim_pal(r->anim) != r->pal) { rag_release(r); return; }
    { const float hp = rdf(r->ent + 0x14C8); if (!(fok(hp) && hp > 0.0f)) { HLOG("[rag-hit] ent=%08X: died during the hit reaction -> released\n", r->ent); rag_release(r); return; } }   /* v1069V */
    if (r->state == 1) {   /* start: drivers from the current pose, body built on it, first bullet */
        read_particles(r, r->snap1);
        if (!rag_start(r, dt)) { rag_release(r); return; }
        r->jh = jr_hr_create(&r->p0[0].x, r->ground);
        if (!r->jh) { rag_release(r); return; }
        memcpy(r->p, r->p0, sizeof r->p);
        r->state = 3; r->t0 = n; r->hr_pending = 1; r->hr_part = -1; r->hr_leg = -1;
    }
    v3 a[NP]; read_particles(r, a);
    for (int i = 0; i < NP; i++) if (!fok(a[i].x) || !fok(a[i].y) || !fok(a[i].z)) { rag_release(r); return; }
    if (r->hr_pending) { r->hr_pending = 0; hr_apply(r); }
    const float t = (float)((n - r->hr_t_hit) / 1000.0);
    float t11[ND]; hr_tones(r, t, t11);
    { const float w = 2.0f * 3.14159265f * 2.0f, z = 0.5f;   /* sway: 2 Hz spring, half damped */
      const v3 acc = sub(mul(r->hr_sw, -w * w), mul(r->hr_sww, 2.0f * z * w));
      r->hr_sww = add(r->hr_sww, mul(acc, dt)); r->hr_sw = add(r->hr_sw, mul(r->hr_sww, dt));
      if (len(r->hr_sw) > 0.35f) r->hr_sw = mul(nrm(r->hr_sw), 0.35f); }
    const v3 piv = mul(add(a[ANL], a[ANR]), 0.5f);
    jr_hr_target(r->jh, &a[0].x, t11, &r->hr_sw.x, &piv.x);
    stumble_pose(r, a);
    jr_hr_step(r->jh, dt);
    float C[ND * 9], Xp[ND * 3], Xa[ND * 3], md = 0;
    if (!jr_hr_get(r->jh, C, Xp, Xa, &r->p[0].x, &md)) { rag_release(r); return; }
    for (int i = 0; i < ND * 9; i++) if (!fok(C[i])) { HLOG("[rag-hit] ent=%08X: physics blew up -> released\n", r->ent); rag_release(r); return; }
    if (r->hr_fade_t0 <= 0 && !r->st_on && ((t > 0.45f && md < 2.5f && len(r->hr_sw) < 0.01f) || t > 1.6f)) r->hr_fade_t0 = n;
    float w = 1.0f;
    if (r->hr_fade_t0 > 0) { w = 1.0f - (float)((n - r->hr_fade_t0) / 150.0); if (w <= 0.0f) {
        HLOG("[rag-hit] ent=%08X: recovered after %.2f s (%d hit%s)\n", r->ent, (n - r->t0) / 1000.0, r->hr_nhits, r->hr_nhits == 1 ? "" : "s");
        rag_release(r); return; } }
    r->frames++;
    hr_write(r, C, Xp, Xa, w);
}
/* a bullet on a death ragdoll that is still simulating (or has settled): push it where it was hit */
/* v1091F (user screenshot: an arm inside the chest): pelvis-upper arms, head-upper arms and pelvis-head start with no
 * self-collision (hanging arms overlap the hip box at the death). Once the body has settled, each pair whose parts are clear
 * of each other (segment distance > both radii + 2 cm) is switched on, so later shots / kicks / the car cannot push them through. */
static float segseg(v3 a0, v3 a1, v3 b0, v3 b1)
{
    float best = 1e9f;
    for (int i = 0; i <= 8; i++) { const v3 p = add(a0, mul(sub(a1, a0), i / 8.0f)); v3 cp; const float d = segcp(p, b0, b1, &cp); if (d < best) best = d; }
    for (int i = 0; i <= 8; i++) { const v3 p = add(b0, mul(sub(b1, b0), i / 8.0f)); v3 cp; const float d = segcp(p, a0, a1, &cp); if (d < best) best = d; }
    return best;
}
static void selfcoll_on(rag_t* r)
{
    if (!r->jh || r->hr || r->selfc) return;
    r->selfc = 1;
    struct { int a, b; v3 a0, a1, b0, b1; float rr; } pr[5] = {
        { D_LT, D_UAL, r->p[PEL], r->p[LUM], r->p[SHL], r->p[ELL], 0.14f + 0.062f }, { D_LT, D_UAR, r->p[PEL], r->p[LUM], r->p[SHR], r->p[ELR], 0.14f + 0.062f },
        { D_HEAD, D_UAL, r->p[NEC], r->p[HED], r->p[SHL], r->p[ELL], 0.10f + 0.062f }, { D_HEAD, D_UAR, r->p[NEC], r->p[HED], r->p[SHR], r->p[ELR], 0.10f + 0.062f },
        { D_LT, D_HEAD, r->p[PEL], r->p[LUM], r->p[NEC], r->p[HED], 0.14f + 0.10f } };
    int n = 0;
    for (int k = 0; k < 5; k++) if (segseg(pr[k].a0, pr[k].a1, pr[k].b0, pr[k].b1) > pr[k].rr + 0.02f) { jr_coll_pair(r->jh, pr[k].a, pr[k].b, 1); n++; }
    RLOG("[rag] v1091F ent=%08X: %d of 5 shoulder / neck self-collision pairs on\n", r->ent, n);
}
static float s_shot_boost = 0.0f, s_corpse_torso = 0.2f;   /* v1091E GF_CORPSE_TORSO: torso share of a corpse hit (0.45 slid the whole body = glide) */   /* v1091D: > 0 = a corpse hit (corpse_fire): this speed for the hit part + a torso share */
static void rag_shot(rag_t* r, v3 hit, v3 sh, float dmg)
{
    const m3 Md = w2mc(&r->Rd, r->conv);
    const v3 hm = mv(&Md, sub(hit, r->Td));
    v3 dw = sub(hit, sh); dw.y *= 0.3f; if (len(dw) < 1e-3f) return; dw = nrm(dw);
    const v3 dm = mv(&Md, dw);
    v3 pt; int arm2; float miss; const int part = hit_part(r->p, hm, &pt, &arm2, &miss);
    if (miss > 0.6f) { HLOG("[rag-hit] dying ent=%08X: hit %.2f m off the body -- ignored\n", r->ent, miss); return; }
    float dv = (1.5f + dmg / 4000.0f); if (dv > 3.0f) dv = 3.0f; dv *= s_hit_scale;   /* v1069V: was 2.5 + dmg/2000 (<= 4.5): bodies slid */
    if (r->state == 4 || r->p[PEL].y - r->ground < 0.25f) dv *= 0.8f;   /* already down (v1091C: 0.8, was 0.5 -- user wants dead bodies to react) */
    v3 dmu = dm; if (s_shot_boost > 0.0f) { dv = s_shot_boost; dmu = nrm(add(dm, V(0, 0.5f, 0))); }   /* v1091D lift; v1091E 0.5 (was 0.25): the part jumps instead of sliding */
    /* v1091H (user: "still moves a bit on its own if I shoot it a lot"): corpse hits STACKED -- every bullet added 4 m/s on top of the
     * speed the part already had, so a burst built into a long slide. Now a corpse hit tops the part up to the target speed along
     * the shot (at least 25 % of it, so every hit still twitches); same for the torso share. */
    #define TOPUP(prt, pnt, want) do { float vc_[3] = { 0, 0, 0 }; jr_part_vel(r->jh, (prt), &(pnt).x, vc_); \
        const float al_ = vc_[0] * dmu.x + vc_[1] * dmu.y + vc_[2] * dmu.z; float dv_ = (want) - (al_ > 0 ? al_ : 0); \
        if (dv_ < 0.25f * (want)) dv_ = 0.25f * (want); const v3 Jx_ = mul(dmu, k_pmass[prt] * dv_); jr_impulse(r->jh, (prt), &(pnt).x, &Jx_.x); } while (0)
    int okp = 1;
    if (s_shot_boost > 0.0f) {   /* v1091D (user: "they don't react enough"): the whole body is shoved, not only the hit limb */
        TOPUP(part, pt, dv);
        if (part != D_LT) TOPUP(D_LT, r->p[PEL], dv * s_corpse_torso);
        if (part != D_UT) TOPUP(D_UT, r->p[THO], dv * s_corpse_torso);
    } else { const v3 J = mul(dmu, k_pmass[part] * dv); okp = jr_impulse(r->jh, part, &pt.x, &J.x); }
    #undef TOPUP
    if (r->state == 4) r->state = 3;
    r->still_ms = 0; r->t_wake = now_ms();
    blood_spray(hit, sh, dmg, 0.8f, 2);
    blood_hit(r->ent, r->p, hm, part, hit, sh, dmg, 1);   /* v1069B; v1091C more */
    HLOG("[rag-hit] dying ent=%08X: bullet on the %s (%.1f m/s)%s\n", r->ent, k_pn[part], dv, okp ? "" : " -- part not pushable");
}
static uint32_t player_ent(void)
{
    uint32_t a = rd32(0x10181290u); if (!ok(a)) return 0; a = rd32(a + 4); if (!ok(a)) return 0; a = rd32(a);
    return (ok(a) && a >= 0x17FCu) ? a - 0x17FCu : 0;
}
/* the PainStateMachine's damage record (sm+0x54: +0x60 shooter, +0x6C hit point, +0x78 damage) changed = a new bullet */
typedef struct { uint32_t ent, sm; v3 hit; float dmg; double t; } hobs_t;
enum { NHOBS = 256 };
static hobs_t s_hobs[NHOBS];
static void hr_new(uint32_t ent, v3 hit, v3 sh, float dmg)
{
    int act = 0, pick = -1;
    for (int i = 0; i < MAXR; i++) { if (s_rg[i].hr && s_rg[i].state >= 1 && s_rg[i].state < 5) act++;
        else if (pick < 0 && (s_rg[i].state == 0 || s_rg[i].state == 5)) pick = i; }
    if (act >= s_hit_max || pick < 0) return;
    const uint32_t anim = rd32(ent + 0xC4);
    if (!ok(anim) || !ok(anim_pal(anim))) return;
    rag_t* r = &s_rg[pick]; if (r->jh) jr_destroy(r->jh); memset(r, 0, sizeof *r);
    r->ent = ent; r->anim = anim; r->pal = anim_pal(anim); r->vt = rd32(ent); r->t0 = now_ms(); r->src38 = ent + 0x38u;
    r->hr = 1; r->hit_leg = r->hit_arm = -1; r->hr_part = -1;
    r->hr_hit_w = hit; r->hr_sh_w = sh; r->hr_dmg = dmg; r->hit_w = hit; r->shooter_w = sh; r->dmg = dmg;
    if (!rag_bones(r, ent, anim)) { r->state = 0; return; }
    r->state = 1;
}
/* ------------------------------------------------------------------ v1097N punch knockdowns
 * User: "punch someone and it's visible that I knocked them back and they maybe even fell, something like RDR2"; v1096M (stronger
 * additive reactions) was rejected: physics and the game's animation fought and the animation won. Rule now: ONE owner at a time.
 *  - trigger (pain_hit_check): a melee hit (<= 1.8 m, < 600 dmg) on a live NPC, not while the player grabs (+0x5C4 & 4);
 *    dmg >= GF_KD_DMG (300) -> GF_KD_CHANCE (0.35); hp < 350 -> +GF_KD_LOW (0.30); same NPC again after 8 s; <= 2 at once.
 *  - fall: the death ragdoll on the living NPC (r->kd: no wound / clutch / blood pool, entity pinned where it stood) + a
 *    knock-back along the punch (upper body GF_KD_PUSH x 3 m/s, hips 0.65, knees 0.25, feet 0.05 -> topples backwards).
 *  - down: settled + GF_KD_DOWN ms (900).
 *  - get-up: entity root moved to the body (pelvis, head direction = forward), the game's recovery anims played on the
 *    main controller: flatToCrawl 0xBF6796A9, then crawlToStandBlocking 0x2BC4B458 (hashes from the recoverStateTable
 *    builder func_001A5664, func_004E0FE8 args; 2.6 s / ? s, not interrupted by the AI in f8_kd3). The ragdoll pose
 *    blends out over GF_KD_BLEND s (0.6).
 * GF_KD=0 off. Log [kd]. */
static int s_kd_on = 1; static float s_kd_dmg = 300.0f, s_kd_chance = 0.35f, s_kd_low = 0.30f, s_kd_push = 1.0f, s_kd_blend = 0.6f; static double s_kd_down = 900.0;
static struct { uint32_t ent; double t; } s_kd_cool_t[32];
#define KD_FLAT  0xBF6796A9u
#define KD_STAND 0x2BC4B458u
static int gcallx(gfn f, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, double f1, uint64_t* ret)
{
    ppu_context* cur = g_active_ctx;
    if (!cur || !f) return 0;
    __try {
        static ppu_context c; memcpy(&c, cur, sizeof c);
        c.gpr[1] = ((uint32_t)cur->gpr[1] - 0x1000u) & ~0xFull; c.gpr[2] = TOC;
        c.gpr[3] = a0; c.gpr[4] = a1; c.gpr[5] = a2; c.gpr[6] = a3; c.fpr[1] = f1; c.lr = 0;
        g_active_ctx = &c; f(&c);
        while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&c); }
        g_active_ctx = cur; if (ret) *ret = c.gpr[3]; return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_active_ctx = cur; g_trampoline_fn = 0; return 0; }
}
static int kd_play(uint32_t ent, uint32_t hash)
{
    uint64_t rv = 0; const int okc = gcallx(lookup(0x00460CE8u), ent, hash, 1, 0, 0.0, &rv);   /* func_00460CE8(ent, hash, 1, 0) */
    return okc && rv;
}
static int kd_wanted(uint32_t ent, v3 hit, v3 sh, float dmg, float hp)
{
    if (!s_kd_on || !(fok(hp) && hp > 0.0f) || len(sub(hit, sh)) > 1.8f || dmg >= 600.0f) return 0;
    const uint32_t pl = player_ent(); if (ok(pl) && (rd32(pl + 0x5C4) & 4u)) return 0;   /* grabbing: the game's throws knock down */
    const double n = now_ms(); int act = 0;
    for (int i = 0; i < MAXR; i++) if (s_rg[i].kd && s_rg[i].state >= 1 && s_rg[i].state < 5) { if (s_rg[i].ent == ent) return 0; act++; }
    if (act >= 2) return 0;
    for (int i = 0; i < 32; i++) if (s_kd_cool_t[i].ent == ent && n - s_kd_cool_t[i].t < 8000.0) return 0;
    float pch = dmg >= s_kd_dmg ? s_kd_chance : 0.0f; if (hp < 350.0f) pch += s_kd_low;
    const float roll = frand();
    HLOG("[kd] ent=%08X punch dmg %.0f hp %.0f: knockdown chance %.2f, roll %.2f%s\n", ent, dmg, hp, pch, roll, roll < pch ? " -> KNOCKDOWN" : "");
    return roll < pch;
}
static void kd_cool(uint32_t ent)
{
    int o = 0; for (int i = 0; i < 32; i++) { if (s_kd_cool_t[i].ent == ent) { o = i; break; } if (s_kd_cool_t[i].t < s_kd_cool_t[o].t) o = i; }
    s_kd_cool_t[o].ent = ent; s_kd_cool_t[o].t = now_ms();
}
static int kd_start(uint32_t ent, v3 hit, v3 sh, float dmg)
{
    for (int i = 0; i < MAXR; i++) if (s_rg[i].ent == ent && s_rg[i].state && s_rg[i].state < 5) { if (!s_rg[i].hr) return 0; rag_release(&s_rg[i]); }
    const uint32_t anim = rd32(ent + 0xC4); if (!ok(anim) || !ok(anim_pal(anim))) return 0;
    int pick = -1; for (int i = 0; i < MAXR; i++) if (s_rg[i].state == 0 || s_rg[i].state == 5) { pick = i; break; }
    if (pick < 0) return 0;
    rag_t* r = &s_rg[pick]; if (r->jh) jr_destroy(r->jh); memset(r, 0, sizeof *r);
    r->ent = ent; r->anim = anim; r->pal = anim_pal(anim); r->vt = rd32(ent); r->t0 = now_ms(); r->src38 = ent + 0x38u;
    r->shooter_w = sh; r->hit_w = hit; r->dmg = dmg; r->kd = 1;
    if (!rag_bones(r, ent, anim)) { r->state = 0; r->kd = 0; return 0; }
    r->state = 1; r->hit_leg = r->hit_arm = -1; r->bhv_zone = -1; r->bhv_on = 0; r->bhv_last = -1; r->bh.prog = BHV_NONE;
    kd_cool(ent); g_kd_dump_req = 1;   /* frames, only with GF_KD_DUMP */
    { uint64_t rv = 0; const int okh = gcallx(lookup(0x00234994u), ent, 1, 0, 0, 6.0, &rv);   /* the AI's generic hold request, 6 s */
      HLOG("[kd] v1097N ent=%08X KNOCKDOWN start (dmg %.0f, hp %.0f), AI hold %s\n", ent, dmg, rdf(ent + 0x14C8), okh ? "ok" : "FAILED"); }
    return 1;
}
/* right after rag_start: no dying behaviour, and the punch knocks the body back (top more than the feet) */
static void kd_knock(rag_t* r, float dt)
{
    r->have_wound = 0; r->bhv_zone = -1;
    v3 dw = sub(r->hit_w, r->shooter_w); dw.y = 0; if (len(dw) < 1e-3f) return; dw = nrm(dw);
    const m3 Md = w2mc(&r->Rd, r->conv); const v3 dm = mv(&Md, dw);
    for (int i = 0; i < NP; i++) {
        const float y = r->p[i].y - r->ground;
        const float f = y > 1.2f ? 1.0f : y > 0.8f ? 0.65f : y > 0.35f ? 0.25f : 0.05f;
        r->pp[i] = sub(r->pp[i], mul(dm, s_kd_push * 3.0f * f * (0.85f + 0.3f * frand()) * dt));
    }
    HLOG("[kd] ent=%08X knocked back along (%.2f %.2f), push x%.2f\n", r->ent, dw.x, dw.z, s_kd_push);
}
static void kd_getup_begin(rag_t* r, double n)
{
    const m3 Md = w2mc(&r->Rd, r->conv), MdT = tr(&Md);
    const v3 pW = add(r->Td, mv(&MdT, r->p[PEL])), hW = add(r->Td, mv(&MdT, r->p[HED]));
    const v3 fw = mv(&MdT, mul(body_fwd(r->p), r->fwd_sign));
    v3 hd = sub(hW, pW); hd.y = 0; if (len(hd) < 0.05f) hd = V(r->Rd.m[6], 0, r->Rd.m[8]); hd = nrm(hd);
    const float ac = atan2f(r->Rd.m[6], r->Rd.m[8]), at = atan2f(hd.x, hd.z), d = at - ac, cd = cosf(d), sd = sinf(d);
    m3 R = r->Rd;
    for (int k = 0; k < 3; k++) { const float x = R.m[k * 3], z = R.m[k * 3 + 2]; R.m[k * 3] = x * cd + z * sd; R.m[k * 3 + 2] = -x * sd + z * cd; }
    const v3 T = V(pW.x, r->Td.y, pW.z);
    const uint32_t ent = r->ent, xf = rd32(ent + 0x68); ppu_context* cur = g_active_ctx;
    gfn f30 = lookup(vfunc(ent, 0x30)), f24 = lookup(vfunc(ent, 0x24));
    int moved = 0;
    if (ok(xf) && cur && f30 && f24) {
        const uint32_t buf = (((uint32_t)cur->gpr[1] - 0x800u) & ~0xFu);
        for (uint32_t k = 0; k < 64; k += 4) wr32(buf + k, rd32(xf + 0x10u + k));
        for (int i = 0; i < 9; i++) wrf(buf + (uint32_t)(i / 3) * 0x10u + (uint32_t)(i % 3) * 4u, R.m[i]);
        wrf(buf + 0x30, T.x); wrf(buf + 0x34, T.y); wrf(buf + 0x38, T.z);
        if (gcall(f30, ent, buf, 0)) { const uint32_t blk = buf + 0x40u; wrf(blk, T.x); wrf(blk + 4, T.y); wrf(blk + 8, T.z); wr32(blk + 12, 2u); gcall(f24, ent, blk, 0); moved = 1; }
    }
    if (ok(xf)) {   /* both matrices, as the v1081P pin does */
        for (int i = 0; i < 9; i++) { const uint32_t o = (uint32_t)(i / 3) * 0x10u + (uint32_t)(i % 3) * 4u; wrf(xf + 0x10u + o, R.m[i]); wrf(xf + 0x50u + o, R.m[i]); }
        wrf(xf + 0x40, T.x); wrf(xf + 0x44, T.y); wrf(xf + 0x48, T.z); wrf(xf + 0x80, T.x); wrf(xf + 0x84, T.y); wrf(xf + 0x88, T.z);
    }
    const int pl = kd_play(ent, KD_FLAT);
    r->kd_ph = 1; r->kd_t1 = n; r->kd_bw = 1.0f;
    HLOG("[kd] ent=%08X down %.2f s -> GET UP (%s, root moved %.2f m %s, turned %.0f deg), flatToCrawl %s\n", ent, (n - r->t0) / 1000.0, fw.y < 0 ? "face down" : "FACE UP",
         len(sub(T, r->Td)), moved ? "via the game" : "direct", d * 57.3f, pl ? "playing" : "REFUSED");
    if (!pl) { r->kd_ph = 3; }
}
static void kd_frame(rag_t* r, double n)
{
    const float hp = rdf(r->ent + 0x14C8);
    if (rd32(r->ent) != r->vt || rd32(r->ent + 0xC4) != r->anim || !(fok(hp) && hp > 0.0f)) { HLOG("[kd] ent=%08X gone / died during the get-up -> released\n", r->ent); rag_release(r); return; }
    if (!r->kd_ph) {   /* down on the ground */
        if (!r->kd_set) { r->kd_set = n; HLOG("[kd] ent=%08X landed after %.2f s\n", r->ent, (n - r->t0) / 1000.0); }
        if (n - r->kd_set > s_kd_down) kd_getup_begin(r, n);
        else { write_pose(r); return; }
    }
    const uint32_t h = rd32(r->anim + 0x10);
    const float t = (float)((n - r->kd_t1) / 1000.0);
    r->kd_bw = r->kd_ph == 1 ? 1.0f - smooth01(t / s_kd_blend) : 0.0f;
    if (r->kd_ph == 1 && ((t > 0.3f && h != KD_FLAT) || t > 3.2f)) {
        const int pl = kd_play(r->ent, KD_STAND); r->kd_ph = 2; r->kd_t1 = n; r->kd_bw = 0.0f;
        HLOG("[kd] ent=%08X flatToCrawl done after %.2f s (anim now %08X) -> crawlToStand %s\n", r->ent, t, h, pl ? "playing" : "REFUSED");
        if (!pl) r->kd_ph = 3;
        return;
    }
    if (r->kd_ph == 3 || (r->kd_ph == 2 && ((t > 0.3f && h != KD_STAND) || t > 4.0f))) {
        HLOG("[kd] ent=%08X back on its feet after %.2f s (anim now %08X)\n", r->ent, (n - r->t0) / 1000.0, h);
        kd_cool(r->ent); rag_release(r); r->kd = 0; return;
    }
    if (r->kd_bw > 0.001f) write_pose(r);
}

/* v1099B melee blood (user: "blood ... also come from other melee stuff, like kicks"): before, punches bled only from inside a
 * live hit reaction (hr_apply), so a hit with no reaction slot / on a body in another state made none. Now every close hit
 * (<= 1.8 m, < 600 dmg) on a living NPC bleeds here: a spray from the impact (count as a 300+ dmg hit), a splat on the floor
 * under it (60 %), a wall splat behind (35 %); hr_apply skips its own blood for these. GF_MELEE_BLOOD=0 off. Log [blood-mel]. */
static void melee_blood(uint32_t ent, v3 hit, v3 sh, float dmg)
{
    if (s_mblood < 0) { const char* e = getenv("GF_MELEE_BLOOD"); s_mblood = (e && *e == '0') ? 0 : 1; }
    if (!s_mblood) return;
    spray_puff(hit, sh, dmg < 300.0f ? 300.0f + dmg : dmg + 300.0f);
    col_load(); const float g = ground_at(hit);
    if (fok(g) && frand() < 0.60f) { v3 q = hit; q.x += frand2(-0.25f, 0.25f); q.z += frand2(-0.25f, 0.25f); q.y = g; blood_ground(q, frand2(0.18f, 0.32f), 1, 0.85f, 0.10f); }
    if (frand() < 0.35f) blood_spray(hit, sh, dmg, 0.55f, 0);
    wound_add(ent, hit, sh, 0.06f, 3);   /* v1117A: punches / kicks leave blood on the face / body */
    static int nl = 0; if (nl < 300) { nl++; printf("[blood-mel] v1099B ent=%08X melee hit dmg %.0f from %.2f m -> blood\n", ent, dmg, len(sub(hit, sh))); fflush(stdout); }
}

static void name_probe_ent(uint32_t t, uint32_t pl);   /* v1102N-2 */
/* v1113S GF_SND_PROBE=1 (diagnostic): "[sndprobe] <qpc> <kind> ..." lines (QueryPerformanceCounter ticks, same clock as
 * cellAudio.c's AUDIO_WAV .idx file) for every player shot (clip change), every bullet the game registered on an NPC and every
 * corpse hit of our own trace -- used offline to find which GAM.sbk sample is the bullet-into-body sound. */
static int s_sndp = -1;
static void snd_mark(const char* kind, uint32_t ent, float a, float b)
{
    if (s_sndp < 0) { const char* e = getenv("GF_SND_PROBE"); s_sndp = (e && *e == '1') ? 1 : 0;
        if (s_sndp) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); printf("[sndprobe] v1113S qpc frequency %lld\n", (long long)f.QuadPart); } }
    if (!s_sndp) return;
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    printf("[sndprobe] %lld %s ent=%08X %.2f %.2f\n", (long long)q.QuadPart, kind, ent, a, b); fflush(stdout);
}
static double s_game_hit_t = -1e9;   /* v1110Z */
static void pain_hit_check(uint32_t sm)
{
    const uint32_t ent = rd32(sm + 0x4C); if (!ok(ent)) return;
    const v3 hit = V(rdf(sm + 0x6C), rdf(sm + 0x70), rdf(sm + 0x74)), sh = V(rdf(sm + 0x60), rdf(sm + 0x64), rdf(sm + 0x68));
    const float dmg = rdf(sm + 0x78);
    if (!fok(hit.x) || !fok(hit.y) || !fok(hit.z) || !fok(sh.x) || !fok(dmg) || !(dmg > 0.0f)) return;
    if (fabsf(hit.x) + fabsf(hit.y) + fabsf(hit.z) < 0.01f) return;
    m3 R; v3 T; if (!ent_xform(ent, &R, &T) || len(sub(hit, T)) > 3.0f) return;   /* not on this NPC: stale / pooled record */
    const double n = now_ms();
    hobs_t* o = 0; int oldest = 0;
    for (int i = 0; i < NHOBS; i++) { if (s_hobs[i].ent == ent) { o = &s_hobs[i]; break; } if (s_hobs[i].t < s_hobs[oldest].t) oldest = i; }
    if (o && o->sm == sm && len(sub(o->hit, hit)) < 0.003f && fabsf(o->dmg - dmg) < 0.5f) { o->t = n; return; }
    if (!o) o = &s_hobs[oldest];
    o->ent = ent; o->sm = sm; o->hit = hit; o->dmg = dmg; o->t = n;
    if (ent == player_ent()) return;
    if (len(sub(hit, sh)) >= 1.8f) s_game_hit_t = n;   /* v1110Z: a bullet the GAME registered (not melee) -> corpse_tick drops its own trace of that shot */
    { const float hp0 = rdf(ent + 0x14C8); snd_mark(len(sub(hit, sh)) >= 1.8f ? "npchit" : "npcmelee", ent, fok(hp0) ? hp0 : -1.0f, len(sub(hit, sh))); }   /* v1113S */
    { const uint32_t plp = player_ent(); if (plp) __try { name_probe_ent(ent, plp); } __except (EXCEPTION_EXECUTE_HANDLER) { } }   /* v1102N-2 */
    const float hp = rdf(ent + 0x14C8);
    if (fok(hp) && hp > 0.0f && len(sub(hit, sh)) < 1.8f && dmg < 600.0f) melee_blood(ent, hit, sh, dmg);   /* v1099B */
    if (kd_wanted(ent, hit, sh, dmg, hp) && kd_start(ent, hit, sh, dmg)) return;   /* v1097N */
    for (int i = 0; i < MAXR; i++) {
        rag_t* r = &s_rg[i];
        if (r->ent != ent || !r->state || r->state == 5) continue;
        if (!r->hr) { if (r->jh && (r->state == 3 || r->state == 4)) rag_shot(r, hit, sh, dmg); return; }
        if (fok(hp) && hp > 0.0f) { r->hr_hit_w = hit; r->hr_sh_w = sh; r->hr_dmg = dmg; r->hr_pending = 1; }
        return;
    }
    if (!s_hit_on || !(fok(hp) && hp > 0.0f)) return;   /* a killing shot: death_init (act 29) follows */
    hr_new(ent, hit, sh, dmg);
}

/* ------------------------------------------------------------------ v1091C corpses stay physical
 * User: "make dead ragdolls be physical and still react to being driven into and attacked, and spurt more blood when shot
 * even killed". Findings: the game sends NO damage to dead NPCs (0 "[rag-hit] dying" lines in every session), settled bodies
 * beyond the 6 newest were frozen (Jolt world freed), and the car only took part at the moment of death. Now:
 *  - shots: the player's shots are counted from the guns' clips (inventory player+0x1A7C, item +0x1EC; a press of fire with a
 *    gun out and no clip change = 1 shot, for infinite ammo) and traced: aiming = the free-aim camera ray (gf_combat.c
 *    gf_aim_ray), hip fire = the corpse nearest the player's facing (10 deg, 25 m). The ray is tested against the 11 body
 *    parts (capsules) of every dead body, walls in front block it. A hit wakes the body (a frozen one gets a new Jolt world
 *    from its current pose) and pushes it like a bullet (rag_shot) + blood.
 *  - the player's car: within 4.5 m and faster than 1.5 m/s, every dead body near it is woken and gets the car as a moving solid
 *    in its world (the v1069C machinery) -> driving into / over corpses shoves them.
 *  - melee: fire (LMB / R1) or Square (F) with no gun out, a dead body within 1.4 m in front -> a kick 0.22 s later.
 * GF_CORPSE=0 off, GF_CORPSE_DMG (500) bullet damage used for corpse hits. Log [corpse]. */
static double s_game_fps = 0.0;   /* v1091E: player skeleton updates per second (~ game fps) */
static int s_corpse = -1; static float s_corpse_dmg = 500.0f, s_corpse_push = 4.0f;   /* v1091D GF_CORPSE_PUSH: m/s a bullet gives the hit part */
#define CLOG(...) do { static int n_ = 0; if (n_ < 300) { n_++; printf(__VA_ARGS__); fflush(stdout); } } while (0)
extern int gf_aim_ray(float* o, float* d);
extern volatile int g_gf_combat_btn;
extern int (*volatile g_pad_drawn_fn)(void);
static int corpse_ok(const rag_t* r) { return !r->hr && !r->kd && (r->state == 3 || r->state == 4) && (r->jh || r->frozen); }
static int rag_wake(rag_t* r, const char* why)
{
    r->cad_n = 0; r->cad_max = 0;
    if (r->jh) { if (r->state == 4) r->state = 3; r->still_ms = 0; r->t_wake = now_ms(); return 1; }
    if (!r->frozen || !s_jolt) return 0;
    r->jh = jr_create(&r->p[0].x, &r->p[0].x, 1.0f / 60.0f, r->ground, (r->knsgn[0] + r->knsgn[1]) >= 0 ? 1.0f : -1.0f, r->elsgn[0]);
    if (!r->jh) return 0;
    { static float* buf = NULL; const int maxn = 60000;
      if (!buf) buf = (float*)malloc((size_t)maxn * 36);
      if (buf) { const m3 Md0 = w2mc(&r->Rd, r->conv), MdT0 = tr(&Md0); const v3 cw = add(r->Td, mv(&MdT0, r->p[PEL]));
          int nfl = 0; const int nt = col_gather(r, cw, 10.0f, buf, maxn, &nfl); if (nt) jr_add_mesh(r->jh, buf, nt, nfl ? 0.6f : 0.0f); } }
    r->jasleep = jr_get(r->jh, r->jq, r->jx, &r->p[0].x);
    r->frozen = 0; r->state = 3; r->still_ms = 0; r->t_wake = now_ms(); r->car_on = 0;
    r->selfc = 0; selfcoll_on(r);   /* v1091F: the new world gets the shoulder / neck pairs too */
    CLOG("[corpse] ent=%08X woken (%s): new Jolt world from its pose\n", r->ent, why);
    return 1;
}
/* closest approach of the ray o + t d (t >= 0, |d| = 1) to the segment a-b: distance, t */
static float ray_seg(v3 o, v3 d, v3 a, v3 b, float* tout)
{
    const v3 u = sub(b, a), w0 = sub(o, a); const float A = dot(u, u), B = dot(u, d), D = dot(u, w0), E = dot(d, w0);
    const float den = A - B * B; float s = 0.0f, t;
    if (A < 1e-8f) { t = -E; }
    else { s = den > 1e-8f ? (D - B * E) / den : 0.0f; s = s < 0 ? 0 : s > 1 ? 1 : s; t = dot(sub(add(a, mul(u, s)), o), d); }
    if (t < 0) t = 0;
    const v3 pr = add(o, mul(d, t)); float s2 = A > 1e-8f ? dot(sub(pr, a), u) / A : 0.0f; s2 = s2 < 0 ? 0 : s2 > 1 ? 1 : s2;
    *tout = t; return len(sub(pr, add(a, mul(u, s2))));
}
/* first dead body along a world ray within maxd m: body index, world hit point.
 * v1113H (user: "some of them just go directly through and it doesn't make the ragdoll move or make blood spawn"):
 *  - margin around the part capsules 0.04 -> 0.07 m (the drawn suit is bulkier than k_hrad: arms 5-6 cm, legs 6-8 cm);
 *  - near_ok = 1 (aimed shots): no capsule on the ray -> the body part passing within 0.20 m of it still takes the bullet (the
 *    camera ray is not the game's bullet: muzzle offset + spread), hit point = the closest point on the ray;
 *  - "wall in front" only when the level hit is > 0.5 m before the body (was 0.15: t is the closest approach to the part's AXIS,
 *    so the kerb / pavement the body lies on, hit just before it, blocked shots at bodies on the ground);
 *  - why = the reason when it returns -1 (for the [corpse] miss log). */
static int corpse_ray_ex(v3 o, v3 d, float maxd, v3* hit, int near_ok, const char** why, float* miss_d)
{
    int best = -1; float bt = maxd; int nb = -1; float nd = 1e9f, nt = 0.0f;
    for (int i = 0; i < MAXR; i++) {
        const rag_t* r = &s_rg[i]; if (!corpse_ok(r)) continue;
        const m3 Md = w2mc(&r->Rd, r->conv); const v3 om = mv(&Md, sub(o, r->Td)), dm = mv(&Md, d);
        for (int k = 0; k < ND; k++) {
            float t; const float dist = ray_seg(om, dm, r->p[k_dj[k]], r->p[k_dtip[k]], &t);
            if (dist < k_hrad[k] + 0.07f && t < bt) { bt = t; best = i; }
            if (t > 0.3f && t < maxd && dist - k_hrad[k] < nd) { nd = dist - k_hrad[k]; nb = i; nt = t; }
        }
    }
    if (miss_d) *miss_d = nd;
    if (best < 0 && near_ok && nb >= 0 && nd < 0.20f) { best = nb; bt = nt; }
    if (best < 0) { if (why) *why = "no body on the ray"; return -1; }
    { float tw; v3 nw; col_load(); if (col_ray(o, d, bt, &tw, &nw) && tw < bt - 0.5f) { if (why) *why = "a wall in front"; if (miss_d) *miss_d = bt - tw; return -1; } }
    *hit = add(o, mul(d, bt));
    return best;
}
static int corpse_ray(v3 o, v3 d, float maxd, v3* hit) { return corpse_ray_ex(o, d, maxd, hit, 0, NULL, NULL); }
static uint32_t gun_item(uint32_t pl, int slot)
{
    const uint32_t w = rd32(pl + 0x1A7Cu + 0x10u + (uint32_t)slot * 0x14u + 4u);
    if (!ok(w) || w < 0x38u) return 0; const uint32_t it = w - 0x38u;
    return (ok(it) && vm_base[it + 0xD4] == 1) ? it : 0;
}
static void corpse_fire(uint32_t pl, const char* how)
{
    m3 R; v3 T; if (!ent_xform(pl, &R, &T)) return;
    float of[3], df[3]; v3 o, d; int aimed = 0;
    if ((g_gf_combat_btn & 0x04) && gf_aim_ray(of, df)) { o = V(of[0], of[1], of[2]); d = V(df[0], df[1], df[2]); aimed = 1; }
    else if (g_gf_combat_btn & 0x04) return;   /* locked on (a living target): not at a corpse */
    else {   /* hip fire: the dead body nearest the facing */
        const m3 Mc = w2mc(&R, s_conv); v3 fw = V(Mc.m[6], 0, Mc.m[8]); if (len(fw) < 0.1f) return; fw = nrm(fw);
        o = add(T, V(0, 1.3f, 0)); int bi = -1; float bd = 25.0f; v3 tgt = o;
        for (int i = 0; i < MAXR; i++) { const rag_t* r = &s_rg[i]; if (!corpse_ok(r)) continue;
            const m3 Md = w2mc(&r->Rd, r->conv), MdT = tr(&Md); const v3 c = add(r->Td, mv(&MdT, r->p[THO]));
            v3 h = sub(c, T); h.y = 0; const float dh = len(h); if (dh < 0.3f || dh > bd) continue;
            if (dot(mul(h, 1.0f / dh), fw) < 0.990f) continue;   /* v1110Z: 8 deg (was 10) */
            bd = dh; bi = i; tgt = c; }
        if (bi < 0) { static int nlh = 0; if (nlh < 200) { nlh++; CLOG("[corpse] v1113H miss (%s, hip): no dead body within 8 deg of the facing\n", how); } return; }
        d = nrm(sub(tgt, o));
        {   /* v1110Z (user: "sometimes another dead NPC reacts to the shots"): a LIVING NPC in the same cone, nearer than the body,
             * took the bullet (the game aims hip fire at the living) -> not at the corpse */
            extern int gf_npcs_within(float maxd, uint32_t* out, int max); uint32_t L[48]; const int nn = gf_npcs_within(bd + 0.5f, L, 48);
            for (int i = 0; i < nn; i++) { m3 Rn; v3 Tn; if (!ent_xform(L[i], &Rn, &Tn)) continue;
                const float hpn = rdf(L[i] + 0x14C8); if (!(fok(hpn) && hpn > 0.0f)) continue;
                {   /* v1112S (user: "ragdolls need to react to ALL shots even when dead"; log: every hip shot refused because the
                     * "living NPC" in front WAS the corpse itself -- a ragdolled body can keep health > 0): any entity with a
                     * ragdoll record is a body, not a living shield */
                    int body = 0; for (int k = 0; k < MAXR; k++) if (s_rg[k].state && s_rg[k].ent == L[i]) { body = 1; break; }
                    if (body) continue; }
                v3 hn = sub(Tn, T); hn.y = 0; const float dn = len(hn); if (dn < 0.3f || dn > bd) continue;
                if (dot(mul(hn, 1.0f / dn), fw) >= 0.985f) { CLOG("[corpse] v1110Z hip shot: living NPC %08X at %.1f m is in front of the body (%.1f m) -> not the corpse\n", L[i], dn, bd); return; } }
        }
    }
    v3 hit; const char* why = "?"; float md = 0.0f; const int bi = corpse_ray_ex(o, d, 60.0f, &hit, aimed, &why, &md);
    if (bi < 0) { static int nlm = 0; if (nlm < 300) { nlm++; CLOG("[corpse] v1113H miss (%s, %s): %s (%.2f m)\n", how, aimed ? "aimed" : "hip", why, md); } return; }
    rag_t* r = &s_rg[bi];
    if (!rag_wake(r, "shot")) return;
    /* v1110Z (user: "if I keep shooting it, they eventually go through objects and into buildings"): a burst on one body gives
     * smaller and smaller shoves (1 / (1 + 0.35 (n - 1)), >= 0.35 x, n = hits within 1.5 s), and once the body is > 6 m from where
     * its level collision was gathered (10 m around the death / wake), the collision around its new place is added. */
    { const double tn = now_ms(); r->cf_n = (tn - r->cf_t < 1500.0) ? r->cf_n + 1 : 1; r->cf_t = tn; }
    float sc = 1.0f / (1.0f + 0.35f * (float)(r->cf_n - 1)); if (sc < 0.35f) sc = 0.35f;
    if (r->jh) { const m3 Mdc = w2mc(&r->Rd, r->conv), MdcT = tr(&Mdc); const v3 pw = add(r->Td, mv(&MdcT, r->p[PEL]));
        if (!r->col_have) { r->col_c = pw; r->col_have = 1; }
        else if (len(sub(pw, r->col_c)) > 6.0f) { static float* buf2 = NULL; const int maxn = 60000; if (!buf2) buf2 = (float*)malloc((size_t)maxn * 36);
            if (buf2) { int nfl = 0; const int nt = col_gather(r, pw, 10.0f, buf2, maxn, &nfl); if (nt) jr_add_mesh(r->jh, buf2, nt, 0.0f);
                CLOG("[corpse] v1110Z ent=%08X moved %.1f m from its collision -> %d more triangles around it\n", r->ent, len(sub(pw, r->col_c)), nt); }
            r->col_c = pw; } }
    snd_mark("corpse", r->ent, len(sub(hit, o)), 0.0f);   /* v1113S */
    { extern void gf_sfx_body_hit(float dist); gf_sfx_body_hit(len(sub(hit, o))); }   /* v1113B: the game's bullet-into-body sound (gf_dialogue.c) */
    s_shot_boost = s_corpse_push * sc; rag_shot(r, hit, o, s_corpse_dmg); s_shot_boost = 0.0f;
    CLOG("[corpse] ent=%08X shot (%s, %s) at (%.2f %.2f %.2f)\n", r->ent, how, aimed ? "aimed" : "hip", hit.x, hit.y, hit.z);
}
static void corpse_melee(uint32_t pl)
{
    m3 R; v3 T; if (!ent_xform(pl, &R, &T)) return;
    const m3 Mc = w2mc(&R, s_conv); v3 fw = V(Mc.m[6], 0, Mc.m[8]); if (len(fw) < 0.1f) return; fw = nrm(fw);
    int bi = -1, bk = 0; float bd = 1.4f; v3 bp = T;
    for (int i = 0; i < MAXR; i++) { const rag_t* r = &s_rg[i]; if (!corpse_ok(r)) continue;
        const m3 Md = w2mc(&r->Rd, r->conv), MdT = tr(&Md);
        for (int k = 0; k < ND; k++) { const v3 c = add(r->Td, mv(&MdT, mul(add(r->p[k_dj[k]], r->p[k_dtip[k]]), 0.5f)));
            v3 h = sub(c, T); h.y = 0; const float dh = len(h); if (dh > bd || (dh > 0.25f && dot(mul(h, 1.0f / dh), fw) < 0.3f)) continue;
            bd = dh; bi = i; bk = k; bp = c; } }
    if (bi < 0) return;
    rag_t* r = &s_rg[bi]; if (!rag_wake(r, "kicked")) return;
    const m3 Md = w2mc(&r->Rd, r->conv);
    const v3 dw = nrm(add(fw, V(0, 0.35f, 0))), dm = mv(&Md, dw), pm = mv(&Md, sub(bp, r->Td));
    const float kv = 1.4f * s_corpse_push;   /* v1091D: 5.6 m/s (was 3.2) + torso share */
    const v3 J = mul(dm, k_pmass[bk] * kv);
    jr_impulse(r->jh, bk, &pm.x, &J.x);
    if (bk != D_LT) { const v3 Jt = mul(dm, k_pmass[D_LT] * kv * (s_corpse_torso + 0.05f)); jr_impulse(r->jh, D_LT, &r->p[PEL].x, &Jt.x); }
    if (bk != D_UT) { const v3 Jt = mul(dm, k_pmass[D_UT] * kv * (s_corpse_torso + 0.05f)); jr_impulse(r->jh, D_UT, &r->p[THO].x, &Jt.x); }
    CLOG("[corpse] ent=%08X kicked on the %s\n", r->ent, k_pn[bk]);
}
/* v1108W (user: "cars don't collide with the dead bodies"): every moving car near a body is a solid in that body's physics world
 * -- traffic too (gf_coop_worldre.c gf_world_cars, an always-on list from the car factory), not only the player's car. Per body
 * the nearest car moving > GF_RAG_CAR_MINV (1.0 m/s) within 4.5 m (|dy| < 2.5 m); one car per body (the Jolt world holds one). */
extern int gf_world_cars(uint32_t* out, int max);
static int car_vel(uint32_t car, v3* vel)
{
    const uint32_t b = rd32(car + 0x138u); if (!ok(b)) return 0;
    *vel = V(rdf(b + 0x180u), rdf(b + 0x184u), rdf(b + 0x188u));
    return fok(vel->x) && fok(vel->y) && fok(vel->z) && len(*vel) < 80.0f;
}
/* v1108X (user 22:01: "I want the car & the body to react, like it's really an obstacle on the road"): the Jolt car is kinematic
 * (it pushes the body, nothing pushes back). Here the CAR reacts, in the game's Havok (body = car+0x138, linear velocity +0x180,
 * angular +0x190; plain stores on an active body, gf_coop_vehicle.c notes):
 *   - wheel bump: each wheel (local +-0.78, ground, +-1.42) that rolls onto a dead body (gf_rag_body_top above the road by
 *     > 4 cm) kicks the car up (rise x 5, 0.15..1.2 m/s, x speed factor) and tilts it about that wheel (rise x 6, <= 1.5 rad/s),
 *     and costs GF_RAG_CAR_BUMPLOSS (2 %) speed; leaving the body re-arms that wheel;
 *   - bumper hit: the first time a body sits in front of the car's nose (< 1 m ahead, inside the width) at > 2 m/s, the car
 *     loses m_body / (m_car + m_body) of its speed (80 / 1480 kg = 5.4 %; GF_RAG_CAR_MASS).
 * GF_RAG_CAR_REACT=0 = off. Log "[car-react]". */
int gf_rag_body_top(float x, float z, float ylo, float yhi, float* ytop);
static float cr_clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
#define clampf cr_clampf
static float s_car_drag = 1.5f;
/* v1108Z (user screenshot 22:24: a body "stuck in air" above a car): the car box height came from gf_collision.bin under the
 * car (ground_at) -- where that is wrong / missing, car_oy fell back to 0 and the box floated up to ~0.5 m above the real car, a
 * body landed on it in mid-air. Now the car's ground = the FLOOR this ragdoll measured (Td.y + ground; same street), the
 * collision file only if that is implausible. */
static float car_oy_for(const rag_t* r, v3 Tc)
{
    const float gb = r->Td.y + r->ground;
    if (fok(gb) && Tc.y - gb < 1.5f && Tc.y - gb > -0.5f) return gb - Tc.y;
    col_load(); const float gy = ground_at(Tc);
    return fok(gy) && Tc.y - gy < 1.5f && Tc.y - gy > -0.5f ? gy - Tc.y : 0.0f;
}
static void car_react(void)
{
    static int on = -1; static float car_m = 1400.0f, bump_loss = 0.02f;
    if (on < 0) { const char* e = getenv("GF_RAG_CAR_REACT"); on = !(e && *e == '0');
        e = getenv("GF_RAG_CAR_MASS"); if (e && *e) car_m = (float)atof(e); if (car_m < 200.0f) car_m = 200.0f;
        e = getenv("GF_RAG_CAR_BUMPLOSS"); if (e && *e) bump_loss = (float)atof(e);
        e = getenv("GF_RAG_CAR_DRAG"); if (e && *e) s_car_drag = (float)atof(e); }
    if (!on) return;
    static struct { uint32_t car; uint8_t in[4]; uint32_t hit_ent[8]; int nh; double t; v3 wp[4]; int have_wp; double t_rum[4]; int rum_sign; } st[16];   /* v1108Y: wheel path + rumble */
    const double n = now_ms();
    for (int i = 0; i < MAXR; i++) {
        rag_t* r = &s_rg[i]; if (!corpse_ok(r) || !r->car_on || !r->car) continue;
        const uint32_t car = r->car; int si = -1, fr = -1;
        for (int k = 0; k < 16; k++) { if (st[k].car == car) { si = k; break; } if (fr < 0 && (!st[k].car || n - st[k].t > 3000.0)) fr = k; }
        if (si < 0) { if (fr < 0) continue; si = fr; memset(&st[si], 0, sizeof st[si]); st[si].car = car; }
        if (st[si].t == n) continue;   /* this car was handled this frame (several bodies) */
        st[si].t = n;
        v3 vel; if (!car_vel(car, &vel)) continue;
        const float sp = len(V(vel.x, 0, vel.z)); if (sp < 1.0f) continue;
        m3 Rc; v3 Tc; if (!ent_xform(car, &Rc, &Tc)) continue;
        const m3 Mc = w2mc(&Rc, s_conv), McT = tr(&Mc);
        const uint32_t b = rd32(car + 0x138u); if (!ok(b)) continue;
        v3 lin = vel, ang = V(rdf(b + 0x190u), rdf(b + 0x194u), rdf(b + 0x198u)); if (!fok(ang.x) || !fok(ang.y) || !fok(ang.z)) continue;
        int changed = 0; const float sf = clampf(sp / 10.0f, 0.3f, 1.0f);
        /* wheels -- v1108Y (user: "the cars don't react to driving over the bodies, I want it to be bumpy"; his log: 4 wheel
         * bumps vs 12 bumper hits, 0.15-0.5 m/s): the whole path each wheel travelled since the last frame is tested (12 m/s =
         * 20 cm per frame skipped thin limbs), a stronger first kick (rise x 9, 0.3..2.0 m/s; tilt rise x 12, <= 2.5 rad/s) and a
         * RUMBLE every 70 ms while the wheel stays on the body (0.35 m/s up, alternating roll); the body under the wheel is
         * pressed down and dragged along (GF_RAG_CAR_DRAG 1.5 m/s on the pelvis). */
        for (int w = 0; w < 4; w++) {
            const v3 loc = V(w & 1 ? 0.78f : -0.78f, r->car_oy, w & 2 ? 1.42f : -1.42f), P = add(Tc, mv(&McT, loc));
            const v3 P0 = st[si].have_wp ? st[si].wp[w] : P; st[si].wp[w] = P;
            float best = -1.0f;
            for (int k = 0; k <= 4; k++) { const v3 Pk = add(P0, mul(sub(P, P0), k / 4.0f)); float top;
                if (gf_rag_body_top(Pk.x, Pk.z, Pk.y - 0.10f, Pk.y + 0.45f, &top) && top - Pk.y > best) best = top - Pk.y; }
            const int on_body = best > 0.04f;
            v3 d = sub(P, Tc); d.y = 0; const float dl = len(d); const v3 ax = dl > 0.1f ? nrm(cross(d, V(0, 1, 0))) : V(0, 0, 0);
            if (on_body && !st[si].in[w]) {
                st[si].in[w] = 1; st[si].t_rum[w] = n;
                const float rise = fminf(best, 0.35f);
                const float dvy = clampf(rise * 9.0f * sf, 0.3f, 2.0f), om = clampf(rise * 12.0f * sf, 0.2f, 2.5f);
                ang = add(ang, mul(ax, om));   /* lift this corner */
                lin.y += dvy; lin.x *= 1.0f - bump_loss; lin.z *= 1.0f - bump_loss; changed = 1;
                if (r->jh) {   /* the body under the wheel: pressed down + dragged along */
                    const m3 Md = w2mc(&r->Rd, r->conv); const v3 dw = nrm(add(mul(V(vel.x, 0, vel.z), 1.0f / sp), V(0, -0.6f, 0)));
                    const v3 dm = mv(&Md, dw); const v3 J = mul(dm, k_pmass[D_LT] * s_car_drag * sf); jr_impulse(r->jh, D_LT, &r->p[PEL].x, &J.x); }
                CLOG("[car-react] car %08X wheel %d rolled onto a body (+%.2f m) at %.1f m/s -> up %.2f m/s, tilt %.2f rad/s\n", car, w, rise, sp, dvy, om);
            } else if (on_body && n - st[si].t_rum[w] > 70.0) {   /* rumble while on it */
                st[si].t_rum[w] = n; st[si].rum_sign = -st[si].rum_sign; if (!st[si].rum_sign) st[si].rum_sign = 1;
                lin.y += 0.35f * sf; ang = add(ang, mul(ax, 0.5f * sf * (float)st[si].rum_sign)); changed = 1;
            } else if (!on_body && st[si].in[w]) st[si].in[w] = 0;
        }
        st[si].have_wp = 1;
        /* bumper: every body attached to this car, once */
        for (int j = 0; j < MAXR; j++) {
            rag_t* q = &s_rg[j]; if (!corpse_ok(q) || !q->car_on || q->car != car) continue;
            int seen = 0; for (int h = 0; h < st[si].nh; h++) if (st[si].hit_ent[h] == q->ent) seen = 1; if (seen) continue;
            const m3 Md = w2mc(&q->Rd, q->conv), MdT = tr(&Md); const v3 pc = add(q->Td, mv(&MdT, q->p[PEL]));
            const v3 pl = mv(&Mc, sub(pc, Tc)), vl = mv(&Mc, vel);
            const float fz = vl.z >= 0 ? pl.z : -pl.z;   /* along the travel direction */
            if (fabsf(pl.x) < 1.05f && fz > 1.9f && fz < 3.2f && sp > 2.0f) {
                const float k = 1.0f - 80.0f / (car_m + 80.0f);
                lin.x *= k; lin.z *= k; lin.y += 0.35f * sf; changed = 1;
                { v3 fwd = V(vel.x, 0, vel.z); fwd = mul(fwd, 1.0f / sp); ang = add(ang, mul(nrm(cross(fwd, V(0, 1, 0))), 0.6f * sf)); }   /* v1108Y: nose jolts up */
                if (st[si].nh < 8) st[si].hit_ent[st[si].nh++] = q->ent;
                CLOG("[car-react] car %08X hit body %08X at %.1f m/s -> -%.1f %% speed\n", car, q->ent, sp, (1.0f - k) * 100.0f);
            }
        }
        if (changed) { wrf(b + 0x180u, lin.x); wrf(b + 0x184u, lin.y); wrf(b + 0x188u, lin.z); wrf(b + 0x190u, ang.x); wrf(b + 0x194u, ang.y); wrf(b + 0x198u, ang.z); }
    }
}
#undef clampf
static void corpse_car(void)
{
    /* v1108Z: a body that settled (or was frozen) while a car was in its world may be lying ON that car's box: when the car moves
     * > 0.3 m or is gone, wake it so it falls (was: frozen in mid-air once the car drove off). */
    for (int i = 0; i < MAXR; i++) {
        rag_t* r = &s_rg[i]; if (!corpse_ok(r)) continue;
        if (r->state == 4 || r->frozen) {
            const uint32_t car = r->car_on ? r->car : r->rest_car; if (!car) continue;
            m3 Rq; v3 Tq;
            if (!r->rest_have) { if (ok(car) && ent_xform(car, &Rq, &Tq)) { r->rest_car = car; r->rest_vt = rd32(car); r->rest_T = Tq; r->rest_have = 1; } }
            else if (!ok(car) || rd32(car) != r->rest_vt || !ent_xform(car, &Rq, &Tq) || len(sub(Tq, r->rest_T)) > 0.3f) {
                r->rest_have = 0; r->rest_car = 0;
                if (rag_wake(r, "car left")) CLOG("[corpse] ent=%08X: the car it lay on moved / is gone -> falls\n", r->ent);
            }
        } else if (r->state == 3 && !r->car_on) { r->rest_have = 0; r->rest_car = 0; }
    }
    static float minv = -1.0f; if (minv < 0.0f) { const char* e = getenv("GF_RAG_CAR_MINV"); minv = e && *e ? (float)atof(e) : 1.0f; }
    uint32_t cars[136]; int nc = 0;
    { v3 pv; const uint32_t pc = player_car(&pv); if (pc) cars[nc++] = pc; }
    __try { nc += gf_world_cars(cars + nc, 128); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    if (!nc) return;
    struct { uint32_t car; v3 T, vel; } cv[136]; int nv = 0;
    for (int c = 0; c < nc; c++) {
        int dup = 0; for (int q = 0; q < nv; q++) if (cv[q].car == cars[c]) dup = 1; if (dup) continue;
        v3 vel; if (!car_vel(cars[c], &vel)) continue;
        if (len(V(vel.x, 0, vel.z)) < minv) continue;
        m3 Rc; v3 Tc; if (!ent_xform(cars[c], &Rc, &Tc) || !fok(Tc.x)) continue;
        cv[nv].car = cars[c]; cv[nv].T = Tc; cv[nv].vel = vel; nv++;
    }
    if (!nv) return;
    const double n = now_ms();
    for (int i = 0; i < MAXR; i++) {
        rag_t* r = &s_rg[i]; if (!corpse_ok(r)) continue;
        const m3 Md = w2mc(&r->Rd, r->conv), MdT = tr(&Md); const v3 c = add(r->Td, mv(&MdT, r->p[PEL]));
        int best = -1; float bd = 4.5f;
        for (int q = 0; q < nv; q++) { v3 h = sub(c, cv[q].T); h.y = 0; const float d = len(h);
            if (d < bd && fabsf(c.y - cv[q].T.y) < 2.5f) { bd = d; best = q; } }
        if (best < 0) continue;
        const uint32_t car = cv[best].car; const v3 vel = cv[best].vel, Tc = cv[best].T;
        if (r->car_on && r->car == car) { if (r->state == 4) { r->state = 3; } r->still_ms = 0; r->t_wake = n; r->car_t0 = n; r->car_vw = vel; continue; }
        if (!rag_wake(r, "car")) continue;
        if (r->car_on) { jr_car_remove(r->jh); r->car_on = 0; }
        float rot9[9]; v3 cp; r->car = car; r->car_vw = vel;
        if (!car_frame(r, car, rot9, &cp)) continue;
        r->car_oy = car_oy_for(r, Tc);   /* v1108Z */
        jr_car_add(r->jh, rot9, &cp.x, r->car_oy); r->car_on = 1; r->car_t0 = n;
        CLOG("[corpse] ent=%08X: a car %08X (%.1f m/s, %.1f m away%s) is in its world\n", r->ent, car, len(V(vel.x, 0, vel.z)), bd, car == cars[0] && player_car(&cp) ? ", the player's" : "");
    }
    car_react();   /* v1108X */
}
/* once per frame, from the player's skeleton update */
/* v1100L contact kicks (user: "kicking dead bodies should move them a bit, just like shooting"): the button guess in corpse_melee
 * never fired in the user's session (0 kicks). Now the player's feet and hands are tracked every frame (toe / ankle / finger
 * slots, world space); a limb moving > 1.4 m/s (relative to the ground) that enters a dead body part's capsule (+3 cm) pushes
 * that part along the limb's motion: part mass x min(0.9 x speed, 7 m/s), +25 % up, a share on hips / chest; 300 ms per limb and
 * body. Walking into a body = small shoves, a kick = a real one. GF_CORPSE_TOUCH=0 off. Log "[corpse] ... touched by". */
int gf_rag_bone_slots(uint32_t ent, uint32_t anim, int* slot21, int* nslot);
static void corpse_touch(uint32_t pl)
{
    static int on = -1; if (on < 0) { const char* e = getenv("GF_CORPSE_TOUCH"); on = (e && *e == '0') ? 0 : 1; }
    if (!on) return;
    static uint32_t c_pl, c_anim, c_pal; static int c_slot[NBN], c_ok; static v3 prev[8], prevT; static double t_prev; static int have_prev;
    static double cool[MAXR][8];
    /* v1108W (user 21:52: "the physicality on the ragdolls is kinda ass... he gets pushed waaaay too much from my side", contact
     * unreliable): the push gave the touched part the limb's FULL speed (part mass x min(0.9 v, 7 m/s)) + hip / chest shares ->
     * walking feet (3-4 m/s) launched bodies. Now a collision of the limb (effective mass GF_CORPSE_TOUCH_M, 4 kg; hands 1.5)
     * with the part: J = m_eff m_p / (m_eff + m_p) x v x 1.2, the part's speed change capped at GF_CORPSE_TOUCH_DV (2 m/s),
     * mostly horizontal, 5 % to the torso. Contact = the limb's SWEPT path since the last frame vs the capsule (a fast foot
     * no longer skips through an arm), from 0.6 m/s (was 1.4), knees added (walking into a body), 200 ms per limb. */
    static float t_meff = -1.0f, t_dv = 2.0f;
    if (t_meff < 0.0f) { const char* e = getenv("GF_CORPSE_TOUCH_M"); t_meff = e && *e ? (float)atof(e) : 4.0f; if (t_meff < 0.2f) t_meff = 0.2f;
        e = getenv("GF_CORPSE_TOUCH_DV"); if (e && *e) t_dv = (float)atof(e); if (t_dv < 0.1f) t_dv = 0.1f; }
    const uint32_t anim = rd32(pl + 0xC4); if (!ok(anim)) return;
    const uint32_t pal = anim_pal(anim); if (!ok(pal)) return;
    if (pl != c_pl || anim != c_anim || pal != c_pal) {
        int ns = 0; c_ok = gf_rag_bone_slots(pl, anim, c_slot, &ns); c_pl = pl; c_anim = anim; c_pal = pal; have_prev = 0;
        if (!c_ok) return; }
    if (!c_ok) return;
    m3 R; v3 T; if (!ent_xform(pl, &R, &T)) return;
    const m3 Mc = w2mc(&R, s_conv), McT = tr(&Mc);
    static const int lb[8] = { TOL, TOR, ANL, ANR, FIL, FIR, KNL, KNR };
    static const char* const ln[8] = { "left foot", "right foot", "left ankle", "right ankle", "left hand", "right hand", "left knee", "right knee" };
    v3 w[8]; for (int i = 0; i < 8; i++) { const int sl = c_slot[lb[i]]; if (sl < 0) return; w[i] = add(T, mv(&McT, slot_pos(pal, sl))); if (!fok(w[i].x)) return; }
    const double n = now_ms(); const float dt = have_prev ? (float)((n - t_prev) / 1000.0) : 0.0f;
    if (!have_prev || !(dt > 0.002f) || dt > 0.1f) { memcpy(prev, w, sizeof prev); prevT = T; t_prev = n; have_prev = 1; return; }
    const v3 vroot = mul(sub(T, prevT), 1.0f / dt);   /* v1100L-3: a limb's own swing counts fully, the player's running only 35 % */
    for (int li = 0; li < 8; li++) {
        const v3 v = sub(mul(sub(w[li], prev[li]), 1.0f / dt), mul(vroot, 0.65f)); const float sp = len(v);
        if (sp < 0.6f || sp > 30.0f) continue;
        for (int i = 0; i < MAXR; i++) { rag_t* r = &s_rg[i]; if (!corpse_ok(r) || n < cool[i][li]) continue;
            const m3 Md = w2mc(&r->Rd, r->conv), MdT = tr(&Md);
            if (len(sub(add(r->Td, mv(&MdT, r->p[PEL])), w[li])) > 1.6f) continue;
            for (int k = 0; k < ND; k++) {
                const v3 a = add(r->Td, mv(&MdT, r->p[k_dj[k]])), b = add(r->Td, mv(&MdT, r->p[k_dtip[k]]));
                const float lim = k_hrad[k] + (li >= 6 ? 0.07f : 0.04f);   /* knees: the shin's thickness */
                if (segseg(prev[li], w[li], a, b) > lim) continue;   /* v1108W: swept since the last frame */
                v3 cp = a; { float bd = 1e9f; for (int q = 0; q <= 6; q++) { const v3 pq = add(prev[li], mul(sub(w[li], prev[li]), q / 6.0f)); v3 c2; const float d = segcp(pq, a, b, &c2); if (d < bd) { bd = d; cp = c2; } } }
                if (!rag_wake(r, "touched")) break;
                const v3 vh = V(v.x, v.y * 0.3f, v.z); const float sh = len(vh); if (sh < 1e-3f) break;
                const v3 dw = nrm(add(mul(vh, 1.0f / sh), V(0, 0.10f, 0))); const v3 dm = mv(&Md, dw), pm = mv(&Md, sub(cp, r->Td));
                const float me = (li == 4 || li == 5) ? t_meff * 0.375f : t_meff, mp = k_pmass[k];
                float jm = me * mp / (me + mp) * sp * 1.2f; if (jm > mp * t_dv) jm = mp * t_dv;
                const v3 J = mul(dm, jm); jr_impulse(r->jh, k, &pm.x, &J.x);
                if (k != D_LT && k != D_UT) { const v3 Jt = mul(dm, jm * 0.05f); jr_impulse(r->jh, D_LT, &r->p[PEL].x, &Jt.x); }
                cool[i][li] = n + 200.0;
                CLOG("[corpse] ent=%08X %s touched by the player's %s at %.1f m/s -> %.2f m/s\n", r->ent, k_pn[k], ln[li], sp, jm / mp);
                break;
            }
        }
    }
    memcpy(prev, w, sizeof prev); prevT = T; t_prev = n;
}
/* v1100L feet on bodies: highest top of a dead body's part capsules under world (x, z), between ylo and yhi; 0 = none */
int gf_rag_body_top(float x, float z, float ylo, float yhi, float* ytop)
{
    if (!s_on) return 0;
    int got = 0; float best = ylo;
    EnterCriticalSection(&s_cs);
    __try {
        for (int i = 0; i < MAXR; i++) { const rag_t* r = &s_rg[i]; if (!(corpse_ok(r) || (r->kd && r->state >= 3 && r->state <= 4 && !r->kd_ph))) continue;
            const m3 Md = w2mc(&r->Rd, r->conv), MdT = tr(&Md);
            const v3 pc = add(r->Td, mv(&MdT, r->p[PEL])); if (fabsf(pc.x - x) > 1.4f || fabsf(pc.z - z) > 1.4f) continue;
            for (int k = 0; k < ND; k++) {
                const v3 a = add(r->Td, mv(&MdT, r->p[k_dj[k]])), b = add(r->Td, mv(&MdT, r->p[k_dtip[k]]));
                const v3 ab = V(b.x - a.x, 0, b.z - a.z); const float L = dot(ab, ab);
                float t = L > 1e-6f ? ((x - a.x) * ab.x + (z - a.z) * ab.z) / L : 0.0f; t = t < 0 ? 0 : t > 1 ? 1 : t;
                const v3 c = add(a, mul(sub(b, a), t)); const float dx = x - c.x, dz = z - c.z, rr = k_hrad[k] * 0.9f, h2 = rr * rr - dx * dx - dz * dz;
                if (h2 <= 0.0f) continue;
                const float top = c.y + sqrtf(h2);
                if (top > best && top <= yhi) { best = top; got = 1; }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { got = 0; }
    LeaveCriticalSection(&s_cs);
    if (got) *ytop = best;
    return got;
}

/* v1102N name probe (GF_NAME_PROBE=1): where does an NPC keep the name the lock-on HUD shows ("LUPE VESPUCCI")? On a new game
 * lock target (*(player+0x1544) - 0x38, within 25 m) the entity's first 0x2400 bytes are searched for inline text and for
 * pointers (and pointers to pointers, first 0x600 bytes) to ASCII / UTF-16BE text; every hit is logged "[name-probe]". */
static int np_ascii(uint32_t ea, char* out, int cap)
{
    if (!ok(ea)) return 0;
    int n = 0;
    for (; n < cap - 1; n++) { const uint8_t c = vm_base[ea + (uint32_t)n]; if (c == 0) break; if (c < 32 || c > 126) return 0; out[n] = (char)c; }
    out[n] = 0; return n >= 4 && n < cap - 1;
}
static int np_utf16(uint32_t ea, char* out, int cap)
{
    if (!ok(ea)) return 0;
    int n = 0;
    for (; n < cap - 1; n++) { const uint8_t hi = vm_base[ea + 2u * (uint32_t)n], lo = vm_base[ea + 2u * (uint32_t)n + 1u];
        if (!hi && !lo) break; if (hi || lo < 32 || lo > 126) return 0; out[n] = (char)lo; }
    out[n] = 0; return n >= 4 && n < cap - 1;
}
static void name_probe_ent(uint32_t t, uint32_t pl)   /* v1102N-2: also on every newly hit NPC (the bot never locks on) */
{
    static int on = -1; if (on < 0) { const char* e = getenv("GF_NAME_PROBE"); on = e && *e == '1'; }
    if (!on) return;
    static uint32_t seen[8]; static int nent;
    if (!ok(t) || t == pl || nent >= 8) return;
    for (int i = 0; i < nent; i++) if (seen[i] == t) return;
    m3 R; v3 T, Tp; if (!ent_xform(t, &R, &T) || !ent_xform(pl, &R, &Tp) || len(sub(T, Tp)) > 25.0f) return;
    seen[nent++] = t;
    printf("[name-probe] v1102N lock target %08X vtable %08X hp %.0f / %.0f, %.1f m\n", t, rd32(t), rdf(t + 0x14C8), rdf(t + 0x14CC), len(sub(T, Tp)));
    int hits = 0; char b[96];
    __try {
        for (uint32_t off = 0; off < 0x2400u && hits < 80; off += 4) {
            if (np_ascii(t + off, b, sizeof b)) { printf("[name-probe]   +%04X inline ascii '%s'\n", off, b); hits++; off += ((uint32_t)strlen(b) & ~3u); continue; }
            if (np_utf16(t + off, b, sizeof b)) { printf("[name-probe]   +%04X inline utf16 '%s'\n", off, b); hits++; off += ((uint32_t)strlen(b) * 2u & ~3u); continue; }
            const uint32_t v = rd32(t + off); if (!ok(v)) continue;
            if (np_ascii(v, b, sizeof b)) { printf("[name-probe]   +%04X -> %08X ascii '%s'\n", off, v, b); hits++; continue; }
            if (np_utf16(v, b, sizeof b)) { printf("[name-probe]   +%04X -> %08X utf16 '%s'\n", off, v, b); hits++; continue; }
            if (off < 0x600u) for (uint32_t o2 = 0; o2 < 0x80u; o2 += 4) { const uint32_t w = rd32(v + o2); if (!ok(w)) continue;
                if (np_ascii(w, b, sizeof b)) { printf("[name-probe]   +%04X -> %08X +%02X -> %08X ascii '%s'\n", off, v, o2, w, b); hits++; }
                else if (np_utf16(w, b, sizeof b)) { printf("[name-probe]   +%04X -> %08X +%02X -> %08X utf16 '%s'\n", off, v, o2, w, b); hits++; } }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { printf("[name-probe]   (fault, stopped)\n"); }
    printf("[name-probe]   %d hit(s)\n", hits); fflush(stdout);
}
static void name_probe(uint32_t pl)
{
    const uint32_t q = rd32(pl + 0x1544); if (!ok(q) || q < 0x38u) return;
    name_probe_ent(q - 0x38u, pl);
}

/* v1103A free-aim target HUD, PPU side: the target from gf_combat.c, its name from the game's own getter func_0012D5BC(npc) (RE: name
 * string object at npc+0x1824; story characters: text key npc+0x1830; civilians: generated on the first request from ethnicity
 * npc+0x153C + gender npc+0x1814 by func_00107BF8 / func_00107AEC: "lw_ital_firstname_m_N" + " " + "lw_ital_lastname_mf_N" ->
 * localized text). hp npc+0x14C8 / max +0x14CC. Published to the overlay (rsx_ui_overlay.c aimhud_build). GF_AIMHUD=0 off. */
volatile int g_aimhud_on; char g_aimhud_name[64]; volatile float g_aimhud_hp, g_aimhud_hpmax; volatile unsigned long long g_aimhud_t;
static int ah_text(uint32_t ea, char* out, int cap)
{
    if (!ok(ea)) return 0;
    int n = 0, wide = vm_base[ea] == 0 && vm_base[ea + 1] >= 32;
    for (; n < cap - 1; n++) {
        uint8_t c;
        if (wide) { if (vm_base[ea + 2u * (uint32_t)n]) return 0; c = vm_base[ea + 2u * (uint32_t)n + 1u]; }
        else c = vm_base[ea + (uint32_t)n];
        if (!c) break;
        if (c < 32 || c > 126) return 0;
        out[n] = (char)c;
    }
    out[n] = 0; return n >= 2 && n < cap - 1;
}
static void aimhud_tick(uint32_t pl)
{
    static int on = -1; if (on < 0) { const char* e = getenv("GF_AIMHUD"); on = !(e && *e == '0'); }
    if (!on) return;
    extern uint32_t gf_aim_target_now(void);
    const uint32_t t = gf_aim_target_now();
    if (!ok(t) || t == pl) return;
    const float hp = rdf(t + 0x14C8), hm = rdf(t + 0x14CC);
    if (!(fok(hp) && hp > 0.0f) || !(fok(hm) && hm > 0.0f)) return;
    static struct { uint32_t ent; char name[64]; } cache[16]; static int ci; static int nraw;
    int k = -1; for (int i = 0; i < 16; i++) if (cache[i].ent == t) { k = i; break; }
    if (k < 0) {
        k = ci; ci = (ci + 1) % 16; cache[k].ent = t; cache[k].name[0] = 0;
        uint64_t obj = 0; gfn f = lookup(0x0012D5BCu);
        if (f && gcallx(f, t, 0, 0, 0, 0.0, &obj) && ok((uint32_t)obj)) {
            const uint32_t o = (uint32_t)obj, w0 = rd32(o), w1 = rd32(o + 4), w2 = rd32(o + 8);
            const uint32_t cand[6] = { w0, w0 + 4u, w0 + 8u, w0 + 12u, w0 + 16u, o };
            for (int c = 0; c < 6 && !cache[k].name[0]; c++) if (ok(cand[c])) ah_text(cand[c], cache[k].name, sizeof cache[k].name);
            if (nraw < 6) { nraw++; printf("[aimhud] v1103A npc %08X name object %08X (npc+%X): %08X %08X %08X | bytes at w0: %02X %02X %02X %02X %02X %02X %02X %02X -> '%s'\n",
                t, o, o - t, w0, w1, w2, ok(w0) ? vm_base[w0] : 0, ok(w0) ? vm_base[w0 + 1] : 0, ok(w0) ? vm_base[w0 + 2] : 0, ok(w0) ? vm_base[w0 + 3] : 0,
                ok(w0) ? vm_base[w0 + 4] : 0, ok(w0) ? vm_base[w0 + 5] : 0, ok(w0) ? vm_base[w0 + 6] : 0, ok(w0) ? vm_base[w0 + 7] : 0, cache[k].name); fflush(stdout); }
        }
    }
    for (int i = 0; cache[k].name[i] && i < 63; i++) g_aimhud_name[i] = cache[k].name[i];
    g_aimhud_name[strlen(cache[k].name) < 63 ? strlen(cache[k].name) : 63] = 0;
    g_aimhud_hp = hp; g_aimhud_hpmax = hm; g_aimhud_on = 1; g_aimhud_t = (unsigned long long)GetTickCount64();
}

/* v1106D gf_dialogue.c: the NPC's name for the menu heading / subtitles (same lookup as the free-aim HUD); main PPU thread. 0 = none */
int gf_npc_name(uint32_t t, char* out, int cap)
{
    if (!out || cap < 2) return 0; out[0] = 0;
    static struct { uint32_t ent; char name[64]; } cache[16]; static int ci;
    __try {
        for (int i = 0; i < 16; i++) if (cache[i].ent == t && cache[i].name[0]) { snprintf(out, (size_t)cap, "%s", cache[i].name); return 1; }
        uint64_t obj = 0; gfn f = lookup(0x0012D5BCu); char nm[64] = "";
        if (f && gcallx(f, t, 0, 0, 0, 0.0, &obj) && ok((uint32_t)obj)) {
            const uint32_t o = (uint32_t)obj, w0 = rd32(o);
            const uint32_t cand[6] = { w0, w0 + 4u, w0 + 8u, w0 + 12u, w0 + 16u, o };
            for (int c = 0; c < 6 && !nm[0]; c++) if (ok(cand[c])) ah_text(cand[c], nm, sizeof nm);
        }
        if (!nm[0]) return 0;
        cache[ci].ent = t; snprintf(cache[ci].name, sizeof cache[ci].name, "%s", nm); ci = (ci + 1) % 16;
        snprintf(out, (size_t)cap, "%s", nm); return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* v1105T voice tester (to learn which of the 216 vocal events is a greeting / reply / insult / scared line by ear): pick
 * the event id (Page Up / Page Down), Insert = the NPC under the crosshair (else the nearest within 6 m) says it, Home = the player says it. Same call as
 * the game's "say once" helper func_0012D05C: context frame func_0009764C(buf, 0, 0) -> func_00239EDC(ent, id, 0, buf) ->
 * func_0009767C(buf); the event's cooldown (table *(TOC-0x5EAC) + 16 id + 8, last time) is cleared first. Readout published to the
 * overlay (g_vt_*). GF_VOICE_TEST=0 off. Log [voice-test]. */
char g_vt_text[96]; volatile unsigned long long g_vt_t;
static int vt_fg(void) { HWND h = GetForegroundWindow(); DWORD pid = 0; if (h) GetWindowThreadProcessId(h, &pid); return pid == GetCurrentProcessId(); }
static int vt_say(uint32_t ent, uint32_t id, uint32_t* res)
{
    ppu_context* cur = g_active_ctx; if (!cur) return 0;
    gfn f_push = lookup(0x0009764Cu), f_say = lookup(0x00239EDCu), f_pop = lookup(0x0009767Cu);
    if (!f_push || !f_say || !f_pop) return 0;
    const uint32_t tab = rd32(TOC - 0x5EACu); if (ok(tab) && id < 0xD8) wrf(tab + 16u * id + 8u, -100000.0f);
    const uint32_t base = ((uint32_t)cur->gpr[1] - 0x1000u) & ~0xFu, buf = base - 0x40u, sp = buf - 0x200u;
    static ppu_context c; int okc = 0;
    __try {
        for (int step = 0; step < 3; step++) {
            memcpy(&c, cur, sizeof c); c.gpr[1] = sp; c.gpr[2] = TOC; c.lr = 0;
            if (step == 0) { c.gpr[3] = buf; c.gpr[4] = 0; c.gpr[5] = 0; }
            else if (step == 1) { c.gpr[3] = ent; c.gpr[4] = id; c.gpr[5] = 0; c.gpr[6] = buf; }
            else { c.gpr[3] = buf; }
            g_active_ctx = &c; (step == 0 ? f_push : step == 1 ? f_say : f_pop)(&c);
            while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&c); }
            g_active_ctx = cur;
            if (step == 1 && res) *res = (uint32_t)c.gpr[3];
        }
        okc = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_active_ctx = cur; g_trampoline_fn = 0; }
    return okc;
}
static void voice_test(uint32_t pl)
{
    static int on = -1; if (on < 0) { const char* e = getenv("GF_VOICE_TEST"); on = !(e && *e == '0'); }
    if (!on) return;
    static int id = 0, prev[4];
    const int fg = vt_fg();
    const int k[4] = { fg && (GetAsyncKeyState(VK_PRIOR) & 0x8000), fg && (GetAsyncKeyState(VK_NEXT) & 0x8000), fg && (GetAsyncKeyState(VK_INSERT) & 0x8000), fg && (GetAsyncKeyState(VK_HOME) & 0x8000) };
    int dn[4]; for (int i = 0; i < 4; i++) { dn[i] = k[i] && !prev[i]; prev[i] = k[i]; }
    if (!(dn[0] || dn[1] || dn[2] || dn[3])) return;
    if (dn[0]) id = (id + 0xD8 - 1) % 0xD8;
    if (dn[1]) id = (id + 1) % 0xD8;
    if (dn[0] || dn[1]) { snprintf(g_vt_text, sizeof g_vt_text, "VOICE TEST  EVENT %02X   INSERT = NPC   HOME = PLAYER", id); g_vt_t = (unsigned long long)GetTickCount64(); return; }
    uint32_t who = 0;
    if (dn[3]) who = pl;
    else { extern uint32_t gf_aim_target_now(void); extern uint32_t gf_nearest_npc(float maxd); who = gf_aim_target_now(); if (!who) who = gf_nearest_npc(6.0f); }
    if (!ok(who)) { snprintf(g_vt_text, sizeof g_vt_text, "VOICE TEST  EVENT %02X   NO NPC NEARBY", id); g_vt_t = (unsigned long long)GetTickCount64();
        printf("[voice-test] v1105T event 0x%02X: no NPC found within 6 m\n", id); fflush(stdout); return; }
    uint32_t r = 0; const int okc = vt_say(who, (uint32_t)id, &r);
    char vb[12] = "-"; { const uint32_t vp = rd32(who + 0x1964); if (ok(vp)) { int L = 0; while (L < 120 && vm_base[vp + (uint32_t)L]) L++; if (L >= 8) { for (int q = 0; q < 8; q++) vb[q] = (char)vm_base[vp + (uint32_t)(L - 8 + q)]; vb[8] = 0; } } }
    snprintf(g_vt_text, sizeof g_vt_text, "VOICE TEST  EVENT %02X  %s %s  %s", id, who == pl ? "PLAYER" : "NPC", vb, !okc ? "CALL FAILED" : r ? "SPEAKING" : "NO LINE");
    g_vt_t = (unsigned long long)GetTickCount64();
    printf("[voice-test] v1105T event 0x%02X on %s %08X (voice %s, gender %u, eth %d): %s\n", id, who == pl ? "PLAYER" : "npc", who, vb,
           (unsigned)vm_base[who + 0x1814 + 3], (int)rd32(who + 0x153C), !okc ? "call failed" : r ? "speaking" : "no line"); fflush(stdout);
}

static void corpse_tick(uint32_t pl)
{
    __try { voice_test(pl); } __except (EXCEPTION_EXECUTE_HANDLER) { }   /* v1105T */
    __try { aimhud_tick(pl); } __except (EXCEPTION_EXECUTE_HANDLER) { }   /* v1103A */
    __try { name_probe(pl); } __except (EXCEPTION_EXECUTE_HANDLER) { }   /* v1102N */
    if (s_corpse < 0) { const char* e = getenv("GF_CORPSE"); s_corpse = (e && *e == '0') ? 0 : 1;
        e = getenv("GF_CORPSE_DMG"); if (e && *e) s_corpse_dmg = (float)atof(e);
        e = getenv("GF_CORPSE_TORSO"); if (e && *e) { s_corpse_torso = (float)atof(e); if (!(s_corpse_torso >= 0.0f)) s_corpse_torso = 0.2f; if (s_corpse_torso > 1.0f) s_corpse_torso = 1.0f; }
        e = getenv("GF_CORPSE_PUSH"); if (e && *e) { s_corpse_push = (float)atof(e); if (!(s_corpse_push > 0.0f)) s_corpse_push = 4.0f; if (s_corpse_push > 12.0f) s_corpse_push = 12.0f; }
        printf("[corpse] v1091C dead bodies stay physical (shots, kicks, the car): %s\n", s_corpse ? "ON" : "off (GF_CORPSE=0)"); fflush(stdout); }
    if (!s_corpse) return;
    { static double t0; static int nf; const double nw = now_ms(); nf++; if (!t0) t0 = nw; if (nw - t0 >= 2000.0) { s_game_fps = nf * 1000.0 / (nw - t0); nf = 0; t0 = nw; } }
    int any = 0; for (int i = 0; i < MAXR; i++) if (corpse_ok(&s_rg[i])) { any = 1; break; }
    static int clip[11]; static uint32_t citem[11]; static int r1_prev, sq_prev; static double t_press, t_kick; static int press_pending;
    const double n = now_ms();
    int shots = 0;
    for (int s = 0; s < 11; s++) { const uint32_t it = gun_item(pl, s); const int c = it ? (int)rd32(it + 0x1ECu) : -1;
        if (it && it == citem[s] && c >= 0 && c < clip[s] && clip[s] - c <= 6) shots += clip[s] - c;
        citem[s] = it; clip[s] = c; }
    if (shots) snd_mark("shot", pl, (float)shots, 0.0f);   /* v1113S */
    const int r1 = (g_gf_combat_btn & 0x08) != 0, sq = (g_gf_combat_btn & 0x80) != 0;
    int drawn = 0; { int (*f)(void) = g_pad_drawn_fn; if (f) drawn = f(); }
    if (r1 && !r1_prev) { t_press = n; press_pending = 1; }
    if (!any) { r1_prev = r1; sq_prev = sq; press_pending = 0; t_kick = 0; return; }
    /* v1110Z (user: "he suddenly reacts to a shot that wasn't there"): (a) the PRESS fallback (fire pressed, no clip change in 90 ms)
     * is for infinite ammo only -- with a normal gun a press that did not fire (cooldown, reload) or whose clip change came late
     * was a phantom / second shot; (b) every shot waits 100 ms: if the GAME registered a bullet on anyone (a living NPC, or this
     * body itself through pain_hit_check) from 80 ms before to 100 ms after it, our own trace of it is dropped. */
    static double q_t[8]; static const char* q_how[8]; static int q_n;
    int all_inf = 1, any_gun = 0; for (int s2 = 0; s2 < 11; s2++) { const uint32_t it = gun_item(pl, s2); if (!it) continue; any_gun = 1; if (!(rd32(it + 0x1DCu) & 0x20u)) all_inf = 0; }
    if (shots) { press_pending = 0; for (int k = 0; k < shots && k < 6 && q_n < 8; k++) { q_t[q_n] = n; q_how[q_n] = "clip"; q_n++; } }
    else if (press_pending && n - t_press > 90.0) {   /* fire pressed, no clip change */
        press_pending = 0;
        if (drawn) { if (any_gun && all_inf && q_n < 8) { q_t[q_n] = n; q_how[q_n] = "press"; q_n++; } }
        else t_kick = n + 130.0;   /* fists: a swing */
    }
    for (int k = 0; k < q_n; ) {
        if (n - q_t[k] < 100.0) { k++; continue; }
        if (n - q_t[k] > 400.0) { static int nls = 0; if (nls < 100) { nls++; CLOG("[corpse] v1113H miss (%s): shot not traced for %.0f ms -> dropped\n", q_how[k], n - q_t[k]); } }   /* stale */
        else if (s_game_hit_t > q_t[k] - 80.0 && s_game_hit_t < q_t[k] + 100.0) CLOG("[corpse] v1110Z shot (%s) hit something the game registered -> not traced into a body\n", q_how[k]);
        else corpse_fire(pl, q_how[k]);
        for (int j = k + 1; j < q_n; j++) { q_t[j - 1] = q_t[j]; q_how[j - 1] = q_how[j]; } q_n--;
    }
    if (sq && !sq_prev && !drawn) t_kick = n + 220.0;
    if (t_kick > 0 && n >= t_kick) { t_kick = 0; corpse_melee(pl); }
    corpse_touch(pl);   /* v1100L */
    corpse_car();
    r1_prev = r1; sq_prev = sq;
}

/* called by the ppu_recomp_001.cpp wrapper after every skeleton update (func_003CC128) */

/* ------------------------------------------------------------------ v1117A WOUNDS ON BODIES
 * (user: "get blood ON the NPC textures when I beat them / shoot them"). Every bullet that lands (blood_hit) and every melee
 * hit (melee_blood) pins a wound to the nearest palette bone (bone-space position + outward normal, like the arteries). After
 * each skeleton update of that body (wound_tick) its world position / normal go to the renderer (rsx_ui_overlay.c gfb_set),
 * which projects the game's own blood splat onto whatever is drawn there (shirt, face, coat) in the decal pass -- it moves
 * with the body, alive, ragdolling or dead. Wounds grow ~40 % over 25 s (soaking in) and fade after 150 s.
 * GF_BODY_BLOOD=0 off. Log [wound]. */
extern void gfb_set(int i, const float* p, const float* n, const float* t, float size, float alpha, int sprite);
typedef struct { int on, slot, sprite; uint32_t pal, ent; v3 loc, nrl; float size; double t0; } wound_t;
#define NWND 64
static wound_t s_wnd[NWND]; static volatile LONG s_wnd_live; static int s_wnd_on = -1;
static void wound_add(uint32_t ent, v3 hit_w, v3 sh_w, float size, int sprite)
{
    if (s_wnd_on < 0) { const char* e = getenv("GF_BODY_BLOOD"); s_wnd_on = !(e && *e == '0'); printf("[wound] v1117A blood on bodies %s\n", s_wnd_on ? "ON" : "off (GF_BODY_BLOOD=0)"); fflush(stdout); }
    if (!s_wnd_on) return;
    const uint32_t anim = rd32(ent + 0xC4); if (!ok(anim)) return;
    const uint32_t pal = anim_pal(anim); if (!ok(pal)) return;
    m3 R; v3 T; if (!ent_xform(ent, &R, &T)) return;
    const m3 Mc = w2mc(&R, s_conv);
    const v3 hm = mv(&Mc, sub(hit_w, T)), tosh = mv(&Mc, nrm(sub(sh_w, hit_w)));
    int ns = 0; if (!rig_key(anim, &ns) || ns <= 0 || ns > MAXS) return;
    int best = -1; float bd = 1e9f;
    for (int sl = 0; sl < ns; sl++) { const v3 q = slot_pos(pal, sl); if (len(q) < 0.03f || !fok(q.x)) continue; const float d = len(sub(q, hm)); if (d < bd) { bd = d; best = sl; } }
    if (best < 0 || bd > 0.35f) return;
    const uint32_t m = pal + (uint32_t)best * 64u;
    const v3 r0 = V(rdf(m), rdf(m + 4), rdf(m + 8)), r1 = V(rdf(m + 16), rdf(m + 20), rdf(m + 24)), r2 = V(rdf(m + 32), rdf(m + 36), rdf(m + 40));
    const float l0 = dot(r0, r0), l1 = dot(r1, r1), l2 = dot(r2, r2); if (l0 < 0.25f || l1 < 0.25f || l2 < 0.25f) return;
    const v3 dd = sub(hm, slot_pos(pal, best));
    const v3 nm = nrm(add(mul(nrm(dd), 0.45f), mul(tosh, 0.55f)));   /* outward: away from the bone, towards the shooter */
    if (!fok(nm.x) || len(nm) < 0.5f) return;
    const v3 loc = V(dot(r0, dd) / l0, dot(r1, dd) / l1, dot(r2, dd) / l2), nrl = V(dot(r0, nm) / l0, dot(r1, nm) / l1, dot(r2, nm) / l2);
    for (int i = 0; i < NWND; i++) { wound_t* w = &s_wnd[i];   /* a hit next to an existing wound makes that one bigger */
        if (w->on && w->ent == ent && w->slot == best && len(sub(w->loc, loc)) < 0.05f) { w->size = fminf(w->size * 1.25f + 0.01f, 0.30f); w->t0 = now_ms(); return; } }
    wound_t* w = 0; for (int i = 0; i < NWND; i++) if (!s_wnd[i].on) { w = &s_wnd[i]; break; }
    if (!w) { w = &s_wnd[0]; for (int i = 1; i < NWND; i++) if (s_wnd[i].t0 < w->t0) w = &s_wnd[i]; }
    memset(w, 0, sizeof *w);
    w->ent = ent; w->pal = pal; w->slot = best; w->loc = loc; w->nrl = nrl; w->size = size; w->sprite = sprite; w->t0 = now_ms(); w->on = 1;
    InterlockedExchange(&s_wnd_live, 1);
    static int nl = 0; if (nl < 200) { nl++; printf("[wound] v1117A ent=%08X wound on bone slot %d (%.2f m from it), %.2f m, sprite %d\n", ent, best, bd, size, sprite); fflush(stdout); }
}
static void wound_tick(uint32_t anim, uint32_t cur)
{
    if (!s_wnd_live) return;
    const uint32_t pal = anim_pal(anim); if (!ok(pal)) return;
    int any = 0;
    for (int i = 0; i < NWND; i++) {
        wound_t* w = &s_wnd[i]; if (!w->on) continue; any = 1;
        if (w->pal != pal) continue;
        const float age = (float)((now_ms() - w->t0) / 1000.0);
        if (age > 150.0f) { w->on = 0; continue; }
        const uint32_t e = ok(cur) ? cur : w->ent;
        m3 R; v3 T; if (!ent_xform(e, &R, &T)) continue;
        const m3 Mc = w2mc(&R, s_conv), MT = tr(&Mc);
        const uint32_t m = pal + (uint32_t)w->slot * 64u;
        const v3 r0 = V(rdf(m), rdf(m + 4), rdf(m + 8)), r1 = V(rdf(m + 16), rdf(m + 20), rdf(m + 24)), r2 = V(rdf(m + 32), rdf(m + 36), rdf(m + 40));
        const v3 pm = add(slot_pos(pal, w->slot), add(add(mul(r0, w->loc.x), mul(r1, w->loc.y)), mul(r2, w->loc.z)));
        const v3 nmd = nrm(add(add(mul(r0, w->nrl.x), mul(r1, w->nrl.y)), mul(r2, w->nrl.z)));
        if (!fok(pm.x) || !fok(nmd.x) || len(nmd) < 0.5f) continue;
        const v3 pw = add(T, mv(&MT, pm)), nw = nrm(mv(&MT, nmd));
        v3 tw = cross(nw, V(0, 1, 0)); if (len(tw) < 0.2f) tw = cross(nw, V(1, 0, 0)); tw = nrm(tw);
        const float grow = 1.0f + 0.4f * smooth01(age / 25.0f), fade = age > 140.0f ? (150.0f - age) / 10.0f : 1.0f;
        gfb_set(i, &pw.x, &nw.x, &tw.x, w->size * grow, 0.92f * fade, w->sprite);
    }
    if (!any) InterlockedExchange(&s_wnd_live, 0);
}
/* ------------------------------------------------------------------ v1117A TALKING JAW (gf_dialogue.c gf_dialogue_mouth)
 * The speaker's JO_mandible (game string 0x10000198, found with the entity's findBone like the ragdoll's JO_* bones) is
 * rotated open after each skeleton update: hinge = the shoulder line (left-right), sign chosen so that a point in front of the
 * hinge (towards the eyes, JO_eye_L / _R) goes DOWN. Up to GF_MOUTH_DEG (13) degrees. GF_MOUTH=0 off. Log [mouth]. */
static struct { uint32_t ent, anim; int sj, se1, se2, ssl, ssr, nec, ok, cl, cr, lo[2], hi[2]; } s_mc;   /* v1117D: + mouth corners / lips */
static m3 mouth_rot(v3 ax, float a)   /* rotation about the unit axis ax by a radians (y = M x, row-major like mv) */
{
    ax = nrm(ax); const float c = cosf(a), sn = sinf(a), t = 1 - c;
    m3 r = { { t * ax.x * ax.x + c, t * ax.x * ax.y - sn * ax.z, t * ax.x * ax.z + sn * ax.y,
               t * ax.x * ax.y + sn * ax.z, t * ax.y * ax.y + c, t * ax.y * ax.z - sn * ax.x,
               t * ax.x * ax.z - sn * ax.y, t * ax.y * ax.z + sn * ax.x, t * ax.z * ax.z + c } };
    return r;
}
static int mouth_slot(gfn fb, uint32_t ent, uint32_t s, uint32_t tab, int nb, uint32_t name)
{
    wr32(s, 0xFFFFFFFFu); if (!gcall(fb, ent, s, name)) return -1;
    const int ix = (int)rd32(s); if (ix < 0 || ix >= nb) return -1;
    const int sl = vm_base[tab + (uint32_t)ix]; return sl == 0xFF ? -1 : sl;
}
static void mouth_post(uint32_t anim, uint32_t ent)
{
    static int on = -1; static float maxdeg = 14.0f;   /* v1117B 13 -> 20; v1117D 14 (the lips sit far from the hinge) */
    if (on < 0) { const char* e = getenv("GF_MOUTH"); on = !(e && *e == '0'); e = getenv("GF_MOUTH_DEG"); if (e && *e) maxdeg = (float)atof(e); printf("[mouth] v1117A talking jaw %s (%.0f deg)\n", on ? "ON" : "off", maxdeg); fflush(stdout); }
    if (!on || !ok(ent)) return;
    extern int gf_dialogue_mouth(uint32_t ent, float* open);
    float o = 0.0f; if (!gf_dialogue_mouth(ent, &o)) return;
    if (rd32(ent + 0xC4) != anim) return;
    const uint32_t pal = anim_pal(anim); if (!ok(pal)) return;
    if (s_mc.ent != ent || s_mc.anim != anim) {
        memset(&s_mc, 0, sizeof s_mc); s_mc.ent = ent; s_mc.anim = anim;
        gfn fb = lookup(vfunc(ent, 0x94)); ppu_context* c = g_active_ctx; const uint32_t s = c ? (((uint32_t)c->gpr[1] - 0x800u) & ~0xFu) : 0;
        const uint32_t tab = rd32(anim + 0x78); const int nb = anim_nbones(anim);
        if (fb && s && ok(tab) && nb > 0) {
            s_mc.sj = mouth_slot(fb, ent, s, tab, nb, 0x10000198u);  /* JO_mandible */
            s_mc.se1 = mouth_slot(fb, ent, s, tab, nb, 0x10000188u); /* JO_eye_L */
            s_mc.se2 = mouth_slot(fb, ent, s, tab, nb, 0x100001A8u); /* JO_eye_R */
            s_mc.ssl = mouth_slot(fb, ent, s, tab, nb, 0x10000148u); /* JO_shoulder_L */
            s_mc.ssr = mouth_slot(fb, ent, s, tab, nb, 0x100001B8u); /* JO_shoulder_R */
            s_mc.nec = mouth_slot(fb, ent, s, tab, nb, 0x10001D30u); /* JO_neck */
            s_mc.ok = s_mc.sj >= 0 && s_mc.ssl >= 0 && s_mc.ssr >= 0 && s_mc.nec >= 0;
        }
        /* v1117D (user: "just the jaw moving down without the mouth opening"): the lips are NOT skinned to JO_mandible. Face rig
         * (probe v1117C, same on every NPC + the player): mouth corners JO_orbOris_CL / _CR (slots 60 / 61), and four lip
         * bones in front of them -- the lower pair (slots 56 / 57, ~7 mm under) and the upper pair (62 / 63). Found here by
         * position: forward of the corners' midpoint, within 2 cm of its height and 3.5 cm of the centre line, sorted by height. */
        s_mc.cl = s_mc.cr = s_mc.lo[0] = s_mc.lo[1] = s_mc.hi[0] = s_mc.hi[1] = -1;
        if (fb && s && ok(tab) && nb > 0 && s_mc.se1 >= 0 && s_mc.se2 >= 0 && s_mc.ssl >= 0 && s_mc.ssr >= 0 && s_mc.nec >= 0) {
            const uint32_t gs = s + 0x100u;
            memcpy(vm_base + gs, "JO_orbOris_CL", 14); s_mc.cl = mouth_slot(fb, ent, s, tab, nb, gs);
            memcpy(vm_base + gs, "JO_orbOris_CR", 14); s_mc.cr = mouth_slot(fb, ent, s, tab, nb, gs);
            int ns = 0; rig_key(anim, &ns);
            if (s_mc.cl >= 0 && s_mc.cr >= 0 && ns > 0 && ns <= MAXS) {
                const v3 em = mul(add(slot_pos(pal, s_mc.se1), slot_pos(pal, s_mc.se2)), 0.5f);
                const v3 ax2 = nrm(sub(slot_pos(pal, s_mc.ssr), slot_pos(pal, s_mc.ssl))), up2 = nrm(sub(em, slot_pos(pal, s_mc.nec)));
                v3 fw2 = nrm(cross(up2, ax2)); if (s_mc.sj >= 0 && dot(sub(em, slot_pos(pal, s_mc.sj)), fw2) < 0) fw2 = mul(fw2, -1.0f);
                const v3 cm = mul(add(slot_pos(pal, s_mc.cl), slot_pos(pal, s_mc.cr)), 0.5f);
                int cand[8], nc = 0; float hgt[8];
                for (int sl = 0; sl < ns && nc < 8; sl++) {
                    if (sl == s_mc.cl || sl == s_mc.cr || sl == s_mc.sj || sl == s_mc.se1 || sl == s_mc.se2) continue;
                    const v3 q = sub(slot_pos(pal, sl), cm);
                    if (dot(q, fw2) > 0.004f && dot(q, fw2) < 0.04f && fabsf(dot(q, up2)) < 0.02f && fabsf(dot(q, ax2)) < 0.035f) { cand[nc] = sl; hgt[nc] = dot(q, up2); nc++; }
                }
                for (int i2 = 1; i2 < nc; i2++) for (int j = i2; j > 0 && hgt[j] < hgt[j - 1]; j--) { float th = hgt[j]; hgt[j] = hgt[j - 1]; hgt[j - 1] = th; int ts = cand[j]; cand[j] = cand[j - 1]; cand[j - 1] = ts; }
                if (nc >= 2) { s_mc.lo[0] = cand[0]; s_mc.lo[1] = cand[1]; }
                if (nc >= 4) { s_mc.hi[0] = cand[2]; s_mc.hi[1] = cand[3]; }
                /* v1117E: a face caught mid-expression can miss some; every rig seen (NPCs + player) has the same layout */
                if (nc < 4 && s_mc.cl == 60 && s_mc.cr == 61 && ns > 63) { s_mc.lo[0] = 56; s_mc.lo[1] = 57; s_mc.hi[0] = 62; s_mc.hi[1] = 63; nc = -nc; }
                printf("[mouth] v1117D ent=%08X mouth corners %d / %d, lower lip %d / %d, upper lip %d / %d (%d candidates)\n", ent, s_mc.cl, s_mc.cr, s_mc.lo[0], s_mc.lo[1], s_mc.hi[0], s_mc.hi[1], nc); fflush(stdout);
            }
        }
        printf("[mouth] v1117A ent=%08X jaw slot %d, eyes %d / %d, shoulders %d / %d, neck %d -> %s\n", ent, s_mc.sj, s_mc.se1, s_mc.se2, s_mc.ssl, s_mc.ssr, s_mc.nec, s_mc.ok ? "talking" : "NO JAW (skeleton has none)"); fflush(stdout);
    }
    if (!s_mc.ok || o < 0.01f) return;
    const v3 pj = slot_pos(pal, s_mc.sj), pn = slot_pos(pal, s_mc.nec);
    v3 ax = nrm(sub(slot_pos(pal, s_mc.ssr), slot_pos(pal, s_mc.ssl)));
    const v3 up = nrm(sub(pj, pn));
    v3 fw = (s_mc.se1 >= 0 && s_mc.se2 >= 0) ? sub(mul(add(slot_pos(pal, s_mc.se1), slot_pos(pal, s_mc.se2)), 0.5f), pj) : cross(up, ax);
    fw = nrm(sub(fw, mul(ax, dot(fw, ax))));
    if (!fok(ax.x) || !fok(fw.x) || len(ax) < 0.5f || len(fw) < 0.5f) return;
    const float a = o * maxdeg * 0.0174533f;
    const m3 Rp = mouth_rot(ax, a), Rn = mouth_rot(ax, -a);
    const v3 chin = add(mul(fw, 0.08f), mul(up, -0.05f));
    const float sgn = dot(mv(&Rp, chin), up) < dot(mv(&Rn, chin), up) ? 1.0f : -1.0f;   /* the chin goes down */
    const m3 Rot = sgn > 0 ? Rp : Rn;
    /* v1117B (user: "barely opens, it's more like their throat"): the mandible joint is not where a jaw hinges -- rotating about
     * it swung the throat. Hinge = the jaw joint estimated from the eyes: GF_MOUTH_BACK (0.085 m) behind and GF_MOUTH_DOWN (0.045 m)
     * below the eye midpoint (just in front of the ear); the bone's position is carried round that pivot. Without eyes: the
     * bone itself (old). Geometry logged once per rig. */
    static float pback = -1.0f, pdown; if (pback < 0.0f) { const char* e = getenv("GF_MOUTH_BACK"); pback = e && *e ? (float)atof(e) : 0.085f; e = getenv("GF_MOUTH_DOWN"); pdown = e && *e ? (float)atof(e) : 0.045f; }
    v3 piv = pj;
    if (s_mc.se1 >= 0 && s_mc.se2 >= 0) {
        const v3 em = mul(add(slot_pos(pal, s_mc.se1), slot_pos(pal, s_mc.se2)), 0.5f);
        piv = sub(sub(em, mul(fw, pback)), mul(up, pdown));
        static uint32_t logged[8]; static int nlg = 0; int seen = 0; for (int q = 0; q < nlg; q++) if (logged[q] == s_mc.anim) seen = 1;
        if (!seen && nlg < 8) { logged[nlg++] = s_mc.anim; const v3 dj = sub(pj, em);
            printf("[mouth] v1117B ent=%08X jaw joint vs eye midpoint: %.3f forward, %.3f up, %.3f side (m); hinge %.3f forward %.3f up\n", ent,
                   dot(dj, fw), dot(dj, up), dot(dj, ax), -pback, -pdown); fflush(stdout); }
    }
    const uint32_t m = pal + (uint32_t)s_mc.sj * 64u;
    for (int r = 0; r < 3; r++) {
        const uint32_t row = m + (uint32_t)r * 16u; const v3 v = mv(&Rot, V(rdf(row), rdf(row + 4), rdf(row + 8)));
        wrf(row, v.x); wrf(row + 4, v.y); wrf(row + 8, v.z);
    }
    { const v3 np = add(piv, mv(&Rot, sub(pj, piv))); wrf(m + 0x30, np.x); wrf(m + 0x34, np.y); wrf(m + 0x38, np.z); }
    /* v1117D: the lower lip goes with the jaw, the corners half way, the upper lip lifts a little (opposite, 15 %) */
    { const m3 Rh = mouth_rot(ax, sgn * a * 0.5f);
      const m3 Ru = mouth_rot(ax, -sgn * a * 0.15f);
      const int sl[6] = { s_mc.lo[0], s_mc.lo[1], s_mc.cl, s_mc.cr, s_mc.hi[0], s_mc.hi[1] };
      for (int k = 0; k < 6; k++) {
          if (sl[k] < 0) continue;
          const m3* R = k < 2 ? &Rot : k < 4 ? &Rh : &Ru;
          const uint32_t mm2 = pal + (uint32_t)sl[k] * 64u; const v3 p0 = slot_pos(pal, sl[k]);
          for (int r = 0; r < 3; r++) { const uint32_t row = mm2 + (uint32_t)r * 16u; const v3 v = mv(R, V(rdf(row), rdf(row + 4), rdf(row + 8))); wrf(row, v.x); wrf(row + 4, v.y); wrf(row + 8, v.z); }
          const v3 np = add(piv, mv(R, sub(p0, piv))); wrf(mm2 + 0x30, np.x); wrf(mm2 + 0x34, np.y); wrf(mm2 + 0x38, np.z);
      } }
    static int nl = 0; if (nl < 20 && o > 0.3f) { nl++; printf("[mouth] v1117A ent=%08X jaw open %.2f (%.1f deg)\n", ent, o, o * maxdeg); fflush(stdout); }
}
void gf_rag_post_skel(uint32_t anim, double dt_in)
{
    if (!s_on) return;
    const uint32_t cur = s_cur_ent;
    if (s_art_live) { EnterCriticalSection(&s_cs); __try { artery_tick(anim, cur); } __except (EXCEPTION_EXECUTE_HANDLER) { } LeaveCriticalSection(&s_cs); }   /* v1069B */
    if (s_wnd_live) { EnterCriticalSection(&s_cs); __try { wound_tick(anim, cur); } __except (EXCEPTION_EXECUTE_HANDLER) { } LeaveCriticalSection(&s_cs); }   /* v1117A */
    __try { mouth_post(anim, cur); } __except (EXCEPTION_EXECUTE_HANDLER) { }   /* v1117A talking jaw */
    int mine = 0, live = 0;
    for (int i = 0; i < MAXR; i++) {
        const int st = s_rg[i].state;
        if (st >= 1 && st <= 4 && s_rg[i].anim == anim && (!cur || cur == s_rg[i].ent)) mine = 1;
        if ((st == 3 || st == 4 || st == 6) && !s_rg[i].hr) live = 1;
    }
    if (!mine) rig_learn(anim);   /* the game's own animation: learn the rig */
    { extern void gf_footik_post_skel(uint32_t anim, uint32_t ent); gf_footik_post_skel(anim, cur); }   /* v1078F player foot IK (gf_footik.c) -- AFTER the rig learner: it must only ever see the game's own animation (v1078F-3) */
    { extern void gf_smoke_post_skel(uint32_t anim, uint32_t ent); gf_smoke_post_skel(anim, cur); }
    { extern void gf_sit_post_skel(uint32_t anim, uint32_t ent); gf_sit_post_skel(anim, cur); }   /* v1109C seated pelvis (gf_sit.c) */   /* v1079S player plays a chosen anim (gf_smoke.c) */
    { const uint32_t pl = cur ? player_ent() : 0;   /* v1091C: the player's update, once per frame */
      if (pl && cur == pl && s_kd_test) { EnterCriticalSection(&s_cs); __try { kd_tick(); } __except (EXCEPTION_EXECUTE_HANDLER) { s_kd.ent = 0; } LeaveCriticalSection(&s_cs); }   /* v1097K */
      if (pl && cur == pl) { EnterCriticalSection(&s_cs); __try { corpse_tick(pl); } __except (EXCEPTION_EXECUTE_HANDLER) { s_corpse = 0; printf("[corpse] fault -> off\n"); fflush(stdout); } LeaveCriticalSection(&s_cs); } }
    if (!mine && !(live && cur)) return;
    EnterCriticalSection(&s_cs);
    __try {
        if (!mine && !try_handoff(anim, cur)) { LeaveCriticalSection(&s_cs); return; }
        for (int i = 0; i < MAXR; i++) {
            rag_t* r = &s_rg[i];
            if (!r->state || r->state >= 5 || r->anim != anim || (cur && cur != r->ent)) continue;
            const double n = now_ms();
            float dt = r->last_call > 0 ? (float)((n - r->last_call) / 1000.0) : (float)dt_in;   /* real time between this body's updates */
            if (!(dt > 1.0f / 240.0f)) dt = (float)dt_in > 1.0f / 240.0f && (float)dt_in < 0.05f ? (float)dt_in : 1.0f / 60.0f;
            if (dt > 0.05f) dt = 0.05f;
            if (r->t_wake > r->t0 + 2500.0 && r->state == 3) { r->cad_n++; if (r->last_call > 0 && n - r->last_call > r->cad_max) r->cad_max = n - r->last_call; }   /* v1091E */
            r->last_call = n; r->frame_dt = dt;
            if (r->hr) { hr_frame(r, dt); continue; }   /* v1069H live hit reaction */
            if (r->kd && (r->kd_ph || r->state == 4)) { kd_frame(r, n); continue; }   /* v1097N down / getting up */
            if (!still_valid(r)) {
                if (r->state >= 3) { RLOG("[rag] ent=%08X: skeleton changed after %.1f s -> waiting for the corpse\n", r->ent, (n - r->t0) / 1000.0);
                    r->resume_state = r->state; r->state = 6; r->wait_t0 = n; }
                else { RLOG("[rag] ent=%08X: skeleton changed before the start -> released\n", r->ent); r->state = 5; }
                continue;
            }
            r->anim_id = rd32(anim + 0x10);
            { m3 R; v3 T; if (ent_xform(r->ent, &R, &T)) r->last_T = T; }
            if (r->state == 1) { read_particles(r, r->snap1); r->state = 2; continue; }
            if (r->state == 2) {
                if (!rag_start(r, dt)) { r->state = 5; continue; }
                if (r->kd) kd_knock(r, dt);   /* v1097N */
                r->state = s_phys ? 3 : 5;
                if (r->car) {   /* v1069C: legs take the car's speed, hips less, the upper body hardly at first -> it folds onto the hood */
                    const m3 Md = w2mc(&r->Rd, r->conv); const v3 vm = mv(&Md, r->car_vw); const float vsp = len(V(vm.x, 0, vm.z));
                    const v3 vh = V(vm.x, 0, vm.z);
                    for (int i = 0; i < NP; i++) {
                        const float y = r->p[i].y - r->ground;
                        const float f = y < 0.6f ? 1.0f : y < 1.05f ? 0.75f : 0.22f, up = y < 0.6f ? 0.10f : y < 1.05f ? 0.18f : 0.04f;
                        const v3 vi = add(mul(vh, f), V(0, up * vsp, 0));
                        r->pp[i] = sub(r->p[i], mul(vi, dt));
                    }
                }
                if (s_jolt && s_phys) {
                    r->jh = jr_create(&r->p[0].x, &r->pp[0].x, dt, r->ground, (r->knsgn[0] + r->knsgn[1]) >= 0 ? 1.0f : -1.0f, r->elsgn[0]);
                    if (r->jh) {
                        { static float* buf = NULL; const int maxn = 60000; static float rad = -1.0f;   /* v1057C level collision */
                          if (rad < 0.0f) { const char* e = getenv("GF_RAG_COLL_R"); rad = e && *e ? (float)atof(e) : 10.0f; if (rad < 2.0f) rad = 2.0f; if (rad > 40.0f) rad = 40.0f; }
                          if (!buf) buf = (float*)malloc((size_t)maxn * 36);
                          if (buf) {
                              const m3 Md0 = w2mc(&r->Rd, r->conv), MdT0 = tr(&Md0);
                              const v3 cw = add(r->Td, mv(&MdT0, r->p[PEL]));   /* pelvis in world space */
                              int nfl = 0; const int nt = col_gather(r, cw, rad, buf, maxn, &nfl);
                              if (nt) jr_add_mesh(r->jh, buf, nt, nfl ? 0.6f : 0.0f);
                              RLOG("[rag] ent=%08X: level collision %d triangles within %.0f m (%s)\n", r->ent, nt, rad,
                                   nt ? (nfl ? "ground from the level, floor box lowered" : "no level ground under the body, floor box kept") : "none here");
                          } }
                        if (r->car) { float rot9[9]; v3 cp;   /* v1069C: the car as a moving solid in this body's world */
                            if (car_frame(r, r->car, rot9, &cp)) {
                                m3 Rc; v3 Tc; ent_xform(r->car, &Rc, &Tc);
                                r->car_oy = car_oy_for(r, Tc);   /* v1108Z: the body's own floor (was gf_collision.bin) */
                                const m3 Md = w2mc(&r->Rd, r->conv); const v3 vm = mv(&Md, r->car_vw); const v3 cp0 = sub(cp, mul(V(vm.x, 0, vm.z), 0.03f));
                                jr_car_add(r->jh, rot9, &cp0.x, r->car_oy); r->car_on = 1;
                                RLOG("[rag-car] ent=%08X: car in the ragdoll's world (origin %.2f m above the ground)\n", r->ent, -r->car_oy);
                            } }
                        r->jasleep = jr_get(r->jh, r->jq, r->jx, &r->p[0].x); RLOG("[rag] ent=%08X: Jolt ragdoll (11 parts)\n", r->ent);
                        r->bhv_on = 0;   /* v1075R dying behaviours: not for cars, corpses, melee (no hit dir) or GF_RAG_BHV=0 */
                        if (s_bhv && r->bhv_zone >= 0 && !r->car && !r->corpse) {
                            bhv_init(&r->bh, &r->p[0].x, r->ground, r->bhv_zone, r->dmg, r->low_start, r->bhv_dir, r->ent * 2654435761u ^ (unsigned)s_ndeath);
                            float wl[3]; wl[0] = r->wound_loc.x; wl[1] = r->wound_loc.y; wl[2] = r->wound_loc.z;
                            bhv_set_side(&r->bh, r->bhv_side, r->have_wound ? wl : NULL);
                            r->bhv_on = r->bh.prog != BHV_NONE;
                            RLOG("[rag-bhv] ent=%08X zone %d side %d -> %s (kneel %d, steps %d, agony %d)\n", r->ent, r->bhv_zone, r->bhv_side,
                                 r->bh.prog == BHV_STAGGER ? "stagger" : r->bh.prog == BHV_FALL ? "fall" : "none", r->bh.kneel, r->bh.steps, r->bh.agony);
                        }
                        if (!r->kd) blood_on_death(r);
                        { const m3 Mh = w2mc(&r->Rd, r->conv); const v3 hmd = mv(&Mh, sub(r->hit_w, r->Td)); v3 ptd; int a2; float ms;   /* v1069B */
                          if (!r->kd && fok(r->hit_w.x) && fok(r->shooter_w.x) && len(sub(r->hit_w, r->shooter_w)) > 0.1f) blood_hit(r->ent, r->p, hmd, hit_part(r->p, hmd, &ptd, &a2, &ms), r->hit_w, r->shooter_w, r->dmg, 1); }
                        r->bl_pool = 0; r->bl_hits = 0; r->bl_vy[0] = r->bl_vy[1] = r->bl_vy[2] = 0; }   /* v1057B */
                }
                if (!s_phys) RLOG("[rag] GF_RAG_PHYS=0: not moving ent=%08X\n", r->ent);
            }
            if (r->state == 3 && r->jh) {
                read_anim(r);
                const float t = (float)((n - r->t0) / 1000.0);
                int bact = 0, clutch_ok = 1, brace_ok = 1; float tb = t;
                if (r->bhv_on) {   /* v1075R: the behaviour owns tone, motor targets and assist forces while it runs */
                    bhv_out* o = &r->bo; float vpel[3]; static float hold[63];
                    jr_part_vel(r->jh, D_LT, &r->p[PEL].x, vpel);
                    bhv_frame(&r->bh, t, dt, &r->p[0].x, vpel, o);
                    if (o->active) {
                        bact = 1;
                        memcpy(hold, &r->p[0].x, sizeof hold);
                        const float* tgt = o->use_pose ? o->pose : o->hold_pose ? hold : ((r->have_anim && !r->corpse) ? &r->a[0].x : NULL);
                        jr_tone(r->jh, tgt, o->use_tone ? o->tone[0] : tone_of(r, 0, t), o->use_tone ? o->tone[1] : tone_of(r, 1, t),
                                o->use_tone ? o->tone[2] : (r->low_start ? 0.0f : tone_of(r, 2, t)), o->phase == BHV_PH_STAGGER ? -1 : r->hit_leg);
                        for (int f = 0; f < o->nf; f++) { const bhv_force* F = &o->f[f];
                            if (F->ext) jr_assist(r->jh, F->part, F->pt, F->tgt, F->kp, F->kd, F->ff); else jr_pull(r->jh, F->part, F->pt, F->tgt, F->k); }
                        if (o->yaw_damp > 0) jr_yaw_damp(r->jh, o->yaw_damp);
                        if (o->yaw_kp > 0) { const float w = 0.5f * (jr_yaw_rate(r->jh, D_LT) + jr_yaw_rate(r->jh, D_UT));
                            const float ty = o->yaw_kp * o->yaw_err - 0.18f * o->yaw_kp * w; const float t1[3] = { 0, 0.55f * ty, 0 }, t2[3] = { 0, 0.45f * ty, 0 };
                            jr_torque(r->jh, D_LT, t1); jr_torque(r->jh, D_UT, t2); }
                        clutch_ok = o->clutch;
                        brace_ok = !o->no_brace && o->phase == BHV_PH_FALL;
                        if (o->phase == BHV_PH_FALL) tb = 0.12f + (t - r->bh.t_fall);   /* the brace window starts when he actually falls */
                        if (o->phase != r->bhv_last) { RLOG("[rag-bhv] ent=%08X %.2f s: %s\n", r->ent, t, bhv_phase_name(o->phase)); r->bhv_last = o->phase; }
                    } else { r->bhv_on = 0; RLOG("[rag-bhv] ent=%08X %.2f s: behaviour done\n", r->ent, t); }
                }
                if (!bact) jr_tone(r->jh, (r->have_anim && !r->corpse) ? &r->a[0].x : NULL, tone_of(r, 0, t), tone_of(r, 1, t), r->low_start ? 0.0f : tone_of(r, 2, t), r->hit_leg);
                if (r->have_wound && !r->headshot && t > 0.08f && clutch_ok && (bact || t < r->writhe_end + 1.0f)) {   /* hand to the wound */
                    const v3 w = wound_now(r); const v3 out = nrm(sub(w, r->p[r->wound_half ? THO : PEL])); const v3 tg = add(w, mul(out, 0.05f));
                    const float fade = bact ? 1.0f : t < r->writhe_end ? 1.0f : 1.0f - (t - r->writhe_end);
                    jr_pull(r->jh, r->clutch_arm ? D_LAR : D_LAL, &r->p[r->clutch_arm ? WRR : WRL].x, &tg.x, (t < 1.0f ? 0.6f : 0.35f) * r->clutch_str * fade);
                }
                if (s_brace && brace_ok && !r->headshot && !r->corpse && tb > 0.12f && tb < 1.3f) {   /* v1069H: falling forward -> hands out */
                    const v3 fw = mul(body_fwd(r->p), r->fwd_sign); const float fd = -fw.y, hgt = r->p[THO].y - r->ground;
                    float k = (fd - 0.15f) / 0.4f; k = k < 0 ? 0 : k > 1 ? 1 : k; k *= 0.5f * (tb < 0.9f ? 1.0f : 1.0f - (tb - 0.9f) / 0.4f);
                    const v3 hz = V(fw.x, 0, fw.z);
                    if (hgt > 0.25f && k > 0.01f && len(hz) > 0.05f) {
                        const v3 hn = nrm(hz);
                        for (int sd = 0; sd < 2; sd++) {
                            if (r->have_wound && r->clutch_arm == sd && clutch_ok && (bact || t < r->writhe_end)) continue;   /* that hand holds the wound */
                            const int sh = sd ? SHR : SHL;
                            const v3 tg = V(r->p[sh].x + hn.x * 0.30f, r->ground + 0.05f, r->p[sh].z + hn.z * 0.30f);
                            jr_pull(r->jh, sd ? D_LAR : D_LAL, &r->p[sd ? WRR : WRL].x, &tg.x, k);
                        }
                        if (!r->hr_nhits) { r->hr_nhits = 1; RLOG("[rag] ent=%08X: v1069H braces for the fall (%.2f s)\n", r->ent, t); }
                    }
                }
                if (r->car_on) {   /* v1069C: follow the real car; let go after 6 s or 12 m */
                    float rot9[9]; v3 cp; const double ta = n - r->car_t0;
                    if (ta > 6000.0 || !car_frame(r, r->car, rot9, &cp) || len(sub(cp, r->p[PEL])) > 12.0f) { jr_car_remove(r->jh); r->car_on = 0;
                        RLOG("[rag-car] ent=%08X: car released after %.1f s\n", r->ent, ta / 1000.0); }
                    else jr_car_move(r->jh, rot9, &cp.x);
                }
                v3 prev[NP]; memcpy(prev, r->p, sizeof prev);
                jr_step(r->jh, dt);
                r->jasleep = jr_get(r->jh, r->jq, r->jx, &r->p[0].x);
                for (int k = 0; k < NP; k++) r->pp[k] = prev[k];
                r->frames++;
                float vmax = 0, vsum = 0; for (int k = 0; k < NP; k++) { const float v = len(sub(r->p[k], prev[k])) / dt; vsum += v; if (v > vmax) vmax = v; }
                r->still_ms = ((vsum / NP < 0.08f && vmax < 0.2f) || r->jasleep) ? r->still_ms + dt * 1000.0 : 0.0;
                {   /* v1057B blood: head / chest / pelvis hitting the ground hard -> a splat there; a pool under the body
                     * once it lies still (grows in over 3 s) */
                    const m3 Mb = w2mc(&r->Rd, r->conv), MbT = tr(&Mb);
                    static const int part[3] = { HED, THO, PEL };
                    for (int k = 0; k < 3 && dt > 0.0f; k++) {
                        const float vy = (r->p[part[k]].y - prev[part[k]].y) / dt;
                        if (r->bl_hits < 3 && r->bl_vy[k] < -2.2f && vy > -0.4f && r->p[part[k]].y - r->ground < 0.35f) {
                            if (blood_ground(add(r->Td, mv(&MbT, r->p[part[k]])), 0.28f + 0.04f * (-r->bl_vy[k]), 2, 0.8f, 0.15f)) r->bl_hits++;
                        }
                        r->bl_vy[k] = vy;
                    }
                    if (0 && !r->bl_pool && (r->still_ms > 300.0 || t > 2.5f) && t > 0.6f) {   /* v1057B-3: the game spawns its own pool */
                        r->bl_pool = 1;
                        const v3 c = mul(add(r->p[PEL], r->p[THO]), 0.5f);
                        blood_ground(add(r->Td, mv(&MbT, c)), r->headshot ? 0.8f : 1.05f, 2, 0.9f, 3.0f);
                        if (r->headshot) blood_ground(add(r->Td, mv(&MbT, r->p[HED])), 0.75f, 2, 0.9f, 2.5f);
                    }
                }
                if (r->frames == 30 || r->frames == 90)
                    RLOG("[rag] ent=%08X frame %d (Jolt): pelvis (%.2f %.2f %.2f) head (%.2f %.2f %.2f) vmax %.2f m/s\n", r->ent, r->frames, r->p[PEL].x, r->p[PEL].y, r->p[PEL].z,
                         r->p[HED].x, r->p[HED].y, r->p[HED].z, vmax);
                const double life = (r->have_wound && !r->headshot) ? (r->writhe_end + 1.0) * 1000.0 : 0.0;
                const double since = n - (r->t_wake > r->t0 ? r->t_wake : r->t0);   /* v1069H: a bullet wakes it */
                if ((!r->bhv_on && r->still_ms > 400.0 && n - r->t0 > life && since > (r->t_wake > r->t0 + 2500.0 ? 1500.0 : 500.0)) || since > (r->bh.prog != BHV_NONE ? 14000.0 : 8000.0)) { r->state = 4; r->bhv_on = 0; selfcoll_on(r); if (r->cad_n) { const double el = (n - r->t_wake) / 1000.0; CLOG2("[corpse] ent=%08X moved %d updates in %.2f s since the hit = %.0f/s (max gap %.0f ms), game %.0f fps\n", r->ent, r->cad_n, el, el > 0 ? r->cad_n / el : 0.0, r->cad_max, s_game_fps); r->cad_n = 0; r->cad_max = 0; } freeze_old(); RLOG("[rag] ent=%08X settled after %.1f s (%d frames, Jolt)%s\n", r->ent, (n - r->t0) / 1000.0, r->frames, r->pin_n ? " [root pinned]" : ""); rag_rest_diag(r); }
            }
            else if (r->state == 3) {
                read_anim(r);
                const float h = dt * 0.25f; for (int k = 0; k < 4; k++) sim_step(r, h); r->frames++;
                float vmax = 0, vsum = 0; for (int k = 0; k < NP; k++) { const float v = len(sub(r->p[k], r->pp[k])) / h; vsum += v; if (v > vmax) vmax = v; }
                r->still_ms = (vsum / NP < 0.08f && vmax < 0.2f) ? r->still_ms + dt * 1000.0 : 0.0;   /* nothing still falling */
                if (r->frames == 8 || r->frames == 20 || r->frames == 40 || r->frames == 80) {
                    m3 Rc; v3 Tc; if (ent_xform(r->ent, &Rc, &Tc)) {
                        const m3 Md = w2mc(&r->Rd, r->conv), MdT = tr(&Md);
                        const v3 hw = mv(&MdT, sub(r->p[HIR], r->p[HIL])), sw = mv(&MdT, sub(r->p[SHR], r->p[SHL]));
                        const m3 Mc = w2mc(&Rc, r->conv);
                        RLOG("[rag] ent=%08X t=%.2f s heading: hips %.0f shoulders %.0f deg (world) | entity facing %.0f (was %.0f) deg\n", r->ent, (n - r->t0) / 1000.0,
                             atan2f(hw.z, hw.x) * 57.3f, atan2f(sw.z, sw.x) * 57.3f, atan2f(Mc.m[2], Mc.m[0]) * 57.3f, atan2f(Md.m[2], Md.m[0]) * 57.3f); } }
                if (r->frames == 30 || r->frames == 90)
                    RLOG("[rag] ent=%08X frame %d: pelvis (%.2f %.2f %.2f) head (%.2f %.2f %.2f) vmax %.2f m/s\n", r->ent, r->frames, r->p[PEL].x, r->p[PEL].y, r->p[PEL].z,
                         r->p[HED].x, r->p[HED].y, r->p[HED].z, vmax);
                const double life = (r->have_wound && !r->headshot) ? (r->writhe_end + 1.0) * 1000.0 : 0.0;
                if ((r->still_ms > 400.0 && n - r->t0 > life) || n - r->t0 > 8000.0) { r->state = 4; RLOG("[rag] ent=%08X settled after %.1f s (%d frames)\n", r->ent, (n - r->t0) / 1000.0, r->frames); }
            }
            write_pose(r);   /* settled bodies keep being posed every frame (the game rewrites the palette each update) */
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        for (int i = 0; i < MAXR; i++) if (s_rg[i].anim == anim && s_rg[i].state && s_rg[i].state < 5) s_rg[i].state = 5;
        RLOG("[rag] fault while posing anim=%08X -> that body released\n", anim);
    }
    LeaveCriticalSection(&s_cs);
}

/* ------------------------------------------------------------------ hooks (logging + death trigger) */
static gfn o_pain_evt;
static void hk_pain_evt(ppu_context* c)   /* v1069H: PainStateMachine event / condition handler (vtable +0x10), r3 = sm */
{
    const uint32_t sm = (uint32_t)c->gpr[3];
    if (s_on && ok(sm)) { __try { EnterCriticalSection(&s_cs); pain_hit_check(sm); { const uint32_t e = rd32(sm + 0x4C); if (ok(e)) stumble_move(e); } LeaveCriticalSection(&s_cs); } __except (EXCEPTION_EXECUTE_HANDLER) { LeaveCriticalSection(&s_cs); } }
    o_pain_evt(c);
}
/* v1104V voice-event logger (user wants dialogue choices answered with fitting in-game voice lines). func_00239EDC(ent, id, flags,
 * extra) = the NPC/player VOCAL event dispatcher: id 0..0xD7 indexes the event table *(TOC-0x5EAC) (16 B: name hash, cooldown s,
 * last time, flags) -- the names are hashed and the dlg/*.str banks index lines another way, so the meaning of each id is
 * learned by watching the game: every call is logged "[vocal]" (who: PLAYER / npc + gender / ethnicity / voice bank, id, flags,
 * caller, result) + a per-id tally every 30 s. GF_VOCAL_LOG=0 off. */
static long s_voc_n[0x100], s_voc_ok[0x100];
void gf_vocal_note(uint32_t ent, uint32_t id, uint32_t fl, uint32_t ex, uint32_t lr, uint32_t r0)   /* v1104V-2: from the hand-edited func_00239EDC wrapper (its 115 callers call it directly, ppu_register_function never sees them) */
{
    static int on = -1; if (on < 0) { const char* e = getenv("GF_VOCAL_LOG"); on = !(e && *e == '0'); }
    if (!on) return;
    __try {
        const uint32_t r = r0;
        if (id < 0x100) { s_voc_n[id]++; if (r) s_voc_ok[id]++; }
        static int nl = 0;
        const uint32_t pl = player_ent();
        if (nl < 4000 && (r || ent == pl)) { nl++;   /* v1105T: only lines that PLAYED (+ every player line): refused attempts filled the cap */
            char vb[12] = "-";
            if (ok(ent)) { const uint32_t vp = rd32(ent + 0x1964); if (ok(vp)) { int L = 0; while (L < 120 && vm_base[vp + (uint32_t)L]) L++;
                if (L >= 8) { for (int k = 0; k < 8; k++) vb[k] = (char)vm_base[vp + (uint32_t)(L - 8 + k)]; vb[8] = 0; } } }
            printf("[vocal] v1104V t=%.1f %s %08X id 0x%02X flags %X extra %X from %08X -> %u | hp %.0f gender %u eth %d voice %s\n",
                   now_ms() / 1000.0, ent == pl ? "PLAYER" : "npc", ent, id, fl, ex, lr, r, ok(ent) ? rdf(ent + 0x14C8) : 0.0f,
                   ok(ent) ? (unsigned)vm_base[ent + 0x1814 + 3] : 0u, ok(ent) ? (int)rd32(ent + 0x153C) : -1, vb);
            fflush(stdout); }
        static double tl = 0; const double n = now_ms();
        if (n - tl > 30000.0) { tl = n; printf("[vocal] v1104V tally (id:calls/played):");
            for (int i = 0; i < 0x100; i++) if (s_voc_n[i]) printf(" %02X:%ld/%ld", i, s_voc_n[i], s_voc_ok[i]);
            printf("\n"); fflush(stdout); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

static void hk_pain_act(ppu_context* c)
{
    const uint32_t sm = (uint32_t)c->gpr[3], act = (uint32_t)c->gpr[7];
    if (s_on && ok(sm) && act != 29) { __try { EnterCriticalSection(&s_cs); pain_hit_check(sm); { const uint32_t e = rd32(sm + 0x4C); if (ok(e)) stumble_move(e); } LeaveCriticalSection(&s_cs); } __except (EXCEPTION_EXECUTE_HANDLER) { LeaveCriticalSection(&s_cs); } }
    if (s_on && ok(sm) && (act == 29 || act == 21 || act == 28 || act == 31)) {
        __try {
            const uint32_t ent = rd32(sm + 0x4C);
            RLOG("[rag] pain act %u ent=%08X deathType=%d hp=%.0f\n", act, ent, ok(ent) ? (int)(int8_t)vm_base[ent + 0x16E0] : -1, ok(ent) ? rdf(ent + 0x14C8) : 0.0f);
            if (act == 29 && ok(ent)) { EnterCriticalSection(&s_cs); rag_death(sm, ent); pain_hit_check(sm); LeaveCriticalSection(&s_cs); }
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    o_pain_act(c);
}
static void npc_note(ppu_context* c, const char* who)
{
    const uint32_t sm = (uint32_t)c->gpr[3], act = (uint32_t)c->gpr[7];
    if (!s_on || !ok(sm) || (act != 16 && act != 14)) return;
    __try {
        const uint32_t ent = rd32(sm + 0x50);
        int tracked = 0; for (int i = 0; i < MAXR; i++) if (s_rg[i].ent == ent && s_rg[i].state) tracked = s_rg[i].state;
        RLOG("[rag] %s act %u (%s) ent=%08X ragdoll state %d\n", who, act, act == 16 ? "dead" : "spawnCorpse", ent, tracked);
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
static void hk_npc_act(ppu_context* c)   /* v1107K: also tells gf_dialogue.c which NPCs are combatants (CombatNPCStateMachine) */
{ npc_note(c, "npc"); { extern void gf_dialogue_combat_note(uint32_t ent, uint32_t act); __try { const uint32_t sm = (uint32_t)c->gpr[3]; if (ok(sm)) gf_dialogue_combat_note(rd32(sm + 0x50), (uint32_t)c->gpr[7]); } __except (EXCEPTION_EXECUTE_HANDLER) { } } o_npc_act(c); }
static void hk_base_act(ppu_context* c) { npc_note(c, "npcbase"); o_base_act(c); }

/* a body whose skeleton stops updating: the NPC was replaced by its corpse -> wait for it (6 s); a corpse that stops
 * updating keeps the pose last written (nothing rewrites its palette) */
static DWORD WINAPI watch_thread(LPVOID arg)
{
    (void)arg;
    for (;;) {
        Sleep(250);
        EnterCriticalSection(&s_cs);
        const double n = now_ms();
        for (int i = 0; i < MAXR; i++) {
            rag_t* r = &s_rg[i];
            if (r->hr && r->state >= 1 && r->state < 5 && n - (r->last_call > 0 ? r->last_call : r->t0) > 1000.0) { rag_release(r); continue; }   /* v1069H: not animated any more */
            if ((r->state == 3 || r->state == 4) && !r->corpse && r->last_call > 0 && n - r->last_call > 1000.0) {
                RLOG("[rag] ent=%08X: skeleton not updated for 1 s -> waiting for the corpse\n", r->ent);
                r->resume_state = r->state; r->state = 6; r->wait_t0 = n;
            }
            if (r->state == 6 && n - r->wait_t0 > 6000.0) { RLOG("[rag] ent=%08X: no corpse within 6 s -> released\n", r->ent); r->state = 5; }
        }
        LeaveCriticalSection(&s_cs);
    }
}

void gf_ragdoll_init(void)
{
    const char* e = getenv("GF_RAG");
    if (e && *e == '0') { printf("[rag] v1056C off (GF_RAG=0)\n"); fflush(stdout); s_on = 0; return; }
    rig_load();   /* v1100L */
    e = getenv("GF_RAG_PHYS"); if (e && *e == '0') s_phys = 0;
    e = getenv("GF_RAG_MAX"); if (e && *e) { s_max = atoi(e); if (s_max < 1) s_max = 1; if (s_max > MAXR) s_max = MAXR; }
    e = getenv("GF_RAG_PUSH"); if (e && *e) s_push = (float)atof(e);
    e = getenv("GF_RAG_GRAV"); if (e && *e) s_grav = (float)atof(e);
    e = getenv("GF_RAG_TONE"); if (e && *e) s_tone = (float)atof(e);
    e = getenv("GF_RAG_ENGINE"); if (e && (*e == 'p' || *e == 'P')) s_jolt = 0;
    e = getenv("GF_RAG_HIT"); if (e && *e == '0') s_hit_on = 0;
    e = getenv("GF_RAG_HIT_SCALE"); if (e && *e) s_hit_scale = (float)atof(e);
    e = getenv("GF_KD_TEST"); if (e && *e) s_kd_test = atoi(e);   /* v1097K knockdown probe */
    e = getenv("GF_KD"); s_kd_on = (e && *e == '1') ? 1 : 0;   /* v1097N punch knockdowns: OFF unless GF_KD=1 (user 04:14 did not like the first try) */
    e = getenv("GF_KD_DMG"); if (e && *e) s_kd_dmg = (float)atof(e);
    e = getenv("GF_KD_CHANCE"); if (e && *e) s_kd_chance = (float)atof(e);
    e = getenv("GF_KD_LOW"); if (e && *e) s_kd_low = (float)atof(e);
    e = getenv("GF_KD_PUSH"); if (e && *e) s_kd_push = (float)atof(e);
    e = getenv("GF_KD_BLEND"); if (e && *e) { s_kd_blend = (float)atof(e); if (!(s_kd_blend >= 0.05f)) s_kd_blend = 0.6f; }
    e = getenv("GF_KD_DOWN"); if (e && *e) s_kd_down = atof(e);
    e = getenv("GF_RAG_HIT_MAX"); if (e && *e) { s_hit_max = atoi(e); if (s_hit_max < 1) s_hit_max = 1; if (s_hit_max > 8) s_hit_max = 8; }
    e = getenv("GF_RAG_BRACE"); if (e && *e == '0') s_brace = 0;
    e = getenv("GF_RAG_BHV"); if (e && *e == '1') s_bhv = 1;   /* v1080B: opt-in */
    e = getenv("GF_RAG_STUMBLE"); if (e && *e == '0') s_stumble = 0;
    e = getenv("GF_BLOOD_SPRAY"); if (e && *e == '0') s_spray = 0;
    e = getenv("GF_RAG_CAR"); if (e && *e == '0') s_carhit = 0;
    e = getenv("GF_BLOOD"); if (e && *e == '0') s_spray = 0;
    srand((unsigned)GetTickCount64());   /* v1069U: stumble rolls differ per session */
    if (!s_jolt) s_hit_on = 0;   /* hit reactions need the Jolt core */
    InitializeCriticalSection(&s_cs);
    e = getenv("GF_RAG_REC"); if (e && *e == '0') s_rec_on = 0;
    if (s_rec_on) { s_rec = fopen("rag_rec.bin", "wb");
        char full[512]; full[0] = 0; GetFullPathNameA("rag_rec.bin", sizeof full, full, NULL);
        printf("[rag] v1055C recorder: %s %s\n", full, s_rec ? "open" : "FAILED to open"); fflush(stdout); }
    o_pain_act = lookup(0x001A4E1Cu); o_npc_act = lookup(0x00148088u); o_base_act = lookup(0x0014DD08u); o_pain_evt = lookup(0x001A1AACu);
    if (!o_pain_act || !o_npc_act || !o_base_act || !o_pain_evt) { printf("[rag] v1056C: a game function is missing -- off\n"); fflush(stdout); s_on = 0; return; }
    ppu_register_function(0x001A4E1Cu, hk_pain_act);
    ppu_register_function(0x001A1AACu, hk_pain_evt);   /* v1069H */
    ppu_register_function(0x00148088u, hk_npc_act);
    ppu_register_function(0x0014DD08u, hk_base_act);
    { HANDLE h = CreateThread(NULL, 256u << 10, watch_thread, NULL, 0, NULL); if (h) CloseHandle(h); }
    printf("[rag] v1069H ready (%s physics): ragdoll deaths %s, up to %d moving bodies, push x%.2f, muscle tone x%.2f, gravity %.2f | live hit reactions %s (x%.2f, up to %d), brace %s, leg-shot stumble %s\n",
           s_jolt ? "Jolt rigid-body" : "particle", s_phys ? "ON" : "OFF (log only)", s_max, s_push, s_tone, s_grav, s_hit_on ? "ON" : "OFF", s_hit_scale, s_hit_max, s_brace ? "ON" : "OFF", s_stumble ? "ON" : "OFF");
    fflush(stdout);
}

/* v1078F exports for gf_footik.c (player foot IK) */
int gf_rag_bone_slots(uint32_t ent, uint32_t anim, int* slot21, int* nslot)
{
    static rag_t t; memset(&t, 0, sizeof t);
    t.ent = ent; t.anim = anim; t.pal = anim_pal(anim);
    if (!ok(t.pal)) return 0;
    int r = 0;
    __try { r = rag_bones(&t, ent, anim); } __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; }
    if (!r) return 0;
    for (int b = 0; b < NBN; b++) slot21[b] = t.slot[b];
    *nslot = t.nslot; return 1;
}
int gf_rag_ray(const float* o, const float* d, float maxd, float* t, float* n)
{
    v3 nn; const int h = col_ray(V(o[0], o[1], o[2]), V(d[0], d[1], d[2]), maxd, t, &nn);
    if (h && n) { n[0] = nn.x; n[1] = nn.y; n[2] = nn.z; }
    return h;
}
/* v1109F gf_sit.c: the collision triangles (9 floats each) whose XZ bounds touch the square x +-r, z +-r; returns the count */
int gf_rag_tris_near(float x, float z, float r, float* out, int max)
{
    col_load(); if (s_col.state != 1 || max <= 0) return 0;
    int n = 0, seen[512], ns = 0;
    const int cx0 = (int)floorf((x - r - s_col.x0) / s_col.cell), cx1 = (int)floorf((x + r - s_col.x0) / s_col.cell);
    const int cz0 = (int)floorf((z - r - s_col.z0) / s_col.cell), cz1 = (int)floorf((z + r - s_col.z0) / s_col.cell);
    for (int cz = cz0; cz <= cz1; cz++) for (int cx = cx0; cx <= cx1; cx++) {
        if (cx < 0 || cz < 0 || cx >= s_col.nx || cz >= s_col.nz) continue;
        const int st = s_col.cells[2 * (cz * s_col.nx + cx)], cnt = s_col.cells[2 * (cz * s_col.nx + cx) + 1];
        for (int k = 0; k < cnt && n < max; k++) {
            const int t = s_col.idx[st + k]; if (t < 0 || t >= s_col.ntri) continue;
            const float* w = s_col.tri + 9 * t;
            const float mnx = fminf(w[0], fminf(w[3], w[6])), mxx = fmaxf(w[0], fmaxf(w[3], w[6]));
            const float mnz = fminf(w[2], fminf(w[5], w[8])), mxz = fmaxf(w[2], fmaxf(w[5], w[8]));
            if (mxx < x - r || mnx > x + r || mxz < z - r || mnz > z + r) continue;
            int dup = 0; for (int q = 0; q < ns; q++) if (seen[q] == t) { dup = 1; break; }
            if (dup) continue; if (ns < 512) seen[ns++] = t;
            memcpy(out + 9 * n, w, 36); n++;
        }
    }
    return n;
}
int gf_rag_ent_frame(uint32_t ent, float* M9, float* T3)   /* model = M (w - T), the v1054Z convention */
{
    m3 R; v3 T; if (!ent_xform(ent, &R, &T)) return 0;
    const m3 M = w2mc(&R, s_conv); memcpy(M9, M.m, 36); T3[0] = T.x; T3[1] = T.y; T3[2] = T.z; return 1;
}
/* v1078F: learned-rig grouping for gf_footik.c -- each slot goes with the key slot (of nk) it moves most rigidly with
 * (lowest positional variance in that key's frame, v1055B stats). Returns the sample count, 0 = rig not learned (enough). */
int gf_rag_rig_groups(uint32_t anim, const int* key, int nk, uint8_t* out, float* var_out, int nslot)
{
    int ns = 0; const uint32_t k = rig_key(anim, &ns); skt_t* T = k ? rig_find(k) : 0;
    if (!T || T->n < 200 || T->nslot != ns || ns != nslot || ns > LS) return 0;
    for (int sl = 0; sl < ns; sl++) { int best = 0; float bv = 1e30f;
        for (int i = 0; i < nk; i++) { if (key[i] < 0 || key[i] >= ns) continue;
            const float v = sl == key[i] ? 0.0f : T->m2[sl * LS + key[i]] / (float)T->n; if (v < bv) { bv = v; best = i; } }
        out[sl] = (uint8_t)best; if (var_out) var_out[sl] = bv; }
    return T->n;
}

/* v1079S-4: a bone's index in the entity's skeleton (findBone, same table as rag_bones): b = PEL / LUM / THO / ... ; -1 = not found */
int gf_rag_bone_index(uint32_t ent, uint32_t anim, int b)
{
    static rag_t t; memset(&t, 0, sizeof t);
    if (b < 0 || b >= NBN) return -1;
    t.ent = ent; t.anim = anim; t.pal = anim_pal(anim);
    if (!ok(t.pal)) return -1;
    int r = 0;
    __try { r = rag_bones(&t, ent, anim); } __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; }
    return r ? t.idx[b] : -1;
}
