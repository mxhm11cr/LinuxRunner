#!/usr/bin/env python3
"""Verify both physical cleanup receipts and emit numeric, allowlisted evidence.

XCTest success alone is insufficient: independently retrieve the guest monitor
log from the selected root's Downloads exchange after the device run.
"""
import argparse
import json
from pathlib import Path
import re

FIELDS = ("segments", "backing_fds", "bytes", "attachments", "capture_segments",
          "scopes", "close_failures")


def verify(log, ui):
    assert ui["didError"] is False, "UI runner reported failure"
    data = ui["data"]
    assert data["summary"]["status"] == "SUCCEEDED"
    cases = data["testCases"]
    assert len(cases) == 1 and cases[0]["test"] == "testReferenceCaptureForcedTermination"
    assert cases[0]["status"] == "passed"
    assert not data["diagnostics"]["errors"] and not data["diagnostics"]["testFailures"]
    lines = iter(log.splitlines())
    cycles = []

    def counters(prefix):
        line = next(lines)
        assert line.startswith(prefix + " "), f"missing {prefix} counters"
        pairs = line[len(prefix) + 1:].split()
        assert len(pairs) == len(FIELDS)
        result = {}
        for item, key in zip(pairs, FIELDS):
            assert re.fullmatch(re.escape(key) + r"=\d+", item), "unexpected counter"
            result[key] = int(item.split("=", 1)[1])
        return result

    for cycle in range(1, 6):
        baseline = counters("baseline")
        assert next(lines) == f"CAPTURE_MONITOR_ARMED_{cycle}"
        live, after = counters("live"), counters("after")
        assert after == baseline, "resource totals did not return to baseline"
        assert all(value == 0 for value in baseline.values()), "unexpected initial resources"
        assert live["capture_segments"] > 0 and live["scopes"] == 1
        assert live["segments"] == live["capture_segments"] == live["backing_fds"]
        assert live["bytes"] > 0 and live["attachments"] > 0 and live["close_failures"] == 0
        phase = "ready-kill" if cycle < 5 else "normal-stop"
        assert next(lines) == f"cycle={cycle} result=PASS phase={phase} same_runtime=yes"
        assert next(lines) == f"CAPTURE_CLEANUP_CYCLE_{cycle}_PASS"
        cycles.append(dict(cycle=cycle, phase=phase, baseline=baseline, live=live, after=after))
    assert next(lines) == "CAPTURE_CLEANUP_ALL_PASS same_runtime=yes cycles=5"
    assert not list(lines), "unexpected trailing output"
    return {"result": "PASS", "same_runtime": True, "cycles": cycles}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--ui-result", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = verify(args.log.read_text(), json.loads(args.ui_result.read_text()))
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print("CAPTURE_CLEANUP_EVIDENCE_PASS cycles=5 forced=4 normal=1 same_runtime=yes")
