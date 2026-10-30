#!/usr/bin/env python3
"""Check that native tabs never accept a stale load completion.

Serves tests/stale_fixture.py on 127.0.0.1, runs test_tabset_stale, answers
its commands, and compares every STATE it reports with the same checkpoint in
tests/fixtures/stale_oracle.json (G1-G3 in docs/stale-callback-plan.md). The
native-only visited-snapshot scenario (G4) reports one VISITED line, checked
against NATIVE_VISITED below.

Intentional differences (PORTING_PLAN.md):
* A navigation superseded while pending: native leaves it out of history
  (Python keeps it), so only history and history_index are adjusted.

    python3 tests/stale_integration.py TEST_TABSET_STALE BROWSER_CSS
"""

import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests" / "fixtures" / "stale_oracle.json"
sys.path.insert(0, str(ROOT / "tests"))
import stale_fixture  # noqa: E402

COMPARED = ("url", "heading", "history", "history_index", "can_go_back",
            "can_go_forward", "requests")

# G4, native only: (at commit, after more pumping, after a restyle). The
# loader styles with the visited set copied when the request was sent; after
# commit the page's net reads the app's live set, so the next restyle turns
# the link visited.
NATIVE_VISITED = (0, 0, 1)


def superseded_expectation(expected):
    """Decision (2026-10-30): a navigation superseded while still pending
    never commits, so native leaves it out of history (Python keeps it)."""
    superseded = {"css_late": ("/css-a",), "js_late": ("/js-a",),
                  "out_of_order_newest_first": ("/slow-a", "/slow-b"),
                  "out_of_order_oldest_first": ("/slow-a", "/slow-b")}
    for scenario, paths in superseded.items():
        for state in expected[scenario].values():
            state["history"] = [url for url in state["history"]
                                if not url.endswith(paths)]
            state["history_index"] = len(state["history"]) - 1
    return expected


def compare(expected, actual, label, failures):
    for key in COMPARED:
        if key not in expected and key not in actual:
            continue
        want, got = expected.get(key), actual.get(key)
        if want != got:
            failures.append("{}.{}: oracle {!r}, native {!r}".format(
                label, key, want, got))


def run_child(command, server, expected, seen, failures):
    process = None
    mark = 0
    visited = None
    try:
        process = subprocess.Popen(
            command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
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
            elif verb == "SERVED":
                server.wait_served(rest)
            elif verb == "RELEASE":
                server.release(rest)
            elif verb == "RESET":
                server.reset_gate(rest)
            elif verb == "VISITED":
                visited = tuple(int(value) for value in rest.split())
            elif verb == "STATE":
                scenario, step, requests, payload = rest.split(" ", 3)
                actual = json.loads(server.normalize(payload))
                if requests == "1":
                    actual["requests"] = server.since(mark)
                label = "{}.{}".format(scenario, step)
                seen.add((scenario, step))
                if step not in expected.get(scenario, {}):
                    failures.append("{}: no oracle checkpoint".format(label))
                else:
                    compare(expected[scenario][step], actual, label, failures)
            else:
                raise AssertionError("unexpected child output {!r}".format(line))
            process.stdin.write("ok\n")
            process.stdin.flush()
        else:
            raise AssertionError("{} ended without DONE".format(command[0]))
        if process.wait(timeout=10) != 0:
            raise AssertionError("{} failed".format(command[0]))
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait()
    return visited


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    expected = superseded_expectation(
        json.loads(FIXTURE.read_text(encoding="utf-8")))
    server = stale_fixture.StaleServer()
    seen = set()
    failures = []
    try:
        visited = run_child([sys.argv[1], str(server.port), sys.argv[2]],
                            server, expected, seen, failures)
        if any(path.startswith("/stale-beacon") for path in server.since(0)):
            failures.append("the superseded script ran: /stale-beacon was "
                            "requested")
    finally:
        server.close()

    missing = sorted("{}.{}".format(scenario, step)
                     for scenario, steps in expected.items()
                     for step in steps if (scenario, step) not in seen)
    failures.extend("{}: not exercised natively".format(label)
                    for label in missing)
    if visited != NATIVE_VISITED:
        failures.append("visited_snapshot: expected (commit, quiet, restyle) "
                        "{}, native {}".format(NATIVE_VISITED, visited))
    if failures:
        raise SystemExit("native stale completions differ:\n  " +
                         "\n  ".join(failures))
    print("native stale completions match tests/fixtures/stale_oracle.json "
          "({} checkpoints); visited snapshot (commit, quiet, restyle) = "
          "{}".format(len(seen), visited))


if __name__ == "__main__":
    main()
