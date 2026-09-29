# 視窗標題跟隨頁面與品牌改名「Tai Ci」：實作計畫與交接

**狀態：已實作，`VALIDATING`（2026-09-29）。** 決定已由使用者確認（見「決定」）。實作與證據見
[驗收紀錄](acceptance/2026-09-29-window-title.md)。實作時的修正：`network_differential` 其實會比對
User-Agent，已改為把 oracle 的 `Tai_Gar/1.0` 對應成 `Tai_Ci/1.0`；問題 12（JS 改寫標題）因 native
JS 沒有 `innerHTML` 而未在 native 比對。
本工作接在 [新視窗（Ctrl+N）](new-window-plan.md) 之後，處理 `PORTING_PLAN.md`「已知差異與範圍」
中的「視窗標題與位置」的標題部分（位置不在範圍內）。

## 一句話摘要

每個 `tai-browser --window` 視窗的標題改成顯示目前 active 分頁的頁面標題（`<title>`），沒有
標題時顯示瀏覽器名稱「Tai Ci」；同時把 native 瀏覽器所有對外可見的「Tai Gar」改成新名稱。

## 整體流程

```
 步驟 1  Python oracle：凍結 Python 的標題規則（哪個 <title>、空白、錯誤頁、pending、切換分頁）
                          ▼
 步驟 2  頁面層：tai_page_title()，從目前 DOM 取標題 ──── C 單元測試
                          ▼
 步驟 3  呈現層：每個視窗在畫面更新時設定 SDL 視窗標題 ─ dummy SDL 讀 SDL_GetWindowTitle 比對
                          ▼
 步驟 4  改名：預設標題「Tai Ci」、User-Agent「Tai_Ci/1.0」、工具與文件
                          ▼
 步驟 5  真實視窗：window_session.sh 改以 PID 找視窗；Xvfb 讀視窗標題（xdotool getwindowname）
                          ▼
 步驟 6  收尾：完整 CTest、ASan/UBSan＋LSan、獨立審查、更新紀錄
```

## 決定（2026-09-29，使用者確認）

1. **名稱：** 瀏覽器名稱是 **`Tai Ci`**（中間一個空格，沿用 Python「Tai Gar」的格式）。用於沒有
   有效 `<title>` 的頁面、尚未有已提交頁面時，以及視窗剛建立時。Python 的後備標題是
   「Tai Gar」，native 改名是**刻意差異**，比對時把 oracle 的 `"Tai Gar"` 對應成 `"Tai Ci"`。
2. **User-Agent：** `src/network.c` 的 `Tai_Gar/1.0` 改為 **`Tai_Ci/1.0`**（Python 送
   `Tai_Gar/1.0`，`browser.py:8734`），記為刻意差異。目前沒有測試比對 User-Agent。
3. **範圍：** 視窗位置（Python 置中）**不做**，差異紀錄保留。`mailto:` 外部開啟也不做。

## Python 依據（`tests/reference/browser.py`）

用 `python3 tests/tools/oracle_symbols.py <符號>` 定位，只讀需要的行段。

| 行為 | 位置 | 內容 |
|---|---|---|
| 取標題 | `Tab.get_title` 6161–6172 | `nodes` 為空 → `"Tai Gar"`。否則依文件順序走訪所有節點，找 `tag == "title"` 的 Element；標題文字是 `style_tag_text(node).strip()`，**只串接直接子 Text 節點**（2505–2512），並用 Python `str.strip()` 去除前後空白。第一個**非空**的標題勝出，空的 `<title>` 會被跳過。都沒有 → `"Tai Gar"`。 |
| 何時計算 | 6256（commit 資料） | 每次 `run_animation_frame` 提交時重新計算，讀的是當下的 DOM，所以 DOM 改動會反映出來。 |
| 視窗採用哪個 | `BrowserWindow` 8070–8090 | 取 **active 分頁已提交狀態**的 `title`；該分頁還沒有已提交狀態 → `"Tai Gar"`。 |
| 設定視窗 | `present_raster_result` 8173–8183 | 每次呈現都 `SDL_SetWindowTitle(title)`（UTF-8，無法編碼的字元用 replace）。 |
| 標題改變觸發重繪 | 7826–7836 | active 分頁的 `title` 改變時 chrome 需要重繪。 |
| 錯誤頁 | Network Error 6020–6033（**沒有** `<title>` → 後備名稱）；Certificate Error 5762–5790（`<title>Certificate Error</title>`） | |
| 書籤頁 | `bookmarks_page` 5795 | `<title>Bookmarks</title>` |
| JS | `runtime.js` 沒有 `document.title` | 只有 DOM 改動會影響標題。 |
| 建立視窗 | 7510 | `SDL_CreateWindow(b"Tai Gar", ...)`，第一次呈現後才換成頁面標題。 |

## Native 現況

- **沒有任何取標題的 API。** `include/tai/browser.h` 沒有 title 相關函式。
- **視窗標題固定：** `src/presentation_tabs.c:71`（tabbed，所有 `--window` 視窗）與
  `src/presentation.c:17`（舊的單頁入口）都是 `SDL_CreateWindow("Tai Gar", ...)`，之後不再改。
- **錯誤頁 markup：** `src/browser.c` 的 `network_error_markup()`（436 附近）對 Network Error 與
  Certificate Error 用同一份 markup，**都沒有 `<title>`**；Python 的 Certificate Error 有
  `<title>Certificate Error</title>`。本工作至少要讓憑證錯誤頁的標題與 Python 相同（見步驟 1 問題 6）。
- **書籤頁：** `src/tabset.c` 產生的 `about:bookmarks` 已有 `<title>Bookmarks</title>`。
- **HTML parser：** `src/dom.c:265–273` 把 `title` 當成 head 元素處理；`<title>` 內容的解析（entity、
  內含標籤）要用 oracle 確認與 Python 相同，不要假設。
- **User-Agent：** `src/network.c:509` `"Tai_Gar/1.0"`。
- **測試現況：** `tests/new_window_integration.py` 目前**跳過** `title` 欄位（`SKIPPED_KEYS`），
  `tests/fixtures/new_window_oracle.json` 已包含 Python 每個視窗的 `title`。
- **工具：** `tests/tools/window_session.sh` 用 `xdotool search --name '^Tai Gar$'` 找視窗
  （109、129 行）。標題跟隨頁面後這個搜尋會失效，必須改成以 PID 找視窗。

### 「Tai Gar」出現位置分類（`grep -rni 'tai.gar'`，排除 build/deps）

| 類別 | 檔案 | 處理 |
|---|---|---|
| 產品可見 | `src/presentation_tabs.c:71`、`src/presentation.c:17`（視窗標題）、`src/network.c:509`（User-Agent） | **改** |
| 工具與規約 | `tests/tools/window_session.sh`、`AGENTS.md:58`、`.agents/skills/native-window-verification/SKILL.md:17` | **改**（說明文字改為「Tai Ci」或「本次啟動、PID 相符的 browser 視窗」） |
| 現行文件 | `docs/reference-presentation.md`（23、31 行）、`PORTING_PLAN.md`（視窗標題列）、`tests/new_window_integration.py` docstring | **改**，描述新行為 |
| 凍結的 oracle 與歷史 | `tests/reference/**`（凍結，**絕對不可改**）、`tests/oracle.py` 的模組名 `tai_gar_fixed_reference`（內部識別字）、`docs/python-*.md`、`docs/architecture/python-reference.md`、`tests/reference/manifest.json`（指向真實路徑 `/home/paulboul/tai_gar`）、`patches/quickjs/*.patch` 的 From 行、舊的 `docs/acceptance/*`、`docs/new-window-plan.md` | **不改**：它們描述 Python 參考實作或已發生的歷史 |

## 需先用 oracle 固定的問題（步驟 1）

新增 `tests/title_oracle_probe.py`＋`tests/fixtures/title_oracle.json`（可沿用
`tests/new_window_oracle_probe.py` 的 `Probe` 骨架與 `tests/new_window_fixture.py` 的做法，另寫
`tests/title_fixture.py`）。每個檢查點記錄：active 分頁的 `get_title()`、committed state 的
`title`，以及 `SDL_GetWindowTitle(window.sdl_window)`。記得先呈現一次（`raster_and_draw` 會呼叫
`present_raster_result`）再讀 SDL 標題。

1. **基本：** `<title>Page A</title>` → `Page A`。
2. **沒有 `<title>`：** → `Tai Gar`（native 對應為 `Tai Ci`）。
3. **空白與多個：** `<title>  padded  </title>` 的前後空白；`<title></title><title>Second</title>`
   → 第一個空的被跳過；`<title>   </title>` 只有空白。
4. **內容解析：** `<title>A &amp; B</title>`、`<title>&lt;b&gt;</title>`、`<title>x<b>y</b>z</title>`
   （只串接直接子 Text 節點）、非 ASCII（例如中文）、標題中間的換行。
5. **位置：** 放在 `<body>` 裡的 `<title>`；沒有 `<head>` 的文件。
6. **錯誤頁：** 連線失敗（Network Error，Python 沒有 title → 後備名稱）與憑證錯誤（`Certificate Error`，
   可沿用 `tests/https_fixture.py` 與 `SSL_CERT_FILE` 的做法，參考 `tests/https_oracle_probe.py`）。
7. **書籤頁：** `about:bookmarks` → `Bookmarks`。
8. **Pending：** 從 A 導覽到被 gate 擋住的 B：pending 期間的標題（預期仍是 A），放行後變 B。
9. **分頁：** 兩個分頁不同標題，切換 active 分頁後視窗標題跟著變；inactive 分頁載入完成**不**改變
   視窗標題。
10. **多視窗：** 兩個視窗各自顯示自己 active 分頁的標題（可直接用 new-window fixture 的資料驗證）。
11. **剛建立的視窗：** 第一個頁面提交前的 SDL 標題（預期 `Tai Gar`）。
12. **DOM 改動（選做）：** 若 Python runtime 能以 JS 改 `<title>` 的文字節點（例如 `innerHTML`），記錄
    改動後的標題；不支援就在 probe 註明並跳過。

注意：Python 的 commit 要以 `tab_call(... run_animation_frame)` 觸發；每個頁面的 `<h1>` 要唯一，
避免 `wait_for` 提前成立；port 正規化成 `<PORT>`；輸出三次相同後才凍結。

## 設計方向（以 oracle 結果為準）

- **頁面層（`src/browser.c`、`include/tai/browser.h`）：** 新增
  `char *tai_page_title(const TaiPage *page)`，回傳 caller 擁有的字串（配置失敗回 `NULL`），依 Python
  規則從**目前的 DOM** 計算。沒有有效標題時回空字串 `""`，由呼叫端決定後備名稱，讓頁面層不必
  知道品牌名稱。strip 的空白集合要與 Python `str.strip()` 相同（至少涵蓋 ASCII 空白；Unicode 空白依
  oracle 問題 3／4 的結果決定）。
- **品牌名稱：** 在 `include/tai/browser.h`（或 presentation 內部 header）定義一個常數，例如
  `TAI_BROWSER_NAME "Tai Ci"`，視窗建立與後備標題共用，避免字串散落各處。
- **錯誤頁：** 讓 `network_error_markup()` 在憑證錯誤時加上 `<title>Certificate Error</title>`；
  Network Error 維持沒有 title（Python 相同）。若這會改變既有 differential 或 https 整合測試的輸出，
  同步更新那些測試的期望值。
- **呈現層（`src/presentation_tabs.c`）：** `PresWindow` 新增 `char *shown_title`。在 `window_frame()`
  重繪時（或每次 frame 都檢查，但只在字串改變時呼叫 SDL），依 `TaiTabSetView.page` 計算標題：
  `page == NULL` 或標題為空 → `TAI_BROWSER_NAME`；與 `shown_title` 不同才 `SDL_SetWindowTitle()` 並更新
  快取。`window_destroy()` 釋放 `shown_title`。所有呼叫都在 SDL owner thread。
  - 注意：只在「有變化」時重算可能漏掉 JS 改動 DOM 的情況；若 oracle 問題 12 顯示 Python 會反映 DOM
    改動，就在 `page_changed` 時也重算。
- **舊單頁入口（`src/presentation.c`）：** 只把建立時的標題改成 `TAI_BROWSER_NAME`，不做跟隨頁面
  （該入口不是 `--window` 使用的路徑）。
- **`TaiTabSetView`：** 不必新增欄位；由 presentation 從 `view.page` 取標題即可。

## 驗證計畫

| 層級 | 檔案／指令 | 證明什麼 |
|---|---|---|
| Python oracle | `tests/title_oracle_probe.py --check`（加入 CTest） | Python 的標題規則被凍結。 |
| C 單元 | 擴充既有 browser 單元測試，或新增 `tests/test_page_title.c` | `tai_page_title()` 對問題 1–5 的 markup 與 oracle 相同（後備名稱在呈現層）。 |
| Dummy SDL | 新 `tests/test_title_window.c`＋`tests/title_integration.py`，或擴充 `test_new_window.c` | 以 `SDL_GetWindowTitle(SDL_GetWindowFromID(id))` 讀每個視窗標題，比對問題 6–11；另外把 `tests/new_window_integration.py` 的 `SKIPPED_KEYS` 移除 `title`，並在 `window_json()` 輸出 `title`，oracle 的 `"Tai Gar"` 對應為 `"Tai Ci"`。 |
| 回歸 | 完整 `ctest --test-dir build` | 其他行為不變（特別是 https、display differential 若受錯誤頁 markup 影響）。 |
| Sanitizer | `build-asan`，`ASAN_OPTIONS=detect_leaks=1`，LSan suppression `leak:libfontconfig.so`、`leak:libcairo.so` | `shown_title` 與 `tai_page_title()` 的配置沒有洩漏或 UAF。 |
| 真實視窗 | `native-window-verification` skill，Xvfb | `xdotool getwindowname <WID>` 在載入、導覽、切換分頁、Ctrl+N 新視窗、錯誤頁時顯示正確標題；截圖與 request log。 |
| 獨立審查 | `ownership-reviewer` | 新增配置的擁有權與清理路徑。 |

## 實作順序與完成條件

| 步驟 | 內容 | 完成條件 |
|---|---|---|
| 1 | Oracle probe 與 fixture | 三次輸出相同，涵蓋問題 1–11（12 選做）；加入 CTest。 |
| 2 | `tai_page_title()`＋憑證錯誤頁 `<title>` | C 單元測試與 oracle 相同；完整 CTest 通過。 |
| 3 | 視窗標題跟隨 active 分頁 | Dummy SDL 比對通過；`new_window_integration` 改為比對 title 並通過。 |
| 4 | 改名 | 產品可見與工具／規約類的「Tai Gar」全部改完；凍結／歷史類保持不變；`grep -rni 'tai.gar' src include` 為空。 |
| 5 | 真實視窗 | `window_session.sh` 改以 PID 找視窗（見下）；Xvfb 標題證據存入驗收紀錄。 |
| 6 | 收尾 | 完整 CTest、整套 sanitizer、`ownership-reviewer`；更新 `PORTING_PLAN.md`（視窗標題差異列改寫：標題已跟隨，保留「位置」與「後備名稱改名」「User-Agent 改名」兩個刻意差異）、`docs/reference-presentation.md`、`docs/architecture/native-runtime.md`（若 `PresWindow` 擁有權有變）、`ARCHITECTURE.md`（若新增檔案），新增 `docs/acceptance/<日期>-window-title.md` 並在 `ACCEPTANCE.md` 表格加一列。 |

每個步驟各自 commit（使用者要求時才 push）。

### `window_session.sh` 改法（步驟 5）

- 視窗標題會隨頁面改變，`xdotool search --name '^Tai Gar$'` 不能再用。改成
  `xdotool search --pid "$pid"`，再過濾真正的頂層視窗（例如同時要求 `--onlyvisible`，或檢查
  `xprop WM_CLASS`）。先實測 SDL3 是否會替同一 PID 建立額外的 X 視窗，再決定過濾條件。
- 保留「每個動作前確認 `getwindowpid WID == PID`」的檢查。
- 新增 `title` 指令，印出選定視窗目前的標題（`xdotool getwindowname "$WID"`），作為證據。
- 更新腳本開頭說明與 `.agents/skills/native-window-verification/SKILL.md`。

## 預期的刻意差異

- 後備名稱：native `Tai Ci`，Python `Tai Gar`（品牌改名）。
- User-Agent：native `Tai_Ci/1.0`，Python `Tai_Gar/1.0`。
- 視窗位置（不在本工作範圍）：native 交給視窗系統，Python 置中。

## 給接手 session 的交接事項

**開始前先讀：** `AGENTS.md`、本文件、`PORTING_PLAN.md` 的「已知差異與範圍」、
`docs/reference-presentation.md` 的「多視窗（Ctrl+N）」段、`src/presentation_tabs.c`。

**repo 狀態（2026-09-29）：** `main` 在 `728c718`（新視窗已 merge 並 push），工作區乾淨。建議開新
分支，例如 `window-title`。

**常用指令：**

```bash
cmake -S . -B build -G Ninja && cmake --build build -j 4
ctest --test-dir build --output-on-failure -j 3                 # 目前 43 個測試，約 75 秒
cmake -S . -B build-asan -G Ninja -DTAI_SANITIZERS=ON && cmake --build build-asan -j 4
printf 'leak:libfontconfig.so\nleak:libcairo.so\n' > "$SCRATCH/lsan.supp"
ASAN_OPTIONS=detect_leaks=1 LSAN_OPTIONS=suppressions="$SCRATCH/lsan.supp" \
  ctest --test-dir build-asan --output-on-failure -j 3        # 約 2 分鐘
python3 tests/new_window_oracle_probe.py --check
python3 tests/new_window_integration.py build/test_new_window tests/reference/browser.css
```

**上一個工作踩過的坑：**

1. **SDL3 的 `SDL_Quit` 會重設 hint。** 同一個測試程序中多次呈現視窗時，每次呈現前都要重新
   `SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy")`，否則第二次起會用真的 X11 driver（在 WSLg 會跳出
   真實視窗，LSan 還會報 X11 的洩漏）。參考 `tests/test_new_window.c` 的 `run()`。
2. **Dummy 迴圈一次處理一個事件。** 推送事件後，用 `SDL_PeepEvents(NULL, 0, SDL_PEEKEVENT, ...) == 0`
   判斷已處理完（`test_new_window.c` 的 `drained()`）。
3. **SDL3 的 TEXT_INPUT 只存指標**，推送前文字要放在會活到事件處理後的 buffer（例如 static）。
4. **Xvfb 在 WSL 上：** `/tmp/.X11-unix` 是唯讀 tmpfs，Xvfb 只有 abstract socket，而 abstract socket
   綁在 network namespace 上。要隔離網路時，必須讓 Xvfb、fixture server、browser 和 `xdotool` 都在
   同一個 namespace：先 `nohup unshare -rn bash -c 'ip link set lo up; exec sleep 3600' &` 取得 holder
   PID，每個指令都用 `nsenter -t $HOLDER -U -n --preserve-credentials tests/tools/window_session.sh ...`，
   結束後終止 holder。只有在會連外（例如開新視窗的正式首頁）時才需要隔離。
5. **Xvfb 沒有 window manager：** 多個視窗疊在同一位置，點擊會落在最上層的視窗；
   `window_session.sh` 已在點擊與截圖前 `windowraise` 選定視窗。關閉單一視窗用 `close`（送
   `WM_DELETE_WINDOW`），不要用 `xdotool windowclose`。
6. **Python oracle 的 commit** 由 `run_animation_frame` 產生，只呼叫 `render()` 不會更新
   `committed_states`。
7. **WSLg `:0` 會丟掉合成按鍵的焦點**（`windowID=0`）。鍵盤證據用 Xvfb；WSLg 只適合使用者手動檢查。
8. 編輯 `src/`、`include/`、`tests/` 後 hook 會自動 `touch`；可疑時用
   `grep -a -c '<新字串>' build/<target>` 確認產物已更新。
