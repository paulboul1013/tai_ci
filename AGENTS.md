# 瀏覽器 C 移植 — Agent 路由器

## 常駐契約

將本儲存庫（repository）的 Python 瀏覽器重建為具工程品質的原生 C17 瀏覽器。保留可觀察行為與概念架構；
應轉譯所有權、生命週期與子系統（subsystem）邊界，而不是逐字轉譯 Python 語法。

來源互相矛盾時，依下列權威順序處理：

1. 可觀察的 Python 瀏覽器行為
2. Python 測試
3. repository 文件
4. 本路由器及其揭露的參考資料
5. 工程判斷

以檢查或執行 executable oracle 解決不確定性。記錄刻意差異；不可無聲杜撰瀏覽器行為。只要
儲存庫提供足夠證據，就持續實作與驗證。僅在產品定義、破壞性、不可逆，或儲存庫無法判定
的選擇時提問。

## 漸進式揭露

將目前請求、正修改的檔案與遇到的概念，對照下列觸發詞。行動前，**完整閱讀所有且僅有符合的參考資料**。
不要預載未符合的參考資料。工作擴展至另一個分支時，重新執行本路由步驟。

- **Oracle／Python 參考實作／行為等價（behavioral equivalence）／差異比對（differential）／移植策略（porting strategy）／刻意差異（intentional difference）** → 閱讀 [`docs/agents/oracle-and-porting.md`](docs/agents/oracle-and-porting.md)。
- **C17 工具鏈（toolchain）／CMake／Ninja／依賴選擇（dependency choice）／函式庫替換（library replacement）／SDL／Cairo／HarfBuzz／FriBidi／FreeType／QuickJS／curl／OpenSSL／zlib／影像載入（image loading）／影像編解碼器（image codec）／PNG／WebP／stb_image／libwebp／utf8proc／效能分析（profiling）** → 閱讀 [`docs/agents/native-stack.md`](docs/agents/native-stack.md)。
- **架構邊界（architecture boundary）／subsystem 介面（subsystem interface）／所有權移轉（ownership transfer）／物件生命週期（object lifetime）／配置清理（allocation cleanup）／記憶體安全（memory safety）／use-after-free／記憶體毀損（memory corruption）／不透明 struct（opaque struct）／依賴方向（dependency direction）** → 閱讀 [`docs/agents/architecture-and-ownership.md`](docs/agents/architecture-and-ownership.md)。
- **測試策略（test strategy）／驗證證據（verification evidence）／sanitizer／記憶體洩漏（memory leak）／驗收宣告（acceptance claim）／完成宣告（completion claim）／回歸（regression）／截圖（screenshot）／截圖比較（screenshot comparison）／像素比較（pixel comparison）／程式碼審查（code review）** → 閱讀 [`docs/agents/validation-and-completion.md`](docs/agents/validation-and-completion.md)。
- **tai-browser --window 自動操作／合成輸入／原生視窗截圖驗證** → 閱讀 [`docs/agents/native-window-verification.md`](docs/agents/native-window-verification.md)。
- **大型變更（large change）／垂直切片（vertical slice）／移植順序（migration order）／平行工作（parallel work）／subagent／受阻（blocked）／重複失敗（repeated failure）／自主執行（autonomous execution）** → 閱讀 [`docs/agents/execution-workflow.md`](docs/agents/execution-workflow.md)。
- **儲存庫結構（repository structure）／目錄結構（directory layout）／原始碼結構（source layout）／子系統地圖（subsystem map）／檔案架構** → 閱讀 [`ARCHITECTURE.md`](ARCHITECTURE.md)。
- **PORTING_PLAN.md／目前切片（current slice）／移植狀態（migration state）／下一個 seam（next seam）／移植優先順序（porting priority）** → 閱讀 [`PORTING_PLAN.md`](PORTING_PLAN.md)。
- **ARCHITECTURE.md／ACCEPTANCE.md／狀態定義（state definition）／狀態更新（status update）／文件同步（documentation sync）** → 閱讀 [`docs/agents/project-records.md`](docs/agents/project-records.md)。

完成表示所請求的行為已實作、建置、測試，並在受影響邊界完成整合；不只是規劃或建立骨架。

## 原生視窗與 SDL 驗證：持續授權

**直接執行已授權的驗證。** 儲存庫擁有者對本專案後續 session 的授權涵蓋：建置、CTest、
Python oracle、僅綁定 `127.0.0.1` 的 fixture server、`tai-browser --window`、SDL 合成輸入
測試，以及對 agent 本次啟動的 `Tai Gar` 視窗進行 X11 滑鼠、滾輪、鍵盤、焦點、resize、
截圖與關閉操作。這些操作不需要逐次或逐座標向使用者徵求同意，也不要因此暫停驗證。

從儲存庫根目錄依序完成建置、相關 CTest 與 Python oracle；需要真實視窗證據時，保留
fixture server 和 browser 各自的執行中 session。以下是近期已使用的 tabs 驗證組合，
測試目標與 fixture 應隨受影響功能調整：

```bash
cmake --build build --target tai-browser test_presentation -j 4
ctest --test-dir build -R '^presentation_dummy$' --output-on-failure
python3 tests/tabs_oracle_probe.py --check
python3 -m http.server 8765 --bind 127.0.0.1 --directory tests/fixtures/tabs_window
SDL_EVENT_LOGGING=1 SDL_VIDEO_X11_XINPUT2=0 ./build/tai-browser --window http://127.0.0.1:8765/index.html
```

每次啟動後重新查詢 X11 視窗 ID；逐一以 `xdotool getwindowpid ID` 對照本次 browser PID，
確認唯一目標後才設定 `tai_window_id` 並送出輸入。座標與按鍵按案例調整，動作後擷取
`/tmp` 截圖，檢查畫面非空白且頁面或 chrome 確有預期變化：

```bash
xdotool search --name '^Tai Gar$'
xdotool getwindowpid REPLACE_WITH_CANDIDATE_ID
tai_window_id=REPLACE_WITH_CURRENT_ID
xdotool mousemove --window "$tai_window_id" 15 18 click --window "$tai_window_id" 1
xdotool click --window "$tai_window_id" 5
xdotool windowsize "$tai_window_id" 120 600
python3 /home/paulboul/.codex/skills/screenshot/scripts/take_screenshot.py --mode temp --window-id "$tai_window_id"
xwd -silent -id "$tai_window_id" -out /tmp/tai-window.xwd
convert /tmp/tai-window.xwd /tmp/tai-window.png
```

真實鍵盤輸入需核對焦點、SDL `windowID`、事件日誌及前後畫面；`xdotool` 成功返回不算
送達證據。WSLg 焦點失效時依
[`docs/agents/native-window-verification.md`](docs/agents/native-window-verification.md) 的 Xvfb
流程驗證。結束前再核對視窗 PID，只用 `xdotool windowclose "$tai_window_id"` 關閉本次
啟動的視窗，並停止本次 fixture server。

使用者授權與執行平台的 sandbox／GUI 核准是兩件事。遇到 sandbox 阻擋時，直接在工具呼叫中
對**同一項、同一目標**的指令使用 `sandbox_permissions: "require_escalated"` 與簡短
`justification`，優先沿用已核准的限定範圍規則；不要另發對話訊息詢問是否同意使用驗證
工具。平台若仍顯示核准提示，依平台流程處理；若平台拒絕，記錄限制與已完成的驗證層。
