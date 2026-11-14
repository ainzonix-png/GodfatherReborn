/* gf_coop_mission.c -- v1072M (agent M, 2026-11-06): co-op missions.
 *
 * PHASE 1 = RE tracer (read only, GF_COOP_MISSION_TRACE, default 1 = bounded log). What is VERIFIED (static RE, see
 * NOTES_mission_RE.md):
 *  - Named events: func_00455324(handle, name) registers; name -> func_003FE410 = sdbm hash with A-Z folded to a-z;
 *    func_00455074(handle, hash) finds / creates the event object (func_004A6410 alloc): +0x00 subscriber list head
 *    (node +0 next, +8 subscriber object), +0x10 hash, +0x14/+0x18 list links, +0x1C refcount / flags. *(handle) = event.
 *  - Dispatch func_00455878(msg, r4): event = *(msg+0); each subscriber's vtable +8 (OPD) is called with (sub, msg).
 *    So every message -- iMsg* types AND script / mission events (finish_igc_* ...) -- passes here: hash = *(*(msg)+0x10).
 *  - Entity classes: func_00453E98(hash, name, factory OPD, size). Mission: hash E65D5AAC, size 0x100, factory
 *    func_00022198 -> ctor func_003587F8, vtable 0x100509A0 (listener sub-object at +0x30, vtable 0x10050A14).
 *    Objective: hash ? (TOC -0x776C name), factory func_0001F9C0 -> ctor func_000D41F4, vtable 0x10044880.
 *  - HUD objective manager *(0x101EC598) (natives GetCurrentMissionName func_00387B18, IsObjectiveComplete func_00387B94,
 *    GetNumObjectives func_00387BEC, GetObjective func_00387C68 all read it): bank select +0xE34 (0 = bank A at +0x40,
 *    count u16 +0xE20; else bank B at +0x740, count u16 +0xE22), entries 0x48 bytes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include "recompiled/ppu_recomp.h"
#include "gf_memq.h"
#include "gf_coop_mission.h"
static void hm_tick(void); static void hm_on_dispatch(uint32_t h); static void igc_test_tick(void); static void igc_joiner_tick(ULONGLONG t, int a, ULONGLONG* warp_at_p);   /* v1073H/S (end of file) */
int gf_stream_toc_find(const char* sub, char out[][128], int max, int log); uint32_t gf_stream_load(const char* name);   /* v1073S */
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
extern int gf_coop_send_event(uint16_t type, const void* data, int len);
typedef void (*gf_coop_event_fn)(uint16_t type, const void* data, int len);
extern void gf_coop_set_event_handler(gf_coop_event_fn fn);

extern uint8_t* vm_base;
extern int gf_coop_mode(void);        /* 0 off, 1 host, 2 join, 3 echo */
extern int gf_coop_connected(void);

#define MV_MISSION_LISTENER_VT 0x10050A14u
#define MV_MISSION_VT          0x100509A0u
/* v1119M: CheckpointMission (class 44D54DA4, factory func_00022204 -> ctor func_00070B7C over the Mission base func_00358608) has its
 * OWN vtable 0x10041AA0 (listener +0x30 = vt+0x74 = 0x10041B14). The act-1 tutorial (t1_load_mission subs 10041B14, l30) and most
 * story missions are CheckpointMissions -> v1073H never saw them ("mission objects found 0"). Accept both classes. */
#define MV_CPMISSION_VT          0x10041AA0u
#define MV_CPMISSION_LISTENER_VT 0x10041B14u
static int is_mission_vt(uint32_t vt) { return vt == MV_MISSION_VT || vt == MV_CPMISSION_VT; }
static int is_mission_listener_vt(uint32_t vt) { return vt == MV_MISSION_LISTENER_VT || vt == MV_CPMISSION_LISTENER_VT; }
#define MV_OBJECTIVE_VT        0x10044880u
#define MV_OBJMGR_PTR          0x101EC598u
#define MV_OBJMGR_SIZE         0xE40u
#define MV_RODATA_LO           0x10000000u
#define MV_RODATA_HI           0x1003F6E0u

static int ea_ok(uint32_t ea, uint32_t n) { return vm_base && ea >= 0x10000u && ea < 0xF0000000u && n <= 0xF0000000u - ea; }
static uint32_t rd32(uint32_t ea) { const uint8_t* p = vm_base + ea; return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static int safe32(uint32_t ea, uint32_t* out)
{
    if (!ea_ok(ea, 4) || (ea & 3)) return 0;
    __try { *out = rd32(ea); return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int safe_copy(void* dst, uint32_t ea, uint32_t n)
{
    if (!ea_ok(ea, n)) return 0;
    __try { memcpy(dst, vm_base + ea, n); return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static ULONGLONG now_ms(void) { return GetTickCount64(); }

/* ------------------------------------------------------------------ the game's name hash */
uint32_t gf_mission_hash(const char* s)
{
    uint32_t h = 0;
    if (!s) return 0;
    for (; *s; s++) { uint32_t c = (uint8_t)*s; if (c - 65u <= 0x19u) c += 0x20u; h = (h << 6) + (h << 16) - h + c; }
    return h;
}

/* ------------------------------------------------------------------ name dictionary: hash -> name */
#define DICT_N 65536
static uint32_t s_dk[DICT_N]; static const char* s_dv[DICT_N]; static volatile LONG s_dict_n, s_dict_ready;
static char s_arena[262144]; static size_t s_arena_used;
static SRWLOCK s_dlock = SRWLOCK_INIT;
static void dict_put_locked(uint32_t h, const char* name, int copy)
{
    uint32_t i = (h * 2654435761u) & (DICT_N - 1);
    if (!h || !name) return;
    for (int k = 0; k < 64; k++, i = (i + 1) & (DICT_N - 1)) {
        if (s_dk[i] == h) return;   /* first name wins (collisions are rare) */
        if (!s_dk[i]) {
            if (copy) { const size_t l = strlen(name) + 1; if (l > 160 || s_arena_used + l > sizeof s_arena) return;
                memcpy(s_arena + s_arena_used, name, l); name = s_arena + s_arena_used; s_arena_used += l; }
            s_dk[i] = h; s_dv[i] = name; s_dict_n++; return; }
    }
}
const char* gf_mission_name_of(uint32_t h)
{
    uint32_t i = (h * 2654435761u) & (DICT_N - 1); const char* r = NULL;
    if (!h) return NULL;
    AcquireSRWLockShared(&s_dlock);
    for (int k = 0; k < 64; k++, i = (i + 1) & (DICT_N - 1)) { if (s_dk[i] == h) { r = s_dv[i]; break; } if (!s_dk[i]) break; }
    ReleaseSRWLockShared(&s_dlock);
    return r;
}
static void dict_seed(void)   /* every C string of the EBOOT's rodata: iMsg* names, class names, event names in code */
{
    static char buf[MV_RODATA_HI - MV_RODATA_LO + 1];
    uint32_t n = MV_RODATA_HI - MV_RODATA_LO, i = 0, added = 0;
    if (InterlockedCompareExchange(&s_dict_ready, 1, 0) != 0) return;
    if (!safe_copy(buf, MV_RODATA_LO, n)) { printf("[mission] v1072M dictionary: rodata not readable\n"); fflush(stdout); return; }
    buf[n] = 0;
    AcquireSRWLockExclusive(&s_dlock);
    while (i < n) {
        uint32_t j = i; int ok = 1;
        while (j < n && buf[j]) { const unsigned char c = (unsigned char)buf[j]; if (c < 0x20 || c > 0x7E) { ok = 0; } j++; }
        if (ok && j - i >= 3 && j - i < 120 && isalpha((unsigned char)buf[i])) { dict_put_locked(gf_mission_hash(buf + i), (const char*)(vm_base + MV_RODATA_LO + i), 0); added++; }   /* rodata stays mapped: no copy */
        i = j + 1;
    }
    ReleaseSRWLockExclusive(&s_dlock);
    printf("[mission] v1072M dictionary: %u rodata strings hashed (%ld names, arena %zu B)\n", added, (long)s_dict_n, s_arena_used); fflush(stdout);
}

/* ------------------------------------------------------------------ config */
static int s_trace = -1, s_lines_max = 4000; static volatile LONG s_lines;
static void cfg(void)
{
    const char* e;
    if (s_trace >= 0) return;
    e = getenv("GF_COOP_MISSION_TRACE"); s_trace = e && *e ? atoi(e) : 1;
    if ((e = getenv("GF_COOP_MISSION_TRACE_LINES")) && atoi(e) > 0) s_lines_max = atoi(e);
    printf("[mission] v1072M tracer %s (GF_COOP_MISSION_TRACE=%d: 1 = first sightings + mission-like events, 2 = every event; max %d lines)\n",
           s_trace ? "ON" : "off", s_trace, s_lines_max); fflush(stdout);
}
static int tline(void) { return InterlockedIncrement(&s_lines) <= s_lines_max; }

/* ------------------------------------------------------------------ registration hook */
static volatile LONG st_reg;
void gf_mission_on_register(uint64_t handle, uint64_t name)
{
    char nm[160]; uint32_t ea = (uint32_t)name; int i;
    (void)handle;
    InterlockedIncrement(&st_reg);
    if (!ea_ok(ea, 1)) return;
    __try { for (i = 0; i < 159; i++) { nm[i] = (char)vm_base[ea + (uint32_t)i]; if (!nm[i]) break; } nm[i] = 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!nm[0]) return;
    AcquireSRWLockExclusive(&s_dlock); dict_put_locked(gf_mission_hash(nm), nm, 1); ReleaseSRWLockExclusive(&s_dlock);
}

/* ------------------------------------------------------------------ dispatch stats */
#define EVS_N 8192
typedef struct { uint32_t hash; uint32_t n; ULONGLONG first, last; uint32_t sub_vt[4]; uint8_t interesting, printed; } evstat;
static evstat s_ev[EVS_N]; static SRWLOCK s_elock = SRWLOCK_INIT;
static volatile LONG st_disp, st_disp_bad, st_ev_distinct, st_ev_named, st_ev_full;
static int name_interesting(const char* n)
{
    static const char* k[] = { "mission", "objective", "checkpoint", "igc", "cutscene", "marker", "blip", "stage", "goal", "succe",
                               "fail", "complete", "objectiv", "story", "target", "hud", "popup", "intro", "outro", "spawn", "wave",
                               "timer", "favor", "hit", "job", "reward", "trigger", "cinematic", "sequence", "finish", "play_", "start", "end" };
    char lo[96]; int i;
    if (!n) return 1;   /* unknown names are interesting until we know them */
    for (i = 0; i < 95 && n[i]; i++) lo[i] = (char)tolower((unsigned char)n[i]); lo[i] = 0;
    if (!strncmp(lo, "imsgrunning", 11) || strstr(lo, "tick") || strstr(lo, "update") || strstr(lo, "anim") || strstr(lo, "physics")) return 0;
    for (i = 0; i < (int)(sizeof k / sizeof k[0]); i++) if (strstr(lo, k[i])) return 1;
    return 0;
}
static evstat* ev_get(uint32_t h, int* isnew)
{
    uint32_t i = (h * 2654435761u) & (EVS_N - 1);
    *isnew = 0;
    for (int k = 0; k < 64; k++, i = (i + 1) & (EVS_N - 1)) {
        if (s_ev[i].hash == h) return &s_ev[i];
        if (!s_ev[i].hash) { s_ev[i].hash = h; *isnew = 1; st_ev_distinct++; return &s_ev[i]; }
    }
    st_ev_full++; return NULL;
}

/* ------------------------------------------------------------------ mission objects (found through their listener) */
#define MS_N 16
typedef struct { uint32_t obj; ULONGLONG seen; uint8_t snap[0x100]; int have; } mobj;
static mobj s_ms[MS_N]; static volatile LONG st_ms_new;
static void ms_note(uint32_t obj)
{
    int fr = -1; uint32_t vt;
    if (!safe32(obj, &vt) || !is_mission_vt(vt)) return;
    for (int i = 0; i < MS_N; i++) { if (s_ms[i].obj == obj) { s_ms[i].seen = now_ms(); return; } if (fr < 0 && !s_ms[i].obj) fr = i; }
    if (fr < 0) { ULONGLONG o = ~0ull; for (int i = 0; i < MS_N; i++) if (s_ms[i].seen < o) { o = s_ms[i].seen; fr = i; } }
    s_ms[fr].obj = obj; s_ms[fr].seen = now_ms(); s_ms[fr].have = 0; st_ms_new++;
}

static void hexline(const char* tag, uint32_t base, const uint8_t* b, uint32_t off, uint32_t n)
{
    char line[300]; int p = 0;
    p += snprintf(line + p, sizeof line - (size_t)p, "%s %08X+%03X:", tag, base, off);
    for (uint32_t k = 0; k < n && p < (int)sizeof line - 12; k += 4)
        p += snprintf(line + p, sizeof line - (size_t)p, " %02X%02X%02X%02X", b[off + k], b[off + k + 1], b[off + k + 2], b[off + k + 3]);
    if (tline()) printf("%s\n", line);
}
static void diff_dump(const char* tag, uint32_t base, const uint8_t* old, const uint8_t* cur, uint32_t size, int max_ranges)
{
    int nr = 0; uint32_t i = 0;
    while (i < size && nr < max_ranges) {
        if (old[i] == cur[i]) { i++; continue; }
        { uint32_t a = i & ~31u, b = a + 32; if (b > size) b = size;
          hexline(tag, base, cur, a, b - a); nr++; i = b; }
    }
}

/* ------------------------------------------------------------------ v1119L Monk's shooting lesson (s1) -> classic aiming
 * The lesson waits for the game's own lock-on (s1_TLN_lock_shoot2 after "LOCK-ON: Hold L1"), which the PC free aim (RMB) never
 * gives. While it runs, cellPad.c switches to the classic layout (g_gf_classic_aim). ON: a training dummy made targetable
 * (s1_settargetable_dummy<N>on...) or the lock-on prompt (s1_text_message_lock_msg). OFF: every dummy made untargetable again
 * after one was on, or 15 minutes after the last lesson event (safety). GF_LESSON_CLASSIC=0 = off. Log [lesson]. */
extern volatile int g_gf_classic_aim;
static unsigned s_les_mask; static int s_les_on, s_les_had; static ULONGLONG s_les_t, s_les_off_at;
static void lesson_set(int on, const char* why)
{
    if (on == s_les_on) return;
    s_les_on = on; g_gf_classic_aim = on;
    printf("[lesson] v1119L Monk's lesson %s (%s)\n", on ? "running -> classic aiming" : "over -> PC free aim", why); fflush(stdout);
}
static void lesson_event(const char* nm)
{
    static int en = -1; if (en < 0) { const char* e = getenv("GF_LESSON_CLASSIC"); en = (e && *e == '0') ? 0 : 1; }
    if (!en) return;
    const ULONGLONG t = now_ms();
    if (s_les_on && t - s_les_t > 15ull * 60ull * 1000ull) { s_les_mask = 0; s_les_had = 0; s_les_off_at = 0; lesson_set(0, "15 min without a lesson event"); }
    if (s_les_off_at && t >= s_les_off_at) { s_les_off_at = 0; if (!s_les_mask) { s_les_had = 0; lesson_set(0, "all dummies off for 8 s"); } }
    if (!nm || strncmp(nm, "s1_", 3) != 0) return;
    /* the lesson's free-aim step (s1_funcspawn_freeaim / s1_text_freeaim_msg, "toggle Free-Aim", then shoot the truck): PC free aim
     * again right away -- RMB, as everywhere else (user: "right after that, we have to free aim") */
    if (!strcmp(nm, "s1_funcspawn_freeaim") || !strcmp(nm, "s1_text_freeaim_msg") || !strcmp(nm, "s1_mrt_notfreeaim")) {
        s_les_mask = 0; s_les_had = 0; s_les_off_at = 0; lesson_set(0, nm); return; }
    const char* d = strstr(nm, "settargetable_dummy");
    if (d) {
        d += 19; int n = 0; while (*d >= '0' && *d <= '9') { n = n * 10 + (*d - '0'); d++; }
        if (n >= 0 && n < 32) {
            if (!strncmp(d, "on", 2)) { s_les_mask |= 1u << n; s_les_had = 1; s_les_t = t; s_les_off_at = 0; lesson_set(1, nm); }
            else if (!strncmp(d, "off", 3)) { s_les_mask &= ~(1u << n);
                if (s_les_had && !s_les_mask && s_les_on) s_les_off_at = t + 8000; }   /* the next step may turn another dummy on */
        }
        return;
    }
    if (!strcmp(nm, "s1_text_message_lock_msg")) { s_les_t = t; lesson_set(1, nm); return; }
    if (s_les_on && (strstr(nm, "lock") || strstr(nm, "shoot") || strstr(nm, "dummy") || strstr(nm, "aim") || strstr(nm, "disarm"))) s_les_t = t;
}

/* ------------------------------------------------------------------ the dispatch hook (game threads) */
static void tick_body(void);
static void mi_log(void);   /* phase 2 */
void gf_mission_on_dispatch(uint64_t msg, uint64_t arg)
{
    uint32_t m = (uint32_t)msg, ev = 0, h = 0, node = 0; int isnew = 0, show = 0; evstat* e; const char* nm;
    (void)arg;
    if (s_trace < 0) cfg();
    InterlockedIncrement(&st_disp);
    if (!s_dict_ready) dict_seed();
    if (!safe32(m, &ev) || !ev || !safe32(ev + 0x10, &h) || !h) { InterlockedIncrement(&st_disp_bad); goto tick; }
    nm = gf_mission_name_of(h);
    __try { lesson_event(nm); } __except (EXCEPTION_EXECUTE_HANDLER) { }   /* v1119L */
    AcquireSRWLockExclusive(&s_elock);
    e = ev_get(h, &isnew);
    if (e) { const ULONGLONG t = now_ms(); e->n++; e->last = t;
        if (isnew) { e->first = t; e->interesting = (uint8_t)name_interesting(nm); if (nm) st_ev_named++; }
        show = s_trace >= 2 || isnew || (s_trace == 1 && e->interesting && e->n <= 40);
        if (isnew || e->interesting) {   /* subscriber classes (first 4), mission objects */
            if (safe32(ev, &node)) for (int k = 0; node && k < 48; k++) { uint32_t sub = 0, vt = 0;
                if (!safe32(node + 8, &sub)) break;
                if (sub && safe32(sub, &vt)) {
                    if (is_mission_listener_vt(vt)) ms_note(sub - 0x30u);
                    for (int q = 0; q < 4; q++) { if (e->sub_vt[q] == vt) break; if (!e->sub_vt[q]) { e->sub_vt[q] = vt; break; } } }
                if (!safe32(node, &node)) break; }
        } }
    ReleaseSRWLockExclusive(&s_elock);
    if (show && s_trace && e && tline()) {
        uint8_t pl[32] = {0}; safe_copy(pl, m, 32);
        printf("[mission-trace] %s%s (%08X) #%u subs %08X %08X %08X | msg %08X: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
               isnew ? "NEW " : "", nm ? nm : "?", h, e->n, e->sub_vt[0], e->sub_vt[1], e->sub_vt[2], m,
               pl[4], pl[5], pl[6], pl[7], pl[8], pl[9], pl[10], pl[11], pl[12], pl[13], pl[14], pl[15],
               pl[16], pl[17], pl[18], pl[19], pl[20], pl[21], pl[22], pl[23], pl[24], pl[25], pl[26], pl[27]);
        fflush(stdout); }
tick:
    {   static volatile LONG s_busy; static ULONGLONG s_t;
        const ULONGLONG t = now_ms();
        if (t - s_t >= 250 && InterlockedCompareExchange(&s_busy, 1, 0) == 0) { s_t = t; __try { tick_body(); } __except (EXCEPTION_EXECUTE_HANDLER) { } InterlockedExchange(&s_busy, 0); }
    }
}

/* ------------------------------------------------------------------ periodic: objective manager + mission objects */
static uint8_t s_om_prev[MV_OBJMGR_SIZE], s_om_cur[MV_OBJMGR_SIZE]; static uint32_t s_om_ptr; static int s_om_have;
static volatile LONG st_om_changes, st_ms_changes;
static void tick_body(void)
{
    static ULONGLONG s_log;
    const ULONGLONG t = now_ms(); uint32_t om = 0;
    if (s_trace < 0) cfg();
    if (s_trace && safe32(MV_OBJMGR_PTR, &om) && om && safe_copy(s_om_cur, om, MV_OBJMGR_SIZE)) {
        if (om != s_om_ptr) { s_om_ptr = om; s_om_have = 0; if (tline()) { printf("[mission-om] objective manager at %08X\n", om); fflush(stdout); } }
        if (!s_om_have) { memcpy(s_om_prev, s_om_cur, MV_OBJMGR_SIZE); s_om_have = 1; }
        else if (memcmp(s_om_prev, s_om_cur, MV_OBJMGR_SIZE)) {
            const uint32_t e34 = ((uint32_t)s_om_cur[0xE34] << 24) | ((uint32_t)s_om_cur[0xE35] << 16) | ((uint32_t)s_om_cur[0xE36] << 8) | s_om_cur[0xE37];
            st_om_changes++;
            if (tline()) printf("[mission-om] change #%ld: bank %s, count A %u, count B %u\n", (long)st_om_changes, e34 ? "B" : "A",
                                ((unsigned)s_om_cur[0xE20] << 8) | s_om_cur[0xE21], ((unsigned)s_om_cur[0xE22] << 8) | s_om_cur[0xE23]);
            diff_dump("[mission-om]", om, s_om_prev, s_om_cur, MV_OBJMGR_SIZE, 12);
            fflush(stdout);
            memcpy(s_om_prev, s_om_cur, MV_OBJMGR_SIZE);
        }
    }
    for (int i = 0; s_trace && i < MS_N; i++) {
        uint8_t cur[0x100]; mobj* o = &s_ms[i];
        if (!o->obj) continue;
        if (!safe_copy(cur, o->obj, 0x100) || (((uint32_t)cur[0] << 24) | ((uint32_t)cur[1] << 16) | ((uint32_t)cur[2] << 8) | cur[3]) == 0 || !is_mission_vt(((uint32_t)cur[0] << 24) | ((uint32_t)cur[1] << 16) | ((uint32_t)cur[2] << 8) | cur[3])) {
            if (tline()) { printf("[mission-obj] %08X gone\n", o->obj); fflush(stdout); } o->obj = 0; continue; }
        if (!o->have) { if (tline()) printf("[mission-obj] NEW mission object %08X\n", o->obj);
            for (uint32_t k = 0; k < 0x100; k += 32) hexline("[mission-obj]", o->obj, cur, k, 32);
            fflush(stdout); memcpy(o->snap, cur, 0x100); o->have = 1; continue; }
        if (memcmp(o->snap, cur, 0x100)) { st_ms_changes++; diff_dump("[mission-obj]", o->obj, o->snap, cur, 0x100, 4); fflush(stdout); memcpy(o->snap, cur, 0x100); }
    }
    if (t - s_log >= 10000) { s_log = t;
        printf("[mission] v1072M trace: dispatches %ld (bad %ld), distinct events %ld (named %ld, table full %ld), registered names %ld, dictionary %ld | mission objects found %ld, field changes %ld | objective manager %08X changes %ld | lines %ld/%d\n",
               (long)st_disp, (long)st_disp_bad, (long)st_ev_distinct, (long)st_ev_named, (long)st_ev_full, (long)st_reg, (long)s_dict_n,
               (long)st_ms_new, (long)st_ms_changes, s_om_ptr, (long)st_om_changes, (long)s_lines, s_lines_max); fflush(stdout);
        mi_log(); }
}

/* ================================================================== PHASE 2: script-event LOCKSTEP (GF_COOP_MISSION=1 on BOTH)
 * The story / mission logic is a graph of script entities (CheckpointMission, TestActiveMission, FuncSpawn, LoadStream,
 * VariableSwitch/Watcher/Operator, TriggerBox, MissionTransitionScreen, MissionTimer, HudControl, MinimapMarker, SequenceInstance,
 * WorldGraphManualLoadSet, ExecuteScoreEvent, ... -- m72-2 trace) wired together by NAMED events (t1_load_mission, start_igc_*,
 * t0_succeed ...). With both games at the same story point (same save), the host's sequence of script events fully determines the
 * mission flow. So:
 *  HOST: every dispatched script event (name not "iMsg*", unknown hashes included, nothing faster than 20/s) is sent as its hash
 *        (GF_COOP_EV_MISSION_EVENT batches, reliable, in order) + a 2 s state beat (GF_COOP_EV_MISSION_STATE).
 *  JOINER: while the host's beat is fresh, its OWN script events are dropped at the dispatcher (return before func_00455878 runs;
 *        hook v1072M2) and the host's are re-dispatched in order on the main thread exactly like the game's own minimal senders
 *        (func_000CF078 / func_0019AABC: msg = {event, 0}, r4 = 0). The event object is found by hash in the registry hash table
 *        *(*(TOC+0x64B8)) (+4 n, +8 buckets; node +0 hash, +4 event, +8 next).
 *  SAFETY: a host hash with no local event = "unmatched" (different story point / save). >= 30 % unmatched over >= 20 events in
 *        20 s -> lockstep switches itself OFF on the joiner (its own events flow again) with a WARNING line.
 * GF_COOP_MISSION=0 (default) = tracer only. GF_COOP_MISSION_INJECT_TEST=<name>[:<sec>] = inject one named event (any mode, echo
 * self-test of the injection path). */
#define GAME_TOC 0x00B4BF60u
static int s_mi = -1, s_auto_off = 0; static char s_itest[96]; static int s_itest_s = 60, s_itest_done = 0;
static __declspec(thread) int t_injecting;
static volatile LONG st_tx_ev, st_tx_batches, st_tx_drop, st_rx_ev, st_inj, st_inj_unmatched, st_inj_nosub, st_inj_fault, st_supp, st_rx_state;
static ULONGLONG s_host_beat; static uint32_t s_host_sent; static ULONGLONG s_first_disp;
static void mi_cfg(void)
{
    const char* e;
    if (s_mi >= 0) return;
    e = getenv("GF_COOP_MISSION"); s_mi = e && *e ? atoi(e) : 0;
    if ((e = getenv("GF_COOP_MISSION_INJECT_TEST")) && *e) { const char* c; strncpy(s_itest, e, sizeof s_itest - 1); if ((c = strchr(s_itest, ':'))) { s_itest_s = atoi(c + 1); s_itest[c - s_itest] = 0; } }
    printf("[mission] v1072M lockstep %s (GF_COOP_MISSION=%d; host sends its script events, joiner replays them and drops its own)%s%s\n",
           s_mi == 1 ? "ON" : "off", s_mi, s_itest[0] ? " | INJECT TEST " : "", s_itest); fflush(stdout);
}
/* replicable = script event: not iMsg*, not high-rate. Cached per evstat (rep: 0 unknown, 1 yes, 2 no). */
static int is_imsg(const char* n) { return n && (n[0] == 'i' || n[0] == 'I') && (n[1] == 'M' || n[1] == 'm') && (n[2] == 's' || n[2] == 'S') && (n[3] == 'g' || n[3] == 'G'); }
typedef struct { uint32_t hash; uint8_t rep; uint16_t win_n; ULONGLONG win_t; } repst;
#define REP_N 4096
static repst s_rep[REP_N]; static SRWLOCK s_rlock = SRWLOCK_INIT;
static int replicable(uint32_t h)
{
    uint32_t i = (h * 2654435761u) & (REP_N - 1); int r = 1; const ULONGLONG t = now_ms();
    AcquireSRWLockExclusive(&s_rlock);
    for (int k = 0; k < 64; k++, i = (i + 1) & (REP_N - 1)) {
        repst* q = &s_rep[i];
        if (!q->hash) { q->hash = h; q->rep = is_imsg(gf_mission_name_of(h)) ? 2 : 1; q->win_t = t; }
        if (q->hash != h) continue;
        if (q->rep == 1) { if (t - q->win_t >= 1000) { q->win_t = t; q->win_n = 0; } if (++q->win_n > 20) q->rep = 2; }
        r = q->rep == 1; break;
    }
    ReleaseSRWLockExclusive(&s_rlock);
    return r;
}
/* host send ring (dispatch hook -> main tick) */
#define TXQ 1024
static uint32_t s_txq[TXQ]; static volatile LONG s_txh, s_txt; static CRITICAL_SECTION s_txcs; static volatile LONG s_txcs_ok;
static void cs_init(void) { if (InterlockedCompareExchange(&s_txcs_ok, 1, 0) == 0) InitializeCriticalSection(&s_txcs); else while (s_txcs_ok != 1) Sleep(0); }
/* joiner inject ring (co-op thread -> main tick) */
#define RXQ 2048
static uint32_t s_rxq[RXQ]; static volatile LONG s_rxh, s_rxt;
static int host_live(void) { return s_host_beat && now_ms() - s_host_beat < 5000; }
static int lockstep_joiner(void) { return s_mi == 1 && !s_auto_off && gf_coop_mode() == 2 && gf_coop_connected() && host_live(); }

/* called from the dispatch hook (any game thread). 1 = drop this dispatch. */
static int mi_dispatch(uint32_t h)
{
    if (s_mi < 0) mi_cfg();
    if (s_mi != 1 || t_injecting) return 0;
    {   const int m = gf_coop_mode();
        if (m == 1 && gf_coop_connected()) {
            if (!replicable(h)) return 0;
            cs_init(); EnterCriticalSection(&s_txcs);
            if (((s_txt + 1) & (TXQ - 1)) != s_txh) { s_txq[s_txt] = h; s_txt = (s_txt + 1) & (TXQ - 1); } else st_tx_drop++;
            LeaveCriticalSection(&s_txcs);
            return 0; }
        if (m == 2 && lockstep_joiner() && replicable(h)) { InterlockedIncrement(&st_supp); return 1; }
    }
    return 0;
}
static void on_event(uint16_t type, const void* data, int len)   /* co-op thread */
{
    const uint8_t* b = (const uint8_t*)data;
    if (type == GF_COOP_EV_MISSION_STATE && len >= 8 && b[0] == 1) { memcpy(&s_host_sent, b + 4, 4); s_host_beat = now_ms(); st_rx_state++; return; }
    if (type == GF_COOP_EV_MISSION_EVENT && len >= 4 && b[0] == 1) {
        const int n = b[1];
        if (len < 4 + 4 * n || gf_coop_mode() != 2) return;
        s_host_beat = now_ms();
        for (int i = 0; i < n; i++) { uint32_t h; memcpy(&h, b + 4 + 4 * i, 4);
            if (((s_rxt + 1) & (RXQ - 1)) != s_rxh) { s_rxq[s_rxt] = h; s_rxt = (s_rxt + 1) & (RXQ - 1); } st_rx_ev++; }
    }
}
/* guest call (main PPU thread only; same scheme as gf_coop_combat.c) */
typedef void (*gfn)(ppu_context*);
static gfn lookup_fn(uint32_t addr)
{
    static uint32_t ca[8]; static gfn cf[8]; static int cn;
    for (int i = 0; i < cn; i++) if (ca[i] == addr) return cf[i];
    { gfn f = 0; for (uint64_t i = 0; i < function_table_count; i++) if ((uint32_t)function_table[i].addr == addr) { f = function_table[i].func; break; }
      if (cn < 8) { ca[cn] = addr; cf[cn] = f; cn++; } return f; }
}
static int gcall2(uint32_t addr, uint64_t a0, uint64_t a1)
{
    ppu_context* cur = g_active_ctx; gfn f; static ppu_context c; static int busy;
    if (!cur || busy || !(f = lookup_fn(addr))) return 0;
    __try {
        memcpy(&c, cur, sizeof c);
        c.gpr[1] = ((uint32_t)cur->gpr[1] - 0x1000u) & ~0xFull; c.gpr[2] = GAME_TOC; c.gpr[3] = a0; c.gpr[4] = a1; c.lr = 0;
        busy = 1; g_active_ctx = &c;
        f(&c);
        while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&c); }
        g_active_ctx = cur; busy = 0; return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_active_ctx = cur; g_trampoline_fn = 0; busy = 0; InterlockedIncrement(&st_inj_fault); return 0; }
}
static uint32_t event_of(uint32_t h)   /* registry hash table lookup (func_00455074 walk), 0 = no such event here */
{
    uint32_t pv, tbl, n, arr, node;
    if (!safe32(GAME_TOC + 0x64B8u, &pv) || !safe32(pv, &tbl) || !safe32(tbl + 4, &n) || !n || !safe32(tbl + 8, &arr)) return 0;
    if (!safe32(arr + 4u * (h % n), &node)) return 0;
    for (int k = 0; node && k < 256; k++) { uint32_t nh, ev; if (!safe32(node, &nh)) return 0; if (nh == h) return safe32(node + 4, &ev) ? ev : 0; if (!safe32(node + 8, &node)) return 0; }
    return 0;
}
static int inject(uint32_t h)   /* 1 dispatched, 0 unmatched / no subscribers, -1 fault */
{
    uint32_t ev = event_of(h), subs = 0, msg;
    if (!ev) { st_inj_unmatched++; return 0; }
    if (!safe32(ev, &subs) || !subs) { st_inj_nosub++; return 0; }   /* the game skips events nobody listens to too */
    if (!g_active_ctx) return -1;
    msg = (((uint32_t)g_active_ctx->gpr[1] - 0x800u) & ~0xFu);   /* scratch above our call frames (callee frames start 0x1000 below sp) */
    if (!ea_ok(msg, 16)) return -1;
    __try { const uint8_t m8[8] = { (uint8_t)(ev >> 24), (uint8_t)(ev >> 16), (uint8_t)(ev >> 8), (uint8_t)ev, 0, 0, 0, 0 }; memset(vm_base + msg, 0, 48); memcpy(vm_base + msg, m8, 8); }   /* v1073H: zero the payload */
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    t_injecting = 1;
    { const int ok = gcall2(0x00455878u, msg, 0); t_injecting = 0; if (!ok) return -1; }
    st_inj++; return 1;
}
void gf_coop_mission_tick(void)   /* main PPU thread (gf_coop_glue tick, v1072S) */
{
    static int s_reg; static ULONGLONG s_beat, s_t0, s_win_t; static long s_win_inj, s_win_un;
    const ULONGLONG t = now_ms();
    if (s_mi < 0) mi_cfg();
    if (!s_t0) { s_t0 = t; printf("[mission] v1072M main tick live (ctx %s, %llu ms after the first event dispatch)\n", g_active_ctx ? "ok" : "NONE",
                                  (unsigned long long)(s_first_disp ? t - s_first_disp : 0)); fflush(stdout); }
    if (!s_reg && s_mi) { gf_coop_set_event_handler(on_event); s_reg = 1; }
    if (s_itest[0] && !s_itest_done && s_first_disp && t - s_first_disp >= (ULONGLONG)s_itest_s * 1000u) {
        const uint32_t h = gf_mission_hash(s_itest); const uint32_t ev = event_of(h); int r;
        s_itest_done = 1; r = inject(h);
        printf("[mission] v1072M INJECT TEST %s (%08X): event %08X -> %s (dispatched %ld, unmatched %ld, no subscribers %ld, faults %ld)\n", s_itest, h, ev,
               r > 0 ? "DISPATCHED" : r < 0 ? "FAILED" : "not dispatched", (long)st_inj, (long)st_inj_unmatched, (long)st_inj_nosub, (long)st_inj_fault); fflush(stdout); }
    { __try { hm_tick(); } __except (EXCEPTION_EXECUTE_HANDLER) { } }   /* v1073H host-run missions */
    { __try { igc_test_tick(); } __except (EXCEPTION_EXECUTE_HANDLER) { } }   /* v1073S */
    if (s_mi != 1) return;
    {   const int m = gf_coop_mode(), conn = gf_coop_connected();
        if (m == 1 && conn) {   /* host: flush the ring + beat */
            uint8_t pkt[4 + 4 * 200]; int n = 0;
            cs_init(); EnterCriticalSection(&s_txcs);
            while (s_txh != s_txt && n < 200) { memcpy(pkt + 4 + 4 * n, &s_txq[s_txh], 4); s_txh = (s_txh + 1) & (TXQ - 1); n++; }
            LeaveCriticalSection(&s_txcs);
            if (n) { pkt[0] = 1; pkt[1] = (uint8_t)n; pkt[2] = pkt[3] = 0;
                if (gf_coop_send_event(GF_COOP_EV_MISSION_EVENT, pkt, 4 + 4 * n)) { st_tx_ev += n; st_tx_batches++; } else st_tx_drop += n; }
            if (t - s_beat >= 2000) { uint8_t st[8] = { 1, 1, 0, 0 }; const uint32_t sent = (uint32_t)st_tx_ev; memcpy(st + 4, &sent, 4); s_beat = t; gf_coop_send_event(GF_COOP_EV_MISSION_STATE, st, 8); }
        }
        if (m == 2 && conn && !s_auto_off) {   /* joiner: replay in order */
            int k = 0;
            while (s_rxh != s_rxt && k < 64) { const uint32_t h = s_rxq[s_rxh]; int r; s_rxh = (s_rxh + 1) & (RXQ - 1); k++;
                r = inject(h); if (r > 0) s_win_inj++; else if (!event_of(h)) s_win_un++; }
            if (t - s_win_t >= 20000) {
                if (s_win_inj + s_win_un >= 20 && s_win_un * 10 >= (s_win_inj + s_win_un) * 3) {
                    s_auto_off = 1;
                    printf("[mission] v1072M WARNING: %ld of %ld host script events have no event here -> the two games are not at the same story point (load the HOST's save on the joiner). Lockstep OFF for this session.\n",
                           s_win_un, s_win_inj + s_win_un); fflush(stdout); }
                s_win_t = t; s_win_inj = s_win_un = 0; }
        }
    }
}
static void mi_log(void)
{
    if (s_mi != 1 && !s_itest[0]) return;
    printf("[mission] v1072M lockstep (%s): host tx events %ld in %ld batches (dropped %ld) | joiner rx %ld, host beat %s (rx %ld, host sent %u), injected %ld (unmatched %ld, no subscribers %ld, faults %ld), own script events dropped %ld%s\n",
           gf_coop_mode() == 1 ? "host" : gf_coop_mode() == 2 ? "joiner" : "echo/off", (long)st_tx_ev, (long)st_tx_batches, (long)st_tx_drop, (long)st_rx_ev,
           host_live() ? "live" : "none", (long)st_rx_state, s_host_sent, (long)st_inj, (long)st_inj_unmatched, (long)st_inj_nosub, (long)st_inj_fault, (long)st_supp,
           s_auto_off ? " | AUTO-OFF (story mismatch)" : ""); fflush(stdout);
}
int gf_mission_on_dispatch2(uint64_t msg, uint64_t arg)   /* v1072M2 hook: 1 = skip this dispatch */
{
    uint32_t ev = 0, h = 0;
    gf_mission_on_dispatch(msg, arg);
    if (s_mi < 0) mi_cfg();
    if (!s_first_disp) s_first_disp = now_ms();
    if (!safe32((uint32_t)msg, &ev) || !ev || !safe32(ev + 0x10, &h) || !h) return 0;
    hm_on_dispatch(h);   /* v1073H */
    if (s_mi != 1) return 0;
    return mi_dispatch(h);
}

/* ================================================================ v1073H (Claude): HOST-RUN MISSIONS (user's choice 19:19)
 * Only the host's game runs a mission. Joiners: see its NPCs/cars (world sync; gf_coop_world.c keeps sharing non-ambient NPCs
 * with far joiners while this is active), fight them (combat forwarding), see a banner with the host's objective count, can warp
 * to the host (F6, joiner window), and get the host's money reward when the mission is passed.
 * Detection (host): live Mission objects (vtable 0x100509A0) found (a) by the tracer's listener scan (ms_note) and (b) by walking
 * the subscriber lists of the Mission message events (handles 0x101EC1C8..0x101EC1E8, see NOTES_mission_RE.md) for the
 * listener vtable 0x10050A14 (obj = sub - 0x30). Result: dispatch of iMsgMissionSucceed / iMsgMissionQuit /
 * iMsgMissionReturnToCheckpoint (hash compare in the dispatch hook). Reward = host money 4 s after "passed" - money at start.
 * Objectives: HUD objective manager *(0x101EC598) bank count (+0xE34 bank, +0xE20/+0xE22 count) -- shown only if sane.
 * Wire: GF_COOP_EV_MISSION_HUD (0x0184), host -> every joiner, 1 Hz + on change. GF_COOP_MISSION_HOST=0 off.
 * UNVERIFIED at runtime (no mission was ever run in co-op): every step logs "[mission-host]" lines for the first test. */
typedef struct { uint8_t ver, active, result, nobj, ndone, pad[3]; int32_t reward; uint32_t seq, obj; } mhud;
static int s_hm = -1; static uint32_t h_succ, h_quit, h_rtc;
static volatile LONG s_ev_succ, s_ev_quit, s_ev_rtc;            /* dispatch hook (any thread) -> tick */
static volatile LONG s_act_now; static volatile uint32_t s_act_obj;   /* host: published for gf_coop_host_mission_active */
enum { EV_IGC = 0x0185 };   /* v1073H2 host cutscene control: {ver 1, kind 1 start / 2 skip / 3 end, pad, pad, hash} */
static volatile LONG s_igc_tx_n, s_igc_tx_done; static volatile uint32_t s_igc_tx_h[16];   /* host: dispatch hook -> tick */
static volatile LONG s_igc_rx_n, s_igc_rx_done; static volatile uint32_t s_igc_rx_h[16]; static volatile uint8_t s_igc_rx_k[16];   /* joiner */
static char s_igc_rx_nm[16][48];   /* v1073H3: cutscene name (start_igc_<name>) from the host */
static volatile LONG s_host_cs, s_mirror_ok;   /* v1119C joiner: host cutscene running / we play it too (HUD) */
static int s_igc_mirror = -1;
static mhud s_rx; static volatile ULONGLONG s_rx_at; static CRITICAL_SECTION s_hcs; static volatile LONG s_hcs_ok;
static volatile LONG s_ban_until_lo; static volatile int32_t s_ban_reward; static volatile LONG s_ban_kind;   /* joiner banner */
static void hm_cfg(void)
{
    const char* e; if (s_hm >= 0) return;
    e = getenv("GF_COOP_MISSION_HOST"); s_hm = !(e && *e == '0');
    h_succ = gf_mission_hash("iMsgMissionSucceed"); h_quit = gf_mission_hash("iMsgMissionQuit"); h_rtc = gf_mission_hash("iMsgMissionReturnToCheckpoint");
    if (InterlockedCompareExchange(&s_hcs_ok, 1, 0) == 0) { InitializeCriticalSection(&s_hcs); InterlockedExchange(&s_hcs_ok, 2); }
    printf("[mission-host] v1073H host-run missions %s (GF_COOP_MISSION_HOST): succeed %08X quit %08X checkpoint %08X\n", s_hm ? "ON" : "off", h_succ, h_quit, h_rtc); fflush(stdout);
}
static void hm_on_dispatch(uint32_t h)   /* any game thread */
{
    if (s_hm < 0) hm_cfg();
    if (!s_hm || !h) return;
    if (h == h_succ) InterlockedIncrement(&s_ev_succ); else if (h == h_quit) InterlockedIncrement(&s_ev_quit); else if (h == h_rtc) InterlockedIncrement(&s_ev_rtc);
    else if (gf_coop_mode() == 1 || gf_coop_mode() == 3) { const char* nm = gf_mission_name_of(h);   /* v1073H2: host cutscene start */
        if (nm && !_strnicmp(nm, "start_igc_", 10)) { const LONG k = InterlockedIncrement(&s_igc_tx_n); s_igc_tx_h[k & 15] = h; } }
}
int gf_coop_host_mission_active(void)   /* host: our mission; joiner: the host's (fresh state) */
{
    if (s_hm < 1) return 0;
    if (gf_coop_mode() == 1 || gf_coop_mode() == 3) return s_act_now != 0;
    if (gf_coop_mode() == 2) { int a = 0; if (s_hcs_ok == 2) { EnterCriticalSection(&s_hcs); a = s_rx.active && GetTickCount64() - s_rx_at < 5000; LeaveCriticalSection(&s_hcs); } return a; }
    return 0;
}
static int mission_scan(uint32_t* first)
{
    static const uint32_t hs[] = { 0x101EC1E8u, 0x101EC1E0u, 0x101EC1D8u, 0x101EC1C8u };
    int n = 0; *first = 0;
    for (int i = 0; i < (int)(sizeof hs / sizeof hs[0]); i++) {
        uint32_t ev = 0, node = 0;
        if (!safe32(hs[i], &ev) || !ev || !safe32(ev, &node)) continue;
        for (int k = 0; node && k < 64; k++) { uint32_t sub = 0, vt = 0, ovt = 0;
            if (!safe32(node + 8, &sub)) break;
            if (sub && safe32(sub, &vt) && is_mission_listener_vt(vt) && safe32(sub - 0x30u, &ovt) && is_mission_vt(ovt)) { ms_note(sub - 0x30u); }
            if (!safe32(node, &node)) break; }
    }
    for (int i = 0; i < MS_N; i++) { uint32_t vt = 0; if (!s_ms[i].obj) continue;
        if (!safe32(s_ms[i].obj, &vt) || !is_mission_vt(vt)) { s_ms[i].obj = 0; continue; }
        if (!*first) *first = s_ms[i].obj; n++; }
    return n;
}
static int objectives(int* nobj, int* ndone)
{
    uint32_t om = 0, bank = 0; *nobj = *ndone = 0;
    if (!safe32(MV_OBJMGR_PTR, &om) || !om || !ea_ok(om, MV_OBJMGR_SIZE)) return 0;
    if (!safe32(om + 0xE34u, &bank)) return 0;
    {   uint8_t c2[2]; const uint32_t cnt_ea = om + (bank ? 0xE22u : 0xE20u), base = om + (bank ? 0x740u : 0x40u);
        int c; if (!safe_copy(c2, cnt_ea, 2)) return 0; c = (c2[0] << 8) | c2[1];
        if (c < 0 || c > 16) return 0;
        for (int i = 0; i < c; i++) { uint8_t f[2]; if (safe_copy(f, base + (uint32_t)i * 0x48u + (bank ? 0xCu : 4u), 2) && (((f[0] << 8) | f[1]) & 8)) (*ndone)++; }
        *nobj = c; }
    return 1;
}
static int32_t money_now(void) { uint32_t mgr = 0, v = 0; if (!safe32(0x101D985Cu, &mgr) || !mgr || !safe32(mgr + 0x24u, &v)) return -1; return (int32_t)v; }
static void hm_send(const mhud* m) { gf_coop_send_event(GF_COOP_EV_MISSION_HUD, m, (int)sizeof *m); }
static void hm_on_event(uint16_t type, const void* data, int len)   /* co-op thread (legacy handler: the host's traffic) */
{
    if (type == EV_IGC && len >= 8 && gf_coop_mode() == 2) { const uint8_t* b = (const uint8_t*)data; uint32_t h; memcpy(&h, b + 4, 4);
        if (b[0] == 1) { const LONG k = s_igc_rx_n + 1; int l = len >= 9 ? b[2] : 0; if (l > 47 || 8 + l > len) l = 0;
            memcpy(s_igc_rx_nm[k & 15], b + 8, (size_t)l); s_igc_rx_nm[k & 15][l] = 0; s_igc_rx_h[k & 15] = h; s_igc_rx_k[k & 15] = b[1];
            InterlockedExchange(&s_igc_rx_n, k); } return; }
    if (type != GF_COOP_EV_MISSION_HUD || len < (int)sizeof(mhud) || gf_coop_mode() != 2 || s_hcs_ok != 2) return;
    { const mhud* m = (const mhud*)data; if (m->ver != 1) return;
      EnterCriticalSection(&s_hcs); s_rx = *m; s_rx_at = GetTickCount64(); LeaveCriticalSection(&s_hcs); }
}
extern int gf_coop_partner_pose(float out_m[16], float* hp);
extern int gf_coop_place_char(uint32_t ent, const float m12[12]);
extern uint32_t gf_coop_player_ent(void);
extern int gf_coop_char_hold(void);
extern int gf_cutscene_active(void);
static int warp_to_host(void)   /* joiner, main PPU thread; v1119F: 1 = done (callers retry otherwise) */
{
    float m[16], hp = 0, m12[12]; const uint32_t pl = gf_coop_player_ent();
    if (!pl || !gf_coop_partner_pose(m, &hp)) { printf("[mission-host] v1073H warp: no host position / no player (retrying)\n"); fflush(stdout); return 0; }
    if (gf_coop_char_hold()) { printf("[mission-host] v1073H warp refused: world still loading (retrying)\n"); fflush(stdout); return 0; }
    for (int r = 0; r < 3; r++) for (int k = 0; k < 3; k++) m12[r * 3 + k] = m[r * 4 + k];
    m12[9] = m[12] + m[0] * 1.5f; m12[10] = m[13] + 0.3f; m12[11] = m[14] + m[2] * 1.5f;   /* 1.5 m to the host's side */
    {   const int ok = gf_coop_place_char(pl, m12);
        printf("[mission-host] v1073H warp to the host at %.1f %.1f %.1f: %s\n", (double)m12[9], (double)m12[10], (double)m12[11], ok ? "done" : "FAILED"); fflush(stdout);
        return ok; }
}
static void igc_joiner_tick(ULONGLONG t, int a, ULONGLONG* warp_at_p)   /* joiner (or echo sim): the host's cutscenes */
{
#define warp_at (*warp_at_p)
            {   /* v1073H3: VERIFIED in echo s73-6: LoadStream("godfather_v4\...\igc_<name>") + start_igc_<name> 2.5 s later plays a story
                 * cutscene on a game that does not run its mission. A SECOND start of the same cutscene hung the game (s73-6, i73-1/2) ->
                 * every cutscene is started at most once per session and never while another one plays. */
                static uint32_t mir_h, done_h[64]; static int n_done, mir_st, mir_on, cs_was; static char mir_nm[48];
                static ULONGLONG mir_t0, mir_tl, skip_at, skip_until;
                extern volatile int g_gf_skip_request, g_gf_skip_block;
                const int cs = gf_cutscene_active();
                if (s_igc_mirror < 0) { const char* e = getenv("GF_COOP_IGC_MIRROR"); s_igc_mirror = !(e && *e == '0');
                    printf("[mission-host] v1073H3 host cutscenes on the joiner %s (GF_COOP_IGC_MIRROR; the host alone can skip)\n", s_igc_mirror ? "ON" : "off"); fflush(stdout); }
                while (s_igc_rx_done != s_igc_rx_n) { const LONG k = ++s_igc_rx_done; const uint32_t h = s_igc_rx_h[k & 15]; const uint8_t kind = s_igc_rx_k[k & 15];
                    InterlockedExchange(&s_host_cs, kind == 1); if (kind != 1) InterlockedExchange(&s_mirror_ok, 0);
                    if (kind == 1) { int seen = 0; for (int q = 0; q < n_done; q++) if (done_h[q] == h) seen = 1;
                        memcpy(mir_nm, s_igc_rx_nm[k & 15], sizeof mir_nm); mir_nm[47] = 0;
                        printf("[mission-host] v1073H3 host cutscene START %s (%08X)%s\n", mir_nm[0] ? mir_nm : "?", h,
                               !s_igc_mirror ? " -- mirror off" : seen ? " -- already played here once (a 2nd start hangs the game): not shown" : !mir_nm[0] ? " -- no name" : ""); fflush(stdout);
                        if (s_igc_mirror && !seen && mir_nm[0]) { mir_h = h; mir_t0 = t; mir_st = 1; mir_on = 0; } }
                    else { if (mir_st && mir_st < 3) { printf("[mission-host] v1073H3 host cutscene over before ours started -> not starting it\n"); fflush(stdout); mir_st = 0; mir_h = 0; }
                        skip_at = t + (kind == 2 ? 0 : 4000); skip_until = skip_at + 10000; warp_at = t + 2000;
                        printf("[mission-host] v1073H3 host cutscene %s -> %s\n", kind == 2 ? "SKIPPED" : "ENDED", kind == 2 ? "skipping ours" : "ours ends within 4 s"); fflush(stdout); } }
                if (mir_st == 1) {   /* load the cutscene's stream */
                    if (t - mir_t0 > 15000) { printf("[mission-host] v1073H3 gave up on the host's cutscene (busy for 15 s)\n"); fflush(stdout); mir_st = 0; }
                    else if (!cs && !gf_coop_char_hold()) {
                        static char m[8][128]; char want[64]; int n, best = -1; snprintf(want, sizeof want, "igc_%s", mir_nm);
                        n = gf_stream_toc_find(want, m, 8, 0);
                        for (int q = 0; q < n && q < 8; q++) { const char* b = strrchr(m[q], '\\'); b = b ? b + 1 : m[q]; if (!_stricmp(b, want)) { best = q; break; } }
                        if (best < 0) { printf("[mission-host] v1073H3 no stream for %s here (%d partial matches) -> cannot show it\n", want, n); fflush(stdout); mir_st = 0; }
                        else { const uint32_t hd = gf_stream_load(m[best]); mir_tl = t; mir_st = 2;
                            printf("[mission-host] v1073H3 loading the host's cutscene: \"%s\" -> handle %08X\n", m[best], hd); fflush(stdout); } } }
                if (mir_st == 2 && t - mir_tl >= 2500) {   /* start it, once */
                    if (gf_coop_char_hold()) { if (t - mir_tl > 12000)   /* NOT cs: loading the stream alone already reads as a cutscene (s73-8) */ { printf("[mission-host] v1073H3 gave up starting the host's cutscene (we stayed busy)\n"); fflush(stdout); mir_st = 0; } }
                    else { const int r = inject(mir_h); if (n_done < 64) done_h[n_done++] = mir_h; mir_st = 3; mir_tl = t;
                        printf("[mission-host] v1073H3 starting the host's cutscene here: %s\n", r > 0 ? "start event dispatched" : r < 0 ? "FAULT" : "not dispatched"); fflush(stdout); if (r <= 0) mir_st = 0; } }
                if (mir_st == 3) { if (cs) { mir_on = 1; mir_st = 0; InterlockedExchange(&s_mirror_ok, 1); printf("[mission-host] v1073H3 the host's cutscene is playing here\n"); fflush(stdout); }
                    else if (t - mir_tl > 6000) { printf("[mission-host] v1073H3 the host's cutscene did not start here within 6 s\n"); fflush(stdout); mir_st = 0; } }
                if (skip_until && t >= skip_at && t < skip_until && cs) g_gf_skip_request = 1;
                if (skip_until && (t >= skip_until || (!cs && cs_was && t >= skip_at))) skip_until = 0;
                g_gf_skip_block = (cs && (mir_on || a)) ? 1 : 0;   /* the host controls cutscenes while it runs a mission / we mirror one */
                if (!cs && cs_was) { mir_on = 0; InterlockedExchange(&s_mirror_ok, 0); }
                { static ULONGLONG hcs_since; if (!s_host_cs) hcs_since = 0; else if (!hcs_since) hcs_since = t; else if (t - hcs_since > 300000) InterlockedExchange(&s_host_cs, 0); }
                cs_was = cs; }
#undef warp_at
}
static void hm_tick(void)   /* main PPU thread, from gf_coop_mission_tick */
{
    static int s_reg, s_act, s_nobj = -1, s_ndone = -1; static uint32_t s_obj, s_seq; static int32_t s_money0; static ULONGLONG s_beat, s_pass_at, s_since;
    static LONG s_succ0, s_quit0, s_rtc0; static uint32_t s_last_seq_applied; static int s_key;
    const int mode = gf_coop_mode(), conn = gf_coop_connected(); const ULONGLONG t = now_ms();
    if (s_hm < 0) hm_cfg();
    if (!s_hm) return;
    if (!s_reg) { gf_coop_set_event_handler(hm_on_event); s_reg = 1; }
    if (mode == 3) {   /* v1073H3 echo sim of the joiner half: GF_COOP_IGC_SIMJOIN=<igc name without igc_>:<seconds> */
        static int sj = -1; static char sn[48]; static int ss; static ULONGLONG sw;
        if (sj < 0) { const char* e = getenv("GF_COOP_IGC_SIMJOIN"); sj = 0; if (e && *e) { const char* c = strchr(e, ':'); size_t l = c ? (size_t)(c - e) : strlen(e); if (l > 47) l = 47; memcpy(sn, e, l); sn[l] = 0; ss = c ? atoi(c + 1) : 70; sj = 1;
            printf("[mission-host] v1073H3 echo SIM joiner: fake host START %s at %d s, END 12 s later\n", sn, ss); fflush(stdout); } }
        if (sj == 1 && s_first_disp && t - s_first_disp >= (ULONGLONG)ss * 1000u) { char ev[64]; const LONG k = s_igc_rx_n + 1; snprintf(ev, sizeof ev, "start_igc_%s", sn);
            strncpy(s_igc_rx_nm[k & 15], sn, 47); s_igc_rx_h[k & 15] = gf_mission_hash(ev); s_igc_rx_k[k & 15] = 1; InterlockedExchange(&s_igc_rx_n, k); sj = 2; }
        if (sj == 2 && t - s_first_disp >= (ULONGLONG)(ss + 12) * 1000u) { const LONG k = s_igc_rx_n + 1; s_igc_rx_nm[k & 15][0] = 0; s_igc_rx_h[k & 15] = 0; s_igc_rx_k[k & 15] = 3; InterlockedExchange(&s_igc_rx_n, k); sj = 3; }
        if (sj > 0) { static ULONGLONG wa; igc_joiner_tick(t, sj >= 2, &wa); (void)sw; }   /* a = 'host mission' only after the fake start */
    }
    if (mode == 1 || mode == 3) {   /* echo (3) runs the host half for testing */
        uint32_t first = 0; const int n = mission_scan(&first); int nobj = 0, ndone = 0; mhud m; int changed = 0, result = 0;
        objectives(&nobj, &ndone);
        if (n && !s_act) { s_act = 1; s_obj = first; s_money0 = money_now(); s_since = t; s_pass_at = 0; changed = 1; s_succ0 = s_ev_succ; s_quit0 = s_ev_quit; s_rtc0 = s_ev_rtc;
            printf("[mission-host] v1073H MISSION STARTED (host): object %08X, %d live, objectives %d/%d, money %d\n", first, n, ndone, nobj, s_money0); fflush(stdout); }
        if (s_act && s_ev_succ != s_succ0 && !s_pass_at) { s_pass_at = t; s_succ0 = s_ev_succ; printf("[mission-host] v1073H iMsgMissionSucceed seen -> reward in 4 s\n"); fflush(stdout); }
        if (s_act && s_ev_quit != s_quit0) { s_quit0 = s_ev_quit; result = 2; printf("[mission-host] v1073H iMsgMissionQuit seen\n"); fflush(stdout); }
        if (s_act && s_ev_rtc != s_rtc0) { s_rtc0 = s_ev_rtc; printf("[mission-host] v1073H iMsgMissionReturnToCheckpoint seen (failed / retry)\n"); fflush(stdout); }
        if (s_pass_at && t - s_pass_at >= 4000) result = 1;
        if (s_act && (result || (!n && t - s_since > 3000))) {
            const int32_t now = money_now(); int32_t rw = (result == 1 && now >= 0 && s_money0 >= 0) ? now - s_money0 : 0; if (rw < 0) rw = 0;
            memset(&m, 0, sizeof m); m.ver = 1; m.active = 0; m.result = (uint8_t)(result ? result : (s_pass_at ? 1 : 3)); m.reward = rw; m.seq = ++s_seq; m.obj = s_obj;
            if (conn) hm_send(&m);
            printf("[mission-host] v1073H MISSION ENDED (host): %s, reward $%d (money %d -> %d), %llu s, mission objects now %d\n",
                   m.result == 1 ? "PASSED" : m.result == 2 ? "quit" : "ended (failed / unloaded)", rw, s_money0, now, (unsigned long long)((t - s_since) / 1000), n); fflush(stdout);
            s_act = 0; s_pass_at = 0; s_beat = t; InterlockedExchange(&s_act_now, 0); return; }
        InterlockedExchange(&s_act_now, s_act); s_act_obj = s_obj;
        if (nobj != s_nobj || ndone != s_ndone) { if (s_act) printf("[mission-host] v1073H objectives %d/%d\n", ndone, nobj), fflush(stdout); s_nobj = nobj; s_ndone = ndone; changed = 1; }
        {   /* v1073H2: our cutscenes -> joiners (start from the dispatch hook; skip = the hold-to-skip flag; end = tracking falls) */
            extern volatile int g_gf_ffwd; static int cs_was, ff_was; static uint32_t cur_h; const int cs = gf_cutscene_active();
            uint8_t pk[56] = { 1, 0, 0, 0 };
            while (s_igc_tx_done != s_igc_tx_n) { const LONG k = ++s_igc_tx_done; cur_h = s_igc_tx_h[k & 15]; pk[1] = 1; memcpy(pk + 4, &cur_h, 4);
                { const char* nm = gf_mission_name_of(cur_h); size_t l = 0; if (nm && !_strnicmp(nm, "start_igc_", 10)) { nm += 10; l = strlen(nm); if (l > 47) l = 47; memcpy(pk + 8, nm, l); } pk[2] = (uint8_t)l;
                  if (conn) gf_coop_send_event(EV_IGC, pk, (int)(8 + l)); }
                printf("[mission-host] v1073H2 our cutscene START %s (%08X) -> joiners\n", gf_mission_name_of(cur_h) ? gf_mission_name_of(cur_h) : "?", cur_h); fflush(stdout); }
            if (cs && g_gf_ffwd && !ff_was) { pk[1] = 2; memcpy(pk + 4, &cur_h, 4); if (conn) gf_coop_send_event(EV_IGC, pk, 8); printf("[mission-host] v1073H2 we SKIPPED the cutscene -> joiners skip too\n"); fflush(stdout); }
            if (!cs && cs_was && !ff_was) { pk[1] = 3; memcpy(pk + 4, &cur_h, 4); if (conn) gf_coop_send_event(EV_IGC, pk, 8); printf("[mission-host] v1073H2 our cutscene ENDED -> joiners\n"); fflush(stdout); }
            ff_was = cs && g_gf_ffwd; cs_was = cs; }
        if (conn && (changed || t - s_beat >= 1000)) { memset(&m, 0, sizeof m); m.ver = 1; m.active = (uint8_t)s_act; m.nobj = (uint8_t)nobj; m.ndone = (uint8_t)ndone; m.seq = s_seq; m.obj = s_obj; hm_send(&m); s_beat = t; }
        return;
    }
    if (mode == 2) {
        mhud m; ULONGLONG at; if (s_hcs_ok != 2) return;
        EnterCriticalSection(&s_hcs); m = s_rx; at = s_rx_at; LeaveCriticalSection(&s_hcs);
        if (at && m.seq && m.seq != s_last_seq_applied && !m.active) {   /* a mission just ended on the host */
            s_last_seq_applied = m.seq;
            if (m.result == 1 && m.reward > 0) { uint32_t mgr = 0; const int32_t cur = money_now();
                if (cur >= 0 && safe32(0x101D985Cu, &mgr) && mgr) { long long v = (long long)cur + m.reward; if (v > 99999995) v = 99999995; gcall2(0x002EB6F8u, mgr, (uint64_t)(int64_t)(int32_t)v); }
                printf("[mission-host] v1073H host PASSED a mission -> our reward +$%d (money %d -> %d)\n", m.reward, cur, money_now()); fflush(stdout); }
            else { printf("[mission-host] v1073H host mission ended (%s)\n", m.result == 2 ? "quit" : m.result == 1 ? "passed, no reward" : "failed / unloaded"); fflush(stdout); }
            InterlockedExchange(&s_ban_kind, m.result); s_ban_reward = m.reward; InterlockedExchange(&s_ban_until_lo, (LONG)(uint32_t)(t + 8000)); }
        {   /* v1073H2 (user 19:32): no F6 -- joiners are moved to the host when its mission starts and after each host cutscene */
            static int was; static ULONGLONG warp_at; const int a = m.active && t - at < 5000;
            if (a != was) { was = a; printf("[mission-host] v1073H host mission %s\n", a ? "STARTED -> moving us to the host" : "not active"); fflush(stdout); if (a) warp_at = t; }   /* v1119C: at once (user 19:37) */
            {   /* v1119F (user 20:16 log: one "no host position" and the joiner was never moved): retry every 1 s for up to 20 s */
                static ULONGLONG warp_since;
                if (warp_at && !warp_since) warp_since = t;
                if (warp_at && t >= warp_at && conn) {
                    if (!gf_coop_char_hold() && !gf_cutscene_active() && warp_to_host()) { warp_at = 0; warp_since = 0; }
                    else if (t - warp_since > 20000) { printf("[mission-host] v1119F gave up moving to the host after 20 s\n"); fflush(stdout); warp_at = 0; warp_since = 0; }
                    else warp_at = t + 1000; }
                if (!warp_at) warp_since = 0; }
            (void)s_key;
            /* cutscenes: the host's start / skip / end */
            igc_joiner_tick(t, a, &warp_at);   /* v1073H3 */
        }
    }
}
int gf_coop_mission_hud(char* out, int cap)   /* RENDER thread: short banner for the co-op HUD line (overlay cap 128 total) */
{
    if (s_hm < 1 || !out || cap < 8 || s_hcs_ok != 2) return 0;
    {   const ULONGLONG t = GetTickCount64(); const uint32_t until = (uint32_t)s_ban_until_lo;
        if (until && (int32_t)(until - (uint32_t)t) > 0) {
            const LONG k = s_ban_kind;
            if (k == 1) snprintf(out, (size_t)cap, "MISSION PASSED +$%d", (int)s_ban_reward); else snprintf(out, (size_t)cap, "MISSION %s", k == 2 ? "QUIT" : "FAILED");
            return 1; }
        if (gf_coop_mode() == 1 || gf_coop_mode() == 3) { if (!s_act_now) return 0; snprintf(out, (size_t)cap, "MISSION (you host)"); return 1; }
        if (gf_coop_mode() == 2 && s_host_cs && !s_mirror_ok) { snprintf(out, (size_t)cap, "HOST CUTSCENE - wait for the host"); return 1; }   /* v1119C */
        {   mhud m; ULONGLONG at; EnterCriticalSection(&s_hcs); m = s_rx; at = s_rx_at; LeaveCriticalSection(&s_hcs);
            if (!m.active || t - at >= 5000) return 0;
            if (m.nobj) snprintf(out, (size_t)cap, "HOST MISSION %d/%d", m.ndone, m.nobj); else snprintf(out, (size_t)cap, "HOST MISSION");
            return 1; } }
}


/* ================================================================ v1073S (Claude): LoadStream log + joiner-side IGC load test
 * Hook at the top of func_004B9D80 (stream manager load, ppu_recomp_002.cpp v1073S): logs the stream names the game loads
 * ("[stream] load ..."), bounded. GF_COOP_IGC_TEST=<igc name>:<seconds> (echo / solo test): at <seconds> after the first event
 * dispatch, list the stream-TOC entries containing <igc name>, LoadStream the best match (as the trainer's v1045B model load),
 * then inject start_igc_<igc name> 3 s and 10 s later -> does the IGC play on a game that does not run its mission? */
static volatile LONG s_sn_lines;
void gf_stream_note(uint64_t mgr, uint64_t name, uint64_t lr)
{
    char nm[128]; int i = 0; const uint32_t ea = (uint32_t)name;
    if (s_sn_lines > 600 || !ea_ok(ea, 1)) return;
    __try { for (; i < 127; i++) { const char c = (char)vm_base[ea + i]; if (!c) break; if (c < 32 || c > 126) { i = 0; break; } nm[i] = c; } } __except (EXCEPTION_EXECUTE_HANDLER) { i = 0; }
    nm[i] = 0; if (!i) return;
    InterlockedIncrement(&s_sn_lines);
    printf("[stream] v1073S load \"%s\" (mgr %08X, lr %08X, t %llu ms)\n", nm, (uint32_t)mgr, (uint32_t)lr, (unsigned long long)now_ms()); fflush(stdout);
}
static uint64_t gcallf6(uint32_t addr, uint64_t a0, uint64_t a1, double f1)
{
    ppu_context* cur = g_active_ctx; gfn f; static ppu_context c;
    if (!cur || !(f = lookup_fn(addr))) return 0;
    __try {
        memcpy(&c, cur, sizeof c);
        c.gpr[1] = ((uint32_t)cur->gpr[1] - 0x1000u) & ~0xFull; c.gpr[2] = GAME_TOC; c.gpr[3] = a0; c.gpr[4] = a1; c.gpr[5] = c.gpr[6] = c.gpr[7] = c.gpr[8] = 0; c.fpr[1] = f1; c.lr = 0;
        g_active_ctx = &c; f(&c);
        while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&c); }
        g_active_ctx = cur; return c.gpr[3];
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_active_ctx = cur; g_trampoline_fn = 0; printf("[stream] v1073S guest call %08X faulted\n", addr); fflush(stdout); return 0; }
}
static int gstr2(uint32_t ea, char* out, int n) { int i = 0; if (!ea_ok(ea, 1)) { out[0] = 0; return 0; } __try { for (; i < n - 1; i++) { const char c = (char)vm_base[ea + i]; if (!c) break; if (c < 32 || c > 126) { out[0] = 0; return 0; } out[i] = c; } } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return 0; } out[i] = 0; return i; }
int gf_stream_toc_find(const char* sub, char out[][128], int max, int log)   /* entries containing sub (case-insensitive) */
{
    uint32_t mgr = 0, nsets = 0; int n = 0; char lsub[64]; int j;
    for (j = 0; sub[j] && j < 63; j++) lsub[j] = (char)tolower((unsigned char)sub[j]); lsub[j] = 0;
    if (!safe32(0x10180F58u, &mgr) || !mgr || !safe32(mgr + 0x2264u + 0x80u, &nsets) || !nsets || nsets > 32) return 0;
    for (uint32_t si = 0; si < nsets; si++) { uint32_t set = 0, pfx = 0; uint8_t c2[2]; char prefix[96];
        if (!safe32(mgr + 0x2264u + 4 * si, &set) || !set || !safe32(set + 8, &pfx) || !safe_copy(c2, set + 0x10, 2)) continue;
        gstr2(pfx, prefix, sizeof prefix);
        { const unsigned cnt = (unsigned)((c2[0] << 8) | c2[1]);
          for (unsigned k = 0; k < cnt && k < 30000; k++) { uint32_t np = 0; char nm[128], low[128];
            if (!safe32(set + 0x1C + 0x14 * k + 4, &np) || !gstr2(np, nm, sizeof nm)) continue;
            for (j = 0; ; j++) { low[j] = (char)tolower((unsigned char)nm[j]); if (!nm[j]) break; }
            if (!strstr(low, lsub)) continue;
            if (log) { printf("[stream] v1073S TOC match \"%s\" (set \"%s\")\n", nm, prefix); fflush(stdout); }
            if (n < max) { snprintf(out[n], 128, "%s\\%s", prefix, nm); } n++; } } }   /* full name = <set prefix>\<entry> (trainer v1045B) */
    return n;
}
uint32_t gf_stream_load(const char* name)   /* main PPU thread: LoadStream (func_004B9D80(mgr, name, 0,0,0,0, f1 = 32768)) */
{
    uint32_t mgr = 0, buf; size_t l = strlen(name);
    if (!g_active_ctx || l > 120 || !safe32(0x10180F58u, &mgr) || !mgr) return 0;
    buf = (((uint32_t)g_active_ctx->gpr[1] - 0x900u) & ~0xFu);
    if (!ea_ok(buf, 128)) return 0;
    __try { memcpy(vm_base + buf, name, l + 1); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return (uint32_t)gcallf6(0x004B9D80u, mgr, buf, 32768.0);
}
static void igc_test_tick(void)   /* main PPU thread */
{
    static int st = -1; static char nm[64]; static int sec; static ULONGLONG t_load;
    if (st < 0) { const char* e = getenv("GF_COOP_IGC_TEST"); st = 0; if (e && *e) { const char* c = strchr(e, ':'); size_t l = c ? (size_t)(c - e) : strlen(e); if (l > 63) l = 63; memcpy(nm, e, l); nm[l] = 0; sec = c ? atoi(c + 1) : 60; st = 1;
        printf("[stream] v1073S IGC test armed: %s at %d s\n", nm, sec); fflush(stdout); } }
    if (st <= 0 || !s_first_disp) return;
    {   const ULONGLONG t = now_ms();
        if (st == 1 && t - s_first_disp >= (ULONGLONG)sec * 1000u) {
            static char m[16][128]; int n = gf_stream_toc_find(nm, m, 16, 1), best = -1;
            for (int i = 0; i < n && i < 16; i++) { const char* b = strrchr(m[i], '\\'); b = b ? b + 1 : m[i]; if (!_stricmp(b, nm)) { best = i; break; } if (best < 0) best = i; }
            if (best >= 0) { const uint32_t h = gf_stream_load(m[best]); printf("[stream] v1073S IGC test: LoadStream \"%s\" -> handle %08X\n", m[best], h); fflush(stdout); }
            else { printf("[stream] v1073S IGC test: no stream TOC entry contains \"%s\"\n", nm); fflush(stdout); }
            t_load = t; st = 2; }
        if ((st == 2 && t - t_load >= 3000) || (st == 3 && t - t_load >= 10000)) {
            char ev[96]; snprintf(ev, sizeof ev, "start_igc_%s", _strnicmp(nm, "igc_", 4) ? nm : nm + 4);   /* stream igc_X -> event start_igc_X */
            { const uint32_t h = gf_mission_hash(ev); const int r = inject(h);
              printf("[stream] v1073S IGC test: inject %s (%08X) -> %s, cutscene active %d\n", ev, h, r > 0 ? "dispatched" : r < 0 ? "FAULT" : "not dispatched", gf_cutscene_active()); fflush(stdout); }
            st = 4; }   /* once: a 2nd start of the same IGC hangs the game (s73-6) */
    }
}

/* ================================================================ v1119X (Claude): who throws the uncaught C++ exception?
 * Co-op runs abort() at story stream transitions (0xABBF14 via __terminate 0xABB528): the library personality routine
 * func_00ABB004 decides to terminate at loc_00ABB4B4 with r25 = the __cxa_exception header (+0 type_info*, +0x10 next,
 * +0x14 handler count, +0x30 unwind header). That runs in the search phase on top of the THROWER's stack, so a back-chain walk
 * from r1 (back chain at *(sp), LR save at back+0x10) gives the real throw site. Logged once per type. */
void gf_cxx_terminate_note(uint32_t xhdr, uint32_t sp, uint32_t lr)
{
    static volatile LONG n; char tn[96] = "?"; uint32_t ti = 0, nm = 0;
    if (InterlockedIncrement(&n) > 4) return;
    __try {
        if (ea_ok(xhdr, 0x40)) ti = rd32(xhdr);
        if (ea_ok(ti, 8)) nm = rd32(ti + 4);
        if (ea_ok(nm, 1)) { int i = 0; for (; i < 95; i++) { const char c = (char)vm_base[nm + i]; if (!c || c < 32 || c > 126) break; tn[i] = c; } tn[i] = 0; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    printf("[cxx] v1119X UNCAUGHT C++ EXCEPTION -> terminate: type_info %08X \"%s\" (header %08X, personality lr %08X, sp %08X)\n[cxx] guest call chain:", ti, tn, xhdr, lr, sp);
    __try { uint32_t s = sp; for (int k = 0; k < 28 && ea_ok(s, 0x18); k++) { const uint32_t back = rd32(s + 4); if (!ea_ok(back, 0x18) || back <= s) break; printf(" %08X", rd32(back + 0x14)); s = back; } }
    __except (EXCEPTION_EXECUTE_HANDLER) { printf(" (fault)"); }
    printf("\n"); fflush(stdout);
}
/* v1119X2: std::terminate (func_00ABB4D0) entry. l39 aborted without passing the personality's terminate branch -> terminate was
 * called directly (__cxa_throw with no handler: begin_catch, then terminate). globals+0 = caughtExceptions = that header. */
void gf_cxx_terminate_entry(uint32_t globals, uint32_t sp, uint32_t lr)
{
    uint32_t hdr = 0;
    __try { if (ea_ok(globals, 8)) hdr = rd32(globals); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    printf("[cxx] v1119X2 std::terminate called from %08X (globals %08X, current exception header %08X, uncaught %u)\n", lr, globals, hdr, ea_ok(globals, 8) ? rd32(globals + 4) : 0); fflush(stdout);
    gf_cxx_terminate_note(hdr, sp, lr);
}

/* ================================================================ v1119P2 (Claude): the co-op transition abort = a PURE VIRTUAL CALL
 * l60 caught it: std::terminate with NO exception active (header 0, uncaught 0) at start_igc_act01_t01_01. func_00ABB528 is
 * __cxa_pure_virtual (its OPD 0x00B43E98 sits directly in hundreds of vtables). Every abort's stack scan starts 0x78A404 =
 * func_0078A1F0 (physics-world style remove + profiler marks) -> func_00772BF0(obj): walks the listener array obj+0x1E8 {ptr,
 * count} backwards and calls listener->vt[+8](listener, obj) = an "entity removed" callback. A listener that was DESTROYED but
 * never unregistered (vtable reset to its abstract base: vt+8 = __cxa_pure_virtual) kills the game when its body is removed at a
 * stream unload. Single player never hit it (0/5) -> most likely a co-op object (puppet / proxy) freed without unregistering.
 * Fix layer 1: func_00772BF0 skips such a listener (gf_pure_listener_skip). Layer 2: __cxa_pure_virtual logs the caller and
 * returns 0 instead of aborting (GF_PURE_VIRTUAL_SURVIVE=0 = old abort). Find the owner from the logged vtables, then fix it. */
int gf_pure_listener_skip(uint32_t listener, uint32_t vt, uint32_t owner)
{
    static volatile LONG n;
    const LONG k = InterlockedIncrement(&n);
    if (k <= 20 || (k & 255) == 0) {
        uint32_t ovt = 0, w[4] = {0}; safe32(owner, &ovt); for (int i = 0; i < 4; i++) safe32(listener + 4u + 4u * (uint32_t)i, &w[i]);
        printf("[pure] v1119P2 DEAD LISTENER skipped (#%ld): listener %08X vt %08X (abstract base) words %08X %08X %08X %08X | on body %08X vt %08X\n",
               (long)k, listener, vt, w[0], w[1], w[2], w[3], owner, ovt); fflush(stdout); }
    return 1;
}
int gf_pure_virtual_called(uint32_t lr, uint32_t r3)
{
    static int survive = -1; static volatile LONG n;
    if (survive < 0) { const char* e = getenv("GF_PURE_VIRTUAL_SURVIVE"); survive = !(e && *e == '0'); }
    const LONG k = InterlockedIncrement(&n);
    if (k <= 20 || (k & 255) == 0) { uint32_t vt = 0; safe32(r3, &vt);
        printf("[pure] v1119P2 PURE VIRTUAL CALL #%ld from %08X on object %08X (vt %08X) -> %s\n", (long)k, lr, r3, vt, survive ? "returning 0 (survive)" : "abort"); fflush(stdout); }
    return survive;
}
