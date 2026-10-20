# UDrive Windows installer

The Windows build requires a **Windows x64 machine**, Git, Python 3.12+, and
Visual Studio 2022 with the Desktop development with C++ workload and Windows SDK.
An Apple Silicon Mac with the project's macOS Qt libraries cannot run this build.
Use a Windows PC, Windows VM, or the included GitHub Actions Windows runner.

## Build on a Windows PC

Copy or check out this UDrive source on Windows. From its root directory:

```powershell
python admin/windows/build.py --work-root C:\udrive-build
```

The helper installs the pinned Craft dependencies, compiles this local checkout,
and creates an NSIS `.exe` installer with the UDrive name, icon, shortcuts and
publisher metadata. It uses an isolated build folder. Automatic updates and
Finder/shell integration are disabled for this initial installer.

On a **clean Windows test machine**, add `--smoke-test` to install temporarily,
check the bundled runtime and x64 app version without the Craft installation
available, then uninstall. It refuses to replace an existing UDrive installation.
It does not sign in to accounts or start file synchronization.

Outputs appear under `build-release-artifacts/windows/run-.../`:

- `UDrive-VERSION-Windows-x64-TESTING-ONLY.exe` — unsigned NSIS test installer.
- `SHA256SUMS` — SHA-256 checksum.
- `download.json` — version, size, architecture and installation-test result.
- `TESTING-ONLY.txt` — package status.

This is an unsigned test package. Production Windows distribution needs the
publisher's Windows code-signing setup and clean-machine acceptance testing.
Apple Developer credentials are not used for Windows.

## Build from this Mac using GitHub Actions

The `.github/workflows/windows-build.yml` workflow runs on a Windows 2022 x64
runner. It builds the installer, checks installation and startup, and uploads
the installer/checksum as the `UDrive-Windows-x64-testing` artifact. It does not
publish a release or change the download website.

The workflow must first be committed and pushed to the UDrive repository.
Changes to these Windows build files can run it through a pull request. Once
the workflow is on the default branch, it is also available under GitHub's
Actions tab using **Run workflow**, or with:

```bash
gh auth login
gh workflow run windows-build.yml --repo unity-workspace-org/udrive-desktop
```

GitHub authentication is needed to upload/run the workflow and retrieve its
artifact. It is not needed by the download website or by customers downloading
an installer. GitHub runner usage follows the repository's Actions plan.

## Add the finished installer to the website

Only enable the Windows download after a real installer has been produced and
tested. Upload that `.exe` and its checksum to the download website alongside
the Mac DMG. A Mac `.app` or `.dmg` cannot substitute for a Windows installer.

The separate website is `/Users/tochratana/Work/web-test-host`. Its Windows
control remains disabled until the actual Windows artifact is available.

## Packaging-policy tests

These tests run on Mac, Linux or Windows without downloading dependencies:

```bash
python3 -m unittest discover -s test -p test_windows_release.py -v
```
