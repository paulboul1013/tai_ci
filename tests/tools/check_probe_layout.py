#!/usr/bin/env python3
"""Fail when a test walks the oracle's tab.document in a function that does
not call oracle.wait_layout first.

A page shows its heading once the HTML is parsed, but tab.document stays
None until _finish_page_resources requests a render. Probes that clicked
after the heading alone failed under load (overflow_scroll, middle_click).
The walk may sit in a nested helper (locate); the method or top-level
function around it must make the call.

    python3 tests/tools/check_probe_layout.py [ROOT]
"""

import ast
import pathlib
import sys

ROOT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2])
WALK = "tree_to_list(tab.document"
WAIT = "oracle.wait_layout("
FUNCTIONS = (ast.FunctionDef, ast.AsyncFunctionDef)


def outer_functions(tree):
    """Top-level functions and class methods: the units that must wait."""
    for node in tree.body:
        if isinstance(node, FUNCTIONS):
            yield node
        elif isinstance(node, ast.ClassDef):
            yield from (item for item in node.body if isinstance(item, FUNCTIONS))


def main():
    failures = []
    for path in sorted((ROOT / "tests").glob("*.py")):
        text = path.read_text(encoding="utf-8", errors="replace")
        if WALK not in text:
            continue
        lines = text.splitlines()
        units = [(node.lineno, node.end_lineno) for node in outer_functions(ast.parse(text))]
        for number, line in enumerate(lines, 1):
            if WALK not in line:
                continue
            unit = next(((start, end) for start, end in units if start <= number <= end), None)
            if unit and WAIT in "\n".join(lines[unit[0] - 1:unit[1]]):
                continue
            failures.append("{}:{}: walks tab.document; call {}run_on_tab, tab) "
                            "first in this function (tests/oracle.py)".format(
                                path.relative_to(ROOT), number, WAIT))
    for failure in failures:
        print(failure)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
