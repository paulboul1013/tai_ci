#!/usr/bin/env python3
"""以真 Skia oracle 比較基本 layout tree；不把 display 尚未移植誤認完成。"""
import json, pathlib, subprocess, sys, tempfile, math
ROOT=pathlib.Path(__file__).resolve().parents[1]
CASES=[('<p>Hello world</p>', 800), ('<div>first</div><div>second</div>', 800), ('<p>first<br>second</p>', 800), ('<pre>one\n\n  two\tthree</pre>', 800), ('<div style="width:80px">one two three four five</div>', 800), ('<p style="text-align:center">hello world</p>', 800), ('<p style="text-align:right">hello</p>', 800), ('<div style="height:0px">a</div><p>b</p>', 800), ('<div><i>A</i><b>B</b><p>C</p><i>D</i></div>', 800)]
CASES += [('', 800), ('<p></p>', 800), ('<p>\u2003Hello\u00a0world</p>', 800), ('<p>soft\u00adhyphen</p>', 800), ('<p><b>Bold</b> <i>Italic</i> normal</p>', 800), ('<pre>long line never wraps despite width</pre>', 800), ('<div style="width:0px">zero width fallback</div>', 800), ('<div style="width:20px">overflowingword next</div>', 800), ('<p style="font-size:16.5px">ties even</p>', 800), ('<p style="font-size:17.5px">ties up</p>', 800), ('<p style="font-family:monospace">mono</p>', 800), ('<p>one two three four five six</p>', 80)]
# Font cache keys: the same words alternate family, weight, slant and size,
# and non-ASCII codepoints use the hashed advance table, so a key collision
# or stale advance shows up as a width mismatch against the oracle.
CASES += [('<p>office AV <b>office AV</b> <i>office AV</i> <b><i>office AV</i></b> office AV</p>'
           '<p style="font-size:24px">office <b>office</b></p><p style="font-size:12px">office <i>office</i></p>'
           '<p style="font-family:sans-serif">office caf\u00e9 na\u00efve \u4e2d\u6587 <b>caf\u00e9</b></p>'
           '<pre>office caf\u00e9</pre><p>office caf\u00e9 \u4e2d\u6587</p>', 800)]
# More than eight keys grow the font list; more than 32 distinct non-ASCII
# codepoints rehash one font's advance table, and the last words reuse early
# codepoints after the rehash; U+E000.. are unmapped (.notdef) codepoints.
CASES += [(''.join(f'<p style="font-size:{n}px">size {n} <b>bold</b></p>' for n in range(10, 21))
           + '<p>' + ' '.join(chr(c) * 2 for c in range(0xc0, 0x100))
           + ' ' + chr(0xc0) * 3 + chr(0xc5) * 2 + ' ' + chr(0xd0) + chr(0xe7) + '</p>'
           + '<p>a\ue000b \ue001\ue002</p>', 800)]
# 每條 oracle 規則各有案例；tests/layout_rule_coverage.py 確認沒有規則漏掉。
# sup：字型減半、頂端對齊一般文字；內層 </sup> 結束外層；pre 內也減半。sub/small 只是位移。
CASES += [('<p>a<sup>sup</sup> b<sub>sub</sub> c<small>small</small> d</p>', 800),
          ('<p>a<sup>b<sup>c</sup>d</sup>e</p>', 800), ('<p><sup>only sup</sup></p>', 800),
          ('<pre>x<sup>y</sup>z</pre>', 800), ('<p style="font-size:3px">a<sup>b</sup></p>', 800),
          ('<p>a<sup>s <button>b</button> <input value="i"></sup> t</p>', 800)]
# button：內容排成 block（含換行、巢狀 block、寬高、置中、overflow 不捲動、display:block）。
CASES += [('<p>x <button>go</button> y</p><p>after</p>', 800), ('<p>a<button></button>b</p>', 800),
          ('<button> </button><p>w</p>', 800), ('<button><b>Bold</b> <i>it</i> go</button>', 800),
          ('<button>one two three four five six seven eight nine ten eleven twelve</button>', 800),
          ('<button style="width:100px">a b c d e f g h i j k l m n o</button>', 800),
          ('<button style="height:50px">tall</button><p>n</p>', 800),
          ('<button style="overflow:scroll;height:10px">a b</button>', 800),
          ('<button><div>block</div><div>two</div></button>', 800),
          ('<button style="text-align:center;font-size:24px">c</button>', 800),
          ('<button style="font-family:monospace;font-weight:bold;font-style:italic">mono</button>', 800),
          ('<button style="display:block">blk</button><p>n</p>', 800),
          ('<button style="display:block"><div>x</div></button>', 800),
          ('<button style="background-color:red;opacity:0.5;filter:blur(2px);mix-blend-mode:multiply">fx</button>', 800),
          ('<button>out <button>in</button></button>', 800),
          ('<form><input value="hi"><textarea>t</textarea><button>s</button></form><p>after</p>', 300)]
# input：文字、密碼、checkbox、hidden、寬度、字型與 display:block。
CASES += [('<p><input value="hi"> <input type="password" value="abc"> <input type="checkbox"> '
           '<input type="hidden" value="h"> <input type="HIDDEN"> <input style="width:50px" value="w"></p>', 800),
          ('<p><input style="font-size:24px;font-weight:bold;font-style:italic;font-family:monospace" value="f"></p>', 800),
          ('<input style="display:block" value="b"><p>n</p>', 800)]
# abbr：小寫字母變粗體大寫、0.8 倍；其他字元照常；整個字一起換行；sup 與 pre 的交互。
CASES += [('<p>x <abbr>Html ok</abbr> y</p>', 800), ('<p><abbr>café A1b­c ­</abbr></p>', 800),
          ('<div style="width:60px">aa <abbr>wrapword</abbr> bb</div>', 800),
          ('<p><sup><abbr>ab</abbr></sup> n</p>', 800), ('<p><i><abbr>it</abbr></i></p>', 800),
          ('<pre><abbr>pre abbr</abbr></pre>', 800), ('<p><abbr>a<b>b</b></abbr>c</p>', 800),
          # Python upper() 結果超過一個碼位的所有小寫字元。
          ('<p><abbr>' + ''.join(chr(c) for c in range(0x110000) if not 0xd800 <= c <= 0xdfff
                                 and chr(c).islower() and len(chr(c).upper()) > 1) + '</abbr></p>', 800)]
# h6：後面（跳過空白文字）是 <p> 時合併成一個 block；後面是 inline 時進入 buffer。
CASES += [('<h6>Head</h6><p>para <b>text</b></p>', 800), ('<div>x</div><h6>Head</h6>\n  <p>after</p><p>n</p>', 800),
          ('<div>x</div><h6>Head</h6><div>block</div>', 800), ('<div>x</div><h6>Head</h6>tail <b>t</b>', 800),
          ('<div>x</div><h6>Last</h6>', 800), ('<div>x</div><h6>Head</h6>   ', 800),
          ('<p>lead</p><h6>H</h6><h6>I</h6><p>a</p>', 800)]
# nav／li：toc 在 block 模式留 22px 標題；links 與 li 只影響 paint（見 display_differential）。
CASES += [('<nav id="toc"><p>a</p><p>b</p></nav><p>after</p>', 800), ('<nav id="toc">inline only</nav>', 800),
          ('<div><nav id="toc"><div>x</div></nav></div>', 800), ('<nav id="tocx"><p>a</p></nav>', 800),
          ('<nav class="links"><p>a</p></nav>', 800), ('<ul><li>one</li><li>two <b>b</b></li></ul>', 800),
          ('<li style="width:50px">x y z</li>', 800)]
# script／style／head 不排版。
CASES += [('<head><title>t</title><style>p{}</style></head><p>x<script>var a</script><style>b{}</style>y</p>', 800)]
# 沒有安裝的字型：native 用 fontconfig 替代字型，Skia oracle 用它的預設字型（刻意差異，
# 見 PORTING_PLAN.md「缺字型的替代字型」）。期望值是 oracle 以 fontconfig 選出的字型排版。
FALLBACK_CASES = [('<p><span style="font-family:Georgia">Georgia</span> plain</p>', 'Georgia'),
                  ('<p><span style="font-family:NoSuchFamilyXyz">missing</span> plain</p>', 'NoSuchFamilyXyz')]
def normalize(node):
    keys=('kind','x','y','width','height')
    # oracle 只有 TextLayout 有 word；native 的控制項與 emoji 也帶 word。
    if node['kind']=='TextLayout': keys+=('word',)
    return {**{k:node[k] for k in keys if k in node},'children':[normalize(n) for n in node['children']]}
def compare(a,b,path='root'):
    if isinstance(a,dict):
        assert a.keys()==b.keys(),(path,a.keys(),b.keys())
        for k in a: compare(a[k],b[k],path+'/'+k)
    elif isinstance(a,list):
        assert len(a)==len(b),(path,len(a),len(b))
        for i,(x,y) in enumerate(zip(a,b)): compare(x,y,path+'/'+str(i))
    elif isinstance(a,(float,int)):
        assert math.isclose(a,b,abs_tol=0.0001),(path,a,b)
    else: assert a==b,(path,a,b)
def fontconfig_family(name):
    return subprocess.check_output(['fc-match','-f','%{family[0]}',name],text=True)
def run(native,tmp,html,width,oracle_html=None):
    path=pathlib.Path(tmp)/'input.html';path.write_text(html)
    expected=json.loads(subprocess.check_output([sys.executable,str(ROOT/'tests/oracle.py'),'layout',oracle_html or html,'--width',str(width)]))['layout']
    actual=json.loads(subprocess.check_output([native,str(path),str(ROOT/'tests/reference/browser.css'),str(width)]))
    return normalize(expected),normalize(actual)
def main():
    with tempfile.TemporaryDirectory() as tmp:
        for i,(html,width) in enumerate(CASES):
            compare(*run(sys.argv[1],tmp,html,width),f'case {i} {html!r}')
        for i,(html,family) in enumerate(FALLBACK_CASES):
            oracle_html=html.replace(f'font-family:{family}',f'font-family:{fontconfig_family(family)}')
            compare(*run(sys.argv[1],tmp,html,800,oracle_html),f'fallback case {i}')
    print(f'{len(CASES)} layout differential and {len(FALLBACK_CASES)} font fallback cases passed')
if __name__=='__main__': main()
