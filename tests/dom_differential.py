"""Execute original AST definitions, without GUI imports or substituted parser logic."""
import ast
import html
import random
import json
import pathlib
import subprocess
import sys
import tempfile

source = pathlib.Path(__file__).parent / 'reference' / 'browser.py'
tree = ast.parse(source.read_text())
names = {'Text', 'Element', 'HTMLParser', 'ViewSourceParser'}
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
# Complete named-reference table and numeric boundary behavior.
cases += [' '.join('&' + key for key in html.entities.html5),
          ' '.join('&#%d;' % n for n in list(range(256)) + [0xd800, 0xdfff, 0xfdd0, 0xfffe, 0x10ffff, 0x110000]),
          '<Straße İD="x" A\u001cb="y">z', '<div>&notit; &amp= &copycat;</div>']
rng = random.Random(7351)
tokens = ['<p>', '</p>', '<b>', '</b>', '<i>', '</i>', '<div>', '</div>', '<head>', '</head>', '<li>', '<ul>', '</ul>', '<br>', 'a', '&amp;', ' ', '<!--x-->']
cases += [''.join(rng.choices(tokens, k=16)) for _ in range(250)]
exe = sys.argv[1]
passed = exceptions = 0
with tempfile.TemporaryDirectory() as directory:
    p = pathlib.Path(directory) / 'case.html'
    for i, value in enumerate(cases):
        p.write_text(value)
        expected_source = ns['ViewSourceParser'](value).handle_view_source()
        actual_source = subprocess.run([exe, '--source', str(p)], check=True, capture_output=True, text=True).stdout
        assert actual_source == expected_source, f'view-source case {i}: {value!r}'
        try:
            expected = normalize(ns['HTMLParser'](value).parse())
        except Exception:
            exceptions += 1
            result = subprocess.run([exe, str(p)], capture_output=True, text=True)
            assert result.returncode == 3, f'Python exception must become native parser error: {value!r}: {result.returncode}'
            continue
        p.write_text(value)
        got = subprocess.run([exe, str(p)], check=True, capture_output=True, text=True)
        actual = json.loads(got.stdout)
        passed += 1
        assert actual == expected, f'case {i}: {value!r}\nexpected {expected}\nactual {actual}'
print(f'DOM differential: {passed} cases passed; {exceptions} Python exceptions mapped to native errors')
