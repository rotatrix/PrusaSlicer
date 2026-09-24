#!/usr/bin/env bash
set -euo pipefail

# Run from the checkout root. Dependency cache paths must remain stable (wx-config).
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEPS_BUILD_DIR="${DEPS_BUILD_DIR:-$ROOT/deps/build-ci}"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-ci}"
DIST_DIR="${DIST_DIR:-$ROOT/dist}"
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-11.0}"
JOBS="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
if (( JOBS > 2 )); then JOBS=2; fi
PREFIX="$DEPS_BUILD_DIR/destdir/usr/local"
cd "$ROOT"
[[ "$(uname -s)" == Darwin && "$(uname -m)" == arm64 ]] || {
    echo 'This script requires a native macOS ARM64 runner.' >&2; exit 1;
}

prerequisites() {
    brew install automake gettext libtool texinfo
}

# gettext and texinfo are keg-only in Homebrew.
export PATH="$(brew --prefix)/opt/gettext/bin:$(brew --prefix)/opt/texinfo/bin:$PATH"

deps() {
    cmake -S deps -B "$DEPS_BUILD_DIR" -G 'Unix Makefiles' \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET" \
        -DPrusaSlicer_deps_PACKAGE_EXCLUDES=OCCT -DDEP_MAX_THREADS=2
    # Each dependency already builds in parallel; avoid simultaneous heavy builds.
    cmake --build "$DEPS_BUILD_DIR" --parallel 1
}

build() {
    cmake -S . -B "$BUILD_DIR" -G 'Unix Makefiles' \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET" \
        -DCMAKE_PREFIX_PATH="$PREFIX" -DSLIC3R_STATIC=ON \
        -DSLIC3R_OPENAXIS=ON -DSLIC3R_ENABLE_FORMAT_STEP=OFF \
        -DSLIC3R_BUILD_TESTS=OFF -DSLIC3R_RELEASE_DEBUG_SYMBOLS=OFF \
        -DCMAKE_EXE_LINKER_FLAGS=-Wl,-headerpad_max_install_names
    cmake --build "$BUILD_DIR" --target slic3r-app-launcher --parallel "$JOBS"
    cmake -S tests/openaxis -B "$BUILD_DIR/openaxis-checks" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET"
    cmake --build "$BUILD_DIR/openaxis-checks" --parallel "$JOBS"
    ctest --test-dir "$BUILD_DIR/openaxis-checks" --output-on-failure
}

package() {
    mkdir -p "$DIST_DIR"
    # A fresh staging folder makes repeated packaging safe without deleting user files.
    local staging app executable revision archive
    staging="$(mktemp -d "$DIST_DIR/macos-stage.XXXXXX")"
    app="$staging/PrusaSlicer-Rotatrix.app"
    mkdir -p "$app/Contents/MacOS"
    # src/CMakeLists.txt configures this file in its binary directory.
    test -f "$BUILD_DIR/src/Info.plist"
    cp "$BUILD_DIR/src/Info.plist" "$app/Contents/Info.plist"
    executable="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$app/Contents/Info.plist")"
    cp "$BUILD_DIR/src/slic3r-app-launcher/slic3r-app-launcher" "$app/Contents/MacOS/$executable"
    ditto "$ROOT/resources" "$app/Contents/Resources"
    # App/Init.cpp resolves resources as ../Resources relative to the executable.
    local resource
    for resource in icons shaders lua localization; do
        test -d "$app/Contents/MacOS/../Resources/$resource"
    done
    cp "$ROOT/resources/icons/PrusaSlicer.icns" "$app/Contents/Resources/PrusaSlicer.icns"
    cp "$ROOT/LICENSE" "$staging/LICENSE"
    local source notice relative
    for source in "$DEPS_BUILD_DIR" "$BUILD_DIR/_deps" "$ROOT/bundled_deps"; do
        [[ -d "$source" ]] || continue
        while IFS= read -r -d '' notice; do
            relative="${notice#"$ROOT/"}"
            mkdir -p "$staging/third-party-notices/$(dirname "$relative")"
            cp "$notice" "$staging/third-party-notices/$relative"
        done < <(find "$source" -type f \( -iname 'LICENSE*' -o -iname 'COPYING*' -o -iname 'NOTICE*' \) -print0)
    done
    revision="$(git rev-parse HEAD)"
    printf 'Unofficial Rotatrix preview, macOS ARM64, no STEP support.\nSource revision: %s\nSource: https://github.com/%s/tree/%s\nAd-hoc signed for testing; not Apple-notarized.\n' \
        "$revision" "${GITHUB_REPOSITORY:-rotatrix/PrusaSlicer}" "$revision" > "$staging/README.txt"
    /usr/libexec/PlistBuddy -c 'Set :CFBundleIdentifier com.rotatrix.prusaslicer.preview' "$app/Contents/Info.plist"
    plutil -lint "$app/Contents/Info.plist"
    cmake "-DAPP=$app" "-DLIB_DIR=$PREFIX/lib" -P "$ROOT/.github/scripts/macos-bundle.cmake"
    # install_name_tool invalidates existing signatures; sign inner Mach-O files first.
    while IFS= read -r -d '' binary; do
        if file "$binary" | grep -q 'Mach-O'; then
            codesign --force --sign - "$binary"
        fi
    done < <(find "$app" -type f -print0)
    codesign --force --sign - "$app"
    codesign --verify --deep --strict "$app"
    mkdir -p "$BUILD_DIR/smoke-datadir"
    "$app/Contents/MacOS/$executable" --datadir "$BUILD_DIR/smoke-datadir" --help
    archive="$DIST_DIR/PrusaSlicer-Rotatrix-macos-arm64-${revision:0:8}.zip"
    ditto -c -k --sequesterRsrc "$staging" "$archive"
    (cd "$DIST_DIR" && shasum -a 256 "$(basename "$archive")" > "$(basename "$archive").sha256")
    echo "Packaged $archive"
}

case "${1:-all}" in
    prerequisites) prerequisites ;;
    deps) deps ;;
    build) build ;;
    package) package ;;
    all) prerequisites; deps; build; package ;;
    *) echo "Usage: $0 [prerequisites|deps|build|package|all]" >&2; exit 2 ;;
esac
