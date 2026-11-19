#!/usr/bin/env python3
"""package_mod.py - Prepares an unpacked Zeebo module directory containing
gta3zeebo.mod, gta3zeebo.mif, and GTA III PC assets for zeebx-emu / Zeebo.
"""
import argparse
import os
import shutil
import struct


def build_minimal_mif(clsid: int, app_name: str) -> bytes:
    """Build a valid BREW .mif matching zeebx-core's MifFile::parse layout."""
    _ = app_name
    header = struct.pack(
        "<HHHHIIIIII",
        0x0011,  # magic
        1,       # version
        1,       # reserved/always 1
        1,       # index entries
        0x20,    # index offset
        8,       # index size (1 * 8)
        0x28,    # section table offset
        1,       # number of sections
        0x30,    # offset of first section
        20,      # total size of sections
    )
    index_entry = struct.pack("<II", 0, 0)
    section_table = struct.pack("<II", 0x30, 0x44)
    applet_record = struct.pack("<IIIII", clsid, 0, 1, 0, 0)
    return header + index_entry + section_table + applet_record


def main() -> None:
    parser = argparse.ArgumentParser(description="Stage GTA3 Zeebo .mod + assets into a folder for zeebx-emu")
    parser.add_argument("--mod", required=True, help="Path to compiled gta3zeebo.mod")
    parser.add_argument("--assets", default="", help="Path to GTA III root folder (containing models/, data/, etc.)")
    parser.add_argument("--out-dir", default="dist/zeebo/gta3zeebo", help="Output module directory")
    parser.add_argument("--clsid", default="0x01099002", help="Applet CLSID hex")
    args = parser.parse_args()

    out_dir = os.path.abspath(args.out_dir)
    os.makedirs(out_dir, exist_ok=True)

    clsid = int(args.clsid, 16)
    mod_dst = os.path.join(out_dir, "gta3zeebo.mod")
    mif_dst = os.path.join(out_dir, "gta3zeebo.mif")

    shutil.copy2(args.mod, mod_dst)
    with open(mif_dst, "wb") as f:
        f.write(build_minimal_mif(clsid, "GTA III"))

    copied = 0
    if args.assets and os.path.isdir(args.assets):
        for sub in ("data", "models", "txd", "anim", "TEXT", "audio", "movies"):
            src_sub = os.path.join(args.assets, sub)
            if os.path.isdir(src_sub):
                dst_sub = os.path.join(out_dir, sub)
                shutil.copytree(src_sub, dst_sub, dirs_exist_ok=True)
                for _, _, files in os.walk(dst_sub):
                    copied += len(files)

    print(f"Prepared {out_dir} (mod={mod_dst}, mif={mif_dst}, assets={copied} files, clsid=0x{clsid:08X})")


if __name__ == "__main__":
    main()
