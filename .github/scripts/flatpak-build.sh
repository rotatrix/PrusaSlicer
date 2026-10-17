#!/usr/bin/env bash
# Run on Linux with flatpak-builder, Flatpak, Python/PyYAML and Git installed.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
SHA=$(git rev-parse HEAD)
RECIPE_REV=9720ef5bfbfd53e6809425a201ff5cdea93e98bf
RECIPE_DIR="$ROOT/build-flatpak-recipe"
if [[ ! -d "$RECIPE_DIR/.git" ]]; then
    git clone https://github.com/flathub/com.prusa3d.PrusaSlicer.git "$RECIPE_DIR"
fi
git -C "$RECIPE_DIR" fetch origin "$RECIPE_REV"
git -C "$RECIPE_DIR" checkout --detach "$RECIPE_REV"
python3 .github/scripts/flatpak-manifest.py "$RECIPE_DIR/com.prusa3d.PrusaSlicer.yml" "$SHA"
flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak-builder --user --install-deps-from=flathub --assumeyes --force-clean --ccache \
    --repo=build-flatpak-repo build-flatpak "$RECIPE_DIR/com.prusa3d.PrusaSlicer.json"
mkdir -p dist
PACKAGE="PrusaSlicer-Rotatrix-linux-x64-${SHA:0:12}.flatpak"
flatpak build-bundle --runtime-repo=https://flathub.org/repo/flathub.flatpakrepo \
    build-flatpak-repo "dist/$PACKAGE" com.prusa3d.PrusaSlicer master
(cd dist && sha256sum "$PACKAGE" > "$PACKAGE.sha256")
# This uses the sandbox runtime, not the developer host's installed libraries.
flatpak-builder --run build-flatpak "$RECIPE_DIR/com.prusa3d.PrusaSlicer.json" prusa-slicer --help
