#!/usr/bin/env python3
"""Regression tests for audit blind spots. --plugin also exercises real Clang ASTs.

The normal CTest run needs only Python. The optional integration run requires
the Clang development tools already used by tools/audit_raw_access.py.
"""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("audit", ROOT / "tools/audit_raw_access.py")
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)
WITH_PLUGIN = "--plugin" in sys.argv
if WITH_PLUGIN:
    sys.argv.remove("--plugin")


def header_layout(pointer_size):
    return audit.Layout({"Header": {"sz": pointer_size * 2, "union": False, "fields": [
        {"n": "pointer", "o": 0, "bw": 0, "t": {"p": "void", "sz": pointer_size}},
        {"n": "count", "o": pointer_size, "bw": 0, "t": {"s": "int", "sz": 4}},
    ]}})


def flow(root, off=0, view=None):
    return {"root": root, "off": off, "terms": [], "view": view,
            "raw": True, "int": False, "key": "fixture:flow"}


class ClassificationTests(unittest.TestCase):
    def classify(self, root_for, offset_for, width_for, flows_for=lambda p: {}, fields_for=lambda p: {}):
        layouts, tracers, sites = {}, {}, {}
        for target, pointer_size in (("i386", 4), ("x86_64", 8)):
            layout = header_layout(pointer_size)
            data = {"flows": flows_for(pointer_size), "sites": {}}
            layouts[target] = layout
            tracers[target] = audit.Tracer(data, layout, {"Header"})
            sites[target] = {"root": root_for(pointer_size), "off": offset_for(pointer_size),
                             "terms": [], "width": width_for(pointer_size), "fn": "fixture",
                             "text": "fixture access", "view": {"s": "int", "sz": 4}}
            sites[target].update(fields_for(pointer_size))
        return audit.classify("fixture", sites, layouts, tracers, "i386", ["x86_64"])

    def test_pointer_truncation_is_independent_of_pointee_layout(self):
        result = self.classify(lambda p: {"k": "untyped", "prov": "param:fixture#0"},
                               lambda p: 0, lambda p: 0,
                               fields_for=lambda p: {"pointerBits": p * 8, "integerBits": 32})
        self.assertEqual(result["verdict"], "bug", result)
        self.assertIn("pointer truncated from 64 to 32", result["reasons"][0])

    def test_pointer_sized_integer_is_not_truncation(self):
        result = self.classify(lambda p: {"k": "untyped", "prov": "param:fixture#0"},
                               lambda p: 0, lambda p: 0,
                               fields_for=lambda p: {"pointerBits": p * 8, "integerBits": p * 8})
        self.assertNotEqual(result["verdict"], "bug", result)

    def test_equal_distance_after_header_does_not_skip_wrong_field(self):
        # On i386 this reads the next Header.count; on x64 it reads half of
        # the next pointer. The previous generic header exemption missed it.
        result = self.classify(
            lambda p: {"k": "pointee", "prov": "param:fixture#0", "type": {"r": "Header"}},
            lambda p: 2 * p + 4, lambda p: 4)
        self.assertEqual(result["verdict"], "bug", result)

    def test_payload_after_a_listed_header_is_checked_as_data(self):
        # Header records are followed by a pointer-free payload at the same
        # distance past sizeof(Header) on every target.
        root = lambda p: {"k": "pointee", "prov": "param:fixture#0", "type": {"r": "Header"}}
        saved = audit.HEADER_RECORDS
        audit.HEADER_RECORDS = {"Header"}
        try:
            result = self.classify(root, lambda p: 2 * p + 4, lambda p: 4)
            self.assertEqual(result["verdict"], "safe", result)
            pointer = self.classify(root, lambda p: 2 * p + 8, lambda p: p,
                                    fields_for=lambda p: {"view": {"p": "void", "sz": p}})
            self.assertEqual(pointer["verdict"], "bug", pointer)
        finally:
            audit.HEADER_RECORDS = saved

    def test_typed_cast_does_not_erase_allocation_bounds(self):
        allocation = {"k": "alloc", "prov": "alloc:fixture", "size": 8}
        result = self.classify(lambda p: allocation, lambda p: 0, lambda p: 2 * p,
                               lambda p: {"var:fixture": [flow(allocation, view={"r": "Header"})]})
        self.assertEqual(result["verdict"], "bug", result)
        self.assertIn("allocation", " ".join(result["reasons"]))

    def test_indirect_carving_prevents_homogeneous_inference(self):
        allocation = {"k": "alloc", "prov": "alloc:fixture", "size": "count * sizeof(Header)",
                      "elementRecord": "Header"}
        data = {"sites": {}, "flows": {
            "var:base": [flow(allocation, view={"r": "Header"})],
            "var:payload": [flow({"k": "untyped", "prov": "var:base"}, off=8)],
        }}
        tracer = audit.Tracer(data, header_layout(4), {"Header"})
        self.assertNotIn("alloc:fixture", tracer.alloc_types())

    def origins(self, root, flows):
        tracer = audit.Tracer({"flows": flows, "sites": {}}, header_layout(8), {"Header"})
        return tracer.origins(root, 0, [])

    def test_loop_through_a_local_adds_no_origin(self):
        # p = FindFile(...); while (...) p = next(p);
        local = {"k": "untyped", "prov": "var:fixture:p:1"}
        flows = {"var:fixture:p:1": [flow({"k": "untyped", "prov": "ret:FindFile"}),
                                     flow({"k": "untyped", "prov": "ret:next"})],
                 "ret:next": [flow({"k": "untyped", "prov": "param:next#0"})],
                 "param:next#0": [flow(local)]}
        origins = self.origins(local, flows)
        self.assertEqual([o.kind for o in origins], ["disk"])

    def test_loop_of_nothing_but_cycles_is_unresolved(self):
        local = {"k": "untyped", "prov": "var:fixture:p:1"}
        flows = {"var:fixture:p:1": [flow({"k": "untyped", "prov": "param:next#0"})],
                 "param:next#0": [flow(local)]}
        self.assertEqual([o.kind for o in self.origins(local, flows)], ["unresolved"])

    def test_loop_through_a_field_the_platform_writes_stays_unresolved(self):
        field = {"k": "untyped", "prov": "field:SysEvent::data"}
        flows = {"field:SysEvent::data": [flow(field)]}
        self.assertIn("unresolved", [o.kind for o in self.origins(field, flows)])

    def test_pointer_loaded_through_a_pointer_variable_follows_its_stores(self):
        # char *files[2]; files[i] = FindFile(...); f(files); ... p = pFiles[i];
        files = {"k": "object", "prov": "var:g_fixtureFiles", "type": {"a": {"p": "byte", "sz": 8}, "n": 2, "sz": 16}}
        flows = {"param:f#0": [flow(files)],
                 "var:g_fixtureFiles": [flow({"k": "untyped", "prov": "ret:FindFile"})]}
        origins = self.origins({"k": "untyped", "prov": "load:param:f#0"}, flows)
        self.assertEqual([o.kind for o in origins], ["disk"])

    def test_pointer_loaded_through_an_unknown_pointer_stays_unresolved(self):
        origins = self.origins({"k": "untyped", "prov": "load:param:f#0"}, {})
        self.assertEqual([o.kind for o in origins], ["unresolved"])

    def test_global_nothing_writes_is_null(self):
        self.assertTrue(audit.never_written("var:g_fixtureNeverWrittenAnywhere"))
        self.assertFalse(audit.never_written("var:g_languageTexts"))
        self.assertEqual(self.origins({"k": "untyped", "prov": "var:g_fixtureNeverWrittenAnywhere"}, {}), [])

    def test_single_sizeof_in_text_is_not_evidence_of_homogeneous_storage(self):
        allocation = {"k": "alloc", "prov": "alloc:fixture",
                      "sizeText": "otherBytes + count * sizeof(Header) + 8"}
        data = {"sites": {}, "flows": {"var:base": [flow(allocation)]}}
        self.assertEqual(audit.Tracer(data, header_layout(4), {"Header"}).alloc_types(), {})


class ReviewTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "game").mkdir()
        self.source = self.root / "game/Fixture.cpp"
        self.source.write_text("fixture source")
        self.header = self.root / "game/Fixture.h"
        self.header.write_text("fixture header")
        self.entry = {"key": "Fixture.cpp:1:1:deref", "fn": "fixture", "text": "*(int *)p",
                      "reason": "The buffer stores a fixed-width disk integer.",
                      "automatic_verdict": "bug", "automatic_reasons": ["automatic evidence"]}
        self.document = {"targets": ["i386", "x86_64"], "guards": {
            str(p.relative_to(self.root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in [self.source, self.header]}, "accesses": [self.entry]}
        self.path = self.root / "reviews.json"
        self.results = [dict(self.entry, verdict="bug", reasons=["automatic evidence"])]

    def apply(self, targets=None):
        self.path.write_text(json.dumps(self.document))
        audit.apply_reviews(self.results, targets or self.document["targets"], self.path, self.root)

    def test_review_retains_automatic_evidence(self):
        self.apply()
        self.assertEqual(self.results[0]["verdict"], "safe")
        self.assertEqual(self.results[0]["automatic_verdict"], "bug")
        self.assertEqual(self.results[0]["automatic_reasons"], ["automatic evidence"])

    def test_source_and_header_edits_invalidate_review(self):
        for path in [self.source, self.header]:
            original = path.read_text()
            path.write_text(original + " changed")
            with self.assertRaisesRegex(ValueError, "stale review guard"):
                self.apply()
            self.assertEqual(self.results[0]["verdict"], "bug")
            path.write_text(original)

    def test_unreviewed_target_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "requested"):
            self.apply(["i386", "windows"])

    def test_changed_expression_is_rejected(self):
        self.results[0]["text"] = "*(void **)p"
        with self.assertRaisesRegex(ValueError, "changed access"):
            self.apply()

    def test_new_automatic_evidence_is_not_hidden_by_an_old_review(self):
        self.results[0]["reasons"].append("new overlap detected")
        with self.assertRaisesRegex(ValueError, "changed automatic evidence"):
            self.apply()
        self.assertEqual(self.results[0]["verdict"], "bug")

    def test_duplicate_reviews_are_rejected_without_partial_application(self):
        self.document["accesses"].append(self.entry)
        with self.assertRaisesRegex(ValueError, "duplicate review"):
            self.apply()
        self.assertEqual(self.results[0]["verdict"], "bug")

    def test_missing_site_and_unguarded_source_are_rejected(self):
        self.results.clear()
        with self.assertRaisesRegex(ValueError, "missing or changed access"):
            self.apply()
        del self.document["guards"]["game/Fixture.cpp"]
        with self.assertRaisesRegex(ValueError, "unguarded source"):
            self.apply()


@unittest.skipUnless(WITH_PLUGIN, "use --plugin to test the Clang integration")
class PluginTests(unittest.TestCase):
    def test_pointer_cast_widths_are_recorded_even_in_return_expressions(self):
        audit.build_plugin()
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            source = directory / "Fixture.cpp"
            source.write_text("int narrow(void *p) { return (int)p; }\n"
                              "__UINTPTR_TYPE__ wide(void *p) { return (__UINTPTR_TYPE__)p; }\n")
            for target, pointer_bits in [("i386", 32), ("x86_64", 64)]:
                output = directory / (target + ".jsonl")
                subprocess.run(["clang++", "-target", audit.TARGETS[target], "-fsyntax-only", "-std=c++17",
                                "-fms-extensions", "-Wno-everything", f"-fplugin={audit.PLUGIN}",
                                f"-fplugin-arg-rawaccess-out={output}",
                                f"-fplugin-arg-rawaccess-dir={directory}/", str(source)], check=True)
                sites = [json.loads(line) for line in output.read_text().splitlines()]
                casts = {s["fn"]: s for s in sites if "pointerBits" in s}
                self.assertEqual(casts["narrow"]["pointerBits"], pointer_bits)
                self.assertEqual(casts["narrow"]["integerBits"], 32)
                self.assertEqual(casts["wide"]["integerBits"], pointer_bits)

    def test_allocation_ast_and_calloc_argument_order(self):
        audit.build_plugin()
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            source = directory / "Fixture.cpp"
            source.write_text('''
struct Header { void *pointer; int count; };
struct Other { void *pointer; };
extern "C" void *malloc(__SIZE_TYPE__);
extern "C" void *calloc(__SIZE_TYPE__, __SIZE_TYPE__);
void fixture(int n, int otherBytes) {
    void *single = malloc(sizeof(Header));
    void *array = malloc(n * sizeof(Header));
    void *padded = malloc(n * sizeof(Header) + 1);
    void *mixed = malloc(otherBytes + n * sizeof(Header) + 8);
    void *twoTypes = malloc(sizeof(Header) + n * sizeof(Other));
    void *shortRecord = malloc(sizeof(Header) - 1);
    void *divided = malloc(sizeof(Header) / 2);
    void *callocLeft = calloc(sizeof(Header), n);
    void *callocRight = calloc(n, sizeof(Header));
    void *callocMixed = calloc(sizeof(Header), sizeof(Other));
}
''')
            for target in ["i386", "x86_64"]:
                output = directory / (target + ".jsonl")
                subprocess.run(["clang++", "-target", audit.TARGETS[target], "-fsyntax-only", "-std=c++17",
                                f"-fplugin={audit.PLUGIN}", f"-fplugin-arg-rawaccess-out={output}",
                                f"-fplugin-arg-rawaccess-dir={directory}/", str(source)], check=True)
                flows = [json.loads(line) for line in output.read_text().splitlines()]
                allocations = {r["dst"].split(":")[-2]: r["root"] for r in flows
                               if r["t"] == "flow" and r["root"]["k"] == "alloc"}
                for name in ["single", "array", "padded", "callocLeft", "callocRight"]:
                    self.assertEqual(allocations[name]["elementRecord"], "Header", (target, name))
                for name in ["mixed", "twoTypes", "shortRecord", "divided", "callocMixed"]:
                    self.assertEqual(allocations[name]["elementRecord"], "", (target, name))
                for name in ["callocLeft", "callocRight"]:
                    self.assertIn("sizeof(Header)", allocations[name]["sizeText"])
                    self.assertIn("(n)", allocations[name]["sizeText"])

    def test_element_loaded_through_a_pointer_is_a_load(self):
        audit.build_plugin()
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            source = directory / "Fixture.cpp"
            source.write_text("char *g_files[2];\n"
                              "char *pick(char **pFiles, int i) { char *p = pFiles[i]; char *q = g_files[i]; return p ? p : q; }\n")
            output = directory / "x86_64.jsonl"
            subprocess.run(["clang++", "-target", audit.TARGETS["x86_64"], "-fsyntax-only", "-std=c++17",
                            f"-fplugin={audit.PLUGIN}", f"-fplugin-arg-rawaccess-out={output}",
                            f"-fplugin-arg-rawaccess-dir={directory}/", str(source)], check=True)
            flows = {r["dst"].split(":")[2]: r["root"]["prov"] for r in map(json.loads, output.read_text().splitlines())
                     if r["t"] == "flow" and r["dst"].startswith("var:pick:")}
            self.assertEqual(flows["p"], "load:param:pick#0")
            self.assertEqual(flows["q"], "var:g_files")

    def test_byte_cursor_defined_once_with_sizeof_keeps_its_stride(self):
        audit.build_plugin()
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            source = directory / "Fixture.cpp"
            source.write_text("struct Row { void *p; int n; };\nRow g_rows[4];\n"
                              "int once(int i) { int off = i * sizeof(Row); return ((Row *)((char *)g_rows + off))->n; }\n"
                              "int twice(int i) { int off = i * sizeof(Row); off += 1; return ((Row *)((char *)g_rows + off))->n; }\n")
            output = directory / "x86_64.jsonl"
            subprocess.run(["clang++", "-target", audit.TARGETS["x86_64"], "-fsyntax-only", "-std=c++17",
                            f"-fplugin={audit.PLUGIN}", f"-fplugin-arg-rawaccess-out={output}",
                            f"-fplugin-arg-rawaccess-dir={directory}/", str(source)], check=True)
            sites = {r["fn"]: r for r in map(json.loads, output.read_text().splitlines())
                     if r["t"] == "site" and r["text"].endswith("->n")}
            self.assertEqual(sites["once"]["terms"], [["i", 16]])
            self.assertEqual(sites["twice"]["terms"], [["off", 1]])


if __name__ == "__main__":
    unittest.main()
