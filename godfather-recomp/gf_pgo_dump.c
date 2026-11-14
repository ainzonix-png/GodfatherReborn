/* gf_pgo_dump.c -- v1121C: periodic profile dump for the GF_PPU_PGO=gen build (CMakeLists.txt links this file only
 * there). The game is often ended by closing the window or by the runner killing it, so the profile runtime's own
 * at-exit write may never run; this thread writes the cumulative counters every GF_PGO_DUMP_SEC seconds (default 30,
 * 0 = only at exit) to LLVM_PROFILE_FILE (default default_%p.profraw in the working directory). Each write replaces
 * the previous one, so the last file holds the whole run. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#endif

int __llvm_profile_write_file(void);

#ifdef _WIN32
static DWORD WINAPI gf_pgo_thread(LPVOID p)
{
    DWORD ms = (DWORD)(size_t)p;
    for (;;) {
        Sleep(ms);
        __llvm_profile_write_file();
    }
    return 0;
}

static void gf_pgo_start(void)
{
    const char* e = getenv("GF_PGO_DUMP_SEC");
    int sec = e ? atoi(e) : 30;
    fprintf(stderr, "[v1121C] PGO instrumented build: profile dump every %d s ON (LLVM_PROFILE_FILE=%s)\n",
            sec, getenv("LLVM_PROFILE_FILE") ? getenv("LLVM_PROFILE_FILE") : "default_%p.profraw");
    if (sec > 0) {
        HANDLE h = CreateThread(NULL, 0, gf_pgo_thread, (LPVOID)(size_t)(sec * 1000u), 0, NULL);
        if (h) CloseHandle(h);
    }
}

/* run before main() through the CRT initializer table */
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) void (*gf_pgo_init)(void) = gf_pgo_start;
#endif
