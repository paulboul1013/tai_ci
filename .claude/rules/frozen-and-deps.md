---
paths:
  - "tests/reference/**"
  - "deps/**"
  - "patches/**"
  - "tests/fixtures/*_oracle.json"
---

# 凍結與第三方檔案

- `tests/reference/**` 是凍結的 Python oracle，禁止修改；查詢用 `python3 tests/tools/oracle_symbols.py`。
- `tests/fixtures/*_oracle.json` 只能由對應的 `tests/*_oracle_probe.py` 重新產生，不可手改。
- `deps/` 未納入版控；修正第三方缺陷要在 `patches/<dep>/` 加 patch 與回歸測試（見 `patches/README.md`），不可直接改 `deps/`。
