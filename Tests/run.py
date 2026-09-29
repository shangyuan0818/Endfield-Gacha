#!/usr/bin/env python3
"""Compile the production statistics core without Win32/GDI+ dependencies."""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def extract_core(source: Path) -> str:
    text = source.read_text(encoding="utf-8-sig")
    excluded_headers = {
        "windows.h", "commctrl.h", "richedit.h", "gdiplus.h", "process.h"
    }
    includes = []
    for line in text.splitlines():
        if line.startswith("#include <"):
            header = line.split("<", 1)[1].split(">", 1)[0]
            if header not in excluded_headers:
                includes.append(line)

    # Deliberately fail if a source reorganization makes a boundary ambiguous.
    # The generated header contains verbatim production code, not a test copy.
    sections = [
        ("enum class ItemType", "// 无堆分配的大小写不敏感包含比较"),
        ("struct StringHash {", "inline std::string WideToUtf8"),
        ("struct PullBucket {", "StatsResult statsChar,"),
        ("constexpr double kBaseRate6", "struct FileGuard {"),
    ]
    chunks = ["#pragma once\n", "\n".join(includes), "\n"]
    for start, end in sections:
        if text.count(start) != 1 or text.count(end) != 1:
            raise ValueError(f"Production extraction boundary changed: {start!r}, {end!r}")
        first, last = text.index(start), text.index(end)
        if last <= first:
            raise ValueError(f"Production extraction boundaries reversed: {start!r}")
        line = text.count("\n", 0, first) + 1
        filename = source.as_posix().replace('"', '\\"')
        chunks.append(f'\n#line {line} "{filename}"\n')
        chunks.append(text[first:last])
    return "".join(chunks)


def main() -> None:
    tests = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=tests.parent / "gui.cpp")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--no-sanitize", action="store_true",
                        help="Disable UBSan if the compiler/runtime does not support it")
    args = parser.parse_args()
    source = args.source.resolve()
    with tempfile.TemporaryDirectory(prefix="endfield-chart-tests-") as tmp:
        build = Path(tmp)
        (build / "gui_core.hpp").write_text(extract_core(source), encoding="utf-8")
        binary = build / ("chart_stats_tests.exe" if os.name == "nt" else "chart_stats_tests")
        command = shlex.split(args.cxx) + [
            "-std=c++20", "-Wall", "-Wextra", "-Wpedantic", "-O1", "-g",
            "-I", str(build), str(tests / "chart_stats_tests.cpp"), "-o", str(binary)
        ]
        if not args.no_sanitize:
            command += ["-fsanitize=undefined", "-fno-sanitize-recover=undefined"]
        print(f"Testing production core from {source}", flush=True)
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
