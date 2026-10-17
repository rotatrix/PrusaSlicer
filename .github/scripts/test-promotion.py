import copy
import hashlib
import json
from pathlib import Path
import importlib.util
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('promote', Path(__file__).with_name('promote-release.py'))
promote = importlib.util.module_from_spec(spec)
sys.modules['promote'] = promote
spec.loader.exec_module(promote)

SHA = 'a' * 40


def run():
    return dict(id=42, repository=dict(full_name=promote.REPO),
                head_repository=dict(full_name=promote.REPO), path=promote.WORKFLOW,
                event='push', head_branch='rotatrix/work/version_2.9.6', head_sha=SHA,
                status='completed', conclusion='success', run_attempt=2)


class PromotionChecks(unittest.TestCase):
    def test_trusted_branch_and_manual_build(self):
        for event in ('push', 'workflow_dispatch'):
            candidate = run()
            candidate['event'] = event
            promote.validate_run(candidate, SHA)

    def test_reject_wrong_origin_workflow_event_ref_sha_or_status(self):
        for key, value in (
            ('repository', dict(full_name='someone/PrusaSlicer')),
            ('head_repository', dict(full_name='someone/PrusaSlicer')),
            ('path', '.github/workflows/untrusted.yml'), ('event', 'pull_request'),
            ('head_branch', 'main'), ('head_sha', 'b' * 40),
            ('status', 'in_progress'), ('conclusion', 'failure')):
            candidate = run()
            candidate[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                promote.validate_run(candidate, SHA)

    def test_all_required_platform_records_and_packages(self):
        names = promote.expected(SHA)
        self.assertEqual(len(names), 4)
        self.assertIn(f'PrusaSlicer-Rotatrix-linux-x64-{SHA[:12]}.flatpak', names)
        self.assertFalse(any(n.startswith('build-') for n in names))
        self.assertFalse(any('installer' in n or 'double' in n for n in names))
        self.assertFalse(any('linux-arm64' in n or 'sources-' in n for n in names))
        self.assertTrue(all(len(n) < 80 and '/' not in n for n in names))

    def test_prepare_downloads_exact_packages_and_rejects_tampering(self):
        payloads = {name: name.encode() for name in promote.expected(SHA)}
        artifacts = [dict(id=i, name=name, expired=False,
                          workflow_run=dict(id=42, head_sha=SHA),
                          digest='sha256:' + hashlib.sha256(data).hexdigest())
                     for i, (name, data) in enumerate(payloads.items(), 1)]
        jobs = [dict(name=p, conclusion='success') for p in promote.JOBS]
        for tamper in (False, True):
            def download(command, stdout, **kwargs):
                artifact_id = int(command[-1].split('/')[-2])
                stdout.write(payloads[artifacts[artifact_id - 1]['name']] + (b'changed' if tamper else b''))
            with tempfile.TemporaryDirectory() as root, \
                    patch('promote.require_new_release'), patch('promote.tag_commit', return_value=SHA), \
                    patch('promote.api', side_effect=[{'status': 'identical'}, run()]), \
                    patch('promote.pages', side_effect=[jobs, artifacts]), \
                    patch('promote.subprocess.run', side_effect=download):
                if tamper:
                    with self.assertRaisesRegex(ValueError, 'digest mismatch'):
                        promote.prepare('version_2.9.6-rotatrix.1', 42, Path(root))
                else:
                    result = promote.prepare('version_2.9.6-rotatrix.1', 42, Path(root))
                    self.assertEqual(result['run_id'], 42)
                    self.assertEqual(len(result['artifacts']), 4)

    def test_existing_draft_or_public_release_is_rejected(self):
        for draft in (True, False):
            with patch('promote.subprocess.check_output', return_value=json.dumps(
                    [[dict(tag_name='tag', draft=draft)]])), self.assertRaisesRegex(ValueError, 'exists'):
                promote.require_new_release('tag')

    def test_draft_contains_only_four_downloads_and_checksums(self):
        with tempfile.TemporaryDirectory() as root:
            directory = Path(root)
            records = []
            for name in promote.expected(SHA):
                path = directory / name
                path.write_bytes(b'app')
                records.append(dict(name=name, sha256=promote.digest(path)))
            (directory / 'signed').mkdir()
            for arch in ('arm64', 'x86_64'):
                (directory / 'signed' / f'PrusaSlicer-macos-{arch}-{SHA}.tar.gz').write_bytes(b'signed')
            plan = dict(repository=promote.REPO, tag='version_2.9.6-rotatrix.1-beta.1',
                        commit=SHA, run_id=42, artifacts=records, prerelease=True)
            (directory / 'promotion.json').write_text(json.dumps(plan))
            with patch('promote.tag_commit', return_value=SHA), patch('promote.require_new_release'), \
                    patch('promote.subprocess.run') as command:
                promote.draft(directory)
                args = command.call_args.args[0]
                self.assertIn('--draft', args)
                self.assertIn('--verify-tag', args)
                self.assertIn('--prerelease', args)
                self.assertEqual(len((directory / 'SHA256SUMS').read_text().splitlines()), 4)
                self.assertFalse(any('macos-app-' in arg or arg.endswith('.json') for arg in args))
            with patch('promote.tag_commit', return_value='b' * 40), self.assertRaisesRegex(ValueError, 'Tag moved'):
                promote.draft(directory)

    def test_missing_platform_or_artifact_is_rejected(self):
        jobs = [dict(name=name, conclusion='success') for name in promote.JOBS]
        for selected_jobs, artifacts in ((jobs[:-1], []), (jobs, [])):
            with tempfile.TemporaryDirectory() as root, patch('promote.require_new_release'), \
                    patch('promote.tag_commit', return_value=SHA), \
                    patch('promote.api', side_effect=[{'status': 'identical'}, run()]), \
                    patch('promote.pages', side_effect=[selected_jobs, artifacts]), \
                    patch('promote.subprocess.run') as download:
                with self.assertRaises(ValueError):
                    promote.prepare('version_2.9.6-rotatrix.1', 42, Path(root))
                download.assert_not_called()

    def test_expired_duplicate_and_wrong_build_artifacts_fail(self):
        jobs = [dict(name=name, conclusion='success') for name in promote.JOBS]
        original = [dict(id=i, name=name, expired=False,
                         workflow_run=dict(id=42, head_sha=SHA), digest='sha256:' + 'b' * 64)
                    for i, name in enumerate(sorted(promote.expected(SHA)), 1)]
        for mutation in ('expired', 'duplicate', 'wrong-sha', 'wrong-run'):
            artifacts = copy.deepcopy(original)
            if mutation == 'expired':
                artifacts[0]['expired'] = True
            elif mutation == 'duplicate':
                artifacts.append(artifacts[0])
            elif mutation == 'wrong-sha':
                artifacts[0]['workflow_run']['head_sha'] = 'b' * 40
            else:
                artifacts[0]['workflow_run']['id'] = 99
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as root, \
                    patch('promote.require_new_release'), patch('promote.tag_commit', return_value=SHA), \
                    patch('promote.api', side_effect=[{'status': 'identical'}, run()]), \
                    patch('promote.pages', side_effect=[jobs, artifacts]), \
                    patch('promote.subprocess.run') as download:
                with self.assertRaises(ValueError):
                    promote.prepare('version_2.9.6-rotatrix.1', 42, Path(root))
                download.assert_not_called()



if __name__ == '__main__':
    unittest.main()
