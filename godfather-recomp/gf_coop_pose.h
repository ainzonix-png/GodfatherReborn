#ifndef GF_COOP_POSE_H
#define GF_COOP_POSE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { GF_COOP_POSE_MAX_PACKET = 1100, GF_COOP_POSE_BLOB_CHANNEL = 1 };

/* Capture must run on the main thread after this entity's skeleton update. */
int  gf_coop_pose_capture(uint32_t ent, uint8_t* out, int cap);
/* Apply succeeds only for the entity selected by gf_coop_pose_set_target. */
int  gf_coop_pose_apply(uint32_t ent, const uint8_t* in, int len);
void gf_coop_pose_set_target(uint32_t ent);
/* Safe for the network thread; validates and copies, but never touches guest memory. */
void gf_coop_pose_on_received(const uint8_t* in, int len);
int  gf_coop_pose_selftest(void);

/* Call immediately after func_003CC128 has updated a skeleton. */
void gf_coop_pose_post_skel(uint32_t anim);

/* v1066N (agent N): world-NPC pose mixing (gf_coop_wpose.c). A pair = two wire poses around a time (pa NULL = hold pb),
 * u = blend a->b (u > 1 extrapolates, clamped to 2). */
typedef struct { const uint8_t* pa; int la; const uint8_t* pb; int lb; float u; } gf_pose_pair;
int  gf_coop_pose_apply_mix(uint32_t ent, uint32_t anim, const gf_pose_pair* s0, const gf_pose_pair* s1, float w, int own_face_live);
int  gf_coop_pose_rig_key(uint32_t ent, uint32_t anim, uint32_t* key, int* slots);
uint32_t gf_coop_pose_wire_key(const uint8_t* in, int len);

#ifdef __cplusplus
}
#endif

#endif
