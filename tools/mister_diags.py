#!/usr/bin/env python3
"""Boot the MiSTer Apple IIgs custom diagnostics under POMIIGS and compare
their on-screen verdicts with tests/cycle_accuracy/mister_goldens.json.

    tools/mister_diags.py [--build-dir build] [--out-dir tests/cycle_accuracy/out]
                          [--goldens tests/cycle_accuracy/mister_goldens.json]
                          [--id ID ...] [--verbose]

Exit 0: every `expect: pass` entry passed (xfails are reported, an unexpected
PASS is flagged so the golden can be promoted); 1: a regression (an expected
pass failed); 77: the disks are not built (run `tools/cycle_suite.py build`).
A machine-readable report is written next to the disks
(mister-results.json) with the raw text page of every run.
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def run_one(diag: Path, rom: Path, disks: Path, entry: dict, defaults: dict, verbose: bool) -> dict:
    frames = entry.get("frames", defaults.get("frames", 600))
    boot = entry.get("boot", defaults.get("boot", "disk35"))
    disk = disks / entry["disk"]
    argv = [str(diag), str(rom), "--frames", str(frames)]
    argv += ["--hdd" if boot == "hdd" else "--disk35", str(disk)]
    scratch = []
    for flag, name in entry.get("media", {}).items():
        # Writable media are copied first so the pristine cp2 output survives.
        src = disks / name
        run = disks / (src.stem + ".run" + src.suffix)
        shutil.copyfile(src, run)
        scratch.append(run)
        argv += [f"--{flag}", str(run)]
    if entry.get("iwm35"): argv.append("--iwm35")
    if entry.get("writeback"): argv.append("--writeback")
    proc = subprocess.run(argv, capture_output=True, text=True)
    rows = [m.group(1) for m in re.finditer(r"^\[text \d\d\] \|(.*)\|$", proc.stdout, re.M)]
    text = "\n".join(r.rstrip() for r in rows)
    if verbose:
        print("   " + " ".join(argv))
        for r in rows:
            if r.strip(): print("   |" + r + "|")
    result = {"id": entry["id"], "argv": argv, "exit": proc.returncode, "rows": rows}
    if proc.returncode == 77:
        result["verdict"] = "SKIP"; return result
    passed = None
    if "pass_regex" in entry:
        passed = re.search(entry["pass_regex"], text, re.M) is not None
    if "fail_regex" in entry:
        failed = re.search(entry["fail_regex"], text, re.M) is not None
        passed = (passed if passed is not None else True) and not failed
    if "subtests_regex" in entry:
        sub = {m.group(1): m.group(2) for m in re.finditer(entry["subtests_regex"], text)}
        result["subtests"] = sub
        failing = sorted(k for k, v in sub.items() if v != "P")
        result["failing_subtests"] = failing
        if entry.get("expect") == "xfail":
            expected = set(entry.get("xfail_subtests", []))
            result["subtest_regressions"] = sorted(set(failing) - expected)
            result["subtest_promotions"] = sorted(expected - set(failing))
    result["verdict"] = "PASS" if passed else "FAIL"
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", default=str(ROOT / "build"))
    ap.add_argument("--out-dir", default=str(ROOT / "tests" / "cycle_accuracy" / "out"))
    ap.add_argument("--goldens", default=str(ROOT / "tests" / "cycle_accuracy" / "mister_goldens.json"))
    ap.add_argument("--id", action="append", default=[])
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    goldens = json.loads(Path(args.goldens).read_text(encoding="utf-8"))
    diag = Path(args.build_dir) / "diag_trace"
    rom = ROOT / goldens["rom"]
    disks = Path(args.out_dir) / "mister-custom-diagnostics" / "customtests"
    if not diag.is_file():
        print(f"[mister] {diag} not built — SKIP"); return 77
    if not rom.is_file():
        print(f"[mister] ROM {rom} missing — SKIP"); return 77
    entries = [e for e in goldens["tests"] if not args.id or e["id"] in args.id]
    optional_missing = [e["id"] for e in entries if e.get("optional") and not (disks / e["disk"]).is_file()]
    for i in optional_missing: print(f"  {i:18s} SKIP     (optional disk not built)")
    entries = [e for e in entries if e["id"] not in optional_missing]
    missing = [e["disk"] for e in entries if not (disks / e["disk"]).is_file()]
    if missing:
        print(f"[mister] disks not built ({', '.join(missing[:3])}…) — run tools/cycle_suite.py build — SKIP"); return 77

    results, regressions, promotions = [], [], []
    for entry in entries:
        r = run_one(diag, rom, disks, entry, goldens.get("defaults", {}), args.verbose)
        expect = entry.get("expect", "pass")
        status = r["verdict"]
        if status == "PASS" and expect == "xfail": tag = "XPASS"; promotions.append(entry["id"])
        elif status == "FAIL" and expect == "xfail": tag = "XFAIL"
        elif status == "FAIL": tag = "FAIL"; regressions.append(entry["id"])
        else: tag = status
        if r.get("subtest_regressions"): tag += " (new failing subtests: " + ",".join(r["subtest_regressions"]) + ")"; regressions.append(entry["id"])
        if r.get("subtest_promotions"): tag += " (now passing: " + ",".join(r["subtest_promotions"]) + ")"
        detail = ""
        if "failing_subtests" in r: detail = f" subtests failing: {','.join(r['failing_subtests']) or 'none'}"
        print(f"  {entry['id']:18s} {tag:8s}{detail}" + (f"  — {entry['note']}" if tag.startswith("XFAIL") and entry.get("note") else ""))
        r["expect"] = expect; r["tag"] = tag
        results.append(r)

    report = disks / "mister-results.json"
    report.write_text(json.dumps({"rom": str(rom), "results": results}, indent=2) + "\n", encoding="utf-8")
    n_pass = sum(r["tag"] == "PASS" for r in results); n_x = sum(r["tag"].startswith("XFAIL") for r in results)
    print(f"[mister] {n_pass} pass, {n_x} xfail, {len(promotions)} xpass, {len(regressions)} regression(s); report={report}")
    if promotions: print(f"[mister] promote to pass: {', '.join(promotions)}")
    return 1 if regressions else 0


if __name__ == "__main__":
    sys.exit(main())
