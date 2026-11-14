#ifndef PS3RECOMP_RSX_COOP_POSE_H
#define PS3RECOMP_RSX_COOP_POSE_H

/* Live networked Cole pose for the inFamous co-op peer (v2.2).
 *
 * The game culls Cole's triangles against its own camera before the RSX sees
 * them, so neither the vertex window nor the index list of a frame is the
 * whole mesh, and the two instances see different subsets.  Everything here
 * is therefore keyed by stable identities:
 *   part key    = Cole mesh part (colour/UV stream addresses)
 *   guest index = vertex inside that part's vertex arrays
 *   submesh id  = part key + material (one local draw's PSO/textures)
 *
 * Sender: a few times per second the backend reads each local part's whole
 * known guest-index range straight from guest memory (positions, normals,
 * tangents), and this module quantizes and ships it.  Both instances also
 * accumulate every triangle they have ever seen per submesh and exchange
 * those sets, so the union converges to the complete mesh.
 *
 * Receiver: a dense vertex window per part (interpolated peer pose) and an
 * index list per submesh containing only triangles whose three vertices have
 * a fresh sample.  Static slots (UVs, colours, tangent sign) come from guest
 * memory of the local copy of the same part.
 *
 * Thread model: send_*, note_*, template_*, render_*, write_* run on the RSX
 * FIFO thread; receive / drain_outgoing / reset on the game thread. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_COOP_POSE_BLOB_MAX  1260u
#define RSX_COOP_POSE_MAX_SPAN  65535u

/* -------- RSX thread: sender -------- */
int  rsx_coop_pose_send_begin(const float anchor[3]);   /* 1 = a pose frame is due */
int  rsx_coop_pose_sending(void);                         /* inside begin..end */
/* Dense guest range [gmin, gmin+count): float4 slots, slot 0 = world position,
 * slot 2 = normal, tangent_slot (0 = none). */
void rsx_coop_pose_send_part(uint64_t key, uint32_t gmin, uint32_t count,
                             const uint8_t* vertices, uint32_t stride, uint32_t tangent_slot);
void rsx_coop_pose_send_end(void);

/* -------- RSX thread: mesh knowledge (both roles) -------- */
/* Triangle list in guest indices (n multiple of 3) drawn this frame. */
void rsx_coop_pose_note_triangles(uint64_t submesh, uint64_t key, const uint32_t* list, uint32_t n);
/* Union of the guest indices known for a part (local and remote). */
int  rsx_coop_pose_part_range(uint64_t key, uint32_t* gmin, uint32_t* count);
int  rsx_coop_pose_template_needed(uint64_t key, uint32_t gmin, uint32_t count);
void rsx_coop_pose_template_store(uint64_t key, uint32_t gmin, uint32_t count,
                                  const uint8_t* vertices, uint32_t stride);

/* -------- RSX thread: receiver -------- */
int  rsx_coop_pose_render_begin(void);                   /* 1 = live remote pose */
/* Dense peer window for the part; returns the vertex count written (<= max)
 * and its first guest index. Vertices without a fresh sample are NaN. */
uint32_t rsx_coop_pose_write_part(uint64_t key, uint32_t stride, uint32_t tangent_slot,
                                  uint8_t* dst, uint32_t max_vertices, uint32_t* gmin);
/* Index list for a submesh relative to gmin (after write_part this frame). */
uint32_t rsx_coop_pose_write_indices(uint64_t submesh, uint32_t gmin, uint32_t count,
                                     uint32_t* dst, uint32_t max_indices);
int  rsx_coop_pose_render_end(void);                     /* 1 = retire the proxy */
int  rsx_coop_pose_remote_has_key(uint64_t key);

/* -------- round 4: parts this instance never draws itself --------
 * The two instances do not always draw Cole through the same mesh streams
 * (the game swaps part streams / LODs, e.g. while climbing with the camera
 * close).  A peer part whose key this instance never draws has no local draw
 * to ride on and no static vertex template from local guest memory.  So:
 *  - each sender announces the material of every submesh it draws
 *    (texture 0, vertex stride, tangent slot), and
 *  - a receiver that has live samples for a part but no template asks the
 *    sender for that part's static vertex slots, which the sender then
 *    streams from the same guest read it already does for the pose.
 * The backend draws such submeshes through any local main-view Cole draw of
 * the same material.  All three messages are 0xC2 packets with a zero
 * triangle count, which older builds ignore. */
/* RSX thread, both roles: the material of a submesh drawn locally this frame. */
void rsx_coop_pose_note_material(uint64_t submesh, uint64_t key, uint32_t tex0,
                                 uint32_t stride, uint32_t tangent_slot);
typedef struct rsx_coop_pose_submat {
    uint64_t submesh, key;
    uint32_t tex0, stride, tangent_slot;
} rsx_coop_pose_submat;
/* RSX thread, receiver: the peer's submeshes of parts with live samples and a
 * known material (call after rsx_coop_pose_render_begin). */
uint32_t rsx_coop_pose_remote_submeshes(rsx_coop_pose_submat* out, uint32_t max);
/* Diagnostics: template requests sent / served, remote-template vertices stored. */
void rsx_coop_pose_template_counts(uint32_t* asked, uint32_t* served, uint32_t* stored);

/* -------- game thread -------- */
typedef int (*rsx_coop_pose_send_fn)(const void* data, uint32_t size);
unsigned rsx_coop_pose_drain_outgoing(rsx_coop_pose_send_fn send);
void rsx_coop_pose_receive(const uint8_t* data, uint32_t size, uint64_t now_ms);
void rsx_coop_pose_reset(void);

typedef struct rsx_coop_pose_stats {
    uint32_t send_hz;
    uint32_t sent_frames, received_frames;
    uint32_t remote_streams, drawn_streams;   /* mesh parts (keys) */
    uint32_t drawn_vertices, missing_vertices;
    uint32_t last_send_packets, last_send_vertices;
    uint32_t last_receive_age_ms;
    uint32_t drawn_triangles, known_triangles;
} rsx_coop_pose_stats;
void rsx_coop_pose_get_stats(rsx_coop_pose_stats* stats);
/* Diagnostics (RSX thread). */
void rsx_coop_pose_debug_counts(uint32_t* ok, uint32_t* out_of_range, uint32_t* not_fresh);
void rsx_coop_pose_debug_parts(void);

#ifdef __cplusplus
}
#endif
#endif
