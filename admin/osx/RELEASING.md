# UDrive macOS releases

The local release workflow is `python3 admin/osx/release.py`. It does not use
GitHub Actions, change the development app, or weaken Keychain access controls.
It builds a separate Release app, bundles its dependencies, signs nested code
before the main app, notarizes the app and disk image, staples Apple's tickets,
and verifies the package before naming it as a customer release.

This workflow is currently exercised on Apple Silicon with the project's Craft
dependencies and macOS 13 or later as the deployment target. Finder shell
integration and automatic updates are disabled in these releases. Other
architectures and a clean customer Mac still require separate verification.

## One-time Apple account setup

An Apple Developer Program account is Apple's publisher account for distributing
Mac apps. A **Developer ID Application** certificate identifies the publisher of
apps downloaded outside the Mac App Store. It is different from a user's UDrive
login or the GitHub organization's Actions allowance.

1. The publisher enrolls through the [Apple Developer Program](https://developer.apple.com/programs/enroll/).
2. Create a [Developer ID Application certificate](https://developer.apple.com/help/account/certificates/create-developer-id-certificates/)
   with its private key on the release Mac. Keep that private key in Keychain;
   do not put certificate exports, passwords, or API private keys in this repo.
3. Find the publisher's Team ID in the Apple Developer account. Keep the same
   publisher and the existing app identifier `com.unity-workspace.udrive` across
   updates. The script rejects mismatched certificates and app identifiers.
4. Store a notarization profile locally. The following command asks for the
   app-specific password interactively and stores credentials in Keychain:

   ```bash
   xcrun notarytool store-credentials UDrive-notary \
     --apple-id 'YOUR_APPLE_ACCOUNT_EMAIL' \
     --team-id 'YOUR_TEAM_ID'
   ```

   Follow [Apple's notarization setup](https://developer.apple.com/documentation/security/customizing-the-notarization-workflow)
   to create the app-specific password, or use a supported App Store Connect
   API-key profile. The release script takes only the profile's name. It never
   takes or prints your Apple password or API private key.

List locally available signing identities:

```bash
security find-identity -v -p codesigning
```

Use the 40-character fingerprint beside **Developer ID Application**, not Apple
Development, a distribution certificate for the App Store, or an ad-hoc identity.
Set these non-secret selectors in your terminal:

```bash
export UDRIVE_TEAM_ID='YOUR_10_CHARACTER_TEAM_ID'
export UDRIVE_SIGN_IDENTITY='YOUR_40_CHARACTER_CERTIFICATE_FINGERPRINT'
export UDRIVE_NOTARY_PROFILE='UDrive-notary'
python3 admin/osx/release.py check
```

`check` validates the local signing identity and tools. The notary profile is
authenticated when submitting to Apple. Missing credentials stop `release`
before it builds, signs, or uploads anything. Keychain may ask the publisher to
authorize use of the signing private key; customers do not perform this setup.

## Build and package

From the repository root:

```bash
# Builds without needing an Apple Developer account.
python3 admin/osx/release.py build

# Rebuilds, bundles, signs, notarizes and verifies the customer package.
python3 admin/osx/release.py release
```

Defaults use dependencies in `build-dev/macos-clang-arm64`, a separate build in
`build-release/macos`, and a new timestamped directory under
`build-release-artifacts/macos`. These output directories are ignored by Git.
`--prefix`, `--build-dir`, `--sdk` and `--jobs` support other local setups. The
default configuration reuses the development build's SDK selection, because
the pinned Qt libraries need an SDK containing AGL. It does not reconfigure the
development build. Python 3.9 or later, Apple command-line tools, and the existing
Craft dependency installation are required.

For an already compiled Release app, use `release --skip-build`. Debug builds
are rejected for customer releases. Outputs are never overwritten; choose a
new `--output-dir` when retrying a failed run.

Successful output includes:

- A signed, notarized, stapled `udrive.app`.
- `UDrive-VERSION-ARCH.dmg`, with an Applications shortcut for drag-and-drop installation.
- `release.json` with the app identifier, publisher Team ID, source commit,
  working-tree status, architecture, and disk-image SHA-256.
- Apple's app and disk-image notarization responses.

A failure retains diagnostics and a `PREPARED-ONLY.txt` marker. A prepared app
or a `.pending.dmg` is not a finished customer release. Notarization rejection,
missing libraries, external runtime paths, architecture mismatches, invalid
signatures, or failed Gatekeeper checks stop the process.

To test dependency packaging without an Apple account:

```bash
python3 admin/osx/release.py prepare --skip-build
```

This prepares an app for inspection only; it is not distribution-signed. Debug
packaging experiments may use `prepare --skip-build --allow-debug --build-dir
PATH`, but `--allow-debug` is never accepted for a customer release.

## Export the production download for your website

After `release` finishes successfully, export the completed release to a new
folder for HTTPS hosting:

```bash
python3 admin/osx/release.py export \
  --release-dir '/absolute/path/to/finished-release' \
  --team-id "$UDRIVE_TEAM_ID" \
  --download-base-url 'https://downloads.example.com/udrive/7.1.1/' \
  --output-dir 'build-release-artifacts/website-7.1.1'
```

Replace the example URL with the directory URL on your website. It must use
HTTPS and must not contain credentials or temporary access tokens. A site path
such as `/downloads/udrive/7.1.1/` also works. Omitting `--download-base-url` uses
links relative to the exported folder. Export runs on the release Mac; it needs
Apple's verification tools, but does not need Craft, Qt, or signing credentials.

The command verifies the release manifest and disk-image checksum, the expected
Developer ID publisher, the stapled notarization ticket, and Gatekeeper
acceptance. It mounts the DMG read-only to verify the app actually inside it,
including its version, architecture, signature and notarization. Prepared,
testing-only, ad-hoc-signed, or modified images cannot become production downloads.
If any check fails, no website download folder is created.

The exported folder contains only these public files:

- `UDrive-VERSION-ARCH.dmg`: the verified installer.
- `SHA256SUMS`: its SHA-256 checksum.
- `download.json`: version, architecture, minimum macOS, file size, checksum,
  source revision and download URL for a website to consume.
- `download-link.html`: an HTML download button/link to paste into your website.

Upload the folder's contents to the website directory used in
`--download-base-url`, then add the generated link to your existing download
page. The folder is ready for a static web server, object storage, or a CDN;
the desktop client does not need to run on the website's server. Serve the DMG
with `Content-Type: application/x-apple-diskimage` and preferably
`Content-Disposition: attachment`. Keep versioned download URLs immutable.

The exporter prepares local files; it does not upload files or change your live
website. Use your website's deployment process to publish them, then download
the hosted DMG onto a clean Mac and complete the installation and upgrade checks
below. Uploading only the exported files keeps signing/notarization logs and
staging archives off the public website. This is a manual download endpoint;
it does not enable automatic updates.

## Verification and Keychain acceptance checks

The script signs the app with hardened runtime, a secure timestamp and its
stable identifier. Only the main Qt Quick app receives the JIT entitlement;
the PDF helper receives its own stable identifier and no extra entitlements.
Versioned dylib aliases resolve to one library image, avoiding duplicate Qt
and Objective-C registrations when deployment tools copy both filenames.
It does not disable library validation, allow unsigned executable memory, use
`--deep` to sign, or change Keychain permissions. Dependencies are rewritten to
bundle-relative paths and every Mach-O file is checked before signing.

Verify a finished app independently:

```bash
python3 admin/osx/release.py verify \
  --app '/absolute/path/to/udrive.app' \
  --team-id 'YOUR_TEAM_ID'
```

Run the release policy tests without a signing identity or network access:

```bash
python3 -m unittest discover -s test -p test_macos_release.py -v
```

Before publishing, use a clean Mac or a separate test user and record results:

| Scenario | Expected result |
| --- | --- |
| Install from the downloaded DMG | Gatekeeper accepts the identified, notarized publisher. |
| First account sign-in | Browser OAuth completes; the refresh token is stored in Keychain. An initial authorization may appear. |
| Quit and reopen the identical release with the login Keychain unlocked | Account remains signed in; no repeated app-identity authorization. |
| Install the next release signed by the same publisher | Existing accounts and tokens work; no authorization caused by changed signing identity. |
| Upgrade from an ad-hoc development build | A one-time Keychain reauthorization may be necessary. |
| Lock the login Keychain | macOS may request its password; this protection remains enabled. |
| Open the sidebar, a PDF, and server files | Bundled QML, the PDF helper, plugins and TLS work without the development directory or Qt installed. |

The current Keychain service name stays `ownCloud` to retain existing accounts
and tokens. Changing that name does not fix signing and would require a separate
credential migration. Signing alone does not guarantee that every Keychain
policy or locked-keychain prompt disappears. Real signing, notarization and
these installation/upgrade tests require the publisher's certificate and test
Mac; unsigned packaging tests cannot substitute for them.

References: [Apple code-signing requirements](https://developer.apple.com/documentation/technotes/tn3127-inside-code-signing-requirements),
[Apple distribution signing](https://developer.apple.com/documentation/xcode/creating-distribution-signed-code-for-the-mac/),
[Qt 6.8 deployment](https://doc.qt.io/qt-6.8/macos-deployment.html).
