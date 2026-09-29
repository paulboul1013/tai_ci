#!/usr/bin/env python3
"""Compare native multi-window behaviour with the frozen Python oracle.

Serves tests/new_window_fixture.py on 127.0.0.1, runs test_new_window (the
real presentation loop under SDL's dummy driver), answers its commands and
compares every checkpoint with tests/fixtures/new_window_oracle.json.

Not compared, with the reason:
* requested_urls: Python's handle_new_window asks for
  https://browser.engineering/, which the probe redirects to /home; native
  opens the app's New Tab URL, which the test sets to /home (decision D4).

Intentional differences (docs/new-window-plan.md):
* D2: a repeated Ctrl+N key event opens no window, so from modifiers.key_repeat
  on native has one window fewer than Python.

    python3 tests/new_window_integration.py TEST_NEW_WINDOW BROWSER_CSS
"""

import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests" / "fixtures" / "new_window_oracle.json"
sys.path.insert(0, str(ROOT / "tests"))
import new_window_fixture  # noqa: E402

SKIPPED_KEYS = set()
SKIPPED_STEPS = {"requested_urls"}


def expectation(expected):
    """Apply the intentional differences to the oracle answers."""
    for step in ("key_repeat", "ctrl_m"):
        expected["modifiers"][step] -= 1
    # Python's quit_all result is the scenario itself, not a named step.
    expected["quit_all"] = {"result": expected["quit_all"]}
    for scenario in expected.values():
        for step in SKIPPED_STEPS:
            scenario.pop(step, None)
    return expected


def compare(want, got, label, failures):
    if isinstance(want, dict) and isinstance(got, dict):
        for key in sorted(set(want) | set(got)):
            if key in SKIPPED_KEYS:
                continue
            if key not in want or key not in got:
                failures.append("{}.{}: oracle {!r}, native {!r}".format(
                    label, key, want.get(key), got.get(key)))
            else:
                compare(want[key], got[key], "{}.{}".format(label, key),
                        failures)
    elif isinstance(want, list) and isinstance(got, list):
        if len(want) != len(got):
            failures.append("{}: oracle {!r}, native {!r}".format(
                label, want, got))
        for index, (a, b) in enumerate(zip(want, got)):
            compare(a, b, "{}[{}]".format(label, index), failures)
    elif isinstance(want, float) or isinstance(got, float):
        if abs(float(want) - float(got)) >= 0.001:
            failures.append("{}: oracle {!r}, native {!r}".format(
                label, want, got))
    elif want != got:
        failures.append("{}: oracle {!r}, native {!r}".format(
            label, want, got))


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    expected = expectation(json.loads(FIXTURE.read_text(encoding="utf-8")))
    server = new_window_fixture.NewWindowServer()
    seen = set()
    failures = []
    process = None
    mark = 0

    def check(scenario, step, actual):
        label = "{}.{}".format(scenario, step)
        seen.add((scenario, step))
        if step not in expected.get(scenario, {}):
            failures.append("{}: no oracle checkpoint".format(label))
        else:
            compare(expected[scenario][step], actual, label, failures)

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
            if verb == "MARK":
                mark = server.mark()
            elif verb == "SEEN":
                server.wait_seen(rest, mark)
            elif verb == "RELEASE":
                server.release(rest)
            elif verb == "STATE":
                scenario, step, payload = rest.split(" ", 2)
                check(scenario, step, json.loads(server.normalize(payload)))
            elif verb == "REQUESTS":
                scenario, step = rest.split(" ")
                check(scenario, step, server.since(mark))
            else:
                raise AssertionError("unexpected output {!r}".format(line))
            process.stdin.write("ok\n")
            process.stdin.flush()
        else:
            raise AssertionError("test_new_window ended without DONE")
        if process.wait(timeout=10) != 0:
            raise AssertionError("test_new_window failed")
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
        raise SystemExit("native new-window behaviour differs from the "
                         "oracle:\n  " + "\n  ".join(failures))
    print("native new windows match tests/fixtures/new_window_oracle.json "
          "({} checkpoints; key repeat is the recorded difference)".format(
              len(seen)))


if __name__ == "__main__":
    main()
