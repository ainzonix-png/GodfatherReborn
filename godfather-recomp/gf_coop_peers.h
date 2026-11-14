/* gf_coop_peers.h -- v1072S (agent S): N-peer co-op API (implementation: gf_coop.c session layer, gf_coop_peers.c puppet pool).
 * Star topology: the host (peer id 0) talks to every joiner; a joiner talks only to the host and sees the other joiners as
 * RELAYED peers (state + player pose forwarded by the host). Peer ids: host 0, joiners 1..GF_COOP_MAX_PEERS-1 (assigned by the
 * host in WELCOME; an older host never assigns one -> self id stays -1 on the joiner, treat as 1).
 * GF_COOP_MAXPEERS (host, default 2) = total players incl. the host. 2 = the classic two-player session, nothing changes.
 * Legacy single-partner calls (gf_coop_partner_pose / _cam / _name / _identity, gf_coop_set_event_handler / _blob_handler)
 * keep working and mean the PRIMARY peer: on the host the first connected joiner, on a joiner the host. Legacy SENDS
 * (gf_coop_send_event / _blob) go from the host to EVERY joiner and from a joiner to the host.
 * Index-based calls take i in [0, gf_coop_peer_count()), ascending peer id; *_id calls take the peer id itself.
 * All calls are thread-safe (one critical section); handlers run on the co-op thread. */
#ifndef GF_COOP_PEERS_H
#define GF_COOP_PEERS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { GF_COOP_MAX_PEERS = 16 };
typedef void (*gf_coop_peer_event_fn)(int peer_id, uint16_t type, const void* data, int len);
typedef void (*gf_coop_peer_blob_fn)(int peer_id, uint8_t channel, const void* data, int len);

int  gf_coop_maxpeers(void);            /* configured total players (2 = classic) */
int  gf_coop_self_id(void);             /* 0 host, 1.. joiner, -1 not assigned (old host) */
int  gf_coop_npeer_active(void);        /* 1 = N-peer session (host GF_COOP_MAXPEERS > 2, or our host announced > 2, or echo fake peers) */
int  gf_coop_peer_count(void);          /* remote peers we know right now (direct + relayed, fresh < 3 s); -1 = NOT active (classic 2-player: use the partner API) */
int  gf_coop_peer_id(int i);            /* peer id of entry i, -1 = none */
int  gf_coop_peer_pose(int i, float out_m[16], uint16_t* peer_id);   /* interpolated player matrix (same layout as gf_coop_partner_pose) + its peer id (W contract) */
int  gf_coop_peer_pose_id(int id, float out_m[16], float* hp);
int  gf_coop_peer_cam(int i, float* fx, float* fz);          /* camera forward x/z, 1 = fresh (< 1 s) */
int  gf_coop_peer_cam_id(int id, float* fx, float* fz);
int  gf_coop_peer_name_id(int id, char out[24]);
int  gf_coop_peer_is_primary_id(int id);  /* 1 = the peer the legacy API talks about */
int  gf_coop_peer_direct_id(int id);      /* 1 = we have a session with it (host: every joiner; joiner: only the host) */
/* latest two player-pose packets (GF_COOP_POSE_BLOB_CHANNEL) of a peer: a = older, b = newer, ms on the sender clock.
 * Returns how many are valid (0..2); age_ms = since b arrived. Buffers >= 1100 bytes (GF_COOP_POSE_MAX_PACKET). */
int  gf_coop_peer_pose_packets(int id, uint8_t* a, int* la, uint8_t* b, int* lb, uint32_t* ma, uint32_t* mb, unsigned long long* age_ms);
/* per-peer sends: only to a DIRECT peer (host -> any joiner, joiner -> host = id 0). Events: type >= 0x100. */
int  gf_coop_send_event_to(uint16_t peer_id, uint16_t type, const void* data, int len);
int  gf_coop_send_blob_to(uint16_t peer_id, uint8_t channel, const void* data, int len);
/* generic relay to EVERY other peer (joiner -> host -> other joiners; host -> all joiners), origin = our id, delivered only to all-peer
 * handlers. Events type >= 0x100, len <= 997, reliable per hop. Blobs: channel 0x80..0xFF, latest-wins per (channel & 15), len <= 1150. */
int  gf_coop_send_event_relay(uint16_t type, const void* data, int len);
int  gf_coop_send_blob_relay(uint8_t channel, const void* data, int len);
/* all-peer receive handlers (every direct peer's events / blobs, with its id); up to 8 each */
void gf_coop_add_peer_event_handler(gf_coop_peer_event_fn fn);
void gf_coop_add_peer_blob_handler(gf_coop_peer_blob_fn fn);
int  gf_coop_cur_peer(void);            /* inside any handler: origin peer id (legacy handlers too), else -1 */

/* gf_coop_peers.c: puppet pool (one puppet per remote peer that is not the primary; the primary keeps gf_coop_puppet.c) */
void gf_coop_peers_tick(void);          /* main PPU thread, gf_coop_glue_tick */
void gf_coop_peers_after_anim(uint32_t ent);
uint32_t gf_coop_peer_puppet(int peer_id);
int  gf_coop_is_puppet(uint32_t ent);   /* primary or extra puppet (world source / suppression must skip them) */   /* entity of that peer's puppet (primary: gf_coop_puppet_entity()), 0 = none */
#ifdef __cplusplus
}
#endif
#endif
