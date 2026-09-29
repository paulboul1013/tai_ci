#!/usr/bin/env python3
"""Runs tests/test_tabset_xhr.c against the tests/js_page_fixture.py server.

    tabset_xhr_integration.py TEST_TABSET_XHR BROWSER_CSS
"""
import pathlib
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import js_page_fixture  # noqa: E402


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    test, css = sys.argv[1:]
    server = js_page_fixture.JsPageServer()
    try:
        subprocess.run([test, css, server.url("")], check=True, timeout=120)
    finally:
        server.close()


if __name__ == "__main__":
    main()
