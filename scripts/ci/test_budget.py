#!/usr/bin/env python3
"""Check ctest durations and fresh line and branch coverage.

Run --record before changing tests. Its .info.sha256.json sidecar binds coverage
paths to source contents. Compare runs must select the same tests as the record.
Recording keeps artifacts even when a test exceeds its time budget.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from contextlib import redirect_stdout
from dataclasses import dataclass, field
from io import StringIO
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SECONDS = 60.0
SOURCE_DIRS = ("src", "include")


@dataclass
class Coverage:
    lines: set[int] = field(default_factory=set)
    branches: set[tuple[int, str, str]] = field(default_factory=set)


def seconds(text: str) -> float:
    value = float(text)
    if not math.isfinite(value) or value < 0:
        raise ValueError(f"invalid duration: {text!r}")
    return value


def read_budgets(path: Path) -> dict[str, float]:
    result = {}
    for number, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.partition("#")[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) != 2:
            raise ValueError(f"{path}:{number}: expected test-name seconds")
        name, duration = parts
        if name in result:
            raise ValueError(f"{path}:{number}: duplicate test {name}")
        result[name] = seconds(duration)
        if result[name] > DEFAULT_SECONDS:
            raise ValueError(f"{path}:{number}: budgets cannot exceed {DEFAULT_SECONDS} s")
    return result


def check_junit(path: Path, budgets: dict[str, float]) -> tuple[list[str], int]:
    cases = list(ET.parse(path).getroot().iter("testcase"))
    if not cases:
        raise ValueError(f"{path}: no tests ran")
    problems = []
    skipped = 0
    for case in cases:
        name = case.attrib["name"]
        elapsed = seconds(case.attrib["time"])
        budget = budgets.get(name, DEFAULT_SECONDS)
        print(f"{name}: {elapsed:.3f} s (budget {budget:g} s)")
        if elapsed > budget:
            problems.append(f"OVER BUDGET: {name}: {elapsed:g} > {budget:g} s")
        if case.find("failure") is not None or case.find("error") is not None:
            problems.append(f"FAILED: {name}")
        if case.find("skipped") is not None or case.get("status") in ("notrun", "disabled"):
            skipped += 1
            problems.append(f"NOT RUN: {name}")
    print(f"tests: {len(cases)}, skipped: {skipped}")
    return problems, len(cases)


def source_name(source: str, root: Path) -> str | None:
    path = Path(source)
    if path.is_absolute():
        try:
            path = path.relative_to(root)
        except ValueError:
            return None
    if ".." in path.parts or not path.parts or path.parts[0] not in SOURCE_DIRS:
        return None
    return path.as_posix()


def read_lcov(path: Path, root: Path) -> dict[str, Coverage]:
    result: dict[str, Coverage] = {}
    current = None
    inside = False
    for raw in path.read_text().splitlines():
        if raw == "end_of_record":
            if not inside:
                raise ValueError(f"{path}: end_of_record without SF")
            current, inside = None, False
            continue
        tag, _, value = raw.partition(":")
        if tag == "SF":
            if inside:
                raise ValueError(f"{path}: missing end_of_record")
            name = source_name(value, root)
            current = result.setdefault(name, Coverage()) if name else None
            inside = True
        elif tag in ("DA", "BRDA"):
            if not inside:
                raise ValueError(f"{path}: {tag} outside SF")
            if current is None:
                continue
            parts = value.split(",")
            if tag == "DA":
                line, hits = int(parts[0]), int(parts[1])
                if hits < 0 or line < 0:
                    raise ValueError(f"{path}: invalid DA: {value}")
                if hits:
                    current.lines.add(line)
            else:
                line, block, branch, taken = parts
                if taken != "-" and int(taken) < 0:
                    raise ValueError(f"{path}: invalid BRDA: {value}")
                if taken != "-" and int(taken) > 0:
                    current.branches.add((int(line), block, branch))
    if inside:
        raise ValueError(f"{path}: missing end_of_record")
    if not result or not any(c.lines for c in result.values()):
        raise ValueError(f"{path}: no covered sources under src/ or include/")
    return result


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_hashes(root: Path) -> dict[str, str]:
    return {
        path.relative_to(root).as_posix(): sha256(path)
        for directory in SOURCE_DIRS
        for path in sorted((root / directory).rglob("*"))
        if path.is_file()
    }


def sidecar(path: Path) -> Path:
    return path.with_suffix(path.suffix + ".sha256.json")


def write_baseline(path: Path, trace: Path, hashes: dict[str, str], root: Path, regex: str) -> None:
    covered = read_lcov(trace, root)
    metadata = {
        "root": str(root),
        "tests": regex,
        "sources": {name: hashes[name] for name in covered},
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(trace, path)
    sidecar(path).write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")


def compare(
    baseline: dict[str, Coverage], current: dict[str, Coverage], hashes: dict[str, str], root: Path
) -> list[str]:
    problems = []
    for name, before in sorted(baseline.items()):
        recorded = hashes.get(name)
        if not isinstance(recorded, str) or re.fullmatch(r"[0-9a-f]{64}", recorded) is None:
            raise ValueError(f"{name}: baseline has no valid source SHA-256")
        source = root / name
        if not source.is_file() or sha256(source) != recorded:
            print(f"NOT COMPARED (source changed): {name}")
            continue
        after = current.get(name, Coverage())
        lines = sorted(before.lines - after.lines)
        branches = sorted(before.branches - after.branches)
        if lines or branches:
            problems.append(f"LOST COVERAGE: {name}")
            if lines:
                problems.append("  lines: " + ", ".join(map(str, lines)))
            if branches:
                problems.append(
                    "  branches (line:block:branch): "
                    + ", ".join(":".join(map(str, b)) for b in branches)
                )
    return problems


def gcov_command(build: Path) -> str:
    cache = (build / "CMakeCache.txt").read_text()
    if "DFTRACER_UTILS_COVERAGE:BOOL=ON" not in cache:
        raise ValueError(f"{build}: configure with the coverage preset first")
    match = re.search(r"^CMAKE_CXX_COMPILER:[^=]+=([^\n]+)$", cache, re.MULTILINE)
    if match is None:
        raise ValueError(f"{build}: no C++ compiler in CMakeCache.txt")
    compiler = match[1]
    version = subprocess.check_output([compiler, "--version"], text=True)
    if "clang" in version.lower():
        return "xcrun llvm-cov gcov" if sys.platform == "darwin" else "llvm-cov gcov"
    major = re.search(r"\b(\d+)\.\d+\.\d+", version)
    if major and shutil.which(f"gcov-{major[1]}"):
        return f"gcov-{major[1]}"
    return "gcov"


def capture(build: Path, root: Path, output: Path, gcov: str) -> None:
    # Fresh counters omit obsolete objects left by renamed targets.
    counters = sorted(build.rglob("*.gcda"))
    if not counters:
        raise ValueError(f"{build}: no fresh coverage counters")
    subprocess.run(
        [
            "uv",
            "tool",
            "run",
            "--from",
            "gcovr",
            "gcovr",
            "--root",
            str(root),
            "--gcov-executable",
            gcov,
            # A full run hits hot inline lines more than 2^32 times, which gcovr
            # rejects as the gcov bug 68080; the counts are real, so keep them.
            "--gcov-ignore-parse-errors",
            "suspicious_hits.warn_once_per_file",
            "--filter",
            str(root / "src") + "/",
            "--filter",
            str(root / "include") + "/",
            "--lcov",
            str(output),
            *map(str, counters),
        ],
        cwd=root,
        check=True,
    )
    read_lcov(output, root)


def run(args: argparse.Namespace) -> int:
    build = args.build.resolve()
    if not (build / "CMakeCache.txt").is_file():
        raise ValueError(f"{build}: not a configured build directory")
    budgets = read_budgets(ROOT / "tests/test_budget.txt")
    metadata = None
    if args.baseline:
        metadata = json.loads(sidecar(args.baseline).read_text())
        if metadata["tests"] != args.tests:
            raise ValueError(
                "baseline test selection differs from --tests; record a matching baseline"
            )
        baseline = read_lcov(args.baseline, Path(metadata["root"]))
    gcov = gcov_command(build) if args.record or args.baseline else None
    if gcov:
        for path in build.rglob("*.gcda"):
            path.unlink()
    hashes = source_hashes(ROOT) if gcov else {}
    with tempfile.TemporaryDirectory(prefix="test-budget-") as temporary:
        directory = Path(temporary)
        junit = directory / "tests.xml"
        env = dict(os.environ, CTEST_PARALLEL_LEVEL="1", DFTRACER_UTILS_ELASTIC="0")
        status = subprocess.run(
            [
                "ctest",
                "--test-dir",
                str(build),
                "-R",
                args.tests,
                "-j",
                "1",
                "--output-on-failure",
                "--output-junit",
                str(junit),
            ],
            env=env,
            check=False,
        ).returncode
        problems, count = check_junit(junit, budgets)
        if args.record:
            args.record.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(junit, args.record.with_suffix(".junit.xml"))
        if status:
            problems.append(f"ctest exited {status}")
        failed = status or any(p.startswith(("FAILED:", "NOT RUN:")) for p in problems)
        if gcov and not failed:
            trace = directory / "run.info"
            capture(build, ROOT, trace, gcov)
            after = source_hashes(ROOT)
            changed = [name for name in hashes if hashes[name] != after.get(name)]
            if changed:
                raise ValueError("sources changed during the run: " + ", ".join(changed))
            if args.record:
                write_baseline(args.record.resolve(), trace, hashes, ROOT, args.tests)
                print(f"RECORDED: {args.record}")
            if metadata is not None:
                problems.extend(
                    compare(baseline, read_lcov(trace, ROOT), metadata["sources"], ROOT)
                )
        for problem in problems:
            print(problem)
        print(f"{'FAIL' if problems else 'PASS'}: {count} tests")
        return int(bool(problems))


class SelfTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / "src").mkdir()
        self.source = self.root / "src/sample.cpp"
        self.source.write_text("int value;\n")
        self.hashes = {"src/sample.cpp": sha256(self.source)}
        self.before = self.root / "before.info"
        self.after = self.root / "after.info"
        self.before.write_text("SF:src/sample.cpp\nDA:1,1\nDA:2,1\nBRDA:1,0,0,1\nend_of_record\n")
        self.after.write_text("SF:src/sample.cpp\nDA:1,1\nDA:3,1\nBRDA:1,0,1,1\nend_of_record\n")

    def differences(self) -> list[str]:
        return compare(
            read_lcov(self.before, self.root),
            read_lcov(self.after, self.root),
            self.hashes,
            self.root,
        )

    def test_lost_line_and_branch(self) -> None:
        lost = self.differences()
        self.assertTrue(any("lines: 2" in p for p in lost))
        self.assertTrue(any("1:0:0" in p for p in lost))

    def test_unchanged(self) -> None:
        self.after.write_text(self.before.read_text())
        self.assertEqual(self.differences(), [])

    def test_changed_source(self) -> None:
        self.source.write_text("int changed;\n")
        out = StringIO()
        with redirect_stdout(out):
            self.assertEqual(self.differences(), [])
        self.assertIn("NOT COMPARED", out.getvalue())

    def test_deleted_source(self) -> None:
        self.source.unlink()
        with redirect_stdout(StringIO()):
            self.assertEqual(self.differences(), [])

    def test_missing_hash(self) -> None:
        self.hashes.clear()
        with self.assertRaises(ValueError):
            self.differences()

    def test_missing_current_file(self) -> None:
        lost = compare(read_lcov(self.before, self.root), {}, self.hashes, self.root)
        self.assertTrue(any("lines: 1, 2" in p for p in lost))

    def test_duplicate_lcov_records(self) -> None:
        self.after.write_text(self.after.read_text() + self.before.read_text())
        self.assertEqual(self.differences(), [])

    def test_absolute_source_paths(self) -> None:
        self.before.write_text(self.before.read_text().replace("src/sample.cpp", str(self.source)))
        self.assertIn("src/sample.cpp", read_lcov(self.before, self.root))

    def test_record_round_trip(self) -> None:
        recorded = self.root / "record.info"
        write_baseline(recorded, self.before, self.hashes, self.root, ".")
        doc = json.loads(sidecar(recorded).read_text())
        self.assertEqual(doc["sources"], self.hashes)
        self.assertEqual(
            compare(
                read_lcov(recorded, self.root),
                read_lcov(self.before, self.root),
                doc["sources"],
                self.root,
            ),
            [],
        )

    def junit(self, cases: str) -> tuple[list[str], int]:
        path = self.root / "junit.xml"
        path.write_text("<testsuite>" + cases + "</testsuite>")
        with redirect_stdout(StringIO()):
            return check_junit(path, {"custom": 10})

    def test_over_budget(self) -> None:
        problems, count = self.junit('<testcase name="custom" time="10.001"/>')
        self.assertEqual(count, 1)
        self.assertTrue(problems[0].startswith("OVER BUDGET"))

    def test_exact_and_default_budgets(self) -> None:
        self.assertEqual(
            self.junit('<testcase name="custom" time="10"/><testcase name="other" time="60"/>')[0],
            [],
        )
        self.assertTrue(self.junit('<testcase name="other" time="60.001"/>')[0])

    def test_failures_and_skips(self) -> None:
        problems, _ = self.junit(
            '<testcase name="failed" time="1"><failure/></testcase><testcase name="skip" time="0"><skipped/></testcase>'
        )
        self.assertEqual(len(problems), 2)

    def test_empty_junit(self) -> None:
        with self.assertRaises(ValueError):
            self.junit("")

    def test_invalid_times(self) -> None:
        for value in ("nan", "inf", "-1", "bad"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.junit(f'<testcase name="bad" time="{value}"/>')

    def test_invalid_budgets(self) -> None:
        path = self.root / "budgets.txt"
        for value in ("name 61", "name nan", "name -1", "name 1\nname 2", "name"):
            path.write_text(value)
            with self.subTest(value=value), self.assertRaises(ValueError):
                read_budgets(path)
        path.write_text("# comment\nname 10 # seconds\n")
        self.assertEqual(read_budgets(path), {"name": 10})

    def test_empty_lcov(self) -> None:
        self.after.write_text("")
        with self.assertRaises(ValueError):
            read_lcov(self.after, self.root)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--self-test", action="store_true", help="run fixture-based checks without a build"
    )
    parser.add_argument("--build", type=Path, help="coverage build directory")
    parser.add_argument(
        "--tests", help="ctest regular expression; must match the baseline selection"
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--record", type=Path, help="write baseline LCOV, source hashes and test timings"
    )
    mode.add_argument("--baseline", type=Path, help="fail on lost covered lines or branches")
    args = parser.parse_args()
    if args.self_test:
        if args.build or args.tests or args.record or args.baseline:
            parser.error("--self-test cannot be combined with a test run")
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(SelfTest)
        return int(not unittest.TextTestRunner().run(suite).wasSuccessful())
    if not args.build or not args.tests:
        parser.error("provide --build and --tests, or use --self-test")
    try:
        return run(args)
    except (OSError, ValueError, KeyError, ET.ParseError, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
