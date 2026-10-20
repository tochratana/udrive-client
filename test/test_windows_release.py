# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise Windows packaging policy without executing Windows code on the host."""

import argparse
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("udrive_windows", Path(__file__).resolve().parents[1] / "admin/windows/build.py")
windows = importlib.util.module_from_spec(spec)
spec.loader.exec_module(windows)

BLUEPRINT = '''
class Subinfo:
    def targets(self):
        self.description = "ownCloud Desktop Client"
        self.displayName = "ownCloud"
        self.webpage = "https://github.com/owncloud/client"
class Package:
    def package(self):
        self.defines["company"] = "ownCloud GmbH"
        self.defines["icon"] = self.buildDir() / "src/gui/owncloud.ico"
'''


class TestWindowsRelease(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def executable(self, path, machine=0x8664):
        path.parent.mkdir(parents=True, exist_ok=True)
        content = bytearray(256)
        content[:2] = b"MZ"
        struct.pack_into("<I", content, 60, 128)
        content[128:132] = b"PE\0\0"
        struct.pack_into("<H", content, 132, machine)
        path.write_bytes(content)
        return path

    def fixture(self, machine=0x8664):
        work = self.root / "craft"
        (work / "CraftMaster").mkdir(parents=True)
        blueprint = work / "blueprints/craft-blueprints-owncloud/owncloud/owncloud-client/owncloud-client.py"
        blueprint.parent.mkdir(parents=True)
        blueprint.write_text(BLUEPRINT)
        self.executable(work / windows.TARGET / "bin/udrive.exe", machine)
        return argparse.Namespace(work_root=work, output_dir=self.root / "output", smoke_test=False)

    def fake_build(self, calls, extra_installer=False):
        def run(*args, **kwargs):
            strings = [str(a) for a in args]
            calls.append(strings)
            if strings[0] == "git":
                if "CraftMaster" in strings[2]:
                    return windows.CRAFTMASTER_REVISION + "\n"
                if "rev-parse" in strings:
                    return "f" * 40 + "\n"
            if "--package" in strings:
                output = Path(next(a.split("=", 1)[1] for a in strings if a.startswith("PackageOutput=")))
                self.executable(output / "upstream-installer.exe", 0x014C)
                if extra_installer:
                    self.executable(output / "stale-installer.exe", 0x014C)
            return ""
        return run

    def test_pe_reader_distinguishes_app_and_nsis_stub(self):
        self.assertEqual(windows.pe_machine(self.executable(self.root / "app.exe")), 0x8664)
        self.assertEqual(windows.pe_machine(self.executable(self.root / "setup.exe", 0x014C)), 0x014C)

    def test_mac_binary_or_invalid_pe_header_is_rejected(self):
        path = self.root / "fake.exe"
        for content in (bytes.fromhex("cffaedfe"), b"MZ" + b"\0" * 62):
            path.write_bytes(content)
            with self.subTest(content=content[:4]), self.assertRaises(windows.BuildError):
                windows.pe_machine(path)

    def test_blueprint_uses_udrive_icon_publisher_and_shortcut_name(self):
        path = self.root / "package.py"
        path.write_text(BLUEPRINT)
        windows.brand_blueprint(path)
        result = path.read_text()
        self.assertIn('self.displayName = "UDrive"', result)
        self.assertIn('"Unity Workspace"', result)
        self.assertIn('"src/gui/udrive.ico"', result)
        self.assertIn('self.sourceDir() / "COPYING"', result)
        windows.brand_blueprint(path)
        self.assertEqual(path.read_text(), result)

    def test_unknown_blueprint_is_unchanged_on_failure(self):
        path = self.root / "unknown.py"
        content = BLUEPRINT.replace('"ownCloud GmbH"', '"Different publisher"')
        path.write_text(content)
        with self.assertRaises(windows.BuildError):
            windows.brand_blueprint(path)
        self.assertEqual(path.read_text(), content)

    def test_non_windows_host_stops_before_downloads_or_output(self):
        args = argparse.Namespace(work_root=self.root / "craft", output_dir=self.root / "output", smoke_test=False)
        with patch.object(windows.sys, "platform", "darwin"), patch.object(windows, "run") as runner:
            with self.assertRaises(windows.BuildError):
                windows.build(args)
        runner.assert_not_called()
        self.assertFalse(args.work_root.exists())
        self.assertFalse(args.output_dir.exists())

    def test_test_installer_metadata_does_not_claim_signing_or_install_validation(self):
        args = self.fixture()
        calls = []
        with patch.object(windows.sys, "platform", "win32"), patch.object(windows.shutil, "which", return_value="git"), \
             patch.object(windows, "run", side_effect=self.fake_build(calls)):
            windows.build(args)
        metadata = json.loads((args.output_dir / "download.json").read_text())
        self.assertEqual(metadata["channel"], "testing")
        self.assertEqual(metadata["signature"], "unsigned")
        self.assertFalse(metadata["installed_smoke_test"])
        self.assertEqual(metadata["architecture"], "x86_64")
        self.assertEqual(metadata["sha256"], windows.sha256(args.output_dir / metadata["filename"]))
        self.assertTrue((args.output_dir / "TESTING-ONLY.txt").is_file())
        self.assertTrue(any("--install-deps" in call for call in calls))
        self.assertTrue(any("--package" in call for call in calls))

    def test_wrong_payload_architecture_never_produces_a_download(self):
        args = self.fixture(machine=0x014C)
        calls = []
        with patch.object(windows.sys, "platform", "win32"), patch.object(windows.shutil, "which", return_value="git"), \
             patch.object(windows, "run", side_effect=self.fake_build(calls)), self.assertRaises(windows.BuildError):
            windows.build(args)
        self.assertFalse(any("--package" in call for call in calls))
        self.assertFalse(args.output_dir.exists())

    def test_ambiguous_installer_outputs_are_not_published(self):
        args = self.fixture()
        calls = []
        with patch.object(windows.sys, "platform", "win32"), patch.object(windows.shutil, "which", return_value="git"), \
             patch.object(windows, "run", side_effect=self.fake_build(calls, extra_installer=True)), self.assertRaises(windows.BuildError):
            windows.build(args)
        self.assertFalse(args.output_dir.exists())


if __name__ == "__main__":
    unittest.main()
