/* v1043C CPU gate: keep the port playable on CPUs without AVX2/FMA.
 *
 * The lifted SPU images (VP6 movie decoder, RWAC audio mixer, SPURS jobs) are compiled with -mavx2 -mfma
 * (v1042Z PORT_SPU_SIMD) -- worth ~18% on the movie SPU alone, but an executable that uses them dies with an
 * illegal-instruction fault on the first SPU job on a CPU without AVX2 (Sandy/Ivy Bridge Core i, every Pentium /
 * Celeron before Alder Lake, AMD FX / Jaguar / Phenom). That is a sizeable part of the "weak PC" population.
 *
 * The build now links the same PPU/runtime objects twice (CMakeLists.txt v1043C): GodfatherRecomp.exe (SPU code
 * SSE4.2 + AVX2 + FMA) and GodfatherRecomp-compat.exe (SPU code SSE4.2 only; bit-identical results, fmaf from the
 * CRT). This file is compiled into BOTH executables WITHOUT any -m flags (GF_SPU_COMPAT tells them apart) and runs
 * from the .CRT$XCB initializer section, i.e. before every C/C++ constructor of the program (all of those, incl.
 * the SPU image registrations, live in .CRT$XCU), so no AVX2 instruction can run before the check.
 *
 * AVX2 exe on a CPU without AVX2+FMA (or with GF_FORCE_COMPAT=1): start GodfatherRecomp-compat.exe from the same
 * folder with the same command line, environment, working directory and std handles (play.cmd's log redirection
 * keeps working), inside a kill-on-close job object (closing / killing this process also ends the child), wait for
 * it and exit with its exit code. No compat exe next to it: a message box instead of a crash.
 * GF_FORCE_COMPAT=1 exists to test the fallback on an AVX2 machine; GF_CPUGATE=0 disables the gate. */
#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GF_SPU_COMPAT
#define GF_SPU_COMPAT 0
#endif

int g_gf_spu_compat_build = GF_SPU_COMPAT;

#if defined(__clang__) || defined(__GNUC__)
__attribute__((target("xsave")))
#endif
static unsigned long long gf_xgetbv0(void) { return _xgetbv(0); }

/* AVX2 + FMA + the OS saving YMM state (OSXSAVE, XCR0 bits 1-2). */
static int gf_cpu_has_avx2_fma(void)
{
    int r[4];
    __cpuid(r, 0);
    if (r[0] < 7) return 0;
    __cpuid(r, 1);
    const int fma = (r[2] >> 12) & 1, osxsave = (r[2] >> 27) & 1, avx = (r[2] >> 28) & 1;
    const int sse42 = (r[2] >> 20) & 1;
    if (!fma || !osxsave || !avx || !sse42) return 0;
    if ((gf_xgetbv0() & 6) != 6) return 0;
    const int movbe = (r[2] >> 22) & 1;   /* v1113I: the lifted PPU code of the main exe also uses MOVBE/BMI1/BMI2/LZCNT */
    __cpuidex(r, 7, 0);
    const int avx2 = (r[1] >> 5) & 1, bmi1 = (r[1] >> 3) & 1, bmi2 = (r[1] >> 8) & 1;   /* EBX bits 5 / 3 / 8 */
    __cpuid(r, (int)0x80000000);
    int lzcnt = 0;
    if ((unsigned)r[0] >= 0x80000001u) { __cpuid(r, (int)0x80000001); lzcnt = (r[2] >> 5) & 1; }   /* ECX bit 5 = ABM/LZCNT */
    return avx2 && bmi1 && bmi2 && lzcnt && movbe;
}

static int gf_cpu_has_sse42(void)
{
    int r[4];
    __cpuid(r, 1);
    return ((r[2] >> 20) & 1) && ((r[2] >> 19) & 1);   /* SSE4.2 + SSE4.1 */
}

static int gf_env_is(const char* name, char c)
{
    char buf[8];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof buf);
    return n > 0 && n < sizeof buf && buf[0] == c;
}

static void gf_cpugate_run(void)
{
    if (gf_env_is("GF_CPUGATE", '0')) return;
    if (GF_SPU_COMPAT) {
        fprintf(stderr, "[V1043C] compat executable: SPU code built for SSE4.2 (no AVX2/FMA needed)\n");
        if (!gf_cpu_has_sse42()) {
            MessageBoxA(NULL,
                "This CPU does not support SSE4.2, which The Godfather PC port needs at minimum.\n"
                "(Every Intel Core i and AMD CPU since about 2009-2011 has it.)",
                "The Godfather", MB_OK | MB_ICONERROR);
            ExitProcess(3);
        }
        return;
    }
    const int force = gf_env_is("GF_FORCE_COMPAT", '1');
    if (!force && gf_cpu_has_avx2_fma()) return;

    wchar_t self[MAX_PATH * 2], compat[MAX_PATH * 2 + 32];
    DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH * 2);
    if (!n || n >= MAX_PATH * 2) return;
    wcscpy(compat, self);
    wchar_t* dot = wcsrchr(compat, L'.');
    wchar_t* slash = wcsrchr(compat, L'\\');
    if (!dot || (slash && dot < slash)) dot = compat + wcslen(compat);
    wcscpy(dot, L"-compat.exe");
    if (GetFileAttributesW(compat) == INVALID_FILE_ATTRIBUTES) {
        if (force) { fprintf(stderr, "[V1043C] GF_FORCE_COMPAT=1 but no %ls -- running the AVX2 build\n", compat); return; }
        fprintf(stderr, "[V1043C] CPU without AVX2/FMA and no compat executable next to this one -- cannot run\n");
        MessageBoxW(NULL,
            L"Your CPU does not support AVX2/FMA, which GodfatherRecomp.exe needs.\n\n"
            L"GodfatherRecomp-compat.exe (the build for older CPUs) was not found next to it. "
            L"Rebuild with build.cmd (it builds both) or copy it into the same folder.",
            L"The Godfather", MB_OK | MB_ICONERROR);
        ExitProcess(3);
    }
    fprintf(stderr, "[V1043C] %s -> starting %ls\n",
            force ? "GF_FORCE_COMPAT=1" : "CPU has no AVX2/FMA", compat);
    fflush(stderr);

    STARTUPINFOW si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    /* the std handles must be inheritable for the child to write into play.cmd's log files */
    HANDLE hs[3] = { si.hStdInput, si.hStdOutput, si.hStdError };
    for (int i = 0; i < 3; i++)
        if (hs[i] && hs[i] != INVALID_HANDLE_VALUE) SetHandleInformation(hs[i], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        memset(&li, 0, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    wchar_t* cmdline = _wcsdup(GetCommandLineW());
    if (!CreateProcessW(compat, cmdline, NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "[V1043C] could not start the compat executable (error %lu)\n", (unsigned long)GetLastError());
        if (force) return;
        MessageBoxW(NULL, L"Could not start GodfatherRecomp-compat.exe (needed on CPUs without AVX2).",
                    L"The Godfather", MB_OK | MB_ICONERROR);
        ExitProcess(3);
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    /* the child inherits this process's affinity / priority class (a restricted soak affinity stays in force) */
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    ExitProcess(code);
}

/* .CRT$XCB sorts before .CRT$XCC (init_seg(compiler)), .CRT$XCL (lib) and .CRT$XCU (every ordinary
 * C++ dynamic initializer and __attribute__((constructor))) */
typedef void (__cdecl *gf_crt_init_fn)(void);
#pragma section(".CRT$XCB", read)
__declspec(allocate(".CRT$XCB")) gf_crt_init_fn g_gf_cpugate_init = gf_cpugate_run;
#if defined(_MSC_VER)
#pragma comment(linker, "/include:g_gf_cpugate_init")
#endif
#else
int g_gf_spu_compat_build = 0;
#endif
