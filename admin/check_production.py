#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run local release checks without signing, publishing, or accessing live accounts."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
REQUIRED_TESTS = {
    "UDriveFileBrowserTest",
    "UDriveAdminTest",
    "UDriveWorkspaceActionsTest",
    "SyncEngineTest",
    "OAuthTest",
    "LocalDevelopmentUrlReleaseTest",
}


class CheckError(RuntimeError):
    pass


def read_cache(build_dir, source=ROOT):
    cache_path = build_dir / "CMakeCache.txt"
    if not cache_path.is_file():
        raise CheckError(f"Configure a test build first: {cache_path} is missing.")
    cache = {}
    for line in cache_path.read_text(encoding="utf-8").splitlines():
        if line.startswith(("#", "//")) or "=" not in line:
            continue
        key, value = line.split("=", 1)
        cache[key.split(":", 1)[0]] = value
    if cache.get("BUILD_TESTING", "").upper() not in {"ON", "TRUE", "YES", "1"}:
        raise CheckError("BUILD_TESTING must be ON; a build without tests cannot pass this check.")
    home = cache.get("CMAKE_HOME_DIRECTORY")
    if not home or Path(home).resolve() != source.resolve():
        raise CheckError("The build directory belongs to another source checkout.")
    return cache


def resolve_tool(override, cached, name):
    candidate = override or cached or shutil.which(name)
    resolved = shutil.which(str(candidate)) if candidate else None
    if not resolved:
        raise CheckError(f"Cannot find {name}. Supply --{name} with its executable path.")
    return resolved


def run(command, **kwargs):
    print("Running: " + " ".join(map(str, command)), flush=True)
    return subprocess.run(command, cwd=ROOT, check=True, **kwargs)


def verify_test_plan(output):
    try:
        plan = json.loads(output)
        tests = {entry["name"] for entry in plan["tests"]}
        disabled = {
            entry["name"] for entry in plan["tests"]
            if any(prop.get("name") == "DISABLED" and prop.get("value")
                   for prop in entry.get("properties", []))
        }
    except (ValueError, KeyError, TypeError) as error:
        raise CheckError("CTest did not return a valid test inventory.") from error
    missing = REQUIRED_TESTS - tests
    if missing:
        raise CheckError("Required tests are not registered: " + ", ".join(sorted(missing)))
    if disabled:
        raise CheckError("Disabled tests cannot pass a production check: " + ", ".join(sorted(disabled)))
    return len(tests)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--build-dir", type=Path, help="Existing CMake build configured with BUILD_TESTING=ON")
    mode.add_argument("--policies-only", action="store_true", help="Check packaging policy only; does not verify the application")
    parser.add_argument("--cmake", help="CMake executable; defaults to the build cache or PATH")
    parser.add_argument("--ctest", help="CTest executable; defaults to the build cache or PATH")
    parser.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 1), help="Parallel build jobs (tests run serially)")
    args = parser.parse_args(argv)
    if args.jobs < 1:
        parser.error("--jobs must be positive")

    try:
        cache = read_cache(args.build_dir.resolve()) if args.build_dir else None
        cmake = resolve_tool(args.cmake, cache.get("CMAKE_COMMAND"), "cmake") if cache is not None else None
        ctest = resolve_tool(args.ctest, cache.get("CMAKE_CTEST_COMMAND"), "ctest") if cache is not None else None
        run([sys.executable, "-m", "unittest", "discover", "-s", "test", "-p", "test_*release.py", "-v"])
        run([sys.executable, "-m", "unittest", "discover", "-s", "test", "-p", "test_production_checks.py", "-v"])
        if args.policies_only:
            print("Packaging/check-runner policies passed. Application build and tests were not run.")
            return 0

        build_dir = str(args.build_dir.resolve())
        run(["git", "diff", "--check"])
        run([cmake, "--build", build_dir, "--parallel", str(args.jobs)])
        inventory = run([ctest, "--test-dir", build_dir, "--show-only=json-v1"], capture_output=True, text=True)
        count = verify_test_plan(inventory.stdout)
        print(f"Verified test inventory: {count} registered tests.", flush=True)
        run([ctest, "--test-dir", build_dir, "--output-on-failure", "--no-tests=error", "--parallel", "1", "--timeout", "600"])
        print("Local production checks passed. Platform acceptance, signing, and release verification are still required.")
        return 0
    except (CheckError, OSError, subprocess.CalledProcessError) as error:
        print(f"Production check failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
