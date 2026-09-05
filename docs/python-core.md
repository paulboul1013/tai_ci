# Python Browser 核心行為分析

來源 `/home/paulboul/tai_gar/browser.py`，唯讀分析；已以 AST 擷取原始 class definitions 實際執行 HTML/CSS edge cases，沒有重新實作 oracle。

## 實際 dependency graph

`URL → socket/ssl/gzip/urllib.parse + http_cache/socket_cache/COOKIE_JAR + cookie/referrer helpers`。
`HTMLParser → Element/Text + html.unescape`。
`CSSParser → Tag/Class/Id/Sequence/Has/Visited/DescendantSelector`。
`style → DOM parent/children + CSSParser.body + selector.matches + defaults`。
`Tab stylesheet 收集 → style_tag_text / URL requests → CSSParser.parse → sorted(cascade_priority) → style → layout`。
DOM 也供 JSContext handles、layout、hit-test 使用；is_checked 初始化由 checked attribute 存在性決定；is_visited 由 Tab 設定。

## DOM 與 HTMLParser（4285–4308、9072–9346）

Text 有 text、children（空）、parent、is_focused；Element 有 tag、attributes、children、parent、is_focused、is_checked。style 與 style_priority 是稍後動態新增。Python 強引用 parent/children 成環；C 必須明確 child-owned、parent borrowed，document 統一銷毀；JS handles 不能持有無追蹤懸空 pointer。

Parser 使用 unfinished stack；文字與 void elements 即時掛接 children，一般 element 在關閉/pop 時掛接。空 input 產生 html/body。保留空白文字節點；文字以完整 html.unescape 解碼，包括 script 文字；attributes 不解碼 HTML entities。tag/attribute keys casefold，value 保留大小寫。引號內 `>` 不結束 tag。comment 跳過，未閉合 comment 忽略到 EOF；未閉合 tag 在 EOF 丟棄。script 特殊處理到大小寫不敏感但必須精確 `</script>`；style 不是 raw-text。

Implicit html/head/body 規則是精簡 stack 特例。HEAD_TAGS 是 base/basefont/bgsound/noscript/link/meta/title/style/script；在 head 直接加入文字會觸發切出 head，但 script 內文字因 stack 深度不同保留。void list 為 area/base/br/col/embed/hr/img/input/link/meta/param/source/track/wbr。不支援一般 XML self-close：`<br/>after` 會變成 tag `br/` 包住 after。

p/li auto-close 僅呼叫一般 end-tag handler；一般 end-tag 不搜尋名稱，只 pop 一層。故 `<p>a<div>b</p>c` 的 c 仍在 p 內。b/i/u/small/big 有 formatting_stack 與簡化 adoption，`<b><i>x</b>y</i>` 產生 b(i(x)) + i(y)，重開 attribute dict 在 Python 共用。

Attribute parser 以非引號空白切 token，因此 `x = "hi"` 不會得到 x=hi，而得到 x、空字串 key、`"hi"` 三個異常 boolean attrs。重複 attributes 最後值覆蓋。這些不是標準 HTML 行為，migration 不可直接改成完整標準 parser 而不記錄差異。

## CSS 與 selectors（4309–4423、9347–9663）

支持 tag（priority 1）、class（10）、id（100）、相連 compound（加總）、空白 descendant（加總）、`:visited`（10，僅 a 且 is_visited）、`:has(inner)`（10 + inner priority，任意深度 descendants，排除自己）。class 用 whitespace split，class/id case-sensitive，tag casefold。Has 的 descendant selector 可沿祖先越出候選節點邊界，應納入 oracle case。

不支持 `*`、`,`、`>`、`+`、attribute selector、其他 pseudo。parse 失敗跳到下一個 `}`，因此 CSS comments 並未真正支援：`/* comment */ a {color:blue} b {color:red}` 只保留 b。browser.css 現有 comments 會吞掉其後第一條規則，不能依肉眼認定 stylesheet 的所有 rules 都生效。

Declaration key casefold；value 保留原字大小寫，空白 tokens 合併單一空格。value_token 保留平衡括號內空白及標點，供 blur/rgb/calc 等後端解析；沒有 quote string 狀態。`!important` case-insensitive，可出現在任意 token 間。property 重複最後宣告覆蓋，即使前次 important：`color:red!important;color:blue` 結果 blue,false。錯誤 declaration 跳到分號或右括號。font shorthand 只辨 italic/bold/normal、px/% 與 size 後 family；family casefold，normal 同時 reset weight/style；不是完整 CSS font shorthand。

## Computed style（9664–9782）

Inherited defaults：font-size=16px、font-style=normal、font-weight=normal、color=black、font-family=Times、text-align=left。Non-inherited defaults：width/height=auto、display=inline、border-radius=0px、overflow=visible、opacity=1.0、mix-blend-mode=normal、filter=none。任意未知 property 也可加入 style。

每次 style 呼叫清空兩個 dict，先由已計算 parent 繼承六項，再套規則，再 inline，最後遞迴 children（Text 也取得 defaults/inherited）。priority 相同後者勝出；important 加 10000，inline priority 1000；繼承 priority reset 0。僅 inherited properties 的 inherit keyword 被解析。font-size 不以 px/% 結尾時退回 parent/default；百分比乘 parent px 轉成 Python float 字串（24.0px），無效數字可拋 ValueError。px 數值此層不驗證。

DEFAULT_STYLE_SHEET 在 module import 時直接 open("browser.css")，依 cwd 而非 source dirname；完整 import 依賴 dukpy/sdl2/skia。AST oracle 可以避開 GUI import，但仍執行原始 class/function bodies，必須記錄 source hash 和擷取白名單。

## URL（8545–9071）

支持 http/https/file/data/about/mailto，加 view-source prefix、fragment。scheme parsing case-sensitive；錯誤 print 後 fallback about:blank，部分 host/port/fragment 舊值未清空。HTTP url_string 固定包含 port（cache key），str 隱藏 default port。host 不 lower-case，無 IPv6 支援，query-only URL 會被視為 host 的一部分。file 沒有強制 absolute path；data request 僅 split comma + unquote，沒有 base64 解碼。about 回空 body；mailto 回固定說明 HTML；file UTF-8 text read，失敗回 error HTML。

resolve：None→None，空白→同一 URL object，#fragment、//host、支援的 explicit scheme、host-relative、path-relative；僅前綴 ../ 收斂父目錄，不 normalize ./ 或路徑中段 ..；about/data 做一般 relative resolution 可 ValueError（path 無 slash）。不支援 scheme 回 None；explicit scheme 判斷 casefold 但 URL constructor 不 casefold，會 fallback。

request 使用同步 IPv4 socket，TLS default context 驗證；GET/POST 由 payload None 決定；POST body UTF-8、Content-Length、Connection:close；GET keep-alive header，但全文沒有 socket_cache[key] 寫入，因此正常從未真正填充 socket cache。User-Agent Tai_Gar/1.0、Accept-Encoding gzip、Host 不含 port。支援 content-length、精簡 chunked（不支援 extensions/trailers）與 read-until-close、gzip、UTF-8 replacement。

最多 10 次 redirect，有 Location 時保留 POST payload；以 / 開頭 Location 自組 origin 時漏掉 nondefault port；其餘相對 Location 無 resolve。HTTP cache 僅 GET 且 origin None、200、max-age 整數、無 no-store；cache key 包 port 不含 fragment，body 是解壓 bytes。response header keys casefold、重複最後值覆蓋。

COOKIE_JAR 每 host 僅一筆 cookie，expires 日期支援；無 path/domain/secure/max-age 正規規則。SameSite lax 只限制跨 host 非 GET；HttpOnly 由 JS layer 處理。referrer 支援 no-referrer/same-origin，default HTTP(S) 互送完整 URL 去 fragment（包含 HTTPS→HTTP）。libcurl multi 的自動 redirect/cookie 政策不能直接當 oracle 規則，需 app 層保留或記錄 intentional differences。

## differential 驗證策略

1. AST 擷取原始 DOM/parser/style/URL definitions，以標準库 html.unescape 與 urllib.parse 注入 dependency；固定 source hash。JSON 輸出 DOM tag/attrs/text/children、selector type/tree/priority、declarations(value,important)、computed style。忽略 memory identity；parent 另驗證父索引。
2. Corpus 包括空文、空白、implicit head/body、script/entity、引號 >、comments、malformed close、formatting reopen、attributes spacing、void/self-close；CSS comments/unsupported selectors/recovery/duplicate important/font shorthand/has ancestry/visited/inherit/%。
3. URL JSON 比較所有 fields、str/origin/resolve；fallback stderr/exception 明確區分。file/data 可離線 oracle；network 使用本機 deterministic HTTP server 記錄 method/path/headers/body，回 gzip/chunked/cache/redirect/cookie；禁止以外網狀態做穩定 acceptance。
4. ASan/UBSan 驗證 parse/style/free 重複生命週期與異常 inputs。Python 的 parser crash 不應在 C 重現 memory corruption；以明確 error return 作記錄差異。

## 已執行確認

AST oracle 實際確認空文、一般錯配 close、attribute spacing/entity、br/、script entity、formatting reopen、CSS comment swallowing、duplicate important 覆蓋、universal selector rejection，結果吻合上述描述。未修改 source，也未執行外部網路 request。
