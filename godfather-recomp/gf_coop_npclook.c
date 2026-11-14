/* gf_coop_npclook.c -- v1127L NPC look sync (agent L): every NPC looks the same on both screens.
 *
 * RE (v1124H + v1127L, lifted code):
 *   *(ent+0x6C) is the character's RenderWare RpClump (func_0046386C: +0xF8 = func_0001362C -> func_006C13BC = RpClumpCreate,
 *   writes type byte 2 = rpCLUMP at +0 -> the "header 02000000"; func_0046E0E0 stores it at ent+0x6C). The per-ped look the game
 *   rolls lives in a clump PLUGIN behind the 0x2C-byte RpClump: +0x34 / +0x38 / +0x3C = 3 tint colours (RGBA), +0x40..+0x50 =
 *   5 swap slots (RwTexture* -- name at tex+0x10, owning RwTexDictionary at tex+4, e.g. "swap_thugheadyng01_lod1" = head).
 *   Same model GUID on both peers = same texture dictionary, so every variant texture exists on both sides: we send the owner's
 *   tints + an FNV-1a hash of every swap texture name and the receiver finds the texture with that name in the RW texture
 *   dictionaries (tex+4 -> dict, dict+8 texturesInDict list, dict+0x10 lInInstance ring over all dictionaries).
 *   C's v1127C puppet look fix (copy +0x34..+0x50) showed the game does not rewrite the block once the ped is built; v1124H's
 *   "reverts" came from applying before the proxy finished its own appearance init -> we apply only after the proxy has its
 *   render instance + 1.5 s, verify, re-apply on change (counted as reverts), give up on an entity after 6 reverts.
 * Protocol: owner (any peer, its own world sources = gf_coop_world_host_peds) -> partner, reliable event 0x0150 (was 0x0140 = collided with EV_VEH_STATE) =
 *   [0] 'L' [1] ver 1 [2] n, then n x 40-byte records {u16 netid, u8 flags (bit0 tints valid), u8 natom, u32 tint[3],
 *   u32 swap[5] (0 = empty slot, 1 = non-texture object, else name hash), u32 atom render mask} (little endian).
 *   Sent when a ped's look changes / first seen, full resend every 20 s and after (re)connect.
 * Migration: a promoted proxy keeps the look applied to it; when we become the owner of a net id we still hold a received
 *   record for, it is applied once more before we start sending ours.
 * Env: GF_COOP_NPC_LOOK=0 off (default ON). GF_COOP_NPC_LOOK_LOG=1 more log lines.
 * Threads: gf_coop_npclook_tick() main PPU thread (from gf_coop_look_tick); on_event() co-op thread (copies under s_cs). */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern uint8_t* vm_base;
extern int gf_coop_connected(void);
typedef void (*gf_coop_event_fn)(uint16_t type, const void* data, int len);
extern void gf_coop_set_event_handler(gf_coop_event_fn fn);
extern int gf_coop_send_event(uint16_t type, const void* data, int len);
extern int gf_coop_world_host_peds(uint32_t* ents, uint16_t* ids, float (*pos)[3], int max);
extern int gf_coop_world_src_has(uint32_t ent);
extern uint32_t gf_proxy_any_ent_of(uint16_t netid);

enum { EV_NPCLOOK = 0x0150 /* v1127G1: 0x0140 = gf_coop_vehicle.c EV_VEH_STATE */, REC = 40, PER_EV = 24, NSLOT = 5, MAXOWN = 256 };

typedef struct {
    uint8_t flags, natom; uint32_t tint[3]; uint32_t swap[NSLOT]; uint32_t amask;
} look_t;
typedef struct {   /* receiver: one per net id */
    uint8_t have; look_t lk; uint32_t rx_ms_lo;
    uint32_t ent, own_ent; ULONGLONG seen_ms, next_ms; uint8_t matched, gave_up, reverts, tries;
} rx_t;
typedef struct {   /* owner: last state sent per net id */
    uint8_t sent; uint32_t ent; look_t lk;
} tx_t;

static int s_on = -1, s_verbose;
static CRITICAL_SECTION s_cs; static int s_cs_ok;
static rx_t* s_rx;   /* [65536] */
static tx_t* s_tx;   /* [65536] */
static uint16_t s_act[8192]; static volatile LONG s_nact;   /* net ids with a record (appended on the co-op thread) */
static ULONGLONG s_next_own, s_next_full, s_next_apply, s_next_census;
static int s_was_conn;
static uint32_t s_any_dict;
static unsigned st_tx_rec, st_tx_ev, st_tx_full, st_rx_rec, st_apply, st_apply_tint, st_apply_swap, st_unres, st_revert, st_giveup, st_mig_apply, st_opaque;

/* ------------------------------------------------------------------ memory */
static int ea_ok(uint32_t ea, uint32_t n) { return ea >= 0x10000u && ea < 0xF0000000u && n <= 0xF0000000u - ea; }
static int rd(uint32_t ea, uint32_t* out)
{
    if (!ea_ok(ea, 4)) return 0;
    __try { const uint8_t* p = vm_base + ea; *out = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int rd8(uint32_t ea, uint8_t* out) { if (!ea_ok(ea, 1)) return 0; __try { *out = vm_base[ea]; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static int wr(uint32_t ea, uint32_t v)
{
    if (!ea_ok(ea, 4)) return 0;
    __try { uint8_t* p = vm_base + ea; p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int heap_ptr(uint32_t v) { return v >= 0x30000000u && v < 0x50000000u && !(v & 3u); }
static uint32_t clump_of(uint32_t ent)   /* RpClump with our plugin, 0 = none */
{
    uint32_t c = 0; uint8_t t = 0;
    if (!ent || !rd(ent + 0x6Cu, &c) || !heap_ptr(c) || !rd8(c, &t) || t != 2) return 0;
    return c;
}
/* RwTexture name -> FNV-1a (0 = not a texture). Structural check: dict = tex+4 is a dictionary whose texture list is sane. */
static int tex_name(uint32_t tex, char nm[33])
{
    uint8_t b; int i;
    if (!heap_ptr(tex)) return 0;
    for (i = 0; i < 32; i++) { if (!rd8(tex + 0x10u + (uint32_t)i, &b)) return 0; nm[i] = (char)b; if (!b) break; if (b < 0x20 || b > 0x7E) return 0; }
    nm[i < 32 ? i : 32] = 0;
    return i >= 2;
}
static uint32_t fnv(const char* s) { uint32_t h = 2166136261u; for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; } return h > 1u ? h : 2u; }
static int dict_ok(uint32_t d)
{
    uint32_t l = 0, t = 0, back = 0;
    if (!heap_ptr(d) || !rd(d + 8u, &l)) return 0;
    if (l == d + 8u) return 1;   /* empty list */
    if (!heap_ptr(l)) return 0;
    t = l - 8u;
    return rd(t + 4u, &back) && back == d;
}
static uint32_t slot_key(uint32_t obj, uint32_t* dict_out)   /* 0 empty, 1 non-texture object, else name hash */
{
    char nm[33]; uint32_t d = 0;
    if (!obj) return 0;
    if (!tex_name(obj, nm) || !rd(obj + 4u, &d) || !dict_ok(d)) return 1;
    if (dict_out) *dict_out = d;
    return fnv(nm);
}
static uint32_t find_in_dict(uint32_t d, uint32_t want)
{
    uint32_t head = d + 8u, l = 0, back = 0; char nm[33];
    if (!rd(head, &l)) return 0;
    for (int n = 0; n < 6000 && l != head && heap_ptr(l); n++) {
        const uint32_t t = l - 8u;
        if (!rd(t + 4u, &back) || back != d) return 0;
        if (tex_name(t, nm) && fnv(nm) == want) return t;
        if (!rd(l, &l)) return 0;
    }
    return 0;
}
static uint32_t find_tex(uint32_t want, const uint32_t* hint, int nhint)   /* hint dicts first, then every dictionary on the ring */
{
    uint32_t t, start = 0;
    for (int i = 0; i < nhint; i++) if (hint[i] && (t = find_in_dict(hint[i], want)) != 0) return t;
    for (int i = 0; i < nhint && !start; i++) if (hint[i]) start = hint[i];
    if (!start && dict_ok(s_any_dict)) start = s_any_dict;
    if (!start) return 0;
    {   const uint32_t head = start + 0x10u; uint32_t l = 0;
        if (!rd(head, &l)) return 0;
        for (int n = 0; n < 3000 && l != head && heap_ptr(l); n++) {
            const uint32_t d = l - 0x10u;
            if (dict_ok(d) && (t = find_in_dict(d, want)) != 0) return t;
            if (!rd(l, &l)) return 0;
        }
    }
    return 0;
}
static int atoms(uint32_t c, uint32_t* mask)   /* diag: render flag (RwObject flags 0x04) of each atomic in the clump */
{
    uint32_t head = c + 8u, l = 0, back = 0; uint8_t ty = 0, fl = 0; int n = 0;
    *mask = 0;
    if (!rd(head, &l)) return 0;
    while (n < 32 && l != head && heap_ptr(l)) {
        const uint32_t a = l - 0x40u;
        if (!rd(l - 4u, &back) || back != c || !rd8(a, &ty) || ty != 1 || !rd8(a + 2u, &fl)) return n ? n : -1;
        if (fl & 4u) *mask |= 1u << n;
        n++;
        if (!rd(l, &l)) break;
    }
    return n;
}
static int capture(uint32_t ent, look_t* lk, uint32_t* dicts)
{
    const uint32_t c = clump_of(ent); uint32_t v = 0, a = 0;
    memset(lk, 0, sizeof *lk);
    if (!c) return 0;
    for (int i = 0; i < 3; i++) { if (!rd(c + 0x34u + 4u * (uint32_t)i, &lk->tint[i])) return 0; }
    lk->flags = (lk->tint[0] | lk->tint[1] | lk->tint[2]) ? 1 : 0;
    for (int i = 0; i < NSLOT; i++) {
        if (!rd(c + 0x40u + 4u * (uint32_t)i, &v)) return 0;
        lk->swap[i] = slot_key(v, dicts ? &dicts[i] : 0);
        if (dicts && dicts[i] && !s_any_dict) s_any_dict = dicts[i];
    }
    { const int n = atoms(c, &a); lk->natom = (uint8_t)(n < 0 ? 0xFF : n); lk->amask = a; }
    return 1;
}
static int same_look(const look_t* a, const look_t* b)   /* tints + texture slots (non-texture objects: presence only) */
{
    if (memcmp(a->tint, b->tint, sizeof a->tint)) return 0;
    for (int i = 0; i < NSLOT; i++) if (a->swap[i] != b->swap[i]) return 0;
    return 1;
}
static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void enc(uint8_t* o, uint16_t id, const look_t* lk)
{
    o[0] = (uint8_t)id; o[1] = (uint8_t)(id >> 8); o[2] = lk->flags; o[3] = lk->natom;
    for (int i = 0; i < 3; i++) put32(o + 4 + 4 * i, lk->tint[i]);
    for (int i = 0; i < NSLOT; i++) put32(o + 16 + 4 * i, lk->swap[i]);
    put32(o + 36, lk->amask);
}

/* ------------------------------------------------------------------ receiver */
static void on_event(uint16_t type, const void* data, int len)   /* co-op thread */
{
    const uint8_t* d = (const uint8_t*)data; int n;
    if (type != EV_NPCLOOK || s_on != 1 || !s_rx || len < 3 || d[0] != 'L' || d[1] != 1) return;
    n = d[2]; if (3 + n * REC > len) return;
    EnterCriticalSection(&s_cs);
    for (int k = 0; k < n; k++) {
        const uint8_t* r = d + 3 + k * REC; const uint16_t id = (uint16_t)(r[0] | (r[1] << 8)); look_t lk; rx_t* x;
        if (!id) continue;
        memset(&lk, 0, sizeof lk); lk.flags = r[2]; lk.natom = r[3];
        for (int i = 0; i < 3; i++) lk.tint[i] = get32(r + 4 + 4 * i);
        for (int i = 0; i < NSLOT; i++) lk.swap[i] = get32(r + 16 + 4 * i);
        lk.amask = get32(r + 36);
        x = &s_rx[id];
        if (!x->have && s_nact < (LONG)(sizeof s_act / sizeof s_act[0])) { s_act[s_nact] = id; InterlockedIncrement(&s_nact); }
        if (!x->have || memcmp(&x->lk, &lk, sizeof lk)) { x->lk = lk; x->have = 1; x->matched = 0; x->gave_up = 0; x->reverts = 0; x->tries = 0; x->next_ms = 0; }
        st_rx_rec++;
    }
    LeaveCriticalSection(&s_cs);
}
/* write the record onto ent; returns 1 = now matches, 0 = not (yet), -1 = cannot (no clump) */
static int apply_to(uint32_t ent, const look_t* want, int* changed)
{
    const uint32_t c = clump_of(ent); look_t have; uint32_t dicts[NSLOT]; int ok = 1;
    *changed = 0;
    if (!c) return -1;
    memset(dicts, 0, sizeof dicts);
    if (!capture(ent, &have, dicts)) return -1;
    if (same_look(&have, want)) return 1;
    if (want->flags & 1) for (int i = 0; i < 3; i++) if (have.tint[i] != want->tint[i]) { if (!wr(c + 0x34u + 4u * (uint32_t)i, want->tint[i])) return 0; *changed = 1; st_apply_tint++; }
    for (int i = 0; i < NSLOT; i++) {
        const uint32_t slot = c + 0x40u + 4u * (uint32_t)i;
        if (have.swap[i] == want->swap[i]) continue;
        if (want->swap[i] == 1u || have.swap[i] == 1u) { st_opaque++; ok = 0; continue; }   /* non-texture object: leave it */
        if (want->swap[i] == 0u) { if (wr(slot, 0)) { *changed = 1; st_apply_swap++; } continue; }
        {   const uint32_t t = find_tex(want->swap[i], dicts, NSLOT); uint32_t rc = 0;
            if (!t) { st_unres++; ok = 0; continue; }
            if (rd(t + 0x54u, &rc) && rc > 0 && rc < 100000u) wr(t + 0x54u, rc + 1u);   /* the plugin's dtor releases its textures */
            if (wr(slot, t)) { *changed = 1; st_apply_swap++; }
        }
    }
    if (*changed) st_apply++;
    return ok;
}
static void apply_tick(ULONGLONG now)
{
    const int na = (int)s_nact;
    for (int ai = 0; ai < na; ai++) {
        const uint32_t id = s_act[ai]; rx_t* x = &s_rx[id]; uint32_t ent; look_t want; int ch = 0, r;
        if (!x->have || x->gave_up || now < x->next_ms) continue;
        ent = gf_proxy_any_ent_of((uint16_t)id);
        if (!ent || gf_coop_world_src_has(ent)) {   /* no proxy (yet) or we own it now (migration): handled by own_tick */
            if (x->ent && !ent) { x->ent = 0; x->matched = 0; x->reverts = 0; }
            x->next_ms = now + 500; continue; }
        if (ent != x->ent) { x->ent = ent; x->seen_ms = now; x->matched = 0; x->reverts = 0; x->tries = 0; }
        if (now - x->seen_ms < 1500) { x->next_ms = x->seen_ms + 1500; continue; }   /* let the game finish its own appearance init */
        { uint32_t inst = 0; if (!rd(ent + 0x64u, &inst) || !inst) { x->next_ms = now + 500; continue; } }
        EnterCriticalSection(&s_cs); want = x->lk; LeaveCriticalSection(&s_cs);
        r = apply_to(ent, &want, &ch);
        if (r < 0) { x->next_ms = now + 1000; continue; }
        if (ch && x->matched) { x->reverts++; st_revert++;
            if (x->reverts > 6) { x->gave_up = 1; st_giveup++; printf("[npclook] v1127L net %u ent %08X: the game keeps changing its look (%u re-applies) -> giving up on it\n", id, ent, x->reverts); fflush(stdout); continue; } }
        if (ch) {
            static int nlog; if (nlog < 16 || s_verbose) { nlog++; look_t now_lk; capture(ent, &now_lk, 0);
                printf("[npclook] v1127L net %u ent %08X: applied owner look tints %08X %08X %08X swaps %08X %08X %08X %08X %08X -> %s\n", id, ent,
                       want.tint[0], want.tint[1], want.tint[2], want.swap[0], want.swap[1], want.swap[2], want.swap[3], want.swap[4], same_look(&now_lk, &want) ? "MATCH" : "partial"); fflush(stdout); }
        }
        if (r == 1) x->matched = 1;
        if (r == 0 && !ch && ++x->tries > 20) { x->gave_up = 1; st_giveup++; continue; }   /* unresolvable (texture not here / opaque slot) */
        x->next_ms = now + (x->matched ? 1000 : 400);
    }
}

/* ------------------------------------------------------------------ owner */
static void own_tick(int full)
{
    uint32_t ents[MAXOWN]; uint16_t ids[MAXOWN]; float pos[MAXOWN][3]; int n = 0, m = 0;
    uint8_t buf[3 + PER_EV * REC];
    __try { n = gf_coop_world_host_peds(ents, ids, pos, MAXOWN); } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
    for (int i = 0; i < n; i++) {
        const uint16_t id = ids[i]; look_t lk; tx_t* t = &s_tx[id]; rx_t* x = &s_rx[id];
        if (!id || !ents[i]) continue;
        if (x->have && x->own_ent != ents[i]) {   /* migration: we became the owner of a ped we had a look record for */
            look_t want; int ch = 0, r; uint32_t inst = 0;
            if (rd(ents[i] + 0x64u, &inst) && inst) {
                EnterCriticalSection(&s_cs); want = x->lk; LeaveCriticalSection(&s_cs);
                r = apply_to(ents[i], &want, &ch);
                if (ch) { st_mig_apply++; static int nl; if (nl++ < 8) { printf("[npclook] v1127L net %u ent %08X: now OURS (migration) -> kept the partner's look (%s)\n", id, ents[i], r == 1 ? "match" : "partial"); fflush(stdout); } }
                x->own_ent = ents[i];   /* done for this entity */
            }
        }
        if (!capture(ents[i], &lk, 0) || !(lk.flags & 1)) continue;   /* look not built yet */
        if (!full && t->sent && t->ent == ents[i] && same_look(&t->lk, &lk)) continue;
        {   static int nlog; if (nlog < 12 || s_verbose) { nlog++; char nm[5][33]; const uint32_t c = clump_of(ents[i]);
                for (int s = 0; s < NSLOT; s++) { uint32_t o = 0; nm[s][0] = 0; if (rd(c + 0x40u + 4u * (uint32_t)s, &o) && o) { if (!tex_name(o, nm[s])) snprintf(nm[s], sizeof nm[s], "obj %08X", o); } }
                printf("[npclook] v1127L owner net %u ent %08X: tints %08X %08X %08X | slots '%s' '%s' '%s' '%s' '%s' | atomics %d mask %08X\n", id, ents[i],
                       lk.tint[0], lk.tint[1], lk.tint[2], nm[0], nm[1], nm[2], nm[3], nm[4], lk.natom == 0xFF ? -1 : lk.natom, lk.amask); fflush(stdout); } }
        enc(buf + 3 + m * REC, id, &lk); m++;
        t->sent = 1; t->ent = ents[i]; t->lk = lk; st_tx_rec++;
        if (m == PER_EV) { buf[0] = 'L'; buf[1] = 1; buf[2] = (uint8_t)m; if (gf_coop_send_event(EV_NPCLOOK, buf, 3 + m * REC)) st_tx_ev++; else { for (int k = 0; k < m; k++) { const uint16_t j = (uint16_t)(buf[3 + k * REC] | (buf[4 + k * REC] << 8)); s_tx[j].sent = 0; } } m = 0; }
    }
    if (m) { buf[0] = 'L'; buf[1] = 1; buf[2] = (uint8_t)m; if (gf_coop_send_event(EV_NPCLOOK, buf, 3 + m * REC)) st_tx_ev++; else { for (int k = 0; k < m; k++) { const uint16_t j = (uint16_t)(buf[3 + k * REC] | (buf[4 + k * REC] << 8)); s_tx[j].sent = 0; } } }
    if (full) st_tx_full++;
}
static void census(void)
{
    unsigned recs = 0, live = 0, match = 0, differ = 0, tint_only = 0, gave = 0, amask_eq = 0, amask_ne = 0;
    const int na = (int)s_nact;
    for (int ai = 0; ai < na; ai++) {
        const uint32_t id = s_act[ai]; rx_t* x = &s_rx[id]; uint32_t ent; look_t have, want;
        if (!x->have) continue;
        recs++;
        ent = gf_proxy_any_ent_of((uint16_t)id);
        if (!ent || gf_coop_world_src_has(ent) || !capture(ent, &have, 0)) continue;
        live++; if (x->gave_up) gave++;
        EnterCriticalSection(&s_cs); want = x->lk; LeaveCriticalSection(&s_cs);
        if (same_look(&have, &want)) match++; else { differ++; if (!memcmp(have.tint, want.tint, sizeof have.tint)) tint_only++;
            static int nd; if (nd < 10 || s_verbose) { nd++; printf("[npclook] v1127L differ net %u ent %08X: have %08X %08X %08X / %08X %08X %08X %08X %08X want %08X %08X %08X / %08X %08X %08X %08X %08X\n", id, ent,
                have.tint[0], have.tint[1], have.tint[2], have.swap[0], have.swap[1], have.swap[2], have.swap[3], have.swap[4], want.tint[0], want.tint[1], want.tint[2], want.swap[0], want.swap[1], want.swap[2], want.swap[3], want.swap[4]); fflush(stdout); } }
        if (have.natom == want.natom && have.amask == want.amask) amask_eq++; else amask_ne++;
    }
    printf("[npclook] v1127L census: look match %u / differ %u (tints equal but slots differ %u, gave up %u) of %u live proxies with a record (%u records) | atomics mask equal %u / differ %u"
           " | tx rec %u ev %u full %u, rx rec %u | applies %u (tint words %u, swaps %u), unresolved swap %u, opaque slot %u, reverts %u, give-ups %u, migration keeps %u\n",
           match, differ, tint_only, gave, live, recs, amask_eq, amask_ne, st_tx_rec, st_tx_ev, st_tx_full, st_rx_rec, st_apply, st_apply_tint, st_apply_swap, st_unres, st_opaque, st_revert, st_giveup, st_mig_apply);
    fflush(stdout);
}

/* ------------------------------------------------------------------ entry points */
void gf_coop_npclook_init(void)
{
    const char* e = getenv("GF_COOP_NPC_LOOK");
    s_on = !(e && *e == '0');
    s_verbose = (e = getenv("GF_COOP_NPC_LOOK_LOG")) && *e == '1';
    if (!s_on) { printf("[npclook] v1127L NPC look sync OFF (GF_COOP_NPC_LOOK=0)\n"); fflush(stdout); return; }
    s_rx = (rx_t*)calloc(65536, sizeof *s_rx); s_tx = (tx_t*)calloc(65536, sizeof *s_tx);
    if (!s_rx || !s_tx) { s_on = 0; return; }
    InitializeCriticalSection(&s_cs); s_cs_ok = 1;
    gf_coop_set_event_handler(on_event);
    printf("[npclook] v1127L NPC look sync ON: owner sends tints + swap textures of the ped clump plugin (event 0x%04X), partner applies to its proxy\n", EV_NPCLOOK); fflush(stdout);
}
void gf_coop_npclook_tick(void)   /* main PPU thread, every frame */
{
    ULONGLONG now; int conn;
    if (s_on != 1 || !s_cs_ok) return;
    now = GetTickCount64(); conn = gf_coop_connected();
    if (!conn) { if (s_was_conn) { s_was_conn = 0; memset(s_tx, 0, 65536 * sizeof *s_tx); } return; }
    if (!s_was_conn) { s_was_conn = 1; s_next_full = now + 5000; }
    if (now >= s_next_own) { const int full = now >= s_next_full; s_next_own = now + 1000; if (full) s_next_full = now + 20000;
        __try { own_tick(full); } __except (EXCEPTION_EXECUTE_HANDLER) { } }
    if (now >= s_next_apply) { s_next_apply = now + 100; __try { apply_tick(now); } __except (EXCEPTION_EXECUTE_HANDLER) { } }
    if (now >= s_next_census) { s_next_census = now + 10000; __try { census(); } __except (EXCEPTION_EXECUTE_HANDLER) { } }
}
