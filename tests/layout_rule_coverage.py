#!/usr/bin/env python3
"""每條 oracle layout／paint 規則都要有 differential 案例。

從凍結 browser.py 的 *Layout class（與它們直接或間接呼叫的模組函式）以 AST 抽出
tag、attribute 與 style 規則，要求 layout_differential.py（layout 方法）或
display_differential.py（paint 方法）至少有一個案例含有該規則、同一條件中的 tag
與所屬控制項的 tag；效果類 style 只能由比對該效果的案例清單滿足。少一個案例，
native 就能在沒人比對的情況下與 oracle 不同。

限制：這是字面檢查，不證明案例走到規則的條件分支（例如 toc 標題只在 block
模式），也不涵蓋不是比較式的規則（例如 get_font 的字型別名表）；這些由案例本身負責。
"""
import ast, importlib.util, pathlib, re, sys
ROOT = pathlib.Path(__file__).resolve().parents[1]
ORACLE = ROOT / 'tests/reference/browser.py'
# 規則屬於控制項 class 時，案例也要含該控制項，否則案例測不到這條規則。
CLASS_TAGS = {'ButtonLayout': 'button', 'InputLayout': 'input'}
# Oracle 自己合成的節點沒有 HTML 寫法，以產生它的 tag 代替。
SYNTHETIC_TAGS = {'button-content': 'button'}
# 效果類 style 只有比對該效果結構的清單才算涵蓋；display 的其他規則任何清單皆可。
EFFECT_LISTS = {'overflow': ('CLIP_CASES', 'SCROLL_CASES'), 'filter': ('BLUR_CASES',),
                'opacity': ('EFFECT_CASES',), 'mix-blend-mode': ('EFFECT_CASES',),
                'border-radius': ('ROUNDED_FILL_CASES', 'CLIP_CASES')}
# make_content_node 同時決定 button 內容的 layout（display/overflow）與 paint（背景、效果）。
BOTH = {'make_content_node'}
# AST 樣式改變時抽取會悄悄變空；這些規則必須被找到。
EXPECTED = {('tag', 'sup', ()), ('tag', 'abbr', ()), ('tag', 'h6', ()),
            ('tag', 'button', ()), ('tag', 'li', ()), ('attr', 'id=toc', ('nav',)),
            ('attr', 'type=checkbox', ('input',)), ('style', 'text-align', ()),
            ('style', 'filter', ())}


def strings(node):
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return [node.value]
    if isinstance(node, (ast.List, ast.Tuple, ast.Set)):
        return [e.value for e in node.elts
                if isinstance(e, ast.Constant) and isinstance(e.value, str)]
    return []


def unwrap(node):
    """去掉 .casefold()／.lower()／.strip()，讓 `x.get(k).casefold() == v` 也算比較式。"""
    while (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
           and node.func.attr in ('casefold', 'lower', 'strip') and not node.args):
        node = node.func.value
    return node


def is_get_on(node, owner):
    return (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
            and node.func.attr == 'get' and isinstance(node.func.value, ast.Attribute)
            and node.func.value.attr == owner and node.args
            and isinstance(node.args[0], ast.Constant))


def tag_values(node):
    """`x.tag == "a"` 或 `x.tag in [...]` 比較的 tag；其他節點回傳空清單。"""
    if not isinstance(node, ast.Compare):
        return []
    left = unwrap(node.left)
    is_tag = ((isinstance(left, ast.Attribute) and left.attr == 'tag')
              or (isinstance(left, ast.Name) and left.id == 'tag')
              or (isinstance(left, ast.Call) and isinstance(left.func, ast.Name)
                  and left.func.id == 'getattr' and len(left.args) > 1
                  and isinstance(left.args[1], ast.Constant)
                  and left.args[1].value == 'tag'))
    return [s for c in node.comparators for s in strings(c)] if is_tag else []


def rules_in(function):
    # 和 attribute 規則在同一個 `and` 條件中的 tag（nav 與 id=toc）也是規則的一部分。
    context = {}
    for node in ast.walk(function):
        if isinstance(node, ast.BoolOp) and isinstance(node.op, ast.And):
            tags = tuple(t for value in node.values for t in tag_values(value))
            for value in node.values:
                for inner in ast.walk(value):
                    context[id(inner)] = context.get(id(inner), ()) + tags
    for node in ast.walk(function):
        for value in tag_values(node):
            yield 'tag', value, ()
        if isinstance(node, ast.Compare) and is_get_on(unwrap(node.left), 'attributes'):
            key = unwrap(node.left).args[0].value
            for value in (s for c in node.comparators for s in strings(c)):
                yield 'attr', f'{key}={value}', tuple(sorted(set(context.get(id(node), ()))))
        if is_get_on(node, 'style'):
            yield 'style', node.args[0].value, ()
        if (isinstance(node, ast.Subscript) and isinstance(node.value, ast.Attribute)
                and node.value.attr == 'style' and isinstance(node.slice, ast.Constant)):
            yield 'style', node.slice.value, ()


def oracle_rules():
    tree = ast.parse(ORACLE.read_text())
    functions = {n.name: n for n in tree.body if isinstance(n, ast.FunctionDef)}
    found = {}
    for cls in (n for n in tree.body if isinstance(n, ast.ClassDef) and n.name.endswith('Layout')):
        for method in (n for n in cls.body if isinstance(n, ast.FunctionDef)):
            surfaces = (('layout', 'display') if method.name in BOTH else
                        ('display',) if method.name.startswith('paint') or
                        method.name == 'should_paint' else ('layout',))
            # 也掃描方法呼叫的模組函式，包含間接呼叫（paint_visual_effects→parse_blur_filter）。
            bodies, pending = [], [method]
            while pending:
                body = pending.pop()
                if body in bodies:
                    continue
                bodies.append(body)
                pending += [functions[c.func.id] for c in ast.walk(body)
                            if isinstance(c, ast.Call) and isinstance(c.func, ast.Name)
                            and c.func.id in functions]
            for body in bodies:
                for rule in rules_in(body):
                    for surface in surfaces:
                        key = (surface, cls.name, rule)
                        found.setdefault(key, f'{cls.name}.{method.name}')
    return found


def module_cases(name):
    """{清單名稱: 該清單的 HTML 案例}。"""
    spec = importlib.util.spec_from_file_location(name, ROOT / f'tests/{name}.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return {attribute: [item[0] if isinstance(item, tuple) else item for item in value]
            for attribute, value in vars(module).items() if attribute.endswith('CASES')}


def token(kind, value):
    if kind == 'tag':
        value = SYNTHETIC_TAGS.get(value, value)
        return re.compile(rf'<{re.escape(value)}[\s>/]', re.I)
    if kind == 'attr':
        key, val = value.split('=', 1)
        return re.compile(rf'''{re.escape(key)}\s*=\s*["']?{re.escape(val)}["'\s>]''', re.I)
    return re.compile(rf'''(style\s*=\s*["'][^"']*|;)\s*{re.escape(value)}\s*:''', re.I)


def main():
    rules = oracle_rules()
    seen = {rule for (_, _, rule) in rules}
    missing_expected = EXPECTED - seen
    assert not missing_expected, f'oracle rule extraction lost {sorted(missing_expected)}; fix rules_in()'
    cases = {'layout': module_cases('layout_differential'),
             'display': module_cases('display_differential')}
    files = {'layout': 'tests/layout_differential.py', 'display': 'tests/display_differential.py'}
    missing = []
    for (surface, cls, (kind, value, tags)), where in sorted(rules.items()):
        owners = set(tags)
        if cls in CLASS_TAGS and (kind, value) != ('tag', CLASS_TAGS[cls]):
            owners.add(CLASS_TAGS[cls])
        needs = [token(kind, value)] + [token('tag', tag) for tag in sorted(owners)]
        lists = EFFECT_LISTS.get(value, ()) if surface == 'display' and kind == 'style' else ()
        pool = [html for name, htmls in cases[surface].items() if not lists or name in lists
                for html in htmls]
        if not any(all(n.search(html) for n in needs) for html in pool):
            extra = ''.join(f' with <{tag}>' for tag in sorted(owners))
            where_list = f' ({" or ".join(lists)})' if lists else ''
            missing.append(f'oracle {kind} rule {value!r}{extra} ({where}) has no case in '
                           f'{files[surface]}{where_list}; add one so native is compared against it')
    if missing:
        print('\n'.join(missing))
        sys.exit(1)
    print(f'{len(rules)} oracle layout/paint rules each have a differential case')


if __name__ == '__main__':
    main()
