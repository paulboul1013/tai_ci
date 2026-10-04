# Layout 字型快取：點擊連結延遲修正（2026-10-04）

狀態：VALIDATING（效能修正，無刻意行為差異）。

## 問題

點擊連結到換頁需要 2–4 秒。在 Debug build 用 callgrind 分析 `browser.engineering/http.html` 的 headless 載入：

- `tai_layout_create` 佔總指令數 96%。
- `face()` 對每個 word 都重做一次 `FcConfigSubstitute`＋`FcFontMatch`＋`FT_New_Face`＋`FT_Done_Face`，共 6,032 次。
- `measure_n()` 對每個字元呼叫 `FT_Load_Char`（含 hinting），共 33,539 次。

perf 在 WSL2 核心無法使用，所以改用 callgrind。

## 修正

`src/layout.c` 改為每個 layout 持有字型快取：

- 快取鍵是（正規化 family、bold、italic、size），與 oracle 的 `get_font`／`TYPEFACES` 概念相同。
- 字寬（advance）依 codepoint 記憶：ASCII 用陣列，其他字元用開放定址雜湊表。`FT_Load_Char` 失敗不記憶，每次重試，與舊路徑相同。
- 量測仍使用同一個 `FT_Load_Char(FT_LOAD_DEFAULT)` 結果，數值與修改前逐位元相同。
- 所有權規則記於 `docs/architecture/native-runtime.md`。

## 證明了什麼

- **輸出逐位元組相同。** 以 127.0.0.1 鏡像的 `index`／`http`／`layout`／`text.html` 比較修改前後的 binary：
  - `--headless` JSON：4/4 頁 `cmp` 相同。
  - `--screenshot` PNG：4/4 頁 `cmp` 相同。
  - ASan build 的 headless JSON 也相同。
- **Oracle 比對：**
  - `layout_differential` 新增兩個案例：
    - 同一 word 在 bold／italic／字級／family 交替出現。
    - 11 種字級（使 fonts 陣列增長）、64 個非 ASCII codepoint（觸發 rehash），rehash 後再重用早期 codepoint；私有區未對映字元（.notdef）。
  - 突變測試：快取鍵分別移除 `bold`、`italic`、`size`，以及 rehash 時寫壞已搬移的字寬，4 種突變都讓 differential 失敗。每次突變都刪除 object 檔強制重新編譯，因為 WSL 時鐘倒退會讓 Ninja 沿用舊物件；第一輪突變就因此誤判為通過，已重做。
  - 獨立 oracle-checker 跑 6 個 layout／hit／display／browser／image differential，全部通過。另以 10 個臨時案例、2 種寬度比對：4 類差異在 HEAD 版逐位元組相同，屬既有差異（見下方「缺口」）。
- **Caret：** `test_browser.c` 新增非 ASCII input（`é中ab`）點擊測試，驗證三件事：
  - x=14 時 caret 為 0，之後隨 x 單調不減。
  - 0 到 4 每個位置都出現過，點到右端時為 4。
  - 同一 x 重複點擊結果相同。這只證明結果穩定，不保證第二次點擊用的是暖快取。
- **CTest：** 突變測試後以 `--clean-first` 完整重建，Debug `ctest -j 3` 54/54 通過。
- **Sanitizer：** `build-asan`（`TAI_SANITIZERS=ON`，ASan＋UBSan）以 `--clean-first` 重建，在 `detect_leaks=1` 與 LSan suppression（`libfontconfig.so`、`libcairo.so`）下跑整套 CTest，54/54 通過；headless http／text 頁輸出與基準相同，無 sanitizer 錯誤。
- **獨立審查：** ownership-reviewer 檢查以下項目，無確認缺陷：失敗路徑、face 釋放順序、借用指標、雜湊表、數值、const 記憶化與執行緒移交。

## 效能（Debug build，本地 127.0.0.1）

| 量測 | 修改前 | 修改後 |
| --- | ---: | ---: |
| http.html callgrind 總指令 | 18.73 G | 0.55 G |
| `tai_layout_create` 指令 | 18.04 G | 0.13 G |
| `FcFontMatch`／`FT_New_Face` 次數 | 6,032 | 18 |
| `FT_Load_Char` 次數 | 33,539 | 597 |

headless wall time 取 5 次的中位數：

| 頁面 | 修改前 | 修改後 |
| --- | ---: | ---: |
| index | 0.16 s | 0.03 s |
| http | 2.45 s | 0.08 s |
| layout | 1.90 s | 0.08 s |
| text | 2.20 s | 0.09 s |

其他量測：

- **真網路 `https://browser.engineering/http.html`：** 從 3.49 s 降到 1.10 s。user time 從 2.18 s 降到 0.14 s，剩餘時間主要是網路（DNS 約 1 s）。
- **Xvfb 真實視窗：** 由 `tests/tools/window_session.sh` 操作，以 monotonic clock 量測從點擊「Downloading Web Pages」到視窗標題改變的時間：
  - 修改前 4 次：3035／2569／2879／4147 ms。
  - 修改後 4 次：111／111／111／112 ms。
  - 換頁後截圖確認畫面為 http.html；`requests` 有 `GET /http.html`；`browser exit=0`。
  - 驗證層級：X server 到 SDL 的事件傳遞，以及真實視窗像素。使用 Debug build，沒有 LSan。修改後的數字用最終版 binary 重測一次，結果相同（110–111 ms）。

## 缺口

- 快取只在單一 layout 內有效。resize 或 JS 觸發的重新 layout，以及換頁，仍會重建快取。這部分可另開切片，把快取提升到 page／tab 層級。
- 沒有失敗注入測試：`font_open` 中途失敗、`FT_Load_Char` 失敗。
- 雜湊表配置失敗時只是不記憶該字元，這條路徑沒有測試。
- 沒有 Perfetto trace。oracle 的 `MeasureTime` 尚未移植，已記入 `PORTING_PLAN.md`（Profiling / trace 列）。
- oracle-checker 找到 4 類既有 oracle 差異，與本修正無關，HEAD 版輸出相同。已記入 `PORTING_PLAN.md`（Fonts / layout 列）。
