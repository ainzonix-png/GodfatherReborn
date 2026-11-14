# Contributing

Thanks for helping out. A few ground rules:

- **Never share game files.** Do not attach, upload or link disc images, game
  files, the decrypted `EBOOT.elf`, recompiled code (`godfather-recomp/recompiled`),
  built executables or memory dumps of the game. Issues and pull requests
  containing them will be removed.
- **Code in this repository must be original.** Hooks may refer to game
  functions by address, but must not contain copied or recompiled game code.

## Reporting bugs

Include where in the game it happens, what you expected, and your CPU, GPU and
driver version. Attach `godfather-recomp/play.out.log` and
`godfather-recomp/play.err.log` from your install. A screenshot you took
yourself is fine.

## Making changes

- Game-specific behaviour goes in `godfather-recomp/gf_*.c`.
- Changes to the recompiled code are kept as patches in `installer/patches/`
  (made with `godfather-installer make-patches`).
- Keep comments short and factual: what a hook does and why it is needed.
