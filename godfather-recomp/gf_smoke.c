/* gf_smoke.c -- v1079S (Claude): "press a key to smoke a cigarette" -- the player plays the game's own "ambient_smoke" clip.
 *
 * RE (2026-11-06, HANDOFF "RE notes: smoking" + v1079S): animation names are hashed with gf_mission_hash (sdbm, lower-case);
 * "ambient_smoke" = 0x41BCB1A8 (animation_banks2; its events reference cig_puff / cig_smoke / cig_xhale and the joints
 * JO_pntB_L (hand) / JO_orbOris_BR (mouth)). anim = *(ent+0xC4) is the main controller; anim+0x10 = hash playing now.
 * v1079S-1/2: playing it on the MAIN controller (func_00460CE8(ent, hash, 1, 0)) works for 0.03-0.05 s, then the player's idle /
 * locomotion logic plays its own clip again (busy bit 0x02000000 did not stop that).
 * v1079S-3 (user: "I want that to work even while walking"): play it on ANIMATION LAYER 0 -- the upper-body layer the game uses
 * for pistol reload / switch / holster / conceal-idle while the legs keep walking. Layers = controllers chained at anim+0x148
 * (layer n = n further +0x148 hops), each with its own +0x10 hash and fade fields +0x134/+0x138. The game's sequence
 * (func_004DEA44, 12 callers, e.g. pistol_s_aim_reload_clip):
 *     func_0046194C(ent, layer) with f1 = blend   -> func_003CD160: layer+0x168 = blend
 *     func_00460E0C(ent, hash, layer, -1, 1, 1, 0, 1)  -> func_003CCE1C -> func_003CCC28 (play on the layer + fade in)
 * v1079S-4 (user: legs don't move while smoking + walking, he glides): the 4th arg of func_00460E0C is the layer's BONE MASK
 * ROOT (func_003C6E10: mask = that bone + all its descendants; -1 = no mask = whole body). ambient_smoke animates the whole body,
 * so with -1 it froze the legs. Now the mask root is the lumbar bone (findBone, gf_rag_bone_index): spine, arms, head smoke,
 * pelvis + legs keep walking. GF_SMOKE_MASK = lumbar (default) / thoracic / none.
 * v1085W (user: "while smoking the player walks instead of the normal run; if they start running they stop smoking"):
 * cellPad.c g_gf_force_walk caps the left stick at the walk deflection (PAD_KBM_WALK) while a smoke is on; sprint (Cross held:
 * Shift on the keyboard, X on a pad -- g_gf_cross_held) ends the smoke.
 * v1086W (user: "so slow I can't even walk, I turn around in frames"): 64/127 is in the game's turn-on-the-spot band. The cap is
 * now GF_SMOKE_WALK (default 90 of 127: 80-95 walk at ~1.6 m/s, 110+ jogs); GF_SMOKE_WALK_SWEEP="70,85,100,115" (test) steps through caps every 4 s while
 * smoking. While smoking the player's ground speed is logged every 0.5 s ("[smoke] speed").
 * v1087C (user: "when the player engages in combat, the smoking stops"; 00:10: "not when a gun is just drawn, only when aiming,
 * and melee"): ends the smoke on any aim / fire / lock-on / attack button (cellPad.c g_gf_combat_btn = L2 R2 L1 R1 SQUARE; LMB / RMB too),
 * or the player losing health.
 * v1087C prop scan (finding the cigarette's prop ID): "prop attach" (func_00243568 loc_00243B9C) builds a prop with
 * func_0029B9F0(player + 0x2698 + 16 * player[0x2A9C], 0,0,0,0) and keeps it in the handle at ent+0x17E0 (prop = handle - 0x38);
 * NPC smokers get theirs through "living world prop attach" -> func_00239EDC(ent, 0x67, ...). GF_SMOKE_SCAN (default 1) logs
 * every distinct prop held by any character (handle at +0x17E0: vtable, the GUID at +0xC4, a header dump) and every NPC
 * seen playing ambient_smoke, plus the player's prop-slot table once. Log tag [smoke-scan].
 * v1088M (user brought models: "Cigarette Low Poly" by pixelAlp, CC-BY-4.0 -> assets/gf_cigarette.bin + CREDITS.txt): the game's
 * ambient smoke has no separate cigarette object, so we draw one: rsx_ui_overlay.c gfm_set(0, world matrix, on) renders the mesh
 * in the blood-decal pass (before the characters, scene-depth tested). While smoking it rides the attach joint the clip's events
 * name, JO_pntB_L (looked up with the character's findBone, vt+0x94; falls back to the left wrist), with GF_CIG_OFS="x,y,z"
 * (metres, joint space) and GF_CIG_ROT="rx,ry,rz" (degrees). GF_CIG_JOINT=<bone name>, GF_CIG=0 = no cigarette.
 * v1089L (user 00:28: "looks like the cigar, not placed right, doesn't follow the hand, doesn't look like it belongs; make the
 * smoking loop until stopped"): the palette rows carry the bone SCALE (|axis| ~1.9 at JO_pntB_L = bone 69 / slot 26) -> the mesh
 * was drawn 1.9x too big; axes are now orthonormalised. GF_CIG_DEBUG=1 colours it by its UVs (texture check), GF_CIG_DELAY=<n>
 * sends the matrix of n frames ago (if it runs ahead of the drawn hand). The clip LOOPS: when the layer ends by itself
 * (hash -> FFFFFFFF) it is started again; it only ends on J, sprint, combat, damage, a car, a cutscene, or the game putting
 * another clip on the layer.
 * v1090C (user: "it looks like ass"): (1) the cigarette hung off the fingertip along the joint's x axis (finger direction)
 *   like a pencil -- it is now GRIPPED between index and middle finger, crossing the hand, filter on the palm side and aimed
 *   at the lips during the clip's drag (measured: GF_CIG_DIAG=1 logs hand joints + lips in the joint frame); GF_CIG_GRIP /
 *   GF_CIG_DIR / GF_CIG_HOLD tune it. (2) the renderer drew it in the blood-decal pass, which indoors sits BEFORE a mid-pass
 *   depth clear -> the characters painted over it (invisible / cut off). It now has its own spot after the main pass's last
 *   character draw (rsx_d3d12_backend.c gfm_at, rsx_ui_overlay.c g_gfd_mode).
 * stop: func_004610F4(ent, layer) -> func_003CD354 (layer+0x138 = 0, +0x134 = -1: fade out) + func_003CD0EC.
 * Runs in gf_rag_post_skel (game thread, after the player's skeleton update).
 * Keys/env: J (GF_SMOKE_KEY=<letter>) starts / stops. GF_SMOKE=0 off; GF_SMOKE_ANIM=<name> another clip; GF_SMOKE_LAYER (0;
 *   -1 = the old main-controller mode); GF_SMOKE_BLEND (0.3 s); GF_SMOKE_SECS (60 s safety stop; the clip itself ends after ~19.8 s);
 *   GF_SMOKE_TEST=<s> auto start <s> s after the player first exists (soak tests). Log tag [smoke]. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "recompiled/ppu_recomp.h"

extern uint8_t* vm_base;
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
extern uint32_t gf_mission_hash(const char* s);
extern int gf_cutscene_active(void);
extern void gfm_set(int id, const float* m12, int on);   /* rsx_ui_overlay.c v1088M */
extern volatile int g_gf_force_walk, g_gf_cross_held, g_gf_combat_btn;   /* cellPad.c v1085W / v1087C */
extern int (*volatile g_pad_drawn_fn)(void);   /* cellPad.c / gf_combat.c: 1 = a weapon is out */
extern int gf_rag_bone_index(uint32_t ent, uint32_t anim, int b);   /* gf_ragdoll.c: 1 = lumbar, 2 = thoracic */
typedef void (*gfn)(ppu_context*);
#define TOC 0x00B4BF60u

static float rdf_(uint32_t ea);
static void wr32(uint32_t ea, uint32_t v);
static int ok(uint32_t ea) { return ea >= 0x10000u && ea < 0xF0000000u; }
static uint32_t rd32(uint32_t ea) { const uint8_t* p = vm_base + ea; return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static float rdf_(uint32_t ea) { uint32_t u = rd32(ea); float f; memcpy(&f, &u, 4); return f; }
static int fok_(float f) { return f == f && f > -1e7f && f < 1e7f; }
static void wr32(uint32_t ea, uint32_t v) { uint8_t* p = vm_base + ea; p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static double now_s(void) { static LARGE_INTEGER fq; LARGE_INTEGER t; if (!fq.QuadPart) QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t); return (double)t.QuadPart / (double)fq.QuadPart; }
static gfn lookup(uint32_t addr)
{
    for (uint64_t k = 0; k < function_table_count; k++) if ((uint32_t)function_table[k].addr == addr) return function_table[k].func;
    return 0;
}
/* call a guest function on the current guest thread: up to 8 integer args (r3..r10) + f1; 0 = no context / fault */
static int gcall(gfn f, const uint64_t* a, int n, double f1, uint64_t* ret)
{
    ppu_context* cur = g_active_ctx; if (!cur || !f) return 0;
    static ppu_context c;
    __try {
        memcpy(&c, cur, sizeof c);
        c.gpr[1] = ((uint32_t)cur->gpr[1] - 0x1400u) & ~0xFull;
        c.gpr[2] = TOC; for (int i = 0; i < n && i < 8; i++) c.gpr[3 + i] = a[i];
        c.fpr[1] = f1; c.lr = 0;
        g_active_ctx = &c;
        f(&c);
        while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&c); }
        g_active_ctx = cur;
        if (ret) *ret = c.gpr[3];
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_active_ctx = cur; g_trampoline_fn = 0; return 0; }
}
static uint16_t rd16_(uint32_t ea) { const uint8_t* p = vm_base + ea; return (uint16_t)((p[0] << 8) | p[1]); }
static int anim_nbones_(uint32_t anim)
{
    const uint32_t a = rd32(anim + 4); if (!ok(a)) return 0;
    const int cnt = (int)rd32(a + 0x18); const uint32_t base = rd32(a + 0x1C); const int i = (int16_t)rd16_(anim + 0xA);
    if (!ok(base) || i < 0 || i >= cnt) return 0;
    const int n = (int)rd32(base + (uint32_t)i * 20u + 4u); return (n > 0 && n <= 255) ? n : 0;
}
/* the character's findBone (vtable +0x94: (ent, int* out, const char* name)) -> palette slot of that bone, -1 = none */
static int bone_slot(uint32_t ent, uint32_t anim, const char* name, int* idx_out)
{
    ppu_context* cur = g_active_ctx; if (!cur) return -1;
    const uint32_t vt = rd32(ent); if (!ok(vt)) return -1; const uint32_t opd = rd32(vt + 0x94u); if (!ok(opd)) return -1;
    gfn fb = lookup(rd32(opd)); if (!fb) return -1;
    const uint32_t sbuf = ((uint32_t)cur->gpr[1] - 0x900u) & ~0xFu, out = sbuf + 0x40u;
    const size_t L = strlen(name); if (L > 60) return -1;
    memcpy(vm_base + sbuf, name, L + 1); wr32(out, 0xFFFFFFFFu);
    const uint64_t a[3] = { ent, out, sbuf };
    if (!gcall(fb, a, 3, 0.0, NULL)) return -1;
    const int ix = (int)rd32(out), nb = anim_nbones_(anim); const uint32_t tab = rd32(anim + 0x78u);
    if (idx_out) *idx_out = ix;
    if (ix < 0 || ix >= nb || !ok(tab)) return -1;
    const int sl = vm_base[tab + (uint32_t)ix]; return sl == 0xFF ? -1 : sl;
}
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
static uint32_t layer_obj(uint32_t anim, int layer)   /* func_003CD014's walk: layer 0 = *(anim+0x148) */
{
    uint32_t L = rd32(anim + 0x148u);
    for (int i = 0; i < layer && ok(L); i++) L = rd32(L + 0x148u);
    return ok(L) ? L : 0;
}

static int s_on = -1, s_key = 'J', s_nlog = 0, s_layer = 0, s_mask_bone = 1;   /* 1 lumbar, 2 thoracic, -1 none */ static float s_blend = 0.3f, s_test = 0.0f, s_secs = 60.0f;   /* the clip ends on its own after ~19.8 s (84_t5_smoke); this is only a safety stop */
static int s_cig_diag = 0; static int s_cig = 1, s_cig_slot = -2, s_cig_dbg = 0, s_cig_delay = 0, s_loops = 0; static float s_cig_ofs[3], s_cig_rot[3]; static float s_cig_grip[3] = { -0.010f, -0.002f, -0.012f }, s_cig_dir[3] = { -0.40f, 0.82f, -0.40f }, s_cig_hold = 0.033f;   /* v1090D (user: "push it further out"): 5.1 cm sticks out past the fingers (was 2.9) */   /* 96_t14: lips at (+0.022 -0.066 +0.019) during the drag -> filter aimed at them */ static char s_cig_joint[48] = "JO_pntB_L";
static int s_scan = 1, s_nscan = 0; static float s_hp0 = -1.0f;
static int s_walk = 90,   /* 86_t6_sweep: cap 64 = 0 m/s (turns on the spot), 80 / 95 = ~1.6 m/s walk, 110 = jog 3.3-3.5 m/s */ s_sweep[8], s_nsweep = 0;
static char s_name[64] = "ambient_smoke"; static uint32_t s_hash;
static gfn f_play_main, f_layer_blend, f_play_layer, f_stop_layer;
static struct { int active, key_prev, test_done, nchg; double t0, t_first; uint32_t last_seen; } S;
volatile int g_smoke_req = 0;   /* v1092S gf_shop.c: "use" a cigarette / cigar from the inventory */
extern int gf_inv_smoke_take(void);   /* v1092S gf_shop.c: takes one from the inventory; 0 = none left (it shows the toast) */
#define SLOG(...) do { if (s_nlog < 1500) { s_nlog++; printf(__VA_ARGS__); fflush(stdout); } } while (0)

static int key_down(void)
{
    HWND w = GetForegroundWindow(); DWORD pid = 0; if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId() && (GetAsyncKeyState(s_key) & 0x8000);
}
static int fg_ours(void) { HWND w = GetForegroundWindow(); DWORD pid = 0; if (w) GetWindowThreadProcessId(w, &pid); return pid == GetCurrentProcessId(); }
static int sprint_key(void) { return (GetAsyncKeyState(VK_LSHIFT) & 0x8000) || (GetAsyncKeyState(VK_RSHIFT) & 0x8000); }
static uint32_t cur_hash(uint32_t anim)
{
    if (s_layer < 0) return rd32(anim + 0x10u);
    const uint32_t L = layer_obj(anim, s_layer); return L ? rd32(L + 0x10u) : 0xFFFFFFFFu;
}
/* v1110V: 1 = LMB / RMB pressed AFTER both were up at least once since the last call chain started (the click that used the cigar in
 * the inventory is still held when the smoke starts -> "stopped (mouse attack / aim)" at +0.00 s); never while a port page is open */
static int mouse_fresh_press(void)
{
    extern volatile int g_uiov_menu_open; static int armed = 0; static ULONGLONG t_last;
    const ULONGLONG t = GetTickCount64(); if (t - t_last > 500) armed = 0; t_last = t;   /* a new smoke (or a gap) re-arms from "held" */
    const int down = ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000) != 0;
    if (g_uiov_menu_open || !fg_ours()) { armed = 0; return 0; }
    if (!down) { armed = 1; return 0; }
    return armed;
}

static void start(uint32_t pl, uint32_t anim)
{
    uint64_t r = 0; int okc;
    const uint32_t before = cur_hash(anim);
    if (s_layer < 0) {
        const uint64_t a[4] = { pl, s_hash, 1, 0 };
        okc = gcall(f_play_main, a, 4, 0.0, &r);
    } else {
        const uint64_t b[2] = { pl, (uint64_t)s_layer };
        gcall(f_layer_blend, b, 2, (double)s_blend, NULL);                        /* func_0046194C(ent, layer) f1 = blend */
        static uint32_t mask_anim; static int mask_idx = -1;
        if (mask_anim != anim) { mask_anim = anim; mask_idx = s_mask_bone > 0 ? gf_rag_bone_index(pl, anim, s_mask_bone) : -1;
            SLOG("[smoke] bone mask root: %s = bone %d%s\n", s_mask_bone == 2 ? "thoracic" : s_mask_bone == 1 ? "lumbar" : "none", mask_idx, mask_idx < 0 ? " (whole body)" : ""); }
        const uint64_t a[8] = { pl, s_hash, (uint64_t)s_layer, (uint64_t)(int64_t)mask_idx, 1, 1, 0, 1 };
        okc = gcall(f_play_layer, a, 8, (double)s_blend, &r);                    /* func_00460E0C(ent, hash, layer, -1, 1, 1, 0, 1) */
    }
    S.last_seen = cur_hash(anim);
    SLOG("[smoke] start \"%s\" %08X on %s (player %08X, anim %08X, layer obj %08X): call %s, returned %d, playing %08X -> %08X\n",
         s_name, s_hash, s_layer < 0 ? "the MAIN controller" : "upper-body layer", pl, anim, s_layer >= 0 ? layer_obj(anim, s_layer) : 0,
         okc ? "ok" : "FAILED", (int)(uint32_t)r, before, S.last_seen);
    S.active = okc && r; S.t0 = now_s(); S.nchg = 0; s_hp0 = rdf_(pl + 0x14C8u);
    g_gf_force_walk = S.active ? (s_nsweep ? s_sweep[0] : s_walk) : 0;   /* v1085W / v1086W: walk while smoking (cap) */
}
extern int gf_rag_ent_frame(uint32_t ent, float* M9, float* T3);
extern int gf_rag_bone_slots(uint32_t ent, uint32_t anim, int* slot21, int* nslot);
/* v1088M: put the cigarette on the attach joint (palette slot, model space) -> world (model = M (w - T)) */
static void fx_hand(uint32_t pl, uint32_t anim, const float* m12);   /* v1093M, below */
static void cig_place(uint32_t pl, uint32_t anim)
{
    if (!s_cig) return;
    if (s_cig_slot == -2) {   /* once: find the joint */
        int ix = -1; s_cig_slot = bone_slot(pl, anim, s_cig_joint, &ix);
        SLOG("[smoke] cigarette joint %s: bone %d, palette slot %d\n", s_cig_joint, ix, s_cig_slot);
        if (s_cig_slot < 0) { int sl[21], ns = 0;
            if (gf_rag_bone_slots(pl, anim, sl, &ns) && sl[7] >= 0) { s_cig_slot = sl[7]; SLOG("[smoke] cigarette: falling back to the left wrist (slot %d)\n", s_cig_slot); }
            else { s_cig_slot = -1; SLOG("[smoke] cigarette: no joint -> no cigarette\n"); } }
    }
    if (s_cig_slot < 0) return;
    const uint32_t pal = rd32(rd32(anim + 0x74u) + 8u); if (!ok(pal)) return;
    float M[9], T[3]; if (!gf_rag_ent_frame(pl, M, T)) return;
    for (int r = 0; r < 3; r++) { const float l = sqrtf(M[r * 3] * M[r * 3] + M[r * 3 + 1] * M[r * 3 + 1] + M[r * 3 + 2] * M[r * 3 + 2]); if (l > 1e-6f) for (int c = 0; c < 3; c++) M[r * 3 + c] /= l; }
    const uint32_t b = pal + (uint32_t)s_cig_slot * 64u;
    float Rb[3][3], P[3];
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) Rb[r][c] = rdf_(b + (uint32_t)r * 16u + (uint32_t)c * 4u);
    for (int c = 0; c < 3; c++) P[c] = rdf_(b + 48u + (uint32_t)c * 4u);
    {   /* v1089L: the rows carry the bone scale -> Gram-Schmidt to a pure rotation */
        float l0 = sqrtf(Rb[0][0] * Rb[0][0] + Rb[0][1] * Rb[0][1] + Rb[0][2] * Rb[0][2]); if (!(l0 > 1e-6f)) return;
        for (int c = 0; c < 3; c++) Rb[0][c] /= l0;
        const float d01 = Rb[1][0] * Rb[0][0] + Rb[1][1] * Rb[0][1] + Rb[1][2] * Rb[0][2];
        for (int c = 0; c < 3; c++) Rb[1][c] -= d01 * Rb[0][c];
        float l1 = sqrtf(Rb[1][0] * Rb[1][0] + Rb[1][1] * Rb[1][1] + Rb[1][2] * Rb[1][2]); if (!(l1 > 1e-6f)) return;
        for (int c = 0; c < 3; c++) Rb[1][c] /= l1;
        const float sgn = (Rb[2][0] * (Rb[0][1] * Rb[1][2] - Rb[0][2] * Rb[1][1]) + Rb[2][1] * (Rb[0][2] * Rb[1][0] - Rb[0][0] * Rb[1][2]) + Rb[2][2] * (Rb[0][0] * Rb[1][1] - Rb[0][1] * Rb[1][0])) < 0 ? -1.0f : 1.0f;
        Rb[2][0] = sgn * (Rb[0][1] * Rb[1][2] - Rb[0][2] * Rb[1][1]); Rb[2][1] = sgn * (Rb[0][2] * Rb[1][0] - Rb[0][0] * Rb[1][2]); Rb[2][2] = sgn * (Rb[0][0] * Rb[1][1] - Rb[0][1] * Rb[1][0]);
    }
    if (s_cig_diag) {   /* v1090C diag: hand joints + mouth in the attach joint's frame (local = Rb (q - P)) */
        static int mouth_sl = -2, n_once = 0; static double t_d;
        if (mouth_sl == -2) { int ix = -1; mouth_sl = bone_slot(pl, anim, "JO_mandible", &ix); if (mouth_sl < 0) mouth_sl = bone_slot(pl, anim, "JO_orbOris_BR", &ix);
            SLOG("[cigdiag] mouth slot %d\n", mouth_sl); }
        int sl[21], ns = 0; const int have = gf_rag_bone_slots(pl, anim, sl, &ns);
        #define LOC(slot, o) do { float q_[3]; for (int c_ = 0; c_ < 3; c_++) q_[c_] = rdf_(pal + (uint32_t)(slot) * 64u + 48u + (uint32_t)c_ * 4u) - P[c_]; \
            for (int r_ = 0; r_ < 3; r_++) (o)[r_] = Rb[r_][0] * q_[0] + Rb[r_][1] * q_[1] + Rb[r_][2] * q_[2]; } while (0)
        if (have && n_once < 1) { n_once++;
            float wl[3]; LOC(sl[7], wl);
            SLOG("[cigdiag] wrist_L slot %d local (%.3f %.3f %.3f); nslot %d\n", sl[7], wl[0], wl[1], wl[2], ns);
            for (int k = 0; k < ns && k < 128; k++) { float o[3]; LOC(k, o); const float d = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
                if (d < 0.13f) { float dw[3] = { o[0] - wl[0], o[1] - wl[1], o[2] - wl[2] };
                    SLOG("[cigdiag]   slot %2d local (%+.3f %+.3f %+.3f) d %.3f  from wrist %.3f\n", k, o[0], o[1], o[2], d, sqrtf(dw[0] * dw[0] + dw[1] * dw[1] + dw[2] * dw[2])); } } }
        if (have && mouth_sl >= 0 && now_s() - t_d > 0.2) { t_d = now_s(); float wl[3], ml[3], el[3]; LOC(sl[7], wl); LOC(mouth_sl, ml); LOC(sl[6], el);
            SLOG("[cigdiag] t %.1f wrist (%+.3f %+.3f %+.3f) elbow (%+.3f %+.3f %+.3f) mouth (%+.3f %+.3f %+.3f) |mouth| %.3f\n", now_s() - S.t0,
                 wl[0], wl[1], wl[2], el[0], el[1], el[2], ml[0], ml[1], ml[2], sqrtf(ml[0] * ml[0] + ml[1] * ml[1] + ml[2] * ml[2])); }
        #undef LOC
    }
    /* v1090C: the cigarette is GRIPPED between the index and middle fingers and crosses the hand palm -> back, filter on the
     * palm side (so the clip's hand-to-mouth brings the filter to the lips). 94_t12_cigdiag measured the JO_pntB_L frame: +x =
     * along the index finger to the tip (wrist at -0.117), +z = thumb side, -z = middle-finger side, -y = palm (fingers curl
     * and the thumb tip sit at -y; the mouth comes in from -y). Mesh: filter end at x = 0, lit end at x = +0.084.
     * GF_CIG_GRIP = grip point, GF_CIG_DIR = filter -> tip direction (joint space), GF_CIG_HOLD = grip distance from the
     * filter end; GF_CIG_ROT (degrees) still rotates the result for live tuning. */
    float Q[3][3];
    {
        float d[3] = { s_cig_dir[0], s_cig_dir[1], s_cig_dir[2] };
        float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]); if (!(l > 1e-6f)) { d[0] = 0; d[1] = 1; d[2] = 0; l = 1; }
        for (int c = 0; c < 3; c++) d[c] /= l;
        if (s_cig_rot[0] != 0 || s_cig_rot[1] != 0 || s_cig_rot[2] != 0) {   /* extra Euler X, Y, Z tweak */
            const float d2r = 0.01745329f; float a[3] = { s_cig_rot[0] * d2r, s_cig_rot[1] * d2r, s_cig_rot[2] * d2r };
            float t;
            t = d[1] * cosf(a[0]) - d[2] * sinf(a[0]); d[2] = d[1] * sinf(a[0]) + d[2] * cosf(a[0]); d[1] = t;
            t = d[0] * cosf(a[1]) + d[2] * sinf(a[1]); d[2] = -d[0] * sinf(a[1]) + d[2] * cosf(a[1]); d[0] = t;
            t = d[0] * cosf(a[2]) - d[1] * sinf(a[2]); d[1] = d[0] * sinf(a[2]) + d[1] * cosf(a[2]); d[0] = t;
        }
        const float up[3] = { 1, 0, 0 };   /* roll reference: along the finger */
        float b[3] = { d[1] * up[2] - d[2] * up[1], d[2] * up[0] - d[0] * up[2], d[0] * up[1] - d[1] * up[0] };
        float lb = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]); if (!(lb > 1e-4f)) { b[0] = 0; b[1] = 0; b[2] = 1; lb = 1; }
        for (int c = 0; c < 3; c++) b[c] /= lb;
        const float e[3] = { d[1] * b[2] - d[2] * b[1], d[2] * b[0] - d[0] * b[2], d[0] * b[1] - d[1] * b[0] };
        for (int c = 0; c < 3; c++) { Q[0][c] = d[c]; Q[1][c] = b[c]; Q[2][c] = e[c]; }
        for (int c = 0; c < 3; c++) s_cig_ofs[c] = s_cig_grip[c] - s_cig_hold * d[c];   /* mesh origin = the filter end */
    }
    if (s_cig_diag) {   /* lips relative to the filter end (joint space) */
        static int lips[2] = { -2, -2 }; static double t_l; static float best = 9;
        if (lips[0] == -2) { int ix; lips[0] = bone_slot(pl, anim, "JO_orbOris_CL", &ix); lips[1] = bone_slot(pl, anim, "JO_orbOris_CR", &ix); SLOG("[cigdiag] lip slots %d %d\n", lips[0], lips[1]); }
        if (lips[0] >= 0 && lips[1] >= 0) {
            float q[3], o[3]; for (int c = 0; c < 3; c++) q[c] = 0.5f * (rdf_(pal + (uint32_t)lips[0] * 64u + 48u + (uint32_t)c * 4u) + rdf_(pal + (uint32_t)lips[1] * 64u + 48u + (uint32_t)c * 4u)) - P[c];
            for (int r = 0; r < 3; r++) o[r] = Rb[r][0] * q[0] + Rb[r][1] * q[1] + Rb[r][2] * q[2];
            const float f[3] = { o[0] - s_cig_ofs[0], o[1] - s_cig_ofs[1], o[2] - s_cig_ofs[2] }, df = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
            if (df < best - 0.002f || now_s() - t_l > 0.5) { if (df < best) best = df; t_l = now_s();
                SLOG("[cigdiag] t %.1f lips (%+.3f %+.3f %+.3f), filter->lips (%+.3f %+.3f %+.3f) %.3f m (best %.3f)\n", now_s() - S.t0, o[0], o[1], o[2], f[0], f[1], f[2], df, best); }
        }
    }
    float m12[12];
    for (int k = 0; k < 3; k++) {   /* axis k: joint-space Q[k] -> model -> world (w = M^T v) */
        float vm[3]; for (int c = 0; c < 3; c++) vm[c] = Q[k][0] * Rb[0][c] + Q[k][1] * Rb[1][c] + Q[k][2] * Rb[2][c];
        for (int c = 0; c < 3; c++) m12[k * 3 + c] = M[0 * 3 + c] * vm[0] + M[1 * 3 + c] * vm[1] + M[2 * 3 + c] * vm[2];
    }
    float pm[3]; for (int c = 0; c < 3; c++) pm[c] = P[c] + s_cig_ofs[0] * Rb[0][c] + s_cig_ofs[1] * Rb[1][c] + s_cig_ofs[2] * Rb[2][c];
    for (int c = 0; c < 3; c++) m12[9 + c] = T[c] + M[0 * 3 + c] * pm[0] + M[1 * 3 + c] * pm[1] + M[2 * 3 + c] * pm[2];
    static float ring[8][12]; static int rh = 0, rn = 0;
    memcpy(ring[rh], m12, sizeof m12); rh = (rh + 1) & 7; if (rn < 8) rn++;
    const int back = s_cig_delay < rn ? s_cig_delay : rn - 1;
    gfm_set(s_cig_dbg ? 1 : 0, ring[(rh - 1 - back) & 7], 1);
    fx_hand(pl, anim, m12);   /* v1093M wisp + exhale after a drag */
    static int nl = 0; if (nl < 3) { nl++; SLOG("[smoke] cigarette at (%.2f %.2f %.2f), axis (%.2f %.2f %.2f)\n", m12[9], m12[10], m12[11], m12[0], m12[1], m12[2]); }
}
/* ------------------------------------------------------------------ v1093M smoke fx + cigarette in the mouth
 * User: "make the player smoke more from the cigarette, and a way to just have it in his mouth for aesthetic purposes, so it
 * can still be there in a gunfight". Our own smoke (rsx_ui_overlay.c gfs_add, drawn after the characters): a thin wisp off
 * the lit end all the time, and an exhale cloud from the mouth after every drag (the filter at the lips for > 0.3 s, then the
 * hand leaves). Mouth mode (K, GF_CIG_MOUTH_KEY, or the inventory): the cigarette sits in the lips at a slight downward
 * angle, off-centre, until K again or it burns out (GF_CIG_MOUTH_SECS, 360); it puffs every 8-13 s. J while it is in the
 * mouth takes it into the hand for drags (no new cigarette); when the smoke ends (sprint / combat / damage / J) it is back in
 * the mouth. Mouth frame from the lip corners JO_orbOris_CL/CR, eyes JO_eye_L/R and JO_neck (findBone).
 * GF_SMOKE_FX = amount (1; 0 = off). */
extern void gfs_add(const float* p, const float* v, float s0, float s1, float life, float alpha);
extern void gf_shop_toast(const char* msg);
volatile int g_cig_mouth_req = 0, g_cig_mouth_on = 0;   /* gf_shop.c inventory row */
static int s_mouth = 0, s_mkey = 'K', s_mkey_prev = 0; static double s_mouth_t0, s_mouth_puff; static float s_mouth_secs = 360.0f, s_fx = 1.0f;
static uint32_t s_m_anim; static int s_m_sl[5] = { -2, -2, -2, -2, -2 };   /* CL CR eyeL eyeR neck */
static float frand_(void) { return (float)rand() / (float)RAND_MAX; }
static void wpt(uint32_t pal, int slot, const float* M, const float* T, float* w)   /* palette slot origin -> world */
{
    const uint32_t b = pal + (uint32_t)slot * 64u + 48u; const float p0 = rdf_(b), p1 = rdf_(b + 4), p2 = rdf_(b + 8);
    for (int c = 0; c < 3; c++) w[c] = T[c] + M[0 * 3 + c] * p0 + M[1 * 3 + c] * p1 + M[2 * 3 + c] * p2;
}
static void nrm3(float* v) { const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; } }
/* mouth centre + face axes (world): fwd out of the face, up to the eyes, lat to the right lip corner. 0 = not found */
static int mouth_frame(uint32_t pl, uint32_t anim, float* mc, float* fwd, float* up, float* lat)
{
    if (s_m_anim != anim) {
        s_m_anim = anim; int ix; static const char* const nm[5] = { "JO_orbOris_CL", "JO_orbOris_CR", "JO_eye_L", "JO_eye_R", "JO_neck" };
        for (int k = 0; k < 5; k++) s_m_sl[k] = bone_slot(pl, anim, nm[k], &ix);
        SLOG("[smoke] v1093M mouth joints: lips %d %d, eyes %d %d, neck %d\n", s_m_sl[0], s_m_sl[1], s_m_sl[2], s_m_sl[3], s_m_sl[4]);
    }
    for (int k = 0; k < 5; k++) if (s_m_sl[k] < 0) return 0;
    const uint32_t pal = rd32(rd32(anim + 0x74u) + 8u); if (!ok(pal)) return 0;
    float M[9], T[3]; if (!gf_rag_ent_frame(pl, M, T)) return 0;
    for (int r = 0; r < 3; r++) { const float l = sqrtf(M[r * 3] * M[r * 3] + M[r * 3 + 1] * M[r * 3 + 1] + M[r * 3 + 2] * M[r * 3 + 2]); if (l > 1e-6f) for (int c = 0; c < 3; c++) M[r * 3 + c] /= l; }
    float cl[3], cr[3], el[3], er[3], nk[3];
    wpt(pal, s_m_sl[0], M, T, cl); wpt(pal, s_m_sl[1], M, T, cr); wpt(pal, s_m_sl[2], M, T, el); wpt(pal, s_m_sl[3], M, T, er); wpt(pal, s_m_sl[4], M, T, nk);
    for (int c = 0; c < 3; c++) { mc[c] = 0.5f * (cl[c] + cr[c]); lat[c] = cr[c] - cl[c]; up[c] = 0.5f * (el[c] + er[c]) - mc[c]; }
    nrm3(lat); { const float d = up[0] * lat[0] + up[1] * lat[1] + up[2] * lat[2]; for (int c = 0; c < 3; c++) up[c] -= d * lat[c]; } nrm3(up);
    fwd[0] = lat[1] * up[2] - lat[2] * up[1]; fwd[1] = lat[2] * up[0] - lat[0] * up[2]; fwd[2] = lat[0] * up[1] - lat[1] * up[0];
    if ((mc[0] - nk[0]) * fwd[0] + (mc[1] - nk[1]) * fwd[1] + (mc[2] - nk[2]) * fwd[2] < 0) for (int c = 0; c < 3; c++) fwd[c] = -fwd[c];
    return fok_(mc[0]) && fok_(fwd[0]) && fok_(up[1]);
}
static void fx_wisp(const float* tip, double now)   /* thin smoke off the lit end */
{
    static double acc, last; const double dt = last > 0 ? now - last : 0; last = now;
    if (s_fx <= 0 || dt <= 0 || dt > 0.2) return;
    acc += dt * 9.0 * s_fx;
    while (acc >= 1.0) { acc -= 1.0;
        const float v[3] = { (frand_() - 0.5f) * 0.03f, 0.04f + frand_() * 0.04f, (frand_() - 0.5f) * 0.03f };
        gfs_add(tip, v, 0.005f, 0.05f + frand_() * 0.03f, 2.0f + frand_() * 1.0f, 0.16f); }
}
static double s_exh_t0 = 0, s_exh_len = 0; static float s_exh_scale = 1.0f;
static void fx_exhale_start(double now, double delay, double len, float scale) { s_exh_t0 = now + delay; s_exh_len = len; s_exh_scale = scale; }
static void fx_exhale(const float* mc, const float* fwd, const float* up, double now)   /* the cloud out of the mouth */
{
    static double acc, last; const double dt = last > 0 ? now - last : 0; last = now;
    if (s_fx <= 0 || dt <= 0 || dt > 0.2 || now < s_exh_t0 || now > s_exh_t0 + s_exh_len) return;
    const float k = (float)((now - s_exh_t0) / s_exh_len), strength = 1.0f - 0.6f * k;   /* strongest first */
    acc += dt * 22.0 * s_fx * s_exh_scale;
    while (acc >= 1.0) { acc -= 1.0;
        const float sp = (0.45f + 0.35f * frand_()) * strength;
        float v[3]; for (int c = 0; c < 3; c++) v[c] = fwd[c] * sp + up[c] * (0.02f + 0.08f * frand_()) + (frand_() - 0.5f) * 0.14f;
        float p[3]; for (int c = 0; c < 3; c++) p[c] = mc[c] + fwd[c] * 0.03f;
        gfs_add(p, v, 0.02f, (0.20f + 0.14f * frand_()) * s_exh_scale, 2.6f + frand_(), 0.24f); }
}
/* hand mode: the filter end (m12 origin) near the lips > 0.3 s, then away -> exhale */
static void fx_hand(uint32_t pl, uint32_t anim, const float* m12)
{
    const double now = now_s();
    const float tip[3] = { m12[9] + m12[0] * 0.084f, m12[10] + m12[1] * 0.084f, m12[11] + m12[2] * 0.084f };
    fx_wisp(tip, now);
    float mc[3], fw[3], up[3], lat[3]; if (!mouth_frame(pl, anim, mc, fw, up, lat)) return;
    static int at = 0; static double t_at = 0;
    const float dx = m12[9] - mc[0], dy = m12[10] - mc[1], dz = m12[11] - mc[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
    if (d < 0.11f) { if (!at) { at = 1; t_at = now; } }
    else if (at && d > 0.15f) { at = 0; if (now - t_at > 0.3) { fx_exhale_start(now, 0.35, 1.5, 1.2f); SLOG("[smoke] v1093M drag (%.1f s at the lips) -> exhale\n", now - t_at); } }
    fx_exhale(mc, fw, up, now);
}
/* mouth mode: place the cigarette in the lips + wisp + a puff now and then */
static void cig_mouth(uint32_t pl, uint32_t anim)
{
    float mc[3], fw[3], up[3], lat[3]; if (!mouth_frame(pl, anim, mc, fw, up, lat)) return;
    float d[3]; for (int c = 0; c < 3; c++) d[c] = fw[c] * 0.88f - up[c] * 0.42f + lat[c] * 0.18f; nrm3(d);   /* down and off to one side */
    float y[3] = { d[1] * up[2] - d[2] * up[1], d[2] * up[0] - d[0] * up[2], d[0] * up[1] - d[1] * up[0] }; nrm3(y);
    const float z[3] = { d[1] * y[2] - d[2] * y[1], d[2] * y[0] - d[0] * y[2], d[0] * y[1] - d[1] * y[0] };
    float m12[12]; for (int c = 0; c < 3; c++) { m12[c] = d[c]; m12[3 + c] = y[c]; m12[6 + c] = z[c]; m12[9 + c] = mc[c] - fw[c] * 0.006f + lat[c] * 0.012f - up[c] * 0.004f; }
    gfm_set(0, m12, 1);
    const double now = now_s();
    const float tip[3] = { m12[9] + d[0] * 0.084f, m12[10] + d[1] * 0.084f, m12[11] + d[2] * 0.084f };
    fx_wisp(tip, now);
    if (now >= s_mouth_puff) { s_mouth_puff = now + 8.0 + 5.0 * frand_(); fx_exhale_start(now, 0.9, 0.9, 0.7f); }
    fx_exhale(mc, fw, up, now);
}
static void stop(uint32_t pl, uint32_t anim, const char* why);
static void mouth_toggle(uint32_t pl, uint32_t anim)
{
    if (s_mouth) { s_mouth = 0; g_cig_mouth_on = 0; if (!S.active) gfm_set(0, NULL, 0); gf_shop_toast("Flicked the cigarette away"); SLOG("[smoke] v1093M mouth: off\n"); return; }
    if (!S.active && !gf_inv_smoke_take()) return;   /* smoking already = that cigarette goes to the mouth */
    s_mouth = 1; g_cig_mouth_on = 1; s_mouth_t0 = now_s(); s_mouth_puff = now_s() + 4.0;
    if (S.active) stop(pl, anim, "K: into the mouth");   /* v1093M-2 (user: "said in your mouth while it's still in my hand"): end the smoke now */
    gf_shop_toast("Cigarette in your mouth - K to put it out"); SLOG("[smoke] v1093M mouth: on\n");
}
static void stop(uint32_t pl, uint32_t anim, const char* why)
{
    if (!s_mouth) gfm_set(0, NULL, 0);   /* v1093M: in mouth mode it goes back to the lips next frame */
    if (s_layer >= 0 && cur_hash(anim) == s_hash) { const uint64_t a[2] = { pl, (uint64_t)s_layer }; gcall(f_stop_layer, a, 2, 0.0, NULL); }
    SLOG("[smoke] +%.2f s: stopped (%s), layer now %08X\n", now_s() - S.t0, why, cur_hash(anim));
    S.active = 0; g_gf_force_walk = 0;
}

#define SCLOG(...) do { if (s_nscan < 150) { s_nscan++; printf(__VA_ARGS__); fflush(stdout); } } while (0)
/* v1087C-2: 88_t7_scan found 3 NPCs playing ambient_smoke with an EMPTY +0x17E0 handle -> the living-world behaviour holds the
 * cigarette elsewhere. Prop instances (func_0029B9F0) have vtable 0x10044CE8 and their template GUID at +0xC4; position at
 * *(prop+0x68)+0x40 like any entity. 3 s and 8 s after an NPC starts smoking, scan the heap (0x40000000-0x44000000) for prop
 * instances within 1.5 m of it and log GUID + distance -> the cigarette's template GUID. */
extern int gf_rag_bone_slots(uint32_t ent, uint32_t anim, int* slot21, int* nslot);
extern int gf_rag_ent_frame(uint32_t ent, float* M9, float* T3);
/* v1087C-3: 89_t8_scan -- 0 class-10044CE8 props near 4 smokers (only 15-27 of them in the heap: pickups). The living-world
 * cigarette is another kind of object -> scan the heap for ANY object with a game vtable (0x10000000-0x10200000) whose frame
 * (*(obj+0x68)+0x40) is within 0.5 m of the smoker's wrists; log each vtable once with the distance. */
static void heap_hand(uint32_t npc, uint32_t anim, const char* tag)
{
    int sl[21], ns = 0; float M[9], T[3];
    if (!gf_rag_bone_slots(npc, anim, sl, &ns) || !gf_rag_ent_frame(npc, M, T)) { SCLOG("[smoke-scan]   %s NPC %08X: no bones\n", tag, npc); return; }
    const uint32_t pal = rd32(rd32(anim + 0x74u) + 8u); if (!ok(pal)) return;
    float hw[2][3];
    for (int k = 0; k < 2; k++) { const int s2 = sl[k ? 10 : 7]; if (s2 < 0) return;   /* WRR / WRL */
        const float px = rdf_(pal + (uint32_t)s2 * 64u + 0x30), py = rdf_(pal + (uint32_t)s2 * 64u + 0x34), pz = rdf_(pal + (uint32_t)s2 * 64u + 0x38);
        for (int a = 0; a < 3; a++) hw[k][a] = T[a] + M[0 * 3 + a] * px + M[1 * 3 + a] * py + M[2 * 3 + a] * pz; }   /* w = T + M^T p */
    static uint32_t vts[64]; static int nv = 0; int found = 0;
    __try {
        for (uint32_t ea = 0x40000000u; ea < 0x44000000u; ea += 16u) {
            const uint32_t vt = rd32(ea); if (vt < 0x10000000u || vt >= 0x10200000u) continue;
            const uint32_t f = rd32(ea + 0x68u); if (!ok(f) || f >= 0x50000000u) continue;
            const float x = rdf_(f + 0x40), y = rdf_(f + 0x44), z = rdf_(f + 0x48);
            float best = 99.0f; int bk = 0;
            for (int k = 0; k < 2; k++) { const float d = sqrtf((x - hw[k][0]) * (x - hw[k][0]) + (y - hw[k][1]) * (y - hw[k][1]) + (z - hw[k][2]) * (z - hw[k][2])); if (d < best) { best = d; bk = k; } }
            if (!(best < 0.5f) || ea == npc) continue;
            found++;
            int k; for (k = 0; k < nv && vts[k] != vt; k++) ;
            if (k < nv) continue; if (nv < 64) vts[nv++] = vt;
            SCLOG("[smoke-scan]   %s NPC %08X: obj %08X vt %08X %.2f m from the %s wrist | +C4 %08X %08X %08X %08X | +00..1C %08X %08X %08X %08X %08X %08X %08X %08X\n",
                  tag, npc, ea, vt, best, bk ? "right" : "left", rd32(ea + 0xC4), rd32(ea + 0xC8), rd32(ea + 0xCC), rd32(ea + 0xD0),
                  rd32(ea), rd32(ea + 4), rd32(ea + 8), rd32(ea + 12), rd32(ea + 16), rd32(ea + 20), rd32(ea + 24), rd32(ea + 28));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { SCLOG("[smoke-scan]   hand scan fault\n"); return; }
    SCLOG("[smoke-scan]   %s NPC %08X: %d objects within 0.5 m of a wrist (L %.1f %.1f %.1f)\n", tag, npc, found, hw[0][0], hw[0][1], hw[0][2]);
}
static int heap_near(uint32_t npc, const char* tag)
{
    const uint32_t nf = rd32(npc + 0x68u); if (!ok(nf)) return 0;
    const float nx = rdf_(nf + 0x40), ny = rdf_(nf + 0x44), nz = rdf_(nf + 0x48);
    int found = 0, scanned = 0;
    __try {
        for (uint32_t ea = 0x40000000u; ea < 0x44000000u; ea += 4u) {
            if (rd32(ea) != 0x10044CE8u) continue;
            scanned++;
            const uint32_t f = rd32(ea + 0x68u); if (!ok(f)) continue;
            const float x = rdf_(f + 0x40), y = rdf_(f + 0x44), z = rdf_(f + 0x48);
            const float d = sqrtf((x - nx) * (x - nx) + (y - ny) * (y - ny) + (z - nz) * (z - nz));
            if (!(d < 1.5f)) continue;
            found++;
            if (found <= 12) SCLOG("[smoke-scan]   %s NPC %08X: prop %08X %.2f m away (dy %+.2f) GUID@C4 %08X %08X %08X %08X\n", tag, npc, ea, d, y - ny,
                                   rd32(ea + 0xC4), rd32(ea + 0xC8), rd32(ea + 0xCC), rd32(ea + 0xD0));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { SCLOG("[smoke-scan]   heap scan fault (%d props seen)\n", scanned); return -1; }
    SCLOG("[smoke-scan]   %s NPC %08X at (%.1f %.1f %.1f): %d of %d prop instances within 1.5 m\n", tag, npc, nx, ny, nz, found, scanned);
    return found;
}
static void prop_scan(uint32_t anim, uint32_t ent, uint32_t pl)
{
    static uint32_t seen_g[96]; static int nseen = 0; static uint32_t smoke_ent[64]; static double smoke_t[64]; static int smoke_done[64]; static int nsm = 0;
    if (ent == pl) {
        static int done = 0; if (done) return; done = 1;
        char b[600]; int o = 0; const uint32_t idx = rd32(pl + 0x2A9Cu);
        for (int i = 0; i < 8 && o < 560; i++) { const uint32_t e = pl + 0x2698u + (uint32_t)i * 16u;
            o += snprintf(b + o, sizeof b - o, " [%d] %08X %08X %08X %08X", i, rd32(e), rd32(e + 4), rd32(e + 8), rd32(e + 12)); }
        SCLOG("[smoke-scan] player %08X prop slots (current index %u):%s | holder +0x17E0 = %08X\n", pl, idx, b, rd32(pl + 0x17E0u));
        return;
    }
    {   /* an NPC playing the smoke clip (main controller or upper-body layer) */
        const uint32_t L = layer_obj(anim, 0);
        if (rd32(anim + 0x10u) == s_hash || (L && rd32(L + 0x10u) == s_hash)) {
            int k; for (k = 0; k < nsm && smoke_ent[k] != ent; k++) ;
            if (k == nsm && nsm < 64) { smoke_t[nsm] = now_s(); smoke_done[nsm] = 0; smoke_ent[nsm++] = ent; SCLOG("[smoke-scan] NPC %08X is SMOKING (%s), prop handle +0x17E0 = %08X\n", ent, rd32(anim + 0x10u) == s_hash ? "main" : "layer 0", rd32(ent + 0x17E0u)); }
        }
    }
    {   /* heap scans 3 s and 8 s after an NPC started smoking (first 4 smokers) */
        int k; for (k = 0; k < nsm && smoke_ent[k] != ent; k++) ;
        if (k < nsm && k < 4) { const double dt = now_s() - smoke_t[k];
            if (smoke_done[k] == 0 && dt > 3.0) { smoke_done[k] = 1; heap_hand(ent, anim, "+3s"); }
            else if (smoke_done[k] == 1 && dt > 8.0) { smoke_done[k] = 2; heap_hand(ent, anim, "+8s"); } }
    }
    return;   /* v1087C-2: the +0x17E0 dump below gave junk for NPCs (not a prop handle on them) */
    const uint32_t h = rd32(ent + 0x17E0u); if (!ok(h) || h < 0x38u) return;
    const uint32_t prop = h - 0x38u; if (!ok(prop) || !ok(rd32(prop))) return;
    uint32_t g[4]; for (int i = 0; i < 4; i++) g[i] = rd32(prop + 0xC4u + (uint32_t)i * 4u);
    const uint32_t key = (g[0] * 2654435761u) ^ g[1] ^ (g[2] << 7) ^ g[3] ^ rd32(prop);
    for (int k = 0; k < nseen; k++) if (seen_g[k] == key) return;
    if (nseen < 96) seen_g[nseen++] = key; else return;
    char b[900]; int o = 0;
    for (uint32_t w = 0; w < 0x100u && o < 860; w += 4) o += snprintf(b + o, sizeof b - o, "%s%08X", (w & 0x1F) ? " " : (w ? " | " : ""), rd32(prop + w));
    SCLOG("[smoke-scan] NPC %08X holds prop %08X vt %08X GUID@C4 %08X %08X %08X %08X%s\n[smoke-scan]   %s\n", ent, prop, rd32(prop), g[0], g[1], g[2], g[3],
          (rd32(anim + 0x10u) == s_hash) ? " (SMOKING)" : "", b);
}

void gf_smoke_post_skel(uint32_t anim, uint32_t ent)
{
    if (s_on < 0) {
        const char* e = getenv("GF_SMOKE"); s_on = (e && *e == '0') ? 0 : 1;
        e = getenv("GF_SMOKE_KEY"); if (e && isalnum((unsigned char)e[0])) s_key = toupper((unsigned char)e[0]);
        e = getenv("GF_SMOKE_ANIM"); if (e && *e) { strncpy(s_name, e, sizeof s_name - 1); s_name[sizeof s_name - 1] = 0; }
        e = getenv("GF_SMOKE_BLEND"); if (e) { s_blend = (float)atof(e); if (s_blend < 0) s_blend = 0; if (s_blend > 2) s_blend = 2; }
        e = getenv("GF_SMOKE_SECS"); if (e) { s_secs = (float)atof(e); if (s_secs < 1) s_secs = 1; if (s_secs > 120) s_secs = 120; }
        e = getenv("GF_SMOKE_LAYER"); if (e) { s_layer = atoi(e); if (s_layer > 4) s_layer = 4; if (s_layer < -1) s_layer = -1; }
        e = getenv("GF_SMOKE_TEST"); if (e) s_test = (float)atof(e);
        e = getenv("GF_SMOKE_SCAN"); if (e && *e == '0') s_scan = 0;
        e = getenv("GF_CIG"); if (e && *e == '0') s_cig = 0;
        e = getenv("GF_CIG_DIAG"); if (e && *e == '1') s_cig_diag = 1;
        e = getenv("GF_CIG_DEBUG"); if (e && *e == '1') s_cig_dbg = 1;
        e = getenv("GF_CIG_DELAY"); if (e) { s_cig_delay = atoi(e); if (s_cig_delay < 0) s_cig_delay = 0; if (s_cig_delay > 7) s_cig_delay = 7; }
        e = getenv("GF_CIG_JOINT"); if (e && *e) { strncpy(s_cig_joint, e, sizeof s_cig_joint - 1); s_cig_joint[sizeof s_cig_joint - 1] = 0; }
        e = getenv("GF_CIG_GRIP"); if (e) sscanf(e, "%f,%f,%f", &s_cig_grip[0], &s_cig_grip[1], &s_cig_grip[2]);
        e = getenv("GF_CIG_DIR"); if (e) sscanf(e, "%f,%f,%f", &s_cig_dir[0], &s_cig_dir[1], &s_cig_dir[2]);
        e = getenv("GF_CIG_HOLD"); if (e) { s_cig_hold = (float)atof(e); if (s_cig_hold < 0) s_cig_hold = 0; if (s_cig_hold > 0.084f) s_cig_hold = 0.084f; }
        e = getenv("GF_CIG_ROT"); if (e) sscanf(e, "%f,%f,%f", &s_cig_rot[0], &s_cig_rot[1], &s_cig_rot[2]);
        e = getenv("GF_SMOKE_WALK"); if (e) { s_walk = atoi(e); if (s_walk < 20) s_walk = 20; if (s_walk > 127) s_walk = 127; }
        e = getenv("GF_SMOKE_WALK_SWEEP"); if (e) { const char* q = e; while (*q && s_nsweep < 8) { const int v = atoi(q); if (v >= 20 && v <= 127) s_sweep[s_nsweep++] = v; q = strchr(q, ','); if (!q) break; q++; } }
        e = getenv("GF_CIG_MOUTH_KEY"); if (e && isalnum((unsigned char)e[0])) s_mkey = toupper((unsigned char)e[0]);
        e = getenv("GF_CIG_MOUTH_SECS"); if (e && *e) { s_mouth_secs = (float)atof(e); if (s_mouth_secs < 5) s_mouth_secs = 5; }
        e = getenv("GF_SMOKE_FX"); if (e && *e) { s_fx = (float)atof(e); if (!(s_fx >= 0)) s_fx = 1; if (s_fx > 4) s_fx = 4; }
        e = getenv("GF_SMOKE_MASK"); if (e) s_mask_bone = !_stricmp(e, "thoracic") ? 2 : !_stricmp(e, "none") ? -1 : 1;
        s_hash = gf_mission_hash(s_name);
        f_play_main = lookup(0x00460CE8u); f_layer_blend = lookup(0x0046194Cu); f_play_layer = lookup(0x00460E0Cu); f_stop_layer = lookup(0x004610F4u);
        if (!f_play_main || !f_layer_blend || !f_play_layer || !f_stop_layer) s_on = 0;
        printf("[smoke] v1090C %s: key %c plays \"%s\" (%08X) on the player's %s, blend %.2f s, %.0f s%s\n", s_on ? "ON" : "OFF",
               s_key, s_name, s_hash, s_layer < 0 ? "main controller" : "upper-body layer", s_blend, s_secs, s_test > 0 ? " (auto test)" : "");
        fflush(stdout);
    }
    if (!s_on || !ok(anim)) return;
    const uint32_t pl = player_ent(); if (!pl) return;
    if (s_scan && ok(ent)) { __try { prop_scan(anim, ent, pl); } __except (EXCEPTION_EXECUTE_HANDLER) { s_scan = 0; } }
    if (ent != pl) return;
    __try {
        if (rd32(pl + 0xC4u) != anim) return;   /* not the player's main controller */
        const double n = now_s();
        if (!S.t_first) S.t_first = n;
        const int kd = key_down(); const int press = kd && !S.key_prev; S.key_prev = kd;
        int fire = press;
        if (g_smoke_req) { g_smoke_req = 0; if (!S.active) fire = 1; }   /* v1092S: Use from the inventory */
        if (s_test > 0 && !S.test_done && n - S.t_first > s_test) { S.test_done = 1; fire = 1; SLOG("[smoke] auto test trigger\n"); }
        if (fire && S.active) { stop(pl, anim, "J again"); fire = 0; }
        if (fire) {
            if (player_in_car(pl) || gf_cutscene_active()) SLOG("[smoke] not now (car / cutscene)\n");
            else if (s_layer >= 0 && !layer_obj(anim, s_layer)) SLOG("[smoke] the player has no animation layer %d\n", s_layer);
            else if (!s_mouth && !gf_inv_smoke_take()) SLOG("[smoke] no cigarettes left\n");   /* v1092S: one per smoke (the loop does not take more) */
            else start(pl, anim);
        }
        if (S.active) {
            const uint32_t cur = cur_hash(anim);
            if (cur != S.last_seen && S.nchg < 40) { S.nchg++; SLOG("[smoke] +%.2f s: %s %08X -> %08X%s\n", n - S.t0, s_layer < 0 ? "player anim" : "layer", S.last_seen, cur, cur == s_hash ? " (ours)" : ""); }
            S.last_seen = cur;
            {   /* v1086W: ground speed while smoking + the cap sweep */
                static double t_sp; static float px, pz; static int have;
                const uint32_t fm = rd32(pl + 0x68u);
                if (ok(fm)) { const float x = rdf_(fm + 0x40), z = rdf_(fm + 0x48);
                    if (!have || n - t_sp > 1.0) { have = 1; t_sp = n; px = x; pz = z; }
                    else if (n - t_sp >= 0.5) { const float v = sqrtf((x - px) * (x - px) + (z - pz) * (z - pz)) / (float)(n - t_sp);
                        SLOG("[smoke] speed %.2f m/s (cap %d)\n", v, g_gf_force_walk); t_sp = n; px = x; pz = z; } }
                if (s_nsweep > 1) { const int k = (int)((n - S.t0) / 4.0) % s_nsweep; if (g_gf_force_walk != s_sweep[k]) { g_gf_force_walk = s_sweep[k]; SLOG("[smoke] sweep: cap %d\n", s_sweep[k]); } }
            }
            if (cur == s_hash) cig_place(pl, anim);   /* v1088M */
            if (cur == 0xFFFFFFFFu && s_layer >= 0 && !(player_in_car(pl) || gf_cutscene_active())) {   /* v1089L: the clip ended by itself -> loop */
                const int keep_walk = g_gf_force_walk; const double t0 = S.t0; s_loops++;
                start(pl, anim); if (S.active) { g_gf_force_walk = keep_walk; S.t0 = t0; S.last_seen = cur_hash(anim); SLOG("[smoke] loop #%d\n", s_loops); }
                else gfm_set(0, NULL, 0);
            }
            else if (cur != s_hash) { S.active = 0; g_gf_force_walk = 0; gfm_set(0, NULL, 0); SLOG("[smoke] +%.2f s: the game took the %s over (%08X) -- smoke over\n", n - S.t0, s_layer < 0 ? "controller" : "layer", cur); }
            else if (g_gf_cross_held || (sprint_key() && fg_ours())) stop(pl, anim, "sprint");
            else if (g_gf_combat_btn) { char w[40]; snprintf(w, sizeof w, "combat button %02X", g_gf_combat_btn); stop(pl, anim, w); }
            else if (mouse_fresh_press()) stop(pl, anim, "mouse attack / aim");   /* v1110V: a NEW press only (not the inventory click) */
            else if (s_hp0 > 0.0f && fok_(rdf_(pl + 0x14C8u)) && rdf_(pl + 0x14C8u) < s_hp0 - 0.5f) stop(pl, anim, "took damage");
            else if (player_in_car(pl) || gf_cutscene_active()) stop(pl, anim, "car / cutscene");
            /* v1089L: no time limit any more -- the smoke loops until something ends it */
        }
        {   /* v1093M cigarette in the mouth: K / inventory toggles; drawn whenever the hand does not hold it */
            extern volatile int g_uiov_menu_open;
            const int mk = fg_ours() && !g_uiov_menu_open && (GetAsyncKeyState(s_mkey) & 0x8000);
            { static float mt = -1; static int mdone = 0; if (mt < 0) { const char* e = getenv("GF_CIG_MOUTH_TEST"); mt = e ? (float)atof(e) : 0; }
              if (mt > 0 && !mdone && S.t_first > 0 && now_s() - S.t_first > mt) { mdone = 1; g_cig_mouth_req = 1; SLOG("[smoke] v1093M mouth test trigger\n"); } }
            if ((mk && !s_mkey_prev) || g_cig_mouth_req) { g_cig_mouth_req = 0; mouth_toggle(pl, anim); }
            s_mkey_prev = mk;
            if (s_mouth && now_s() - s_mouth_t0 > s_mouth_secs) { s_mouth = 0; g_cig_mouth_on = 0; if (!S.active) gfm_set(0, NULL, 0); gf_shop_toast("The cigarette burned out"); }
            const int in_hand = S.active && cur_hash(anim) == s_hash;
            if (s_mouth && !in_hand) { if (gf_cutscene_active()) gfm_set(0, NULL, 0); else cig_mouth(pl, anim); }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { s_on = 0; g_gf_force_walk = 0; gfm_set(0, NULL, 0); printf("[smoke] fault -> off\n"); fflush(stdout); }
}
