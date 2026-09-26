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

# This work branch produces test packages. Do not ship them as final releases.
if [[ -z "$suffix" ]]; then
    echo 'Final releases are not enabled: restore upstream-equivalent packaging/features and configure platform signing/notarization first. See doc/Rotatrix-workflow.md.' >&2
    exit 1
fi
