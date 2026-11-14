/* rsx_post.h -- v1118A wet streets (SSR, GF_POST_SSR 1 damp / 2 wet) on top of v1116A (+ v1116D close-up AO: radius 0.12 x distance, softer near the camera, tighter blur, no halos from thin occluders in front) "lighting pack": ambient occlusion, bloom, colour grading on the main 3D pass (Godfather).
 * Included by rsx_ui_overlay.c (Windows part). The backend calls gpost_run right after the main scene pass's last
 * depth-tested draw at the decal spot (gfd_at: the end of the world stretch, characters included), BEFORE the HUD, so the
 * HUD / menus are never touched. Inputs: the scene colour target (state tracked by the backend, passed by pointer), its
 * depth (R24G8 typeless, already in PIXEL_SHADER_RESOURCE), the sampled camera (c0..c3 + epilogue, same as the decals).
 *   1. AO (camera needed): half-res R16G16F (ao, view depth w) from the depth only -- 12 samples in a 0.6 m hemisphere,
 *      normal from the depth neighbours, 4x4 interleaved rotation (removed by the 4x4 bilateral upsample in pass 4).
 *   2. bloom: copy of the scene -> quarter-res soft-threshold bright pass -> 2 x (horizontal + vertical 9-tap gaussian).
 *   3. final (full res, into the scene target): scene x AO (bilateral upsample) + bloom, then the grade: gentle S-curve,
 *      +12 % saturation, warm highlights / cool shadows, light vignette. Alpha is kept.
 * Settings (PC Settings, live): GF_POST_AO 0 off / 1 on / 2 strong, GF_POST_BLOOM 0 / 1 low / 2 high, GF_POST_GRADE 0 / 1.
 * Log [post]. */
volatile int g_post_ao = -1, g_post_bloom = -1, g_post_grade = -1, g_post_sharpen = -1, g_post_ssr = -1;   /* v1117F + sharpen; v1118A + wet streets */   /* -1 = env (rsx_ui_overlay.c settings menu sets them) */
int (*volatile g_gf_rain_poll)(void) = NULL;   /* v1118B: set by the game (gf_cutscene.c): 1 = raining */
static float s_po_wet = 0.0f; static ULONGLONG s_po_wet_t = 0;
/* v1118B: wetness follows the rain -- wets in ~25 s while it rains, dries in ~4 min after. Setting 1 / 2 = rain only
 * (subtle / strong), 3 = always wet (strong), 0 = off. Unknown weather (no game hook) = dry. */
static float po_wetness(int sss)
{
    if (sss >= 3) return 1.0f;
    const ULONGLONG now = GetTickCount64();
    float dt = s_po_wet_t ? (float)(now - s_po_wet_t) * 0.001f : 0.0f; s_po_wet_t = now;
    if (dt > 0.5f) dt = 0.5f;   /* pauses / loads do not jump it */
    const int rain = g_gf_rain_poll ? g_gf_rain_poll() : 0;
    static int st = -2; if (st == -2) { const char* e = getenv("GF_WET_START"); st = e && *e ? 1 : 0; if (st) s_po_wet = (float)atof(e); }
    s_po_wet += rain ? dt / 25.0f : -dt / 240.0f;
    s_po_wet = s_po_wet < 0 ? 0 : s_po_wet > 1 ? 1 : s_po_wet;
    static int lr = -1; static float lw = -1;
    if (rain != lr || (s_po_wet == 0) != (lw == 0) || (s_po_wet == 1) != (lw == 1)) { fprintf(stderr, "[post] v1118B rain %d wetness %.2f%c", rain, s_po_wet, 10); lr = rain; }
    lw = s_po_wet;
    return s_po_wet * s_po_wet * (3 - 2 * s_po_wet);
}
static int s_po_env[5] = { -2, -2, -2, -2, -2 };
static int po_setting(int i)
{
    static const char* const env[5] = { "GF_POST_AO", "GF_POST_BLOOM", "GF_POST_GRADE", "GF_POST_SHARPEN", "GF_POST_SSR" };
    const int g = i == 0 ? g_post_ao : i == 1 ? g_post_bloom : i == 2 ? g_post_grade : i == 3 ? g_post_sharpen : g_post_ssr;
    if (g >= 0) return g;
    if (s_po_env[i] == -2) { const char* e = getenv(env[i]); s_po_env[i] = (e && *e) ? atoi(e) : 0; }
    return s_po_env[i];
}
#define PO_RING 8   /* v1117F: two calls per frame now (AO, then grade) */
#define PO_DPF 24   /* descriptors per frame (6 passes x 4) */
static ID3D12RootSignature* s_po_sig;
static ID3D12PipelineState *s_po_ao, *s_po_bright, *s_po_blur, *s_po_fin[4];
static DXGI_FORMAT s_po_fin_fmt[4];
static ID3D12DescriptorHeap *s_po_heap, *s_po_rtvh;
static ID3D12Resource *s_po_copy, *s_po_aot, *s_po_b[2];
static D3D12_RESOURCE_STATES s_po_st_copy, s_po_st_ao, s_po_st_b[2];
static UINT s_po_w, s_po_h; static DXGI_FORMAT s_po_fmt;
static int s_po_state, s_po_ri;
static ID3DBlob* s_po_ps_fin;
static const char s_po_hlsl[] =
    "cbuffer K : register(b0) { float4 k0, k1, k2, k3, k4, k5, k6, k7, k8; };\n"
    /* k0 = (W, H, 1/W, 1/H) scene; k1 = (kzA, kzB, P00, P11); k2 = (ao strength, ao radius m, bloom strength, grade);
     * k3 = (-, -, blur step u, blur step v); k4 = (bloom threshold, -, 1/aoW, 1/aoH)
     * v1118A: k5..k7 = (view x / y / z axis in world, camera world x / y / z) -> world = P.x k5 + P.y k6 + P.z k7 + C;
     * k8 = (world up in view space, wet amount) */
    "Texture2D t0 : register(t0); Texture2D t1 : register(t1); Texture2D t2 : register(t2); Texture2D<float> tz : register(t3);\n"
    "SamplerState sl : register(s0); SamplerState sp : register(s1);\n"
    "struct V { float4 p : SV_POSITION; float2 uv : TEXCOORD; };\n"
    "V vs(uint id : SV_VertexID) { V o; float2 t = float2((id << 1) & 2, id & 2); o.uv = t; o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "float lw(float d) { return k1.y / (d - k1.x); }\n"
    "float2 snap(float2 u) { return (floor(u * k0.xy) + 0.5) * k0.zw; }\n"   /* v1116A-4: position = the centre of the depth texel actually read */
    "float3 vpos(float2 uv, float d) { float w = lw(d); float2 n = uv * 2 - 1; return float3(n.x * w / k1.z, n.y * w / k1.w, w); }\n"
    "float4 ps_ao(V i) : SV_TARGET {\n"
    "  float2 uv = snap(i.uv); float d = tz.SampleLevel(sp, uv, 0);\n"
    "  if (d >= 0.99999) return float4(1, 1e6, 0, 0);\n"
    "  float3 P = vpos(uv, d); if (!(P.z > 0.05)) return float4(1, 1e6, 0, 0);\n"
    "  float2 px = k0.zw * 2;\n"
    "  float2 ux = float2(px.x, 0), uy = float2(0, px.y);\n"
    "  float3 Pr = vpos(uv + ux, tz.SampleLevel(sp, uv + ux, 0)), Pl = vpos(uv - ux, tz.SampleLevel(sp, uv - ux, 0));\n"
    "  float3 Pd = vpos(uv + uy, tz.SampleLevel(sp, uv + uy, 0)), Pu = vpos(uv - uy, tz.SampleLevel(sp, uv - uy, 0));\n"
    "  float3 dx = (abs(Pr.z - P.z) < abs(P.z - Pl.z)) ? Pr - P : P - Pl;\n"
    "  float3 dy = (abs(Pd.z - P.z) < abs(P.z - Pu.z)) ? Pd - P : P - Pu;\n"
    "  float3 n = normalize(cross(dx, dy)); if (dot(n, P) > 0) n = -n;\n"
    "  float R = clamp(0.12 * P.z, 0.08, k2.y); float2 rs = float2(R * k1.z / P.z * 0.5, R * k1.w / P.z * 0.5);\n"
    "  if (rs.x * k0.x < 3.0) return float4(1, P.z, 0, 0);\n"
    "  rs = min(rs, float2(0.1, 0.1 * k0.x / k0.y));\n"
    "  uint2 ip = uint2(i.p.xy); float rot = ((ip.x & 3) + (ip.y & 3) * 4) / 16.0;\n"
    "  float ao = 0;\n"
    "  [unroll] for (int s = 0; s < 12; s++) {\n"
    "    float a = s * 2.3999632 + rot * 6.2831853; float r = (s + 0.5 + frac(rot * 3.7)) / 12.5; r *= r;\n"
    "    float2 suv = snap(uv + float2(cos(a), sin(a)) * rs * r);\n"
    "    float3 Q = vpos(suv, tz.SampleLevel(sp, suv, 0)); float3 v = Q - P; float vv = dot(v, v);\n"
    "    float thin = saturate((-v.z - 0.6 * sqrt(vv)) / (0.25 * R));\n"   /* v1116D: occluder floating in front (glasses, hair) */
    "    ao += max(0, dot(v, n) * rsqrt(vv + 1e-6) - 0.1) * saturate(1 - vv / (R * R)) * (1 - thin);\n"
    "  }\n"
    "  ao /= 12; float fade = saturate(1 - (P.z - 50) / 50) * lerp(0.35, 1.0, saturate((P.z - 1.0) / 4.0));\n"
    "  return float4(saturate(1 - k2.x * ao * fade * 1.6), P.z, 0, 0);\n"
    "}\n"
    "float4 ps_bright(V i) : SV_TARGET { float2 t = k0.zw;\n"
    "  float3 c = (t0.SampleLevel(sl, i.uv + t * float2(-1, -1), 0).rgb + t0.SampleLevel(sl, i.uv + t * float2(1, -1), 0).rgb +\n"
    "              t0.SampleLevel(sl, i.uv + t * float2(-1, 1), 0).rgb + t0.SampleLevel(sl, i.uv + t * float2(1, 1), 0).rgb) * 0.25;\n"
    "  float l = max(c.r, max(c.g, c.b)); float s = saturate((l - k4.x) / (1.0 - k4.x + 1e-4)); return float4(c * s * s, 1); }\n"
    "float4 ps_blur(V i) : SV_TARGET { float2 d = k3.zw; float3 c = t0.SampleLevel(sl, i.uv, 0).rgb * 0.2270270;\n"
    "  c += (t0.SampleLevel(sl, i.uv + d * 1.3846154, 0).rgb + t0.SampleLevel(sl, i.uv - d * 1.3846154, 0).rgb) * 0.3162162;\n"
    "  c += (t0.SampleLevel(sl, i.uv + d * 3.2307692, 0).rgb + t0.SampleLevel(sl, i.uv - d * 3.2307692, 0).rgb) * 0.0702703;\n"
    "  return float4(c, 1); }\n"
    /* v1118A wet streets: screen-space reflections on up-facing ground (normal from the depth, mirror = the world up),
     * puddles from a world-space noise, grey / dark surfaces only (no mirror grass), darker when wet. */
    "float h3(float3 p) { p = frac(p * 0.3183099 + 0.1); p *= 17.0; return frac(p.x * p.y * p.z * (p.x + p.y + p.z)); }\n"
    "float n3(float3 x) { float3 i = floor(x), f = frac(x); f = f * f * (3 - 2 * f);\n"
    "  return lerp(lerp(lerp(h3(i), h3(i + float3(1, 0, 0)), f.x), lerp(h3(i + float3(0, 1, 0)), h3(i + float3(1, 1, 0)), f.x), f.y),\n"
    "              lerp(lerp(h3(i + float3(0, 0, 1)), h3(i + float3(1, 0, 1)), f.x), lerp(h3(i + float3(0, 1, 1)), h3(i + float3(1, 1, 1)), f.x), f.y), f.z); }\n"
    "float2 proj(float3 Q) { return float2(Q.x * k1.z / Q.z, Q.y * k1.w / Q.z) * 0.5 + 0.5; }\n"
    "float3 wet(V i, float3 c) {\n"
    "  float2 uv = snap(i.uv); float d = tz.SampleLevel(sp, uv, 0); if (d >= 0.99999) return c;\n"
    "  float3 P = vpos(uv, d); if (!(P.z > 0.3) || P.z > 150) return c;\n"
    "  float2 px = k0.zw * 1.5;\n"
    "  float3 Pr = vpos(uv + float2(px.x, 0), tz.SampleLevel(sp, uv + float2(px.x, 0), 0)), Pl = vpos(uv - float2(px.x, 0), tz.SampleLevel(sp, uv - float2(px.x, 0), 0));\n"
    "  float3 Pd = vpos(uv + float2(0, px.y), tz.SampleLevel(sp, uv + float2(0, px.y), 0)), Pu = vpos(uv - float2(0, px.y), tz.SampleLevel(sp, uv - float2(0, px.y), 0));\n"
    "  float3 dx = (abs(Pr.z - P.z) < abs(P.z - Pl.z)) ? Pr - P : P - Pl; float3 dy = (abs(Pd.z - P.z) < abs(P.z - Pu.z)) ? Pd - P : P - Pu;\n"
    "  float3 n = normalize(cross(dx, dy)); if (dot(n, P) > 0) n = -n;\n"
    "  float3 U = k8.xyz; float m = smoothstep(0.88, 0.97, dot(n, U)); if (m <= 0) return c;\n"
    "  float mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b)); float sat = (mx - mn) / max(mx, 1e-3);\n"
    "  m *= saturate(1 - (sat - 0.18) * 3.5) * saturate(1 - (mx - 0.75) * 4);\n"   /* grey / dark ground only (asphalt, stone) */
    "  if (m <= 0) return c;\n"
    "  float pud = saturate((k8.w - 0.4) * 1.5);\n"   /* v1118A-2: uniform (the camera matrix is camera-relative -> world noise would swim) */
    "  float A = k8.w * m * saturate(1 - (P.z - 80) / 70); if (A < 0.02) return c;\n"
    "  float3 Vv = normalize(P); float3 R = reflect(Vv, U);\n"
    "  float cosv = saturate(dot(-Vv, U)); float F = 0.03 + 0.97 * pow(1 - cosv, 5);\n"
    "  uint2 ip = uint2(i.p.xy); float jit = frac(52.9829189 * frac(dot(float2(ip), float2(0.06711056, 0.00583715))));\n"
    "  float t = (0.04 * P.z + 0.08) * (0.6 + 0.8 * jit), st = t; float hit = 0; float2 huv = 0;\n"
    "  [loop] for (int s = 0; s < 16; s++) {\n"
    "    float3 Q = P + R * t; if (Q.z < 0.1) break; float2 q = proj(Q); if (any(q < 0) || any(q > 1)) break;\n"
    "    float sw = lw(tz.SampleLevel(sp, q, 0)); float dz = Q.z - sw;\n"
    "    float th = max(0.35, 0.06 * Q.z) + st; if (dz > 0 && dz < th) {\n"
    "      float a = t - st, b = t; [unroll] for (int r = 0; r < 4; r++) { float mt = (a + b) * 0.5; float3 M = P + R * mt; float2 mq = proj(M);\n"
    "        if (M.z - lw(tz.SampleLevel(sp, mq, 0)) > 0) b = mt; else a = mt; }\n"
    "      float3 H = P + R * b; huv = proj(H); hit = saturate(1.6 - 2.0 * abs(H.z - lw(tz.SampleLevel(sp, huv, 0))) / th); t = b; break; }\n"
    "    st *= 1.26; t += st; }\n"
    "  float3 col = c * (1 - 0.2 * A * (0.6 + 0.4 * pud));\n"   /* wet = darker */
    "  if (hit > 0) {\n"
    "    float2 e = saturate(min(huv, 1 - huv) * float2(8, 6)); float ef = e.x * e.y * saturate(1 - t / 60) * hit;\n"
    "    float rough = (1 - pud) * 0.006 + 0.0008;\n"
    "    float3 rc = (t0.SampleLevel(sl, huv + float2(rough, rough * 0.5), 0).rgb + t0.SampleLevel(sl, huv - float2(rough, rough * 0.5), 0).rgb +\n"
    "                 t0.SampleLevel(sl, huv + float2(-rough * 0.5, rough), 0).rgb + t0.SampleLevel(sl, huv + float2(rough * 0.5, -rough), 0).rgb) * 0.25;\n"
    "    float k = A * ef * saturate(F * lerp(1.6, 3.2, pud) + 0.12 * pud);\n"
    "    col = lerp(col, rc, saturate(k)); }\n"
    "  if (k3.y > 2.5) return float3(m, pud * m, hit);\n"   /* GF_POST_DEBUG=3: red = wet mask, green = puddles, blue = ray hit */
    "  return col; }\n"
    "float4 ps_final(V i) : SV_TARGET {\n"
    "  float4 s = t0.SampleLevel(sp, i.uv, 0); float3 c = s.rgb;\n"
    "  if (k4.y > 0) {\n"   /* v1117F contrast-adaptive sharpening (AMD CAS, 5 taps) */
    "    float3 a = t0.SampleLevel(sp, i.uv + float2(0, -k0.w), 0).rgb, b = t0.SampleLevel(sp, i.uv + float2(-k0.z, 0), 0).rgb;\n"
    "    float3 d = t0.SampleLevel(sp, i.uv + float2(k0.z, 0), 0).rgb, e = t0.SampleLevel(sp, i.uv + float2(0, k0.w), 0).rgb;\n"
    "    float3 mn = min(min(a, b), min(min(c, d), e)), mx = max(max(a, b), max(max(c, d), e));\n"
    "    float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-4)));\n"
    "    float3 w = amp * (-1.0 / lerp(8.0, 5.0, saturate(k4.y)));\n"
    "    c = saturate((c + (a + b + d + e) * w) / (1.0 + 4.0 * w)); }\n"
    "  if (k3.y > 1.5 && k3.y < 2.5) { float d = tz.SampleLevel(sp, i.uv, 0); float w = lw(d); return float4(frac(w), frac(w * 0.1), d >= 0.99999 ? 1 : 0, s.a); }\n"
    "  if (k3.x > 0 && i.uv.x > 0.5) return (abs(i.uv.x - 0.5) < 0.0008) ? float4(1, 1, 1, s.a) : s;\n"
    "  if (k8.w > 0) { c = wet(i, c); if (k3.y > 2.5) return float4(c, s.a); }\n"
    "  if (k2.x > 0) { float d = tz.SampleLevel(sp, i.uv, 0); if (d < 0.99999) { float w = lw(d); float sum = 0, ws = 0;\n"
    "    int2 b0 = int2(i.uv / k4.zw) - 1;\n"   /* v1116A-2: exactly 4 x 4 consecutive AO texels = one full rotation period (no stripes) */
    "    [unroll] for (int y = 0; y < 4; y++) [unroll] for (int x = 0; x < 4; x++) {\n"
    "      float2 a = t1.Load(int3(b0 + int2(x, y), 0)).rg; float wt = saturate(1.0 - abs(a.y - w) / (lerp(0.05, 0.15, saturate((w - 2.0) / 8.0)) * w + 0.02)); sum += a.x * wt; ws += wt; }\n"
    "    if (ws > 1e-3) { float ao = sum / ws; if (k3.y > 0) return float4(ao, ao, ao, s.a); c *= ao; } } else if (k3.y > 0) return float4(1, 1, 1, s.a); }\n"
    "  if (k2.z > 0) c += t2.SampleLevel(sl, i.uv, 0).rgb * k2.z;\n"
    "  if (k2.w > 1.5) { c = saturate(c); float3 sc = c * c * (3 - 2 * c); c = lerp(c, sc, 0.5);\n"   /* v1116E "Contrast" = the first v1116A grade */
    "    float l = dot(c, float3(0.2126, 0.7152, 0.0722)); c = lerp(l.xxx, c, 1.18);\n"
    "    float3 tint = lerp(float3(0.96, 1.0, 1.05), float3(1.05, 1.0, 0.93), smoothstep(0.12, 0.7, l)); c *= tint;\n"
    "    float2 v = i.uv - 0.5; c *= 1 - 0.22 * pow(saturate(dot(v, v) * 2.2), 1.5); }\n"
    "  else if (k2.w > 0) { c = saturate(c); c = lerp(c, pow(c, 0.9) * (1.0 + 0.04 * c), k2.w);\n"   /* "Film" */
    "    float l = dot(c, float3(0.2126, 0.7152, 0.0722)); c = lerp(l.xxx, c, 1 + 0.18 * k2.w);\n"
    "    float3 tint = lerp(float3(0.96, 1.0, 1.05), float3(1.05, 1.0, 0.93), smoothstep(0.12, 0.7, l)); c *= lerp(float3(1, 1, 1), tint, k2.w);\n"
    "    float2 v = i.uv - 0.5; c *= 1 - 0.12 * k2.w * pow(saturate(dot(v, v) * 2.2), 1.5); }\n"
    "  return float4(saturate(c), s.a); }\n";
static ID3DBlob* po_compile(const char* entry, const char* prof)
{
    ID3DBlob *b = NULL, *err = NULL;
    if (FAILED(D3DCompile(s_po_hlsl, sizeof s_po_hlsl - 1, "gf_post", NULL, NULL, entry, prof, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &err))) {
        fprintf(stderr, "[post] shader %s failed: %s\n", entry, err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "?"); return NULL; }
    return b;
}
static ID3DBlob* s_po_vs;
static ID3D12PipelineState* po_pso(ID3D12Device* dev, ID3DBlob* ps, DXGI_FORMAT fmt)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; memset(&pd, 0, sizeof pd);
    pd.pRootSignature = s_po_sig;
    pd.VS.pShaderBytecode = s_po_vs->lpVtbl->GetBufferPointer(s_po_vs); pd.VS.BytecodeLength = s_po_vs->lpVtbl->GetBufferSize(s_po_vs);
    pd.PS.pShaderBytecode = ps->lpVtbl->GetBufferPointer(ps); pd.PS.BytecodeLength = ps->lpVtbl->GetBufferSize(ps);
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable = TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL; pd.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    pd.SampleMask = UINT_MAX; pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = fmt; pd.SampleDesc.Count = 1;
    ID3D12PipelineState* p = NULL;
    if (FAILED(dev->lpVtbl->CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void**)&p))) { fprintf(stderr, "[post] PSO failed (fmt %d)\n", (int)fmt); return NULL; }
    return p;
}
static int po_init(ID3D12Device* dev)
{
    if (s_po_state) return s_po_state > 0;
    s_po_state = -1;
    D3D12_DESCRIPTOR_RANGE rg; memset(&rg, 0, sizeof rg);
    rg.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rg.NumDescriptors = 4;
    D3D12_ROOT_PARAMETER rp[2]; memset(rp, 0, sizeof rp);
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants.Num32BitValues = 36; rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[1].DescriptorTable.NumDescriptorRanges = 1; rp[1].DescriptorTable.pDescriptorRanges = &rg;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sd[2]; memset(sd, 0, sizeof sd);
    for (int i = 0; i < 2; i++) { sd[i].Filter = i ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sd[i].AddressU = sd[i].AddressV = sd[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP; sd[i].MaxLOD = D3D12_FLOAT32_MAX;
        sd[i].ShaderRegister = (UINT)i; sd[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL; }
    D3D12_ROOT_SIGNATURE_DESC rd; memset(&rd, 0, sizeof rd);
    rd.NumParameters = 2; rd.pParameters = rp; rd.NumStaticSamplers = 2; rd.pStaticSamplers = sd;
    ID3DBlob *sig = NULL, *err = NULL;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(dev->lpVtbl->CreateRootSignature(dev, 0, sig->lpVtbl->GetBufferPointer(sig), sig->lpVtbl->GetBufferSize(sig), &IID_ID3D12RootSignature, (void**)&s_po_sig))) {
        fprintf(stderr, "[post] root signature failed\n"); return 0; }
    sig->lpVtbl->Release(sig);
    ID3DBlob *pa = NULL, *pb = NULL, *pc = NULL;
    if (!(s_po_vs = po_compile("vs", "vs_5_0")) || !(pa = po_compile("ps_ao", "ps_5_0")) || !(pb = po_compile("ps_bright", "ps_5_0")) ||
        !(pc = po_compile("ps_blur", "ps_5_0")) || !(s_po_ps_fin = po_compile("ps_final", "ps_5_0"))) return 0;
    s_po_ao = po_pso(dev, pa, DXGI_FORMAT_R16G16_FLOAT);
    s_po_bright = po_pso(dev, pb, DXGI_FORMAT_R11G11B10_FLOAT);
    s_po_blur = po_pso(dev, pc, DXGI_FORMAT_R11G11B10_FLOAT);
    pa->lpVtbl->Release(pa); pb->lpVtbl->Release(pb); pc->lpVtbl->Release(pc);
    if (!s_po_ao || !s_po_bright || !s_po_blur) return 0;
    D3D12_DESCRIPTOR_HEAP_DESC hd; memset(&hd, 0, sizeof hd);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = PO_RING * PO_DPF; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->lpVtbl->CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_po_heap))) return 0;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = PO_RING * 4; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(dev->lpVtbl->CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_po_rtvh))) return 0;
    s_po_state = 1; fprintf(stderr, "[post] v1116A lighting pack ready (AO / bloom / grade)\n");
    return 1;
}
static ID3D12Resource* po_tex(ID3D12Device* dev, UINT w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_STATES st, int rt)
{
    D3D12_HEAP_PROPERTIES hp; memset(&hp, 0, sizeof hp); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td; memset(&td, 0, sizeof td);
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = f; td.SampleDesc.Count = 1; td.Flags = rt ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* r = NULL;
    if (FAILED(dev->lpVtbl->CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &td, st, NULL, &IID_ID3D12Resource, (void**)&r))) return NULL;
    return r;
}
static int po_resize(ID3D12Device* dev, UINT w, UINT h, DXGI_FORMAT f)
{
    if (s_po_copy && s_po_w == w && s_po_h == h && s_po_fmt == f) return 1;
    /* resolution change (rare): the old textures may still be in flight -> leaked on purpose, not released */
    s_po_copy = po_tex(dev, w, h, f, D3D12_RESOURCE_STATE_COPY_DEST, 0); s_po_st_copy = D3D12_RESOURCE_STATE_COPY_DEST;
    s_po_aot = po_tex(dev, (w + 1) / 2, (h + 1) / 2, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_RENDER_TARGET, 1); s_po_st_ao = D3D12_RESOURCE_STATE_RENDER_TARGET;
    for (int i = 0; i < 2; i++) { s_po_b[i] = po_tex(dev, (w + 3) / 4, (h + 3) / 4, DXGI_FORMAT_R11G11B10_FLOAT, D3D12_RESOURCE_STATE_RENDER_TARGET, 1); s_po_st_b[i] = D3D12_RESOURCE_STATE_RENDER_TARGET; }
    if (!s_po_copy || !s_po_aot || !s_po_b[0] || !s_po_b[1]) { fprintf(stderr, "[post] textures failed (%ux%u fmt %d) -> off\n", w, h, (int)f); s_po_copy = NULL; s_po_state = -1; return 0; }
    s_po_w = w; s_po_h = h; s_po_fmt = f;
    fprintf(stderr, "[post] scene %ux%u fmt %d: AO %ux%u, bloom %ux%u\n", w, h, (int)f, (w + 1) / 2, (h + 1) / 2, (w + 3) / 4, (h + 3) / 4);
    return 1;
}
static void po_bar(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES* st, D3D12_RESOURCE_STATES to)
{
    if (*st == to) return;
    D3D12_RESOURCE_BARRIER b; memset(&b, 0, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = r; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = *st; b.Transition.StateAfter = to;
    cl->lpVtbl->ResourceBarrier(cl, 1, &b); *st = to;
}
int gpost_wanted(void) { return s_po_state >= 0 && (po_setting(0) > 0 || po_setting(1) > 0 || po_setting(2) > 0 || po_setting(3) > 0 || po_setting(4) > 0); }
int gpost_wants(int what) { return s_po_state >= 0 && (((what & 1) && (po_setting(0) > 0 || po_setting(4) > 0)) || ((what & 2) && (po_setting(1) > 0 || po_setting(2) > 0 || po_setting(3) > 0))); }   /* v1117F */
/* rt / *rtst: the scene colour target and its tracked state (left in RENDER_TARGET when 1 is returned); z: its depth in
 * PIXEL_SHADER_RESOURCE; c: camera c0..c3 (16) + posscale (4) + posoffset (4), camok: c is valid. Returns 1 when it drew
 * (the caller rebinds its own root signature / heaps / targets). */
/* v1117F what: 1 = AO (needs z), 2 = bloom + grade + sharpen (z may be NULL), 3 = both (old single call) */
int gpost_run(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, ID3D12Resource* rt, D3D12_RESOURCE_STATES* rtst, ID3D12Resource* z, const float* c, int camok, int what)
{
    const int sao = (what & 1) ? po_setting(0) : 0, sss = (what & 1) ? po_setting(4) : 0, sbl = (what & 2) ? po_setting(1) : 0, sgr = (what & 2) ? po_setting(2) : 0, ssh = (what & 2) ? po_setting(3) : 0;
    if (!z) camok = 0;
    if ((sao <= 0 && sss <= 0 && sbl <= 0 && sgr <= 0 && ssh <= 0) || !rt || !po_init(dev)) return 0;
    D3D12_RESOURCE_DESC rdsc; RSX_D3D12_RESOURCE_DESC(rt)(rt, &rdsc);
    const UINT W = (UINT)rdsc.Width, H = rdsc.Height; const DXGI_FORMAT F = rdsc.Format;
    if (W < 64 || H < 64 || rdsc.SampleDesc.Count != 1 || !po_resize(dev, W, H, F)) return 0;
    int fi = -1; for (int i = 0; i < 4; i++) if (s_po_fin[i] && s_po_fin_fmt[i] == F) fi = i;
    if (fi < 0) { for (int i = 0; i < 4 && fi < 0; i++) if (!s_po_fin[i]) { s_po_fin[i] = po_pso(dev, s_po_ps_fin, F); s_po_fin_fmt[i] = F; if (s_po_fin[i]) fi = i; } }
    if (fi < 0) return 0;
    float k[36]; memset(k, 0, sizeof k);
    k[0] = (float)W; k[1] = (float)H; k[2] = 1.0f / W; k[3] = 1.0f / H;
    int ao_ok = 0;
    int ssr_ok = 0;
    if ((sao > 0 || sss > 0) && camok && c) {   /* depth -> view depth w: d = A + B / w (the decals' fit); P00 / P11 from the camera rows */
        const double ww = (double)c[3] * c[3] + (double)c[7] * c[7] + (double)c[11] * c[11];
        const double a = ww > 1e-12 ? ((double)c[2] * c[3] + (double)c[6] * c[7] + (double)c[10] * c[11]) / ww : 0.0, bb = c[14] - a * c[15];
        const float A = (float)(c[18] * a + c[22]), B = (float)(c[18] * bb);
        const double lx = sqrt((double)c[0] * c[0] + (double)c[4] * c[4] + (double)c[8] * c[8]);
        const double ly = sqrt((double)c[1] * c[1] + (double)c[5] * c[5] + (double)c[9] * c[9]);
        const double lw2 = sqrt(ww);
        if (fabsf(B) > 1e-6f && A == A && lw2 > 1e-9 && lx > 1e-9 && ly > 1e-9) {
            k[4] = A; k[5] = B; k[6] = (float)(lx / lw2); k[7] = (float)(ly / lw2);
            k[8] = sao >= 2 ? 2.8f : 1.8f; k[9] = 0.8f; ao_ok = sao > 0;
            if (sss > 0) {   /* v1118A: view axes / camera position in world (clip_k = sum_i c[4i+k] X_i + c[12+k]) */
                const double R[3] = { c[0] / lx, c[4] / lx, c[8] / lx }, Uu[3] = { c[1] / ly, c[5] / ly, c[9] / ly }, Fw[3] = { c[3] / lw2, c[7] / lw2, c[11] / lw2 };
                double Cw[3]; for (int j = 0; j < 3; j++) Cw[j] = -(c[12] / lx) * R[j] - (c[13] / ly) * Uu[j] - (c[15] / lw2) * Fw[j];
                /* shader view space: x = right, y = DOWN (uv), z = forward; P scaled by lw2 (w = clip.w) */
                for (int j = 0; j < 3; j++) { k[20 + j] = (float)(R[j] / lw2); k[24 + j] = (float)(-Uu[j] / lw2); k[28 + j] = (float)(Fw[j] / lw2); }
                k[23] = (float)Cw[0]; k[27] = (float)Cw[1]; k[31] = (float)Cw[2];
                /* world up = the world axis closest to the camera's up (GF_SSR_UP=0..5 forces +x -x +y -y +z -z) */
                static int fu = -2; if (fu == -2) { const char* e = getenv("GF_SSR_UP"); fu = e && *e ? atoi(e) : -1; }
                double Wu[3] = { 0, 0, 0 };
                if (fu >= 0 && fu < 6) Wu[fu / 2] = (fu & 1) ? -1 : 1;
                else { int ax = 0; for (int j = 1; j < 3; j++) if (fabs(Uu[j]) > fabs(Uu[ax])) ax = j; Wu[ax] = Uu[ax] > 0 ? 1 : -1; }
                const double ux = Wu[0] * R[0] + Wu[1] * R[1] + Wu[2] * R[2], uy = -(Wu[0] * Uu[0] + Wu[1] * Uu[1] + Wu[2] * Uu[2]), uz = Wu[0] * Fw[0] + Wu[1] * Fw[1] + Wu[2] * Fw[2];
                const double ul = sqrt(ux * ux + uy * uy + uz * uz);
                const float wet = po_wetness(sss);   /* v1118B: only when it rains / has rained */
                if (ul > 0.5 && wet > 0.01f) { k[32] = (float)(ux / ul); k[33] = (float)(uy / ul); k[34] = (float)(uz / ul); k[35] = (sss >= 2 ? 0.85f : 0.5f) * wet; ssr_ok = 1; }
                static int nu = 0; if (nu < 3) { nu++; fprintf(stderr, "[post] v1118A SSR: cam up (%.2f %.2f %.2f) fwd (%.2f %.2f %.2f) pos (%.1f %.1f %.1f) world up (%g %g %g) view up (%.2f %.2f %.2f) lw2 %.3f%c",
                    Uu[0], Uu[1], Uu[2], Fw[0], Fw[1], Fw[2], Cw[0], Cw[1], Cw[2], Wu[0], Wu[1], Wu[2], k[32], k[33], k[34], lw2, 10); }
            }   /* v1116A-2: was 1.0 / 1.6, 0.6 m -- far too faint */
            static int nl = 0; if (nl < 3) { nl++; fprintf(stderr, "[post] AO camera: kz A %.6f B %.6f, P00 %.3f P11 %.3f\n", A, B, k[6], k[7]); }
        }
    }
    k[10] = sbl >= 2 ? 0.55f : sbl == 1 ? 0.30f : 0.0f;
    k[11] = sgr >= 2 ? 2.0f : sgr > 0 ? 1.0f : 0.0f;   /* v1116E: 2 = Contrast */
    k[16] = 0.72f; k[18] = 2.0f / W; k[19] = 2.0f / H;
    { static int sp = -2, dbg = -2;   /* GF_POST_SPLIT=1: left half processed, right half original; GF_POST_DEBUG=1: AO only */
      if (sp == -2) { const char* e = getenv("GF_POST_SPLIT"); sp = e && *e == '1'; const char* d = getenv("GF_POST_DEBUG"); dbg = d && *d ? atoi(d) : 0; }
      k[12] = (float)sp; k[13] = (float)dbg; }
    if (!ao_ok) k[8] = 0.0f;
    k[17] = ssh >= 2 ? 1.0f : ssh == 1 ? 0.5f : 0.0f;   /* v1117F CAS amount */
    if (!ssr_ok) k[35] = 0.0f;
    if (k[8] <= 0 && k[10] <= 0 && k[11] <= 0 && k[17] <= 0 && k[35] <= 0) return 0;
    /* descriptors: this frame's block of the ring; slots 0 copy, 1 ao, 2 bloom0, 3 depth, 4 bloom1 */
    const UINT inc = dev->lpVtbl->GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT rinc = dev->lpVtbl->GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const int ri = s_po_ri; s_po_ri = (s_po_ri + 1) % PO_RING;
    D3D12_CPU_DESCRIPTOR_HANDLE ch0; RSX_D3D12_CPU_HEAP_START(s_po_heap)(s_po_heap, &ch0); ch0.ptr += (SIZE_T)(ri * PO_DPF) * inc;
    D3D12_GPU_DESCRIPTOR_HANDLE gh0; RSX_D3D12_GPU_HEAP_START(s_po_heap)(s_po_heap, &gh0); gh0.ptr += (UINT64)(ri * PO_DPF) * inc;
    D3D12_CPU_DESCRIPTOR_HANDLE rh0; RSX_D3D12_CPU_HEAP_START(s_po_rtvh)(s_po_rtvh, &rh0); rh0.ptr += (SIZE_T)(ri * 4) * rinc;
    /* tables: [copy, ao, b0, depth] (passes ao / bright / final), [b0, ao, b0, depth] (blur from b0), [b1, ao, b0, depth] (blur from b1) */
    ID3D12Resource* zz = z ? z : s_po_copy;   /* v1117F: the grade pass has no depth; any valid view in t3 */
    ID3D12Resource* tabs[3][4] = { { s_po_copy, s_po_aot, s_po_b[0], zz }, { s_po_b[0], s_po_aot, s_po_b[0], zz }, { s_po_b[1], s_po_aot, s_po_b[0], zz } };
    for (int t = 0; t < 3; t++) for (int j = 0; j < 4; j++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv; memset(&sv, 0, sizeof sv);
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
        ID3D12Resource* r = tabs[t][j];
        sv.Format = (j == 3 && z) ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : r == s_po_copy ? F : r == s_po_aot ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R11G11B10_FLOAT;
        D3D12_CPU_DESCRIPTOR_HANDLE h = ch0; h.ptr += (SIZE_T)(t * 4 + j) * inc;
        dev->lpVtbl->CreateShaderResourceView(dev, r, &sv, h);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[4]; ID3D12Resource* rtr[4] = { s_po_aot, s_po_b[0], s_po_b[1], rt };
    for (int j = 0; j < 4; j++) { rtv[j] = rh0; rtv[j].ptr += (SIZE_T)j * rinc; dev->lpVtbl->CreateRenderTargetView(dev, rtr[j], NULL, rtv[j]); }
    /* the scene copy (source of bloom and of the final pass) */
    po_bar(cl, rt, rtst, D3D12_RESOURCE_STATE_COPY_SOURCE);
    po_bar(cl, s_po_copy, &s_po_st_copy, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->lpVtbl->CopyResource(cl, s_po_copy, rt);
    po_bar(cl, s_po_copy, &s_po_st_copy, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_po_sig);
    cl->lpVtbl->SetDescriptorHeaps(cl, 1, &s_po_heap);
    cl->lpVtbl->IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
#define PO_PASS(pso, rti, tw, th, tab) do { \
        D3D12_VIEWPORT vp = { 0, 0, (float)(tw), (float)(th), 0, 1 }; D3D12_RECT sc = { 0, 0, (LONG)(tw), (LONG)(th) }; \
        cl->lpVtbl->RSSetViewports(cl, 1, &vp); cl->lpVtbl->RSSetScissorRects(cl, 1, &sc); \
        cl->lpVtbl->OMSetRenderTargets(cl, 1, &rtv[rti], FALSE, NULL); cl->lpVtbl->SetPipelineState(cl, pso); \
        cl->lpVtbl->SetGraphicsRoot32BitConstants(cl, 0, 36, k, 0); \
        D3D12_GPU_DESCRIPTOR_HANDLE g = gh0; g.ptr += (UINT64)((tab) * 4) * inc; cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 1, g); \
        cl->lpVtbl->DrawInstanced(cl, 3, 1, 0, 0); } while (0)
    const UINT aw = (W + 1) / 2, ah = (H + 1) / 2, bw = (W + 3) / 4, bh = (H + 3) / 4;
    if (k[8] > 0) {
        po_bar(cl, s_po_aot, &s_po_st_ao, D3D12_RESOURCE_STATE_RENDER_TARGET);
        PO_PASS(s_po_ao, 0, aw, ah, 0);
        po_bar(cl, s_po_aot, &s_po_st_ao, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    } else po_bar(cl, s_po_aot, &s_po_st_ao, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (k[10] > 0) {
        po_bar(cl, s_po_b[0], &s_po_st_b[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
        PO_PASS(s_po_bright, 1, bw, bh, 0);
        for (int it = 0; it < 2; it++) {
            const float sp = it ? 2.0f : 1.0f;
            po_bar(cl, s_po_b[0], &s_po_st_b[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            po_bar(cl, s_po_b[1], &s_po_st_b[1], D3D12_RESOURCE_STATE_RENDER_TARGET);
            k[14] = sp / bw; k[15] = 0; PO_PASS(s_po_blur, 2, bw, bh, 1);
            po_bar(cl, s_po_b[1], &s_po_st_b[1], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            po_bar(cl, s_po_b[0], &s_po_st_b[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
            k[14] = 0; k[15] = sp / bh; PO_PASS(s_po_blur, 1, bw, bh, 2);
        }
    }
    po_bar(cl, s_po_b[0], &s_po_st_b[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    po_bar(cl, rt, rtst, D3D12_RESOURCE_STATE_RENDER_TARGET);
    PO_PASS(s_po_fin[fi], 3, W, H, 0);
#undef PO_PASS
    static int nl = 0; if (nl < 3) { nl++; fprintf(stderr, "[post] frame: AO %s (%.1f), wet %.2f, bloom %.2f, grade %.0f on %ux%u\n", k[8] > 0 ? "on" : "off", k[8], k[35], k[10], k[11], W, H); }
    return 1;
}
