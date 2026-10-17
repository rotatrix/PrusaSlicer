"""Release selection and provenance checks without network or publication."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("promotion", Path(__file__).with_name("promote-release.py"))
promotion = importlib.util.module_from_spec(spec)
spec.loader.exec_module(promotion)


class PromotionTests(unittest.TestCase):
    def setUp(self):
        self.sha = "a" * 40
        self.tag = "version_2.9.6-rotatrix.1-beta.1"
        self.run = {"id": 2, "repository": {"id": 1}, "head_repository": {"id": 1},
                    "head_sha": self.sha, "head_branch": "rotatrix/work/version_2.9.6",
                    "event": "workflow_dispatch", "status": "completed", "conclusion": "success",
                    "path": promotion.WORKFLOW, "run_attempt": 1}
        names = promotion.expected_packages(self.sha)
        names += [n + ".sha256" for n in names[:2]]
        self.artifacts = [{"id": i, "name": name, "expired": False,
                           "size_in_bytes": 5, "digest": "sha256:" + "b" * 64}
                          for i, name in enumerate(names, 1)]

    def test_trusted_branch_and_tag(self):
        self.assertEqual(promotion.validate_run(self.run, 1, self.sha, self.tag),
                         "refs/heads/rotatrix/work/version_2.9.6")
        self.run.update(event="push", head_branch=self.tag)
        self.assertEqual(promotion.validate_run(self.run, 1, self.sha, self.tag), "refs/tags/" + self.tag)

    def test_ineligible_builds(self):
        for changes in ({"event": "pull_request"}, {"event": "pull_request_target"},
                        {"head_repository": {"id": 9}}, {"head_sha": "c" * 40},
                        {"path": ".github/workflows/other.yml"}, {"conclusion": "failure"},
                        {"status": "in_progress"}, {"head_branch": "other"}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                promotion.validate_run({**self.run, **changes}, 1, self.sha, self.tag)

    def test_complete_artifacts_required(self):
        self.assertEqual(len(promotion.validate_artifacts(self.artifacts, self.sha)), 6)
        for changes in ({"expired": True}, {"digest": None}, {"size_in_bytes": 0}, {"name": "wrong.zip"}):
            artifacts = copy.deepcopy(self.artifacts)
            artifacts[0].update(changes)
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                promotion.validate_artifacts(artifacts, self.sha)
        with self.assertRaises(ValueError):
            promotion.validate_artifacts(self.artifacts + [self.artifacts[0]], self.sha)

    def test_all_platform_jobs_required(self):
        def api(path):
            return self.run if "/actions/runs/" in path else {"id": 1}
        def pages(path, field):
            if field == "jobs":
                return [{"name": name, "conclusion": "success"} for name in self.jobs]
            return self.artifacts
        with tempfile.TemporaryDirectory() as tmp:
            receipt, output = Path(tmp) / "receipt.json", Path(tmp) / "outputs"
            with patch.object(promotion, "tag_commit", return_value=self.sha), patch.object(promotion, "api", side_effect=api), patch.object(promotion, "pages", side_effect=pages), patch.dict(os.environ, GITHUB_OUTPUT=str(output)):
                self.jobs = promotion.JOBS - {"macos-arm64"}
                with self.assertRaises(ValueError):
                    promotion.select("rotatrix/PrusaSlicer", self.tag, "2", receipt)
                self.assertFalse(receipt.exists())
                self.jobs = promotion.JOBS
                promotion.select("rotatrix/PrusaSlicer", self.tag, "2", receipt)
                self.assertEqual(json.loads(receipt.read_text())["commit"], self.sha)

    def test_attestation_run_and_event(self):
        record = {"repository": "rotatrix/PrusaSlicer", "run_id": 2, "run_attempt": 2, "build_event": "push"}
        cert = {"runInvocationURI": "https://github.com/rotatrix/PrusaSlicer/actions/runs/2/attempts/1", "buildTrigger": "push"}
        result = [{"verificationResult": {"signature": {"certificate": cert}}}]
        promotion.verify_certificate(result, record)
        for key, value in (("run_id", 3), ("build_event", "workflow_dispatch")):
            with self.assertRaises(ValueError):
                promotion.verify_certificate(result, {**record, key: value})
        with self.assertRaises(ValueError):
            promotion.verify_certificate([], record)

    def test_verify_rejects_moved_tag_and_changed_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            receipt = directory / "promotion.json"
            record = {"repository": "rotatrix/PrusaSlicer", "repository_id": 1,
                      "commit": self.sha, "tag": self.tag, "run_id": 2, "run_attempt": 1,
                      "source_ref": "refs/heads/rotatrix/work/version_2.9.6",
                      "build_event": "workflow_dispatch", "artifacts": self.artifacts}
            receipt.write_text(json.dumps(record))
            with patch.object(promotion, "tag_commit", return_value="c" * 40), self.assertRaisesRegex(ValueError, "tag moved"):
                promotion.verify(receipt, directory)
            (directory / self.artifacts[0]["name"]).write_bytes(b"changed")
            with patch.object(promotion, "tag_commit", return_value=self.sha), patch.object(promotion, "api", return_value=self.run), self.assertRaisesRegex(ValueError, "hash mismatch"):
                promotion.verify(receipt, directory)


if __name__ == "__main__":
    unittest.main()
