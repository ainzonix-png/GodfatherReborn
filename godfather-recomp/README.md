# The Godfather: The Don's Edition recomp

Early native Windows port of the European PS3 release (`BLES00043`) using
`ps3recomp`.

## Current state

- Retail SELF decrypted to `EBOOT.elf` with the locally configured RPCS3.
- 40,400 PPU functions lifted into ten translation units.
- Three embedded SPU images extracted, lifted, and registered.
- Native D3D12 boot, asset I/O, audio setup, controller polling, save-data,
  lifted SPU execution, movie loading, and sustained rendering are verified.
- A 55-second soak remained alive past 990 flips. Gameplay is not yet claimed:
  the loading-movie SPU still spends most of its time interpreting a generated
  code block at local-store address `0x1EE0`.
- The launcher now bypasses only `load_en.vp6`, allowing native execution to
  continue into frontend and Mobface asset loading. The frontend submits 6–11
  draws per flip, but its presented image is still black.
- The audio ring starts in the correct unwritten-sentinel state and the missing
  pad sensor-mode APIs used by this title are implemented.

## Build and run

Run `build.cmd`, then `play.cmd`. `play.cmd` points the runtime at the adjacent
disc dump and writes `play.out.log` and `play.err.log` here.

For an unattended build/run check, use:

```powershell
powershell -ExecutionPolicy Bypass -File .\verify-soak.ps1 -DurationSeconds 240 -StallSeconds 10 -TargetFps 60
```

The verifier launches and stops only its own game process, stores timestamped
logs under `verify-logs`, fails on fatal runtime diagnostics or stalled flips,
and reports average and fifth-percentile FPS. `-PadScript` accepts the runtime's
wall-clock input format (for example, `"5:0x0008,8:0x4000"` for START then
CROSS), while `-AutoPress` enables the existing headless boot-screen input.
Use `-ContinueAfterFatal` when collecting a full soak despite an early error.

If SPU sources must be regenerated, run `lift-spu.cmd` before `build.cmd`. It
preserves the decoder kernel's required indirect continuation entries.

The port requires the user's legally obtained PS3 game dump. Game assets and
the decrypted executable are not redistributable parts of this source tree.

See `HANDOFF.md` for verified status, blockers, and the next diagnostic pass.
