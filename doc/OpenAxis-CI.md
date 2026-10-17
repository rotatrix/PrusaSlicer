# OpenAxis preview builds

`OpenAxis desktop builds` is a self-contained GitHub Actions workflow. It builds
Windows x64, macOS ARM64/Intel and Ubuntu 24.04 x64 independently on pushes to
`rotatrix/**`, pull requests targeting maintained `rotatrix/*` branches, or by
manual dispatch. It does not call Prusa's private workflows.
Manual dispatch accepts a platform selector to retry one platform independently.
Packages are previews with OpenAxis and upstream features enabled, including STEP.
macOS receives an ad-hoc signature, not Apple notarization. Windows mesh repair is
enabled using the installed Windows SDK Printing3D headers; configuration fails
when those headers are missing.

Routine builds upload temporary artifacts, named with the source SHA and retained
for 14 days. They create no tags or releases. To recover a failed full matrix,
use GitHub's **Re-run failed jobs** or dispatch only the failed platform. Avoid
dispatching `all` while healthy jobs from an earlier run are still active.

Permanent previews use an immutable tag such as
`version_2.9.6-rotatrix.1-beta.1` pointing at the tested work-branch commit.
The tagged release workflow selects an existing successful full desktop build at
that exact SHA, verifies all four platform jobs, artifact hashes and original build
attestations, then publishes the Windows/Linux packages without recompilation.
`workflow_dispatch` can retry an existing tag with an explicit `source_run_id`.
Missing/expired artifacts, missing attestations or a mismatched SHA stop promotion;
there is no automatic rebuild or cross-commit substitution. The initial implementation
requires one complete successful run rather than combining partial platform runs.

Builds on trusted Rotatrix branches now attest all package files at build time.
PR builds cannot be promoted. The default-branch `Request release signing` relay
reads the successful promotion record and dispatches the central signer using the
original Mac build/artifact IDs. The signer verifies original branch provenance and
the tag's exact SHA, signs/notarizes and attaches Mac packages to the same release.
Checksums and verified provenance records are retained with the release.

Stable tags use the same promotion path, with the repository Actions variable
`ROTATRIX_STABLE_RELEASE_READY=true` required after native/device acceptance and
release onboarding. It is not enabled by these changes. Beta/alpha/rc tags produce
GitHub prereleases. Never move published tags or overwrite published assets.
History cleanup is optional and separate; squashed commits require their own build.

Download each package artifact from the Actions run. Windows ZIPs and Linux tarballs contain
resources, dependency notices, source revision information and runtime setup
instructions. A separate artifact retains configure/build/package logs on failure.
Checksums accompany the archives. Ubuntu packages require the listed system
libraries on Ubuntu 24.04; they are not universal Linux distributions.

Windows ZIPs and Linux tarballs are uploaded directly, without a second ZIP
wrapper; checksums are separate downloadable files. Download the individual
package artifact. Windows archives contain one short `PrusaSlicer/` folder.
Third-party notices retain their original contents under short numbered names,
with `third-party-notices/index.csv` mapping each name to its source path. This
avoids embedding long dependency-build paths in Windows extraction paths.
The release downloader preserves these files without unpacking the package ZIP.

macOS uses `macos-app-<architecture>-<full-sha>.tar.gz` for all build types,
uploaded directly without an outer ZIP. Its only top-level entry is the complete
upstream-named `PrusaSlicer.app`; licenses and source metadata live inside it.
The upstream bundle ID and preference locations are preserved. Both architecture
variants are built natively. Packaging verifies the extracted ad-hoc signed app.

Release setup: create the source `signing-dispatch` environment restricted to the
default branch and add `SIGNER_DISPATCH_TOKEN` with Actions write on the private
signer. The signer's `SOURCE_RELEASE_TOKEN` must cover this repository. Apple
credentials stay in the signer. This environment was absent when checked on
2026-10-16; ordinary branch builds need none of these credentials.

The scripts in `.github/scripts` can reproduce CI locally. Run from a checkout:

```powershell
# Windows, with CMake and Visual Studio 2022 available
./.github/scripts/windows-build.ps1 deps
./.github/scripts/windows-build.ps1 build
./.github/scripts/windows-build.ps1 package
```

```bash
# Native macOS ARM64 (replace macos with linux on Ubuntu 24.04)
bash .github/scripts/macos-build.sh prerequisites
bash .github/scripts/macos-build.sh deps
bash .github/scripts/macos-build.sh build
bash .github/scripts/macos-build.sh package
```

Linux prerequisites are listed in the workflow. Scripts accept `DEPS_BUILD_DIR`,
`BUILD_DIR` and `DIST_DIR`; defaults are `deps/build-ci`, `build-ci` and `dist`.
Use a fresh packaging output directory. CMake is pinned in the workflow. The
dependency cache includes platform and dependency/script hashes, and is saved
after dependencies succeed so an application failure does not discard it.
An older cache for the same platform and CMake version can seed a changed build;
the dependency configure/build steps bring fallback restores up to date. Exact
hits also match the hosted runner image version and skip dependency rebuilding.

### Build performance

Hosted jobs select `BUILD_JOBS` from available CPUs and memory: reserve 2 GiB,
allow 2.5 GiB per compilation worker, and cap at eight workers. This is an initial
budget, not a measured optimum. Logs record the hardware and selected count.
Dependency projects run one at a time with parallel compilation inside each.
Windows application builds similarly use one MSBuild project at a time with a
bounded `/MP` count. Local scripts retain their two-worker default; explicitly
set `BUILD_JOBS` to override it without changing any global tools or settings.

Linux/macOS application compilation uses ccache, limited to 2 GiB per platform.
Each run restores the newest platform cache and saves a new snapshot before
packaging, including partial compilation after a build failure. Compiler contents
and compilation inputs determine reuse; cache statistics are uploaded with logs.
Precompiled headers remain enabled and ccache's correctness checks are not relaxed,
so PCH-dependent compilations may remain uncacheable. Measure the reported hit rate
before assuming a warm build will avoid all compilation. Windows retains the native
Visual Studio generator, which does not support CMake compiler launchers; it has
dependency caching but no application compiler cache yet.

No persistent personal-machine runners are used. Full application build trees are
not cached: packaging retries still configure/build, but can reuse cached Unix
compilations. Packages and smoke tests are always regenerated for the current SHA.
No feature, test, architecture or signing requirement is removed for performance.

Build stages run the standalone OpenAxis viewport tests. Packaging runs the
staged executable's CLI help and real STEP import; Linux also slices a cube. These do not verify GUI
rendering or hardware input. Before a release, test an extracted package on a
clean target machine: startup, slicing, Rotatrix navigation, mouse continuation,
reconnect, focus/modal behavior, plater/preview switching and diagnostics.

### OpenSSL compatibility for the pinned SDK

PrusaSlicer 2.9.6 builds OpenSSL 1.1.0l. OpenAxis C++ rc.1 uses the newer
one-shot `EVP_DigestVerify` API, which fails to compile against those headers.
`cmake/openaxis-openssl-compat.patch` is applied to the fetched SDK and replaces
that call with `EVP_DigestVerifyUpdate` followed by `EVP_DigestVerifyFinal`.
ECDSA/SHA-256 verification, signature decoding and failure handling are retained;
no verification is bypassed and the upstream dependency version is unchanged.
Local SDK overrides must contain the same compatibility fix. Remove this patch
when the pinned SDK release includes the equivalent upstream fix.
