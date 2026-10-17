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
# macOS
bash .github/scripts/macos-build.sh deps
bash .github/scripts/macos-build.sh build
bash .github/scripts/macos-build.sh package
```

The workflow lists platform dependencies and pinned tool versions. Scripts accept
`DEPS_BUILD_DIR`, `BUILD_DIR` and `DIST_DIR`; use a fresh packaging output directory.

Windows ZIPs include resources and dependency notices. Linux releases use the
public upstream Flatpak recipe, pinned in `.github/scripts/flatpak-build.sh`.
Install `flatpak`, `flatpak-builder`, and `python3-yaml` on the Linux builder, then:

```bash
bash .github/scripts/flatpak-build.sh
```

The script downloads the upstream SDK/runtime and builds the exact checked-out
commit (which must already be pushed). Its incremental cache is `.flatpak-builder`.
The Ubuntu tarball script remains only for native development diagnostics.

For a downloaded release bundle:

```bash
flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak install --user ./PrusaSlicer-Rotatrix-linux-x64-<revision>.flatpak
flatpak run com.prusa3d.PrusaSlicer
```

Flatpak installs the required runtime; individual application libraries do not
need installing with apt. This retains the upstream Flatpak app ID and settings,
so it replaces that user's upstream Flatpak installation. Native installation
settings are separate. Close the existing app before installing a replacement;
use an isolated test account to test without replacing an existing installation.
macOS archives contain `PrusaSlicer.app` with its resources and notices.
CI macOS packages are ad-hoc signed; Windows packages are unsigned.

Standalone adapter tests are under `tests/openaxis`. Build and package scripts
also exercise CLI startup; native package checks exercise STEP import and slicing.
Flatpak retains upstream CTest and the standalone adapter tests.
These tests do not replace GUI and physical-device testing.
