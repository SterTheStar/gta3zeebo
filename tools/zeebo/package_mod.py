#!/usr/bin/env python3
"""package_mod.py - Packages gta3zeebo.mod, gta3zeebo.mif, and GTA III PC assets
for real Zeebo hardware (SD:/mif/<id>.mif + SD:/mod/<id>/) and/or zeebx-emu.
"""
import argparse
import os
import shutil
import struct


def _utf16le_bom(text: str) -> bytes:
    return b"\xff\xfe" + text.encode("utf-16-le")


def build_brew_mif(clsid: int, mod_name: str, app_name: str) -> bytes:
    """Build a valid BREW 4.0.2 / Zeebo .mif matching real Zeebo MIF layout."""
    sec0 = _utf16le_bom(mod_name)
    sec1 = _utf16le_bom(app_name)
    sec2 = (
        b"\x02\x10\x00\x00"
        + b"\x00" * 20
        + b"\xff\xff\x00\x00\x01\x00\x00\x00"
        + b"\x00" * 8
    )
    sec3 = struct.pack("<IIIII", clsid, 0, 0x0028, 0, 0)
    tail = (
        b"\x01\x00\x00\x00\x01\x00\x00\x00"
        b"\x02\x01\x00\x00\x00\x71\x02\x00\x10\x00ls"
    )

    idx_cnt = 4
    sec_cnt = 4
    idx_off = 0x20
    idx_sz = idx_cnt * 8
    sec_off = idx_off + idx_sz
    first_sec = sec_off + (sec_cnt + 1) * 4

    off0 = first_sec
    off1 = off0 + len(sec0)
    off2 = off1 + len(sec1)
    off3 = off2 + len(sec2)
    off4 = off3 + len(sec3)
    sec_tot = off4 - first_sec

    header = struct.pack(
        "<HHHHIIIIII",
        0x0011, 1, 1, idx_cnt, idx_off, idx_sz, sec_off, sec_cnt, first_sec, sec_tot
    )
    index_table = (
        struct.pack("<II", 0x00140001, 0x00000000)
        + struct.pack("<II", 0x00280001, 0x00010000)
        + struct.pack("<II", 0x00005000, 0x00020000)
        + struct.pack("<II", 0x00025000, 0x00030000)
    )
    section_table = struct.pack("<5I", off0, off1, off2, off3, off4)
    return header + index_table + section_table + sec0 + sec1 + sec2 + sec3 + tail


def _copy_assets(assets_dir: str, dst_root: str) -> int:
    copied = 0
    if assets_dir and os.path.isdir(assets_dir):
        for sub in ("data", "models", "txd", "anim", "TEXT", "audio", "movies", "neo"):
            src_sub = os.path.join(assets_dir, sub)
            if os.path.isdir(src_sub):
                dst_sub = os.path.join(dst_root, sub)
                shutil.copytree(src_sub, dst_sub, dirs_exist_ok=True)
                for _, _, files in os.walk(dst_sub):
                    copied += len(files)
        gcdb = os.path.join(assets_dir, "gamecontrollerdb.txt")
        if os.path.isfile(gcdb):
            shutil.copy2(gcdb, os.path.join(dst_root, "gamecontrollerdb.txt"))
            copied += 1
    return copied


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Package GTA3 Zeebo .mod + .mif + assets for real Zeebo SD or zeebx-emu"
    )
    parser.add_argument("--mod", required=True, help="Path to compiled gta3zeebo.mod")
    parser.add_argument("--assets", default="", help="Path to GTA III root folder (containing models/, data/, etc.)")
    parser.add_argument("--out-dir", default="dist/zeebo_sd", help="Output root directory")
    parser.add_argument("--app-id", default="990002", help="Zeebo numeric module/folder ID (e.g. 990002)")
    parser.add_argument("--app-name", default="GTA III", help="Display name in BREW AppMgr")
    parser.add_argument("--clsid", default="0x01099002", help="Applet CLSID hex")
    parser.add_argument("--layout", choices=("sd", "flat"), default="sd",
                        help="sd = mif/<id>.mif + mod/<id>/<id>.mod (real Zeebo); flat = single folder (zeebx-emu)")
    args = parser.parse_args()

    out_dir = os.path.abspath(args.out_dir)
    clsid = int(args.clsid, 16)
    mif_bytes = build_brew_mif(clsid, args.app_id, args.app_name)

    if args.layout == "sd":
        mif_dir = os.path.join(out_dir, "mif")
        mod_dir = os.path.join(out_dir, "mod", args.app_id)
        os.makedirs(mif_dir, exist_ok=True)
        os.makedirs(mod_dir, exist_ok=True)

        mif_dst = os.path.join(mif_dir, f"{args.app_id}.mif")
        mod_dst = os.path.join(mod_dir, f"{args.app_id}.mod")
        shutil.copy2(args.mod, mod_dst)
        with open(mif_dst, "wb") as f:
            f.write(mif_bytes)
        copied = _copy_assets(args.assets, mod_dir)
        print(f"Prepared Zeebo SD package at {out_dir} (mif={mif_dst}, mod={mod_dst}, assets={copied} files, clsid=0x{clsid:08X})")
    else:
        os.makedirs(out_dir, exist_ok=True)
        mod_dst = os.path.join(out_dir, f"{args.app_id}.mod")
        mif_dst = os.path.join(out_dir, f"{args.app_id}.mif")
        shutil.copy2(args.mod, mod_dst)
        with open(mif_dst, "wb") as f:
            f.write(mif_bytes)
        copied = _copy_assets(args.assets, out_dir)
        print(f"Prepared flat package at {out_dir} (mod={mod_dst}, mif={mif_dst}, assets={copied} files, clsid=0x{clsid:08X})")


if __name__ == "__main__":
    main()
