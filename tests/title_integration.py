#!/usr/bin/env python3
"""Compare native window titles with tests/fixtures/title_oracle.json.

    title_integration.py <test_page_title>

test_page_title prints tai_page_title() for each tests/title_fixture.py
markup page. The oracle's fallback title "Tai Gar" is the empty string at the
page layer; the presentation layer supplies the renamed fallback "Tai Ci".
"""

import json
import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests" / "fixtures" / "title_oracle.json"
PYTHON_NAME = "Tai Gar"

sys.path.insert(0, str(ROOT / "tests"))
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


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    oracle = json.loads(FIXTURE.read_text(encoding="utf-8"))
    failures = check_page_titles(sys.argv[1], oracle)
    if failures:
        raise SystemExit("title differences:\n" + "\n".join(failures))
    print("native page titles match tests/fixtures/title_oracle.json")


if __name__ == "__main__":
    main()
