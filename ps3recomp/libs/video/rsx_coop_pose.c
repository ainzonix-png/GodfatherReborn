/* Live networked Cole pose -- see rsx_coop_pose.h for the design. */
#include "rsx_coop_pose.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef SRWLOCK pose_lock_t;
#define POSE_LOCK_INIT SRWLOCK_INIT
static void lock_x(pose_lock_t* l) { AcquireSRWLockExclusive(l); }
static void unlock_x(pose_lock_t* l) { ReleaseSRWLockExclusive(l); }
static int  try_lock_x(pose_lock_t* l) { return TryAcquireSRWLockExclusive(l) != 0; }
static void lock_s(pose_lock_t* l) { AcquireSRWLockShared(l); }
static void unlock_s(pose_lock_t* l) { ReleaseSRWLockShared(l); }
static uint64_t pose_now_ms(void) { return (uint64_t)GetTickCount64(); }
#else
#include <pthread.h>
#include <time.h>
typedef pthread_rwlock_t pose_lock_t;
#define POSE_LOCK_INIT PTHREAD_RWLOCK_INITIALIZER
static void lock_x(pose_lock_t* l) { pthread_rwlock_wrlock(l); }
static void unlock_x(pose_lock_t* l) { pthread_rwlock_unlock(l); }
static int  try_lock_x(pose_lock_t* l) { return pthread_rwlock_trywrlock(l) == 0; }
static void lock_s(pose_lock_t* l) { pthread_rwlock_rdlock(l); }
static void unlock_s(pose_lock_t* l) { pthread_rwlock_unlock(l); }
static uint64_t pose_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
#endif

#define POSE_FLAG_NORMALS 1u
#define POSE_FLAG_TANGENT 2u
#define POSE_MAX_STREAMS  256u
#define POSE_OUT_PACKETS  320u
#define POSE_UNITS_PER_CM 10.0f        /* 1 mm quantization */
#define POSE_LIVE_MS      1500u

/* ---------------------------------------------------------------- codec */
static void wr16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v) { wr16(p, v); wr16(p + 2, v >> 16); }
static void wr64(uint8_t* p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }
static void wrf(uint8_t* p, float f) { uint32_t u; memcpy(&u, &f, 4); wr32(p, u); }
static uint32_t rd16(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t* p) { return rd16(p) | (rd16(p + 2) << 16); }
static uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
static float rdf(const uint8_t* p) { uint32_t u = rd32(p); float f; memcpy(&f, &u, 4); return f; }

static uint32_t zigzag(int32_t v) { return ((uint32_t)v << 1) ^ (uint32_t)(v >> 31); }
static int32_t unzigzag(uint32_t v) { return (int32_t)(v >> 1) ^ -(int32_t)(v & 1u); }
static size_t put_varint(uint8_t* p, uint32_t v)
{
    size_t n = 0;
    while (v >= 0x80u) { p[n++] = (uint8_t)(v | 0x80u); v >>= 7; }
    p[n++] = (uint8_t)v;
    return n;
}
static int get_varint(const uint8_t* p, size_t size, size_t* o, uint32_t* out)
{
    uint32_t v = 0;
    for (unsigned shift = 0; shift < 32; shift += 7) {
        if (*o >= size) return 0;
        const uint8_t b = p[(*o)++];
        v |= (uint32_t)(b & 0x7Fu) << shift;
        if (!(b & 0x80u)) { *out = v; return 1; }
    }
    return 0;
}

static float signf1(float v) { return v < 0.0f ? -1.0f : 1.0f; }
static void oct_encode(const float n_in[3], uint8_t out[2])
{
    float x = n_in[0], y = n_in[1], z = n_in[2];
    if (!isfinite(x) || !isfinite(y) || !isfinite(z)) { x = 0; y = 0; z = 1; }
    const float l1 = fabsf(x) + fabsf(y) + fabsf(z);
    if (l1 < 1e-8f) { x = 0; y = 0; z = 1; }
    else { x /= l1; y /= l1; z /= l1; }
    if (z < 0.0f) {
        const float ox = (1.0f - fabsf(y)) * signf1(x);
        const float oy = (1.0f - fabsf(x)) * signf1(y);
        x = ox; y = oy;
    }
    long qx = lrintf((x * 0.5f + 0.5f) * 255.0f), qy = lrintf((y * 0.5f + 0.5f) * 255.0f);
    out[0] = (uint8_t)(qx < 0 ? 0 : qx > 255 ? 255 : qx);
    out[1] = (uint8_t)(qy < 0 ? 0 : qy > 255 ? 255 : qy);
}
static void oct_decode(const uint8_t in[2], float n[3])
{
    float x = in[0] / 255.0f * 2.0f - 1.0f, y = in[1] / 255.0f * 2.0f - 1.0f;
    float z = 1.0f - fabsf(x) - fabsf(y);
    if (z < 0.0f) {
        const float ox = (1.0f - fabsf(y)) * signf1(x);
        const float oy = (1.0f - fabsf(x)) * signf1(y);
        x = ox; y = oy;
    }
    const float l = sqrtf(x * x + y * y + z * z);
    n[0] = x / l; n[1] = y / l; n[2] = z / l;
}

/* --------------------------------------------------------------- config */
static int s_cfg_loaded;
static uint32_t s_cfg_hz = 15, s_cfg_interval_ms = 66, s_cfg_tangents = 1, s_cfg_delay_ms = 140;
static int s_cfg_delay_override;
static void load_config(void)
{
    if (s_cfg_loaded) return;
    s_cfg_loaded = 1;
    const char* e = getenv("INFAMOUS_COOP_POSE_HZ");
    if (e && *e) { long v = strtol(e, NULL, 10); if (v >= 1 && v <= 60) s_cfg_hz = (uint32_t)v; }
    s_cfg_interval_ms = 1000u / s_cfg_hz;
    e = getenv("INFAMOUS_COOP_POSE_TANGENTS");
    s_cfg_tangents = !(e && *e == '0');
    s_cfg_delay_ms = s_cfg_interval_ms * 3u / 2u + 40u;
    e = getenv("INFAMOUS_COOP_POSE_DELAY_MS");
    if (e && *e) { long v = strtol(e, NULL, 10); if (v >= 0 && v <= 2000) { s_cfg_delay_ms = (uint32_t)v; s_cfg_delay_override = 1; } }
    fprintf(stderr, "[COOP-POSE] live pose stream: %u Hz, interpolation delay %u ms, tangents %s%c",
            s_cfg_hz, s_cfg_delay_ms, s_cfg_tangents ? "on" : "off", 10);
}

/* ------------------------------------------------------------- packets */
/* Blob kinds (protocol v2, little endian):
 *  0xC3 pose:  u8 kind, u8 flags, u16 n, u32 pose id, u32 sender ms,
 *              f32 anchor[3], u64 part key, u32 first guest index,
 *              u16 sender interval ms, u16 reserved               (40 bytes)
 *              then n consecutive vertices: 3 zigzag-varint position
 *              deltas (1 mm, relative to anchor), oct normal, oct tangent.
 *              A vertex the sender rejected is encoded as delta 0x7FFFFFFF.
 *  0xC2 tris:  u8 kind, u8 0, u16 n, u64 submesh, u64 part key   (20 bytes)
 *              then n triangles as zigzag-varint index deltas. */
#define KIND_POSE 0xC3u
#define KIND_TRIS 0xC2u
#define POSE_HDR  40u
#define TRIS_HDR  20u
#define POSE_VERTEX_MAX 19u            /* 3 varints (<=5 B) + 4 */
#define TRI_MAX_BYTES   15u
#define POSE_SKIP_TOKEN 0x7FFFFFFFu
#define POSE_REACH_CM   400.0f         /* no part of Cole is 4 m from his root */
/* polish r1: a vertex whose newest sample is this far behind the playback
 * time is hidden instead of frozen (a part the sender stopped streaming, e.g.
 * culled or swapped for another LOD, used to hold its last pose for a full
 * second -> limbs visibly from an older pose).  max(POSE_STALE_MS_MIN,
 * POSE_STALE_INTERVALS * sender interval). */
#define POSE_STALE_MS_MIN    250.0
#define POSE_STALE_INTERVALS 4.0
/* Samples kept per vertex.  The playback point sits ~2 sender intervals
 * behind the newest sample, so two samples were never enough to bracket it. */
#define POSE_HIST 4u
#define TRI_NEW_PACKETS_PER_FRAME 8u
#define TRI_RESEND_PACKETS_PER_FRAME 2u
/* Round 4 extension packets: KIND_TRIS with a zero triangle count (older
 * receivers drop those), byte 1 = sub-kind.
 *  MATERIALS: u8 0xC2, u8 1, u16 0, u16 n, u16 0, then n x 24 bytes
 *             { u64 submesh, u64 part key, u32 texture 0, u16 stride, u8 tangent slot, u8 0 }
 *  TEMPLATE:  u8 0xC2, u8 2, u16 0, u64 part key, u32 first guest index, u16 n,
 *             u8 slots (stride / 16), u8 tangent slot, u32 slot mask   (24 bytes)
 *             then n vertices x popcount(mask) float4 slots (static slots only)
 *  WANT:      u8 0xC2, u8 3, u16 0, u16 n, u16 0, then n x u64 part key */
#define EXT_MATERIALS 1u
#define EXT_TEMPLATE  2u
#define EXT_WANT      3u
#define MAT_HDR  8u
#define MAT_ENTRY 24u
#define TMPL_HDR 24u
#define WANT_HDR 8u
#define TMPL_PACKETS_PER_FRAME 24u      /* <= ~430 KB/s at 15 Hz, only while a part is requested */
#define MAT_INTERVAL_MS  1000u
#define WANT_INTERVAL_MS 1000u
#define TMPL_REMOTE 0x80u               /* tmpl_slots flag: static slots came from the peer */

static uint8_t  s_out_pk[2][POSE_OUT_PACKETS][RSX_COOP_POSE_BLOB_MAX];
static uint16_t s_out_len[2][POSE_OUT_PACKETS];
static uint32_t s_out_count[2];
static int s_out_back, s_out_front = 1, s_out_ready;
static pose_lock_t s_out_lock = POSE_LOCK_INIT;

/* ------------------------------------------------------------ mesh state */
typedef struct vsample {
    float pos[3];                    /* world position (anchor + delta of its own frame) */
    uint32_t id, ms;
    uint8_t nrm[2], tan[2];
    uint8_t flags, valid;
} vsample;

/* Per-vertex history, ascending by pose id (wrap-safe), n valid entries. */
typedef struct vhist {
    vsample s[POSE_HIST];
    uint8_t n;
} vhist;

typedef struct part {
    uint64_t key;
    int used, have_range;
    uint32_t lo, hi;                 /* union of known guest indices */
    uint32_t gmin, span;             /* sample storage */
    vhist* hist;
    uint32_t* ok_stamp;              /* render stamp when the vertex was drawable */
    uint64_t last_rx_ms;
    /* FIFO thread only */
    uint8_t* tmpl;
    uint8_t* tmpl_slots;             /* slots of the stored row; | TMPL_REMOTE when from the peer */
    uint32_t tmpl_gmin, tmpl_span;
    uint32_t drawn_stamp;
    /* round 4 */
    uint8_t tx_want, asked_logged, served_logged;   /* sender: the peer asked for our static slots */
    uint32_t tx_cursor;              /* sender: next guest index to stream */
    uint64_t want_sent_ms;           /* receiver (RSX thread): last request */
} part;

typedef struct submesh {
    uint64_t id, key;
    int used;
    uint32_t* tri;                   /* 3 guest indices per triangle */
    uint32_t ntri, cap;
    uint32_t* table;                 /* open addressing: triangle index + 1 */
    uint32_t tcap;
    uint32_t sent_new, resend;       /* sender cursors */
    /* round 4: material (texture 0, vertex stride, tangent slot) */
    uint32_t l_tex0, l_stride, r_tex0, r_stride;
    uint8_t l_tan, r_tan, l_have, r_have;
    uint64_t l_note_ms;
} submesh;

static part s_parts[POSE_MAX_STREAMS];
static uint32_t s_part_count;
static submesh s_subs[POSE_MAX_STREAMS * 2u];
static uint32_t s_sub_count;
static pose_lock_t s_mesh_lock = POSE_LOCK_INIT;

static part* part_find(uint64_t key, int create)
{
    for (uint32_t i = 0; i < s_part_count; ++i)
        if (s_parts[i].used && s_parts[i].key == key) return &s_parts[i];
    if (!create || s_part_count >= POSE_MAX_STREAMS) return NULL;
    part* p = &s_parts[s_part_count++];
    memset(p, 0, sizeof *p);
    p->used = 1; p->key = key;
    return p;
}

static submesh* sub_find(uint64_t id, uint64_t key, int create)
{
    for (uint32_t i = 0; i < s_sub_count; ++i)
        if (s_subs[i].used && s_subs[i].id == id) return &s_subs[i];
    if (!create || s_sub_count >= POSE_MAX_STREAMS * 2u) return NULL;
    submesh* s = &s_subs[s_sub_count++];
    memset(s, 0, sizeof *s);
    s->used = 1; s->id = id; s->key = key;
    return s;
}

static void part_note_range(part* p, uint32_t lo, uint32_t hi)
{
    if (!p->have_range) { p->lo = lo; p->hi = hi; p->have_range = 1; return; }
    if (lo < p->lo && p->hi - lo < RSX_COOP_POSE_MAX_SPAN) p->lo = lo;
    if (hi > p->hi && hi - p->lo < RSX_COOP_POSE_MAX_SPAN) p->hi = hi;
}

/* Grow sample storage to include [lo, hi]. */
static int part_cover(part* r, uint32_t lo, uint32_t hi)
{
    if (r->span && lo >= r->gmin && hi < r->gmin + r->span) return 1;
    const uint32_t nlo = r->span ? (lo < r->gmin ? lo : r->gmin) : lo;
    const uint32_t ohi = r->span ? r->gmin + r->span - 1u : hi;
    const uint32_t nhi = hi > ohi ? hi : ohi;
    if (nhi - nlo + 1u > RSX_COOP_POSE_MAX_SPAN + 1u) return 0;
    const uint32_t nspan = nhi - nlo + 1u;
    vhist* hist = (vhist*)calloc(nspan, sizeof(vhist));
    uint32_t* ok = (uint32_t*)calloc(nspan, sizeof(uint32_t));
    if (!hist || !ok) { free(hist); free(ok); return 0; }
    if (r->span) {
        memcpy(hist + (r->gmin - nlo), r->hist, (size_t)r->span * sizeof(vhist));
        memcpy(ok + (r->gmin - nlo), r->ok_stamp, (size_t)r->span * sizeof(uint32_t));
        free(r->hist); free(r->ok_stamp);
    }
    r->hist = hist; r->ok_stamp = ok; r->gmin = nlo; r->span = nspan;
    return 1;
}

static uint32_t tri_hash(uint32_t a, uint32_t b, uint32_t c)
{
    uint64_t h = (uint64_t)a * 0x9E3779B97F4A7C15ull;
    h ^= (uint64_t)b * 0xC2B2AE3D27D4EB4Full + (h >> 29);
    h ^= (uint64_t)c * 0x165667B19E3779F9ull + (h >> 31);
    return (uint32_t)(h ^ (h >> 32));
}

/* Insert keeping winding; the rotation starting at the smallest index is the
 * canonical form.  Returns 1 when new. */
static int sub_insert(submesh* s, uint32_t a, uint32_t b, uint32_t c)
{
    if (a == b || b == c || a == c) return 0;
    if (b < a && b < c) { uint32_t t = a; a = b; b = c; c = t; }
    else if (c < a && c < b) { uint32_t t = c; c = b; b = a; a = t; }
    if ((s->ntri + 1u) * 2u > s->tcap) {
        uint32_t ncap = s->tcap ? s->tcap * 2u : 4096u;
        uint32_t* table = (uint32_t*)calloc(ncap, sizeof(uint32_t));
        if (!table) return 0;
        for (uint32_t i = 0; i < s->ntri; ++i) {
            const uint32_t* t = s->tri + (size_t)i * 3u;
            uint32_t h = tri_hash(t[0], t[1], t[2]) & (ncap - 1u);
            while (table[h]) h = (h + 1u) & (ncap - 1u);
            table[h] = i + 1u;
        }
        free(s->table); s->table = table; s->tcap = ncap;
    }
    uint32_t h = tri_hash(a, b, c) & (s->tcap - 1u);
    while (s->table[h]) {
        const uint32_t* t = s->tri + (size_t)(s->table[h] - 1u) * 3u;
        if (t[0] == a && t[1] == b && t[2] == c) return 0;
        h = (h + 1u) & (s->tcap - 1u);
    }
    if (s->ntri >= s->cap) {
        uint32_t ncap = s->cap ? s->cap * 2u : 2048u;
        uint32_t* tri = (uint32_t*)realloc(s->tri, (size_t)ncap * 3u * sizeof(uint32_t));
        if (!tri) return 0;
        s->tri = tri; s->cap = ncap;
    }
    uint32_t* t = s->tri + (size_t)s->ntri * 3u;
    t[0] = a; t[1] = b; t[2] = c;
    s->table[h] = ++s->ntri;
    return 1;
}

void rsx_coop_pose_note_triangles(uint64_t id, uint64_t key, const uint32_t* list, uint32_t n)
{
    if (!id || !key || !list || n < 3u) return;
    lock_x(&s_mesh_lock);
    submesh* s = sub_find(id, key, 1);
    part* p = part_find(key, 1);
    if (s && p) {
        uint32_t lo = 0xFFFFFFFFu, hi = 0;
        for (uint32_t i = 0; i + 2u < n; i += 3u) {
            const uint32_t a = list[i], b = list[i + 1], c = list[i + 2];
            sub_insert(s, a, b, c);
            if (a < lo) lo = a; if (b < lo) lo = b; if (c < lo) lo = c;
            if (a > hi) hi = a; if (b > hi) hi = b; if (c > hi) hi = c;
        }
        if (hi >= lo && hi - lo < RSX_COOP_POSE_MAX_SPAN) part_note_range(p, lo, hi);
    }
    unlock_x(&s_mesh_lock);
}

int rsx_coop_pose_part_range(uint64_t key, uint32_t* gmin, uint32_t* count)
{
    int ok = 0;
    lock_s(&s_mesh_lock);
    const part* p = part_find(key, 0);
    if (p && p->have_range) { *gmin = p->lo; *count = p->hi - p->lo + 1u; ok = 1; }
    unlock_s(&s_mesh_lock);
    return ok;
}

static int part_template_cover(part* r, uint32_t lo, uint32_t count)
{
    const uint32_t hi = lo + count - 1u;
    if (r->tmpl && lo >= r->tmpl_gmin && hi < r->tmpl_gmin + r->tmpl_span) return 1;
    const uint32_t nlo = r->tmpl ? (lo < r->tmpl_gmin ? lo : r->tmpl_gmin) : lo;
    const uint32_t ohi = r->tmpl ? r->tmpl_gmin + r->tmpl_span - 1u : hi;
    const uint32_t nhi = hi > ohi ? hi : ohi;
    if (nhi - nlo + 1u > RSX_COOP_POSE_MAX_SPAN + 1u) return 0;
    const uint32_t nspan = nhi - nlo + 1u;
    uint8_t* tmpl = (uint8_t*)calloc(nspan, 256u);
    uint8_t* slots = (uint8_t*)calloc(nspan, 1u);
    if (!tmpl || !slots) { free(tmpl); free(slots); return 0; }
    if (r->tmpl) {
        memcpy(tmpl + (size_t)(r->tmpl_gmin - nlo) * 256u, r->tmpl, (size_t)r->tmpl_span * 256u);
        memcpy(slots + (r->tmpl_gmin - nlo), r->tmpl_slots, r->tmpl_span);
        free(r->tmpl); free(r->tmpl_slots);
    }
    r->tmpl = tmpl; r->tmpl_slots = slots; r->tmpl_gmin = nlo; r->tmpl_span = nspan;
    return 1;
}

int rsx_coop_pose_template_needed(uint64_t key, uint32_t gmin, uint32_t count)
{
    int needed = 1;
    if (!count) return 0;
    lock_s(&s_mesh_lock);
    const part* p = part_find(key, 0);
    if (p && p->tmpl && gmin >= p->tmpl_gmin && gmin + count <= p->tmpl_gmin + p->tmpl_span) {
        needed = 0;
        for (uint32_t i = 0; i < count && !needed; ++i) {
            const uint8_t s = p->tmpl_slots[gmin - p->tmpl_gmin + i];
            if (!s || (s & TMPL_REMOTE)) needed = 1;   /* local guest data replaces the peer's copy */
        }
    }
    unlock_s(&s_mesh_lock);
    return needed;
}

void rsx_coop_pose_template_store(uint64_t key, uint32_t gmin, uint32_t count,
                                  const uint8_t* vertices, uint32_t stride)
{
    if (!vertices || !count || stride < 16u || stride > 256u || (stride & 15u)) return;
    lock_x(&s_mesh_lock);
    part* p = part_find(key, 1);
    if (p && part_template_cover(p, gmin, count)) {
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t idx = gmin - p->tmpl_gmin + i;
            memcpy(p->tmpl + (size_t)idx * 256u, vertices + (size_t)i * stride, stride);
            p->tmpl_slots[idx] = (uint8_t)(stride / 16u);
        }
    }
    unlock_x(&s_mesh_lock);
}

/* --------------------------------------------------------------- sender */
static struct {
    int active, overflow_reported;
    uint64_t last_ms;
    uint32_t id, sender_ms, vertices, nparts;
    float anchor[3];
    uint64_t keys[POSE_MAX_STREAMS];
    uint32_t tri_resend_sub;
    uint32_t tmpl_budget;            /* round 4: template packets left in this pose frame */
    uint64_t last_mat_ms;
} s_tx;
static volatile uint32_t s_stat_tmpl_asked, s_stat_tmpl_served, s_stat_tmpl_stored;

static volatile uint32_t s_stat_sent_frames, s_stat_last_packets, s_stat_last_vertices;

int rsx_coop_pose_send_begin(const float anchor[3])
{
    load_config();
    if (s_tx.active) return 1;
    if (!anchor || !isfinite(anchor[0]) || !isfinite(anchor[1]) || !isfinite(anchor[2])) return 0;
    const uint64_t now = pose_now_ms();
    if (s_tx.last_ms && now - s_tx.last_ms < s_cfg_interval_ms) return 0;
    s_tx.active = 1;
    s_tx.last_ms = now;
    s_tx.id++;
    s_tx.sender_ms = (uint32_t)now;
    memcpy(s_tx.anchor, anchor, sizeof s_tx.anchor);
    s_tx.vertices = 0;
    s_tx.nparts = 0;
    s_tx.tmpl_budget = TMPL_PACKETS_PER_FRAME;
    s_out_count[s_out_back] = 0;
    return 1;
}

int rsx_coop_pose_sending(void) { return s_tx.active; }

static void read_slot(const uint8_t* vertex, uint32_t slot, float out[3])
{
    float v[4];
    memcpy(v, vertex + (size_t)slot * 16u, sizeof v);
    out[0] = v[0]; out[1] = v[1]; out[2] = v[2];
}

static uint8_t* next_packet(void)
{
    const int b = s_out_back;
    if (s_out_count[b] >= POSE_OUT_PACKETS) {
        if (!s_tx.overflow_reported) {
            s_tx.overflow_reported = 1;
            fprintf(stderr, "[COOP-POSE] pose frame exceeds %u packets; the rest is skipped%c", POSE_OUT_PACKETS, 10);
        }
        return NULL;
    }
    return s_out_pk[b][s_out_count[b]];
}
static void close_packet(size_t size) { s_out_len[s_out_back][s_out_count[s_out_back]++] = (uint16_t)size; }

/* Round 4: stream the static slots of a part the peer asked for, from the
 * vertices this pose frame already read.  Every slot that holds data goes,
 * including position / normal / tangent: the receiver overwrites their xyz
 * with the live pose but keeps w from the template (tangent sign!). */
static void send_template(part* p, uint64_t key, uint32_t gmin, uint32_t count, const uint8_t* vertices,
                          uint32_t stride, uint32_t tangent_slot)
{
    const uint32_t slots = stride / 16u;
    uint32_t mask = 0;
    for (uint32_t s = 0; s < slots && s < 32u; ++s) {
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* q = vertices + (size_t)i * stride + (size_t)s * 16u;
            int nz = 0;
            for (int b = 0; b < 16 && !nz; ++b) nz = q[b] != 0;
            if (nz) { mask |= 1u << s; break; }
        }
    }
    uint32_t nslots = 0;
    for (uint32_t s = 0; s < 32u; ++s) if (mask & (1u << s)) ++nslots;
    const uint32_t vbytes = nslots ? nslots * 16u : 1u;
    const uint32_t per_packet = (RSX_COOP_POSE_BLOB_MAX - TMPL_HDR) / vbytes;
    if (p->tx_cursor < gmin || p->tx_cursor >= gmin + count) p->tx_cursor = gmin;
    while (s_tx.tmpl_budget && p->tx_want) {
        uint8_t* pk = next_packet();
        if (!pk) { s_tx.tmpl_budget = 0; break; }
        const uint32_t first = p->tx_cursor;
        uint32_t n = gmin + count - first;
        if (n > per_packet) n = per_packet;
        if (n > 0xFFFFu) n = 0xFFFFu;
        pk[0] = (uint8_t)KIND_TRIS; pk[1] = (uint8_t)EXT_TEMPLATE; wr16(pk + 2, 0);
        wr64(pk + 4, key); wr32(pk + 12, first); wr16(pk + 16, n);
        pk[18] = (uint8_t)slots; pk[19] = (uint8_t)tangent_slot; wr32(pk + 20, mask);
        size_t o = TMPL_HDR;
        for (uint32_t i = 0; i < n; ++i) {
            const uint8_t* vx = vertices + (size_t)(first - gmin + i) * stride;
            for (uint32_t s = 0; s < 32u; ++s)
                if (mask & (1u << s)) {
                    float f[4];
                    memcpy(f, vx + (size_t)s * 16u, 16);
                    for (int k = 0; k < 4; ++k) wrf(pk + o + (size_t)k * 4u, f[k]);
                    o += 16u;
                }
        }
        close_packet(o);
        --s_tx.tmpl_budget;
        p->tx_cursor = first + n;
        if (p->tx_cursor >= gmin + count) {
            p->tx_want = 0; p->tx_cursor = gmin;
            s_stat_tmpl_served++;
            if (!p->served_logged) {
                p->served_logged = 1;
                fprintf(stderr, "[COOP-POSE] round 4: sent the peer the static vertex data of part %016llX (%u vertices)%c",
                        (unsigned long long)key, count, 10);
            }
        }
    }
}

void rsx_coop_pose_send_part(uint64_t key, uint32_t gmin, uint32_t count, const uint8_t* vertices,
                             uint32_t stride, uint32_t tangent_slot)
{
    if (!s_tx.active || !key || !vertices || !count || count > RSX_COOP_POSE_MAX_SPAN + 1u ||
        stride < 48u || (stride & 15u) || stride > 256u) return;
    if (tangent_slot * 16u + 16u > stride || !s_cfg_tangents) tangent_slot = 0;
    for (uint32_t i = 0; i < s_tx.nparts; ++i) if (s_tx.keys[i] == key) return;
    if (s_tx.nparts >= POSE_MAX_STREAMS) return;
    const int b = s_out_back;
    const uint32_t first_packet = s_out_count[b];
    const uint8_t flags = (uint8_t)(POSE_FLAG_NORMALS | (tangent_slot ? POSE_FLAG_TANGENT : 0u));
    uint32_t v = 0, sent = 0;
    while (v < count) {
        uint8_t* pk = next_packet();
        if (!pk) { s_out_count[b] = first_packet; return; }
        pk[0] = (uint8_t)KIND_POSE; pk[1] = flags;
        wr32(pk + 4, s_tx.id); wr32(pk + 8, s_tx.sender_ms);
        wrf(pk + 12, s_tx.anchor[0]); wrf(pk + 16, s_tx.anchor[1]); wrf(pk + 20, s_tx.anchor[2]);
        wr64(pk + 24, key); wr32(pk + 32, gmin + v);
        wr16(pk + 36, s_cfg_interval_ms); wr16(pk + 38, 0);
        size_t o = POSE_HDR;
        int32_t prev[3] = { 0, 0, 0 };
        uint32_t n = 0;
        while (v < count && o + POSE_VERTEX_MAX <= RSX_COOP_POSE_BLOB_MAX && n < 0xFFFFu) {
            const uint8_t* vx = vertices + (size_t)v * stride;
            float p[3], nrm[3];
            read_slot(vx, 0, p);
            int ok = 1;
            for (int k = 0; k < 3; ++k)
                if (!isfinite(p[k]) || fabsf(p[k] - s_tx.anchor[k]) > POSE_REACH_CM) ok = 0;
            if (!ok) {   /* not (yet) a skinned Cole vertex: tell the receiver to skip it */
                o += put_varint(pk + o, POSE_SKIP_TOKEN);
                ++n; ++v;
                continue;
            }
            for (int k = 0; k < 3; ++k) {
                const int32_t q = (int32_t)lrintf((p[k] - s_tx.anchor[k]) * POSE_UNITS_PER_CM);
                o += put_varint(pk + o, zigzag(q - prev[k]));
                prev[k] = q;
            }
            read_slot(vx, 2, nrm);
            oct_encode(nrm, pk + o); o += 2;
            if (tangent_slot) { read_slot(vx, tangent_slot, nrm); oct_encode(nrm, pk + o); o += 2; }
            ++n; ++v; ++sent;
        }
        wr16(pk + 2, n);
        close_packet(o);
    }
    s_tx.keys[s_tx.nparts++] = key;
    s_tx.vertices += sent;
    /* Round 4: the peer asked for this part's static slots (it never draws it). */
    if (s_tx.tmpl_budget) {
        lock_x(&s_mesh_lock);
        part* p = part_find(key, 0);
        if (p && p->tx_want) send_template(p, key, gmin, count, vertices, stride, tangent_slot);
        unlock_x(&s_mesh_lock);
    }
}

static int emit_tris(submesh* s, uint32_t* cursor, uint32_t end)
{
    uint8_t* pk = next_packet();
    if (!pk) return 0;
    pk[0] = (uint8_t)KIND_TRIS; pk[1] = 0;
    wr64(pk + 4, s->id); wr64(pk + 12, s->key);
    size_t o = TRIS_HDR;
    uint32_t n = 0, prev = 0;
    while (*cursor < end && o + TRI_MAX_BYTES <= RSX_COOP_POSE_BLOB_MAX && n < 0xFFFFu) {
        const uint32_t* t = s->tri + (size_t)(*cursor) * 3u;
        o += put_varint(pk + o, zigzag((int32_t)(t[0] - prev)));
        o += put_varint(pk + o, zigzag((int32_t)(t[1] - t[0])));
        o += put_varint(pk + o, zigzag((int32_t)(t[2] - t[1])));
        prev = t[2];
        ++n; ++*cursor;
    }
    wr16(pk + 2, n);
    close_packet(o);
    return 1;
}

/* Round 4 (RSX thread, inside a pose frame): announce our submesh materials
 * once a second, and ask the peer for the static slots of live parts we have
 * samples for but no template (we never draw that part ourselves). */
static uint64_t s_last_commit_ms;   /* receiver state, defined with it below */
static void emit_round4(void)
{
    const uint64_t now = pose_now_ms();
    lock_s(&s_mesh_lock);
    if (!s_tx.last_mat_ms || now - s_tx.last_mat_ms >= MAT_INTERVAL_MS) {
        s_tx.last_mat_ms = now;
        uint8_t* pk = NULL; size_t o = 0; uint32_t n = 0;
        for (uint32_t i = 0; i < s_sub_count; ++i) {
            const submesh* s = &s_subs[i];
            if (!s->used || !s->l_have || now - s->l_note_ms > 10000u) continue;
            if (!pk || o + MAT_ENTRY > RSX_COOP_POSE_BLOB_MAX) {
                if (pk) { wr16(pk + 4, n); close_packet(o); }
                pk = next_packet();
                if (!pk) break;
                pk[0] = (uint8_t)KIND_TRIS; pk[1] = (uint8_t)EXT_MATERIALS; wr16(pk + 2, 0); wr16(pk + 6, 0);
                o = MAT_HDR; n = 0;
            }
            wr64(pk + o, s->id); wr64(pk + o + 8, s->key); wr32(pk + o + 16, s->l_tex0);
            wr16(pk + o + 20, s->l_stride); pk[o + 22] = s->l_tan; pk[o + 23] = 0;
            o += MAT_ENTRY; ++n;
        }
        if (pk && n) { wr16(pk + 4, n); close_packet(o); }
    }
    uint64_t want[(RSX_COOP_POSE_BLOB_MAX - WANT_HDR) / 8u];
    uint32_t nw = 0;
    for (uint32_t i = 0; i < s_part_count && nw < sizeof want / sizeof want[0]; ++i) {
        part* r = &s_parts[i];
        if (!r->used || !r->span || !r->last_rx_ms || r->last_rx_ms + POSE_LIVE_MS < s_last_commit_ms) continue;
        if (r->want_sent_ms && now - r->want_sent_ms < WANT_INTERVAL_MS) continue;
        /* any vertex with a sample but no static slots at all? */
        int lacking = 0;
        for (uint32_t k = 0; k < r->span && !lacking; ++k) {
            const vhist* h = &r->hist[k];
            if (!h->n) continue;
            const uint32_t gi = r->gmin + k;
            lacking = !r->tmpl || gi < r->tmpl_gmin || gi >= r->tmpl_gmin + r->tmpl_span ||
                      !r->tmpl_slots[gi - r->tmpl_gmin];
        }
        if (!lacking) continue;
        r->want_sent_ms = now;   /* RSX-thread-only field */
        want[nw++] = r->key;
        if (!r->asked_logged) {
            r->asked_logged = 1;
            fprintf(stderr, "[COOP-POSE] round 4: peer streams part %016llX that this instance never drew -- asking it for the static vertex data%c",
                    (unsigned long long)r->key, 10);
        }
    }
    unlock_s(&s_mesh_lock);
    if (nw) {
        uint8_t* pk = next_packet();
        if (pk) {
            pk[0] = (uint8_t)KIND_TRIS; pk[1] = (uint8_t)EXT_WANT; wr16(pk + 2, 0); wr16(pk + 4, nw); wr16(pk + 6, 0);
            for (uint32_t i = 0; i < nw; ++i) wr64(pk + WANT_HDR + (size_t)i * 8u, want[i]);
            close_packet(WANT_HDR + (size_t)nw * 8u);
            s_stat_tmpl_asked += nw;
        }
    }
}

void rsx_coop_pose_send_end(void)
{
    if (!s_tx.active) return;
    /* Triangle knowledge: new triangles first, plus a slow round-robin resend
     * so a lost datagram is eventually repaired. */
    lock_s(&s_mesh_lock);
    uint32_t budget = TRI_NEW_PACKETS_PER_FRAME;
    for (uint32_t i = 0; i < s_sub_count && budget; ++i) {
        submesh* s = &s_subs[i];
        while (s->sent_new < s->ntri && budget) { if (!emit_tris(s, &s->sent_new, s->ntri)) budget = 1; --budget; }
    }
    for (uint32_t k = 0; k < TRI_RESEND_PACKETS_PER_FRAME && s_sub_count; ++k) {
        submesh* s = &s_subs[s_tx.tri_resend_sub % s_sub_count];
        if (s->resend >= s->sent_new) { s->resend = 0; s_tx.tri_resend_sub++; continue; }
        emit_tris(s, &s->resend, s->sent_new);
    }
    unlock_s(&s_mesh_lock);
    emit_round4();

    s_tx.active = 0;
    const int b = s_out_back;
    if (!s_out_count[b]) return;
    /* Never stall the renderer on the network thread. */
    if (!try_lock_x(&s_out_lock)) return;
    s_out_front = b;
    s_out_back = b ^ 1;
    s_out_ready = 1;
    unlock_x(&s_out_lock);
    s_stat_sent_frames++;
    s_stat_last_packets = s_out_count[b];
    s_stat_last_vertices = s_tx.vertices;
}

unsigned rsx_coop_pose_drain_outgoing(rsx_coop_pose_send_fn send)
{
    unsigned sent = 0;
    if (!send) return 0;
    lock_x(&s_out_lock);
    if (s_out_ready) {
        const int f = s_out_front;
        for (uint32_t i = 0; i < s_out_count[f]; ++i)
            if (send(s_out_pk[f][i], s_out_len[f][i])) ++sent;
        s_out_ready = 0;
    }
    unlock_x(&s_out_lock);
    return sent;
}

/* ------------------------------------------------------------- receiver */
#define POSE_ANCHOR_RING 32u
static int s_have_clock;
static uint32_t s_ref_sender;
static double s_clock_offset;
static uint32_t s_newest_id, s_remote_interval_ms = 66, s_clock_id;
static double s_render_t_last;
static int s_render_t_valid;
static uint32_t s_stat_received_frames;
static struct { uint32_t id, ms; float anchor[3]; } s_anchor_ring[POSE_ANCHOR_RING];
static uint32_t s_anchor_count, s_anchor_head;

static uint32_t s_dbg_out_of_range, s_dbg_not_fresh, s_dbg_ok;
static uint32_t s_render_stamp, s_render_drawn, s_render_remote, s_render_vtx, s_render_missing, s_render_tris;
static double s_render_t;
static float s_render_anchor[3];
static int s_render_live;
static volatile uint32_t s_stat_drawn, s_stat_remote, s_stat_vtx, s_stat_missing, s_stat_tris, s_stat_known_tris;

static double sender_rel(uint32_t ms) { return (double)(int32_t)(ms - s_ref_sender); }

/* Insert keeping ascending pose-id order; same id overwrites (duplicate
 * datagram), older than the whole full history is dropped. */
static void hist_insert(vhist* h, const vsample* smp)
{
    uint32_t pos = 0;
    for (uint32_t i = 0; i < h->n; ++i) {
        if (h->s[i].id == smp->id) { h->s[i] = *smp; return; }
        if ((int32_t)(h->s[i].id - smp->id) < 0) pos = i + 1u;
    }
    if (h->n == POSE_HIST) {
        if (pos == 0) return;
        memmove(&h->s[0], &h->s[1], (POSE_HIST - 1u) * sizeof(vsample));
        --h->n; --pos;
    }
    memmove(&h->s[pos + 1u], &h->s[pos], (size_t)(h->n - pos) * sizeof(vsample));
    h->s[pos] = *smp;
    ++h->n;
}

static void note_anchor(uint32_t id, uint32_t ms, const float anchor[3])
{
    if (s_anchor_count) {
        const uint32_t newest = (s_anchor_head + POSE_ANCHOR_RING - 1u) % POSE_ANCHOR_RING;
        if ((int32_t)(id - s_anchor_ring[newest].id) <= 0) return;
    }
    s_anchor_ring[s_anchor_head].id = id;
    s_anchor_ring[s_anchor_head].ms = ms;
    memcpy(s_anchor_ring[s_anchor_head].anchor, anchor, 12);
    s_anchor_head = (s_anchor_head + 1u) % POSE_ANCHOR_RING;
    if (s_anchor_count < POSE_ANCHOR_RING) ++s_anchor_count;
}

static void receive_pose(const uint8_t* data, uint32_t size, uint64_t now_ms)
{
    if (size < POSE_HDR) return;
    const uint8_t flags = data[1];
    const uint32_t n = rd16(data + 2), id = rd32(data + 4), sender_ms = rd32(data + 8);
    const float anchor[3] = { rdf(data + 12), rdf(data + 16), rdf(data + 20) };
    const uint64_t key = rd64(data + 24);
    const uint32_t first = rd32(data + 32), interval = rd16(data + 36);
    if (!key || !n || first > 0xFFFFFFFFu - n ||
        !isfinite(anchor[0]) || !isfinite(anchor[1]) || !isfinite(anchor[2])) return;

    static float rel[0x10000][3];
    static uint8_t nt[0x10000][4];
    static uint8_t ok[0x10000];
    size_t o = POSE_HDR;
    int32_t prev[3] = { 0, 0, 0 };
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t z;
        if (!get_varint(data, size, &o, &z)) return;
        if (z == POSE_SKIP_TOKEN) { ok[i] = 0; continue; }
        ok[i] = 1;
        prev[0] += unzigzag(z);
        for (int k = 1; k < 3; ++k) {
            if (!get_varint(data, size, &o, &z)) return;
            prev[k] += unzigzag(z);
        }
        for (int k = 0; k < 3; ++k) rel[i][k] = anchor[k] + (float)prev[k] * (1.0f / POSE_UNITS_PER_CM);
        nt[i][0] = nt[i][1] = nt[i][2] = nt[i][3] = 128;
        if (flags & POSE_FLAG_NORMALS) { if (o + 2u > size) return; nt[i][0] = data[o]; nt[i][1] = data[o + 1]; o += 2; }
        if (flags & POSE_FLAG_TANGENT) { if (o + 2u > size) return; nt[i][2] = data[o]; nt[i][3] = data[o + 1]; o += 2; }
    }

    lock_x(&s_mesh_lock);
    part* r = part_find(key, 1);
    if (!r || !part_cover(r, first, first + n - 1u)) { unlock_x(&s_mesh_lock); return; }
    part_note_range(r, first, first + n - 1u);
    for (uint32_t i = 0; i < n; ++i) {
        if (!ok[i]) continue;
        vsample smp;
        memcpy(smp.pos, rel[i], 12);
        smp.id = id; smp.ms = sender_ms; smp.flags = flags; smp.valid = 1;
        smp.nrm[0] = nt[i][0]; smp.nrm[1] = nt[i][1]; smp.tan[0] = nt[i][2]; smp.tan[1] = nt[i][3];
        hist_insert(&r->hist[first + i - r->gmin], &smp);
    }
    r->last_rx_ms = now_ms;
    if (!s_have_clock) { s_have_clock = 1; s_ref_sender = sender_ms; s_clock_offset = (double)now_ms; s_clock_id = id - 1u; }
    /* Clock: once per pose frame, from its first datagram.  A pose frame is a
     * burst of ~100 datagrams; feeding every one of them into the filter
     * dragged the offset up by the burst length each frame and back down on
     * the next frame (playback time jittering backwards = stutter). */
    if ((int32_t)(id - s_clock_id) > 0) {
        s_clock_id = id;
        const double sample = (double)now_ms - sender_rel(sender_ms);
        if (sample < s_clock_offset) s_clock_offset = sample;
        else s_clock_offset += (sample - s_clock_offset) * 0.05;
    }
    s_last_commit_ms = now_ms;
    note_anchor(id, sender_ms, anchor);
    if ((int32_t)(id - s_newest_id) > 0) { s_newest_id = id; s_stat_received_frames++; }
    if (interval >= 10 && interval <= 1000) s_remote_interval_ms = interval;
    unlock_x(&s_mesh_lock);
}

static void receive_tris(const uint8_t* data, uint32_t size)
{
    if (size < TRIS_HDR) return;
    const uint32_t n = rd16(data + 2);
    const uint64_t id = rd64(data + 4), key = rd64(data + 12);
    if (!id || !key || !n) return;
    static uint32_t tri[0x10000 * 3];
    size_t o = TRIS_HDR;
    uint32_t prev = 0, lo = 0xFFFFFFFFu, hi = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t z, a, b, c;
        if (!get_varint(data, size, &o, &z)) return; a = prev + (uint32_t)unzigzag(z);
        if (!get_varint(data, size, &o, &z)) return; b = a + (uint32_t)unzigzag(z);
        if (!get_varint(data, size, &o, &z)) return; c = b + (uint32_t)unzigzag(z);
        tri[i * 3] = a; tri[i * 3 + 1] = b; tri[i * 3 + 2] = c;
        prev = c;
        if (a < lo) lo = a; if (b < lo) lo = b; if (c < lo) lo = c;
        if (a > hi) hi = a; if (b > hi) hi = b; if (c > hi) hi = c;
    }
    if (hi - lo >= RSX_COOP_POSE_MAX_SPAN) return;
    lock_x(&s_mesh_lock);
    submesh* s = sub_find(id, key, 1);
    part* p = part_find(key, 1);
    if (s && p) {
        for (uint32_t i = 0; i < n; ++i) {
            /* Remote triangles are not re-sent by us: insert after the sender
             * cursor would also echo them back.  Keep them at the end and
             * advance the cursor when it already covered everything. */
            const int all_sent = s->sent_new == s->ntri;
            if (sub_insert(s, tri[i * 3], tri[i * 3 + 1], tri[i * 3 + 2]) && all_sent) s->sent_new = s->ntri;
        }
        part_note_range(p, lo, hi);
    }
    unlock_x(&s_mesh_lock);
}

/* ---- round 4 receivers (game thread) ---- */
static void receive_materials(const uint8_t* data, uint32_t size)
{
    if (size < MAT_HDR) return;
    uint32_t n = rd16(data + 4);
    if ((size_t)MAT_HDR + (size_t)n * MAT_ENTRY > size) n = (size - MAT_HDR) / MAT_ENTRY;
    lock_x(&s_mesh_lock);
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* e = data + MAT_HDR + (size_t)i * MAT_ENTRY;
        const uint64_t id = rd64(e), key = rd64(e + 8);
        const uint32_t stride = rd16(e + 20), tan = e[22];
        if (!id || !key || stride < 48u || stride > 256u || (stride & 15u) || tan * 16u + 16u > stride) continue;
        submesh* s = sub_find(id, key, 1);
        if (!s || s->key != key) continue;
        s->r_tex0 = rd32(e + 16); s->r_stride = stride; s->r_tan = (uint8_t)tan; s->r_have = 1;
    }
    unlock_x(&s_mesh_lock);
}

static void receive_template(const uint8_t* data, uint32_t size)
{
    if (size < TMPL_HDR) return;
    const uint64_t key = rd64(data + 4);
    const uint32_t first = rd32(data + 12), n = rd16(data + 16), slots = data[18], mask = rd32(data + 20);
    if (!key || !n || !slots || slots > 16u || first > 0xFFFFFFFFu - n) return;
    uint32_t nslots = 0;
    for (uint32_t s = 0; s < 32u; ++s) if (mask & (1u << s)) { if (s >= slots) return; ++nslots; }
    if ((size_t)TMPL_HDR + (size_t)n * nslots * 16u > size) return;
    lock_x(&s_mesh_lock);
    part* r = part_find(key, 1);
    if (r && part_template_cover(r, first, n)) {
        size_t o = TMPL_HDR;
        uint32_t stored = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t idx = first - r->tmpl_gmin + i;
            const uint8_t cur = r->tmpl_slots[idx];
            const int take = !cur || (cur & TMPL_REMOTE);   /* local guest data always wins */
            uint8_t* row = r->tmpl + (size_t)idx * 256u;
            if (take) memset(row, 0, 256u);
            for (uint32_t s = 0; s < 32u; ++s)
                if (mask & (1u << s)) {
                    if (take) for (int k = 0; k < 4; ++k) { const float f = rdf(data + o + (size_t)k * 4u); memcpy(row + (size_t)s * 16u + (size_t)k * 4u, &f, 4); }
                    o += 16u;
                }
            if (take) { r->tmpl_slots[idx] = (uint8_t)(slots | TMPL_REMOTE); ++stored; }
        }
        s_stat_tmpl_stored += stored;
    }
    unlock_x(&s_mesh_lock);
}

static void receive_want(const uint8_t* data, uint32_t size)
{
    if (size < WANT_HDR) return;
    uint32_t n = rd16(data + 4);
    if ((size_t)WANT_HDR + (size_t)n * 8u > size) n = (size - WANT_HDR) / 8u;
    lock_x(&s_mesh_lock);
    for (uint32_t i = 0; i < n; ++i) {
        part* p = part_find(rd64(data + WANT_HDR + (size_t)i * 8u), 0);
        if (p && !p->tx_want) { p->tx_want = 1; p->tx_cursor = 0; }
    }
    unlock_x(&s_mesh_lock);
}

void rsx_coop_pose_receive(const uint8_t* data, uint32_t size, uint64_t now_ms)
{
    if (!data || !size) return;
    if (data[0] == KIND_POSE) receive_pose(data, size, now_ms);
    else if (data[0] == KIND_TRIS) {
        if (size >= 4u && rd16(data + 2) == 0u) {     /* round 4 extension (zero triangles) */
            if (data[1] == EXT_MATERIALS) receive_materials(data, size);
            else if (data[1] == EXT_TEMPLATE) receive_template(data, size);
            else if (data[1] == EXT_WANT) receive_want(data, size);
            return;
        }
        receive_tris(data, size);
    }
}

void rsx_coop_pose_note_material(uint64_t id, uint64_t key, uint32_t tex0, uint32_t stride, uint32_t tangent_slot)
{
    if (!id || !key || stride < 48u || stride > 256u || (stride & 15u) || tangent_slot * 16u + 16u > stride) return;
    lock_x(&s_mesh_lock);
    submesh* s = sub_find(id, key, 1);
    if (s && s->key == key) {
        s->l_tex0 = tex0; s->l_stride = stride; s->l_tan = (uint8_t)tangent_slot; s->l_have = 1;
        s->l_note_ms = pose_now_ms();
    }
    unlock_x(&s_mesh_lock);
}

uint32_t rsx_coop_pose_remote_submeshes(rsx_coop_pose_submat* out, uint32_t max)
{
    uint32_t n = 0;
    if (!out || !max || !s_render_live) return 0;
    lock_s(&s_mesh_lock);
    for (uint32_t i = 0; i < s_sub_count && n < max; ++i) {
        const submesh* s = &s_subs[i];
        if (!s->used || !s->r_have || !s->ntri) continue;
        const part* r = part_find(s->key, 0);
        if (!r || !r->span || !r->last_rx_ms || r->last_rx_ms + POSE_LIVE_MS < s_last_commit_ms) continue;
        out[n].submesh = s->id; out[n].key = s->key;
        out[n].tex0 = s->r_tex0; out[n].stride = s->r_stride; out[n].tangent_slot = s->r_tan;
        ++n;
    }
    unlock_s(&s_mesh_lock);
    return n;
}

void rsx_coop_pose_template_counts(uint32_t* asked, uint32_t* served, uint32_t* stored)
{
    if (asked) *asked = s_stat_tmpl_asked;
    if (served) *served = s_stat_tmpl_served;
    if (stored) *stored = s_stat_tmpl_stored;
}

int rsx_coop_pose_remote_has_key(uint64_t key)
{
    int found = 0;
    if (!key) return 0;
    lock_s(&s_mesh_lock);
    const part* r = part_find(key, 0);
    found = r && r->span && r->last_rx_ms && r->last_rx_ms + POSE_LIVE_MS * 2u >= s_last_commit_ms;
    unlock_s(&s_mesh_lock);
    return found;
}

void rsx_coop_pose_reset(void)
{
    lock_x(&s_mesh_lock);
    for (uint32_t i = 0; i < s_part_count; ++i) {
        part* r = &s_parts[i];
        if (r->span) memset(r->hist, 0, (size_t)r->span * sizeof(vhist));
        r->last_rx_ms = 0;
        r->tx_want = 0; r->want_sent_ms = 0;
    }
    for (uint32_t i = 0; i < s_sub_count; ++i) s_subs[i].r_have = 0;
    s_have_clock = 0;
    s_render_t_valid = 0;
    s_last_commit_ms = 0;
    s_anchor_count = 0;
    s_anchor_head = 0;
    unlock_x(&s_mesh_lock);
}

int rsx_coop_pose_render_begin(void)
{
    load_config();
    const uint64_t now = pose_now_ms();
    lock_s(&s_mesh_lock);
    s_render_live = s_have_clock && s_anchor_count && now >= s_last_commit_ms &&
                    now - s_last_commit_ms <= POSE_LIVE_MS;
    uint32_t delay = s_cfg_delay_ms;
    if (!s_cfg_delay_override) delay = s_remote_interval_ms * 3u / 2u + 40u;
    s_render_t = (double)now - s_clock_offset - (double)delay;
    /* Playback time never runs backwards (clock-filter corrections are
     * absorbed by holding instead of rewinding the animation). */
    if (s_have_clock) {
        if (s_render_t_valid && s_render_t < s_render_t_last && s_render_t_last - s_render_t < 500.0)
            s_render_t = s_render_t_last;
        s_render_t_last = s_render_t; s_render_t_valid = 1;
    }
    if (s_anchor_count) {
        const uint32_t newest = (s_anchor_head + POSE_ANCHOR_RING - 1u) % POSE_ANCHOR_RING;
        memcpy(s_render_anchor, s_anchor_ring[newest].anchor, 12);
        for (uint32_t k = 1; k < s_anchor_count; ++k) {
            const uint32_t b = (s_anchor_head + POSE_ANCHOR_RING - k) % POSE_ANCHOR_RING;
            const uint32_t a = (b + POSE_ANCHOR_RING - 1u) % POSE_ANCHOR_RING;
            const double ta = sender_rel(s_anchor_ring[a].ms), tb = sender_rel(s_anchor_ring[b].ms);
            if (s_render_t >= tb) break;
            memcpy(s_render_anchor, s_anchor_ring[a].anchor, 12);
            if (s_render_t >= ta && tb > ta) {
                const float t = (float)((s_render_t - ta) / (tb - ta));
                for (int i = 0; i < 3; ++i)
                    s_render_anchor[i] = s_anchor_ring[a].anchor[i] +
                                         (s_anchor_ring[b].anchor[i] - s_anchor_ring[a].anchor[i]) * t;
                break;
            }
        }
    }
    s_render_remote = 0;
    uint32_t known = 0;
    for (uint32_t i = 0; i < s_part_count; ++i)
        if (s_parts[i].used && s_parts[i].span && s_parts[i].last_rx_ms + POSE_LIVE_MS >= s_last_commit_ms)
            ++s_render_remote;
    for (uint32_t i = 0; i < s_sub_count; ++i) known += s_subs[i].ntri;
    s_stat_known_tris = known;
    unlock_s(&s_mesh_lock);
    ++s_render_stamp;
    s_render_drawn = s_render_vtx = s_render_missing = s_render_tris = 0;
    s_dbg_out_of_range = s_dbg_not_fresh = s_dbg_ok = 0;
    return s_render_live;
}

static void lerp_oct(const uint8_t* a, const uint8_t* b, float t, float out[3])
{
    float na[3], nb[3];
    oct_decode(b, nb);
    if (!a || t >= 1.0f) { memcpy(out, nb, sizeof nb); return; }
    oct_decode(a, na);
    float x = na[0] + (nb[0] - na[0]) * t, y = na[1] + (nb[1] - na[1]) * t, z = na[2] + (nb[2] - na[2]) * t;
    float l = sqrtf(x * x + y * y + z * z);
    if (l < 1e-6f) { memcpy(out, nb, sizeof nb); return; }
    out[0] = x / l; out[1] = y / l; out[2] = z / l;
}

uint32_t rsx_coop_pose_write_part(uint64_t key, uint32_t stride, uint32_t tangent_slot,
                                  uint8_t* dst, uint32_t max_vertices, uint32_t* gmin_out)
{
    if (!s_render_live || !dst || !gmin_out || stride < 48u || stride > 256u || (stride & 15u)) return 0;
    if (tangent_slot * 16u + 16u > stride) tangent_slot = 0;
    lock_s(&s_mesh_lock);
    part* r = part_find(key, 0);
    if (!r || !r->span || !r->tmpl) { unlock_s(&s_mesh_lock); return 0; }
    /* Drawable range: samples and static template both present. */
    uint32_t lo = r->gmin > r->tmpl_gmin ? r->gmin : r->tmpl_gmin;
    uint32_t hi1 = r->gmin + r->span < r->tmpl_gmin + r->tmpl_span ? r->gmin + r->span : r->tmpl_gmin + r->tmpl_span;
    if (hi1 <= lo) { unlock_s(&s_mesh_lock); return 0; }
    uint32_t count = hi1 - lo;
    if (count > max_vertices) count = max_vertices;
    const uint32_t slots_needed = stride / 16u;
    const union { uint32_t u; float f; } qnan = { 0x7FC00000u };
    uint8_t vbuf[256];
    uint32_t drawn = 0, missing = 0;
    double stale_ms = POSE_STALE_INTERVALS * (double)s_remote_interval_ms;
    if (stale_ms < POSE_STALE_MS_MIN) stale_ms = POSE_STALE_MS_MIN;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t gi = lo + i, si = gi - r->gmin, ti = gi - r->tmpl_gmin;
        const int have_tmpl = (uint32_t)(r->tmpl_slots[ti] & ~TMPL_REMOTE) >= slots_needed;
        if (have_tmpl) memcpy(vbuf, r->tmpl + (size_t)ti * 256u, stride);
        else memset(vbuf, 0, stride);
        float* p = (float*)vbuf;
        const vhist* h = &r->hist[si];
        const vsample* c = h->n ? &h->s[h->n - 1u] : NULL;   /* newest */
        if (!have_tmpl || !c || !c->valid || s_render_t - sender_rel(c->ms) > stale_ms) {
            p[0] = p[1] = p[2] = qnan.f;
            if (have_tmpl) ++missing;
        } else {
            /* Bracket the playback time: a = newest sample at or before it,
             * c = the one after.  Past the newest: hold the newest; before the
             * oldest: hold the oldest. */
            const vsample* a = NULL;
            float t = 1.0f;
            if (s_render_t < sender_rel(h->s[0].ms)) c = &h->s[0];
            else {
                for (uint32_t k = h->n; k-- > 1u;) {
                    if (s_render_t >= sender_rel(h->s[k - 1u].ms) && s_render_t < sender_rel(h->s[k].ms)) {
                        a = &h->s[k - 1u]; c = &h->s[k];
                        break;
                    }
                }
            }
            const int use_a = a && (uint32_t)(c->id - a->id) <= 8u;
            if (use_a) {
                const double ta = sender_rel(a->ms), tb = sender_rel(c->ms);
                t = tb > ta ? (float)((s_render_t - ta) / (tb - ta)) : 1.0f;
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
            }
            for (int k = 0; k < 3; ++k)
                p[k] = use_a ? a->pos[k] + (c->pos[k] - a->pos[k]) * t : c->pos[k];
            if (c->flags & POSE_FLAG_NORMALS) {
                float nv[3];
                lerp_oct(use_a && (a->flags & POSE_FLAG_NORMALS) ? a->nrm : NULL, c->nrm, t, nv);
                memcpy(vbuf + 32, nv, sizeof nv);
            }
            if (tangent_slot && (c->flags & POSE_FLAG_TANGENT)) {
                float nv[3];
                lerp_oct(use_a && (a->flags & POSE_FLAG_TANGENT) ? a->tan : NULL, c->tan, t, nv);
                memcpy(vbuf + (size_t)tangent_slot * 16u, nv, sizeof nv);
            }
            r->ok_stamp[si] = s_render_stamp;
            ++drawn;
        }
        memcpy(dst + (size_t)i * stride, vbuf, stride);
    }
    if (drawn && r->drawn_stamp != s_render_stamp) {
        r->drawn_stamp = s_render_stamp;
        ++s_render_drawn;
        s_render_vtx += drawn;
        s_render_missing += missing;
    }
    unlock_s(&s_mesh_lock);
    *gmin_out = lo;
    return drawn ? count : 0;
}

uint32_t rsx_coop_pose_write_indices(uint64_t id, uint32_t gmin, uint32_t count,
                                     uint32_t* dst, uint32_t max_indices)
{
    uint32_t n = 0;
    if (!s_render_live || !dst) return 0;
    lock_s(&s_mesh_lock);
    const submesh* s = sub_find(id, 0, 0);
    const part* r = s ? part_find(s->key, 0) : NULL;
    if (s && r && r->span) {
        for (uint32_t i = 0; i < s->ntri && n + 3u <= max_indices; ++i) {
            const uint32_t* t = s->tri + (size_t)i * 3u;
            int ok = 1, range_ok = 1;
            for (int k = 0; k < 3 && ok; ++k) {
                const uint32_t gi = t[k];
                range_ok = gi >= gmin && gi - gmin < count && gi >= r->gmin && gi - r->gmin < r->span;
                ok = range_ok && r->ok_stamp[gi - r->gmin] == s_render_stamp;
            }
            if (!ok) { if (!range_ok) ++s_dbg_out_of_range; else ++s_dbg_not_fresh; continue; }
            ++s_dbg_ok;
            dst[n++] = t[0] - gmin; dst[n++] = t[1] - gmin; dst[n++] = t[2] - gmin;
        }
    }
    unlock_s(&s_mesh_lock);
    s_render_tris += n / 3u;
    return n;
}

int rsx_coop_pose_render_end(void)
{
    s_stat_drawn = s_render_drawn;
    s_stat_remote = s_render_remote;
    s_stat_vtx = s_render_vtx;
    s_stat_missing = s_render_missing;
    s_stat_tris = s_render_tris;
    /* Round 4: any drawn part retires the proxy.  The old 60 % rule brought
     * the blocky proxy back whenever the peer's game swapped some of Cole's
     * part streams this instance could not draw yet (climbing, close camera). */
    return s_render_live && s_render_drawn && s_render_tris;
}

void rsx_coop_pose_get_stats(rsx_coop_pose_stats* st)
{
    if (!st) return;
    load_config();
    memset(st, 0, sizeof *st);
    st->send_hz = s_cfg_hz;
    st->sent_frames = s_stat_sent_frames;
    st->last_send_packets = s_stat_last_packets;
    st->last_send_vertices = s_stat_last_vertices;
    st->drawn_streams = s_stat_drawn;
    st->remote_streams = s_stat_remote;
    st->drawn_vertices = s_stat_vtx;
    st->missing_vertices = s_stat_missing;
    st->drawn_triangles = s_stat_tris;
    st->known_triangles = s_stat_known_tris;
    lock_s(&s_mesh_lock);
    st->received_frames = s_stat_received_frames;
    const uint64_t now = pose_now_ms();
    st->last_receive_age_ms = s_last_commit_ms && now >= s_last_commit_ms
                            ? (uint32_t)(now - s_last_commit_ms) : 0xFFFFFFFFu;
    unlock_s(&s_mesh_lock);
}

void rsx_coop_pose_debug_counts(uint32_t* ok, uint32_t* out_of_range, uint32_t* not_fresh)
{
    if (ok) *ok = s_dbg_ok;
    if (out_of_range) *out_of_range = s_dbg_out_of_range;
    if (not_fresh) *not_fresh = s_dbg_not_fresh;
}

void rsx_coop_pose_debug_parts(void)
{
    lock_s(&s_mesh_lock);
    for (uint32_t i = 0; i < s_part_count; ++i) {
        const part* p = &s_parts[i];
        fprintf(stderr, "[COOP-DIAG] part %016llX range %u..%u samples %u+%u tmpl %u+%u rx_age %lld%c",
                (unsigned long long)p->key, p->lo, p->hi, p->gmin, p->span, p->tmpl_gmin, p->tmpl_span,
                p->last_rx_ms ? (long long)(pose_now_ms() - p->last_rx_ms) : -1LL, 10);
    }
    for (uint32_t i = 0; i < s_sub_count; ++i) {
        const submesh* m = &s_subs[i];
        uint32_t lo = 0xFFFFFFFFu, hi = 0;
        for (uint32_t t = 0; t < m->ntri * 3u; ++t) { if (m->tri[t] < lo) lo = m->tri[t]; if (m->tri[t] > hi) hi = m->tri[t]; }
        fprintf(stderr, "[COOP-DIAG] submesh %016llX part %016llX tris %u idx %u..%u%c",
                (unsigned long long)m->id, (unsigned long long)m->key, m->ntri, lo, hi, 10);
    }
    unlock_s(&s_mesh_lock);
}
