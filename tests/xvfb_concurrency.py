#!/usr/bin/env python3
"""Start several tests/tools/xvfb.sh at once; each must own its own display.

Concurrent window tests once shared :90 because display selection checked
for a free number and then started Xvfb. Exits 77 (CTest skip) without Xvfb.

    python3 tests/xvfb_concurrency.py [HELPER]
"""

import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
HELPER = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "tests" / "tools" / "xvfb.sh")
COUNT = 6


def main():
    if not shutil.which("Xvfb") or not shutil.which("xset"):
        print("skip: Xvfb or xset missing")
        return 77
    with tempfile.TemporaryDirectory() as directory:
        starts = [subprocess.Popen([HELPER, "{}/xvfb{}.log".format(directory, index)],
                                   stdout=subprocess.PIPE, text=True)
                  for index in range(COUNT)]
        results = [start.communicate(timeout=60)[0].split() for start in starts]
        servers = [(display, int(pid)) for display, pid in (r for r in results if len(r) == 2)]
        try:
            failures = []
            if len(servers) != COUNT:
                failures.append("{} of {} helpers failed".format(COUNT - len(servers), COUNT))
            displays = [display for display, _ in servers]
            if len(set(displays)) != len(displays):
                failures.append("displays shared: {}".format(sorted(displays)))
            for display, pid in servers:
                lock = pathlib.Path("/tmp/.X{}-lock".format(display[1:]))
                owner = lock.read_text().strip() if lock.exists() else "missing"
                if owner != str(pid):
                    failures.append("{} lock holds {}, helper said {}".format(display, owner, pid))
        finally:
            for _, pid in servers:
                subprocess.run(["kill", str(pid)], stderr=subprocess.DEVNULL)
    for failure in failures:
        print(failure)
    print("ok: {} distinct displays".format(COUNT) if not failures else "FAIL")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
