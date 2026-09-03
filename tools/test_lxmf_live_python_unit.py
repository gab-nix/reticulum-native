#!/usr/bin/env python3
"""Harness safety tests; no upstream imports or network required."""
import hashlib
from pathlib import Path
import signal
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import Mock, patch

import test_lxmf_live_python as harness


class HarnessSafety(unittest.TestCase):
    def test_recovering_sanitizer_diagnostic_rejected(self):
        harness.check_sanitizers(b"")
        with self.assertRaisesRegex(harness.VerificationFailure, "native_sanitizer_diagnostic"):
            harness.check_sanitizers(b"source.c: runtime error: synthetic test")

    def test_correct_clean_pin(self):
        with patch.object(harness.subprocess, "run", side_effect=[
            SimpleNamespace(returncode=0, stdout=harness.RNS_COMMIT + "\n"),
            SimpleNamespace(returncode=0),
        ]):
            harness.pinned_checkout(Path("unused"), harness.RNS_COMMIT)

    def test_wrong_pin_rejected(self):
        with patch.object(harness.subprocess, "run", return_value=
                          SimpleNamespace(returncode=0, stdout="wrong")):
            with self.assertRaisesRegex(harness.VerificationFailure, "upstream_revision_mismatch"):
                harness.pinned_checkout(Path("unused"), harness.RNS_COMMIT)

    def test_dirty_pin_rejected(self):
        with patch.object(harness.subprocess, "run", side_effect=[
            SimpleNamespace(returncode=0, stdout=harness.RNS_COMMIT),
            SimpleNamespace(returncode=1),
        ]):
            with self.assertRaisesRegex(harness.VerificationFailure, "upstream_tracked_changes"):
                harness.pinned_checkout(Path("unused"), harness.RNS_COMMIT)

    def test_streaming_binary_fingerprint(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "synthetic"
            value = bytes(range(256)) * 300
            path.write_bytes(value)
            self.assertEqual(harness.fingerprint(path), hashlib.sha256(value).hexdigest())

    def test_live_process_group_stopped(self):
        process = Mock(pid=12345)
        process.poll.return_value = None
        with patch.object(harness.os, "killpg") as kill:
            harness.stop_process_group(process)
            kill.assert_called_once_with(12345, signal.SIGKILL)
        process.wait.assert_called_once_with(timeout=5)

    def test_finished_child_not_signalled(self):
        process = Mock()
        process.poll.return_value = 0
        with patch.object(harness.os, "killpg") as kill:
            harness.stop_process_group(process)
            kill.assert_not_called()

    def test_exit_race_reaped(self):
        process = Mock(pid=12345)
        process.poll.return_value = None
        with patch.object(harness.os, "killpg", side_effect=ProcessLookupError):
            harness.stop_process_group(process)
        process.wait.assert_called_once_with(timeout=5)


if __name__ == "__main__":
    unittest.main()
