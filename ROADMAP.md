# Roadmap

What is next, what is deferred, and what is out of scope. What is true today
is in the README's Status section and in `docs/`.

## Next

- **Build the host** on netlab (`netlab build mechcommander2-recomp`) and get
  the dry run's two milestones: Mc2Rel.exe mapped, every import bound.
- **First `--run`**: through the CRT startup into GameOS's WinMain, under
  offstage at the console (REPO_RULES section 13). Expect a CD check (the
  SafeDisc build looks for its disc) and a registry install key; shim them
  in the host as MechWarrior 3 did, once a run shows what they ask for.
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
