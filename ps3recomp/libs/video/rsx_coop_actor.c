/* Native inFamous co-op peer renderer.
 *
 * This deliberately has no relationship to the optional Skate mod.  It
 * recognizes Cole's already-skinned, world-space triangle streams produced by
 * inFamous itself, keeps their current pose, and translates a cyan copy to the
 * network peer's position.  Consequently the prototype uses the game's own
 * character silhouette and animation without shipping or loading an asset.
 */
#include "rsx_coop_actor.h"

#if defined(PS3RECOMP_COOP_MOD)
volatile int g_coop_render_hooks;
#endif

#if defined(_WIN32) && defined(PS3RECOMP_COOP_MOD)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler.lib")

extern unsigned char* vm_base;
extern int infamous_coop_remote_actor_snapshot(float position[3], float* heading,
                                                float* speed, unsigned* age_ms);
/* Peer bolts from the title bridge (infamous-recomp/coop_mod.cpp).  Mirrors infamous_coop_bolt in
 * coop_mod.h (40 bytes, static_assert there). */
typedef struct coop_bolt {
    float start[3], end[3];
    unsigned age_ms, seed, flags, lifetime_ms;
} coop_bolt;
#define COOP_BOLT_HIT 1u
extern int infamous_coop_peer_bolts(coop_bolt* out, int max_bolts);
extern int rsx_coop_fx_peer_recent(unsigned ms);   /* coop v8 (L): rsx_d3d12_backend.c */

#define COOP_MAX_VERTICES 131072u
#define COOP_PROXY_VERTICES 216u
#define COOP_RING_SLOTS 8u
#define COOP_CB_BYTES 256u
#define COOP_BOLT_MAX 4u
#define COOP_BOLT_SEGMENTS 12u

typedef struct { float p[3]; } coop_vertex;
typedef struct {
    int attempted, ready;
    ID3D12Resource *vb, *cb;
    unsigned char *vb_map, *cb_map;
    ID3D12RootSignature* root;
    ID3DBlob *vs, *ps;
    ID3DBlob *bvs, *bps;                   /* peer bolt: additive glow shaders */
    struct { u32 fmt; ID3D12PipelineState* pso; } pipelines[4];
    struct { u32 fmt; ID3D12PipelineState* pso; } bolt_pipelines[4];
    u32 next_slot;
    u32 slot_count[COOP_RING_SLOTS];       /* proxy body vertices (first in the slot) */
    u32 slot_bolt_count[COOP_RING_SLOTS];  /* peer bolt vertices (coop_bolt_vertex); CB slot RING+slot */
    u32 slot_bolt_offset[COOP_RING_SLOTS]; /* byte offset of the bolt vertices inside the slot */
    u32 slot_frame[COOP_RING_SLOTS];
} coop_gpu;

static coop_gpu g;
typedef struct { volatile LONG frame, complete; } coop_native_health;
static coop_native_health native_health[COOP_RING_SLOTS];

#define COOP_TRACKED_STREAMS 512u
typedef struct coop_stream_track {
    u64 key;
    float player[3], center[3];
    u32 frame;
    unsigned char motion_matches;
    unsigned char confirmed;
} coop_stream_track;
static coop_stream_track stream_tracks[COOP_TRACKED_STREAMS];
static u32 stream_track_count;

void rsx_coop_actor_set_native_frame(u32 frame, int complete)
{
    /* The backend publishes, once per recorded frame, whether the peer's live
     * networked pose covered enough of Cole to retire the diagnostic proxy. */
    coop_native_health* health = &native_health[frame % COOP_RING_SLOTS];
    InterlockedExchange(&health->frame, -1);
    InterlockedExchange(&health->complete, complete ? 1 : 0);
    InterlockedExchange(&health->frame, (LONG)frame);
}

int rsx_coop_actor_native_frame_complete(u32 frame)
{
    const coop_native_health* health = &native_health[frame % COOP_RING_SLOTS];
    return (u32)InterlockedCompareExchange((volatile LONG*)&health->frame, 0, 0) == frame &&
           InterlockedCompareExchange((volatile LONG*)&health->complete, 0, 0) != 0;
}

/* A captured native character can occasionally contain only a tiny accessory
 * draw (for example a strap or effect pass).  Keep a simple full-size body in
 * the same centimetre coordinate system as a visibility guarantee.  It is
 * intentionally generated here rather than loaded from an external asset. */
static void append_box(coop_vertex* out, u32* count,
                       float x0, float y0, float z0,
                       float x1, float y1, float z1,
                       float heading)
{
    static const unsigned char triangles[36] = {
        0,2,1, 0,3,2, 4,5,6, 4,6,7,
        0,1,5, 0,5,4, 3,7,6, 3,6,2,
        0,4,7, 0,7,3, 1,2,6, 1,6,5
    };
    const float corners[8][3] = {
        {x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},
        {x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1}
    };
    const float co = cosf(heading), si = sinf(heading);
    if (*count > COOP_MAX_VERTICES - 36u) return;
    for (u32 i = 0; i < 36u; ++i) {
        const float* p = corners[triangles[i]];
        coop_vertex* v = &out[(*count)++];
        v->p[0] = p[0] * co - p[1] * si;
        v->p[1] = p[0] * si + p[1] * co;
        v->p[2] = p[2];
    }
}

static void append_visible_peer_proxy(coop_vertex* out, u32* count, float heading)
{
    /* Cole's tracked origin sits roughly around the pelvis; feet are about
     * 145 cm below it.  Six solid parts produce a readable human silhouette
     * at gameplay distance even before every native clothing draw is found. */
    append_box(out,count,-23,-14,-145,-4,14,-38,heading); /* left leg */
    append_box(out,count,  4,-14,-145,23,14,-38,heading); /* right leg */
    append_box(out,count,-32,-19, -42,32,19, 48,heading); /* torso */
    append_box(out,count,-18,-18,  48,18,18, 86,heading); /* head */
    append_box(out,count,-52,-13, -35,-32,13,40,heading); /* left arm */
    append_box(out,count, 32,-13, -35, 52,13,40,heading); /* right arm */
}

/* Peer: the peer's lightning bolt (polish round 1).  Drawn with its OWN additive pipeline
 * (ONE + ONE, depth test on, no depth write, no texture) so it glows instead of covering the scene.
 * Every vertex carries a procedural coordinate t.xy (0 on the bolt's axis / flash centre, 1 at the
 * quad's edge) and t.z = brightness; the pixel shader turns length(t.xy) into a blue-cyan glow with a
 * thin white-hot core, so quad edges are black (= invisible when added).  A strand is a jagged chain
 * of segments, each two crossed quads so it reads from any camera angle without the eye position;
 * the jitter is re-rolled every COOP_BOLT_JITTER_MS (crackle), stable within that step.  Branches fork
 * off the main channel; the impact flash is three crossed quads with a radial falloff (a soft star,
 * never a box).  Coordinates are relative to `base` (the peer position the shader adds back). */
#define COOP_BOLT_JITTER_MS 30u
#define COOP_BOLT_BRANCHES 3u
typedef struct { float p[3]; float t[3]; } coop_bolt_vertex;   /* 24 B: POSITION + TEXCOORD0 */

static u32 bolt_rng(u32* s) { *s = *s * 1664525u + 1013904223u; return *s; }
static float bolt_rnd(u32* s) { return (float)(bolt_rng(s) >> 8) * (2.0f / 16777216.0f) - 1.0f; }

static void bolt_put(coop_bolt_vertex* out, u32* count, u32 cap, const float p[3], float tx, float ty, float k)
{
    if (*count >= cap) return;
    coop_bolt_vertex v;   /* built locally, then one store: `out` is write-combined upload memory */
    v.p[0] = p[0]; v.p[1] = p[1]; v.p[2] = p[2]; v.t[0] = tx; v.t[1] = ty; v.t[2] = k;
    out[(*count)++] = v;
}

/* Quad from segment a->b, extruded by +-w; t.x runs -1..+1 across, t.y = 0 (a ribbon's glow depends
 * only on the distance from its axis).  ka / kb = brightness at the two ends. */
static void bolt_quad(coop_bolt_vertex* out, u32* count, u32 cap,
                      const float a[3], const float b[3], const float w[3], float ka, float kb)
{
    if (*count + 6u > cap) return;
    const float a0[3] = {a[0]-w[0],a[1]-w[1],a[2]-w[2]}, a1[3] = {a[0]+w[0],a[1]+w[1],a[2]+w[2]};
    const float b0[3] = {b[0]-w[0],b[1]-w[1],b[2]-w[2]}, b1[3] = {b[0]+w[0],b[1]+w[1],b[2]+w[2]};
    bolt_put(out,count,cap,a0,-1.0f,0.0f,ka); bolt_put(out,count,cap,a1,1.0f,0.0f,ka); bolt_put(out,count,cap,b1,1.0f,0.0f,kb);
    bolt_put(out,count,cap,a0,-1.0f,0.0f,ka); bolt_put(out,count,cap,b1,1.0f,0.0f,kb); bolt_put(out,count,cap,b0,-1.0f,0.0f,kb);
}

/* Orthonormal u, v perpendicular to unit d. */
static void bolt_basis(const float d[3], float u[3], float v[3])
{
    u[0] = d[1]; u[1] = -d[0]; u[2] = 0.0f;
    float ul = sqrtf(u[0]*u[0] + u[1]*u[1]);
    if (ul < 0.1f) { u[0] = 0.0f; u[1] = d[2]; u[2] = -d[1]; ul = sqrtf(u[1]*u[1] + u[2]*u[2]); }
    u[0] /= ul; u[1] /= ul; u[2] /= ul;
    v[0] = d[1]*u[2]-d[2]*u[1]; v[1] = d[2]*u[0]-d[0]*u[2]; v[2] = d[0]*u[1]-d[1]*u[0];
}

/* Jagged strand from s to e (already relative to base).  half_width is the GLOW radius in cm (the
 * white core is ~1/6 of it).  picks[0..npicks) receive evenly spaced interior joints (branch roots).
 * Returns 0 when the strand is degenerate. */
static int bolt_strand(coop_bolt_vertex* out, u32* count, u32 cap, const float s[3], const float e[3],
                       u32 seed, u32 segments, float amplitude, float half_width, float k0, float k1,
                       float picks[][3], u32 npicks)
{
    float d[3] = { e[0]-s[0], e[1]-s[1], e[2]-s[2] };
    const float len = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    if (!(len > 5.0f) || len > 20000.0f || segments < 1u) return 0;
    d[0] /= len; d[1] /= len; d[2] /= len;
    float u[3], v[3]; bolt_basis(d, u, v);
    float amp = len * 0.04f; if (amp > amplitude) amp = amplitude;
    float prev[3]; float kprev = k0;
    for (u32 i = 0; i <= segments; ++i) {
        const float t = (float)i / (float)segments;
        const float j = (i == 0 || i == segments) ? 0.0f : amp;
        const float ju = bolt_rnd(&seed) * j, jv = bolt_rnd(&seed) * j;
        /* per-joint flicker 0.65..1.0 so the channel pulses along its length */
        const float k = (k0 + (k1 - k0) * t) * (0.825f + 0.175f * bolt_rnd(&seed));
        float p[3];
        for (u32 c = 0; c < 3u; ++c) p[c] = s[c] + (e[c]-s[c]) * t + u[c]*ju + v[c]*jv;
        if (i) {
            const float wu[3] = { u[0]*half_width, u[1]*half_width, u[2]*half_width };
            const float wv[3] = { v[0]*half_width, v[1]*half_width, v[2]*half_width };
            bolt_quad(out, count, cap, prev, p, wu, kprev, k);
            bolt_quad(out, count, cap, prev, p, wv, kprev, k);
        }
        for (u32 n = 0; n < npicks; ++n)
            if (i == (segments * (n + 1u)) / (npicks + 1u)) memcpy(picks[n], p, sizeof p);
        memcpy(prev, p, sizeof p); kprev = k;
    }
    return 1;
}

/* Soft impact star: three axis-aligned crossed quads, t.xy = corner (+-1, +-1), radial falloff. */
static void bolt_flash(coop_bolt_vertex* out, u32* count, u32 cap, const float c[3], float r, float k)
{
    static const signed char corner[6][2] = { {-1,-1},{1,-1},{1,1}, {-1,-1},{1,1},{-1,1} };
    if (*count + 18u > cap || !(r > 0.0f)) return;
    for (u32 plane = 0; plane < 3u; ++plane)
        for (u32 i = 0; i < 6u; ++i) {
            const float x = (float)corner[i][0], y = (float)corner[i][1];
            float p[3] = { c[0], c[1], c[2] };
            const u32 ax = plane, ay = (plane + 1u) % 3u;   /* XY, YZ, ZX planes */
            p[ax] += x * r; p[ay] += y * r;
            bolt_put(out, count, cap, p, x, y, k);
        }
}

static void append_bolt(coop_bolt_vertex* out, u32* count, u32 cap, const float base[3], const coop_bolt* b, u32 frame)
{
    (void)frame;
    const float life = b->lifetime_ms ? (float)b->lifetime_ms : 130.0f;
    float age = (float)b->age_ms / life; if (age > 1.0f) age = 1.0f;
    /* full brightness for the first ~35 %, then fade out to 0 (additive: fading = disappearing) */
    float fade = age < 0.35f ? 1.0f : 1.0f - (age - 0.35f) / 0.65f;
    if (fade < 0.0f) fade = 0.0f;
    if (fade <= 0.01f) return;
    const u32 step = b->age_ms / COOP_BOLT_JITTER_MS;
    const u32 seed = b->seed * 2654435761u ^ (step + 1u) * 0x85EBCA6Bu;
    const float s[3] = { b->start[0]-base[0], b->start[1]-base[1], b->start[2]-base[2] };
    const float e[3] = { b->end[0]-base[0],   b->end[1]-base[1],   b->end[2]-base[2] };
    float roots[COOP_BOLT_BRANCHES][3];
    /* main channel: wide soft glow + white core (the core comes from the pixel shader) */
    if (!bolt_strand(out, count, cap, s, e, seed, COOP_BOLT_SEGMENTS, 60.0f, 16.0f, 0.85f * fade, 1.0f * fade,
                     roots, COOP_BOLT_BRANCHES)) return;
    /* second, thinner channel that wraps around the first (the classic doubled inFamous arc) */
    bolt_strand(out, count, cap, s, e, seed ^ 0x5bd1e995u, COOP_BOLT_SEGMENTS, 85.0f, 6.0f, 0.55f * fade, 0.7f * fade,
                NULL, 0u);
    /* short branches forking off the main channel, away from the axis, fading toward their tips */
    float d[3] = { e[0]-s[0], e[1]-s[1], e[2]-s[2] };
    const float len = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    d[0] /= len; d[1] /= len; d[2] /= len;
    float u[3], v[3]; bolt_basis(d, u, v);
    u32 bs = seed ^ 0x27d4eb2fu;
    for (u32 n = 0; n < COOP_BOLT_BRANCHES; ++n) {
        if ((bolt_rng(&bs) & 3u) == 0u) continue;   /* ~1 in 4 branches missing per step: crackle */
        float blen = len * (0.12f + 0.10f * (bolt_rnd(&bs) * 0.5f + 0.5f));
        if (blen > 450.0f) blen = 450.0f;
        const float a = bolt_rnd(&bs) * 3.14159265f;
        const float spread = 0.6f + 0.4f * (bolt_rnd(&bs) * 0.5f + 0.5f);
        float tip[3];
        for (u32 c = 0; c < 3u; ++c)
            tip[c] = roots[n][c] + (d[c] * 0.8f + (u[c] * cosf(a) + v[c] * sinf(a)) * spread) * blen;
        bolt_strand(out, count, cap, roots[n], tip, bs ^ (n * 0x9e3779b9u), 5u, 30.0f, 5.0f,
                    0.6f * fade, 0.05f * fade, NULL, 0u);
    }
    /* small soft flash where the shot ended (a little bigger when it hit this player) and at the hand */
    const float r = (b->flags & COOP_BOLT_HIT) ? 45.0f : 30.0f;
    bolt_flash(out, count, cap, e, r * (0.7f + 0.3f * fade), 0.9f * fade);
    bolt_flash(out, count, cap, s, 14.0f, 0.7f * fade);
}

static float guest_be_float(u32 address)
{
    if (!vm_base) return 0.0f;
    const unsigned char* p = vm_base + address;
    union { u32 u; float f; } v;
    v.u = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
    return v.f;
}

static int local_position(float p[3])
{
    p[0] = guest_be_float(0x00915430u);
    p[1] = guest_be_float(0x00915434u);
    p[2] = guest_be_float(0x00915438u);
    return isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]) &&
           (fabsf(p[0]) > 1.0f || fabsf(p[1]) > 1.0f);
}

int rsx_coop_actor_capture_vertices(u32 frame, u64 stream_key,
                                    const float* positions, size_t stride,
                                    u32 count)
{
    float player[3], lo[3] = {1e30f,1e30f,1e30f}, hi[3] = {-1e30f,-1e30f,-1e30f};
    float center[3] = {0,0,0};
    u32 near_count = 0;
    if (!g_coop_render_hooks || !stream_key || !positions || stride < 12 || count < 3 ||
        !local_position(player)) return 0;
    /* A part is learned once; do not re-read its write-combined window. */
    for (u32 i = 0; i < stream_track_count; ++i)
        if (stream_tracks[i].key == stream_key) {
            if (stream_tracks[i].confirmed) return 1;
            break;
        }

    /* Cole is emitted as several compact, SPU-skinned world-space triangle
     * draws.  Reject terrain, props, particles, screen quads and distant NPCs.
     * Requiring most vertices inside his standing capsule is intentionally
     * conservative, while still admitting wide combat poses and coat pieces. */
    for (u32 i = 0; i < count; ++i) {
        const float* p = (const float*)((const unsigned char*)positions + (size_t)i * stride);
        if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2])) return 0;
        for (u32 k = 0; k < 3; ++k) {
            if (p[k] < lo[k]) lo[k] = p[k]; if (p[k] > hi[k]) hi[k] = p[k];
            center[k] += p[k];
        }
        const float dx = p[0]-player[0], dy = p[1]-player[1], dz = p[2]-player[2];
        if (dx*dx + dy*dy < 135.0f*135.0f && dz > -170.0f && dz < 170.0f) ++near_count;
    }
    if (near_count * 10u < count * 8u || hi[0]-lo[0] > 280.0f ||
        hi[1]-lo[1] > 280.0f || hi[2]-lo[2] > 340.0f) return 0;

    center[0] /= (float)count; center[1] /= (float)count; center[2] /= (float)count;
    /* Proximity also admits pavement, props and nearby NPCs.  Cole's skinned
     * streams are the ones whose centroids translate with the local player.
     * Require two matching moving-frame observations before native replay. */
    coop_stream_track* tr = NULL;
    for (u32 i = 0; i < stream_track_count; ++i)
        if (stream_tracks[i].key == stream_key) { tr = &stream_tracks[i]; break; }
    if (!tr && stream_track_count < COOP_TRACKED_STREAMS) {
        tr = &stream_tracks[stream_track_count++];
        memset(tr, 0, sizeof(*tr)); tr->key = stream_key;
    }
    if (!tr) return 0;
    if (tr->frame && tr->frame != frame) {
        const float pdx=player[0]-tr->player[0], pdy=player[1]-tr->player[1], pdz=player[2]-tr->player[2];
        const float mdx=center[0]-tr->center[0], mdy=center[1]-tr->center[1], mdz=center[2]-tr->center[2];
        const float pm2=pdx*pdx+pdy*pdy+pdz*pdz;
        const float ex=mdx-pdx, ey=mdy-pdy, ez=mdz-pdz;
        const float err2=ex*ex+ey*ey+ez*ez;
        if (pm2 > 64.0f && pm2 < 250000.0f) {
            if (err2 < 625.0f) {
                if (tr->motion_matches < 255) ++tr->motion_matches;
                if (tr->motion_matches >= 2) tr->confirmed = 1;
            } else tr->motion_matches = 0;
        }
    }
    if (tr->frame != frame) {
        memcpy(tr->player, player, sizeof(player));
        memcpy(tr->center, center, sizeof(center));
        tr->frame = frame;
    }
    /* Learning only: the backend now draws the peer from the remote player's
     * networked pose.  (Uploading these unique-vertex positions as a triangle
     * list used to add cyan shards around the proxy.) */
    return tr->confirmed ? 1 : 0;
}

static int upload_buffer(ID3D12Device* dev, size_t size, ID3D12Resource** out, unsigned char** map)
{
    D3D12_HEAP_PROPERTIES hp = {0}; D3D12_RESOURCE_DESC rd = {0}; D3D12_RANGE empty = {0,0};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->lpVtbl->CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)out))) return 0;
    return SUCCEEDED((*out)->lpVtbl->Map(*out, 0, &empty, (void**)map));
}

static int initialize(ID3D12Device* dev)
{
    static const char shader[] =
        "cbuffer Peer:register(b0){float4 camera[4];float4 peer;float4 depth;float4 tint;};"
        "struct I{float3 p:POSITION;};struct O{float4 p:SV_Position;float4 c:COLOR;};"
        "O vs(I i){O o;float4 q=float4(i.p+peer.xyz,1);"
        "float4 h=float4(dot(camera[0],q),dot(camera[1],q),dot(camera[2],q),dot(camera[3],q));"
        "o.p=float4(h.xy,h.z*depth.x+h.w*depth.y,h.w);o.c=tint;return o;}"
        "float4 ps(O i):SV_Target{return i.c;}";
    /* peer bolt, in its own source so a problem here can never take the proxy/peer shaders down.
     * t.xy = 0 on the axis / flash centre, 1 at the quad edge; t.z = brightness.
     * Additive, so black edges vanish: blue-cyan glow (tint) + a thin white-hot core. */
    static const char bolt_shader[] =
        "cbuffer Peer:register(b0){float4 camera[4];float4 peer;float4 depth;float4 tint;};"
        "struct BI{float3 p:POSITION;float3 t:TEXCOORD0;};struct BO{float4 p:SV_Position;float3 t:TEXCOORD0;};"
        "BO bvs(BI i){BO o;float4 q=float4(i.p+peer.xyz,1);"
        "float4 h=float4(dot(camera[0],q),dot(camera[1],q),dot(camera[2],q),dot(camera[3],q));"
        "o.p=float4(h.xy,h.z*depth.x+h.w*depth.y,h.w);o.t=i.t;return o;}"
        "float4 bps(BO i):SV_Target{float r=length(i.t.xy);float a=saturate(1-r);"
        "float glow=a*a*a;float core=saturate(1-r*5.5);core*=core;"
        "float3 c=tint.rgb*(glow*1.1+a*0.15)+float3(1,1,1)*core*1.4;"
        "return float4(c*i.t.z,0);}";
    ID3DBlob *err = NULL, *sig = NULL;
    D3D12_ROOT_PARAMETER param = {0}; D3D12_ROOT_SIGNATURE_DESC desc = {0};
    g.attempted = 1;
    if (FAILED(D3DCompile(shader,sizeof(shader)-1,"coop_native_actor",NULL,NULL,"vs","vs_5_0",0,0,&g.vs,&err))) goto fail;
    if (FAILED(D3DCompile(shader,sizeof(shader)-1,"coop_native_actor",NULL,NULL,"ps","ps_5_0",0,0,&g.ps,&err))) goto fail;
    if (FAILED(D3DCompile(bolt_shader,sizeof(bolt_shader)-1,"coop_pvp_bolt",NULL,NULL,"bvs","vs_5_0",0,0,&g.bvs,&err)) ||
        FAILED(D3DCompile(bolt_shader,sizeof(bolt_shader)-1,"coop_pvp_bolt",NULL,NULL,"bps","ps_5_0",0,0,&g.bps,&err))) {
        /* non-fatal: the peer still draws, only the bolt is skipped (bolt_pipeline() checks bvs/bps) */
        fprintf(stderr,"[COOP-BOLT] bolt shader error: %s\n",err ? (char*)err->lpVtbl->GetBufferPointer(err) : "?");
        if (err) { err->lpVtbl->Release(err); err=NULL; }
        if (g.bvs) { g.bvs->lpVtbl->Release(g.bvs); g.bvs=NULL; }
        if (g.bps) { g.bps->lpVtbl->Release(g.bps); g.bps=NULL; }
    }
    param.ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV; param.Descriptor.ShaderRegister=0;
    param.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL; desc.NumParameters=1; desc.pParameters=&param;
    desc.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    if (FAILED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&sig,&err))) goto fail;
    if (FAILED(dev->lpVtbl->CreateRootSignature(dev,0,sig->lpVtbl->GetBufferPointer(sig),
            sig->lpVtbl->GetBufferSize(sig),&IID_ID3D12RootSignature,(void**)&g.root))) goto fail;
    if (!upload_buffer(dev,(size_t)COOP_RING_SLOTS*COOP_MAX_VERTICES*sizeof(coop_vertex),&g.vb,&g.vb_map) ||
        !upload_buffer(dev,(size_t)2u*COOP_RING_SLOTS*COOP_CB_BYTES,&g.cb,&g.cb_map)) goto fail;   /* proxy + bolt CBs */
    if (sig) sig->lpVtbl->Release(sig); if (err) err->lpVtbl->Release(err);
    g.ready=1; fprintf(stderr,"[COOP-ACTOR] native Cole geometry renderer ready (no external assets)\n"); return 1;
fail:
    if (err) fprintf(stderr,"[COOP-ACTOR] shader/resource error: %s\n",(char*)err->lpVtbl->GetBufferPointer(err));
    if (sig) sig->lpVtbl->Release(sig); if (err) err->lpVtbl->Release(err); return 0;
}

static ID3D12PipelineState* pipeline(ID3D12Device* dev, u32 fmt)
{
    for (u32 i=0;i<4;++i) if (g.pipelines[i].pso && g.pipelines[i].fmt==fmt) return g.pipelines[i].pso;
    u32 slot=4; for (u32 i=0;i<4;++i) if (!g.pipelines[i].pso) { slot=i; break; }
    if (slot==4 || !g.ready) return NULL;
    D3D12_INPUT_ELEMENT_DESC input={"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd={0};
    pd.pRootSignature=g.root; pd.VS.pShaderBytecode=g.vs->lpVtbl->GetBufferPointer(g.vs); pd.VS.BytecodeLength=g.vs->lpVtbl->GetBufferSize(g.vs);
    pd.PS.pShaderBytecode=g.ps->lpVtbl->GetBufferPointer(g.ps); pd.PS.BytecodeLength=g.ps->lpVtbl->GetBufferSize(g.ps);
    pd.InputLayout.pInputElementDescs=&input; pd.InputLayout.NumElements=1;
    pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable=TRUE;
    pd.DepthStencilState.DepthEnable=TRUE; pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL; pd.DSVFormat=DXGI_FORMAT_D24_UNORM_S8_UINT;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_RED|D3D12_COLOR_WRITE_ENABLE_GREEN|D3D12_COLOR_WRITE_ENABLE_BLUE;
    pd.SampleMask=UINT_MAX; pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets=1; pd.RTVFormats[0]=(DXGI_FORMAT)fmt; pd.SampleDesc.Count=1;
    if (FAILED(dev->lpVtbl->CreateGraphicsPipelineState(dev,&pd,&IID_ID3D12PipelineState,(void**)&g.pipelines[slot].pso))) return NULL;
    g.pipelines[slot].fmt=fmt; return g.pipelines[slot].pso;
}

/* peer bolt pipeline: same root signature / depth as the proxy, but POSITION + TEXCOORD0 input,
 * additive colour blend (ONE + ONE), alpha untouched, depth test on, depth write off. */
static ID3D12PipelineState* bolt_pipeline(ID3D12Device* dev, u32 fmt)
{
    for (u32 i=0;i<4;++i) if (g.bolt_pipelines[i].pso && g.bolt_pipelines[i].fmt==fmt) return g.bolt_pipelines[i].pso;
    u32 slot=4; for (u32 i=0;i<4;++i) if (!g.bolt_pipelines[i].pso) { slot=i; break; }
    if (slot==4 || !g.ready || !g.bvs || !g.bps) return NULL;
    D3D12_INPUT_ELEMENT_DESC input[2]={
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd={0};
    pd.pRootSignature=g.root; pd.VS.pShaderBytecode=g.bvs->lpVtbl->GetBufferPointer(g.bvs); pd.VS.BytecodeLength=g.bvs->lpVtbl->GetBufferSize(g.bvs);
    pd.PS.pShaderBytecode=g.bps->lpVtbl->GetBufferPointer(g.bps); pd.PS.BytecodeLength=g.bps->lpVtbl->GetBufferSize(g.bps);
    pd.InputLayout.pInputElementDescs=input; pd.InputLayout.NumElements=2;
    pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable=TRUE;
    pd.DepthStencilState.DepthEnable=TRUE; pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL; pd.DSVFormat=DXGI_FORMAT_D24_UNORM_S8_UINT;
    pd.BlendState.RenderTarget[0].BlendEnable=TRUE;
    pd.BlendState.RenderTarget[0].SrcBlend=D3D12_BLEND_ONE;       pd.BlendState.RenderTarget[0].DestBlend=D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].BlendOp=D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].SrcBlendAlpha=D3D12_BLEND_ZERO; pd.BlendState.RenderTarget[0].DestBlendAlpha=D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].BlendOpAlpha=D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].LogicOp=D3D12_LOGIC_OP_NOOP;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_RED|D3D12_COLOR_WRITE_ENABLE_GREEN|D3D12_COLOR_WRITE_ENABLE_BLUE;
    pd.SampleMask=UINT_MAX; pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets=1; pd.RTVFormats[0]=(DXGI_FORMAT)fmt; pd.SampleDesc.Count=1;
    if (FAILED(dev->lpVtbl->CreateGraphicsPipelineState(dev,&pd,&IID_ID3D12PipelineState,(void**)&g.bolt_pipelines[slot].pso))) {
        static int warned=0; if (!warned) { warned=1; fprintf(stderr,"[COOP-BOLT] bolt pipeline creation failed (fmt %u)\n",fmt); }
        return NULL;
    }
    g.bolt_pipelines[slot].fmt=fmt; return g.bolt_pipelines[slot].pso;
}

long rsx_coop_actor_prepare_scene(void* device, u32 frame, const float camera[16], float zscale, float zoffset)
{
    float remote[3], heading, speed; unsigned age;
    if (!g_coop_render_hooks || !infamous_coop_remote_actor_snapshot(remote,&heading,&speed,&age) || age>1500u ||
        !isfinite(remote[0]) || !isfinite(remote[1]) || !isfinite(remote[2])) return -1;
    ID3D12Device* dev=(ID3D12Device*)device; if (!g.attempted && !initialize(dev)) return -1;
    if (!g.ready) return -1;
    const u32 slot=g.next_slot++%COOP_RING_SLOTS;
    u32 n=0;
    coop_vertex* upload=(coop_vertex*)(g.vb_map+(size_t)slot*COOP_MAX_VERTICES*sizeof(coop_vertex));
    const u32 native_count=0;
    /* Once the backend is replaying Cole's real cached draws, retire the
     * blocky diagnostic body.  It remains an automatic fallback if no native
     * player draw has been recognized recently. */
    const int native_live = rsx_coop_actor_native_frame_complete(frame);
    if (!native_live) append_visible_peer_proxy(upload,&n,heading);
    const u32 proxy_n = n;
    /* Peer: the peer's live lightning bolts are drawn whether or not the proxy is. */
    /* They live in the same slot, after the proxy, in their own 24-byte format (16-byte aligned). */
    coop_bolt bolts[COOP_BOLT_MAX];
    int nb = infamous_coop_peer_bolts(bolts,(int)COOP_BOLT_MAX);
    /* coop v8 (L): the peer's REAL lightning (captured strips, rsx_d3d12_backend.c coop_fx_*) replaces this
     * procedural v4 ribbon while it is arriving. INFAMOUS_COOP_FX_FAKEBOLT=1 always draws it, 0 never. */
    { static int mode=-1; if (mode<0) { const char* e=getenv("INFAMOUS_COOP_FX_FAKEBOLT"); mode=(e&&*e=='1')?1:(e&&*e=='0')?0:2; }
      if (nb>0 && (mode==0 || (mode==2 && rsx_coop_fx_peer_recent(400u)))) {
          static unsigned skipped=0; if (!skipped++) fprintf(stderr,"[COOP-FX] v4 fake R1 bolt suppressed: the peer's real lightning strips are drawn instead\n");
          nb=0; } }
    const u32 bolt_off = ((proxy_n*(u32)sizeof(coop_vertex)) + 15u) & ~15u;
    const u32 bolt_cap = (u32)(((size_t)COOP_MAX_VERTICES*sizeof(coop_vertex) - bolt_off) / sizeof(coop_bolt_vertex));
    coop_bolt_vertex* bolt_out=(coop_bolt_vertex*)((unsigned char*)upload+bolt_off);
    u32 bolt_n=0;
    for (int i=0;i<nb;++i) append_bolt(bolt_out,&bolt_n,bolt_cap,remote,&bolts[i],frame);
    if (bolt_n) { static int bolt_reported=0; if (!bolt_reported) { bolt_reported=1;
        fprintf(stderr,"[COOP-BOLT] drawing the peer's lightning bolt (%u glow vertices, additive)\n",bolt_n); } }
    if (n == native_count && !bolt_n) return -1;
    if (proxy_n) { static int reported=0; if (!reported) {
        reported=1;
        fprintf(stderr,"[COOP-ACTOR] waiting for the peer's live pose: showing the proxy body (%u vertices)\n",
                proxy_n);
    } }
    float* cb=(float*)(g.cb_map+(size_t)slot*COOP_CB_BYTES);
    memset(cb,0,COOP_CB_BYTES); memcpy(cb,camera,16*sizeof(float));
    cb[16]=remote[0]; cb[17]=remote[1]; cb[18]=remote[2]; cb[19]=1.0f;
    cb[20]=zscale; cb[21]=zoffset; cb[24]=0.08f; cb[25]=0.95f; cb[26]=1.0f; cb[27]=1.0f;
    { float* bcb=(float*)(g.cb_map+(size_t)(COOP_RING_SLOTS+slot)*COOP_CB_BYTES);
      memset(bcb,0,COOP_CB_BYTES); memcpy(bcb,camera,16*sizeof(float));   /* written, never read back (WC memory) */
      bcb[16]=remote[0]; bcb[17]=remote[1]; bcb[18]=remote[2]; bcb[19]=1.0f; bcb[20]=zscale; bcb[21]=zoffset;
      bcb[24]=0.22f; bcb[25]=0.62f; bcb[26]=1.0f; bcb[27]=1.0f; }   /* bolt glow: electric blue-cyan (core is white in the PS) */
    g.slot_count[slot]=proxy_n;
    g.slot_bolt_count[slot]=bolt_n;
    g.slot_bolt_offset[slot]=bolt_off;
    g.slot_frame[slot]=frame;
    return (long)slot;
}

void rsx_coop_actor_draw_scene(void* device, void* command_list, long token,
                               u64 rtv_value, u64 dsv_value, u32 width,
                               u32 height, u32 rt_format)
{
    if (token<0 || token>=(long)COOP_RING_SLOTS || !g.ready) return;
    /* Candidate collection can finish after prepare_scene made this proxy
     * token.  Re-check the exact frame here so proxy and native Cole can never
     * be drawn together by the asynchronous recorder/replay pair.  Peer bolts
     * are drawn either way. */
    const u32 proxy_n = rsx_coop_actor_native_frame_complete(g.slot_frame[token]) ? 0u : g.slot_count[token];
    const u32 bolt_n = g.slot_bolt_count[token];
    if (!proxy_n && !bolt_n) return;
    ID3D12Device* dev=(ID3D12Device*)device; ID3D12GraphicsCommandList* list=(ID3D12GraphicsCommandList*)command_list;
    ID3D12PipelineState* pso=proxy_n ? pipeline(dev,rt_format) : NULL;
    ID3D12PipelineState* bpso=bolt_n ? bolt_pipeline(dev,rt_format) : NULL;
    if (!pso && !bpso) return;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv,dsv; rtv.ptr=(SIZE_T)rtv_value; dsv.ptr=(SIZE_T)dsv_value;
    D3D12_VIEWPORT vp={0,0,(float)width,(float)height,0,1}; D3D12_RECT sc={0,0,(LONG)width,(LONG)height};
    D3D12_VERTEX_BUFFER_VIEW vbv;
    vbv.BufferLocation=g.vb->lpVtbl->GetGPUVirtualAddress(g.vb)+(u64)token*COOP_MAX_VERTICES*sizeof(coop_vertex);
    vbv.SizeInBytes=(UINT)(g.slot_count[token]*sizeof(coop_vertex)); vbv.StrideInBytes=sizeof(coop_vertex);
    list->lpVtbl->OMSetRenderTargets(list,1,&rtv,FALSE,&dsv); list->lpVtbl->RSSetViewports(list,1,&vp); list->lpVtbl->RSSetScissorRects(list,1,&sc);
    list->lpVtbl->SetGraphicsRootSignature(list,g.root);
    list->lpVtbl->IASetPrimitiveTopology(list,D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if (proxy_n && pso) {
        list->lpVtbl->SetPipelineState(list,pso);
        list->lpVtbl->IASetVertexBuffers(list,0,1,&vbv);
        list->lpVtbl->SetGraphicsRootConstantBufferView(list,0,g.cb->lpVtbl->GetGPUVirtualAddress(g.cb)+(u64)token*COOP_CB_BYTES);
        list->lpVtbl->DrawInstanced(list,proxy_n,1,0,0);
    }
    if (bolt_n && bpso) {   /* after the body: additive glow over it */
        D3D12_VERTEX_BUFFER_VIEW bvbv;
        bvbv.BufferLocation=vbv.BufferLocation+g.slot_bolt_offset[token];
        bvbv.SizeInBytes=(UINT)(bolt_n*sizeof(coop_bolt_vertex)); bvbv.StrideInBytes=sizeof(coop_bolt_vertex);
        list->lpVtbl->SetPipelineState(list,bpso);
        list->lpVtbl->IASetVertexBuffers(list,0,1,&bvbv);
        list->lpVtbl->SetGraphicsRootConstantBufferView(list,0,g.cb->lpVtbl->GetGPUVirtualAddress(g.cb)+
                                                        (u64)(COOP_RING_SLOTS+(u32)token)*COOP_CB_BYTES);
        list->lpVtbl->DrawInstanced(list,bolt_n,1,0,0);
    }
}

/* ======================================================================================================
 * coop v8 (agent L): FALLBACK for the peer's lightning (rsx_d3d12_backend.c coop_fx_*).
 * The main path re-draws the peer's captured lightning strips with the game's own lightning material, hung on a
 * local lightning draw of the same frame (the donor).  A frame without any local lightning draw has no donor:
 * then the backend expands the strips itself (camera-facing quads, t.x -1..+1 across, t.z brightness) and hands
 * them here; they are drawn with the peer bolt glow pipeline (additive, depth test, no depth write) right after
 * the LAST blended scene draw of the frame -- the stage where the game draws its own lightning -- not at the
 * opaque-run token.  Vertices are absolute world cm (cb peer offset = 0).
 * ====================================================================================================== */
#define COOP_FX_SLOTS 8u
#define COOP_FX_MAX_VERTS 49152u
static ID3D12Resource *fx_vb, *fx_cb;
static unsigned char *fx_vb_map, *fx_cb_map;
static u32 fx_next, fx_count[COOP_FX_SLOTS];
static int fx_failed;

long rsx_coop_fx_ribbon_prepare(void* device, const float camera[16], float zscale, float zoffset,
                                const float* verts, u32 nverts, const float tint[4])
{
    if (!g_coop_render_hooks || !verts || nverts < 3u || fx_failed) return -1;
    ID3D12Device* dev=(ID3D12Device*)device;
    if (!g.attempted && !initialize(dev)) return -1;
    if (!g.ready || !g.bvs || !g.bps) return -1;
    if (!fx_vb) {
        if (!upload_buffer(dev,(size_t)COOP_FX_SLOTS*COOP_FX_MAX_VERTS*sizeof(coop_bolt_vertex),&fx_vb,&fx_vb_map) ||
            !upload_buffer(dev,(size_t)COOP_FX_SLOTS*COOP_CB_BYTES,&fx_cb,&fx_cb_map)) {
            fx_failed=1; fprintf(stderr,"[COOP-FX] fallback ribbon: upload buffers failed, fallback off\n"); return -1;
        }
        fprintf(stderr,"[COOP-FX] fallback ribbon renderer ready (own additive pipeline, drawn at the lightning stage)\n");
    }
    if (nverts > COOP_FX_MAX_VERTS) nverts = COOP_FX_MAX_VERTS;
    nverts -= nverts % 3u;
    const u32 slot=fx_next++%COOP_FX_SLOTS;
    memcpy(fx_vb_map+(size_t)slot*COOP_FX_MAX_VERTS*sizeof(coop_bolt_vertex),verts,(size_t)nverts*sizeof(coop_bolt_vertex));
    float cb[64]; memset(cb,0,sizeof cb); memcpy(cb,camera,16*sizeof(float));
    cb[19]=1.0f; cb[20]=zscale; cb[21]=zoffset;
    cb[24]=tint[0]; cb[25]=tint[1]; cb[26]=tint[2]; cb[27]=tint[3];
    memcpy(fx_cb_map+(size_t)slot*COOP_CB_BYTES,cb,sizeof cb);   /* one store into write-combined memory */
    fx_count[slot]=nverts;
    return (long)slot;
}

void rsx_coop_fx_ribbon_draw(void* device, void* command_list, long token, u64 rtv_value, u64 dsv_value,
                             u32 width, u32 height, u32 rt_format)
{
    if (token<0 || token>=(long)COOP_FX_SLOTS || !g.ready || !fx_vb || !fx_count[token]) return;
    ID3D12Device* dev=(ID3D12Device*)device; ID3D12GraphicsCommandList* list=(ID3D12GraphicsCommandList*)command_list;
    ID3D12PipelineState* bpso=bolt_pipeline(dev,rt_format);
    if (!bpso) return;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv,dsv; rtv.ptr=(SIZE_T)rtv_value; dsv.ptr=(SIZE_T)dsv_value;
    D3D12_VIEWPORT vp={0,0,(float)width,(float)height,0,1}; D3D12_RECT sc={0,0,(LONG)width,(LONG)height};
    D3D12_VERTEX_BUFFER_VIEW vbv;
    vbv.BufferLocation=fx_vb->lpVtbl->GetGPUVirtualAddress(fx_vb)+(u64)token*COOP_FX_MAX_VERTS*sizeof(coop_bolt_vertex);
    vbv.SizeInBytes=(UINT)(fx_count[token]*sizeof(coop_bolt_vertex)); vbv.StrideInBytes=sizeof(coop_bolt_vertex);
    list->lpVtbl->OMSetRenderTargets(list,1,&rtv,FALSE,dsv_value ? &dsv : NULL);
    list->lpVtbl->RSSetViewports(list,1,&vp); list->lpVtbl->RSSetScissorRects(list,1,&sc);
    list->lpVtbl->SetGraphicsRootSignature(list,g.root);
    list->lpVtbl->IASetPrimitiveTopology(list,D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->lpVtbl->SetPipelineState(list,bpso);
    list->lpVtbl->IASetVertexBuffers(list,0,1,&vbv);
    list->lpVtbl->SetGraphicsRootConstantBufferView(list,0,fx_cb->lpVtbl->GetGPUVirtualAddress(fx_cb)+(u64)token*COOP_CB_BYTES);
    list->lpVtbl->DrawInstanced(list,fx_count[token],1,0,0);
}

void rsx_coop_actor_shutdown(void)
{
    if (fx_vb) { fx_vb->lpVtbl->Unmap(fx_vb,0,NULL); fx_vb->lpVtbl->Release(fx_vb); fx_vb=NULL; }
    if (fx_cb) { fx_cb->lpVtbl->Unmap(fx_cb,0,NULL); fx_cb->lpVtbl->Release(fx_cb); fx_cb=NULL; }
    for (u32 i=0;i<4;++i) if (g.pipelines[i].pso) g.pipelines[i].pso->lpVtbl->Release(g.pipelines[i].pso);
    for (u32 i=0;i<4;++i) if (g.bolt_pipelines[i].pso) g.bolt_pipelines[i].pso->lpVtbl->Release(g.bolt_pipelines[i].pso);
    if (g.bvs) g.bvs->lpVtbl->Release(g.bvs); if (g.bps) g.bps->lpVtbl->Release(g.bps);
    if (g.root) g.root->lpVtbl->Release(g.root); if (g.vs) g.vs->lpVtbl->Release(g.vs); if (g.ps) g.ps->lpVtbl->Release(g.ps);
    if (g.vb) { g.vb->lpVtbl->Unmap(g.vb,0,NULL); g.vb->lpVtbl->Release(g.vb); }
    if (g.cb) { g.cb->lpVtbl->Unmap(g.cb,0,NULL); g.cb->lpVtbl->Release(g.cb); }
    memset(&g,0,sizeof(g));
}

#elif defined(PS3RECOMP_COOP_MOD)
int rsx_coop_actor_capture_vertices(u32 f,u64 k,const float*p,size_t s,u32 n){(void)f;(void)k;(void)p;(void)s;(void)n;return 0;}
void rsx_coop_actor_set_native_frame(u32 f,int c){(void)f;(void)c;}
int rsx_coop_actor_native_frame_complete(u32 f){(void)f;return 0;}
long rsx_coop_actor_prepare_scene(void*d,u32 f,const float c[16],float zs,float zo){(void)d;(void)f;(void)c;(void)zs;(void)zo;return -1;}
void rsx_coop_actor_draw_scene(void*d,void*l,long t,u64 r,u64 z,u32 w,u32 h,u32 f){(void)d;(void)l;(void)t;(void)r;(void)z;(void)w;(void)h;(void)f;}
void rsx_coop_actor_shutdown(void){}
long rsx_coop_fx_ribbon_prepare(void*d,const float c[16],float zs,float zo,const float*v,u32 n,const float t[4]){(void)d;(void)c;(void)zs;(void)zo;(void)v;(void)n;(void)t;return -1;}
void rsx_coop_fx_ribbon_draw(void*d,void*l,long t,u64 r,u64 z,u32 w,u32 h,u32 f){(void)d;(void)l;(void)t;(void)r;(void)z;(void)w;(void)h;(void)f;}
#endif
