#!/usr/bin/env python3
"""Generate and enforce ggui's strict production-source line coverage report."""

from __future__ import annotations

import argparse
import collections
import gzip
import json
import pathlib
import subprocess
import tempfile


SOURCES = {
    "Application.cpp": "CMakeFiles/ggui.dir/Source/Application.cpp.gcno",
    "Core.cpp": "CMakeFiles/ggui_core.dir/Source/Core.cpp.gcno",
    "Graph.cpp": "CMakeFiles/ggui_core.dir/Source/Graph.cpp.gcno",
    "Main.cpp": "CMakeFiles/ggui.dir/Source/Main.cpp.gcno",
}
STRUCTURAL_LINES = {"", "{", "}", "};", "},"}


def exclusions(lines: list[str]) -> set[int]:
    excluded: set[int] = set()
    in_region = False
    for number, line in enumerate(lines, 1):
        if "GCOV_EXCL_START" in line:
            in_region = True
        if in_region or "GCOV_EXCL_LINE" in line:
            excluded.add(number)
        if "GCOV_EXCL_STOP" in line:
            in_region = False
        code = line.split("//", 1)[0].strip()
        if code in STRUCTURAL_LINES:
            excluded.add(number)
    return excluded


def gcov_record(build: pathlib.Path, source: pathlib.Path, object_path: str, output: pathlib.Path) -> dict:
    gcno = (build / object_path).resolve()
    if not gcno.is_file():
        raise RuntimeError(f"coverage object is missing: {gcno}")
    subprocess.run(
        ["gcov", "--json-format", "--branch-probabilities", "--branch-counts", str(gcno)],
        cwd=output,
        check=True,
        stdout=subprocess.DEVNULL,
    )
    archive = output / f"{source.name}.gcov.json.gz"
    with gzip.open(archive, "rt", encoding="utf-8") as stream:
        report = json.load(stream)
    wanted = source.resolve()
    return next(record for record in report["files"] if pathlib.Path(record["file"]).resolve() == wanted)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("build", nargs="?", default="build/coverage", type=pathlib.Path)
    arguments = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    build = arguments.build.resolve()
    total_hit = total_coverable = total_excluded = 0
    failed = False

    with tempfile.TemporaryDirectory(prefix="ggui-gcov-") as temporary:
        output = pathlib.Path(temporary)
        for name, object_path in SOURCES.items():
            source = root / "Source" / name
            source_lines = source.read_text(encoding="utf-8").splitlines()
            record = gcov_record(build, source, object_path, output)
            counts: collections.defaultdict[int, int] = collections.defaultdict(int)
            for line in record["lines"]:
                counts[line["line_number"]] += line["count"]

            excluded = exclusions(source_lines) & counts.keys()
            coverable = sorted(counts.keys() - excluded)
            missed = [number for number in coverable if counts[number] == 0]
            hit = len(coverable) - len(missed)
            percent = 100.0 if not coverable else hit * 100.0 / len(coverable)
            print(f"{name}: {hit}/{len(coverable)} lines ({percent:.2f}%), {len(excluded)} explicit/structural exclusions")
            for number in missed:
                print(f"  MISS {number}: {source_lines[number - 1].strip()}")
            total_hit += hit
            total_coverable += len(coverable)
            total_excluded += len(excluded)
            failed |= bool(missed)

    percent = 100.0 if total_coverable == 0 else total_hit * 100.0 / total_coverable
    print(f"TOTAL: {total_hit}/{total_coverable} production lines ({percent:.2f}%), {total_excluded} exclusions")
    if failed:
        print("Coverage gate failed: every non-excluded production line must execute.")
        return 1
    print("Coverage gate passed: 100.00% line coverage.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
