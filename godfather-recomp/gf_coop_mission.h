/* gf_coop_mission.h -- v1072M (agent M): co-op mission sync (host authoritative, 2-player).
 * Hooks (apply-pending blocks v1072M / v1072M2, ppu_recomp_002.cpp): gf_mission_on_register(handle, name) at the top of the
 * named-event registrar func_00455324; at the top of the event dispatcher func_00455878:
 *   if (gf_mission_on_dispatch2(msg, r4)) return;   (1 = joiner lockstep dropped its own script event) */
#ifndef GF_COOP_MISSION_H
#define GF_COOP_MISSION_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { GF_COOP_EV_MISSION_STATE = 0x0180, GF_COOP_EV_MISSION_EVENT = 0x0181, GF_COOP_EV_MISSION_OBJ = 0x0182,
       GF_COOP_EV_MISSION_UP = 0x0183, GF_COOP_EV_MISSION_HUD = 0x0184 };   /* 0x0180..0x018F reserved for M */
void gf_mission_on_register(uint64_t handle, uint64_t name);
void gf_mission_on_dispatch(uint64_t msg, uint64_t arg);
int  gf_mission_on_dispatch2(uint64_t msg, uint64_t arg);
void gf_coop_mission_tick(void);   /* main PPU thread (gf_coop_glue tick, v1072S) */
uint32_t gf_mission_hash(const char* s);   /* the game's event-name hash (func_003FE410: sdbm, A-Z folded) */
const char* gf_mission_name_of(uint32_t hash);   /* NULL = unknown */
#ifdef __cplusplus
}
#endif
#endif
