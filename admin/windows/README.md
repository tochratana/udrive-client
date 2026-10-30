# Udrive Windows installer

The Windows build requires a **Windows x64 machine**, Git, Python 3.12+, and
Visual Studio 2022 with the Desktop development with C++ workload and Windows SDK.
An Apple Silicon Mac with the project's macOS Qt libraries cannot run this build.
Use a Windows PC, Windows VM, or the included GitHub Actions Windows runner.

## Build on a Windows PC

Copy or check out this Udrive source on Windows. From its root directory:

```powershell
python admin/windows/build.py --work-root C:\udrive-build
```

The helper installs the pinned Craft dependencies, compiles this local checkout,
and creates an NSIS `.exe` installer with the Udrive name, icon, shortcuts and
publisher metadata. It uses an isolated build folder. Automatic updates and
Finder/shell integration are disabled for this initial installer.

On a **clean Windows test machine**, add `--smoke-test` to install temporarily,
check the bundled runtime and x64 app version without the Craft installation
available, then uninstall. It refuses to replace an existing Udrive installation.
It does not sign in to accounts or start file synchronization.

Outputs appear under `build-release-artifacts/windows/run-.../`:

- `Udrive-VERSION-Windows-x64.exe` — versioned unsigned NSIS installer.
- `SHA256SUMS` — SHA-256 checksum.
- `download.json` — version, size, architecture and installation-test result.
- `RELEASE-NOTES.md` — installation requirements, signing status and checks.

This is an unsigned installer. Windows code signing is recommended for public
distribution, but is not required to offer this `.exe` for download. Windows may
show SmartScreen or unknown-publisher warnings. Run clean-machine acceptance
checks, including sign-in and synchronization, before distributing it to customers.
Apple Developer credentials are not used for Windows.

## Dependency bootstrap compatibility

The helper checks out the exact Craft core and blueprint repository commits
from `.craft.shelf` before running Craft. CraftMaster receives the same core
revision on every invocation. This prevents newer blueprint metadata (including
the `native` platform token) from being parsed by the older pinned runtime.

Unshelving uses `udrive-pinned.craft.shelf` inside the build directory. This copy
keeps the dependency versions/revisions and omits the repository-add step that
would otherwise fetch `master` before applying those pins. The project's shelf
is not modified by the helper. Reused checkouts preserve local overlays; if Git
reports conflicting local edits, use a fresh `--work-root`.

If GitHub Actions previously failed with `KeyError: 'native'`, run the workflow
against a commit containing this bootstrap fix. Rerunning the old commit uses
the old helper and does not apply the fix.

## Build from this Mac using GitHub Actions

The `.github/workflows/windows-build.yml` workflow runs on a Windows 2022 x64
runner. It builds the installer, checks installation and startup, and uploads
the installer/checksum as the `Udrive-VERSION-Windows-x64` artifact. Manual and
pull-request builds do not publish a release or change the download website.
Existing workflow runs retain their old artifact names; start a new run from
the updated commit to get versioned names.

If a run fails, download `Udrive-Windows-build-diagnostics` from its Artifacts
section. It contains the build transcript, installed-file list (when installation
was reached), and installer scripts. An uninstall failure is reported separately
so it cannot hide the original installation or startup error.

The workflow must first be committed and pushed to the Udrive repository.
Changes to these Windows build files can run it through a pull request. Once
the workflow is on the default branch, it is also available under GitHub's
Actions tab using **Run workflow**, or with:

```bash
gh auth login
gh workflow run windows-build.yml --repo tochratana/udrive-client
```

GitHub authentication is needed to upload/run the workflow and retrieve its
artifact. It is not needed by the download website or by customers downloading
an installer. GitHub runner usage follows the repository's Actions plan.

## Publish a versioned GitHub Release

The version comes from `VERSION.cmake` (currently `7.1.1`). After committing and
pushing the updated workflow and build helper, tag that commit:

```bash
git tag -a windows-v7.1.1 -m "Udrive 7.1.1 for Windows"
git push ratana windows-v7.1.1
```

In this checkout, `ratana` points to `tochratana/udrive-client`; check `git remote -v`
before pushing from another checkout. The upstream `origin` is not the Udrive fork.
Use a new version in `VERSION.cmake` and a matching tag for each subsequent release.
Do not move or replace an existing published tag.

Pushing `windows-v*` starts the Windows workflow. The helper rejects a version
mismatch, a dirty checkout or a release without the installation smoke test.
After the build and checks pass, a separate publishing job verifies the checksum
and creates a regular GitHub Release named **Udrive VERSION for Windows** with
the `.exe`, `SHA256SUMS`, `download.json` and release notes. The release starts as
a draft during asset upload and is published only after upload succeeds.
If publishing fails and leaves a draft, inspect that draft and finish publishing
it from GitHub after verifying all four assets; rerunning will not overwrite an
existing release. The repository must allow the publishing job's `GITHUB_TOKEN`
to write repository contents; no personal token secret is required.

Tagged downloads use `channel: stable`; other builds use `channel: build`.
Both retain the true `signature: unsigned` status. A release tag does not sign
the executable or imply that every application feature was tested.

Users download the installer from the repository's **Releases** page. This
workflow does not upload to the Udrive website. For a private repository, release
downloads also require repository access; use your download website for public
distribution.

## Add the finished installer to the website

Only enable the Windows download after a real installer has been produced and
tested. Upload that `.exe` and its checksum to the download website alongside
the Mac DMG. A Mac `.app` or `.dmg` cannot substitute for a Windows installer.

Keep the website's Windows download control disabled until the actual Windows
artifact is available and has passed the required installation checks.

## Packaging-policy tests

These tests run on Mac, Linux or Windows without downloading dependencies:

```bash
python3 -m unittest discover -s test -p test_windows_release.py -v
```
