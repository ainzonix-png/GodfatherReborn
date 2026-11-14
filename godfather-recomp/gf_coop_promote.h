/* gf_coop_promote.h -- v1126P (agent P): turn a live world PROXY into a real AI-driven NATIVE in place (same entity).
 * RE (v1126P, lifted code; details in _tmp/agents/P.md):
 *   Update manager mgr = *(0x101D9884); per-frame update lists = *(mgr+4) + idx*16 {data, count, cap, holes}
 *   (func_0034CCDC: list 2 func_0047117C, 3 anim func_0046346C/func_00463D58, 4 physics->frame func_0024AE24/func_0024CE78,
 *   5 character LOD func_00140238, 6/7/8 BRAIN func_00241614). Add = func_0034BD5C(mgr, idx, ent) (idempotent), remove =
 *   func_0034B58C(mgr, idx, ent). Character vtable +0x1C = "register for updates" (base func_0012D28C: lists 2..5, then the class'
 *   brain list: pedestrian 1CC0 class func_001438B0 -> 6, combat 1F80 class func_0012A84C -> 7, A37F4AB4 1F90 class func_0012AF68 -> 8),
 *   +0x20 = unregister. The game calls vt+0x1C only from the character's iMsgManagedEntityCreated handler (func_00140FE4 @0x0014153C),
 *   which the managed-entity manager delivers for its own spawns; our factory proxies (owner = requester = 0, model resident)
 *   never get it -> no update lists -> no brain. The brain starts its HSM (ent+0x108) by itself (vt+0x288) on its first tick.
 * All functions: main PPU thread (g_active_ctx valid), __try-guarded, never throw. */
#ifndef GF_COOP_PROMOTE_H
#define GF_COOP_PROMOTE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Promote: 1 = ent is now a native with a running brain (in its brain update list), 0 = failed (entity untouched).
 * mat = world rows right/up/at/pos (12 floats, as gf_world_ent.m) to place it at first, NULL = leave where it is.
 * health > 0 is written to ent+0x14C8 (float), <= 0 = keep. Characters only (cars: 0). The CALLER must stop the proxy
 * drive for it (gf_proxy_detach(netid) right after a 1) -- gf_coop_world.c mig_promote does. GF_PROMOTE=0 -> always 0.
 * GF_PROMOTE_MODE=lists (default: vt+0x1C only) | created (send iMsgManagedEntityCreated = the game's full spawn-complete init). */
int gf_proxy_promote(uint32_t ent, const float* mat /* may be NULL */, float health);
/* 1 = ent is a live character native (in a brain list, alive bit set, health > 0) that can be demoted to a proxy. */
int gf_native_demote_ok(uint32_t ent);
/* bit mask of the update lists (bit i = list i, 2..8) that hold ent; 0 = in none (a factory proxy); -1 = cannot read. */
int gf_native_update_lists(uint32_t ent);
/* 1 = ent is in a brain list (6 / 7 / 8). */
int gf_native_has_brain(uint32_t ent);
/* EXPERIMENTAL (for demotion): take ent out of its brain list only (anim / physics / LOD lists stay) -> its AI stops, the pose
 * stream owns it. 1 = removed (or was not in one). Undo = gf_proxy_promote. */
int gf_native_brain_off(uint32_t ent);
#ifdef __cplusplus
}
#endif
#endif
