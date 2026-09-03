#!/usr/bin/env python3
"""Catalog, prepare, build and execute POMIIGS cycle-accuracy gates.

The catalog deliberately separates automated local gates, downloadable open
sources, and user-supplied copyrighted workloads. Commands are argv arrays and
are never passed through a shell.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path, PurePosixPath
from typing import Any, Iterable

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CATALOG = ROOT / "tests" / "cycle_accuracy" / "catalog.json"
VALID_TIERS = {"unit", "cycle", "external-open", "external-user-supplied"}
VALID_READINESS = {"automated", "staged", "blocked-live-scanout", "blocked-floating-bus"}
VALID_POLICIES = {"none", "fetchable", "reference-only", "user-supplied", "bundled"}
REQUIRED_DOMAINS = {
    "cpu65816", "fpi-cya", "mega2", "vgc", "interrupts", "shadowing",
    "adb", "scc", "doc", "iwm-sony", "smartport", "rtc",
}


def load_catalog(path: Path | str = DEFAULT_CATALOG) -> dict[str, Any]:
    with Path(path).open("r", encoding="utf-8") as handle:
        data = json.load(handle)
    validate_catalog(data)
    return data


def _require_mapping(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValueError(f"{label} must be an object")
    return value


def _require_list(value: Any, label: str) -> list[Any]:
    if not isinstance(value, list):
        raise ValueError(f"{label} must be an array")
    return value


def validate_catalog(catalog: dict[str, Any]) -> None:
    _require_mapping(catalog, "catalog")
    if catalog.get("schema_version") != 1:
        raise ValueError("schema_version must be 1")
    sources = _require_mapping(catalog.get("sources"), "sources")
    tests = _require_list(catalog.get("tests"), "tests")
    if not tests:
        raise ValueError("tests must not be empty")

    for source_id, source_value in sources.items():
        source = _require_mapping(source_value, f"source {source_id}")
        for key in ("url", "revision", "license", "role"):
            if not isinstance(source.get(key), str) or not source[key].strip():
                raise ValueError(f"source {source_id} missing {key}")

    seen: set[str] = set()
    covered: set[str] = set()
    for index, case_value in enumerate(tests):
        case = _require_mapping(case_value, f"tests[{index}]")
        case_id = case.get("id")
        if not isinstance(case_id, str) or not case_id:
            raise ValueError(f"tests[{index}] missing id")
        if case_id in seen:
            raise ValueError(f"duplicate test id: {case_id}")
        seen.add(case_id)
        for key in ("title", "oracle"):
            if not isinstance(case.get(key), str) or not case[key].strip():
                raise ValueError(f"test {case_id} missing {key}")
        if case.get("tier") not in VALID_TIERS:
            raise ValueError(f"test {case_id} has invalid tier")
        if case.get("readiness") not in VALID_READINESS:
            raise ValueError(f"test {case_id} has invalid readiness")
        policy = case.get("asset_policy")
        if policy not in VALID_POLICIES:
            raise ValueError(f"test {case_id} has invalid asset_policy")
        domains = _require_list(case.get("domains"), f"test {case_id} domains")
        if not domains or not all(isinstance(item, str) and item in REQUIRED_DOMAINS for item in domains):
            raise ValueError(f"test {case_id} has invalid domains")
        covered.update(domains)
        references = _require_list(case.get("references"), f"test {case_id} references")
        unknown = [ref for ref in references if ref not in sources]
        if unknown:
            raise ValueError(f"test {case_id} references unknown source(s): {', '.join(unknown)}")
        assets = _require_list(case.get("assets"), f"test {case_id} assets")
        command = _require_list(case.get("command"), f"test {case_id} command")
        requirements = _require_list(case.get("requires", []), f"test {case_id} requires")
        if not all(isinstance(item, str) for item in command):
            raise ValueError(f"test {case_id} command must contain strings")
        for requirement in requirements:
            if not _valid_requirement(requirement):
                raise ValueError(f"test {case_id} has invalid requirement")
        members = _require_list(case.get("members", []), f"test {case_id} members")
        member_ids: set[str] = set()
        for member in members:
            if not isinstance(member, dict) or not all(isinstance(member.get(key), str) and member[key] for key in ("id", "source", "covers")):
                raise ValueError(f"test {case_id} has invalid member")
            if member["id"] in member_ids:
                raise ValueError(f"test {case_id} has duplicate member id: {member['id']}")
            member_ids.add(member["id"])
        if case.get("readiness") == "automated" and not command:
            raise ValueError(f"automated test {case_id} has no command")
        if policy == "user-supplied":
            for asset in assets:
                if not isinstance(asset, dict) or "url" in asset:
                    raise ValueError(f"user-supplied test {case_id} cannot declare download URLs")
        if policy in {"fetchable", "reference-only"}:
            for asset in assets:
                if not isinstance(asset, dict):
                    raise ValueError(f"{policy} test {case_id} has invalid asset")
                for key in ("name", "path", "url", "sha256", "license"):
                    if not isinstance(asset.get(key), str) or not asset[key].strip():
                        raise ValueError(f"{policy} test {case_id} asset missing {key}")
                asset_path = PurePosixPath(asset["path"])
                if asset_path.is_absolute() or ".." in asset_path.parts:
                    raise ValueError(f"{policy} test {case_id} asset has unsafe path")
                digest = asset["sha256"]
                if not isinstance(digest, str) or len(digest) != 64:
                    raise ValueError(f"{policy} test {case_id} asset has invalid sha256")
                extraction = asset.get("extract")
                if extraction is not None:
                    extraction = _require_mapping(extraction, f"{policy} test {case_id} extraction")
                    if extraction.get("format") != "tar.gz":
                        raise ValueError(f"{policy} test {case_id} has unsupported extraction format")
                    for key in ("prefix", "destination"):
                        value = extraction.get(key)
                        if not isinstance(value, str) or not value.strip():
                            raise ValueError(f"{policy} test {case_id} extraction missing {key}")
                        path = PurePosixPath(value)
                        if path.is_absolute() or ".." in path.parts:
                            raise ValueError(f"{policy} test {case_id} extraction has unsafe {key}")
                    required = _require_list(extraction.get("required", []), f"{policy} test {case_id} extraction required")
                    if not all(isinstance(item, str) and item and not PurePosixPath(item).is_absolute() and ".." not in PurePosixPath(item).parts for item in required):
                        raise ValueError(f"{policy} test {case_id} extraction has unsafe required path")

        build = case.get("build")
        if build is not None:
            build = _require_mapping(build, f"test {case_id} build")
            build_command = _require_list(build.get("command"), f"test {case_id} build command")
            build_requirements = _require_list(build.get("requires"), f"test {case_id} build requires")
            build_outputs = _require_list(build.get("outputs"), f"test {case_id} build outputs")
            if not build_command or not all(isinstance(item, str) and item for item in build_command):
                raise ValueError(f"test {case_id} has invalid build command")
            for requirement in build_requirements:
                if not _valid_requirement(requirement):
                    raise ValueError(f"test {case_id} has invalid build requirement")
            if not build_outputs or not all(isinstance(item, str) and item for item in build_outputs):
                raise ValueError(f"test {case_id} has invalid build outputs")

    if covered != REQUIRED_DOMAINS:
        missing = sorted(REQUIRED_DOMAINS - covered)
        extra = sorted(covered - REQUIRED_DOMAINS)
        raise ValueError(f"domain coverage mismatch; missing={missing}, extra={extra}")


def _valid_requirement(requirement: Any) -> bool:
    if not isinstance(requirement, dict):
        return False
    kind = requirement.get("kind")
    if kind == "variable":
        return isinstance(requirement.get("name"), str) and bool(requirement["name"])
    return kind in {"file", "directory"} and isinstance(requirement.get("path"), str)


def expand_command(command: Iterable[str], variables: dict[str, str]) -> list[str]:
    expanded: list[str] = []
    for item in command:
        try:
            expanded.append(item.format_map(variables))
        except KeyError as exc:
            raise ValueError(f"missing command variable: {exc.args[0]}") from exc
    return expanded


def verify_sha256(path: Path | str, expected: str) -> None:
    digest = hashlib.sha256()
    with Path(path).open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    actual = digest.hexdigest()
    if actual.lower() != expected.lower():
        raise ValueError(f"SHA-256 mismatch for {path}: got {actual}, expected {expected}")


def missing_requirements(requirements: Iterable[dict[str, str]], variables: dict[str, str]) -> list[str]:
    missing: list[str] = []
    for requirement in requirements:
        if requirement["kind"] == "variable":
            name = requirement["name"]
            if not variables.get(name):
                missing.append(f"variable:{name}")
            continue
        template = requirement["path"]
        if any(not value and f"{{{name}}}" in template for name, value in variables.items()):
            continue  # the matching variable requirement gives the useful reason
        path = Path(expand_command([template], variables)[0])
        kind = requirement["kind"]
        present = path.is_file() if kind == "file" else path.is_dir()
        if not present:
            missing.append(f"{kind}:{path}")
    return missing


def list_rows(catalog: dict[str, Any]) -> list[tuple[str, str, str, str, str]]:
    rows = [
        (case["id"], case["tier"], case["readiness"], case["asset_policy"], case["title"])
        for case in catalog["tests"]
    ]
    return sorted(rows, key=lambda row: row[0])


def selected_tests(catalog: dict[str, Any], ids: list[str], tier: str | None) -> list[dict[str, Any]]:
    known = {case["id"]: case for case in catalog["tests"]}
    unknown = [case_id for case_id in ids if case_id not in known]
    if unknown:
        raise ValueError(f"unknown test id(s): {', '.join(unknown)}")
    cases = [known[case_id] for case_id in ids] if ids else list(catalog["tests"])
    if tier:
        cases = [case for case in cases if case["tier"] == tier]
    return cases


def fetch_assets(cases: Iterable[dict[str, Any]], destination: Path) -> int:
    destination.mkdir(parents=True, exist_ok=True)
    count = 0
    for case in cases:
        if case["asset_policy"] != "fetchable":
            continue
        for asset in case["assets"]:
            target = destination / asset["path"]
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.exists():
                verify_sha256(target, asset["sha256"])
                print(f"PRESENT {case['id']}: {target}")
                count += 1
                continue
            partial = target.with_suffix(target.suffix + ".part")
            print(f"FETCH {case['id']}: {asset['url']}")
            try:
                with urllib.request.urlopen(asset["url"], timeout=60) as response, partial.open("wb") as output:
                    shutil.copyfileobj(response, output)
                verify_sha256(partial, asset["sha256"])
                partial.replace(target)
            finally:
                if partial.exists():
                    partial.unlink()
            print(f"OK {target}")
            count += 1
    return count


def _selected_tar_members(archive: tarfile.TarFile, prefix: str) -> list[tuple[tarfile.TarInfo, PurePosixPath]]:
    root = PurePosixPath(prefix.rstrip("/"))
    selected: list[tuple[tarfile.TarInfo, PurePosixPath]] = []
    total_size = 0
    for member in archive.getmembers():
        member_path = PurePosixPath(member.name)
        try:
            relative = member_path.relative_to(root)
        except ValueError:
            continue
        if not relative.parts:
            continue
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError(f"unsafe archive member: {member.name}")
        if not (member.isdir() or member.isfile()):
            raise ValueError(f"unsupported archive member type: {member.name}")
        total_size += member.size
        if member.size > 32 * 1024 * 1024 or total_size > 128 * 1024 * 1024:
            raise ValueError("selected archive content exceeds extraction safety limit")
        selected.append((member, relative))
    if not selected:
        raise ValueError(f"archive contains no members below {prefix}")
    return selected


def _check_prepared_requirements(destination: Path, required: Iterable[str]) -> None:
    missing = [name for name in required if not (destination / name).is_file()]
    if missing:
        raise ValueError(f"prepared source is missing required file(s): {', '.join(missing)}")


def prepare_assets(cases: Iterable[dict[str, Any]], assets_dir: Path, out_dir: Path) -> int:
    prepared = 0
    for case in cases:
        for asset in case["assets"]:
            extraction = asset.get("extract")
            if extraction is None:
                continue
            archive_path = assets_dir / asset["path"]
            if not archive_path.is_file():
                print(f"SKIP {case['id']}: missing fetched archive {archive_path}")
                continue
            verify_sha256(archive_path, asset["sha256"])
            destination = out_dir / extraction["destination"]
            required = list(extraction.get("required", []))
            required.extend(member["source"] for member in case.get("members", []))
            marker_path = destination / ".pomiigs-source.json"
            marker = {
                "test_id": case["id"],
                "archive": asset["path"],
                "sha256": asset["sha256"].lower(),
                "prefix": extraction["prefix"],
            }
            if marker_path.is_file():
                with marker_path.open("r", encoding="utf-8") as handle:
                    existing = json.load(handle)
                if existing != marker:
                    raise ValueError(f"prepared destination marker does not match catalog: {destination}")
                _check_prepared_requirements(destination, required)
                print(f"PRESENT {case['id']}: {destination}")
                prepared += 1
                continue
            if destination.exists():
                raise ValueError(f"refusing to overwrite unmarked prepared destination: {destination}")

            destination.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(prefix=f".{destination.name}.", dir=destination.parent) as temp_name:
                staging = Path(temp_name) / "source"
                staging.mkdir()
                with tarfile.open(archive_path, "r:gz") as archive:
                    for member, relative in _selected_tar_members(archive, extraction["prefix"]):
                        target = staging.joinpath(*relative.parts)
                        if member.isdir():
                            target.mkdir(parents=True, exist_ok=True)
                            continue
                        target.parent.mkdir(parents=True, exist_ok=True)
                        source = archive.extractfile(member)
                        if source is None:
                            raise ValueError(f"cannot read archive member: {member.name}")
                        with source, target.open("wb") as output:
                            shutil.copyfileobj(source, output)
                _check_prepared_requirements(staging, required)
                with (staging / ".pomiigs-source.json").open("w", encoding="utf-8") as handle:
                    json.dump(marker, handle, indent=2, sort_keys=True)
                    handle.write("\n")
                staging.replace(destination)
            print(f"PREPARED {case['id']}: {destination}")
            prepared += 1
    return prepared


def variables_from_args(args: argparse.Namespace) -> dict[str, str]:
    merlin_root = args.merlin_root or os.environ.get("POMIIGS_MERLIN32_ROOT", "")
    default_merlin = "MacOS/Merlin32" if sys.platform == "darwin" else "Linux/Merlin32"
    merlin32 = args.merlin32 or os.environ.get("POMIIGS_MERLIN32", "")
    if not merlin32 and merlin_root:
        merlin32 = str(Path(merlin_root) / default_merlin)
    cp2 = args.cp2 or os.environ.get("POMIIGS_CP2", "")
    return {
        "root": str(ROOT),
        "build_dir": str(Path(args.build_dir).resolve()),
        "rom01": os.environ.get("POMIIGS_ROM01", str(ROOT / "roms" / "iigs-rom01.rom")),
        "rom03": os.environ.get("POMIIGS_ROM03", str(ROOT / "roms" / "iigs-rom03.rom")),
        "charrom": os.environ.get("POMIIGS_CHAR_ROM", str(ROOT / "roms" / "iigs-char.rom")),
        "tomharte_dir": os.environ.get("POMIIGS_TOMHARTE_DIR", str(ROOT / "tests" / "cycle_accuracy" / "cache" / "tomharte")),
        "assets_dir": str(Path(args.assets_dir).resolve()),
        "out_dir": str(Path(args.out_dir).resolve()),
        "merlin_root": str(Path(merlin_root).resolve()) if merlin_root else "",
        "merlin32": str(Path(merlin32).resolve()) if merlin32 else "",
        "cp2": str(Path(cp2).resolve()) if cp2 else "",
        "cp2_dir": str(Path(cp2).resolve().parent) if cp2 else "",
    }


def variables_for_case(case: dict[str, Any], variables: dict[str, str]) -> dict[str, str]:
    case_variables = dict(variables)
    extraction = next((asset.get("extract") for asset in case["assets"] if asset.get("extract")), None)
    if extraction is not None:
        case_variables["source_dir"] = str(Path(variables["out_dir"]) / extraction["destination"])
    return case_variables


def build_cases(cases: Iterable[dict[str, Any]], variables: dict[str, str]) -> int:
    failures = 0
    for case in cases:
        build = case.get("build")
        if build is None:
            print(f"SKIP {case['id']}: no build adapter")
            continue
        case_variables = variables_for_case(case, variables)
        extraction = next((asset.get("extract") for asset in case["assets"] if asset.get("extract")), None)
        if extraction is None:
            print(f"SKIP {case['id']}: source has not been assigned an extraction recipe")
            continue
        missing = missing_requirements(build["requires"], case_variables)
        if missing:
            print(f"SKIP {case['id']}: missing build prerequisite(s): {', '.join(missing)}")
            continue
        argv = expand_command(build["command"], case_variables)
        print(f"BUILD {case['id']}: {' '.join(argv)}", flush=True)
        result = subprocess.run(argv, cwd=ROOT, check=False)
        if result.returncode == 77:
            print(f"SKIP {case['id']}: builder reported missing external data")
            continue
        if result.returncode:
            print(f"FAIL {case['id']}: build exit={result.returncode}")
            failures += 1
            continue
        outputs = [Path(path) for path in expand_command(build["outputs"], case_variables)]
        missing_outputs = [str(path) for path in outputs if not path.is_file()]
        if missing_outputs:
            print(f"FAIL {case['id']}: missing build output(s): {', '.join(missing_outputs)}")
            failures += 1
            continue
        manifest = {
            "test_id": case["id"],
            "source_marker": json.loads((Path(case_variables["source_dir"]) / ".pomiigs-source.json").read_text(encoding="utf-8")),
            "tools": {
                "merlin32_sha256": hashlib.sha256(Path(case_variables["merlin32"]).read_bytes()).hexdigest(),
                "cp2_sha256": hashlib.sha256(Path(case_variables["cp2"]).read_bytes()).hexdigest(),
            },
            "outputs": {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in outputs},
        }
        manifest_path = Path(case_variables["source_dir"]) / "pomiigs-build-manifest.json"
        with manifest_path.open("w", encoding="utf-8") as handle:
            json.dump(manifest, handle, indent=2, sort_keys=True)
            handle.write("\n")
        print(f"BUILT {case['id']}: {len(outputs)} outputs; manifest={manifest_path}")
    return failures


def run_cases(cases: Iterable[dict[str, Any]], variables: dict[str, str]) -> int:
    failures = 0
    for case in cases:
        if case["readiness"] != "automated":
            print(f"SKIP {case['id']}: readiness={case['readiness']} — {case['oracle']}")
            continue
        missing = missing_requirements(case.get("requires", []), variables)
        if missing:
            print(f"SKIP {case['id']}: missing prerequisite(s): {', '.join(missing)}")
            continue
        argv = expand_command(case["command"], variables)
        print(f"RUN  {case['id']}: {' '.join(argv)}", flush=True)
        result = subprocess.run(argv, cwd=ROOT, check=False)
        if result.returncode == 77:
            print(f"SKIP {case['id']}: command reported missing external data")
            continue
        if result.returncode:
            failures += 1
            print(f"FAIL {case['id']}: exit={result.returncode}")
        else:
            print(f"PASS {case['id']}")
    return failures


def doctor(cases: Iterable[dict[str, Any]], variables: dict[str, str]) -> int:
    build_dir = Path(variables["build_dir"])
    print(f"catalog={DEFAULT_CATALOG}")
    print(f"build_dir={build_dir} ({'present' if build_dir.is_dir() else 'missing'})")
    print(f"rom01={variables['rom01']} ({'present' if Path(variables['rom01']).is_file() else 'missing/user-supplied'})")
    print(f"rom03={variables['rom03']} ({'present' if Path(variables['rom03']).is_file() else 'missing/user-supplied'})")
    print(f"charrom={variables['charrom']} ({'present' if Path(variables['charrom']).is_file() else 'missing/user-supplied'})")
    for case in cases:
        missing_env = [asset["env"] for asset in case["assets"] if "env" in asset and not os.environ.get(asset["env"])]
        notes = [f"missing_env={','.join(missing_env)}"] if missing_env else []
        case_variables = variables_for_case(case, variables)
        for asset in case["assets"]:
            if asset.get("extract"):
                archive = Path(variables["assets_dir"]) / asset["path"]
                source = Path(case_variables["source_dir"])
                notes.append(f"archive={'present' if archive.is_file() else 'missing'}")
                notes.append(f"source={'prepared' if (source / '.pomiigs-source.json').is_file() else 'not-prepared'}")
        if case.get("build"):
            missing_build = missing_requirements(case["build"]["requires"], case_variables)
            notes.append(f"build={'ready' if not missing_build else 'missing:' + ','.join(missing_build)}")
        suffix = f" {' '.join(notes)}" if notes else ""
        print(f"{case['id']}: {case['readiness']}{suffix}")
    return 0


def parser() -> argparse.ArgumentParser:
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--catalog", default=str(DEFAULT_CATALOG))
    common.add_argument("--build-dir", default=str(ROOT / "build"))
    common.add_argument("--assets-dir", default=str(ROOT / "tests" / "cycle_accuracy" / "cache"))
    common.add_argument("--out-dir", default=str(ROOT / "tests" / "cycle_accuracy" / "out"))
    common.add_argument("--merlin-root", default="", help="Merlin32 distribution root (or POMIIGS_MERLIN32_ROOT)")
    common.add_argument("--merlin32", default="", help="Merlin32 executable (or POMIIGS_MERLIN32)")
    common.add_argument("--cp2", default="", help="CiderPress2 cp2 executable (or POMIIGS_CP2)")
    common.add_argument("--id", action="append", default=[])
    common.add_argument("--tier", choices=sorted(VALID_TIERS))

    top = argparse.ArgumentParser(description=__doc__)
    sub = top.add_subparsers(dest="action", required=True)
    sub.add_parser("validate", parents=[common], help="validate catalog schema and policies")
    sub.add_parser("list", parents=[common], help="list catalog entries")
    sub.add_parser("doctor", parents=[common], help="report prerequisites without changing state")
    sub.add_parser("fetch", parents=[common], help="download only explicitly fetchable, checksummed assets")
    sub.add_parser("prepare", parents=[common], help="safely extract pinned source fixtures into the ignored output tree")
    sub.add_parser("build", parents=[common], help="build prepared open fixtures with explicitly supplied tools")
    sub.add_parser("run", parents=[common], help="run automated entries; staged/blocked entries are explicit SKIPs")
    return top


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        catalog = load_catalog(args.catalog)
        cases = selected_tests(catalog, args.id, args.tier)
        variables = variables_from_args(args)
        if args.action == "validate":
            print(f"OK: {len(catalog['tests'])} tests, {len(catalog['sources'])} sources, {len(REQUIRED_DOMAINS)} domains")
            return 0
        if args.action == "list":
            for row in list_rows({**catalog, "tests": cases}):
                print("\t".join(row))
            return 0
        if args.action == "doctor":
            return doctor(cases, variables)
        if args.action == "fetch":
            count = fetch_assets(cases, Path(variables["assets_dir"]))
            print(f"fetched_or_present={count}")
            return 0
        if args.action == "prepare":
            count = prepare_assets(cases, Path(variables["assets_dir"]), Path(variables["out_dir"]))
            print(f"prepared_or_present={count}")
            return 0
        if args.action == "build":
            return 1 if build_cases(cases, variables) else 0
        if args.action == "run":
            return 1 if run_cases(cases, variables) else 0
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
