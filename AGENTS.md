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
