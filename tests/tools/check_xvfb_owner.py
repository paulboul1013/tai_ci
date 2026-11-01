#!/usr/bin/env python3
"""Fail when a test script starts Xvfb itself instead of tests/tools/xvfb.sh.

Two copies of display selection let concurrent tests share one display
(clicks landed on another browser's window).

    python3 tests/tools/check_xvfb_owner.py [ROOT]
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2])
OWNER = pathlib.Path("tests/tools/xvfb.sh")
# Shell: Xvfb followed by a display argument. Python: an argv list that
# starts with "Xvfb". Docstrings and tool lists ("Xvfb", "xdotool") pass.
SHELL = re.compile(r"""(^|[;&|(]\s*|\bsetsid\s+|\bexec\s+)Xvfb\s+["']?[:$]""")
PYTHON = re.compile(r"""\[\s*["']Xvfb["']\s*,""")


def main():
    failures = []
    for path in sorted((ROOT / "tests").rglob("*")):
        relative = path.relative_to(ROOT)
        if path.suffix not in (".py", ".sh") or relative == OWNER:
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            pattern = SHELL if path.suffix == ".sh" else PYTHON
            if line.lstrip().startswith("#") or not pattern.search(line.strip()):
                continue
            failures.append("{}:{}: starts Xvfb directly; use {} (it owns the "
                            "display lock)".format(relative, number, OWNER))
    for failure in failures:
        print(failure)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
