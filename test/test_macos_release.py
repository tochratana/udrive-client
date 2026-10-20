# SPDX-License-Identifier: GPL-2.0-or-later
"""Release-policy tests use synthetic identities and never access a Keychain."""

import argparse
from contextlib import contextmanager
import importlib.util
import json
from pathlib import Path
import plistlib
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location("udrive_release", Path(__file__).resolve().parents[1] / "admin/osx/release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)
TEAM = "TEST123456"
FINGERPRINT = "A" * 40
IDENTITIES = f'1) {FINGERPRINT} "Developer ID Application: Test Publisher ({TEAM})"\n1 valid identities found\n'


class TestMacRelease(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()

    def workflow(self, **overrides):
        values = dict(prefix=self.root / "prefix", build_dir=self.root / "release-build", seed_build_dir=self.root / "debug-build",
                      output_dir=self.root / "output", sdk=None, jobs=2, allow_debug=False,
                      team_id=TEAM, identity=FINGERPRINT, notary_profile="test-profile")
        values.update(overrides)
        return release.Workflow(argparse.Namespace(**values))

    def app(self):
        app = self.root / "udrive.app"
        (app / "Contents/MacOS").mkdir(parents=True)
        info = {"CFBundleIdentifier": release.BUNDLE_ID, "CFBundleExecutable": "udrive",
                "CFBundleShortVersionString": "7.1.1", "CFBundleVersion": "7.1.1"}
        (app / "Contents/Info.plist").write_bytes(plistlib.dumps(info))
        for name in ("udrive", release.PDF_HELPER):
            (app / "Contents/MacOS" / name).write_bytes(bytes.fromhex("cffaedfe") + b"synthetic")
        for relative in ("PlugIns/platforms/libqcocoa.dylib", "Resources/qt.conf",
                         "Resources/qml/org/ownCloud/gui/qmldir", "Resources/qml/org/ownCloud/libsync/qmldir",
                         "Resources/qml/org/ownCloud/resources/qmldir"):
            path = app / "Contents" / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
        for relative in ("PlugIns/platforms/libqcocoa.dylib", "Resources/qml/org/ownCloud/gui/libowncloudGuiplugin.dylib",
                         "Resources/qml/org/ownCloud/libsync/liblibsyncplugin.dylib",
                         "Resources/qml/org/ownCloud/resources/libowncloudResourcesplugin.dylib"):
            (app / "Contents" / relative).write_bytes(bytes.fromhex("cffaedfe") + b"synthetic")
        return app

    def test_only_matching_developer_id_is_accepted(self):
        self.assertEqual(release.identity_for_team(FINGERPRINT.lower(), TEAM, IDENTITIES), FINGERPRINT)
        for fingerprint, team, output in (
            ("-", TEAM, IDENTITIES), (FINGERPRINT, "OTHER12345", IDENTITIES),
            (FINGERPRINT, TEAM, IDENTITIES.replace("Developer ID Application:", "Apple Development:")),
            (FINGERPRINT, TEAM, "0 valid identities found"), (FINGERPRINT, "", IDENTITIES)):
            with self.subTest(team=team, fingerprint=fingerprint), self.assertRaises(release.ReleaseError):
                release.identity_for_team(fingerprint, team, output)

    def test_missing_identity_stops_before_build_or_upload(self):
        with patch.object(release.Workflow, "tools"), patch.object(release, "run", return_value="0 valid identities found") as runner:
            with patch.object(release.Workflow, "build_app") as build:
                self.assertEqual(release.main(["release", "--team-id", TEAM, "--identity", FINGERPRINT,
                                               "--notary-profile", "test-profile"]), 1)
                build.assert_not_called()
                self.assertEqual(runner.call_count, 1)
                self.assertEqual(runner.call_args.args[0], "/usr/bin/security")

    def test_never_reconfigures_development_build(self):
        workflow = self.workflow(build_dir=self.root / "debug-build")
        with patch.object(release, "run") as runner, self.assertRaises(release.ReleaseError):
            workflow.build_app()
        runner.assert_not_called()

    def test_debug_cannot_become_customer_release(self):
        workflow = self.workflow(allow_debug=True)
        with patch.object(release, "cache_values", return_value={"CMAKE_BUILD_TYPE": "Debug"}):
            with self.assertRaises(release.ReleaseError):
                workflow.prepare(release=True)
        self.assertFalse(workflow.args.output_dir.exists())

    def test_build_requests_all_runtime_qml_plugins(self):
        workflow = self.workflow()
        with patch.object(release, "run") as runner:
            workflow.build_app()
        arguments = runner.call_args.args
        for target in ("owncloud", "owncloudGuiplugin", "libsyncplugin", "owncloudResourcesplugin"):
            self.assertIn(target, arguments)

    @unittest.skipUnless(sys.platform == "darwin", "macOS native verification tools")
    def test_independent_verification_does_not_require_craft_or_qt(self):
        workflow = self.workflow(prefix=self.root / "no-developer-tools")
        self.assertFalse(workflow.prefix.exists())
        workflow.tools(deploy=False)

    def test_load_command_paths_with_spaces_and_percent_characters(self):
        parsed = release.load_commands("""Load command 1
 cmd LC_LOAD_DYLIB
 name @rpath/100% Library.dylib (offset 24)
Load command 2
 cmd LC_RPATH
 path /Users/Developer Name/lib (offset 12)
Load command 3
 cmd LC_ID_DYLIB
 name @rpath/self.dylib (offset 24)
Load command 4
 cmd LC_UUID
 uuid A-B-C
""")
        self.assertEqual(parsed, (["@rpath/100% Library.dylib"], ["/Users/Developer Name/lib"], "@rpath/self.dylib", ["A-B-C"]))

    def test_external_symlink_is_rejected(self):
        app = self.app()
        (app / "Contents/escape").symlink_to(self.root)
        with self.assertRaises(release.ReleaseError):
            release.macho_files(app)

    def test_relocation_bundles_transitive_dependencies_and_removes_search_paths(self):
        app = self.app()
        binary = app / "Contents/MacOS/udrive"
        prefix = self.root / "prefix"
        (prefix / "lib").mkdir(parents=True)
        dependency = prefix / "lib/Library with spaces.dylib"
        dependency.write_bytes(bytes.fromhex("cffaedfe") + b"library")
        (prefix / "lib/Transitive.dylib").write_bytes(bytes.fromhex("cffaedfe") + b"transitive")
        commands = []

        def inspect(path):
            if path == binary:
                return (["@rpath/Library with spaces.dylib"], [str(prefix / "lib")], None, ["main"])
            if path.name == dependency.name:
                return (["@rpath/Transitive.dylib"], [str(prefix / "lib")], "old-id", ["library"])
            return ([], [], None, [path.name])

        with patch.object(release, "inspect_macho", side_effect=inspect), patch.object(release, "run", side_effect=lambda *args, **kw: commands.append(args)):
            release.relocate_dependencies(app, self.root / "build", prefix)
        frameworks = app / "Contents/Frameworks"
        self.assertTrue((frameworks / dependency.name).is_file())
        self.assertTrue((frameworks / "Transitive.dylib").is_file())
        edits = next(c for c in commands if c[-1] == binary)
        self.assertIn("@loader_path/../Frameworks/Library with spaces.dylib", edits)
        self.assertIn("-delete_rpath", edits)
        self.assertIn(str(prefix / "lib"), edits)
        transitive = next(c for c in commands if c[-1] == frameworks / dependency.name)
        self.assertIn("@loader_path/Transitive.dylib", transitive)
        self.assertEqual(dependency.read_bytes(), bytes.fromhex("cffaedfe") + b"library")

    def test_staged_abi_filename_is_preferred_over_prefix_version_alias(self):
        app = self.app()
        frameworks = app / "Contents/Frameworks"; frameworks.mkdir()
        staged = frameworks / "libExample.1.dylib"
        staged.write_bytes(bytes.fromhex("cffaedfe") + b"staged")
        prefix = self.root / "prefix/lib"; prefix.mkdir(parents=True)
        (prefix / staged.name).write_bytes(bytes.fromhex("cffaedfe") + b"prefix")
        binary = app / "Contents/MacOS/udrive"
        self.assertEqual(release.resolve_dependency("@rpath/" + staged.name, binary, binary,
                                                   [frameworks, prefix], [str(prefix)]), staged)

    def test_version_aliases_share_one_library_image(self):
        app = self.app()
        frameworks = app / "Contents/Frameworks"; frameworks.mkdir()
        abi = frameworks / "libExample.1.dylib"
        full = frameworks / "libExample.1.2.3.dylib"
        for file in (abi, full):
            file.write_bytes(bytes.fromhex("cffaedfe") + b"same original library")
        with patch.object(release, "inspect_macho", return_value=([], [], "old-id", ["same-linker-uuid"])):
            release.deduplicate_libraries(app)
        self.assertTrue(full.is_file()); self.assertFalse(full.is_symlink())
        self.assertTrue(abi.is_symlink()); self.assertEqual(abi.resolve(), full)
        self.assertNotIn(abi, release.macho_files(app))

    def test_missing_helper_or_wrong_identity_is_rejected(self):
        app = self.app()
        (app / "Contents/MacOS" / release.PDF_HELPER).unlink()
        with self.assertRaises(release.ReleaseError):
            release.validate_bundle(app)
        path = app / "Contents/Info.plist"
        info = plistlib.loads(path.read_bytes()); info["CFBundleIdentifier"] = "com.other.app"
        path.write_bytes(plistlib.dumps(info))
        with self.assertRaises(release.ReleaseError):
            release.validate_bundle(app)

    def test_external_load_commands_are_rejected(self):
        app = self.app()
        with patch.object(release, "run", return_value="arm64"), patch.object(release, "inspect_macho", return_value=(["/tmp/development.dylib"], [], None, [])):
            with self.assertRaises(release.ReleaseError):
                release.validate_bundle(app)

    def test_architecture_mismatch_is_rejected(self):
        app = self.app()
        with patch.object(release, "run", side_effect=["arm64", "x86_64"]), patch.object(release, "inspect_macho", return_value=([], [], None, [])):
            with self.assertRaises(release.ReleaseError):
                release.validate_bundle(app)

    def test_nested_code_signed_first_and_jit_only_on_main_app(self):
        app = self.app()
        framework = app / "Contents/Frameworks/Example.framework"
        library = framework / "Versions/A/Example"
        library.parent.mkdir(parents=True); library.touch()
        binaries = [app / "Contents/MacOS/udrive", app / "Contents/MacOS" / release.PDF_HELPER, library]
        with patch.object(release, "run") as runner:
            release.sign_bundle(app, binaries, FINGERPRINT)
        commands = [call.args for call in runner.call_args_list]
        self.assertEqual(commands[-1][-1], app)
        self.assertLess(next(i for i, c in enumerate(commands) if c[-1] == library), next(i for i, c in enumerate(commands) if c[-1] == framework))
        for command in commands:
            self.assertNotIn("--deep", command)
            self.assertIn("--timestamp", command); self.assertIn("runtime", command)
            self.assertEqual("--entitlements" in command, command[-1] == app)
        helper = next(c for c in commands if c[-1].name == release.PDF_HELPER)
        self.assertIn(release.BUNDLE_ID + ".pdf-renderer", helper)

    def test_rejected_notarization_never_staples(self):
        with patch.object(release, "run", return_value='{"status":"Invalid","id":"test"}') as runner:
            with self.assertRaises(release.ReleaseError):
                release.notarize(self.root / "test.dmg", "test-profile", self.root / "notary.json")
        self.assertEqual(runner.call_count, 1)
        self.assertEqual(json.loads((self.root / "notary.json").read_text())["status"], "Invalid")

    def test_accepted_notarization_staples_then_validates(self):
        with patch.object(release, "run", side_effect=['{"status":"Accepted"}', "", ""]) as runner:
            release.notarize(self.root / "test.dmg", "test-profile", self.root / "notary.json")
        self.assertEqual(runner.call_args_list[1].args[1:3], ("stapler", "staple"))
        self.assertEqual(runner.call_args_list[2].args[1:3], ("stapler", "validate"))

    def test_rejected_app_does_not_create_customer_image(self):
        app = self.app()
        (app.parent / "PREPARED-ONLY.txt").touch()
        workflow = self.workflow()
        with patch.object(workflow, "prepare", return_value=(app, {"CFBundleShortVersionString": "7.1.1"}, [])), \
             patch.object(release, "sign_bundle"), patch.object(release, "verify_signature"), \
             patch.object(release, "run", return_value='{"status":"Invalid"}') as runner:
            with self.assertRaises(release.ReleaseError):
                workflow.release(FINGERPRINT)
        self.assertFalse(list(self.root.glob("*.dmg")))
        self.assertTrue((app.parent / "PREPARED-ONLY.txt").exists())
        self.assertFalse(any("hdiutil" in str(call.args[0]) for call in runner.call_args_list))

    def test_success_requires_app_then_image_notarization_and_gatekeeper(self):
        app = self.app()
        (app.parent / "PREPARED-ONLY.txt").touch()
        workflow = self.workflow()
        calls = []

        def fake_run(*args, **kwargs):
            calls.append(args)
            if args[0] == "/usr/bin/ditto":
                if "-k" in args:
                    args[-1].write_bytes(b"zip")
                else:
                    shutil.copytree(args[-2], args[-1])
            elif args[0] == "/usr/bin/hdiutil" and args[1] == "create":
                args[-1].write_bytes(b"image")
            elif args[0] == "/usr/bin/xcrun" and args[1] == "notarytool":
                return '{"status":"Accepted"}'
            elif args[0] == "/usr/bin/lipo":
                return "arm64"
            elif args[0] == "git" and "rev-parse" in args:
                return "f" * 40
            return ""

        with patch.object(workflow, "prepare", return_value=(app, {"CFBundleShortVersionString": "7.1.1"}, [])), \
             patch.object(release, "sign_bundle"), patch.object(release, "verify_signature") as verify, \
             patch.object(release, "run", side_effect=fake_run), patch("builtins.print"):
            workflow.release(FINGERPRINT)
        submissions = [c for c in calls if "notarytool" in c]
        self.assertEqual(len(submissions), 2)
        self.assertEqual(submissions[0][3].suffix, ".zip")
        self.assertEqual(submissions[1][3].suffix, ".dmg")
        self.assertEqual(verify.call_count, 2)
        self.assertTrue(any(c[0] == "/usr/sbin/spctl" and "open" in c for c in calls))
        self.assertFalse((app.parent / "PREPARED-ONLY.txt").exists())
        manifest = json.loads((app.parent / "release.json").read_text())
        self.assertEqual(manifest["team_id"], TEAM)
        self.assertTrue((app.parent / manifest["dmg"]).is_file())

    def finished_download(self, **overrides):
        directory = self.root / "finished"
        directory.mkdir()
        image = directory / "UDrive-7.1.1-arm64.dmg"
        image.write_bytes(b"signed disk image")
        manifest = {"bundle_id": release.BUNDLE_ID, "version": "7.1.1", "architecture": "arm64",
                    "dmg": image.name, "team_id": TEAM, "sha256": release.file_sha256(image),
                    "source_commit": "f" * 40, "source_dirty": False}
        manifest.update(overrides)
        (directory / "release.json").write_text(json.dumps(manifest))
        return directory

    @contextmanager
    def export_checks(self):
        calls = []

        def fake_run(*args, **kwargs):
            calls.append(args)
            if args[0] == "/usr/bin/codesign" and "--display" in args:
                return f"Authority=Developer ID Application: Test Publisher ({TEAM})\nTeamIdentifier={TEAM}\nTimestamp=Oct 20, 2026\n"
            if args[0] == "/usr/bin/lipo":
                return "arm64"
            return ""

        info = {"CFBundleShortVersionString": "7.1.1", "LSMinimumSystemVersion": "13.0"}
        with patch.object(release, "run", side_effect=fake_run), \
             patch.object(release, "validate_bundle", return_value=(info, [])) as validate, \
             patch.object(release, "verify_signature") as signature:
            yield info, calls, validate, signature

    def test_export_rejects_prepared_and_testing_packages_before_native_checks(self):
        for marker in ("PREPARED-ONLY.txt", "TESTING-ONLY.txt"):
            directory = self.root / marker.replace(".txt", "")
            directory.mkdir()
            (directory / marker).touch()
            with self.subTest(marker=marker), patch.object(release, "run") as runner:
                with self.assertRaises(release.ReleaseError):
                    release.export_download(directory, self.root / "public", TEAM, "./")
                runner.assert_not_called()
                self.assertFalse((self.root / "public").exists())

    def test_export_rejects_modified_disk_image_before_native_checks(self):
        directory = self.finished_download()
        (directory / "UDrive-7.1.1-arm64.dmg").write_bytes(b"tampered image")
        with patch.object(release, "run") as runner, self.assertRaises(release.ReleaseError):
            release.export_download(directory, self.root / "public", TEAM, "./")
        runner.assert_not_called()
        self.assertFalse((self.root / "public").exists())

    def test_export_rejects_external_image_symlink(self):
        directory = self.finished_download()
        external = self.root / "external.dmg"
        image = directory / "UDrive-7.1.1-arm64.dmg"
        image.rename(external)
        image.symlink_to(external)
        with patch.object(release, "run") as runner, self.assertRaises(release.ReleaseError):
            release.export_download(directory, self.root / "public", TEAM, "./")
        runner.assert_not_called()

    def test_export_rejects_manifest_for_another_publisher(self):
        directory = self.finished_download(team_id="OTHER12345")
        with patch.object(release, "run") as runner, self.assertRaises(release.ReleaseError):
            release.export_download(directory, self.root / "public", TEAM, "./")
        runner.assert_not_called()

    def test_export_rejects_ad_hoc_image_signature(self):
        directory = self.finished_download()
        with patch.object(release, "run", return_value="Signature=adhoc\nTeamIdentifier=not set\n"), \
             patch.object(release, "validate_bundle") as validate, self.assertRaises(release.ReleaseError):
            release.export_download(directory, self.root / "public", TEAM, "./")
        validate.assert_not_called()
        self.assertFalse((self.root / "public").exists())

    def test_export_gatekeeper_failure_creates_no_downloads(self):
        directory = self.finished_download()
        def fake_run(*args, **kwargs):
            if args[0] == "/usr/bin/codesign" and "--display" in args:
                return f"TeamIdentifier={TEAM}\nAuthority=Developer ID Application: Test\nTimestamp=test\n"
            if args[0] == "/usr/sbin/spctl":
                raise release.ReleaseError("Gatekeeper rejected image")
            return ""
        with patch.object(release, "run", side_effect=fake_run), self.assertRaises(release.ReleaseError):
            release.export_download(directory, self.root / "public", TEAM, "./")
        self.assertFalse((self.root / "public").exists())

    def test_export_checks_embedded_app_and_detaches_after_failure(self):
        directory = self.finished_download()
        with self.export_checks() as (info, calls, validate, signature):
            info["CFBundleShortVersionString"] = "7.1.0"
            with self.assertRaises(release.ReleaseError):
                release.export_download(directory, self.root / "public", TEAM, "./")
        self.assertEqual(validate.call_args.args[0].parent.name, "volume")
        self.assertTrue(any(c[0] == "/usr/bin/hdiutil" and c[1] == "detach" for c in calls))
        signature.assert_not_called()
        self.assertFalse((self.root / "public").exists())

    def test_export_only_copies_verified_public_download_files(self):
        directory = self.finished_download()
        (directory / "notary-upload.zip").write_bytes(b"private staging file")
        (directory / "app-notarization.json").write_text("private notary response")
        output = self.root / "public"
        with self.export_checks() as (_, calls, validate, signature):
            release.export_download(directory, output, TEAM, "https://downloads.example.com/udrive/7.1.1/")
        self.assertEqual({p.name for p in output.iterdir()},
                         {"UDrive-7.1.1-arm64.dmg", "SHA256SUMS", "download.json", "download-link.html"})
        metadata = json.loads((output / "download.json").read_text())
        self.assertEqual(metadata["url"], "https://downloads.example.com/udrive/7.1.1/UDrive-7.1.1-arm64.dmg")
        self.assertEqual(metadata["sha256"], release.file_sha256(output / metadata["filename"]))
        self.assertEqual(metadata["minimum_macos"], "13.0")
        self.assertIn("Apple Silicon", (output / "download-link.html").read_text())
        self.assertNotIn(str(directory), (output / "download.json").read_text())
        signature.assert_called_once_with(validate.call_args.args[0], [], TEAM)
        self.assertTrue(any(c[0] == "/usr/bin/xcrun" and c[1:3] == ("stapler", "validate") for c in calls))
        self.assertEqual((directory / "notary-upload.zip").read_bytes(), b"private staging file")

    def test_export_does_not_overwrite_existing_downloads(self):
        directory = self.finished_download()
        output = self.root / "public"
        output.mkdir()
        (output / "keep.txt").write_text("existing download")
        with patch.object(release, "run") as runner, self.assertRaises(release.ReleaseError):
            release.export_download(directory, output, TEAM, "./")
        runner.assert_not_called()
        self.assertEqual((output / "keep.txt").read_text(), "existing download")

    def test_download_urls_require_https_or_a_local_website_path(self):
        for base, expected in (("./", "./UDrive.dmg"), ("/downloads", "/downloads/UDrive.dmg"),
                               ("https://example.com/downloads/", "https://example.com/downloads/UDrive.dmg")):
            with self.subTest(base=base):
                self.assertEqual(release.download_url(base, "UDrive.dmg"), expected)
        for base in ("http://example.com", "//example.com", "javascript:alert(1)",
                     "https://user:secret@example.com", "https://example.com/?token=secret",
                     "https://example.com/#download", "https://example.com:invalid", "downloads"):
            with self.subTest(base=base), self.assertRaises((release.ReleaseError, ValueError)):
                release.download_url(base, "UDrive.dmg")


if __name__ == "__main__":
    unittest.main()
