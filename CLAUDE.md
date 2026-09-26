# CLAUDE.md

本檔案為 Claude Code（claude.ai/code）在本儲存庫中處理程式碼時提供指引。

## 儲存庫概述

將一個 Python 教學用瀏覽器（「Tai Gar」，凍結於 `tests/reference/browser.py` + `runtime.js`）
移植為原生 C17 瀏覽器。Python 瀏覽器是**可執行規格（executable specification）**：來源互相矛盾時，
權威順序為 (1) 可觀察的 Python 行為、(2) Python 測試、(3) repository 文件、
(4) `AGENTS.md` 及其揭露的參考資料、(5) 工程判斷。移植的是概念、所有權與 subsystem 邊界，
而不是 Python 語法。

**`AGENTS.md` 是具約束力的規約與關鍵字路由器。** 行動前，將任務對照其觸發詞清單，完整閱讀
所有符合的 `docs/agents/*.md`（以及 `ARCHITECTURE.md`／`PORTING_PLAN.md`／`ACCEPTANCE.md`）；
不要預載未符合的文件。`ARCHITECTURE.md` 另有第二層路由，指向 `docs/architecture/` 與
`docs/reference-*.md`。`CONTEXT.md` 定義領域詞彙（Browser Window、Tab、Active Tab、
Nonblocking Navigation）。

## 建置與測試

有兩個預先設定好的 Ninja 建置目錄（Debug）；`deps/`（QuickJS-NG、SDL3，以及含 utf8proc／cmocka
的本機 sysroot）是未納入版本控制的本機 checkout。

```bash
cmake -S . -B build -G Ninja                            # 一般建置
cmake -S . -B build-asan -G Ninja -DTAI_SANITIZERS=ON   # ASan/UBSan
cmake --build build -j 4                                # 或 --target tai-browser test_foo
ctest --test-dir build --output-on-failure              # 完整測試（較慢，需數分鐘）
ctest --test-dir build -R '^presentation_dummy$' --output-on-failure   # 單一測試
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-asan -R '^browser_tabs$' --output-on-failure
```

- CTest 名稱與執行檔名稱不同（例如 `test_presentation` → `presentation_dummy`、
  `test_browser` → `browser_headless`、`test_chrome` → `presentation_chrome`）；對應關係請查
  `CMakeLists.txt`，並在執行 ctest 前先建置正確的 target。
- `*_differential`／`*_integration`／`*_oracle_probe` 測試是 Python 驅動程式，會執行凍結的
  Python oracle，並與 C probe 執行檔或 `tai-browser` 比對。Oracle probe 也可單獨執行：
  `python3 tests/tabs_oracle_probe.py --check`（與 `tests/fixtures/` 中凍結的 JSON 比對）。
- `tai_core`、`tai_presentation` 與 `tai-browser` 以 `-Wall -Wextra -Wpedantic -Werror` 建置。
- Sanitizer 執行通常設定 `detect_leaks=0`；ASan/UBSan 通過**不**等於記憶體洩漏證據——回報時
  須明確說明。
- 部分 network／tab 測試會綁定 `127.0.0.1`，可能需要在 sandbox 外允許 loopback。

CLI：`./build/tai-browser [--headless|--window] [--rtl] [--screenshot OUT.png] URL`
（預設為 headless JSON 輸出；`--window` 開啟標題為 `Tai Gar` 的 SDL3 分頁視窗）。
真實視窗驗證（fixture server、`xdotool` 輸入、視窗截圖、WSLg 下的 Xvfb 備援流程）已預先授權——
依 `AGENTS.md` 與 `docs/agents/native-window-verification.md` 的流程執行；送出輸入或關閉視窗前
先核對視窗 PID。

## 架構

公開契約位於 `include/tai/*.h`（opaque handle、所有權註解、以回傳值表示錯誤的 API）；
`src/` 中的實作與其同名。跨檔案的整體圖像：

- `tai_core` 靜態函式庫：`core`（字串／map／JSON）→ `url` → `network`（libcurl multi、
  cache／cookies）／`dom`（HTML 解析；`html_entities.inc` 為產生檔）→ `css`（cascade／computed
  style）→ `js`（QuickJS-NG DOM bridge／事件）→ `layout`（FreeType／fontconfig 度量）→
  `render`（自足的 display list + Cairo PNG）；`browser.c`（`TaiPage`）負責在上述所有 subsystem
  之間協調一次頁面載入。`session` 擁有每個分頁的 URL 歷史與 commit；`tabset` 擁有有序分頁、
  loader thread，以及經 generation 檢查的非同步載入完成（載入結果屬於發起它的分頁，即使 active
  tab 已經切換）。`bookmarks` 是最新的切片。
- `tai_presentation`（SDL3）：視窗、texture、chrome／分頁工具列；`presentation_geometry.{c,h}`
  （header 位於 `src/`，屬內部介面）負責 chrome／分頁幾何與 hit-testing。它與 headless PNG 輸出
  使用同一份不可變的 display list。
- `scheduler`（優先權任務、frame deadline）已存在，但尚未接入 browser／window。
- `main.c` 是 CLI 進入點；預設 CSS 來自 `assets/browser.css`。

每個配置都需要單一擁有者與一條確定的銷毀路徑；變更邊界時，須說明 borrow 與 transfer、執行緒
假設，以及錯誤／取消時的清理（見 `docs/agents/architecture-and-ownership.md`）。

## 依賴

只使用核准的技術堆疊（SDL3、Cairo、HarfBuzz、FriBidi、FreeType、QuickJS-NG、libcurl／OpenSSL、
zlib、stb_image、libwebp、utf8proc、cmocka）。不可直接修改 `deps/`：依賴的缺陷須以
`patches/<dep>/` 下的受追蹤 patch 加上回歸測試修正（見 `patches/README.md`）；CMake 會在
configure 時套用 `patches/quickjs/*.patch`。

## 專案紀錄

- `PORTING_PLAN.md`：各 subsystem 的移植狀態（`NOT_ANALYZED` … `COMPLETE`、`BLOCKED`）、
  證據，以及每一項與 Python 的刻意差異（原因、影響、後果）。
- `ACCEPTANCE.md`：整體專案完成條件與附日期的驗證快照；只有在每個子句都已證明時才勾選項目。
- 僅在證據存在後才更新這些紀錄；每項事實應只有一個權威紀錄
  （見 `docs/agents/project-records.md`）。

「完成」表示已實作、建置、測試、在可觀察行為上與 oracle 比對，並在受影響邊界完成整合——而非
僅有計畫或骨架。驗證層級：focused test → integration → Python oracle 比對 → 原生工作流程 →
sanitizer（見 `docs/agents/validation-and-completion.md`）。
