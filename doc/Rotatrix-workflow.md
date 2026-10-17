# Rotatrix development and release workflow

The active and default branch is `rotatrix/work/version_2.9.6`, based on the
upstream release tag `version_2.9.6`. No separate maintained branch is needed to
ship previews or stable releases. The exploratory 3.0.0-alpha11 work is abandoned.

Work history may be rewritten with coordination, but published release tags must
never move or disappear. Tags preserve each distributed commit despite later
work-branch rewrites. Contributors target the active work branch.

Clean up history on a separate branch when useful for upstream submission or
porting; verify before adopting it. Squashing is not a release prerequisite.
It creates a new SHA, so a cleaned commit needs its own build to be released.
For a new upstream version, create its work branch from the official tag and port
only the necessary patches, using the previous baseline and tagged release as
references.

## CI and packages

Normal CI runs on pushes and PRs targeting `rotatrix/**`. It builds Windows x64,
macOS ARM64/Intel and Linux x64, runs OpenAxis tests and package smoke checks,
and uploads packages/checksums/logs with source SHAs and 14-day retention.
Trusted push/manual builds attest packages for later release promotion; PR builds
are not eligible. Routine builds do not create tags or releases.

Prefer upstream CI/build/dependency/package commands wherever practical. This
upstream release delegates its builds to `Prusa-Development/PrusaSlicer-Actions`,
which is unavailable to this fork's current credentials (GitHub returns 404).
Keep those upstream wrappers for reference and upstream use, but exclude
Rotatrix branches/tags to avoid duplicate unavailable builds. The existing
working desktop pipeline remains the fallback, reusing upstream CMake and deps.
The upstream-only static analysis schedule is removed for this fork. An owner
guard alone did not prevent GitHub from validating its inaccessible reusable
workflow and reporting daily failures.

See [OpenAxis CI](OpenAxis-CI.md) for local reproduction, platform limitations,
artifact downloads and manual hardware checks.

## Immutable release tags and promotion

Stable tags are `<upstream-tag>-rotatrix.N`; reset N for each upstream version.
Preview tags append `-alpha.N`, `-beta.N` or `-rc.N`, for example
`version_2.9.6-rotatrix.1-beta.1`. Tag the exact tested work-branch commit. Never
move/delete a published tag or overwrite its release assets.

Pushing a release tag selects an existing successful full desktop build of that
exact SHA, verifies its required jobs, package hashes and original attestations,
then publishes existing Windows/Linux packages. The default-branch completion
relay sends the selected original Mac build/artifact IDs to the central signer.
The signer independently verifies original provenance and tag-to-SHA equality,
then signs/notarizes and attaches Mac packages to that release. No recompilation
occurs during promotion. A manual promotion retry accepts an existing tag and
optional explicit source run ID. Missing eligible artifacts fail with instructions
to explicitly build/test the tagged commit; they never trigger a hidden rebuild.

See [OpenAxis CI](OpenAxis-CI.md) for the exact workflow and credential setup.
No release tag or publication is authorized merely by implementing this workflow.

## Before enabling stable releases

The same promotion path supports stable tags after the repository Actions variable
`ROTATRIX_STABLE_RELEASE_READY` is explicitly set to `true`. Until then stable
validation fails. Preview publication remains available after acceptance and setup.

1. Verify package completeness and all supported features/architectures.
2. Configure `SIGNER_DISPATCH_TOKEN` in the source `signing-dispatch` environment
   and verify the central signer's publication token covers PrusaSlicer. Apple
   credentials stay only in the central service. Verify a real signed/notarized
   package on a separate Mac. Windows packages currently remain unsigned.
3. Verify GUI, slicing and hardware behavior on target machines. Linux packages
   require Ubuntu 24.04 system libraries.
4. Record acceptance, then enable the stable release variable. Stable releases
   use verified existing packages without the GitHub prerelease flag.

Lifecycle: upstream tag -> work branch -> build/test packages -> immutable tag ->
verify existing artifacts -> central signing and GitHub preview/stable release.
