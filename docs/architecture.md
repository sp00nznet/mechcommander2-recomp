# Architecture

How MechCommander 2 goes from the disc to `build/mc2.exe`, and why each step
is the way it is.

```
your disc / .iso / .zip
  └─ tools/gamefiles.py ──► game/          Mc2Rel.exe, MC2REL.PDB, MC2RES.DLL, BINKW32.DLL, *.FST, DATA\
       └─ tools/catalog.py ──► work/pdb_ref.json      every function the PDB names (pcrecomp pdb2_ref.py)
                           └─► work/functions.json    seeded disasm32 catalog
            └─ run_lift.py ──► src/recomp/gen/        lifted C, one file per 400 functions
                 └─ build.cmd ──► build/mc2.exe      32-bit host on pcrecomp runtime/native32
```

`game/`, `work/` and `src/recomp/gen/` are made on your machine from your copy
and are gitignored (REPO_RULES section 3).

## The binary

| | |
|---|---|
| File | `Mc2Rel.exe`, 4.38 MB, linked 2001-06-26 (`TimeDateStamp` 0x3B382558) |
| Compiler | Visual C++ 6, GameOS statically linked |
| Image | base 0x00400000, `.text` 2.5 MB, relocations present |
| Imports | MSVCRT, KERNEL32, USER32, GDI32, ADVAPI32, WINMM, MSACM32, ole32, VERSION, SHELL32, binkw32 |
| Run-time loads | GameOS `LoadLibrary`s ddraw/Direct3D 7, dsound, dinput, dplayx, ijl10 |
| Protection | SafeDisc 2 sections (`stxt774`, `stxt371`) are present, but the entry point is the plain VC6 CRT start and no code in `.text` calls into them: `.text` is readable as it is on the disc |

## Symbols: the disc ships the PDB

`MC2REL.PDB` on the disc is the release build's program database, and its
signature matches the exe's CodeView record (0x3B382558, age 1). It is the
old VC6 format (MSF 2.00, 16-bit page numbers, `_ST` symbol records), which
llvm-pdbutil does not read, so pcrecomp gained `tools/pe/pdb2_ref.py` for it.

It gives 9,776 named functions in `.text`, 5,525 of them with exact sizes
(the modules built with debug info; the rest are publics from libraries).
That is the recomp-ideas rule's best case: the binary names everything.

## The catalog

`tools/catalog.py` writes the PDB catalog, then runs pcrecomp's disasm32 with
every PDB address as a seed. disasm32 still finds what the PDB does not list:
compiler-generated thunks and code reached only through data. The unseeded
disasm32 run is scored against the PDB with `score_recovery.py`; see
CHANGELOG for the figures.

## The host

`src/runtime/host.c` is copied in shape from mechwarrior3-recomp: map the
guest image at 0x00400000, bind its imports through native32 (real Windows
DLLs, a few shims), and enter the CRT startup. The shims are only those the
guest needs to believe it is `Mc2Rel.exe` in the game folder
(`GetModuleHandleA`, `GetModuleFileNameA`, `GetCommandLineA`) plus
`ExitProcess` for the log. Everything game-specific beyond that is found by
running it.
