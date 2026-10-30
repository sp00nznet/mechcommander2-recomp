#!/usr/bin/env python3
"""Function catalog for Mc2Rel.exe: the disc's own PDB, then pcrecomp disasm32.

    py -3 tools/catalog.py            # -> work/functions.json

The disc ships the release PDB (MC2REL.PDB, VC6 format 2.00), which matches
Mc2Rel.exe exactly. pcrecomp's pdb2_ref.py reads it into work/pdb_ref.json:
every procedure and public in .text, by name. Those addresses seed disasm32,
which adds what the PDB does not list (thunks, code reached only from data).
Everything here is derived from the game, so it lives in work/ and is never
committed.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(os.environ.get('PCRECOMP', os.path.join(ROOT, '..', 'tools')), 'tools')
GAME, WORK = os.path.join(ROOT, 'game'), os.path.join(ROOT, 'work')
EXE = os.path.join(GAME, 'Mc2Rel.exe')


def run(*args):
    subprocess.run([sys.executable] + list(args), check=True)


os.makedirs(WORK, exist_ok=True)     # gitignored, so absent in a fresh copy
run(os.path.join(TOOLS, 'pe', 'pdb2_ref.py'), EXE, os.path.join(GAME, 'MC2REL.PDB'),
    '-o', os.path.join(WORK, 'pdb_ref.json'), '--seeds', os.path.join(WORK, 'pdb_seeds.json'))
run(os.path.join(TOOLS, 'disasm', 'disasm32.py'), EXE,
    '--seed-functions', os.path.join(WORK, 'pdb_seeds.json'), '-o', os.path.join(WORK, 'functions.json'))
