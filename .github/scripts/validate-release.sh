#!/usr/bin/env bash
set -euo pipefail

tag=${GITHUB_REF_NAME:?Release tag is required}
if [[ ! "$tag" =~ ^(.+)-rotatrix\.([1-9][0-9]*)(-(alpha|beta|rc)\.([1-9][0-9]*))?$ ]]; then
    echo 'Expected <upstream-tag>-rotatrix.N[-alpha.N|-beta.N|-rc.N]' >&2
    exit 1
fi
upstream_tag=${BASH_REMATCH[1]}
suffix=${BASH_REMATCH[3]}
git rev-parse --verify "refs/tags/$upstream_tag^{commit}" >/dev/null
git merge-base --is-ancestor "refs/tags/$upstream_tag^{commit}" HEAD

# Enable stable promotion only after documented native acceptance/onboarding.
if [[ -z "$suffix" && "${ROTATRIX_STABLE_RELEASE_READY:-}" != true ]]; then
    echo 'Final releases await native/device acceptance and verified platform release/signing onboarding.' >&2
    exit 1
fi
