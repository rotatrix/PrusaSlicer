# OpenAxis preview builds

`OpenAxis desktop builds` is a self-contained GitHub Actions workflow. It builds
Windows x64, macOS ARM64 and Ubuntu 24.04 x64 independently on pushes to
`rotatrix-prusaslicer-2.9.6`, or by manual dispatch. It does not call Prusa's private workflows.
Manual dispatch accepts a platform selector to retry one platform independently.
Packages are unsigned previews with OpenAxis enabled and STEP support disabled.
macOS receives an ad-hoc signature, not Apple notarization. Windows mesh repair is
also disabled.

After all three platforms pass in a full matrix run, CI creates a draft
prerelease named `PrusaSlicer 2.9.6 OpenAxis preview <commit>`. It uploads all three packages and
their verified SHA256 checksums from that same run and source commit. The draft
targets the full commit SHA and is never published automatically. Rerunning the
release job updates that commit's existing draft; published releases are left
unchanged. Standalone platform dispatches do not create releases. To recover a
failed full matrix, use GitHub's **Re-run failed jobs** so successful artifacts
remain associated with the same run, or dispatch `all` again.

Download each package artifact from the Actions run. ZIPs and tarballs contain
resources, dependency notices, source revision information and runtime setup
instructions. A separate artifact retains configure/build/package logs on failure.
Checksums accompany the archives. Ubuntu packages require the listed system
libraries on Ubuntu 24.04; they are not universal Linux distributions.

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
staged executable's CLI help; Linux also slices a cube. These do not verify GUI
rendering or hardware input. Before a release, test an extracted package on a
clean target machine: startup, slicing, Rotatrix navigation, mouse continuation,
reconnect, focus/modal behavior, plater/preview switching and diagnostics.
