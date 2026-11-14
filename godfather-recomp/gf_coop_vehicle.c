/* gf_coop_vehicle.c -- v1063V co-op car sync (agent V).
 *
 * 1. Guest car proxies are driven through their Havok rigid body instead of being teleported:
 *      body = *(veh + 0x138) (hkRigidBody, Havok 4.1; vt+0x2C func_00437D68 -> func_00011AAC -> func_0076FEE0 setTransform)
 *      motion = body + 0xA0: transform at motion+0x10 (rotation columns +0x10/+0x20/+0x30, translation +0x40),
 *      linear velocity motion+0xE0 (body+0x180), angular velocity motion+0xF0 (body+0x190) -- motion vt+0x40 / +0x44 are
 *      the plain stores func_00773668 / func_00773678 (verified in the lifted code).
 *      setLinearVelocity  = func_0076FF90(body, &v4)  (world lock -> queued op 0x18, else activate func_0076BBCC + store)
 *      setAngularVelocity = func_00770064(body, &v4)  (op 0x19). Both need the Havok TOC 0x00B5BF60 (thunks add 0x10000).
 *    Mode GF_COOP_VEH_DRIVE=soft (default): velocity servo. Every frame lin = host_vel + KP * pos_err,
 *      ang = host_avel + KR * rot_err; snap (vt+0x2C + vt+0x24 block) only beyond GF_COOP_VEH_SNAP m / 60 deg. Physics
 *      integrates between our sets -> smooth motion, wheels see real ground speed, a bump is pulled back smoothly.
 *    hard = vt+0x2C to the target every frame + velocities (keyframe). old = previous gf_coop_proxy.c path.
 * 2. Player cars + seated puppet: reliable event 0x0140 VEH_STATE (on change + every 1 s) {in car, seat, net id}.
 *    (seating: GF_COOP_VEH_SEAT, see veh_seat_tick).
 * Env: GF_COOP_VEH=0 off (old car path), GF_COOP_VEH_DRIVE, _KP, _KR, _SNAP, _LOG. Log [coop-vehicle] every 10 s.
 * Threads: everything on the main PPU thread (gf_proxy_tick / drive_one), except on_event (co-op thread: copy only). */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "recompiled/ppu_recomp.h"
#include "gf_coop_world.h"
#include "gf_memq.h"

extern uint8_t* vm_base;
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
typedef void (*gfn)(ppu_context*);
typedef void (*gf_coop_event_fn)(uint16_t type, const void* data, int len);
extern void gf_coop_set_event_handler(gf_coop_event_fn fn);
extern int gf_coop_send_event(uint16_t type, const void* data, int len);
extern int gf_coop_mode(void);        /* 0 off, 1 host, 2 join, 3 echo */
extern int gf_coop_connected(void);

int  gf_coop_world_netid_of(uint32_t ent);   /* H (host) */
uint32_t gf_coop_world_ent_of(uint16_t netid);   /* H (host); weak default below (v1064K3 prototype) */
enum { GAME_TOC = 0x00B4BF60u, HK_TOC = 0x00B5BF60u };
enum { HK_SET_LINVEL = 0x0076FF90u, HK_SET_ANGVEL = 0x00770064u };
enum { EV_VEH_STATE = 0x0140 };
enum { VS_MAX = 64 };

/* ---------------------------------------------------------------- config */
static int s_on = -1, s_drive = 1 /* 0 old, 1 soft, 2 hard */, s_logv = 0;
static float s_kp = 5.0f /* v1066C 4 -> 5 */, s_kr = 6.0f /* v1066C 5 -> 6 */, s_snap = 6.0f, s_maxcorr = 12.0f;
/* v1066C (agent C) servo upgrades, each with a kill switch:
 *  GF_COOP_VEH_DRAG=1   drag compensation: the body velocity read back one frame later vs what we set = what the car's own
 *                       physics took away (driverless proxy car: brakes / wheel friction / ground contact); its EMA is added
 *                       to the next set along the travel direction (scalar 0..6 m/s, horizontal only; impulses > 6 m/s ignored)
 *  GF_COOP_VEH_BLEND=1  no teleport between SNAP (6 m / 60 deg) and GF_COOP_VEH_HARDSNAP (15 m / 90 deg): catch-up servo
 *                       (2.5x gain, <= 25 m/s) for up to 1.5 s, teleport only if that fails
 *  GF_COOP_VEH_AFF=traj angular feed-forward = derivative of the played target rotation (QPC timed); host (v1066C3 default) = WSN2 avel
 *  GF_COOP_VEH_DRAG=2 (v1066C3 default) also compensates the lateral scrub (<= 3 m/s); =3 adds the yaw-rate loss (<= 1.5 rad/s, opt-in)
 *  (the linear feed-forward is gf_coop_world.c's v1066C trajectory velocity). Counters: "v1066C servo" 10 s line. */
static int s_drag = 2 /* v1066C3: 2 = along + lateral, 3 = + yaw (experimental: made parked floating echo twins oscillate, c66-c rot 9 deg), 1 = along only, 0 off */, s_blend = 1, s_aff = 0 /* v1066C3: host avel default (traj = GF_COOP_VEH_AFF=traj) */; static float s_hardsnap = 15.0f;
static double st_dlat_sum, st_dyaw_sum;
static float s_ki = 4.0f;   /* v1066C GF_COOP_VEH_KI: integral gain (1/s^2) on the horizontal position error (0 = off): removes the
                             * steady lag of a velocity servo whose physics advances less than the wall-clock target (sim dt != frame
                             * time, friction); KP 5 / KI 4 = overdamped. |KI * integral| <= 6 m/s, frozen while > 3 m off.
                             * v1066C2: leaky (2 s), <= 3 m/s (v1066C4: integrates also while parked). */
static double st_along_sum, st_lat_sum, st_ratio_sum, st_icorr_sum, st_mv_err, st_mv_rot; static unsigned st_along_n, st_ratio_n; static float st_mv_max;
static unsigned st_init, st_catch, st_catch_ok, st_e05, st_e1, st_drag_n, st_imp; static double st_drag_sum;
static int bodyrot_mode(void);   /* v1064K1 */
/* v1068S (agent S) car polish, each with a kill switch:
 *  GF_COOP_VEH_CATCHFIX=1 (default ON, bug fix): a car that started a blended catch-up (6..15 m off) and then fell further behind
 *                       (> HARDSNAP 15 m / > 90 deg) was never teleported again: the catch-up branch only runs while e <= 15 m,
 *                       so catch_since stayed set forever and blocked the re-snap (joiner 17:36: "catch-up frames 225 (recovered 0)"
 *                       frozen, moving err max 10 -> 21 -> 53 -> 85 m, mean 6.2 m). Now: beyond HARDSNAP -> catch-up ends -> re-snap.
 *  GF_COOP_VEH_UNSTICK=1 (default ON): a moving target (> 3 m/s) whose proxy body is not moving (< 25 % of the commanded speed)
 *                       for GF_COOP_VEH_UNSTICK_MS (1200) while > 2.5 m off = wedged (kerb / a local object / brakes) -> re-snap.
 *  GF_COOP_VEH_ROTSET=1 (v1068S2 default ON; echo on-ground A/B: moving rot err 5.9 -> 1.2 deg, err 0.24 -> 0.13 m; 0 = off): while moving and < 30 deg off, set the body rotation to the target every frame
 *                       (setRotation, position stays on the velocity servo): removes the 4-8 deg yaw lag of driverless tyres on the ground. */
/* v1068S GF_COOP_VEH_FARKIN=90 (default ON, metres; 0 off): a car proxy farther than this from the LOCAL player is keyframed (game placement +
 *                       host velocities every frame, like GF_COOP_VEH_DRIVE=hard) instead of servoed: far from the viewer the proxy's own
 *                       physics/brakes (and possibly unstreamed ground) only add error, nobody can touch it (17:36: the error grew to 86 m
 *                       exactly while the joiner walked away from the host's traffic). Back inside FARKIN-10 m the servo resumes. */
static int s_catchfix = 1, s_unstick = 1, s_rotset = 1 /* v1068S2: default ON after echo A/B s68-1 vs s68-2 */; static unsigned s_unstick_ms = 1200; static float s_farkin = 90.0f;
static unsigned st_catchfix, st_unstick, st_rotset, st_resnap_log, st_farkin;
static int veh_on(void)
{
    if (s_on < 0) {
        const char* e = getenv("GF_COOP_VEH"); s_on = !(e && *e == '0');
        if ((e = getenv("GF_COOP_VEH_DRIVE")) && *e) s_drive = !_stricmp(e, "hard") ? 2 : !_stricmp(e, "old") ? 0 : 1;
        if ((e = getenv("GF_COOP_VEH_KP")) && *e) s_kp = (float)atof(e);
        if ((e = getenv("GF_COOP_VEH_KR")) && *e) s_kr = (float)atof(e);
        if ((e = getenv("GF_COOP_VEH_SNAP")) && *e) s_snap = (float)atof(e);
        if ((e = getenv("GF_COOP_VEH_LOG")) && *e) s_logv = atoi(e);
        if ((e = getenv("GF_COOP_VEH_DRAG")) && *e) s_drag = atoi(e);                 /* v1066C (v1066C3: 0 / 1 / 2) */
        if ((e = getenv("GF_COOP_VEH_BLEND")) && *e) s_blend = atoi(e) != 0;          /* v1066C */
        if ((e = getenv("GF_COOP_VEH_HARDSNAP")) && *e) s_hardsnap = (float)atof(e);  /* v1066C */
        if ((e = getenv("GF_COOP_VEH_AFF")) && *e) s_aff = !_stricmp(e, "traj") ? 1 : 0;
        if ((e = getenv("GF_COOP_VEH_KI")) && *e) s_ki = (float)atof(e);
        if ((e = getenv("GF_COOP_VEH_CATCHFIX")) && *e) s_catchfix = atoi(e) != 0;      /* v1068S */
        if ((e = getenv("GF_COOP_VEH_UNSTICK")) && *e) s_unstick = atoi(e) != 0;        /* v1068S */
        if ((e = getenv("GF_COOP_VEH_UNSTICK_MS")) && *e) s_unstick_ms = (unsigned)atoi(e);
        if ((e = getenv("GF_COOP_VEH_ROTSET")) && *e) s_rotset = atoi(e) != 0;          /* v1068S */
        if ((e = getenv("GF_COOP_VEH_FARKIN")) && *e) s_farkin = (float)atof(e);        /* v1068S */
        if (s_on) { printf("[coop-vehicle] v1068S car polish: catch-up re-snap fix %s, unstick %s (%u ms), rotation set while moving %s, far keyframing %s (> %.0f m from the local player) (GF_COOP_VEH_CATCHFIX / _UNSTICK / _UNSTICK_MS / _ROTSET / _FARKIN)\n",
                           s_catchfix ? "ON" : "off", s_unstick ? "ON" : "off", s_unstick_ms, s_rotset ? "ON" : "off", s_farkin > 0.0f ? "ON" : "off", (double)s_farkin); fflush(stdout); }
        if (s_hardsnap < s_snap) s_hardsnap = s_snap;
        printf("[coop-vehicle] v1066C servo: drag compensation %s, blended re-snaps %s (teleport > %.0f m), angular feed-forward %s (GF_COOP_VEH_DRAG / _BLEND / _HARDSNAP / _AFF)\n",
               s_drag >= 3 ? "ON (along + lateral + yaw)" : s_drag == 2 ? "ON (along + lateral)" : s_drag ? "ON (along only)" : "off", s_blend ? "ON" : "off", (double)s_hardsnap, s_aff ? "target trajectory" : "host avel");
        printf("[coop-vehicle] v1063V car sync %s: drive %s (KP %.1f /s, KR %.1f /s, snap %.1f m)\n", s_on ? "ON" : "off",
               s_drive == 2 ? "hard (keyframe)" : s_drive == 1 ? "soft (velocity servo)" : "old (teleport)", (double)s_kp, (double)s_kr, (double)s_snap);
        fflush(stdout);
        bodyrot_mode();   /* v1064K1 startup line */
    }
    return s_on;
}

/* ---------------------------------------------------------------- guest memory + calls */
static int s_call_fail;
static int ea_range(uint32_t ea, size_t n) { return ea >= 0x10000u && ea < 0xF0000000u && n <= 0xF0000000u - ea; }
static int mem_access(uint32_t ea, size_t n, int write) { return gf_memq_access(vm_base, ea, n, write); }
static uint32_t rd32(uint32_t ea) { const uint8_t* p = vm_base + ea; return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void wr32(uint32_t ea, uint32_t v) { uint8_t* p = vm_base + ea; p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static int read_u32(uint32_t ea, uint32_t* out)
{
    if (!mem_access(ea, 4, 0)) return 0;
    __try { *out = rd32(ea); return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int read_f(uint32_t ea, float* out) { uint32_t u; if (!read_u32(ea, &u)) return 0; memcpy(out, &u, 4); return 1; }
static int write_f(uint32_t ea, float f)
{
    uint32_t u; memcpy(&u, &f, 4);
    if (!mem_access(ea, 4, 1)) return 0;
    __try { wr32(ea, u); return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static uint32_t player(void)
{
    uint32_t a;
    if (!read_u32(0x10181290u, &a) || !ea_range(a, 8)) return 0;
    if (!read_u32(a + 4, &a) || !ea_range(a, 4)) return 0;
    if (!read_u32(a, &a) || a < 0x17FCu) return 0;
    a -= 0x17FCu;
    return ea_range(a, 4) ? a : 0;
}
static gfn lookup(uint32_t addr)
{
    static uint32_t ca[64]; static gfn cf[64]; static int cn;
    uint64_t i; gfn f = 0;
    for (i = 0; i < (uint64_t)cn; i++) if (ca[i] == addr) return cf[i];
    for (i = 0; i < function_table_count; i++)
        if ((uint32_t)function_table[i].addr == addr) { f = function_table[i].func; break; }
    if (cn < 64) { ca[cn] = addr; cf[cn] = f; cn++; }
    if (!f) { printf("[coop-vehicle] func_%08X is not in the function table\n", addr); fflush(stdout); }
    return f;
}
static uint64_t gcall_raw(gfn f, uint32_t toc, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    ppu_context* cur = g_active_ctx;
    static ppu_context c;
    if (!cur || !f) return 0;
    memcpy(&c, cur, sizeof c);
    c.gpr[1] = ((uint32_t)cur->gpr[1] - 0x1800u) & ~0xFull;   /* below the scratch block (sp - 0x800) */
    c.gpr[2] = toc;
    c.gpr[3] = a0; c.gpr[4] = a1; c.gpr[5] = a2; c.gpr[6] = a3; c.gpr[7] = a4;
    c.lr = 0;
    g_active_ctx = &c;
    f(&c);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&c); }
    g_active_ctx = cur;
    return c.gpr[3];
}
static uint64_t gcall_toc(uint32_t addr, uint32_t toc, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    ppu_context* cur = g_active_ctx; gfn f;
    if (!cur || !(f = lookup(addr))) return 0;
    __try { return gcall_raw(f, toc, a0, a1, a2, a3, a4); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_active_ctx = cur; g_trampoline_fn = 0; s_call_fail++;
        printf("[coop-vehicle] guest call %08X faulted\n", addr); fflush(stdout); return 0;
    }
}
static uint32_t scratch(void)
{
    uint32_t ea;
    if (!g_active_ctx) return 0;
    ea = ((uint32_t)g_active_ctx->gpr[1] - 0x800u) & ~0xFu;
    return mem_access(ea, 0x100, 1) ? ea : 0;
}
static int call_virtual(uint32_t ent, uint32_t slot, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4)
{
    uint32_t vt, opd, fn, toc; int before = s_call_fail;
    if (!read_u32(ent, &vt) || !ea_range(vt + slot, 4) || !read_u32(vt + slot, &opd) ||
        !ea_range(opd, 8) || !read_u32(opd, &fn) || !read_u32(opd + 4, &toc) || !fn || !toc) return 0;
    if (!lookup(fn)) return 0;
    gcall_toc(fn, toc, ent, a1, a2, a3, a4);
    return s_call_fail == before;
}
static int read_xf12(uint32_t ent, float m[12])
{
    uint32_t xf;
    if (!read_u32(ent + 0x68u, &xf) || !ea_range(xf, 0x50u)) return 0;
    for (int r = 0; r < 4; r++) for (int k = 0; k < 3; k++)
        if (!read_f(xf + 0x10u + (uint32_t)(r * 16 + k * 4), &m[r * 3 + k])) return 0;
    return 1;
}
/* m12 -> RwMatrix at dst (padding words copied from the entity's own matrix) */
static int write_matrix12(uint32_t dst, uint32_t ent, const float m[12])
{
    uint32_t xf;
    if (!read_u32(ent + 0x68u, &xf) || !ea_range(xf, 0x50u)) return 0;
    if (!mem_access(dst, 0x40, 1) || !mem_access(xf + 0x10u, 0x40, 0)) return 0;
    __try {
        for (int r = 0; r < 4; r++) {
            for (int k = 0; k < 3; k++) { uint32_t u; memcpy(&u, &m[r * 3 + k], 4); wr32(dst + (uint32_t)(r * 16 + k * 4), u); }
            wr32(dst + (uint32_t)(r * 16 + 12), rd32(xf + 0x10u + (uint32_t)(r * 16 + 12)));
        }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
/* the game's own placement: vt+0x2C SetTransform (writes the Havok body) [+ vt+0x24 SetPosition {x,y,z,2}] */
static int place_car(uint32_t ent, const float m[12], int with_block)
{
    uint32_t buf = scratch(); int before = s_call_fail;
    if (!buf || !write_matrix12(buf, ent, m)) return 0;
    if (!call_virtual(ent, 0x2Cu, buf, 0, 0, 0)) return 0;
    if (with_block) {
        const uint32_t blk = buf + 0x40u; uint32_t u;
        __try { memcpy(&u, &m[9], 4); wr32(blk, u); memcpy(&u, &m[10], 4); wr32(blk + 4, u); memcpy(&u, &m[11], 4); wr32(blk + 8, u); wr32(blk + 12, 2u); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        call_virtual(ent, 0x24u, blk, 0, 0, 0);
    }
    return s_call_fail == before;
}

/* ---------------------------------------------------------------- Havok body */
static uint32_t car_body(uint32_t ent)
{
    uint32_t b = 0, mvt = 0;
    if (!read_u32(ent + 0x138u, &b) || !ea_range(b, 0x200u) || !mem_access(b, 0x1A0u, 1)) return 0;
    if (!read_u32(b + 0xA0u, &mvt) || !ea_range(mvt, 0x48u)) return 0;   /* hkMotion vtable */
    return b;
}
/* the body is simulated only while it is in a Havok world (body+0x8) and the car is not on its ent+0x1B0 representation
 * (veh+0x7C4 & 0x10 -> vt+0x184 = 1 -> vt+0x2C writes ent+0x1B0, func_002B5F70). activate() on a body outside the world
 * ABORTS the game (v63-6: abort from 0x0076FFD0 = func_0076FF90 -> func_0076BBCC). */
static int body_live(uint32_t ent, uint32_t b)
{
    uint32_t w = 0, f = 0;
    if (!b || !read_u32(b + 0x8u, &w) || !ea_range(w, 0x100u)) return 0;
    if (!read_u32(ent + 0x7C4u, &f) || (f & 0x10u)) return 0;
    return 1;
}
static int body_vec(uint32_t b, uint32_t off, float v[3]) { return read_f(b + off, &v[0]) && read_f(b + off + 4, &v[1]) && read_f(b + off + 8, &v[2]); }
static int set_body_vel(uint32_t b, const float lin[3], const float ang[3])
{
    uint32_t buf = scratch(); int before = s_call_fail;
    if (!buf) return 0;
    for (int k = 0; k < 3; k++) { if (!isfinite(lin[k]) || !isfinite(ang[k])) return 0; }
    if (!write_f(buf + 0xC0, lin[0]) || !write_f(buf + 0xC4, lin[1]) || !write_f(buf + 0xC8, lin[2]) || !write_f(buf + 0xCC, 0.0f)) return 0;
    if (!write_f(buf + 0xD0, ang[0]) || !write_f(buf + 0xD4, ang[1]) || !write_f(buf + 0xD8, ang[2]) || !write_f(buf + 0xDC, 0.0f)) return 0;
    gcall_toc(HK_SET_LINVEL, HK_TOC, b, buf + 0xC0, 0, 0, 0);
    gcall_toc(HK_SET_ANGVEL, HK_TOC, b, buf + 0xD0, 0, 0, 0);
    return s_call_fail == before;
}

/* v1064K1 FLAT-CAR FIX. func_0076FEE0(body, X) only forwards to motion(body+0xA0) vt+0x34 = hkMotion::setRotation(const
 * hkQuaternion&) (vt+0x30 = setPosition func_0076FF38, vt+0x40/+0x44 = setLinear/AngularVelocity). It is NOT setTransform:
 * vt+0x2C func_00437D68 packs the RwMatrix rows right/up/at as hkRotation columns at sp+0x80, converts them with
 * func_0001420C -> func_00871CD0 (hkQuaternion::set(hkRotation)) to a 16-byte quaternion at sp+0x70 and passes &quat
 * (func_00011AAC thunk -> func_0076FEE0). v1063V passed the 3x4 rows -> quaternion (right.x, right.y, right.z, w = 0) =
 * a 180 deg turn about the right axis: every car proxy was flipped UPSIDE DOWN at its first drive frame and on every
 * re-snap (roof in the road = the "flat" cars; joiner log rot err mean 179.8 deg, snaps every 15-frame cooldown; echo
 * snap details: 'at' kept, right/up negated). Now: proper quaternion (x, y, z, w) of the target basis.
 * GF_COOP_VEH_BODYROT = 1 (default, fixed) | 0 (no body rotation set, vt+0x2C only) | old (v1063V bug, for bisecting). */
static int s_bodyrot = -1, s_qgame = 0, s_qchk = 0;
static unsigned st_qfix;
static int bodyrot_mode(void)
{
    if (s_bodyrot < 0) {
        const char* e = getenv("GF_COOP_VEH_BODYROT");
        s_bodyrot = !e || !*e ? 1 : !_stricmp(e, "old") ? 2 : atoi(e) ? 1 : 0;
        printf("[coop-vehicle] v1064K1 flat-car fix %s: car body rotation = %s (GF_COOP_VEH_BODYROT)\n", s_bodyrot == 1 ? "ON" : "OFF",
               s_bodyrot == 1 ? "hkQuaternion of the target basis (setRotation func_0076FEE0)" : s_bodyrot == 2 ? "OLD v1063V 3x4 rows (cars flip upside down)" : "not set (vt+0x2C only)");
        fflush(stdout);
    }
    return s_bodyrot;
}
/* rows r,u,a (orthonormalised here) = hkRotation columns -> quaternion q = (x, y, z, w) */
static int quat_of_rows(const float m[12], float q[4])
{
    float r[3] = {m[0], m[1], m[2]}, u[3] = {m[3], m[4], m[5]}, a[3], l, d, t, s;
    l = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]); if (!(l > 1e-4f) || !isfinite(l)) return 0;
    for (int k = 0; k < 3; k++) r[k] /= l;
    d = r[0] * u[0] + r[1] * u[1] + r[2] * u[2]; for (int k = 0; k < 3; k++) u[k] -= r[k] * d;
    l = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]); if (!(l > 1e-4f) || !isfinite(l)) return 0;
    for (int k = 0; k < 3; k++) u[k] /= l;
    a[0] = r[1] * u[2] - r[2] * u[1]; a[1] = r[2] * u[0] - r[0] * u[2]; a[2] = r[0] * u[1] - r[1] * u[0];
    {   /* R[i][j] = component i of basis j (column j = r, u, a) */
        const float R00 = r[0], R10 = r[1], R20 = r[2], R01 = u[0], R11 = u[1], R21 = u[2], R02 = a[0], R12 = a[1], R22 = a[2];
        t = R00 + R11 + R22;
        if (t > 0.0f) { s = sqrtf(t + 1.0f) * 2.0f; q[3] = 0.25f * s; q[0] = (R21 - R12) / s; q[1] = (R02 - R20) / s; q[2] = (R10 - R01) / s; }
        else if (R00 > R11 && R00 > R22) { s = sqrtf(1.0f + R00 - R11 - R22) * 2.0f; q[3] = (R21 - R12) / s; q[0] = 0.25f * s; q[1] = (R01 + R10) / s; q[2] = (R02 + R20) / s; }
        else if (R11 > R22) { s = sqrtf(1.0f + R11 - R00 - R22) * 2.0f; q[3] = (R02 - R20) / s; q[0] = (R01 + R10) / s; q[1] = 0.25f * s; q[2] = (R12 + R21) / s; }
        else { s = sqrtf(1.0f + R22 - R00 - R11) * 2.0f; q[3] = (R10 - R01) / s; q[0] = (R02 + R20) / s; q[1] = (R12 + R21) / s; q[2] = 0.25f * s; }
    }
    l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]); if (!(l > 1e-4f) || !isfinite(l)) return 0;
    for (int k = 0; k < 4; k++) q[k] /= l;
    return 1;
}
/* the game's own conversion (func_00871CD0 = hkQuaternion::set(hkRotation), Havok TOC) -> q; 0 if the call failed */
static int quat_game(uint32_t buf, const float m[12], float q[4])
{
    int before = s_call_fail; float l = 0;
    for (int c = 0; c < 3; c++) {
        for (int k = 0; k < 3; k++) if (!write_f(buf + 0x80u + (uint32_t)(c * 16 + k * 4), m[c * 3 + k])) return 0;
        if (!write_f(buf + 0x80u + (uint32_t)(c * 16 + 12), 0.0f)) return 0;
    }
    for (int k = 0; k < 4; k++) if (!write_f(buf + 0xB0u + (uint32_t)(k * 4), 0.0f)) return 0;
    gcall_toc(0x00871CD0u, HK_TOC, buf + 0xB0u, buf + 0x80u, 0, 0, 0);
    if (s_call_fail != before) return 0;
    for (int k = 0; k < 4; k++) { if (!read_f(buf + 0xB0u + (uint32_t)(k * 4), &q[k]) || !isfinite(q[k])) return 0; l += q[k] * q[k]; }
    return l > 0.9f && l < 1.1f;
}
static int set_body_xform(uint32_t b, const float m[12])
{
    uint32_t buf = scratch(); int before = s_call_fail; float q[4], qg[4];
    const int mode = bodyrot_mode();
    if (!buf || mode == 0) return 0;
    if (mode == 2) {   /* OLD v1063V (wrong: 3x4 rows read as a quaternion) */
        for (int r = 0; r < 4; r++) {
            for (int k = 0; k < 3; k++) if (!isfinite(m[r * 3 + k]) || !write_f(buf + 0x80u + (uint32_t)(r * 16 + k * 4), m[r * 3 + k])) return 0;
            if (!write_f(buf + 0x80u + (uint32_t)(r * 16 + 12), r == 3 ? 1.0f : 0.0f)) return 0;
        }
        gcall_toc(0x0076FEE0u, HK_TOC, b, buf + 0x80u, 0, 0, 0);
        return s_call_fail == before;
    }
    for (int k = 0; k < 9; k++) if (!isfinite(m[k])) return 0;
    if (!quat_of_rows(m, q)) return 0;
    if (s_qchk < 6) {   /* proof: our quaternion vs the game's own conversion of the same basis (first 6 sets) */
        if (quat_game(buf, m, qg)) {
            const float dot = q[0] * qg[0] + q[1] * qg[1] + q[2] * qg[2] + q[3] * qg[3];
            s_qchk++;
            if (fabsf(dot) < 0.99f) s_qgame = 1;   /* convention differs -> use the game's conversion from now on */
            printf("[coop-vehicle] v1064K1 quat check %d: ours %.3f %.3f %.3f %.3f vs game %.3f %.3f %.3f %.3f (dot %.3f)%s\n", s_qchk,
                   (double)q[0], (double)q[1], (double)q[2], (double)q[3], (double)qg[0], (double)qg[1], (double)qg[2], (double)qg[3], (double)dot,
                   s_qgame ? " -> using the game's conversion" : ""); fflush(stdout);
        } else if (s_qchk == 0) { s_qchk = 6; printf("[coop-vehicle] v1064K1 quat check: game conversion func_00871CD0 not callable -> ours\n"); fflush(stdout); }
    }
    if (s_qgame) { if (!quat_game(buf, m, q)) return 0; }
    for (int k = 0; k < 4; k++) if (!write_f(buf + 0x70u + (uint32_t)(k * 4), q[k])) return 0;
    gcall_toc(0x0076FEE0u, HK_TOC, b, buf + 0x70u, 0, 0, 0);
    st_qfix++;
    return s_call_fail == before;
}
/* ---------------------------------------------------------------- rotation helpers (RwMatrix rows r,u,a = world basis) */
static float det3(const float m[9])
{
    return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
}
/* rotation vector (axis * angle, world) that turns basis B into basis A: R = C_A * C_B^T, C = columns r,u,a */
static void rot_err(const float A[9], const float B[9], float out[3], float* ang)
{
    float R[3][3], tr, v[3], s, c, th;
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) R[i][j] = A[i] * B[j] + A[3 + i] * B[3 + j] + A[6 + i] * B[6 + j];
    tr = R[0][0] + R[1][1] + R[2][2];
    v[0] = R[2][1] - R[1][2]; v[1] = R[0][2] - R[2][0]; v[2] = R[1][0] - R[0][1];
    s = 0.5f * sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    c = 0.5f * (tr - 1.0f);
    th = atan2f(s, c);
    if (s > 1e-5f) { const float k = th / (2.0f * s); out[0] = v[0] * k; out[1] = v[1] * k; out[2] = v[2] * k; }
    else { out[0] = v[0] * 0.5f; out[1] = v[1] * 0.5f; out[2] = v[2] * 0.5f; }
    if (ang) *ang = th;
}

/* ---------------------------------------------------------------- per-car state */
typedef struct {
    uint32_t ent; uint16_t netid; int init;
    float prev_tgt[12]; ULONGLONG prev_ms; float w[3];     /* target angular velocity estimate (rad/s) */
    float avel_in[3]; ULONGLONG avel_ms;                  /* v1063H avel from the snapshot, if any */
    int probed; ULONGLONG last_ms;
    ULONGLONG eject_at;
    int snap_cool;
    uint16_t occ_want[3]; ULONGLONG occ_ms;     /* host's seat occupants (net ids) from the snapshot ext */
    uint32_t occ_ent[3];                         /* ped proxies we seated in this car */
    int looked;                                  /* v1064K2 car look compared with the host's spawn ext */
    int mfix_tries, mtest, mwait;                /* v1070C model (paint variant) swaps tried on this proxy / echo self-test state */
    float vset[3]; int vset_ok; float drag[3];   /* v1066C last linear velocity we set + drag estimate */
    ULONGLONG catch_since; double w_ms;          /* v1066C catch-up start; QPC ms of prev_tgt for the angular feed-forward */
    float integ[3]; float ppos[3], pset[3]; double pq_ms; int ppos_ok;
    ULONGLONG stuck_since;                       /* v1068S unstick: first frame the body stopped following a moving target */
    int farkin;                                   /* v1068S FARKIN (v1068R: renamed from 'far' = windef.h macro): keyframed because far from the local player */
    float dlat[2], dyaw, aset_y; int aset_ok;   /* v1066C3 lateral / yaw drag estimate, last yaw rate set */   /* v1066C integral + advance-ratio diagnostic */
    /* v1070E (agent E): host car details from snapshot ext v2 (driver input, damage, siren / police light) */
    int8_t in_x, in_y; uint8_t in_fl, hp8; uint32_t zones; int ext2; ULONGLONG ext2_ms;
    float wr_x, wr_y; int wr_ok;                 /* last driver input we wrote into the proxy's status object */
    float hp_last; uint32_t zones_last; int hp_ok; ULONGLONG dmg_at, hold_until;   /* proxy health we last saw / set */
    ULONGLONG mres_at; int mres_n;               /* v1070E model-not-resident re-check */
    uint32_t st_ptr; ULONGLONG plt_at;           /* v1070E proxy device status object, last police-light call */
    int redir; uint32_t di, s_orig, s_clone;     /* v1070E2 driver input status redirect (the proxy's own AI writes into s_clone) */
    ULONGLONG noeff_until;                       /* v1070E2 damage send had no effect (health floor) -> pause */
    ULONGLONG hexpl_ms; int expl_said;           /* v1070E2 host car exploded at (first ext) / proxy explosion counted */
} vstate;
static vstate s_v[VS_MAX];
static uint16_t s_claimed;   /* guest: net id of the proxy car our player drives (claimed -> not driven from host data) */
static unsigned st_kin;
static unsigned st_ext_tx, st_ext_occ, st_ext_occ_id, st_ext_rx, st_ext_rx_occ;
static unsigned st_drive, st_snap, st_fail, st_nobody, st_hard, st_probe_bad;
static double st_err_sum, st_rerr_sum; static float st_err_max; static unsigned st_err_n;
static unsigned st_upside, st_rclamp;   /* v1064K1: driven frames with the car's up row pointing down / rotation correction suppressed */
static int s_rclamp = -1;               /* v1064K1 GF_COOP_VEH_ROTCLAMP=1 (default): no rotation servo beyond 115 deg (axis undefined near 180), |corr| <= 4 rad/s; 0 = old */
static float len3(const float* v) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
/* v1064K1 diag (guest, every 10 s, first 4 cars): row lengths + det of the matrix we WRITE vs the car's matrix as READ back vs
 * the Havok body rotation columns (body+0xB0/C0/D0). A flipped car shows up.y < 0 / rot ~180; a squashed one |u| << 1. */
static void diag_car(uint16_t netid, uint32_t ent, uint32_t b, const float want[12], const float cur[12], float rang, ULONGLONG now)
{
    static ULONGLONG win; static int n;
    float c0[3] = {0, 0, 0}, c1[3] = {0, 0, 0}, c2[3] = {0, 0, 0}, bm[9];
    if (now - win >= 10000) { win = now; n = 0; }
    if (n >= 4) return;
    n++;
    body_vec(b, 0xB0u, c0); body_vec(b, 0xC0u, c1); body_vec(b, 0xD0u, c2);
    memcpy(bm, c0, 12); memcpy(bm + 3, c1, 12); memcpy(bm + 6, c2, 12);
    printf("[coop-vehicle] v1064K diag net %u car %08X: WRITTEN |r| %.3f |u| %.3f |a| %.3f det %.3f up %.2f %.2f %.2f | CAR |r| %.3f |u| %.3f |a| %.3f det %.3f up %.2f %.2f %.2f | BODY |c0| %.3f |c1| %.3f |c2| %.3f det %.3f c1 %.2f %.2f %.2f | rot err %.1f deg\n",
           netid, ent, (double)len3(want), (double)len3(want + 3), (double)len3(want + 6), (double)det3(want), (double)want[3], (double)want[4], (double)want[5],
           (double)len3(cur), (double)len3(cur + 3), (double)len3(cur + 6), (double)det3(cur), (double)cur[3], (double)cur[4], (double)cur[5],
           (double)len3(c0), (double)len3(c1), (double)len3(c2), (double)det3(bm), (double)c1[0], (double)c1[1], (double)c1[2], (double)(rang * 57.2958f));
    fflush(stdout);
}
static vstate* vs_get(uint32_t ent, uint16_t netid, ULONGLONG now)
{
    int free_i = -1, old_i = 0;
    for (int i = 0; i < VS_MAX; i++) {
        if (s_v[i].ent == ent && s_v[i].netid == netid) return &s_v[i];
        if (!s_v[i].ent && free_i < 0) free_i = i;
        if (s_v[i].last_ms < s_v[old_i].last_ms) old_i = i;
    }
    for (int i = 0; i < VS_MAX; i++) if (s_v[i].ent == ent) { free_i = i; break; }   /* same body, new net id (pool reuse) */
    if (free_i < 0) free_i = old_i;
    memset(&s_v[free_i], 0, sizeof s_v[free_i]);
    s_v[free_i].ent = ent; s_v[free_i].netid = netid; s_v[free_i].last_ms = now;
    return &s_v[free_i];
}
static void probe_car(vstate* v, uint32_t ent, uint32_t b)
{
    /* one-time: is our layout right? body translation (motion+0x40 = body+0xE0) vs the entity matrix, motion type/vtable,
     * world pointer, the vt+0x2C branch flags (veh+0x7C4 & 0x10 -> SetTransform goes to ent+0x1B0, not the body). */
    float xm[12], bp[3] = {0, 0, 0}, lv[3] = {0, 0, 0}, c0[3] = {0, 0, 0}; uint32_t mvt = 0, world = 0, f7c4 = 0, mt = 0;
    v->probed = 1;
    read_xf12(ent, xm); body_vec(b, 0xE0u, bp); body_vec(b, 0x180u, lv); body_vec(b, 0xB0u, c0);
    read_u32(b + 0xA0u, &mvt); read_u32(b + 0x8u, &world); read_u32(ent + 0x7C4u, &f7c4);
    if (mem_access(b + 0xA8u, 1, 0)) { __try { mt = vm_base[b + 0xA8u]; } __except (EXCEPTION_EXECUTE_HANDLER) { } }
    {
        const float dx = bp[0] - xm[9], dy = bp[1] - xm[10], dz = bp[2] - xm[11];
        const float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d > 2.0f) st_probe_bad++;
        static int nlog; if (nlog++ < 12 || d > 2.0f)
            printf("[coop-vehicle] v1063V probe net %u car %08X body %08X motion vt %08X type %u world %08X +7C4 %08X | body pos %.2f %.2f %.2f vs matrix %.2f %.2f %.2f (%.2f m) | col0 %.2f %.2f %.2f vs right %.2f %.2f %.2f | linvel %.2f %.2f %.2f\n",
                   v->netid, ent, b, mvt, mt, world, f7c4, (double)bp[0], (double)bp[1], (double)bp[2], (double)xm[9], (double)xm[10], (double)xm[11], (double)d,
                   (double)c0[0], (double)c0[1], (double)c0[2], (double)xm[0], (double)xm[1], (double)xm[2], (double)lv[0], (double)lv[1], (double)lv[2]);
        fflush(stdout);
    }
}

/* ---------------------------------------------------------------- v1064K2 car look (spawn ext, <= 32 B)
 * Host gf_world_spawn_ext_car: [0]='C' [1]=1 [2]=flags (bit0 model GUID valid) [3]=0 [4..19] render model GUID (ent+0x64
 * instance +0x10 resource -> model registry, same lookup as P's ped look) [20..23] veh+0xB8 (big endian, model identity
 * candidate, Codex K) [24..31] reserved (paint / variant once proven). Guest: compares with its proxy at the first drive
 * frame and logs "[coop-vehicle] v1064K2 car look: model match N, differs M, unknown U". Diagnostic only this round. */
extern int gf_proxy_model_guid_of(uint32_t ent, uint8_t g[16]);   /* gf_coop_proxy.c (v1064K) */
enum { CX_TAB = 64 };
static uint16_t s_cx_id[CX_TAB]; static uint8_t s_cx[CX_TAB][32]; static int s_cx_i;
static unsigned st_cx_tx, st_cx_rx, st_look_match, st_look_diff, st_look_unk, st_b8_match, st_b8_diff;
int gf_world_spawn_ext_car(uint32_t ent, uint8_t kind, uint8_t* out, int max)
{
    uint8_t g[16]; uint32_t b8 = 0;
    if (!veh_on() || kind != GF_WK_CAR || !out || max < 32 || !ent) return 0;
    memset(out, 0, 32); out[0] = 'C'; out[1] = 1;
    if (gf_proxy_model_guid_of(ent, g)) { out[2] |= 1; memcpy(out + 4, g, 16); }
    if (read_u32(ent + 0xB8u, &b8)) { out[20] = (uint8_t)(b8 >> 24); out[21] = (uint8_t)(b8 >> 16); out[22] = (uint8_t)(b8 >> 8); out[23] = (uint8_t)b8; }
    st_cx_tx++;
    return 32;
}
void gf_proxy_spawn_ext_car(uint16_t netid, const uint8_t* ext, int len)
{
    int k = -1;
    if (!ext || len < 24 || ext[0] != 'C' || ext[1] != 1) return;
    for (int i = 0; i < CX_TAB; i++) if (s_cx_id[i] == netid) { k = i; break; }
    if (k < 0) {   /* v1066C: reuse a slot of a net id that has no live car (a plain ring evicted live cars' ext after 64 spawns) */
        for (int i = 0; i < CX_TAB && k < 0; i++) { int live = 0; if (!s_cx_id[i]) { k = i; break; }
            for (int j = 0; j < VS_MAX; j++) if (s_v[j].ent && s_v[j].netid == s_cx_id[i] && GetTickCount64() - s_v[j].last_ms < 5000) { live = 1; break; }
            if (!live && i != (s_cx_i % CX_TAB)) k = i; }
        if (k < 0) k = s_cx_i++ % CX_TAB; }
    s_cx_id[k] = netid; memset(s_cx[k], 0, 32); memcpy(s_cx[k], ext, len > 32 ? 32 : (size_t)len);
    st_cx_rx++;
    for (int i = 0; i < VS_MAX; i++) if (s_v[i].ent && s_v[i].netid == netid) s_v[i].looked = 0;   /* re-compare (resend / reconnect) */
}
/* v1064K3 echo-only RE aid (GF_COOP_VEH_LOOKDIFF=0 off): the guest half can see the host's source car (same process), so for the
 * first 3 car pairs log the words that differ between source and proxy in the vehicle struct (0..0xA00) and its render
 * instance (*(veh+0x64), 0..0x68), skipping heap pointers -> paint / variant / dirt field candidates for the next round. */
static void look_diff(uint16_t netid, uint32_t prx)
{
    static int s_ld = -1, done;
    uint32_t src = 0;
    if (s_ld < 0) { const char* e = getenv("GF_COOP_VEH_LOOKDIFF"); s_ld = !(e && *e == '0'); }
    if (!s_ld || done >= 3 || gf_coop_mode() != 3) return;
    __try { src = gf_coop_world_ent_of(netid); } __except (EXCEPTION_EXECUTE_HANDLER) { src = 0; }
    if (!src || src == prx) return;
    done++;
    for (int reg = 0; reg < 2; reg++) {
        uint32_t a0 = src, b0 = prx, len = 0xA00u; char line[1600]; int o = 0, nd = 0;
        if (reg == 1) { if (!read_u32(src + 0x64u, &a0) || !read_u32(prx + 0x64u, &b0) || !ea_range(a0, 0x68) || !ea_range(b0, 0x68)) break; len = 0x68u; }
        if (!mem_access(a0, len, 0) || !mem_access(b0, len, 0)) break;
        o = snprintf(line, sizeof line, "[coop-vehicle] v1064K3 lookdiff net %u %s src %08X prx %08X:", netid, reg ? "inst" : "veh", a0, b0);
        __try {
            for (uint32_t off = 0; off < len && o < (int)sizeof line - 40; off += 4) {
                const uint32_t a = rd32(a0 + off), b = rd32(b0 + off);
                if (a == b) continue;
                if (a >= 0x10000000u && a < 0x50000000u && b >= 0x10000000u && b < 0x50000000u) continue;   /* heap / data pointers */
                nd++;
                o += snprintf(line + o, sizeof line - (size_t)o, " +%03X:%08X/%08X", off, a, b);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
        printf("%s (%d differ)\n", line, nd); fflush(stdout);
        if (reg == 1) {   /* one level deeper: objects the render instance points to (per-instance material / tint blocks?) */
            for (uint32_t off = 0; off < 0x68u; off += 4) {
                uint32_t pa = 0, pb = 0; int n2 = 0;
                __try { pa = rd32(a0 + off); pb = rd32(b0 + off); } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
                if (pa == pb || pa < 0x10000000u || pa >= 0x50000000u || pb < 0x10000000u || pb >= 0x50000000u) continue;
                if (!mem_access(pa, 0x60, 0) || !mem_access(pb, 0x60, 0)) continue;
                o = snprintf(line, sizeof line, "[coop-vehicle] v1064K3 lookdiff net %u inst+%02X -> src %08X prx %08X:", netid, off, pa, pb);
                __try {
                    for (uint32_t k = 0; k < 0x60u && o < (int)sizeof line - 40; k += 4) {
                        const uint32_t a = rd32(pa + k), b = rd32(pb + k);
                        if (a == b || (a >= 0x10000000u && a < 0x50000000u && b >= 0x10000000u && b < 0x50000000u)) continue;
                        n2++; o += snprintf(line + o, sizeof line - (size_t)o, " +%02X:%08X/%08X", k, a, b);
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) { }
                printf("%s (%d differ)\n", line, n2); fflush(stdout);
            }
        }
    }
}
/* v1066C echo-only RE aid (GF_COOP_VEH_WHEELDIFF=0 off): which vehicle words animate while a car drives (wheel spin / steer /
 * suspension candidates) on the host's SOURCE car, and do they animate on the proxy twin too? For up to 4 moving source cars:
 * two samples 250 ms apart of veh+0x000..0xA00 for source + proxy; prints offsets whose value changed on the source (heap
 * pointers skipped) with source a->b and proxy a->b. */
static void wheel_diff(uint16_t netid, uint32_t prx, ULONGLONG now)
{
    static int s_wd = -1, done; static uint16_t cur_id; static uint32_t cur_src, cur_prx; static ULONGLONG t0; static uint32_t sa[0x280], pa[0x280];
    if (s_wd < 0) { const char* e = getenv("GF_COOP_VEH_WHEELDIFF"); s_wd = !(e && *e == '0'); }
    if (!s_wd || done >= 4 || gf_coop_mode() != 3) return;
    if (!cur_id) {
        uint32_t src = 0, b; float lv[3] = {0, 0, 0};
        __try { src = gf_coop_world_ent_of(netid); } __except (EXCEPTION_EXECUTE_HANDLER) { src = 0; }
        if (!src || src == prx || !(b = car_body(src)) || !body_vec(b, 0x180u, lv) || lv[0] * lv[0] + lv[2] * lv[2] < 2.25f) return;
        if (!mem_access(src, 0xA00u, 0) || !mem_access(prx, 0xA00u, 0)) return;
        __try { for (uint32_t k = 0; k < 0x280u; k++) { sa[k] = rd32(src + k * 4u); pa[k] = rd32(prx + k * 4u); } } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        cur_id = netid; cur_src = src; cur_prx = prx; t0 = now; return;
    }
    if (netid != cur_id || prx != cur_prx) return;
    if (now - t0 < 250) return;
    {
        char line[2400]; int o, nd = 0;
        o = snprintf(line, sizeof line, "[coop-vehicle] v1066C wheeldiff net %u src %08X prx %08X (moving):", netid, cur_src, cur_prx);
        __try {
            for (uint32_t k = 0; k < 0x280u && o < (int)sizeof line - 60; k++) {
                const uint32_t sb = rd32(cur_src + k * 4u), pb = rd32(cur_prx + k * 4u); float fa, fb, ga, gb;
                if (sb == sa[k]) continue;
                if (sa[k] >= 0x10000000u && sa[k] < 0x50000000u && sb >= 0x10000000u && sb < 0x50000000u && ((sa[k] ^ sb) >> 20) == 0) continue;   /* pointers */
                memcpy(&fa, &sa[k], 4); memcpy(&fb, &sb, 4); memcpy(&ga, &pa[k], 4); memcpy(&gb, &pb, 4);
                nd++; o += snprintf(line + o, sizeof line - (size_t)o, " +%03X:%.3g>%.3g/%.3g>%.3g", k * 4u, (double)fa, (double)fb, (double)ga, (double)gb);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
        printf("%s (%d changed)\n", line, nd); fflush(stdout);
    }
    done++; cur_id = 0;
}
/* v1066C2 echo-only wheel RE probe (GF_COOP_VEH_WHEELDIFF=0 off): for 2 moving source cars, once per second for 12 s:
 * source forward speed, spin rate (rad/s) of the two wheel matrices veh+0x240 (angle atan2(+0x24C, +0x240)) and veh+0x2A0
 * (atan2(+0x2AC, +0x2A0)) on source AND proxy, the candidate words +0x4A8 +0x4AC +0x580 +0x9E4; plus once the non-pointer
 * words that DIFFER (constant) between source and proxy in 0x400..0x600 / 0x900..0xA00 (brake / handbrake candidates).
 * GF_COOP_VEH_WHEELCOPY=1 (echo experiment, default off): copy the source's +0x4A8 / +0x580 into the proxy every frame. */
static float wang(uint32_t veh, uint32_t off) { float c = 0, sn = 0; if (!read_f(veh + off, &c) || !read_f(veh + off + 0xC, &sn)) return 0; return atan2f(sn, c); }
static void wheel_probe(uint16_t netid, uint32_t prx, ULONGLONG now)
{
    static int s_on2 = -1, s_copy = -1, ncar; static uint16_t ids[2]; static uint32_t srcs[2], prxs[2]; static ULONGLONG t0[2], tl[2]; static float la[2][4]; static int nl[2];
    if (s_on2 < 0) { const char* e = getenv("GF_COOP_VEH_WHEELDIFF"); s_on2 = !(e && *e == '0'); e = getenv("GF_COOP_VEH_WHEELCOPY"); s_copy = e && *e == '1';
        if (s_copy) { printf("[coop-vehicle] v1066C2 WHEELCOPY experiment ON (echo: source +0x4A8/+0x580 copied into the proxy)\n"); fflush(stdout); } }
    if (!s_on2 || gf_coop_mode() != 3) return;
    int k = -1; for (int i = 0; i < ncar; i++) if (ids[i] == netid && prxs[i] == prx) k = i;
    if (k < 0) {
        uint32_t src = 0, b; float lv[3] = {0, 0, 0};
        if (ncar >= 2) return;
        __try { src = gf_coop_world_ent_of(netid); } __except (EXCEPTION_EXECUTE_HANDLER) { src = 0; }
        if (!src || src == prx || !(b = car_body(src)) || !body_vec(b, 0x180u, lv) || lv[0] * lv[0] + lv[2] * lv[2] < 4.0f) return;
        k = ncar++; ids[k] = netid; srcs[k] = src; prxs[k] = prx; t0[k] = now; tl[k] = 0; nl[k] = 0;
        {   /* one-time static diff src vs proxy (non-pointer words) */
            char line[2400]; int o = snprintf(line, sizeof line, "[coop-vehicle] v1066C2 wheelprobe net %u static diff src %08X prx %08X:", netid, src, prx), nd = 0;
            static const uint32_t rg[2][2] = {{0x400u, 0x600u}, {0x900u, 0xA00u}};
            for (int r = 0; r < 2; r++) for (uint32_t off = rg[r][0]; off < rg[r][1] && o < (int)sizeof line - 40; off += 4) {
                uint32_t a = 0, c = 0; if (!read_u32(src + off, &a) || !read_u32(prx + off, &c) || a == c) continue;
                if (a >= 0x10000000u && a < 0x50000000u && c >= 0x10000000u && c < 0x50000000u) continue;
                nd++; o += snprintf(line + o, sizeof line - (size_t)o, " +%03X:%08X/%08X", off, a, c); }
            printf("%s (%d)\n", line, nd); fflush(stdout); }
    }
    if (s_copy) { uint32_t a = 0; if (read_u32(srcs[k] + 0x4A8u, &a)) { float f; memcpy(&f, &a, 4); write_f(prx + 0x4A8u, f); } if (read_u32(srcs[k] + 0x580u, &a)) { float f; memcpy(&f, &a, 4); write_f(prx + 0x580u, f); } }
    if (nl[k] >= 12 || now - t0[k] > 20000) return;
    {
        const float sa = wang(srcs[k], 0x240u), sb = wang(srcs[k], 0x2A0u), pa = wang(prx, 0x240u), pb = wang(prx, 0x2A0u);
        if (tl[k] && now - tl[k] >= 1000) {
            const float dt = (float)(now - tl[k]) * 0.001f; float m[12], lv[3] = {0, 0, 0}, sp = 0, w4a8 = 0, w4ac = 0, w580 = 0, w9e4 = 0, p4a8 = 0, p4ac = 0, p580 = 0, p9e4 = 0;
            const uint32_t b = car_body(srcs[k]);
            if (b && body_vec(b, 0x180u, lv) && read_xf12(srcs[k], m)) sp = lv[0] * m[6] + lv[1] * m[7] + lv[2] * m[8];
            read_f(srcs[k] + 0x4A8u, &w4a8); read_f(srcs[k] + 0x4ACu, &w4ac); read_f(srcs[k] + 0x580u, &w580); read_f(srcs[k] + 0x9E4u, &w9e4);
            read_f(prx + 0x4A8u, &p4a8); read_f(prx + 0x4ACu, &p4ac); read_f(prx + 0x580u, &p580); read_f(prx + 0x9E4u, &p9e4);
            /* angle deltas sampled 1 s apart alias for fast wheels -> also print the per-frame rate measured below */
            printf("[coop-vehicle] v1066C2 wheelprobe net %u t+%.1fs src fwd speed %.2f m/s | per-frame spin A src %.2f prx %.2f rad/s, B src %.2f prx %.2f | +4A8 %.3f/%.3f +4AC %.3f/%.3f +580 %.3f/%.3f +9E4 %.3f/%.3f\n",
                   netid, (double)(now - t0[k]) / 1000.0, (double)sp, (double)la[k][0], (double)la[k][1], (double)la[k][2], (double)la[k][3],
                   (double)w4a8, (double)p4a8, (double)w4ac, (double)p4ac, (double)w580, (double)p580, (double)w9e4, (double)p9e4);
            fflush(stdout); nl[k]++; tl[k] = now; (void)dt;
        } else if (!tl[k]) tl[k] = now;
        {   /* per-frame spin rate (EMA) */
            static float ps[2][4]; static ULONGLONG pt[2]; static int pok[2];
            const float fdt = pok[k] && now > pt[k] ? (float)(now - pt[k]) * 0.001f : 0.0f; const float cur[4] = {sa, sb, pa, pb};
            if (pok[k] && fdt <= 0.004f) return;   /* same GetTickCount64 tick: accumulate */
            if (pok[k] && fdt < 0.2f) for (int j = 0; j < 4; j++) { float d = cur[j] - ps[k][j]; while (d > 3.14159f) d -= 6.28318f; while (d < -3.14159f) d += 6.28318f;
                const int slot = j == 0 ? 0 : j == 1 ? 2 : j == 2 ? 1 : 3; la[k][slot] = la[k][slot] * 0.8f + (d / fdt) * 0.2f; }
            ps[k][0] = sa; ps[k][1] = sb; ps[k][2] = pa; ps[k][3] = pb; pt[k] = now; pok[k] = 1;
        }
    }
}
/* ---------------------------------------------------------------- v1070C (agent C) car colour RE dump (echo only, default OFF)
 * GF_COOP_VEH_COLDUMP=<existing dir>: for up to 8 car pairs (host source car + its proxy twin, same process) dump both at
 * t+4 s and t+16 s after the proxy's first drive frame to <dir>\c<tick>_<netid>_<h|p>.bin. Record: [u32 BE tag][u32 BE ea]
 * [u32 BE len][bytes]. Tags: 0xE0000000 vehicle 0xA00; 0xE1000000|off vehicle pointer 0x200 (+ 0xE2000000|off<<12|off2 0x80); 0x10000000 render instance
 * *(veh+0x64) 0x68; 0x11000000|off inst pointer 0x200; 0x12000000|off<<12|off2 2nd level 0x100. Offline diff -> paint field. */
static uint8_t s_cd_buf[0x2000];
static void cd_rec(FILE* f, uint32_t tag, uint32_t ea, uint32_t len)
{
    uint8_t h[12];
    if (len > sizeof s_cd_buf || !ea_range(ea, len) || !mem_access(ea, len, 0)) return;
    __try { memcpy(s_cd_buf, vm_base + ea, len); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    h[0] = (uint8_t)(tag >> 24); h[1] = (uint8_t)(tag >> 16); h[2] = (uint8_t)(tag >> 8); h[3] = (uint8_t)tag;
    h[4] = (uint8_t)(ea >> 24); h[5] = (uint8_t)(ea >> 16); h[6] = (uint8_t)(ea >> 8); h[7] = (uint8_t)ea;
    h[8] = (uint8_t)(len >> 24); h[9] = (uint8_t)(len >> 16); h[10] = (uint8_t)(len >> 8); h[11] = (uint8_t)len;
    fwrite(h, 1, 12, f); fwrite(s_cd_buf, 1, len, f);
}
static int cd_ptr(uint32_t v) { return v >= 0x20000000u && v < 0x50000000u && !(v & 3u); }
static void cd_ent(const char* dir, int tick, uint16_t netid, char side, uint32_t ent)
{
    char path[512]; FILE* f; uint32_t inst = 0, v, w;
    snprintf(path, sizeof path, "%s\\c%d_%u_%c.bin", dir, tick, netid, side);
    if (!(f = fopen(path, "wb"))) return;
    cd_rec(f, 0xE0000000u, ent, 0xA00);
    for (uint32_t o = 0; o < 0xA00u; o += 4) if (read_u32(ent + o, &v) && cd_ptr(v) && v != ent) {
        cd_rec(f, 0xE1000000u | o, v, 0x200);
        for (uint32_t o2 = 0; o2 < 0x200u; o2 += 4) if (read_u32(v + o2, &w) && cd_ptr(w) && w != ent && w != v) cd_rec(f, 0xE2000000u | (o << 12) | o2, w, 0x80);   /* 2nd level */
    }
    if (read_u32(ent + 0x64u, &inst) && cd_ptr(inst)) {
        cd_rec(f, 0x10000000u, inst, 0x68);
        for (uint32_t o = 0; o < 0x68u; o += 4) if (read_u32(inst + o, &v) && cd_ptr(v) && v != ent) {
            cd_rec(f, 0x11000000u | o, v, 0x200);
            for (uint32_t o2 = 0; o2 < 0x200u; o2 += 4) if (read_u32(v + o2, &w) && cd_ptr(w) && w != ent && w != inst) cd_rec(f, 0x12000000u | (o << 12) | o2, w, 0x100);
        }
    }
    fclose(f);
}
static void col_dump(uint16_t netid, uint32_t prx, ULONGLONG now)
{
    static const char* dir = (const char*)1; static uint16_t ids[8]; static uint32_t prxs[8]; static ULONGLONG t0[8]; static int st[8], n;
    int k = -1; uint32_t src = 0;
    if (dir == (const char*)1) { dir = getenv("GF_COOP_VEH_COLDUMP"); if (dir && !*dir) dir = 0;
        if (dir) { printf("[coop-vehicle] v1070C colour dump ON -> %s (echo only)\n", dir); fflush(stdout); } }
    if (!dir || gf_coop_mode() != 3) return;
    for (int i = 0; i < n; i++) if (ids[i] == netid && prxs[i] == prx) { k = i; break; }
    if (k < 0) { if (n >= 8) return; k = n++; ids[k] = netid; prxs[k] = prx; t0[k] = now; st[k] = 0; return; }
    if (st[k] >= 2 || now - t0[k] < (st[k] ? 16000u : 4000u)) return;
    __try { src = gf_coop_world_ent_of(netid); } __except (EXCEPTION_EXECUTE_HANDLER) { src = 0; }
    if (!src || src == prx) { st[k] = 2; return; }
    cd_ent(dir, st[k], netid, 'h', src); cd_ent(dir, st[k], netid, 'p', prx);
    printf("[coop-vehicle] v1070C colour dump tick %d net %u src %08X prx %08X\n", st[k], netid, src, prx); fflush(stdout);
    st[k]++;
}
/* ---------------------------------------------------------------- v1070C (agent C) car paint = render model variant
 * RE (verified by reading the lifted code + the user's 21:05 joiner log): a car template carries several render models (the
 * paint variants); the factory proxy gets the template's local pick, so the joiner's copy can show another colour although the
 * template matches ("v1064K2 car look: model match 43, differs 3"). Vehicles use the generic entity SetModel. v1070C2: the
 * model-REGISTRY NODE (= func_004596B8(GUID) result; type word *(*(node+4)+8) = 0x9E3791CF / 0xF3F21004) goes to vt+0x104 = func_00464890 (palette func_0046461C
 * with *(*(obj+8)+8), then func_00470E54: vt+0x11C release, new 0x68-byte instance ctor(func_005227C4, inner model
 * *(*(node+8)+8)), GUID record node+0x1C, stored at ent+0x64). vt+0x108 = func_00464818 / func_00470FC0 expects the INNER model:
 * giving it node+8 (c70-9) left instance+0x10 = 0 (c70-9-mtest: "instance+0x10 434A7480 -> 00000000" = invisible car;
 * NOTE gf_coop_proxy.c set_model() passes node+8 to vt+0x108 for peds -- check against this (its i64 freezes?)). The echo dump (GF_COOP_VEH_COLDUMP, c70-4) found NO other per-car paint value: the
 * vehicle struct 0..0xA00 differs only in handles/positions and the render instance only in flags +0x50 (3 vs 7).
 * Fix GF_COOP_VEH_MODELFIX=1 (default ON, 0 = off): when the host's spawn ext names another model GUID than the proxy shows and
 * that model is registered here (model registry *(0x101F3670), same walk as gf_coop_proxy.c mreg_lookup), the proxy gets the
 * host's model through vt+0x104 (max 2 tries per proxy, never the car our player drives). Model not resident -> left as is
 * (counted "not resident").
 * GF_COOP_VEH_MODELTEST=1 (echo only, default off): self-test -- the first 3 proxies whose model already matches are switched to a
 * DIFFERENT registered car model GUID (taken from another car's ext); the fix must then switch them back ("restored"). */
static int s_mfix = -1, s_mtest = -1;
static unsigned st_mfix_try, st_mfix_ok, st_mfix_nores, st_mfix_fail, st_mtest_set, st_mtest_back;
/* v1070E (agent E): a host model that is not resident on the joiner is re-checked every 3 s (<= 40 times = 2 min per proxy, at
 * most one registry walk per 250 ms over all cars): the joiner's own streaming (traffic in that area) may load the variant
 * later. No stream request is made: the only model stream request found (func_00338C78, used by the ped created-handler)
 * links the REQUESTER entity into the stream entry -- a released proxy would leave a dangling requester (not proven safe).
 * GF_COOP_VEH_MODELRETRY=0 = old (one try). */
static unsigned st_mres_cars, st_mres_tries, st_mres_late;
static int mres_on(void)
{
    static int s = -1;
    if (s < 0) { const char* e = getenv("GF_COOP_VEH_MODELRETRY"); s = !(e && *e == '0');
        printf("[coop-vehicle] v1070E host model not resident -> re-check every 3 s %s (GF_COOP_VEH_MODELRETRY)\n", s ? "ON" : "off"); fflush(stdout); }
    return s;
}
static int mreg_res_of(const uint8_t want[16], uint32_t* res_out)
{
    uint32_t tab = 0, bk = 0, n = 0; long budget = 300000;
    if (!read_u32(0x101F3670u, &tab) || !ea_range(tab, 8) || !read_u32(tab, &bk) || !read_u32(tab + 4, &n)) return 0;
    if (!ea_range(bk, 4) || n == 0 || n > (1u << 20) || !mem_access(bk, (size_t)n * 4u, 0)) return 0;
    __try {
        for (uint32_t b = 0; b < n; b++) {
            uint32_t node = rd32(bk + 4 * b);
            while (node && ea_range(node, 0x30) && budget-- > 0) {
                if (!mem_access(node, 0x30, 0)) break;
                { const uint32_t r = rd32(node + 8); if (r && !memcmp(vm_base + node + 0x1C, want, 16)) { *res_out = node; return 1; } }   /* v1070C2: the NODE (= func_004596B8's result) */
                node = rd32(node + 0x2C);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 0;
}
static int car_set_model(uint32_t ent, const uint8_t want[16])
{
    uint32_t res = 0, vt = 0, opd = 0, fn = 0, inst = 0;
    if (!mreg_res_of(want, &res) || !ea_range(res, 0x30)) { st_mfix_nores++; return 0; }
    {   /* v1070C2: only the two node types func_00464890 / func_00470E54 build an instance for (else it would just drop ours) */
        uint32_t ti = 0, ty = 0;
        if (!read_u32(res + 4u, &ti) || !ea_range(ti, 0x10) || !read_u32(ti + 8u, &ty) || (ty != 0x9E3791CFu && ty != 0xF3F21004u)) { st_mfix_fail++; return 0; }
    }
    if (!read_u32(ent, &vt) || !read_u32(vt + 0x104u, &opd) || !read_u32(opd, &fn) || fn != 0x00464890u) { st_mfix_fail++; return 0; }
    {   uint32_t i0 = 0, m0 = 0, m1 = 0;
        read_u32(ent + 0x64u, &i0); if (ea_range(i0, 0x20)) read_u32(i0 + 0x10u, &m0);
        if (!call_virtual(ent, 0x104u, res, 0, 0, 0)) { st_mfix_fail++; return 0; }   /* v1070C2: vt+0x104(registry node) = the game's own path (iMsgManagedEntityCreated: func_004596B8(GUID) -> vt+0x104) */
        if (!read_u32(ent + 0x64u, &inst) || !ea_range(inst, 0x68)) { st_mfix_fail++; return 0; }   /* no render instance afterwards */
        read_u32(inst + 0x10u, &m1);
        if (st_mfix_try + st_mtest_set < 12) { printf("[coop-vehicle] v1070C SetModel car %08X: registry node %08X | instance %08X -> %08X, instance+0x10 %08X -> %08X\n", ent, res, i0, inst, m0, m1); fflush(stdout); }
    }
    return 1;
}
static void model_fix_cfg(void)
{
    if (s_mfix >= 0) return;
    { const char* e = getenv("GF_COOP_VEH_MODELFIX"); s_mfix = !(e && *e == '0'); e = getenv("GF_COOP_VEH_MODELTEST"); s_mtest = e && *e == '1'; }
    printf("[coop-vehicle] v1070C car paint/model fix %s (proxy gets the host car's render model variant via SetModel vt+0x104; GF_COOP_VEH_MODELFIX)%s\n",
           s_mfix ? "ON" : "off", s_mtest ? " + ECHO SELF-TEST (GF_COOP_VEH_MODELTEST=1)" : ""); fflush(stdout);
}
static void look_compare(vstate* v, uint32_t ent)
{
    const uint8_t* x = 0; uint8_t g[16]; uint32_t b8 = 0, hb8;
    model_fix_cfg();
    for (int i = 0; i < CX_TAB; i++) if (s_cx_id[i] == v->netid) { x = s_cx[i]; break; }
    if (!x) return;   /* no ext (yet) -> try again next frame */
    v->looked = 1;
    if (!v->mfix_tries && !v->mtest) look_diff(v->netid, ent);   /* v1064K3 (echo only) */
    hb8 = ((uint32_t)x[20] << 24) | ((uint32_t)x[21] << 16) | ((uint32_t)x[22] << 8) | x[23];
    if (!(x[2] & 1) || !gf_proxy_model_guid_of(ent, g)) {
        if ((v->mfix_tries || v->mtest == 1) && v->mwait++ < 600) { v->looked = 0; return; }   /* v1070C: swapped model may resolve a few frames later */
        st_look_unk++; return; }
    if (!memcmp(g, x + 4, 16)) {
        if (v->mfix_tries) { st_mfix_ok++; if (v->mtest == 1) { v->mtest = 2; st_mtest_back++; }
            if (v->mres_n) { st_mres_late++; printf("[coop-vehicle] v1070E net %u: host model became resident after %d re-checks -> swapped\n", v->netid, v->mres_n); fflush(stdout); v->mres_n = 0; }
            if (st_mfix_ok <= 20) { printf("[coop-vehicle] v1070C net %u car %08X: now shows the host's model %02X%02X%02X%02X.. (paint variant fixed%s)\n",
                v->netid, ent, g[0], g[1], g[2], g[3], v->mtest == 2 ? ", echo self-test RESTORED" : ""); fflush(stdout); }
            v->mfix_tries = 0; return; }
        if (read_u32(ent + 0xB8u, &b8)) { if (b8 == hb8) st_b8_match++; else st_b8_diff++; }
        st_look_match++;
        if (s_mtest && !v->mtest && st_mtest_set < 3 && gf_coop_mode() == 3 && s_mfix) {   /* echo self-test: switch to another car model */
            for (int i = 0; i < CX_TAB; i++) {
                if (!s_cx_id[i] || s_cx_id[i] == v->netid || !(s_cx[i][2] & 1) || !memcmp(s_cx[i] + 4, x + 4, 16)) continue;
                if (car_set_model(ent, s_cx[i] + 4)) {
                    uint8_t g2[16]; int sw = gf_proxy_model_guid_of(ent, g2) && memcmp(g2, x + 4, 16) != 0;
                    v->mtest = 1; st_mtest_set++; st_look_match--; v->looked = 0;
                    printf("[coop-vehicle] v1070C ECHO SELF-TEST net %u car %08X: switched to another car model %02X%02X%02X%02X.. (%s) -> the fix must restore %02X%02X%02X%02X..\n",
                           v->netid, ent, s_cx[i][4], s_cx[i][5], s_cx[i][6], s_cx[i][7], sw ? "now differs" : "STILL SAME?", x[4], x[5], x[6], x[7]); fflush(stdout);
                    return;
                }
            }
        }
        return;
    }
    if (v->mtest != 1) st_look_diff++;
    if (st_look_diff <= 10 && v->mtest != 1) { printf("[coop-vehicle] v1064K2 car look DIFFERS net %u car %08X: host model %02X%02X%02X%02X.. proxy %02X%02X%02X%02X.. (+B8 host %08X proxy %08X)\n",
        v->netid, ent, x[4], x[5], x[6], x[7], g[0], g[1], g[2], g[3], hb8, (read_u32(ent + 0xB8u, &b8), b8)); fflush(stdout); }
    if (s_mfix && v->mfix_tries < 2 && v->netid != s_claimed) {   /* v1070C: give the proxy the host's paint variant */
        const unsigned nores0 = st_mfix_nores;
        v->mfix_tries++; st_mfix_try++;
        if (car_set_model(ent, x + 4)) v->looked = 0;   /* re-check next frame */
        else {
            if (st_mfix_nores + st_mfix_fail <= 10) { printf("[coop-vehicle] v1070C net %u car %08X: host model %02X%02X%02X%02X.. %s -> keeps its own paint%s\n",
                v->netid, ent, x[4], x[5], x[6], x[7], st_mfix_nores != nores0 ? "not resident here" : "SetModel failed", st_mfix_nores != nores0 && mres_on() ? " (v1070E: re-checked every 3 s)" : ""); fflush(stdout); }
            if (st_mfix_nores != nores0 && mres_on() && v->mres_n < 40) {   /* v1070E: the joiner's own traffic may stream that variant in later */
                if (!v->mres_n) st_mres_cars++; else { st_mfix_try--; st_mfix_nores--; }   /* C's counters stay per car (first try) */
                v->mres_n++; v->mfix_tries--; st_mres_tries++;
                v->mres_at = GetTickCount64() + 3000; }
        }
    }
}

/* ---------------------------------------------------------------- v1070E (agent E) car details: wheels / steering / brake
 * input, damage + explosions, siren / police light.
 * RE (EBOOT disassembly with capstone in the cloud container, 2026-11-06; VERIFIED by reading the code):
 *  - Cars are Havok 4.1 vehicles: hkVehicleInstance (vtable 0x1010DD88, stepVehicle func_00754E98), derived by the game
 *    (stepVehicle override func_004323F8 -> func_0042EAB4 input/steering/engine/brake, func_00431CAC wheel spin). Offsets (both
 *    step functions agree): +0x18 chassis hkRigidBody (== veh+0x138), +0x1C hkVehicleData (+0x20 s8 wheel count), +0x20 driver
 *    input (vt+0xC calculateDriverInput(dt, this, deviceStatus, out)), +0x24 steering, +0x28 engine, +0x2C transmission, +0x30
 *    brake, +0x48/+0x4C wheelsInfo (0xC0 per wheel: +0x24 contact body, +0xA0 spinVelocity, +0xA4 spinAngle), +0x9C device
 *    status, +0xA0 isFixed[] (u8 per wheel), +0xB4 torque, +0xB8 rpm, +0xBC mainSteeringAngle.
 *  - device status (the game's analog status, clone func_00432F20, 0x1C B). The game's driver input (calculateDriverInput
 *    func_00432AE0) copies it 1:1 into the filtered input: accelerator = status+0x14, brake pedal = status+0x18, steering =
 *    status+8, handbrake = u8 status+0x10, trying to reverse = u8 status+0x11 (Havok's positionY +0xC is NOT used by the game:
 *    e70-2 echo probe found garbage 7.0 / 169.6 there on proxies).
 *  - e70-2 echo probe (VERIFIED): hkVehicleInstance pointer at veh+0x1B0 (game vtable 0x1005E7D8), one status object per car.
 *    Host AMBIENT traffic does not drive through it (status all 0, mainSteeringAngle 0 while driving: free-rolling wheels, the
 *    traffic AI moves the body directly); the proxies had handbrake 1 -> rear wheels isFixed (fixed 0011), spin 0.
 *  - func_00431CAC: spinAngle += spinVelocity * dt, spinVelocity = (forward slip + chassis velocity . forward) / radius, 0 for a
 *    FIXED (blocked) wheel. A driverless proxy whose status says handbrake / brake keeps blocked wheels = what v1066C measured
 *    (rear wheel matrix veh+0x2A0 never moves, front veh+0x240 slow / erratic).
 *  => GF_COOP_VEH_WHEELS=1 (default): the host writes its car's device status into the snapshot ext (v2), the joiner writes it into
 *     its proxy's status every frame -> the proxy's own Havok vehicle steers its front wheels, rolls / blocks its wheels and (INFERRED,
 *     not proven) lights its brake lights like the host's car. =2: steering + brake only (gas pedal 0), =0 off (old).
 *  - Damage: veh+0x388 damage component (secondary vtable 0x1004C630): +4 health (veh+0x38C, vt+0xBC), +8 max (veh+0x390, vt+0xC0),
 *    +0x10 (veh+0x398) 8 zone damage nibbles, +0x58 (veh+0x3E0) weak ref (handle = ent+0x38) to the LAST ATTACKER. iMsgDamage ->
 *    car message handler vt+0x18 (func_002D60BC / func_002B1BB4) -> base func_0043CBFC -> func_00433A04 -> vt+0x1F0 TakeDamage
 *    func_002CC174 (veh+0x624 bit0 = invulnerable) -> func_002A0138: health -= damage, zone nibble by hit position, attacker kept.
 *    Per frame vt+0x1EC Update(dt) func_002CD904 -> func_002CD640 -> func_0029F0AC(veh+0x388) spawns smoke / fire effects (3 slots)
 *    from the damage level.
 *  => GF_COOP_VEH_DAMAGE=1 (default): host ext v2 carries health (1/255 of max), a destroyed flag and the zone nibbles. The joiner
 *     sends its proxy the missing damage through the game's own iMsgDamage (same sender as gf_coop_combat.c: func_000283D4 new,
 *     ctor func_002951CC, func_00473FD4 synchronous) so the proxy's own code makes the smoke / fire / explosion, then copies the
 *     zone nibbles. Local proxy damage: last attacker == the local player (or the car he drives) -> forwarded to the owner (event
 *     0x0148, the owner applies it to the real car); any other local damage (proxy bumping a kerb, an explosion of the joiner's
 *     own) is UNDONE (health + zones restored). Owner side: damage whose last attacker is the co-op PUPPET (its shots are
 *     visual, the partner forwards the real hit) is undone too.
 *  - Siren (car msg handler, iMsgSetVehicleSirenOn/Off): veh+0x7C4 bit 0x80 (only if bit 0x40 = car has a siren), veh+0x7C0 = 0.
 *    Police light (iMsgSetPoliceLightOn/Off): func_002CE054(veh, on), effect handle veh+0x614 (template veh+0x5EC).
 *  => GF_COOP_VEH_SIREN=1 (default): host ext bits -> joiner proxy (same writes / call as the game's message handler).
 * SNAPSHOT EXT v2 (16 B; v1 receivers read only [0..7]): [0]=1 [1] bit0 body simulated, bit1 v2 fields valid [2..7] occupants
 *   [8] s8 steering*127 [9] s8 (accelerator - brake)*127 [10] bit0 handbrake, bit1 reverse, bit2 input valid, bit3 damage valid, bit4 destroyed,
 *   bit5 siren on, bit6 police light on, bit7 exploded (v1070E2) [11] health*255/max (>= 1 while > 0) [12..15] zone nibbles (big endian).
 * Echo-only test aids (default off): GF_COOP_VEH_WPROBE=1 (hkVehicle input / wheel time series of source + proxy),
 *   GF_COOP_VEH_WHEELS_AB=1 (input sync only in even 5 s windows -> A/B in one run), GF_COOP_VEH_DMGTEST=1 (damage script on one car). */
enum { EV_CAR_DMG = 0x0148 };
typedef struct { uint16_t netid, ver; float frac; uint32_t zones; } car_dmg_wire;   /* 12 B */
static int s_wheels = -1, s_dmg = 1, s_siren = 1, s_wprobe = 0, s_dmgtest = 0, s_wab = 0, s_redir = 1;
static uint32_t s_hkv_off, s_hkv_off2;   /* veh offset of the hkVehicleInstance pointer (+ 2nd level + 4), found by scan, logged once */
static unsigned st_hkv_scan, st_in_tx, st_in_txnz, st_in_rx, st_in_set, st_in_over, st_in_shared, st_in_nostat;
static unsigned st_dmg_burnfr, st_dmg_noeff, st_expl_ok, st_expl_miss;
static unsigned st_dmg_tx, st_dmg_apply, st_dmg_kill, st_dmg_undo, st_dmg_fwd, st_dmg_fwd_rx, st_dmg_fwd_ok, st_dmg_heal, st_dmg_zone, st_host_undo, st_sir_set, st_plt_set, st_dmg_fail;
static uint32_t local_car(int* seat);
static uint32_t puppet_car(void);
extern uint32_t gf_coop_puppet_entity(void);
static void e70_cfg(void)
{
    const char* e;
    if (s_wheels >= 0) return;
    e = getenv("GF_COOP_VEH_WHEELS"); s_wheels = !e || !*e ? 1 : atoi(e);
    if ((e = getenv("GF_COOP_VEH_DAMAGE")) && *e) s_dmg = atoi(e) != 0;
    if ((e = getenv("GF_COOP_VEH_SIREN")) && *e) s_siren = atoi(e) != 0;
    if ((e = getenv("GF_COOP_VEH_WPROBE")) && *e) s_wprobe = atoi(e);
    if ((e = getenv("GF_COOP_VEH_DMGTEST")) && *e) s_dmgtest = atoi(e);
    if ((e = getenv("GF_COOP_VEH_WHEELS_AB")) && *e) s_wab = atoi(e) != 0;
    if ((e = getenv("GF_COOP_VEH_INPUTREDIR")) && *e) s_redir = atoi(e) != 0;
    printf("[coop-vehicle] v1070E car driver input sync (wheels roll / steer / brake like the host's car) %s (GF_COOP_VEH_WHEELS)%s\n",
           s_wheels == 1 ? "ON (steer, pedal, handbrake, reverse)" : s_wheels == 2 ? "ON (steer + brake only)" : "off", s_wab ? " + ECHO A/B (even 5 s windows only)" : "");
    printf("[coop-vehicle] v1070E car damage / explosion sync %s (GF_COOP_VEH_DAMAGE) | siren + police light sync %s (GF_COOP_VEH_SIREN)%s%s\n",
           s_dmg ? "ON (health via iMsgDamage, zones, local damage forwarded or undone)" : "off", s_siren ? "ON" : "off",
           s_wprobe ? " | ECHO wheel probe ON (GF_COOP_VEH_WPROBE)" : "", s_dmgtest ? " | ECHO damage test ON (GF_COOP_VEH_DMGTEST)" : "");
    fflush(stdout);
}
static float rdf_(uint32_t ea) { float f = 0; read_f(ea, &f); return f; }
static int rd8_(uint32_t ea, uint8_t* out) { if (!mem_access(ea, 1, 0)) return 0; __try { *out = vm_base[ea]; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static int wr8_(uint32_t ea, uint8_t v) { if (!mem_access(ea, 1, 1)) return 0; __try { vm_base[ea] = v; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static int wr32_(uint32_t ea, uint32_t v) { if (!mem_access(ea, 4, 1)) return 0; __try { wr32(ea, v); return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static int hkv_ok(uint32_t p, uint32_t body)
{
    uint32_t ch = 0, n = 0, wi = 0;
    if ((p & 3u) || !ea_range(p, 0xE0) || !mem_access(p, 0xE0, 0)) return 0;
    if (!read_u32(p + 0x18u, &ch) || ch != body) return 0;
    if (!read_u32(p + 0x4Cu, &n) || n < 2 || n > 8) return 0;
    if (!read_u32(p + 0x48u, &wi) || !ea_range(wi, n * 0xC0u) || !mem_access(wi, n * 0xC0u, 0)) return 0;
    return 1;
}
static uint32_t hkv_of(uint32_t veh)
{
    uint32_t body = 0, p = 0, q = 0;
    if (!read_u32(veh + 0x138u, &body) || !ea_range(body, 0x200u)) return 0;
    if (s_hkv_off) {
        if (!read_u32(veh + s_hkv_off, &p)) return 0;
        if (s_hkv_off2) { q = p; if (!ea_range(q, 0x100) || !read_u32(q + s_hkv_off2 - 4u, &p)) return 0; }
        return hkv_ok(p, body) ? p : 0;
    }
    if (st_hkv_scan >= 64 || !mem_access(veh, 0xA00u, 0)) return 0;   /* layout constant: scan the first cars only */
    st_hkv_scan++;
    __try {
        for (uint32_t off = 0; off < 0xA00u; off += 4) { p = rd32(veh + off); if (p != body && hkv_ok(p, body)) { s_hkv_off = off;
            printf("[coop-vehicle] v1070E hkVehicleInstance found at veh+0x%03X (car %08X -> %08X, vtable %08X, wheels %u, device status %08X)\n", off, veh, p, rd32(p), rd32(p + 0x4Cu), rd32(p + 0x9Cu)); fflush(stdout); return p; } }
        for (uint32_t off = 0; off < 0xA00u; off += 4) {
            q = rd32(veh + off); if ((q & 3u) || !ea_range(q, 0x100) || q == body || !mem_access(q, 0x100, 0)) continue;
            for (uint32_t o2 = 0; o2 < 0x100u; o2 += 4) { p = rd32(q + o2); if (p != body && hkv_ok(p, body)) { s_hkv_off = off; s_hkv_off2 = o2 + 4u;
                printf("[coop-vehicle] v1070E hkVehicleInstance found at *(veh+0x%03X)+0x%02X (car %08X -> %08X, vtable %08X)\n", off, o2, veh, p, rd32(p)); fflush(stdout); return p; } }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (st_hkv_scan == 64) { printf("[coop-vehicle] v1070E hkVehicleInstance NOT found in 64 cars -> driver input sync inactive\n"); fflush(stdout); }
    return 0;
}
static uint32_t hkv_status(uint32_t p)
{
    uint32_t s = 0;
    if (!p || !read_u32(p + 0x9Cu, &s) || (s & 3u) || !ea_range(s, 0x1C) || !mem_access(s, 0x1C, 1)) return 0;
    return s;
}
/* 6-argument guest call with f1 (func_00473FD4 message send) -- own context (gcall_raw's static one may be live) */
static uint64_t gcall6(uint32_t addr, uint32_t toc, const uint64_t a[6], double f1)
{
    ppu_context* cur = g_active_ctx; gfn f; static ppu_context c;
    if (!cur || !(f = lookup(addr))) return 0;
    __try {
        memcpy(&c, cur, sizeof c);
        c.gpr[1] = ((uint32_t)cur->gpr[1] - 0x1800u) & ~0xFull; c.gpr[2] = toc;
        for (int i = 0; i < 6; i++) c.gpr[3 + i] = a[i];
        c.fpr[1] = f1; c.lr = 0;
        g_active_ctx = &c; f(&c);
        while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&c); }
        g_active_ctx = cur;
        return c.gpr[3];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_active_ctx = cur; g_trampoline_fn = 0; s_call_fail++;
        printf("[coop-vehicle] guest call %08X faulted\n", addr); fflush(stdout); return 0;
    }
}
/* the game's own damage message to a car (attacker 0 = none, else the attacker entity) */
static int car_damage_send(uint32_t target, float dmg, uint32_t attacker)
{
    float m[12]; uint32_t d, q = 0, ty = 0, tq = 0; int before = s_call_fail;
    if (!target || !(dmg > 0.0f) || !isfinite(dmg) || !read_xf12(target, m)) return 0;
    d = (uint32_t)gcall_toc(0x000283D4u, GAME_TOC, 0x3C, 0, 0, 0, 0);
    if (!ea_range(d, 0x3C) || !mem_access(d, 0x3C, 1)) { st_dmg_fail++; return 0; }
    gcall_toc(0x002951CCu, GAME_TOC, d, 0, 0, 0, 0);
    if (s_call_fail != before) { st_dmg_fail++; return 0; }
    __try {
        for (int k = 0; k < 3; k++) { uint32_t u; memcpy(&u, &m[9 + k], 4); wr32(d + 0xCu + 4u * (uint32_t)k, u); wr32(d + 0x18u + 4u * (uint32_t)k, u); }
        { uint32_t u; memcpy(&u, &dmg, 4); wr32(d + 0x24u, u); }
        wr32(d + 0x28u, 0);
        if (attacker) { const uint32_t hd = attacker + 0x38u; wr32(d + 4, hd); wr32(d + 8, rd32(hd + 4)); wr32(hd + 4, d + 4); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { st_dmg_fail++; return 0; }
    if (!read_u32(GAME_TOC + 0xD00u, &tq) || !read_u32(tq, &q) || !read_u32(GAME_TOC + 0xD04u, &ty) || ty != 0x101DB968u) { st_dmg_fail++; return 0; }
    { const uint64_t a[6] = { q, ty, target + 0x30u, 0, d, 0 }; gcall6(0x00473FD4u, GAME_TOC, a, 0.0); }
    if (s_call_fail != before) { st_dmg_fail++; return 0; }
    return 1;
}
static uint32_t weak_ent(uint32_t ref) { uint32_t h = 0; if (!read_u32(ref, &h) || h == 0 || h == 0x38u || !ea_range(h, 0x40)) return 0; return h - 0x38u; }
static int car_hp(uint32_t veh, float* hp, float* mx, uint32_t* zones)
{
    if (!read_f(veh + 0x38Cu, hp) || !read_f(veh + 0x390u, mx) || !read_u32(veh + 0x398u, zones)) return 0;
    return isfinite(*hp) && isfinite(*mx) && *mx > 0.0f && *mx < 1e7f;
}
/* host side (the peer that SOURCES the car): ext v2 bytes [8..15], plus the puppet-damage undo */
enum { HD_MAX = 128 };
static struct { uint32_t ent; float hp; uint32_t zones; ULONGLONG ms; } s_hd[HD_MAX];
static void e70_snap_ext(uint32_t ent, uint8_t* out)
{
    uint32_t p, s, f7c4 = 0, h614 = 0; float hp = 0, mx = 0; uint32_t zones = 0; uint8_t hb = 0, rv = 0;
    e70_cfg();
    out[1] |= 2;
    if ((p = hkv_of(ent)) && (s = hkv_status(p))) {
        float x = rdf_(s + 8u), y = rdf_(s + 0x14u) - rdf_(s + 0x18u);   /* steering; accelerator - brake pedal (func_00432AE0) */
        if (!isfinite(x)) x = 0; if (!isfinite(y)) y = 0;
        if (x > 1.0f) x = 1.0f; if (x < -1.0f) x = -1.0f; if (y > 1.0f) y = 1.0f; if (y < -1.0f) y = -1.0f;
        rd8_(s + 0x10u, &hb); rd8_(s + 0x11u, &rv);
        out[8] = (uint8_t)(int8_t)lrintf(x * 127.0f); out[9] = (uint8_t)(int8_t)lrintf(y * 127.0f);
        out[10] |= (uint8_t)(4u | (hb ? 1u : 0u) | (rv ? 2u : 0u));
        st_in_tx++; if (out[8] || out[9] || hb || rv) st_in_txnz++;
    }
    if (car_hp(ent, &hp, &mx, &zones)) {
        int k = -1; const ULONGLONG now = GetTickCount64();
        if (s_dmg && gf_coop_connected()) {   /* owner: damage dealt by the co-op puppet is visual only -> undo (the partner forwards the real hit) */
            for (int i = 0; i < HD_MAX; i++) if (s_hd[i].ent == ent) { k = i; break; }
            if (k < 0) { int o = 0; for (int i = 0; i < HD_MAX; i++) { if (!s_hd[i].ent) { o = i; break; } if (s_hd[i].ms < s_hd[o].ms) o = i; } k = o; s_hd[k].ent = ent; s_hd[k].hp = hp; s_hd[k].zones = zones; }
            if (hp < s_hd[k].hp - 0.01f * mx) {
                const uint32_t att = weak_ent(ent + 0x3E0u), pup = gf_coop_puppet_entity(), pc = puppet_car();
                if (att && ((pup && att == pup) || (pc && att == pc))) {
                    if (write_f(ent + 0x38Cu, s_hd[k].hp) && wr32_(ent + 0x398u, s_hd[k].zones)) { hp = s_hd[k].hp; zones = s_hd[k].zones; st_host_undo++;
                        if (st_host_undo <= 10) { printf("[coop-vehicle] v1070E owner: damage to car %08X by the co-op puppet undone (health back to %.0f / %.0f)\n", ent, (double)hp, (double)mx); fflush(stdout); } }
                }
            }
            s_hd[k].hp = hp; s_hd[k].zones = zones; s_hd[k].ms = now;
        }
        {   int q8 = (int)ceilf(hp / mx * 255.0f); if (q8 > 255) q8 = 255; if (q8 < 0) q8 = 0; if (hp > 0.0f && q8 < 1) q8 = 1;
            out[11] = (uint8_t)q8; }
        out[10] |= 8u; if (!(hp > 0.0f)) out[10] |= 0x10u;
        out[12] = (uint8_t)(zones >> 24); out[13] = (uint8_t)(zones >> 16); out[14] = (uint8_t)(zones >> 8); out[15] = (uint8_t)zones;
    }
    if (read_u32(ent + 0x7C4u, &f7c4) && (f7c4 & 0x80u)) out[10] |= 0x20u;
    if ((f7c4 & 0x10u) && (out[10] & 0x10u)) out[10] |= 0x80u;   /* v1070E2: exploded = wreck representation (+0x7C4 bit 0x10) with health <= 0 (e70-4 DMGTEST) */
    if (read_u32(ent + 0x614u, &h614) && h614) out[10] |= 0x40u;
    st_dmg_tx++;
}
/* v1070E2 (e70-4 probe: "game rewrote the proxy status" 50-65 % of frames, handbrake back to 1): the proxy car's own controller
 * keeps writing "parked" through the driver input's setters (func_00432AA4 gas / _AB0 brake / _ABC steer / _AC8 handbrake / _AD4
 * reverse, all *(driverInput+0x18) = the status; e.g. func_002A3290 = gas 0, brake 0, HANDBRAKE 1, steer 0 on *(*(this+4)+0x1C8)).
 * hkVehicle reads the status through its OWN pointer (+0x9C). So the proxy's driver input (vtable 0x1005E818, == hkVehicle+0x20)
 * gets a private copy (clone func_00432F20) at +0x18: the controller writes go there, ours go to the status the vehicle reads.
 * Ref counts (u16 +6): clone 1 (released by the driver input's dtor func_00432B0C), original -1 (the vehicle keeps its ref).
 * Undone in gf_veh_forget before the proxy is released (an adopted car goes back to the game with its normal wiring). */
static unsigned st_in_redir, st_in_redir_fail, st_in_unredir;
static int rd16_(uint32_t ea, uint16_t* v) { if (!mem_access(ea, 2, 0)) return 0; __try { *v = (uint16_t)((vm_base[ea] << 8) | vm_base[ea + 1]); return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static int wr16_(uint32_t ea, uint16_t v) { if (!mem_access(ea, 2, 1)) return 0; __try { vm_base[ea] = (uint8_t)(v >> 8); vm_base[ea + 1] = (uint8_t)v; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static void input_redirect(vstate* v, uint32_t p, uint32_t s)
{
    uint32_t di = 0, dvt = 0, cur = 0, cl, cvt = 0, svt = 0; uint16_t rc = 0;
    v->redir = -1;
    if (!s_redir) return;
    if (!read_u32(p + 0x20u, &di) || !ea_range(di, 0x20) || !read_u32(di, &dvt) || dvt != 0x1005E818u || !read_u32(di + 0x18u, &cur) || cur != s || !rd16_(s + 6u, &rc) || rc < 2 || !read_u32(s, &svt)) { st_in_redir_fail++; return; }
    cl = (uint32_t)gcall_toc(0x00432F20u, GAME_TOC, s, 0, 0, 0, 0);
    if (!ea_range(cl, 0x1C) || !mem_access(cl, 0x1C, 1) || !read_u32(cl, &cvt) || cvt != svt) { st_in_redir_fail++; return; }
    if (!wr16_(cl + 6u, 1) || !wr16_(s + 6u, (uint16_t)(rc - 1)) || !wr32_(di + 0x18u, cl)) { st_in_redir_fail++; return; }
    v->redir = 1; v->di = di; v->s_orig = s; v->s_clone = cl; st_in_redir++;
    if (st_in_redir <= 5) { printf("[coop-vehicle] v1070E2 net %u: proxy driver input %08X now writes a private status %08X (vehicle keeps reading %08X, refs %u)\n", v->netid, di, cl, s, rc); fflush(stdout); }
}
static void input_unredirect(vstate* v)
{
    uint32_t cur = 0; uint16_t rc = 0;
    if (v->redir != 1) return;
    v->redir = 0;
    __try {
        if (read_u32(v->di + 0x18u, &cur) && cur == v->s_clone && wr32_(v->di + 0x18u, v->s_orig)) {
            if (rd16_(v->s_orig + 6u, &rc)) wr16_(v->s_orig + 6u, (uint16_t)(rc + 1));
            if (rd16_(v->s_clone + 6u, &rc) && rc > 1) wr16_(v->s_clone + 6u, (uint16_t)(rc - 1));   /* the last ref is leaked (28 B) rather than freed here */
            st_in_unredir++; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
/* joiner side, every frame for a live car proxy (not the one our player drives) */
static int status_shared(const vstate* self, uint32_t s, ULONGLONG now);
static void e70_apply(vstate* v, uint32_t ent, uint32_t b, ULONGLONG now)
{
    e70_cfg();
    if (!v->ext2 || now - v->ext2_ms > 1000 || v->netid >= 0xFFF0u) { v->wr_ok = 0; return; }
    /* 1. driver input */
    if (s_wheels && (v->in_fl & 4u) && body_live(ent, b) && (!s_wab || ((now / 5000u) & 1u) == 0)) {
        const uint32_t p = hkv_of(ent), s = hkv_status(p);
        if (!s) st_in_nostat++;
        else if (status_shared(v, s, now)) { st_in_shared++; v->wr_ok = 0; }
        else {
            if (!v->redir) input_redirect(v, p, s);
            float x = (float)v->in_x / 127.0f, y = (float)v->in_y / 127.0f; const float cx = rdf_(s + 8u), cy = rdf_(s + 0x14u) - rdf_(s + 0x18u);
            if (v->wr_ok && (fabsf(cx - v->wr_x) > 0.02f || fabsf(cy - v->wr_y) > 0.02f)) { st_in_over++;   /* the game wrote its own input since our last frame */
                if (s_wprobe && st_in_over <= 12) { uint8_t h0 = 0; rd8_(s + 0x10u, &h0); printf("[coop-vehicle] v1070E wprobe net %u: game rewrote the proxy status: steer %.2f (ours %.2f), gas-brake %.2f (ours %.2f), hb %u, +C %.2f\n", v->netid, (double)cx, (double)v->wr_x, (double)cy, (double)v->wr_y, h0, (double)rdf_(s + 0xCu)); fflush(stdout); } }
            if (s_wheels == 2 && y > 0.0f) y = 0.0f;
            if (write_f(s + 8u, x) && write_f(s + 0x14u, y > 0.0f ? y : 0.0f) && write_f(s + 0x18u, y < 0.0f ? -y : 0.0f) && wr8_(s + 0x10u, (v->in_fl & 1u) ? 1 : 0) && wr8_(s + 0x11u, (v->in_fl & 2u) ? 1 : 0)) {
                v->wr_x = x; v->wr_y = y; v->wr_ok = 1; st_in_set++; }
            v->st_ptr = s;
        }
    } else v->wr_ok = 0;
    /* 2. damage */
    if (s_dmg && (v->in_fl & 8u)) {
        float hp = 0, mx = 0; uint32_t zones = 0;
        if (car_hp(ent, &hp, &mx, &zones)) {
            const float tol = 0.02f * mx > 0.5f ? 0.02f * mx : 0.5f;
            const int destroyed = (v->in_fl & 0x10u) != 0;
            const float host_hp = destroyed ? 0.0f : (float)v->hp8 * mx / 255.0f;
            uint32_t burn = 0; read_u32(ent + 0x71Cu, &burn);   /* v1070E2: fire effect handle = the car burns down (health below its 10 % floor) */
            if (burn) st_dmg_burnfr++;
            if (v->hp_ok && hp < v->hp_last - 0.01f * mx && !burn) {   /* local damage since the last frame (our own applies are already in hp_last); a burning car loses health by itself */
                int seat; const uint32_t att = weak_ent(ent + 0x3E0u), pl = player(), lc = local_car(&seat);
                if (att && ((pl && att == pl) || (lc && att == lc))) {
                    car_dmg_wire w; w.netid = v->netid; w.ver = 1; w.frac = (v->hp_last - hp) / mx; w.zones = zones;
                    if (gf_coop_send_event(EV_CAR_DMG, &w, sizeof w)) { st_dmg_fwd++; v->hold_until = now + 2500;
                        if (st_dmg_fwd <= 10) { printf("[coop-vehicle] v1070E net %u: local player damaged the car proxy (%.0f -> %.0f / %.0f) -> forwarded to the owner\n", v->netid, (double)v->hp_last, (double)hp, (double)mx); fflush(stdout); } }
                } else if (write_f(ent + 0x38Cu, v->hp_last) && wr32_(ent + 0x398u, v->zones_last)) {
                    st_dmg_undo++;
                    if (st_dmg_undo <= 10) { printf("[coop-vehicle] v1070E net %u: local damage to the car proxy by %08X (not the local player) undone (%.0f -> %.0f / %.0f)\n", v->netid, att, (double)hp, (double)v->hp_last, (double)mx); fflush(stdout); }
                    hp = v->hp_last; zones = v->zones_last;
                }
            }
            if ((hp > host_hp + tol || (destroyed && hp > 0.0f && !burn)) && now - v->dmg_at >= 200 && now >= v->noeff_until) {   /* the host's car took damage the proxy has not */
                const float d = destroyed ? hp + mx : hp - host_hp;
                v->dmg_at = now;
                if (car_damage_send(ent, d, 0)) { st_dmg_apply++; if (destroyed) st_dmg_kill++;
                    if (st_dmg_apply <= 20 || destroyed) { float h2 = hp, m2 = mx; uint32_t z2 = 0; car_hp(ent, &h2, &m2, &z2);
                        printf("[coop-vehicle] v1070E net %u: host car damage %s -> proxy health %.0f -> %.0f / %.0f (host %.0f)\n", v->netid, destroyed ? "DESTROYED" : "applied", (double)hp, (double)h2, (double)mx, (double)host_hp); fflush(stdout); } }
                { const float h0 = hp; car_hp(ent, &hp, &mx, &zones); if (!(hp < h0 - 0.5f)) { v->noeff_until = now + 3000; st_dmg_noeff++; } }   /* e.g. clamped at the 10 % floor: the burn does the rest */
            } else if (!destroyed && !burn && hp < host_hp - tol && now >= v->hold_until && hp > 0.0f) {   /* host authoritative (repaired, or a forward that was not applied) */
                if (write_f(ent + 0x38Cu, host_hp)) { st_dmg_heal++; hp = host_hp; }
            }
            if (zones != v->zones && now >= v->hold_until && wr32_(ent + 0x398u, v->zones)) { zones = v->zones; st_dmg_zone++; }
            v->hp_last = hp; v->zones_last = zones; v->hp_ok = 1;
            if ((v->in_fl & 0x80u) && !v->expl_said) {   /* v1070E2: did the proxy blow up too? */
                uint32_t f7 = 0; if (!v->hexpl_ms) v->hexpl_ms = now;
                if (read_u32(ent + 0x7C4u, &f7) && (f7 & 0x10u)) { v->expl_said = 1; st_expl_ok++;
                    printf("[coop-vehicle] v1070E2 net %u: host car exploded -> proxy exploded too (%.1f s later)\n", v->netid, (double)(now - v->hexpl_ms) / 1000.0); fflush(stdout); }
                else if (now - v->hexpl_ms > 15000) { v->expl_said = 1; st_expl_miss++;
                    printf("[coop-vehicle] v1070E2 net %u: host car exploded, proxy did NOT within 15 s (health %.0f / %.0f, burning %s)\n", v->netid, (double)hp, (double)mx, burn ? "yes" : "no"); fflush(stdout); }
            }
        }
    }
    /* 3. siren / police light */
    if (s_siren) {
        uint32_t f7c4 = 0, h614 = 0;
        if (read_u32(ent + 0x7C4u, &f7c4) && (f7c4 & 0x40u) && (((f7c4 & 0x80u) != 0) != ((v->in_fl & 0x20u) != 0))) {
            if (wr32_(ent + 0x7C4u, f7c4 ^ 0x80u)) { wr32_(ent + 0x7C0u, 0); st_sir_set++; } }
        if (read_u32(ent + 0x614u, &h614) && ((h614 != 0) != ((v->in_fl & 0x40u) != 0)) && now - v->plt_at >= 1500) {
            v->plt_at = now; gcall_toc(0x002CE054u, GAME_TOC, ent, (v->in_fl & 0x40u) ? 1u : 0u, 0, 0, 0); st_plt_set++; }
    }
}
static int status_shared(const vstate* self, uint32_t s, ULONGLONG now)
{
    for (int i = 0; i < VS_MAX; i++) if (&s_v[i] != self && s_v[i].ent && s_v[i].ent != self->ent && s_v[i].st_ptr == s && now - s_v[i].last_ms < 1000) return 1;
    if (gf_coop_mode() == 3) {   /* echo: never write into the SOURCE car's status */
        uint32_t src = 0; __try { src = gf_coop_world_ent_of(self->netid); } __except (EXCEPTION_EXECUTE_HANDLER) { src = 0; }
        if (src && src != self->ent && hkv_status(hkv_of(src)) == s) return 1;
    }
    return 0;
}
/* owner: a forwarded hit (the partner's player damaged our car's proxy) -> the real car */
static volatile LONG s_dq_w, s_dq_r; static car_dmg_wire s_dq[32];
static void e70_rx_tick(void)
{
    while (s_dq_r != s_dq_w) {
        const car_dmg_wire w = s_dq[s_dq_r & 31]; uint32_t src = 0; float hp = 0, mx = 0; uint32_t z = 0;
        InterlockedIncrement(&s_dq_r);
        st_dmg_fwd_rx++;
        if (!s_dmg || !(w.frac > 0.0f) || w.frac > 2.0f) continue;
        __try { src = gf_coop_world_ent_of(w.netid); } __except (EXCEPTION_EXECUTE_HANDLER) { src = 0; }
        if (!src || !car_hp(src, &hp, &mx, &z)) continue;
        {   const uint32_t pup = gf_coop_puppet_entity();   /* attacker = the partner's puppet: the game's AI / wanted logic sees who did it */
            if (car_damage_send(src, w.frac * mx, pup)) { float h2 = hp; car_hp(src, &h2, &mx, &z); st_dmg_fwd_ok++;
                for (int i = 0; i < HD_MAX; i++) if (s_hd[i].ent == src) { s_hd[i].hp = h2; s_hd[i].zones = z; }   /* not undone as puppet damage */
                if (st_dmg_fwd_ok <= 10) { printf("[coop-vehicle] v1070E owner: partner damaged car net %u -> real car %08X health %.0f -> %.0f / %.0f\n", w.netid, src, (double)hp, (double)h2, (double)mx); fflush(stdout); } } }
    }
}
/* ---- echo-only RE probe (GF_COOP_VEH_WPROBE=1): for 3 MOVING source/proxy pairs, once per second for 15 s: action world,
 * device status (steer, pedal, handbrake, reverse), main steering angle, wheel 0 / 2 spin velocity + fixed flags + contacts,
 * the visual wheel matrices veh+0x240 / +0x2A0 angle, and whether our input write is active. */
static void wp_side(char* o, size_t n, uint32_t veh)
{
    const uint32_t p = hkv_of(veh), s = hkv_status(p); uint32_t world = 0, fx = 0, wi = 0, nw = 0; uint8_t hb = 0, rv = 0, f[4] = {0, 0, 0, 0}; int contacts = 0;
    float w0 = 0, w2 = 0, a0 = 0;
    if (!p) { snprintf(o, n, "no hkVehicle"); return; }
    read_u32(p + 0x8u, &world); read_u32(p + 0xA0u, &fx); read_u32(p + 0x48u, &wi); read_u32(p + 0x4Cu, &nw);
    if (ea_range(fx, 4)) for (int k = 0; k < 4 && k < (int)nw; k++) rd8_(fx + (uint32_t)k, &f[k]);
    if (s) { rd8_(s + 0x10u, &hb); rd8_(s + 0x11u, &rv); }
    if (ea_range(wi, nw * 0xC0u)) { read_f(wi + 0xA0u, &w0); read_f(wi + 0xA4u, &a0); if (nw > 2) read_f(wi + 2u * 0xC0u + 0xA0u, &w2);
        for (uint32_t k = 0; k < nw && k < 8; k++) { uint32_t cb = 0; if (read_u32(wi + k * 0xC0u + 0x24u, &cb) && cb) contacts++; } }
    {   uint32_t vi = 0, vs = 0, vb = 0, md = 0, fl = 0, fi = 0, fs = 0, fb = 0, t;
        if (read_u32(p + 0x20u, &t) && ea_range(t, 4) && read_u32(t, &vi) && ea_range(vi + 0xCu, 4) && read_u32(vi + 0xCu, &t) && ea_range(t, 4)) read_u32(t, &fi);
        if (read_u32(p + 0x24u, &t) && ea_range(t, 4) && read_u32(t, &vs) && ea_range(vs + 0xCu, 4) && read_u32(vs + 0xCu, &t) && ea_range(t, 4)) read_u32(t, &fs);
        if (read_u32(p + 0x30u, &t) && ea_range(t, 4) && read_u32(t, &vb) && ea_range(vb + 0xCu, 4) && read_u32(vb + 0xCu, &t) && ea_range(t, 4)) read_u32(t, &fb);
        read_u32(p + 0x108u, &md); read_u32(p + 0x10Cu, &fl);
        const int w = snprintf(o, n, "vt %08X input %08X/%08X steer %08X/%08X brake %08X/%08X mode %u fl %X | ", rd32(p), vi, fi, vs, fs, vb, fb, md, fl);
        if (w > 0 && (size_t)w < n) { o += w; n -= (size_t)w; } }
    snprintf(o, n, "world %08X status %08X steer %.2f gas %.2f brake %.2f (+C %.2f) hb %u rev %u | mainSteer %.3f | spin w0 %.1f w2 %.1f rad/s (angle0 %.2f) fixed %u%u%u%u contacts %d | vis A %.2f B %.2f",
             world, s, s ? (double)rdf_(s + 8u) : 0.0, s ? (double)rdf_(s + 0x14u) : 0.0, s ? (double)rdf_(s + 0x18u) : 0.0, s ? (double)rdf_(s + 0xCu) : 0.0, hb, rv, (double)rdf_(p + 0xBCu), (double)w0, (double)w2, (double)a0, f[0], f[1], f[2], f[3], contacts,
             (double)wang(veh, 0x240u), (double)wang(veh, 0x2A0u));
}
static void wheel_probe2(vstate* v, uint32_t prx, ULONGLONG now)
{
    static uint16_t ids[3]; static ULONGLONG t0[3], tl[3]; static int n;
    int k = -1; uint32_t src = 0, b; float lv[3] = {0, 0, 0};
    if (s_wprobe <= 0 || gf_coop_mode() != 3 || v->netid >= 0xFFF0u) return;
    for (int i = 0; i < n; i++) if (ids[i] == v->netid) k = i;
    __try { src = gf_coop_world_ent_of(v->netid); } __except (EXCEPTION_EXECUTE_HANDLER) { src = 0; }
    if (!src || src == prx) return;
    if (k < 0) {
        if (n >= 3 || !(b = car_body(src)) || !body_vec(b, 0x180u, lv) || lv[0] * lv[0] + lv[2] * lv[2] < 4.0f) return;
        k = n++; ids[k] = v->netid; t0[k] = now; tl[k] = 0;
    }
    if (now - t0[k] > 15000 || (tl[k] && now - tl[k] < 1000)) return;
    tl[k] = now;
    {   char a[400], c[400]; float sp = 0; const uint32_t bs = car_body(src);
        if (bs && body_vec(bs, 0x180u, lv)) sp = sqrtf(lv[0] * lv[0] + lv[2] * lv[2]);
        wp_side(a, sizeof a, src); wp_side(c, sizeof c, prx);
        printf("[coop-vehicle] v1070E wprobe net %u t+%.0fs speed %.1f m/s | SRC %08X %s\n[coop-vehicle] v1070E wprobe net %u        PRX %08X %s | ext in %d/%d fl %02X write %s (set %u over %u shared %u nostatus %u)\n",
               v->netid, (double)(now - t0[k]) / 1000.0, (double)sp, src, a, v->netid, prx, c, v->in_x, v->in_y, v->in_fl, v->wr_ok ? "ON" : "off", st_in_set, st_in_over, st_in_shared, st_in_nostat);
        fflush(stdout);
    }
}
/* ---- echo-only damage script (GF_COOP_VEH_DMGTEST=1) on ONE source/proxy pair, steps 7 s apart from t+60 s:
 * 1 owner car -30 % (attacker none: proxy must follow), 2 proxy -20 % by the local player (must be forwarded: owner drops),
 * 3 proxy -20 % by nobody (must be undone), 4 owner car -20 % by the puppet (owner must undo), 5 owner car destroyed (proxy must
 * follow). Logs both cars 1 s and 5 s after each step + the vehicle words that changed with the destruction. */
static void dt_state(const char* tag, int step, uint32_t veh)
{
    float hp = 0, mx = 0; uint32_t z = 0, f7c4 = 0, f624 = 0, f9b0 = 0, f71c = 0, att; uint8_t g[16]; int gok;
    car_hp(veh, &hp, &mx, &z); read_u32(veh + 0x7C4u, &f7c4); read_u32(veh + 0x624u, &f624); read_u32(veh + 0x9B0u, &f9b0); read_u32(veh + 0x71Cu, &f71c);
    att = weak_ent(veh + 0x3E0u); gok = gf_proxy_model_guid_of(veh, g);
    printf("[coop-vehicle] v1070E dmgtest step %d %s %08X: health %.1f / %.1f zones %08X | +7C4 %08X +624 %08X +9B0 %08X +71C %08X | last attacker %08X | body live %d | model %02X%02X%02X%02X\n",
           step, tag, veh, (double)hp, (double)mx, z, f7c4, f624, f9b0, f71c, att, body_live(veh, car_body(veh)), gok ? g[0] : 0, gok ? g[1] : 0, gok ? g[2] : 0, gok ? g[3] : 0);
    fflush(stdout);
}
static uint32_t s_dt_snap[2][0x280];
static void dt_diff(const char* tag, int which, uint32_t veh)
{
    char line[2400]; int o = snprintf(line, sizeof line, "[coop-vehicle] v1070E dmgtest destroyed-state diff %s %08X:", tag, veh), nd = 0;
    if (!mem_access(veh, 0xA00u, 0)) return;
    __try {
        for (uint32_t k = 0; k < 0x280u && o < (int)sizeof line - 40; k++) {
            const uint32_t a = s_dt_snap[which][k], c = rd32(veh + k * 4u);
            if (a == c || (a >= 0x10000000u && a < 0x50000000u && c >= 0x10000000u && c < 0x50000000u)) continue;
            nd++; o += snprintf(line + o, sizeof line - (size_t)o, " +%03X:%08X>%08X", k * 4u, a, c);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    printf("%s (%d)\n", line, nd); fflush(stdout);
}
static void dmg_test_tick(ULONGLONG now)
{
    static ULONGLONG t_arm, t_step; static int step; static uint16_t id; static uint32_t prx, src;
    if (s_dmgtest <= 0 || gf_coop_mode() != 3) return;
    if (!t_arm) { t_arm = now + 60000; printf("[coop-vehicle] v1070E DMGTEST armed (first step in 60 s)\n"); fflush(stdout); }
    if (now < t_arm) return;
    if (!id) {   /* pick the live proxy nearest to the player whose source is a separate, healthy car */
        float pm[12], bd = 1e12f; const uint32_t pl = player(); int best = -1;
        if (!pl || !read_xf12(pl, pm)) return;
        for (int i = 0; i < VS_MAX; i++) {
            float cm[12], hp, mx, d2 = 0; uint32_t z, s2 = 0;
            if (!s_v[i].ent || now - s_v[i].last_ms > 300 || s_v[i].netid >= 0xFFF0u || !s_v[i].ext2) continue;
            __try { s2 = gf_coop_world_ent_of(s_v[i].netid); } __except (EXCEPTION_EXECUTE_HANDLER) { s2 = 0; }
            if (!s2 || s2 == s_v[i].ent || !car_hp(s2, &hp, &mx, &z) || !(hp > 0.5f * mx) || !read_xf12(s_v[i].ent, cm)) continue;
            for (int k = 0; k < 3; k++) { const float d = cm[9 + k] - pm[9 + k]; d2 += d * d; }
            if (d2 < bd) { bd = d2; best = i; }
        }
        if (best < 0) { t_arm = now + 5000; return; }
        id = s_v[best].netid; prx = s_v[best].ent; src = gf_coop_world_ent_of(id); step = 0; t_step = now;
        printf("[coop-vehicle] v1070E DMGTEST car net %u: source %08X, proxy %08X (%.0f m from the player)\n", id, src, prx, (double)sqrtf(bd)); fflush(stdout);
    }
    {   int live = 0; for (int i = 0; i < VS_MAX; i++) if (s_v[i].ent == prx && s_v[i].netid == id && now - s_v[i].last_ms < 6000) live = 1;
        if (!live && step < 6) { printf("[coop-vehicle] v1070E DMGTEST proxy net %u gone at step %d -> stop\n", id, step); fflush(stdout); step = 9; return; } }
    if (step >= 6) return;
    {   const ULONGLONG dt = now - t_step;
        static int logged1, logged5;
        if (step > 0 && dt >= 1000 && !logged1) { logged1 = 1; dt_state("SRC", step, src); dt_state("PRX", step, prx); }
        if (step > 0 && dt >= 5000 && !logged5) { logged5 = 1; dt_state("SRC", step, src); dt_state("PRX", step, prx);
            if (step == 5) { dt_diff("SRC", 0, src); dt_diff("PRX", 1, prx); } }
        if (step > 0 && dt < 7000) return;
        if (step == 5) { step = 6; printf("[coop-vehicle] v1070E DMGTEST done: applied %u (kills %u), forwarded %u -> owner applied %u, proxy undo %u, owner puppet undo %u, heals %u, zone copies %u, send failures %u\n",
            st_dmg_apply, st_dmg_kill, st_dmg_fwd, st_dmg_fwd_ok, st_dmg_undo, st_host_undo, st_dmg_heal, st_dmg_zone, st_dmg_fail); fflush(stdout); return; }
        logged1 = logged5 = 0; step++; t_step = now;
        {   float hp = 0, mx = 0; uint32_t z = 0; int ok = 0; const char* what = "";
            dt_state("SRC", step * 10, src); dt_state("PRX", step * 10, prx);
            if (step == 5 && mem_access(src, 0xA00u, 0) && mem_access(prx, 0xA00u, 0)) { __try { for (uint32_t k = 0; k < 0x280u; k++) { s_dt_snap[0][k] = rd32(src + k * 4u); s_dt_snap[1][k] = rd32(prx + k * 4u); } } __except (EXCEPTION_EXECUTE_HANDLER) { } }
            if (step == 1 || step == 4 || step == 5) { car_hp(src, &hp, &mx, &z); }
            else car_hp(prx, &hp, &mx, &z);
            switch (step) {
            case 1: what = "owner car -30% (no attacker)"; ok = car_damage_send(src, 0.30f * mx, 0); break;
            case 2: what = "proxy -20% by the local player"; ok = car_damage_send(prx, 0.20f * mx, player()); break;
            case 3: what = "proxy -20% by nobody"; ok = car_damage_send(prx, 0.20f * mx, 0); break;
            case 4: what = "owner car -20% by the puppet"; ok = gf_coop_puppet_entity() ? car_damage_send(src, 0.20f * mx, gf_coop_puppet_entity()) : 0; break;
            case 5: what = "owner car DESTROYED (2x max)"; ok = car_damage_send(src, 2.0f * mx, 0); break;
            }
            printf("[coop-vehicle] v1070E DMGTEST step %d: %s -> %s\n", step, what, ok ? "sent" : "SEND FAILED"); fflush(stdout);
            dt_state("SRC", step * 10 + 1, src); dt_state("PRX", step * 10 + 1, prx);
        }
    }
}

/* gf_coop_proxy.c drive_one delegates every LIVE car proxy here (v1063V hook). 1 = handled, 0 = use the old path,
 * -1 = the entity refused our calls (proxy releases / respawns it). tgt = interpolated host matrix (echo offset included),
 * vel = host velocity, tgt_ms = when W2 last updated tgt (GetTickCount64). */
int gf_veh_proxy_drive(uint16_t netid, uint32_t ent, const float tgt[12], const float vel[3], unsigned long long tgt_ms, float dt)
{
    const ULONGLONG now = GetTickCount64();
    vstate* v; uint32_t b; float cur[12], want[12], err[3], e, rv[3], rang = 0, lin[3], ang[3];
    if (!veh_on() || s_drive == 0 || !ent || !tgt) return 0;
    v = vs_get(ent, netid, now);
    if (s_claimed && netid == s_claimed) { v->last_ms = now; v->init = 0; return 1; }   /* our player drives it: hands off */
    b = car_body(ent);
    if (!b) { st_nobody++; return 0; }
    if (!v->probed) probe_car(v, ent, b);
    if (!v->looked && netid != 0xFFFEu) look_compare(v, ent);   /* v1064K2 */
    if (netid < 0xFFF0u) wheel_diff(netid, ent, now);            /* v1066C echo RE aid */
    if (netid < 0xFFF0u) wheel_probe(netid, ent, now);           /* v1066C2 echo RE aid */
    if (netid < 0xFFF0u) col_dump(netid, ent, now);              /* v1070C echo colour RE dump (GF_COOP_VEH_COLDUMP) */
    __try { e70_apply(v, ent, b, now); wheel_probe2(v, ent, now); } __except (EXCEPTION_EXECUTE_HANDLER) { }   /* v1070E input / damage / siren */
    if (v->mres_at && now >= v->mres_at && v->looked) { static ULONGLONG s_mres_last; if (now - s_mres_last >= 250) { s_mres_last = now; v->mres_at = 0; v->looked = 0; } }   /* v1070E */
    if (!read_xf12(ent, cur)) return 0;
    if (!body_live(ent, b)) {   /* not physically simulated right now: the game's own placement every frame (old path) */
        memcpy(want, tgt, sizeof want);
        v->last_ms = now; v->init = 0; st_kin++;
        if (!place_car(ent, want, 0)) { st_fail++; return -1; }
        return 1;
    }
    {   /* body position is newer than the entity matrix (which lags one physics step) */
        float bp[3]; if (body_vec(b, 0xE0u, bp)) { const float dx = bp[0] - cur[9], dz = bp[2] - cur[11]; if (dx * dx + dz * dz < 4.0f) { cur[9] = bp[0]; cur[10] = bp[1]; cur[11] = bp[2]; } }
    }
    memcpy(want, tgt, sizeof want);
    {
        float ahead = (float)(now - tgt_ms) * 0.001f; if (ahead > 0.25f) ahead = 0.25f; if (ahead < 0.0f) ahead = 0.0f;
        if (vel) for (int k = 0; k < 3; k++) want[9 + k] += vel[k] * ahead;
    }
    /* target angular velocity: snapshot avel if fresh (v1063H), else finite difference of successive targets */
    {   /* v1066C: QPC-timed (GetTickCount64 ticks in 15.6 ms steps) */
        extern uint32_t gf_coop_pose_now_ms(void); const double qn = (double)gf_coop_pose_now_ms();
        if (v->init && v->w_ms > 0 && qn > v->w_ms) {
            const float tdt = (float)(qn - v->w_ms) * 0.001f;
            if (memcmp(v->prev_tgt, tgt, 9 * sizeof(float)) != 0 && tdt > 0.004f && tdt < 0.5f) {
                float wv[3], a; rot_err(tgt, v->prev_tgt, wv, &a);
                for (int k = 0; k < 3; k++) { const float w = wv[k] / tdt; v->w[k] = isfinite(w) && fabsf(w) < 10.0f ? v->w[k] * 0.5f + w * 0.5f : 0.0f; }
                memcpy(v->prev_tgt, tgt, sizeof v->prev_tgt); v->prev_ms = now; v->w_ms = qn;
            } else if (tdt >= 0.5f) { memcpy(v->prev_tgt, tgt, sizeof v->prev_tgt); v->prev_ms = now; v->w_ms = qn; memset(v->w, 0, sizeof v->w); }
        } else { memcpy(v->prev_tgt, tgt, sizeof v->prev_tgt); v->prev_ms = now; v->w_ms = qn; }
    }
    if (s_aff && v->init) memcpy(ang, v->w, sizeof ang);
    else if (now - v->avel_ms < 300) memcpy(ang, v->avel_in, sizeof ang); else memcpy(ang, v->w, sizeof ang);
    for (int k = 0; k < 3; k++) err[k] = want[9 + k] - cur[9 + k];
    e = sqrtf(err[0] * err[0] + err[1] * err[1] + err[2] * err[2]);
    rot_err(want, cur, rv, &rang);
    if (cur[4] < 0.0f) st_upside++;   /* v1064K1: car up row points down (upside down) */
    if (netid != 0xFFFEu) diag_car(netid, ent, b, want, cur, rang, now);   /* v1064K1 */
    v->last_ms = now;
    st_drive++;
    if (v->snap_cool > 0) v->snap_cool--;
    if (e > 0.5f) st_e05++; if (e > 1.0f) st_e1++;   /* v1066C error histogram */
    {   /* v1066C blended re-snap: between SNAP and HARDSNAP the servo catches up (no teleport) for up to 1.5 s */
        const int off = e > s_snap || rang > 1.05f;
        if (!off) { if (v->catch_since) st_catch_ok++; v->catch_since = 0; }
        else if (s_blend && v->init && e <= s_hardsnap && rang <= 1.57f) {
            if (!v->catch_since) v->catch_since = now;
            if (now - v->catch_since <= 1500) { v->snap_cool = v->snap_cool > 1 ? v->snap_cool : 1; st_catch++; }   /* hold the teleport */
            else v->catch_since = 0;   /* failed: teleport below */
        }
        else if (s_catchfix && v->catch_since) { v->catch_since = 0; v->snap_cool = 0; st_catchfix++; }   /* v1068S: fell beyond HARDSNAP while catching -> re-snap now */
    }
    if (s_unstick && v->init && !v->catch_since) {   /* v1068S unstick: moving target, body not following, well off -> re-snap */
        const float tsp = vel ? sqrtf(vel[0] * vel[0] + vel[2] * vel[2]) : 0.0f; float bv[3] = {0, 0, 0};
        int stuck = 0;
        if (tsp > 3.0f && e > 2.5f && body_vec(b, 0x180u, bv)) { const float bsp = sqrtf(bv[0] * bv[0] + bv[2] * bv[2]); stuck = bsp < 0.25f * tsp; }
        if (!stuck) v->stuck_since = 0;
        else if (!v->stuck_since) v->stuck_since = now;
        else if (now - v->stuck_since >= s_unstick_ms) { v->stuck_since = 0; v->snap_cool = 0; v->catch_since = 0; st_unstick++;
            if (e <= s_snap) e = s_snap + 0.01f;   /* force the re-snap branch below */ }
    }
    if (!v->init || ((e > s_snap || rang > 1.05f) && v->snap_cool <= 0 && !v->catch_since)) {   /* first frame / far off / > 60 deg: game placement, then velocities */
        v->snap_cool = 15; v->vset_ok = 0; memset(v->drag, 0, sizeof v->drag); v->catch_since = 0; memset(v->integ, 0, sizeof v->integ); v->ppos_ok = 0; v->dlat[0] = v->dlat[1] = v->dyaw = 0; v->aset_ok = 0;
        if (!v->init) st_init++;
        if (!place_car(ent, want, 1)) { st_fail++; return -1; }
        set_body_xform(b, want);
        if (s_logv && v->init && (st_snap <= 40 || st_snap % 200 == 0)) {
            float lv[3] = {0, 0, 0}; uint32_t f7c4 = 0; body_vec(b, 0x180u, lv); read_u32(ent + 0x7C4u, &f7c4);
            printf("[coop-vehicle] v1063V net %u snap detail: car %.1f %.1f %.1f -> target %.1f %.1f %.1f (%.1f m, %.0f deg), det car %.2f target %.2f, at car %.2f %.2f %.2f target %.2f %.2f %.2f, body vel %.1f %.1f %.1f, +7C4 %08X\n", netid,
                   (double)cur[9], (double)cur[10], (double)cur[11], (double)want[9], (double)want[10], (double)want[11], (double)e, (double)(rang * 57.3f), (double)det3(cur), (double)det3(want),
                   (double)cur[6], (double)cur[7], (double)cur[8], (double)want[6], (double)want[7], (double)want[8], (double)lv[0], (double)lv[1], (double)lv[2], f7c4); fflush(stdout); }
        if (v->init) { st_snap++;   /* v1066C: init placements counted separately (st_init) */
            if (st_resnap_log < 20 || st_snap % 50 == 0) { float lv[3] = {0, 0, 0}; uint32_t f7c4 = 0; body_vec(b, 0x180u, lv); read_u32(ent + 0x7C4u, &f7c4); st_resnap_log++;
                printf("[coop-vehicle] v1068S re-snap net %u car %08X: %.1f m / %.0f deg off, target speed %.1f m/s, body speed %.1f m/s, +7C4 %08X (catch-fix total %u, unstick total %u)\n", netid, ent, (double)e, (double)(rang * 57.3f),
                       vel ? sqrt((double)vel[0] * vel[0] + (double)vel[2] * vel[2]) : 0.0, sqrt((double)lv[0] * lv[0] + (double)lv[2] * lv[2]), f7c4, st_catchfix, st_unstick); fflush(stdout); } }
        v->init = 1;
        if (s_logv && (st_snap <= 20 || st_snap % 200 == 0)) { printf("[coop-vehicle] v1063V net %u snap (%.1f m, %.0f deg off)\n", netid, (double)e, (double)(rang * 57.3f)); fflush(stdout); }
        { float l0[3] = {0, 0, 0}; if (vel) memcpy(l0, vel, sizeof l0); set_body_vel(b, l0, ang); }
        return 1;
    }
    st_err_sum += e; st_rerr_sum += rang; if (e > st_err_max) st_err_max = e; st_err_n++;
    if (s_farkin > 0.0f && netid != 0xFFFEu) {   /* v1068S FARKIN (hysteresis 10 m); never for the host's guest-claimed car (0xFFFE) */
        float pm[12]; const uint32_t pl = player();
        if (pl && read_xf12(pl, pm)) { const float dx = want[9] - pm[9], dz = want[11] - pm[11], d2 = dx * dx + dz * dz;
            const float on = s_farkin * s_farkin, off = (s_farkin - 10.0f) * (s_farkin - 10.0f);
            if (!v->farkin && d2 > on) v->farkin = 1; else if (v->farkin && d2 < off) { v->farkin = 0; v->vset_ok = 0; memset(v->integ, 0, sizeof v->integ); v->ppos_ok = 0; } }
    }
    if (s_drive == 2 || (v->farkin && s_farkin > 0.0f)) {   /* hard keyframe: exact target pose + host velocities (v1068S: also far proxies) */
        if (v->farkin) st_farkin++;
        if (!place_car(ent, want, 0)) { st_fail++; return -1; }
        set_body_xform(b, want);
        st_hard++;
        { float l0[3] = {0, 0, 0}; if (vel) memcpy(l0, vel, sizeof l0); set_body_vel(b, l0, ang); }
        return 1;
    }
    {   /* soft: velocity servo */
        float c[3], cl; const int catching = v->catch_since != 0; const float kp = catching ? s_kp * 2.5f : s_kp, mc = catching ? 25.0f : s_maxcorr;
        for (int k = 0; k < 3; k++) c[k] = err[k] * kp;
        cl = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
        if (cl > mc) for (int k = 0; k < 3; k++) c[k] *= mc / cl;
        for (int k = 0; k < 3; k++) lin[k] = (vel ? vel[k] : 0.0f) + c[k];
        {   /* v1066C diagnostics (along-track / lateral error, physics advance ratio) + integral term */
            extern uint32_t gf_coop_pose_now_ms(void); const double qn = (double)gf_coop_pose_now_ms();
            const float sp = vel ? sqrtf(vel[0] * vel[0] + vel[2] * vel[2]) : 0.0f;
            const float fdt = v->ppos_ok && qn > v->pq_ms ? (float)(qn - v->pq_ms) * 0.001f : 0.0f;
            if (sp > 3.0f) { const float dx = vel[0] / sp, dz = vel[2] / sp;
                st_along_sum += err[0] * dx + err[2] * dz; st_lat_sum += fabsf(err[0] * dz - err[2] * dx); st_along_n++;
                st_mv_err += e; st_mv_rot += rang; if (e > st_mv_max) st_mv_max = e;   /* moving cars only */
                if (v->ppos_ok && fdt > 0.005f && fdt < 0.1f) {   /* how far the body really moved vs what we commanded for that wall time */
                    const float cmd = (v->pset[0] * dx + v->pset[2] * dz) * fdt, got = (cur[9] - v->ppos[0]) * dx + (cur[11] - v->ppos[2]) * dz;
                    if (cmd > 0.05f) { st_ratio_sum += got / cmd; st_ratio_n++; } } }
            if (s_ki > 0.0f && fdt > 0.0f && fdt < 0.1f) {   /* v1066C2 anti-windup: leaky (2 s), |corr| <= 3 m/s */
                const float leak = 1.0f - fdt * 0.5f; v->integ[0] *= leak; v->integ[2] *= leak;
                if (e < 3.0f && !catching) { v->integ[0] += err[0] * fdt; v->integ[2] += err[2] * fdt; }   /* v1066C4: also when parked (c66-i: a parked twin kept a 0.5 m offset) */
                { const float il = sqrtf(v->integ[0] * v->integ[0] + v->integ[2] * v->integ[2]) * s_ki; if (il > 3.0f) { v->integ[0] *= 3.0f / il; v->integ[2] *= 3.0f / il; } }
            }
            if (s_ki > 0.0f) { lin[0] += s_ki * v->integ[0]; lin[2] += s_ki * v->integ[2]; st_icorr_sum += s_ki * sqrtf(v->integ[0] * v->integ[0] + v->integ[2] * v->integ[2]); }
            v->pq_ms = qn; v->ppos[0] = cur[9]; v->ppos[1] = cur[10]; v->ppos[2] = cur[11]; v->ppos_ok = 1;
        }
        if (s_drag) {   /* v1066C drag compensation: what the proxy car's own physics removed ALONG the direction of travel since our
                         * last set (driverless car: brakes / rolling friction). Scalar, >= 0, horizontal only (vertical = ground contact). */
            float bv[3], dir[3] = {vel ? vel[0] : 0.0f, 0.0f, vel ? vel[2] : 0.0f}; const float sp = sqrtf(dir[0] * dir[0] + dir[2] * dir[2]);
            if (sp > 1.0f) { dir[0] /= sp; dir[2] /= sp;
                if (v->vset_ok && body_vec(b, 0x180u, bv)) {
                    const float loss = (v->vset[0] - bv[0]) * dir[0] + (v->vset[2] - bv[2]) * dir[2];
                    if (fabsf(loss) < 6.0f && isfinite(loss)) v->drag[0] = v->drag[0] * 0.8f + loss * 0.2f; else st_imp++;   /* impulse / collision: not drag */
                    if (s_drag >= 2) {   /* v1066C3 lateral: what the tyres took away sideways (driverless car scrubbing its correction) */
                        const float lx = v->vset[0] - bv[0] - loss * dir[0], lz = v->vset[2] - bv[2] - loss * dir[2];
                        if (fabsf(lx) + fabsf(lz) < 4.0f && isfinite(lx + lz)) { v->dlat[0] = v->dlat[0] * 0.8f + lx * 0.2f; v->dlat[1] = v->dlat[1] * 0.8f + lz * 0.2f; }
                        { const float ll = sqrtf(v->dlat[0] * v->dlat[0] + v->dlat[1] * v->dlat[1]); if (ll > 3.0f) { v->dlat[0] *= 3.0f / ll; v->dlat[1] *= 3.0f / ll; } st_dlat_sum += ll > 3.0f ? 3.0f : ll; }
                    }
                    if (v->drag[0] < 0.0f) v->drag[0] = 0.0f; if (v->drag[0] > 6.0f) v->drag[0] = 6.0f;
                    st_drag_sum += v->drag[0]; st_drag_n++;
                }
                lin[0] += dir[0] * v->drag[0]; lin[2] += dir[2] * v->drag[0];
                if (s_drag >= 2) { lin[0] += v->dlat[0]; lin[2] += v->dlat[1]; }
            } else { v->drag[0] *= 0.9f; v->dlat[0] *= 0.9f; v->dlat[1] *= 0.9f; }
        }
        memcpy(v->vset, lin, sizeof v->vset); v->vset_ok = 1; memcpy(v->pset, lin, sizeof v->pset);
        if (s_rclamp < 0) { const char* e = getenv("GF_COOP_VEH_ROTCLAMP"); s_rclamp = !(e && *e == '0');
            printf("[coop-vehicle] v1064K1 rotation servo clamp %s (GF_COOP_VEH_ROTCLAMP)\n", s_rclamp ? "ON (no correction > 115 deg, <= 4 rad/s)" : "off"); fflush(stdout); }
        if (s_rotset && rang < 0.52f && vel && vel[0] * vel[0] + vel[2] * vel[2] > 1.0f && set_body_xform(b, want)) { st_rotset++; }   /* v1068S: exact rotation, host avel feed-forward only */
        else if (s_rclamp && rang > 2.0f) { st_rclamp++; }   /* v1064K1: axis is noise near 180 deg -> host avel only, the snap fixes it */
        else {
            float rc[3], rl; for (int k = 0; k < 3; k++) rc[k] = rv[k] * s_kr;
            rl = len3(rc); if (s_rclamp && rl > 4.0f) for (int k = 0; k < 3; k++) rc[k] *= 4.0f / rl;
            for (int k = 0; k < 3; k++) ang[k] += rc[k];
        }
        if (s_drag >= 3) {   /* v1066C3 yaw drag (opt-in, GF_COOP_VEH_DRAG=3): yaw rate we set last frame vs the body's now (tyres / brakes resisting the turn) */
            float bw[3];
            if (v->aset_ok && body_vec(b, 0x190u, bw)) { const float l = v->aset_y - bw[1];
                if (fabsf(l) < 3.0f && isfinite(l)) v->dyaw = v->dyaw * 0.8f + l * 0.2f;
                if (v->dyaw > 1.5f) v->dyaw = 1.5f; if (v->dyaw < -1.5f) v->dyaw = -1.5f;
                st_dyaw_sum += fabsf(v->dyaw); }
            if (fabsf(ang[1]) > 0.05f || fabsf(v->dyaw) > 0.05f) ang[1] += v->dyaw; else v->dyaw *= 0.9f;
            v->aset_y = ang[1]; v->aset_ok = 1;
        }
        if (!set_body_vel(b, lin, ang)) { st_fail++; return -1; }
    }
    (void)dt;
    return 1;
}
/* v1063H: per-frame drive info for cars (avel from WSN2) -- optional; H calls it if wired */
void gf_proxy_drive_info_car(uint16_t netid, const gf_world_drive_info* di)
{
    if (!di) return;
    for (int i = 0; i < VS_MAX; i++) if (s_v[i].ent && s_v[i].netid == netid) {
        memcpy(s_v[i].avel_in, di->avel, sizeof s_v[i].avel_in);
        if (di->avel[0] != 0.0f || di->avel[1] != 0.0f || di->avel[2] != 0.0f) s_v[i].avel_ms = GetTickCount64();
        if (di->ext_b && di->ext_len >= 8 && di->ext_b[0] == 1) {   /* car ext v1: seat occupants (newest sample) */
            for (int k = 0; k < 3; k++) { s_v[i].occ_want[k] = (uint16_t)((di->ext_b[2 + 2 * k] << 8) | di->ext_b[3 + 2 * k]); if (s_v[i].occ_want[k]) st_ext_rx_occ++; }
            st_ext_rx++;
            s_v[i].occ_ms = GetTickCount64();
        }
        if (di->ext_b && di->ext_len >= 16 && di->ext_b[0] == 1 && (di->ext_b[1] & 2u)) {   /* v1070E ext v2 */
            const uint8_t* x = di->ext_b;
            s_v[i].in_x = (int8_t)x[8]; s_v[i].in_y = (int8_t)x[9]; s_v[i].in_fl = x[10]; s_v[i].hp8 = x[11];
            s_v[i].zones = ((uint32_t)x[12] << 24) | ((uint32_t)x[13] << 16) | ((uint32_t)x[14] << 8) | x[15];
            s_v[i].ext2 = 1; s_v[i].ext2_ms = GetTickCount64(); st_in_rx++;
        }
        return;
    }
}
/* host: snapshot ext for a car (H calls it per snapshot, <= 16 B). v1: [0]=1 [1]=flags (bit0 body simulated)
 * [2..7] = net ids (big endian) of the occupants of seats 0..2 (0 = empty / not synced). */
static uint32_t seat_occupant(uint32_t veh, int i);
static int seat_count(uint32_t veh);

/* v1066C (agent C) host driver detection. v1063V read ONLY the seat array (veh+0x10C -> 8 B per seat); the 15:4x coop-local host
 * log had "occupied seats 0" for every traffic car. Now three sources, each counted:
 *   (a) seat array (as before), (b) veh+0x104 = driver handle (char+0x38, v1061B RE), (c) character side: a SYNCED ped whose
 *   +0x1718 (vehicle handle) == car+0x38 and +0x4C == +0x1718 (the player's in-car link, func_00233CCC) -- built once per host
 *   frame from gf_coop_world_host_peds. GF_COOP_VEH_OCCSRC=0 = seat array only (old). GF_COOP_VEH_OCCPROBE=1 (default) logs the
 *   seat words / traffic fields of up to 6 MOVING host cars + the nearest enumerated ped (RE evidence for the next round). */
extern int gf_coop_world_host_peds(uint32_t* ents, uint16_t* ids, float (*pos)[3], int max);
static int seat_of(uint32_t veh, uint32_t ch);
static int s_occsrc = -1, s_occprobe = -1;
static unsigned st_occ_a, st_occ_b, st_occ_c, st_occ_cand;
enum { OCC_LINKS = 96 };
static uint32_t s_lk_car[OCC_LINKS], s_lk_ped[OCC_LINKS]; static uint16_t s_lk_id[OCC_LINKS]; static int s_lk_n; static uint32_t s_lk_frame_ms;
static void occ_links_build(void)
{
    extern uint32_t gf_coop_pose_now_ms(void); const uint32_t qn = gf_coop_pose_now_ms();
    uint32_t ents[OCC_LINKS]; uint16_t ids[OCC_LINKS]; static float pos[OCC_LINKS][3]; int n = 0;
    if (s_lk_frame_ms && qn - s_lk_frame_ms < 5) return;   /* once per host snapshot tick (all cars of one snapshot within ~1 ms) */
    s_lk_frame_ms = qn; s_lk_n = 0;
    __try { n = gf_coop_world_host_peds(ents, ids, pos, OCC_LINKS); } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
    for (int i = 0; i < n && s_lk_n < OCC_LINKS; i++) {
        uint32_t h = 0, own = 0;
        if (!read_u32(ents[i] + 0x1718u, &h) || h < 0x38u || !ea_range(h, 0x200u) || !read_u32(ents[i] + 0x4Cu, &own) || own != h) continue;
        s_lk_car[s_lk_n] = h - 0x38u; s_lk_ped[s_lk_n] = ents[i]; s_lk_id[s_lk_n] = ids[i]; s_lk_n++;
    }
}
static int occ_links_to(uint32_t veh) { int n = 0; for (int i = 0; i < s_lk_n; i++) if (s_lk_car[i] == veh) n++; return n; }
static void occ_probe(uint32_t veh)
{
    static ULONGLONG win; static int nwin; static uint32_t seen[32]; static int nseen;
    const ULONGLONG now = GetTickCount64(); float m[12]; uint32_t w104 = 0, arr = 0, cnt = 0, sw[4] = {0, 0, 0, 0}, f9b0 = 0, f7bc = 0, f970 = 0, g79c = 0, g7ac = 0, g9cc = 0;
    if (now - win >= 60000) { win = now; nwin = 0; nseen = 0; }
    if (nwin >= 6) return;
    for (int i = 0; i < nseen; i++) if (seen[i] == veh) return;
    {   /* moving cars only (traffic): body linear velocity */
        const uint32_t b = car_body(veh); float lv[3] = {0, 0, 0};
        if (!b || !body_vec(b, 0x180u, lv) || lv[0] * lv[0] + lv[2] * lv[2] < 2.0f * 2.0f) return;
    }
    if (nseen < 32) seen[nseen++] = veh; nwin++;
    read_xf12(veh, m); read_u32(veh + 0x104u, &w104); read_u32(veh + 0x10Cu, &arr); read_u32(veh + 0x110u, &cnt);
    if (ea_range(arr, 32)) for (int k = 0; k < 4; k++) read_u32(arr + (uint32_t)k * 8u, &sw[k]);
    read_u32(veh + 0x9B0u, &f9b0); read_u32(veh + 0x7BCu, &f7bc); read_u32(veh + 0x970u, &f970); read_u32(veh + 0x79Cu, &g79c); read_u32(veh + 0x7ACu, &g7ac); read_u32(veh + 0x9CCu, &g9cc);
    printf("[coop-vehicle] v1066C occprobe car %08X net %d pos %.1f %.1f %.1f | +104 %08X | seats ptr %08X count %u words %08X %08X %08X %08X | +9B0 %08X +7BC %08X +970 %08X | guid +79C %08X +7AC %08X +9CC %08X | links to it %d\n",
           veh, gf_coop_world_netid_of(veh), (double)m[9], (double)m[10], (double)m[11], w104, arr, cnt, sw[0], sw[1], sw[2], sw[3], f9b0, f7bc, f970, g79c, g7ac, g9cc,
           occ_links_to(veh));
    {   /* nearest enumerated ped (any, synced or not) + its vehicle link words */
        static gf_world_ent e[256]; int n = 0, best = -1; float bd = 1e9f;
        __try { n = gf_world_enum(e, 256); } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
        for (int i = 0; i < n; i++) { if (e[i].kind != GF_WK_PED) continue; { float d2 = 0; for (int k = 0; k < 3; k++) { const float d = e[i].m[9 + k] - m[9 + k]; d2 += d * d; } if (d2 < bd) { bd = d2; best = i; } } }
        if (best >= 0) { uint32_t h1718 = 0, h4c = 0, f5c4 = 0; const uint32_t pe = e[best].ent;
            read_u32(pe + 0x1718u, &h1718); read_u32(pe + 0x4Cu, &h4c); read_u32(pe + 0x5C4u, &f5c4);
            printf("[coop-vehicle] v1066C occprobe   nearest ped %08X (net %d, class %08X) %.1f m | +1718 %08X +4C %08X +5C4 %08X | car+0x38 %08X\n",
                   pe, gf_coop_world_netid_of(pe), e[best].class_hash, (double)sqrtf(bd), h1718, h4c, f5c4, veh + 0x38u); }
        else printf("[coop-vehicle] v1066C occprobe   no enumerated ped (%d entities)\n", n);
    }
    fflush(stdout);
}
int gf_world_snap_ext_car(uint32_t ent, uint8_t kind, uint8_t* out, int max)
{
    if (!veh_on() || kind != GF_WK_CAR || !out || max < 8 || !ent) return 0;
    memset(out, 0, 8); out[0] = 1;
    {   /* v1064K diag (host, every 10 s, first 4 cars): the game's own car matrix (row lengths, det, up row) = the source of the guest's target */
        static ULONGLONG win; static int n; const ULONGLONG now = GetTickCount64(); float m[12];
        if (now - win >= 10000) { win = now; n = 0; }
        if (n < 4 && read_xf12(ent, m)) { n++;
            printf("[coop-vehicle] v1064K host src car %08X net %d: |r| %.3f |u| %.3f |a| %.3f det %.3f up %.2f %.2f %.2f at %.2f %.2f %.2f pos %.1f %.1f %.1f\n", ent, gf_coop_world_netid_of(ent),
                   (double)len3(m), (double)len3(m + 3), (double)len3(m + 6), (double)det3(m), (double)m[3], (double)m[4], (double)m[5], (double)m[6], (double)m[7], (double)m[8], (double)m[9], (double)m[10], (double)m[11]);
            fflush(stdout); }
    }
    { const uint32_t b = car_body(ent); if (b && body_live(ent, b)) out[1] |= 1; }
    if (s_occsrc < 0) { const char* e = getenv("GF_COOP_VEH_OCCSRC"); s_occsrc = !(e && *e == '0'); const char* p2 = getenv("GF_COOP_VEH_OCCPROBE"); s_occprobe = !(p2 && *p2 == '0');
        printf("[coop-vehicle] v1066C host driver detection: %s, probe %s (GF_COOP_VEH_OCCSRC / _OCCPROBE)\n", s_occsrc ? "seat array + veh+0x104 + ped vehicle link" : "seat array only (old)", s_occprobe ? "ON" : "off"); fflush(stdout); }
    {
        int ids[3] = {0, 0, 0}; uint32_t occs[3] = {0, 0, 0};
        for (int k = 0; k < 3 && k < seat_count(ent); k++) {   /* (a) seat array */
            const uint32_t occ = seat_occupant(ent, k);
            if (occ) { occs[k] = occ; st_occ_a++; __try { ids[k] = gf_coop_world_netid_of(occ); } __except (EXCEPTION_EXECUTE_HANDLER) { ids[k] = 0; } }
        }
        if (s_occsrc) {
            uint32_t h = 0;
            if (!occs[0] && read_u32(ent + 0x104u, &h) && h >= 0x38u && ea_range(h - 0x38u, 0x2000u)) {   /* (b) driver handle */
                const uint32_t occ = h - 0x38u; int id = 0; __try { id = gf_coop_world_netid_of(occ); } __except (EXCEPTION_EXECUTE_HANDLER) { id = 0; }
                if (id) { occs[0] = occ; ids[0] = id; st_occ_b++; } }
            occ_links_build();
            for (int i = 0; i < s_lk_n; i++) if (s_lk_car[i] == ent) {   /* (c) character-side vehicle link */
                int dup = 0; for (int k = 0; k < 3; k++) if (occs[k] == s_lk_ped[i]) dup = 1;
                if (dup) continue;
                { int k = seat_of(ent, s_lk_ped[i]); if (k < 0 || k > 2 || occs[k]) { k = -1; for (int j = 0; j < 3; j++) if (!occs[j]) { k = j; break; } }
                  if (k >= 0) { occs[k] = s_lk_ped[i]; ids[k] = s_lk_id[i]; st_occ_c++; } }
            }
            if (s_occprobe) { __try { occ_probe(ent); } __except (EXCEPTION_EXECUTE_HANDLER) { } }
        }
        for (int k = 0; k < 3; k++) {
            out[2 + 2 * k] = (uint8_t)(ids[k] >> 8); out[3 + 2 * k] = (uint8_t)ids[k];
            if (occs[k]) st_ext_occ++;
            if (occs[k] && ids[k]) st_ext_occ_id++;
        }
    }
    st_ext_tx++;
    if (max >= 16) { memset(out + 8, 0, 8); __try { e70_snap_ext(ent, out); } __except (EXCEPTION_EXECUTE_HANDLER) { } return 16; }   /* v1070E ext v2 */
    return 8;
}
static uint32_t seat_occupant(uint32_t veh, int i);
static void unseat_char(uint32_t veh, uint32_t ch, const char* why);
static unsigned st_eject;
/* v1063H host side: the guest claimed (drives) this host car -> move the host's real car with the same servo.
 * di = the guest's state ~60 ms behind. Called every frame by gf_coop_world.c while the claim holds. */
static unsigned st_owned;
void gf_world_owned_apply(uint32_t host_ent, uint8_t kind, const gf_world_drive_info* di)
{
    const ULONGLONG now = GetTickCount64(); vstate* v;
    if (kind != GF_WK_CAR || !di || !host_ent || !veh_on()) return;
    v = vs_get(host_ent, 0xFFFEu, now);
    memcpy(v->avel_in, di->avel, sizeof v->avel_in); v->avel_ms = now;
    if (!v->probed) { printf("[coop-vehicle] v1063V host car %08X is now driven by the guest\n", host_ent); fflush(stdout); }
    if (now >= v->eject_at) {   /* an AI driver still sits in the host's car -> out (the guest's puppet takes the seat) */
        extern uint32_t gf_coop_puppet_entity(void);
        const uint32_t occ = seat_occupant(host_ent, 0);
        v->eject_at = now + 1000;
        if (occ && occ != player() && occ != gf_coop_puppet_entity()) { unseat_char(host_ent, occ, "guest drives this car"); st_eject++; }
    }
    st_owned++;
    gf_veh_proxy_drive(0xFFFEu, host_ent, di->m, di->vel, now, 0.016f);
}
/* the proxy released a car (or the pool parked it): forget our state for that body */
static uint32_t s_seat_car, s_seat_pup; static int s_seat_idx = -1;
static uint32_t puppet_car(void) { return s_seat_car; }   /* v1070E: the car the partner's puppet sits in */
void gf_veh_forget(uint32_t ent)
{
    for (int i = 0; i < VS_MAX; i++) if (s_v[i].ent == ent) { input_unredirect(&s_v[i]); memset(&s_v[i], 0, sizeof s_v[i]); }   /* v1070E2 */
    if (ent && ent == s_seat_car) {   /* the car the puppet sits in is being released / parked: puppet out first */
        __try { unseat_char(s_seat_car, s_seat_pup, "car released"); } __except (EXCEPTION_EXECUTE_HANDLER) { }
        s_seat_car = 0; s_seat_pup = 0; s_seat_idx = -1;
    }
}

/* ---------------------------------------------------------------- player cars (VEH_STATE) */
/* H's helpers (gf_coop_world.c); weak defaults until they exist */
int  gf_coop_world_netid_of_v0(uint32_t ent) { (void)ent; return 0; }
void gf_coop_world_claim_v0(uint16_t netid, int on) { (void)netid; (void)on; }
#pragma comment(linker, "/alternatename:gf_coop_world_netid_of=gf_coop_world_netid_of_v0")
#pragma comment(linker, "/alternatename:gf_coop_world_claim=gf_coop_world_claim_v0")
/* v1070B (agent B) REVERSE CAR CLAIM: the HOST player driving a car proxy of a JOINER-owned car (net id >= 0x8000, zone
 * authority) claims it like the joiner claims host cars: the joiner stops sourcing its snapshot and drives its native car
 * from the host's upstream state (gf_coop_world.c host_owned_tick on the joiner), the host's proxy is left to the host's
 * physics. GF_COOP_VEH_REVCLAIM=0 = old (no claim, the joiner keeps driving the car under the host). */
extern int gf_coop_world_guest_auth(void);
static int rev_claim_on(void)
{
    static int s_rc = -1;
    if (s_rc < 0) { const char* e = getenv("GF_COOP_VEH_REVCLAIM"); s_rc = !(e && *e == '0');
        printf("[coop-vehicle] v1070B reverse car claim (host drives a joiner-owned car) %s (GF_COOP_VEH_REVCLAIM)\n", s_rc ? "ON" : "off"); fflush(stdout); }
    return s_rc && gf_coop_world_guest_auth();
}
static int car_proxy_netid(uint32_t ent, ULONGLONG now)   /* live car proxies are exactly the ones gf_veh_proxy_drive saw */
{
    for (int i = 0; i < VS_MAX; i++) if (s_v[i].ent == ent && now - s_v[i].last_ms < 1000) return s_v[i].netid;
    return 0;
}
typedef struct { uint8_t ver, in_car, seat, pad; uint16_t netid, pad2; float m[12]; } veh_state_wire;   /* 56 B */
static volatile LONG s_rx_seq; static veh_state_wire s_rx; static CRITICAL_SECTION s_cs; static int s_cs_ok;
static veh_state_wire s_tx_last; static ULONGLONG s_tx_at; static unsigned st_tx, st_rx;
static uint32_t s_my_car; static int s_my_seat = -1;
static void on_event(uint16_t type, const void* data, int len)   /* co-op thread: copy only */
{
    if (type == EV_CAR_DMG && len >= (int)sizeof(car_dmg_wire)) {   /* v1070E: single producer (co-op thread), main thread consumes */
        if (s_dq_w - s_dq_r < 32) { memcpy(&s_dq[s_dq_w & 31], data, sizeof(car_dmg_wire)); InterlockedIncrement(&s_dq_w); }
        return; }
    if (type != EV_VEH_STATE || len < (int)sizeof(veh_state_wire) || !s_cs_ok) return;
    EnterCriticalSection(&s_cs); memcpy(&s_rx, data, sizeof s_rx); LeaveCriticalSection(&s_cs);
    InterlockedIncrement(&s_rx_seq); st_rx++;
}
/* seats (verified in the lifted code): veh+0x10C = seat array, 8 B per seat, word 0 = occupant handle (char + 0x38);
 * veh+0x110 = seat count (func_004334F4). vt+0x200(veh, i, desc8) fills desc = {i + 6, ...} (func_002CCA60);
 * vt+0x1F8(veh, char, desc) seats char (func_002CC7C4: links the seat, then the character's vehicle component
 * 0x55859EFA vt+0x84 enters) -- the path the game uses for AI occupants (func_002CE458, seat 0 = driver);
 * vt+0x1F8(veh, 0, desc) only clears the seat link. vt+0x1E0(veh, char, desc) = exit (func_002B5648). */
static int seat_count(uint32_t veh) { uint32_t n = 0; return read_u32(veh + 0x110u, &n) && n <= 8 ? (int)n : 0; }
static uint32_t seat_occupant(uint32_t veh, int i)
{
    uint32_t arr = 0, h = 0;
    if (i < 0 || i >= seat_count(veh) || !read_u32(veh + 0x10Cu, &arr) || !ea_range(arr, 64) || !read_u32(arr + (uint32_t)i * 8u, &h) || h < 0x38u) return 0;
    return h - 0x38u;
}
static int seat_of(uint32_t veh, uint32_t ch)
{
    const int n = seat_count(veh);
    for (int i = 0; i < n; i++) if (seat_occupant(veh, i) == ch) return i;
    return -1;
}
/* which car does the local player sit in, and in which seat (0 driver, 1.. passenger, -1 none) */
static uint32_t local_car(int* seat)
{
    const uint32_t pl = player(); uint32_t hv = 0, veh, own = 0, fl = 0, h = 0;
    *seat = -1;
    /* player+0x1718 = the car's entity HANDLE (car + 0x38, func_00233CCC), == player+0x4C while really seated */
    if (!pl || !read_u32(pl + 0x1718u, &hv) || hv < 0x38u || !read_u32(pl + 0x4Cu, &own) || own != hv || !read_u32(pl + 0x5C4u, &fl) || !(fl & 8u)) return 0;
    veh = hv - 0x38u;
    if (!ea_range(veh, 0x200u)) return 0;
    { const int i = seat_of(veh, pl); *seat = i >= 0 ? i : 0; (void)h; }
    return veh;
}
static void player_car_tick(ULONGLONG now)
{
    int seat; const uint32_t car = local_car(&seat); veh_state_wire w; int netid = 0;
    const int mode = gf_coop_mode();
    if (!mode || !gf_coop_connected()) return;
    memset(&w, 0, sizeof w); w.ver = 1;
    if (car) {
        /* v1068S (R request): the joiner's OWN car (guest-owned native, zone authority) has a world net id on the guest too */
        __try { netid = gf_coop_world_netid_of(car); } __except (EXCEPTION_EXECUTE_HANDLER) { netid = 0; }
        if (!netid && (mode == 2 || mode == 3 || (mode == 1 && rev_claim_on()))) netid = car_proxy_netid(car, now);   /* guest: a proxy car we drive; v1070B host: a joiner-owned car proxy */
        w.in_car = 1; w.seat = (uint8_t)seat; w.netid = (uint16_t)netid;
        read_xf12(car, w.m);
    }
    if (car != s_my_car || seat != s_my_seat) {
        printf("[coop-vehicle] v1063V local player %s car %08X seat %d (net id %d)\n", car ? "IN" : "left", car ? car : s_my_car, seat, netid); fflush(stdout);
        if (s_claimed && (!car || seat != 0 || netid != s_claimed)) {
            printf("[coop-vehicle] v1063V release claim of net %u\n", s_claimed); fflush(stdout);
            __try { gf_coop_world_claim(s_claimed, 0); } __except (EXCEPTION_EXECUTE_HANDLER) { } s_claimed = 0; }
        if (car && seat == 0 && netid && !s_claimed && ((mode == 2 && netid < 0x8000) || (mode == 1 && netid >= 0x8000 && netid < 0xFFF0 && rev_claim_on()))) {   /* v1068S: claims only for HOST-owned proxies; v1070B + host on a JOINER-owned proxy */
            printf("[coop-vehicle] v1063V CLAIM net %u (guest drives the host's car)\n", netid); fflush(stdout);
            __try { gf_coop_world_claim((uint16_t)netid, 1); } __except (EXCEPTION_EXECUTE_HANDLER) { } s_claimed = (uint16_t)netid; }
        s_my_car = car; s_my_seat = seat; s_tx_at = 0;
    }
    if (now >= s_tx_at || w.in_car != s_tx_last.in_car || w.seat != s_tx_last.seat || w.netid != s_tx_last.netid) {
        if (gf_coop_send_event(EV_VEH_STATE, &w, sizeof w)) { s_tx_last = w; s_tx_at = now + 1000; st_tx++; }
    }
}

/* ---------------------------------------------------------------- the remote player's puppet in its car */
extern uint32_t gf_coop_puppet_entity(void);
extern uint32_t gf_proxy_any_ent_of(uint16_t netid);           /* P (gf_coop_proxy.c): live proxy incl. cars */
uint32_t gf_coop_world_ent_of_v0(uint16_t netid) { (void)netid; return 0; }
#pragma comment(linker, "/alternatename:gf_coop_world_ent_of=gf_coop_world_ent_of_v0")
uint32_t gf_coop_world_ent_of(uint16_t netid);                  /* H (host): entity of a synced net id */
static int s_seat_on = -1;
static veh_state_wire s_fake_rx; static ULONGLONG s_fake_until;
static ULONGLONG s_seat_retry, s_seat_at; static uint32_t s_noseat_car; static int s_seat_lost;   /* v1063V3 stick check */
static unsigned st_seat, st_unseat, st_seat_fail;
int gf_veh_puppet_seated(uint32_t pup) { return pup && pup == s_seat_pup && s_seat_car != 0; }   /* gf_coop_puppet.c: skip root drive */
static int seat_desc(uint32_t veh, int idx, uint32_t desc)
{
    __try { wr32(desc, 0); wr32(desc + 4, 0); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return call_virtual(veh, 0x200u, (uint32_t)idx, desc, 0, 0);
}
static int seat_char(uint32_t veh, uint32_t ch, int idx)
{
    const uint32_t buf = scratch(); uint32_t occ;
    if (!buf || idx < 0 || idx >= seat_count(veh)) return 0;
    occ = seat_occupant(veh, idx);
    if (occ && occ != ch) return 0;
    if (occ == ch) return 1;
    if (!seat_desc(veh, idx, buf + 0xE0u)) return 0;
    if (!call_virtual(veh, 0x1F8u, ch, buf + 0xE0u, 0, 0)) return 0;
    return seat_of(veh, ch) == idx;
}
static void unseat_char(uint32_t veh, uint32_t ch, const char* why)
{
    const uint32_t buf = scratch(); const int idx = seat_of(veh, ch);
    if (!buf || idx < 0) return;
    if (seat_desc(veh, idx, buf + 0xE0u)) {
        call_virtual(veh, 0x1E0u, ch, buf + 0xE0u, 0, 0);
        if (seat_of(veh, ch) >= 0 && seat_desc(veh, idx, buf + 0xE0u)) call_virtual(veh, 0x1F8u, 0, buf + 0xE0u, 0, 0);
    }
    printf("[coop-vehicle] v1063V unseat %08X from car %08X seat %d (%s) -> %s\n", ch, veh, idx, why, seat_of(veh, ch) < 0 ? "out" : "STILL SEATED"); fflush(stdout);
}
static void seat_tick(ULONGLONG now)
{
    veh_state_wire rx; uint32_t pup, want = 0; const int mode = gf_coop_mode();
    if (s_seat_on < 0) { const char* e = getenv("GF_COOP_VEH_SEAT"); s_seat_on = !(e && *e == '0');
        printf("[coop-vehicle] v1063V puppet seating %s (GF_COOP_VEH_SEAT)\n", s_seat_on ? "ON" : "off"); fflush(stdout); }
    if (!s_seat_on || !s_cs_ok) return;
    EnterCriticalSection(&s_cs); rx = s_rx; LeaveCriticalSection(&s_cs);
    if (now < s_fake_until) rx = s_fake_rx;   /* GF_COOP_VEH_TEST=2 */
    pup = gf_coop_puppet_entity();
    if (rx.in_car && rx.netid && pup && gf_coop_connected()) {
        /* v1068S (R request): an entity this peer owns, else our proxy of the partner's car (both modes, zone authority) */
        /* echo (mode 3): the world table holds the SOURCE car of the same process -> proxy only, as before */
        __try { want = mode != 3 ? gf_coop_world_ent_of(rx.netid) : 0; if (!want) want = gf_proxy_any_ent_of(rx.netid); } __except (EXCEPTION_EXECUTE_HANDLER) { want = 0; }
    }
    if (s_seat_car && (want != s_seat_car || pup != s_seat_pup || seat_of(s_seat_car, s_seat_pup) < 0)) {
        if (pup == s_seat_pup) { __try { unseat_char(s_seat_car, s_seat_pup, !rx.in_car ? "partner left the car" : "car changed"); } __except (EXCEPTION_EXECUTE_HANDLER) { } }
        if (want == s_seat_car && pup == s_seat_pup && now - s_seat_at < 1500 && ++s_seat_lost >= 3) {   /* the game keeps dropping the seat link */
            s_noseat_car = want;
            printf("[coop-vehicle] v1063V the game drops the puppet's seat in car %08X (3x within 1.5 s) -> puppet follows the partner's pose instead (no seat)\n", want); fflush(stdout);
        }
        s_seat_car = 0; s_seat_pup = 0; s_seat_idx = -1; st_unseat++;
    }
    if (want != s_noseat_car) { if (s_noseat_car && !want) s_noseat_car = 0; }
    if (!want) s_seat_lost = 0;
    if (want && want == s_noseat_car) want = 0;
    if (want && !s_seat_car && now >= s_seat_retry) {
        int idx = rx.seat, ok = 0;
        __try { ok = seat_char(want, pup, idx); if (!ok && idx != 0) ok = seat_char(want, pup, idx = 0); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
        if (ok) { s_seat_car = want; s_seat_pup = pup; s_seat_idx = idx; st_seat++; s_seat_at = now;
            if (st_seat <= 20 || st_seat % 100 == 0) { printf("[coop-vehicle] v1063V puppet %08X SEATED in car %08X (net %u) seat %d\n", pup, want, rx.netid, idx); fflush(stdout); } }
        else { st_seat_fail++; s_seat_retry = now + 2000;
            if (st_seat_fail <= 10 || st_seat_fail % 50 == 0) { printf("[coop-vehicle] v1063V seat puppet %08X in car %08X (net %u) seat %d FAILED (seats %d, occupant %08X)\n",
                pup, want, rx.netid, rx.seat, seat_count(want), seat_occupant(want, rx.seat)); fflush(stdout); } }
    }
}

/* guest: seat the host's car occupants (ped proxies) in our car proxies; seated peds are not driven by gf_coop_proxy.c
 * (gf_veh_ped_seated) -- the car carries them, the pose stream animates them. GF_COOP_VEH_OCC=0 off. */
static int s_occ_on = -1; static unsigned st_occ_seat, st_occ_unseat, st_occ_fail;
int gf_veh_ped_seated(uint32_t ent)
{
    if (!ent || s_occ_on != 1) return 0;
    for (int i = 0; i < VS_MAX; i++) if (s_v[i].ent) for (int k = 0; k < 3; k++) if (s_v[i].occ_ent[k] == ent) return 1;
    return 0;
}
static void occ_tick(ULONGLONG now)
{
    if (s_occ_on < 0) { const char* e = getenv("GF_COOP_VEH_OCC"); s_occ_on = !(e && *e == '0');
        printf("[coop-vehicle] v1063V car occupants (host drivers/passengers seated in car proxies) %s (GF_COOP_VEH_OCC)\n", s_occ_on ? "ON" : "off"); fflush(stdout); }
    if (!s_occ_on) return;
    for (int i = 0; i < VS_MAX; i++) {
        vstate* v = &s_v[i];
        if (!v->ent || v->netid >= 0xFFF0u) continue;
        for (int k = 0; k < 3; k++) {
            const int fresh = now - v->occ_ms < 1500 && now - v->last_ms < 1000;
            uint32_t want = 0;
            if (fresh && v->occ_want[k]) { __try { want = gf_proxy_any_ent_of(v->occ_want[k]); } __except (EXCEPTION_EXECUTE_HANDLER) { want = 0; } }
            if (want && want == gf_coop_puppet_entity()) want = 0;
            if (v->occ_ent[k] && v->occ_ent[k] != want) {
                __try { if (seat_of(v->ent, v->occ_ent[k]) >= 0) unseat_char(v->ent, v->occ_ent[k], "host occupant left"); } __except (EXCEPTION_EXECUTE_HANDLER) { }
                v->occ_ent[k] = 0; st_occ_unseat++;
            }
            if (want && !v->occ_ent[k]) {
                int ok = 0;
                __try { ok = seat_char(v->ent, want, k); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
                if (ok) { v->occ_ent[k] = want; st_occ_seat++;
                    if (st_occ_seat <= 20) { printf("[coop-vehicle] v1063V net %u: ped proxy %08X (net %u) seated in car proxy %08X seat %d\n", v->netid, want, v->occ_want[k], v->ent, k); fflush(stdout); } }
                else { st_occ_fail++; v->occ_want[k] = 0;   /* retry on the next ext sample */
                    if (st_occ_fail <= 10) { printf("[coop-vehicle] v1063V net %u: seating ped proxy %08X in seat %d FAILED (occupant %08X)\n", v->netid, want, k, seat_occupant(v->ent, k)); fflush(stdout); } }
            }
        }
    }
}

/* ---------------------------------------------------------------- echo test (GF_COOP_VEH_TEST=1): the local player gets
 * into the nearest empty native car, it drives a circle for 25 s (body velocity), stops, the player gets out. */
static int s_test = -1;
static void test_tick(ULONGLONG now)
{
    static int phase = 0; static ULONGLONG t0, tph; static uint32_t car;
    const uint32_t pl = player(); const uint32_t buf = scratch();
    if (s_test < 0) { const char* e = getenv("GF_COOP_VEH_TEST"); s_test = e && *e ? atoi(e) : 0; }
    if (s_test <= 0 || !pl || !buf || !gf_coop_connected()) return;
    if (!t0) { t0 = now; printf("[coop-vehicle] v1063V TEST %d armed (in 35 s)\n", s_test); fflush(stdout); }
    if (s_test == 2) {   /* the partner "sits" in the nearest live car proxy for 30 s: puppet seat + unseat path */
        static int ph2 = 0; static ULONGLONG t2;
        if (ph2 == 0 && now - t0 > 35000) {
            float pm[12]; int best = -1; float bd = 1e9f;
            if (!read_xf12(pl, pm)) return;
            for (int i = 0; i < VS_MAX; i++) if (s_v[i].ent && now - s_v[i].last_ms < 500 && s_v[i].netid < 0xFFF0u) {
                float cm[12], d2 = 0; if (!read_xf12(s_v[i].ent, cm)) continue;
                for (int k = 0; k < 3; k++) { const float d = cm[9 + k] - pm[9 + k]; d2 += d * d; }
                if (d2 < bd && !seat_occupant(s_v[i].ent, 0)) { bd = d2; best = i; }
            }
            if (best < 0) return;
            memset(&s_fake_rx, 0, sizeof s_fake_rx); s_fake_rx.ver = 1; s_fake_rx.in_car = 1; s_fake_rx.seat = 0; s_fake_rx.netid = s_v[best].netid;
            s_fake_until = now + 30000; ph2 = 1; t2 = now;
            printf("[coop-vehicle] v1063V TEST2 partner -> car proxy net %u (%08X, %.1f m away) for 30 s\n", s_v[best].netid, s_v[best].ent, (double)sqrtf(bd)); fflush(stdout);
        } else if (ph2 == 1 && now - t2 > 30000) {
            ph2 = 2; printf("[coop-vehicle] v1063V TEST2 partner leaves the car (puppet seated now: %s)\n", s_seat_car ? "yes" : "no"); fflush(stdout);
        }
        return;
    }
    if (phase == 0 && now - t0 > 35000) {
        gf_world_ent e[96]; int n = 0, best = -1; float bd = 45.0f * 45.0f, pm[12];
        if (!read_xf12(pl, pm)) return;
        __try { n = gf_world_enum(e, 96); } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
        for (int i = 0; i < n; i++) {
            float d2 = 0; if (e[i].kind != GF_WK_CAR) continue;
            for (int k = 0; k < 3; k++) { const float d = e[i].m[9 + k] - pm[9 + k]; d2 += d * d; }
            if (d2 < bd && seat_count(e[i].ent) > 0 && !seat_occupant(e[i].ent, 0)) { bd = d2; best = i; }
        }
        if (best < 0) { if ((now - t0) % 5000 < 20) { printf("[coop-vehicle] v1063V TEST: no empty car within 45 m (%d entities)\n", n); fflush(stdout); } return; }
        car = e[best].ent;
        {
            int ok = 0, seat; uint32_t inc;
            if (seat_desc(car, 0, buf + 0xE0u)) ok = call_virtual(car, 0x1DCu, pl, 1, 0, buf + 0xE0u);
            inc = local_car(&seat);
            printf("[coop-vehicle] v1063V TEST enter car %08X (%.1f m) via vt+0x1DC: call %s, local car now %08X seat %d, seat0 occupant %08X\n",
                   car, (double)sqrtf(bd), ok ? "ok" : "FAILED", inc, seat, seat_occupant(car, 0)); fflush(stdout);
            if (!inc) {   /* fallback: the AI-occupant path */
                ok = seat_char(car, pl, 0); inc = local_car(&seat);
                printf("[coop-vehicle] v1063V TEST fallback vt+0x1F8 seat: %s, local car now %08X seat %d\n", ok ? "ok" : "FAILED", inc, seat); fflush(stdout);
            }
        }
        phase = 1; tph = now;
    } else if (phase == 1) {
        const uint32_t b = car_body(car); float m[12];
        if (now - tph > 25000 || !b || !read_xf12(car, m)) { phase = 2; tph = now; return; }
        if (!body_live(car, b)) return;   /* parked car on its static representation: never activate() it */
        {   /* 7 m/s along the car's at-row, turning 0.25 rad/s */
            const float sp = now - tph < 2000 ? 3.5f : 7.0f; float lin[3], ang[3] = {0, 0.25f, 0}, vy = 0;
            body_vec(b, 0x180u, lin); vy = lin[1];
            for (int k = 0; k < 3; k++) lin[k] = m[6 + k] * sp;
            lin[1] = vy;
            set_body_vel(b, lin, ang);
        }
    } else if (phase == 2 && now - tph > 4000) {
        int seat; const uint32_t inc = local_car(&seat);
        if (inc) unseat_char(inc, pl, "TEST exit");
        printf("[coop-vehicle] v1063V TEST exit: local car before %08X, after %08X\n", inc, local_car(&seat)); fflush(stdout);
        phase = 3;
    }
}

/* ---------------------------------------------------------------- tick (main PPU thread, from gf_proxy_tick) */
void gf_veh_tick(void)
{
    static ULONGLONG log_at; static int inited;
    const ULONGLONG now = GetTickCount64();
    if (!veh_on()) return;
    if (!inited) { inited = 1; InitializeCriticalSection(&s_cs); s_cs_ok = 1; gf_coop_set_event_handler(on_event); }
    e70_cfg(); mres_on();   /* v1070E startup lines */
    __try { player_car_tick(now); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    __try { seat_tick(now); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    __try { occ_tick(now); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    __try { test_tick(now); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    __try { e70_rx_tick(); dmg_test_tick(now); } __except (EXCEPTION_EXECUTE_HANDLER) { }   /* v1070E */
    if (now - log_at >= 10000) {
        int live = 0; log_at = now;
        for (int i = 0; i < VS_MAX; i++) if (s_v[i].ent && now - s_v[i].last_ms < 2000) live++;
        if (live || st_drive || st_tx || st_rx) {
            printf("[coop-vehicle] v1063V %d cars driven (%s) | frames %u (kinematic %u), snaps %u, hard sets %u, failures %u, no body %u, probe mismatches %u | track err mean %.2f m max %.2f m, rot %.1f deg | v1064K upside-down frames %u, rot clamps %u, body rot sets %u | VEH_STATE tx %u rx %u, local car %08X seat %d, claim %u, owned applies %u | puppet seated %u, unseated %u, seat failures %u (in car %08X), AI drivers ejected %u | occupants: ext tx %u (occupied seats %u, with net id %u), rx %u (occupants %u), seated %u, unseated %u, failed %u\n",
                   live, s_drive == 2 ? "hard" : s_drive == 1 ? "soft" : "old", st_drive, st_kin, st_snap, st_hard, st_fail, st_nobody, st_probe_bad,
                   st_err_n ? st_err_sum / st_err_n : 0.0, (double)st_err_max, st_err_n ? st_rerr_sum / st_err_n * 57.2958 : 0.0, st_upside, st_rclamp, st_qfix, st_tx, st_rx, s_my_car, s_my_seat, s_claimed, st_owned, st_seat, st_unseat, st_seat_fail, s_seat_car, st_eject, st_ext_tx, st_ext_occ, st_ext_occ_id, st_ext_rx, st_ext_rx_occ, st_occ_seat, st_occ_unseat, st_occ_fail);
            fflush(stdout);
        }
        if (live || st_drive || st_ext_tx) {   /* v1066C */
            printf("[coop-vehicle] v1066C servo: init placements %u, re-snaps (teleports) %u, catch-up frames %u (recovered %u), frames err > 0.5 m %u, > 1 m %u | drag comp mean %.2f m/s, lateral %.2f m/s, yaw %.3f rad/s (impulses %u) | moving (> 3 m/s, %u frames): err mean %.2f max %.2f m, rot %.1f deg, along-track %+.2f m, lateral %.2f m, physics advance ratio %.2f, integral corr %.2f m/s | host occupants by seat array %u, +0x104 %u, ped link %u (links now %d)\n",
                   st_init, st_snap, st_catch, st_catch_ok, st_e05, st_e1, st_drag_n ? st_drag_sum / st_drag_n : 0.0, st_drag_n ? st_dlat_sum / st_drag_n : 0.0, st_err_n ? st_dyaw_sum / st_err_n : 0.0, st_imp,
                   st_along_n, st_along_n ? st_mv_err / st_along_n : 0.0, (double)st_mv_max, st_along_n ? st_mv_rot / st_along_n * 57.2958 : 0.0,
                   st_along_n ? st_along_sum / st_along_n : 0.0, st_along_n ? st_lat_sum / st_along_n : 0.0, st_ratio_n ? st_ratio_sum / st_ratio_n : 0.0, st_err_n ? st_icorr_sum / st_err_n : 0.0, st_occ_a, st_occ_b, st_occ_c, s_lk_n);
            fflush(stdout); }
        if (live || st_drive) { int nfar = 0; for (int i = 0; i < VS_MAX; i++) if (s_v[i].ent && s_v[i].farkin && now - s_v[i].last_ms < 2000) nfar++;
            printf("[coop-vehicle] v1068S polish: catch-up -> re-snap fixes %u, unstick re-snaps %u, rotation sets %u, far keyframed frames %u (cars far now %d) (cumulative)\n", st_catchfix, st_unstick, st_rotset, st_farkin, nfar); fflush(stdout); }
        st_e05 = st_e1 = 0; st_drag_sum = 0; st_drag_n = 0; st_dlat_sum = st_dyaw_sum = 0; st_along_sum = st_lat_sum = st_ratio_sum = st_icorr_sum = st_mv_err = st_mv_rot = 0; st_along_n = st_ratio_n = 0; st_mv_max = 0;
        if (st_cx_tx || st_cx_rx) { printf("[coop-vehicle] v1064K2 car look: model match %u, differs %u, unknown %u | +B8 match %u, differs %u | spawn ext tx %u rx %u\n",
            st_look_match, st_look_diff, st_look_unk, st_b8_match, st_b8_diff, st_cx_tx, st_cx_rx); fflush(stdout); }
        if (st_mfix_try || st_mtest_set) { printf("[coop-vehicle] v1070C paint/model fix: differing proxies swapped %u, now matching %u, host model not resident %u, SetModel failed %u | echo self-test switched %u, restored %u (cumulative)\n",
            st_mfix_try, st_mfix_ok, st_mfix_nores, st_mfix_fail, st_mtest_set, st_mtest_back); fflush(stdout); }
        if (st_dmg_tx || st_in_rx || st_mres_cars) { printf("[coop-vehicle] v1070E car details: input ext tx %u (non-zero %u), rx %u, proxy writes %u (game overwrote %u, shared status %u, no status %u) | damage applied %u (destroyed %u), local undone %u, forwarded %u, partner hits applied %u/%u, owner puppet-undo %u, heals %u, zone copies %u, send failures %u | siren sets %u, police light calls %u | model not resident: cars %u, re-checks %u, resident later %u | v1070E2 input redirects %u (failed %u, undone %u), burning proxy frames %u, no-effect damage pauses %u, explosions followed %u / missed %u (cumulative)\n",
            st_in_tx, st_in_txnz, st_in_rx, st_in_set, st_in_over, st_in_shared, st_in_nostat, st_dmg_apply, st_dmg_kill, st_dmg_undo, st_dmg_fwd, st_dmg_fwd_ok, st_dmg_fwd_rx, st_host_undo, st_dmg_heal, st_dmg_zone, st_dmg_fail, st_sir_set, st_plt_set, st_mres_cars, st_mres_tries, st_mres_late, st_in_redir, st_in_redir_fail, st_in_unredir, st_dmg_burnfr, st_dmg_noeff, st_expl_ok, st_expl_miss); fflush(stdout); }
        st_err_sum = st_rerr_sum = 0; st_err_max = 0; st_err_n = 0; st_upside = 0; st_rclamp = 0;   /* v1064K: per 10 s */
    }
}
