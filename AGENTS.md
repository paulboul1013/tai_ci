# Browser C Port — Agent 工程規約

## 任務目標

將此 repository 中現有的 Python Browser 實作，移植成具備工程品質的 C17 實作。

現有 Python implementation 是行為上的參考標準與規格來源。

這是一個：

> 保留既有行為與架構語意的移植工作

而不是：

> 將 Python 程式碼逐行翻譯成 C。

最終 C 實作必須盡可能保留 Python 版本中可觀察到的行為，包括：

* 主要架構邊界
* Rendering semantics
* Event flow
* Networking behavior
* JavaScript integration
* 可測試的 Browser 行為

除非某些差異是經過分析後刻意產生，並且有清楚文件紀錄。

---

## 主要目標

建立一個完整的原生 C17 Browser implementation。

最終成果應能取代原本 Python implementation，同時維持：

* 可理解性
* 可測試性
* 可除錯性
* 可維護性
* 可逐步擴充性

如果 repository 中已有足夠資訊可以繼續工作，不得只停留在：

* scaffolding
* architecture proposal
* pseudocode
* TODO
* partial implementation

Agent 應對整個 migration 流程負責到底。

---

## Source of Truth

資訊優先順序如下：

1. 現有 Python Browser 的實際可觀察行為
2. 現有 Python tests
3. 現有 project documentation
4. 本 `AGENTS.md`
5. 合理的工程判斷

如果 documentation 與實際 implementation 發生衝突：

必須調查差異原因。

不得默默自行創造新的 Browser 行為。

對於無法確定的行為，應優先：

* 閱讀 Python implementation
* 執行 Python implementation
* 建立測試
* 觀察實際輸出

以確認真實行為。

---

## Target Platform

### Language

* C17

### Build

* CMake
* Ninja

### Primary Environment

* Linux
* WSL2

在實際可行的情況下，implementation 應維持一般且可移植的標準 C。

不得引入 C++。

---

## Target Technology Stack

除非有明確且充分的技術理由，否則使用 repository 中已核准的 native stack。

### Window / Input

* SDL3

### 2D Rasterization

* Cairo

### Text Shaping

* HarfBuzz

### Bidirectional Text

* FriBidi

### Font Rasterization

* FreeType

### JavaScript

* QuickJS-NG

### HTTP

* libcurl multi

### TLS

* OpenSSL through libcurl

### Compression

* zlib

### PNG / Image Loading

* 在適當情境使用 stb_image

### WebP

* libwebp

### Unicode Utilities

* utf8proc

### Profiling

* Perfetto-compatible tracing
* 以及／或 Tracy

### Tests

* CTest
* cmocka

不得僅因為另一套 library 比較熟悉，就自行替換既定 dependency。

如果某個 dependency 必須更換，必須記錄：

* 為什麼現有 dependency 無法滿足需求
* 評估過哪些替代方案
* 對整體 architecture 的影響
* 對 migration 的影響

---

## Architecture Policy

必須保留：

> 概念上的 architecture

而不是：

> Python 語法形式。

Python 中隱含的：

* ownership
* object relationship
* lifetime

在 C implementation 中都必須轉換成明確規則。

每一個重要 subsystem 都必須具備：

* 清楚的 public interface
* 明確的 ownership rules
* 明確的 lifetime rules
* error handling
* 可測試的 subsystem boundary

避免大量 global state。

避免 hidden ownership。

避免 subsystem 之間形成 circular dependency。

不得只是機械式地把 Python class 轉換成大型 C struct。

如果使用更單純的 C abstraction 更合理，應優先採用。

當 opaque struct 能改善 encapsulation 時，應優先在 subsystem boundary 使用 opaque struct。

---

## Memory Policy

Memory safety 是第一級需求。

每一次 allocation，都必須能清楚知道：

> 誰擁有這塊記憶體。

每個 owned resource 都必須具有確定性的 destruction path。

特別注意以下資源：

* strings
* DOM nodes
* CSS objects
* layout tree nodes
* display lists
* surfaces
* images
* fonts
* JavaScript values
* network buffers
* callbacks
* event objects

在環境支援時，development 過程應使用 sanitizer。

只要仍存在可重現的：

* memory error
* use-after-free
* systematic memory leak

就不得視為 migration 已完成。

---

## Porting Strategy

不得直接進行沒有分析的 whole-project rewrite。

首先必須完整檢查 Python implementation，並建立實際 dependency graph。

Migration 應優先按照 dependency order 進行。

同時應盡可能使用：

> executable vertical slice

逐步建立可執行功能。

典型順序可能類似：

```text
utilities / core data structures
            ↓
        URL / HTTP
            ↓
       HTML parsing
            ↓
          DOM
            ↓
       CSS parsing
            ↓
     style computation
            ↓
         layout
            ↓
     display list
            ↓
      rasterization
            ↓
     window / input
            ↓
      JavaScript
            ↓
 scheduling / browser orchestration
```

但這只是可能的順序。

不得直接假設此順序一定正確。

必須先從 repository 本身推導真正 architecture。

Migration 過程中，應盡可能維持 C project：

* 可以 build
* 可以執行
* 可以測試

---

## Behavioral Equivalence

Python Browser 應視為：

> executable oracle

也就是 C Browser 行為的執行參考標準。

在實際可行時，建立 differential tests：

```text
相同 input
   │
   ├── Python implementation
   │
   └── C implementation
   │
   ▼
標準化可觀察結果
   │
   ▼
compare
```

可比較項目包括：

* parsed DOM structure
* CSS rules
* computed style
* layout geometry
* display lists
* text measurements
* network results
* JavaScript-visible behavior
* event dispatch
* rendered screenshots
* normalized rendering artifacts

當 pixel-perfect comparison 太脆弱且沒有必要時：

優先使用 structural comparison，而不是單純 pixel comparison。

---

## Autonomous Execution

執行原則：

> 優先採取行動，而不是詢問使用者。

如果問題可以透過以下方式解決：

* 閱讀 repository files
* tracing call graph
* 執行 Python implementation
* 執行 tests
* 查閱 dependency documentation
* compile code
* debugging failure
* 建立 isolated experiment

就應直接自行執行。

不得因為一般工程問題就中斷工作詢問使用者。

對於：

* 可逆
* 局部
* 有測試保護
* 可以從 repository 推導

的工程決策，應自主做出合理決定。

只有以下情況才需要詢問使用者：

* product-defining decision
* destructive decision
* irreversible decision
* repository 中完全無法推導的關鍵需求

如果已經可以開始 implementation：

不得只寫完 plan 就停止。

---

## Subagent Policy

當工作彼此獨立，而且平行化能明顯改善：

* 執行速度
* 分析品質
* 驗證品質

就應使用 subagent。

Root agent 的角色是：

> Architect + Integrator

Root agent 負責：

* global architecture
* subsystem contracts
* shared data structures
* ownership conventions
* dependency direction
* integration
* final acceptance

適合交給 subagent 的工作包括：

* 分析獨立 Python subsystem
* 在 interface 已經確定後實作 subsystem
* 撰寫 differential tests
* 獨立調查 failing test
* review memory ownership
* 執行 sanitizer analysis
* review concurrency assumptions
* 驗證 C implementation 與 Python behavior
* 研究第三方 dependency API 的正確使用方式

除非刻意進行 competing solution exploration：

否則不要讓多個 agent 同時修改同一個 architectural boundary。

進行 parallel implementation 時：

優先使用 isolated worktree。

---

## Independent Verification

對於重要或大型變更：

implementation 與 verification 不應完全依賴同一條 reasoning path。

在適當情況下，應將 verification 交給獨立 subagent。

Verification agent 應檢查：

* behavioral equivalence
* edge cases
* resource ownership
* API contract violations
* concurrency hazards
* regression risk
* missing tests

Verifier 必須可以：

> 拒絕 implementation。

驗證 agent 的目標不是證明 implementation 正確，而是積極尋找 implementation 的問題。

---

## Planning Files

Migration 過程中維護以下文件。

---

### ARCHITECTURE.md

描述從 Python implementation 中實際推導出的 architecture。

至少包含：

* subsystems
* dependency graph
* important state
* data flow
* event flow
* rendering pipeline
* thread model
* ownership model

當 architecture 發生實質變更時：

必須同步更新此文件。

---

### PORTING_PLAN.md

記錄 migration 狀態。

每個 subsystem 至少記錄：

* Python source
* C destination
* dependencies
* migration state
* behavioral tests
* known discrepancies

建議狀態：

```text
NOT_ANALYZED
ANALYZED
INTERFACE_DEFINED
IMPLEMENTING
VALIDATING
COMPLETE
BLOCKED
```

---

### ACCEPTANCE.md

定義客觀的完成條件。

不得因為：

> project 已經可以 compile

就宣告 migration 完成。

---

## Completion Criteria

一個 subsystem 只有在以下條件全部成立後，才能標記為完成：

1. 必要 functionality 已實作
2. 可以正常 build
3. 相關 tests 通過
4. 重要 behavior 與 Python reference 一致
5. resource ownership 已完成 review
6. 所有已知 intentional differences 都有文件
7. 與 upstream / downstream modules 的 integration 已經測試

整個 project 只有在以下條件成立後才算完成：

> C Browser 可以在不依賴 Python runtime 的情況下，完整執行具有代表性的 Browser workflow。

---

## Engineering Quality

優先撰寫：

> 簡單、可閱讀、容易檢查的 C。

避免 unnecessary abstraction。

避免 premature optimization。

任何非簡單的 performance optimization 都必須：

* 保持 correctness
* 在適當情況下透過 measurement 證明有必要

如果根本 architecture 可以正確修正：

不得持續堆疊 compatibility hack。

Compiler warnings 必須認真處理。

新增程式碼在可行情況下應使用：

> strict warning settings

並盡可能保持 clean compile。

---

## Progress Behavior

對大型工作採取以下循環：

```text
1. inspect
2. understand
3. establish interfaces
4. implement
5. compile
6. test
7. compare against Python
8. debug
9. integrate
10. update migration records
```

應自主持續執行這個 cycle。

不得在一般 engineering stage 之間停下來等待使用者。

遇到 failure 時：

先自行進行：

* diagnosis
* reproduction
* debugging
* attempted fix

只有在確實無法解決時才 escalation。

---

## Final Principle

這個專案的目標不是：

> 把 Python syntax 轉換成 C syntax。

真正目標是：

> 將同一個 Browser 重新建構成一致且完整的原生 C 系統，並且讓其 architecture、behavior、memory ownership 與 tests 都變成明確、可驗證的工程結構。
