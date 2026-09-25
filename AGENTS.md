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

儲存庫擁有者已授權 agent 在本專案後續工作中直接執行建置、CTest、Python oracle、
本機 fixture server、`tai-browser --window`、SDL 合成輸入測試，以及只針對本次啟動的
`Tai Gar` 視窗進行 X11 滑鼠、滾輪、鍵盤、resize、截圖和關閉操作。這項授權持續有效，
執行這些驗證時不必再次向使用者詢問。每次啟動後重新查詢視窗 ID，並確認只命中本次
測試視窗；截圖存放 `/tmp`，關閉時只關閉 agent 自己啟動的視窗。

從儲存庫根目錄執行以下指令；有相依性的步驟依序執行，server 與視窗各保留一個執行中
session：

```bash
cmake --build build --target tai-browser test_presentation -j 4
ctest --test-dir build -R '^presentation_dummy$' --output-on-failure
python3 tests/tabs_oracle_probe.py --check
python3 -m http.server 8765 --bind 127.0.0.1 --directory tests/fixtures/tabs_window
SDL_VIDEO_X11_XINPUT2=0 ./build/tai-browser --window http://127.0.0.1:8765/index.html
```

在另一個 shell 查詢本次視窗 ID，只有一個匹配時才設定 `tai_window_id`；多個匹配時先用
`xdotool getwindowpid ID` 對照剛啟動的 browser process：

```bash
xdotool search --name '^Tai Gar$'
tai_window_id=REPLACE_WITH_CURRENT_ID
xdotool mousemove --window "$tai_window_id" 15 18 click --window "$tai_window_id" 1
xdotool click --window "$tai_window_id" 5
xdotool key --window "$tai_window_id" Page_Down
xdotool windowsize "$tai_window_id" 120 600
python3 /home/paulboul/.codex/skills/screenshot/scripts/take_screenshot.py --mode temp --window-id "$tai_window_id"
xwd -silent -id "$tai_window_id" -out /tmp/tai-window.xwd
convert /tmp/tai-window.xwd /tmp/tai-window.png
```

滑鼠座標與按鍵依測試案例調整；每次操作後檢查非空白截圖及可觀察的頁面／chrome 變化。
關閉前確認 ID 仍屬本次視窗，再使用 `xdotool windowclose "$tai_window_id"`。真實鍵盤
事件必須另核對 SDL `windowID`、焦點與前後畫面；注入指令成功不等於事件已送達。
完整判讀與失敗處理見 [`docs/agents/native-window-verification.md`](docs/agents/native-window-verification.md)。

此段是使用者對測試操作的授權；執行平台的 sandbox／GUI 權限由平台獨立管理。若工具要求
`require_escalated`，直接透過工具送出所需的限定範圍執行申請，並沿用已核准的規則。
