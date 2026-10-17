#!/usr/bin/env bash
set -euo pipefail

: "${GITHUB_SHA:?Full source commit is required}"
: "${GH_REPO:?Repository is required}"
[[ "$GITHUB_SHA" =~ ^[0-9a-f]{40}$ ]] || { echo 'Invalid source commit' >&2; exit 1; }
cd "${1:?Artifact directory is required}"

packages=(
  "PrusaSlicer-Rotatrix-windows-x64-${GITHUB_SHA:0:12}.zip"
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
[[ "$tag" =~ ^.+-rotatrix\.[1-9][0-9]*(-(alpha|beta|rc)\.[1-9][0-9]*)?$ ]] || { echo 'Expected a Rotatrix release tag' >&2; exit 1; }
release_options=()
if [[ -n "${BASH_REMATCH[1]}" ]]; then
  release_options+=(--prerelease)
elif [[ "${ROTATRIX_STABLE_RELEASE_READY:-}" != true ]]; then
  echo 'Stable release acceptance has not been enabled' >&2; exit 1
fi
# Listing fails closed on API/auth errors and includes drafts visible to this token.
existing=$(gh api --paginate "repos/$GH_REPO/releases" --jq ".[] | select(.tag_name == \"$tag\") | [.draft, .prerelease, .target_commitish] | @tsv")
if [[ -n "$existing" ]]; then
  echo 'A release already exists for this immutable tag; leaving it unchanged.' >&2
  exit 1
else
  notes=$(mktemp)
  trap 'rm -f "$notes"' EXIT
  cat > "$notes" <<EOF
PrusaSlicer 2.9.6 OpenAxis preview built from commit $GITHUB_SHA.

All four platform variants passed build, OpenAxis tests and package smoke checks in the selected source run:
${GITHUB_SERVER_URL:-https://github.com}/$GH_REPO/actions/runs/${SOURCE_BUILD_RUN_ID:-${GITHUB_RUN_ID:?Run ID is required}}

Existing exact-commit packages were verified and promoted without recompilation.

Packages include upstream features, including STEP import and Windows mesh repair.
Windows is unsigned. Linux requires Ubuntu 24.04 system libraries.
macOS ARM64 and Intel apps are signed and notarized by rotatrix/release-signing
after this source workflow succeeds, then attached to this release separately.

GUI and Rotatrix hardware testing remain required before publication.
EOF
  gh release create "$tag" "${assets[@]}" --verify-tag "${release_options[@]}" \
    --title "$tag" --notes-file "$notes"
fi
