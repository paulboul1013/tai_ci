#!/usr/bin/env python3
"""Compare native overflow-container scrolling with the Python oracle.

Serves tests/overflow_scroll_fixture.py on 127.0.0.1, runs
test_overflow_scroll_window against it, and compares every STATE the test
reports with the same checkpoint in tests/fixtures/overflow_scroll_oracle.json.
Scroll offsets come from font metrics, so they match to 0.0001px.

    python3 tests/overflow_scroll_integration.py TEST_WINDOW BROWSER_CSS
"""

import json
import math
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests" / "fixtures" / "overflow_scroll_oracle.json"
sys.path.insert(0, str(ROOT / "tests"))
import overflow_scroll_fixture  # noqa: E402

TOLERANCE = 0.0001


def same(want, actual):
    if isinstance(want, dict) and isinstance(actual, dict):
        return (want.keys() == actual.keys()
                and all(same(want[key], actual[key]) for key in want))
    if (isinstance(want, (int, float))
            and isinstance(actual, (int, float))):
        return math.isclose(want, actual, rel_tol=0.0, abs_tol=TOLERANCE)
    return want == actual


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    expected = json.loads(FIXTURE.read_text(encoding="utf-8"))
    server = overflow_scroll_fixture.OverflowScrollServer()
    seen = set()
    failures = []
    process = None
    try:
        process = subprocess.Popen(
            [sys.argv[1], str(server.port), sys.argv[2]],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
            bufsize=1)
        for line in process.stdout:
            line = line.rstrip("\n")
            if line == "DONE":
                break
            verb, _, rest = line.partition(" ")
            if verb != "STATE":
                raise AssertionError(
                    "unexpected child output {!r}".format(line))
            scenario, step, payload = rest.split(" ", 2)
            label = "{}.{}".format(scenario, step)
            seen.add((scenario, step))
            want = expected.get(scenario, {}).get(step)
            actual = json.loads(payload)
            if want is None:
                failures.append("{}: no oracle checkpoint".format(label))
            elif not same(want, actual):
                failures.append("{}: oracle {!r}, native {!r}".format(
                    label, want, actual))
            process.stdin.write("ok\n")
            process.stdin.flush()
        else:
            failures.append("test_overflow_scroll_window ended without DONE")
        if process.wait(timeout=10) != 0:
            failures.append("test_overflow_scroll_window failed")
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait()
        server.close()
    missing = sorted("{}.{}".format(scenario, step)
                     for scenario, steps in expected.items()
                     for step in steps if (scenario, step) not in seen)
    failures.extend("{}: not exercised natively".format(label)
                    for label in missing)
    if failures:
        raise SystemExit(
            "native overflow scrolling differs from the oracle:\n  "
            + "\n  ".join(failures))
    print("native overflow scrolling matches "
          "tests/fixtures/overflow_scroll_oracle.json "
          "({} checkpoints)".format(len(seen)))


if __name__ == "__main__":
    main()
