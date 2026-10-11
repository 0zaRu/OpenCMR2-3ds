#!/usr/bin/env python3
"""Write every float-to-integer conversion of the /QIfist game TUs as FTOI().

The original compiles most of its translation units with /QIfist: a float to
integer conversion there is an x87 FISTP, which rounds to nearest (even) under
the game's control word. Other compilers truncate. FTOI(type, value)
(CMR2Decomp/FloatToInt.h) expands to the plain cast under MSVC6, so the matching
build is unchanged, and rounds like FISTP in the port's CMR2_QIFIST TUs.

This runs a Clang plugin (tools/raw_access/FloatToInt.cpp) over CMR2Decomp's
sources, parsed with MSVC6's headers, and rewrites the conversions in place:
`(int)x` -> `FTOI(int, x)`, an implicit `i = f` -> `i = FTOI(int, f)`. Running
it again finds nothing new. Conversions it cannot rewrite (inside macro bodies,
compound assignments) are listed for a manual edit.

    tools/ftoi_rewrite.py [--decomp ../CMR2Decomp] [--dry-run]

Needs clang-devel and the MSVC6 headers (CMR2_TOOLS/msvc600/VC98/Include).
"""
import argparse
import collections
import concurrent.futures
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PLUGIN_SOURCE = ROOT / "tools/raw_access/FloatToInt.cpp"
PLUGIN = ROOT / "build/tools/FloatToInt.so"
WORK = ROOT / "build/tools/ftoi"
HEADER = "FloatToInt.h"


def build_plugin():
    if PLUGIN.exists() and PLUGIN.stat().st_mtime >= PLUGIN_SOURCE.stat().st_mtime:
        return
    PLUGIN.parent.mkdir(parents=True, exist_ok=True)
    flags = [f for f in subprocess.run(["llvm-config", "--cxxflags"], capture_output=True, text=True,
                                       check=True).stdout.split() if not f.startswith("-std=")]
    subprocess.run(["clang++", "-shared", "-fPIC", "-O1", "-std=c++17", "-fno-rtti", *flags, str(PLUGIN_SOURCE),
                    "-o", str(PLUGIN)], check=True)


def qifist_units(decomp):
    """The /QIfist list of CMR2Decomp's build script."""
    text = (decomp / "scripts/build.py").read_text()
    m = re.search(r'QIFIST = set\((.*?)\.split\(\)\)', text, re.S)
    return sorted(set("".join(re.findall(r'"([^"]*)"', m.group(1))).split()))


def msvc_overlay(include):
    """MSVC6's headers under lower- and upper-case names (Linux is case-sensitive)."""
    overlay = WORK / "vc6include"
    if not overlay.exists():
        for path in include.rglob("*"):
            if not path.is_file():
                continue
            rel = path.relative_to(include)
            for name in {str(rel).lower(), str(rel.parent / rel.name.upper()).replace("./", "")}:
                target = overlay / name
                target.parent.mkdir(parents=True, exist_ok=True)
                if not target.exists():
                    target.symlink_to(path)
    return overlay


def friend_shim(source):
    """MSVC6 finds functions declared only as friends; Clang needs them declared."""
    types, decls = {}, set()
    for header in source.glob("*.h"):
        text = header.read_text(encoding="latin1")
        for m in re.finditer(r'^\s*(struct|class|union)\s+(\w+)\s*(?::[^{;]*)?\{', text, re.M):
            types[m.group(2)] = m.group(1)
        for m in re.finditer(r'^\s*friend\s+([^;{]*\([^;{]*\))\s*;', text, re.M):
            if "operator" not in m.group(1):
                decls.add(m.group(1))
    shim = WORK / "friends.h"
    shim.write_text("#include <windows.h>\n" + "".join(f"{k} {n};\n" for n, k in sorted(types.items()))
                    + "".join(d + ";\n" for d in sorted(decls)))
    return shim


def run(unit, source, decomp, overlay, shim):
    out = WORK / (Path(unit).stem + ".jsonl")
    if decomp is None:
        # OpenCMR2's game/, with the port's own headers.
        cmd = ["clang++", "-m64", "-fsyntax-only", "-std=gnu++17", "-fms-extensions", "-fsigned-char",
               f"-I{source}", f"-I{ROOT / 'src'}", f"-I{ROOT / 'src/port/compat'}",
               "-include", str(ROOT / "src/port/types.h"), "-include", str(ROOT / "src/port/msvcrt.h"),
               "-Wno-everything", f"-fplugin={PLUGIN}", f"-fplugin-arg-ftoi-out={out}",
               f"-fplugin-arg-ftoi-dir={source}/", str(source / unit)]
        result = subprocess.run(cmd, capture_output=True, text=True)
        return unit, out, [l for l in result.stderr.splitlines() if ": error:" in l]
    resource = subprocess.run(["clang", "-print-resource-dir"], capture_output=True, text=True).stdout.strip()
    cmd = ["clang++", "-target", "i386-pc-windows-msvc", "-fms-compatibility-version=12.0", "-fms-extensions",
           "-fms-compatibility", "-fsyntax-only", "-nostdinc", "-nostdinc++", "-isystem", f"{resource}/include",
           "-isystem", str(overlay), f"-I{source}", f"-I{decomp / 'third_party/dx7sdk-7001/include'}",
           "-include", str(shim), "-Wno-everything", f"-fplugin={PLUGIN}", f"-fplugin-arg-ftoi-out={out}",
           f"-fplugin-arg-ftoi-dir={source}/", str(source / unit)]
    result = subprocess.run(cmd, capture_output=True, text=True)
    errors = [l for l in result.stderr.splitlines() if ": error:" in l]
    return unit, out, errors


def apply(path, convs):
    """Rewrite one file: nested conversions open outermost first."""
    data = path.read_bytes()
    opens = collections.defaultdict(list)   # offset -> [(end, text, skip)]
    closes = collections.Counter()          # offset -> count
    for c in convs:
        start = c["prefix"] if c["explicit"] else c["begin"]
        skip = c["prefixEnd"] - c["prefix"] if c["explicit"] else 0
        end = c["end"]
        if wrapped(data, c["begin"], end) and (not c["explicit"] or not data[c["prefixEnd"]:c["begin"]].strip()):
            # `(int)(x * k)` -> `FTOI(int, x * k)`: the value's own parentheses
            # become the macro's; an extra pair changes MSVC6's code.
            skip = c["begin"] + 1 - start
            end -= 1
            closes[end] += 0
        else:
            closes[end] += 1
        opens[start].append((end, f"FTOI({c['type']}, ".encode(), skip))
    out, i = bytearray(), 0
    points = sorted(set(opens) | set(closes))
    for p in points:
        out += data[i:p]
        i = p
        out += b")" * closes[p]
        for end, text, skip in sorted(opens[p], key=lambda o: -o[0]):
            out += text
            i = max(i, p + skip)
    out += data[i:]
    return bytes(out)


def wrapped(data, begin, end):
    """Whether data[begin:end] is one parenthesised group."""
    if data[begin:begin + 1] != b"(" or data[end - 1:end] != b")":
        return False
    depth = 0
    for i in range(begin, end):
        depth += {40: 1, 41: -1}.get(data[i], 0)
        if depth == 0 and i < end - 1:
            return False
    return True


def add_include(text, is_header):
    if f'#include "{HEADER}"' in text:
        return text
    lines = text.split("\n")
    for i, line in enumerate(lines):
        if is_header and re.match(r"#define\s+_\w+_H\b", line):
            lines.insert(i + 1, f'#include "{HEADER}"')
            return "\n".join(lines)
        if not is_header and line.startswith("#include"):
            lines.insert(i + 1, f'#include "{HEADER}"')
            return "\n".join(lines)
    return f'#include "{HEADER}"\n' + text


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--decomp", type=Path, default=ROOT.parent / "CMR2Decomp")
    ap.add_argument("--msvc", type=Path,
                    default=Path(os.environ.get("CMR2_TOOLS", ROOT.parent / "tools")) / "msvc600/VC98/Include")
    ap.add_argument("--port", action="store_true",
                    help="rewrite OpenCMR2's game/ instead (port-only lines after a sync)")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    decomp = args.decomp.resolve()
    units = qifist_units(decomp)
    build_plugin()
    WORK.mkdir(parents=True, exist_ok=True)
    if args.port:
        source, decomp, overlay, shim = ROOT / "game", None, None, None
    else:
        source = decomp / "CMR2Decomp"
        overlay, shim = msvc_overlay(args.msvc), friend_shim(source)
    convs, skips = {}, {}
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        for unit, out, errors in pool.map(lambda u: run(u, source, decomp, overlay, shim), units):
            if errors:
                print(f"{unit}: {len(errors)} parse errors (conversions in those statements are missed):",
                      errors[0][:160], file=sys.stderr)
            for line in out.read_text().splitlines():
                row = json.loads(line)
                if row["t"] == "conv":
                    convs[(row["file"], row["begin"], row["end"], row.get("prefix"))] = row
                else:
                    skips[(row["file"], row["line"], row["why"])] = row
    by_file = collections.defaultdict(list)
    for row in convs.values():
        by_file[row["file"]].append(row)
    for path, rows in sorted(by_file.items()):
        print(f"{Path(path).name:28} {len(rows):4} conversions")
        if not args.dry_run:
            p = Path(path)
            text = apply(p, rows).decode("latin1")
            p.write_bytes(add_include(text, p.suffix == ".h").encode("latin1"))
    print(f"{len(convs)} conversions in {len(by_file)} files; {len(skips)} to do by hand:")
    for row in sorted(skips.values(), key=lambda r: (r["file"], r["line"])):
        print(f"  {Path(row['file']).name}:{row['line']}: {row['why']}" + (f" ({row['macro']})" if row["macro"] else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
