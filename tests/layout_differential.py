#!/usr/bin/env python3
"""以真 Skia oracle 比較基本 layout tree；不把 display 尚未移植誤認完成。"""
import json, pathlib, subprocess, sys, tempfile, math
ROOT=pathlib.Path(__file__).resolve().parents[1]
CASES=[('<p>Hello world</p>', 800), ('<div>first</div><div>second</div>', 800), ('<p>first<br>second</p>', 800), ('<pre>one\n\n  two\tthree</pre>', 800), ('<div style="width:80px">one two three four five</div>', 800), ('<p style="text-align:center">hello world</p>', 800), ('<p style="text-align:right">hello</p>', 800), ('<div style="height:0px">a</div><p>b</p>', 800), ('<div><i>A</i><b>B</b><p>C</p><i>D</i></div>', 800)]
CASES += [('', 800), ('<p></p>', 800), ('<p>\u2003Hello\u00a0world</p>', 800), ('<p>soft\u00adhyphen</p>', 800), ('<p><b>Bold</b> <i>Italic</i> normal</p>', 800), ('<pre>long line never wraps despite width</pre>', 800), ('<div style="width:0px">zero width fallback</div>', 800), ('<div style="width:20px">overflowingword next</div>', 800), ('<p style="font-size:16.5px">ties even</p>', 800), ('<p style="font-size:17.5px">ties up</p>', 800), ('<p style="font-family:monospace">mono</p>', 800), ('<p>one two three four five six</p>', 80)]
def normalize(node):
    keys=('kind','x','y','width','height','word')
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
with tempfile.TemporaryDirectory() as tmp:
    for i,(html,width) in enumerate(CASES):
        path=pathlib.Path(tmp)/'input.html';path.write_text(html)
        expected=json.loads(subprocess.check_output([sys.executable,str(ROOT/'tests/oracle.py'),'layout',html,'--width',str(width)]))['layout']
        actual=json.loads(subprocess.check_output([sys.argv[1],str(path),str(ROOT/'tests/reference/browser.css'),str(width)]))
        compare(normalize(expected),normalize(actual),f'case {i}')
print(f'{len(CASES)} layout differential cases passed')
