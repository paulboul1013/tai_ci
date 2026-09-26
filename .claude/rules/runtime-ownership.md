---
paths:
  - "src/{tabset,session,browser,bookmarks,network,scheduler}.c"
  - "include/tai/{tabset,session,browser,bookmarks,network,scheduler}.h"
---

# Runtime／執行緒與所有權

- 所有權與執行緒模型見 `docs/architecture/native-runtime.md`；改變邊界時同步更新。
- `TaiTabSet` 的公開 API 只在 SDL owner thread 呼叫；loader thread 獨佔 `TaiNetwork` 與建構中的 page，完成時以 tab ID＋generation 交回。
- loader 不得讀可變的共用狀態（例如書籤集合）；在 SDL thread 先複製快照。
- 每個配置要有單一擁有者；成功、部分建構、取消與錯誤路徑都要清理。新增 OOM 路徑時，考慮 `test_tabset_loader_oom`／`test_bookmarks` 的 malloc 注入模式。
- 書籤檔路徑與原子寫入規則見 `include/tai/bookmarks.h`；測試一律用暫存 `XDG_DATA_HOME`，不可碰使用者真實資料。
