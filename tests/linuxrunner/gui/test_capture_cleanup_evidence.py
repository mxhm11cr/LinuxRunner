"""Reject UI-only and incomplete physical capture-cleanup receipts."""
from copy import deepcopy
from pathlib import Path
import runpy
import unittest

verify = runpy.run_path(str(Path(__file__).resolve().parents[1] /
                           "ios/verify_capture_cleanup.py"))["verify"]


class CaptureEvidence(unittest.TestCase):
    def setUp(self):
        zero = "segments=0 backing_fds=0 bytes=0 attachments=0 capture_segments=0 scopes=0 close_failures=0"
        live = "segments=51 backing_fds=51 bytes=11064160 attachments=102 capture_segments=51 scopes=1 close_failures=0"
        lines = []
        for cycle in range(1, 6):
            phase = "ready-kill" if cycle < 5 else "normal-stop"
            lines += [f"baseline {zero}", f"CAPTURE_MONITOR_ARMED_{cycle}",
                      f"live {live}", f"after {zero}",
                      f"cycle={cycle} result=PASS phase={phase} same_runtime=yes",
                      f"CAPTURE_CLEANUP_CYCLE_{cycle}_PASS"]
        self.log = "\n".join(lines + ["CAPTURE_CLEANUP_ALL_PASS same_runtime=yes cycles=5"]) + "\n"
        self.ui = {"didError": False, "data": {
            "summary": {"status": "SUCCEEDED"},
            "testCases": [{"test": "testReferenceCaptureForcedTermination", "status": "passed"}],
            "diagnostics": {"errors": [], "testFailures": []}}}

    def test_complete_pair_passes(self):
        result = verify(self.log, self.ui)
        self.assertEqual(result["result"], "PASS")
        self.assertEqual(len(result["cycles"]), 5)

    def test_ui_success_cannot_replace_counters(self):
        for log in ("", "CAPTURE_CLEANUP_ALL_PASS same_runtime=yes cycles=5\n"):
            with self.subTest(log=log), self.assertRaises((AssertionError, StopIteration)):
                verify(log, self.ui)

    def test_leak_or_failed_close_rejected(self):
        for replacement in ("segments=1 backing_fds=0", "segments=0 backing_fds=1"):
            log = self.log.replace("after segments=0 backing_fds=0", "after " + replacement, 1)
            with self.subTest(replacement=replacement), self.assertRaises(AssertionError):
                verify(log, self.ui)
        with self.assertRaises(AssertionError):
            verify(self.log.replace("close_failures=0", "close_failures=1"), self.ui)

    def test_missing_duplicate_or_restarted_cycle_rejected(self):
        for log in (self.log.replace("CYCLE_3_PASS", "CYCLE_2_PASS"),
                    self.log.replace("same_runtime=yes", "same_runtime=no", 1),
                    self.log.rsplit("CAPTURE_CLEANUP_ALL_PASS", 1)[0],
                    self.log + "unqualified extra output\n"):
            with self.subTest(log=log[-100:]), self.assertRaises((AssertionError, StopIteration)):
                verify(log, self.ui)

    def test_guest_success_cannot_replace_ui_readiness(self):
        ui = deepcopy(self.ui)
        ui["data"]["testCases"][0]["status"] = "failed"
        with self.assertRaises(AssertionError):
            verify(self.log, ui)


if __name__ == "__main__":
    unittest.main()
