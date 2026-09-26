# Rotatrix maintained fork workflow

This checkout is development work on `rotatrix/work/version_3.0.0-alpha11`,
based on the official upstream tag `version_3.0.0-alpha11`. It is not yet a
maintained release. The work branch is the temporary GitHub default until the
first maintained branch is ready. The separate legacy 2.9.6 development branch
is outside this migration.

## Branch lifecycle

- `rotatrix/work/*`: disposable development; rebasing, squashing and deliberate
  force pushes are allowed. Preserve other people's work when doing so.
- `rotatrix/<upstream-tag>`: clean maintained patch stack. Mirror the exact
  upstream tag, including prefixes. Once published or used by contributors,
  append commits; never rewrite its existing history.
- Maintain the current upstream release and at most one previous release where
  useful. Use official upstream release tags as bases whenever possible.

For a new upstream version, fetch official tags from the `upstream` remote and
start `rotatrix/work/<tag>` from that tag. Port only the downstream patches,
using the previous upstream/Rotatrix pair as a reference. Use `git range-diff`
to compare the old and new patch stacks when helpful.

When this version is ready, clean up the downstream commits on the work branch
and create `rotatrix/version_3.0.0-alpha11`. Publish that maintained branch and
set it as GitHub's default. Do not promote it merely to obtain test builds.
For later fixes, start a new work branch from the maintained branch, squash only
the new work, and append the resulting clean commits to the maintained branch.

Contributors branch from the relevant maintained branch and open PRs directly
against it. Their own branch serves as their work branch. During this initial
development phase, coordinate changes against the temporary default work branch;
automatic PR builds target maintained branches only. Work-branch pushes and
manual dispatch provide the current test builds.

## CI and packages

Normal CI runs on pushes to `rotatrix/**` and PRs targeting `rotatrix/*` (a
single path segment, excluding `rotatrix/work/*`). It builds Windows x64,
macOS ARM64 and Linux x64, runs the OpenAxis tests and package smoke checks,
and uploads packages/checksums/logs with source SHAs and 14-day retention.
Routine testing creates no release tags or draft releases.

Prefer upstream CI/build/dependency/package commands wherever practical. This
upstream tag delegates its builds to `Prusa-Development/PrusaSlicer-Actions`,
which is unavailable to this fork's current credentials (GitHub returns 404).
Keep those upstream wrappers for reference and upstream use, but exclude
Rotatrix branches/tags to avoid duplicate unavailable builds. The existing
working desktop pipeline remains the fallback, reusing upstream CMake and deps.
The upstream-only scheduled static analysis job is disabled for this fork.

See [OpenAxis CI](OpenAxis-CI.md) for local reproduction, platform limitations,
artifact downloads and manual hardware checks.

## Immutable release tags

Final tags are `<upstream-tag>-rotatrix.N`; reset N for each upstream version.
For example, `version_3.0.0-alpha11-rotatrix.1`. Never move or overwrite a shipped
tag or its release assets. Create the tag on the corresponding maintained branch.

If a test build needs permanent publication, use an explicit prerelease tag,
for example `version_3.0.0-alpha11-rotatrix.1-beta.1`. Supported suffixes are
`alpha.N`, `beta.N` and `rc.N`. Pushing `*-rotatrix.*` runs release validation,
then the full desktop build matrix and checksum verification, then publishes a
GitHub prerelease. The source must descend from the upstream tag encoded in the
release name. Existing releases are never updated by reruns.

No release tag is created as part of setup migration. Historical preview drafts
are left untouched; the routine draft-creation workflow has been removed.

## Before enabling final releases

Final tags currently fail validation with an explicit explanation. Preview
packages disable STEP support and Windows mesh repair, Windows is unsigned,
macOS is ad-hoc signed without notarization, and Linux requires Ubuntu 24.04
system libraries. These are usable test builds, not upstream-equivalent releases.

Before removing that gate:

1. Adapt accessible upstream packaging or restore comparable full-featured
   distributions, including the supported platform/architecture coverage.
2. Configure Rotatrix-owned signing credentials and macOS notarization, and
   verify signatures on the packaged distributions.
3. Verify GUI, slicing and hardware behavior on clean target machines.
4. Require final-tag commits to belong to the matching maintained branch, then
   enable final publication (without `--prerelease`) using verified packages.

The lifecycle remains upstream tag -> work branch -> CI artifacts -> clean
maintained branch -> immutable release tag -> packaged/signed GitHub Release.
