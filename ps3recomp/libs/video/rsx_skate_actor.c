/* Host presentation bridge for the optional inFamous Skate actor.
 *
 * Draws the converted Skate skater + board at Cole's position after the guest
 * frame has been copied to the back buffer.  Poses come from a private pack
 * (SKATEPK8) built on the owner's machine by
 * infamous-recomp/mods/skate/build_asset_pack.py; each clip frame holds one
 * skinning matrix per joint, already composed with RIG_TPOSE and with the
 * clip's root travel removed (inFamous owns translation).
 *
 * Per frame this file samples the active clip with frame interpolation, blends
 * the steering lean clips, cross-fades clip changes, and renders with a
 * private depth buffer so the mesh sorts against itself.  It also tells the
 * live draw path which skinned guest draws belong to Cole so he can be hidden
 * while the skater stands in for him.
 *
 * License: GPL-3.0-only, like the rest of the Skate mod (see
 * infamous-recomp/mods/skate/README.md). */
#include "rsx_skate_actor.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include "rsx_d3d12_abi.h"   /* struct-returning D3D12 calls for every Windows SDK */
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler.lib")

/* ABI shared with infamous-recomp/skate_mod.h (infamous_skate_actor_state). */
typedef struct {
    float position[3];
    float heading;
    unsigned clip;
    float clip_time;
    float lean;
    float speed;
} skate_actor_snapshot_t;
extern int infamous_skate_actor_snapshot(skate_actor_snapshot_t* out);
/* Passthrough mode: Skate 3 Rust's live render_pose (64 x row-major native globals), the native->host yaw and a
 * mount generation counter.  Returns the bone count, 0 when no live pose is available. */
extern int infamous_skate_live_pose(float* bones, float* yaw, unsigned* gen);
/* Tells the mod where the game camera looks, so the board can be steered in world space. */
extern void infamous_skate_camera_forward(float x, float y);

enum { CLIP_IDLE, CLIP_PUSH, CLIP_OLLIE, CLIP_LAND, CLIP_BRAKE,
       CLIP_LEAN_NEG, CLIP_LEAN_POS, CLIP_KICKFLIP, CLIP_HEELFLIP,
       CLIP_POPSHUVIT, CLIP_360FLIP, CLIP_OLLIE_POP,
       CLIP_MANUAL, CLIP_NOSEMANUAL, CLIP_GRIND, CLIP_BOARDSLIDE, CLIP_CROOKED,
       CLIP_KICKFLIP_POP, CLIP_HEELFLIP_POP, CLIP_POPSHUVIT_POP, CLIP_360FLIP_POP, CLIP_COUNT };
enum { MAX_BONES = 64, MATRIX_FLOATS = 16, COLE_LANDMARKS = 23 };
#define CROSSFADE_SECONDS 0.16f
#define CB_SIZE 8192u
#define CB_SLOTS 32u  /* two actors/frame; recorded frames may still read older slots */

typedef struct {
    char magic[8];
    u32 version, vertices, indices, bones, clips, vertex_stride, matrix_stride, board_indices;
} skate_pack_header;
typedef struct { u32 frames; float fps; u32 loops; u32 loop_start; } skate_pack_clip;   /* loops from loop_start */
typedef struct { u32 joint; float bind[3]; } skate_pack_landmark;

typedef struct {
    int attempted, ready;
    skate_pack_header header;
    skate_pack_clip clip[CLIP_COUNT];
    skate_pack_landmark landmark[COLE_LANDMARKS];
    u8* bytes;
    size_t size;
    size_t frame_offset[CLIP_COUNT];
    ID3D12Resource *vb, *ib, *board_ib, *cb, *depth;
    size_t board_offset;
    int rider;   /* INFAMOUS_SKATE_ACTOR=rider is a diagnostic preview; gameplay keeps Cole */
    ID3D12DescriptorHeap* dsv_heap;
    u32 depth_w, depth_h;
    u8* cb_mapped;
    u32 cb_slot;
    ID3D12RootSignature* root;
    ID3D12PipelineState* pso;
    /* in-scene board: drawn inside the game's own lit pass with its depth buffer (one PSO per target format) */
    ID3DBlob *vs_blob, *ps_blob;
    struct { u32 fmt; ID3D12PipelineState* pso; } scene_pso[4];
    volatile LONG scene_tick;   /* GetTickCount of the last in-scene frame: the overlay path stands down */
    /* animation presentation */
    float pose[MAX_BONES * MATRIX_FLOATS];
    float fade_from[MAX_BONES * MATRIX_FLOATS];
    unsigned last_clip;
    int have_pose;
    LARGE_INTEGER fade_start, qpc_freq;
    int in_scene_off;   /* INFAMOUS_SKATE_IN_SCENE=0: old overlay drawing */
    float scale, z_offset;
    /* ground clamp: the board's own vertices (model space) and the rolling stance's lowest point */
    struct { float p[3]; u8 bone[4]; float w[4]; } *gv;
    u32 gv_count;
    float ground_ref;
    /* player hiding */
    int hide_enabled;
    u32 frames_drawn, hidden_draws, hidden_this_frame;
    float nearest_skinned;
} skate_actor_state;
static skate_actor_state s;
/* SKLIVE01 pack trailer: how to skin Skate's live native pose (build_asset_pack.py compose()). */
static struct { int ok; u32 hier; u32 joint_bone[MAX_BONES]; float ib[MAX_BONES][16]; } s_live;
static volatile LONG s_scene_calls;   /* bumped by every in-scene board placement (record side) */
static LONG s_scene_calls_seen;        /* value at the previous overlay call: unchanged = no in-scene board this frame */
static long s_board_frames, s_board_overlay;   /* [SKATE-BOARD] stats */
static int s_live_used;          /* build_pose took the live pose this frame */
static float s_live_heading;     /* heading for the live pose (native -> host yaw) */

/* Set by the Skate mod while it is mounted (and by SKATE_DIAG); the renderer's per-draw hooks cost nothing
 * otherwise.  Defined outside the _WIN32 block so every backend links. */
volatile int g_skate_render_hooks;

/* Scene depth for the board (set by the backend right before rsx_skate_actor_draw). */
static ID3D12DescriptorHeap* g_scene_heaps[2];
static u64 g_scene_srv; static int g_scene_valid; static float g_scene_zscale = 1.0f, g_scene_zoffset;
void rsx_skate_actor_set_scene_depth(void* srv_heap, void* sampler_heap, u64 gpu_srv, int valid,
                                     float zscale, float zoffset)
{
    g_scene_heaps[0] = (ID3D12DescriptorHeap*)srv_heap; g_scene_heaps[1] = (ID3D12DescriptorHeap*)sampler_heap;
    static int depth_test = -1;
    if (depth_test < 0) { const char* e = getenv("INFAMOUS_SKATE_BOARD_DEPTH"); depth_test = !(e && *e == '0'); }
    g_scene_srv = gpu_srv; g_scene_valid = depth_test && valid && srv_heap && gpu_srv;
    g_scene_zscale = zscale; g_scene_zoffset = zoffset;
}

/* Written by the present path, read by the draw sink on the same thread. */
static volatile int g_hide_active;
static float g_hide_position[3];

static float env_float(const char* name, float fallback)
{
    const char* v = getenv(name);
    if (!v || !*v) return fallback;
    return (float)atof(v);
}

static int upload_buffer(ID3D12Device* dev, const void* data, size_t size,
                         ID3D12Resource** result, u8** mapped_out)
{
    D3D12_HEAP_PROPERTIES hp = {0};
    D3D12_RESOURCE_DESC rd = {0};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->lpVtbl->CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE,
            &rd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource,
            (void**)result))) return 0;
    u8* mapped = NULL; D3D12_RANGE empty = {0, 0};
    if (FAILED((*result)->lpVtbl->Map(*result, 0, &empty, (void**)&mapped))) return 0;
    if (data) memcpy(mapped, data, size);
    if (mapped_out) *mapped_out = mapped;
    else (*result)->lpVtbl->Unmap(*result, 0, NULL);
    return 1;
}

static int pack_error(const char* why)
{
    fprintf(stderr, "[SKATE-ACTOR] pack rejected: %s (rebuild it with play_skate.cmd)\n", why);
    return 0;
}

static int load_pack(void)
{
    const char* path = getenv("INFAMOUS_SKATE_PACK");
    if (!path || !*path) path = "build-fast/skate_actor_v8.pack";
    FILE* file = fopen(path, "rb");
    if (!file) { fprintf(stderr, "[SKATE-ACTOR] pack not found: %s\n", path); return 0; }
    fseek(file, 0, SEEK_END); s.size = (size_t)ftell(file); fseek(file, 0, SEEK_SET);
    s.bytes = (u8*)malloc(s.size);
    if (!s.bytes || fread(s.bytes, 1, s.size, file) != s.size) {
        fclose(file); return pack_error("read failed");
    }
    fclose(file);
    if (s.size < sizeof(s.header)) return pack_error("truncated header");
    memcpy(&s.header, s.bytes, sizeof(s.header));
    if (memcmp(s.header.magic, "SKATEPK8", 8) || s.header.version != 8)
        return pack_error("old or unknown pack version");
    if (s.header.vertex_stride != 44 || s.header.matrix_stride != 64 ||
        s.header.bones == 0 || s.header.bones > MAX_BONES ||
        s.header.clips != CLIP_COUNT)
        return pack_error("unexpected layout");
    size_t cursor = sizeof(s.header);
    if (s.size < cursor + sizeof(s.clip)) return pack_error("truncated clip table");
    memcpy(s.clip, s.bytes + cursor, sizeof(s.clip));
    cursor += sizeof(s.clip);
    if (s.size < cursor + sizeof(s.landmark)) return pack_error("truncated landmark table");
    memcpy(s.landmark, s.bytes + cursor, sizeof(s.landmark));
    cursor += sizeof(s.landmark);
    for (u32 i = 0; i < COLE_LANDMARKS; i++)
        if (s.landmark[i].joint >= s.header.bones)
            return pack_error("invalid landmark joint");
    cursor += (size_t)s.header.vertices * s.header.vertex_stride
            + (size_t)s.header.indices * sizeof(u32);
    for (u32 clip = 0; clip < CLIP_COUNT; clip++) {
        if (!s.clip[clip].frames || !(s.clip[clip].fps > 0.0f))
            return pack_error("empty clip");
        s.frame_offset[clip] = cursor;
        cursor += (size_t)s.clip[clip].frames * s.header.bones * s.header.matrix_stride;
    }
    s.board_offset = cursor;
    cursor += (size_t)s.header.board_indices * sizeof(u32);
    if (!s.header.board_indices || cursor > s.size) return pack_error("size mismatch");
    if (cursor != s.size) {   /* optional SKLIVE01 trailer (live Skate pose) */
        u32 hj[2];
        if (s.size < cursor + 16 || memcmp(s.bytes + cursor, "SKLIVE01", 8)) return pack_error("size mismatch");
        memcpy(hj, s.bytes + cursor + 8, 8);
        const size_t need = cursor + 16 + (size_t)hj[1] * 4 + (size_t)hj[1] * 64;
        if (hj[1] != s.header.bones || hj[0] == 0 || hj[0] > MAX_BONES || need != s.size) return pack_error("bad live trailer");
        memcpy(s_live.joint_bone, s.bytes + cursor + 16, (size_t)hj[1] * 4);
        memcpy(s_live.ib, s.bytes + cursor + 16 + (size_t)hj[1] * 4, (size_t)hj[1] * 64);
        s_live.hier = hj[0];
        s_live.ok = 1;
        for (u32 j = 0; j < hj[1]; j++) if (s_live.joint_bone[j] >= hj[0]) s_live.ok = 0;
        fprintf(stderr, "[SKATE-LIVE] pack carries the live-pose skin map (%u native bones, %u joints)%s\n",
                hj[0], hj[1], s_live.ok ? "" : " -- INVALID, live pose off");
    }
    for (u32 clip = 0; clip < CLIP_COUNT; clip++)
        if (s.clip[clip].loop_start >= s.clip[clip].frames) return pack_error("bad loop start");
    /* every 4th distinct board vertex is plenty to find the deck/wheel extremes */
    {
        const u8* vb = s.bytes + sizeof(s.header) + sizeof(s.clip) + sizeof(s.landmark);
        const u32* bi = (const u32*)(s.bytes + s.board_offset);
        u8* seen = (u8*)calloc(s.header.vertices, 1);
        s.gv = calloc(s.header.board_indices, sizeof *s.gv);
        if (!seen || !s.gv) return pack_error("out of memory");
        u32 distinct = 0;
        for (u32 i = 0; i < s.header.board_indices; i++) {
            const u32 v = bi[i];
            if (v >= s.header.vertices || seen[v]) continue;
            seen[v] = 1;
            if (distinct++ % 4) continue;
            const u8* r = vb + (size_t)v * 44;
            memcpy(s.gv[s.gv_count].p, r, 12);
            u16 bone[4], w[4]; memcpy(bone, r + 24, 8); memcpy(w, r + 32, 8);
            for (int k = 0; k < 4; k++) { s.gv[s.gv_count].bone[k] = (u8)(bone[k] < s.header.bones ? bone[k] : 0);
                                          s.gv[s.gv_count].w[k] = w[k] / 65535.0f; }
            s.gv_count++;
        }
        free(seen);
    }
    return 1;
}

static int create_pipeline(ID3D12Device* dev)
{
    /* Model space is the GLB's: metres, +Y up, +Z = direction of travel,
     * +X = rider's left.  inFamous world space is centimetres, +Z up. */
    static const char shader[] =
        "cbuffer Actor:register(b0){float4 camera[4];float4 xf0;float4 xf1;float4 actor;float4 params;float4 tint;float4x4 bones[64];};"
        "struct I{float3 p:POSITION;float3 n:NORMAL;uint4 j:BLENDINDICES;float4 w:BLENDWEIGHT;float4 c:COLOR;};"
        "Texture2D<float> scene_depth:register(t0);"
        "struct O{float4 p:SV_Position;float4 c:COLOR;float2 gz:TEXCOORD0;};"
        "O vs(I i){O o;float4 m=0;float3 n=0;"
        "[unroll]for(int k=0;k<4;k++){m+=mul(bones[i.j[k]],float4(i.p,1))*i.w[k];n+=mul((float3x3)bones[i.j[k]],i.n)*i.w[k];}"
        "float S=params.x,co=cos(actor.w),si=sin(actor.w);"
        "float4 q=float4(actor.x+(m.z*co-m.x*si)*S,actor.y+(m.z*si+m.x*co)*S,actor.z+m.y*S,1);"
        "float4 h=float4(dot(camera[0],q),dot(camera[1],q),dot(camera[2],q),dot(camera[3],q));"
        "o.p=float4(h.xyz*xf0.xyz+h.w*xf1.xyz,h.w);"
        "o.gz=float2(h.z*xf0.z+h.w*xf1.z,h.w);"   /* the guest's own depth for this point */
        "o.p.z=params.w>0.5?o.gz.x:h.w*saturate(h.w*params.y);"   /* in-scene: the guest's own depth */
        "n=normalize(n+1e-6);float3 nw=float3(n.z*co-n.x*si,n.z*si+n.x*co,n.y);"
        "float light=.42+.48*saturate(dot(nw,normalize(float3(.35,.25,.9))))+.10*saturate(nw.z);"
        "o.c=float4(i.c.rgb*tint.rgb*light,i.c.a);return o;}"
        /* no alpha test: the Skate textures keep a material mask (not opacity) in alpha; testing it threw
         * away the wheels, the trucks and most of the deck */
        /* hidden behind the game's scene (Cole's shoes and shins, curbs, walls): compare against its depth */
        "float4 ps(O i):SV_Target{"
        "if(params.z>0.5){uint w,hh;scene_depth.GetDimensions(w,hh);"
        "int2 c=int2(i.p.xy*float2(xf0.w,xf1.w)*float2(w,hh));"
        "float sd=scene_depth.Load(int3(c,0));if(i.gz.x/i.gz.y>sd+0.00002)discard;}"
        "return float4(i.c.rgb,1);}";
    ID3DBlob *vs = NULL, *ps = NULL, *errors = NULL, *signature = NULL;
    if (FAILED(D3DCompile(shader, sizeof(shader)-1, "skate_actor", NULL, NULL,
                          "vs", "vs_5_0", 0, 0, &vs, &errors))) goto fail;
    if (FAILED(D3DCompile(shader, sizeof(shader)-1, "skate_actor", NULL, NULL,
                          "ps", "ps_5_0", 0, 0, &ps, &errors))) goto fail;
    D3D12_ROOT_PARAMETER parameter[2] = {{0}}; D3D12_ROOT_SIGNATURE_DESC root_desc = {0};
    parameter[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter[0].Descriptor.ShaderRegister = 0;
    parameter[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_DESCRIPTOR_RANGE depth_range = {0};
    depth_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; depth_range.NumDescriptors = 1;
    depth_range.BaseShaderRegister = 0; depth_range.OffsetInDescriptorsFromTableStart = 0;
    parameter[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter[1].DescriptorTable.NumDescriptorRanges = 1; parameter[1].DescriptorTable.pDescriptorRanges = &depth_range;
    parameter[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_desc.NumParameters = 2; root_desc.pParameters = parameter;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &signature, &errors))) goto fail;
    if (FAILED(dev->lpVtbl->CreateRootSignature(dev, 0,
            signature->lpVtbl->GetBufferPointer(signature), signature->lpVtbl->GetBufferSize(signature),
            &IID_ID3D12RootSignature, (void**)&s.root))) goto fail;
    D3D12_INPUT_ELEMENT_DESC input[] = {
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"BLENDINDICES",0,DXGI_FORMAT_R16G16B16A16_UINT,0,24,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"BLENDWEIGHT",0,DXGI_FORMAT_R16G16B16A16_UNORM,0,32,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R8G8B8A8_UNORM,0,40,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature=s.root; pd.VS.pShaderBytecode=vs->lpVtbl->GetBufferPointer(vs);
    pd.VS.BytecodeLength=vs->lpVtbl->GetBufferSize(vs); pd.PS.pShaderBytecode=ps->lpVtbl->GetBufferPointer(ps);
    pd.PS.BytecodeLength=ps->lpVtbl->GetBufferSize(ps); pd.InputLayout.pInputElementDescs=input;
    pd.InputLayout.NumElements=5; pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    /* Depth is the actor's own linear view depth (see the shader), so it is
     * never clipped against the guest's near/far planes. */
    pd.RasterizerState.DepthClipEnable=FALSE;
    pd.DepthStencilState.DepthEnable=TRUE;
    pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS;
    pd.DSVFormat=DXGI_FORMAT_D32_FLOAT;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask=UINT_MAX; pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets=1; pd.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM; pd.SampleDesc.Count=1;
    if (FAILED(dev->lpVtbl->CreateGraphicsPipelineState(dev, &pd,
            &IID_ID3D12PipelineState, (void**)&s.pso))) goto fail;
    if(signature)signature->lpVtbl->Release(signature);
    s.vs_blob = vs; s.ps_blob = ps;   /* kept for the in-scene pipelines (one per target format) */
    if(errors)errors->lpVtbl->Release(errors); return 1;
fail:
    if(errors)fprintf(stderr,"[SKATE-ACTOR] shader error: %s\n",(char*)errors->lpVtbl->GetBufferPointer(errors));
    else fprintf(stderr,"[SKATE-ACTOR] pipeline creation failed\n");
    if(signature)signature->lpVtbl->Release(signature); if(vs)vs->lpVtbl->Release(vs);
    if(ps)ps->lpVtbl->Release(ps); if(errors)errors->lpVtbl->Release(errors); return 0;
}

/* The in-scene board pipeline: the game's colour target format, its D24S8 depth (tested and written,
 * never cleared), and depth in the guest's own convention (shader params.w). */
static ID3D12PipelineState* scene_pipeline(ID3D12Device* dev, u32 fmt)
{
    for (int i = 0; i < 4; i++) if (s.scene_pso[i].pso && s.scene_pso[i].fmt == fmt) return s.scene_pso[i].pso;
    int slot = -1;
    for (int i = 0; i < 4; i++) if (!s.scene_pso[i].pso) { slot = i; break; }
    if (slot < 0 || !s.vs_blob || !s.ps_blob) return NULL;
    D3D12_INPUT_ELEMENT_DESC input[] = {
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"BLENDINDICES",0,DXGI_FORMAT_R16G16B16A16_UINT,0,24,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"BLENDWEIGHT",0,DXGI_FORMAT_R16G16B16A16_UNORM,0,32,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R8G8B8A8_UNORM,0,40,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = s.root;
    pd.VS.pShaderBytecode = s.vs_blob->lpVtbl->GetBufferPointer(s.vs_blob); pd.VS.BytecodeLength = s.vs_blob->lpVtbl->GetBufferSize(s.vs_blob);
    pd.PS.pShaderBytecode = s.ps_blob->lpVtbl->GetBufferPointer(s.ps_blob); pd.PS.BytecodeLength = s.ps_blob->lpVtbl->GetBufferSize(s.ps_blob);
    pd.InputLayout.pInputElementDescs = input; pd.InputLayout.NumElements = 5;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    /* colour only: the scene target's alpha is the game's own post-processing channel (bloom/glow), and a
     * full alpha made the board glow white -- it keeps the alpha of the ground it covers */
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN |
                                                          D3D12_COLOR_WRITE_ENABLE_BLUE;
    pd.SampleMask = UINT_MAX; pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = (DXGI_FORMAT)fmt; pd.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = NULL;
    if (FAILED(dev->lpVtbl->CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void**)&pso))) {
        fprintf(stderr, "[SKATE-ACTOR] in-scene pipeline for format %u failed\n", fmt);
        return NULL;
    }
    s.scene_pso[slot].fmt = fmt; s.scene_pso[slot].pso = pso;
    fprintf(stderr, "[SKATE-ACTOR] board now drawn inside the game's scene pass (target format %u)\n", fmt);
    return pso;
}

static int ensure_depth(ID3D12Device* dev, u32 width, u32 height)
{
    if (s.depth && s.depth_w == width && s.depth_h == height) return 1;
    if (s.depth) { s.depth->lpVtbl->Release(s.depth); s.depth = NULL; }
    if (!s.dsv_heap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors = 1;
        if (FAILED(dev->lpVtbl->CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap,
                                                     (void**)&s.dsv_heap))) return 0;
    }
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = width; rd.Height = height; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_D32_FLOAT; rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear = {0}; clear.Format = DXGI_FORMAT_D32_FLOAT; clear.DepthStencil.Depth = 1.0f;
    if (FAILED(dev->lpVtbl->CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, &IID_ID3D12Resource, (void**)&s.depth)))
        return 0;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv;
    RSX_D3D12_CPU_HEAP_START(s.dsv_heap)(s.dsv_heap, &dsv);
    dev->lpVtbl->CreateDepthStencilView(dev, s.depth, NULL, dsv);
    s.depth_w = width; s.depth_h = height;
    return 1;
}

static void sample_clip(unsigned clip, float seconds, float* out);
static float board_min_y(const float* pose);
static int initialize(ID3D12Device* dev)
{
    s.attempted = 1;
    if (!load_pack()) return 0;
    size_t vb_size = (size_t)s.header.vertices * s.header.vertex_stride;
    size_t ib_size = (size_t)s.header.indices * sizeof(u32);
    const u8* vb_data = s.bytes + sizeof(s.header) + sizeof(s.clip) + sizeof(s.landmark);
    const u8* ib_data = vb_data + vb_size;
    if (!upload_buffer(dev, vb_data, vb_size, &s.vb, NULL) ||
        !upload_buffer(dev, ib_data, ib_size, &s.ib, NULL) ||
        !upload_buffer(dev, s.bytes + s.board_offset, (size_t)s.header.board_indices * sizeof(u32), &s.board_ib, NULL) ||
        !upload_buffer(dev, NULL, (size_t)CB_SIZE * CB_SLOTS, &s.cb, &s.cb_mapped) ||
        !create_pipeline(dev)) return 0;
    QueryPerformanceFrequency(&s.qpc_freq);
    s.scale = env_float("INFAMOUS_SKATE_SCALE", 100.0f);
    /* Cole's tracked position (0x00915430) is at his waist: the camera centres on it and his soles are
     * ~145 cm lower (measured from a frame with the camera rows c263..c266 and position c467). */
    s.z_offset = env_float("INFAMOUS_SKATE_Z_OFFSET", -145.0f);
    { const char* a = getenv("INFAMOUS_SKATE_ACTOR"); s.rider = a && !strcmp(a, "rider"); }
    s.nearest_skinned = 1e30f;
    {   static float rest[MAX_BONES * MATRIX_FLOATS];
        sample_clip(CLIP_IDLE, 0.0f, rest);
        s.ground_ref = board_min_y(rest);   /* wheels on the ground while rolling */
    }
    s.ready = 1;
    fprintf(stderr, "[SKATE-ACTOR] ready: %u vertices, %u triangles, %u bones, %u clips "
                    "(scale %.1f, z offset %.1f cm, drawing %s)\n",
            s.header.vertices, s.header.indices / 3, s.header.bones, s.header.clips,
            s.scale, s.z_offset, s.rider ? "the whole rider" : "the board only");
    return 1;
}

static void lerp_pose(float* out, const float* a, const float* b, float t, u32 count)
{
    for (u32 i = 0; i < count; i++) out[i] = a[i] + (b[i] - a[i]) * t;
}

/* Clip pose at a time in seconds, interpolating between neighbouring frames. */
static void sample_clip(unsigned clip, float seconds, float* out)
{
    const skate_pack_clip* c = &s.clip[clip];
    const u32 floats = s.header.bones * MATRIX_FLOATS;
    float frame = seconds * c->fps;
    if (frame < 0.0f) frame = 0.0f;
    u32 i0, i1; float t;
    if (c->loops) {
        /* a lead-in [0, loop_start) plays once, then [loop_start, frames) loops */
        const float ls = (float)c->loop_start, span = (float)(c->frames - c->loop_start);
        if (frame >= ls) frame = ls + fmodf(frame - ls, span);
        i0 = (u32)frame; if (i0 >= c->frames) i0 = c->frames - 1;
        i1 = i0 + 1 < c->frames ? i0 + 1 : c->loop_start;
        t = frame - (float)i0;
    } else {
        const float last = (float)(c->frames - 1);
        if (frame > last) frame = last;
        i0 = (u32)frame; i1 = i0 + 1 < c->frames ? i0 + 1 : i0;
        t = frame - (float)i0;
    }
    const float* base = (const float*)(s.bytes + s.frame_offset[clip]);
    lerp_pose(out, base + (size_t)i0 * floats, base + (size_t)i1 * floats, t, floats);
}

static void skin_point(const float* m, const float p[3], float out[3]);
/* Board joints in the pack's skin (SKATEBOARD_ROOT, trucks, wheels) and the board pivot landmark. */
enum { BOARD_JOINT_FIRST = 24, BOARD_JOINT_LAST = 30, LANDMARK_BOARD = 22 };
#define TRICK_SPIN_SECONDS 0.42f

/* Spin the board joints of a pose about the board's own centre: roll about its long axis (flips) and/or
 * yaw about its up axis (shove-its).  Skate 3 drives the board with its physics in the air -- the *_A
 * clips only carry about half a rotation -- so the full rotation is applied here. */
static void spin_board(float* pose, float roll, float yaw)
{
    const float* root = pose + (size_t)s.landmark[LANDMARK_BOARD].joint * MATRIX_FLOATS;
    float c[3];
    skin_point(root, s.landmark[LANDMARK_BOARD].bind, c);
    /* column-major: column k of the skin's 3x3 is mem[k*4 .. k*4+2] */
    float lng[3] = {root[8], root[9], root[10]}, up[3] = {root[4], root[5], root[6]};
    float ln = sqrtf(lng[0]*lng[0] + lng[1]*lng[1] + lng[2]*lng[2]), un = sqrtf(up[0]*up[0] + up[1]*up[1] + up[2]*up[2]);
    if (ln < 1e-6f || un < 1e-6f) return;
    for (int k = 0; k < 3; k++) { lng[k] /= ln; up[k] /= un; }
    float R[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
    const float* axes[2] = {lng, up};
    const float angles[2] = {roll, yaw};
    for (int t = 0; t < 2; t++) {
        if (angles[t] == 0.0f) continue;
        const float* u = axes[t]; const float co = cosf(angles[t]), si = sinf(angles[t]), ic = 1.0f - co;
        const float Q[3][3] = {
            {co + u[0]*u[0]*ic,      u[0]*u[1]*ic - u[2]*si, u[0]*u[2]*ic + u[1]*si},
            {u[1]*u[0]*ic + u[2]*si, co + u[1]*u[1]*ic,      u[1]*u[2]*ic - u[0]*si},
            {u[2]*u[0]*ic - u[1]*si, u[2]*u[1]*ic + u[0]*si, co + u[2]*u[2]*ic}};
        float T[3][3];
        for (int r = 0; r < 3; r++) for (int k = 0; k < 3; k++)
            T[r][k] = Q[r][0]*R[0][k] + Q[r][1]*R[1][k] + Q[r][2]*R[2][k];
        memcpy(R, T, sizeof T);
    }
    /* A = [R | c - R c] applied on the left of every board joint's skin matrix */
    const float tr[3] = { c[0] - (R[0][0]*c[0] + R[0][1]*c[1] + R[0][2]*c[2]),
                          c[1] - (R[1][0]*c[0] + R[1][1]*c[1] + R[1][2]*c[2]),
                          c[2] - (R[2][0]*c[0] + R[2][1]*c[1] + R[2][2]*c[2]) };
    for (u32 j = BOARD_JOINT_FIRST; j <= BOARD_JOINT_LAST && j < s.header.bones; j++) {
        float* m = pose + (size_t)j * MATRIX_FLOATS;
        float o[16];
        for (int col = 0; col < 4; col++) {
            const float x = m[col*4 + 0], y = m[col*4 + 1], z = m[col*4 + 2], w = m[col*4 + 3];
            o[col*4 + 0] = R[0][0]*x + R[0][1]*y + R[0][2]*z + tr[0]*w;
            o[col*4 + 1] = R[1][0]*x + R[1][1]*y + R[1][2]*z + tr[1]*w;
            o[col*4 + 2] = R[2][0]*x + R[2][1]*y + R[2][2]*z + tr[2]*w;
            o[col*4 + 3] = w;
        }
        memcpy(m, o, sizeof o);
    }
}

/* Lowest point of the board in model space (GLB: y up) for a pose. */
static float board_min_y(const float* pose)
{
    float lo = 1e30f;
    for (u32 i = 0; i < s.gv_count; i++) {
        float y = 0.0f;
        for (int k = 0; k < 4; k++) {
            if (s.gv[i].w[k] <= 0.0f) continue;
            const float* m = pose + (size_t)s.gv[i].bone[k] * MATRIX_FLOATS;
            y += s.gv[i].w[k] * (m[1]*s.gv[i].p[0] + m[5]*s.gv[i].p[1] + m[9]*s.gv[i].p[2] + m[13]);
        }
        if (y < lo) lo = y;
    }
    return lo;
}

static float smooth01(float x) { if (x <= 0.0f) return 0.0f; if (x >= 1.0f) return 1.0f; return x * x * (3.0f - 2.0f * x); }

/* Every trick, like in Skate: the trick's own pop (*_G), then its air clip (*_A) while the board spins.
 * The kickflip/heelflip air clips are short and end with the flicking foot high above the deck (Skate's physics
 * and catch logic bring it back); here the rider blends into the plain ollie's air clip, which comes back down
 * onto the deck, right as the spin finishes -- the catch -- so he lands with both feet on the board. */
/* Board spin per trick and variant (skate_mod.cpp trick_name: 0 plain, 1 = R1 held, 2 = L1 held), in whole turns:
 * roll = about the deck's long axis (flips, - = kickflip way), yaw = about its up axis (shove-its). */
static void trick_spin(unsigned clip, unsigned variant, float* roll, float* yaw)
{
    static const float tab[5][3][2] = {
        /* ollie    */ { { 0.0f, 0.0f }, {  0.0f,  1.0f }, { -3.0f,  0.0f } },   /* -, big spin, triple kickflip */
        /* kickflip */ { {-1.0f, 0.0f }, { -2.0f,  0.0f }, { -1.0f,  0.5f } },   /* -, double, varial */
        /* heelflip */ { { 1.0f, 0.0f }, {  2.0f,  0.0f }, {  1.0f, -0.5f } },   /* -, double, varial */
        /* shove-it */ { { 0.0f, 0.5f }, {  0.0f,  1.0f }, {  0.0f,  1.5f } },   /* -, 360, 540 */
        /* 360 flip */ { {-1.0f, 1.0f }, {  1.0f, -1.0f }, { -1.0f, -0.5f } },   /* -, laser flip, hardflip */
    };
    const int row = clip == CLIP_KICKFLIP ? 1 : clip == CLIP_HEELFLIP ? 2 : clip == CLIP_POPSHUVIT ? 3
                  : clip == CLIP_360FLIP ? 4 : 0;
    if (variant > 2) variant = 0;
    *roll = tab[row][variant][0]; *yaw = tab[row][variant][1];
}

static void sample_trick(unsigned clip, unsigned variant, float t, float* out)
{
    static float air[MAX_BONES * MATRIX_FLOATS], board[MAX_BONES * MATRIX_FLOATS];
    const u32 floats = s.header.bones * MATRIX_FLOATS;
    const unsigned pop_clip = clip == CLIP_KICKFLIP ? CLIP_KICKFLIP_POP : clip == CLIP_HEELFLIP ? CLIP_HEELFLIP_POP
                            : clip == CLIP_POPSHUVIT ? CLIP_POPSHUVIT_POP : clip == CLIP_360FLIP ? CLIP_360FLIP_POP
                            : CLIP_OLLIE_POP;
    const float pop = (float)s.clip[pop_clip].frames / s.clip[pop_clip].fps;
    if (t < pop) { sample_clip(pop_clip, t, out); return; }
    const float ta = t - pop;
    sample_clip(clip, ta, air);
    float roll_turns, yaw_turns;
    trick_spin(clip, variant, &roll_turns, &yaw_turns);
    /* multi-rotation tricks spin a little longer (still inside the 0.62 s guaranteed air arc) */
    const float spin_s = TRICK_SPIN_SECONDS * (fabsf(roll_turns) + fabsf(yaw_turns) > 1.6f ? 1.2f : 1.0f);
    if (clip != CLIP_OLLIE || roll_turns != 0.0f || yaw_turns != 0.0f) {
        /* the board flies with the plain ollie's board path and is spun procedurally */
        sample_clip(CLIP_OLLIE, ta, board);
        if (clip == CLIP_KICKFLIP || clip == CLIP_HEELFLIP) {
            /* catch: rider into the ollie air clip over the last part of the spin */
            const float w = smooth01((ta - (spin_s - 0.14f)) / 0.16f);
            if (w > 0.0f) lerp_pose(air, air, board, w, floats);
        }
        memcpy(air + (size_t)BOARD_JOINT_FIRST * MATRIX_FLOATS, board + (size_t)BOARD_JOINT_FIRST * MATRIX_FLOATS,
               (size_t)(BOARD_JOINT_LAST - BOARD_JOINT_FIRST + 1) * MATRIX_FLOATS * sizeof(float));
        const float k = smooth01(ta / spin_s);
        const float turn = 6.2831853f * k;
        spin_board(air, roll_turns * turn, yaw_turns * turn);
    }
    /* blend out of the pop over 50 ms so the hand-over is not a snap */
    if (ta < 0.05f) {
        sample_clip(pop_clip, pop, out);
        lerp_pose(out, out, air, ta / 0.05f, floats);
    } else memcpy(out, air, floats * sizeof(float));
}

/* Skin Skate 3 Rust's live pose exactly like build_asset_pack.py compose(): M = G^T * basis * inverse_bind,
 * stored column-major.  The trajectory's horizontal position is removed (Cole is the body that moves); the height
 * is pinned once per mount so the wheels sit where the rolling stance has them. */
static int build_live_pose(float* out, float* heading)
{
    static float bones[64 * 16];
    static unsigned last_gen = ~0u; static int frames; static float shift_y;
    float yaw = 0.0f; unsigned gen = 0;
    if (!s_live.ok) return 0;
    const int n = infamous_skate_live_pose(bones, &yaw, &gen);
    if (n < (int)s_live.hier) {
        static int warned;
        if (n > 0 && !warned) { warned = 1; fprintf(stderr, "[SKATE-LIVE] Skate sent %d bones, the pack needs %u -- clip animation instead\n", n, s_live.hier); }
        return 0;
    }
    if (gen != last_gen) { last_gen = gen; frames = 0; shift_y = 0.0f; }
    const float tx = bones[12], tz = bones[14];   /* TRAJECTORY global translation (row 3) */
    static const float B[4][4] = {{1,0,0,0},{0,0,1,0},{0,-1,0,0},{0,0,0,1}};
    for (u32 j = 0; j < s.header.bones; j++) {
        const float* G = bones + (size_t)s_live.joint_bone[j] * 16;   /* G[r*4+c], row-vector form */
        float A[4][4], M[4][4];
        for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
            float v = 0.0f;
            for (int k = 0; k < 4; k++) v += G[k * 4 + r] * B[k][c];   /* G^T[r][k] = G[k][r] */
            A[r][c] = v;
        }
        for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
            float v = 0.0f;
            for (int k = 0; k < 4; k++) v += A[r][k] * s_live.ib[j][k * 4 + c];
            M[r][c] = v;
        }
        M[0][3] -= tx; M[2][3] -= tz;
        float* o = out + (size_t)j * MATRIX_FLOATS;
        for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
            if (!isfinite(M[r][c])) return 0;
            o[c * 4 + r] = M[r][c];
        }
    }
    /* height: the first frames of a mount line the wheels up with the clip stance; afterwards Skate's own height
     * (pops, airs) comes through unchanged */
    if (frames < 15) shift_y = s.ground_ref - board_min_y(out);
    frames++;
    for (u32 j = 0; j < s.header.bones; j++) out[(size_t)j * MATRIX_FLOATS + 13] += shift_y;
    static float yaw_off = 1e9f;
    if (yaw_off > 1e8f) { const char* e = getenv("INFAMOUS_SKATE_LIVE_YAW"); yaw_off = e ? (float)atof(e) * 0.01745329f : 0.0f; }
    /* turn the pose so its own deck (SKATEBOARD_ROOT's long axis, model angle atan2(x, z) as the shader maps it)
     * points along the deck direction the mod reports in host space */
    {   const float* root = out + (size_t)s.landmark[LANDMARK_BOARD].joint * MATRIX_FLOATS;
        const float phi = (root[8] * root[8] + root[10] * root[10] > 1e-8f) ? atan2f(root[8], root[10]) : 0.0f;
        /* Skate's render_pose is in the SKATER's frame: while riding its deck sits at ~0 deg, but during a shove-it or
         * a 180 the pose's own deck spins (-180 deg) together with the physical deck body.  The rider's facing is
         * therefore host deck - pose deck: the board ends up exactly where the physics deck points and the body
         * keeps its own facing (heading = host deck alone turned the whole rider with every board spin).  Both angles
         * come from the same Skate snapshot, so they never disagree in time. */
        *heading = yaw - phi + yaw_off;
        static DWORD next;
        if (GetTickCount() >= next) { next = GetTickCount() + 1000;
            fprintf(stderr, "[SKATE-LIVE] pose deck %.0f deg, host deck %.0f deg -> heading %.0f deg\n",
                    phi * 57.2958f, yaw * 57.2958f, *heading * 57.2958f); }
    }
    if (frames == 1) fprintf(stderr, "[SKATE-LIVE] rider follows Skate 3 Rust's live pose (mount %u)\n", gen);
    return 1;
}

static void build_pose(const skate_actor_snapshot_t* snap)
{
    static float target[MAX_BONES * MATRIX_FLOATS];
    static float lean_pose[MAX_BONES * MATRIX_FLOATS];
    const u32 floats = s.header.bones * MATRIX_FLOATS;
    const unsigned variant = snap->clip >> 8;   /* skate_mod.cpp: trick variant in bits 8+ */
    unsigned clip = (snap->clip & 0xFFu) < CLIP_COUNT ? (snap->clip & 0xFFu) : CLIP_IDLE;
    if (clip == CLIP_LEAN_NEG || clip == CLIP_LEAN_POS || clip == CLIP_OLLIE_POP || clip >= CLIP_KICKFLIP_POP) clip = CLIP_IDLE;
    const int trick = clip == CLIP_OLLIE || (clip >= CLIP_KICKFLIP && clip <= CLIP_360FLIP);
    s_live_used = build_live_pose(target, &s_live_heading);
    if (s_live_used) clip = 0xFFFFu;   /* no clip blending; a fade from the last clip pose still runs once */
    else if (trick) sample_trick(clip, variant, snap->clip_time, target);
    else sample_clip(clip, snap->clip_time, target);

    /* Steering: lean the riding stance toward the turn.  The lean clips were
     * authored on the idle stance, so they only blend over idle/brake. */
    float lean = snap->lean;
    if (lean > 1.0f) lean = 1.0f;
    if (lean < -1.0f) lean = -1.0f;
    if ((clip == CLIP_IDLE || clip == CLIP_BRAKE) && fabsf(lean) > 0.02f) {
        sample_clip(lean < 0.0f ? CLIP_LEAN_NEG : CLIP_LEAN_POS, snap->clip_time, lean_pose);
        lerp_pose(target, target, lean_pose, fabsf(lean) * 0.85f, floats);
    }

    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (!s.have_pose) {
        memcpy(s.pose, target, floats * sizeof(float));
        s.have_pose = 1; s.last_clip = clip; s.fade_start.QuadPart = 0;
        return;
    }
    if (clip != s.last_clip) {
        memcpy(s.fade_from, s.pose, floats * sizeof(float));
        s.fade_start = now;
        s.last_clip = clip;
    }
    float fade = 1.0f;
    if (s.fade_start.QuadPart) {
        const float fade_seconds = trick ? 0.035f : CROSSFADE_SECONDS;
        fade = (float)((double)(now.QuadPart - s.fade_start.QuadPart) / (double)s.qpc_freq.QuadPart)
             / fade_seconds;
        if (fade >= 1.0f) { fade = 1.0f; s.fade_start.QuadPart = 0; }
    }
    if (fade < 1.0f) lerp_pose(s.pose, s.fade_from, target, fade, floats);
    else memcpy(s.pose, target, floats * sizeof(float));
}

int rsx_skate_actor_hides_player_draw(const float point[3])
{
    if (!g_hide_active) return 0;
    const float dx = point[0] - g_hide_position[0];
    const float dy = point[1] - g_hide_position[1];
    const float dz = point[2] - g_hide_position[2];
    const float horizontal = sqrtf(dx * dx + dy * dy);
    const float distance = sqrtf(horizontal * horizontal + dz * dz);
    if (distance < s.nearest_skinned) s.nearest_skinned = distance;
    if (horizontal > 100.0f || dz < -80.0f || dz > 250.0f) return 0;
    if (s.hidden_draws < 4)
        fprintf(stderr, "[SKATE-ACTOR] hiding Cole draw: skinned point=(%.0f %.0f %.0f) player=(%.0f %.0f %.0f)\n",
                point[0], point[1], point[2],
                g_hide_position[0], g_hide_position[1], g_hide_position[2]);
    s.hidden_draws++;
    s.hidden_this_frame++;
    return 1;
}

int rsx_skate_actor_get_snapshot(float position[3], float* heading,
                                 unsigned* clip, float* clip_time, float* lean)
{
    skate_actor_snapshot_t snap;
    if (!position || !infamous_skate_actor_snapshot(&snap)) return 0;
    position[0] = snap.position[0];
    position[1] = snap.position[1];
    position[2] = snap.position[2];
    if (heading) *heading = snap.heading;
    if (clip) *clip = snap.clip & 0xFFu;   /* without the trick variant bits */
    if (clip_time) *clip_time = snap.clip_time;
    if (lean) *lean = snap.lean;
    return 1;
}

static void skin_point(const float* m, const float p[3], float out[3])
{
    out[0] = m[0]*p[0] + m[4]*p[1] + m[8]*p[2] + m[12];
    out[1] = m[1]*p[0] + m[5]*p[1] + m[9]*p[2] + m[13];
    out[2] = m[2]*p[0] + m[6]*p[1] + m[10]*p[2] + m[14];
}

/* Cole reaches the renderer after the game's animation SPU has skinned him.
 * Convert each position to Skate model space, attach it to the four closest
 * authored skeleton landmarks, and blend the sampled retail skin matrices.
 * Cole therefore keeps his model/materials while following the real riding,
 * pushing, braking, landing, lean, ollie, and flip poses. */
int rsx_skate_actor_pose_cole_vertices(float* positions,
                                       size_t position_stride,
                                       u32 vertex_count)
{
    skate_actor_snapshot_t snap;
    if (!positions || position_stride < 12 || vertex_count < 3 ||
        !s.ready || !s.have_pose || !infamous_skate_actor_snapshot(&snap))
        return 0;

    float lo[3] = {1e30f, 1e30f, 1e30f};
    float hi[3] = {-1e30f, -1e30f, -1e30f};
    u32 near_count = 0;
    for (u32 i = 0; i < vertex_count; i++) {
        const float* p = (const float*)((const u8*)positions + (size_t)i * position_stride);
        if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2])) return 0;
        for (u32 k = 0; k < 3; k++) {
            if (p[k] < lo[k]) lo[k] = p[k];
            if (p[k] > hi[k]) hi[k] = p[k];
        }
        const float dx = p[0] - snap.position[0];
        const float dy = p[1] - snap.position[1];
        const float dz = p[2] - snap.position[2];
        if (dx*dx + dy*dy < 85.0f*85.0f && dz > -170.0f && dz < 80.0f)
            near_count++;
    }
    /* Reject terrain, props and screen-space draws. Cole is split into body
     * and clothing draws, so a small nearby draw is still legitimate. */
    if (!near_count || near_count * 10u < vertex_count * 9u ||
        hi[0]-lo[0] > 100.0f || hi[1]-lo[1] > 100.0f ||
        hi[2]-lo[2] > 200.0f)
        return 0;

    const float co = cosf(snap.heading), si = sinf(snap.heading);
    const float scale = s.scale != 0.0f ? s.scale : 100.0f;
    for (u32 i = 0; i < vertex_count; i++) {
        float* p = (float*)((u8*)positions + (size_t)i * position_stride);
        const float original[3] = {p[0], p[1], p[2]};
        const float dx = p[0] - snap.position[0];
        const float dy = p[1] - snap.position[1];
        /* Same basis as the actor shader: model +X is rider-left, +Y is up,
         * and +Z is the direction of travel. */
        const float model[3] = {
            (-dx*si + dy*co) / scale,
            (p[2] - snap.position[2] - s.z_offset) / scale,
            ( dx*co + dy*si) / scale,
        };

        /* Keep weights on one anatomical chain. A global nearest-landmark
         * search lets neighbouring vertices choose unrelated arm/head/leg
         * joints; adjacent triangles then stretch across the whole screen as
         * those joints move. The landmark ABI is ordered by chain. */
        static const u32 torso[] = {0, 1, 2, 3};
        static const u32 right_leg[] = {7, 6, 5, 4, 0};
        static const u32 left_leg[] = {11, 10, 9, 8, 0};
        static const u32 right_arm[] = {2, 12, 13, 14};
        static const u32 left_arm[] = {2, 15, 16, 17};
        const u32* chain = torso;
        u32 chain_count = (u32)(sizeof(torso) / sizeof(torso[0]));
        if (model[1] < 0.98f) {
            chain = model[0] < 0.0f ? right_leg : left_leg;
            chain_count = 5;
        } else if (model[1] < 1.58f && fabsf(model[0]) > 0.21f) {
            chain = model[0] < 0.0f ? right_arm : left_arm;
            chain_count = 4;
        }

        u32 best_i[2] = {chain[0], chain[0]};
        float best_d[2] = {1e30f, 1e30f};
        for (u32 c = 0; c < chain_count; c++) {
            const u32 j = chain[c];
            const float ax = model[0] - s.landmark[j].bind[0];
            const float ay = model[1] - s.landmark[j].bind[1];
            const float az = model[2] - s.landmark[j].bind[2];
            const float d = ax*ax + ay*ay + az*az;
            for (u32 slot = 0; slot < 2; slot++) if (d < best_d[slot]) {
                if (slot == 0) {
                    best_d[1] = best_d[0];
                    best_i[1] = best_i[0];
                }
                best_d[slot] = d;
                best_i[slot] = j;
                break;
            }
        }

        float deformed[3] = {0, 0, 0};
        float weight_sum = 0.0f;
        for (u32 slot = 0; slot < 2; slot++) {
            const u32 landmark = best_i[slot];
            const u32 joint = s.landmark[landmark].joint;
            float q[3];
            skin_point(s.pose + (size_t)joint * MATRIX_FLOATS, model, q);
            /* Blend only adjacent candidates on the selected body chain. */
            const float d = best_d[slot] + 0.0125f;
            const float w = 1.0f / (d * d);
            deformed[0] += q[0] * w;
            deformed[1] += q[1] * w;
            deformed[2] += q[2] * w;
            weight_sum += w;
        }
        if (weight_sum > 0.0f) {
            const float side = deformed[0] / weight_sum * scale;
            const float up = deformed[1] / weight_sum * scale + s.z_offset;
            const float forward = deformed[2] / weight_sum * scale;
            const float x = snap.position[0] + forward*co - side*si;
            const float y = snap.position[1] + forward*si + side*co;
            const float z = snap.position[2] + up;
            const float hx = x - snap.position[0], hy = y - snap.position[1];
            const float mx = x - original[0], my = y - original[1], mz = z - original[2];
            /* A guessed chain weight must never be allowed to turn one bad
             * vertex into a screen-wide triangle. Retail push/trick motion is
             * comfortably inside these limits; invalid transfers fall back
             * to the coherent mount-time Cole position. */
            if (hx*hx + hy*hy <= 100.0f*100.0f && up >= -170.0f && up <= 85.0f &&
                mx*mx + my*my + mz*mz <= 90.0f*90.0f) {
                p[0] = x;
                p[1] = y;
                p[2] = z;
            }
        }
    }
    static u32 reports;
    if (reports++ < 8)
        fprintf(stderr, "[SKATE-COLE] retail skeleton retarget: %u verts clip=%u time=%.2f lean=%.2f bounds %.0fx%.0fx%.0f relz %.0f..%.0f\n",
                vertex_count, snap.clip, snap.clip_time, snap.lean,
                hi[0]-lo[0], hi[1]-lo[1], hi[2]-lo[2],
                lo[2]-snap.position[2], hi[2]-snap.position[2]);
    return 1;
}

/* ===================== Cole bone-palette retarget =====================
 *
 * inFamous skins Cole on the SPUs from two guest arrays the animation job leaves in main memory:
 *   joints[247]  world joint matrices (row-vector 4x4, translation in row 3; [0] is an extra root)
 *   palette[246] skinning matrices = inverse-bind[j] * joints[j + 1]  (double-buffered: two arrays)
 * (found 2026-10-18 with the MEMCMD "joints"/"dump"/"jflat" tools: forcing a palette deforms Cole on screen).
 *
 * While Skate Mode is on, Cole's palette is replaced: the sampled Skate 3 pose (s.pose, the same pose the
 * board uses) is retargeted onto Cole's own rest skeleton -- recovered at mount time from palette * joints^-1 --
 * segment by segment (pelvis/chest frames, limb directions, head), and the result is written over the palette
 * each time an SPU PUT lands on it (g_spu_put_hook), so the game's own skinning draws Cole in the skate pose.
 * His joints (and so movement, collision and the camera) are untouched.  INFAMOUS_SKATE_COLE=0 disables it. */
extern uint8_t* vm_base;
extern void (*g_spu_put_hook)(uint32_t ea, uint32_t size);   /* runtime/spu/spu_channels.c; cellGcmSys uses it too */
static void (*cole_prev_hook)(uint32_t ea, uint32_t size);

enum { COLE_BONES = 246 };
enum { CJ_HIPS, CJ_SPINE, CJ_CHEST, CJ_NECK, CJ_HEAD, CJ_HEADTOP,
       CJ_LCLAV, CJ_LSHO, CJ_LELB, CJ_LWRI, CJ_LTIP, CJ_RCLAV, CJ_RSHO, CJ_RELB, CJ_RWRI, CJ_RTIP,
       CJ_LHIP, CJ_LKNEE, CJ_LANK, CJ_LTOE, CJ_LTT, CJ_RHIP, CJ_RKNEE, CJ_RANK, CJ_RTOE, CJ_RTT, CJ_COUNT };
/* Cole's skeleton indices (palette order) for those joints; CJ_HIPS = mean of the two hip joints. */
static const int k_cole_bone[CJ_COUNT] = { -1, 2, 10, 63, 79, 202,
    11, 12, 13, 14, 23, 37, 38, 39, 40, 49,
    99, 100, 101, 102, 103, 104, 105, 106, 107, 108 };
/* Skate landmark (pack table order) for each Cole joint, -1 = none. */
static const int k_skate_landmark[CJ_COUNT] = { 0, 19, 2, 18, 3, -1,
    21, 15, 16, 17, -1, 20, 12, 13, 14, -1,
    8, 9, 10, 11, -1, 4, 5, 6, 7, -1 };
enum { SG_PELVIS, SG_SPINE, SG_CHEST, SG_HEAD,
       SG_LCLAV, SG_LSHO, SG_LELB, SG_LWRI, SG_RCLAV, SG_RSHO, SG_RELB, SG_RWRI,
       SG_LHIP, SG_LKNEE, SG_LANK, SG_LTOE, SG_RHIP, SG_RKNEE, SG_RANK, SG_RTOE, SG_COUNT };
static const int k_seg_line[SG_COUNT][2] = {
    {CJ_HIPS, CJ_SPINE}, {CJ_SPINE, CJ_CHEST}, {CJ_CHEST, CJ_NECK}, {CJ_NECK, CJ_HEADTOP},
    {CJ_LCLAV, CJ_LSHO}, {CJ_LSHO, CJ_LELB}, {CJ_LELB, CJ_LWRI}, {CJ_LWRI, CJ_LTIP},
    {CJ_RCLAV, CJ_RSHO}, {CJ_RSHO, CJ_RELB}, {CJ_RELB, CJ_RWRI}, {CJ_RWRI, CJ_RTIP},
    {CJ_LHIP, CJ_LKNEE}, {CJ_LKNEE, CJ_LANK}, {CJ_LANK, CJ_LTOE}, {CJ_LTOE, CJ_LTT},
    {CJ_RHIP, CJ_RKNEE}, {CJ_RKNEE, CJ_RANK}, {CJ_RANK, CJ_RTOE}, {CJ_RTOE, CJ_RTT} };

typedef struct { double R[3][3]; double o_rest[3], o_new[3]; } cole_seg;
static struct {
    int enabled;                 /* INFAMOUS_SKATE_COLE (default on) */
    int found;                   /* arrays located + rest skeleton known */
    u32 joints, pal[2];          /* guest addresses */
    double ib[COLE_BONES][4][4]; /* inverse bind (row-vector) */
    double rest[CJ_COUNT][3];    /* Cole rest joints, model units */
    unsigned char seg_of[COLE_BONES];
    double skate_scale;          /* Cole model units per Skate metre */
    u8 out[2][COLE_BONES * 64];  /* big-endian palettes ready to copy (fallback writer) */
    double seg_A[2][SG_COUNT][4][4];   /* model rest -> model animated, per segment (row-vector) */
    double heading[2];
    double game_root[4][4];      /* the game's own root skin (model -> world) from its latest palette write */
    volatile LONG have_game_root;
    volatile float lift;         /* guaranteed ollie arc (cm) on top of the game's own jump */
    float takeoff_z, last_clip_time; unsigned last_clip;
    volatile LONG pub;           /* index of the newest out[] */
    volatile LONG active;
    volatile LONG hits;
    u32 frames_active, last_scan_frame;
    int writer_started;
    float feet[3], world_cm_per_m;
    int have_root;
} cole;

static inline float gbe_f(u32 ea)
{
    const u8* b = vm_base + ea;
    const u32 u = ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
    float f; memcpy(&f, &u, 4); return f;
}
static inline u32 gbe_u(u32 ea)
{
    const u8* b = vm_base + ea;
    return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
}
static void g_mat(u32 ea, double m[4][4])
{
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) m[r][c] = gbe_f(ea + (u32)(r * 16 + c * 4));
}
static void mat_mul(const double a[4][4], const double b[4][4], double o[4][4])
{
    double t[4][4];
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++)
        t[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c] + a[r][3] * b[3][c];
    memcpy(o, t, sizeof t);
}
/* inverse of a row-vector affine (last column 0,0,0,1) */
static int aff_inv(const double m[4][4], double o[4][4])
{
    const double a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2],
                 g = m[2][0], h = m[2][1], i = m[2][2];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (fabs(det) < 1e-12) return 0;
    double r[3][3] = { {(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det},
                       {(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det},
                       {(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det} };
    memset(o, 0, sizeof(double) * 16);
    for (int x = 0; x < 3; x++) for (int y = 0; y < 3; y++) o[x][y] = r[x][y];
    for (int y = 0; y < 3; y++) o[3][y] = -(m[3][0] * r[0][y] + m[3][1] * r[1][y] + m[3][2] * r[2][y]);
    o[3][3] = 1.0;
    return 1;
}
static int is_matrix_near(u32 ea, const float p[3], float radius)
{
    /* compare as floats: the joint array stores some zeros as -0.0 */
    if (gbe_f(ea + 60) != 1.0f || gbe_f(ea + 12) != 0.0f || gbe_f(ea + 28) != 0.0f || gbe_f(ea + 44) != 0.0f) return 0;
    return fabsf(gbe_f(ea + 48) - p[0]) < radius && fabsf(gbe_f(ea + 52) - p[1]) < radius &&
           fabsf(gbe_f(ea + 56) - p[2]) < radius;
}

/* Find runs of >= 200 consecutive 64-byte matrices on Cole: 2 palettes (246) + the joint array (247). */
static int cole_locate(const float pos[3])
{
    u32 runs[8], lens[8]; int n = 0;
    for (unsigned long long a = 0x30000000ull; a < 0x60000000ull && n < 8; ) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(vm_base + a, &mi, sizeof mi)) break;
        unsigned long long end = (unsigned long long)((u8*)mi.BaseAddress - vm_base) + mi.RegionSize;
        if (end > 0x60000000ull) end = 0x60000000ull;
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            for (unsigned long long e = (a + 15) & ~15ull; e + 64 <= end && n < 8; e += 16) {
                if (!is_matrix_near((u32)e, pos, 320.0f)) continue;
                u32 len = 0;
                while (e + 64ull * (len + 1) <= end && is_matrix_near((u32)(e + 64ull * len), pos, 320.0f)) len++;
                if (len >= 200) { runs[n] = (u32)e; lens[n] = len; n++; }
                e += 64ull * (len ? len : 1) - 16;
            }
        }
        if (end <= a) break;
        a = end;
    }
    int w = -1, p0 = -1, p1 = -1;
    for (int i = 0; i < n; i++) {
        if (lens[i] >= 247 && w < 0) w = i;
        else if (lens[i] == 246) { if (p0 < 0) p0 = i; else if (p1 < 0) p1 = i; }
    }
    fprintf(stderr, "[SKATE-COLE] scan: %d matrix runs near Cole", n);
    for (int i = 0; i < n; i++) fprintf(stderr, " 0x%08X x%u", runs[i], lens[i]);
    fprintf(stderr, "\n");
    if (w < 0 || p0 < 0 || p1 < 0) return 0;
    cole.joints = runs[w]; cole.pal[0] = runs[p0]; cole.pal[1] = runs[p1];
    return 1;
}

static void v3_sub(const double a[3], const double b[3], double o[3]) { o[0]=a[0]-b[0]; o[1]=a[1]-b[1]; o[2]=a[2]-b[2]; }
static double v3_len(const double a[3]) { return sqrt(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]); }
static void v3_norm(double a[3]) { const double l = v3_len(a); if (l > 1e-12) { a[0]/=l; a[1]/=l; a[2]/=l; } }
static void v3_cross(const double a[3], const double b[3], double o[3])
{ const double x=a[1]*b[2]-a[2]*b[1], y=a[2]*b[0]-a[0]*b[2], z=a[0]*b[1]-a[1]*b[0]; o[0]=x; o[1]=y; o[2]=z; }
static void m3_vec(const double R[3][3], const double v[3], double o[3])
{ const double x=R[0][0]*v[0]+R[0][1]*v[1]+R[0][2]*v[2], y=R[1][0]*v[0]+R[1][1]*v[1]+R[1][2]*v[2],
  z=R[2][0]*v[0]+R[2][1]*v[1]+R[2][2]*v[2]; o[0]=x; o[1]=y; o[2]=z; }
static void m3_mul(const double A[3][3], const double B[3][3], double O[3][3])
{ double t[3][3]; for(int r=0;r<3;r++) for(int c=0;c<3;c++) t[r][c]=A[r][0]*B[0][c]+A[r][1]*B[1][c]+A[r][2]*B[2][c];
  memcpy(O,t,sizeof t); }
/* rows = (forward, side, up) */
static void frame3(const double up_in[3], const double side_in[3], double F[3][3])
{
    double up[3] = {up_in[0], up_in[1], up_in[2]}, fwd[3], side[3];
    v3_norm(up); v3_cross(side_in, up, fwd); v3_norm(fwd); v3_cross(up, fwd, side);
    for (int k = 0; k < 3; k++) { F[0][k] = fwd[k]; F[1][k] = side[k]; F[2][k] = up[k]; }
}
/* R (column convention) mapping Cole's rest frame Fc onto the animated frame Fs: R = Fs^T Fc */
static void frame_rot(double Fs[3][3], double Fc[3][3], double R[3][3])
{
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++)
        R[r][c] = Fs[0][r] * Fc[0][c] + Fs[1][r] * Fc[1][c] + Fs[2][r] * Fc[2][c];
}
/* smallest rotation (column convention) taking direction a onto direction b */
static void minrot(const double a_in[3], const double b_in[3], double R[3][3])
{
    double a[3] = {a_in[0], a_in[1], a_in[2]}, b[3] = {b_in[0], b_in[1], b_in[2]}, v[3];
    v3_norm(a); v3_norm(b); v3_cross(a, b, v);
    const double c = a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
    if (c < -0.9999) { memset(R, 0, sizeof(double) * 9); R[0][0] = R[1][1] = R[2][2] = -1.0; return; }
    const double K[3][3] = {{0, -v[2], v[1]}, {v[2], 0, -v[0]}, {-v[1], v[0], 0}};
    double K2[3][3]; m3_mul(K, K, K2);
    for (int r = 0; r < 3; r++) for (int cc = 0; cc < 3; cc++)
        R[r][cc] = (r == cc ? 1.0 : 0.0) + K[r][cc] + K2[r][cc] / (1.0 + c);
}

/* Rotation halfway between A and B: A * (A^T B)^(1/2), via axis-angle. */
static void rot_halfway(const double A[3][3], const double B[3][3], double O[3][3])
{
    double Q[3][3];
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++)
        Q[r][c] = A[0][r]*B[0][c] + A[1][r]*B[1][c] + A[2][r]*B[2][c];
    double cs = (Q[0][0] + Q[1][1] + Q[2][2] - 1.0) * 0.5;
    if (cs > 1.0) cs = 1.0; if (cs < -1.0) cs = -1.0;
    const double ang = acos(cs);
    double ax[3] = {Q[2][1] - Q[1][2], Q[0][2] - Q[2][0], Q[1][0] - Q[0][1]};
    const double n = v3_len(ax);
    if (n < 1e-9 || ang < 1e-6) { memcpy(O, A, sizeof(double) * 9); return; }
    for (int k = 0; k < 3; k++) ax[k] /= n;
    const double h = 0.5 * ang, c = cos(h), s_ = sin(h), ic = 1.0 - c;
    const double H[3][3] = {
        {c + ax[0]*ax[0]*ic,       ax[0]*ax[1]*ic - ax[2]*s_, ax[0]*ax[2]*ic + ax[1]*s_},
        {ax[1]*ax[0]*ic + ax[2]*s_, c + ax[1]*ax[1]*ic,       ax[1]*ax[2]*ic - ax[0]*s_},
        {ax[2]*ax[0]*ic - ax[1]*s_, ax[2]*ax[1]*ic + ax[0]*s_, c + ax[2]*ax[2]*ic}};
    m3_mul(A, H, O);
}

/* Rest skeleton from the live arrays (before anything is overridden). */
static int cole_learn_rest(void)
{
    double W[COLE_BONES + 1][4][4];
    for (int j = 0; j <= COLE_BONES; j++) g_mat(cole.joints + 64u * (u32)j, W[j]);
    int best = -1; double best_err = 1e30;
    static double ib_try[2][COLE_BONES][4][4];
    for (int p = 0; p < 2; p++) {
        for (int j = 0; j < COLE_BONES; j++) {
            double S[4][4], Wi[4][4];
            g_mat(cole.pal[p] + 64u * (u32)j, S);
            if (!aff_inv(W[j + 1], Wi)) return 0;
            mat_mul(S, Wi, ib_try[p][j]);
        }
        /* the root bone's inverse bind is the identity: use it to pick the palette that matches joints */
        double err = 0;
        for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++)
            err += fabs(ib_try[p][0][r][c] - (r == c ? 1.0 : 0.0)) * (r == 3 ? 0.01 : 1.0);
        if (err < best_err) { best_err = err; best = p; }
    }
    memcpy(cole.ib, ib_try[best], sizeof cole.ib);
    double bindpos[COLE_BONES][3];
    for (int j = 0; j < COLE_BONES; j++) {
        double B[4][4];
        if (!aff_inv(cole.ib[j], B)) return 0;
        bindpos[j][0] = B[3][0]; bindpos[j][1] = B[3][1]; bindpos[j][2] = B[3][2];
    }
    for (int k = 0; k < CJ_COUNT; k++) {
        if (k == CJ_HIPS) {
            for (int c = 0; c < 3; c++) cole.rest[k][c] = 0.5 * (bindpos[99][c] + bindpos[104][c]);
        } else for (int c = 0; c < 3; c++) cole.rest[k][c] = bindpos[k_cole_bone[k]][c];
    }
    /* sanity: Cole stands ~155 model units tall, T-pose facing +X, left = +Y */
    const double h = cole.rest[CJ_HEADTOP][2], hips = cole.rest[CJ_HIPS][2], span = cole.rest[CJ_LWRI][1] - cole.rest[CJ_RWRI][1];
    fprintf(stderr, "[SKATE-COLE] rest skeleton from palette %d (root err %.4f): head %.1f hips %.1f wrist span %.1f\n",
            best, best_err, h, hips, span);
    if (!(h > 120 && h < 200 && hips > 60 && hips < 120 && span > 60)) return 0;
    /* each bone follows the body segment its pivot lies on (joint pivots belong to the segment they start) */
    for (int j = 0; j < COLE_BONES; j++) {
        int bestk = SG_CHEST; double bd = 1e30;
        for (int k = 0; k < SG_COUNT; k++) {
            const double* a = cole.rest[k_seg_line[k][0]]; const double* b = cole.rest[k_seg_line[k][1]];
            double ab[3], ap[3]; v3_sub(b, a, ab); v3_sub(bindpos[j], a, ap);
            const double L2 = ab[0]*ab[0] + ab[1]*ab[1] + ab[2]*ab[2];
            double t = L2 > 1e-9 ? (ap[0]*ab[0] + ap[1]*ab[1] + ap[2]*ab[2]) / L2 : 0.0;
            if (t < 0) t = 0; if (t > 1) t = 1;
            double q[3] = {a[0] + ab[0]*t - bindpos[j][0], a[1] + ab[1]*t - bindpos[j][1], a[2] + ab[2]*t - bindpos[j][2]};
            const double d = v3_len(q) + 0.05 * t;
            if (d < bd) { bd = d; bestk = k; }
        }
        if (j >= 231) bestk = SG_CHEST;   /* backpack straps hang off the chest */
        cole.seg_of[j] = (unsigned char)bestk;
    }
    return 1;
}

/* Animated Skate joint (landmark) in Cole model axes/units: GLB (x left, y up, z travel) -> (travel, left, up) */
static void skate_joint(int landmark, double o[3])
{
    float q[3];
    skin_point(s.pose + (size_t)s.landmark[landmark].joint * MATRIX_FLOATS, s.landmark[landmark].bind, q);
    o[0] = q[2] * cole.skate_scale; o[1] = q[0] * cole.skate_scale; o[2] = q[1] * cole.skate_scale;
}

static void cole_solve(cole_seg seg[SG_COUNT])
{
    double S[CJ_COUNT][3]; int has[CJ_COUNT];
    for (int k = 0; k < CJ_COUNT; k++) { has[k] = k_skate_landmark[k] >= 0; if (has[k]) skate_joint(k_skate_landmark[k], S[k]); }
    const double (*C)[3] = (const double (*)[3])cole.rest;
    double a[3], b[3], Fc[3][3], Fs[3][3], Rp[3][3], Rc[3][3], v[3];
    v3_sub(C[CJ_SPINE], C[CJ_HIPS], a); v3_sub(C[CJ_LHIP], C[CJ_RHIP], b); frame3(a, b, Fc);
    v3_sub(S[CJ_SPINE], S[CJ_HIPS], a); v3_sub(S[CJ_LHIP], S[CJ_RHIP], b); frame3(a, b, Fs);
    frame_rot(Fs, Fc, Rp);
    v3_sub(C[CJ_NECK], C[CJ_CHEST], a); v3_sub(C[CJ_LSHO], C[CJ_RSHO], b); frame3(a, b, Fc);
    v3_sub(S[CJ_NECK], S[CJ_CHEST], a); v3_sub(S[CJ_LSHO], S[CJ_RSHO], b); frame3(a, b, Fs);
    frame_rot(Fs, Fc, Rc);
#define SEG(i, R_, rest_, newp_) do { memcpy(seg[i].R, R_, sizeof(double)*9); \
        memcpy(seg[i].o_rest, rest_, sizeof(double)*3); memcpy(seg[i].o_new, newp_, sizeof(double)*3); } while (0)
    double hips[3] = {S[CJ_HIPS][0], S[CJ_HIPS][1], S[CJ_HIPS][2]};
    SEG(SG_PELVIS, Rp, C[CJ_HIPS], hips);
    double spine[3]; v3_sub(C[CJ_SPINE], C[CJ_HIPS], v); m3_vec(Rp, v, spine);
    for (int c = 0; c < 3; c++) spine[c] += hips[c];
    /* the spine bends between pelvis and chest: give the middle segment half of the rotation between them */
    double Rs[3][3]; rot_halfway(Rp, Rc, Rs);
    SEG(SG_SPINE, Rs, C[CJ_SPINE], spine);
    double chest[3]; v3_sub(C[CJ_CHEST], C[CJ_SPINE], v); m3_vec(Rs, v, chest);
    for (int c = 0; c < 3; c++) chest[c] += spine[c];
    SEG(SG_CHEST, Rc, C[CJ_CHEST], chest);
    double neck[3]; v3_sub(C[CJ_NECK], C[CJ_CHEST], v); m3_vec(Rc, v, neck);
    for (int c = 0; c < 3; c++) neck[c] += chest[c];
    {   double restv[3], animv[3], M[3][3], Rh[3][3];
        v3_sub(C[CJ_HEAD], C[CJ_NECK], restv); m3_vec(Rc, restv, restv);
        v3_sub(S[CJ_HEAD], S[CJ_NECK], animv); minrot(restv, animv, M); m3_mul(M, Rc, Rh);
        SEG(SG_HEAD, Rh, C[CJ_NECK], neck); }
    for (int side = 0; side < 2; side++) {
        const int clav = side ? CJ_RCLAV : CJ_LCLAV;
        const int arm[4] = {clav, clav + 1, clav + 2, clav + 3};             /* CLAV SHO ELB WRI */
        const int armseg = side ? SG_RCLAV : SG_LCLAV;
        double R[3][3]; memcpy(R, Rc, sizeof R);
        double pos[3]; v3_sub(C[clav], C[CJ_CHEST], v); m3_vec(Rc, v, pos);
        for (int c = 0; c < 3; c++) pos[c] += chest[c];
        for (int i = 0; i < 3; i++) {
            double restv[3], rr[3], animv[3], M[3][3];
            v3_sub(C[arm[i + 1]], C[arm[i]], restv);
            if (has[arm[i]] && has[arm[i + 1]]) {
                m3_vec(R, restv, rr); v3_sub(S[arm[i + 1]], S[arm[i]], animv); minrot(rr, animv, M); m3_mul(M, R, R);
            }
            SEG(armseg + i, R, C[arm[i]], pos);
            m3_vec(R, restv, rr); for (int c = 0; c < 3; c++) pos[c] += rr[c];
        }
        SEG(armseg + 3, R, C[arm[3]], pos);                                   /* hand rides the forearm */
        const int hip = side ? CJ_RHIP : CJ_LHIP;
        const int leg[4] = {hip, hip + 1, hip + 2, hip + 3};                 /* HIP KNEE ANK TOE */
        const int legseg = side ? SG_RHIP : SG_LHIP;
        /* Two-bone IK: Cole's legs are not the Skate rider's length, so copying joint angles leaves his feet
         * off the deck.  Pin the ankle to the rider's ankle (on the board) and bend the knee in the rider's
         * knee plane; then aim the foot along the rider's foot. */
        double Hn[3]; v3_sub(C[hip], C[CJ_HIPS], v); m3_vec(Rp, v, Hn);
        for (int c = 0; c < 3; c++) Hn[c] += hips[c];
        double thigh_rest[3], shin_rest[3];
        v3_sub(C[leg[1]], C[leg[0]], thigh_rest); v3_sub(C[leg[2]], C[leg[1]], shin_rest);
        const double la = v3_len(thigh_rest), lb = v3_len(shin_rest);
        double dir[3]; v3_sub(S[leg[2]], Hn, dir);
        double d = v3_len(dir); v3_norm(dir);
        const double dmax = (la + lb) * 0.9995, dmin = fabs(la - lb) + 1e-3;
        if (d > dmax) d = dmax;
        if (d < dmin) d = dmin;
        double pole[3]; v3_sub(S[leg[1]], Hn, pole);                          /* rider's knee: bend plane */
        { const double t = pole[0]*dir[0] + pole[1]*dir[1] + pole[2]*dir[2];
          for (int c = 0; c < 3; c++) pole[c] -= t * dir[c]; }
        if (v3_len(pole) < 1e-6) { double fwd[3] = {Rp[0][0], Rp[1][0], Rp[2][0]};   /* fallback: pelvis forward */
            const double t = fwd[0]*dir[0] + fwd[1]*dir[1] + fwd[2]*dir[2];
            for (int c = 0; c < 3; c++) pole[c] = fwd[c] - t * dir[c]; }
        v3_norm(pole);
        const double x = (la*la - lb*lb + d*d) / (2.0 * d);
        const double y = sqrt(fmax(la*la - x*x, 0.0));
        double K[3], A_[3];
        for (int c = 0; c < 3; c++) { K[c] = Hn[c] + dir[c]*x + pole[c]*y; A_[c] = Hn[c] + dir[c]*d; }
        double rr[3], animv[3], M_[3][3];
        memcpy(R, Rp, sizeof R);
        m3_vec(R, thigh_rest, rr); v3_sub(K, Hn, animv); minrot(rr, animv, M_); m3_mul(M_, R, R);
        SEG(legseg + 0, R, C[leg[0]], Hn);
        m3_vec(R, shin_rest, rr); v3_sub(A_, K, animv); minrot(rr, animv, M_); m3_mul(M_, R, R);
        SEG(legseg + 1, R, C[leg[1]], K);
        { double foot_rest[3]; v3_sub(C[leg[3]], C[leg[2]], foot_rest);
          if (has[leg[2]] && has[leg[3]]) {
              m3_vec(R, foot_rest, rr); v3_sub(S[leg[3]], S[leg[2]], animv); minrot(rr, animv, M_); m3_mul(M_, R, R); }
          SEG(legseg + 2, R, C[leg[2]], A_);
          m3_vec(R, foot_rest, rr); for (int c = 0; c < 3; c++) pos[c] = A_[c] + rr[c]; }
        SEG(legseg + 3, R, C[leg[3]], pos);                                   /* toes ride the foot */
    }
#undef SEG
}

static void put_be(u8* d, float f) { u32 u; memcpy(&u, &f, 4); d[0] = (u8)(u >> 24); d[1] = (u8)(u >> 16); d[2] = (u8)(u >> 8); d[3] = (u8)u; }

static void put_be(u8* d, float f);
static void cole_apply_palette(u32 dst)
{
    const LONG k = InterlockedCompareExchange(&cole.pub, 0, 0);
    memcpy(vm_base + dst, cole.out[k], sizeof cole.out[k]);
}
/* Build our palette straight into guest memory against the root the game just wrote (no frame lag). */
static void cole_write_palette(u32 dst)
{
    double root[4][4];
    g_mat(dst, root);                        /* game's skin for bone 0 = inverse bind(root) * root joint */
    if (!(root[3][3] == 1.0)) { cole_apply_palette(dst); return; }
    memcpy(cole.game_root, root, sizeof root);
    InterlockedExchange(&cole.have_game_root, 1);
    const LONG k = InterlockedCompareExchange(&cole.pub, 0, 0);
    const double sc = sqrt(root[0][0]*root[0][0] + root[0][1]*root[0][1] + root[0][2]*root[0][2]);
    const double ch = cos(cole.heading[k]), sh = sin(cole.heading[k]);
    const double M[4][4] = { { sc*ch, sc*sh, 0, 0}, {-sc*sh, sc*ch, 0, 0}, {0, 0, sc, 0},
                             { root[3][0], root[3][1], root[3][2] + cole.lift, 1 } };
    u8 seg_be[SG_COUNT][64];
    for (int g = 0; g < SG_COUNT; g++) {
        double A[4][4];
        mat_mul(cole.seg_A[k][g], M, A);
        for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++)
            put_be(seg_be[g] + r * 16 + c * 4, (float)(c == 3 ? (r == 3 ? 1.0 : 0.0) : A[r][c]));
    }
    u8* d = vm_base + dst;
    for (int j = 0; j < COLE_BONES; j++) memcpy(d + j * 64, seg_be[cole.seg_of[j]], 64);
}
static void cole_put_hook(uint32_t ea, uint32_t size)
{
    if (cole_prev_hook) cole_prev_hook(ea, size);
    /* g_skate_render_hooks drops the moment the rider steps off (the renderer then stops calling us), so
     * the game's own palette is back on the very next SPU write. */
    if (!cole.active) return;
    if (!g_skate_render_hooks) { cole.active = 0; return; }
    for (int p = 0; p < 2; p++) {
        const u32 a = cole.pal[p], e = a + COLE_BONES * 64u;
        if (ea < e && ea + size > a) {
            cole_write_palette(a);
            const LONG n = InterlockedIncrement(&cole.hits);
            if (n <= 12) fprintf(stderr, "[SKATE-COLE] palette PUT ea=0x%08X size=0x%X (array %d at 0x%08X)\n", ea, size, p, a);
        }
    }
}
static DWORD WINAPI cole_writer_thread(LPVOID unused)
{   /* fallback when the palette is not written by an SPU PUT: keep it overwritten */
    (void)unused;
    for (;;) {
        if (!cole.active || !g_skate_render_hooks) { Sleep(4); continue; }
        cole_apply_palette(cole.pal[0]); cole_apply_palette(cole.pal[1]);
        Sleep(0);
    }
    return 0;
}

/* Per presented frame, after build_pose(): (re)locate Cole, retarget, publish. */
static void cole_update(const skate_actor_snapshot_t* snap)
{
    if (!cole.enabled || !vm_base) return;
    const float pos[3] = {snap->position[0], snap->position[1], snap->position[2]};
    if (cole.found && !is_matrix_near(cole.pal[0], pos, 400.0f) && !is_matrix_near(cole.pal[1], pos, 400.0f)) {
        cole.found = 0; cole.active = 0;
        fprintf(stderr, "[SKATE-COLE] Cole's palette moved; searching again\n");
    }
    if (!cole.found) {
        static int tries = 0;
        if (tries >= 6 || (cole.last_scan_frame && s.frames_drawn - cole.last_scan_frame < 120)) return;
        cole.last_scan_frame = s.frames_drawn ? s.frames_drawn : 1; tries++;
        if (!cole_locate(pos) || !cole_learn_rest()) {
            fprintf(stderr, "[SKATE-COLE] Cole's bone arrays not found yet; board only\n");
            return;
        }
        const double skate_hips = s.landmark[0].bind[1];
        cole.skate_scale = skate_hips > 0.1 ? cole.rest[CJ_HIPS][2] / skate_hips : 93.5;
        cole.found = 1;
        if (g_spu_put_hook != cole_put_hook) { cole_prev_hook = g_spu_put_hook; g_spu_put_hook = cole_put_hook; }
        fprintf(stderr, "[SKATE-COLE] joints 0x%08X, palettes 0x%08X / 0x%08X; %.1f model units per Skate metre\n",
                cole.joints, cole.pal[0], cole.pal[1], cole.skate_scale);
    }
    /* model -> world: the game's own root (inverse bind of the root bone * root joint), with the board heading */
    double W1[4][4], root[4][4];
    if (cole.have_game_root) memcpy(root, cole.game_root, sizeof root);
    else { g_mat(cole.joints + 64u, W1); mat_mul(cole.ib[0], W1, root); }
    const double sc = sqrt(root[0][0]*root[0][0] + root[0][1]*root[0][1] + root[0][2]*root[0][2]);
    const double ch = cos(snap->heading), sh = sin(snap->heading);
    /* Guaranteed ollie arc: inFamous's own jump does not always fire (it depends on Cole's state), and a
     * trick must leave the ground every time.  The arc is added only where the game's rise falls short. */
    {
        const unsigned c = snap->clip & 0xFFu;   /* without the trick variant bits */
        const int trick = c == 2 || (c >= 7 && c <= 10);
        if (trick && (c != cole.last_clip || snap->clip_time < cole.last_clip_time)) cole.takeoff_z = (float)root[3][2];
        cole.last_clip = c; cole.last_clip_time = snap->clip_time;
        float lift = 0.0f;
        if (trick) {
            const float u = (snap->clip_time - 0.12f) / 0.62f;
            if (u > 0.0f && u < 1.0f) {
                const float arc = 45.0f * sinf(3.14159265f * u);
                const float rise = (float)root[3][2] - cole.takeoff_z;
                lift = arc - rise; if (lift < 0.0f) lift = 0.0f;
            }
        }
        /* Ground clamp: the clips pivot the board about its centre (Skate's physics lifts it), so a manual or a
         * pop would push the tail into the street.  Raise board and rider until the lowest wheel/tail point is
         * on the ground again. */
        const float ground = (s.ground_ref - board_min_y(s.pose)) * (float)(sc * cole.skate_scale);
        if (ground > lift) lift = ground;
        cole.lift = lift;
    }
    const double M[4][4] = { { sc*ch, sc*sh, 0, 0}, {-sc*sh, sc*ch, 0, 0}, {0, 0, sc, 0},
                             { root[3][0], root[3][1], root[3][2] + cole.lift, 1 } };
    cole.feet[0] = (float)root[3][0]; cole.feet[1] = (float)root[3][1]; cole.feet[2] = (float)root[3][2];
    cole.world_cm_per_m = (float)(sc * cole.skate_scale);
    cole.have_root = 1;

    cole_seg seg[SG_COUNT];
    cole_solve(seg);
    double A[SG_COUNT][4][4];
    double (*A_dbg)[4][4] = A;
    for (int k = 0; k < SG_COUNT; k++) {
        double T[4][4] = {{0}};
        for (int i = 0; i < 3; i++) for (int c = 0; c < 3; c++) T[i][c] = seg[k].R[c][i];   /* row-vector = R^T */
        for (int c = 0; c < 3; c++)
            T[3][c] = seg[k].o_new[c] - (seg[k].o_rest[0]*seg[k].R[c][0] + seg[k].o_rest[1]*seg[k].R[c][1] + seg[k].o_rest[2]*seg[k].R[c][2]);
        T[3][3] = 1.0;
        mat_mul(T, M, A[k]);
    }
    const LONG next = 1 - InterlockedCompareExchange(&cole.pub, 0, 0);
    for (int k = 0; k < SG_COUNT; k++) {
        double T[4][4] = {{0}};
        for (int i = 0; i < 3; i++) for (int c = 0; c < 3; c++) T[i][c] = seg[k].R[c][i];
        for (int c = 0; c < 3; c++)
            T[3][c] = seg[k].o_new[c] - (seg[k].o_rest[0]*seg[k].R[c][0] + seg[k].o_rest[1]*seg[k].R[c][1] + seg[k].o_rest[2]*seg[k].R[c][2]);
        T[3][3] = 1.0;
        memcpy(cole.seg_A[next][k], T, sizeof T);
    }
    cole.heading[next] = snap->heading;
    u8* o = cole.out[next];
    for (int j = 0; j < COLE_BONES; j++) {
        const double (*m)[4] = A[cole.seg_of[j]];
        for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++)
            put_be(o + j * 64 + r * 16 + c * 4, (float)(c == 3 ? (r == 3 ? 1.0 : 0.0) : m[r][c]));
    }
    InterlockedExchange(&cole.pub, next);
    if (!cole.active) {
        cole.active = 1; cole.frames_active = 0; cole.hits = 0;
        fprintf(stderr, "[SKATE-COLE] Cole now follows the Skate animations\n");
    }
    cole.frames_active++;
    if (cole.frames_active == 120 && cole.hits == 0 && !cole.writer_started) {
        cole.writer_started = 1;
        CreateThread(NULL, 0, cole_writer_thread, NULL, 0, NULL);
        fprintf(stderr, "[SKATE-COLE] no SPU PUT hit the palette in 120 frames; using the overwrite thread\n");
    }
    if (cole.frames_active % 120 == 1) {
        /* world positions of key joints in our pose (rest joint * segment * M) */
        static const int keys[] = {CJ_HIPS, CJ_LHIP, CJ_RHIP, CJ_LSHO, CJ_RSHO, CJ_HEAD, CJ_LTOE, CJ_RTOE};
        static const int key_seg[] = {SG_PELVIS, SG_LHIP, SG_RHIP, SG_LSHO, SG_RSHO, SG_HEAD, SG_LTOE, SG_RTOE};
        static const char* key_name[] = {"hips", "Lhip", "Rhip", "Lsho", "Rsho", "head", "Ltoe", "Rtoe"};
        fprintf(stderr, "[SKATE-COLE] f%u clip %u heading %.0f deg board fwd (%.2f %.2f) root (%.0f %.0f %.0f):",
                cole.frames_active, snap->clip, snap->heading * 57.2958, ch, sh, M[3][0], M[3][1], M[3][2]);
        for (int i = 0; i < 8; i++) {
            const double* p = cole.rest[keys[i]]; const double (*A)[4] = (const double (*)[4])A_dbg[key_seg[i]];
            const double w[3] = { p[0]*A[0][0] + p[1]*A[1][0] + p[2]*A[2][0] + A[3][0] - M[3][0],
                                  p[0]*A[0][1] + p[1]*A[1][1] + p[2]*A[2][1] + A[3][1] - M[3][1],
                                  p[0]*A[0][2] + p[1]*A[1][2] + p[2]*A[2][2] + A[3][2] - M[3][2] };
            fprintf(stderr, " %s(%.0f %.0f %.0f)", key_name[i], w[0], w[1], w[2]);
        }
        fprintf(stderr, "\n");
    }
    if (cole.frames_active == 300)
        fprintf(stderr, "[SKATE-COLE] palette replaced on %ld SPU writes in 300 frames\n", (long)cole.hits);
}
static void cole_deactivate(void)
{
    if (cole.active) fprintf(stderr, "[SKATE-COLE] Cole back to his own animation\n");
    cole.active = 0;
}
/* =================== end Cole bone-palette retarget =================== */

void rsx_skate_actor_draw(void* device_ptr, void* list_ptr, u64 rtv_value,
                          u32 width, u32 height, const float camera[16],
                          const float viewport_xf[8])
{
    skate_actor_snapshot_t snap;
    const int have_local = infamous_skate_actor_snapshot(&snap);
    /* Row 3 of the view-projection gives clip w = distance along the view
     * direction, so its XY part is the camera's horizontal forward vector. */
    infamous_skate_camera_forward(camera[12], camera[13]);
    if (!s.attempted) {
        const char* hide = getenv("INFAMOUS_SKATE_HIDE_COLE");
        s.hide_enabled = !(hide && *hide == '0');
        const char* ce = getenv("INFAMOUS_SKATE_COLE");
        cole.enabled = !(ce && *ce == '0');
    }
    if (!have_local) {
        cole_deactivate();
        g_hide_active = 0;
        s.have_pose = 0;
    }
    if (!have_local) return;
    int draw_local = have_local;
    /* the board is being drawn inside the scene pass: nothing to overlay (and the record side, not this thread,
     * creates the resources, so the two never initialise at once) */
    { static int in_scene = -1;
      if (in_scene < 0) { const char* e = getenv("INFAMOUS_SKATE_IN_SCENE"); in_scene = !(e && *e == '0'); }
      static u32 waited = 0;   /* no scene pass found within ~2 s: fall back to the overlay */
      if (have_local && in_scene && !s.attempted && ++waited < 120) return; }
    /* Per frame, not per 250 ms: a frame whose scene pass did not place the board (no long opaque run, camera
     * constants not found ...) gets the overlay instead, so the board never blinks out for a few frames. */
    {   const LONG calls = InterlockedCompareExchange(&s_scene_calls, 0, 0);
        const int in_scene_now = calls != s_scene_calls_seen;
        s_scene_calls_seen = calls;
        if (s.ready) {
            if (++s_board_frames % 600 == 0)
                fprintf(stderr, "[SKATE-BOARD] last 600 frames: %ld drawn by the overlay fallback (rest in the scene pass)\n",
                        s_board_overlay), s_board_overlay = 0;
            if (in_scene_now) draw_local = 0;
            else if (have_local) s_board_overlay++;
        }
    }
    ID3D12Device* dev = (ID3D12Device*)device_ptr;
    ID3D12GraphicsCommandList* list = (ID3D12GraphicsCommandList*)list_ptr;
    if (!s.attempted && !initialize(dev)) return;
    if (!s.ready || !ensure_depth(dev, width, height)) { g_hide_active = 0; return; }

    if (have_local) {
        g_hide_position[0] = snap.position[0];
        g_hide_position[1] = snap.position[1];
        g_hide_position[2] = snap.position[2];
        g_hide_active = s.hide_enabled && s.rider;
        s.frames_drawn++;
        if (s.rider && s.hide_enabled && s.frames_drawn == 240 && s.hidden_draws == 0)
            fprintf(stderr, "[SKATE-ACTOR] no skinned draw matched Cole after 240 frames "
                            "(nearest skinned vertex %.0f cm); set INFAMOUS_SKATE_HIDE_COLE=0 "
                            "if another character disappears\n", s.nearest_skinned);
    } else {
        g_hide_active = 0;
    }
    s.hidden_this_frame = 0;
    s.nearest_skinned = 1e30f;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv; rtv.ptr = (SIZE_T)rtv_value;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv;
    RSX_D3D12_CPU_HEAP_START(s.dsv_heap)(s.dsv_heap, &dsv);
    D3D12_VIEWPORT vp = {0, 0, (float)width, (float)height, 0, 1};
    D3D12_RECT sc = {0, 0, (LONG)width, (LONG)height};
    D3D12_VERTEX_BUFFER_VIEW vbv = {s.vb->lpVtbl->GetGPUVirtualAddress(s.vb),
        s.header.vertices * s.header.vertex_stride, s.header.vertex_stride};
    list->lpVtbl->ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
    list->lpVtbl->OMSetRenderTargets(list, 1, &rtv, FALSE, &dsv);
    list->lpVtbl->RSSetViewports(list, 1, &vp);
    list->lpVtbl->RSSetScissorRects(list, 1, &sc);
    list->lpVtbl->SetPipelineState(list, s.pso);
    list->lpVtbl->SetGraphicsRootSignature(list, s.root);
    if (g_scene_heaps[0]) {
        /* the same heaps the backend binds, so its later draws find them unchanged */
        list->lpVtbl->SetDescriptorHeaps(list, g_scene_heaps[1] ? 2 : 1, g_scene_heaps);
        D3D12_GPU_DESCRIPTOR_HANDLE t;
        if (g_scene_valid) t.ptr = g_scene_srv;
        else RSX_D3D12_GPU_HEAP_START(g_scene_heaps[0])(g_scene_heaps[0], &t);
        list->lpVtbl->SetGraphicsRootDescriptorTable(list, 1, t);
    }
    list->lpVtbl->IASetVertexBuffers(list, 0, 1, &vbv);
    list->lpVtbl->IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    if (draw_local) {
        build_pose(&snap);
        if (s_live_used) snap.heading = s_live_heading;   /* live Skate pose: native -> host yaw */
        if (!s.rider) cole_update(&snap);
        const int on_cole = cole.active && cole.have_root;
        const u32 cb_offset = (s.cb_slot++ % CB_SLOTS) * CB_SIZE;
        float* cb = (float*)(s.cb_mapped + cb_offset);
        memcpy(cb, camera, 16 * sizeof(float));
        memcpy(cb + 16, viewport_xf, 8 * sizeof(float));
        /* With Cole retargeted, the board stands where his retargeted feet are: the skate origin at his root. */
        cb[24] = on_cole ? cole.feet[0] : snap.position[0];
        cb[25] = on_cole ? cole.feet[1] : snap.position[1];
        cb[26] = on_cole ? cole.feet[2] + cole.lift : snap.position[2] + s.z_offset;
        cb[27] = snap.heading;
        cb[28] = on_cole ? cole.world_cm_per_m : s.scale;
        cb[29] = 1.0f / 100000.0f;   /* linear depth range: 1 km */
        cb[30] = g_scene_valid ? 1.0f : 0.0f; cb[31] = 0.0f;
        cb[32] = cb[33] = cb[34] = cb[35] = 1.0f;       /* original material colours */
        cb[18] = g_scene_zscale; cb[22] = g_scene_zoffset;              /* xf0.z / xf1.z: guest depth mapping */
        cb[19] = 1.0f / (float)width; cb[23] = 1.0f / (float)height;   /* xf0.w / xf1.w: pixel -> uv */
        memcpy(cb + 36, s.pose, (size_t)s.header.bones * MATRIX_FLOATS * sizeof(float));
        ID3D12Resource* ib = s.rider ? s.ib : s.board_ib;
        const u32 index_count = s.rider ? s.header.indices : s.header.board_indices;
        D3D12_INDEX_BUFFER_VIEW ibv = {ib->lpVtbl->GetGPUVirtualAddress(ib),
            index_count * (u32)sizeof(u32), DXGI_FORMAT_R32_UINT};
        list->lpVtbl->SetGraphicsRootConstantBufferView(list, 0, s.cb->lpVtbl->GetGPUVirtualAddress(s.cb) + cb_offset);
        list->lpVtbl->IASetIndexBuffer(list, &ibv);
        list->lpVtbl->DrawIndexedInstanced(list, index_count, 1, 0, 0, 0);
    }

}

/* In-scene path, part 1 (record side, once per frame at the game's opaque scene pass): sample the pose, retarget
 * Cole and fill a constant slot with this frame's camera, so the board drawn later in the replay of this very
 * frame matches Cole and the world exactly.  Returns the slot's byte offset, or -1 when nothing is to be drawn. */
long rsx_skate_actor_prepare_scene(void* device_ptr, const float camera[16], float zscale, float zoffset)
{
    skate_actor_snapshot_t snap;
    infamous_skate_camera_forward(camera[12], camera[13]);
    if (!s.attempted) {
        const char* hide = getenv("INFAMOUS_SKATE_HIDE_COLE");
        s.hide_enabled = !(hide && *hide == '0');
        const char* ce = getenv("INFAMOUS_SKATE_COLE");
        cole.enabled = !(ce && *ce == '0');
        const char* sc = getenv("INFAMOUS_SKATE_IN_SCENE");
        s.in_scene_off = sc && *sc == '0';
    }
    if (s.in_scene_off) return -1;
    if (!infamous_skate_actor_snapshot(&snap)) { cole_deactivate(); s.have_pose = 0; return -1; }
    ID3D12Device* dev = (ID3D12Device*)device_ptr;
    if (!s.attempted && !initialize(dev)) return -1;
    if (!s.ready || s.rider) return -1;
    InterlockedExchange(&s.scene_tick, (LONG)GetTickCount());
    InterlockedIncrement(&s_scene_calls);
    s.frames_drawn++;
    build_pose(&snap);
    if (s_live_used) snap.heading = s_live_heading;   /* live Skate pose: native -> host yaw */
    cole_update(&snap);
    const int on_cole = cole.active && cole.have_root;
    const u32 cb_offset = (s.cb_slot++ % CB_SLOTS) * CB_SIZE;
    float* cb = (float*)(s.cb_mapped + cb_offset);
    memcpy(cb, camera, 16 * sizeof(float));
    static const float xf[8] = {1, 1, 1, 0, 0, 0, 0, 0};
    memcpy(cb + 16, xf, sizeof xf);
    cb[18] = zscale; cb[22] = zoffset;   /* guest window depth = z/w * scale + offset */
    cb[24] = on_cole ? cole.feet[0] : snap.position[0];
    cb[25] = on_cole ? cole.feet[1] : snap.position[1];
    cb[26] = on_cole ? cole.feet[2] + cole.lift : snap.position[2] + s.z_offset;
    cb[27] = snap.heading;
    cb[28] = on_cole ? cole.world_cm_per_m : s.scale;
    cb[29] = 1.0f / 100000.0f;
    cb[30] = 0.0f;                        /* hardware depth test against the game's own buffer */
    cb[31] = 1.0f;                        /* depth in the guest's convention */
    cb[32] = cb[33] = cb[34] = cb[35] = 1.0f;
    memcpy(cb + 36, s.pose, (size_t)s.header.bones * MATRIX_FLOATS * sizeof(float));
    return (long)cb_offset;
}

/* In-scene path, part 2 (replay side, right after the game's last opaque scene draw): draw the board into the
 * bound scene target with the game's depth buffer.  The caller restores its own pipeline state afterwards. */
void rsx_skate_actor_draw_scene(void* device_ptr, void* list_ptr, long cb_offset, u64 rtv_value, u64 dsv_value,
                                u32 width, u32 height, u32 rt_format, void* srv_heap, void* sampler_heap)
{
    if (cb_offset < 0 || !s.ready) return;
    ID3D12Device* dev = (ID3D12Device*)device_ptr;
    ID3D12GraphicsCommandList* list = (ID3D12GraphicsCommandList*)list_ptr;
    ID3D12PipelineState* pso = scene_pipeline(dev, rt_format);
    if (!pso) return;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv, dsv; rtv.ptr = (SIZE_T)rtv_value; dsv.ptr = (SIZE_T)dsv_value;
    D3D12_VIEWPORT vp = {0, 0, (float)width, (float)height, 0, 1};
    D3D12_RECT sc = {0, 0, (LONG)width, (LONG)height};
    D3D12_VERTEX_BUFFER_VIEW vbv = {s.vb->lpVtbl->GetGPUVirtualAddress(s.vb),
        s.header.vertices * s.header.vertex_stride, s.header.vertex_stride};
    D3D12_INDEX_BUFFER_VIEW ibv = {s.board_ib->lpVtbl->GetGPUVirtualAddress(s.board_ib),
        s.header.board_indices * (u32)sizeof(u32), DXGI_FORMAT_R32_UINT};
    list->lpVtbl->OMSetRenderTargets(list, 1, &rtv, FALSE, &dsv);
    list->lpVtbl->RSSetViewports(list, 1, &vp);
    list->lpVtbl->RSSetScissorRects(list, 1, &sc);
    list->lpVtbl->SetPipelineState(list, pso);
    list->lpVtbl->SetGraphicsRootSignature(list, s.root);
    list->lpVtbl->SetGraphicsRootConstantBufferView(list, 0, s.cb->lpVtbl->GetGPUVirtualAddress(s.cb) + (u64)cb_offset);
    if (srv_heap) {
        ID3D12DescriptorHeap* heaps[2] = {(ID3D12DescriptorHeap*)srv_heap, (ID3D12DescriptorHeap*)sampler_heap};
        list->lpVtbl->SetDescriptorHeaps(list, heaps[1] ? 2 : 1, heaps);
        D3D12_GPU_DESCRIPTOR_HANDLE t; RSX_D3D12_GPU_HEAP_START(heaps[0])(heaps[0], &t);
        list->lpVtbl->SetGraphicsRootDescriptorTable(list, 1, t);   /* unused (params.z = 0) but bound */
    }
    list->lpVtbl->IASetVertexBuffers(list, 0, 1, &vbv);
    list->lpVtbl->IASetIndexBuffer(list, &ibv);
    list->lpVtbl->IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->lpVtbl->DrawIndexedInstanced(list, s.header.board_indices, 1, 0, 0, 0);
}

void rsx_skate_actor_shutdown(void)
{
    cole.active = 0;
    g_hide_active = 0;
    if (s.vb) s.vb->lpVtbl->Release(s.vb);
    if (s.ib) s.ib->lpVtbl->Release(s.ib);
    if (s.board_ib) s.board_ib->lpVtbl->Release(s.board_ib);
    if (s.cb) { s.cb->lpVtbl->Unmap(s.cb, 0, NULL); s.cb->lpVtbl->Release(s.cb); }
    if (s.depth) s.depth->lpVtbl->Release(s.depth);
    if (s.dsv_heap) s.dsv_heap->lpVtbl->Release(s.dsv_heap);
    if (s.pso) s.pso->lpVtbl->Release(s.pso);
    if (s.root) s.root->lpVtbl->Release(s.root);
    free(s.bytes);
    memset(&s, 0, sizeof(s));
}
#else
volatile int g_skate_render_hooks;
void rsx_skate_actor_set_scene_depth(void* a, void* b, u64 c, int d, float e, float f)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; }
void rsx_skate_actor_draw(void* d, void* c, u64 r, u32 w, u32 h, const float m[16], const float x[8])
{ (void)d; (void)c; (void)r; (void)w; (void)h; (void)m; (void)x; }
int rsx_skate_actor_hides_player_draw(const float b[3]) { (void)b; return 0; }
int rsx_skate_actor_get_snapshot(float p[3], float* h, unsigned* c, float* t, float* l)
{ (void)p; (void)h; (void)c; (void)t; (void)l; return 0; }
int rsx_skate_actor_pose_cole_vertices(float* p, size_t stride, u32 count)
{ (void)p; (void)stride; (void)count; return 0; }
void rsx_skate_actor_shutdown(void) {}
long rsx_skate_actor_prepare_scene(void* d, const float c[16], float a, float b) { (void)d; (void)c; (void)a; (void)b; return -1; }
void rsx_skate_actor_draw_scene(void* d, void* l, long o, u64 r, u64 z, u32 w, u32 h, u32 f, void* a, void* b)
{ (void)d; (void)l; (void)o; (void)r; (void)z; (void)w; (void)h; (void)f; (void)a; (void)b; }
#endif
