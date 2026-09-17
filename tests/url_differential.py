#!/usr/bin/env python3
"""URL class AST differential；只執行原 class，不替換其方法。"""
import ast
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import oracle

oracle.verify_reference()
tree = ast.parse((oracle.REFERENCE / 'browser.py').read_text())
node = next(n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == 'URL')
namespace = {}
exec(compile(ast.Module(body=[node], type_ignores=[]), 'reference/browser.py', 'exec'), namespace)
URL = namespace['URL']
probe = sys.argv[1] if len(sys.argv) > 1 else '/tmp/tai-url-probe'
inputs = ['','x','HTTP://a','http://a','https://a/x#f','file:///tmp/a','file://host/a',
          'about:blank','about:a/b','data:text/plain,a/b','mailto:a/b','view-source:http://a/x#f',
          'view-source:file:///tmp/a','ftp://a','data://x','about://a','mailto://a',
          'http://[::1]/x','http://a:bad/x#frag']
inputs += ['http://h:'+p+'/x#f' for p in ['0','80','443','-1','+80',' 80 ','8_0','8__0',
           '١٢','１２','1_٢','9223372036854775808','-9223372036854775809','0x10','',
           '\u200380\u2003','1\u001c','1:2','+_1','1_','-0','+00080','٠٠٨٠','+ 80','_1','1_ ','1__2','𝟠𝟘']]
relatives = ['', ' ', '#', '#new', '//other/x', '/abs', '../x', '../../x', './x','x',
             '?q', 'HTTP://x', 'javascript:a','httpſ://a','data:a','mailto:a','\u2003x\u2003']
cases = [(x,None) for x in inputs]
cases += [(b,r) for b in inputs for r in relatives]
failures=[]; exceptions=0
for base, relative in cases:
    with contextlib.redirect_stdout(io.StringIO()):
        try:
            expected=URL(base)
            if relative is not None: expected=expected.resolve(relative)
            expected=None if expected is None else dict(vars(expected),serialized=str(expected),origin=expected.origin())
        except Exception:
            exceptions+=1
            expected=None  # Native API maps reference resolve exception to NULL.
    command=[probe,base]+([] if relative is None else [relative])
    result=subprocess.run(command,capture_output=True,text=True,check=True)
    actual=json.loads(result.stdout)
    if actual != expected: failures.append({'base':base,'relative':relative,'expected':expected,'actual':actual})
print(json.dumps({'cases':len(cases),'python_exceptions':exceptions,'mismatches':len(failures),'examples':failures},ensure_ascii=False,indent=2))
sys.exit(bool(failures))
