#!/usr/bin/env python3
"""Compare native mailto handling with the frozen Python oracle answers.

Serves tests/mailto_fixture.py on 127.0.0.1, writes a fake external opener
that appends its argument to a log, runs test_mailto_window with it, and
compares every STATE the test reports, plus the URLs the opener received and
the requests sent since the step began, with the same checkpoint in
tests/fixtures/mailto_oracle.json.

    python3 tests/mailto_integration.py TEST_MAILTO_WINDOW BROWSER_CSS
"""

import json
import pathlib
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests" / "fixtures" / "mailto_oracle.json"
sys.path.insert(0, str(ROOT / "tests"))
import mailto_fixture  # noqa: E402

# The opener runs asynchronously: wait for the expected launches, then a
# little longer so that an unexpected extra launch is still caught.
OPEN_TIMEOUT = 5
OPEN_GRACE = 0.3

OPENER = """#!/bin/sh
printf '%s\\n' "$1" >> {log}
"""


def logged(log):
    if not log.exists():
        return []
    return log.read_text(encoding="utf-8").splitlines()


def opened_since(log, start, expected_count):
    deadline = time.monotonic() + OPEN_TIMEOUT
    while (len(logged(log)) - start < expected_count
           and time.monotonic() < deadline):
        time.sleep(0.01)
    time.sleep(OPEN_GRACE)
    return logged(log)[start:]


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    expected = json.loads(FIXTURE.read_text(encoding="utf-8"))
    server = mailto_fixture.MailtoServer()
    seen = set()
    failures = []
    process = None
    with tempfile.TemporaryDirectory(prefix="tai-mailto-") as directory:
        log = pathlib.Path(directory) / "opened.log"
        opener = pathlib.Path(directory) / "opener"
        opener.write_text(OPENER.format(log=log), encoding="utf-8")
        opener.chmod(0o755)
        try:
            process = subprocess.Popen(
                [sys.argv[1], str(server.port), sys.argv[2], str(opener)],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                bufsize=1)
            request_mark = log_mark = 0
            for line in process.stdout:
                line = line.rstrip("\n")
                if line == "DONE":
                    break
                verb, _, rest = line.partition(" ")
                if verb == "MARK":
                    request_mark = server.mark()
                    log_mark = len(logged(log))
                elif verb == "STATE":
                    scenario, step, payload = rest.split(" ", 2)
                    label = "{}.{}".format(scenario, step)
                    seen.add((scenario, step))
                    want = expected.get(scenario, {}).get(step)
                    if want is None:
                        failures.append("{}: no oracle checkpoint".format(
                            label))
                    else:
                        actual = json.loads(server.normalize(payload))
                        actual["opened"] = opened_since(
                            log, log_mark, len(want["opened"]))
                        actual["requests"] = server.since(request_mark)
                        for key in sorted(set(want) | set(actual)):
                            if want.get(key) != actual.get(key):
                                failures.append(
                                    "{}.{}: oracle {!r}, native {!r}".format(
                                        label, key, want.get(key),
                                        actual.get(key)))
                else:
                    raise AssertionError(
                        "unexpected child output {!r}".format(line))
                process.stdin.write("ok\n")
                process.stdin.flush()
            else:
                raise AssertionError("test_mailto_window ended without DONE")
            if process.wait(timeout=10) != 0:
                raise AssertionError("test_mailto_window failed")
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
        raise SystemExit("native mailto handling differs from the oracle:\n  "
                         + "\n  ".join(failures))
    print("native mailto handling matches tests/fixtures/mailto_oracle.json "
          "({} checkpoints)".format(len(seen)))


if __name__ == "__main__":
    main()
