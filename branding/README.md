# UDrive desktop branding

This directory is selected automatically by `THEME.cmake`. The default identity
is UDrive / Unity Workspace, with green and teal accents and the supplied UDrive
logo. The original transparent PNG is `theme/universal/udrive-icon.png` (the
uploaded file had a `.jpg` extension, but its contents were already a PNG).
Application and sidebar PNG sizes are under `theme/colored`. Color menu bar
icons embed the supplied logo unchanged, with distinct sync status badges.
Monochrome icons use a simplified outline of its layered papers and wrapping
folder. Badges have transparent interiors so their status symbols remain distinct
when macOS uses the icon's alpha as a template mask.

`render-assets.cpp` resizes the original logo and composes the sign-in wordmark
using Qt, preserving transparency. To regenerate assets on macOS, from the
repository root:

```sh
udrive_prefix="$PWD/build-dev/macos-clang-arm64"
clang++ -std=c++17 -include arm_acle.h branding/render-assets.cpp \
  -F "$udrive_prefix/lib" \
  -I "$udrive_prefix/lib/QtCore.framework/Headers" \
  -I "$udrive_prefix/lib/QtGui.framework/Headers" \
  -framework QtCore -framework QtGui \
  -Wl,-rpath,"$udrive_prefix/lib" -o /tmp/udrive-render-assets
QT_QPA_PLATFORM=offscreen /tmp/udrive-render-assets branding/theme
python3 branding/render-tray-icons.py
```

The visible app name, executable (`udrive`) and macOS bundle ID are branded.
UDrive stays a regular macOS app during startup and while its window is hidden;
its Dock entry is not removed and recreated as the UI opens. The development
helper uses macOS Launch Services (`open -W`) to launch the app bundle, explicitly
forwarding the localhost HTTP flag with `--env`. `MacDockTest` verifies the native
activation policy through window creation, hide, and reopen using Cocoa.
During development, `UDriveTheme` retains the existing ownCloud configuration
directory, config filename and Keychain service, so existing accounts and sync
folders can be reused. This version shares settings with the upstream client;
do not run both against the same sync folders. A separately distributed app
should adopt its own settings identity with an explicit migration plan.

Automatic updates are disabled until UDrive has its own signed update feed.
OAuth settings remain compatible with the desktop client already registered
in your Keycloak realm. Open on Web uses the selected item's sign-in link,
falling back to the selected space's web URL.

For an HTTP-only local development server, quit any running UDrive instance,
then launch the Debug build with `bash scripts/dev-macos.sh run-local`. This
sets `UDRIVE_ALLOW_LOCAL_HTTP=1` and permits HTTP for `localhost`, `127.0.0.1`
and `::1` only. Remote server and authentication addresses still require HTTPS.
Release builds ignore this flag. A local account with an HTTP identity provider
also requires this mode when refreshing tokens. The server and identity provider
must advertise addresses reachable from the Mac; Docker service names are not
loopback addresses. Ordinary `run` keeps HTTPS requirements enabled.

The UDrive workspace uses a compact header with the logo, folder search,
Activity, Settings, and an account menu. Click the initials in the upper-right
corner to open the account popup: cards show avatars or initials, the display
name, and a separate server address. The active account has a green check.
The popup supports Tab/arrow navigation, Escape, scrolling for long account
lists, and stays inside the current screen. Account changes dismiss it so stale
entries cannot remain open. Use the footer to add an account or manage sync. Each account has its
own search field and filter; removing an account also removes its header field.

Document, spreadsheet, presentation and drawing files use the supplied colored
icons in both table and tile views. Transparent PNGs are embedded from
`src/resources/filetypes`; see its README for extension mappings and asset
preparation. File-type icons do not add editing or preview support for those formats.

`src/gui/qml/UDriveSidebar.qml` and `UDriveNavigationButton.qml` provide the
rounded sidebar, New menu, file sections, Sync, Quit, and a storage meter based
on the selected account's personal-space quota. Missing quota is reported as
unavailable. Sidebar icons are scalable SVG resources in `src/gui/icons`;
file-type colors and native tool icons are drawn by `UDriveIcons`.

The file table includes single-row selection indicators, Favorite indicators,
copy-sign-in-link controls, and a menu on each row. Keyboard users can use the
Open, Download, and Actions controls that appear when a row is selected. The
sidebar and content follow the system light/dark palette. `SettingsDialog`
retains the upstream toolbar when building with a different application name.
Sync management, Finder, and Open on Web remain available alongside the file
browser. The redesign does not add bulk selection, global search, or new sharing
APIs.

## Languages

UDrive offers exactly three UI languages: English (`en_US`), Khmer (`km`),
and Simplified Chinese (`zh_CN`). Only these catalogs are embedded in UDrive
builds. The unbranded build retains its full translation list. Language names
remain recognizable in their native script. A saved regional locale is mapped
to one of the three choices; unsupported old preferences use English. With no
saved preference, the system's preferred UI language is mapped the same way.
Selecting a language saves its canonical locale; fully quit and reopen UDrive
to apply it.

The Khmer and Chinese catalogs include the UDrive workspace, file actions and
messages, PDF preview, account and sign-in screens, general/network settings,
and common sync controls. Additional upstream diagnostics and platform-owned
dialog text can still fall back to English. `TranslationsTest` checks packaged
catalogs, locale mappings, translation contexts, placeholders, and item counts.
`UDriveFileBrowserTest` checks the three-item selector and saved preferences.

## Desktop file browser

`src/gui/udrivefilebrowser.cpp` browses the selected account's Personal and
non-personal spaces (including Shares) using the existing Graph drive discovery
and authenticated WebDAV jobs. The main window opens Personal by default; Sync
in the sidebar opens the existing sync-management screen.

The first version supports folder navigation, name filtering within the current
folder, name/size/date sorting, opening existing synced files, and downloading
remote files to a location chosen in the Save dialog. Downloads stream into an
atomic `QSaveFile`; cancellation, server errors and truncated responses preserve
an existing destination. A change of space cancels an active download.

Image rows now replace the file icon with an authenticated server thumbnail.
Only visible raster-image rows request previews, with two request/decode slots
shared across the app. Running decoders retain their slots across navigation,
sign-out and account/browser destruction;
the client requests 64px list or 256px tile thumbnails using the DAV `preview=1` API and the listed
ETag as its cache version. The per-account 8 MiB memory cache never writes image
previews to disk and is cleared on sign-out. Navigation and hiding cancel pending
results. Responses over 512 KiB, non-raster formats, invalid images and decoded
dimensions over 2048px fall back to the file icon. Missing or disabled server
previews also retain that icon; full original photos are never fetched for icons.

The view button next to Refresh offers Condensed table view, Default table view
and Tiles view. The views share selection, filtering, sorting, account-scoped
file actions and navigation. Tiles show larger previews, file names, sizes and
a per-file actions menu. The last selected mode is saved for future windows;
each account still owns its own file model, thumbnail cache and requests.

Select all beside Sort by selects the current filtered results, including rows
outside the viewport. Its checkbox shows none, some or all selected. Row and tile
checkboxes toggle individual items; the standard Select All shortcut also works
while the file view is focused. Selection survives switching view modes and
sorting, and clears on refresh or navigation. File actions currently require one
selected item; multiple selections show a hint and disable those actions. A
per-file actions menu selects the clicked item explicitly.

The Sort by menu is available in all three views: Recent, A-Z, Z-A, Newest,
Oldest, Largest and Smallest. Table headings and the menu stay in sync; folders
remain first and missing dates/sizes sort last. Newest/Oldest use modified dates.
Recent uses files opened in this desktop app, with unopened files following by
modified date. The selected order is saved, and up to 256 recent file identifiers
(hashes of the space URL and relative path) and timestamps are stored per account
UUID. File names, file contents and tokens are not stored in this history.

The New menu creates folders and uploads a selected file. Actions provides
rename, same-folder copy, move to a relative folder in the same space, Favorite
changes, copying the server-provided sign-in link, and moving an item to Deleted
files. Uploads use `If-None-Match: *`; copy, move and restore use `Overwrite: F`.
Rename/move/delete also send the listed ETag when available. A write rejected
with HTTP 401 can retry once after credential refresh, including writes queued
during an earlier refresh. Redirects, HTTP 403 and ambiguous failures are never
replayed automatically, because a response can be lost after a successful write.
HTTP 207 mutation responses are checked for individual failures. Partial or
unconfirmed results are reported as failures with a reminder to refresh, since
some changes may already have reached the server.

Favorites uses the server's DAV REPORT and PROPPATCH APIs. Results are mapped to
known account spaces before any file operation. Both `/remote.php/dav/` and
`/dav/` response paths are supported, including the account's personal files
namespace and space references identified by `oc:spaceid`. Absolute response
URLs must belong to the account or an advertised space. Mapping preserves
literal percent, question-mark and hash characters in file names. Shares has its own sidebar page.
Deleted files lists the chosen drive's trash bin and restores to the original
location. Restore requires the parent folder to exist and never replaces an
existing destination. Delete permanently removes a selected trash item after
an explicit confirmation. Both deletion prompts use asynchronous, parent-owned
dialogs; closing or removing their browser cannot leave a stack-owned dialog
inside a nested event loop. Confirmation revalidates the account, space, item,
section and listed ETag. Changed or reloaded items require a new confirmation,
and deletion sends `If-Match` when an ETag is available. Hiding the browser or
signing out dismisses the prompt. Permanent deletion cannot be undone.
Reported permissions disable unsupported write actions; the server remains the
authority when deciding whether an operation is allowed.

Search currently filters this folder. A server-wide Recent page, global search, public-link creation,
sharing-permission management, and other file previews remain on the web. Copy
sign-in link preserves existing access permissions. Upload cancellation stops
the request; refresh to verify server state if it had already accepted the file.

API references: [ownCloud WebDAV](https://owncloud.dev/apis/http/webdav/),
[Favorites client](https://github.com/owncloud/web/blob/master/packages/web-client/src/webdav/listFavoriteFiles.ts),
[space/trash paths](https://github.com/owncloud/web/blob/master/packages/web-client/src/helpers/space/functions.ts),
and [restore client](https://github.com/owncloud/web/blob/master/packages/web-client/src/webdav/restoreFile.ts).

The integration test `test/testudrivefilebrowser.cpp` uses a localhost mock
Graph/WebDAV server and dummy credentials. With tests enabled, build
`UDriveFileBrowserTest` and run it with `QT_QPA_PLATFORM=offscreen`. It covers
authenticated listing, path encoding, stale-response cancellation, navigation,
filtering, downloading, failed/truncated saves and cancellation. It also covers
streamed uploads, upload conflicts/cancellation, folder creation, rename/copy/move,
server Favorites (including a failed property response), delete/restore,
restore conflicts, Shares, and read-only permissions. It never writes to a live
server or uses real account credentials.
Regression cases also cover failed and malformed HTTP 207 responses for file
operations and Favorites, plus distinct monochrome status masks at tray sizes.


On macOS, double-click a PDF or select **Open PDF** to read it inside UDrive.
The preview supports page navigation, Fit page / Fit width, and 50–300% zoom.
Synced PDFs use a read-only snapshot of the local file; cloud PDFs are downloaded
through the selected account into a private temporary directory. Closing the
preview, switching accounts/sections, signing out or quitting normally removes
the temporary copy. Folder activation also cancels unfinished PDF downloads.
The bundled `udrive-pdf-renderer` helper opens, inspects and renders the PDF using
macOS CoreGraphics in a separate process, with a 4096-pixel raster bound and
bounded raw RGBA output. The helper keeps one immutable snapshot across paging
and zoom. Each render has a five-second time limit; timeout, preview closure,
replacement, account hiding/sign-out and destruction kill and reap the helper.
UDrive then offers Download when a PDF cannot be previewed. This process boundary
is not an OS sandbox or a hard memory limit. It executes no PDF links, scripts or
embedded actions. No new Qt module or runtime installation is needed.
The first version previews PDFs up to 100 MiB; password-protected PDFs still need
Download and an external viewer. Text selection/search, annotations and printing
are not included. Other desktop platforms retain their existing open/download
behavior. The browser tests additionally exercise real two-page and rotated PDF
fixtures, zoom, local/cloud opening, stale render cancellation, temporary file
permissions/cleanup, account hiding/sign-out, invalid/oversized documents,
expensive compressed/nested PDFs, timeout recovery and helper process cleanup.


UDrive action buttons and file rows use the rounded outline icons defined in
`src/gui/udriveicons.h`. They render at the current device pixel ratio and follow
light/dark and disabled palettes. Button spacing, centered dropdown chevrons,
menu rows and the space/PDF zoom selectors are styled in
`SettingsDialog::updateUDriveAppearance`; the chevron SVGs are bundled through
`src/gui/udriveicons.qrc`.


On macOS, once the UDrive window has been opened, closing or hiding it keeps
the running app in the Dock. Clicking the Dock icon reopens the existing window;
use Quit UDrive to exit. The original ownCloud menu-bar-only activation behavior
is retained for builds that do not use the UDrive branding.


Folder navigation uses clickable breadcrumbs instead of an Up button or a plain
slash-delimited path. Click the space name to return to its root, or a parent
folder to open that level. Deep paths keep the most recent parent visible and
place earlier folders in the “…” menu. Long labels are shortened on screen while
retaining the full name in tooltips and accessibility metadata. Navigation stays
within the current account/space and cancels stale listings and downloads.


`render-tray-icons.py` composes all six status SVGs in the colored, dark, black
and white themes. It embeds the existing 128-pixel logo in the color variants,
so the Qt SVG renderer requires no external image paths. Choose color icons by
turning off **Use monochrome icons** in Settings. The existing status-mask test
checks that all monochrome badges remain distinguishable at 22 and 32 pixels.

## Authentication and diagnostic safeguards

All requests through an account's authenticated network manager require HTTPS, including
server-advertised DAV roots, space images and explicitly supplied credentials.
Debug loopback HTTP requires `run-local` and a loopback HTTP account; a remote
HTTPS account cannot use that exception. Account requests retain manual redirect
handling. Release builds reject HTTP even when the development flag is set.

HTTP diagnostics always redact credential headers, cookies and OAuth fields in
form/JSON bodies. URL userinfo, queries and fragments are omitted. Malformed JSON
and oversized bodies are omitted; ordinary DAV XML and non-secret JSON remain
available for troubleshooting. The legacy `OWNCLOUD_HTTPLOGGER_NO_REDACT`
override no longer disables credential protection. These logs still include file
names, server addresses and other metadata, so retain the existing log privacy
warning.

Logging out invalidates pending authentication completions and refresh retries.
Old Keychain reads, token refreshes and interactive logins cannot restore tokens
after logout or replace a newer login. Malformed stored credentials finish with
an error instead of leaving login waiting.

`HttpLoggerTest` and `CredentialSecurityTest` cover these boundaries using dummy
credentials and mock replies. `LocalDevelopmentUrlReleaseTest` checks the URL
policy with `QT_NO_DEBUG`. Native Keychain integration is a separate platform
test; the mocked tests never read the user's stored secrets.
