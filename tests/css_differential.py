"""CSS/style differential against original AST bodies, without GUI dependencies."""
import ast
import json
import pathlib
import random
import subprocess
import sys
import tempfile

source = pathlib.Path(__file__).parent / 'reference/browser.py'
tree = ast.parse(source.read_text())
names = {'Text', 'Element', 'TagSelector', 'ClassSelector', 'IdSelector',
         'SelectorSequence', 'HasSelector', 'VisitedSelector', 'DescendantSelector',
         'CSSParser', 'style', 'apply_style'}
constants = {'INHERITED_PROPERTIES', 'NON_INHERITED_PROPERTIES',
             'IMPORTANT_OFFSET', 'INLINE_STYLE_PRIORITY'}
body = [n for n in tree.body if
        isinstance(n, (ast.ClassDef, ast.FunctionDef)) and n.name in names or
        isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id in constants for t in n.targets)]
ns = {}
exec(compile(ast.Module(body=body, type_ignores=[]), str(source), 'exec'), ns)

def selector(s):
    kind = type(s).__name__
    kinds = {'TagSelector': 'tag', 'ClassSelector': 'class', 'IdSelector': 'id',
             'SelectorSequence': 'sequence', 'HasSelector': 'has',
             'VisitedSelector': 'visited', 'DescendantSelector': 'descendant'}
    result = dict(kind=kinds[kind], priority=s.priority)
    for key in ('tag', 'class_name', 'id_name'):
        if hasattr(s, key):
            result['name'] = getattr(s, key)
    if hasattr(s, 'selectors'):
        result['children'] = [selector(c) for c in s.selectors]
    if hasattr(s, 'selector'):
        result['children'] = [selector(s.selector)]
    return result

def fixture():
    root = ns['Element']('div', {'id': 'Root', 'class': 'card\u2003wide'}, None)
    child = ns['Element']('a', {'class': 'x', 'style': '  color:inherit; width:33px'}, root)
    child.is_visited = True
    text = ns['Text']('hello', child)
    child.children = [text]
    root.children = [child]
    return root

def styles(node):
    return dict(style=node.style, children=[styles(c) for c in node.children])

cases = ['', 'p {color:red}', 'Straße {CÖLÖR:RED} div {font:12px STRAẞE}',
         '\u2003div {color:red}', '😀 {color:red} div {color:green}',
         'e\u0301 {color:red} div {color:green}',
         'div:has( a) {color:red} div:has(a{unused) {color:blue}',
         '/* comment */ div {color:red} a {color:blue}',
         '* {color:red} div, a {color:red} div > a {color:red}',
         'div {color:red!important;color:blue}',
         'div {color:! IMPORTANT red !important blue; font:italic BOLD 150% Times New Roman}',
         'div {font:normal 12px bold italic 25% Another Font; empty:; bad!:x; width:2px}',
         'div {filter:blur(2px) rgb(1, 2, 3);color:"red blue";bad:);width:3px}',
         'div {filter:foo(;color:red} a {color:blue}',
         'div {color:red', 'div:has(a:has(.x)) {color:red}',
         '.wide {color:blue} #Root {font-size:150%} a:visited {font-size:50%}',
         'div {font-size:12px;color:red!important} a {color:blue;font-size:inherit}',
         'div {font-size:unsupported; width:inherit; unknown:value}',
         'div {font-size:33.3333333333333%}',
         'div {font-size:1e-5%}',
         'div {font-size:1_0%}',
         'div {font-size:１２%}',
         'div {font-size:NaN%}',
         'div {font-size:inf%}',
         'div {font-size:bad%}',
         'div {font-size:4px} a {font-size:0%}',
         '.card:has(div a) {color:purple}',
         'div {color:red}\u0085a {color:blue}',
         'div {co😀lor:red; color:blue}',
         'div {color:red\u2003blue; font:12px\u2003Times}',
         'div {font-size:100000000000000000000%}']
rng = random.Random(823)
selectors = ['div', '.card', '.wide', '#Root', 'a:visited', 'div a',
             'div:has(a)', 'a:has(div)', '.card:has(div a)', 'div😀', 'Σ', 'K']
values = ['red', 'blue!important', 'inherit', '', 'rgb(1, 2, 3)', 'red !foo', 'x)', 'K\u2003x']
for _ in range(100):
    cases.append(' '.join(f'{rng.choice(selectors)} {{color:{rng.choice(values)}; font:italic 125% Times}}' for _ in range(4)))
for _ in range(200):
    size = rng.uniform(-1e8, 1e8) * 10.0 ** rng.randint(-20, 20)
    cases.append(f'div {{font-size:{size}%}} a {{font-size:123.456789%}}')
exe = sys.argv[1]
with tempfile.TemporaryDirectory() as directory:
    path = pathlib.Path(directory) / 'case.css'
    for i, css in enumerate(cases):
        path.write_text(css)
        rules = ns['CSSParser'](css).parse()
        expected = [{'selector': selector(s), 'declarations': {k: list(v) for k, v in d.items()}} for s, d in rules]
        result = subprocess.run([exe, '--json', str(path)], check=True, text=True, capture_output=True)
        actual = json.loads(result.stdout)
        assert actual == expected, f'CSS case {i}: {css!r}\nexpected {expected}\nactual {actual}'
        node = fixture()
        try:
            ns['style'](node, rules)
            expected_style = styles(node)
        except (ValueError, OverflowError):
            expected_style = None
        result = subprocess.run([exe, '--style', str(path)], text=True, capture_output=True)
        if expected_style is None:
            assert result.returncode != 0, f'CSS style error expected {css!r}'
        else:
            assert result.returncode == 0, result.stderr
            actual_style = json.loads(result.stdout)
            assert actual_style == expected_style, f'Style case {i}: {css!r}\nexpected {expected_style}\nactual {actual_style}'
print(f'CSS/style differential: {len(cases)} cases passed')
