# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

所有回答都用繁體中文，用asd ste 100風格

## 決策

在還沒實作任何任務時，要說一步步清楚說明每個步驟，並且都要圖片視覺化，並且也說明你的決策是甚麼，改善了甚麼，優缺點是甚麼，一定要和我討論出來，才可以開始實作

## Claude Code 專屬

- Skills 經 `.claude/skills/` symlink 指向 `.agents/skills/`；編輯請改 `.agents/skills/` 下的檔案。
- 路徑規則在 `.claude/rules/`（讀到對應檔案時自動載入）。
- Hook（`.claude/settings.json`）：編輯 `src/`、`include/`、`tests/` 後自動 `touch` 並以
  `tests/tools/clear_stale_objects.sh` 刪除依賴該檔的物件檔，避免過期物件。
- 專案 subagents（`.claude/agents/`）：`ownership-reviewer`（C 所有權／執行緒審查）與
  `oracle-checker`（Python／C 行為比對）。大型變更完成前用前者做獨立審查。
- 查 `browser.py` 先跑 `python3 tests/tools/oracle_symbols.py <Symbol>`，只讀需要的行段。
- clangd：`build/compile_commands.json` 已由 CMake 產生，根目錄有 symlink。
- 啟動任何 `tai-browser --window`（手動測試、`window_session.sh`、整合測試）時，一律設定
  `BROWSER_TRACE_FILE=<該次工作目錄>/trace.json`。結束後讀 trace，向使用者說明發生了哪些事件；
  trace 不足以解釋行為時，提出改進 trace 記錄程式碼的建議。
