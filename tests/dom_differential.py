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
# JSContext's serializer methods and VOID_ELEMENTS, run as-is on a bare class.
js_context = next(n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == 'JSContext')
serializer = ast.ClassDef(name='Serializer', bases=[], keywords=[], decorator_list=[],
                          body=[n for n in js_context.body if isinstance(n, ast.FunctionDef)
                                and n.name in ('serialize_node', 'serialize_attributes')])
void = [n for n in tree.body if isinstance(n, ast.Assign) and any(
    isinstance(t, ast.Name) and t.id == 'VOID_ELEMENTS' for t in n.targets)]
ns['escape'] = html.escape
exec(compile(ast.fix_missing_locations(ast.Module(body=void + [serializer], type_ignores=[])),
             str(source), 'exec'), ns)
serialize = ns['Serializer']().serialize_node
def normalize(node):
    if isinstance(node, ns['Text']):
        return {'text': node.text}
    # Attribute order is observable through outerHTML, so it is compared too.
    return {'tag': node.tag, 'attributes': list(node.attributes.items()), 'children': [normalize(c) for c in node.children]}
def ordered(node):
    if 'tag' in node:
        node['attributes'] = list(node['attributes'].items())
        node['children'] = [ordered(c) for c in node['children']]
    return node

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
# Attribute values keep entities undecoded; duplicates keep the first position.
cases += ['<p class="a&quot;b" title=\'x&amp;y\' data-q="it\'s">t</p>',
          '<input disabled="" value checked=>', '<a b=1 c=2 B=3 C="4">x</a>',
          '<img src=x/><br/>t</br/>', '<p a="<>&\'">x < y & z > w</p>',
          '<script>if (a < b && c > d) { s = "</b>"; }</script>']
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
        actual = ordered(json.loads(got.stdout))
        passed += 1
        assert actual == expected, f'case {i}: {value!r}\nexpected {expected}\nactual {actual}'
        python_root = ns['HTMLParser'](value).parse()
        for outer in (True, False):
            want = serialize(python_root) if outer else ''.join(serialize(c) for c in python_root.children)
            got = subprocess.run([exe, '--serialize' if outer else '--serialize-inner', str(p)],
                                 check=True, capture_output=True).stdout.decode('utf-8')
            assert got == want, f'serialize case {i} outer={outer}: {value!r}\nexpected {want!r}\nactual {got!r}'
print(f'DOM differential: {passed} cases passed; {exceptions} Python exceptions mapped to native errors')
