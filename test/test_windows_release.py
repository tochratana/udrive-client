# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise Windows packaging policy without executing Windows code on the host."""

import argparse
import configparser
from contextlib import ExitStack, redirect_stderr, redirect_stdout
import importlib.util
import io
import json
from pathlib import Path
import shutil
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

spec = importlib.util.spec_from_file_location("udrive_windows", Path(__file__).resolve().parents[1] / "admin/windows/build.py")
windows = importlib.util.module_from_spec(spec)
spec.loader.exec_module(windows)

BLUEPRINT = '''
class Subinfo:
    def targets(self):
        self.description = "ownCloud Desktop Client"
        self.displayName = "ownCloud"
        self.webpage = "https://github.com/owncloud/client"
        self.runtimeDependencies["libs/qt/qtdeclarative"] = None
class Package:
    def package(self):
        self.defines["company"] = "ownCloud GmbH"
        self.defines["icon"] = self.buildDir() / "src/gui/owncloud.ico"
'''
NSIS_TEMPLATE = '''Function .onInit
    !insertmacro MULTIUSER_INIT
    !if "@{architecture}" == "x86_64"
        ; Existing architecture validation remains here.
    !endif
FunctionEnd
'''
QT_VERSION_INFO = '''[General]
include = version_upstream.ini
tarballs = 6.5.2
branches = 6.8;dev
defaulttarget = 6.10.1
'''
QT_UPSTREAM_INFO = '''[General]
tarballs = 6.8.3;6.10.1
tarballUrl = https://download.qt.io/official_releases/qt/${VERSION_MAJOR}.${VERSION_MINOR}/${VERSION}/submodules/${PACKAGE_NAME}-everywhere-src-${VERSION}.tar.xz
tarballDigestUrl = https://download.qt.io/official_releases/qt/${VERSION_MAJOR}.${VERSION_MINOR}/${VERSION}/submodules/${PACKAGE_NAME}-everywhere-src-${VERSION}.tar.xz.sha256
tarballInstallSrc = ${PACKAGE_NAME}-everywhere-src-${VERSION}
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
        nsis = work / "craft-clone/bin/Packager/Nsis/NullsoftInstaller.nsi"
        nsis.parent.mkdir(parents=True)
        nsis.write_text(NSIS_TEMPLATE)
        self.qt_fixture(work)
        self.executable(work / windows.TARGET / "bin/udrive.exe", machine)
        return argparse.Namespace(work_root=work, output_dir=self.root / "output", smoke_test=False)

    def qt_fixture(self, work=None):
        qt_root = (work or self.root) / "blueprints/craft-blueprints-kde/libs/qt6"
        qt_root.mkdir(parents=True)
        (qt_root / "version.ini").write_text(QT_VERSION_INFO)
        (qt_root / "version_upstream.ini").write_text(QT_UPSTREAM_INFO)
        return qt_root

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

    def smoke_fixture(self, missing_runtime=False, cleanup_fails=False, startup_fails=False):
        work = self.root / 'smoke work'
        prefix = work / windows.TARGET
        prefix.mkdir(parents=True)
        installer = self.executable(work / 'installer.exe', 0x014C)
        commands = []
        stdout, stderr = io.StringIO(), io.StringIO()

        def process(command, **kwargs):
            commands.append(command)
            if ' /D=' in command:
                installed = Path(command.split(' /D=', 1)[1])
                self.executable(installed / 'bin/udrive.exe')
                for name in ('Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'vcruntime140.dll', 'msvcp140.dll'):
                    if missing_runtime and name == 'Qt6Core.dll':
                        continue
                    (installed / 'bin' / name).write_bytes(b'runtime')
                plugin = installed / 'bin/platforms/qwindows.dll'
                plugin.parent.mkdir()
                plugin.write_bytes(b'plugin')
                (installed / 'uninstall.exe').write_bytes(b'uninstaller')
            else:
                installed = Path(command.split(' _?=', 1)[1])
                running = Path(command.split(' /S', 1)[0].strip('"'))
                self.assertTrue(running.is_file())
                self.assertFalse(running.is_relative_to(installed), 'Uninstaller must not kill itself inside the app folder')
                self.assertEqual(running.read_bytes(), b'uninstaller')
                if cleanup_fails:
                    raise windows.subprocess.CalledProcessError(4294967295, command)
                shutil.rmtree(installed)

        def startup(*args, **kwargs):
            self.assertFalse(prefix.exists(), 'Installed app must run without the Craft prefix available')
            self.assertTrue(prefix.with_name(prefix.name + '.smoke-isolated').is_dir())
            self.assertEqual(kwargs['env']['PATH'], windows.os.pathsep.join([
                str(Path('C:/Windows') / 'System32'), 'C:/Windows']))
            if startup_fails:
                raise windows.subprocess.CalledProcessError(1, args)
            return 'UDrive 7.1.1\n'

        stack = ExitStack()
        self.addCleanup(stack.close)
        stack.enter_context(redirect_stdout(stdout))
        stack.enter_context(redirect_stderr(stderr))
        registry = SimpleNamespace(HKEY_CURRENT_USER=1, HKEY_LOCAL_MACHINE=2,
                                   OpenKey=Mock(side_effect=FileNotFoundError))
        stack.enter_context(patch.dict(windows.sys.modules, {'winreg': registry}))
        # Windows normalizes os.environ keys to uppercase. A plain copied dict
        # no longer supports the case-insensitive lookup os.environ provides.
        stack.enter_context(patch.dict(windows.os.environ, {'SYSTEMROOT': 'C:/Windows'}, clear=True))
        stack.enter_context(patch.object(windows.subprocess, 'run', side_effect=process))
        stack.enter_context(patch.object(windows, 'run', side_effect=startup))
        return (installer, prefix, work, '7.1.1'), commands, stdout, stderr

    def test_install_validation_and_external_uninstaller_complete(self):
        args, commands, stdout, _ = self.smoke_fixture()
        windows.smoke_test(*args)
        self.assertEqual(len(commands), 2)
        self.assertTrue(args[1].is_dir())
        self.assertIn('version check passed', stdout.getvalue())
        files = (args[2] / 'logs/udrive-installed-files.txt').read_text()
        self.assertIn('Qt6Core.dll', files)
        self.assertIn('udrive.exe', files)

    def test_uninstall_failure_cannot_hide_missing_runtime_error(self):
        args, _, _, stderr = self.smoke_fixture(missing_runtime=True, cleanup_fails=True)
        with self.assertRaisesRegex(windows.BuildError, 'Missing installed runtime dependency: Qt6Core.dll'):
            windows.smoke_test(*args)
        self.assertIn('preserving the installation error', stderr.getvalue())
        self.assertTrue(args[1].is_dir())

    def test_uninstall_failure_after_validation_still_fails_acceptance(self):
        args, _, _, _ = self.smoke_fixture(cleanup_fails=True)
        with self.assertRaisesRegex(windows.BuildError, 'Application validation passed, but test uninstall failed'):
            windows.smoke_test(*args)

    def test_startup_failure_restores_craft_prefix_and_runs_cleanup(self):
        args, commands, _, _ = self.smoke_fixture(startup_fails=True)
        with self.assertRaises(windows.subprocess.CalledProcessError):
            windows.smoke_test(*args)
        self.assertTrue(args[1].is_dir())
        self.assertFalse(args[1].with_name(args[1].name + '.smoke-isolated').exists())
        self.assertEqual(len(commands), 2)

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

    def test_pinned_qt_sources_use_archive_and_keep_checksum_verification(self):
        qt_root = self.qt_fixture()
        windows.use_archived_qt_sources(qt_root)
        parent = configparser.ConfigParser(interpolation=None)
        parent.read(qt_root / "version.ini")
        self.assertEqual(parent["General"]["include"], "version_udrive_archive.ini")
        self.assertEqual(parent["General"]["tarballs"], "6.5.2")
        self.assertEqual(parent["General"]["branches"], "6.8;dev")
        self.assertEqual((qt_root / "version_upstream.ini").read_text(), QT_UPSTREAM_INFO)
        overlay = configparser.ConfigParser(interpolation=None)
        overlay.read(qt_root / parent["General"]["include"])
        target = overlay["General"]
        self.assertEqual(target["include"], "version_upstream.ini")
        self.assertEqual(target["tarballs"], "6.8.3")
        source_url = target["tarballUrl"]
        self.assertTrue(source_url.startswith("https://download.qt.io/archive/qt/"))
        self.assertEqual(target["tarballDigestUrl"], source_url + ".sha256")
        self.assertEqual(target["tarballInstallSrc"], "${PACKAGE_NAME}-everywhere-src-${VERSION}")
        # Reusing a build directory must not add another include layer.
        before = {path.name: path.read_bytes() for path in qt_root.iterdir()}
        windows.use_archived_qt_sources(qt_root)
        self.assertEqual({path.name: path.read_bytes() for path in qt_root.iterdir()}, before)

    def test_unknown_qt_metadata_is_unchanged_on_failure(self):
        qt_root = self.qt_fixture()
        source = QT_VERSION_INFO.replace("version_upstream.ini", "different.ini")
        (qt_root / "version.ini").write_text(source)
        with self.assertRaisesRegex(windows.BuildError, "Qt version metadata has changed"):
            windows.use_archived_qt_sources(qt_root)
        self.assertEqual((qt_root / "version.ini").read_text(), source)
        self.assertFalse((qt_root / "version_udrive_archive.ini").exists())

    def test_nsis_keeps_requested_folder_across_multiuser_initialization(self):
        path = self.root / 'installer.nsi'
        path.write_text(NSIS_TEMPLATE)
        windows.preserve_nsis_install_directory(path)
        result = path.read_text()
        save = result.index('StrCpy $UDriveRequestedInstallDir $INSTDIR')
        initialize = result.index('!insertmacro MULTIUSER_INIT')
        restore = result.index('StrCpy $INSTDIR $UDriveRequestedInstallDir')
        self.assertLess(save, initialize)
        self.assertLess(initialize, restore)
        self.assertIn('${If} $UDriveRequestedInstallDir != ""', result)
        self.assertIn('!if "@{architecture}" == "x86_64"', result)
        windows.preserve_nsis_install_directory(path)
        self.assertEqual(path.read_text(), result)

    def test_unknown_nsis_template_is_not_modified(self):
        path = self.root / 'installer.nsi'
        source = NSIS_TEMPLATE.replace('MULTIUSER_INIT', 'NEW_INIT')
        path.write_text(source)
        with self.assertRaises(windows.BuildError):
            windows.preserve_nsis_install_directory(path)
        self.assertEqual(path.read_text(), source)

    def test_non_windows_host_stops_before_downloads_or_output(self):
        args = argparse.Namespace(work_root=self.root / "craft", output_dir=self.root / "output", smoke_test=False)
        with patch.object(windows.sys, "platform", "darwin"), patch.object(windows, "run") as runner:
            with self.assertRaises(windows.BuildError):
                windows.build(args)
        runner.assert_not_called()
        self.assertFalse(args.work_root.exists())
        self.assertFalse(args.output_dir.exists())

    def test_windows_settings_resolve_without_apple_signing_environment(self):
        # Match CraftMaster's config resolution on a clean Windows runner.
        config = configparser.ConfigParser(interpolation=configparser.ExtendedInterpolation())
        config.optionxform = str
        config.read([windows.ROOT / '.craft.ini', windows.ROOT / '.github/workflows/craft_override.ini'])
        config['Variables'].update(Root='C:/udrive-build', WORKSPACE='C:/udrive-build',
                                   PythonExecutable='C:/Python312/python.exe',
                                   PackageOutput='C:/udrive-build/packages', CiBuild='False', CreateCache='False')
        config.read_string(windows.WINDOWS_CRAFT_OVERRIDE)
        settings = dict(config['GeneralSettings'])
        self.assertEqual(settings['CodeSigning/MacDeveloperId'], '')
        self.assertEqual(settings['CodeSigning/MacKeychainPath'], '')
        self.assertEqual(settings['CodeSigning/Enabled'], 'False')
        self.assertEqual(settings['Paths/Python'], 'C:/Python312/python.exe')
        self.assertEqual(settings['Packager/Destination'], 'C:/udrive-build/packages')

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
        self.assertTrue((args.work_root / "blueprints/craft-blueprints-kde/libs/qt6/version_udrive_archive.ini").is_file())

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
