# Roadmap

What is next, what is deferred, and what is out of scope. What is true today
is in the README's Status section and in `docs/`.

## Next

- **Play the first mission through**: move, fight, win; then the next
  missions. Expect more lift bugs in the same family as fsincos: pcrecomp's
  difftest is the place to pin each one down.
- **GameOS pauses when its window is inactive**, so a test run on a virtual
  monitor stalls while someone uses the desktop. Posting WM_ACTIVATE to keep
  it going broke mouse clicks; find the flag GameOSWinProc keeps instead.
- **The joystick fault** (CMCreateJoystick7 with some HID devices) is hidden,
  not fixed.
- **The driver warm-up** works around a failed first load of the display
  driver from the game's thread; find out why it fails.
- **Windowed by default**: the first run writes options.cfg with
  FullScreen = TRUE; Setup should set it to FALSE.

- **See a frame.** `--run` is alive at 30 s in `SoundRendererUpdate`, but
  offstage recorded nothing on the workstation. Capture on a netlab VM with
  the game (`netlab play`), or through the headless mode below.
- **Move the IsBadReadPtr/IsBadWritePtr shims into native32**: every
  native32 game with a vectored crash handler has the same problem.
- **Name the lifted functions.** The PDB names every function; the lift
  still emits `sub_XXXXXXXX`. Carrying the names into the generated C (and
  the fault report) is a pcrecomp change to the lift driver.
- **Headless mode** (`--headless --record out.mp4`), required by the house
  style. GameOS draws through DirectDraw 7 / Direct3D 7, so this needs a
  host-side DirectDraw.
- **The launcher**: `launcher/launcher.json` for recomp-launcher.

## Deferred

- **The 156 functions with no terminator** and the 562 "invented" catalog
  starts (code the PDB does not list). Look when a run hits one.
- **Multiplayer** (DirectPlay, `dplayx`).
- **Bink movies** run through the real `BINKW32.DLL` from the disc; replace
  only if it fails on Windows 11.

## Out of scope

- The editor (`EDITREL.EXE`) and viewer (`VIEWREL.EXE`). Separate binaries;
  the editor also ships a PDB, so it is a cheap follow-up later.
- Microsoft's 2006 source release, in any form (NOTICE).
- Any game data or symbols in this repository, ever.
