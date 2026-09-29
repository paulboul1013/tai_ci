#!/usr/bin/env python3
"""Run test_browser_app against the 127.0.0.1 new-window fixture.

    python3 tests/browser_app_integration.py build/test_browser_app
"""

import pathlib
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import new_window_fixture  # noqa: E402


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: browser_app_integration.py TEST_BROWSER_APP")
    server = new_window_fixture.NewWindowServer()
    try:
        result = subprocess.run([sys.argv[1], str(server.port)], timeout=120,
                                check=False)
        requests = server.since(0)
    finally:
        server.close()
    if result.returncode != 0:
        raise SystemExit("test_browser_app failed with {}".format(
            result.returncode))
    checks = [item for item in requests if item["path"] == "/cookie-check"]
    if [item["cookie"] for item in checks] != [new_window_fixture.COOKIE]:
        raise SystemExit("cookie-check requests: {}".format(checks))
    delays = [item for item in requests if item["path"] == "/delay"]
    if len(delays) != 2:
        raise SystemExit("expected two in-flight /delay loads: {}".format(
            delays))


if __name__ == "__main__":
    main()
