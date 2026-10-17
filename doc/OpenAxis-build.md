# Building with OpenAxis

Enable `SLIC3R_OPENAXIS=ON` for a native GUI build. CMake 3.24+ and Git fetch
the pinned OpenAxis SDK; `OPENAXIS_SOURCE_DIR` can select a local SDK checkout.

The scripts in `.github/scripts` provide dependency, build and package stages.
Run from the repository root with the platform prerequisites installed:

```powershell
# Windows: Visual Studio 2022 C++, Windows SDK, CMake
./.github/scripts/windows-build.ps1 deps
./.github/scripts/windows-build.ps1 build
./.github/scripts/windows-build.ps1 package
```

```bash
# macOS; use linux-build.sh on Ubuntu 24.04
bash .github/scripts/macos-build.sh deps
bash .github/scripts/macos-build.sh build
bash .github/scripts/macos-build.sh package
```

The workflow lists platform dependencies and pinned tool versions. Scripts accept
`DEPS_BUILD_DIR`, `BUILD_DIR` and `DIST_DIR`; use a fresh packaging output directory.

Windows ZIPs and Linux tar archives include resources, dependency notices and
runtime setup instructions. Linux packages require Ubuntu 24.04 system libraries.
macOS archives contain `PrusaSlicer.app` with its resources and notices.
CI macOS packages are ad-hoc signed; Windows packages are unsigned.

Standalone adapter tests are under `tests/openaxis`. Build and package scripts
also exercise CLI startup and STEP import; Linux performs a slicing smoke test.
These tests do not replace GUI and physical-device testing.
