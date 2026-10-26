# Udrive GitHub workflows

Two workflows are active in this checkout:

- `production-policies.yml` runs packaging and production-check policy tests
  on pushes, pull requests, or manual dispatch. It does not compile the desktop
  application, use signing credentials, or publish a release.
- `windows-build.yml` builds an unsigned Windows test installer on manual
  dispatch or pull requests changing its packaging files. Its installation
  smoke test does not validate signed production distribution.

The five upstream workflows retain the `.yml.disabled` suffix and do not run.
Workflow edits take effect remotely after they are committed and pushed.
Local app builds are unaffected.

The PowerShell scripts and `craft_override.ini` are retained as supporting
files; they do not run automatically on their own.

To re-enable an upstream workflow, review its services and credentials first,
then remove the `.disabled` suffix, commit, and push the change. A green policy
check does not replace platform builds, application tests, or release acceptance.
