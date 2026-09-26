---
name: oracle-lookup
description: Cheap lookup of Python oracle behavior/class/method in tests/reference/browser.py (9,862 lines) or tests/reference/runtime.js, before reading either file directly. Use before grepping or paging through the oracle. 觸發：查 Python oracle／參考實作／browser.py／runtime.js／oracle 行為查詢。
---

# Oracle Lookup

`tests/reference/browser.py` and `tests/reference/runtime.js` are the frozen
Python/JS oracle. **Never edit `tests/reference/**`.** It is the executable
spec, not something to fix.

## Always run the tool first

```
python3 tests/tools/oracle_symbols.py                       # outline: every class + method, line ranges
python3 tests/tools/oracle_symbols.py Chrome.click           # exact Class.method
python3 tests/tools/oracle_symbols.py bookmarks_page         # bare method name, all matching classes
python3 tests/tools/oracle_symbols.py Tab.bookmarks_page --show --max-lines 40   # print source
python3 tests/tools/oracle_symbols.py --grep bookmarks       # symbols whose source mentions a pattern
python3 tests/tools/oracle_symbols.py --js runRAFHandlers    # same, against runtime.js
```

Then use `Read` with the exact `file:start-end` the tool printed — do not open
the whole file. Only fall back to grepping the raw file if the tool errors or
a symbol genuinely isn't a class/def (e.g. inside a nested closure).

## Existing oracle probes / differential drivers (tests/)

File — ctest name — what it covers:

- `blur_oracle_probe.py` — `blur_oracle_probe` — frozen blur parser incl. non-finite edge.
- `bookmarks_oracle_probe.py` — `bookmarks_oracle_probe` (`--check`) — bookmark behavior vs. local HTTP fixture.
- `tabs_oracle_probe.py` — `tabs_oracle_probe` (`--check`) — native tabs slice probe.
- `browser_differential.py` — `browser_differential` — headless nav DOM/style/layout vs. oracle.
- `css_differential.py` — `css_differential` — CSS/style vs. original AST, no GUI deps.
- `display_differential.py` — `display_differential` — supported native display leaves vs. oracle.
- `dom_differential.py` — `dom_differential` — original AST defs, no GUI/parser substitution.
- `hit_differential.py` — `hit_differential` — native display hit testing vs. oracle.
- `image_differential.py` — `image_differential` — OpenMoji layout/display + fallback.
- `layout_differential.py` — `layout_differential` — real-Skia oracle vs. basic layout tree.
- `page_scroll_differential.py` — `page_scroll_differential` — scroll clamp/viewport hits.
- `resize_layout_differential.py` — `resize_layout_differential` — width-sensitive relayout.
- `url_differential.py` — `url_differential` — URL class AST differential (class unmodified).
- `network_integration.py` — `network_differential` — network behavior integration.
- `bookmarks_integration.py` — `bookmarks_integration` — bookmark tab-set HTTP fixture.
- `navigation_integration.py` — `browser_navigation` — localhost navigation driver.
- `tabset_integration.py` — `browser_tabs` — deterministic tab-set integration driver.

Run: `ctest --test-dir build -R '^<name>$' --output-on-failure`, or directly,
e.g. `python3 tests/tabs_oracle_probe.py --check`.
