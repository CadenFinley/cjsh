#!/usr/bin/env python3

# test_build_system.py
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

from __future__ import annotations

import json
import os
import platform
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path
from typing import Mapping


TIMESTAMP_PATTERN = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")

METADATA_DEFINITION_KEYS = (
    "CJSH_GIT_HASH",
    "CJSH_GIT_HASH_FULL",
    "CJSH_BUILD_ARCH",
    "CJSH_BUILD_PLATFORM",
    "CJSH_BUILD_TIME",
    "CJSH_BUILD_COMPILER",
    "CJSH_CXX_STANDARD",
    "CJSH_BUILD_TYPE",
)


def parse_cmake_cache(cache_path: Path) -> dict[str, str]:
    cache_values: dict[str, str] = {}
    for raw_line in cache_path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or line.startswith("//"):
            continue
        if ":" not in line or "=" not in line:
            continue
        key_with_type, value = line.split("=", 1)
        key, _sep, _value_type = key_with_type.partition(":")
        cache_values[key] = value
    return cache_values


def load_compile_commands_entries(build_dir: Path) -> list[dict[str, object]]:
    compile_commands_path = build_dir / "compile_commands.json"
    if not compile_commands_path.exists():
        raise AssertionError(f"Missing compilation database: {compile_commands_path}")

    entries = json.loads(compile_commands_path.read_text(encoding="utf-8"))
    if not isinstance(entries, list) or not entries:
        raise AssertionError("compile_commands.json was empty")
    return [entry for entry in entries if isinstance(entry, dict)]


def parse_compile_arguments(entry: dict[str, object]) -> list[str]:
    command_args = entry.get("arguments")
    if isinstance(command_args, list):
        return [str(arg) for arg in command_args]

    command = entry.get("command")
    if isinstance(command, str):
        return shlex.split(command)

    raise AssertionError("compile_commands entry did not provide command arguments")


def load_cpp_compile_arguments(build_dir: Path) -> list[str]:
    entries = load_compile_commands_entries(build_dir)

    selected_entry: dict[str, object] | None = None
    fallback_entry: dict[str, object] | None = None

    for entry in entries:
        source_file = Path(str(entry.get("file", "")))
        if source_file.suffix not in {".cpp", ".cc", ".cxx"}:
            continue
        if fallback_entry is None:
            fallback_entry = entry
        source_parts = source_file.parts
        if (
            len(source_parts) >= 2
            and source_parts[-2] == "src"
            and source_parts[-1] == "builtin.cpp"
        ):
            selected_entry = entry
            break

    if selected_entry is None:
        selected_entry = fallback_entry
    if selected_entry is None:
        raise AssertionError(
            "No C++ compile command entries found in compile_commands.json"
        )

    return parse_compile_arguments(selected_entry)


def load_compile_arguments_for_source_suffix(
    build_dir: Path,
    source_suffix: str,
) -> list[str]:
    normalized_suffix = source_suffix.replace("\\", "/")
    for entry in load_compile_commands_entries(build_dir):
        source_file = str(entry.get("file", "")).replace("\\", "/")
        if source_file.endswith(normalized_suffix):
            return parse_compile_arguments(entry)

    raise AssertionError(
        "No compile command entry matched source suffix "
        f"'{source_suffix}' in compile_commands.json"
    )


def extract_definitions(args: list[str]) -> dict[str, str]:
    definitions: dict[str, str] = {}
    for arg in args:
        if not arg.startswith("-D"):
            continue
        body = arg[2:]
        if "=" in body:
            key, value = body.split("=", 1)
        else:
            key, value = body, "1"
        if len(value) >= 2 and value[0] == value[-1] and value[0] in {'"', "'"}:
            value = value[1:-1]
        definitions[key] = value
    return definitions


def remove_dirty_suffix(git_hash: str) -> str:
    if git_hash.endswith("-dirty"):
        return git_hash[:-6]
    return git_hash


def expected_platform(system_name: str) -> str:
    if system_name == "Darwin":
        return "apple-darwin"
    if system_name == "Linux":
        return "linux"
    if system_name.lower().startswith("windows"):
        return "windows"
    return "unix"


def expected_arch(system_processor: str, pointer_size: int | None) -> str:
    processor = system_processor.lower()
    if processor in {"aarch64", "arm64"}:
        return "arm64"
    if processor in {"x86_64", "amd64"}:
        return "x86_64"
    if re.match(r"^(i[3-6]86|x86)$", processor):
        return "x86"
    if "ppc" in processor or "power" in processor:
        if pointer_size == 4:
            return "ppc"
        return "ppc64"
    return "unknown"


def create_fake_git_binary(directory: Path) -> Path:
    fake_git_path = directory / "git"
    fake_git_path.write_text(
        """#!/usr/bin/env sh
set -eu

cmd=\"${1:-}\"
arg1=\"${2:-}\"

if [ \"$cmd\" = \"rev-parse\" ] && [ \"$arg1\" = \"--short\" ]; then
    printf '%s\\n' \"${CJSH_FAKE_GIT_SHORT:-fake-short-hash}\"
    exit \"${CJSH_FAKE_GIT_SHORT_EXIT:-0}\"
fi

if [ \"$cmd\" = \"rev-parse\" ] && [ \"$arg1\" = \"HEAD\" ]; then
    printf '%s\\n' \"${CJSH_FAKE_GIT_FULL:-fake-full-hash}\"
    exit \"${CJSH_FAKE_GIT_FULL_EXIT:-0}\"
fi

if [ \"$cmd\" = \"status\" ] && [ \"$arg1\" = \"--porcelain\" ]; then
    printf '%s' \"${CJSH_FAKE_GIT_STATUS:-}\"
    exit \"${CJSH_FAKE_GIT_STATUS_EXIT:-0}\"
fi

exit \"${CJSH_FAKE_GIT_DEFAULT_EXIT:-1}\"
""",
        encoding="utf-8",
    )
    fake_git_path.chmod(0o755)
    return fake_git_path


def configure_cmake(
    source_dir: Path,
    cmake_args: list[str],
    env_overrides: Mapping[str, str] | None = None,
) -> Path:
    build_dir = Path(tempfile.mkdtemp(prefix="cjsh-build-system-test-"))
    command = ["cmake", "-S", str(source_dir), "-B", str(build_dir), *cmake_args]
    env = os.environ.copy()
    if env_overrides:
        env.update(env_overrides)

    completed = subprocess.run(
        command,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if completed.returncode != 0:
        raise AssertionError(
            "CMake configure failed with non-zero exit status\n"
            f"Command: {' '.join(command)}\n"
            f"Output:\n{completed.stdout}"
        )
    return build_dir


class BuildSystemConfigurationTests(unittest.TestCase):
    source_dir: Path = Path()

    @classmethod
    def setUpClass(cls) -> None:
        if shutil.which("cmake") is None:
            raise unittest.SkipTest("cmake is not available in PATH")
        if not (cls.source_dir / "CMakeLists.txt").exists():
            raise unittest.SkipTest(
                f"CMakeLists.txt was not found under {cls.source_dir}"
            )

    def configure_project(
        self,
        cmake_args: list[str],
        env_overrides: Mapping[str, str] | None = None,
    ) -> Path:
        build_dir = configure_cmake(self.source_dir, cmake_args, env_overrides)
        self.addCleanup(shutil.rmtree, build_dir, True)
        return build_dir

    def configure_and_get_definitions(
        self,
        cmake_args: list[str],
        env_overrides: Mapping[str, str] | None = None,
    ) -> tuple[Path, dict[str, str]]:
        build_dir = self.configure_project(cmake_args, env_overrides)
        args = load_cpp_compile_arguments(build_dir)
        return build_dir, extract_definitions(args)

    def build_project(self, build_dir: Path, config: str = "Release") -> None:
        # This build runs inside CTest alongside other suites. A bare --parallel
        # gives Make an unlimited job count and can starve the interactive tests.
        # Keep this count aligned with the CTest PROCESSORS reservation.
        command = ["cmake", "--build", str(build_dir), "--config", config, "--parallel", "2"]
        completed = subprocess.run(
            command,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if completed.returncode != 0:
            raise AssertionError(
                "CMake build failed with non-zero exit status\n"
                f"Command: {' '.join(command)}\n"
                f"Output:\n{completed.stdout}"
            )

    def run_cjsh_command(self, binary: Path, command_text: str) -> str:
        isolated_home = Path(tempfile.mkdtemp(prefix="cjsh-build-metadata-home-"))
        self.addCleanup(shutil.rmtree, isolated_home, True)

        env = os.environ.copy()
        env.update(
            {
                "HOME": str(isolated_home),
                "XDG_CONFIG_HOME": str(isolated_home),
                "CJSH_CI_MODE": "true",
            }
        )

        command = [
            str(binary),
            "--no-source",
            "--no-history",
            "--no-colors",
            "--no-titleline",
            "-c",
            command_text,
        ]
        completed = subprocess.run(
            command,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if completed.returncode != 0:
            raise AssertionError(
                "Running cjsh command failed with non-zero exit status\n"
                f"Command: {' '.join(command)}\n"
                f"Output:\n{completed.stdout}"
            )
        return completed.stdout

    def fake_git_env(
        self, extra_overrides: Mapping[str, str] | None = None
    ) -> dict[str, str]:
        fake_git_dir = Path(tempfile.mkdtemp(prefix="cjsh-fake-git-"))
        self.addCleanup(shutil.rmtree, fake_git_dir, True)
        create_fake_git_binary(fake_git_dir)

        path_value = os.environ.get("PATH", "")
        env: dict[str, str] = {
            "PATH": f"{fake_git_dir}{os.pathsep}{path_value}"
            if path_value
            else str(fake_git_dir)
        }
        if extra_overrides:
            env.update(extra_overrides)
        return env

    def test_metadata_injection_uses_hash_override(self) -> None:
        forced_hash = "meta-test-hash-123"
        _build_dir, definitions = self.configure_and_get_definitions(
            ["-DCMAKE_BUILD_TYPE=Release"],
            env_overrides={"CJSH_GIT_HASH_OVERRIDE": forced_hash},
        )

        for key in METADATA_DEFINITION_KEYS:
            self.assertIn(key, definitions, f"Missing build metadata definition: {key}")

        self.assertEqual(definitions["CJSH_GIT_HASH"], forced_hash)
        self.assertEqual(definitions["CJSH_GIT_HASH_FULL"], forced_hash)
        self.assertEqual(definitions["CJSH_BUILD_TYPE"], "Release")
        self.assertRegex(definitions["CJSH_BUILD_TIME"], TIMESTAMP_PATTERN)

    def test_metadata_definitions_without_override_have_expected_shape(self) -> None:
        _build_dir, definitions = self.configure_and_get_definitions(
            ["-DCMAKE_BUILD_TYPE=Release"]
        )

        for key in METADATA_DEFINITION_KEYS:
            self.assertIn(key, definitions, f"Missing build metadata definition: {key}")
            self.assertNotEqual(
                definitions[key], "", f"Build metadata was empty: {key}"
            )

        short_hash = definitions["CJSH_GIT_HASH"]
        full_hash = definitions["CJSH_GIT_HASH_FULL"]

        if short_hash != "unknown":
            self.assertRegex(short_hash, r"^[0-9a-f]{7,40}(-dirty)?$")
        if full_hash != "unknown":
            self.assertRegex(full_hash, r"^[0-9a-f]{40}(-dirty)?$")
        if short_hash != "unknown" and full_hash != "unknown":
            self.assertTrue(
                remove_dirty_suffix(full_hash).startswith(
                    remove_dirty_suffix(short_hash)
                ),
                "Full hash should include the short hash prefix",
            )

    def test_metadata_build_time_is_recent_utc_timestamp(self) -> None:
        _build_dir, definitions = self.configure_and_get_definitions(
            ["-DCMAKE_BUILD_TYPE=Release"]
        )

        build_time = definitions["CJSH_BUILD_TIME"]
        self.assertRegex(build_time, TIMESTAMP_PATTERN)

        parsed_time = datetime.strptime(build_time, "%Y-%m-%dT%H:%M:%SZ").replace(
            tzinfo=timezone.utc
        )
        age_seconds = abs((datetime.now(timezone.utc) - parsed_time).total_seconds())
        self.assertLess(
            age_seconds,
            24 * 60 * 60,
            f"Build timestamp looked stale: {build_time}",
        )

    def test_metadata_build_type_relwithdebinfo(self) -> None:
        _build_dir, definitions = self.configure_and_get_definitions(
            ["-DCMAKE_BUILD_TYPE=RelWithDebInfo"]
        )
        self.assertEqual(definitions["CJSH_BUILD_TYPE"], "RelWithDebInfo")

    def test_metadata_build_type_minsizerel(self) -> None:
        _build_dir, definitions = self.configure_and_get_definitions(
            ["-DCMAKE_BUILD_TYPE=MinSizeRel"]
        )
        self.assertEqual(definitions["CJSH_BUILD_TYPE"], "MinSizeRel")

    def test_metadata_platform_matches_cmake_system_name(self) -> None:
        _build_dir, definitions = self.configure_and_get_definitions(
            ["-DCMAKE_BUILD_TYPE=Release"]
        )

        system_name = platform.system()
        self.assertEqual(
            definitions["CJSH_BUILD_PLATFORM"], expected_platform(system_name)
        )

    def test_metadata_arch_matches_cmake_system_processor(self) -> None:
        _build_dir, definitions = self.configure_and_get_definitions(
            ["-DCMAKE_BUILD_TYPE=Release"]
        )

        system_processor = platform.machine()
        self.assertNotEqual(system_processor, "")

        expected = expected_arch(system_processor, struct.calcsize("P"))
        self.assertEqual(definitions["CJSH_BUILD_ARCH"], expected)

    @unittest.skipUnless(platform.system() == "Darwin", "Universal2 is macOS-only")
    def test_metadata_arch_reports_universal2_for_fat_macos_build(self) -> None:
        _build_dir, definitions = self.configure_and_get_definitions(
            [
                "-DCMAKE_BUILD_TYPE=Release",
                "-DCMAKE_OSX_ARCHITECTURES=x86_64;arm64",
            ]
        )

        self.assertEqual(definitions["CJSH_BUILD_ARCH"], "universal2")

    def test_metadata_compiler_and_standard_are_populated(self) -> None:
        build_dir, definitions = self.configure_and_get_definitions(
            ["-DCMAKE_BUILD_TYPE=Release"]
        )
        cache = parse_cmake_cache(build_dir / "CMakeCache.txt")

        compiler_string = definitions["CJSH_BUILD_COMPILER"]
        self.assertNotEqual(compiler_string, "unknown")
        self.assertNotEqual(compiler_string.strip(), "")

        compiler_id = cache.get("CMAKE_CXX_COMPILER_ID")
        if compiler_id:
            self.assertIn(compiler_id, compiler_string)

        configured_standard = cache.get("CMAKE_CXX_STANDARD")
        if configured_standard:
            self.assertEqual(definitions["CJSH_CXX_STANDARD"], configured_standard)
        else:
            self.assertEqual(definitions["CJSH_CXX_STANDARD"], "17")

    def test_metadata_propagates_to_test_targets(self) -> None:
        build_dir = self.configure_project(
            [
                "-DCMAKE_BUILD_TYPE=Release",
                "-DCJSH_BUILD_TESTS=ON",
                "-DBUILD_TESTING=ON",
            ]
        )

        main_definitions = extract_definitions(load_cpp_compile_arguments(build_dir))
        completion_args = load_compile_arguments_for_source_suffix(
            build_dir,
            "tests/completions/test_completions.cpp",
        )
        completion_definitions = extract_definitions(completion_args)

        for key in METADATA_DEFINITION_KEYS:
            self.assertIn(key, completion_definitions)
            self.assertEqual(
                completion_definitions[key],
                main_definitions[key],
                f"Metadata key did not propagate to test target: {key}",
            )

    def test_metadata_propagates_to_c_sources(self) -> None:
        build_dir = self.configure_project(
            ["-DCMAKE_BUILD_TYPE=Release"],
            env_overrides={"CJSH_GIT_HASH_OVERRIDE": "c-source-meta-hash"},
        )

        main_definitions = extract_definitions(load_cpp_compile_arguments(build_dir))
        isocline_c_args = load_compile_arguments_for_source_suffix(
            build_dir,
            "cjsh-isocline/src/core/isocline.c",
        )
        isocline_c_definitions = extract_definitions(isocline_c_args)

        for key in METADATA_DEFINITION_KEYS:
            self.assertIn(
                key,
                isocline_c_definitions,
                f"Missing metadata definition for C source: {key}",
            )
            self.assertEqual(
                isocline_c_definitions[key],
                main_definitions[key],
                f"Metadata key did not match between C and C++ targets: {key}",
            )

    def test_metadata_fake_git_dirty_status_appends_dirty_suffix(self) -> None:
        fake_full_hash = "1234567890abcdef1234567890abcdef12345678"
        fake_short_hash = fake_full_hash[:10]
        env = self.fake_git_env(
            {
                "CJSH_FAKE_GIT_SHORT": fake_short_hash,
                "CJSH_FAKE_GIT_FULL": fake_full_hash,
                "CJSH_FAKE_GIT_STATUS": " M src/cjsh.cpp",
            }
        )

        _build_dir, definitions = self.configure_and_get_definitions(
            ["-DCMAKE_BUILD_TYPE=Release"],
            env_overrides=env,
        )

        self.assertEqual(definitions["CJSH_GIT_HASH"], f"{fake_short_hash}-dirty")
        self.assertEqual(definitions["CJSH_GIT_HASH_FULL"], f"{fake_full_hash}-dirty")

    def test_metadata_fake_git_clean_status_has_no_dirty_suffix(self) -> None:
        fake_full_hash = "fedcba9876543210fedcba9876543210fedcba98"
        fake_short_hash = fake_full_hash[:9]
        env = self.fake_git_env(
            {
                "CJSH_FAKE_GIT_SHORT": fake_short_hash,
                "CJSH_FAKE_GIT_FULL": fake_full_hash,
                "CJSH_FAKE_GIT_STATUS": "",
            }
        )

        _build_dir, definitions = self.configure_and_get_definitions(
            ["-DCMAKE_BUILD_TYPE=Release"],
            env_overrides=env,
        )

        self.assertEqual(definitions["CJSH_GIT_HASH"], fake_short_hash)
        self.assertEqual(definitions["CJSH_GIT_HASH_FULL"], fake_full_hash)

    def test_metadata_fake_git_failures_fall_back_to_unknown_hashes(self) -> None:
        env = self.fake_git_env(
            {
                "CJSH_FAKE_GIT_SHORT": "abcdef0",
                "CJSH_FAKE_GIT_FULL": "abcdef0123456789abcdef0123456789abcdef01",
                "CJSH_FAKE_GIT_SHORT_EXIT": "1",
                "CJSH_FAKE_GIT_FULL_EXIT": "1",
                "CJSH_FAKE_GIT_STATUS": "",
            }
        )

        _build_dir, definitions = self.configure_and_get_definitions(
            ["-DCMAKE_BUILD_TYPE=Release"],
            env_overrides=env,
        )

        self.assertEqual(definitions["CJSH_GIT_HASH"], "unknown")
        self.assertEqual(definitions["CJSH_GIT_HASH_FULL"], "unknown")

    def test_metadata_fake_git_short_hash_failure_keeps_full_hash(self) -> None:
        fake_full_hash = "1234567890abcdef1234567890abcdef12345678"
        env = self.fake_git_env(
            {
                "CJSH_FAKE_GIT_SHORT": "abcdef0",
                "CJSH_FAKE_GIT_FULL": fake_full_hash,
                "CJSH_FAKE_GIT_SHORT_EXIT": "1",
                "CJSH_FAKE_GIT_STATUS": "",
            }
        )

        _build_dir, definitions = self.configure_and_get_definitions(
            ["-DCMAKE_BUILD_TYPE=Release"],
            env_overrides=env,
        )

        self.assertEqual(definitions["CJSH_GIT_HASH"], "unknown")
        self.assertEqual(definitions["CJSH_GIT_HASH_FULL"], fake_full_hash)

    def test_metadata_fake_git_full_hash_failure_keeps_short_hash(self) -> None:
        fake_short_hash = "abcdef123"
        env = self.fake_git_env(
            {
                "CJSH_FAKE_GIT_SHORT": fake_short_hash,
                "CJSH_FAKE_GIT_FULL": "1234567890abcdef1234567890abcdef12345678",
                "CJSH_FAKE_GIT_FULL_EXIT": "1",
                "CJSH_FAKE_GIT_STATUS": "",
            }
        )

        _build_dir, definitions = self.configure_and_get_definitions(
            ["-DCMAKE_BUILD_TYPE=Release"],
            env_overrides=env,
        )

        self.assertEqual(definitions["CJSH_GIT_HASH"], fake_short_hash)
        self.assertEqual(definitions["CJSH_GIT_HASH_FULL"], "unknown")

    def test_metadata_hash_override_takes_priority_over_git_status(self) -> None:
        forced_hash = "priority-override-hash"
        env = self.fake_git_env(
            {
                "CJSH_FAKE_GIT_SHORT": "aaaaaaaa",
                "CJSH_FAKE_GIT_FULL": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                "CJSH_FAKE_GIT_STATUS": " M src/cjsh.cpp",
                "CJSH_GIT_HASH_OVERRIDE": forced_hash,
            }
        )

        _build_dir, definitions = self.configure_and_get_definitions(
            ["-DCMAKE_BUILD_TYPE=Release"],
            env_overrides=env,
        )

        self.assertEqual(definitions["CJSH_GIT_HASH"], forced_hash)
        self.assertEqual(definitions["CJSH_GIT_HASH_FULL"], forced_hash)

    def test_runtime_version_fields_match_compiled_metadata(self) -> None:
        forced_hash = "runtime-meta-hash-42"
        build_dir, definitions = self.configure_and_get_definitions(
            ["-DCMAKE_BUILD_TYPE=Release"],
            env_overrides={"CJSH_GIT_HASH_OVERRIDE": forced_hash},
        )
        self.build_project(build_dir, config="Release")

        cjsh_binary = build_dir / "cjsh"
        self.assertTrue(cjsh_binary.exists(), f"Missing cjsh binary at {cjsh_binary}")

        summary_output = self.run_cjsh_command(cjsh_binary, "version")
        summary_lines = [
            line.strip() for line in summary_output.splitlines() if line.strip()
        ]
        self.assertGreaterEqual(len(summary_lines), 1)

        summary_header = summary_lines[0]
        match = re.match(
            r"^cjsh v.+ \(git ([^)]+)\) \(([^-]+)-([^)]+)\)$", summary_header
        )
        self.assertIsNotNone(match, f"Unexpected version output: {summary_header}")
        assert match is not None
        self.assertEqual(match.group(1), definitions["CJSH_GIT_HASH"])
        self.assertEqual(match.group(2), definitions["CJSH_BUILD_ARCH"])
        self.assertEqual(match.group(3), definitions["CJSH_BUILD_PLATFORM"])

        all_output = self.run_cjsh_command(cjsh_binary, "version -a")
        self.assertIn(f"Build time: {definitions['CJSH_BUILD_TIME']}", all_output)
        self.assertIn(f"Git hash: {definitions['CJSH_GIT_HASH_FULL']}", all_output)
        self.assertIn(f"Compiler: {definitions['CJSH_BUILD_COMPILER']}", all_output)
        self.assertIn(
            f"C++ standard: C++{definitions['CJSH_CXX_STANDARD']}", all_output
        )
        self.assertIn(f"Build type: {definitions['CJSH_BUILD_TYPE']}", all_output)

    def test_edge_case_default_build_type_and_strip_env_override(self) -> None:
        build_dir = self.configure_project([], env_overrides={"CJSH_STRIP_BINARY": "0"})
        cache = parse_cmake_cache(build_dir / "CMakeCache.txt")

        self.assertEqual(cache.get("CJSH_STRIP_BINARY"), "OFF")

        configuration_types = cache.get("CMAKE_CONFIGURATION_TYPES", "")
        if not configuration_types:
            self.assertEqual(cache.get("CMAKE_BUILD_TYPE"), "Release")
            args = load_cpp_compile_arguments(build_dir)
            self.assertIn("-DNDEBUG", args)

    def test_edge_case_cjsh_build_tests_forces_build_testing(self) -> None:
        build_dir = self.configure_project(
            [
                "-DCMAKE_BUILD_TYPE=Release",
                "-DCJSH_BUILD_TESTS=ON",
                "-DBUILD_TESTING=OFF",
            ],
        )
        cache = parse_cmake_cache(build_dir / "CMakeCache.txt")

        self.assertEqual(cache.get("CJSH_BUILD_TESTS"), "ON")
        self.assertEqual(cache.get("BUILD_TESTING"), "ON")

    def test_release_preset_settings(self) -> None:
        build_dir = self.configure_project(["-DCMAKE_BUILD_TYPE=Release"])
        args = load_cpp_compile_arguments(build_dir)
        definitions = extract_definitions(args)

        self.assertIn("-O3", args)
        self.assertIn("-DNDEBUG", args)
        self.assertNotIn("-O0", args)
        self.assertNotIn("-O2", args)
        self.assertNotIn("-Oz", args)
        self.assertIn("IC_NO_DEBUG_MSG", definitions)
        self.assertNotIn("DEBUG", definitions)
        self.assertNotIn("CJSH_MINIMAL_BUILD", definitions)
        self.assertEqual(definitions.get("_FORTIFY_SOURCE"), "2")
        self.assertNotIn("-U_FORTIFY_SOURCE", args)

    def test_debug_preset_settings(self) -> None:
        build_dir = self.configure_project(["-DCMAKE_BUILD_TYPE=Debug"])
        args = load_cpp_compile_arguments(build_dir)
        definitions = extract_definitions(args)

        self.assertIn("-O0", args)
        self.assertIn("-g", args)
        self.assertIn("-fsanitize=address", args)
        self.assertNotIn("-O3", args)
        self.assertIn("CJSH_ENABLE_DEBUG", definitions)
        self.assertNotIn("DEBUG", definitions)
        self.assertNotIn("IC_NO_DEBUG_MSG", definitions)
        self.assertNotIn("NDEBUG", definitions)
        self.assertNotIn("_FORTIFY_SOURCE", definitions)
        self.assertNotIn("-U_FORTIFY_SOURCE", args)

    def test_minimal_preset_settings(self) -> None:
        build_dir = self.configure_project(
            ["-DCMAKE_BUILD_TYPE=Release", "-DCJSH_MINIMAL_BUILD=ON"],
        )
        args = load_cpp_compile_arguments(build_dir)
        definitions = extract_definitions(args)

        self.assertIn("-Oz", args)
        self.assertNotIn("-O2", args)
        self.assertIn("-DNDEBUG", args)
        self.assertNotIn("CJSH_MINIMAL_BUILD", definitions)
        self.assertNotIn("CJSH_NO_FANCY_FEATURES", definitions)
        self.assertEqual(definitions.get("IC_NO_DEBUG_MSG"), "1")


def resolve_source_dir(argv: list[str]) -> Path:
    if len(argv) >= 2:
        return Path(argv[1]).resolve()
    return Path(__file__).resolve().parents[2]


def main(argv: list[str]) -> int:
    source_dir = resolve_source_dir(argv)
    BuildSystemConfigurationTests.source_dir = source_dir

    test_suite = unittest.defaultTestLoader.loadTestsFromTestCase(
        BuildSystemConfigurationTests
    )
    result = unittest.TextTestRunner(verbosity=2).run(test_suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
