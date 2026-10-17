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
use GitHub's **Re-run failed jobs** or dispatch `all` again.

Permanent test builds require an explicit tag such as
`version_2.9.6-rotatrix.1-beta.1`. The tagged release workflow builds all
platforms, verifies Windows/Linux checksums, and publishes a GitHub prerelease.
macOS archives receive GitHub build attestations on tags. After the complete
source workflow succeeds, the default-branch `Request release signing` relay
dispatches `rotatrix/release-signing`, which signs/notarizes and attaches the
macOS assets to the same release without replacing Windows/Linux assets.
It never overwrites an existing release. Final releases are deliberately blocked
pending native acceptance and release onboarding; see the
[fork lifecycle and release prerequisites](Rotatrix-workflow.md).

Download each package artifact from the Actions run. Windows ZIPs and Linux tarballs contain
resources, dependency notices, source revision information and runtime setup
instructions. A separate artifact retains configure/build/package logs on failure.
Checksums accompany the archives. Ubuntu packages require the listed system
libraries on Ubuntu 24.04; they are not universal Linux distributions.

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
the dependency configure/build steps always run to bring it up to date.

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
