#pragma once
#include "ps3emu/ps3types.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

#ifdef PS3RECOMP_SKATE_MOD

extern volatile int g_skate_render_hooks;

/* Host presentation bridge for the optional inFamous Skate actor.
 *
 * camera: the guest's world camera rows (c108..c111).
 * viewport_xf: the guest viewport mapped to D3D NDC, laid out like the live
 *   draw path's epilogue: [0..2] clip.xyz scale, [4..6] clip.w offset. */
void rsx_skate_actor_draw(void* device, void* command_list, u64 rtv_handle,
                          u32 width, u32 height, const float camera[16],
                          const float viewport_xf[8]);

/* In-scene board: prepare at record time (returns a constant-slot offset or -1), draw during the replay of the
 * same frame into the game's own scene target and depth buffer. */
long rsx_skate_actor_prepare_scene(void* device, const float camera[16], float zscale, float zoffset);
void rsx_skate_actor_draw_scene(void* device, void* command_list, long cb_offset, u64 rtv_handle, u64 dsv_handle,
                                u32 width, u32 height, u32 rt_format, void* srv_heap, void* sampler_heap);

/* Called with actual world-space skinned vertex positions. Returns 1 when a
 * draw overlaps the player and the Skate actor is replacing him. */
int rsx_skate_actor_hides_player_draw(const float world_position[3]);

/* Current mounted state for Cole-palette retargeting in the guest draw path. */
int rsx_skate_actor_get_snapshot(float position[3], float* heading,
                                 unsigned* clip, float* clip_time, float* lean);

/* Retarget the sampled Skate skeleton onto a strided stream of Cole's
 * world-space, already-skinned positions. Returns nonzero only for a draw
 * recognized as part of Cole's body or clothing. */
int rsx_skate_actor_pose_cole_vertices(float* positions,
                                       size_t position_stride,
                                       u32 vertex_count);

/* The main 3D pass's depth surface (shader-visible SRV in srv_heap) and the guest depth mapping
 * (depth = (clip.z * zscale + clip.w * zoffset) / clip.w), so the board is hidden behind Cole and the world. */
void rsx_skate_actor_set_scene_depth(void* srv_heap, void* sampler_heap, u64 gpu_srv, int valid,
                                     float zscale, float zoffset);

void rsx_skate_actor_shutdown(void);

#else
#include "rsx_actor_hooks_off.h"
#endif

#ifdef __cplusplus
}
#endif
