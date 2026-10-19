# GitHub workflows are disabled

All five workflow definitions have been renamed from `.yml` to `.yml.disabled`.
No active `.yml` or `.yaml` workflow files remain in this directory.

Commit and push these changes to the organization's repository for the change
to take effect on GitHub. Local app builds are unaffected. Existing remote
workflow runs are not canceled by these local changes.

The PowerShell scripts and `craft_override.ini` are retained as supporting
files; they do not run automatically on their own.

To re-enable a workflow later, remove the `.disabled` suffix from its filename,
then commit and push that change.
