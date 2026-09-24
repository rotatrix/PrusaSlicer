#!/usr/bin/env bash
# Ubuntu 24.04 x86_64 preview. System GTK/WebKit/glibc remain host dependencies.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
DEPS_BUILD_DIR=${DEPS_BUILD_DIR:-"$ROOT/deps/build-ci"}
BUILD_DIR=${BUILD_DIR:-"$ROOT/build-ci"}
DIST_DIR=${DIST_DIR:-"$ROOT/dist"}
JOBS=${BUILD_JOBS:-2}
PREFIX="$DEPS_BUILD_DIR/destdir/usr/local"
REVISION=$(git -C "$ROOT" rev-parse --short=12 HEAD)
NAME="PrusaSlicer-Rotatrix-ubuntu-24.04-x64-$REVISION"
STAGE="$DIST_DIR/$NAME"

deps() {
    cmake -S "$ROOT/deps" -B "$DEPS_BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DPrusaSlicer_deps_PACKAGE_EXCLUDES=OCCT -DDEP_MAX_THREADS="$JOBS"
    cmake --build "$DEPS_BUILD_DIR" --parallel 1
}

build() {
    cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PREFIX" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DSLIC3R_STATIC=ON \
        -DSLIC3R_FHS=OFF -DSLIC3R_OPENAXIS=ON \
        -DSLIC3R_ENABLE_FORMAT_STEP=OFF -DSLIC3R_BUILD_TESTS=OFF \
        -DSLIC3R_RELEASE_DEBUG_SYMBOLS=OFF
    cmake --build "$BUILD_DIR" --target slic3r-app-launcher --parallel "$JOBS"
    cmake -S "$ROOT/tests/openaxis" -B "$BUILD_DIR/openaxis-tests" -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build "$BUILD_DIR/openaxis-tests" --parallel "$JOBS"
    ctest --test-dir "$BUILD_DIR/openaxis-tests" --output-on-failure
}

copy_notices() {
    local source=$1 category=$2 notice relative
    [[ -d "$source" ]] || return 0
    while IFS= read -r -d '' notice; do
        relative=${notice#"$source/"}
        mkdir -p "$STAGE/third-party-notices/$category/$(dirname "$relative")"
        cp "$notice" "$STAGE/third-party-notices/$category/$relative"
    done < <(find "$source" -type f \( -iname 'license*' -o -iname 'licence*' -o -iname 'copying*' -o -iname 'copyright*' -o -iname 'notice*' \) -print0)
}

package() {
    mkdir -p "$STAGE/bin" "$STAGE/lib" "$STAGE/third-party-notices"
    cp "$BUILD_DIR/src/slic3r-app-launcher/slic3r-app-launcher" "$STAGE/bin/prusa-slicer"
    cp -a "$ROOT/resources" "$STAGE/"
    cp "$ROOT/LICENSE" "$STAGE/"
    git -C "$ROOT" rev-parse HEAD > "$STAGE/SOURCE-REVISION.txt"
    git -C "$ROOT" remote get-url origin >> "$STAGE/SOURCE-REVISION.txt"
    # Keep system libraries on the host; record their complete transitive ldd
    # closure and package owners so the same Ubuntu release can install them.
    ldd "$STAGE/bin/prusa-slicer" > "$STAGE/runtime-libraries.txt"
    if grep -q 'not found' "$STAGE/runtime-libraries.txt"; then
        cat "$STAGE/runtime-libraries.txt"
        return 1
    fi
    : > "$STAGE/runtime-packages.txt"
    while IFS= read -r library; do
        if [[ "$library" == "$PREFIX/"* || "$library" == "$BUILD_DIR/"* ]]; then
            cp -L "$library" "$STAGE/lib/"
        else
            owner=$(dpkg-query -S "$library" 2>/dev/null | head -1 | sed 's/: \/.*//') || true
            if [[ -z "$owner" ]]; then
                owner=$(dpkg-query -S "$(readlink -f "$library")" 2>/dev/null | head -1 | sed 's/: \/.*//') || true
            fi
            if [[ -n "$owner" ]]; then
                printf '%s\n' "$owner" >> "$STAGE/runtime-packages.txt"
            fi
        fi
    done < <(awk '/=> \// {print $3} /^\s*\// {print $1}' "$STAGE/runtime-libraries.txt")
    sort -u -o "$STAGE/runtime-packages.txt" "$STAGE/runtime-packages.txt"
    copy_notices "$DEPS_BUILD_DIR" dependencies
    copy_notices "$ROOT/bundled_deps" bundled
    copy_notices "$BUILD_DIR/_deps" fetched
    cat > "$STAGE/prusa-slicer" <<'LAUNCHER'
#!/usr/bin/env bash
set -e
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$HERE/bin/prusa-slicer" "$@"
LAUNCHER
    chmod +x "$STAGE/prusa-slicer" "$STAGE/bin/prusa-slicer"
    cat > "$STAGE/README.txt" <<'README'
Unofficial PrusaSlicer / Rotatrix OpenAxis preview for Ubuntu 24.04 x86_64.
STEP import is disabled. This is not a portable package for other distributions.
Extract the complete archive, then run ./prusa-slicer.
Install required system runtime libraries on Ubuntu 24.04 with:
  sudo apt-get update
  xargs -a runtime-packages.txt sudo apt-get install -y
GTK, WebKit, graphics drivers, and their runtime data remain system dependencies.
OpenAxis device input and GUI behavior require manual testing with Rotatrix.
SOURCE-REVISION.txt identifies the matching source repository and revision.
README
    smoke
    tar -czf "$DIST_DIR/$NAME.tar.gz" -C "$DIST_DIR" "$NAME"
    (cd "$DIST_DIR" && sha256sum "$NAME.tar.gz" > "$NAME.tar.gz.sha256")
}

smoke() {
    local temporary
    temporary=$(mktemp -d)
    # Run from outside the checkout, with isolated user config, against staged files.
    test -d "$STAGE/resources/presets"
    test ! -L "$STAGE/resources"
    (cd "$temporary" && "$STAGE/prusa-slicer" --datadir "$temporary/config" --help)
    (cd "$temporary" && "$STAGE/prusa-slicer" --datadir "$temporary/config" \
        --export-gcode --output "$temporary/cube.gcode" "$ROOT/tests/data/20mm_cube.obj")
    test -s "$temporary/cube.gcode"
    grep -Eq '^G[01] .*E' "$temporary/cube.gcode"
}

case "${1:-all}" in
    deps) deps ;;
    build) build ;;
    package) package ;;
    smoke) smoke ;;
    all) deps; build; package ;;
    *) echo "Usage: $0 [deps|build|package|smoke|all]" >&2; exit 2 ;;
esac
