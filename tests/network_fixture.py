"""Deterministic HTTP fixture. No external network dependencies."""
import gzip
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def handle(self):
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError):
            pass  # expected when the native body limit or cancellation closes a socket
    def log_message(self, *_args): pass
    def do_POST(self): self.do_GET()
    def do_GET(self):
        payload = self.rfile.read(int(self.headers.get('Content-Length', '0'))).decode()
        count = self.server.counts.get(self.path, 0) + 1
        self.server.counts[self.path] = count
        headers = {}
        code = 200
        body = json.dumps({'method': self.command, 'path': self.path, 'payload': payload,
                           'headers': dict(self.headers)}, ensure_ascii=False).encode()
        if self.path == '/gzip': body = gzip.compress('héllo 😀'.encode()); headers['Content-Encoding'] = 'gzip'
        if self.path == '/chunked':
            self.send_response(200); self.send_header('Transfer-Encoding','chunked'); self.end_headers()
            self.wfile.write(b'3\r\nabc\r\n3\r\ndef\r\n0\r\n\r\n'); return
        if self.path == '/large': body=gzip.compress(b'x'*(33*1024*1024)); headers['Content-Encoding']='gzip'
        if self.path == '/invalid-utf8': body=b'A\xf0\x9fB\xed\xa0\x80'
        if self.path == '/redirect': code=302; headers['Location']=f'http://127.0.0.1:{self.server.server_port}/echo'
        if self.path == '/loop': code=302; headers['Location']=f'http://127.0.0.1:{self.server.server_port}/loop'
        if self.path == '/cache': headers['Cache-Control']='max-age=600'; body=str(count).encode()
        if self.path == '/no-store': headers['Cache-Control']='no-store,max-age=600'; body=str(count).encode()
        if self.path == '/set-cookie': headers['Set-Cookie']='session=secret; HttpOnly; SameSite=Lax'
        if self.path == '/expire-cookie': headers['Set-Cookie']='session=gone; Expires=Thu, 01 Jan 1970 00:00:00 GMT'
        self.send_response(code)
        self.send_header('Content-Length', str(len(body)))
        for key,value in headers.items(): self.send_header(key,value)
        self.end_headers(); self.wfile.write(body)

def server():
    value=ThreadingHTTPServer(('127.0.0.1',0), Handler)
    value.counts={}
    return value
