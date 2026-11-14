/* Loopback test for the live co-op pose module (v2.2):
 * gcc -I.. rsx_coop_pose.c tests/test_coop_pose.c -lm -lpthread */
#define _POSIX_C_SOURCE 200809L
#include "../rsx_coop_pose.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STRIDE 176u
#define TAN 10u
#define MESH 3200u
static uint8_t wire[1024][1300]; static uint32_t wire_len[1024]; static unsigned wire_n;
static int fake_send(const void* d, uint32_t n) { memcpy(wire[wire_n], d, n); wire_len[wire_n++] = n; return 1; }
static uint64_t mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u; }
static void sleep_ms(int ms) { struct timespec t = { 0, ms * 1000000L }; nanosleep(&t, NULL); }
static void deliver(void) { for (unsigned i = 0; i < wire_n; ++i) rsx_coop_pose_receive(wire[i], wire_len[i], mono_ms()); wire_n = 0; }

static void mesh_vertex(uint32_t gi, const float anchor[3], float* v)
{
    for (int s = 0; s < 11; ++s) { v[s*4+0] = 100.0f + s; v[s*4+1] = 200.0f + gi; v[s*4+2] = 0.25f; v[s*4+3] = -1.0f; }
    v[0] = anchor[0] + 30.0f * sinf(gi * 0.01f);
    v[1] = anchor[1] + 30.0f * cosf(gi * 0.013f);
    v[2] = anchor[2] + (float)(gi % 170) - 85.0f;
    float n[3] = { sinf(gi * 0.1f), cosf(gi * 0.1f), 0.3f };
    float l = sqrtf(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
    v[8] = n[0]/l; v[9] = n[1]/l; v[10] = n[2]/l;
    v[TAN*4+0] = 1; v[TAN*4+1] = 0; v[TAN*4+2] = 0; v[TAN*4+3] = (gi & 1) ? 1.0f : -1.0f;
}

int main(void)
{
    const uint64_t key = 0x2B28BC002B28BC4ull, sub = key ^ 0x1234ull;
    float anchor[3] = { -71046.0f, 5608.0f, 108.0f };
    uint8_t* win = malloc(MESH * STRIDE); uint8_t* dst = malloc(MESH * STRIDE);
    uint32_t tris[3 * MESH]; uint32_t ib[3 * MESH];

    /* "Remote" knows triangles over 0..1999 (its camera); vertex 500 is garbage. */
    uint32_t nt = 0;
    for (uint32_t i = 0; i + 2 <= 2000; i += 2) { tris[nt++] = i; tris[nt++] = i + 1; tris[nt++] = i + 2; }
    rsx_coop_pose_note_triangles(sub, key, tris, nt);
    uint32_t lo, cnt;
    assert(rsx_coop_pose_part_range(key, &lo, &cnt) && lo == 0 && cnt == 2001);
    for (uint32_t i = 0; i < cnt; ++i) mesh_vertex(lo + i, anchor, (float*)(win + (size_t)i * STRIDE));
    ((float*)(win + 500u * STRIDE))[0] = 1e9f;
    assert(rsx_coop_pose_send_begin(anchor));
    rsx_coop_pose_send_part(key, lo, cnt, win, STRIDE, TAN);
    rsx_coop_pose_send_end();
    unsigned sent = rsx_coop_pose_drain_outgoing(fake_send);
    size_t bytes = 0; for (unsigned i = 0; i < wire_n; ++i) bytes += wire_len[i];
    printf("frame 1: %u packets, %zu bytes\n", sent, bytes);

    /* Local instance saw only triangles over 1000..2999 (its camera), and has
     * static slots for its own whole range. Deliver remote data in reverse. */
    for (int i = (int)wire_n - 1; i >= 0; --i) rsx_coop_pose_receive(wire[i], wire_len[i], mono_ms());
    wire_n = 0;
    nt = 0;
    for (uint32_t i = 1000; i + 2 <= 3000; i += 2) { tris[nt++] = i + 2; tris[nt++] = i + 1; tris[nt++] = i; }
    rsx_coop_pose_note_triangles(sub, key, tris, nt);
    assert(rsx_coop_pose_part_range(key, &lo, &cnt) && lo == 0 && cnt == 2001 + 1000);
    assert(rsx_coop_pose_template_needed(key, lo, cnt));
    float zero[3] = { 0, 0, 0 };
    for (uint32_t i = 0; i < cnt; ++i) {
        mesh_vertex(i, zero, (float*)(win + (size_t)i * STRIDE));
        ((float*)(win + (size_t)i * STRIDE))[9*4] = 7.0f + i;
    }
    rsx_coop_pose_template_store(key, lo, cnt, win, STRIDE);
    assert(!rsx_coop_pose_template_needed(key, lo, cnt));

    assert(rsx_coop_pose_render_begin());
    uint32_t gmin = 0;
    const uint32_t vc = rsx_coop_pose_write_part(key, STRIDE, TAN, dst, MESH, &gmin);
    assert(vc == 2001 && gmin == 0);   /* samples only for 0..2000 */
    double maxerr = 0;
    for (uint32_t i = 0; i < vc; ++i) {
        const float* o = (const float*)(dst + (size_t)i * STRIDE);
        if (i == 500) { assert(isnan(o[0])); continue; }
        float r[44]; mesh_vertex(i, anchor, r);
        for (int k = 0; k < 3; ++k) { double e = fabs(o[k] - r[k]); if (e > maxerr) maxerr = e; }
        assert(o[9*4] == 7.0f + i);
        assert(o[TAN*4+3] == r[TAN*4+3]);
        for (int k = 0; k < 3; ++k) assert(fabsf(o[8+k] - r[8+k]) < 0.02f);
    }
    const uint32_t ni = rsx_coop_pose_write_indices(sub, gmin, vc, ib, 3 * MESH);
    /* Union = 999 remote tris (0..1999) + local tris inside 1000..1999 dedupe by
     * winding only if identical; local ones are reversed -> distinct.  Drawable:
     * all with every vertex < 2000 and != 500. */
    for (uint32_t i = 0; i < ni; ++i) assert(ib[i] < vc && ib[i] != 500);
    printf("max position error %.3f cm, %u drawable triangles\n", maxerr, ni / 3);
    assert(maxerr <= 0.051 && ni / 3 > 1000 - 3);
    assert(rsx_coop_pose_render_end());

    /* The local instance sends its triangles to the peer; peer inserts them. */
    sleep_ms(80);
    assert(rsx_coop_pose_send_begin(anchor));
    rsx_coop_pose_send_end();
    rsx_coop_pose_drain_outgoing(fake_send);
    unsigned tri_packets = wire_n;
    deliver();   /* loops back into the same module: all duplicates */
    rsx_coop_pose_stats st; rsx_coop_pose_get_stats(&st);
    printf("triangle packets %u, known triangles %u\n", tri_packets, st.known_triangles);
    assert(tri_packets > 0 && st.known_triangles == 1000u + 1000u);

    /* polish r1: playback must interpolate between buffered samples.  The old
     * receiver kept only two samples per vertex while playing ~2 intervals
     * behind the newest, so it always clamped to the older one: the peer's
     * animation stepped at the 15 Hz send rate and never blended.  Stream 5
     * frames in which both the anchor (root) and the vertices move, deliver
     * one frame out of order, and check that playback lands strictly between
     * two sent poses and advances smoothly. */
    rsx_coop_pose_reset();
    {
        const uint32_t N = 64;
        float vx0[8];               /* x of vertex 10 per frame */
        unsigned frames = 0;
        uint8_t held[8][4][1300]; uint32_t held_len[8][4]; unsigned held_n[8];
        while (frames < 5) {
            float a[3] = { anchor[0] + 50.0f * frames, anchor[1], anchor[2] };
            if (!rsx_coop_pose_send_begin(a)) { sleep_ms(5); continue; }
            for (uint32_t i = 0; i < N; ++i) {
                mesh_vertex(i, a, (float*)(win + (size_t)i * STRIDE));
                ((float*)(win + (size_t)i * STRIDE))[1] += 20.0f * frames;   /* limb swing */
            }
            vx0[frames] = ((float*)(win + 10u * STRIDE))[1];
            rsx_coop_pose_send_part(key, 0, N, win, STRIDE, TAN);
            rsx_coop_pose_send_end();
            rsx_coop_pose_drain_outgoing(fake_send);
            /* keep only pose packets for this test */
            held_n[frames] = 0;
            for (unsigned i = 0; i < wire_n; ++i)
                if (wire[i][0] == 0xC3 && held_n[frames] < 4) {
                    memcpy(held[frames][held_n[frames]], wire[i], wire_len[i]);
                    held_len[frames][held_n[frames]++] = wire_len[i];
                }
            wire_n = 0;
            /* frame 2 arrives after frame 3 (reordering) */
            if (frames != 2) for (unsigned i = 0; i < held_n[frames]; ++i) rsx_coop_pose_receive(held[frames][i], held_len[frames][i], mono_ms());
            if (frames == 3) for (unsigned i = 0; i < held_n[2]; ++i) rsx_coop_pose_receive(held[2][i], held_len[2][i], mono_ms());
            ++frames;
            sleep_ms(1);
        }
        float last = -1e30f; int between = 0;
        for (int step = 0; step < 6; ++step) {
            assert(rsx_coop_pose_render_begin());
            uint32_t g = 0;
            const uint32_t c = rsx_coop_pose_write_part(key, STRIDE, TAN, dst, MESH, &g);
            assert(c >= N && g == 0);
            const float y = ((const float*)(dst + 10u * STRIDE))[1];
            assert(!isnan(y));
            assert(y >= vx0[0] - 0.06f && y <= vx0[4] + 0.06f);
            int on_sample = 0;
            for (unsigned f = 0; f < 5; ++f) if (fabsf(y - vx0[f]) < 0.2f) on_sample = 1;
            if (!on_sample) ++between;
            assert(y >= last - 0.001f);           /* never runs backwards */
            last = y;
            rsx_coop_pose_render_end();
            sleep_ms(12);
        }
        printf("interpolated playback: %d of 6 frames between samples, y %.2f\n", between, last);
        assert(between >= 3);
        /* Stop streaming: the vertex must disappear (NaN) rather than freeze
         * for a whole second. */
        sleep_ms(600);
        assert(rsx_coop_pose_render_begin());
        uint32_t g = 0;
        rsx_coop_pose_write_part(key, STRIDE, TAN, dst, MESH, &g);
        assert(isnan(((const float*)(dst + 10u * STRIDE))[0]));
        rsx_coop_pose_render_end();
        puts("interpolation / reorder / stale tests passed");
    }

    /* Round 4: the peer's game swaps Cole to part streams this instance never
     * drew (climbing / close camera: other stream keys).  Before: no local
     * draw and no local template for the new key -> "peer parts drawn 0/25"
     * and the blocky proxy.  Now the sender announces the submesh material,
     * the receiver asks for the part's static slots, the sender streams them,
     * and the part becomes drawable through a same-material local draw. */
    rsx_coop_pose_reset();
    {
        const uint64_t keyC = 0x303A8F60303A8F0ull, subC = keyC ^ 0xBEEFull;
        const uint32_t N = 600, TEX = 0x0ABC1000u;
        uint32_t tc[3 * 600]; uint32_t ntc = 0;
        for (uint32_t i = 0; i + 2 < N; i += 2) { tc[ntc++] = i; tc[ntc++] = i + 1; tc[ntc++] = i + 2; }
        /* sender side of the loopback: it draws C (triangles + material) */
        rsx_coop_pose_note_triangles(subC, keyC, tc, ntc);
        rsx_coop_pose_note_material(subC, keyC, TEX, STRIDE, TAN);
        int drawable = 0, frames = 0, saw_want = 0, saw_tmpl = 0, saw_mat = 0;
        while (frames < 200 && !drawable) {
            float a[3] = { anchor[0] + frames, anchor[1], anchor[2] };
            if (!rsx_coop_pose_send_begin(a)) { sleep_ms(5); continue; }
            ++frames;
            for (uint32_t i = 0; i < N; ++i) mesh_vertex(i, a, (float*)(win + (size_t)i * STRIDE));
            rsx_coop_pose_send_part(keyC, 0, N, win, STRIDE, TAN);
            rsx_coop_pose_send_end();
            rsx_coop_pose_drain_outgoing(fake_send);
            for (unsigned i = 0; i < wire_n; ++i) {
                if (wire[i][0] == 0xC2 && wire[i][2] == 0 && wire[i][3] == 0) {
                    saw_mat |= wire[i][1] == 1; saw_tmpl |= wire[i][1] == 2; saw_want |= wire[i][1] == 3;
                }
            }
            deliver();
            /* receiver side: it never drew C, so it has no local template */
            if (!rsx_coop_pose_render_begin()) continue;
            rsx_coop_pose_submat sm[8];
            const uint32_t nsm = rsx_coop_pose_remote_submeshes(sm, 8);
            int found = 0;
            for (uint32_t k = 0; k < nsm; ++k)
                if (sm[k].submesh == subC) { found = 1; assert(sm[k].key == keyC && sm[k].tex0 == TEX && sm[k].stride == STRIDE && sm[k].tangent_slot == TAN); }
            uint32_t g = 0;
            const uint32_t vc = found ? rsx_coop_pose_write_part(keyC, STRIDE, TAN, dst, MESH, &g) : 0;
            uint32_t ni = 0;
            if (vc) {
                ni = rsx_coop_pose_write_indices(subC, g, vc, ib, 3 * MESH);
                if (ni / 3u >= (N - 2u) / 2u) {
                    drawable = 1;
                    for (uint32_t i = 0; i < vc; ++i) {
                        const float* o = (const float*)(dst + (size_t)i * STRIDE);
                        float r[44]; mesh_vertex(g + i, anchor, r);
                        assert(o[9*4] == r[9*4] && o[9*4+1] == r[9*4+1]);    /* UV slot from the peer */
                        assert(o[3*4] == r[3*4]);                             /* colour slot from the peer */
                        assert(o[TAN*4+3] == r[TAN*4+3]);
                    }
                }
            }
            const int complete = rsx_coop_pose_render_end();
            assert(complete == (ni > 0));   /* any drawn part retires the proxy */
            sleep_ms(20);
        }
        uint32_t asked = 0, served = 0, stored = 0;
        rsx_coop_pose_template_counts(&asked, &served, &stored);
        printf("round 4 key switch: drawable after %d frames (materials %d, requests %d, templates %d; asked %u served %u stored %u)\n",
               frames, saw_mat, saw_want, saw_tmpl, asked, served, stored);
        assert(drawable && saw_mat && saw_want && saw_tmpl && stored >= N);
        /* local guest data replaces the peer's template */
        assert(rsx_coop_pose_template_needed(keyC, 0, N));
        rsx_coop_pose_template_store(keyC, 0, N, win, STRIDE);
        assert(!rsx_coop_pose_template_needed(keyC, 0, N));
        /* extension packets carry a zero triangle count (older builds drop them) */
        puts("round 4 key-switch test passed");
    }

    rsx_coop_pose_reset();
    assert(!rsx_coop_pose_render_begin());
    free(win); free(dst);
    puts("coop pose tests passed");
    return 0;
}
