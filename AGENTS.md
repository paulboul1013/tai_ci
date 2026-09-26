# 瀏覽器 C 移植 — Agent 契約

<!-- 維護者注意：Claude Code 經 CLAUDE.md 的 @AGENTS.md 匯入本檔；Codex 直接讀取。
     本檔每個 session 都會載入，只放「每次都需要」的事實。程序與主題知識放 .agents/skills/。 -->

## 常駐契約

將本儲存庫的 Python 瀏覽器（凍結於 `tests/reference/browser.py`）重建為具工程品質的原生 C17
瀏覽器。保留可觀察行為與概念架構；轉譯所有權、生命週期與 subsystem 邊界，而非逐字轉譯 Python 語法。

來源互相矛盾時的權威順序：(1) 可觀察的 Python 行為 (2) Python 測試 (3) repository 文件
(4) 本檔與 skills (5) 工程判斷。以執行 oracle 解決不確定性；刻意差異記入 `PORTING_PLAN.md`，
不可無聲杜撰瀏覽器行為。證據足夠就持續實作與驗證；僅在產品定義、破壞性、不可逆或 repo
無法判定的選擇時提問。完成＝已實作、建置、測試並在受影響邊界整合，不只是規劃或骨架。

## Skills（按需載入）

主題規約位於 `.agents/skills/<name>/SKILL.md`（Codex 直接掃描；Claude Code 經 `.claude/skills/`
symlink 讀取），由描述自動觸發。不支援 skills 的工具請依主題直接讀該檔：

| Skill | 何時使用 |
|---|---|
| `oracle-and-porting` | 行為不確定、Python／C 差異比對、移植順序、刻意差異 |
| `oracle-lookup` | 查 `browser.py`／`runtime.js` 的類別或方法、執行 oracle probe |
| `native-stack` | 依賴選擇或替換、`deps/`、`patches/`、建置旗標 |
| `architecture-and-ownership` | subsystem 介面、所有權移轉、生命週期、記憶體安全 |
| `validation-and-completion` | 測試策略、sanitizer、截圖比較、宣告完成前 |
| `native-window-verification` | `tai-browser --window` 真實視窗操作與截圖證據 |
| `execution-workflow` | 大型變更、垂直切片、平行工作與 subagent、重複失敗 |
| `project-records` | 更新 `ARCHITECTURE.md`／`PORTING_PLAN.md`／`ACCEPTANCE.md` |

其他入口：repo／subsystem 地圖 → `ARCHITECTURE.md`；目前切片與刻意差異 → `PORTING_PLAN.md`；
驗收條件與各切片證據索引 → `ACCEPTANCE.md`。

## 建置與測試

```bash
cmake -S . -B build -G Ninja && cmake --build build -j 4          # Debug；-DTAI_SANITIZERS=ON 用 build-asan/
ctest --test-dir build -R '^presentation_dummy$' --output-on-failure
ctest --test-dir build --output-on-failure -j 3                    # 全部（約 1 分鐘）
```

- **過期物件陷阱：** WSL2 時鐘會倒退，原始碼 mtime 可能早於既有 `.o`，Ninja 會沉默沿用舊物件。
  編輯後建置前先 `touch` 改過的檔案（Claude Code 已有 hook 自動處理），可疑時以
  `grep -a -c '<新字串>' build/<target>` 確認產物。
- CTest 名稱不等於執行檔名稱（`test_presentation`→`presentation_dummy`、`test_browser`→
  `browser_headless`、`test_chrome`→`presentation_chrome`）；對照見 `CMakeLists.txt`。
- `*_differential`／`*_integration`／`*_oracle_probe` 是 Python 驅動，會跑凍結的 oracle。
- 專案目標以 `-Wall -Wextra -Wpedantic -Werror` 建置；警告即缺陷。
- LeakSanitizer 可執行。SDL 視窗測試的 Cairo toy font／fontconfig 全域快取洩漏是既有第三方問題，
  以 `leak:libfontconfig.so`、`leak:libcairo.so` suppression 排除；回報時區分有無執行 LSan。
- 綁定 `127.0.0.1` 的測試在 sandbox 內可能需要 loopback 權限。

## 驗證授權（持續有效）

儲存庫擁有者授權後續 session 直接執行：建置、CTest、Python oracle、僅綁定 `127.0.0.1` 的
fixture server、`tai-browser --window`、SDL 合成輸入，以及對 agent 本次啟動、且已核對 PID 的
`Tai Gar` 視窗進行滑鼠、滾輪、鍵盤、焦點、resize、截圖與關閉。不需逐次徵求同意。程序與證據標準見
`native-window-verification` skill（`tests/tools/window_session.sh`）。

**僅 Codex：** 使用者授權不等於平台 sandbox 核准。遇 sandbox 阻擋時，對同一指令使用
`sandbox_permissions: "require_escalated"` 與簡短 `justification`，不要另發訊息詢問；平台拒絕則
記錄限制與已完成的驗證層。
