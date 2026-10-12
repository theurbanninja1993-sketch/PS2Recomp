#!/usr/bin/env python3
"""Detect and convert raw-sector PS2 disc images (.bin) to standard ISO-9660 (.iso).

CHD extractcd output is typically MODE1/2352 (16-byte header + 2048 data + 288 EDC/ECC).
Standard ISO tools expect 2048-byte sectors.
"""
import sys
import os

CANDIDATES = [
    # (sector_size, data_offset, description)
    (2352, 16, "MODE1/2352"),
    (2448, 16, "MODE1/2448 (raw+sub)"),
    (2048, 0, "MODE1/2048 (already ISO)"),
    (2336, 16, "MODE2/2336"),
]


def detect(path):
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        for sector_size, offset, desc in CANDIDATES:
            if size < 17 * sector_size:
                continue
            if size % sector_size != 0:
                continue
            f.seek(16 * sector_size + offset)
            magic = f.read(6)
            if magic[1:6] == b"CD001":
                return sector_size, offset, desc
    return None


def convert(src, dst, sector_size, offset):
    total = os.path.getsize(src)
    n_sectors = total // sector_size
    written = 0
    with open(src, "rb") as fin, open(dst, "wb") as fout:
        for i in range(n_sectors):
            fin.seek(i * sector_size + offset)
            fout.write(fin.read(2048))
            written += 1
            if written % 200000 == 0:
                print(f"  {written}/{n_sectors} sectors...", flush=True)
    print(f"Wrote {written} sectors -> {dst}")


def main():
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <input.bin> [--convert <output.iso>]")
        sys.exit(1)
    src = sys.argv[1]
    result = detect(src)
    if not result:
        print("Could not detect sector format (no CD001 magic found).")
        sys.exit(2)
    sector_size, offset, desc = result
    print(f"Detected: {desc} (sector_size={sector_size}, data_offset={offset})")
    if sector_size == 2048:
        print("Already a 2048-byte ISO image; no conversion needed.")
        if "--convert" in sys.argv:
            print("(copy/rename it instead)")
        return
    if "--convert" in sys.argv:
        dst = sys.argv[sys.argv.index("--convert") + 1]
        convert(src, dst, sector_size, offset)
    else:
        print("Pass --convert <output.iso> to extract 2048-byte sectors.")


if __name__ == "__main__":
    main()
