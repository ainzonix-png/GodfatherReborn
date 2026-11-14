@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b %errorlevel%
rem installer jobs (2026-11-09): when _runner\hook.cmd exists, a "build" job runs IT instead of a build (once).
if exist "%~dp0_runner\hook.cmd" goto :runhook
rem v1042Z: pending source patches (_patches\apply-pending.ps1, idempotent) -- used for files too large to copy
rem back from a cloud session. Applied before cmake so this build compiles them.
if exist "%~dp0_patches\apply-pending.ps1" powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0_patches\apply-pending.ps1"
if errorlevel 1 exit /b %errorlevel%

cmake -S "%~dp0." -B "%~dp0build-perf" -G Ninja ^
  -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DPORT_PPU_MEMLEAN=OFF ^
  -DCMAKE_C_FLAGS="/DWIN32 /D_WINDOWS" ^
  -DCMAKE_CXX_FLAGS="/DWIN32 /D_WINDOWS /EHsc" ^
  -DCMAKE_C_FLAGS_RELWITHDEBINFO="/Zi /O2 /Ob1 /DNDEBUG" ^
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="/Zi /O2 /Ob1 /DNDEBUG" ^
  -DCMAKE_EXE_LINKER_FLAGS="/machine:x64" ^
  -DCMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO="/debug /INCREMENTAL" ^
  -DCMAKE_STATIC_LINKER_FLAGS="/machine:x64" ^
  -DCMAKE_C_COMPILER=clang-cl ^
  -DCMAKE_CXX_COMPILER=clang-cl ^
  -DPython3_EXECUTABLE="C:\Users\MitNa\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe" ^
  -DPS3RECOMP_DIR="%~dp0..\ps3recomp"
if errorlevel 1 exit /b %errorlevel%

cmake --build "%~dp0build-perf" --parallel 3
if errorlevel 1 exit /b %errorlevel%

rem v1041R: the RelWithDebInfo/linker flags above are pinned explicitly: build-perf's cache had them EMPTY
rem since 18:55 on Oct 31, so everything compiled at -O0 (the "gameplay got much slower" build).
rem v1041R: A/B variant. With build-memlean.flag present, also build the lean-accessor variant into build-memlean2.
if exist "%~dp0build-memlean.flag" (
  cmake -S "%~dp0." -B "%~dp0build-memlean2" -G Ninja ^
    -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
    -DPORT_PPU_MEMLEAN=ON ^
    -DCMAKE_C_FLAGS="/DWIN32 /D_WINDOWS" ^
    -DCMAKE_CXX_FLAGS="/DWIN32 /D_WINDOWS /EHsc" ^
    -DCMAKE_C_FLAGS_RELWITHDEBINFO="/Zi /O2 /Ob1 /DNDEBUG" ^
    -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="/Zi /O2 /Ob1 /DNDEBUG" ^
    -DCMAKE_EXE_LINKER_FLAGS="/machine:x64" ^
    -DCMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO="/debug /INCREMENTAL" ^
    -DCMAKE_STATIC_LINKER_FLAGS="/machine:x64" ^
    -DCMAKE_C_COMPILER=clang-cl ^
    -DCMAKE_CXX_COMPILER=clang-cl ^
    -DPython3_EXECUTABLE="C:\Users\MitNa\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe" ^
    -DPS3RECOMP_DIR="%~dp0..\ps3recomp"
  if errorlevel 1 exit /b %errorlevel%
  cmake --build "%~dp0build-memlean2" --parallel 3
)
if errorlevel 1 exit /b %errorlevel%
rem v1057T: call-count tracker build (build-fncount\, launcher play-fncount.cmd) while build-fncount.flag exists.
if exist "%~dp0build-fncount.flag" (
  cmake -S "%~dp0." -B "%~dp0build-fncount" -G Ninja ^
    -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
    -DPORT_PPU_MEMLEAN=OFF -DGF_FNCOUNT=ON ^
    -DCMAKE_C_FLAGS="/DWIN32 /D_WINDOWS" ^
    -DCMAKE_CXX_FLAGS="/DWIN32 /D_WINDOWS /EHsc" ^
    -DCMAKE_C_FLAGS_RELWITHDEBINFO="/Zi /O2 /Ob1 /DNDEBUG" ^
    -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="/Zi /O2 /Ob1 /DNDEBUG" ^
    -DCMAKE_EXE_LINKER_FLAGS="/machine:x64" ^
    -DCMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO="/debug /INCREMENTAL" ^
    -DCMAKE_STATIC_LINKER_FLAGS="/machine:x64" ^
    -DCMAKE_C_COMPILER=clang-cl ^
    -DCMAKE_CXX_COMPILER=clang-cl ^
    -DPython3_EXECUTABLE="C:\Users\MitNa\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe" ^
    -DPS3RECOMP_DIR="%~dp0..\ps3recomp"
  if errorlevel 1 exit /b %errorlevel%
  cmake --build "%~dp0build-fncount" --parallel 3
)
exit /b %errorlevel%

:runhook
move /y "%~dp0_runner\hook.cmd" "%~dp0_runner\hook-running.cmd" >nul
call "%~dp0_runner\hook-running.cmd"
exit /b %errorlevel%
