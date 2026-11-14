/* gf_coop_world_stub_proxy.c -- v1060W2 link stub, used by CMake ONLY while gf_coop_proxy.c (agent W3) is missing. */
#include "gf_coop_world.h"
int      gf_world_is_proxy(uint32_t ent) { (void)ent; return 0; }
uint32_t gf_proxy_spawn(uint16_t netid, uint8_t kind, uint32_t class_hash, const uint8_t guid[16], const float m[12]) { (void)netid; (void)kind; (void)class_hash; (void)guid; (void)m; return 0; }
void     gf_proxy_drive(uint16_t netid, const float m[12], const float vel[3]) { (void)netid; (void)m; (void)vel; }
void     gf_proxy_release(uint16_t netid) { (void)netid; }
void     gf_proxy_release_all(void) { }
void     gf_proxy_tick(void) { }
