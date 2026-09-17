#!/usr/bin/env python3
"""固定 Python reference 的 JSON oracle；不修改或替換任何行為。"""
import sys
sys.dont_write_bytecode = True
import argparse
import contextlib
import hashlib
import importlib.util
import json
import os
from pathlib import Path

REFERENCE = Path(__file__).resolve().parent / 'reference'


def verify_reference():
    manifest = json.loads((REFERENCE / 'manifest.json').read_text())
    for name, digest in manifest['files'].items():
        path = (REFERENCE / name).resolve()
        if path.parent != REFERENCE or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise RuntimeError('reference manifest mismatch: ' + name)
    return manifest


def load_reference():
    verify_reference()
    spec = importlib.util.spec_from_file_location('tai_gar_fixed_reference', REFERENCE / 'browser.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def selector_value(selector):
    kinds = {'TagSelector': 'tag', 'ClassSelector': 'class', 'IdSelector': 'id',
             'VisitedSelector': 'visited', 'HasSelector': 'has',
             'SelectorSequence': 'sequence', 'DescendantSelector': 'descendant'}
    out = {'kind': kinds[type(selector).__name__], 'priority': selector.priority}
    for key in ('tag', 'class_name', 'id_name'):
        if hasattr(selector, key):
            out['name'] = getattr(selector, key)
    if hasattr(selector, 'selectors'):
        out['children'] = [selector_value(item) for item in selector.selectors]
    elif hasattr(selector, 'selector'):
        out['children'] = [selector_value(selector.selector)]
    return out


def dom_value(node, styled=False):
    if hasattr(node, 'text'):
        out = {'text': node.text}
    else:
        out = {'tag': node.tag, 'attributes': node.attributes,
               'children': [dom_value(child, styled) for child in node.children]}
    if styled:
        out['style'] = node.style
    return out


def node_paths(node, path='0', out=None):
    if out is None:
        out = {}
    out[id(node)] = path
    for index, child in enumerate(node.children):
        node_paths(child, path + '/' + str(index), out)
    return out


def target_path(node, paths):
    if node is None:
        return None
    if id(node) in paths:
        return paths[id(node)]
    return {'synthetic': getattr(node, 'tag', type(node).__name__),
            'parent': target_path(getattr(node, 'parent', None), paths)}


def font_value(font):
    metrics = font.getMetrics()
    return {'family': font.getTypeface().getFamilyName(), 'size': font.getSize(),
            'ascent': -metrics.fAscent, 'descent': metrics.fDescent,
            'space': font.measureText(' ')}


def layout_value(layout, paths):
    out = {'kind': type(layout).__name__, 'node': target_path(getattr(layout, 'node', None), paths)}
    for key in ('x', 'y', 'width', 'height', 'content_height', 'scroll', 'word',
                'ascent', 'descent', 'space_after', 'is_sup', 'is_small_caps'):
        if hasattr(layout, key) and not callable(getattr(layout, key)):
            out[key] = getattr(layout, key)
    if getattr(layout, 'font', None) is not None:
        out['font'] = font_value(layout.font)
    if hasattr(layout, 'nodes'):
        out['nodes'] = [target_path(node, paths) for node in layout.nodes]
    out['children'] = [layout_value(child, paths) for child in layout.children]
    return out


def display_value(command, paths):
    out = {'kind': type(command).__name__}
    for key in ('rect', 'clip_rect'):
        if hasattr(command, key):
            out[key] = list(getattr(command, key))
    for key in ('text', 'color', 'x1', 'y1', 'x2', 'y2', 'thickness', 'radius',
                'sigma', 'opacity', 'blend_mode', 'should_save', 'scroll_y',
                'icon_name', 'stroke_width', 'fill'):
        if hasattr(command, key):
            out[key] = getattr(command, key)
    if hasattr(command, 'font'):
        out['font'] = font_value(command.font)
    if hasattr(command, 'img'):
        out['image'] = {'width': command.img.width(), 'height': command.img.height()}
    if hasattr(command, 'layout_object'):
        out['target'] = target_path(command.layout_object.node, paths)
    if hasattr(command, 'children'):
        out['children'] = [display_value(child, paths) for child in command.children]
    return out


def run(browser, args, source):
    if args.command == 'url':
        url = browser.URL(source)
        if args.resolve is not None:
            url = url.resolve(args.resolve)
        return dict(vars(url), serialized=str(url), origin=url.origin())
    if args.command == 'css':
        return [{'selector': selector_value(selector), 'declarations': body}
                for selector, body in browser.CSSParser(source).parse()]
    nodes = browser.HTMLParser(source).parse()
    if args.command == 'dom':
        return dom_value(nodes)
    rules = browser.DEFAULT_STYLE_SHEET.copy()
    for node in browser.tree_to_list(nodes, []):
        if isinstance(node, browser.Element) and node.tag == 'style':
            rules.extend(browser.CSSParser(browser.style_tag_text(node)).parse())
    rules.extend(browser.CSSParser(args.css).parse())
    rules.sort(key=browser.cascade_priority)
    browser.style(nodes, rules)
    if args.command == 'style':
        return dom_value(nodes, True)
    browser.USE_RTL = args.rtl
    document = browser.DocumentLayout(nodes, args.width)
    document.layout()
    display = []
    browser.paint_tree(document, display)
    paths = node_paths(nodes)
    return {'layout': layout_value(document, paths),
            'display': [display_value(command, paths) for command in display]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('dom', 'style', 'css', 'url', 'layout'))
    parser.add_argument('input', nargs='?', help='省略時從 stdin 讀取')
    parser.add_argument('--file', type=Path)
    parser.add_argument('--css', default='')
    parser.add_argument('--resolve')
    parser.add_argument('--width', type=int, default=800)
    parser.add_argument('--rtl', action='store_true')
    args = parser.parse_args()
    source = args.file.read_text() if args.file else args.input if args.input is not None else sys.stdin.read()
    previous = Path.cwd()
    try:
        # 原程式以 cwd 讀 browser.css/openmoji；固定 cwd，禁止尋找外部可變副本。
        os.chdir(REFERENCE)
        with contextlib.redirect_stdout(sys.stderr):
            result = run(load_reference(), args, source)
    finally:
        os.chdir(previous)
    print(json.dumps(result, ensure_ascii=False, sort_keys=True, allow_nan=False))


if __name__ == '__main__':
    main()
