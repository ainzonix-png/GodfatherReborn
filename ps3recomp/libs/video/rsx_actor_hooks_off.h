/* Normal builds: the renderer's optional actor hook points compile to nothing.
 * Opt-in mod builds include their own headers instead; those are not part of the public tree. */
#pragma once
#include "ps3emu/ps3types.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

#if !(defined(PS3RECOMP_SKATE_MOD) && PS3RECOMP_SKATE_MOD)
#define g_skate_render_hooks 0
static inline void rsx_skate_actor_draw(void* device, void* command_list, u64 rtv_handle,
                                        u32 width, u32 height, const float camera[16],
                                        const float viewport_xf[8])
{ (void)device; (void)command_list; (void)rtv_handle; (void)width; (void)height; (void)camera; (void)viewport_xf; }
static inline long rsx_skate_actor_prepare_scene(void* device, const float camera[16], float zscale, float zoffset)
{ (void)device; (void)camera; (void)zscale; (void)zoffset; return -1; }
static inline void rsx_skate_actor_draw_scene(void* device, void* command_list, long cb_offset,
                                              u64 rtv_handle, u64 dsv_handle, u32 width, u32 height,
                                              u32 rt_format, void* srv_heap, void* sampler_heap)
{ (void)device; (void)command_list; (void)cb_offset; (void)rtv_handle; (void)dsv_handle; (void)width; (void)height; (void)rt_format; (void)srv_heap; (void)sampler_heap; }
static inline int rsx_skate_actor_hides_player_draw(const float world_position[3])
{ (void)world_position; return 0; }
static inline int rsx_skate_actor_get_snapshot(float position[3], float* heading,
                                               unsigned* clip, float* clip_time, float* lean)
{ (void)position; (void)heading; (void)clip; (void)clip_time; (void)lean; return 0; }
static inline int rsx_skate_actor_pose_cole_vertices(float* positions, size_t position_stride,
                                                     u32 vertex_count)
{ (void)positions; (void)position_stride; (void)vertex_count; return 0; }
static inline void rsx_skate_actor_set_scene_depth(void* srv_heap, void* sampler_heap, u64 gpu_srv,
                                                   int valid, float zscale, float zoffset)
{ (void)srv_heap; (void)sampler_heap; (void)gpu_srv; (void)valid; (void)zscale; (void)zoffset; }
static inline void rsx_skate_actor_shutdown(void) {}
#endif

#if !defined(PS3RECOMP_COOP_MOD)
#define g_coop_render_hooks 0
static inline int rsx_coop_actor_capture_vertices(u32 f, u64 k, const float* p, size_t s, u32 n)
{ (void)f; (void)k; (void)p; (void)s; (void)n; return 0; }
static inline void rsx_coop_actor_set_native_frame(u32 f, int c) { (void)f; (void)c; }
static inline int rsx_coop_actor_native_frame_complete(u32 f) { (void)f; return 0; }
static inline long rsx_coop_actor_prepare_scene(void* d, u32 f, const float c[16], float zs, float zo)
{ (void)d; (void)f; (void)c; (void)zs; (void)zo; return -1; }
static inline void rsx_coop_actor_draw_scene(void* d, void* l, long t, u64 r, u64 z,
                                             u32 w, u32 h, u32 f)
{ (void)d; (void)l; (void)t; (void)r; (void)z; (void)w; (void)h; (void)f; }
static inline void rsx_coop_actor_shutdown(void) {}
#endif

#ifdef __cplusplus
}
#endif
