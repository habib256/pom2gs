#!/usr/bin/env python3
"""Contract tests for tools/cycle_suite.py (stdlib-only)."""

from __future__ import annotations

import copy
import contextlib
import hashlib
import io
import json
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import cycle_suite  # noqa: E402


class CycleSuiteTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.catalog_path = ROOT / "tests" / "cycle_accuracy" / "catalog.json"
        cls.catalog = cycle_suite.load_catalog(cls.catalog_path)

    def test_repository_catalog_is_valid_and_covers_every_core_domain(self) -> None:
        cycle_suite.validate_catalog(self.catalog)
        covered = {domain for case in self.catalog["tests"] for domain in case["domains"]}
        self.assertEqual(
            covered,
            {
                "cpu65816", "fpi-cya", "mega2", "vgc", "interrupts",
                "shadowing", "adb", "scc", "doc", "iwm-sony",
                "smartport", "rtc",
            },
        )
        mister = next(case for case in self.catalog["tests"] if case["id"] == "mister-custom-diagnostics")
        self.assertEqual(len(mister["members"]), 21)
        self.assertEqual(len({member["id"] for member in mister["members"]}), 21)
        self.assertEqual(len(mister["build"]["outputs"]), 23)

    def test_duplicate_test_ids_are_rejected(self) -> None:
        broken = copy.deepcopy(self.catalog)
        broken["tests"].append(copy.deepcopy(broken["tests"][0]))
        with self.assertRaisesRegex(ValueError, "duplicate test id"):
            cycle_suite.validate_catalog(broken)

    def test_user_supplied_assets_cannot_have_download_urls(self) -> None:
        broken = copy.deepcopy(self.catalog)
        case = next(c for c in broken["tests"] if c["asset_policy"] == "user-supplied")
        case["assets"] = [{"url": "https://example.invalid/copyrighted.2mg"}]
        with self.assertRaisesRegex(ValueError, "user-supplied"):
            cycle_suite.validate_catalog(broken)

    def test_reference_only_asset_is_catalogued_but_never_fetched(self) -> None:
        changed = copy.deepcopy(self.catalog)
        case = changed["tests"][0]
        case["asset_policy"] = "reference-only"
        case["assets"] = [{
            "name": "author archive",
            "path": "author.zip",
            "url": "https://example.invalid/author.zip",
            "sha256": "0" * 64,
            "size": 1,
            "license": "NOASSERTION",
        }]
        cycle_suite.validate_catalog(changed)
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(cycle_suite.fetch_assets([case], Path(tmp)), 0)

    def test_catalog_rejects_unsafe_asset_paths(self) -> None:
        broken = copy.deepcopy(self.catalog)
        case = next(c for c in broken["tests"] if c["id"] == "mister-custom-diagnostics")
        case["assets"][0]["path"] = "../outside.tar.gz"
        with self.assertRaisesRegex(ValueError, "unsafe path"):
            cycle_suite.validate_catalog(broken)

    def test_command_expansion_rejects_unknown_placeholders(self) -> None:
        with self.assertRaisesRegex(ValueError, "missing command variable"):
            cycle_suite.expand_command(["{build_dir}/speed_test", "{missing}"], {"build_dir": "build"})

    def test_missing_requirements_are_reported_as_skip_reasons(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            variables = {"asset_dir": td, "missing": str(Path(td) / "not-there")}
            requirements = [
                {"path": "{asset_dir}", "kind": "directory"},
                {"path": "{missing}", "kind": "file"},
            ]
            self.assertEqual(
                cycle_suite.missing_requirements(requirements, variables),
                [f"file:{variables['missing']}"],
            )

    def test_sha256_check_uses_real_file_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "asset.bin"
            p.write_bytes(b"cycle-accurate\n")
            digest = hashlib.sha256(p.read_bytes()).hexdigest()
            cycle_suite.verify_sha256(p, digest)
            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                cycle_suite.verify_sha256(p, "0" * 64)

    def test_list_rows_are_stable_and_include_readiness(self) -> None:
        rows = cycle_suite.list_rows(self.catalog)
        self.assertEqual(rows, sorted(rows, key=lambda row: row[0]))
        self.assertTrue(all(len(row) == 5 for row in rows))
        self.assertIn("textfunk-beamrace", {row[0] for row in rows})

    def test_external_command_skip_code_is_not_a_failure(self) -> None:
        case = {
            "id": "external-fixture",
            "readiness": "automated",
            "requires": [],
            "command": ["fixture"],
        }
        with mock.patch.object(
            cycle_suite.subprocess,
            "run",
            return_value=cycle_suite.subprocess.CompletedProcess(["fixture"], 77),
        ), io.StringIO() as output, contextlib.redirect_stdout(output):
            self.assertEqual(cycle_suite.run_cases([case], {}), 0)
            self.assertIn("SKIP external-fixture", output.getvalue())

    def test_prepare_extracts_only_the_pinned_subtree_and_is_idempotent(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            assets = root / "assets"
            output = root / "output"
            assets.mkdir()
            archive_path = assets / "fixture.tar.gz"
            with tarfile.open(archive_path, "w:gz") as archive:
                for name, payload in {
                    "upstream/customtests/README.md": b"fixture\n",
                    "upstream/customtests/TEST.S": b" org $2000\n",
                    "upstream/unrelated.txt": b"do not extract\n",
                }.items():
                    info = tarfile.TarInfo(name)
                    info.size = len(payload)
                    archive.addfile(info, io.BytesIO(payload))
            digest = hashlib.sha256(archive_path.read_bytes()).hexdigest()
            case = {
                "id": "fixture",
                "assets": [{
                    "path": archive_path.name,
                    "sha256": digest,
                    "extract": {
                        "prefix": "upstream/customtests",
                        "destination": "fixture/source",
                        "required": ["README.md"],
                    },
                }],
                "members": [{"source": "TEST.S"}],
            }
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(cycle_suite.prepare_assets([case], assets, output), 1)
            destination = output / "fixture" / "source"
            self.assertEqual((destination / "TEST.S").read_bytes(), b" org $2000\n")
            self.assertFalse((destination / "unrelated.txt").exists())
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(cycle_suite.prepare_assets([case], assets, output), 1)

    def test_prepare_rejects_parent_traversal_members(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            assets = root / "assets"
            assets.mkdir()
            archive_path = assets / "bad.tar.gz"
            with tarfile.open(archive_path, "w:gz") as archive:
                info = tarfile.TarInfo("upstream/customtests/../escape")
                info.size = 1
                archive.addfile(info, io.BytesIO(b"x"))
            case = {
                "id": "bad-fixture",
                "assets": [{
                    "path": archive_path.name,
                    "sha256": hashlib.sha256(archive_path.read_bytes()).hexdigest(),
                    "extract": {
                        "prefix": "upstream/customtests",
                        "destination": "fixture/source",
                    },
                }],
            }
            with self.assertRaisesRegex(ValueError, "unsafe archive member"):
                with contextlib.redirect_stdout(io.StringIO()):
                    cycle_suite.prepare_assets([case], assets, root / "output")
            self.assertFalse((root / "output" / "escape").exists())

    def test_build_records_source_tool_and_output_hashes(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            output = root / "output"
            source = output / "fixture" / "source"
            merlin_root = root / "merlin"
            (merlin_root / "Library").mkdir(parents=True)
            source.mkdir(parents=True)
            marker = {"test_id": "fixture", "sha256": "1" * 64}
            (source / ".pomiigs-source.json").write_text(json.dumps(marker), encoding="utf-8")
            disk = source / "fixture.2mg"
            disk.write_bytes(b"disk\n")
            merlin = merlin_root / "Merlin32"
            cp2 = root / "cp2"
            merlin.write_bytes(b"assembler\n")
            cp2.write_bytes(b"disk tool\n")
            case = {
                "id": "fixture",
                "assets": [{"extract": {"destination": "fixture/source"}}],
                "build": {
                    "requires": [
                        {"kind": "variable", "name": "merlin32"},
                        {"kind": "file", "path": "{source_dir}/.pomiigs-source.json"},
                    ],
                    "command": [sys.executable, "-c", "pass"],
                    "outputs": ["{source_dir}/fixture.2mg"],
                },
            }
            variables = {
                "out_dir": str(output),
                "merlin_root": str(merlin_root),
                "merlin32": str(merlin),
                "cp2": str(cp2),
                "cp2_dir": str(root),
            }
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(cycle_suite.build_cases([case], variables), 0)
            manifest = json.loads((source / "pomiigs-build-manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["source_marker"], marker)
            self.assertEqual(manifest["outputs"][disk.name], hashlib.sha256(disk.read_bytes()).hexdigest())
            self.assertEqual(manifest["tools"]["merlin32_sha256"], hashlib.sha256(merlin.read_bytes()).hexdigest())


if __name__ == "__main__":
    unittest.main()
