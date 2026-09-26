---
paths:
  - "src/presentation*.{c,h}"
  - "tests/test_{presentation,chrome,tabs_window}.c"
---

# Presentation／Chrome

- 幾何與事件路由的契約在 `docs/reference-presentation.md`；改座標、斷點或命中區時同步更新它與 `tests/test_presentation.c`、`tests/test_chrome.c`。
- SDL 座標先轉成實體像素再命中；chrome 命中順序：書籤按鈕 → 欄內收藏星 → 地址欄。
- 單頁 Chrome 必須符合 Python 的寬度斷點（232／128／94／79px、最小 100px 地址欄）；Tabbed 的差異已記在 `PORTING_PLAN.md`。
- 繪製不得做檔案 I/O；只能在 SDL owner thread 呼叫 `TaiTabSet` API。
- 真實視窗證據用 `native-window-verification` skill。
