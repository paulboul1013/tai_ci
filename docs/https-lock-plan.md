# HTTPS 鎖頭與地址欄寬度：實作計畫

**狀態：已實作，`VALIDATING`（2026-09-26）。** 實作與設計的差別：安全狀態沿用既有
`TaiPage.secure`（以請求 URL 計算、錯誤頁為 false）並以 `tai_page_secure()` 公開，而非在
`TabSlot` 另存 `committed_secure`，觀察行為相同；證據見
[acceptance](acceptance/2026-09-26-https-lock.md)。 本切片是 [PORTING_PLAN.md](../PORTING_PLAN.md)
「續做 Chrome 與 History」的第一項。完成後的驗證證據寫入 `docs/acceptance/`，刻意差異寫入
`PORTING_PLAN.md`。

## 為什麼先做

Native 完全沒有安全狀態。Python 在 HTTPS 頁面會縮短地址欄並畫鎖頭，而預設首頁
`https://browser.engineering/` 就是 HTTPS，所以一打開視窗，地址欄起點、寬度、文字位置、
欄內收藏星與地址欄命中區就和 Python 不一致。之後 HTTPS 頁面的 chrome 幾何比對都依賴本項。

## Python 依據（`tests/reference/browser.py`）

用 `python3 tests/tools/oracle_symbols.py <符號>` 定位。

| 行為 | 位置 | 內容 |
|---|---|---|
| 設定安全狀態 | `Tab._finish_document_load` 6016–6037 | 憑證錯誤與其他網路錯誤 → `False`；成功 → `url.scheme == "https"`。`url` 是**請求的** URL，不是 redirect 後的 URL。 |
| 開始導覽即清除 | `Tab.load` 6084 | 導覽一開始就設 `secure = False`，內部頁（`about:`）也是。 |
| 傳給 chrome | `CommitData` 5649–5697、`Tab` 6251–6257 | 每次 commit 都帶 `secure`，而 commit 每幀都會發生，所以 pending 期間鎖頭已消失；`secure` 改變會重畫 chrome（7834）。 |
| 讀取 | `BrowserWindow.active_is_secure` 8266 | 只讀 active tab 的已提交狀態；分頁各自獨立。 |
| 地址欄寬度 | `Chrome` HTML 5386–5390 | `address_width = max(100, width − 150 − 30)`（安全時），否則 `max(100, width − 150)`。 |
| 鎖頭位置 | 5542–5570 | 地址欄 layout 的 x 加 30（`SECURITY_ICON_SLOT = 30`，1453）；鎖頭 14×14，中心在 (原 x + 15, 地址欄中線)。寬視窗時地址欄右緣不變：132 + 30 + (w − 180) = w − 18。 |
| 繪製 | `build_lock_path` 1971、`Chrome` icons 5484 | 外框鎖頭：矩形鎖身加上方鎖環，黑色（`ICON_COLOR_ENABLED`），筆寬 1.8，不填滿。 |
| 點擊 | — | 鎖頭沒有點擊處理；[原 x, 原 x + 30) 是空白 chrome，不會聚焦地址欄。 |

## Native 現況

- `TaiTabSetView`（`include/tai/tabset.h`）沒有安全欄位；`src/presentation_chrome.c` 與
  `src/presentation_address.c` 用 `address_x()`、`tabs_address_width()` 算固定的地址欄幾何。
- 網路層已區分憑證錯誤（`TaiResponse.certificate_error`，`src/network.c:657`），失敗頁標題為
  「Certificate Error」／「Network Error」（`src/browser.c:438`）。
- 已記錄的刻意差異：之後的載入失敗會回滾到舊頁面（Python 顯示錯誤頁）；pending 期間舊頁面仍
  顯示；Tabbed 地址欄寬度夾限為不超過視窗寬度，讓收藏星保持可見。

## 設計

### 1. 狀態：每個分頁的已提交安全旗標

- 在 `TabSlot` 記錄 `committed_secure`，只在 SDL thread 的 `tai_tabset_pump()` commit 時設定：
  文件成功載入 → 請求 URL 的 scheme 是否為 `https`；首次失敗提交的錯誤頁（含憑證錯誤）→ `false`。
  Back/Forward 的 commit 相同。回滾（之後的失敗）不改變舊值，因為舊頁面仍在。
- 請求 URL 取自 load task 的 `task->url`（與 Python 使用請求 URL 一致，不用 redirect 後的 URL）。
  `about:bookmarks`、`about:blank`、`data:` 都是 `false`。
- `TaiTabSetView` 新增 `bool secure`＝`committed_secure`。pending 期間 native 仍顯示舊頁面，
  鎖頭也跟著舊頁面保留，直到新頁面 commit 才更新（使用者決定，見「決定」）。
- 不需要新鎖或跨執行緒存取：旗標只在 SDL thread 讀寫。

### 2. 幾何：集中到 `presentation_internal.h`

- 新增 `tabs_address_field(width, secure)` 回傳 `{x, width}`：
  `x = address_x(width) + (secure ? 30 : 0)`；
  `width = min(max(100, width − 150 − (secure ? 30 : 0)), 視窗寬 − x)`。
  前者與 Python 相同，後者沿用既有的「不超出視窗」夾限，確保窄視窗仍看得到收藏星。
- chrome 繪製、文字裁切（field − 28）、游標定位（`tai_pres_editor_cursor_from_x`）、地址欄命中、
  欄內收藏星的位置與命中區，全部改用這個函式，不再各自呼叫 `address_x()`／`tabs_address_width()`。
- 單頁（legacy）chrome 不變：Python 對照的是 tabbed chrome，單頁入口沒有安全狀態。

### 3. 繪製

- `presentation_chrome.c` 新增 `draw_lock()`，比例照 `build_lock_path`：黑色外框、線寬 1.8、
  14×14，中心 (`address_x(width)` + 15, 地址欄中線)。只在 `view->secure` 時畫。
- 鎖頭區域 [`address_x`, `address_x` + 30) 不是任何命中區：點擊時只丟棄地址草稿（和其他空白
  chrome 一樣），不聚焦地址欄。

## 需先用 oracle 固定的問題

先擴充 probe，把 Python 實際行為凍結進 fixture，再寫 native：

1. 寬視窗（800px）與窄視窗（120px）、安全與不安全時的地址欄矩形與鎖頭矩形。
2. HTTPS 頁面 pending 期間、成功、憑證錯誤、一般網路錯誤後的 `secure`。
3. `http→https` 與 `https→http` redirect 後的 `secure`（預期跟隨請求 URL）。
4. 兩個分頁一個 HTTPS、一個 HTTP 時，切換後鎖頭跟隨 active tab。

## 測試計畫

- **本機 HTTPS fixture：** 在測試中用 `openssl req -x509` 產生暫時的自簽 CA 與 `127.0.0.1` 憑證，
  以 `ssl.SSLContext.wrap_socket` 包裝 fixture server。Python 端設 `SSL_CERT_FILE` 讓
  `ssl.create_default_context()` 信任它；另跑一次不設定，得到真正的憑證錯誤。
- **Native 信任測試 CA：** libcurl 不讀 `SSL_CERT_FILE`，需要一個只給測試用的入口。
  在 `TaiNetwork` 加上 `tai_network_set_ca_file()`，由 `TaiTabSet` 的測試建構函式傳入；
  不使用全域環境變數，避免正式執行時被環境改變信任根。`tai-browser` 本身不呼叫它。
- **Oracle：** 擴充 `tests/tabs_oracle_probe.py` 並重新凍結 `tests/fixtures/tabs_oracle.json`，
  或新增 `tests/https_oracle_probe.py`（建議新增，避免改動既有的 tabs fixture）。
- **Native 單元／整合：** `test_tabset.c`（或新的 `test_tabset_secure.c` 搭配 Python 驅動）驗證
  `view.secure` 在成功、pending、首次失敗、憑證錯誤、回滾、Back/Forward、切換分頁時的變化，
  且兩個分頁互不影響。
- **Dummy SDL：** `test_presentation.c` 驗證安全頁面時的地址欄命中起點、鎖頭區不聚焦、欄內收藏星
  位置，以及 800／232／231／120／70px 各斷點。
- **真實視窗：** `native-window-verification` skill（`tests/tools/window_session.sh`）開 HTTPS
  fixture 截圖，確認鎖頭可見、地址文字右移，切到 HTTP 分頁後鎖頭消失。
- **Sanitizer：** ASan/UBSan＋LSan 跑受影響的測試。

## 實作順序與完成條件

1. **Oracle probe 與 HTTPS fixture。** 完成條件：probe 可重跑，fixture 凍結上述 4 類問題的答案。
2. **Native 信任測試 CA。** 完成條件：native 可成功載入自簽的 `https://127.0.0.1` fixture；
   不設定時得到 `certificate_error`；正式路徑行為不變。
3. **`TaiTabSetView.secure`。** 完成條件：整合測試涵蓋所有狀態轉換與跨分頁獨立，並與 oracle 一致。
4. **幾何與繪製。** 完成條件：dummy SDL 在各斷點的命中與收藏星位置通過；地址欄矩形與 oracle 一致。
5. **真實視窗與紀錄。** 完成條件：截圖證據、完整 CTest、sanitizer；更新 `docs/reference-presentation.md`、
   `docs/architecture/native-runtime.md`、`PORTING_PLAN.md`，並新增 `docs/acceptance/` 紀錄。

## 預期的刻意差異

- **之後的載入失敗：** Native 回滾到舊的 HTTPS 頁面時鎖頭會恢復；Python 顯示錯誤頁、沒有鎖頭。
  這是既有回滾策略的延伸，記入 `PORTING_PLAN.md`。
- **Pending 期間保留鎖頭：** Python 開始導覽就移除鎖頭；native 在新頁面 commit 前保留舊頁面的
  鎖頭，讓鎖頭與畫面上的頁面一致。這是既有「pending 時顯示舊頁面」差異的延伸。
- **窄視窗：** 安全時 Python 地址欄起點移到 x = 30、寬 100，右端超出 <130px 的視窗；native 沿用
  「不超出視窗」夾限。

## 決定（2026-09-26，使用者確認）

1. 新增只給測試用的 `tai_network_set_ca_file()`；不讀環境變數。
2. Pending 期間保留舊頁面的鎖頭，到新頁面 commit 才更新（與 Python 不同，列為刻意差異）。
