#!/usr/bin/env python3
"""Generate src/port/disk_layouts.h: how game records stored in files map to
native structures.

Some game files hold records that the original loads in place and whose
32-bit offsets it turns into pointers (stage mesh files: scene nodes, meshes,
stage objects, sectors, collision records). On 64-bit targets the structures
holding those pointers are larger, so the port builds native copies. This
script compiles the game headers for i386 (the file layout, as MSVC6 laid it
out) and x86-64, and writes for each record type:

- runs: byte ranges without pointers, copied as they are (their internal
  layout is checked to be the same on both targets);
- pointers: fields that hold a 32-bit offset in the file and a native pointer
  in memory, with both offsets, so the loader can relocate them.

    tools/gen_disk_layouts.py [--check]

--check exits 1 if the committed header is out of date. Run it after
syncing game/ when one of the structures changes.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "src/port/disk_layouts.h"
HEADERS = ["SceneNode.h", "Mesh.h", "Sector.h", "MeshCollisionRecord.h", "Car.h", "CarParts.h"]
# Stage mesh file records, then the per-car records the state capture writes
# in the original layout (tests/stage_probe.cpp).
RECORDS = ["SceneNode", "Mesh", "StageObject", "Sector", "MeshCollisionRecord", "Car", "CarPartSet"]
TARGETS = {"disk": "i386-pc-linux-gnu", "native": "x86_64-pc-linux-gnu",
           "windows": "x86_64-pc-windows-msvc"}
LINE = re.compile(r"^\s*(\d+)(?::\d+-\d+)? \|( *)(\S.*)$")


def dump(target, optional=False):
    source = "".join(f'#include "{h}"\n' for h in HEADERS)
    source += "".join(f"int sizeof_{r} = sizeof({r});\n" for r in RECORDS)
    cmd = ["clang++", "-target", target, "-fsyntax-only", "-x", "c++", "-std=gnu++17", "-fms-extensions",
           "-Wno-everything", f"-I{ROOT / 'game'}", f"-I{ROOT / 'src'}", f"-I{ROOT / 'src/port/compat'}",
           "-include", str(ROOT / "src/port/types.h"), "-Xclang", "-fdump-record-layouts", "-"]
    result = subprocess.run(cmd, input=source, capture_output=True, text=True)
    if result.returncode != 0:
        if optional:
            print(f"note: no {target} headers here; its layout is not compared", file=sys.stderr)
            return None
        sys.exit(f"clang ({target}) failed:\n{result.stderr[:2000]}")
    return result.stdout


def leaves(text, record):
    """(path, offset, pointer element count or 0) of every leaf of record."""
    block = re.search(r"\*\*\* Dumping AST Record Layout\n\s*0 \| (?:struct|class) %s\n(.*?)\n\s*\| \[sizeof=(\d+)"
                      % re.escape(record), text, re.S)
    if not block:
        sys.exit(f"no layout for {record}")
    rows = []
    for line in block.group(1).split("\n"):
        m = LINE.match(line)
        if m:
            rows.append((int(m.group(1)), len(m.group(2)) // 2, m.group(3)))
    out, stack = [], []
    for i, (offset, depth, text) in enumerate(rows):
        name = text.split()[-1]
        stack = stack[:depth - 1] + [name]
        is_leaf = i + 1 == len(rows) or rows[i + 1][1] <= depth
        if not is_leaf:
            continue
        kind = text[:len(text) - len(name)].strip()
        count = 0
        if "*" in kind:
            m = re.search(r"\[(\d+)\]$", kind)
            count = int(m.group(1)) if m else 1
        out.append((".".join(stack), offset, count))
    return out, int(block.group(2))


def layout(disk, native, record):
    dleaves, dsize = leaves(disk, record)
    nleaves, nsize = leaves(native, record)
    if [l[0] for l in dleaves] != [l[0] for l in nleaves]:
        sys.exit(f"{record}: field lists differ between targets")
    fields = sorted(((d[1], n[1], d[2], d[0]) for d, n in zip(dleaves, nleaves)), key=lambda f: (f[0], -f[2]))
    runs, pointers, run = [], [], None
    bounds = sorted({f[0] for f in fields}) + [dsize]

    def close(end32, end64):
        nonlocal run
        if run is not None:
            size = end32 - run[0]
            if run[1] + size > end64:
                sys.exit(f"{record}: run at 0x{run[0]:x} does not fit natively")
            if size > 0:
                runs.append((run[0], run[1], size))
        run = None

    for d32, d64, count, name in fields:
        if count:
            close(d32, d64)
            for k in range(count):
                pointers.append((d32 + 4 * k, d64 + 8 * k, name + (f"[{k}]" if count > 1 else "")))
            continue
        if run is not None and d32 >= run[0] and d64 - d32 == run[1] - run[0]:
            continue
        if run is not None and d32 < run[2]:
            sys.exit(f"{record}: overlapping fields with different layouts at 0x{d32:x}")
        close(d32, d64)
        run = [d32, d64, d32]
    close(dsize, nsize)
    # A run ends at the next field it does not contain; make sure no pointer
    # bytes fall inside a run.
    for start, _, size in runs:
        for p in pointers:
            if start <= p[0] < start + size:
                sys.exit(f"{record}: pointer {p[2]} inside a copied run")
    return dsize, nsize, runs, pointers


def generate():
    disk, native = dump(TARGETS["disk"]), dump(TARGETS["native"])
    windows = dump(TARGETS["windows"], optional=True)
    out = ["// Generated by tools/gen_disk_layouts.py from the game headers. Do not edit.",
           "// File records of the stage mesh loader (see that script): byte runs copied",
           "// as they are and pointer fields stored as 32-bit file offsets.",
           "#ifndef OPENCMR2_PORT_DISK_LAYOUTS_H", "#define OPENCMR2_PORT_DISK_LAYOUTS_H", "",
           '#include "port/disk.h"'] + [f'#include "{h}"' for h in HEADERS] + [""]
    for record in RECORDS:
        dsize, nsize, runs, pointers = layout(disk, native, record)
        if windows is not None and layout(disk, windows, record) != (dsize, nsize, runs, pointers):
            sys.exit(f"{record}: Windows x64 lays it out differently; extend the generator")
        out.append(f"// {record}: 0x{dsize:x} bytes in files, 0x{nsize:x} on 64-bit targets.")
        out.append("#if UINTPTR_MAX > 0xffffffffu")
        out.append(f"static const DiskRun g_diskRuns_{record}[] = {{")
        out += [f"    {{0x{a:x}, 0x{b:x}, 0x{c:x}}}," for a, b, c in runs]
        out.append("};")
        out.append(f"static const DiskPointer g_diskPointers_{record}[] = {{")
        out += [f"    {{0x{a:x}, 0x{b:x}}}, // {n}" for a, b, n in pointers]
        out.append("};")
        out.append(f"static_assert(sizeof({record}) == 0x{nsize:x}, \"run tools/gen_disk_layouts.py\");")
        out.append("#else")
        out.append(f"static const DiskRun g_diskRuns_{record}[] = {{{{0, 0, 0x{dsize:x}}}}};")
        out.append(f"static const DiskPointer g_diskPointers_{record}[] = {{")
        out += [f"    {{0x{a:x}, 0x{a:x}}}, // {n}" for a, b, n in pointers]
        out.append("};")
        out.append(f"static_assert(sizeof({record}) == 0x{dsize:x}, \"run tools/gen_disk_layouts.py\");")
        out.append("#endif")
        out.append(f"static const DiskLayout g_diskLayout_{record} = {{0x{dsize:x}, sizeof({record}), "
                   f"g_diskRuns_{record}, sizeof(g_diskRuns_{record}) / sizeof(DiskRun), "
                   f"g_diskPointers_{record}, sizeof(g_diskPointers_{record}) / sizeof(DiskPointer)}};")
        for a, b, n in pointers:
            if "[" not in n:
                out.append(f"#define DISK_{record}_{n.split('.')[-1]} 0x{a:x}")
        out.append("")
    out.append("#endif")
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    text = generate()
    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text() != text:
            print(f"{OUTPUT.relative_to(ROOT)} is out of date; run tools/gen_disk_layouts.py")
            return 1
        return 0
    OUTPUT.write_text(text)
    print(f"wrote {OUTPUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
