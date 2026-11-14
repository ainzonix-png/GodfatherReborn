/* gf_coop_glue.c -- v1058E: wires the co-op modules together (Claude).
 *   gf_coop.c         session / network (co-op thread)
 *   gf_coop_probe.c   read-only runtime probes (GF_COOP_*_PROBE)
 *   gf_coop_puppet.c  partner puppet entity (GF_COOP_PUPPET=trace|spawn|drive)
 *   gf_coop_pose.c    bone-pose stream (GF_COOP_POSE=1)
 * Threads: gf_coop_glue_tick() runs on the main PPU thread once per frame (g_pad_title_tick_fn chain in gf_trainer.c);
 * gf_coop_after_anim() runs on the main PPU thread right after an entity's animation update (hand edit in
 * ppu_recomp_002.cpp func_0046316C); the blob handler runs on the co-op thread and only copies. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "gf_coop_pose.h"

extern uint8_t* vm_base;
extern void gf_coop_probe_init(void);  extern void gf_coop_probe_tick(void);
extern void gf_coop_puppet_init(void); extern void gf_coop_puppet_tick(void); extern uint32_t gf_coop_puppet_entity(void);
extern int gf_coop_connected(void);
typedef void (*gf_coop_blob_fn)(uint8_t channel, const void* data, int len);
extern void gf_coop_set_blob_handler(gf_coop_blob_fn fn);
extern int gf_coop_send_blob(uint8_t channel, const void* data, int len);
extern void gf_npcfix_on_blob(uint8_t channel, const void* data, int len);   /* v1067X */

static int s_pose_on = 0, s_pose_hz = 30;   /* v1059G: 30 Hz (was 20; GetTickCount gating made it ~15) */
static ULONGLONG s_pose_next = 0;
static uint32_t s_pose_target = 0;
static unsigned s_pose_sent = 0, s_pose_fail = 0;

static uint32_t rd32(uint32_t ea) { const uint8_t* p = vm_base + ea; return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static int ok(uint32_t ea) { return ea >= 0x10000u && ea < 0xF0000000u; }
static uint32_t player(void)
{
    __try { uint32_t a = rd32(0x10181290u); if (!ok(a)) return 0; a = rd32(a + 4); if (!ok(a)) return 0; a = rd32(a);
            if (!ok(a) || a < 0x17FCu) return 0; a -= 0x17FCu; return ok(a) ? a : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void on_blob(uint8_t ch, const void* data, int len)
{
    if (ch == GF_COOP_POSE_BLOB_CHANNEL) gf_coop_pose_on_received((const uint8_t*)data, len);
    else if ((ch >= 12 && ch < 16) || (ch >= 18 && ch < 22)) { extern void gf_wpose_on_blob(uint8_t c, const void* d, int l); gf_wpose_on_blob(ch, data, len); }   /* v1061F world NPC poses; v1066N (agent N) + ch 18..21 (blob queue slots 2..5) */
    else if ((ch >= 8 && ch < 12) || ch == 6) {   /* v1063H: ch 6 = guest -> host claimed-entity snapshots */ extern void gf_coop_world_on_blob(uint8_t c, const void* d, int l); gf_coop_world_on_blob(ch, data, len); }   /* v1060W2 world snapshots */
    else if (ch == 7) gf_npcfix_on_blob(ch, data, len);   /* v1067X joiner-owned grab pose; queue slot 7 */
}
void gf_coop_mission_tick_s0(void) { }   /* v1072S weak default (agent M's gf_coop_mission.c overrides) */
#pragma comment(linker, "/alternatename:gf_coop_mission_tick=gf_coop_mission_tick_s0")
void gf_coop_glue_init(void)
{
    const char* e = getenv("GF_COOP_POSE"); s_pose_on = e && atoi(e) == 1;
    if ((e = getenv("GF_COOP_POSE_HZ")) && atoi(e) > 0) s_pose_hz = atoi(e) > 60 ? 60 : atoi(e);
    gf_coop_probe_init();
    gf_coop_puppet_init();
    gf_coop_set_blob_handler(on_blob);
    { extern void gf_coop_look_init(void); gf_coop_look_init(); }   /* v1059L partner MobFace look */
    { extern void gf_coop_world_init(void); gf_coop_world_init(); }   /* v1060W2 peds + traffic sync */
    if (s_pose_on) { const int st = gf_coop_pose_selftest(); printf("[coop-glue] v1058E pose stream ON (%d Hz), codec selftest %s\n", s_pose_hz, st ? "PASS" : "FAIL"); fflush(stdout); }
}
void gf_coop_glue_tick(void)   /* main PPU thread */
{
    { extern void gf_coop_frame_begin(void); __try { gf_coop_frame_begin(); } __except (EXCEPTION_EXECUTE_HANDLER) { } }   /* v1068R2 hitch -> character hold (gf_coop_proxy.c) */
    __try { gf_coop_probe_tick(); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    __try { gf_coop_puppet_tick(); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    { extern void gf_coop_peers_tick(void); __try { gf_coop_peers_tick(); } __except (EXCEPTION_EXECUTE_HANDLER) { } }   /* v1072S extra puppets (N peers) */
    { extern void gf_coop_mission_tick(void); __try { gf_coop_mission_tick(); } __except (EXCEPTION_EXECUTE_HANDLER) { } }   /* v1072S for agent M (weak default below until gf_coop_mission.c defines it) */
    { extern void gf_coop_look_tick(void); __try { gf_coop_look_tick(); } __except (EXCEPTION_EXECUTE_HANDLER) { } }   /* v1059L */
    { extern void gf_coop_world_tick(void); __try { gf_coop_world_tick(); } __except (EXCEPTION_EXECUTE_HANDLER) { } }   /* v1060W2 (before W3's gf_proxy_tick) */
    { extern void gf_proxy_tick(void); __try { gf_proxy_tick(); } __except (EXCEPTION_EXECUTE_HANDLER) { } }   /* v1060W3 world proxies (W2 must not call it) */
    { extern void gf_wpose_tick(void); __try { gf_wpose_tick(); } __except (EXCEPTION_EXECUTE_HANDLER) { } }   /* v1061F */
    const uint32_t ent = gf_coop_puppet_entity();
    if (ent != s_pose_target) { s_pose_target = ent; gf_coop_pose_set_target(ent); printf("[coop-glue] pose target -> %08X\n", ent); fflush(stdout); }
    static ULONGLONG last = 0; const ULONGLONG now = GetTickCount64();
    if (s_pose_on && now - last >= 10000) { last = now;
        extern volatile long g_coop_pose_ok, g_coop_pose_badkey, g_coop_pose_norig; extern volatile uint32_t g_coop_pose_lkey, g_coop_pose_wkey; extern volatile int g_coop_pose_lslots, g_coop_pose_wslots;
        { extern volatile long g_coop_pose_rx, g_coop_pose_hook, g_coop_pose_hook_match; extern volatile uint32_t g_coop_pose_last_anim, g_coop_pose_tgt_anim;
          const uint32_t pl = player(); uint32_t plan = 0; __try { if (pl) plan = rd32(pl + 0xC4); } __except (EXCEPTION_EXECUTE_HANDLER) { }
          printf("[coop-glue] pose rx %ld | post-skel hook calls %ld, matched puppet %ld | last hook anim %08X, puppet +C4 %08X, player +C4 %08X\n",
                 g_coop_pose_rx, g_coop_pose_hook, g_coop_pose_hook_match, g_coop_pose_last_anim, g_coop_pose_tgt_anim, plan); }
        printf("[coop-glue] pose packets sent %u (capture failures %u) | applied to puppet %ld, rig mismatch %ld (local key %08X/%d slots, wire %08X/%d), no rig %ld\n",
               s_pose_sent, s_pose_fail, g_coop_pose_ok, g_coop_pose_badkey, g_coop_pose_lkey, g_coop_pose_lslots, g_coop_pose_wkey, g_coop_pose_wslots, g_coop_pose_norig);
        { extern volatile long g_pose_enc_degen, g_pose_enc_clamp, g_pose_cap_norig, g_pose_cap_span, g_pose_cap_enc;   /* v1119F */
          printf("[coop-glue] v1119F capture failure reasons (player + NPC captures): no rig %ld, palette refused %ld, encode %ld | degenerate bones sent as identity %ld, positions clamped %ld\n",
                 g_pose_cap_norig, g_pose_cap_span, g_pose_cap_enc, g_pose_enc_degen, g_pose_enc_clamp); }
        { extern volatile long g_coop_pose_interp, g_coop_pose_hold; printf("[coop-glue] v1059G pose playback: interpolated %ld, held %ld (send %d Hz)\n", g_coop_pose_interp, g_coop_pose_hold, s_pose_hz); }
        { extern volatile float g_coop_pose_scale[3]; printf("[coop-glue] v1059B puppet mesh scale kept: slot0 %.3f slot1 %.3f slot2 %.3f\n", g_coop_pose_scale[0], g_coop_pose_scale[1], g_coop_pose_scale[2]); }
        fflush(stdout); }
}
void gf_coop_after_anim(uint32_t ent)   /* main PPU thread, right after func_0046316C(ent, dt) */
{
    { extern void gf_coop_puppet_late(uint32_t e); __try { gf_coop_puppet_late(ent); } __except (EXCEPTION_EXECUTE_HANDLER) { } }   /* v1059A (agent A) */
    { extern void gf_coop_peers_after_anim(uint32_t e); __try { gf_coop_peers_after_anim(ent); } __except (EXCEPTION_EXECUTE_HANDLER) { } }   /* v1072S extra puppets: late set + pose */
    { extern void gf_proxy_late(uint32_t e); __try { gf_proxy_late(ent); } __except (EXCEPTION_EXECUTE_HANDLER) { } }   /* v1060W3 */
    { extern void gf_wpose_after_anim(uint32_t e); __try { gf_wpose_after_anim(ent); } __except (EXCEPTION_EXECUTE_HANDLER) { } }   /* v1061F host NPC pose capture */
    if (!s_pose_on || !gf_coop_connected()) return;
    extern uint32_t gf_coop_pose_now_ms(void);
    const ULONGLONG now = gf_coop_pose_now_ms(); if (now < s_pose_next && s_pose_next - now < 1000) return;   /* v1059G 1 ms clock */
    if (ent != player()) return;
    { const ULONGLONG per = (ULONGLONG)(1000 / s_pose_hz);
      s_pose_next = (s_pose_next && now - s_pose_next < per) ? s_pose_next + per : now + per; }   /* fixed schedule, no drift */
    uint8_t pkt[GF_COOP_POSE_MAX_PACKET];
    int n = 0; __try { n = gf_coop_pose_capture(ent, pkt, sizeof pkt); } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
    if (n > 0 && gf_coop_send_blob(GF_COOP_POSE_BLOB_CHANNEL, pkt, n)) s_pose_sent++; else s_pose_fail++;
}
