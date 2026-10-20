#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build a UDrive x64 test installer on Windows with the pinned Craft dependencies."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TARGET = "windows-cl-msvc2022-x86_64"
PACKAGE = "owncloud/owncloud-client"
CRAFTMASTER_REVISION = "8a19d8fcab223b8b27126d98a46ed824b5948665"
WINDOWS_CRAFT_OVERRIDE = """[GeneralSettings]
Paths/Python = ${Variables:PythonExecutable}
Packager/Destination = ${Variables:PackageOutput}
Packager/CreateCache = False
Packager/PackageType = NullsoftInstallerPackager
CodeSigning/Enabled = False
# Craft resolves shared settings even when signing is disabled.
# Windows builds must not require the publisher's Apple signing environment.
CodeSigning/MacDeveloperId =
CodeSigning/MacKeychainPath =
ContinuousIntegration/UpdateRepository = False
Compile/BuildType = RelWithDebInfo
"""


class BuildError(Exception):
    pass


def run(*args, env=None, capture=False, timeout=None):
    print("+ " + subprocess.list2cmdline([str(a) for a in args]), flush=True)
    result = subprocess.run([str(a) for a in args], env=env, check=True, text=True,
                            stdout=subprocess.PIPE if capture else None, timeout=timeout)
    return result.stdout or ""


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def pe_machine(path):
    with path.open("rb") as source:
        header = source.read(64)
        if len(header) != 64 or header[:2] != b"MZ":
            raise BuildError(f"Not a Windows executable: {path}")
        offset = struct.unpack_from("<I", header, 60)[0]
        if offset < 64 or offset > path.stat().st_size - 6:
            raise BuildError(f"Invalid PE header: {path}")
        source.seek(offset)
        signature = source.read(6)
        if signature[:4] != b"PE\0\0":
            raise BuildError(f"Missing PE signature: {path}")
        return struct.unpack_from("<H", signature, 4)[0]


def version():
    source = (ROOT / "VERSION.cmake").read_text()
    parts = []
    for component in ("MAJOR", "MINOR", "PATCH"):
        match = re.search(rf"set\(\s*MIRALL_VERSION_{component}\s+(\d+)\s*\)", source)
        if not match:
            raise BuildError("Could not read application version from VERSION.cmake.")
        parts.append(match[1])
    return ".".join(parts)


def brand_blueprint(path):
    """Adapt the pinned upstream package in the isolated Craft build directory."""
    source = path.read_text()
    changes = {
        'self.description = "ownCloud Desktop Client"': 'self.description = "UDrive Desktop Client"',
        'self.displayName = "ownCloud"': 'self.displayName = "UDrive"',
        'self.webpage = "https://github.com/owncloud/client"': 'self.webpage = "https://drive.unity-workspace.com"',
        'self.defines["company"] = "ownCloud GmbH"': 'self.defines["company"] = "Unity Workspace"\n        self.defines["license"] = self.sourceDir() / "COPYING"',
        'self.buildDir() / "src/gui/owncloud.ico"': 'self.buildDir() / "src/gui/udrive.ico"',
    }
    for old, new in changes.items():
        if old not in source and new in source:
            continue
        if source.count(old) != 1:
            raise BuildError("The pinned Craft blueprint has changed; stopped before packaging another product.")
        source = source.replace(old, new)
    compile(source, str(path), "exec")
    path.write_text(source)


def preserve_nsis_install_directory(path):
    """Keep /D's destination when the isolated Craft installer's MultiUser starts."""
    source = path.read_text()
    original = 'Function .onInit\n    !insertmacro MULTIUSER_INIT'
    replacement = '''Var UDriveRequestedInstallDir

Function .onInit
    ; NSIS initializes $INSTDIR from /D before .onInit. MultiUser resets it.
    StrCpy $UDriveRequestedInstallDir $INSTDIR
    !insertmacro MULTIUSER_INIT
    ${If} $UDriveRequestedInstallDir != ""
        StrCpy $INSTDIR $UDriveRequestedInstallDir
    ${EndIf}'''
    if replacement in source:
        return
    if source.count(original) != 1:
        raise BuildError("The Craft NSIS initialization has changed; cannot preserve the requested install folder.")
    path.write_text(source.replace(original, replacement))


def smoke_test(installer, prefix, work_root, expected_version):
    import winreg

    # Never replace an existing local installation while checking a build.
    for key in (r"Software\Unity Workspace\UDrive", r"Software\Microsoft\Windows\CurrentVersion\Uninstall\UDrive"):
        for hive in (winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE):
            try:
                with winreg.OpenKey(hive, key):
                    raise BuildError("UDrive is already installed. Use a clean Windows VM or CI runner for --smoke-test.")
            except FileNotFoundError:
                pass
    hidden_prefix = prefix.with_name(prefix.name + ".smoke-isolated")
    if hidden_prefix.exists():
        raise BuildError(f"Restore the interrupted test's Craft prefix first: {hidden_prefix}")
    with tempfile.TemporaryDirectory(prefix="udrive-install-", dir=work_root) as temporary:
        installed = Path(temporary) / "app"
        # NSIS requires /D last and unquoted, including when its path contains spaces.
        command = subprocess.list2cmdline([str(installer), "/S", "/CurrentUser"]) + " /D=" + str(installed)
        print("+ " + command, flush=True)
        subprocess.run(command, check=True, timeout=180)
        try:
            executable = installed / "bin/udrive.exe"
            if not executable.is_file():
                files = sorted(str(path.relative_to(installed)) for path in installed.rglob("*") if path.is_file())
                raise BuildError(f"Installer exited without creating {executable}. "
                                 f"Files in the requested install folder: {files[:40]}")
            if pe_machine(executable) != 0x8664:
                raise BuildError("The installed UDrive application is not x64.")
            for name in ("Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "vcruntime140.dll", "msvcp140.dll"):
                if not (executable.parent / name).is_file():
                    raise BuildError(f"Missing installed runtime dependency: {name}")
            if not (executable.parent / "platforms/qwindows.dll").is_file():
                raise BuildError("Missing installed Qt Windows platform plugin.")
            env = os.environ.copy()
            for key in list(env):
                if key.upper().startswith(("QT_", "QML", "CRAFT")):
                    del env[key]
            env["PATH"] = os.pathsep.join([str(Path(env["SystemRoot"]) / "System32"), env["SystemRoot"]])
            # Prove that the installed app cannot load missing files from Craft.
            prefix.rename(hidden_prefix)
            try:
                output = run(executable, "--version", env=env, capture=True, timeout=30)
            finally:
                hidden_prefix.rename(prefix)
            if "UDrive" not in output or expected_version not in output:
                raise BuildError("The installed application reported an unexpected version or brand.")
            print(output, flush=True)
        finally:
            uninstaller = installed / "uninstall.exe"
            if uninstaller.is_file():
                # _?= keeps NSIS in this process so cleanup waits for uninstall.
                command = subprocess.list2cmdline([str(uninstaller), "/S", "/CurrentUser"]) + " _?=" + str(installed)
                print("+ " + command, flush=True)
                subprocess.run(command, check=True, timeout=180)


def build(args):
    if sys.platform != "win32":
        raise BuildError("This build needs Windows x64 (a Windows PC, VM, or GitHub Actions Windows runner). This Mac has only Mac build dependencies.")
    if sys.version_info < (3, 12):
        raise BuildError("Python 3.12 or later is required.")
    if not shutil.which("git"):
        raise BuildError("Install Git for Windows first.")
    work_root, output = args.work_root.resolve(), args.output_dir.resolve()
    if work_root == ROOT or ROOT.is_relative_to(work_root):
        raise BuildError("Use a separate Windows build directory, not the source root or its parent.")
    if output.exists():
        raise BuildError(f"Output already exists; choose a new --output-dir: {output}")
    work_root.mkdir(parents=True, exist_ok=True)
    master = work_root / "CraftMaster"
    if not master.exists():
        run("git", "clone", "--no-checkout", "https://invent.kde.org/kde/craftmaster.git", master)
        run("git", "-C", master, "checkout", "--detach", CRAFTMASTER_REVISION)
    actual_revision = run("git", "-C", master, "rev-parse", "HEAD", capture=True).strip()
    if actual_revision != CRAFTMASTER_REVISION:
        raise BuildError("Unexpected CraftMaster revision. Use a new --work-root for the pinned Windows build.")
    packages = Path(tempfile.mkdtemp(prefix="windows-packages-", dir=work_root))
    override = work_root / "udrive-windows.ini"
    override.write_text(WINDOWS_CRAFT_OVERRIDE)
    env = os.environ.copy()
    env.update(APPLICATION_EXECUTABLE="udrive", APPLICATION_SHORTNAME="UDrive", SIGN_PACKAGE="False")
    base = [sys.executable, str(master / "CraftMaster.py"), "--config", str(ROOT / ".craft.ini"),
            "--config-override", str(ROOT / ".github/workflows/craft_override.ini"), "--config-override", str(override),
            "--variables", f"Root={work_root}", f"WORKSPACE={work_root}", f"PythonExecutable={sys.executable}",
            f"PackageOutput={packages}", "CiBuild=False", "CreateCache=False", "--targets", TARGET]

    def craft(*commands):
        run(*base, *commands, env=env)

    craft("--setup")
    craft("-c", "--unshelve", ROOT / ".craft.shelf")
    craft("-c", "--set", f"srcDir={ROOT}", PACKAGE)
    craft("-c", "--set", "args=-DBUILD_TESTING=OFF -DBUILD_SHELL_INTEGRATION=OFF -DWITH_AUTO_UPDATER=OFF", PACKAGE)
    craft("-c", "dev-utils/nsis")
    craft("-c", "--install-deps", PACKAGE)
    preserve_nsis_install_directory(work_root / "craft-clone/bin/Packager/Nsis/NullsoftInstaller.nsi")
    brand_blueprint(work_root / "blueprints/craft-blueprints-owncloud/owncloud/owncloud-client/owncloud-client.py")
    craft("-c", "--no-cache", PACKAGE)
    prefix = work_root / TARGET
    if pe_machine(prefix / "bin/udrive.exe") != 0x8664:
        raise BuildError("The compiled UDrive application is not Windows x64.")
    craft("-c", "--no-cache", "--package", PACKAGE)
    installers = list(packages.glob("*.exe"))
    if len(installers) != 1 or pe_machine(installers[0]) not in (0x014C, 0x8664):
        raise BuildError("Expected exactly one valid Windows NSIS installer.")
    app_version = version()
    if args.smoke_test:
        smoke_test(installers[0], prefix, work_root, app_version)
    output.mkdir(parents=True)
    filename = f"UDrive-{app_version}-Windows-x64-TESTING-ONLY.exe"
    destination = output / filename
    shutil.copy2(installers[0], destination)
    metadata = {"schema_version": 1, "platform": "windows", "channel": "testing", "version": app_version,
                "architecture": "x86_64", "minimum_windows": "10.1809", "filename": filename,
                "size_bytes": destination.stat().st_size, "sha256": sha256(destination), "signature": "unsigned",
                "installed_smoke_test": args.smoke_test, "source_commit": run("git", "-C", ROOT, "rev-parse", "HEAD", capture=True).strip(),
                "source_dirty": bool(run("git", "-C", ROOT, "status", "--porcelain", capture=True).strip())}
    (output / "download.json").write_text(json.dumps(metadata, indent=2) + "\n")
    (output / "SHA256SUMS").write_text(f'{metadata["sha256"]}  {filename}\n')
    (output / "TESTING-ONLY.txt").write_text("Unsigned UDrive Windows x64 test installer. Not a signed production release.\n")
    print(f"Windows test installer: {destination}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-root", type=Path, default=ROOT / "build-windows")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build-release-artifacts/windows" / datetime.now(timezone.utc).strftime("run-%Y%m%d-%H%M%S"))
    parser.add_argument("--smoke-test", action="store_true", help="Install and test on a clean Windows machine, then uninstall")
    args = parser.parse_args(argv)
    try:
        build(args)
    except (BuildError, OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired, ValueError) as error:
        print(f"Windows build stopped: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
