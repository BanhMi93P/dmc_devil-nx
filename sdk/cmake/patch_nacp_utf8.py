#!/usr/bin/env python3
"""Patch NACP title/author/version with exact UTF-8 bytes after nacptool on Windows.

nacptool.exe receives command-line arguments in the Windows ANSI codepage (e.g. CP1252),
corrupting non-ASCII UTF-8 characters like "Bánh Mì". CMake writes the metadata file in
UTF-8 at configure time, and this script patches the populated NACP entries in place.
"""
import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: patch_nacp_utf8.py <nacp> <meta_utf8.txt>\n")
        return 2
    nacp_path = pathlib.Path(sys.argv[1])
    meta_lines = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8").splitlines()
    if len(meta_lines) < 3:
        sys.stderr.write(f"invalid metadata file: {sys.argv[2]}\n")
        return 2
    title = meta_lines[0].encode("utf-8")[:0x1FF]
    author = meta_lines[1].encode("utf-8")[:0xFF]
    version = meta_lines[2].encode("utf-8")[:0x0F]

    buf = bytearray(nacp_path.read_bytes())
    if len(buf) != 0x4000:
        sys.stderr.write(f"unexpected NACP size {len(buf)} in {nacp_path}\n")
        return 2

    for i in range(16):
        off = i * 0x300
        if any(buf[off : off + 0x300]):
            buf[off : off + 0x200] = title.ljust(0x200, b"\x00")
            buf[off + 0x200 : off + 0x300] = author.ljust(0x100, b"\x00")

    buf[0x3060 : 0x3070] = version.ljust(0x10, b"\x00")
    nacp_path.write_bytes(bytes(buf))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
