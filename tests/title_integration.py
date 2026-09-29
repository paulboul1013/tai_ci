#!/usr/bin/env python3
"""Compare native window titles with tests/fixtures/title_oracle.json.

    title_integration.py TEST_PAGE_TITLE [TEST_TITLE_WINDOW BROWSER_CSS]

test_page_title prints tai_page_title() for each tests/title_fixture.py
markup page. The oracle's fallback title "Tai Gar" is the empty string at the
page layer. test_title_window replays the oracle scenarios in the real
presentation loop under SDL's dummy driver; there the fallback is the renamed
browser name "Tai Ci" (an intentional difference, PORTING_PLAN.md).

Not compared, with the reason:
* committed_title: native windows read the committed page directly, so it is
  get_title.
* fresh_window.created: the observer first runs after the window's first
  frame; native reports that frame as presented_before_commit.
"""

import json
import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests" / "fixtures" / "title_oracle.json"
PYTHON_NAME = "Tai Gar"
NATIVE_NAME = "Tai Ci"
SKIPPED_KEYS = {"committed_title"}
SKIPPED_STEPS = {("fresh_window", "created")}

sys.path.insert(0, str(ROOT / "tests"))
import https_fixture  # noqa: E402
import title_fixture  # noqa: E402


def check_page_titles(binary, oracle):
    paths = list(title_fixture.MARKUP_PAGES)
    with tempfile.TemporaryDirectory(prefix="tai-title-") as directory:
        files = []
        for index, path in enumerate(paths):
            file = pathlib.Path(directory) / "{}.html".format(index)
            file.write_text(title_fixture.MARKUP_PAGES[path][0],
                            encoding="utf-8")
            files.append(str(file))
        output = subprocess.run([binary, *files], check=True,
                                capture_output=True, text=True).stdout
    titles = json.loads(output)
    failures = []
    for path, title in zip(paths, titles):
        expected = oracle["markup"][path]["get_title"]
        if expected == PYTHON_NAME:
            expected = ""
        if title != expected:
            failures.append("{}: native {!r}, oracle {!r}".format(
                path, title, expected))
    return failures


def renamed(value):
    """The oracle answers with the fallback name mapped to native's."""
    if isinstance(value, dict):
        return {key: renamed(item) for key, item in value.items()
                if key not in SKIPPED_KEYS}
    if isinstance(value, list):
        return [renamed(item) for item in value]
    return NATIVE_NAME if value == PYTHON_NAME else value


def check_windows(binary, css, oracle):
    expected = renamed(oracle)
    for scenario, step in SKIPPED_STEPS:
        expected[scenario].pop(step)
    server = title_fixture.TitleServer()
    seen = set()
    failures = []
    process = None
    with tempfile.TemporaryDirectory(prefix="tai-title-tls-") as directory:
        https = https_fixture.FixtureServers(
            https_fixture.make_material(pathlib.Path(directory)))
        try:
            process = subprocess.Popen(
                [binary, str(server.port), css,
                 str(https.untrusted.server_port),
                 *title_fixture.MARKUP_PAGES],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                bufsize=1)
            for line in process.stdout:
                line = line.rstrip("\n")
                if line == "DONE":
                    break
                verb, _, rest = line.partition(" ")
                if verb == "SEEN":
                    server.wait_seen(rest)
                elif verb == "RELEASE":
                    server.release(rest)
                elif verb == "STATE":
                    scenario, step, payload = rest.split(" ", 2)
                    payload = https.normalize(server.normalize(payload))
                    label = "{}.{}".format(scenario, step)
                    seen.add((scenario, step))
                    want = expected.get(scenario, {}).get(step)
                    got = json.loads(payload)
                    if want is None:
                        failures.append("{}: no oracle checkpoint".format(
                            label))
                    elif want != got:
                        failures.append("{}: oracle {!r}, native {!r}".format(
                            label, want, got))
                else:
                    raise AssertionError("unexpected output {!r}".format(line))
                process.stdin.write("ok\n")
                process.stdin.flush()
            else:
                raise AssertionError("test_title_window ended without DONE")
            if process.wait(timeout=10) != 0:
                raise AssertionError("test_title_window failed")
        finally:
            if process is not None and process.poll() is None:
                process.kill()
                process.wait()
            server.close()
            https.close()
    failures.extend(
        "{}.{}: not exercised natively".format(scenario, step)
        for scenario, steps in sorted(expected.items())
        for step in sorted(steps) if (scenario, step) not in seen)
    return failures, len(seen)


def main():
    if len(sys.argv) not in (2, 4):
        raise SystemExit(__doc__)
    oracle = json.loads(FIXTURE.read_text(encoding="utf-8"))
    failures = check_page_titles(sys.argv[1], oracle)
    checkpoints = 0
    if len(sys.argv) == 4:
        window_failures, checkpoints = check_windows(sys.argv[2], sys.argv[3],
                                                     oracle)
        failures.extend(window_failures)
    if failures:
        raise SystemExit("title differences:\n  " + "\n  ".join(failures))
    print("native titles match tests/fixtures/title_oracle.json "
          "({} markup pages, {} window checkpoints)".format(
              len(title_fixture.MARKUP_PAGES), checkpoints))


if __name__ == "__main__":
    main()
