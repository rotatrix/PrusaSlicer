#!/usr/bin/env bash
# Exercise publication safeguards without GitHub writes or desktop builds.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT
export GITHUB_SHA=0123456789012345678901234567890123456789
export GH_REPO=rotatrix/PrusaSlicer GITHUB_RUN_ID=1
export GITHUB_REF_NAME=version_3.0.0-alpha11-rotatrix.1-beta.1
bash "$root/.github/scripts/validate-release.sh"
for invalid in version_3.0.0-alpha11-rotatrix.1 version_3.0.0-alpha11-rotatrix.0-beta.1 invalid-rotatrix.1-beta.1; do
    if GITHUB_REF_NAME="$invalid" bash "$root/.github/scripts/validate-release.sh" >"$temporary/error" 2>&1; then
        echo "Unexpectedly accepted $invalid" >&2; exit 1
    fi
done
mkdir -p "$temporary/bin" "$temporary/assets"
export MOCK_GH_LOG="$temporary/gh.log"
cat > "$temporary/bin/gh" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
if [[ "$1" == api ]]; then
    case "${MOCK_GH_STATE:-new}" in
        existing) printf 'false\ttrue\tcommit\n' ;;
        failure) exit 1 ;;
    esac
    exit 0
fi
printf '%s\n' "$*" >> "$MOCK_GH_LOG"
MOCK
chmod +x "$temporary/bin/gh"
export PATH="$temporary/bin:$PATH"
cd "$temporary/assets"
packages=(
    "PrusaSlicer-Rotatrix-windows-x64-${GITHUB_SHA:0:12}.zip"
    "PrusaSlicer-Rotatrix-macos-arm64-${GITHUB_SHA:0:8}.zip"
    "PrusaSlicer-Rotatrix-ubuntu-24.04-x64-${GITHUB_SHA:0:12}.tar.gz"
)
for package in "${packages[@]}"; do
    printf 'fixture\n' > "$package"
    sha256sum --text "$package" > "$package.sha256"
done
bash "$root/.github/scripts/publish-prerelease.sh" "$PWD" >"$temporary/output"
grep -q 'release create .*--verify-tag --prerelease' "$MOCK_GH_LOG"
[[ $(wc -l < "$MOCK_GH_LOG") -eq 1 ]]
for state in existing failure; do
    if MOCK_GH_STATE="$state" bash "$root/.github/scripts/publish-prerelease.sh" "$PWD" >"$temporary/output" 2>&1; then
        echo "Unexpected publication with API state $state" >&2; exit 1
    fi
done
printf 'corrupt\n' >> "${packages[0]}"
if bash "$root/.github/scripts/publish-prerelease.sh" "$PWD" >"$temporary/output" 2>&1; then
    echo 'Corrupt package was accepted' >&2; exit 1
fi
[[ $(wc -l < "$MOCK_GH_LOG") -eq 1 ]]
echo 'Release tag, checksum, API failure and immutability checks passed.'
