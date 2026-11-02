# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions
follow [SemVer](https://semver.org/).

## [Unreleased]

### Added
- First scaffold, in the shape of mechwarrior3-recomp: `tools/gamefiles.py`
  (drive, folder, ISO/MDF, or a .zip holding the image), `tools/catalog.py`,
  `run_lift.py`, the native32 host, `Setup.cmd`, conformance harness.
- Function catalog from the disc's own release PDB (`MC2REL.PDB`, VC6 format
  2.00), read by pcrecomp's new `pdb2_ref.py` (pcrecomp#162): 9,776 named
  functions. disasm32 seeded with them: 15,002 functions, 99.99% of the
  PDB's. Unseeded, disasm32 scores 89.7% precision and 85.4% recall.
- Whole-program lift: 15,007 functions, 0 errors, 5.46M lines of C.
- Host builds on netlab and runs: through the CRT and GameOS init, alive
  at 30 s. Fixes on the way: `IsBadReadPtr`/`IsBadWritePtr` answer from
  `VirtualQuery` (the crash handler saw their probe fault first);
  `DirectInputCreateEx` gets the host's module handle, not the mapped
  0x00400000 (E_INVALIDARG); `OutputDebugStringA` goes to the log, which is
  how GameOS reports a STOP.
- **Main menu.** Fixes on the way, all in the host or the setup script:
  - the display driver's user-mode DLL is loaded once before the game
    starts; loaded first from the game's own DirectDrawCreateEx it fails, so
    DirectDraw saw no 3D hardware and GameOS fell back to its software
    rasterizer;
  - the host relaunches itself with `__COMPAT_LAYER=DWM8And16BitMitigation`,
    so the game's 16-bit display mode is allowed (no registry change);
  - no joysticks: GameOS's joystick setup faults with some HID devices, and
    the game is mouse and keyboard;
  - `tools/gamefiles.py` gives the disc's 8.3 file names their install
    names back from SETUP.EXE's file table (`AGENCY_4.D3F` ->
    `assets\Graphicsgencyfb14.d3f`), copying files installed twice;
  - `--ddtest` reports whether DirectDraw finds the hardware at each stage.
- **Into the first mission.** `--input FILE` scripts the mouse ("MS X Y B"
  per line, in window coordinates including the title bar) through the
  host's GetCursorPos/GetAsyncKeyState/GetKeyState, so a run clicks through
  menus without the real cursor. `tools/input/first-mission.txt` goes New
  Campaign, Load, skips the intro movie, and lands in Carver V's first
  mission: HUD, minimap and units are drawn, the 3D view is not.
