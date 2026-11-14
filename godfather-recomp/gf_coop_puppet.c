/* Co-op partner character probe and root-transform puppet.
 *
 * GF_COOP_PUPPET=trace records an authentic BuddySpawn.
 * GF_COOP_PUPPET=spawn creates one recorded character on request.
 * GF_COOP_PUPPET=drive also applies the partner root transform every frame.
 * All guest calls are made by gf_coop_puppet_tick() on the main PPU thread. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "recompiled/ppu_recomp.h"
#include "gf_memq.h"   /* v1061E */

extern uint8_t* vm_base;
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
extern int gf_coop_partner_pose(float out_m[16], float* hp);
extern int gf_coop_connected(void);

enum { MODE_OFF, MODE_TRACE, MODE_SPAWN, MODE_DRIVE };
enum { GAME_TOC = 0x00B4BF60u, MAX_SPAWN_TRIES = 3 };
typedef void (*gfn)(ppu_context*);

static int s_mode;
static int s_auto;
static volatile LONG s_request = -1;
static uint32_t s_ent; static uint32_t s_ent_vt;   /* v1127C: vtable at spawn */
static int pup_alive(uint32_t e); static long st_pup_gone;   /* v1127C (defined near gf_coop_puppet_entity) */
static uint32_t s_owner;
static uint32_t s_template_vt;
static uint8_t s_guid[16];
static int s_have_guid;
static int s_guid_authentic;
static int s_spawn_tries;
static ULONGLONG s_next_try;
static ULONGLONG s_pose_lost_at;
static int s_call_fail;

static float s_want_pos[3];
static int s_have_want;
static double s_drift_sum;
static float s_drift_max;
static unsigned s_drift_n;
static unsigned s_drift_over;
static ULONGLONG s_drift_log_at;

static uint32_t s_scan_ent;
static uint32_t s_scan_off;
static int s_live_found;

static int ea_range(uint32_t ea, size_t n)
{
    return ea >= 0x10000u && ea < 0xF0000000u && n <= 0xF0000000u - ea;
}

static int mem_access(uint32_t ea, size_t n, int write)
{   /* v1061E: cached (gf_memq.h) -- was one VirtualQuery per read */
    if (!ea_range(ea, n)) return 0;
    return gf_memq_access(vm_base, ea, n, write);
}

static uint32_t rd32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void wr32(uint32_t ea, uint32_t v)
{
    uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static float rdf(uint32_t ea)
{
    uint32_t u = rd32(ea); float f; memcpy(&f, &u, 4); return f;
}

static int read_u32(uint32_t ea, uint32_t* out)
{
    if (!mem_access(ea, 4, 0)) return 0;
    __try { *out = rd32(ea); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
/* v1068Z (agent Z): func_004530A8(attrib) walks records p = attrib+0x14 while (*(p+4) != 0x40000000) p += *p (when byte +0x10
 * bit 0 is clear). An attrib whose block was freed / zeroed / reused (area change) makes that loop spin FOREVER on the main thread:
 * t68-6-echo FLIP_STALL ~165 s after "entity gone (area change)" -- every [HANG] RIP symbolized (build-perf pdb) to
 * func_004530A8 ppu_recomp_002.cpp:63433-63435 (+ vm_read32). Pre-walk it here, bounded and with checked reads; 0 = the game's
 * walk would not terminate -> the caller treats the attrib as invalid. GF_COOP_ATTRIB_GUARD=0 = old (no pre-walk). */
static int attrib_walk_ok(uint32_t at)
{
    static int s_ag = -1; uint32_t w = 0, p, step = 0, tag = 0; int i;
    if (s_ag < 0) { const char* e = getenv("GF_COOP_ATTRIB_GUARD"); s_ag = !(e && *e == '0');
        printf("[coop-attrib] v1068Z %s: template attrib walk (func_004530A8) pre-checked %s (GF_COOP_ATTRIB_GUARD)\n", __FILE__, s_ag ? "ON" : "off"); fflush(stdout); }
    if (!s_ag) return 1;
    if (!read_u32(at + 0x10u, &w)) return 0;
    if ((w >> 24) & 1u) return 1;   /* byte +0x10 bit 0 set: the game takes the other (non-looping) branch */
    p = at + 0x14u;
    for (i = 0; i < 16384; i++) {
        if (!read_u32(p + 4u, &tag)) return 0;
        if (tag == 0x40000000u) return 1;
        if (!read_u32(p, &step) || !step) { static int nl; if (nl++ < 10) { printf("[coop-attrib] v1068Z attrib %08X: record walk stuck at %08X (step 0) -> attrib rejected (would hang func_004530A8)\n", at, p); fflush(stdout); } return 0; }
        p += step;   /* 32-bit wrap like the game's rldicl */
    }
    { static int nl; if (nl++ < 10) { printf("[coop-attrib] v1068Z attrib %08X: record walk did not end after %d steps -> attrib rejected\n", at, i); fflush(stdout); } }
    return 0;
}

static int read_bytes(uint32_t ea, void* out, size_t n)
{
    if (!mem_access(ea, n, 0)) return 0;
    __try { memcpy(out, vm_base + ea, n); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
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
    static uint32_t ca[48]; static gfn cf[48]; static int cn;
    uint64_t i; gfn f = 0;
    for (i = 0; i < (uint64_t)cn; i++) if (ca[i] == addr) return cf[i];
    for (i = 0; i < function_table_count; i++)
        if ((uint32_t)function_table[i].addr == addr) { f = function_table[i].func; break; }
    if (cn < 48) { ca[cn] = addr; cf[cn] = f; cn++; }
    if (!f) { printf("[coop-puppet] func_%08X is not in the function table\n", addr); fflush(stdout); }
    return f;
}

static uint64_t gcall_raw(gfn f, uint32_t toc, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    ppu_context* cur = g_active_ctx;
    static ppu_context c;
    if (!cur || !f) return 0;
    memcpy(&c, cur, sizeof c);
    c.gpr[1] = ((uint32_t)cur->gpr[1] - 0x1000u) & ~0xFull;
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
    ppu_context* cur = g_active_ctx;
    gfn f;
    if (!cur || !(f = lookup(addr))) return 0;
    __try { return gcall_raw(f, toc, a0, a1, a2, a3, a4); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_active_ctx = cur; g_trampoline_fn = 0; s_call_fail++;
        printf("[coop-puppet] guest call %08X faulted\n", addr); fflush(stdout); return 0;
    }
}

static uint64_t gcall(uint32_t addr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    return gcall_toc(addr, GAME_TOC, a0, a1, a2, a3, a4);
}

static uint32_t scratch(void)
{
    uint32_t ea;
    if (!g_active_ctx) return 0;
    ea = ((uint32_t)g_active_ctx->gpr[1] - 0x800u) & ~0xFu;
    return mem_access(ea, 0x80, 1) ? ea : 0;
}

static void guid_text(const uint8_t g[16], char out[33])
{
    static const char h[] = "0123456789ABCDEF";
    int i;
    for (i = 0; i < 16; i++) { out[i * 2] = h[g[i] >> 4]; out[i * 2 + 1] = h[g[i] & 15]; }
    out[32] = 0;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_guid(const char* s, uint8_t out[16])
{
    int hi = -1, n = 0, v;
    while (*s && n < 16) {
        v = hexval((unsigned char)*s++);
        if (v < 0) continue;
        if (hi < 0) hi = v;
        else { out[n++] = (uint8_t)((hi << 4) | v); hi = -1; }
    }
    return n == 16 && hi < 0;
}

static int template_path(char out[MAX_PATH])
{
    const char* src = __FILE__;
    const char* a = strrchr(src, '\\');
    const char* b = strrchr(src, '/');
    const char* slash = a;
    size_t n;
    if (!slash || (b && b > slash)) slash = b;
    if (!slash || !(strlen(src) > 2 && src[1] == ':')) return 0;
    n = (size_t)(slash - src + 1);
    if (n + strlen("gf_coop_templates.txt") + 1 > MAX_PATH) return 0;
    memcpy(out, src, n);
    strcpy(out + n, "gf_coop_templates.txt");
    return 1;
}

static void save_template(const char* kind, const uint8_t guid[16], uint32_t owner, uint32_t vt, int off)
{
    char path[MAX_PATH], g[33]; FILE* f;
    guid_text(guid, g);
    if (!template_path(path)) {
        printf("[coop-puppet] cannot derive source-side gf_coop_templates.txt path; template not saved\n"); fflush(stdout); return;
    }
    f = fopen(path, "a");
    if (!f) { printf("[coop-puppet] cannot append %s\n", path); fflush(stdout); return; }
    if (off >= 0) fprintf(f, "%s guid=%s entity_offset=0x%X vtable=%08X\n", kind, g, off, vt);
    else fprintf(f, "%s guid=%s owner=%08X vtable=%08X\n", kind, g, owner, vt);
    fclose(f);
    printf("[coop-puppet] saved %s template %s to %s\n", kind, g, path); fflush(stdout);
}

static void load_template_file(void)
{
    char path[MAX_PATH], line[256], last[33] = ""; FILE* f; unsigned vt = 0;
    if (!template_path(path) || !(f = fopen(path, "r"))) return;
    while (fgets(line, sizeof line, f)) {
        char* p = strstr(line, "buddy guid=");
        unsigned x;
        if (!p) continue;
        p += strlen("buddy guid=");
        if (strlen(p) < 32) continue;
        memcpy(last, p, 32); last[32] = 0;
        p = strstr(p, "vtable=");
        if (p && sscanf(p + 7, "%x", &x) == 1) vt = x;
    }
    fclose(f);
    if (last[0] && parse_guid(last, s_guid)) {
        s_have_guid = 1; s_guid_authentic = 1; s_template_vt = (uint32_t)vt;
        printf("[coop-puppet] loaded BuddySpawn template %s vtable=%08X\n", last, s_template_vt); fflush(stdout);
    }
}

static int entity_info(uint32_t ent, uint32_t* vt, uint32_t* xf)
{
    uint32_t v, x, opd;
    if (!ea_range(ent, 0x6Cu) || !read_u32(ent, &v) || !ea_range(v, 0x30u) || !v) return 0;
    if (v == 0x1005F6C8u) return 0;   /* v1127C: destroyed character (base vtable, pure virtuals) -- c27-15: the game frees the puppet in
                                       * the LOCAL player's death reload; calls on it gave ~20 PURE VIRTUAL CALLs (c27-11: process exit) */
    if (!read_u32(ent + 0x68u, &x) || !ea_range(x, 0x50u) || !mem_access(x + 0x10u, 0x40, 0)) return 0;
    if (!read_u32(v + 0x2Cu, &opd) || !ea_range(opd, 8) || !mem_access(opd, 8, 0)) return 0;
    if (vt) *vt = v; if (xf) *xf = x;
    return 1;
}

static int call_virtual(uint32_t ent, uint32_t slot, uint32_t a1)
{
    uint32_t vt, opd, fn, toc; int before = s_call_fail;
    if (!read_u32(ent, &vt) || !ea_range(vt + slot, 4) || !read_u32(vt + slot, &opd) ||
        !ea_range(opd, 8) || !read_u32(opd, &fn) || !read_u32(opd + 4, &toc) || !fn || !toc)
        return 0;
    if (!lookup(fn)) return 0;
    gcall_toc(fn, toc, ent, a1, 0, 0, 0);
    return s_call_fail == before;
}

/* v1058F: two-argument virtual call that returns r3 (component lookup) */
static uint32_t call_virtual2(uint32_t ent, uint32_t slot, uint32_t a1, uint32_t a2)
{
    uint32_t vt, opd, fn, toc;
    if (!read_u32(ent, &vt) || !ea_range(vt + slot, 4) || !read_u32(vt + slot, &opd) ||
        !ea_range(opd, 8) || !read_u32(opd, &fn) || !read_u32(opd + 4, &toc) || !fn || !toc || !lookup(fn)) return 0;
    return (uint32_t)gcall_toc(fn, toc, ent, a1, a2, 0, 0);
}
/* v1058F: BuddySpawn's post-spawn placement. The genuine action+0xB0 block is a world position (x, y, z) followed by the
 * option word 2 (traces q58m). BuddySpawn: comp = ent->vt[0x14](0xA5975EB2, &out); if (comp && out) func_002CCFD0(comp,
 * block); ent->vt[0x24](block). Without it the entity never leaves its default transform (q58p: 127 m drift, never moves). */
static int post_init(uint32_t ent, uint32_t buf, const float m[16])
{
    const uint32_t blk = buf + 0x40u, outp = buf + 0x50u; uint32_t u, comp, outv = 0; int before = s_call_fail;
    __try { memcpy(&u, &m[12], 4); wr32(blk, u); memcpy(&u, &m[13], 4); wr32(blk + 4, u); memcpy(&u, &m[14], 4); wr32(blk + 8, u);
            wr32(blk + 12, 2u); wr32(outp, 0); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    comp = call_virtual2(ent, 0x14u, 0xA5975EB2u, outp);
    read_u32(outp, &outv);
    if (comp && outv) gcall(0x002CCFD0u, comp, blk, 0, 0, 0);
    if (!call_virtual(ent, 0x24u, blk)) return 0;
    printf("[coop-puppet] v1058F post-init: component %08X (out %08X), vtable +0x24 with position %.2f %.2f %.2f%s\n",
           comp, outv, (double)m[12], (double)m[13], (double)m[14], s_call_fail != before ? " (a call faulted)" : ""); fflush(stdout);
    return s_call_fail == before;
}

static int write_matrix(uint32_t dst, uint32_t source_xf, const float m[16])
{
    int i;
    if (!mem_access(dst, 0x40, 1) || !mem_access(source_xf + 0x10u, 0x40, 0)) return 0;
    __try {
        for (i = 0; i < 16; i++) {
            uint32_t u;
            if ((i & 3) == 3) u = rd32(source_xf + 0x10u + (uint32_t)i * 4u);
            else memcpy(&u, &m[i], 4);
            wr32(dst + (uint32_t)i * 4u, u);
        }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* v1059A late frame set. gf_coop_puppet_late(ent) is called by gf_coop_after_anim for every animated entity (main PPU
 * thread, right after func_0046316C = after this frame's AI / movement / collision). For the puppet it re-asserts the
 * frame with the frame-only setter vt+0x2C so the rendered root is exactly the network pose even when collision pushed
 * the body (q: cA-6 showed 0.4-0.6 m per-frame pushback when the target overlaps furniture). The main-tick vt+0x30 call
 * still moves the physics / world-partition node. GF_COOP_DRIVE_LATE=0 turns it off. */
static float s_late_m[16]; static int s_late_ok = 0, s_late_on = -1; static unsigned s_late_n = 0;
void gf_coop_puppet_late(uint32_t ent)
{
    uint32_t vt, xf, buf; int before;
    if (!s_late_ok || !ent || ent != s_ent || s_mode != MODE_DRIVE) return;
    if (s_late_on < 0) { const char* e = getenv("GF_COOP_DRIVE_LATE"); s_late_on = !(e && *e == '0');
        printf("[coop-puppet] v1059A late frame set after the puppet's anim update: %s (GF_COOP_DRIVE_LATE)\n", s_late_on ? "ON" : "off"); fflush(stdout); }
    if (!s_late_on) return;
    buf = scratch(); before = s_call_fail;
    if (!buf || !entity_info(ent, &vt, &xf) || !write_matrix(buf, xf, s_late_m)) return;
    if (call_virtual(ent, 0x2Cu, buf) && s_call_fail == before) s_late_n++;
    else { s_late_ok = 0; printf("[coop-puppet] v1059A late frame set faulted -> off for this puppet\n"); fflush(stdout); }
}
static void release_puppet(const char* why)
{
    s_late_ok = 0;   /* v1059A */
    uint32_t ent = s_ent;
    s_ent = 0; s_have_want = 0;
    if (!ent) return;
    printf("[coop-puppet] release ent=%08X handle=%08X reason=%s\n", ent, ent + 0x30u, why); fflush(stdout);
    gcall(0x00455F58u, ent + 0x30u, 0, 0, 0, 0);
}

static void disable_crew_ai(uint32_t ent, uint32_t vt)
{
    uint32_t a, b, c, d; int before = s_call_fail;
    if (!s_guid_authentic || !s_template_vt || vt != s_template_vt) {
        printf("[coop-puppet] AI disable skipped: entity is not matched to an authentic BuddySpawn vtable\n"); fflush(stdout); return;
    }
    if (!read_u32(ent + 0x508u, &a) || !read_u32(ent + 0x1F70u, &b)) {
        printf("[coop-puppet] AI disable skipped: character flag fields are unreadable\n"); fflush(stdout); return;
    }
    { const char* ae = getenv("GF_COOP_PUPPET_AIOFF"); if (!(ae && atoi(ae) == 1)) {   /* v1058F: crew-disable froze on a non-crew puppet (q58n) */
        printf("[coop-puppet] AI disable skipped (GF_COOP_PUPPET_AIOFF=1 to try func_001281AC; it froze the game in q58n)\n"); fflush(stdout); return; } }
    printf("[coop-puppet] AI disable pre flags508=%08X flags1f70=%08X\n", a, b); fflush(stdout);
    gcall(0x001281ACu, ent, 0, 0, 0, 0);
    if (s_call_fail != before || !read_u32(ent + 0x508u, &c) || !read_u32(ent + 0x1F70u, &d))
        printf("[coop-puppet] AI disable did not complete safely\n");
    else
        printf("[coop-puppet] AI disable post flags508=%08X flags1f70=%08X\n", c, d);
    fflush(stdout);
}

/* v1059A drive setter: GF_COOP_DRIVE=30 (default) = vtable +0x30 SetMatrix (func_00233248: RwFrameTransform + scale cache
 * ent+0x70 + world-partition node ent+0x120 + vt+0x64 moved notification); 2C = old frame-only setter func_0046E9F4;
 * 24 = 2C plus vtable +0x24 SetPosition {x,y,z,2} (func_002331F8). */
static int s_drive_slot = -1;
static uint32_t drive_slot(void)
{
    if (s_drive_slot < 0) { const char* e = getenv("GF_COOP_DRIVE"); s_drive_slot = 0x30;
        if (e && *e) { const long v = strtol(e, NULL, 16); if (v == 0x2C || v == 0x30 || v == 0x24) s_drive_slot = (int)v; }
        printf("[coop-puppet] v1059A drive setter vtable +0x%02X (GF_COOP_DRIVE=30|2C|24)\n", s_drive_slot); fflush(stdout); }
    return (uint32_t)s_drive_slot;
}
static int apply_pose(uint32_t ent, const float m[16])
{
    uint32_t vt, xf, buf = scratch(), slot = drive_slot(); int before = s_call_fail;
    if (!buf || !entity_info(ent, &vt, &xf) || !write_matrix(buf, xf, m)) return 0;
    if (!call_virtual(ent, slot == 0x30u ? 0x30u : 0x2Cu, buf) || s_call_fail != before) return 0;
    if (slot == 0x24u) { const uint32_t blk = buf + 0x40u; uint32_t u;
        __try { memcpy(&u, &m[12], 4); wr32(blk, u); memcpy(&u, &m[13], 4); wr32(blk + 4, u); memcpy(&u, &m[14], 4); wr32(blk + 8, u); wr32(blk + 12, 2u); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        if (!call_virtual(ent, 0x24u, blk) || s_call_fail != before) return 0; }
    s_want_pos[0] = m[12]; s_want_pos[1] = m[13]; s_want_pos[2] = m[14]; s_have_want = 1;
    return 1;
}

/* ------------------------------------------------------------------ v1058I real character puppet
 * q58z + agent RE: func_00252298 is FuncSpawn (dynamic objects), not BuddySpawn; func_0029B9F0 builds prop classes
 * (vtable 10044CE8) with NO animation controller (+0xC4 is a GUID field there). Characters come from the managed
 * factory func_003388AC(classHash, attrib, owner, requester): crew 0xBDA2CAC1/0xD1812E99/0xF48F270E, 0xA37F4AB4,
 * pedestrians 0xBA1F9E7B/0x3E8BF7B3/0x5DA5581E; it builds the entity, creates the anim controller (func_0046348C via the
 * Animated base ctor if the model is resident, else the NPC appearance handler on iMsgManagedEntityCreated) and sends
 * iMsgPostSpawnInitialize. gf_coop_char_factory_note() (hand edit at the entry of func_003388AC) records hash + attrib
 * only; the main tick resolves the template GUID (func_004530A8(attrib) -> GUID pointer) and keeps one (crew preferred). */
static uint32_t s_cf_hash[64], s_cf_attrib[64]; static volatile LONG s_cf_head = 0; static LONG s_cf_done = 0;
static uint8_t s_char_guid[16]; static uint32_t s_char_hash = 0; static int s_have_char = 0, s_char_rank = 99;
static int char_rank(uint32_t h)
{
    if (h == 0xBDA2CAC1u || h == 0xD1812E99u || h == 0xF48F270Eu) return 0;   /* crew / gangster */
    if (h == 0xA37F4AB4u) return 1;
    if (h == 0xBA1F9E7Bu || h == 0x3E8BF7B3u || h == 0x5DA5581Eu) return 2;   /* pedestrians */
    return 99;
}
void gf_coop_char_factory_note(uint32_t hash, uint32_t attrib)   /* PPU thread, inside the factory: record only */
{
    { extern void gf_proxy_factory_note(uint32_t h, uint32_t a); gf_proxy_factory_note(hash, attrib); }   /* v1060W3 world proxies: every class */
    if (!s_mode || char_rank(hash) > 2) return;
    const LONG i = InterlockedIncrement(&s_cf_head) - 1;
    s_cf_hash[i & 63] = hash; s_cf_attrib[i & 63] = attrib;
}
static void save_char_template(void)
{
    char path[MAX_PATH], g[33]; FILE* f; guid_text(s_char_guid, g);
    if (!template_path(path) || !(f = fopen(path, "a"))) return;
    fprintf(f, "char hash=%08X guid=%s\n", s_char_hash, g); fclose(f);
}
static void load_char_template(void)
{
    char path[MAX_PATH], line[256]; FILE* f;
    if (!template_path(path) || !(f = fopen(path, "r"))) return;
    while (fgets(line, sizeof line, f)) {
        unsigned h; char g[40]; uint8_t gg[16];
        if (sscanf(line, "char hash=%x guid=%32s", &h, g) == 2 && parse_guid(g, gg) && char_rank(h) <= s_char_rank) {
            memcpy(s_char_guid, gg, 16); s_char_hash = h; s_have_char = 1; s_char_rank = char_rank(h); }
    }
    fclose(f);
    if (s_have_char) { char g[33]; guid_text(s_char_guid, g); printf("[coop-puppet] v1058I loaded character template class %08X guid %s\n", s_char_hash, g); fflush(stdout); }
}
/* v1059C: every factory note of this session is also kept as a LIVE (hash, attrib) candidate per rank. The saved GUID
 * only resolves through the template registry (func_004AD7F0: hash table at reg+0x48, chain +0xC, func_004530A8(attrib)
 * -> GUID ptr) once that archetype is loaded; when it is not, spawn_char falls back to a live candidate that still
 * resolves (attrib -> GUID -> registry -> same attrib). Test switches: GF_COOP_PUPPET_TEST_NOLOOKUP=1 (pretend the saved
 * GUID is not registered), GF_COOP_PUPPET_TEST_NONOTES=1 (ignore factory notes). */
static uint32_t s_live_hash[3], s_live_attrib[3];
static int s_test_nolookup = -1, s_test_nonotes = -1;
static int env_on(const char* n) { const char* e = getenv(n); return e && *e && strcmp(e, "0"); }
static void char_resolve(void)   /* main PPU thread */
{
    const LONG head = s_cf_head;
    if (s_test_nonotes < 0) { s_test_nonotes = env_on("GF_COOP_PUPPET_TEST_NONOTES"); s_test_nolookup = env_on("GF_COOP_PUPPET_TEST_NOLOOKUP");
        if (s_test_nonotes || s_test_nolookup) { printf("[coop-puppet] v1059C TEST nonotes=%d nolookup=%d\n", s_test_nonotes, s_test_nolookup); fflush(stdout); } }
    if (s_test_nonotes) { s_cf_done = head; return; }
    if (head - s_cf_done > 64) s_cf_done = head - 64;
    for (; s_cf_done < head; s_cf_done++) {
        const uint32_t h = s_cf_hash[s_cf_done & 63], at = s_cf_attrib[s_cf_done & 63]; const int r = char_rank(h);
        if (r <= 2 && ea_range(at, 0x20)) {
            if (!s_live_attrib[r]) { printf("[coop-puppet] v1059C live template candidate class %08X attrib %08X (rank %d)\n", h, at, r); fflush(stdout); }
            s_live_hash[r] = h; s_live_attrib[r] = at; }
        if (r >= s_char_rank || !ea_range(at, 0x20) || !attrib_walk_ok(at)) continue;   /* v1068Z */
        const uint32_t gp = (uint32_t)gcall(0x004530A8u, at, 0, 0, 0, 0); uint8_t g[16]; int nz = 0;
        if (!ea_range(gp, 16) || !read_bytes(gp, g, 16)) continue;
        for (int i = 0; i < 16; i++) nz |= g[i];
        if (!nz) continue;
        memcpy(s_char_guid, g, 16); s_char_hash = h; s_have_char = 1; s_char_rank = r; save_char_template();
        { char t[33]; guid_text(g, t); printf("[coop-puppet] v1058I character template: class %08X guid %s (%s)\n", h, t, r == 0 ? "crew" : r == 1 ? "npc" : "pedestrian"); fflush(stdout); }
    }
}
/* v1059C: crew-character AI off. func_001281AC = the game's own "disable crew character" (clears +0x508 bit 0x10, resets
 * the embedded AI/action state +0x1CBC/+0x1D9C/+0x1F70, removes it from the combat managers). q58n froze because it ran on
 * the old PROP puppet (vtable 10044CE8, no such fields); the v1058I puppet is a real crew character. Only for crew classes.
 * GF_COOP_PUPPET_AIOFF=0 off, =bit only clears +0x508 bit 0x10, default/1 = func_001281AC. */
static void char_ai_off(uint32_t ent, uint32_t cls)
{
    const char* e = getenv("GF_COOP_PUPPET_AIOFF"); uint32_t a = 0, b = 0, c = 0, d = 0; int before = s_call_fail;
    if (e && *e == '0') return;
    if (char_rank(cls) != 0) { printf("[coop-puppet] v1059C AI off skipped: class %08X is not a crew class\n", cls); fflush(stdout); return; }
    if (!read_u32(ent + 0x508u, &a) || !read_u32(ent + 0x1F70u, &b)) { printf("[coop-puppet] v1059C AI off skipped: fields unreadable\n"); fflush(stdout); return; }
    if (e && !_stricmp(e, "bit")) { wr32(ent + 0x508u, a & ~0x10u); read_u32(ent + 0x508u, &c);
        printf("[coop-puppet] v1059C AI off (bit): +508 %08X -> %08X\n", a, c); fflush(stdout); return; }
    printf("[coop-puppet] v1059C AI off: func_001281AC pre +508=%08X +1F70=%08X\n", a, b); fflush(stdout);
    gcall(0x001281ACu, ent, 0, 0, 0, 0);
    read_u32(ent + 0x508u, &c); read_u32(ent + 0x1F70u, &d);
    printf("[coop-puppet] v1059C AI off: %s post +508=%08X +1F70=%08X\n", s_call_fail != before ? "CALL FAILED" : "done", c, d); fflush(stdout);
}

/* v1059P player look: the puppet is spawned as a crew character (the only class the managed factory makes), then its
 * model is replaced by the LOCAL PLAYER's model. func_0046348C(ent, model) is the game's own Animated::SetModel: it
 * unregisters + destroys the old anim controller (+0xC4), allocates a new 0x320-byte one, func_003CB1F0(anim, ent, model)
 * binds it, registers it (func_004D461C) and sets anim+0x84 = ent+0x30 -- exactly what the Animated ctors
 * (func_004636C4 / func_0046386C) do after model = func_004D42C0(GUID). +0xB8 is the model the ctor stored.
 * Result: player skeleton + mobface_player mesh (rig key == the player's, mesh scale 1.0), the partner's pose stream
 * drives it. GF_COOP_PUPPET_LOOK=crew keeps the crew model; default/player = this. Falls back to crew on any failure.
 * The model is shared with the local player, so for now the puppet wears YOUR look; the partner's own MobFace look
 * (NOTES_mobface_RE.md) has to build/choose a model for it and call the same SetModel. */
static int s_look = -1;
uint32_t g_coop_puppet_model = 0;   /* v1059P: model now on the puppet (for the look agent) */
static int look_player(uint32_t ent)
{
    uint32_t pl = player(), pm = 0, old_m = 0, old_a = 0, na = 0, ctl = 0; int before = s_call_fail;
    if (s_look < 0) { const char* e = getenv("GF_COOP_PUPPET_LOOK"); s_look = !(e && !_stricmp(e, "crew"));
        printf("[coop-puppet] v1059P puppet look: %s (GF_COOP_PUPPET_LOOK=player|crew)\n", s_look ? "player model" : "crew model"); fflush(stdout); }
    if (!s_look || !pl || !ent) return 0;
    if (!read_u32(pl + 0xB8u, &pm) || !ea_range(pm, 0x40)) { printf("[coop-puppet] v1059P no player model (+B8=%08X) -> crew look\n", pm); fflush(stdout); return 0; }
    read_u32(ent + 0xB8u, &old_m); read_u32(ent + 0xC4u, &old_a);
    if (old_m == pm) return 1;
    wr32(ent + 0xB8u, pm);
    gcall(0x0046348Cu, ent, pm, 0, 0, 0);
    read_u32(ent + 0xC4u, &na); if (na) read_u32(na + 0x84u, &ctl);
    if (s_call_fail != before || !na || ctl != ent + 0x30u) {
        printf("[coop-puppet] v1059P SetModel(player model %08X) FAILED (call %s, anim %08X, bound %s) -> restoring crew model\n",
               pm, s_call_fail != before ? "faulted" : "ok", na, ctl == ent + 0x30u ? "yes" : "no"); fflush(stdout);
        wr32(ent + 0xB8u, old_m); if (old_m) gcall(0x0046348Cu, ent, old_m, 0, 0, 0);
        return 0;
    }
    g_coop_puppet_model = pm;
    { uint32_t pa = 0; read_u32(pl + 0xC4u, &pa);
      printf("[coop-puppet] v1059P puppet now uses the player model: model %08X -> %08X, anim %08X -> %08X (player anim %08X)\n",
             old_m, pm, old_a, na, pa); fflush(stdout); }
    {   /* v1127C look diagnosis (grey-suit pedestrian puppets): the per-ped appearance block *(ent+0x6C) of the puppet vs the player */
        uint32_t ab = 0, pb = 0, w[2][12]; memset(w, 0, sizeof w); read_u32(ent + 0x6Cu, &ab); read_u32(pl + 0x6Cu, &pb);
        for (int k = 0; k < 12; k++) { if (ea_range(ab, 0x60)) read_u32(ab + 0x30u + (uint32_t)k * 4u, &w[0][k]); if (ea_range(pb, 0x60)) read_u32(pb + 0x30u + (uint32_t)k * 4u, &w[1][k]); }
        uint32_t ah = 0, ph = 0; if (ea_range(ab, 4)) read_u32(ab, &ah); if (ea_range(pb, 4)) read_u32(pb, &ph);
        printf("[coop-puppet] v1127C look: puppet +6C %08X (hdr %08X) +30.. %08X %08X %08X %08X %08X %08X %08X %08X | player +6C %08X (hdr %08X) +30.. %08X %08X %08X %08X %08X %08X %08X %08X\n",
               ab, ah, w[0][0], w[0][1], w[0][2], w[0][3], w[0][4], w[0][5], w[0][6], w[0][7], pb, ph, w[1][0], w[1][1], w[1][2], w[1][3], w[1][4], w[1][5], w[1][6], w[1][7]); fflush(stdout); }
    return 1;
}

static void spawn_char(const float m[16])
{
    uint32_t buf = scratch(), reg, attrib, ent, vt, xf, c4 = 0, b8 = 0; int before = s_call_fail; char gt[33];
    if (!buf || !read_u32(0x10181088u, &reg) || !ea_range(reg, 4)) return;
    __try { memcpy(vm_base + buf + 0x60, s_char_guid, 16); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    guid_text(s_char_guid, gt);
    uint32_t cls = s_char_hash;
    attrib = s_test_nolookup > 0 ? 0 : (uint32_t)gcall(0x004AD7F0u, reg, buf + 0x60, 0, 0, 0);
    s_spawn_tries++;
    if (!attrib && s_call_fail == before) {   /* v1059C: saved archetype not loaded here -> live candidate of this session */
        for (int r = 0; r <= 2 && !attrib; r++) {
            const uint32_t at = s_live_attrib[r]; if (!at || !ea_range(at, 0x20)) continue;
            if (!attrib_walk_ok(at)) { s_live_attrib[r] = 0; continue; }   /* v1068Z: freed since it was noted */
            const uint32_t gp = (uint32_t)gcall(0x004530A8u, at, 0, 0, 0, 0);
            if (s_call_fail != before || !ea_range(gp, 16)) break;
            const uint32_t back = (uint32_t)gcall(0x004AD7F0u, reg, gp, 0, 0, 0);
            if (s_call_fail != before) break;
            if (back == at) { attrib = at; cls = s_live_hash[r];
                printf("[coop-puppet] v1059C saved template not registered -> live candidate class %08X attrib %08X\n", cls, at); fflush(stdout); }
            else { s_live_attrib[r] = 0; }   /* unloaded since it was noted */
        }
    }
    if (s_spawn_tries <= 5 || (s_spawn_tries % 10) == 0 || attrib) {
        printf("[coop-puppet] v1059C spawn character try %d class %08X guid %s attrib %08X\n", s_spawn_tries, cls, gt, attrib); fflush(stdout); }
    if (!attrib || s_call_fail != before) return;
    ent = (uint32_t)gcall(0x003388ACu, cls, attrib, 0, 0, 0);
    if (s_call_fail != before || !ent || !entity_info(ent, &vt, &xf)) { printf("[coop-puppet] v1058I character factory failed (%08X)\n", ent); fflush(stdout); return; }
    read_u32(ent + 0xC4u, &c4); read_u32(ent + 0xB8u, &b8);
    printf("[coop-puppet] v1058I character ent=%08X vtable=%08X xf=%08X model +B8=%08X anim +C4=%08X\n", ent, vt, xf, b8, c4); fflush(stdout);
    if (!c4) {   /* model was not resident at construction: run the NPC appearance init (iMsgManagedEntityCreated) */
        uint32_t id = 0; read_u32(0x101DBFD8u, &id);
        __try { wr32(buf + 0x70, id); wr32(buf + 0x74, ent); } __except (EXCEPTION_EXECUTE_HANDLER) { }
        gcall(0x00455A2Cu, buf + 0x70, ent + 0x30u, 0, 0, 0);
        read_u32(ent + 0xC4u, &c4);
        printf("[coop-puppet] v1058I sent iMsgManagedEntityCreated (%08X) -> anim +C4=%08X\n", id, c4); fflush(stdout);
    }
    s_ent = ent; { uint32_t v0 = 0; read_u32(ent, &v0); s_ent_vt = v0; }   /* v1127C */
    if (!apply_pose(ent, m)) { release_puppet("initial transform failed"); return; }
    {   const uint32_t blk = buf + 0x40u; uint32_t u;   /* vtable +0x24 placement block {x, y, z, 2} like BuddySpawn */
        __try { memcpy(&u, &m[12], 4); wr32(blk, u); memcpy(&u, &m[13], 4); wr32(blk + 4, u); memcpy(&u, &m[14], 4); wr32(blk + 8, u); wr32(blk + 12, 2u); }
        __except (EXCEPTION_EXECUTE_HANDLER) { }
        call_virtual(ent, 0x24u, blk); }
    { uint32_t c4b = 0, ctl = 0; read_u32(ent + 0xC4u, &c4b); if (c4b) read_u32(c4b + 0x84u, &ctl);
      printf("[coop-puppet] v1058I puppet placed: anim +C4=%08X bound=%s\n", c4b, ctl == ent + 0x30u ? "yes" : "no"); fflush(stdout); }
    if (s_ent == ent) char_ai_off(ent, cls);   /* v1059C */
    if (s_ent == ent) { __try { look_player(ent); } __except (EXCEPTION_EXECUTE_HANDLER) { printf("[coop-puppet] v1059P look_player faulted\n"); fflush(stdout); } }   /* v1059P */
}

static void spawn_puppet(const float m[16])
{
    { const char* pp = getenv("GF_COOP_PUPPET_PROP");   /* v1058I: the old prop-class path only on request */
      if (!(pp && *pp == '1')) {
          if (s_have_char) spawn_char(m);
          else { static ULONGLONG said; const ULONGLONG t = GetTickCount64(); if (t - said > 10000) { said = t;
              printf("[coop-puppet] v1058I waiting for a character template (any pedestrian / crew spawn near you records one)\n"); fflush(stdout); } }
          return; } }
    uint32_t buf = scratch(), pl = player(), owner = s_owner, ent, vt, xf;
    char gt[33]; int before = s_call_fail;
    if (!s_have_guid || !buf) return;
    if (!owner && pl) read_u32(pl + 0x18u, &owner);
    if (!owner) { printf("[coop-puppet] spawn deferred: no captured owner and player+0x18 is unavailable\n"); fflush(stdout); return; }
    __try { memcpy(vm_base + buf, s_guid, 16); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    guid_text(s_guid, gt);
    printf("[coop-puppet] spawn try %d/%d guid=%s owner=%08X%s\n", s_spawn_tries + 1, MAX_SPAWN_TRIES, gt, owner,
           s_owner ? " (captured)" : " (player+0x18 fallback)"); fflush(stdout);
    ent = (uint32_t)gcall(0x0029B9F0u, buf, owner, 0, 0, 0);
    s_spawn_tries++;
    if (s_call_fail != before || !ent || !entity_info(ent, &vt, &xf)) {
        printf("[coop-puppet] factory failed or returned an invalid entity (%08X)\n", ent); fflush(stdout); return;
    }
    s_ent = ent; { uint32_t v0 = 0; read_u32(ent, &v0); s_ent_vt = v0; }   /* v1127C */
    printf("[coop-puppet] factory ent=%08X vtable=%08X xf=%08X\n", ent, vt, xf); fflush(stdout);
    if (!apply_pose(ent, m)) { release_puppet("initial transform failed"); return; }
    { const char* pe = getenv("GF_COOP_PUPPET_POSTINIT"); if (!(pe && *pe == '0')) post_init(ent, buf, m); }   /* v1058F */
    { uint32_t vt2, xf2; if (entity_info(ent, &vt2, &xf2)) { printf("[coop-puppet] after placement xf=%08X (was %08X)\n", xf2, xf); fflush(stdout); } }
    disable_crew_ai(ent, vt);
}

static float s_last_drift = 0; static int s_stuck_frames = 0;   /* v1058F */
static void drift_sample(uint32_t ent)
{
    uint32_t vt, xf; float dx, dy, dz, d;
    if (!s_have_want || !entity_info(ent, &vt, &xf)) return;
    __try {
        dx = rdf(xf + 0x40u) - s_want_pos[0];
        dy = rdf(xf + 0x44u) - s_want_pos[1];
        dz = rdf(xf + 0x48u) - s_want_pos[2];
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    d = sqrtf(dx * dx + dy * dy + dz * dz);
    if (!(d == d) || d > 100000.0f) return;
    s_drift_sum += d; if (d > s_drift_max) s_drift_max = d; s_drift_n++; if (d > 0.05f) s_drift_over++;
    s_last_drift = d;   /* v1058F stuck detection */
}

static void drift_log(ULONGLONG now)
{
    if (!s_drift_log_at) s_drift_log_at = now;
    if (now - s_drift_log_at < 5000) return;
    printf("[coop-puppet] drift 5s samples=%u avg=%.4f m max=%.4f m overwritten(>5cm)=%u | v1059A late frame sets %u\n",
           s_drift_n, s_drift_n ? s_drift_sum / s_drift_n : 0.0, (double)s_drift_max, s_drift_over, s_late_n); s_late_n = 0;
    fflush(stdout);
    s_drift_sum = 0; s_drift_max = 0; s_drift_n = s_drift_over = 0; s_drift_log_at = now;
}

/* v1059A GF_COOP_DRIVE_PROBE=1: who owns the puppet's position? Every 2 s (16 lines max per kind) compare the frame
 * position the game left (before our setter) with what we set last frame, and scan the entity (+0..+0x2400) and every
 * heap pointer in +0..+0x800 (0x400 bytes behind it) for xyz triples equal to the game's position. Same for the player. */
static int near3(uint32_t ea, const float p[3], float tol)
{
    float a = rdf(ea), b = rdf(ea + 4), c = rdf(ea + 8);
    return fabsf(a - p[0]) < tol && fabsf(b - p[1]) < tol && fabsf(c - p[2]) < tol;
}
static void pos_scan(const char* who, uint32_t ent, const float p[3])
{
    char line[900]; int n = 0, hits = 0; uint32_t o, q;
    n += snprintf(line + n, sizeof line - (size_t)n, "[coop-drvprobe] %s %08X pos %.3f %.3f %.3f found:", who, ent, (double)p[0], (double)p[1], (double)p[2]);
    __try {
        for (o = 0; o < 0x2400u && hits < 24; o += 4) if (mem_access(ent + o, 12, 0) && near3(ent + o, p, 0.02f)) { hits++; n += snprintf(line + n, sizeof line - (size_t)n, " +%X", o); }
        for (o = 0; o < 0x800u && hits < 40; o += 4) { uint32_t ptr = 0; if (!read_u32(ent + o, &ptr) || ptr < 0x30000000u || ptr >= 0xD0000000u || (ptr >= ent && ptr < ent + 0x2400u)) continue;
            if (!mem_access(ptr, 0x400, 0)) continue;
            for (q = 0; q < 0x400u && hits < 40; q += 4) if (near3(ptr + q, p, 0.02f)) { hits++; n += snprintf(line + n, sizeof line - (size_t)n, " [+%X]+%X", o, q); } }
    } __except (EXCEPTION_EXECUTE_HANDLER) { n += snprintf(line + n, sizeof line - (size_t)n, " (fault)"); }
    printf("%s\n", line); fflush(stdout);
}
static void drive_probe(uint32_t ent, const float want[16])
{
    static int on = -1; static ULONGLONG next; static int logs; uint32_t vt, xf; float g[3], at[3]; const ULONGLONG now = GetTickCount64();
    if (on < 0) { const char* e = getenv("GF_COOP_DRIVE_PROBE"); on = e && atoi(e) == 1; }
    if (!on || now < next || logs >= 16 || !entity_info(ent, &vt, &xf)) return;
    next = now + 2000; logs++;
    __try { g[0] = rdf(xf + 0x40); g[1] = rdf(xf + 0x44); g[2] = rdf(xf + 0x48); at[0] = rdf(xf + 0x30); at[1] = rdf(xf + 0x34); at[2] = rdf(xf + 0x38); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    printf("[coop-drvprobe] puppet frame %.3f %.3f %.3f (game) vs last set %.3f %.3f %.3f -> d %.3f %.3f %.3f | yaw game %.1f want %.1f\n",
           (double)g[0], (double)g[1], (double)g[2], (double)s_want_pos[0], (double)s_want_pos[1], (double)s_want_pos[2],
           (double)(g[0] - s_want_pos[0]), (double)(g[1] - s_want_pos[1]), (double)(g[2] - s_want_pos[2]),
           (double)(atan2f(at[0], at[2]) * 57.29578f), (double)(atan2f(want[8], want[10]) * 57.29578f)); fflush(stdout);
    pos_scan("puppet", ent, g);
    { const uint32_t pl = player(); uint32_t pvt, pxf; float pp[3];
      if (pl && entity_info(pl, &pvt, &pxf)) { __try { pp[0] = rdf(pxf + 0x40); pp[1] = rdf(pxf + 0x44); pp[2] = rdf(pxf + 0x48); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
          pos_scan("player", pl, pp); } }
}

static void live_guid_scan(void)
{
    uint32_t pl, link = 0, ent, reg, attrib, valid, vt; uint8_t g[16]; char gt[33];
    if (s_live_found || !(pl = player()) || !read_u32(0x10181088u, &reg) || !ea_range(reg, 4)) return;
    read_u32(pl + 0x1544u, &link);
    ent = link > 0x38u && ea_range(link - 0x38u, 4) ? link - 0x38u : pl;
    if (!read_u32(ent, &vt) || !vt) return;
    if (ent != s_scan_ent) { s_scan_ent = ent; s_scan_off = 0; }
    if (s_scan_off > 0x400u) return;
    if (!read_bytes(ent + s_scan_off, g, 16)) { s_scan_off += 4; return; }
    { int i, nz = 0; for (i = 0; i < 16; i++) nz |= g[i]; if (!nz) { s_scan_off += 4; return; } }
    {
        uint32_t buf = scratch();
        if (!buf) return;
        __try { memcpy(vm_base + buf, g, 16); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        attrib = (uint32_t)gcall(0x004AD7F0u, reg, buf, 0, 0, 0);
        valid = attrib ? (uint32_t)gcall(0x00299DC4u, attrib, 0, 0, 0, 0) : 0;
    }
    if (attrib && valid) {
        guid_text(g, gt); s_live_found = 1;
        printf("[coop-puppet] live character template candidate ent=%08X vtable=%08X field=+%03X guid=%s attrib=%08X\n",
               ent, vt, s_scan_off, gt, attrib); fflush(stdout);
        save_template("live-candidate", g, 0, vt, (int)s_scan_off);
    }
    s_scan_off += 4;
}

/* Called by the one direct-call hand insertion in func_00252298, immediately after
 * func_0029B9F0 returns. It does not call guest code. */
void gf_coop_puppet_trace_buddy(uint32_t action, uint32_t guid_ea, uint32_t ent)
{
    uint8_t g[16], ag[16]; uint32_t owner = 0, vt = 0, f14 = 0, f24 = 0, f2c = 0;
    uint32_t o14 = 0, o24 = 0, o2c = 0, t14 = 0, t24 = 0, t2c = 0;
    uint32_t b0 = 0, b4 = 0, b8 = 0, bc = 0, f5c = 0;
    char gt[33], at[33]; int i;
    if (s_mode != MODE_TRACE || !ea_range(action, 0xC0u) || !read_bytes(guid_ea, g, 16)) return;
    if (!read_bytes(action + 0x44u, ag, 16)) memset(ag, 0, sizeof ag);
    read_u32(action + 0x18u, &owner); read_u32(action + 0x5Cu, &f5c);
    read_u32(action + 0xB0u, &b0); read_u32(action + 0xB4u, &b4); read_u32(action + 0xB8u, &b8); read_u32(action + 0xBCu, &bc);
    if (ent) {
        read_u32(ent, &vt);
        if (vt) { read_u32(vt + 0x14u, &o14); read_u32(vt + 0x24u, &o24); read_u32(vt + 0x2Cu, &o2c); }
        if (o14) { read_u32(o14, &f14); read_u32(o14 + 4, &t14); }
        if (o24) { read_u32(o24, &f24); read_u32(o24 + 4, &t24); }
        if (o2c) { read_u32(o2c, &f2c); read_u32(o2c + 4, &t2c); }
    }
    guid_text(g, gt); guid_text(ag, at);
    printf("[coop-puppet] BuddySpawn action=%08X guid_ea=%08X guid=%s action+44=%s owner(action+18)=%08X ent=%08X\n",
           action, guid_ea, gt, at, owner, ent);
    printf("[coop-puppet] action fields +5C=%08X +B0=%08X +B4=%08X +B8=%08X +BC=%08X\n", f5c, b0, b4, b8, bc);
    for (i = 0; i < 4; i++) {
        uint32_t p = action + 0x70u + (uint32_t)i * 16u, pad = 0; float x = 0, y = 0, z = 0;
        __try { x = rdf(p); y = rdf(p + 4); z = rdf(p + 8); pad = rd32(p + 12); }
        __except (EXCEPTION_EXECUTE_HANDLER) { }
        printf("[coop-puppet] action +%02X row%d %.6f %.6f %.6f pad=%08X\n", 0x70 + i * 16, i, x, y, z, pad);
    }
    printf("[coop-puppet] entity vtable=%08X opd14=%08X fn14=%08X toc14=%08X opd24=%08X fn24=%08X toc24=%08X opd2c=%08X fn2c=%08X toc2c=%08X\n",
           vt, o14, f14, t14, o24, f24, t24, o2c, f2c, t2c); fflush(stdout);
    memcpy(s_guid, g, 16); s_have_guid = 1; s_guid_authentic = 1; s_owner = owner; s_template_vt = vt;
    save_template("buddy", g, owner, vt, -1);
}

void gf_coop_puppet_request(int on)
{
    LONG old = InterlockedExchange(&s_request, on ? 1 : 0);
    if (on && old != 1) { s_spawn_tries = 0; s_next_try = 0; }
}

void gf_coop_puppet_init(void)
{
    { extern void gf_coop_combat_init(void); gf_coop_combat_init(); }   /* v1062C combat sync (gf_coop_combat.c) */
    const char* e = getenv("GF_COOP_PUPPET");
    const char* a = getenv("GF_COOP_PUPPET_AUTO");
    const char* ge = getenv("GF_COOP_PUPPET_GUID");
    char gt[33];
    if (!e || !*e || !strcmp(e, "0") || !strcmp(e, "off")) return;
    if (!_stricmp(e, "trace")) s_mode = MODE_TRACE;
    else if (!_stricmp(e, "spawn")) s_mode = MODE_SPAWN;
    else if (!_stricmp(e, "drive")) s_mode = MODE_DRIVE;
    else { printf("[coop-puppet] unknown GF_COOP_PUPPET=%s; expected trace, spawn, or drive\n", e); fflush(stdout); return; }
    s_auto = a && *a && strcmp(a, "0");
    load_template_file();
    load_char_template();   /* v1058I */
    if (ge && *ge) {
        uint8_t g[16];
        if (parse_guid(ge, g)) {
            int same_authentic = s_guid_authentic && s_have_guid && !memcmp(g, s_guid, 16);
            memcpy(s_guid, g, 16); s_have_guid = 1;
            if (!same_authentic) { s_guid_authentic = 0; s_template_vt = 0; }
        } else printf("[coop-puppet] GF_COOP_PUPPET_GUID is not a 16-byte GUID\n");
    }
    printf("[coop-puppet] GF_COOP_PUPPET=%s ON auto=%d\n", e, s_auto);
    if (s_have_guid) { guid_text(s_guid, gt); printf("[coop-puppet] selected template %s authentic=%d vtable=%08X\n", gt, s_guid_authentic, s_template_vt); }
    if (s_mode == MODE_TRACE) printf("[coop-puppet] waiting for a genuine BuddySpawn direct-call trace\n");
    else if (!s_have_guid) printf("[coop-puppet] no template yet; run trace and spawn/hire a crew member first, or set GF_COOP_PUPPET_GUID\n");
    fflush(stdout);
}

void gf_coop_puppet_tick(void)
{
    float m[16], hp = 0; int have_pose, want; LONG request; ULONGLONG now;
    uint32_t vt, xf;
    { extern void gf_coop_combat_tick(void); __try { gf_coop_combat_tick(); } __except (EXCEPTION_EXECUTE_HANDLER) { } }   /* v1062C combat sync */
    if (!s_mode || !g_active_ctx) return;
    char_resolve();   /* v1058I */
    if (s_mode == MODE_TRACE) { live_guid_scan(); return; }
    now = GetTickCount64();
    have_pose = gf_coop_connected() && gf_coop_partner_pose(m, &hp);
    request = InterlockedCompareExchange(&s_request, -1, -1);
    want = request > 0 || (request < 0 && s_auto && have_pose);
    if (request == 0) { if (s_ent) release_puppet("request off"); s_spawn_tries = 0; return; }
    if (!have_pose) {
        if (!s_pose_lost_at) s_pose_lost_at = now;
        if (now - s_pose_lost_at >= 1000) {
            if (s_ent) release_puppet("partner left world or timed out");
            s_spawn_tries = 0; s_next_try = 0;
        }
        return;
    }
    s_pose_lost_at = 0;
    if (s_ent && !pup_alive(s_ent)) {   /* v1127C: the game destroyed it (our own death reload): forget, never touch it again */
        uint32_t v = 0; read_u32(s_ent, &v); st_pup_gone++;
        printf("[coop-puppet] v1127C puppet %08X destroyed by the game (vtable %08X, was %08X) -> forgotten without any call, rebuild (%ld)\n", s_ent, v, s_ent_vt, st_pup_gone); fflush(stdout);
        {   static ULONGLONG s_gone_last; static int s_gone_streak;   /* v1127G1: the death reload destroys fresh puppets for ~10 s -> back off 1,2,4,8 s */
            s_gone_streak = (s_gone_last && now - s_gone_last < 15000) ? s_gone_streak + 1 : 1; s_gone_last = now;
            s_ent = 0; s_have_want = 0; s_late_ok = 0; s_spawn_tries = 0; s_next_try = now + (ULONGLONG)(1000u << (s_gone_streak > 4 ? 3 : s_gone_streak - 1)); }
    }
    if (s_ent && !entity_info(s_ent, &vt, &xf)) {
        printf("[coop-puppet] entity validation failed; forgetting %08X without dereferencing or release\n", s_ent); fflush(stdout);
        s_ent = 0; s_have_want = 0;
    }
    if (!want) { if (s_ent) release_puppet("not requested"); return; }
    if (!s_ent) {
        /* v1059C: the guest tried 3x while its own world was still loading (join log: tries before "first player
         * transform") and gave up for good. Spawn only once the LOCAL player has existed for 2 s, retry forever with
         * backoff (1, 2, 4, 8, then every 10 s), reset when the local player entity changes (area / save load). */
        static uint32_t s_ready_pl; static ULONGLONG s_ready_since;
        const uint32_t pl = player();
        if (pl != s_ready_pl) { s_ready_pl = pl; s_ready_since = now; s_spawn_tries = 0; s_next_try = 0; }
        if (!pl || now - s_ready_since < 2000) return;
        if ((!s_have_guid && !s_have_char && !s_live_attrib[0] && !s_live_attrib[1] && !s_live_attrib[2]) || now < s_next_try) return;
        if (!s_have_char && (s_live_attrib[0] || s_live_attrib[1] || s_live_attrib[2])) {   /* no saved GUID at all: live only */
            static const uint8_t zero[16]; memcpy(s_char_guid, zero, 16); s_have_char = 1; s_char_rank = 98; }
        spawn_puppet(m);
        if (!s_ent) { const int t = s_spawn_tries < 1 ? 1 : s_spawn_tries; s_next_try = now + (t >= 4 ? 10000u : (1000u << (t - 1))); }
        return;
    }
    if (s_mode == MODE_DRIVE) {
        {   /* v1063V (agent V): the puppet sits in the partner's car (gf_coop_vehicle.c seat_tick) -> the car carries it */
            extern int gf_veh_puppet_seated(uint32_t pup);
            if (gf_veh_puppet_seated(s_ent)) { s_late_ok = 0; s_stuck_frames = 0; return; }
        }
        s_last_drift = 0;
        drift_sample(s_ent);
        /* v1058F: after an area / mission load the entity stops taking transforms (q58r: stuck 110 m behind). Also covers
         * the partner teleporting. Stuck > 5 m for 90 frames -> release and respawn at the partner's pose. */
        if (s_last_drift > 1.5f) s_stuck_frames++; else s_stuck_frames = 0;   /* v1058G: 5 m missed a 3 m stall (q58t) */
        if (s_stuck_frames > 90) {
            printf("[coop-puppet] v1058F puppet stuck %.1f m from its target for %d frames -> respawn\n", (double)s_last_drift, s_stuck_frames); fflush(stdout);
            s_stuck_frames = 0; release_puppet("stuck after area change / teleport"); s_spawn_tries = 0; s_next_try = now + 500; return;
        }
        { extern int gf_coop_char_hold(void); if (gf_coop_char_hold()) { s_stuck_frames = 0; return; } }   /* v1068R2: world reloading after a hitch -> no placement (Havok abort) */
        drive_probe(s_ent, m);   /* v1059A */
        if (!apply_pose(s_ent, m)) { release_puppet("drive transform failed"); return; }
        memcpy(s_late_m, m, sizeof s_late_m); s_late_ok = 1;   /* v1059A */
        drift_log(now);
    }
}

/* v1058E glue: the live puppet entity (0 = none) */
static int pup_alive(uint32_t e)   /* v1127C: same class object still there (vtable unchanged, not the destroyed base) */
{ uint32_t v = 0; if (!e || !read_u32(e, &v) || v == 0x1005F6C8u || (s_ent_vt && e == s_ent && v != s_ent_vt)) return 0; return 1; }
uint32_t gf_coop_puppet_entity(void)   /* v1127C: never hand out a puppet the game has destroyed (death reload / area change) */
{
    const uint32_t e = s_ent; if (!e) return 0;
    if (!pup_alive(e)) return 0;
    return e;
}

/* ================================================================ v1072S (agent S): extra puppets for N-peer sessions (gf_coop_peers.c)
 * One more character per non-primary remote peer, built exactly like the primary (spawn_char: managed factory func_003388AC +
 * player model SetModel). spawn_char works on s_ent, so the primary's state (s_ent, tries, late set, wanted position) is saved
 * around the call and restored; the new entity is handed to the caller. Main PPU thread only. */
uint32_t gf_coop_puppet_spawn_extra(const float m[16])
{
    uint32_t keep = s_ent, keep_vt = s_ent_vt, ent; int keep_tries = s_spawn_tries, keep_late = s_late_ok, keep_hw = s_have_want; float keep_w[3];
    if ((s_mode != MODE_DRIVE && s_mode != MODE_SPAWN) || !g_active_ctx) return 0;
    char_resolve();
    if (!s_have_char && (s_live_attrib[0] || s_live_attrib[1] || s_live_attrib[2])) { static const uint8_t zero[16]; memcpy(s_char_guid, zero, 16); s_have_char = 1; s_char_rank = 98; }
    if (!s_have_char) return 0;
    memcpy(keep_w, s_want_pos, sizeof keep_w);
    s_ent = 0;
    spawn_char(m);
    ent = s_ent;
    s_ent = keep; s_ent_vt = keep_vt; s_spawn_tries = keep_tries; s_late_ok = keep_late; s_have_want = keep_hw; memcpy(s_want_pos, keep_w, sizeof keep_w);
    return ent;
}
int gf_coop_puppet_valid(uint32_t ent) { uint32_t vt, xf; return ent && entity_info(ent, &vt, &xf); }
int gf_coop_puppet_drive_extra(uint32_t ent, const float m[16])
{
    int keep_hw = s_have_want, r; float keep_w[3];
    memcpy(keep_w, s_want_pos, sizeof keep_w);
    r = apply_pose(ent, m);
    s_have_want = keep_hw; memcpy(s_want_pos, keep_w, sizeof keep_w);
    return r;
}
int gf_coop_puppet_late_extra(uint32_t ent, const float m[16])
{
    uint32_t vt, xf, buf = scratch(); int before = s_call_fail;
    if (!buf || !entity_info(ent, &vt, &xf) || !write_matrix(buf, xf, m)) return 0;
    return call_virtual(ent, 0x2Cu, buf) && s_call_fail == before;
}
void gf_coop_puppet_release_extra(uint32_t ent)
{
    if (!ent || ent == s_ent) return;
    printf("[coop-puppet] v1072S release extra puppet ent=%08X\n", ent); fflush(stdout);
    gcall(0x00455F58u, ent + 0x30u, 0, 0, 0, 0);
}
