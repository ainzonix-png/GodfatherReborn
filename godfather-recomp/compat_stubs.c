/* Compatibility shims for optional, inFamous-specific extensions present in
 * the playbook-tested runtime snapshot. Godfather does not ship the lifted
 * WWS plugin pack or the inFamous LOD experiment that normally define these
 * symbols, so their neutral behavior belongs in the port rather than in the
 * shared runtime. */
#include <stdint.h>
#include <stdlib.h>

#include "spu_context.h"

typedef void (*spu_plug_fn)(spu_context*);

/* v1039A: spu_plug_lookup now lives in spu/gfmix/spu_recomp.c (content-verified RWAC mixer lift). */

volatile int g_lodx_ab_set = -1;

int ps3_stdout_immediate(void)
{
    const char* value = getenv("PS3_STDOUT_IMMEDIATE");
    return value && atoi(value) != 0;
}
