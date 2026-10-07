#!/usr/bin/env python3

# lint.py
#
# This file is part of cjsh, CJ's Shell
#
# MIT License
#
# Copyright (c) 2026 Caden Finley
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

"""Check C/C++ formatting and static analysis without modifying source files."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOTS = ("cjsh-core", "cjsh-isocline", "tests")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx"}
HEADER_SUFFIXES = {".h", ".hh", ".hpp", ".hxx"}
# These implementation fragments require their owner's private types and macros.
# Analyze them through that translation unit, not as independent C programs.
INCLUDED_SOURCES = {
    "cjsh-isocline/src/edit/editline_help.c": "cjsh-isocline/src/edit/editline.c",
    "cjsh-isocline/src/edit/editline_menu.c": "cjsh-isocline/src/edit/editline.c",
    "cjsh-isocline/src/edit/editline_history.c": "cjsh-isocline/src/edit/editline.c",
    "cjsh-isocline/src/edit/editline_command_palette.c": "cjsh-isocline/src/edit/editline.c",
    "cjsh-isocline/src/edit/editline_custom_menu.c": "cjsh-isocline/src/edit/editline.c",
    "cjsh-isocline/src/edit/editline_completion.c": "cjsh-isocline/src/edit/editline.c",
    "cjsh-isocline/src/terminal/bbcode_colors.c": "cjsh-isocline/src/terminal/bbcode.c",
    "cjsh-isocline/src/terminal/term_color.c": "cjsh-isocline/src/terminal/term.c",
}


def discover_files(root: Path) -> list[Path]:
    return sorted(
        path.resolve()
        for directory in SOURCE_ROOTS
        for path in (root / directory).rglob("*")
        if path.is_file() and path.suffix in SOURCE_SUFFIXES | HEADER_SUFFIXES
    )


def load_database(build_dir: Path, files: list[Path]) -> dict[Path, dict]:
    database = build_dir / "compile_commands.json"
    if not database.is_file():
        raise ValueError(
            f"Missing {database}; run cmake --preset release first "
            "(CJSH_BUILD_TESTS and CJSH_GENERATE_COMPILE_COMMANDS must be ON)."
        )
    entries = json.loads(database.read_text(encoding="utf-8"))
    if not isinstance(entries, list) or not entries:
        raise ValueError(f"Empty or invalid compilation database: {database}")
    wanted = set(files)
    commands = {}
    for entry in entries:
        directory = Path(entry["directory"])
        if not directory.is_absolute():
            directory = build_dir / directory
        directory = directory.resolve()
        source = (directory / entry["file"]).resolve()
        if source not in wanted:
            continue
        arguments = entry.get("arguments")
        if arguments is None:
            arguments = shlex.split(entry["command"])
        if not isinstance(arguments, list) or not arguments or not all(
            isinstance(arg, str) for arg in arguments
        ):
            raise ValueError(f"Invalid compile arguments for {source}")
        normalized = {
            "directory": str(directory),
            "file": str(source),
            "arguments": list(arguments),
        }
        if source in commands and commands[source] != normalized:
            raise ValueError(f"Multiple compile commands for {source}; use a single-config build.")
        commands[source] = normalized
    return commands


def verify_source_coverage(root: Path, files: list[Path], commands: dict[Path, dict]) -> None:
    missing = []
    for source in files:
        if source.suffix not in SOURCE_SUFFIXES or source in commands:
            continue
        owner_name = INCLUDED_SOURCES.get(source.relative_to(root).as_posix())
        owner = (root / owner_name).resolve() if owner_name else None
        if owner in commands:
            includes = re.findall(r'^\s*#\s*include\s+"([^"]+)"', owner.read_text(), re.MULTILINE)
            if any((owner.parent / name).resolve() == source for name in includes):
                continue
        missing.append(str(source.relative_to(root)))
    if missing:
        raise ValueError(
            "Sources missing from compilation database (reconfigure with tests enabled):\n  "
            + "\n  ".join(missing)
        )


def header_command(header: Path, commands: dict[Path, dict], root: Path) -> dict:
    # C headers use the isocline C build; core and test headers use C++.
    use_c = header.is_relative_to(root / "cjsh-isocline")
    candidates = [
        source for source in commands
        if (source.suffix == ".c") == use_c and source.suffix in SOURCE_SUFFIXES
    ]
    if not candidates:
        raise ValueError(f"No {'C' if use_c else 'C++'} compile command for {header}")
    source = max(
        sorted(candidates),
        key=lambda path: (
            path.with_suffix("") == header.with_suffix(""),
            len(Path(os.path.commonpath([path.parent, header.parent])).parts),
        ),
    )
    entry = commands[source]
    arguments = [entry["arguments"][0]]
    skip_next = False
    for arg in entry["arguments"][1:]:
        if skip_next:
            skip_next = False
            continue
        if arg in {"-o", "-MF", "-MT", "-MQ", "-x"}:
            skip_next = True
        elif arg in {"-c", "-MD", "-MMD", "-MP"}:
            continue
        elif not arg.startswith("-") and (Path(entry["directory"]) / arg).resolve() == source:
            continue
        else:
            arguments.append(arg)
    arguments.extend(["-x", "c-header" if use_c else "c++-header", str(header)])
    return {"directory": entry["directory"], "file": str(header), "arguments": arguments}


def add_macos_sdk(commands: dict[Path, dict]) -> None:
    if platform.system() != "Darwin":
        return
    result = subprocess.run(
        ["xcrun", "--sdk", "macosx", "--show-sdk-path"],
        check=True, capture_output=True, text=True, timeout=30,
    )
    default_sdk = result.stdout.strip()
    for source, entry in commands.items():
        args = entry["arguments"]
        sdk = default_sdk
        for index, arg in enumerate(args):
            if arg in {"-isysroot", "--sysroot"}:
                sdk = args[index + 1]
                break
            if arg.startswith("--sysroot="):
                sdk = arg.split("=", 1)[1]
                break
        else:
            args.extend(["-isysroot", sdk])
        # Homebrew LLVM does not always discover Apple's libc++ headers when
        # CMake's compiler is AppleClang. Keep the configured SDK authoritative.
        cxx = source.suffix in {".cc", ".cpp", ".cxx"} or "c++-header" in args
        if cxx:
            include_dir = Path(sdk) / "usr/include/c++/v1"
            if not include_dir.is_dir():
                raise ValueError(f"Cannot find libc++ headers in SDK: {include_dir}")
            args.extend(["-isystem", str(include_dir)])


def check_one(command: list[str], timeout: int) -> tuple[bool, str]:
    try:
        result = subprocess.run(
            command, cwd=ROOT, capture_output=True, text=True, errors="replace", timeout=timeout,
        )
        return result.returncode == 0, result.stdout + result.stderr
    except (OSError, subprocess.TimeoutExpired) as error:
        return False, str(error)


def run_checks(label: str, tasks: list[tuple[Path, list[str]]], jobs: int, timeout: int) -> bool:
    failures = 0
    print(f"{label}: checking {len(tasks)} files with {jobs} workers", flush=True)
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        pending = {pool.submit(check_one, command, timeout): path for path, command in tasks}
        for index, future in enumerate(as_completed(pending), 1):
            passed, output = future.result()
            if not passed:
                failures += 1
                print(f"\nFAIL {label}: {pending[future].relative_to(ROOT)}\n{output}", flush=True)
            if index % 50 == 0:
                print(f"{label}: {index}/{len(tasks)} complete", flush=True)
    print(f"{label}: {len(tasks)} checked, {failures} failed", flush=True)
    return failures == 0


def positive_int(value: str) -> int:
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return number


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/release")
    parser.add_argument("--jobs", type=positive_int, default=min(4, os.cpu_count() or 1))
    parser.add_argument("--timeout", type=positive_int, default=300, help="seconds per file")
    parser.add_argument("--clang-format", default="clang-format")
    parser.add_argument("--clang-tidy", default="clang-tidy")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--format-only", action="store_true")
    mode.add_argument("--tidy-only", action="store_true")
    args = parser.parse_args(argv)
    try:
        files = discover_files(ROOT)
        if not files:
            raise ValueError("No C/C++ files found")
        tools = {}
        for name, executable, enabled in (
            ("format", args.clang_format, not args.tidy_only),
            ("tidy", args.clang_tidy, not args.format_only),
        ):
            if enabled:
                tools[name] = shutil.which(executable)
                if tools[name] is None:
                    raise ValueError(f"Cannot find {executable}; install LLVM or use --clang-{name}.")
        commands = {}
        if "tidy" in tools:
            commands = load_database(args.build_dir.resolve(), files)
            verify_source_coverage(ROOT, files, commands)
            headers = {
                header: header_command(header, commands, ROOT)
                for header in files if header.suffix in HEADER_SUFFIXES
            }
            commands.update(headers)
            add_macos_sdk(commands)
        passed = True
        if "format" in tools:
            tasks = [
                (path, [tools["format"], "--dry-run", "--Werror", "--style=file", str(path)])
                for path in files
            ]
            passed = run_checks("clang-format", tasks, args.jobs, args.timeout)
        if "tidy" in tools:
            # Do not modify the real compilation database. Headers need their own
            # language/flags; included .c fragments are checked through owners.
            with tempfile.TemporaryDirectory(prefix="cjsh-lint-") as temporary:
                database = Path(temporary) / "compile_commands.json"
                database.write_text(json.dumps(list(commands.values())), encoding="utf-8")
                tasks = [
                    (path, [tools["tidy"], "-p", temporary, "--warnings-as-errors=*",
                            f"--config-file={ROOT / '.clang-tidy'}", str(path)])
                    for path in sorted(commands)
                ]
                passed = run_checks("clang-tidy", tasks, args.jobs, args.timeout) and passed
        return 0 if passed else 1
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print(f"lint: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
