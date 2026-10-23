# SPDX-License-Identifier: CDDL-1.0
"""Retag the ZFS partition of a raw disk image from V_USR to Basic Data.

`zpool create` marks the pool's partition as a Solaris /usr partition
(6a898cc3-1dd2-11b2-99a6-080020736631). Windows still builds a volume for it,
but the Mount Manager only registers volumes of types it knows, so the
partition never gets a drive letter and the boot volume is never found
(the boot thread waits forever in MountMgrBootDriveLetterAssignment).

On a live system, Set-Partition -GptType '{ebd0a0a2-...}' does this with the
pool exported. This does the same to an image file: it patches the primary and
the backup GPT entry arrays and recomputes both CRCs. It works on a copy:

    retag-gpt.py <in> <out> [disk_bytes]

For a fixed VHD, which is the raw disk followed by a 512 byte footer, pass the
size of the raw disk as disk_bytes.
"""
import shutil
import struct
import sys
import uuid
import zlib

SECTOR = 512
V_USR = uuid.UUID('6a898cc3-1dd2-11b2-99a6-080020736631').bytes_le
BASIC = uuid.UUID('ebd0a0a2-b9e5-4433-87c0-68b6b72699c7').bytes_le


def read(f, off, n):
    f.seek(off)
    return f.read(n)


def parse(f, lba):
    h = bytearray(read(f, lba * SECTOR, SECTOR))
    assert h[0:8] == b'EFI PART', 'no GPT header at LBA %d' % lba
    size = struct.unpack_from('<I', h, 12)[0]
    crc = struct.unpack_from('<I', h, 16)[0]
    my, alt = struct.unpack_from('<QQ', h, 24)
    ent_lba = struct.unpack_from('<Q', h, 72)[0]
    n, esz = struct.unpack_from('<II', h, 80)
    ecrc = struct.unpack_from('<I', h, 88)[0]
    hh = bytearray(h[:size])
    struct.pack_into('<I', hh, 16, 0)
    assert zlib.crc32(bytes(hh)) == crc, 'header CRC mismatch at LBA %d' % lba
    ents = read(f, ent_lba * SECTOR, n * esz)
    assert zlib.crc32(ents) == ecrc, 'entries CRC mismatch at LBA %d' % lba
    return h, size, my, alt, ent_lba, n, esz, bytearray(ents)


def patch(f, lba):
    h, size, my, alt, ent_lba, n, esz, ents = parse(f, lba)
    changed = 0
    for i in range(n):
        if ents[i * esz:i * esz + 16] == V_USR:
            ents[i * esz:i * esz + 16] = BASIC
            changed += 1
    struct.pack_into('<I', h, 88, zlib.crc32(bytes(ents)))
    struct.pack_into('<I', h, 16, 0)
    struct.pack_into('<I', h, 16, zlib.crc32(bytes(h[:size])))
    f.seek(ent_lba * SECTOR)
    f.write(ents)
    f.seek(lba * SECTOR)
    f.write(h)
    return changed, alt


def verify(f, lba):
    h, size, my, alt, ent_lba, n, esz, ents = parse(f, lba)
    types = [uuid.UUID(bytes_le=bytes(ents[i * esz:i * esz + 16]))
             for i in range(n) if ents[i * esz:i * esz + 16] != b'\0' * 16]
    return my, alt, ent_lba, [str(t) for t in types]


def main():
    src, dst = sys.argv[1], sys.argv[2]
    raw = int(sys.argv[3]) if len(sys.argv) > 3 else None
    shutil.copyfile(src, dst)
    with open(dst, 'r+b') as f:
        print('before primary:', verify(f, 1))
        c1, alt = patch(f, 1)
        last = (raw // SECTOR - 1) if raw else alt
        assert alt == last, 'backup header at %d, expected %d' % (alt, last)
        print('before backup :', verify(f, alt))
        c2, _ = patch(f, alt)
        print('patched entries: primary %d, backup %d' % (c1, c2))
        print('after primary :', verify(f, 1))
        print('after backup  :', verify(f, alt))
        assert c1 == c2 == 1, 'expected exactly one V_USR partition'


main()
