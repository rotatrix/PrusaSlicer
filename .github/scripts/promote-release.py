"""Select and verify existing exact-commit packages; never compile during release."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
from urllib.parse import quote

WORKFLOW = ".github/workflows/openaxis-build.yml"
JOBS = {"release-checks", "windows-x64", "linux-x64", "macos-arm64", "macos-x86_64"}
TAG = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*-rotatrix\.[1-9][0-9]*(?:-(?:alpha|beta|rc)\.[1-9][0-9]*)?\Z")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def api(path):
    return json.loads(subprocess.check_output(["gh", "api", path], text=True))


def pages(path, field):
    result = json.loads(subprocess.check_output(
        ["gh", "api", "--paginate", "--slurp", path], text=True))
    return [item for page in result for item in page[field]]


def tag_commit(repo, tag):
    obj = api(f"repos/{repo}/git/ref/tags/{quote(tag, safe='')}")["object"]
    for _ in range(8):
        if obj["type"] == "commit":
            return obj["sha"]
        require(obj["type"] == "tag", "Tag must resolve to a commit")
        obj = api(f"repos/{repo}/git/tags/{obj['sha']}")["object"]
    raise ValueError("Excessively nested tag")


def source_ref(run, tag):
    if run["event"] == "push" and run["head_branch"] == tag:
        return "refs/tags/" + tag
    require(run["event"] in ("push", "workflow_dispatch") and
            re.fullmatch(r"rotatrix/[A-Za-z0-9._/-]+", run["head_branch"] or ""),
            "Only trusted repository branch builds or exact-tag pushes qualify")
    return "refs/heads/" + run["head_branch"]


def validate_run(run, repo_id, sha, tag):
    require(run["repository"]["id"] == repo_id and run["head_repository"]["id"] == repo_id,
            "Fork build is not eligible")
    require(run["path"] == WORKFLOW and run["head_sha"] == sha, "Wrong workflow or commit")
    require(run["status"] == "completed" and run["conclusion"] == "success", "Build has not succeeded")
    return source_ref(run, tag)


def expected_packages(sha):
    return [f"PrusaSlicer-Rotatrix-windows-x64-{sha[:12]}.zip",
            f"PrusaSlicer-Rotatrix-ubuntu-24.04-x64-{sha[:12]}.tar.gz",
            f"macos-app-arm64-{sha}.tar.gz", f"macos-app-x86_64-{sha}.tar.gz"]


def validate_artifacts(artifacts, sha):
    packages = expected_packages(sha)
    names = packages + [name + ".sha256" for name in packages[:2]]
    selected = []
    for name in names:
        matches = [a for a in artifacts if a["name"] == name and not a["expired"]]
        require(len(matches) == 1, f"Missing, expired or ambiguous artifact: {name}")
        artifact = matches[0]
        require(artifact["size_in_bytes"] > 0 and
                re.fullmatch(r"sha256:[0-9a-f]{64}", artifact.get("digest") or ""),
                f"Missing artifact digest: {name}")
        selected.append(artifact)
    return selected


def select(repo, tag, run_id, receipt):
    require(repo == "rotatrix/PrusaSlicer" and TAG.fullmatch(tag), "Invalid repository or release tag")
    sha = tag_commit(repo, tag)
    metadata = api(f"repos/{repo}")
    candidates = ([api(f"repos/{repo}/actions/runs/{int(run_id)}")] if run_id else
                  pages(f"repos/{repo}/actions/workflows/openaxis-build.yml/runs?head_sha={sha}&status=success&per_page=100", "workflow_runs"))
    errors = []
    for run in candidates:
        try:
            ref = validate_run(run, metadata["id"], sha, tag)
            jobs = pages(f"repos/{repo}/actions/runs/{run['id']}/jobs?filter=latest&per_page=100", "jobs")
            require(JOBS <= {j["name"] for j in jobs if j["conclusion"] == "success"},
                    "Build must include all four platform variants and release checks")
            artifacts = validate_artifacts(pages(f"repos/{repo}/actions/runs/{run['id']}/artifacts?per_page=100", "artifacts"), sha)
        except ValueError as error:
            errors.append(f"Run {run['id']}: {error}")
            continue
        record = {"repository": repo, "repository_id": metadata["id"], "tag": tag,
                  "commit": sha, "run_id": run["id"], "run_attempt": run["run_attempt"],
                  "source_ref": ref, "build_event": run["event"], "artifacts": artifacts}
        receipt.write_text(json.dumps(record, indent=2) + "\n")
        with open(os.environ["GITHUB_OUTPUT"], "a") as output:
            output.write(f"run_id={run['id']}\ncommit={sha}\n")
            output.write("artifact_ids=" + ",".join(str(a["id"]) for a in artifacts) + "\n")
        return
    raise ValueError("No eligible existing full build. Explicitly build the tagged commit; no automatic rebuild. " + "; ".join(errors))


def verify_certificate(results, record):
    prefix = f"https://github.com/{record['repository']}/actions/runs/{record['run_id']}/attempts/"
    for item in results:
        cert = item.get("verificationResult", {}).get("signature", {}).get("certificate", {})
        uri = cert.get("runInvocationURI", "")
        attempt = uri.removeprefix(prefix)
        if (uri.startswith(prefix) and attempt.isdecimal()
                and 1 <= int(attempt) <= record["run_attempt"]
                and cert.get("buildTrigger") == record["build_event"]):
            return
    raise ValueError("Attestation does not identify the selected run/event")


def verify(receipt, directory):
    record = json.loads(receipt.read_text())
    repo, sha, tag = record["repository"], record["commit"], record["tag"]
    require(tag_commit(repo, tag) == sha, "Release tag moved")
    run = api(f"repos/{repo}/actions/runs/{record['run_id']}")
    require(validate_run(run, record["repository_id"], sha, tag) == record["source_ref"], "Build ref changed")
    require(run["run_attempt"] == record["run_attempt"], "Build was rerun during promotion; retry selection")
    for artifact in record["artifacts"]:
        path = directory / artifact["name"]
        with path.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        require("sha256:" + digest == artifact["digest"], f"Artifact hash mismatch: {path.name}")
        if path.name.endswith(".sha256"):
            continue
        verified = json.loads(subprocess.check_output([
            "gh", "attestation", "verify", str(path), "--repo", repo,
            "--signer-workflow", repo + "/" + WORKFLOW, "--source-ref", record["source_ref"],
            "--source-digest", sha, "--deny-self-hosted-runners", "--format", "json"], text=True))
        verify_certificate(verified, record)
        (directory / (path.name + ".attestation.json")).write_text(json.dumps(verified, indent=2) + "\n")
    print(f"Verified all packages from run {record['run_id']} at {sha}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["select", "verify", "recheck"])
    parser.add_argument("--receipt", type=Path, default=Path("promotion.json"))
    parser.add_argument("--directory", type=Path, default=Path("release-assets"))
    args = parser.parse_args()
    if args.command == "select":
        select(os.environ["GITHUB_REPOSITORY"], os.environ["RELEASE_TAG"], os.environ.get("SOURCE_RUN_ID", ""), args.receipt)
    elif args.command == "verify":
        verify(args.receipt, args.directory)
    else:
        record = json.loads(args.receipt.read_text())
        require(tag_commit(record["repository"], record["tag"]) == record["commit"], "Release tag moved before publication")
