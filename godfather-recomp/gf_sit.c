/* gf_sit.c -- v1109C sittable furniture (user: "let's see if we can add sittable furniture").
 *
 * U (GF_SIT_KEY) near something to sit on -> the player turns, sits on it with a seated whole-body clip, and stays there until a
 * movement key (W A S D, Space, Shift), U again, a cutscene, a car or damage. Found (2026-11-08 RE, see HANDOFF v1109C):
 *  - The animation banks have NO chair / bench clip; the car seat idles are seated poses: vehicle_front_passenger_idle (default),
 *    vehicle_backseat_idle, vehicle_backseat_passenger_idle, vehicle_front_driver_idle (GF_SIT_ANIM).
 *  - Played whole body on layer 0 (mask -1, gf_dialogue.c v1107W play_gesture_m: func_0046194C blend + func_00460E0C play),
 *    stopped with func_004610F4. The pad is locked meanwhile (g_gfd_lock -> rsx_ui_overlay.c uiov_pad_gate) so the game's
 *    on-foot logic gets no input; the player's transform is pinned every frame with SetMatrix vt+0x30 + position vt+0x24.
 *  - Where to sit, in this order:
 *    1. assets/gf_seats.txt (tools/gf_seats_build.py: 200 chairs / couches placed in the level MMdl data) within 1.4 m.
 *    2. the collision file (gf_rag_ray, assets/gf_collision.bin): a flat surface 0.33..0.70 m above the floor within 0.75 m
 *       behind / in front / beside the player with 0.8 m of head room -> benches, ledges, steps, low walls, crates, chairs.
 *    The player sits facing AWAY from the seat (the way he came), feet toward where he stood.
 *  - Hips on the seat: in gf_sit_post_skel (after every player skeleton update, gf_ragdoll.c) the pelvis = palette slot 0
 *    (model space, model = M (w - T)) is measured; the root is placed so the pelvis lands GF_SIT_PELVIS_UP (0.10 m) above the
 *    seat surface and GF_SIT_BACK (0.06 m) behind the seat point. Foot IK is off while seated (g_gf_sitting).
 * Main PPU thread (gf_sit_tick from gf_trainer.c main_tick). GF_SIT=0 off, GF_SIT_TEST=<s> (soak: try to sit s seconds after the
 * player exists, stand up 8 s later). Log [sit]. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "recompiled/ppu_recomp.h"

extern uint8_t* vm_base;
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
extern int gf_cutscene_active(void);
extern uint32_t gf_mission_hash(const char* s);
extern int gf_rag_ray(const float* o, const float* d, float maxd, float* t, float* n);
extern int gf_rag_tris_near(float x, float z, float r, float* out, int max);
extern int gf_rag_ent_frame(uint32_t ent, float* M9, float* T3);
extern int gf_rag_bone_slots(uint32_t ent, uint32_t anim, int* slot21, int* nslot);
extern volatile int g_gfd_lock, g_gfd_menu;
typedef void (*gfn)(ppu_context*);
#define TOC 0x00B4BF60u
volatile int g_gf_sitting = 0;   /* read by gf_footik.c (no foot IK while seated) */

static int ok(uint32_t ea) { return ea >= 0x10000u && ea < 0xF0000000u; }
static uint32_t rd32(uint32_t ea) { const uint8_t* p = vm_base + ea; return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static float rdf(uint32_t ea) { uint32_t u = rd32(ea); float f; memcpy(&f, &u, 4); return f; }
static void wrf(uint32_t ea, float v) { uint32_t u; memcpy(&u, &v, 4); uint8_t* d = vm_base + ea; d[0] = (uint8_t)(u >> 24); d[1] = (uint8_t)(u >> 16); d[2] = (uint8_t)(u >> 8); d[3] = (uint8_t)u; }
static int fok(float f) { return f == f && f > -1e7f && f < 1e7f; }
static double now_s(void) { return (double)GetTickCount64() / 1000.0; }
static int s_nlog;
#define SLOG(...) do { if (s_nlog < 1500) { s_nlog++; printf(__VA_ARGS__); fflush(stdout); } } while (0)

static uint32_t player(void)
{
    __try { uint32_t a = rd32(0x10181290u); if (!ok(a)) return 0; a = rd32(a + 4); if (!ok(a)) return 0; a = rd32(a);
        return (ok(a) && a >= 0x17FCu) ? a - 0x17FCu : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int in_car(uint32_t pl) { const uint32_t car = rd32(pl + 0x1718u); return ok(car) && car == rd32(pl + 0x4Cu) && (rd32(pl + 0x5C4u) & 0x8u); }
static int fg_ours(void) { HWND w = GetForegroundWindow(); DWORD pid = 0; if (w) GetWindowThreadProcessId(w, &pid); return pid == GetCurrentProcessId(); }
static int key(int vk) { return fg_ours() && (GetAsyncKeyState(vk) & 0x8000); }
static gfn lookup(uint32_t addr)
{
    for (uint64_t k = 0; k < function_table_count; k++) if ((uint32_t)function_table[k].addr == addr) return function_table[k].func;
    return 0;
}
static int gcall(gfn f, const uint64_t* a, int n, double f1, uint64_t* ret)
{
    ppu_context* cur = g_active_ctx; if (!cur || !f) return 0;
    static ppu_context c;
    __try {
        memcpy(&c, cur, sizeof c);
        c.gpr[1] = ((uint32_t)cur->gpr[1] - 0x1400u) & ~0xFull;
        c.gpr[2] = TOC; for (int i = 0; i < n && i < 8; i++) c.gpr[3 + i] = a[i];
        c.fpr[1] = f1; c.lr = 0;
        g_active_ctx = &c; f(&c);
        while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&c); }
        g_active_ctx = cur; if (ret) *ret = c.gpr[3]; return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_active_ctx = cur; g_trampoline_fn = 0; return 0; }
}
static uint32_t vfunc(uint32_t ent, uint32_t off) { const uint32_t vt = rd32(ent); if (!ok(vt)) return 0; const uint32_t opd = rd32(vt + off); return ok(opd) ? rd32(opd) : 0; }

/* ---------------------------------------------------------------- clips (layer 0, whole body) */
static int play_whole(uint32_t ent, const char* clip, double blend)
{
    static gfn f_blend, f_play; if (!f_play) { f_blend = lookup(0x0046194Cu); f_play = lookup(0x00460E0Cu); }
    if (!f_play || !ok(ent) || !ok(rd32(ent + 0xC4u))) return 0;
    const uint64_t b[2] = { ent, 0 }; gcall(f_blend, b, 2, blend, NULL);
    const uint64_t a[8] = { ent, gf_mission_hash(clip), 0, (uint64_t)(int64_t)-1, 1, 1, 0, 1 }; uint64_t r = 0;
    const int okc = gcall(f_play, a, 8, blend, &r);
    return okc && r;
}
static void stop_layer(uint32_t ent) { static gfn f; if (!f) f = lookup(0x004610F4u); if (!f || !ok(ent)) return; const uint64_t a[2] = { ent, 0 }; gcall(f, a, 2, 0.0, NULL); }
static uint32_t layer_hash(uint32_t ent) { const uint32_t anim = rd32(ent + 0xC4u); if (!ok(anim)) return 0; const uint32_t L = rd32(anim + 0x148u); return ok(L) ? rd32(L + 0x10u) : 0; }

/* ---------------------------------------------------------------- transform: yaw (model +z = forward) + position */
static float ent_yaw(uint32_t ent, float* T) { float M[9]; if (!gf_rag_ent_frame(ent, M, T)) return 1e9f; return atan2f(M[6], M[8]); }
static int set_xform(uint32_t ent, float x, float y, float z, float yaw)
{
    float T[3]; const float cur = ent_yaw(ent, T); if (cur > 1e8f) return 0;
    float st = yaw - cur; while (st > 3.14159265f) st -= 6.2831853f; while (st < -3.14159265f) st += 6.2831853f;
    const uint32_t xf = rd32(ent + 0x68); ppu_context* cc = g_active_ctx;
    static gfn f30, f24; static uint32_t vt_seen;
    if (rd32(ent) != vt_seen) { vt_seen = rd32(ent); f30 = lookup(vfunc(ent, 0x30)); f24 = lookup(vfunc(ent, 0x24)); }
    if (!ok(xf) || !cc || !f30 || !f24) return 0;
    const uint32_t buf = (((uint32_t)cc->gpr[1] - 0x800u) & ~0xFu);
    for (uint32_t k = 0; k < 64; k += 4) { const uint8_t* q = vm_base + xf + 0x10u + k; uint8_t* d = vm_base + buf + k; d[0] = q[0]; d[1] = q[1]; d[2] = q[2]; d[3] = q[3]; }
    float R[3][3]; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) R[i][j] = rdf(buf + 16u * (uint32_t)i + 4u * (uint32_t)j);
    float M[9], TT[3]; gf_rag_ent_frame(ent, M, TT);
    const int rows = fabsf(R[2][0] - M[6]) + fabsf(R[2][2] - M[8]) < 0.01f;   /* which way the matrix holds the model axes (gf_dialogue.c turn_toward) */
    const float c = cosf(st), s = sinf(st);
    for (int i = 0; i < 3; i++) {
        if (rows) { const float a = R[i][0], b = R[i][2]; R[i][0] = a * c + b * s; R[i][2] = -a * s + b * c; }
        else      { const float a = R[0][i], b = R[2][i]; R[0][i] = a * c + b * s; R[2][i] = -a * s + b * c; }
    }
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) wrf(buf + 16u * (uint32_t)i + 4u * (uint32_t)j, R[i][j]);
    wrf(buf + 0x30, x); wrf(buf + 0x34, y); wrf(buf + 0x38, z);
    const uint64_t a[2] = { ent, buf }; if (!gcall(f30, a, 2, 0.0, NULL)) return 0;
    const uint32_t blk = buf + 0x40u; wrf(blk, x); wrf(blk + 4, y); wrf(blk + 8, z);
    { uint8_t* d = vm_base + blk + 12u; d[0] = 0; d[1] = 0; d[2] = 0; d[3] = 2; }
    const uint64_t b[2] = { ent, blk }; gcall(f24, b, 2, 0.0, NULL);
    return 1;
}

/* ---------------------------------------------------------------- seats */
typedef struct { char kind; float x, y, z, ax, az, hx, hz, top; char name[32]; } Seat;
static Seat* s_db; static int s_ndb;
static void load_db(void)
{
    char exe[MAX_PATH]; DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH); FILE* f = NULL; char path[MAX_PATH + 64];
    if (n && n < MAX_PATH) { char* sl = strrchr(exe, '\\'); if (sl) *sl = 0;
        static const char* rel[] = { "\\assets\\gf_seats.txt", "\\..\\assets\\gf_seats.txt", "\\..\\..\\assets\\gf_seats.txt" };
        for (int i = 0; i < 3 && !f; i++) { snprintf(path, sizeof path, "%s%s", exe, rel[i]); f = fopen(path, "rb"); } }
    if (!f) { SLOG("[sit] no assets\\gf_seats.txt (tools/gf_seats_build.py) -> collision only\n"); return; }
    int cap = 512; s_db = (Seat*)calloc((size_t)cap, sizeof(Seat)); char l[256], k[16];
    while (fgets(l, sizeof l, f)) {
        if (l[0] == '#') continue;
        Seat S; memset(&S, 0, sizeof S);
        if (sscanf(l, "%15s %f %f %f %f %f %f %f %f %31s", k, &S.x, &S.y, &S.z, &S.ax, &S.az, &S.hx, &S.hz, &S.top, S.name) < 6) continue;
        if (fabsf(S.x) < 0.01f && fabsf(S.z) < 0.01f) continue;   /* interior-local placement (0,0,0) -- not a world position */
        S.kind = k[0];
        if (s_ndb == cap) { cap *= 2; Seat* np = (Seat*)realloc(s_db, (size_t)cap * sizeof(Seat)); if (!np) break; s_db = np; }
        s_db[s_ndb++] = S;
    }
    fclose(f); SLOG("[sit] v1109C %d placed seats loaded (%s)\n", s_ndb, path);
}
static float s_h_chair = 0.46f, s_h_sofa = 0.42f, s_h_stool = 0.72f, s_h_bench = 0.45f;
static float s_up = 0.10f, s_back = 0.06f, s_reach = 0.75f; static int s_anywhere = 0, s_nostop = 0, s_dumped = 0;
static const char* s_anim = "vehicle_backseat_passenger_idle";   /* v1109H: the user's pick ([ ] cycling, 00:40) */
typedef struct { float sx, sy, sz; float yaw; float floor; const char* src; char what[40]; } Pick;
static int ray_down(float x, float y, float z, float len, float* hy, float* ny)
{
    const float o[3] = { x, y, z }, d[3] = { 0, -1, 0 }; float t = 0, n[3] = { 0, 1, 0 };
    if (!gf_rag_ray(o, d, len, &t, n)) return 0; *hy = y - t; if (ny) *ny = n[1]; return 1;
}
static int ray_up_free(float x, float y, float z, float len) { const float o[3] = { x, y, z }, d[3] = { 0, 1, 0 }; float t = 0, n[3]; return !gf_rag_ray(o, d, len, &t, n); }
/* v1109D (user: "he's sitting on the seat, he's just looking the wrong way"): a chair faces AWAY from its backrest. From a point on
 * the seat: centre the flat seat patch (down rays on a 0.1 m grid within 0.35 m, |dh| < 0.05), then 24 horizontal rays at seat +0.30 m;
 * the nearest wall-like hit within 0.55 m = backrest -> facing = the other way. Returns 1 when a backrest was found. */
static int refine_chair(float* sx, float* sz, float sy, float* yaw)
{
    float cx = 0, cz = 0; int n = 0;
    for (float ox = -0.35f; ox <= 0.351f; ox += 0.1f) for (float oz = -0.35f; oz <= 0.351f; oz += 0.1f) {
        float hy; if (ray_down(*sx + ox, sy + 0.25f, *sz + oz, 0.4f, &hy, NULL) && fabsf(hy - sy) < 0.05f) { cx += *sx + ox; cz += *sz + oz; n++; } }
    if (n >= 3) { const float nx = cx / n, nz = cz / n; if ((nx - *sx) * (nx - *sx) + (nz - *sz) * (nz - *sz) < 0.3f * 0.3f) { *sx = nx; *sz = nz; } }
    /* v1109F (user: "I sit the wrong way every time"): the collision around a chair = the seat top, the BACKREST panel (rises from
     * seat level to ~+0.7 m) and a convex hull box of the whole chair (floor to top) + the table's box. v1109E's horizontal rays hit
     * the hull's inside walls. Now: near-vertical triangles (|n.y| < 0.6) within 0.5 m of the seat centre that START at seat level
     * (-0.12..+0.15 m) and reach >= +0.35 m above it = backrest; their area-weighted horizontal normals, turned to point at the seat
     * centre, = the way the chair faces. Offline on the user's Falconite chair: 330 deg (his correct sit); the room's other chairs
     * come out in facing pairs across their tables. */
    static float tri[9 * 400]; const int nt = gf_rag_tris_near(*sx, *sz, 0.6f, tri, 400);
    float ax = 0, az = 0; int k = 0;
    for (int i = 0; i < nt; i++) {
        const float* w = tri + 9 * i;
        const float cxx = (w[0] + w[3] + w[6]) / 3, cyy = (w[1] + w[4] + w[7]) / 3, czz = (w[2] + w[5] + w[8]) / 3; (void)cyy;
        if ((cxx - *sx) * (cxx - *sx) + (czz - *sz) * (czz - *sz) > 0.25f) continue;
        const float ylo = fminf(w[1], fminf(w[4], w[7])) - sy, yhi = fmaxf(w[1], fmaxf(w[4], w[7])) - sy;
        if (ylo < -0.12f || ylo > 0.15f || yhi < 0.35f) continue;
        const float e1[3] = { w[3] - w[0], w[4] - w[1], w[5] - w[2] }, e2[3] = { w[6] - w[0], w[7] - w[1], w[8] - w[2] };
        float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        const float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]); if (l < 1e-6f) continue;
        if (fabsf(n[1] / l) > 0.6f) continue;
        float hx = n[0], hz = n[2]; const float hl = sqrtf(hx * hx + hz * hz); if (hl < 1e-6f) continue; hx /= hl; hz /= hl;
        if (hx * (*sx - cxx) + hz * (*sz - czz) < 0) { hx = -hx; hz = -hz; }
        ax += hx * l * 0.5f; az += hz * l * 0.5f; k++;
    }
    if (!k || ax * ax + az * az < 1e-8f) return 0;
    *yaw = atan2f(ax, az);
    return 1;
}
/* best place to sit near the player at (P, facing fy); 0 = nothing */
static int find_seat(const float* P, float fy, Pick* out)
{
    float floor_y = P[1]; ray_down(P[0], P[1] + 0.5f, P[2], 1.2f, &floor_y, NULL);
    if (fabsf(floor_y - P[1]) > 0.35f) floor_y = P[1];
    /* 1. placed seats */
    int bi = -1; float bd = 1.4f;
    for (int i = 0; i < s_ndb; i++) {
        const Seat* S = &s_db[i]; const float dx = S->x - P[0], dz = S->z - P[2], d = sqrtf(dx * dx + dz * dz);
        if (d < bd && fabsf(S->y - floor_y) < 1.0f) { bd = d; bi = i; }
    }
    if (bi >= 0) {
        const Seat* S = &s_db[bi];
        const float h = S->kind == 's' && strstr(S->name, "stool") ? s_h_stool : S->kind == 's' ? s_h_sofa : S->kind == 'b' ? s_h_bench : s_h_chair;
        /* face away from the seat: the local axis (+-X, +-Z) nearest to "seat -> player" */
        const float vx = P[0] - S->x, vz = P[2] - S->z; const float axes[4][2] = { { S->ax, S->az }, { -S->ax, -S->az }, { -S->az, S->ax }, { S->az, -S->ax } };
        int ba = 0; float bdot = -9;
        for (int a = 0; a < 4; a++) { const float dd = axes[a][0] * vx + axes[a][1] * vz; if (dd > bdot) { bdot = dd; ba = a; } }
        if (vx * vx + vz * vz < 0.01f) { ba = 0; bdot = -9; for (int a = 0; a < 4; a++) { const float dd = axes[a][0] * sinf(fy) + axes[a][1] * cosf(fy); if (dd > bdot) { bdot = dd; ba = a; } } }
        float sy = S->y + h; float hy;
        if (ray_down(S->x, S->y + 1.0f, S->z, 1.0f, &hy, NULL) && hy - S->y > 0.3f && hy - S->y < 0.8f) sy = hy;   /* collision knows better */
        out->sx = S->x; out->sz = S->z; out->sy = sy; out->yaw = atan2f(axes[ba][0], axes[ba][1]); out->floor = S->y; out->src = "placed seat";
        if (refine_chair(&out->sx, &out->sz, sy, &out->yaw)) out->src = "placed seat, backrest";
        snprintf(out->what, sizeof out->what, "%s", S->name);
        return 1;
    }
    /* 2. the collision file: behind first (the natural way to sit down), then front, left, right */
    const float dirs[4] = { fy + 3.14159265f, fy, fy + 1.5707963f, fy - 1.5707963f };
    for (int k = 0; k < 4; k++) {
        const float dx = sinf(dirs[k]), dz = cosf(dirs[k]);
        for (float dist = 0.30f; dist <= s_reach + 0.001f; dist += 0.15f) {
            const float x = P[0] + dx * dist, z = P[2] + dz * dist; float hy, ny;
            if (!ray_down(x, floor_y + 1.2f, z, 1.15f, &hy, &ny)) continue;
            const float h = hy - floor_y;
            if (h < 0.33f || h > 0.70f || ny < 0.85f) continue;
            /* a real seat, not a step of stairs: flat for 0.25 m further out too, and head room above */
            float h2; if (!ray_down(x + dx * 0.2f, floor_y + 1.2f, z + dz * 0.2f, 1.15f, &h2, NULL) || fabsf(h2 - hy) > 0.06f) continue;
            if (!ray_up_free(x, hy + 0.05f, z, 0.85f)) continue;
            const float in = 0.12f;   /* sit a little in from the edge */
            out->sx = x + dx * in; out->sz = z + dz * in; out->sy = hy; out->floor = floor_y;
            out->yaw = atan2f(-dx, -dz); out->src = "collision";
            if (refine_chair(&out->sx, &out->sz, hy, &out->yaw)) out->src = "collision, backrest";
            snprintf(out->what, sizeof out->what, "surface %.2f m high", h);
            return 1;
        }
    }
    if (s_anywhere) {   /* test aid / "sit on the air": a chair-high seat right behind the player */
        const float dx = sinf(fy + 3.14159265f), dz = cosf(fy + 3.14159265f);
        out->sx = P[0] + dx * 0.35f; out->sz = P[2] + dz * 0.35f; out->floor = floor_y; out->sy = floor_y + s_h_chair;
        out->yaw = fy; out->src = "GF_SIT_ANYWHERE"; snprintf(out->what, sizeof out->what, "nothing (anywhere mode)");
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- state */
static int s_on = -1, s_key = 'U', s_prev_key = 0; static float s_test = 0.0f; static double s_test_t0 = 0.0, s_test_sat = 0.0; static int s_test_done = 0;
static struct { int st; uint32_t pl; Pick seat; float stand[3], stand_yaw; double t0, t_replay; uint32_t clip_h; int clip_ok; } A;
static volatile float s_pel_m[3]; static volatile int s_pel_ok; static volatile LONG s_pel_n;   /* pelvis in model space (post_skel) */
static void cfg(void)
{
    const char* e = getenv("GF_SIT"); s_on = !(e && *e == '0');
    if ((e = getenv("GF_SIT_KEY")) && *e) s_key = (unsigned char)(e[0] >= 'a' && e[0] <= 'z' ? e[0] - 32 : e[0]);
    if ((e = getenv("GF_SIT_ANIM")) && *e) s_anim = e;
    if ((e = getenv("GF_SIT_PELVIS_UP"))) s_up = (float)atof(e);
    if ((e = getenv("GF_SIT_BACK"))) s_back = (float)atof(e);
    if ((e = getenv("GF_SIT_REACH")) && atof(e) > 0.2) s_reach = (float)atof(e);
    if ((e = getenv("GF_SIT_TEST"))) s_test = (float)atof(e);
    if ((e = getenv("GF_SIT_ANYWHERE"))) s_anywhere = *e == '1';
    if ((e = getenv("GF_SIT_NOSTOP"))) s_nostop = *e == '1';
    if (s_on) load_db();
    SLOG("[sit] v1109G sittable furniture %s: key %c ([ ] = other seated clip), clip %s, pelvis %.2f m above the seat, %.2f m back, reach %.2f m%s (GF_SIT)\n",
         s_on ? "ON" : "off", s_key, s_anim, (double)s_up, (double)s_back, (double)s_reach, s_test > 0 ? " + TEST" : "");
}
/* v1109G (user: "smoother when sitting down, so he doesn't just stand and then glide into the sitting position"; "legs look
 * incredibly short"; camera spinning -> gf_footcam.c): phases
 *   st 1 TURN   0 .. T_TURN       standing, turns on the spot to the seat's facing (smoothstep)
 *   st 2 LOWER  .. + T_LOWER      seated clip fades in (blend = T_LOWER), the root travels back and down onto the seat (ease in-out;
 *                                 height follows a little later than the back step, like bending the knees)
 *   st 3 SEATED                   held
 *   st 4 RISE   0 .. T_RISE       clip fades out, root back up / forward to where he stood, then the pad is free again
 * [ / ] while seated = the next / previous seated clip (each one's pelvis is measured again); the choice is kept in gf_sit_clip.txt. */
static const char* const k_clips[] = { "vehicle_backseat_passenger_idle", "vehicle_front_passenger_idle", "vehicle_backseat_idle", "vehicle_front_driver_idle" };
#define NCLIP 4
static int s_clip = -1; static float T_TURN = 0.35f, T_LOWER = 0.75f, T_RISE = 0.6f;
static float smooth(float x) { x = x < 0 ? 0 : x > 1 ? 1 : x; return x * x * (3.0f - 2.0f * x); }
static float wrapa(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }
static void clip_save(void) { FILE* f = fopen("gf_sit_clip.txt", "w"); if (f) { fprintf(f, "%s\n", k_clips[s_clip]); fclose(f); } }
static void clip_load(void)
{
    s_clip = 0; for (int i = 0; i < NCLIP; i++) if (!strcmp(s_anim, k_clips[i])) s_clip = i;
    FILE* f = fopen("gf_sit_clip.txt", "r"); char l[96];
    if (f) { if (fgets(l, sizeof l, f)) { l[strcspn(l, "\r\n")] = 0; for (int i = 0; i < NCLIP; i++) if (!strcmp(l, k_clips[i])) s_clip = i; } fclose(f); }
    if (!getenv("GF_SIT_ANIM")) s_anim = k_clips[s_clip];
}
static float s_root_end[3];   /* seated root (for the rise) */
static void seated_root(float* r)   /* root position that puts the pelvis on the seat */
{
    float ox = 0.02f, oy = 0.723f, oz = -0.367f;   /* v1109H backseat passenger idle, measured -- until this clip's own pelvis is known */
    const float c = cosf(A.seat.yaw), sn = sinf(A.seat.yaw);
    if (s_pel_ok) { ox = s_pel_m[0]; oy = s_pel_m[1]; oz = s_pel_m[2]; }
    const float wx = ox * c + oz * sn, wz = -ox * sn + oz * c;
    r[0] = A.seat.sx - sinf(A.seat.yaw) * s_back - wx; r[2] = A.seat.sz - cosf(A.seat.yaw) * s_back - wz; r[1] = A.seat.sy + s_up - oy;
}
static void stand_up(const char* why)
{
    const uint32_t pl = A.pl;
    if (ok(pl) && pl == player()) { stop_layer(pl); A.st = 4; A.t0 = now_s(); SLOG("[sit] standing up (%s)\n", why); return; }
    A.st = 0; g_gf_sitting = 0; g_gfd_lock = 0;
}
static int try_sit(uint32_t pl)
{
    float T[3]; const float fy = ent_yaw(pl, T); if (fy > 1e8f) return 0;
    Pick p; memset(&p, 0, sizeof p);
    if (!find_seat(T, fy, &p)) { SLOG("[sit] nothing to sit on near (%.2f, %.2f, %.2f)\n", T[0], T[1], T[2]); return 0; }
    A.pl = pl; A.seat = p; A.stand[0] = T[0]; A.stand[1] = T[1]; A.stand[2] = T[2]; A.stand_yaw = fy;
    A.st = 1; A.t0 = A.t_replay = now_s(); g_gf_sitting = 1; g_gfd_lock = 1; s_pel_ok = 0; A.clip_ok = 0;
    SLOG("[sit] sit down on %s (%s) at (%.2f, %.2f, %.2f) seat %.2f m above the floor, facing %.0f deg, clip %s\n",
         p.what, p.src, p.sx, p.sy, p.sz, (double)(p.sy - p.floor), (double)(p.yaw * 57.2958f), s_anim);
    return 1;
}
static void start_clip(uint32_t pl, double blend)
{
    A.clip_ok = play_whole(pl, s_anim, blend); A.clip_h = layer_hash(pl); A.t_replay = now_s();
    SLOG("[sit] clip %s %s (layer %08X)\n", s_anim, A.clip_ok ? "playing" : "NOT FOUND", A.clip_h);
}
static void hold(uint32_t pl)
{
    const double t = now_s(); const float el = (float)(t - A.t0);
    float r[3]; seated_root(r);
    if (A.st == 1) {   /* turn on the spot */
        const float w = smooth(el / T_TURN), yaw = A.stand_yaw + wrapa(A.seat.yaw - A.stand_yaw) * w;
        set_xform(pl, A.stand[0], A.stand[1], A.stand[2], yaw);
        if (el >= T_TURN) { A.st = 2; A.t0 = t; start_clip(pl, T_LOWER);
            if (!A.clip_ok) { A.st = 0; g_gf_sitting = 0; g_gfd_lock = 0; SLOG("[sit] no seated clip -> off\n"); } }
        return;
    }
    if (A.st == 2 || A.st == 3) {
        if (layer_hash(pl) != A.clip_h && t - A.t_replay > 0.5) { start_clip(pl, 0.2); SLOG("[sit] clip re-played (layer was taken)\n"); }
        float px = r[0], py = r[1], pz = r[2];
        if (A.st == 2) {
            const float wh = smooth(el / T_LOWER), wv = smooth((el - 0.12f * T_LOWER) / (0.88f * T_LOWER));
            px = A.stand[0] + (r[0] - A.stand[0]) * wh; pz = A.stand[2] + (r[2] - A.stand[2]) * wh; py = A.stand[1] + (r[1] - A.stand[1]) * wv;
            if (el >= T_LOWER) A.st = 3;
        }
        set_xform(pl, px, py, pz, A.seat.yaw);
        s_root_end[0] = px; s_root_end[1] = py; s_root_end[2] = pz;
        static double lt; if (A.st == 3 && t - lt > 5.0) { lt = t; SLOG("[sit] seated: pelvis model (%.2f, %.2f, %.2f)%s, root (%.2f, %.2f, %.2f) seat y %.2f, clip %s\n",
            s_pel_m[0], s_pel_m[1], s_pel_m[2], s_pel_ok ? "" : " (not measured)", px, py, pz, A.seat.sy, s_anim); }
        return;
    }
    if (A.st == 4) {   /* rise: the layer fades out (func_004610F4), the root goes back to where he stood */
        const float wh = smooth((el - 0.1f * T_RISE) / (0.9f * T_RISE)), wv = smooth(el / (0.8f * T_RISE));
        const float px = s_root_end[0] + (A.stand[0] - s_root_end[0]) * wh, pz = s_root_end[2] + (A.stand[2] - s_root_end[2]) * wh;
        const float py = s_root_end[1] + (A.stand[1] - s_root_end[1]) * wv;
        set_xform(pl, px, py, pz, A.seat.yaw);
        if (el >= T_RISE) { A.st = 0; g_gf_sitting = 0; g_gfd_lock = 0; SLOG("[sit] stood up\n"); }
    }
}
void gf_sit_tick(void)
{
    if (s_on < 0) { cfg(); clip_load(); }
    if (!s_on) return;
    const uint32_t pl = player();
    const int kd = key(s_key), press = kd && !s_prev_key; s_prev_key = kd;
    static int pb_prev = 0; const int pb = key(VK_OEM_6) ? 1 : key(VK_OEM_4) ? -1 : 0; const int pbe = pb && pb != pb_prev; pb_prev = pb;
    if (A.st) {
        if (!pl || pl != A.pl) { A.st = 0; g_gf_sitting = 0; g_gfd_lock = 0; SLOG("[sit] player changed -> off\n"); return; }
        if (A.st != 4) {
            const float hp = rdf(pl + 0x14C8u);
            const char* why = gf_cutscene_active() ? "cutscene" : in_car(pl) ? "car" : !(fok(hp) && hp > 0.0f) ? "dead" : NULL;
            if (!why && A.st == 3) {
                if (press) why = "key";
                else if (!s_nostop) { static const struct { int vk; const char* n; } mk[] = { { 'W', "W" }, { 'A', "A" }, { 'S', "S" }, { 'D', "D" }, { VK_SPACE, "Space" }, { VK_SHIFT, "Shift" }, { VK_ESCAPE, "Esc" } };
                    for (int i = 0; i < 7 && !why; i++) if (key(mk[i].vk)) why = mk[i].n; }
                if (!why && pbe) { s_clip = (s_clip + pb + NCLIP) % NCLIP; s_anim = k_clips[s_clip]; s_pel_ok = 0; clip_save(); start_clip(pl, 0.35);
                    SLOG("[sit] clip -> %s (%d of %d)\n", s_anim, s_clip + 1, NCLIP); }
            }
            if (!why && s_test > 0 && s_test_sat > 0 && now_s() - s_test_sat > 8.0) why = "test";
            if (!why && s_test > 0 && s_test_sat > 0 && !s_dumped && now_s() - s_test_sat > 2.0) { extern volatile int g_kd_dump_req; g_kd_dump_req = 1; s_dumped = 1; }
            if (why) {
                if (why[0] == 'c' || why[0] == 'd' || (why[0] == 'c' && why[1] == 'a')) { stop_layer(pl); A.st = 0; g_gf_sitting = 0; g_gfd_lock = 0; SLOG("[sit] off (%s)\n", why); return; }
                stand_up(why);
            }
        }
        hold(pl);
        return;
    }
    if (!pl || in_car(pl) || gf_cutscene_active() || g_gfd_menu || g_gfd_lock) return;
    int go = press;
    if (s_test > 0 && !s_test_done) { if (s_test_t0 == 0.0) s_test_t0 = now_s(); if (now_s() - s_test_t0 > s_test) { go = 1; s_test_done = 1; } }
    if (go && try_sit(pl) && s_test_done) s_test_sat = now_s();
}
/* gf_ragdoll.c gf_rag_post_skel, after every skeleton update: the player's pelvis (palette slot of bone 0) in model space, once the
 * seated clip has blended in (seated, or the last 15 % of lowering) */
void gf_sit_post_skel(uint32_t anim, uint32_t ent)
{
    if (!(A.st == 3 || A.st == 2) || !ent || ent != A.pl || !ok(anim)) return;
    if (A.st == 2 && now_s() - A.t0 < 0.85 * T_LOWER) return;
    if (now_s() - A.t_replay < 0.3) return;   /* a clip switch is blending */
    __try {
        static uint32_t s_anim_c; static int s_slot0 = -1;
        if (anim != s_anim_c) { int sl[21], ns = 0; s_anim_c = anim; s_slot0 = gf_rag_bone_slots(ent, anim, sl, &ns) ? sl[0] : -1; }
        if (s_slot0 < 0) return;
        const uint32_t h = rd32(anim + 0x74); const uint32_t pal = ok(h) ? rd32(h + 8) : 0; if (!ok(pal)) return;
        const uint32_t m = pal + (uint32_t)s_slot0 * 64u;
        const float x = rdf(m + 0x30), y = rdf(m + 0x34), z = rdf(m + 0x38);
        if (!(fok(x) && fok(y) && fok(z)) || fabsf(x) > 2 || fabsf(z) > 2 || y < -0.5f || y > 1.5f) return;
        if (!s_pel_ok) { s_pel_m[0] = x; s_pel_m[1] = y; s_pel_m[2] = z; s_pel_ok = 1; SLOG("[sit] pelvis measured (%s): model (%.3f, %.3f, %.3f)\n", s_anim, x, y, z); }
        else { s_pel_m[0] += (x - s_pel_m[0]) * 0.1f; s_pel_m[1] += (y - s_pel_m[1]) * 0.1f; s_pel_m[2] += (z - s_pel_m[2]) * 0.1f; }
        InterlockedIncrement(&s_pel_n);
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
