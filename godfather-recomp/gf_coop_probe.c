/* gf_coop_probe.c -- read-only runtime probes for co-op player-state capture.
 *
 * Public entry points are called on the main PPU thread.  Every probe is opt-in,
 * all guest-memory reads are guarded, and the probes never call or write guest code.
 * Log tag: [coop-probe]. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

extern uint8_t* vm_base;

enum { MOVE_WORDS = (0x2400 - 0x1700) / 4, CAM_MAX = 24 };
enum { PROBE_LOG_LIMIT = 380 };

static int s_init, s_xform_on, s_move_on, s_action_on, s_car_on;
static unsigned s_log_count;

static int ok(uint32_t ea) { return ea >= 0x10000u && ea < 0xF0000000u; }
static uint32_t rd32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static float rdf(uint32_t ea)
{
    uint32_t u = rd32(ea); float f; memcpy(&f, &u, sizeof f); return f;
}
static int finitef(float f) { return f == f && f > -10000000.0f && f < 10000000.0f; }
static int env_on(const char* name)
{
    const char* e = getenv(name); return e && atoi(e) == 1;
}
static void probe_log(const char* fmt, ...)
{
    if (s_log_count >= PROBE_LOG_LIMIT) return;
    s_log_count++;
    va_list ap; va_start(ap, fmt);
    printf("[coop-probe] "); vprintf(fmt, ap); printf("\n");
    va_end(ap); fflush(stdout);
}

static uint32_t player_raw(void)
{
    uint32_t a = rd32(0x10181290u); if (!ok(a)) return 0;
    a = rd32(a + 4); if (!ok(a)) return 0;
    a = rd32(a); if (!ok(a) || a < 0x17FCu) return 0;
    a -= 0x17FCu; return ok(a) ? a : 0;
}
static uint32_t player(void)
{
#ifdef _WIN32
    __try { return player_raw(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
#else
    return player_raw();
#endif
}

typedef struct { float r[3], u[3], a[3], p[3]; } matrix_sample;
static int read_matrix(uint32_t ent, matrix_sample* m)
{
#ifdef _WIN32
    __try {
#endif
        const uint32_t xf = rd32(ent + 0x68u); if (!ok(xf)) return 0;
        const uint32_t b = xf + 0x10u;
        for (int i = 0; i < 3; i++) m->r[i] = rdf(b + 4u * (uint32_t)i);
        for (int i = 0; i < 3; i++) m->u[i] = rdf(b + 0x10u + 4u * (uint32_t)i);
        for (int i = 0; i < 3; i++) m->a[i] = rdf(b + 0x20u + 4u * (uint32_t)i);
        for (int i = 0; i < 3; i++) m->p[i] = rdf(b + 0x30u + 4u * (uint32_t)i);
        for (int i = 0; i < 3; i++)
            if (!finitef(m->r[i]) || !finitef(m->u[i]) || !finitef(m->a[i]) || !finitef(m->p[i])) return 0;
        return 1;
#ifdef _WIN32
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
#endif
}
static float vlen(const float v[3]) { return sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]); }
static float vdot(const float a[3], const float b[3]) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }

static void xform_tick(uint32_t pl, ULONGLONG now)
{
    static ULONGLONG last_t; static matrix_sample last; static int have;
    if (last_t && now - last_t < 500) return;
    matrix_sample m; if (!read_matrix(pl, &m)) return;
    float dt = have ? (float)(now - last_t) * 0.001f : 0.0f;
    float vx = 0.0f, vy = 0.0f, vz = 0.0f, speed = 0.0f;
    if (dt > 0.0f) {
        vx = (m.p[0] - last.p[0]) / dt; vy = (m.p[1] - last.p[1]) / dt; vz = (m.p[2] - last.p[2]) / dt;
        speed = sqrtf(vx*vx + vy*vy + vz*vz);
    }
    probe_log("XFORM pl=%08X R=(%.4f %.4f %.4f) U=(%.4f %.4f %.4f) A=(%.4f %.4f %.4f) P=(%.3f %.3f %.3f) len=(%.4f %.4f %.4f) dot=(RU %.4f RA %.4f UA %.4f) yaw=%.2fdeg dPdt=(%.3f %.3f %.3f) speed=%.3fm/s dt=%.3f",
              pl, m.r[0],m.r[1],m.r[2], m.u[0],m.u[1],m.u[2], m.a[0],m.a[1],m.a[2], m.p[0],m.p[1],m.p[2],
              vlen(m.r),vlen(m.u),vlen(m.a), vdot(m.r,m.u),vdot(m.r,m.a),vdot(m.u,m.a),
              atan2f(m.a[0],m.a[2]) * 57.2957795f, vx,vy,vz,speed,dt);
    last = m; last_t = now; have = 1;
}

typedef struct {
    uint32_t raw[10];
    float f1804[5], f1838, f1f4c, f1830, f1834;
    uint32_t flags, w1838, w1830, w1834;
} move_sample;
typedef struct { uint32_t prev; unsigned samples, hit, miss, extra; unsigned char have, prev_small; } move_candidate;
static move_candidate s_mc[MOVE_WORDS];
static int s_move_band = -1;

static int read_move(uint32_t pl, move_sample* s)
{
#ifdef _WIN32
    __try {
#endif
        memset(s, 0, sizeof *s);
        for (int i = 0; i < 5; i++) { s->raw[i] = rd32(pl + 0x1804u + 4u*(uint32_t)i); s->f1804[i] = rdf(pl + 0x1804u + 4u*(uint32_t)i); }
        s->w1838 = s->raw[5] = rd32(pl + 0x1838u); s->f1838 = rdf(pl + 0x1838u);
        s->raw[6] = rd32(pl + 0x1F4Cu); s->f1f4c = rdf(pl + 0x1F4Cu);
        s->flags = s->raw[7] = rd32(pl + 0x5C4u);
        s->w1830 = s->raw[8] = rd32(pl + 0x1830u); s->f1830 = rdf(pl + 0x1830u);
        s->w1834 = s->raw[9] = rd32(pl + 0x1834u); s->f1834 = rdf(pl + 0x1834u);
        return 1;
#ifdef _WIN32
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
#endif
}
static int small_int(uint32_t v) { return v <= 1024u || v >= 0xFFFFFFC0u; }
static int speed_band(float v, int old)
{
    const float h = 0.15f;
    if (old < 0) return v < 0.20f ? 0 : v < 2.00f ? 1 : v < 4.50f ? 2 : 3;
    if (old == 0) return v <= 0.20f + h ? 0 : v < 2.00f ? 1 : v < 4.50f ? 2 : 3;
    if (v < 0.20f - h) return 0;
    if (old == 1) return v > 4.50f + h ? 3 : v > 2.00f + h ? 2 : 1;
    if (old == 2) return v < 2.00f - h ? 1 : v > 4.50f + h ? 3 : 2;
    return v < 2.00f - h ? 1 : v < 4.50f - h ? 2 : 3;
}
static int scan_move_words(uint32_t pl, int crossed)
{
    int changed = 0;
#ifdef _WIN32
    __try {
#endif
        for (int i = 0; i < MOVE_WORDS; i++) {
            move_candidate* c = &s_mc[i]; const uint32_t v = rd32(pl + 0x1700u + 4u*(uint32_t)i);
            const int sm = small_int(v);
            if (c->have && sm && c->prev_small) {
                const int ch = v != c->prev; c->samples++;
                if (crossed) { if (ch) c->hit++; else c->miss++; }
                else if (ch) c->extra++;
                if (ch) changed++;
            }
            c->prev = v; c->prev_small = (unsigned char)sm; c->have = 1;
        }
        return changed;
#ifdef _WIN32
    } __except (EXCEPTION_EXECUTE_HANDLER) { return changed; }
#endif
}
static int candidate_score(const move_candidate* c)
{
    if (!c->hit || c->samples < 20) return -1000000;
    return (int)c->hit * 100 - (int)c->miss * 40 - (int)c->extra * 12;
}
static void move_candidates_report(unsigned crossings)
{
    int top[5] = {-1,-1,-1,-1,-1}, score[5] = {-1000000,-1000000,-1000000,-1000000,-1000000};
    for (int i = 0; i < MOVE_WORDS; i++) {
        const int sc = candidate_score(&s_mc[i]);
        for (int k = 0; k < 5; k++) if (sc > score[k]) {
            for (int j = 4; j > k; j--) { score[j] = score[j-1]; top[j] = top[j-1]; }
            score[k] = sc; top[k] = i; break;
        }
    }
    char out[640]; int n = snprintf(out, sizeof out, "MOVE-CAND 10s crossings=%u thresholds=0.20/2.00/4.50m/s top", crossings);
    for (int k = 0; k < 5 && top[k] >= 0 && n > 0 && n < (int)sizeof out; k++) {
        const int i = top[k]; const move_candidate* c = &s_mc[i];
        n += snprintf(out+n, sizeof out-(size_t)n, " +%04X=0x%08X(score %d hit %u miss %u extra %u)",
                      0x1700 + 4*i, c->prev, score[k], c->hit, c->miss, c->extra);
    }
    if (top[0] < 0) snprintf(out+n, sizeof out-(size_t)n, " none (exercise threshold transitions)");
    probe_log("%s", out);
    for (int i = 0; i < MOVE_WORDS; i++) s_mc[i].samples = s_mc[i].hit = s_mc[i].miss = s_mc[i].extra = 0;
}
static void move_tick(uint32_t pl, ULONGLONG now, float hspeed)
{
    static move_sample prev; static int have; static ULONGLONG last_log, window_t, scan_t; static unsigned crossings;
    move_sample m; if (!read_move(pl, &m)) return;
    const int nb = speed_band(hspeed, s_move_band); const int crossed = s_move_band >= 0 && nb != s_move_band;
    if (crossed) crossings++;
    int scan_changes = 0;
    if (!scan_t || now - scan_t >= 100) { scan_changes = scan_move_words(pl, crossed); scan_t = now; }
    const int fields_changed = !have || memcmp(m.raw, prev.raw, sizeof m.raw) != 0;
    if (!have || crossed || (now - last_log >= 500 && (hspeed >= 0.05f || fields_changed))) {
        probe_log("MOVE pl=%08X band=%d%s hspeed=%.3fm/s +1804..1814=(%.5g %.5g %.5g %.5g %.5g) +1838=%08X/%.5g +1F4C=%.5g +5C4=%08X +1830=%08X/%.5g +1834=%08X/%.5g scanSmallChanges=%d",
                  pl, nb, crossed ? " EDGE" : "", hspeed, m.f1804[0],m.f1804[1],m.f1804[2],m.f1804[3],m.f1804[4],
                  m.w1838,m.f1838,m.f1f4c,m.flags,m.w1830,m.f1830,m.w1834,m.f1834,scan_changes);
        last_log = now;
    }
    if (!window_t) window_t = now;
    if (now - window_t >= 10000) { move_candidates_report(crossings); crossings = 0; window_t = now; }
    prev = m; have = 1; s_move_band = nb;
}

typedef struct {
    uint32_t cam; float pos[3], pivot[3], pitch10c, yaw110;
} cam_sample;
static uint32_t s_cams[CAM_MAX]; static int s_ncam; static uint32_t s_cam_scan = 0x40000000u;

static int read_camera(uint32_t cam, cam_sample* c)
{
#ifdef _WIN32
    __try {
#endif
        if (!ok(cam) || rd32(cam) != 0x100417A8u) return 0;
        c->cam = cam;
        for (int i = 0; i < 3; i++) { c->pos[i] = rdf(cam + 0xE0u + 4u*(uint32_t)i); c->pivot[i] = rdf(cam + 0xECu + 4u*(uint32_t)i); }
        c->pitch10c = rdf(cam + 0x10Cu); c->yaw110 = rdf(cam + 0x110u);
        for (int i = 0; i < 3; i++) if (!finitef(c->pos[i]) || !finitef(c->pivot[i])) return 0;
        return finitef(c->pitch10c) && finitef(c->yaw110);
#ifdef _WIN32
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
#endif
}
static void camera_add(uint32_t ea)
{
    for (int i = 0; i < s_ncam; i++) if (s_cams[i] == ea) return;
    if (s_ncam < CAM_MAX) s_cams[s_ncam++] = ea;
}
static void camera_scan_step(void)
{
#ifdef _WIN32
    uint32_t ea = s_cam_scan; size_t budget = 1u << 20;
    while (ea < 0x50000000u && budget) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(vm_base + ea, &mi, sizeof mi)) { ea += 0x10000u; continue; }
        uintptr_t hb = (uintptr_t)mi.BaseAddress, he = hb + mi.RegionSize, vb = (uintptr_t)vm_base;
        uint64_t ge64 = he > vb ? (uint64_t)(he - vb) : (uint64_t)ea + 0x10000u;
        uint32_t ge = ge64 > 0x50000000ull ? 0x50000000u : (uint32_t)ge64;
        if (ge <= ea) { ea += 0x10000u; continue; }
        if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) { ea = ge; continue; }
        uint32_t end = ge; if ((uint64_t)end - ea > budget) end = ea + (uint32_t)budget;
        __try {
            uint32_t p = (ea + 3u) & ~3u;
            for (; p + 4u <= end; p += 4u) if (rd32(p) == 0x100417A8u) camera_add(p);
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
        budget -= end - ea; ea = end;
    }
    if (ea >= 0x50000000u) ea = 0x40000000u;
    s_cam_scan = ea;
#endif
}
static int best_camera(const matrix_sample* pm, cam_sample* out)
{
    int found = 0; float best = 1e30f;
    for (int i = 0; i < s_ncam; i++) {
        cam_sample c; if (!read_camera(s_cams[i], &c)) continue;
        const float dx=c.pos[0]-pm->p[0], dy=c.pos[1]-pm->p[1], dz=c.pos[2]-pm->p[2], d=dx*dx+dy*dy+dz*dz;
        if (d < best && d < 64.0f) { best = d; *out = c; found = 1; }
    }
    return found;
}
typedef struct { uint32_t flags, held_link, item, target_link, target; int item_id; } action_sample;
static int read_action(uint32_t pl, action_sample* a)
{
#ifdef _WIN32
    __try {
#endif
        memset(a, 0, sizeof *a); a->item_id = -1;
        a->flags = rd32(pl + 0x5C4u); a->held_link = rd32(pl + 0x468u);
        if (a->held_link && ok(a->held_link) && a->held_link >= 0x38u) {
            a->item = a->held_link - 0x38u; a->item_id = vm_base[a->item + 0xD9u];
        }
        a->target_link = rd32(pl + 0x1544u);
        if (a->target_link && ok(a->target_link) && a->target_link >= 0x38u) a->target = a->target_link - 0x38u;
        return 1;
#ifdef _WIN32
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
#endif
}
static void action_tick(uint32_t pl, ULONGLONG now, const matrix_sample* pm)
{
    static action_sample prev; static uint32_t prev_cam; static int have; static ULONGLONG last_periodic, last_scan;
    if (!last_scan || now - last_scan >= 25) { camera_scan_step(); last_scan = now; }
    action_sample a; cam_sample c; if (!read_action(pl, &a)) return;
    const int cam_ok = best_camera(pm, &c); const uint32_t cam = cam_ok ? c.cam : 0;
    const int edge = !have || a.flags != prev.flags || a.item_id != prev.item_id || a.item != prev.item ||
                     a.target_link != prev.target_link || a.target != prev.target || cam != prev_cam;
    if (edge || (cam_ok && now - last_periodic >= 1000)) {
        if (cam_ok) {
            probe_log("ACTION%s pl=%08X drawn=%d flags5C4=%08X heldLink=%08X item=%08X itemId=%d lockLink1544=%08X target=%08X cam=%08X camPos=(%.3f %.3f %.3f) pivot=(%.3f %.3f %.3f) view=(%.3f %.3f %.3f) pitch10C=%.3f yaw110=%.3f candidates=%d",
                      edge ? " EDGE" : "",pl,(a.flags&0x200u)!=0,a.flags,a.held_link,a.item,a.item_id,a.target_link,a.target,c.cam,
                      c.pos[0],c.pos[1],c.pos[2],c.pivot[0],c.pivot[1],c.pivot[2],c.pivot[0]-c.pos[0],c.pivot[1]-c.pos[1],c.pivot[2]-c.pos[2],
                      c.pitch10c,c.yaw110,s_ncam);
        } else {
            probe_log("ACTION%s pl=%08X drawn=%d flags5C4=%08X heldLink=%08X item=%08X itemId=%d lockLink1544=%08X target=%08X cam=none candidates=%d",
                      edge ? " EDGE" : "",pl,(a.flags&0x200u)!=0,a.flags,a.held_link,a.item,a.item_id,a.target_link,a.target,s_ncam);
        }
        last_periodic = now;
    }
    prev = a; prev_cam = cam; have = 1;
}

static int guid_for_resource(uint32_t res, uint8_t out[16])
{
#ifdef _WIN32
    __try {
#endif
        const uint32_t tab = rd32(0x101F3670u); if (!ok(tab) || !ok(res)) return 0;
        const uint32_t buckets = rd32(tab), n = rd32(tab + 4u); if (!ok(buckets) || !n || n > (1u << 20)) return 0;
        long budget = 400000;
        for (uint32_t b = 0; b < n && budget > 0; b++) {
            uint32_t node = rd32(buckets + 4u*b);
            while (node && ok(node) && budget-- > 0) {
                if (rd32(node + 8u) == res) { memcpy(out, vm_base + node + 0x1Cu, 16); return 1; }
                node = rd32(node + 0x2Cu);
            }
        }
        return 0;
#ifdef _WIN32
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
#endif
}
static void guid_text(const uint8_t g[16], char out[33])
{
    static const char h[] = "0123456789ABCDEF";
    for (int i = 0; i < 16; i++) { out[2*i] = h[g[i] >> 4]; out[2*i+1] = h[g[i] & 15]; }
    out[32] = 0;
}
typedef struct { uint32_t veh, owner, flags; } car_state;
static int read_car_state(uint32_t pl, car_state* c)
{
#ifdef _WIN32
    __try { c->veh=rd32(pl+0x1718u); c->owner=rd32(pl+0x4Cu); c->flags=rd32(pl+0x5C4u); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
#else
    c->veh=rd32(pl+0x1718u); c->owner=rd32(pl+0x4Cu); c->flags=rd32(pl+0x5C4u); return 1;
#endif
}
static int read_car_detail(uint32_t veh, uint32_t* vt, uint32_t* res, matrix_sample* m)
{
#ifdef _WIN32
    __try {
#endif
        if (!ok(veh)) return 0; *vt = rd32(veh); *res = rd32(veh + 0xB8u);
        return read_matrix(veh, m);
#ifdef _WIN32
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
#endif
}
static void car_edge_log(const char* edge, uint32_t veh)
{
    uint32_t vt=0,res=0; matrix_sample m; uint8_t guid[16]; char gs[33]="-";
    if (!read_car_detail(veh,&vt,&res,&m)) { probe_log("CAR %s veh=%08X detail=unreadable/stale",edge,veh); return; }
    if (guid_for_resource(res,guid)) guid_text(guid,gs);
    probe_log("CAR %s veh=%08X vt=%08X R=(%.4f %.4f %.4f) U=(%.4f %.4f %.4f) A=(%.4f %.4f %.4f) P=(%.3f %.3f %.3f) +B8=%08X modelGUID=%s",
              edge,veh,vt,m.r[0],m.r[1],m.r[2],m.u[0],m.u[1],m.u[2],m.a[0],m.a[1],m.a[2],m.p[0],m.p[1],m.p[2],res,gs);
}
static void car_tick(uint32_t pl, ULONGLONG now)
{
    static uint32_t last_veh; static int was_in; static ULONGLONG last_t; static matrix_sample last_m; static int have_m;
    car_state c; if (!read_car_state(pl,&c)) return;
    const int in = ok(c.veh) && c.veh == c.owner && (c.flags & 8u);
    if (in && (!was_in || c.veh != last_veh)) { car_edge_log("ENTER",c.veh); last_t=0; have_m=0; }
    if (!in && was_in) car_edge_log("EXIT",last_veh);
    if (in && (!last_t || now-last_t >= 1000)) {
        matrix_sample m; uint32_t vt,res;
        if (read_car_detail(c.veh,&vt,&res,&m)) {
            float dt=have_m?(float)(now-last_t)*0.001f:0.0f, vx=0,vy=0,vz=0;
            if (dt>0) { vx=(m.p[0]-last_m.p[0])/dt; vy=(m.p[1]-last_m.p[1])/dt; vz=(m.p[2]-last_m.p[2])/dt; }
            probe_log("CAR TICK veh=%08X P=(%.3f %.3f %.3f) dPdt=(%.3f %.3f %.3f) hspeed=%.3fm/s speed3=%.3fm/s",
                      c.veh,m.p[0],m.p[1],m.p[2],vx,vy,vz,sqrtf(vx*vx+vz*vz),sqrtf(vx*vx+vy*vy+vz*vz));
            last_m=m; last_t=now; have_m=1;
        }
    }
    if (in) last_veh=c.veh; was_in=in;
}

void gf_coop_probe_init(void)
{
    if (s_init) return; s_init = 1;
    s_xform_on=env_on("GF_COOP_XFORM_PROBE"); s_move_on=env_on("GF_COOP_MOVE_PROBE");
    s_action_on=env_on("GF_COOP_ACTION_PROBE"); s_car_on=env_on("GF_COOP_CAR_PROBE");
    if (s_xform_on) { printf("[coop-probe] GF_COOP_XFORM_PROBE ON\n"); s_log_count++; }
    if (s_move_on) { printf("[coop-probe] GF_COOP_MOVE_PROBE ON\n"); s_log_count++; }
    if (s_action_on) { printf("[coop-probe] GF_COOP_ACTION_PROBE ON\n"); s_log_count++; }
    if (s_car_on) { printf("[coop-probe] GF_COOP_CAR_PROBE ON\n"); s_log_count++; }
    fflush(stdout);
}

void gf_coop_probe_tick(void)
{
    if (!s_init) gf_coop_probe_init();
    if (!(s_xform_on || s_move_on || s_action_on || s_car_on)) return;
    const uint32_t pl=player(); if (!pl) return;
    const ULONGLONG now=GetTickCount64(); matrix_sample pm; const int have_pm=read_matrix(pl,&pm);
    static ULONGLONG speed_t; static float speed_p[3], speed_hs; static int speed_have; float hs=speed_hs;
    if (have_pm) {
        if (!speed_have) { memcpy(speed_p,pm.p,sizeof speed_p); speed_t=now; speed_have=1; }
        else if (now-speed_t >= 100) {
            const float dt=(float)(now-speed_t)*0.001f, dx=pm.p[0]-speed_p[0], dz=pm.p[2]-speed_p[2];
            speed_hs=sqrtf(dx*dx+dz*dz)/dt; hs=speed_hs;
            memcpy(speed_p,pm.p,sizeof speed_p); speed_t=now;
        }
    }
    if (s_xform_on) xform_tick(pl,now);
    if (s_move_on && have_pm) move_tick(pl,now,hs);
    if (s_action_on && have_pm) action_tick(pl,now,&pm);
    if (s_car_on) car_tick(pl,now);
}
