#!/usr/bin/env bash
set -euo pipefail

: "${GITHUB_SHA:?Full source commit is required}"
: "${GH_REPO:?Repository is required}"
[[ "$GITHUB_SHA" =~ ^[0-9a-f]{40}$ ]] || { echo 'Invalid source commit' >&2; exit 1; }
cd "${1:?Artifact directory is required}"

packages=(
  "PrusaSlicer-Rotatrix-windows-x64-${GITHUB_SHA:0:12}.zip"
  "PrusaSlicer-Rotatrix-macos-arm64-${GITHUB_SHA:0:8}.zip"
  "PrusaSlicer-Rotatrix-ubuntu-24.04-x64-${GITHUB_SHA:0:12}.tar.gz"
)
assets=()
for package in "${packages[@]}"; do
  [[ -s "$package" && -f "$package.sha256" ]] || { echo "Missing package/checksum: $package" >&2; exit 1; }
  # Validate the expected file itself, never a path supplied by a checksum file.
  read -r expected recorded < "$package.sha256"
  recorded="${recorded%$'\r'}"
  [[ "$expected" =~ ^[0-9a-fA-F]{64}$ && "$recorded" == "$package" ]] || { echo "Invalid checksum: $package" >&2; exit 1; }
  printf '%s  %s\n' "$expected" "$package" | sha256sum --check --strict
  assets+=("$package" "$package.sha256")
done

tag=${GITHUB_REF_NAME:?Release tag is required}
[[ "$tag" =~ ^.+-rotatrix\.[1-9][0-9]*-(alpha|beta|rc)\.[1-9][0-9]*$ ]] || { echo 'Only explicit Rotatrix prerelease tags are supported by preview packaging' >&2; exit 1; }
# Listing fails closed on API/auth errors and includes drafts visible to this token.
existing=$(gh api --paginate "repos/$GH_REPO/releases" --jq ".[] | select(.tag_name == \"$tag\") | [.draft, .prerelease, .target_commitish] | @tsv")
if [[ -n "$existing" ]]; then
  echo 'A release already exists for this immutable tag; leaving it unchanged.' >&2
  exit 1
else
  notes=$(mktemp)
  trap 'rm -f "$notes"' EXIT
  cat > "$notes" <<EOF
OpenAxis preview built from commit $GITHUB_SHA.

All three platforms passed build, OpenAxis tests and package smoke checks in the same Actions run:
${GITHUB_SERVER_URL:-https://github.com}/$GH_REPO/actions/runs/${GITHUB_RUN_ID:?Run ID is required}

Packages: Windows x64, macOS ARM64 and Ubuntu 24.04 x64, with SHA256 checksums.
These previews have STEP support disabled. Windows is unsigned; macOS is ad-hoc signed and not notarized. See each package's README for runtime requirements.

GUI and Rotatrix hardware testing remain required before publication.
EOF
  gh release create "$tag" "${assets[@]}" --verify-tag --prerelease \
    --title "$tag" --notes-file "$notes"
fi
