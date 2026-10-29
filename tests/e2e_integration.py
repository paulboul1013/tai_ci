#!/usr/bin/env python3
"""Compare the native full-pipeline E2E run with the Python oracle.

Serves tests/e2e_fixture.py on 127.0.0.1, runs test_e2e_window against it,
turns every STATE it reports into a checkpoint with tests/e2e_shape.py,
attaches the requests the server saw since the previous checkpoint, and
compares it with the same checkpoint in tests/fixtures/e2e_oracle.json.
Numbers match to 0.0001px. A failure names scenario.step and the JSON path
of the first differing field.

    python3 tests/e2e_integration.py TEST_WINDOW BROWSER_CSS
"""

import json
import math
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests" / "fixtures" / "e2e_oracle.json"
sys.path.insert(0, str(ROOT / "tests"))
import e2e_fixture  # noqa: E402
import e2e_shape  # noqa: E402

TOLERANCE = 0.0001
# The window width test_e2e_window uses; it must be the oracle tab width.
WIDTH = 800


def first_difference(want, actual, path):
    """The JSON path and both values of the first difference, or None."""
    if isinstance(want, dict) and isinstance(actual, dict):
        if want.keys() != actual.keys():
            return path, sorted(want), sorted(actual)
        for key in want:
            found = first_difference(want[key], actual[key],
                                     path + "/" + key)
            if found:
                return found
        return None
    if isinstance(want, list) and isinstance(actual, list):
        for index, (left, right) in enumerate(zip(want, actual)):
            found = first_difference(left, right,
                                     "{}/{}".format(path, index))
            if found:
                return found
        if len(want) != len(actual):
            return path + "/length", len(want), len(actual)
        return None
    if (isinstance(want, (int, float)) and isinstance(actual, (int, float))
            and not isinstance(want, bool) and not isinstance(actual, bool)):
        if math.isclose(want, actual, rel_tol=0.0, abs_tol=TOLERANCE):
            return None
        return path, want, actual
    return None if want == actual else (path, want, actual)


def native_checkpoint(raw, requests):
    return e2e_shape.checkpoint(
        raw["page"]["url"], raw["title"], raw["back"], raw["forward"],
        raw["scroll"], e2e_shape.native_page(raw["page"]), raw["inputs"],
        requests)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    oracle = json.loads(FIXTURE.read_text(encoding="utf-8"))
    assert oracle["viewport"]["width"] == WIDTH, oracle["viewport"]
    expected = oracle["scenarios"]
    server = e2e_fixture.E2EServer()
    seen = set()
    failures = []
    process = None
    mark = 0
    try:
        process = subprocess.Popen(
            [sys.argv[1], str(server.port), sys.argv[2], str(WIDTH)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
            bufsize=1)
        for line in process.stdout:
            line = line.rstrip("\n")
            if line == "DONE":
                break
            verb, _, rest = line.partition(" ")
            if verb != "STATE":
                raise AssertionError(
                    "unexpected child output {!r}".format(line[:200]))
            scenario, step, payload = rest.split(" ", 2)
            label = "{}.{}".format(scenario, step)
            seen.add((scenario, step))
            now = server.mark()
            actual = native_checkpoint(json.loads(payload),
                                       server.since(mark))
            mark = now
            want = {item["step"]: item["checkpoint"]
                    for item in expected.get(scenario, [])}.get(step)
            if want is None:
                failures.append("{}: no oracle checkpoint".format(label))
            else:
                found = first_difference(want, actual, "")
                if found:
                    path, oracle_value, native_value = found
                    failures.append("{} {}: oracle {}, native {}".format(
                        label, path or "/",
                        json.dumps(oracle_value)[:300],
                        json.dumps(native_value)[:300]))
            process.stdin.write("ok\n")
            process.stdin.flush()
        else:
            failures.append("test_e2e_window ended without DONE")
        if process.wait(timeout=10) != 0:
            failures.append("test_e2e_window failed")
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait()
        server.close()
    missing = sorted("{}.{}".format(scenario, item["step"])
                     for scenario, steps in expected.items()
                     for item in steps
                     if (scenario, item["step"]) not in seen)
    failures.extend("{}: not exercised natively".format(label)
                    for label in missing)
    if failures:
        raise SystemExit("native E2E run differs from the oracle:\n  "
                         + "\n  ".join(failures))
    print("native E2E run matches tests/fixtures/e2e_oracle.json "
          "({} checkpoints)".format(len(seen)))


if __name__ == "__main__":
    main()
