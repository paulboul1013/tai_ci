#!/usr/bin/env python3
"""Fail when a Python oracle or comparison test never runs under CTest, or
an acceptance detail page is unreachable from ACCEPTANCE.md.

A frozen oracle probe that is not registered can drift from browser.py
unnoticed (mailto_oracle_probe.py went unregistered for a whole slice).

    python3 tests/tools/check_test_registration.py
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SUFFIXES = ("_oracle_probe.py", "_integration.py", "_differential.py")


def main():
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    registered = set(re.findall(r"tests/([\w.]+\.py)", cmake))
    failures = []
    for path in sorted((ROOT / "tests").glob("*.py")):
        if path.name.endswith(SUFFIXES) and path.name not in registered:
            failures.append(
                "{} is not run by any add_test in CMakeLists.txt; register "
                "it (oracle probes with --check)".format(path.name))
    acceptance = (ROOT / "ACCEPTANCE.md").read_text(encoding="utf-8")
    for path in sorted((ROOT / "docs" / "acceptance").glob("*.md")):
        if path.name not in acceptance:
            failures.append(
                "docs/acceptance/{} is not linked from ACCEPTANCE.md"
                .format(path.name))
    if failures:
        raise SystemExit("\n".join(failures))
    print("every oracle/comparison test is registered and every acceptance "
          "page is linked")


if __name__ == "__main__":
    sys.exit(main())
