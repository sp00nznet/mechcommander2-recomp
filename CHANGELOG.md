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
