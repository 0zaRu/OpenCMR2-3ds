#!/usr/bin/env python3
"""Classify every raw memory access in game/ as harmless or broken on 64-bit.

The decompiled code still reads many fields through byte offsets, pointer
reinterpretations and integer addresses written against the original 32-bit
layout. Those compile silently on 64-bit targets. A Clang plugin
(tools/raw_access/RawAccess.cpp) records each one as "root object + byte
offset (+ variable index terms) viewed as type V" on every target; this script
resolves that against each target's real record layouts and compares:

- the fields an access covers on i386 (what the original meant) against the
  fields it covers on x86-64 (what the port touches);
- the array each variable stride steps through on both targets;
- for untyped pointers (BYTE *, void *, int addresses), what they point at,
  following assignments, call arguments and return values across the game.

Verdicts:
  bug        the access touches different fields, steps through a different
             array, or goes past its object on 64-bit
  safe       layout comparison passed, or individually reviewed with a reason
  unresolved the root could not be traced to a typed object or a plain
             buffer; needs a look (listed with the reason)

Needs clang-devel (the plugin is built against the installed Clang).

    tools/audit_raw_access.py [--json out.json] [--markdown out.md] [--jobs N]
                              [--targets i386,x86_64[,windows]] [--verbose]
"""
import argparse
import collections
import functools
import concurrent.futures
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME = ROOT / "game"
PLUGIN_SOURCE = ROOT / "tools/raw_access/RawAccess.cpp"
CACHE = ROOT / "build/tools/raw_access"
PLUGIN = ROOT / "build/tools/RawAccess.so"
REVIEWS = ROOT / "tools/raw_access/reviewed.json"
TARGETS = {"i386": "i386-pc-linux-gnu", "x86_64": "x86_64-pc-linux-gnu", "windows": "x86_64-pc-windows-msvc"}
FLAGS = ["-fsyntax-only", "-std=gnu++17", "-fwrapv", "-fno-strict-aliasing", "-fms-extensions", "-fsigned-char",
         f"-I{GAME}", f"-I{ROOT / 'src'}", f"-I{ROOT / 'src/port/compat'}", "-include", str(ROOT / "src/port/types.h"),
         "-include", str(ROOT / "src/port/msvcrt.h"), "-Wno-everything"]
ANNOTATION = re.compile(r"^// (?:FUNCTION|LIBRARY|TODO|STUB): CMR2 (0x[0-9a-fA-F]+)")
# Values holding file contents as loaded: records there keep the original
# 32-bit layout, so 32-bit offsets into them are right on every target.
DISK_SOURCES = {"ret:FindFile", "ret:FindFileInArchive", "ret:Profile_ReadLocalFile", "field:GenericFile::buffer",
                "ret:Disk_Data"}
# Pointers the platform code (src/, not analysed) stores through an argument
# of the game's calls: what they point at. "wire" data has the original's
# fixed 32-bit layout, like files; "bytes" are pixels or samples, which hold
# no records at all. Both are checked like file data.
OUT_SOURCES = {
    "param:Network_PollReceivedMessageBuffer#1": "wire",  # player packets, or a system message
    "param:Audio_LockBuffer#3": "bytes",
    "param:Audio_LockBuffer#5": "bytes",
}
# Platform fields that point at pixels.
DATA_FIELDS = {"field:GfxLockedRect::pixels": "bytes"}
# Native records the platform hands over instead of wire data: the system
# messages (sender id 0) of the network layer, built in native layout.
NATIVE_VIEWS = {"param:Network_PollReceivedMessageBuffer#1":
                {"NetMsgCreatePlayer", "NetMsgDestroyPlayer", "NetMsgSetPlayerData", "NetMsgSetSessionDesc"}}
# Records that head an allocation whose payload follows them directly, all
# pointer-free: past sizeof(header) lies the payload, at the same distance on
# every target, not another header.
HEADER_RECORDS = {
    "ReplayStream",  # lanes (0x114c or 0x5c bytes), frame words, samples, counts (Replay_SetupPointers)
}
MAX_DEPTH = 16
MAX_ORIGINS = 24


# ---- running the plugin ------------------------------------------------------

def build_plugin():
    if PLUGIN.exists() and PLUGIN.stat().st_mtime >= PLUGIN_SOURCE.stat().st_mtime:
        return
    PLUGIN.parent.mkdir(parents=True, exist_ok=True)
    cxxflags = subprocess.run(["llvm-config", "--cxxflags"], capture_output=True, text=True, check=True).stdout.split()
    cxxflags = [f for f in cxxflags if not f.startswith("-std=")]
    print("building the raw-access plugin", file=sys.stderr)
    subprocess.run(["clang++", "-shared", "-fPIC", "-O1", "-std=c++17", "-fno-rtti", *cxxflags, str(PLUGIN_SOURCE),
                    "-o", str(PLUGIN)], check=True)


def run_one(target, source):
    out = CACHE / target / (source.stem + ".jsonl")
    out.parent.mkdir(parents=True, exist_ok=True)
    cmd = ["clang++", "-target", TARGETS[target], *FLAGS, f"-fplugin={PLUGIN}",
           f"-fplugin-arg-rawaccess-out={out}", f"-fplugin-arg-rawaccess-dir={GAME}/", str(source)]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        return source, target, result.stderr[:2000]
    return source, target, None


def collect(targets, jobs):
    build_plugin()
    sources = sorted(GAME.glob("*.cpp"))
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for source, target, error in pool.map(lambda st: run_one(*st), [(t, s) for t in targets for s in sources]):
            if error:
                sys.exit(f"{source.name} ({target}) failed:\n{error}")
    data = {}
    for target in targets:
        records, sites, flows = {}, {}, collections.defaultdict(list)
        seen_flows = set()
        for source in sources:
            for line in (CACHE / target / (source.stem + ".jsonl")).read_text().splitlines():
                row = json.loads(line)
                kind = row.pop("t")
                if kind == "record":
                    records.setdefault(row["n"], row)
                elif kind == "site":
                    sites.setdefault(row["key"], row)
                elif kind == "flow":
                    if row["key"] + row["dst"] in seen_flows:
                        continue
                    seen_flows.add(row["key"] + row["dst"])
                    flows[row["dst"]].append(row)
                    if row["raw"]:
                        sites.setdefault(row["key"], row)
        data[target] = {"records": records, "sites": sites, "flows": flows}
    return data


# ---- layouts -------------------------------------------------------------------

class Layout:
    """Resolves byte ranges of a type into the fields they cover, on one target."""

    def __init__(self, records):
        self.records = records

    def size(self, t):
        if t is None:
            return 0
        if "r" in t and t["r"] in self.records:
            return self.records[t["r"]]["sz"]
        return t.get("sz", 0)

    def fields(self, t):
        rec = self.records.get(t["r"])
        return rec["fields"] if rec else []

    def is_union(self, t):
        rec = self.records.get(t["r"])
        return bool(rec and rec["union"])

    def cover(self, t, start, length, path, out):
        """Append (path, detail) for every field of t overlapping [start, start+length)."""
        size = self.size(t)
        end = start + length
        if start <= 0 and end >= size and size > 0:
            out.append((path, "all"))
            return
        if "r" in t:
            covered = []
            for f in self.fields(t):
                fsize = self.size(f["t"]) if not f["bw"] else 0
                fo = f["o"]
                if f["bw"]:
                    # Bit-fields: name the field; its storage unit is the type's size.
                    fsize = f["t"].get("sz", 4)
                    fo = fo - fo % fsize
                if fsize <= 0 or fo >= end or fo + fsize <= start:
                    continue
                covered.append((fo, fo + fsize))
                name = f"{path}.{f['n']}"
                if start <= fo and end >= fo + fsize:
                    out.append((name, "all"))
                else:
                    lo, hi = max(start, fo), min(end, fo + fsize)
                    self.cover(f["t"], lo - fo, hi - lo, name, out)
            if not covered:
                out.append((path, f"pad+{start}:{length}"))
            return
        if "a" in t:
            esize = self.size(t["a"])
            if esize <= 0:
                out.append((path, f"bytes+{start}:{length}"))
                return
            first, last = start // esize, (end - 1) // esize
            i = first
            while i <= last:
                lo, hi = i * esize, (i + 1) * esize
                if start <= lo and end >= hi:
                    j = i
                    while j + 1 <= last and end >= (j + 2) * esize:
                        j += 1
                    out.append((f"{path}[{i}]" if i == j else f"{path}[{i}..{j}]", "all"))
                    i = j + 1
                    continue
                self.cover(t["a"], max(start - lo, 0), min(end, hi) - max(start, lo), f"{path}[{i}]", out)
                i += 1
            return
        out.append((path, f"part+{start}:{length}/{size}" + ("/ptr" if "p" in t else "")))

    def point(self, t, offset, path):
        """The innermost field containing byte offset, as (path, offset within it)."""
        while True:
            if "r" in t:
                best = None
                for f in self.fields(t):
                    fsize = self.size(f["t"])
                    if f["o"] <= offset < f["o"] + max(fsize, 1):
                        best = f
                        break
                if best is None:
                    return path, offset
                path, offset, t = f"{path}.{best['n']}", offset - best["o"], best["t"]
                continue
            if "a" in t:
                esize = self.size(t["a"])
                if esize <= 0:
                    return path, offset
                path, offset, t = f"{path}[{offset // esize}]", offset % esize, t["a"]
                continue
            return path, offset

    def arrays_at(self, t, offset, path):
        """(path, element size) of every array containing byte offset, outermost first."""
        out = []
        while True:
            if "r" in t:
                hit = None
                for f in self.fields(t):
                    fsize = self.size(f["t"])
                    if f["o"] <= offset < f["o"] + max(fsize, 1):
                        hit = f
                        break
                if hit is None:
                    return out
                path, offset, t = f"{path}.{hit['n']}", offset - hit["o"], hit["t"]
                continue
            if "a" in t:
                esize = self.size(t["a"])
                out.append((path, esize))
                if esize <= 0:
                    return out
                path, offset, t = f"{path}[]", offset % esize, t["a"]
                continue
            return out

    def field_at(self, t, offset):
        """(record, field) of the innermost record field starting at offset, for pointer loads."""
        last = None
        while True:
            if "r" in t:
                hit = None
                for f in self.fields(t):
                    if f["o"] <= offset < f["o"] + max(self.size(f["t"]), 1):
                        hit = f
                        break
                if hit is None:
                    return last
                last = (t["r"], hit)
                offset, t = offset - hit["o"], hit["t"]
                continue
            if "a" in t:
                esize = self.size(t["a"])
                if esize <= 0:
                    return last
                offset, t = offset % esize, t["a"]
                continue
            return last

    def identical(self, a, other, b, seen=None):
        """Whether type a here and type b on the other target have the same layout."""
        if a is None or b is None:
            return a is b
        if self.size(a) != other.size(b):
            return False
        if "p" in a or "p" in b:
            return False
        if "a" in a:
            return "a" in b and a["n"] == b["n"] and self.identical(a["a"], other, b["a"], seen)
        if "r" in a:
            if "r" not in b or a["r"] != b["r"]:
                return False
            seen = seen or set()
            if a["r"] in seen:
                return True
            seen.add(a["r"])
            fa, fb = self.fields(a), other.fields(b)
            return len(fa) == len(fb) and all(
                x["n"] == y["n"] and x["o"] == y["o"] and self.identical(x["t"], other, y["t"], seen)
                for x, y in zip(fa, fb))
        return a.get("s") == b.get("s")


# ---- provenance ----------------------------------------------------------------

class Origin:
    __slots__ = ("kind", "type", "prov", "size", "offset", "terms", "via", "size_terms")

    def __init__(self, kind, type_, prov, size, offset, terms, via, size_terms=()):
        self.kind, self.type, self.prov, self.size = kind, type_, prov, size
        self.offset, self.terms, self.via = offset, terms, via
        self.size_terms = list(size_terms)

    def ident(self):
        return (self.kind, self.prov, self.offset, tuple(tuple(t) for t in self.terms))


@functools.lru_cache(maxsize=None)
def named_in_platform(name):
    named = re.compile(rf"\b{re.escape(name)}\b")
    return any(path.suffix in (".c", ".cpp", ".h") and named.search(path.read_text(errors="replace"))
               for path in (ROOT / "src").rglob("*"))


@functools.lru_cache(maxsize=None)
def address_taken(name):
    taken = re.compile(rf"&\s*(\w+\s*(\.|->)\s*)*{re.escape(name)}\b")
    return any(taken.search(path.read_text(errors="replace"))
               for path in list(GAME.glob("*.cpp")) + list(GAME.glob("*.h")))


def game_only(prov):
    """A global or field only game/ code can store to: its flows are complete."""
    if prov.startswith("var:") and prov.count(":") == 1:
        name = prov[len("var:"):]
        return not named_in_platform(name) and not address_taken(name)
    if prov.startswith("field:"):
        record, _, field = prov[len("field:"):].rpartition("::")
        return not named_in_platform(record.split("::")[-1]) and not address_taken(field)
    return False


@functools.lru_cache(maxsize=None)
def never_written(prov):
    """A global no code stores to, takes the address of or names outside
    game/ (the platform code is not analysed): it keeps its zero value."""
    if not prov.startswith("var:") or prov.count(":") != 1:
        return False
    name = re.escape(prov[len("var:"):])
    stores = re.compile(rf"(&\s*{name}\b|\b{name}\s*(\[[^\]]*\]\s*)*([-+*/|&^]|<<|>>)?=(?!=)|\b{name}\s*(\+\+|--)|(\+\+|--)\s*{name}\b)")
    for path in list(GAME.glob("*.cpp")) + list(GAME.glob("*.h")):
        if stores.search(path.read_text(errors="replace")):
            return False
    named = re.compile(rf"\b{name}\b")
    for path in (ROOT / "src").rglob("*"):
        if path.suffix in (".c", ".cpp", ".h") and named.search(path.read_text(errors="replace")):
            return False
    return True


def needs_provenance(root):
    if root["k"] == "untyped":
        return True
    if root["k"] == "pointee":
        t = root.get("type")
        return not t or "r" not in t
    return False


class Tracer:
    def __init__(self, target_data, layout, differing):
        self.differing = differing  # records whose layout is not the same on every target
        self.flows = target_data["flows"]
        self.sites = target_data["sites"]
        self.layout = layout
        self.blob_views = None
        self.cycles = 0

    def origins(self, root, offset, terms, pun_view=None):
        """Where a root ultimately points: typed objects, buffers, or dead ends.

        pun_view: the record a typed pointer was reinterpreted as; its
        declared type is then not trusted and its assignments are followed
        until they reach that record or run out."""
        out, seen = [], set()
        self.cycles = 0
        self._trace(root, offset, list(terms), [], out, seen, 0, pun_view)
        if not out and self.cycles:
            out.append(Origin("unresolved", None, root.get("prov", ""), None, offset, terms, ["(only cycles)"]))
        uniq = {}
        for o in out:
            uniq.setdefault(o.ident(), o)
        return list(uniq.values())[:MAX_ORIGINS]

    def _trace(self, root, offset, terms, via, out, seen, depth, pun_view=None):
        if len(out) >= MAX_ORIGINS:
            return
        kind, prov = root["k"], root.get("prov", "")
        if kind == "abs" and 0 <= offset < 0x400000:
            return  # an integer constant (a sentinel or a count), not an address
        if kind == "opaque" and root.get("why") in ("null", "FunctionToPointerDecay", "Dependent"):
            return
        if prov in DISK_SOURCES or prov in DATA_FIELDS:
            out.append(Origin("disk", None, prov, None, offset, terms, via))
            return
        if prov.startswith("ret:?ZALLOC") or prov.startswith("ret:?ALLOC"):
            out.append(Origin("alloc", None, prov, None, offset, terms, via))
            return
        if kind == "alloc" and prov in self.alloc_types():
            # A buffer the game also uses as a typed array.
            kind, root = "pointee", dict(root, k="pointee", type=self.alloc_types()[prov])
        rtype = (root.get("type") or {}).get("r")
        # A typed root is trusted when its record has the same layout
        # everywhere (whatever it really points at reads the same bytes).
        follow_typed = kind == "pointee" and rtype and self.flows.get(prov) and (
            (pun_view and rtype != pun_view)
            # a typed pointer that was also assigned an untyped buffer: that
            # buffer may not hold native records
            or rtype in self.differing and any(f["raw"] and (needs_provenance(f["root"]) or f["root"].get("prov") in DISK_SOURCES)
                   for f in self.flows[prov]))
        if not needs_provenance(root) and not follow_typed:
            out.append(Origin(kind, root.get("type"), prov, root.get("size"), offset, terms, via,
                              root.get("sizeTerms") or ()))
            return
        if prov in seen:
            # A loop back to a local or parameter already being traced
            # (p = next(p)) adds no origin: the other paths into the loop give
            # them all. Fields and globals may also be written outside game/
            # (the platform code), so a loop through them stays unresolved.
            if prov.startswith("param:") or (prov.startswith("var:") and prov.count(":") == 3) or game_only(prov):
                self.cycles += 1
                return
            out.append(Origin("unresolved", root.get("type"), prov, None, offset, terms, via + ["(cycle)"]))
            return
        if depth >= MAX_DEPTH:
            out.append(Origin("unresolved", root.get("type"), prov, None, offset, terms, via + ["(too deep)"]))
            return
        seen = seen | {prov}
        keys = [prov]
        if prov.startswith("site:"):
            # A pointer loaded through a raw access: the field that access reads.
            loaded = self._loaded_field(prov)
            if loaded:
                rec, field = loaded
                ptype = field["t"]
                pointee = ptype.get("p", "") if "p" in ptype else ""
                if pointee.startswith("r:"):
                    out.append(Origin("pointee", {"r": pointee[2:]}, f"field:{rec}::{field['n']}", None, offset, terms,
                                      via + [prov]))
                    return
                keys = [f"field:{rec}::{field['n']}"]
            else:
                # Loaded from untyped memory: when that memory is file data,
                # the pointer was relocated in place and points into it too.
                site = self.sites.get(prov[len("site:"):])
                if site:
                    inner = []
                    self._trace(site["root"], site["off"], site["terms"], via + [prov], inner, seen, depth + 1)
                    if inner and all(o.kind == "disk" for o in inner):
                        out.append(Origin("disk", None, inner[0].prov, None, offset, terms, via + [prov]))
                        return
        if prov.startswith("load:"):
            # A pointer loaded through another pointer: what was stored where
            # that pointer points. Through file data it was relocated in place
            # and points into the file; through pointer variables it is
            # whatever was assigned to them.
            inner = []
            self._trace({"k": "untyped", "prov": prov[len("load:"):]}, 0, [], via + [prov], inner, seen, depth + 1)
            if inner and all(o.kind == "disk" for o in inner):
                out.append(Origin("disk", None, inner[0].prov, None, offset, terms, via + [prov]))
                return
            if not inner or any(not (o.kind == "object" and self._holds_pointers(o) or o.kind == "alloc")
                                for o in inner):
                out.append(Origin("unresolved", root.get("type"), prov, None, offset, terms, via))
                return
            keys = sorted({o.prov for o in inner if o.kind == "object"})
            # Stores into those buffers through any pointer that reaches them.
            buffers = {o.prov for o in inner}
            keys += [k for k, held in self._stores_through().items() if held & buffers]
        incoming = [f for k in keys for f in self.flows.get(k, [])]
        if prov.startswith("var:") and not incoming:
            # Stored only through its address: by game code (stores through a
            # pointer that reaches it) or by the platform (an out argument).
            writers = [k for k, held in self._stores_through().items() if prov in held]
            incoming = [f for k in writers for f in self.flows.get(k, [])]
            outs = {f["dst"] for fs in self.flows.values() for f in fs
                    if f["dst"].startswith("param:") and f["root"]["k"] == "object"
                    and f["root"].get("prov") == prov and f["off"] == 0 and not f["terms"]}
            for dst in sorted(outs):
                if dst in OUT_SOURCES:
                    out.append(Origin("disk", None, dst, None, offset, terms, via + [prov]))
                elif not any(k == "load:" + dst for k in writers):
                    out.append(Origin("unresolved", root.get("type"), dst, None, offset, terms, via + [prov]))
            if outs and not incoming:
                return
        if not incoming and never_written(prov):
            return  # a global nothing stores to holds its zero initialiser: NULL
        if not incoming:
            out.append(Origin("unresolved", root.get("type"), prov, None, offset, terms, via))
            return
        for f in incoming:
            self._trace(f["root"], offset + f["off"], terms + f["terms"], via + [f["key"]], out, seen, depth + 1,
                        pun_view)

    def alloc_types(self):
        """Allocations stored straight into a typed pointer: their element type."""
        if self.blob_views is None:
            views = collections.defaultdict(set)
            carved = set()  # allocations split into several records

            def allocations(root, seen=frozenset()):
                prov = root.get("prov", "")
                if root["k"] == "alloc":
                    return {prov}
                if prov in seen or len(seen) >= MAX_DEPTH:
                    return set()
                return set().union(*(allocations(f["root"], seen | {prov})
                                     for f in self.flows.get(prov, [])))

            for flows in self.flows.values():
                for f in flows:
                    r = f["root"]
                    if f["off"] or f["terms"]:
                        carved.update(allocations(r))
                    if r["k"] == "alloc" and f["off"] == 0 and not f["terms"] and f.get("view") and not f.get("int"):
                        v = f["view"]
                        if "r" in v:
                            views[r["prov"]].add(json.dumps(v, sort_keys=True))
            self.blob_views = {k: json.loads(next(iter(v))) for k, v in views.items()
                               if len(v) == 1 and k not in carved}
            # The plugin proves the complete AST describes records and optional
            # constant trailing padding. A textual sizeof match is not proof.
            for flows in self.flows.values():
                for f in flows:
                    r = f["root"]
                    record = r.get("elementRecord") if r["k"] == "alloc" else None
                    if record in self.layout.records \
                            and r["prov"] not in carved:
                        self.blob_views[r["prov"]] = {"r": record}
        return self.blob_views

    def _stores_through(self):
        """For each pointer stored through ("load:K" destinations), the
        variables and allocations K points at."""
        if getattr(self, "_through", None) is None:
            self._through = {}
            for key in [k for k in self.flows if k.startswith("load:")]:
                inner = []
                self._trace({"k": "untyped", "prov": key[len("load:"):]}, 0, [], [], inner, frozenset(), 1)
                self._through[key] = {o.prov for o in inner if o.kind in ("object", "alloc")}
        return self._through

    @staticmethod
    def _holds_pointers(o):
        """A variable that is a pointer or an array of pointers, read whole."""
        t = o.type or {}
        while "a" in t:
            t = t["a"]
        return "p" in t and o.prov.startswith("var:")

    def _loaded_field(self, prov):
        key = prov[len("site:"):]
        site = self.sites.get(key)
        if not site or needs_provenance(site["root"]) or not site["root"].get("type"):
            return None
        return self.layout.field_at(site["root"]["type"], site["off"])


# ---- classification ------------------------------------------------------------

def view_width(site):
    return site.get("width") or 0


def resolve(layout, origin, width, site):
    """Fields covered (or the point reached) by an access from origin; plus problems."""
    t, off = origin.type, origin.offset
    problems = []
    if origin.kind == "object":
        size = layout.size(t)
        if not origin.terms and (off < 0 or (size and off + max(width, 1) > size)):
            problems.append(f"outside {origin.prov} (0x{size:x} bytes) at 0x{off:x}+{width}")
        path = origin.prov.split(":")[-1] if origin.prov.startswith("var:") and origin.prov.count(":") == 1 else origin.prov
    elif origin.kind == "pointee":
        if isinstance(origin.size, int) and not origin.terms and (
                off < 0 or off + max(width, 1) > origin.size):
            problems.append(f"outside the 0x{origin.size:x}-byte allocation at 0x{off:x}+{width}")
        # A pointer may point into an array of its pointee type.
        size = layout.size(t) or 1
        t = {"a": t, "n": 1 << 30, "sz": size << 30}
        path = "*"
    elif origin.kind == "alloc":
        # An untyped allocation has no fields to compare; only its bounds,
        # when its size is a constant.
        size = origin.size if isinstance(origin.size, int) else None
        if size is not None and not origin.terms and (off < 0 or off + max(width, 1) > size):
            problems.append(f"outside the 0x{size:x}-byte allocation at 0x{off:x}+{width}")
        return [], problems
    else:
        return None, problems
    if off < 0:
        return [(path, f"before-{-off}:{width}")], problems
    out = []
    if width > 0:
        layout.cover(t, off, width, path, out)
    else:
        out.append(layout.point(t, off, path))
    return out, problems


def is_storage(t):
    """An array of plain scalars: bytes the code lays its own records over."""
    while t and "a" in t:
        t = t["a"]
    return bool(t) and "s" in t


def stride_labels(layout, origin, terms):
    labels = []
    if origin.type is None and origin.kind != "alloc":
        return [("?", s) for _, s in terms]
    t = origin.type
    if origin.kind == "pointee":
        t = {"a": t, "n": 1 << 30, "sz": (layout.size(t) or 1) << 30}
    elif origin.kind == "alloc":
        t = {"a": {"s": "byte", "sz": 1}, "n": 1 << 30, "sz": 1 << 30}
    arrays = layout.arrays_at(t, max(origin.offset, 0), "")
    for text, stride in terms:
        label = "?"
        for path, esize in reversed(arrays):
            if esize and abs(stride) == esize:
                label = path or "[]"
                break
        else:
            for path, esize in reversed(arrays):
                if esize and abs(stride) % esize == 0:
                    label = f"{path or '[]'}*{abs(stride) // esize}"
                    break
        labels.append((label, stride))
    return labels


def describe_origin(o):
    if o.kind in ("object", "pointee"):
        t = o.type or {}
        name = t.get("r") or t.get("s") or ("array" if "a" in t else "?")
        return f"{o.kind} {name} via {o.prov} +0x{o.offset:x}" if o.offset >= 0 else f"{o.kind} {name} via {o.prov} -0x{-o.offset:x}"
    if o.kind == "alloc":
        return f"allocation {o.prov} ({o.size}) +0x{o.offset:x}"
    return f"{o.kind} {o.prov}"


def classify(key, sites, layouts, tracers, base, others):
    """Verdict for one access, comparing the base target against the others."""
    s0 = sites[base]
    result = {"key": key, "fn": s0["fn"], "text": s0["text"], "verdict": "safe", "reasons": [], "origins": [],
              "notes": [], "storage": []}
    per_target = {}
    for target, site in sites.items():
        view = site.get("view") or {}
        origins = tracers[target].origins(site["root"], site["off"], site["terms"], site.get("pun") or None)
        # A polymorphic pointer (several object types reach it): keep the
        # origins the access is typed for.
        exact = [o for o in origins if view.get("r") and (o.type or {}).get("r") == view["r"] and o.offset == 0]
        per_target[target] = exact or origins
    o0 = per_target[base]
    result["origins"] = [describe_origin(o) for o in o0]
    narrowing = [f"pointer truncated from {sites[t]['pointerBits']} to {sites[t]['integerBits']} bits on {t}"
                 for t in others if sites[t].get("pointerBits", 0) > sites[t].get("integerBits", 0)]
    if narrowing:
        result["verdict"] = "bug"
        result["reasons"] = narrowing
        return result
    if "pointerBits" in s0:
        # Merely converting an address does not read its pointee; in particular
        # a one-past array sentinel is legal. Dereferences/flows have their own
        # separately recorded sites and still receive layout/bounds checks.
        result["verdict"] = "safe"
        return result
    unresolved = []
    verdicts = []
    for o in o0:
        if o.kind == "disk":
            view = s0.get("view") or {}
            bugs = []
            if "p" in view:
                bugs.append(f"pointer-sized access in file data ({o.prov}), whose slots are 32-bit")
            elif "r" in view and view["r"] in NATIVE_VIEWS.get(o.prov, ()):
                pass
            elif "r" in view and not layouts[base].identical(view, layouts[others[0]], sites[others[0]].get("view")):
                bugs.append(f"file data ({o.prov}) viewed as {view['r']}, whose layout differs on 64-bit")
            verdicts.append((describe_origin(o), bugs, []))
        if o.kind in ("unresolved", "opaque", "untyped"):
            unresolved.append(f"root {o.prov or o.kind}" + (f" ({site_why(s0)})" if o.kind == "opaque" else ""))
        elif o.kind == "abs":
            unresolved.append(f"absolute address 0x{o.offset:x}")
    # Per resolved origin: the problems found on any 64-bit target.
    for i, a in enumerate(o0):
        if a.kind not in ("object", "pointee", "alloc"):
            continue
        payload = header_payload(a, i, per_target, layouts, base, others)
        if payload is not None:
            view = s0.get("view") or {}
            bugs = []
            if "p" in view:
                bugs.append(f"pointer-sized access in the payload after a {a.type['r']} header")
            elif "r" in view and not layouts[base].identical(view, layouts[others[0]], sites[others[0]].get("view")):
                bugs.append(f"payload after a {a.type['r']} header viewed as {view['r']}, whose layout differs")
            verdicts.append((f"payload after {a.type['r']} +0x{payload:x} via {a.prov}", bugs, []))
            continue
        bugs, doubts = [], []
        for target in others:
            o1 = per_target[target]
            if [o.ident()[:2] for o in o0] != [o.ident()[:2] for o in o1]:
                doubts.append(f"provenance differs on {target}")
                continue
            b = o1[i]
            c0, p0 = resolve(layouts[base], a, view_width(s0), s0)
            c1, p1 = resolve(layouts[target], b, view_width(sites[target]), sites[target])
            if a.kind == "object" and is_storage(a.type):
                # Scalar storage: what matters is where each view lies, checked
                # against the other views of the same object afterwards.
                if not a.terms and view_width(s0) > 0:
                    result["storage"].append((a.prov, a.offset, view_width(s0), b.offset,
                                              view_width(sites[target])))
                c0 = c1 = []
            # Out of bounds on every target is not a 64-bit problem (and often
            # a path-insensitive merge of call sites); note it only.
            result["notes"] += [p for p in p0 if p not in result["notes"]]
            bugs += [f"{target}: {p}" for p in p1 if p not in p0]
            if c0 != c1:
                if widened_scalar_only(c0, c1):
                    doubts.append(f"partial access of a scalar that is wider on {target}: {fmt_cover(c1)}")
                else:
                    bugs.append(f"touches {fmt_cover(c1)} on {target}, meant {fmt_cover(c0)}")
            if a.kind == "alloc" or a.prov.startswith("alloc:"):
                # Sized with a fixed record size but indexed with sizeof.
                for (text, st0), (_, st1) in zip(a.terms, b.terms):
                    for (atext, as0), (_, as1) in zip(a.size_terms, b.size_terms):
                        if st0 != st1 and as0 == as1 and abs(st0) == as0:
                            bugs.append(f"allocated as {atext} * 0x{as0:x} (the 32-bit record size) but indexed "
                                        f"with {text} * 0x{abs(st1):x} on {target}")
            terms = a.terms + width_terms(s0)
            l0 = stride_labels(layouts[base], a, terms)
            l1 = stride_labels(layouts[target], b, b.terms + width_terms(sites[target]))
            for (lab0, st0), (lab1, st1), (text, _) in zip(l0, l1, terms):
                if st0 != st1:
                    continue  # computed from sizeof: the code already scales it per target
                if lab0 != lab1:
                    bugs.append(f"index {text}: stride 0x{st1:x} on {target} steps {lab1}, meant {lab0} (0x{st0:x})")
                elif lab0 == "?" and not same_layout(layouts, base, target, a, b):
                    doubts.append(f"index {text}: stride 0x{st0:x} matches no array")
        verdicts.append((describe_origin(a), bugs, doubts))
    buggy = [v for v in verdicts if v[1]]
    reasons = []
    if buggy and len(buggy) == len(verdicts):
        result["verdict"] = "bug"
        for _, bugs, _ in buggy:
            reasons += bugs
    else:
        if buggy:
            unresolved.append(f"bug through {len(buggy)} of {len(verdicts)} possible origins: "
                              + "; ".join(f"{o}: {b[0]}" for o, b, _ in buggy[:3]))
        for _, _, doubts in verdicts:
            unresolved += doubts
        result["verdict"] = "unresolved" if unresolved else "safe"
    if result["verdict"] == "bug":
        reasons += [u for u in unresolved]
        reasons = sorted(set(reasons), key=reasons.index)
    else:
        reasons = sorted(set(unresolved), key=unresolved.index)
    result["reasons"] = reasons
    return result


def header_payload(a, i, per_target, layouts, base, others):
    """Offset into the payload when origin a lies past a HEADER_RECORDS header
    by the same distance on every target (sizeof-based), else None."""
    record = (a.type or {}).get("r")
    if a.kind != "pointee" or record not in HEADER_RECORDS:
        return None
    past = set()
    for target in [base] + others:
        o = per_target[target][i] if target != base else a
        if len(per_target[target]) <= i or (o.type or {}).get("r") != record:
            return None
        if [st for _, st in o.terms] != [st for _, st in a.terms]:
            return None
        past.add(o.offset - layouts[target].size({"r": record}))
    distance = past.pop()
    return distance if not past and distance >= 0 else None


def widened_scalar_only(c0, c1):
    """The covers differ only in the size of non-pointer scalars read from their start."""
    if len(c0) != len(c1):
        return False
    for (p0, d0), (p1, d1) in zip(c0, c1):
        if p0 != p1:
            return False
        if d0 == d1:
            continue
        if not (isinstance(d0, str) and isinstance(d1, str) and d0.startswith("part+") and d1.startswith("part+")):
            return False
        if d0.endswith("/ptr") or d1.endswith("/ptr") or d0.rsplit("/", 1)[0] != d1.rsplit("/", 1)[0]:
            return False
    return True


def same_layout(layouts, base, target, a, b):
    if a.kind == "alloc":
        return True
    return layouts[base].identical(a.type, layouts[target], b.type)


def width_terms(site):
    return site.get("wterms") or []


def site_why(site):
    return site["root"].get("why", "")


def fmt_cover(cover):
    parts = []
    for path, detail in cover[:6]:
        if isinstance(detail, int):
            parts.append(f"{path}+{detail}" if detail else path)
        elif detail == "all":
            parts.append(path)
        else:
            parts.append(f"{path} {detail}")
    if len(cover) > 6:
        parts.append(f"... {len(cover) - 6} more")
    return ", ".join(parts)


def check_storage(results):
    """Records laid over scalar arrays: two views that were apart on i386 must
    stay apart on 64-bit (bounds were checked per access)."""
    views = collections.defaultdict(list)
    for r in results:
        for prov, o32, w32, o64, w64 in r["storage"]:
            views[prov].append((o32, w32, o64, w64, r))
    for prov, rows in views.items():
        distinct = {(o32, w32, o64, w64): r for o32, w32, o64, w64, r in rows}
        items = list(distinct.items())
        for i, ((a32, aw32, a64, aw64), ra) in enumerate(items):
            for (b32, bw32, b64, bw64), rb in items[i + 1:]:
                apart32 = a32 + aw32 <= b32 or b32 + bw32 <= a32
                apart64 = a64 + aw64 <= b64 or b64 + bw64 <= a64
                if apart32 and not apart64:
                    for r, other in ((ra, rb), (rb, ra)):
                        r["verdict"] = "bug"
                        r["reasons"].append(f"overlaps {other['key']} in {prov} on 64-bit "
                                            f"(0x{a64:x}+{aw64} / 0x{b64:x}+{bw64})")


# ---- reporting -------------------------------------------------------------------

def apply_reviews(results, targets, path, root=ROOT):
    """Apply individually identified reviews of unchanged code, retaining the
    automatic evidence. Stale, duplicate or missing entries fail closed."""
    document = json.loads(path.read_text())
    if document["targets"] != targets:
        raise ValueError(f"{path}: reviews cover {document['targets']}, requested {targets}")
    guards = document["guards"]
    for name, expected in guards.items():
        source = root / name
        if not source.is_file() or hashlib.sha256(source.read_bytes()).hexdigest() != expected:
            raise ValueError(f"{path}: stale review guard for {name}; inspect the accesses again")
    by_key = {r["key"]: r for r in results}
    pending, seen = [], set()
    for entry in document["accesses"]:
        key = entry["key"]
        if key in seen:
            raise ValueError(f"{path}: duplicate review {key}")
        seen.add(key)
        if "game/" + key.split(":")[0] not in guards:
            raise ValueError(f"{path}: unguarded source for {key}")
        result = by_key.get(key)
        if not result or result["fn"] != entry["fn"] or result["text"] != entry["text"]:
            raise ValueError(f"{path}: missing or changed access {key}")
        if (result["verdict"] != entry["automatic_verdict"] or
                result["reasons"] != entry["automatic_reasons"]):
            raise ValueError(f"{path}: changed automatic evidence for {key}; inspect it again")
        if not entry["reason"].strip():
            raise ValueError(f"{path}: review without a reason for {key}")
        pending.append((result, entry))
    # Validate the entire document before changing any verdict.
    for result, entry in pending:
        result["automatic_verdict"] = result["verdict"]
        result["automatic_reasons"] = result["reasons"]
        result["review_reason"] = entry["reason"]
        result["verdict"] = "safe"
        result["reasons"] = []


def markdown(results, targets):
    """Bugs by function, unresolved accesses by the root that blocks them."""
    counts = collections.Counter(r["verdict"] for r in results)
    out = ["# Raw memory accesses on 64-bit targets", "",
           "Generated by `tools/audit_raw_access.py`; do not edit. Each raw access in `game/` (byte offsets,",
           "reinterpreted pointers, integer addresses, mem* calls) is resolved against the i386 layout (what the",
           f"original meant) and the {', '.join(targets[1:])} layout (what the port touches).", "",
           f"| Verdict | Accesses |", "| --- | --- |",
           f"| bug | {counts['bug']} |", f"| unresolved | {counts['unresolved']} |", f"| safe | {counts['safe']} |", "",
           "`safe` means the layout checks passed or the access was individually reviewed below. It is not",
           "a proof of bounds, lifetime, control flow or physics equivalence. `bug` is an automatic finding",
           "that still needs inspection; `unresolved` is not a clean bill of health.", "",
           "## Individually reviewed accesses", "",
           "Reviews live in `tools/raw_access/reviewed.json`, with exact sites, explanations and SHA-256",
           "guards for the source and headers. A source change or new automatic evidence invalidates the review and stops",
           "report generation until it is checked again. The JSON report retains automatic verdicts and",
           "reasons. Use `--without-reviews` to inspect all automatic findings.", ""]
    for r in results:
        if r.get("review_reason"):
            out.append(f"- `{r['key']}` ({r['fn']}): `{r['text']}` — {r['review_reason']}")
    out += ["", "## Bugs", ""]
    by_fn = collections.defaultdict(list)
    for r in results:
        if r["verdict"] == "bug":
            by_fn[(r["key"].split(":")[0], r["fn"], r["address"])].append(r)
    for (file, fn, address), rows in sorted(by_fn.items()):
        out.append(f"### {fn}" + (f" ({address})" if address else "") + f" — {file}")
        out.append("")
        for r in rows:
            line = r["key"].split(":")[1]
            out.append(f"- `{file}:{line}` `{r['text']}`: {r['reasons'][0]}")
        out.append("")
    out += ["## Unresolved, by blocking root", "",
            "Fixing or typing one root usually settles every access listed under it.", ""]
    by_root = collections.defaultdict(list)
    for r in results:
        if r["verdict"] == "unresolved":
            by_root[r["reasons"][0] if r["reasons"] else "?"].append(r)
    for reason, rows in sorted(by_root.items(), key=lambda kv: -len(kv[1])):
        keys = ", ".join(f"`{r['key'].rsplit(':', 2)[0]}`" for r in rows[:8]) + (" ..." if len(rows) > 8 else "")
        out.append(f"- {len(rows)} × {reason}: {keys}")
    return "\n".join(out) + "\n"


def function_addresses():
    """Map (file, function name) to its CMR2 address annotation."""
    out = {}
    for path in GAME.glob("*.cpp"):
        lines = path.read_text(encoding="latin1").splitlines()
        for i, line in enumerate(lines):
            m = ANNOTATION.match(line)
            if not m:
                continue
            for nxt in lines[i + 1:i + 4]:
                fm = re.search(r"\b(\w+)\s*\(", nxt)
                if fm and not nxt.startswith("//"):
                    out[(path.name, fm.group(1))] = m.group(1).lower()
                    break
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--json", type=Path)
    ap.add_argument("--markdown", type=Path)
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--targets", default="i386,x86_64")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--no-run", action="store_true", help="reuse the plugin output in build/tools/raw_access")
    ap.add_argument("--reviews", type=Path, default=REVIEWS)
    ap.add_argument("--without-reviews", action="store_true", help="show automatic verdicts without manual reviews")
    args = ap.parse_args()
    targets = args.targets.split(",")
    base, others = targets[0], targets[1:]
    if args.no_run:
        global build_plugin, run_one
        run_one = lambda target, source: (source, target, None)  # noqa: E731
        build_plugin = lambda: None  # noqa: E731
    data = collect(targets, args.jobs)
    layouts = {t: Layout(data[t]["records"]) for t in targets}
    differing = {name for name in data[base]["records"]
                 if any(not layouts[base].identical({"r": name}, layouts[t], {"r": name}) for t in others)}
    tracers = {t: Tracer(data[t], layouts[t], differing) for t in targets}
    addresses = function_addresses()

    keys = sorted(data[base]["sites"])
    results, missing = [], []
    for key in keys:
        sites = {t: data[t]["sites"].get(key) for t in targets}
        if any(s is None for s in sites.values()):
            missing.append(key)
            continue
        r = classify(key, sites, layouts, tracers, base, others)
        r["address"] = addresses.get((key.split(":")[0], r["fn"]))
        results.append(r)
    check_storage(results)
    if not args.without_reviews and args.reviews.exists():
        try:
            apply_reviews(results, targets, args.reviews)
        except (ValueError, KeyError, OSError) as error:
            sys.exit(str(error))
    elif not args.without_reviews and args.reviews != REVIEWS:
        sys.exit(f"review file does not exist: {args.reviews}")

    counts = collections.Counter(r["verdict"] for r in results)
    print(f"{len(results)} raw accesses: {counts['bug']} bug, {counts['unresolved']} unresolved, {counts['safe']} safe"
          + (f"; {len(missing)} only on {base}" if missing else "")
          + f"; {sum('review_reason' in r for r in results)} individually reviewed")
    by_file = collections.defaultdict(collections.Counter)
    for r in results:
        by_file[r["key"].split(":")[0]][r["verdict"]] += 1
    for name, c in sorted(by_file.items(), key=lambda kv: (-kv[1]["bug"], -kv[1]["unresolved"])):
        if c["bug"] or c["unresolved"]:
            print(f"  {name:28} {c['bug']:4} bug {c['unresolved']:5} unresolved {c['safe']:5} safe")
    if args.verbose:
        for r in results:
            if r["verdict"] != "safe":
                print(f"{r['verdict']:10} {r['key']} {r['fn']} {r['address'] or ''}\n    {r['text']}")
                for reason in r["reasons"]:
                    print(f"    - {reason}")
    if args.markdown:
        args.markdown.write_text(markdown(results, targets))
    if args.json:
        args.json.write_text(json.dumps({"targets": targets, "results": results, "only_on_base": missing}, indent=1) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
