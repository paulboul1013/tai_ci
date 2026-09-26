# 25-tab native chrome — 2026-09-25

使用者在修正前的 800×600 真實視窗展示第 17 個分頁已貼住右緣：
[`pre-compact-17.png`](/tmp/tai-tabs-acceptance/pre-compact-17.png)。因舊 UI 只向右堆放文字，
後續 tabs 雖存在卻超出視窗。依使用者要求，native 在自然 tab row 超過視窗且至少三個
tabs 時，改繪製等寬編號方框；所有方框共用同一條 y=6..30 命中列。TabSet 限制為
最多 25 個 tabs；第 26 次 New Tab 不建新 session/載入，停用按鈕點擊保留地址草稿與焦點。
這些是刻意與無上限 Python chrome 不同的產品行為，記錄於 `PORTING_PLAN.md`。

修正後以 `cmake --build build --target tai-browser test_presentation test_tabset -j 4`
建置，`ctest --test-dir build -R '^(presentation_dummy|presentation_chrome|browser_tabs|tabs_oracle_probe)$' --output-on-failure -j 2`
通過 4/4。新增的 TabSet 測試先觀察到第 26 次建立錯誤，再驗證總數與 active index
不變；dummy SDL 測試覆蓋第 25 個、額外的停用按鈕點擊、首／中／末格命中，及停用
按鈕不清掉編輯中的地址。兩項測試均先在修正前失敗，修正後通過。
`cmake --build build-asan --target test_presentation test_tabset -j 4` 與
`ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-asan -R '^(presentation_dummy|browser_tabs)$' --output-on-failure -j 2`
通過 2/2；本次仍未驗證 LeakSanitizer。

新 binary 的另一個 X11 視窗（本次 ID `12582965`，不沿用）在 800×600 顯示
0–24 共 25 個格子，第 25 次額外 New Tab 點擊沒有新增格子：
[`compact-25-active24.png`](/tmp/tai-tabs-acceptance/compact-25-active24.png)。
用 `xdotool mousemove --window <ID> 49 18 click --window <ID> 1` 選到最左格後，
本機頁面與地址回來：[`compact-25-active0.png`](/tmp/tai-tabs-acceptance/compact-25-active0.png)；
點 `(784,18)` 回到 Tab 24，確認右端格也可由 OS 輸入命中。這個新視窗保留供手動檢查，
再用 `xdotool windowsize <本次 ID> 1200 600` 放寬後 25 個標籤切回一般文字列
[`compact-25-wide1200.png`](/tmp/tai-tabs-acceptance/compact-25-wide1200.png)；還原
800×600 後全部重回編號格
[`compact-25-restored800.png`](/tmp/tai-tabs-acceptance/compact-25-restored800.png)，
Tab 24 仍作用中。測試視窗已以最新 binary 重開並保留供手動檢查；X11 ID 可在重開時重用，
須每次重新搜尋。極窄視窗中每格可能小到無法顯示編號；完整
Python/native pixel diff 與 OS 鍵盤輸入順序仍是獨立缺口，tabs slice 維持 `VALIDATING`。
