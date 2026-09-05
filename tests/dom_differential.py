"""Execute original AST definitions, without GUI imports or substituted parser logic."""
import ast
import html
import json
import pathlib
import subprocess
import sys
import tempfile

source = pathlib.Path(__file__).parent / 'reference' / 'browser.py'
tree = ast.parse(source.read_text())
names = {'Text', 'Element', 'HTMLParser'}
ns = {'unescape': html.unescape}
exec(compile(ast.Module(body=[n for n in tree.body if isinstance(n, ast.ClassDef) and n.name in names], type_ignores=[]), str(source), 'exec'), ns)
def normalize(node):
    if isinstance(node, ns['Text']):
        return {'text': node.text}
    return {'tag': node.tag, 'attributes': node.attributes, 'children': [normalize(c) for c in node.children]}

cases = [
    '', ' ', 'hello', '<!doctype html><title>T</title><p>Hi',
    '<div title="a>b">x</div>', '<p>a<div>b</p>c',
    '<div x = "hi" title="a&gt;b">X&amp;Y</div>', '<br/>after',
    '<script>a &lt; b && x > 2</SCRIPT><p>next',
    '<b><i>x</b>y</i>', '<p>one<p>two', '<ul><li>A<li>B</ul>',
    '<div>one<!-- hidden -->two</div>', 'before<!-- unclosed',
    'a <unclosed', '<input disabled checked=false ID="X">',
    '&#0; &#128; &#x1f600; &NotEqualTilde; &copy x &unknown;',
    '<DIV CLASS="A" class="B">Straße\u00a0text</DIV>',
    '<\u212aELVIN a\u2003b="c">text</\u212aELVIN>',
    '<head> \n<style>a {color:red}</style></head><body>x',
    '<script>unterminated &amp;', '<p><b>x</p>y</b>',
]
exe = sys.argv[1]
with tempfile.TemporaryDirectory() as directory:
    p = pathlib.Path(directory) / 'case.html'
    for i, value in enumerate(cases):
        try:
            expected = normalize(ns['HTMLParser'](value).parse())
        except Exception:
            # Native reports explicit parser failure instead of Python exception.
            continue
        p.write_text(value)
        got = subprocess.run([exe, str(p)], check=True, capture_output=True, text=True)
        actual = json.loads(got.stdout)
        assert actual == expected, f'case {i}: {value!r}\nexpected {expected}\nactual {actual}'
print(f'DOM differential: {len(cases)} cases passed')
