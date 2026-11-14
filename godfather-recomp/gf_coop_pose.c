/* Network co-op final-palette codec.
 *
 * The palette at *(*(anim+0x74)+8) contains 64-byte RenderWare matrices in
 * model space: right/up/at/position are float3 rows, each followed by a word
 * owned by the game.  Model-space positions are intentional: the puppet's
 * entity root places the same pose anywhere in the world.
 *
 * Wire v1 is little-endian.  The 16-byte header is "POSE", version, flags
 * (bit 0 = model space), u16 slot count, u32 rig key, u32 sender timestamp.
 * Each 13-byte slot is three i16 smallest-three quaternion values, a two-bit
 * omitted-component index, and three i16 model positions at 1/4096 metre.
 * A 74-slot pose is 978 bytes.  GF_COOP_POSE=1 enables capture, receive and
 * application; the default is off.
 */
#include "gf_coop_pose.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern uint8_t* vm_base;

enum {
    POSE_MAGIC = 0x45534F50u, /* "POSE" as little-endian u32 */
    POSE_VERSION = 1,
    POSE_MODEL_SPACE = 1,
    POSE_HEADER = 16,
    POSE_SLOT = 13,
    POSE_MAX_SLOTS = (GF_COOP_POSE_MAX_PACKET - POSE_HEADER) / POSE_SLOT
};

typedef struct { float x, y, z; } vec3;
typedef struct { float w, x, y, z; } quat;
typedef struct { float r[3], u[3], a[3], p[3]; } pose_matrix;

static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
static int s_enabled;
static SRWLOCK s_latest_lock = SRWLOCK_INIT;
static volatile LONG s_target;
/* v1059G jitter buffer: the last POSE_BUF poses (sorted by sender time). Playback runs POSE_DELAY ms behind the newest
 * sender time (mapped to our clock) and interpolates between the two poses around it, so the partner's animation plays
 * at its real speed and smoothly instead of jumping 15-20 times a second (latest-wins). */
enum { POSE_BUF = 8 };
typedef struct { uint32_t ms; int len; uint8_t d[GF_COOP_POSE_MAX_PACKET]; } pose_pkt;
static pose_pkt s_buf[POSE_BUF];
static int s_buf_n;
static double s_clk_off;      /* local ms - sender ms (minimum seen, slowly relaxed upward) */
static int s_clk_ok;
static double s_delay_ms = -1.0;
volatile long g_coop_pose_interp = 0, g_coop_pose_hold = 0;   /* v1059G stats */
uint32_t gf_coop_pose_now_ms(void)
{
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint32_t)(uint64_t)((double)c.QuadPart * 1000.0 / (double)f.QuadPart);
}

static BOOL CALLBACK pose_init(PINIT_ONCE once, PVOID param, PVOID* ctx)
{
    const char* e;
    (void)once; (void)param; (void)ctx;
    e = getenv("GF_COOP_POSE");
    s_enabled = e && *e && *e != '0';
    if (s_enabled) {
        printf("[coop-pose] GF_COOP_POSE ON: v1 model-space palette, 13 bytes/slot, v1059G jitter buffer + interpolation (GF_COOP_POSE_DELAY ms, default 80)\n");
        fflush(stdout);
    }
    return TRUE;
}

static int pose_enabled(void)
{
    InitOnceExecuteOnce(&s_once, pose_init, NULL, NULL);
    return s_enabled;
}

static int guest_range(uint32_t ea, uint32_t n)
{
    return vm_base && ea >= 0x10000u && ea < 0xF0000000u && n &&
           ea + n > ea && ea + n <= 0xF0000000u;
}

static uint16_t g_be16(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t g_be32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static float g_float(uint32_t ea)
{
    uint32_t u = g_be32(ea);
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

static void g_put_float(uint32_t ea, float f)
{
    uint32_t u;
    uint8_t* p = vm_base + ea;
    memcpy(&u, &f, sizeof u);
    p[0] = (uint8_t)(u >> 24); p[1] = (uint8_t)(u >> 16);
    p[2] = (uint8_t)(u >> 8);  p[3] = (uint8_t)u;
}

static uint16_t get_u16(const uint8_t* p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_u16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static int finite_f(float f) { return isfinite(f) && fabsf(f) < 1000000.0f; }
static float vdot(vec3 a, vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static vec3 vscale(vec3 a, float s) { vec3 r = {a.x*s, a.y*s, a.z*s}; return r; }
static vec3 vsub(vec3 a, vec3 b) { vec3 r = {a.x-b.x, a.y-b.y, a.z-b.z}; return r; }
static vec3 vcross(vec3 a, vec3 b)
{
    vec3 r = {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
    return r;
}

static int vnormal(vec3* v)
{
    float d = vdot(*v, *v);
    if (!finite_f(d) || d < 0.25f || d > 4.0f) return 0;
    *v = vscale(*v, 1.0f / sqrtf(d));
    return 1;
}

static int orthonormalize(pose_matrix* m)
{
    vec3 r = {m->r[0], m->r[1], m->r[2]};
    vec3 u = {m->u[0], m->u[1], m->u[2]};
    vec3 a;
    if (!vnormal(&r)) return 0;
    u = vsub(u, vscale(r, vdot(r, u)));
    if (!vnormal(&u)) return 0;
    a = vcross(r, u);
    if (!vnormal(&a)) return 0;
    m->r[0]=r.x; m->r[1]=r.y; m->r[2]=r.z;
    m->u[0]=u.x; m->u[1]=u.y; m->u[2]=u.z;
    m->a[0]=a.x; m->a[1]=a.y; m->a[2]=a.z;
    return 1;
}

static quat matrix_to_quat(const pose_matrix* m)
{
    const float m00=m->r[0], m01=m->r[1], m02=m->r[2];
    const float m10=m->u[0], m11=m->u[1], m12=m->u[2];
    const float m20=m->a[0], m21=m->a[1], m22=m->a[2];
    quat q;
    float t = m00 + m11 + m22;
    if (t > 0.0f) {
        float s = sqrtf(t + 1.0f) * 2.0f;
        q.w=.25f*s; q.x=(m21-m12)/s; q.y=(m02-m20)/s; q.z=(m10-m01)/s;
    } else if (m00 > m11 && m00 > m22) {
        float s = sqrtf(1.0f+m00-m11-m22)*2.0f;
        q.w=(m21-m12)/s; q.x=.25f*s; q.y=(m01+m10)/s; q.z=(m02+m20)/s;
    } else if (m11 > m22) {
        float s = sqrtf(1.0f+m11-m00-m22)*2.0f;
        q.w=(m02-m20)/s; q.x=(m01+m10)/s; q.y=.25f*s; q.z=(m12+m21)/s;
    } else {
        float s = sqrtf(1.0f+m22-m00-m11)*2.0f;
        q.w=(m10-m01)/s; q.x=(m02+m20)/s; q.y=(m12+m21)/s; q.z=.25f*s;
    }
    return q;
}

static void quat_to_matrix(quat q, pose_matrix* m)
{
    float n = sqrtf(q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z);
    float w,x,y,z;
    if (!(n > 0.000001f) || !finite_f(n)) { q.w=1.0f; q.x=q.y=q.z=0.0f; n=1.0f; }
    w=q.w/n; x=q.x/n; y=q.y/n; z=q.z/n;
    m->r[0]=1-2*(y*y+z*z); m->r[1]=2*(x*y-w*z); m->r[2]=2*(x*z+w*y);
    m->u[0]=2*(x*y+w*z); m->u[1]=1-2*(x*x+z*z); m->u[2]=2*(y*z-w*x);
    m->a[0]=2*(x*z-w*y); m->a[1]=2*(y*z+w*x); m->a[2]=1-2*(x*x+y*y);
    (void)orthonormalize(m);
}

/* v1119F (user 20:16: partner animation desync, worst when fighting): logs showed ~50 % of the partner's 30 Hz captures FAILING
 * (host 1939 sent / 1852 failed, joiner 1478 / 2074) -> ~13 Hz and more frames HELD than interpolated on the other side. One
 * degenerate bone (zero scale = a hidden prop / holstered weapon / helper, toggled a lot in fights) or one bone > 8 m out made
 * encode_slot fail and the WHOLE pose was dropped. Now: a degenerate rotation is sent as identity (the receiver keeps its own
 * per-bone scale, v1070A scalefix) and an out-of-range position is clamped. Counters for the log. */
volatile long g_pose_enc_degen = 0, g_pose_enc_clamp = 0, g_pose_cap_norig = 0, g_pose_cap_span = 0, g_pose_cap_enc = 0;
static int encode_slot(uint8_t* d, pose_matrix m)
{
    float c[4], keep[3];
    quat q;
    int omit=0, k=0;
    const float qs = 46339.5358f; /* 32767 * sqrt(2) */
    if (!finite_f(m.p[0]) || !finite_f(m.p[1]) || !finite_f(m.p[2])) return 0;
    if (!orthonormalize(&m)) {   /* v1119F: zero-scale / degenerate bone -> identity rotation, keep the position */
        for (int j=0; j<3; ++j) { m.r[j] = j == 0 ? 1.0f : 0.0f; m.u[j] = j == 1 ? 1.0f : 0.0f; m.a[j] = j == 2 ? 1.0f : 0.0f; }
        InterlockedIncrement(&g_pose_enc_degen);
    }
    q = matrix_to_quat(&m);
    c[0]=q.w; c[1]=q.x; c[2]=q.y; c[3]=q.z;
    for (int i=1; i<4; ++i) if (fabsf(c[i]) > fabsf(c[omit])) omit=i;
    if (c[omit] < 0.0f) for (int i=0; i<4; ++i) c[i] = -c[i];
    for (int i=0; i<4; ++i) if (i != omit) keep[k++] = c[i];
    for (int i=0; i<3; ++i) {
        long v = lroundf(keep[i] * qs);
        if (v < -32767) v=-32767; if (v > 32767) v=32767;
        put_u16(d+i*2, (uint16_t)(int16_t)v);
    }
    d[6] = (uint8_t)omit;
    for (int i=0; i<3; ++i) {
        long v = lroundf(m.p[i] * 4096.0f);
        if (v < -32768 || v > 32767) { v = v < 0 ? -32768 : 32767; InterlockedIncrement(&g_pose_enc_clamp); }   /* v1119F: clamp, do not drop the pose */
        put_u16(d+7+i*2, (uint16_t)(int16_t)v);
    }
    return 1;
}

/* v1059G: decode to quaternion + position (interpolation works on these) */
static int decode_slot_qp(const uint8_t* s, quat* qo, float p[3])
{
    float c[4] = {0,0,0,0}, sum=0.0f;
    int omit = s[6] & 3, k=0;
    if (s[6] & 0xFC) return 0;
    for (int i=0; i<4; ++i) if (i != omit) {
        c[i] = (float)(int16_t)get_u16(s+k*2) / 46339.5358f;
        sum += c[i]*c[i]; ++k;
    }
    if (!finite_f(sum) || sum > 1.0002f) return 0;
    c[omit] = sqrtf(fmaxf(0.0f, 1.0f-sum));
    qo->w=c[0]; qo->x=c[1]; qo->y=c[2]; qo->z=c[3];
    for (int i=0; i<3; ++i) p[i] = (float)(int16_t)get_u16(s+7+i*2) / 4096.0f;
    return 1;
}

static int decode_slot(const uint8_t* s, pose_matrix* m)
{
    float c[4] = {0,0,0,0}, sum=0.0f;
    int omit = s[6] & 3, k=0;
    quat q;
    if (s[6] & 0xFC) return 0;
    for (int i=0; i<4; ++i) if (i != omit) {
        c[i] = (float)(int16_t)get_u16(s+k*2) / 46339.5358f;
        sum += c[i]*c[i]; ++k;
    }
    if (!finite_f(sum) || sum > 1.0002f) return 0;
    c[omit] = sqrtf(fmaxf(0.0f, 1.0f-sum));
    q.w=c[0]; q.x=c[1]; q.y=c[2]; q.z=c[3];
    quat_to_matrix(q, m);
    for (int i=0; i<3; ++i) m->p[i] = (float)(int16_t)get_u16(s+7+i*2) / 4096.0f;
    return 1;
}

/* Resolve the proven animation layout and hash GUID / slot count / map. */
static uint32_t s_anim_override = 0;   /* v1058H: anim controller from the post-skel hook (the puppet has none at +0xC4) */
static int resolve_rig(uint32_t ent, uint32_t* anim_out, uint32_t* pal_out,
                       uint32_t* key_out, int* slots_out)
{
    uint32_t anim, defs, base, map, holder, pal;
    int count, index, bones, maxslot=-1;
    uint32_t h=2166136261u;
    if (!guest_range(ent, 0xC8u)) return 0;
    anim = s_anim_override ? s_anim_override : g_be32(ent+0xC4u);
    if (!guest_range(anim, 0x7Cu)) return 0;
    defs = g_be32(anim+4u);
    if (!guest_range(defs, 0x20u)) return 0;
    count = (int)g_be32(defs+0x18u);
    base = g_be32(defs+0x1Cu);
    index = (int)(int16_t)g_be16(anim+0x0Au);
    if (count <= 0 || count > 4096 || index < 0 || index >= count ||
        !guest_range(base, (uint32_t)count*20u)) return 0;
    bones = (int)g_be32(base+(uint32_t)index*20u+4u);
    map = g_be32(anim+0x78u);
    if (bones <= 0 || bones > 255 || !guest_range(map, (uint32_t)bones)) return 0;
    /* v1058G: no skeleton GUID in the key -- +0x2244 is a PLAYER field, the puppet is another class (q58t: never applied) */
    for (int i=0; i<bones; ++i) {
        int slot=vm_base[map+(uint32_t)i];
        if (slot != 0xFF && slot > maxslot) maxslot=slot;
    }
    if (maxslot < 0 || maxslot+1 > POSE_MAX_SLOTS) return 0;
    h ^= (uint32_t)(maxslot+1); h *= 16777619u;
    h ^= (uint32_t)bones; h *= 16777619u; /* delimit the map length */
    for (int i=0; i<bones; ++i) { h ^= vm_base[map+(uint32_t)i]; h *= 16777619u; }
    holder = g_be32(anim+0x74u);
    if (!guest_range(holder, 12u)) return 0;
    pal = g_be32(holder+8u);
    if (!guest_range(pal, (uint32_t)(maxslot+1)*64u)) return 0;
    *anim_out=anim; *pal_out=pal; *key_out=h ? h : 1u; *slots_out=maxslot+1;
    return 1;
}

static int wire_info(const uint8_t* in, int len, int* slots, uint32_t* key, uint32_t* ms)
{
    int n;
    if (!in || len < POSE_HEADER || len > GF_COOP_POSE_MAX_PACKET) return 0;
    if (get_u32(in) != POSE_MAGIC || in[4] != POSE_VERSION || in[5] != POSE_MODEL_SPACE) return 0;
    n = (int)get_u16(in+6);
    if (n <= 0 || n > POSE_MAX_SLOTS || len != POSE_HEADER+n*POSE_SLOT) return 0;
    *slots=n; *key=get_u32(in+8); *ms=get_u32(in+12);
    return *key != 0;
}

/* ---------------------------------------------------------------- v1070A (agent A) pose guard
 * User 2026-11-06: "if the joiner walks far away, the NPCs only they can see look kind of weird sometimes, or they're all
 * mushed together so they're like 2 cm tall". Fixed here (details: HANDOFF "v1070A"):
 *  1. STALE ANIM CONTROLLER (GF_COOP_POSE_ANIMCHK=0 = old): gf_wpose_after_anim re-applies the last palette (gf_coop_pose_rewrite,
 *     or a full re-apply) through the anim controller CACHED at the last post-skel. When the receiver's character LOD strips a proxy
 *     (> 40 m from the local focus: func_001370A4 frees model + anim controller, ent+0xC4 = 0) func_0046316C still runs for it (c4 == 0
 *     only skips the eval) -> after_anim -> the freed controller's palette pointer -> 4.7 KB written every frame into memory the game
 *     hands to the next NPC it loads. Now the live ent+0xC4 must still be the cached controller, else nothing is written.
 *  2. VALIDATION (GF_COOP_POSE_VALIDATE=0 = old): a pose (>= 20 slots) whose bone positions span < 0.35 m (all bones at the root =
 *     an unevaluated / identity palette = the "2 cm" NPC) or with most bones > 6 m out (garbage) is neither captured nor applied; the receiver keeps its
 *     own animation for that frame.
 *  3. ROW SCALE (GF_COOP_POSE_SCALEFIX=0 = old): the mesh scale (|row| 1.88) is taken from the receiver's palette; if that palette is
 *     itself unevaluated (collapsed / rows outside 0.5..3) the per-rig reference learned from valid palettes is used instead of 1.0.
 *     Face caches (v1061G) and live own faces (v1066N donor) are only taken from valid receiver palettes. */
static int s_v70_animchk = -1, s_v70_validate = 1, s_v70_scalefix = 1;
volatile long g_v70_rw_noctl = 0, g_v70_rw_changed = 0, g_v70_cap_coll = 0, g_v70_cap_expl = 0, g_v70_cap_nonfin = 0;
volatile long g_v70_app_coll = 0, g_v70_app_expl = 0, g_v70_app_nonfin = 0, g_v70_scale_fix = 0, g_v70_scale_noref = 0;
volatile long g_v70_face_skip = 0, g_v70_live_face_skip = 0, g_v70_recv_bad = 0;
static void v70_cfg(void)
{
    const char* e;
    if (s_v70_animchk >= 0) return;
    e = getenv("GF_COOP_POSE_ANIMCHK"); s_v70_animchk = !(e && *e == '0');
    e = getenv("GF_COOP_POSE_VALIDATE"); s_v70_validate = !(e && *e == '0');
    e = getenv("GF_COOP_POSE_SCALEFIX"); s_v70_scalefix = !(e && *e == '0');
    printf("[coop-pose] v1070A pose guard: anim-controller check %s (GF_COOP_POSE_ANIMCHK), palette validation %s (bones must span >= 0.35 m, most within 6 m of the origin, GF_COOP_POSE_VALIDATE), row-scale repair + face-cache check %s (GF_COOP_POSE_SCALEFIX)\n",
           s_v70_animchk ? "ON" : "off", s_v70_validate ? "ON" : "off", s_v70_scalefix ? "ON" : "off");
    fflush(stdout);
}
/* 0 ok, 1 collapsed (span < 0.35 m), 2 exploded (> 6 m), 3 non-finite. p0 = first position, stride in floats. */
static int v70_span_bad(const float* p0, int n, int stride)
{
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}, d2 = 0.0f; int nfar = 0;
    if (n < 20) return 0;   /* only full character rigs */
    for (int i = 0; i < n; i++) {
        const float* p = p0 + (size_t)i * (size_t)stride;
        for (int k = 0; k < 3; k++) { if (!finite_f(p[k])) return 3; if (p[k] < lo[k]) lo[k] = p[k]; if (p[k] > hi[k]) hi[k] = p[k]; }
        if (p[0]*p[0] + p[1]*p[1] + p[2]*p[2] > 36.0f) nfar++;
    }
    for (int k = 0; k < 3; k++) d2 += (hi[k] - lo[k]) * (hi[k] - lo[k]);
    if (d2 < 0.35f * 0.35f) return 1;
    /* exploded = MOST bones > 6 m from the model origin. Not the bbox: a70-9-echo showed the local player's palette (puppet stream) with
     * ONE hand-chain slot (31) legitimately 5-8.4 m out (attached prop / IK target) -- refusing that froze the partner puppet. */
    if (nfar * 2 > n) return 2;
    return 0;
}
/* 1 = ent's live anim controller (ent+0xC4) is anim. count = 1: count + honour GF_COOP_POSE_ANIMCHK (off -> always 1). */
int gf_coop_pose_anim_live(uint32_t ent, uint32_t anim, int count)
{
    uint32_t c4 = 0;
    v70_cfg();
    if (count && !s_v70_animchk) return 1;
    if (!anim || !guest_range(ent, 0xC8u)) { if (count) g_v70_rw_noctl++; return 0; }
    __try { c4 = g_be32(ent + 0xC4u); } __except (EXCEPTION_EXECUTE_HANDLER) { c4 = 0; }
    if (c4 == anim) return 1;
    if (count) { if (!c4) g_v70_rw_noctl++; else g_v70_rw_changed++; }
    return 0;
}
/* per-rig reference row scales (learned from receiver palettes that are valid) */
enum { V70_KEYS = 16 };
typedef struct { uint32_t key; int n; float sc[POSE_MAX_SLOTS][3]; } v70_ref;
static v70_ref s_v70_ref[V70_KEYS]; static unsigned s_v70_ref_i;
static float s_v70_sc[POSE_MAX_SLOTS][3];   /* main PPU thread only */
static v70_ref* v70_ref_get(uint32_t key, int n, int create)
{
    for (int i = 0; i < V70_KEYS; i++) if (s_v70_ref[i].key == key && s_v70_ref[i].n == n) return &s_v70_ref[i];
    if (!create) return 0;
    { v70_ref* r = &s_v70_ref[s_v70_ref_i++ % V70_KEYS]; r->key = key; r->n = n; return r; }
}

static int pose_capture_impl(uint32_t ent, uint8_t* out, int cap);
int gf_coop_pose_capture(uint32_t ent, uint8_t* out, int cap)
{
    if (!pose_enabled()) return 0;
    return pose_capture_impl(ent, out, cap);
}
int gf_coop_pose_capture_any(uint32_t ent, uint8_t* out, int cap)   /* v1061F world NPC poses (gf_coop_wpose.c) */
{
    return pose_capture_impl(ent, out, cap);
}
static int pose_capture_impl(uint32_t ent, uint8_t* out, int cap)
{
    uint32_t anim=0, pal=0, key=0;
    int slots=0, bytes;
    if (!out || cap <= 0) return 0;
    __try {
        if (!resolve_rig(ent, &anim, &pal, &key, &slots)) { InterlockedIncrement(&g_pose_cap_norig); return 0; }
        bytes = POSE_HEADER + slots*POSE_SLOT;
        if (bytes > cap || bytes > GF_COOP_POSE_MAX_PACKET) return 0;
        put_u32(out, POSE_MAGIC); out[4]=POSE_VERSION; out[5]=POSE_MODEL_SPACE;
        put_u16(out+6, (uint16_t)slots); put_u32(out+8, key);
        { extern uint32_t gf_coop_pose_now_ms(void); put_u32(out+12, gf_coop_pose_now_ms()); }   /* v1059G: 1 ms clock */
        {
        float pp[POSE_MAX_SLOTS][3];   /* v1070A: positions for the span check */
        for (int i=0; i<slots; ++i) {
            uint32_t b=pal+(uint32_t)i*64u;
            pose_matrix m;
            for (int j=0; j<3; ++j) {
                m.r[j]=g_float(b+(uint32_t)j*4u);
                m.u[j]=g_float(b+16u+(uint32_t)j*4u);
                m.a[j]=g_float(b+32u+(uint32_t)j*4u);
                m.p[j]=g_float(b+48u+(uint32_t)j*4u);
                pp[i][j]=m.p[j];
            }
            if (!encode_slot(out+POSE_HEADER+i*POSE_SLOT, m)) { InterlockedIncrement(&g_pose_cap_enc); return 0; }
        }
        v70_cfg();
        if (s_v70_validate) {   /* v1070A: never send an unevaluated (collapsed) or garbage palette -- the receiver keeps its own anim */
            const int bad = v70_span_bad(&pp[0][0], slots, 3);
            if (bad) { InterlockedIncrement(&g_pose_cap_span); if (bad == 1) g_v70_cap_coll++; else if (bad == 2) g_v70_cap_expl++; else g_v70_cap_nonfin++;
                {   /* v1070A diag: the first refusals in detail (which bone is where) */
                    static int nl; if (nl < 8) { int fi = 0;  /* not "far": windef.h macro */ float fd = -1.0f; nl++;
                        for (int i=0; i<slots; ++i) { const float d = pp[i][0]*pp[i][0] + pp[i][1]*pp[i][1] + pp[i][2]*pp[i][2]; if (finite_f(d) && d > fd) { fd = d; fi = i; } }
                        printf("[coop-pose] v1070A capture refused (%s): ent %08X anim %08X slots %d | slot0 (%.2f %.2f %.2f) | farthest slot %d at (%.2f %.2f %.2f) = %.2f m from the model origin\n",
                               bad == 1 ? "collapsed" : bad == 2 ? "exploded" : "non-finite", ent, anim, slots, (double)pp[0][0], (double)pp[0][1], (double)pp[0][2],
                               fi, (double)pp[fi][0], (double)pp[fi][1], (double)pp[fi][2], fd >= 0.0f ? (double)sqrtf(fd) : -1.0); fflush(stdout); } }
                return 0; }
        }
        }
        return bytes;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

volatile long g_coop_pose_rx = 0, g_coop_pose_hook = 0, g_coop_pose_hook_match = 0; volatile uint32_t g_coop_pose_last_anim = 0, g_coop_pose_tgt_anim = 0;
volatile long g_coop_pose_ok = 0, g_coop_pose_badkey = 0, g_coop_pose_norig = 0;   /* v1058G stats (gf_coop_glue.c) */
volatile float g_coop_pose_scale[3] = {0,0,0};   /* v1059B: puppet palette row scale of slots 0..2 (glue log) */
volatile uint32_t g_coop_pose_lkey = 0, g_coop_pose_wkey = 0; volatile int g_coop_pose_lslots = 0, g_coop_pose_wslots = 0;
/* v1059B: GF_COOP_POSE_DUMP=1 -> 3 dumps (5 s apart) of the puppet's own palette vs the wire pose, per slot */
static void pose_dump(uint32_t ent, uint32_t anim, uint32_t pal, const pose_matrix* w, int n)
{
    static int s_dump = -1, s_count = 0; static ULONGLONG s_next = 0;
    ULONGLONG now = GetTickCount64();
    if (s_dump < 0) { const char* e = getenv("GF_COOP_POSE_DUMP"); s_dump = e && *e && *e != '0'; }
    if (!s_dump || s_count >= 3 || now < s_next) return;
    s_count++; s_next = now + 5000;
    printf("[coop-pose] DUMP %d ent=%08X anim=%08X pal=%08X slots=%d (own = puppet palette before apply | wire = partner)\n", s_count, ent, anim, pal, n);
    for (int i=0; i<n; ++i) {
        uint32_t b=pal+(uint32_t)i*64u;
        printf("[coop-pose]  s%02d own p(%7.3f %7.3f %7.3f) r(%6.3f %6.3f %6.3f) w3 %08X | wire p(%7.3f %7.3f %7.3f) r(%6.3f %6.3f %6.3f)\n", i,
               g_float(b+48), g_float(b+52), g_float(b+56), g_float(b), g_float(b+4), g_float(b+8), g_be32(b+60),
               w[i].p[0], w[i].p[1], w[i].p[2], w[i].r[0], w[i].r[1], w[i].r[2]);
    }
    fflush(stdout);
}

static int apply_decoded(uint32_t ent, uint32_t wire_key, int wire_slots, pose_matrix* decoded)
{
    uint32_t anim=0, pal=0, local_key=0;
    int local_slots=0;
    __try {
        if (!resolve_rig(ent, &anim, &pal, &local_key, &local_slots)) { g_coop_pose_norig++; return 0; }
        if (local_key != wire_key || local_slots != wire_slots) { g_coop_pose_badkey++; g_coop_pose_lkey = local_key; g_coop_pose_wkey = wire_key;
            g_coop_pose_lslots = local_slots; g_coop_pose_wslots = wire_slots; return 0; }
        v70_cfg();
        if (s_v70_validate) {   /* v1070A */
            const int bad = v70_span_bad(&decoded[0].p[0], wire_slots, (int)(sizeof(pose_matrix) / sizeof(float)));
            if (bad) { if (bad == 1) g_v70_app_coll++; else if (bad == 2) g_v70_app_expl++; else g_v70_app_nonfin++; return 0; }
        }
        pose_dump(ent, anim, pal, decoded, wire_slots);   /* v1059B GF_COOP_POSE_DUMP=1 diagnostics */
        for (int i=0; i<wire_slots; ++i) {
            uint32_t b=pal+(uint32_t)i*64u;
            pose_matrix* m=&decoded[i];
            float sc[3];
            /* v1059B: the palette rows carry the MESH's scale (crew BDA2CAC1: |row| = 1.88 on every slot but 0, the player
             * 1.0). The wire is orthonormal, so keep the receiving puppet's own per-row length -- writing unit rows shrank
             * every vertex toward its bone (tiny head under the hat, long thin neck / arms: user's 2-PC screenshot). */
            for (int k=0; k<3; ++k) {
                uint32_t rb=b+(uint32_t)k*16u;
                float x=g_float(rb), y=g_float(rb+4u), z=g_float(rb+8u), l=sqrtf(x*x+y*y+z*z);
                sc[k] = (finite_f(l) && l > 0.2f && l < 5.0f) ? l : 1.0f;
            }
            if (i < 3) { g_coop_pose_scale[i] = sc[0]; }
            for (int j=0; j<3; ++j) {
                g_put_float(b+(uint32_t)j*4u, m->r[j]*sc[0]);
                g_put_float(b+16u+(uint32_t)j*4u, m->u[j]*sc[1]);
                g_put_float(b+32u+(uint32_t)j*4u, m->a[j]*sc[2]);
                g_put_float(b+48u+(uint32_t)j*4u, m->p[j]);
            }
        }
        g_coop_pose_ok++;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

int gf_coop_pose_apply(uint32_t ent, const uint8_t* in, int len)
{
    pose_matrix decoded[POSE_MAX_SLOTS];
    uint32_t wire_key=0, ms=0;
    int wire_slots=0;
    if (!pose_enabled()) return 0;
    if (!ent || (uint32_t)InterlockedCompareExchange(&s_target, 0, 0) != ent) return 0;
    __try {
        if (!wire_info(in, len, &wire_slots, &wire_key, &ms)) return 0;
        for (int i=0; i<wire_slots; ++i)
            if (!decode_slot(in+POSE_HEADER+i*POSE_SLOT, &decoded[i])) return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return apply_decoded(ent, wire_key, wire_slots, decoded);
}

void gf_coop_pose_set_target(uint32_t ent)
{
    LONG old;
    if (!pose_enabled()) return;
    old = InterlockedExchange(&s_target, (LONG)ent);
    if ((uint32_t)old != ent) {
        AcquireSRWLockExclusive(&s_latest_lock);
        s_buf_n=0;
        ReleaseSRWLockExclusive(&s_latest_lock);
    }
}

void gf_coop_pose_on_received(const uint8_t* in, int len)
{
    uint8_t packet[GF_COOP_POSE_MAX_PACKET];
    int slots=0;
    uint32_t key=0, ms=0;
    if (!pose_enabled()) return;
    __try {
        if (!wire_info(in, len, &slots, &key, &ms)) return;
        memcpy(packet, in, (size_t)len);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    (void)slots; (void)key;
    {
        const double now = (double)gf_coop_pose_now_ms();
        AcquireSRWLockExclusive(&s_latest_lock);
        /* sender clock -> ours: the smallest (local - sender) = least delayed packet; relax upward 1 ms/s for drift */
        {
            double off = now - (double)ms;
            if (!s_clk_ok || off < s_clk_off || off - s_clk_off > 2000.0) { s_clk_off = off; s_clk_ok = 1; }
            else s_clk_off += 0.03;
        }
        if (s_buf_n > 0 && (int32_t)(ms - s_buf[s_buf_n-1].ms) <= 0) {
            /* out of order / duplicate: insert in place, drop if older than everything we keep */
            int i = s_buf_n - 1;
            while (i >= 0 && (int32_t)(ms - s_buf[i].ms) < 0) i--;
            if (i >= 0 && s_buf[i].ms == ms) { ReleaseSRWLockExclusive(&s_latest_lock); return; }
            if (i < 0 && s_buf_n == POSE_BUF) { ReleaseSRWLockExclusive(&s_latest_lock); return; }
            if (s_buf_n == POSE_BUF) { memmove(&s_buf[0], &s_buf[1], sizeof(pose_pkt)*(size_t)i); }   /* drop oldest */
            else { memmove(&s_buf[i+2], &s_buf[i+1], sizeof(pose_pkt)*(size_t)(s_buf_n-1-i)); s_buf_n++; i++; }
            s_buf[i].ms = ms; s_buf[i].len = len; memcpy(s_buf[i].d, packet, (size_t)len);
        } else {
            if (s_buf_n == POSE_BUF) { memmove(&s_buf[0], &s_buf[1], sizeof(pose_pkt)*(POSE_BUF-1)); s_buf_n--; }
            s_buf[s_buf_n].ms = ms; s_buf[s_buf_n].len = len; memcpy(s_buf[s_buf_n].d, packet, (size_t)len); s_buf_n++;
        }
        g_coop_pose_rx++;
        ReleaseSRWLockExclusive(&s_latest_lock);
    }
}

void gf_coop_pose_post_skel(uint32_t anim)
{
    uint8_t packet[GF_COOP_POSE_MAX_PACKET];
    uint32_t ent;
    int len;
    { extern void gf_wpose_post_skel(uint32_t a); __try { gf_wpose_post_skel(anim); } __except (EXCEPTION_EXECUTE_HANDLER) { } }   /* v1061F world NPC poses */
    if (!pose_enabled()) return;
    ent=(uint32_t)InterlockedCompareExchange(&s_target, 0, 0);
    if (!ent) return;
    g_coop_pose_hook++; g_coop_pose_last_anim = anim;
    __try {
        extern uint32_t gf_rag_cur_ent(void);
        if (guest_range(ent, 0xC8u)) g_coop_pose_tgt_anim = g_be32(ent+0xC4u);
        /* v1058H: match through the entity being animated (gf_ragdoll.c, func_0046316C wrapper); +0xC4 is 0 on the puppet */
        if (gf_rag_cur_ent() != ent && !(guest_range(ent, 0xC8u) && g_be32(ent+0xC4u) == anim)) return;
        g_coop_pose_hook_match++;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    (void)packet; (void)len;
    {
        static uint8_t pa[GF_COOP_POSE_MAX_PACKET], pb[GF_COOP_POSE_MAX_PACKET];   /* main PPU thread only */
        int la = 0, lb = 0; uint32_t ma = 0, mb = 0; double t;
        if (s_delay_ms < 0.0) { const char* e = getenv("GF_COOP_POSE_DELAY"); s_delay_ms = (e && atoi(e) >= 0) ? (double)atoi(e) : 80.0; }
        AcquireSRWLockShared(&s_latest_lock);
        t = (double)gf_coop_pose_now_ms() - s_clk_off - s_delay_ms;   /* playback time on the sender's clock */
        if (s_buf_n > 0) {
            int i = 0;
            const double newest = (double)s_buf[s_buf_n-1].ms;
            if (t >= newest || s_buf_n == 1) { lb = s_buf[s_buf_n-1].len; mb = s_buf[s_buf_n-1].ms; memcpy(pb, s_buf[s_buf_n-1].d, (size_t)lb); }
            else {
                while (i + 1 < s_buf_n && (double)s_buf[i+1].ms <= t) i++;
                if ((double)s_buf[i].ms > t) { lb = s_buf[0].len; mb = s_buf[0].ms; memcpy(pb, s_buf[0].d, (size_t)lb); }
                else {
                    la = s_buf[i].len; ma = s_buf[i].ms; memcpy(pa, s_buf[i].d, (size_t)la);
                    lb = s_buf[i+1].len; mb = s_buf[i+1].ms; memcpy(pb, s_buf[i+1].d, (size_t)lb);
                }
            }
        }
        ReleaseSRWLockShared(&s_latest_lock);
        if (lb > 0) {
            pose_matrix out[POSE_MAX_SLOTS];
            int sa=0, sb=0, ok=1; uint32_t ka=0, kb=0, xa=0, xb=0;
            if (!wire_info(pb, lb, &sb, &kb, &xb)) ok = 0;
            if (ok && la > 0 && (!wire_info(pa, la, &sa, &ka, &xa) || sa != sb || ka != kb)) la = 0;
            if (ok) {
                const float f = (la > 0 && mb != ma) ? (float)((t - (double)ma) / (double)(int32_t)(mb - ma)) : 1.0f;
                const float u = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
                for (int s=0; s<sb && ok; ++s) {
                    quat qb, qa; float pbp[3], pap[3];
                    if (!decode_slot_qp(pb+POSE_HEADER+s*POSE_SLOT, &qb, pbp)) { ok = 0; break; }
                    if (la > 0) {
                        if (!decode_slot_qp(pa+POSE_HEADER+s*POSE_SLOT, &qa, pap)) { ok = 0; break; }
                        { float d = qa.w*qb.w + qa.x*qb.x + qa.y*qb.y + qa.z*qb.z;
                          if (d < 0.0f) { qb.w=-qb.w; qb.x=-qb.x; qb.y=-qb.y; qb.z=-qb.z; } }
                        qb.w = qa.w + (qb.w-qa.w)*u; qb.x = qa.x + (qb.x-qa.x)*u; qb.y = qa.y + (qb.y-qa.y)*u; qb.z = qa.z + (qb.z-qa.z)*u;
                        for (int k=0; k<3; ++k) pbp[k] = pap[k] + (pbp[k]-pap[k])*u;
                    }
                    quat_to_matrix(qb, &out[s]);   /* normalises (nlerp) */
                    out[s].p[0]=pbp[0]; out[s].p[1]=pbp[1]; out[s].p[2]=pbp[2];
                }
                if (ok) {
                    if (la > 0) g_coop_pose_interp++; else g_coop_pose_hold++;
                    s_anim_override = anim; (void)apply_decoded(ent, kb, sb, out); s_anim_override = 0;
                }
            }
        }
    }
}

int gf_coop_pose_selftest(void)
{
    uint8_t packet[POSE_HEADER+74*POSE_SLOT];
    pose_matrix src[74], dst[74];
    float rw[16];
    float max_pos=0.0f, max_deg=0.0f;
    int padding_ok=1;
    memset(packet, 0, sizeof packet);
    put_u32(packet, POSE_MAGIC); packet[4]=POSE_VERSION; packet[5]=POSE_MODEL_SPACE;
    put_u16(packet+6, 74); put_u32(packet+8, 0x12345678u); put_u32(packet+12, 1u);
    for (int i=0; i<74; ++i) {
        float ax=0.31f+(float)(i%5), ay=0.73f+(float)(i%7), az=1.11f+(float)(i%3);
        float al=sqrtf(ax*ax+ay*ay+az*az), ang=(float)i*0.071f-2.4f;
        quat q={cosf(ang*.5f), ax/al*sinf(ang*.5f), ay/al*sinf(ang*.5f), az/al*sinf(ang*.5f)};
        quat_to_matrix(q, &src[i]);
        src[i].p[0]=(float)(i%11)*0.137f-0.65f;
        src[i].p[1]=(float)i*0.028f-0.15f;
        src[i].p[2]=(float)(i%9)*-0.119f+0.47f;
        if (!encode_slot(packet+POSE_HEADER+i*POSE_SLOT, src[i]) ||
            !decode_slot(packet+POSE_HEADER+i*POSE_SLOT, &dst[i])) return 0;
    }
    for (int i=0; i<74; ++i) {
        float dx=src[i].p[0]-dst[i].p[0], dy=src[i].p[1]-dst[i].p[1], dz=src[i].p[2]-dst[i].p[2];
        float pe=sqrtf(dx*dx+dy*dy+dz*dz), trace=0.0f, c, deg;
        for (int r=0; r<3; ++r) for (int k=0; k<3; ++k) {
            const float* sr = r==0 ? src[i].r : (r==1 ? src[i].u : src[i].a);
            const float* dr = r==0 ? dst[i].r : (r==1 ? dst[i].u : dst[i].a);
            trace += sr[k]*dr[k];
        }
        c=fmaxf(-1.0f, fminf(1.0f, (trace-1.0f)*.5f));
        deg=acosf(c)*57.2957795f;
        if (pe>max_pos) max_pos=pe; if (deg>max_deg) max_deg=deg;
    }
    /* Mirror the live word layout and prove the four game-owned words survive. */
    for (int i=0; i<16; ++i) rw[i]=12345.0f;
    for (int j=0; j<3; ++j) { rw[j]=dst[0].r[j]; rw[4+j]=dst[0].u[j]; rw[8+j]=dst[0].a[j]; rw[12+j]=dst[0].p[j]; }
    padding_ok = rw[3]==12345.0f && rw[7]==12345.0f && rw[11]==12345.0f && rw[15]==12345.0f;
    printf("[coop-pose] selftest: 74 slots, %u bytes, max position %.3f mm, max angle %.4f deg, padding %s\n",
           (unsigned)sizeof packet, max_pos*1000.0f, max_deg, padding_ok ? "untouched-by-layout" : "FAIL");
    fflush(stdout);
    return max_pos < 0.01f && max_deg < 0.5f && padding_ok;
}

/* v1061F: interpolate two wire poses (a may be NULL / la 0 = hold b) and write them into ent's palette, for the
 * world-NPC pose stream (gf_coop_wpose.c). anim = the controller from the post-skel hook. Same rig check and
 * per-row mesh-scale preservation as the puppet. Returns 1 applied, 0 failed, -1 rig mismatch. Main PPU thread. */

/* v1061G own-face preservation. NPC faces are procedural (MobFace): the face shape lives in the face-bone offsets, so
 * the host NPC's face bones on a proxy with ANOTHER face distorted it (user screenshot 2026-11-05 01:32, woman on the
 * right). 74-slot character rig (key 3DAD0F3D, we-2-diag dump): slot 43 = head (y 1.61 m), 44..63 = face (1.64-1.81 m).
 * On the first apply to an entity (its palette is still its OWN animation) each face slot is stored relative to the
 * head (rotation + offset, row scale removed); every apply then re-attaches those to the WIRE head.
 * GF_COOP_WPOSE_FACE=0 off. */
enum { FACE_HEAD = 43, FACE_LO = 44, FACE_HI = 63, FACE_N = FACE_HI - FACE_LO + 1, FACE_CACHE = 160 };
typedef struct { uint32_t ent; float rel[FACE_N][12]; } face_rel;   /* rel: rows r,u,a (orthonormal) + p, in head space */
static face_rel s_face[FACE_CACHE]; static int s_face_i; static int s_face_on = -1;
volatile long g_wpose_face_cap = 0, g_wpose_face_apply = 0;
static uint32_t s_face_wire[128];
void gf_coop_pose_face_reset(void) { memset(s_face, 0, sizeof s_face); memset(s_face_wire, 0, sizeof s_face_wire); }   /* v1063P5: + wire-face list */   /* v1061G: world change (addresses get reused) */
static void rows_unit(const float in[12], float out[12])
{
    memcpy(out, in, 12 * sizeof(float));
    for (int k = 0; k < 3; k++) { float* r = out + 3 * k; float l = sqrtf(r[0]*r[0] + r[1]*r[1] + r[2]*r[2]); if (l > 1e-6f) { r[0] /= l; r[1] /= l; r[2] /= l; } }
}
static void read_slot12(uint32_t pal, int i, float m[12])
{
    const uint32_t b = pal + (uint32_t)i * 64u;
    for (int k = 0; k < 4; k++) for (int j = 0; j < 3; j++) m[k*3 + j] = g_float(b + (uint32_t)k*16u + (uint32_t)j*4u);
}
/* row-vector convention: world = v * R + p. rel = face * inv(head): R_rel = R_f * R_h^T, p_rel = (p_f - p_h) * R_h^T */
static void rel_of(const float h[12], const float f[12], float out[12])
{
    for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) out[a*3 + b] = f[a*3]*h[b*3] + f[a*3+1]*h[b*3+1] + f[a*3+2]*h[b*3+2];
    { const float d[3] = { f[9]-h[9], f[10]-h[10], f[11]-h[11] };
      for (int b = 0; b < 3; b++) out[9 + b] = d[0]*h[b*3] + d[1]*h[b*3+1] + d[2]*h[b*3+2]; }
}
/* new = rel * head: R = R_rel * R_h, p = p_rel * R_h + p_h */
static void compose(const float rel[12], const float h[12], float out[12])
{
    for (int a = 0; a < 3; a++) for (int c = 0; c < 3; c++) out[a*3 + c] = rel[a*3]*h[c] + rel[a*3+1]*h[3+c] + rel[a*3+2]*h[6+c];
    for (int c = 0; c < 3; c++) out[9 + c] = rel[9]*h[c] + rel[10]*h[3+c] + rel[11]*h[6+c] + h[9+c];
}
/* v1063P5: proxies whose model is verified identical to the host NPC's (gf_coop_proxy.c spawn ext: same model GUID) take
 * the WIRE face bones = the host NPC's exact procedural face (+ expressions); others keep their own face (v1061G). */
static int s_face_wire_i;
void gf_coop_pose_face_wire(uint32_t ent, int on)
{
    int i;
    for (i = 0; i < 128; i++) if (s_face_wire[i] == ent) { if (!on) s_face_wire[i] = 0; return; }
    if (on && ent) s_face_wire[s_face_wire_i++ & 127] = ent;
}
volatile long g_wpose_face_wire = 0;
void gf_coop_pose_face_forget(uint32_t ent)   /* v1063P5: a proxy (re)bound to ent: its cached own face / wire flag belong to an older body */
{
    if (!ent) return;
    for (int i = 0; i < FACE_CACHE; i++) if (s_face[i].ent == ent) s_face[i].ent = 0;
    for (int i = 0; i < 128; i++) if (s_face_wire[i] == ent) s_face_wire[i] = 0;
}
static int s_v70_pal_valid = 1;   /* v1070A: set by gf_coop_pose_apply_mix before face_get (main PPU thread) */
static face_rel* face_get(uint32_t ent, uint32_t pal, int slots)
{
    if (s_face_on < 0) { const char* e = getenv("GF_COOP_WPOSE_FACE"); s_face_on = !(e && *e == '0'); }
    if (!s_face_on || slots != 74) return 0;
    { static int wf = -1; if (wf < 0) { const char* e = getenv("GF_COOP_WPOSE_WIREFACE"); wf = !(e && *e == '0'); }
      if (wf) for (int i = 0; i < 128; i++) if (s_face_wire[i] == ent) { g_wpose_face_wire++; return 0; } }
    for (int i = 0; i < FACE_CACHE; i++) if (s_face[i].ent == ent) return &s_face[i];
    if (!s_v70_pal_valid) { g_v70_face_skip++; return 0; }   /* v1070A: unevaluated palette -> no face cache yet (wire face this frame) */
    {   /* first apply to this entity: its palette is its own animation -> capture its face relative to its head */
        face_rel* c = &s_face[s_face_i++ % FACE_CACHE]; float h[12], hu[12], f[12], fu[12];
        read_slot12(pal, FACE_HEAD, h); rows_unit(h, hu);
        for (int i = 0; i < FACE_N; i++) { read_slot12(pal, FACE_LO + i, f); rows_unit(f, fu); rel_of(hu, fu, c->rel[i]); }
        c->ent = ent; g_wpose_face_cap++;
        return c;
    }
}
/* v1063P11 perf: per-entity copy of the last palette written by gf_coop_pose_apply_interp. gf_coop_wpose.c re-applies the
 * host pose after the whole anim update (v1061H) -- it used to decode + interpolate + face-compose everything a second time
 * per proxy per frame (p63-13b: the pose stream cost ~15 % of the echo frame). Now it just copies these bytes back. */
typedef struct { uint32_t ent, pal; int n; uint8_t w[POSE_MAX_SLOTS][64]; } pcache;
enum { PCACHE_N = 64 };
static pcache s_pc[PCACHE_N]; static unsigned s_pc_i;
static pcache* pc_get(uint32_t ent, int create)
{
    for (int i = 0; i < PCACHE_N; i++) if (s_pc[i].ent == ent) return &s_pc[i];
    if (!create) return 0;
    { pcache* c = &s_pc[s_pc_i++ % PCACHE_N]; c->ent = ent; c->pal = 0; c->n = 0; return c; }
}
int gf_coop_pose_rewrite(uint32_t ent, uint32_t anim)   /* 1 = cached palette written back, 0 = no cache (caller applies fully) */
{
    pcache* c = pc_get(ent, 0); uint32_t lanim = 0, lpal = 0, lkey = 0; int lslots = 0, r;
    if (!c || !c->n || !c->pal) return 0;
    if (!gf_coop_pose_anim_live(ent, anim, 1)) { c->n = 0; return 0; }   /* v1070A: anim controller freed / replaced -> never write through it */
    __try {
        s_anim_override = anim; r = resolve_rig(ent, &lanim, &lpal, &lkey, &lslots); s_anim_override = 0;
        if (!r || lpal != c->pal || lslots != c->n) { c->n = 0; return 0; }
        for (int i = 0; i < c->n; i++) memcpy(vm_base + lpal + (uint32_t)i * 64u, c->w[i], 64);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { s_anim_override = 0; c->n = 0; return 0; }
}
void gf_coop_pose_cache_forget(uint32_t ent) { pcache* c = pc_get(ent, 0); if (c) { c->ent = 0; c->n = 0; } }
/* v1066N (agent N): decode one wire pair (pa may be NULL = hold pb) at u into normalised quaternions + positions.
 * u in [0, umax]: u > 1 = linear extrapolation past pb (the guest's short-gap extrapolation instead of a frozen hold). */
static int decode_pair(const gf_pose_pair* sp, quat* q, float (*p)[3], int* slots, uint32_t* key, float umax)
{
    int sa = 0, sb = 0, la; uint32_t ka = 0, kb = 0, xa = 0, xb = 0; float u;
    if (!sp || !sp->pb || !wire_info(sp->pb, sp->lb, &sb, &kb, &xb)) return 0;
    la = sp->la;
    if (sp->pa && la > 0 && (!wire_info(sp->pa, la, &sa, &ka, &xa) || sa != sb || ka != kb)) la = 0;
    if (!sp->pa) la = 0;
    u = sp->u; if (!(u >= 0.0f)) u = 0.0f; if (u > umax) u = umax;
    for (int s = 0; s < sb; ++s) {
        quat qb, qa; float pbp[3], pap[3], n;
        if (!decode_slot_qp(sp->pb + POSE_HEADER + s*POSE_SLOT, &qb, pbp)) return 0;
        if (la > 0) {
            if (!decode_slot_qp(sp->pa + POSE_HEADER + s*POSE_SLOT, &qa, pap)) return 0;
            { float d = qa.w*qb.w + qa.x*qb.x + qa.y*qb.y + qa.z*qb.z;
              if (d < 0.0f) { qb.w=-qb.w; qb.x=-qb.x; qb.y=-qb.y; qb.z=-qb.z; } }
            qb.w = qa.w + (qb.w-qa.w)*u; qb.x = qa.x + (qb.x-qa.x)*u; qb.y = qa.y + (qb.y-qa.y)*u; qb.z = qa.z + (qb.z-qa.z)*u;
            for (int k=0; k<3; ++k) pbp[k] = pap[k] + (pbp[k]-pap[k])*u;
        }
        n = sqrtf(qb.w*qb.w + qb.x*qb.x + qb.y*qb.y + qb.z*qb.z);
        if (!(n > 1e-6f) || !finite_f(n)) { qb.w = 1.0f; qb.x = qb.y = qb.z = 0.0f; n = 1.0f; }
        q[s].w = qb.w/n; q[s].x = qb.x/n; q[s].y = qb.y/n; q[s].z = qb.z/n;
        for (int k=0; k<3; ++k) { if (!finite_f(pbp[k])) return 0; p[s][k] = pbp[k]; }
    }
    *slots = sb; *key = kb;
    return 1;
}
int gf_coop_pose_rig_key(uint32_t ent, uint32_t anim, uint32_t* key, int* slots)   /* v1066N: the receiver's rig key (donor match) */
{
    uint32_t lanim = 0, lpal = 0, lkey = 0; int lslots = 0, r = 0;
    __try { s_anim_override = anim; r = resolve_rig(ent, &lanim, &lpal, &lkey, &lslots); } __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; }
    s_anim_override = 0;
    if (!r) return 0;
    if (key) *key = lkey; if (slots) *slots = lslots;
    return 1;
}
uint32_t gf_coop_pose_wire_key(const uint8_t* in, int len)   /* v1066N: rig key of a wire pose (0 = invalid) */
{
    int n = 0; uint32_t k = 0, ms = 0;
    return wire_info(in, len, &n, &k, &ms) ? k : 0;
}
volatile long g_wpose_face_live = 0;   /* v1066N */
int gf_coop_pose_apply_interp(uint32_t ent, uint32_t anim, const uint8_t* pa, int la, const uint8_t* pb, int lb, float u)
{
    gf_pose_pair s0; s0.pa = pa; s0.la = la; s0.pb = pb; s0.lb = lb; s0.u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
    return gf_coop_pose_apply_mix(ent, anim, &s0, NULL, 0.0f, 0);
}
/* v1066N: apply s0 (blended with s1 by w, s1 may be NULL) to ent. own_face_live = keep the face bones the receiver's own
 * animation put in its palette this frame (relative to its own head) on the applied head -- used when the pose comes from
 * ANOTHER NPC (donor fallback), so a donor never brings its face shape. Returns 1 applied, 0 failed, -1 rig mismatch. */
int gf_coop_pose_apply_mix(uint32_t ent, uint32_t anim, const gf_pose_pair* s0, const gf_pose_pair* s1, float w, int own_face_live)
{
    pose_matrix out[POSE_MAX_SLOTS];
    static quat q0[POSE_MAX_SLOTS], q1[POSE_MAX_SLOTS]; static float p0[POSE_MAX_SLOTS][3], p1[POSE_MAX_SLOTS][3];   /* main PPU thread only */
    int sb = 0, s1n = 0, r; uint32_t kb = 0, k1 = 0, lanim=0, lpal=0, lkey=0; int lslots=0;
    if (!decode_pair(s0, q0, p0, &sb, &kb, 2.0f)) return 0;
    if (s1 && w > 0.0f) { if (!decode_pair(s1, q1, p1, &s1n, &k1, 2.0f) || s1n != sb || k1 != kb) w = 0.0f; } else w = 0.0f;
    v70_cfg();
    if (s_v70_validate) {   /* v1070A: reject a collapsed / garbage wire pose (receiver keeps its own anim this frame) */
        int bad = v70_span_bad(&p0[0][0], sb, 3);
        if (bad) { if (bad == 1) g_v70_app_coll++; else if (bad == 2) g_v70_app_expl++; else g_v70_app_nonfin++; return 0; }
        if (w > 0.0f && (bad = v70_span_bad(&p1[0][0], sb, 3)) != 0) { if (bad == 1) g_v70_app_coll++; else if (bad == 2) g_v70_app_expl++; else g_v70_app_nonfin++; w = 0.0f; }
    }
    if (w > 1.0f) w = 1.0f;
    __try {
        s_anim_override = anim;
        r = resolve_rig(ent, &lanim, &lpal, &lkey, &lslots);
        s_anim_override = 0;
        if (!r) return 0;
        if (lkey != kb || lslots != sb) return -1;
        {   /* v1070A pre-pass: the receiver's row scales + is its palette a valid (evaluated) pose? */
            static float rp[POSE_MAX_SLOTS][3];
            int valid = 1, nbad = 0;
            for (int i=0; i<sb; ++i) {
                const uint32_t b=lpal+(uint32_t)i*64u;
                for (int k=0; k<3; ++k) {
                    const uint32_t rb=b+(uint32_t)k*16u;
                    const float x=g_float(rb), y=g_float(rb+4u), z=g_float(rb+8u), l=sqrtf(x*x+y*y+z*z);
                    s_v70_sc[i][k] = (finite_f(l) && l > 0.2f && l < 5.0f) ? l : 1.0f;   /* old rule */
                    if (!(finite_f(l) && l >= 0.5f && l <= 3.0f)) nbad++;   /* a few zero-scaled (hidden) bones are normal */
                    rp[i][k] = g_float(b+48u+(uint32_t)k*4u);
                }
            }
            if (nbad > sb || v70_span_bad(&rp[0][0], sb, 3)) valid = 0;   /* > 1/3 of the rows degenerate or all bones at one point */
            s_v70_pal_valid = valid || !s_v70_scalefix;
            if (!valid) g_v70_recv_bad++;
            if (s_v70_scalefix) {
                v70_ref* rf = v70_ref_get(lkey, sb, valid);
                if (valid && rf) memcpy(rf->sc, s_v70_sc, sizeof(float) * 3u * (size_t)sb);
                else if (!valid && rf) { memcpy(s_v70_sc, rf->sc, sizeof(float) * 3u * (size_t)sb); g_v70_scale_fix++; }
                else if (!valid) g_v70_scale_noref++;
            }
        }
        for (int s=0; s<sb; ++s) {
            quat qb = q0[s]; float pbp[3] = { p0[s][0], p0[s][1], p0[s][2] };
            if (w > 0.0f) {
                quat qc = q1[s]; float d = qb.w*qc.w + qb.x*qc.x + qb.y*qc.y + qb.z*qc.z;
                if (d < 0.0f) { qc.w=-qc.w; qc.x=-qc.x; qc.y=-qc.y; qc.z=-qc.z; }
                qb.w += (qc.w-qb.w)*w; qb.x += (qc.x-qb.x)*w; qb.y += (qc.y-qb.y)*w; qb.z += (qc.z-qb.z)*w;
                for (int k=0; k<3; ++k) pbp[k] += (p1[s][k]-pbp[k])*w;
            }
            quat_to_matrix(qb, &out[s]);
            out[s].p[0]=pbp[0]; out[s].p[1]=pbp[1]; out[s].p[2]=pbp[2];
        }
        if (own_face_live == 1 && !s_v70_pal_valid) g_v70_live_face_skip++;   /* v1070A: no live face from an unevaluated palette */
        if (own_face_live == 2) {   /* v1072X: co-op player puppet -> the sender's own face bones from the wire (its MobFace matches ours,
                                     * like gf_coop_pose_apply on the primary); a cached receiver face distorted the extra puppets */
        } else if (own_face_live == 1 && s_v70_pal_valid && sb == 74 && FACE_HI < sb) {   /* v1066N: the receiver's own face from its palette right now */
            float h[12], hu[12], f[12], fu[12], rel[12], nh[12], nf[12];
            read_slot12(lpal, FACE_HEAD, h); rows_unit(h, hu);
            for (int k = 0; k < 3; k++) { nh[k] = out[FACE_HEAD].r[k]; nh[3+k] = out[FACE_HEAD].u[k]; nh[6+k] = out[FACE_HEAD].a[k]; nh[9+k] = out[FACE_HEAD].p[k]; }
            for (int i = 0; i < FACE_N; i++) {
                pose_matrix* m = &out[FACE_LO + i];
                read_slot12(lpal, FACE_LO + i, f); rows_unit(f, fu); rel_of(hu, fu, rel); compose(rel, nh, nf);
                for (int k = 0; k < 3; k++) { m->r[k] = nf[k]; m->u[k] = nf[3+k]; m->a[k] = nf[6+k]; m->p[k] = nf[9+k]; }
            }
            g_wpose_face_live++;
        } else
        {   /* v1061G: keep the receiver's own face on the wire head */
            face_rel* fc = face_get(ent, lpal, sb);
            if (fc) {
                float h[12], nf[12];
                for (int k = 0; k < 3; k++) { h[k] = out[FACE_HEAD].r[k]; h[3+k] = out[FACE_HEAD].u[k]; h[6+k] = out[FACE_HEAD].a[k]; h[9+k] = out[FACE_HEAD].p[k]; }
                for (int i = 0; i < FACE_N; i++) {
                    pose_matrix* m = &out[FACE_LO + i];
                    compose(fc->rel[i], h, nf);
                    for (int k = 0; k < 3; k++) { m->r[k] = nf[k]; m->u[k] = nf[3+k]; m->a[k] = nf[6+k]; m->p[k] = nf[9+k]; }
                }
                g_wpose_face_apply++;
            }
        }
        {   /* v1061G GF_COOP_WPOSE_DUMP=1: 2 dumps of receiver's own palette vs the wire (face distortion RE) */
            static int dump = -1, nd = 0; static ULONGLONG nx = 0;
            if (dump < 0) { const char* e = getenv("GF_COOP_WPOSE_DUMP"); dump = e && *e && *e != '0'; }
            if (dump && nd < 3 && GetTickCount64() >= nx) {
                nd++; nx = GetTickCount64() + 7000;
                printf("[coop-wdump] %d ent=%08X slots=%d key=%08X (own palette before apply | wire)\n", nd, ent, sb, kb);
                for (int i=0; i<sb; ++i) { uint32_t b=lpal+(uint32_t)i*64u;
                    float rl = sqrtf(g_float(b)*g_float(b)+g_float(b+4)*g_float(b+4)+g_float(b+8)*g_float(b+8));
                    printf("[coop-wdump]  s%02d own p(%7.3f %7.3f %7.3f) |r| %.3f | wire p(%7.3f %7.3f %7.3f) | d %.3f\n", i,
                        g_float(b+48), g_float(b+52), g_float(b+56), rl, out[i].p[0], out[i].p[1], out[i].p[2],
                        sqrtf((g_float(b+48)-out[i].p[0])*(g_float(b+48)-out[i].p[0])+(g_float(b+52)-out[i].p[1])*(g_float(b+52)-out[i].p[1])+(g_float(b+56)-out[i].p[2])*(g_float(b+56)-out[i].p[2]))); }
                fflush(stdout);
            }
        }
        {   /* apply_decoded counts into the puppet stats; write directly with the same scale rule */
            pcache* pc = pc_get(ent, 1);   /* v1063P11: remember the written palette for the cheap re-apply after the anim update */
            if (pc) { pc->pal = lpal; pc->n = sb; }
            for (int i=0; i<sb; ++i) {
                uint32_t b=lpal+(uint32_t)i*64u; pose_matrix* m=&out[i]; float sc[3];
                for (int k=0; k<3; ++k) sc[k] = s_v70_sc[i][k];   /* v1070A: pre-pass (old rule, or the rig reference for an unevaluated palette) */
                for (int j=0; j<3; ++j) {
                    g_put_float(b+(uint32_t)j*4u, m->r[j]*sc[0]);
                    g_put_float(b+16u+(uint32_t)j*4u, m->u[j]*sc[1]);
                    g_put_float(b+32u+(uint32_t)j*4u, m->a[j]*sc[2]);
                    g_put_float(b+48u+(uint32_t)j*4u, m->p[j]);
                }
                if (pc) memcpy(pc->w[i], vm_base + b, 64);   /* raw guest bytes (already big-endian) */
            }
        }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { s_anim_override = 0; return 0; }
}
