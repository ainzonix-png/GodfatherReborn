#define _CRT_SECURE_NO_WARNINGS
#include "gf_hotfn.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern uint8_t* vm_base;
extern volatile int g_v243a_sel;

int gf_hotfn_on = 0;

enum {
    GF_HOTFN_DISABLE_00696704 = 1u << 0
};

static uint64_t s_gf_hotfn_mask;

static void gf_hotfn_init(void)
{
    const char* enabled = getenv("GF_HOTFN");
    const char* mask = getenv("GF_HOTFN_MASK");
    if (mask && *mask) s_gf_hotfn_mask = strtoull(mask, NULL, 0);
    if (enabled && *enabled && strtol(enabled, NULL, 0) != 0) {
        gf_hotfn_on = 1;
        fprintf(stderr, "[v1121F] native hot functions ON (GF_HOTFN_MASK=0x%llX)\n",
                (unsigned long long)s_gf_hotfn_mask);
        fflush(stderr);
    }
}

#if defined(_MSC_VER)
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU"))
__attribute__((used)) static void (__cdecl* s_gf_hotfn_init)(void) = gf_hotfn_init;
#else
__attribute__((constructor))
static void gf_hotfn_ctor(void)
{
    gf_hotfn_init();
}
#endif

static void gf_cr_field(ppu_context* ctx, unsigned shift, uint32_t value)
{
    ctx->cr = (ctx->cr & ~(0xFu << shift)) | (value << shift);
}

static uint32_t gf_cr_s64(int64_t a, int64_t b)
{
    return a < b ? 8u : a > b ? 4u : 2u;
}

static uint32_t gf_cr_u32(uint32_t a, uint32_t b)
{
    return a < b ? 8u : a > b ? 4u : 2u;
}

int gf_hot_00696704(ppu_context* ctx)
{
    const uint64_t in3 = ctx->gpr[3];
    const uint64_t in4 = ctx->gpr[4];
    const uint64_t n64 = ctx->gpr[5];
    const uint32_t a0 = (uint32_t)in3;
    const uint32_t b0 = (uint32_t)in4;
    uint32_t n;

    if (s_gf_hotfn_mask & GF_HOTFN_DISABLE_00696704) return 0;

    /* Direct byte loads are identical to the validated lean accessor only in
     * its standard memory mode.  Fail back for diagnostics, wrapped ranges,
     * and non-ABI sizes so their full lifted behavior remains observable. */
    if (g_v243a_sel != 3 || n64 > UINT32_MAX) return 0;
    n = (uint32_t)n64;
    if ((uint64_t)a0 + n > 0x100000000ull ||
        (uint64_t)b0 + n > 0x100000000ull) return 0;

    gf_cr_field(ctx, 28, gf_cr_s64((int64_t)n64, 0));
    ctx->gpr[10] = in3;
    ctx->gpr[11] = in4;
    if (n == 0) {
        ctx->gpr[3] = 0;
        return 1;
    }

    ctx->gpr[3] = vm_base[a0];
    ctx->gpr[4] = vm_base[b0];
    gf_cr_field(ctx, 0, gf_cr_s64((int32_t)ctx->gpr[3],
                                  (int32_t)ctx->gpr[4]));
    if (ctx->gpr[3] != ctx->gpr[4]) {
        gf_cr_field(ctx, 0, gf_cr_u32((uint32_t)ctx->gpr[4],
                                      (uint32_t)ctx->gpr[3]));
        ctx->gpr[3] = ctx->gpr[4] > ctx->gpr[3] ? 1 : (uint64_t)(int64_t)-1;
        return 1;
    }

    ctx->gpr[9] = (uint32_t)(n64 - 1);
    ctx->gpr[9] += 1;
    ctx->ctr = (uint32_t)ctx->gpr[9];

    for (;;) {
        ctx->gpr[0] = ctx->gpr[10] + 1;
        ctx->gpr[9] = ctx->gpr[11] + 1;
        ctx->gpr[10] = (uint32_t)ctx->gpr[0];
        ctx->gpr[11] = (uint32_t)ctx->gpr[9];
        ctx->ctr = (uint32_t)(ctx->ctr - 1);
        if (ctx->ctr == 0) {
            ctx->gpr[3] = 0;
            return 1;
        }

        ctx->gpr[3] = vm_base[(uint32_t)ctx->gpr[10]];
        ctx->gpr[4] = vm_base[(uint32_t)ctx->gpr[11]];
        gf_cr_field(ctx, 0, gf_cr_s64((int32_t)ctx->gpr[4],
                                      (int32_t)ctx->gpr[3]));
        if (ctx->gpr[3] != ctx->gpr[4]) {
            gf_cr_field(ctx, 0, gf_cr_u32((uint32_t)ctx->gpr[4],
                                          (uint32_t)ctx->gpr[3]));
            ctx->gpr[3] = ctx->gpr[4] > ctx->gpr[3] ? 1 : (uint64_t)(int64_t)-1;
            return 1;
        }
    }
}
