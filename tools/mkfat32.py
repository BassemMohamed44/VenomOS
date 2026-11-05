#!/usr/bin/env python3
"""Build a small FAT32 volume image for VenomOS (host-side tool).

usage: mkfat32.py OUT.img SIZE_MIB [--fragment] [DEST=SRC ...]

DEST is a path inside the volume ("HELLO.TXT" or "DOCS/NOTES.TXT"); names must
be valid 8.3 names. SRC is a host file. Directories are created as needed.
The volume uses 512-byte sectors and 1 sector per cluster, so any size of at
least 34 MiB has >= 65525 clusters and is a genuine FAT32 volume.
--fragment allocates clusters in an interleaved order so chain-following code
is exercised with non-contiguous files.
"""
import struct
import sys

SECTOR = 512
RESERVED = 32
NUM_FATS = 2
SPC = 1


def short_name(part):
    name, _, ext = part.upper().partition('.')
    if not (1 <= len(name) <= 8 and len(ext) <= 3) or ' ' in part:
        raise SystemExit("not a valid 8.3 name: %r" % part)
    return (name.ljust(8) + ext.ljust(3)).encode('ascii')


def main(argv):
    args = [a for a in argv[1:] if a != '--fragment']
    fragment = '--fragment' in argv
    if len(args) < 2:
        raise SystemExit(__doc__)
    out, size_mib = args[0], int(args[1])
    total = size_mib * 1024 * 1024 // SECTOR
    clusters_guess = total
    fat_sectors = ((clusters_guess + 2) * 4 + SECTOR - 1) // SECTOR
    data_start = RESERVED + NUM_FATS * fat_sectors
    clusters = (total - data_start) // SPC
    if clusters < 65525:
        raise SystemExit("volume too small for FAT32 (need >= 34 MiB)")

    fat = [0] * (clusters + 2)
    fat[0] = 0x0FFFFFF8
    fat[1] = 0x0FFFFFFF
    data = {}                      # cluster -> 512 bytes
    free = list(range(2, clusters + 2))
    if fragment:
        # take clusters in an interleaved order: 2,4,6,...,3,5,7,...
        free = free[0::2] + free[1::2]
    cursor = [0]

    def alloc(n):
        got = free[cursor[0]:cursor[0] + n]
        cursor[0] += n
        if len(got) < n:
            raise SystemExit("volume full")
        for a, b in zip(got, got[1:]):
            fat[a] = b
        fat[got[-1]] = 0x0FFFFFFF
        return got

    def store(content):
        n = max(1, (len(content) + SECTOR - 1) // SECTOR)
        chain = alloc(n)
        padded = content.ljust(n * SECTOR, b'\0')
        for i, c in enumerate(chain):
            data[c] = padded[i * SECTOR:(i + 1) * SECTOR]
        return chain[0]

    # directory tree: dict name -> ('file', bytes) | ('dir', dict)
    root = {}
    for spec in args[2:]:
        dest, _, src = spec.partition('=')
        parts = dest.split('/')
        node = root
        for p in parts[:-1]:
            node = node.setdefault(p.upper(), ('dir', {}))[1]
        node[parts[-1].upper()] = ('file', open(src, 'rb').read())

    def dir_entry(name83, attr, first, size):
        hi, lo = (first >> 16) & 0xFFFF, first & 0xFFFF
        return struct.pack('<11sBBBHHHHHHHI', name83, attr, 0, 0, 0, 0x21, 0x21,
                           hi, 0, 0x21, lo, size)

    def build_dir(tree, parent_cluster):
        entries = []
        subdirs = []
        for name, (kind, val) in tree.items():
            if kind == 'file':
                first = store(val) if val else 0
                entries.append(dir_entry(short_name(name), 0x20, first, len(val)))
            else:
                subdirs.append((name, val))
        return entries, subdirs

    def write_dir_content(tree, parent_cluster):
        # allocate a first cluster up front so '.' can reference it
        n_est = max(1, (len(tree) + 2) * 32 // SECTOR + 1)
        chain = alloc(n_est)
        self_cluster = chain[0]
        entries, subs = build_dir(tree, parent_cluster)
        sub_entries = []
        for name, sub in subs:
            c = write_dir_content(sub, self_cluster)
            sub_entries.append(dir_entry(short_name(name), 0x10, c, 0))
        ents = [dir_entry(b'.          ', 0x10, self_cluster, 0),
                dir_entry(b'..         ', 0x10, parent_cluster, 0)] + entries + sub_entries
        raw = b''.join(ents)
        if len(raw) > len(chain) * SECTOR:
            raise SystemExit("directory too large for the tool")
        raw = raw.ljust(len(chain) * SECTOR, b'\0')
        for i, c in enumerate(chain):
            data[c] = raw[i * SECTOR:(i + 1) * SECTOR]
        return self_cluster

    # root directory at cluster 2 (the first allocation)
    root_chain = alloc(max(1, (len(root) + 2) * 32 // SECTOR + 1))
    root_cluster = root_chain[0]
    entries, subs = build_dir(root, 0)
    sub_entries = []
    for name, sub in subs:
        c = write_dir_content(sub, 0)
        sub_entries.append(dir_entry(short_name(name), 0x10, c, 0))
    raw = b''.join(entries + sub_entries)
    if len(raw) > len(root_chain) * SECTOR:
        raise SystemExit("root directory too large for the tool")
    raw = raw.ljust(len(root_chain) * SECTOR, b'\0')
    for i, c in enumerate(root_chain):
        data[c] = raw[i * SECTOR:(i + 1) * SECTOR]

    # boot sector (BPB)
    bs = bytearray(SECTOR)
    bs[0:3] = b'\xEB\x58\x90'
    bs[3:11] = b'VENOMFAT'
    struct.pack_into('<HBHBHHBHHHII', bs, 11, SECTOR, SPC, RESERVED, NUM_FATS,
                     0, 0, 0xF8, 0, 32, 64, 0, total)
    struct.pack_into('<IHHIHH12s', bs, 36, fat_sectors, 0, 0, root_cluster, 1, 6, b'')
    struct.pack_into('<BBBI11s8s', bs, 64, 0x80, 0, 0x29, 0x56454E4F, b'VENOMVOL   ', b'FAT32   ')
    bs[510:512] = b'\x55\xAA'
    fsinfo = bytearray(SECTOR)
    struct.pack_into('<I', fsinfo, 0, 0x41615252)
    struct.pack_into('<III', fsinfo, 484, 0x61417272, 0xFFFFFFFF, 0xFFFFFFFF)
    fsinfo[510:512] = b'\x55\xAA'

    img = bytearray(total * SECTOR)
    img[0:SECTOR] = bs
    img[SECTOR:2 * SECTOR] = fsinfo
    img[6 * SECTOR:7 * SECTOR] = bs          # backup boot sector
    fat_bytes = struct.pack('<%dI' % len(fat), *fat).ljust(fat_sectors * SECTOR, b'\0')
    for f in range(NUM_FATS):
        off = (RESERVED + f * fat_sectors) * SECTOR
        img[off:off + len(fat_bytes)] = fat_bytes
    for c, blk in data.items():
        off = (data_start + (c - 2) * SPC) * SECTOR
        img[off:off + SECTOR] = blk
    open(out, 'wb').write(img)
    print("wrote %s: %d sectors, %d clusters, FAT %d sectors" % (out, total, clusters, fat_sectors))


main(sys.argv)
