#!/usr/bin/env python3
"""Create a blank, formatted ProDOS hard-disk image for POMIIGS.

The volume mounts in GS/OS (valid empty directory + bitmap) so the Installer can
target it. By default block 0 is zeroed so the slot-7 card boot chains to the
slot-5 install disk instead of trying to boot an empty disk — do the install,
then make it bootable.

NB: the System 6 "Easy Update" copies the System Folder but does NOT rewrite the
ProDOS boot block on an already-formatted volume, so the freshly-installed disk
stays non-bootable. Pass `--boot-from <prodos.2mg/.po/.hdv>` to copy a standard
ProDOS boot block (block 0) from any bootable ProDOS/GS-OS disk (e.g. one of the
System 6 install disks) so the finished volume boots on its own.

  python3 tools/make_prodos_hdd.py hdv/GSOS.hdv [volume_name] [size_mb]
  python3 tools/make_prodos_hdd.py --boot-from "disks35/.../Disk 2 of 7 System Disk.2mg" hdv/GSOS.hdv
"""
import sys, struct

def read_block0(path):
    """Return the 512-byte block 0 of a ProDOS image (.2mg header-aware)."""
    d = open(path, "rb").read()
    off = 0
    if d[:4] == b"2IMG":                                 # .2mg: data offset at $18
        off = d[0x18] | (d[0x19] << 8) | (d[0x1a] << 16) | (d[0x1b] << 24)
    b0 = d[off:off + BS]
    if not b0 or b0[0] != 0x01:
        sys.exit(f"--boot-from: {path} block 0 is not a ProDOS boot block (byte0 != $01)")
    return b0

BS = 512
def main():
    args = sys.argv[1:]
    boot0 = None
    if "--boot-from" in args:
        i = args.index("--boot-from")
        boot0 = read_block0(args[i + 1])
        del args[i:i + 2]
    out  = args[0] if len(args) > 0 else "hdv/GSOS.hdv"
    name = (args[1] if len(args) > 1 else "GSOS").upper()[:15]
    mb   = int(args[2]) if len(args) > 2 else 16
    total = (mb * 1024 * 1024) // BS                 # blocks
    img = bytearray(total * BS)

    # ── block 2: volume directory key block ─────────────────────────────
    b = bytearray(BS)
    struct.pack_into("<HH", b, 0, 0, 3)              # prev=0, next=block 3
    b[4] = 0xF0 | len(name)                          # storage type $F (vol dir) | name len
    b[5:5+len(name)] = name.encode("ascii")
    b[0x14] = 0x00                                   # reserved
    # creation date/time = 0; version/min_version = 0
    b[0x22] = 0xC3                                   # access: destroy/rename/write/read
    b[0x23] = 0x27                                   # entry_length = 39
    b[0x24] = 0x0D                                   # entries_per_block = 13
    struct.pack_into("<H", b, 0x25, 0)              # file_count = 0
    struct.pack_into("<H", b, 0x27, 6)              # bitmap starts at block 6
    struct.pack_into("<H", b, 0x29, total & 0xFFFF)# total_blocks
    img[2*BS:3*BS] = b

    # ── blocks 3-5: rest of the volume directory (empty, linked) ────────
    for i, blk in enumerate((3, 4, 5)):
        d = bytearray(BS)
        prev = blk - 1
        nxt  = blk + 1 if blk < 5 else 0
        struct.pack_into("<HH", d, 0, prev, nxt)
        img[blk*BS:(blk+1)*BS] = d

    # ── volume bitmap (starts block 6): 1 bit/block, 1 = free ───────────
    nbits  = total
    nbytes = (nbits + 7) // 8
    bmblocks = (nbytes + BS - 1) // BS
    used = 6 + bmblocks                              # blocks 0..used-1 are the system area
    bitmap = bytearray(b"\xff" * (bmblocks * BS))    # all free
    for blk in range(used):                          # mark the system blocks used (bit=0)
        bitmap[blk // 8] &= ~(0x80 >> (blk % 8))
    for blk in range(total, bmblocks * BS * 8):      # blocks past the volume: used
        bitmap[blk // 8] &= ~(0x80 >> (blk % 8))
    img[6*BS:6*BS + len(bitmap)] = bitmap

    # block 0: a copied ProDOS boot block (bootable) or left zero (chains to slot 5).
    if boot0:
        img[0:BS] = boot0
    with open(out, "wb") as f:
        f.write(img)
    print(f"wrote {out}: {mb} MB ({total} blocks), volume '/{name}', "
          f"{'bootable' if boot0 else 'non-bootable'}, {bmblocks}-block bitmap")

if __name__ == "__main__":
    main()
