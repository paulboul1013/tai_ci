#!/usr/bin/env python3
"""Cheap symbol lookup for the frozen Python oracle (tests/reference/browser.py
and tests/reference/runtime.js) so agents don't need to grep/page through a
9,800+ line file just to find a class or method.

Usage:
  oracle_symbols.py                       # compact outline of all classes/methods
  oracle_symbols.py SYMBOL [SYMBOL ...]   # look up Class, Class.method, or bare method
  oracle_symbols.py SYMBOL --show         # also print source of each match
  oracle_symbols.py SYMBOL --show --max-lines N   # cap printed source (default 120)
  oracle_symbols.py --grep REGEX          # symbols whose source matches REGEX
  oracle_symbols.py --js ...              # operate on runtime.js instead of browser.py

Reference files are frozen: this tool only reads them, never edits them.
"""
from __future__ import annotations

import argparse
import ast
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
PY_REF = REPO_ROOT / "tests" / "reference" / "browser.py"
JS_REF = REPO_ROOT / "tests" / "reference" / "runtime.js"


# ---------------------------------------------------------------------------
# Python (browser.py) via ast
# ---------------------------------------------------------------------------

class PySymbol:
    __slots__ = ("qualname", "kind", "start", "end", "cls")

    def __init__(self, qualname, kind, start, end, cls=None):
        self.qualname = qualname
        self.kind = kind  # "class" or "method"/"function"
        self.start = start
        self.end = end
        self.cls = cls


def _end_line(node) -> int:
    end = getattr(node, "end_lineno", None)
    if end is not None:
        return end
    # Fallback: max lineno of any descendant.
    best = node.lineno
    for child in ast.walk(node):
        ln = getattr(child, "lineno", None)
        if ln is not None and ln > best:
            best = ln
    return best


def parse_python(path: Path):
    src = path.read_text()
    tree = ast.parse(src, filename=str(path))
    symbols: list[PySymbol] = []
    for node in tree.body:
        if isinstance(node, ast.ClassDef):
            symbols.append(PySymbol(node.name, "class", node.lineno, _end_line(node)))
            for item in node.body:
                if isinstance(item, (ast.FunctionDef, ast.AsyncFunctionDef)):
                    symbols.append(
                        PySymbol(f"{node.name}.{item.name}", "method",
                                 item.lineno, _end_line(item), cls=node.name)
                    )
        elif isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            symbols.append(PySymbol(node.name, "function", node.lineno, _end_line(node)))
    return src, symbols


# ---------------------------------------------------------------------------
# JS (runtime.js) via simple regex (top-level function declarations only)
# ---------------------------------------------------------------------------

JS_FUNC_RE = re.compile(
    r"^(?:function\s+(\w+)\s*\(|(?:const|let|var)\s+(\w+)\s*=\s*(?:async\s*)?"
    r"(?:function)?\s*\(?[^=]*=>)",
    re.MULTILINE,
)


def parse_js(path: Path):
    src = path.read_text()
    lines = src.splitlines()
    n = len(lines)
    symbols: list[PySymbol] = []
    # Find "function name(" declarations and locate their matching closing brace
    # by brace counting from the opening "{" after the signature.
    decl_re = re.compile(r"^function\s+(\w+)\s*\(", re.MULTILINE)
    for m in decl_re.finditer(src):
        name = m.group(1)
        start_line = src.count("\n", 0, m.start()) + 1
        brace_open = src.find("{", m.end())
        if brace_open == -1:
            continue
        depth = 0
        end_line = start_line
        for i, ch in enumerate(src[brace_open:], start=brace_open):
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    end_line = src.count("\n", 0, i) + 1
                    break
        symbols.append(PySymbol(name, "function", start_line, end_line))
    symbols.sort(key=lambda s: s.start)
    return src, symbols


# ---------------------------------------------------------------------------
# Outline (default, no args)
# ---------------------------------------------------------------------------

def print_outline(path: Path, src: str, symbols, is_js: bool):
    total_lines = src.count("\n") + 1
    rel = path.relative_to(REPO_ROOT)
    print(f"# {rel} ({total_lines} lines) -- oracle symbol outline")
    if is_js:
        # Flat list of top-level functions, packed several per line.
        packed = []
        line_buf = []
        cur_len = 0
        for s in symbols:
            token = f"{s.qualname}:{s.start}-{s.end}"
            if cur_len + len(token) + 2 > 100 and line_buf:
                packed.append("  " + "  ".join(line_buf))
                line_buf = []
                cur_len = 0
            line_buf.append(token)
            cur_len += len(token) + 2
        if line_buf:
            packed.append("  " + "  ".join(line_buf))
        for row in packed:
            print(row)
        return

    classes = [s for s in symbols if s.kind == "class"]
    funcs = [s for s in symbols if s.kind == "function"]
    methods_by_cls: dict[str, list[PySymbol]] = {}
    for s in symbols:
        if s.kind == "method":
            methods_by_cls.setdefault(s.cls, []).append(s)

    if funcs:
        print("# top-level functions:")
        line_buf, cur_len = [], 0
        for s in funcs:
            token = f"{s.qualname}:{s.start}-{s.end}"
            if cur_len + len(token) + 2 > 220 and line_buf:
                print("  " + "  ".join(line_buf))
                line_buf, cur_len = [], 0
            line_buf.append(token)
            cur_len += len(token) + 2
        if line_buf:
            print("  " + "  ".join(line_buf))

    for c in classes:
        print(f"class {c.qualname}:{c.start}-{c.end}")
        methods = methods_by_cls.get(c.qualname, [])
        line_buf, cur_len = [], 0
        for m in methods:
            token = f"{m.qualname.split('.', 1)[1]}:{m.start}-{m.end}"
            if cur_len + len(token) + 2 > 220 and line_buf:
                print("  " + "  ".join(line_buf))
                line_buf, cur_len = [], 0
            line_buf.append(token)
            cur_len += len(token) + 2
        if line_buf:
            print("  " + "  ".join(line_buf))


# ---------------------------------------------------------------------------
# Symbol lookup
# ---------------------------------------------------------------------------

def find_matches(symbols, query: str):
    """Match Class, Class.method, or bare method name (all matches)."""
    matches = []
    if "." in query:
        for s in symbols:
            if s.qualname == query:
                matches.append(s)
    else:
        # exact class match, exact bare-function match, or bare method name match
        for s in symbols:
            if s.kind in ("class", "function") and s.qualname == query:
                matches.append(s)
        for s in symbols:
            if s.kind == "method" and s.qualname.rsplit(".", 1)[-1] == query:
                matches.append(s)
    return matches


def print_source(path: Path, src_lines, start: int, end: int, max_lines: int):
    rel = path.relative_to(REPO_ROOT)
    capped_end = min(end, start + max_lines - 1)
    print(f"--- {rel}:{start}-{end} (showing {start}-{capped_end}) ---")
    for i in range(start, capped_end + 1):
        if 1 <= i <= len(src_lines):
            print(f"{i:>6}  {src_lines[i - 1]}")
    if capped_end < end:
        print(f"  ... ({end - capped_end} more lines, use --max-lines to see more)")


def grep_symbols(symbols, src_lines, pattern: str):
    rx = re.compile(pattern)
    hits = []
    for s in symbols:
        matched_lines = []
        for i in range(s.start, s.end + 1):
            if 1 <= i <= len(src_lines) and rx.search(src_lines[i - 1]):
                matched_lines.append(i)
        if matched_lines:
            hits.append((s, matched_lines))
    return hits


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("symbols", nargs="*", help="Class, Class.method, or bare method name")
    ap.add_argument("--show", action="store_true", help="print source of matches")
    ap.add_argument("--max-lines", type=int, default=120, help="cap printed source lines (default 120)")
    ap.add_argument("--grep", metavar="REGEX", help="list symbols whose source matches REGEX")
    ap.add_argument("--js", action="store_true", help="operate on tests/reference/runtime.js instead of browser.py")
    args = ap.parse_args(argv)

    path = JS_REF if args.js else PY_REF
    if not path.exists():
        print(f"error: reference file not found: {path}", file=sys.stderr)
        return 1

    if args.js:
        src, symbols = parse_js(path)
    else:
        src, symbols = parse_python(path)
    src_lines = src.splitlines()

    if args.grep:
        hits = grep_symbols(symbols, src_lines, args.grep)
        if not hits:
            print(f"no symbols matched --grep {args.grep!r}")
            return 0
        for s, lines in hits:
            rel = path.relative_to(REPO_ROOT)
            lines_str = ",".join(str(l) for l in lines[:10])
            more = f" (+{len(lines) - 10} more)" if len(lines) > 10 else ""
            print(f"{rel}:{s.start}-{s.end}  {s.qualname}  lines[{lines_str}{more}]")
        return 0

    if not args.symbols:
        print_outline(path, src, symbols, args.js)
        return 0

    exit_code = 0
    for query in args.symbols:
        matches = find_matches(symbols, query)
        if not matches:
            print(f"no match for {query!r}", file=sys.stderr)
            exit_code = 1
            continue
        for s in matches:
            rel = path.relative_to(REPO_ROOT)
            print(f"{rel}:{s.start}-{s.end}  {s.qualname}")
            if args.show:
                print_source(path, src_lines, s.start, s.end, args.max_lines)
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
