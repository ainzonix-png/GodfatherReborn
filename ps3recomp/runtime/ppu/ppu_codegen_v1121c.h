/* ppu_codegen_v1121c.h -- v1121C codegen switches for the lifted PPU TUs.
 * Included ONLY by TUs rewritten by ps3recomp/tools/ppu_codegen_v1121c.py, which runs after ppu_memacc_v243.py in the
 * PORT_PPU_MEMLEAN all-function build when one of the CMake options GF_PPU_VMB_LOCAL / GF_PPU_RESTRICT is ON. The
 * script writes the matching "#define GF_PPU_* 1" lines in front of the #include, so with every option OFF nothing
 * includes this file and nothing changes.
 *
 * GF_PPU_VMB_LOCAL: every rewritten function starts with GF_PPU_FN_ENTRY, a local copy of vm_base, and the lean
 *   accessors use that copy (PPU_VMB). vm_base is assigned once in main() before the first guest instruction and never
 *   changes, so the copy always equals the global. Without it every guest load/store re-reads the global after any
 *   store through a host pointer (each ctx->gpr / fpr store may alias vm_base under -fno-strict-aliasing).
 *   Code outside the rewritten bodies falls back to the file-scope reference below (reads the global every time,
 *   exactly the old behaviour).
 * GF_PPU_RESTRICT (experimental): the rewritten functions take ctx as __restrict (GF_PPU_CTX_RESTRICT). Only pays off
 *   together with the larger capture-tracking budget CMake adds for this option; see NOTES_v1121C_codegen.md for the
 *   contract (nothing may write a running function's context through g_active_ctx while that function has not passed
 *   ctx to the callee). */
#pragma once
#include <stdint.h>

#if defined(GF_PPU_VMB_LOCAL) && GF_PPU_VMB_LOCAL
#ifdef __cplusplus
extern "C" {
#endif
extern uint8_t* vm_base;
#ifdef __cplusplus
}
static uint8_t* const& _gf_vmb = vm_base;   /* fallback: same object, re-read on every use */
#endif
#undef PPU_VMB
#define PPU_VMB _gf_vmb
#define GF_PPU_FN_ENTRY uint8_t* const _gf_vmb = vm_base;
#else
#define GF_PPU_FN_ENTRY
#endif

#if defined(GF_PPU_RESTRICT) && GF_PPU_RESTRICT
#define GF_PPU_CTX_RESTRICT __restrict
#else
#define GF_PPU_CTX_RESTRICT
#endif
