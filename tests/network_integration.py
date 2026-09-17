import ast
import contextlib
import gzip
import io
import json
import pathlib
import socket
import ssl
import subprocess
import sys
import threading
import time
import tempfile
from datetime import datetime, timezone
from email.utils import parsedate_to_datetime
from urllib.parse import unquote
from network_fixture import server

# Same original definitions as executable oracle; only GUI imports are excluded.
p=pathlib.Path(__file__).parent/'reference'/'browser.py'
names={'URL','parse_cookie_string','cookie_expiration','cookie_is_expired','get_valid_cookie','normalize_referrer_policy','should_send_referrer','referer_value','serialize_cookie'}
tree=ast.parse(p.read_text())
ns=dict(globals(), http_cache={}, socket_cache={}, COOKIE_JAR={})
exec(compile(ast.Module(body=[n for n in tree.body if isinstance(n,(ast.ClassDef,ast.FunctionDef)) and n.name in names],type_ignores=[]),str(p),'exec'),ns)
s=server();thread=threading.Thread(target=s.serve_forever,daemon=True);thread.start()
base=f'http://127.0.0.1:{s.server_port}'
try:
    for path,payload,ref,policy in [('/echo',None,None,None),('/echo','中文=hello',base+'/page#frag',None),('/gzip',None,None,None),('/chunked',None,None,None),('/redirect','keep POST',None,None),('/invalid-utf8',None,None,None),('/echo',None,base+'/ref','no-referrer'),('/echo',None,'http://example.invalid/x','same-origin')]:
        args=[sys.argv[1],base+path,payload if payload is not None else '-']
        if ref: args.append(ref)
        if policy: args.append(policy)
        actual=json.loads(subprocess.run(args,check=True,capture_output=True,text=True).stdout)
        with contextlib.redirect_stdout(io.StringIO()):
            headers,body=ns['URL'](base+path).request(ns['URL'](ref) if ref else None,payload=payload,referrer_policy=policy)
        assert not actual['error'],actual
        if path in ('/echo','/redirect'):
            a,b=json.loads(actual['body']),json.loads(body)
            # curl chooses a different ordering; HTTP fields are case insensitive.
            a['headers']={k.lower():v for k,v in a['headers'].items()}
            b['headers']={k.lower():v for k,v in b['headers'].items()}
            assert a==b,(a,b)
        else: assert actual['body']==body,(actual,body)
    loop=json.loads(subprocess.run([sys.argv[1],base+'/loop'],check=True,capture_output=True,text=True).stdout)
    assert loop['error']=='Redirect loop detected!',loop
    large=json.loads(subprocess.run([sys.argv[1],base+'/large'],check=True,capture_output=True,text=True).stdout)
    assert '32 MiB limit' in large['error'],large['error']
    with tempfile.TemporaryDirectory() as directory:
        file=pathlib.Path(directory)/'text.html';file.write_bytes(b'hello\r\nworld\rend')
        invalid=pathlib.Path(directory)/'invalid';invalid.write_bytes(b'\xff')
        bad=json.loads(subprocess.run([sys.argv[1],'file://'+str(invalid)],check=True,capture_output=True,text=True).stdout)
        assert bad['error']=='file is not valid UTF-8'
        for url in ['about:blank','data:text/html,a%20b%FF','data:text/html;base64,SGVsbG8=', 'file://'+str(file), 'file://'+str(file)+'-missing', 'mailto:test@example.invalid']:
            actual=json.loads(subprocess.run([sys.argv[1],url],check=True,capture_output=True,text=True).stdout)
            with contextlib.redirect_stdout(io.StringIO()):
                _,body=ns['URL'](url).request(None)
            assert not actual['error'] and actual['body']==body,(url,actual,body)
    subprocess.run([sys.argv[1],base,'--suite'],check=True)
    print('Network differential: 8 HTTP + 6 local URL cases; loop/cache/cookie/parallel/cancellation suite passed')
finally:
    s.shutdown();s.server_close();thread.join()
