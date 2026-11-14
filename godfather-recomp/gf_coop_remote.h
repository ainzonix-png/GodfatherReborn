/* v1072W (agent W, 2026-11-06): remote players for the world sync, N-peer ready.
 * gf_world_remotes() = every connected remote player (position, body facing, camera forward), cached 20 ms, any thread.
 * Source: agent S's N-peer API (gf_coop_peer_count / _pose / _cam in gf_coop.c) when it is linked and active (count >= 0),
 * else the single 2-player partner (gf_coop_partner_pose / gf_coop_partner_cam). Echo: the partner = the echo twin. */
#ifndef GF_COOP_REMOTE_H
#define GF_COOP_REMOTE_H
#include <stdint.h>
#define GF_REMOTE_MAX 16
typedef struct { float p[3]; float bf[2]; float fwd[2]; int cam; uint16_t peer; } gf_remote;   /* bf = body facing (x,z), fwd = camera (x,z) if cam */
int gf_world_remotes(gf_remote* out, int max);
#endif
