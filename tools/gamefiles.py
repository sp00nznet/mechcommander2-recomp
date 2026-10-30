#!/usr/bin/env python3
r"""Put your copy of MechCommander 2 in game/, from wherever it is.

    py -3 tools/gamefiles.py E:\                    # the CD in a drive
    py -3 tools/gamefiles.py MC2.iso                # a 2048-byte ISO
    py -3 tools/gamefiles.py MC2.mdf                # a raw image (2352 or 2448-byte sectors)
    py -3 tools/gamefiles.py MC2.zip                # a .zip with the ISO inside
    py -3 tools/gamefiles.py "C:\Games\MechCommander 2"   # an installed copy
    py -3 tools/gamefiles.py                        # look in every drive

The disc runs as it is: Mc2Rel.exe, its DLLs and the .fst archives sit loose
in the root, so a disc source is copied, not installed. The release PDB
(MC2REL.PDB) on the disc is the function catalog's ground truth.
Disc images are read with pcrecomp's ISO9660 walker, so nothing is mounted.

It supplies your own copy to the local build; nothing is downloaded, and none
of it is ever committed.
"""
import os
import shutil
import string
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'game')
TOOLS = os.path.join(os.environ.get('PCRECOMP', os.path.join(ROOT, '..', 'tools')), 'tools', 'assets')
NEEDED = ('Mc2Rel.exe', 'MC2REL.PDB', 'MC2RES.DLL', 'BINKW32.DLL', 'MISSION.FST', 'ART.FST')


def find(d, name):
    """Case-insensitive: a disc image keeps upper case, an install may not."""
    return next((os.path.join(d, f) for f in os.listdir(d) if f.lower() == name.lower()), None) \
        if os.path.isdir(d) else None


def complete(d):
    return all(find(d, n) for n in NEEDED)


def from_image(path):
    sys.path.insert(0, TOOLS)
    import iso_peek

    size = os.path.getsize(path)
    # 2048 is a plain ISO. A raw image keeps each sector's 2048 bytes 16 bytes
    # into a 2352-byte sector (12 sync + 4 header); an MDF adds 96 bytes of
    # subchannel after that, 2448 in all.
    raw = next((n for n in (2448, 2352) if size % n == 0 and size % 2048), None)

    class RawReader(iso_peek.Reader):
        def read(self, off, n):
            out = bytearray()
            while n > 0:
                sec, within = divmod(off, 2048)
                take = min(n, 2048 - within)
                self.fh.seek(sec * raw + 16 + within)
                out += self.fh.read(take)
                off += take
                n -= take
            return bytes(out)

    reader = (RawReader if raw else iso_peek.Reader)(path)
    files = iso_peek.walk(reader)
    if not any(p.lower() == 'mc2rel.exe' for p, _, _ in files):
        return False
    print('reading %d files (%d MB) from %s' % (len(files), sum(n for _, _, n in files) >> 20, path))
    for p, off, n in files:
        dst = os.path.join(OUT, *p.split('/'))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as f:
            for o in range(0, n, 1 << 24):              # 16 MB at a time
                f.write(reader.read(off + o, min(1 << 24, n - o)))
    return True


def from_zip(path):
    with zipfile.ZipFile(path) as z:
        iso = next((m for m in z.namelist() if m.lower().endswith(('.iso', '.mdf', '.bin'))), None)
        if not iso:
            return False
        print('taking %s out of %s' % (iso, path))
        stage = os.path.join(ROOT, 'work', 'disc')
        os.makedirs(stage, exist_ok=True)
        img = z.extract(iso, stage)
    try:
        return from_image(img)
    finally:
        os.remove(img)


def main():
    src = sys.argv[1].strip('"') if len(sys.argv) > 1 else ''
    os.makedirs(os.path.join(ROOT, 'work'), exist_ok=True)
    if not src:
        src = next(('%s:\\' % c for c in string.ascii_uppercase if find('%s:\\' % c, 'Mc2Rel.exe')), '')
        if not src:
            sys.exit('no drive holds the MechCommander 2 CD; pass a drive, folder or disc image')
    if os.path.isdir(src):
        if not find(src, 'Mc2Rel.exe'):
            sys.exit('%s holds neither the MechCommander 2 disc nor an install (no Mc2Rel.exe)' % src)
        print('copying %s' % src)
        shutil.copytree(src, OUT, dirs_exist_ok=True)
    elif src.lower().endswith('.zip'):
        if not from_zip(src):
            sys.exit('%s holds no MechCommander 2 disc image' % src)
    elif os.path.isfile(src):
        if not from_image(src):
            sys.exit('%s is not a disc image holding MechCommander 2 (no Mc2Rel.exe on it)' % src)
    else:
        sys.exit('%s: no such drive, folder or file' % src)
    if not complete(OUT):
        sys.exit('copied, but %s is still missing a file the game needs (%s)' % (OUT, ', '.join(NEEDED)))
    print('done: %s' % OUT)


if __name__ == '__main__':
    main()
