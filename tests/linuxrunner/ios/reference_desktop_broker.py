#!/usr/bin/env python3
"""Companion to the explicitly opted-in physical Arch reference UI test.

Transfers an xterm launch into that root's existing managed request queue.
UIKit touches/text remain in the project's XCTest runner. No credentials or
existing guest configuration are changed.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import time
import uuid

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--device", required=True)
parser.add_argument("--bundle", required=True)
parser.add_argument("--root", required=True)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--timeout", type=int, default=1800)
parser.add_argument("--delay-after-ready", type=int, default=0)
probe = parser.add_mutually_exclusive_group()
probe.add_argument("--probe-source", type=Path)
probe.add_argument("--probe-binary", type=Path)
args = parser.parse_args()
if not re.fullmatch(r"[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}", args.root):
    parser.error("root must be an installed environment UUID")
if not 1 <= args.timeout <= 2400:
    parser.error("timeout must be between 1 and 2400 seconds")
if not 0 <= args.delay_after_ready <= 120:
    parser.error("delay-after-ready must be between 0 and 120 seconds")
args.output.mkdir(parents=True, exist_ok=True)
base = f"Library/Application Support/LinuxRunner/exchanges/{args.root}/.linuxrunner/gui"
common = ["--device", args.device, "--domain-type", "appDataContainer", "--domain-identifier", args.bundle]


def device(*command):
    return subprocess.run(["xcrun", "devicectl", "device", *command, *common, "--quiet", "--timeout", "10"],
                          capture_output=True, text=True, timeout=15)


if args.probe_source or args.probe_binary:
    local = args.probe_source or args.probe_binary
    if not local.is_file():
        parser.error("probe input must be an existing local file")
    filename = "capture-probe-fixture.c" if args.probe_source else "capture-probe-fixture"
    result = device("copy", "to", "--source", str(local), "--destination",
                    f"Library/Application Support/LinuxRunner/exchanges/{args.root}/{filename}")
    if result.returncode:
        raise SystemExit(result.stderr or "Could not transfer the controlled capture fixture")

deadline = time.monotonic() + args.timeout
listing = args.output / "sessions.json"
initial = device("info", "files", "--subdirectory", base, "--no-recurse", "--json-output", str(listing))
if initial.returncode:
    raise SystemExit(initial.stderr or "Could not enumerate the reference root's existing sessions")
existing = {entry["name"] for entry in json.loads(listing.read_text()).get("result", {}).get("files", [])}
print("Waiting for a new Arch managed desktop (start this broker before the UI test)", flush=True)
while time.monotonic() < deadline:
    listing = args.output / "sessions.json"
    result = device("info", "files", "--subdirectory", base, "--no-recurse", "--json-output", str(listing))
    if result.returncode == 0:
        files = json.loads(listing.read_text()).get("result", {}).get("files", [])
        sessions = [entry for entry in files if entry["name"].startswith("session-") and entry["name"] not in existing]
        if sessions:
            name = max(sessions, key=lambda entry: entry.get("metadata", {}).get("lastModDate", ""))["name"]
            if not re.fullmatch(r"session-[0-9a-f-]{36}", name):
                raise SystemExit("Unexpected managed session name")
            state = args.output / "desktop-state"
            result = device("copy", "from", "--source", f"{base}/{name}/desktop-state", "--destination", str(state))
            if result.returncode == 0 and "ready=1" in state.read_text().splitlines():
                time.sleep(args.delay_after_ready)
                request = args.output / f"launch-reference-{uuid.uuid4()}.sh"
                command = ("exec xterm -T 'LinuxRunner reference input' -geometry 80x24+40+40 "
                                   "-fa 'DejaVu Sans Mono' -fs 12 -e /bin/sh -c "
                                   "'printf \"LINUXRUNNER GUI %s\\n\" READY; exec /bin/bash --noprofile --norc -i'\n")
                if args.probe_source:
                    command = "set -e\ngcc -O2 /home/mobile/Downloads/capture-probe-fixture.c -lX11 -o /home/mobile/Downloads/capture-probe-fixture\n"
                elif args.probe_binary:
                    command = "set -e\nchmod 700 /home/mobile/Downloads/capture-probe-fixture\n"
                if args.probe_source or args.probe_binary:
                    command += "exec /home/mobile/Downloads/capture-probe-fixture\n"
                request.write_text("#!/bin/sh\n" + command)
                result = device("copy", "to", "--source", str(request),
                                "--destination", f"{base}/{name}/requests/{request.name}")
                if result.returncode:
                    raise SystemExit(result.stderr or "Could not submit the managed xterm request")
                (args.output / "selected-session.txt").write_text(name)
                print("Submitted controlled probe" if args.probe_source or args.probe_binary else "Submitted reference xterm through the managed desktop queue", flush=True)
                break
    time.sleep(5)
else:
    raise SystemExit("Reference desktop did not become ready before the broker timeout")
