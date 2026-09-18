#!/usr/bin/env python3
"""Lock the frozen reference's blur parser, including its non-finite edge."""
import contextlib
import math
import os
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import oracle

previous = pathlib.Path.cwd()
os.chdir(oracle.REFERENCE)
try:
    with contextlib.redirect_stdout(sys.stderr):
        browser = oracle.load_reference()
    cases = {
        "blur(2px)": 2.0,
        " BLUR( 2PX ) ": 2.0,
        "blur()": 0.0,
        "blur(+0)": 0.0,
        "blur(-0.0)": 0.0,
        "blur(-2px)": 0.0,
        "blur(nanpx)": 0.0,
        "blur(2)": 0.0,
        "blur(2px) extra": 0.0,
    }
    for value, expected in cases.items():
        assert browser.parse_blur_filter(value) == expected, value
    assert math.isinf(browser.parse_blur_filter("blur(infpx)"))
finally:
    os.chdir(previous)

print("Blur oracle parser probe: 10 cases passed")
