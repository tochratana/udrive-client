#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build and distribute Udrive locally; never edit the running development app."""

import argparse
from datetime import datetime
import hashlib
import html
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
from urllib.parse import quote, urlsplit

ROOT = Path(__file__).resolve().parents[2]
BUNDLE_ID = "com.unity-workspace.udrive"
PRODUCT_NAME = "Udrive"
APP_BUNDLE = PRODUCT_NAME + ".app"
PDF_HELPER = "udrive-pdf-renderer"
MACHO_MAGICS = {bytes.fromhex(s) for s in (
    "feedface", "cefaedfe", "feedfacf", "cffaedfe", "cafebabe", "bebafeca", "cafebabf", "bfbafeca")}


class ReleaseError(Exception):
    pass


def run(*args, capture=False, env=None):
    # Arguments are passed directly, never through a shell. No password flags.
    result = subprocess.run([str(a) for a in args], text=True, env=env,
                            stdout=subprocess.PIPE if capture else None,
                            stderr=subprocess.PIPE if capture else None)
    if result.returncode:
        detail = (result.stderr or result.stdout or "").strip() if capture else "See output above."
        raise ReleaseError(f"{Path(str(args[0])).name} failed ({result.returncode}): {detail}")
    return ((result.stdout or "") + (result.stderr or "")) if capture else ""


def cache_values(build):
    cache = build / "CMakeCache.txt"
    if not cache.is_file():
        raise ReleaseError(f"No configured build at {build}. Run the build command first.")
    values = {}
    for line in cache.read_text().splitlines():
        if line and not line.startswith(("#", "//")) and "=" in line and ":" in line.split("=", 1)[0]:
            key, value = line.split("=", 1)
            values[key.split(":", 1)[0]] = value
    return values


def inside(path, root):
    return path.resolve().is_relative_to(root.resolve())


def is_macho(path):
    if not path.is_file():
        return False
    with path.open("rb") as source:
        return source.read(4) in MACHO_MAGICS


def macho_files(app):
    result = set()
    for path in app.rglob("*"):
        if path.is_symlink() and not inside(path, app):
            raise ReleaseError(f"Bundle symlink escapes the app: {path}")
        if path.is_file() and is_macho(path):
            result.add(path.resolve())
    return sorted(result)


def load_commands(output):
    dependencies, rpaths, install_id, uuids = [], [], None, []
    command = ""
    for line in output.splitlines():
        line = line.strip()
        if line.startswith("cmd "):
            command = line[4:]
        elif line.startswith("name "):
            name = line[5:].rsplit(" (offset ", 1)[0]
            if command in ("LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB", "LC_LOAD_UPWARD_DYLIB"):
                dependencies.append(name)
            elif command == "LC_ID_DYLIB":
                install_id = name
        elif command == "LC_RPATH" and line.startswith("path "):
            rpaths.append(line[5:].rsplit(" (offset ", 1)[0])
        elif command == "LC_UUID" and line.startswith("uuid "):
            uuids.append(line[5:])
    return list(dict.fromkeys(dependencies)), list(dict.fromkeys(rpaths)), install_id, sorted(uuids)


def inspect_macho(path):
    return load_commands(run("/usr/bin/otool", "-l", path, capture=True))


def system_dependency(name):
    return name.startswith(("/System/Library/", "/usr/lib/"))


def expand_loader_path(name, binary, executable):
    for token, root in (("@loader_path", binary.parent), ("@executable_path", executable.parent)):
        if name == token or name.startswith(token + "/"):
            return root / name[len(token):].lstrip("/")
    return Path(name) if name.startswith("/") else None


def resolve_dependency(name, binary, executable, search_roots, rpaths):
    candidates = []
    direct = expand_loader_path(name, binary, executable)
    if direct is not None:
        candidates.append(direct)
    if name.startswith("@rpath/"):
        suffix = name[len("@rpath/"):]
        # Prefer the staged dependency over the build prefix. macdeployqt may
        # preserve the ABI filename while the prefix symlink resolves to a full
        # version filename; loading both duplicates static/Objective-C registries.
        candidates.append(search_roots[0] / suffix)
        for rpath in rpaths:
            root = expand_loader_path(rpath, binary, executable)
            if root is not None:
                candidates.append(root / suffix)
        candidates.extend(root / suffix for root in search_roots)
    for candidate in candidates:
        if candidate.is_file() and is_macho(candidate):
            return candidate.resolve()
    raise ReleaseError(f"Unresolved dependency {name!r} in {binary}")


def deduplicate_libraries(app):
    frameworks = app / "Contents/Frameworks"
    groups = {}
    for path in macho_files(app):
        if path.parent != frameworks or path.suffix != ".dylib":
            continue
        _, _, install_id, uuids = inspect_macho(path)
        if install_id and uuids:
            groups.setdefault(tuple(uuids), []).append(path)
    for paths in groups.values():
        canonical = max(paths, key=lambda p: (len(p.name), p.name))
        for alias in paths:
            if alias != canonical:
                alias.unlink()
                alias.symlink_to(canonical.name)


def copy_dependency(source, frameworks):
    framework = next((p for p in source.parents if p.suffix == ".framework"), None)
    if framework:
        target_root = frameworks / framework.name
        target = target_root / source.relative_to(framework)
        if not target_root.exists():
            shutil.copytree(framework, target_root, symlinks=True)
    else:
        target = frameworks / source.name
        if not target.exists():
            shutil.copy2(source, target)
    if not target.is_file():
        raise ReleaseError(f"Dependency layout collision: {source} -> {target}")
    # install_name_tool changes bytes but preserves UUIDs. Reject two unrelated
    # libraries with the same destination name, including different architectures.
    if source != target.resolve():
        source_uuids, target_uuids = inspect_macho(source)[3], inspect_macho(target)[3]
        if source_uuids != target_uuids or (not source_uuids and source.read_bytes() != target.read_bytes()):
            raise ReleaseError(f"Conflicting bundled library: {target.name}")
    return target.resolve()


def relocate_dependencies(app, build, prefix):
    executable = app / "Contents/MacOS/udrive"
    frameworks = app / "Contents/Frameworks"
    frameworks.mkdir(exist_ok=True)
    search_roots = [frameworks, build / "bin", prefix / "lib"]
    main_rpaths = inspect_macho(executable)[1]
    done = set()
    while True:
        deduplicate_libraries(app)
        pending = [p for p in macho_files(app) if p not in done]
        if not pending:
            break
        for binary in pending:
            dependencies, rpaths, install_id, _ = inspect_macho(binary)
            edits = []
            for name in dependencies:
                if system_dependency(name):
                    continue
                source = resolve_dependency(name, binary, executable, search_roots, rpaths + main_rpaths)
                target = source if inside(source, app) else copy_dependency(source, frameworks)
                relative = os.path.relpath(target, binary.parent).replace(os.sep, "/")
                replacement = "@loader_path/" + relative
                if replacement != name:
                    edits.extend(("-change", name, replacement))
            # Every load command now resolves inside the bundle; remove all
            # development search paths so installed copies cannot load local code.
            for rpath in rpaths:
                edits.extend(("-delete_rpath", rpath))
            if install_id:
                identity = "@rpath/" + (str(binary.relative_to(frameworks)) if inside(binary, frameworks) else binary.name)
                edits.extend(("-id", identity))
            if edits:
                run("/usr/bin/install_name_tool", *edits, binary)
            done.add(binary)


def validate_bundle(app):
    with (app / "Contents/Info.plist").open("rb") as source:
        info = plistlib.load(source)
    if info.get("CFBundleIdentifier") != BUNDLE_ID or info.get("CFBundleExecutable") != "udrive":
        raise ReleaseError("Unexpected app identity. Udrive releases must retain " + BUNDLE_ID)
    if info.get("CFBundleName") != PRODUCT_NAME or info.get("CFBundleDisplayName") != PRODUCT_NAME:
        raise ReleaseError("The application display name must be " + PRODUCT_NAME)
    for key in ("CFBundleShortVersionString", "CFBundleVersion"):
        if not re.fullmatch(r"\d+(?:\.\d+){0,2}", info.get(key, "")):
            raise ReleaseError(f"Invalid release {key}: {info.get(key)!r}")
    for name in ("udrive", PDF_HELPER):
        if not is_macho(app / "Contents/MacOS" / name):
            raise ReleaseError(f"Missing Mach-O executable: {name}")
    if not is_macho(app / "Contents/PlugIns/platforms/libqcocoa.dylib"):
        raise ReleaseError("Missing bundled Qt Cocoa platform plugin")
    if not (app / "Contents/Resources/qt.conf").is_file():
        raise ReleaseError("Missing bundled qt.conf")
    for module, plugin in (("gui", "libowncloudGuiplugin.dylib"), ("libsync", "liblibsyncplugin.dylib"),
                           ("resources", "libowncloudResourcesplugin.dylib")):
        folder = app / "Contents/Resources/qml/org/ownCloud" / module
        if not (folder / "qmldir").is_file() or not is_macho(folder / plugin):
            raise ReleaseError(f"Missing bundled QML module: {module}")
    executable = app / "Contents/MacOS/udrive"
    binaries = macho_files(app)
    seen_libraries = {}
    main_arches = set(run("/usr/bin/lipo", "-archs", executable, capture=True).split())
    for binary in binaries:
        if not main_arches or not main_arches.issubset(set(run("/usr/bin/lipo", "-archs", binary, capture=True).split())):
            raise ReleaseError(f"Architecture mismatch in {binary}")
        dependencies, rpaths, install_id, uuids = inspect_macho(binary)
        if install_id and uuids:
            key = tuple(uuids)
            if key in seen_libraries:
                raise ReleaseError(f"Duplicate native library images: {seen_libraries[key]} and {binary}")
            seen_libraries[key] = binary
        if rpaths:
            raise ReleaseError(f"Unexpected runtime search paths in {binary}")
        for name in dependencies:
            if system_dependency(name):
                continue
            target = expand_loader_path(name, binary, executable)
            if target is None or not inside(target, app) or not is_macho(target):
                raise ReleaseError(f"Non-bundled dependency {name!r} in {binary}")
    return info, binaries


def identity_for_team(identity, team, output):
    if not re.fullmatch(r"[A-Z0-9]{10}", team or ""):
        raise ReleaseError("Set UDRIVE_TEAM_ID to your 10-character Apple Developer Team ID.")
    if not re.fullmatch(r"[0-9A-Fa-f]{40}", identity or ""):
        raise ReleaseError("Set UDRIVE_SIGN_IDENTITY to the SHA-1 fingerprint of a Developer ID Application certificate.")
    for line in output.splitlines():
        match = re.search(r'\b([0-9A-Fa-f]{40})\s+"(Developer ID Application: .+ \(([A-Z0-9]{10})\))"', line)
        if match and match[1].lower() == identity.lower() and match[3] == team:
            return identity.upper()
    raise ReleaseError("No valid Developer ID Application certificate with this fingerprint and Team ID was found in the local Keychain.")


def sign_bundle(app, binaries, identity):
    bundles = [p for p in app.rglob("*") if p.is_dir() and not p.is_symlink()
               and p.suffix in (".framework", ".app", ".xpc", ".appex", ".bundle")]
    # Sign leaves before their enclosing frameworks/helpers, then the main app.
    targets = sorted(set(binaries + bundles), key=lambda p: (-len(p.parts), str(p)))
    for target in targets:
        args = ["/usr/bin/codesign", "--force", "--sign", identity, "--timestamp", "--options", "runtime"]
        if target.name == PDF_HELPER:
            args += ["--identifier", BUNDLE_ID + ".pdf-renderer"]
        run(*args, target)
    run("/usr/bin/codesign", "--force", "--sign", identity, "--timestamp", "--options", "runtime",
        "--identifier", BUNDLE_ID, "--entitlements", ROOT / "admin/osx/udrive.entitlements", app)


def verify_signature(app, binaries, team):
    for target in [*binaries, app]:
        run("/usr/bin/codesign", "--verify", "--strict", target)
        details = run("/usr/bin/codesign", "--display", "--verbose=4", target, capture=True)
        if (f"TeamIdentifier={team}" not in details or "Authority=Developer ID Application:" not in details
                or "Timestamp=" not in details or "runtime" not in details):
            raise ReleaseError(f"Missing Developer ID, secure timestamp, or hardened runtime: {target}")
    details = run("/usr/bin/codesign", "--display", "--verbose=4", app, capture=True)
    if f"Identifier={BUNDLE_ID}\n" not in details:
        raise ReleaseError("Signed app identifier changed")
    # --deep is verification only; never use it to sign unrelated nested code.
    run("/usr/bin/codesign", "--verify", "--deep", "--strict", app)


def notarize(artifact, profile, log):
    output = run("/usr/bin/xcrun", "notarytool", "submit", artifact, "--keychain-profile", profile,
                 "--wait", "--output-format", "json", capture=True)
    log.write_text(output)
    try:
        status = json.loads(output)
    except json.JSONDecodeError as error:
        raise ReleaseError(f"Invalid notarization response; see {log}") from error
    if status.get("status") != "Accepted":
        raise ReleaseError(f"Notarization was not accepted; see {log}. No customer package was published.")
    run("/usr/bin/xcrun", "stapler", "staple", artifact)
    run("/usr/bin/xcrun", "stapler", "validate", artifact)


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download_url(base, filename):
    parts = urlsplit(base)
    # Accessing port also rejects malformed ports before producing a website link.
    parts.port
    if parts.scheme or parts.netloc:
        if (parts.scheme != "https" or not parts.hostname or parts.username or parts.password
                or parts.query or parts.fragment or any(c.isspace() for c in base)):
            raise ReleaseError("Download base URL must be HTTPS without credentials, a query or a fragment.")
    elif not (base.startswith("/") or base.startswith("./")) or parts.query or parts.fragment:
        raise ReleaseError("Download base must be an HTTPS URL or a website path starting with / or ./.")
    return base.rstrip("/") + "/" + quote(filename)


def export_download(release_dir, output, team, base_url):
    """Export a verified customer image for static HTTPS hosting; never upload it."""
    if not re.fullmatch(r"[A-Z0-9]{10}", team or ""):
        raise ReleaseError("Export requires --team-id for the expected Apple Developer publisher.")
    release_dir, output = release_dir.resolve(), output.resolve()
    if output.exists():
        raise ReleaseError(f"Output already exists; choose a new --output-dir: {output}")
    for marker in ("PREPARED-ONLY.txt", "TESTING-ONLY.txt"):
        if (release_dir / marker).exists():
            raise ReleaseError("Prepared and testing-only packages cannot be exported as production downloads.")
    manifest = json.loads((release_dir / "release.json").read_text())
    if not isinstance(manifest, dict):
        raise ReleaseError("Invalid release manifest.")
    version, architecture, filename = (manifest.get(k) for k in ("version", "architecture", "dmg"))
    if (manifest.get("bundle_id") != BUNDLE_ID or manifest.get("team_id") != team
            or not isinstance(version, str) or not re.fullmatch(r"\d+(?:\.\d+){0,2}", version)
            or architecture not in ("arm64", "x86_64", "arm64-x86_64")
            or filename != f"Udrive-{version}-{architecture}.dmg"):
        raise ReleaseError("Release manifest has an unexpected publisher, application, version or architecture.")
    dmg = release_dir / filename
    if not dmg.is_file() or not inside(dmg, release_dir):
        raise ReleaseError("Release disk image is missing or outside the release directory.")
    checksum = file_sha256(dmg)
    if checksum != manifest.get("sha256"):
        raise ReleaseError("Disk image checksum does not match the finished release.")
    url = download_url(base_url, filename)
    run("/usr/bin/hdiutil", "verify", dmg)
    run("/usr/bin/codesign", "--verify", "--strict", dmg)
    signature = run("/usr/bin/codesign", "--display", "--verbose=4", dmg, capture=True)
    if (f"TeamIdentifier={team}\n" not in signature or "Authority=Developer ID Application:" not in signature
            or "Timestamp=" not in signature):
        raise ReleaseError("Disk image is not signed by the expected Developer ID publisher with a secure timestamp.")
    run("/usr/bin/xcrun", "stapler", "validate", dmg)
    run("/usr/sbin/spctl", "--assess", "--type", "open", "--context", "context:primary-signature", "--verbose=2", dmg)

    # Verify the app inside the image, rather than trusting an adjacent app bundle.
    with tempfile.TemporaryDirectory(prefix="udrive-download-verify-") as temporary:
        mount = Path(temporary) / "volume"
        mount.mkdir()
        run("/usr/bin/hdiutil", "attach", "-readonly", "-nobrowse", "-mountpoint", mount, dmg)
        try:
            app = mount / APP_BUNDLE
            info, binaries = validate_bundle(app)
            if info["CFBundleShortVersionString"] != version:
                raise ReleaseError("Disk image contains a different application version.")
            actual_arch = "-".join(sorted(run("/usr/bin/lipo", "-archs", app / "Contents/MacOS/udrive", capture=True).split()))
            if actual_arch != architecture:
                raise ReleaseError("Disk image contains a different architecture.")
            verify_signature(app, binaries, team)
            run("/usr/bin/xcrun", "stapler", "validate", app)
            run("/usr/sbin/spctl", "--assess", "--type", "execute", "--verbose=2", app)
        finally:
            run("/usr/bin/hdiutil", "detach", mount)

    output.parent.mkdir(parents=True, exist_ok=True)
    # Publish the folder locally only after all checks and copies succeed.
    with tempfile.TemporaryDirectory(prefix=".udrive-download-", dir=output.parent) as temporary:
        staged = Path(temporary) / "download"
        staged.mkdir()
        shutil.copy2(dmg, staged / filename)
        if file_sha256(staged / filename) != checksum:
            raise ReleaseError("Disk image changed while exporting; no download folder was created.")
        metadata = {"schema_version": 1, "channel": "stable", "signature": "developer-id", "notarized": True,
                    "version": version, "platform": "macos", "architecture": architecture,
                    "minimum_macos": info.get("LSMinimumSystemVersion"), "url": url,
                    "filename": filename, "size_bytes": dmg.stat().st_size, "sha256": checksum,
                    "bundle_id": BUNDLE_ID, "team_id": team,
                    "source_commit": manifest.get("source_commit"), "source_dirty": manifest.get("source_dirty")}
        (staged / "download.json").write_text(json.dumps(metadata, indent=2) + "\n")
        (staged / "SHA256SUMS").write_text(f"{checksum}  {filename}\n")
        label = {"arm64": "Apple Silicon", "x86_64": "Intel", "arm64-x86_64": "Apple Silicon and Intel"}[architecture]
        (staged / "download-link.html").write_text(
            f'<a href="{html.escape(url, quote=True)}" download>Download Udrive {html.escape(version)} for Mac ({label})</a>\n')
        staged.rename(output)
    print(f"Verified website download files: {output}")


class Workflow:
    def __init__(self, args):
        self.args = args
        self.prefix = args.prefix.resolve()
        self.build = args.build_dir.resolve()
        self.cmake = self.prefix / "dev-utils/bin/cmake"
        self.macdeployqt = self.prefix / "bin/macdeployqt"

    def tools(self, deploy=True):
        if sys.platform != "darwin":
            raise ReleaseError("This release workflow runs on macOS.")
        paths = [Path("/usr/bin/otool"), Path("/usr/bin/lipo"), Path("/usr/bin/codesign")]
        if deploy:
            paths += [self.cmake, self.macdeployqt, Path("/usr/bin/install_name_tool")]
        for path in paths:
            if not path.is_file():
                raise ReleaseError(f"Missing tool: {path}. Set --prefix to the Craft/Qt dependency prefix.")

    def signing_identity(self):
        identities = run("/usr/bin/security", "find-identity", "-v", "-p", "codesigning", capture=True)
        identity = identity_for_team(self.args.identity, self.args.team_id, identities)
        if not self.args.notary_profile:
            raise ReleaseError("Set UDRIVE_NOTARY_PROFILE to a local notarytool Keychain profile; see admin/osx/RELEASING.md.")
        run("/usr/bin/xcrun", "--find", "notarytool", capture=True)
        run("/usr/bin/xcrun", "--find", "stapler", capture=True)
        return identity

    def build_app(self):
        if (self.build == self.args.seed_build_dir.resolve() or inside(self.build, self.prefix)
                or inside(ROOT, self.build)):
            raise ReleaseError("Use a separate release build directory, outside the development dependency prefix.")
        args = [self.cmake, "-S", ROOT, "-B", self.build, "-G", "Ninja",
                f"-DCMAKE_MAKE_PROGRAM={self.prefix}/dev-utils/bin/ninja",
                f"-DCMAKE_PREFIX_PATH={self.prefix}", "-DCMAKE_BUILD_TYPE=Release",
                "-DBUILD_TESTING=OFF", "-DBUILD_SHELL_INTEGRATION=OFF", "-DWITH_AUTO_UPDATER=OFF",
                "-DKDE_INSTALL_BUNDLEDIR=Applications"]
        if self.args.sdk:
            args.append(f"-DCMAKE_OSX_SYSROOT={self.args.sdk.resolve()}")
        elif (self.args.seed_build_dir / "CMakeCache.txt").is_file():
            sdk = cache_values(self.args.seed_build_dir).get("CMAKE_OSX_SYSROOT")
            if sdk:
                args.append(f"-DCMAKE_OSX_SYSROOT={sdk}")
        env = os.environ.copy()
        env["PATH"] = str(self.prefix / "bin") + os.pathsep + str(self.prefix / "dev-utils/bin") + os.pathsep + env.get("PATH", "")
        env["PKG_CONFIG_PATH"] = str(self.prefix / "lib/pkgconfig")
        run(*args, env=env)
        run(self.cmake, "--build", self.build, "--target", "owncloud", "owncloudGuiplugin", "libsyncplugin",
            "owncloudResourcesplugin", "--parallel", str(self.args.jobs), env=env)

    def prepare(self, release=False):
        values = cache_values(self.build)
        if values.get("CMAKE_BUILD_TYPE") not in ("Release", "RelWithDebInfo"):
            if release or not self.args.allow_debug:
                raise ReleaseError("A Release or RelWithDebInfo build is required; Debug is only allowed for prepare --allow-debug.")
        if Path(values.get("CMAKE_HOME_DIRECTORY", "")).resolve() != ROOT:
            raise ReleaseError("Build directory belongs to a different source checkout.")
        source = self.build / "bin/udrive.app"
        if not (source / "Contents/Info.plist").is_file():
            raise ReleaseError(f"App was not built: {source}")
        output = self.args.output_dir.resolve()
        if inside(output, source):
            raise ReleaseError("Output directory cannot be inside the source app bundle.")
        if output.exists():
            raise ReleaseError(f"Output already exists; use a new --output-dir: {output}")
        output.mkdir(parents=True)
        (output / "PREPARED-ONLY.txt").write_text("Not a customer release. Signing, notarization and release verification have not completed.\n")
        app = output / APP_BUNDLE
        # Preserve framework symlinks, but never mutate the development bundle.
        shutil.copytree(source, app, symlinks=True)
        macho_files(app)  # Reject escaping source symlinks before deployment tools run.
        helper = app / "Contents/MacOS" / PDF_HELPER
        run(self.macdeployqt, app, "-always-overwrite", "-verbose=1", "-no-strip",
            f"-executable={helper}", f"-qmldir={ROOT / 'src/gui'}",
            f"-qmlimport={self.build / 'bin'}",
            f"-libpath={self.build / 'bin'}", f"-libpath={self.prefix / 'lib'}")
        resources = app / "Contents/Resources"
        resources.mkdir(exist_ok=True)
        (resources / "qt.conf").write_text("[Paths]\nPlugins = PlugIns\nQmlImports = Resources/qml\n")
        # This generated module contains the QML sidebar and its compiled plugin.
        modules = self.build / "bin/org/ownCloud"
        for module in ("gui", "libsync", "resources"):
            if not (modules / module / "qmldir").is_file():
                raise ReleaseError(f"Missing QML module {module}; build its plugin target first.")
        target = resources / "qml/org/ownCloud"
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(modules, target, dirs_exist_ok=True, symlinks=True,
                        ignore=shutil.ignore_patterns("*.cpp", "*.h", "*.o", "*.a"))
        relocate_dependencies(app, self.build, self.prefix)
        info, binaries = validate_bundle(app)
        print(f"Prepared {len(binaries)} Mach-O files: {app}")
        return app, info, binaries

    def release(self, identity):
        app, info, binaries = self.prepare(release=True)
        output = app.parent
        sign_bundle(app, binaries, identity)
        verify_signature(app, binaries, self.args.team_id)
        archive = output / "notary-upload.zip"
        run("/usr/bin/ditto", "-c", "-k", "--keepParent", app, archive)
        # A ZIP cannot be stapled. Submit it, then staple its app, before DMG creation.
        response = run("/usr/bin/xcrun", "notarytool", "submit", archive, "--keychain-profile", self.args.notary_profile,
                       "--wait", "--output-format", "json", capture=True)
        (output / "app-notarization.json").write_text(response)
        if json.loads(response).get("status") != "Accepted":
            raise ReleaseError("App notarization was not accepted; see app-notarization.json.")
        run("/usr/bin/xcrun", "stapler", "staple", app)
        run("/usr/bin/xcrun", "stapler", "validate", app)
        run("/usr/sbin/spctl", "--assess", "--type", "execute", "--verbose=2", app)
        image_root = Path(tempfile.mkdtemp(prefix="dmg-root-", dir=output))
        try:
            run("/usr/bin/ditto", app, image_root / APP_BUNDLE)
            run("/usr/bin/codesign", "--verify", "--deep", "--strict", image_root / APP_BUNDLE)
            (image_root / "Applications").symlink_to("/Applications")
            architecture = "-".join(sorted(run("/usr/bin/lipo", "-archs", app / "Contents/MacOS/udrive", capture=True).split()))
            name = f"Udrive-{info['CFBundleShortVersionString']}-{architecture}.dmg"
            pending = output / (name + ".pending.dmg")
            run("/usr/bin/hdiutil", "create", "-volname", "Udrive", "-srcfolder", image_root,
                "-format", "UDZO", "-fs", "HFS+", pending)
            run("/usr/bin/codesign", "--force", "--sign", identity, "--timestamp", pending)
            notarize(pending, self.args.notary_profile, output / "dmg-notarization.json")
            run("/usr/bin/codesign", "--verify", "--strict", pending)
            run("/usr/bin/hdiutil", "verify", pending)
            run("/usr/sbin/spctl", "--assess", "--type", "open", "--context", "context:primary-signature", "--verbose=2", pending)
            verify_signature(app, binaries, self.args.team_id)
            final = output / name
            pending.rename(final)
        finally:
            shutil.rmtree(image_root)
        manifest = {"bundle_id": BUNDLE_ID, "version": info["CFBundleShortVersionString"], "team_id": self.args.team_id,
                    "architecture": architecture, "dmg": final.name, "sha256": file_sha256(final),
                    "source_commit": run("git", "-C", ROOT, "rev-parse", "HEAD", capture=True).strip(),
                    "source_dirty": bool(run("git", "-C", ROOT, "status", "--porcelain", capture=True).strip())}
        (output / "release.json").write_text(json.dumps(manifest, indent=2) + "\n")
        (output / "PREPARED-ONLY.txt").unlink()
        archive.unlink()
        print(f"Verified customer package: {final}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("check", "build", "prepare", "release", "verify", "export"))
    parser.add_argument("--prefix", type=Path, default=ROOT / "build-dev/macos-clang-arm64")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-release/macos")
    parser.add_argument("--seed-build-dir", type=Path, default=ROOT / "build-dev/macos-clang-arm64/build/owncloud/owncloud-client/work/build")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build-release-artifacts/macos" / ("run-" + datetime.now().strftime("%Y%m%d-%H%M%S")))
    parser.add_argument("--sdk", type=Path)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--allow-debug", action="store_true", help="Only for prepare; never for a customer release")
    parser.add_argument("--app", type=Path, help="App to inspect with verify")
    parser.add_argument("--release-dir", type=Path, help="Finished release directory to verify and export for website hosting")
    parser.add_argument("--download-base-url", default="./", help="HTTPS download directory URL or website path (export only)")
    parser.add_argument("--team-id", default=os.environ.get("UDRIVE_TEAM_ID", ""))
    parser.add_argument("--identity", default=os.environ.get("UDRIVE_SIGN_IDENTITY", ""), help="Developer ID Application certificate SHA-1 fingerprint")
    parser.add_argument("--notary-profile", default=os.environ.get("UDRIVE_NOTARY_PROFILE", ""), help="Name of a notarytool profile stored in Keychain")
    args = parser.parse_args(argv)
    try:
        workflow = Workflow(args)
        workflow.tools(deploy=args.command not in ("verify", "export"))
        if args.jobs < 1:
            raise ReleaseError("--jobs must be positive")
        if args.command == "export":
            if not args.release_dir:
                raise ReleaseError("Export requires --release-dir pointing to a finished signed and notarized release.")
            export_download(args.release_dir, args.output_dir, args.team_id, args.download_base_url)
        elif args.command == "check":
            print("Local CMake and Qt deployment tools found.")
            workflow.signing_identity()
            print("Developer ID signing identity and notarization tools found. Profile access is checked on submission.")
        elif args.command == "build":
            workflow.build_app()
        elif args.command == "verify":
            if not args.app or not re.fullmatch(r"[A-Z0-9]{10}", args.team_id):
                raise ReleaseError("verify requires --app and --team-id")
            _, binaries = validate_bundle(args.app.resolve())
            verify_signature(args.app.resolve(), binaries, args.team_id)
            run("/usr/bin/xcrun", "stapler", "validate", args.app.resolve())
            run("/usr/sbin/spctl", "--assess", "--type", "execute", "--verbose=2", args.app.resolve())
        else:
            identity = workflow.signing_identity() if args.command == "release" else None
            if args.command == "release" and args.allow_debug:
                raise ReleaseError("--allow-debug cannot be used for a release")
            if not args.skip_build:
                workflow.build_app()
            if args.command == "release":
                workflow.release(identity)
            else:
                workflow.prepare()
    except (ReleaseError, OSError, ValueError, plistlib.InvalidFileException) as error:
        print(f"Release stopped: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
