#ifndef PS3RECOMP_RSX_COOP_ACTOR_H
#define PS3RECOMP_RSX_COOP_ACTOR_H

#include <stddef.h>
#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(PS3RECOMP_COOP_MOD)

/* Cheap hot-path gate owned by the title-side co-op bridge. */
extern volatile int g_coop_render_hooks;

/* Feed an expanded triangle list after the guest vertex fetch.  The renderer
 * recognizes the small, world-space skinned draws around the local player and
 * snapshots their positions; it never reads a replacement model or asset. */
int rsx_coop_actor_capture_vertices(u32 frame, u64 stream_key,
                                    const float* positions,
                                    size_t position_stride, u32 vertex_count);
/* Once per recorded frame: did the peer's networked pose cover Cole? */
void rsx_coop_actor_set_native_frame(u32 frame, int complete);
int rsx_coop_actor_native_frame_complete(u32 frame);

/* Freeze this frame's captured native Cole geometry and camera into a ring
 * slot.  The returned token belongs to draw_scene, or is -1 when disconnected
 * or no valid player geometry has been observed. */
long rsx_coop_actor_prepare_scene(void* device, u32 frame, const float camera[16],
                                  float zscale, float zoffset);
void rsx_coop_actor_draw_scene(void* device, void* command_list, long token,
                               u64 rtv_handle, u64 dsv_handle, u32 width,
                               u32 height, u32 rt_format);
void rsx_coop_actor_shutdown(void);
/* coop v8 (agent L): fallback for the peer's lightning when no local lightning draw can lend the game's material.
 * verts = triangle list of {float p[3] (world cm), float t[3] (t.x -1..1 across, t.y 0, t.z brightness)}. */
long rsx_coop_fx_ribbon_prepare(void* device, const float camera[16], float zscale, float zoffset,
                                const float* verts, u32 nverts, const float tint[4]);
void rsx_coop_fx_ribbon_draw(void* device, void* command_list, long token, u64 rtv_handle, u64 dsv_handle,
                             u32 width, u32 height, u32 rt_format);

#else
#include "rsx_actor_hooks_off.h"
#endif

#ifdef __cplusplus
}
#endif
#endif
