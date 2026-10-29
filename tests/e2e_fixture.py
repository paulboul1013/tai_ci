"""127.0.0.1 HTTP fixture for the full-pipeline E2E test (docs/e2e-plan.md).

GET / redirects to /index.html, which loads /style.css (sent gzip-encoded)
and /app.js (sent chunked). app.js appends a <p id="added">, retitles the
page and sets a cookie. #to-bottom jumps to the #bottom fragment; #to-form,
inside #bottom, opens /form.html, whose POST to /submit echoes the received
fields. /inline.html runs an inline <script> and is native-only: the Python
oracle never runs inline scripts (PORTING_PLAN.md D6).

The server records each request's method, path, Cookie header and body.
Pages use only the default sans-serif font and no <img>. #bottom sits
above the index page's maximum scroll: Python sizes its first tab without
the tab strip (531.66px at 800x600, native 525.18px; PORTING_PLAN.md "Tab
列換行時的 viewport"), so the two maximums differ while a fragment offset
below them does not.
"""

import gzip
import html
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qsl


COOKIE = "e2e=1"
INDEX_TITLE = "E2E index"
READY_TITLE = "E2E ready"
FORM_TITLE = "E2E form"
SUBMITTED_TITLE = "E2E submitted"
INLINE_TITLE = "E2E inline"
# What the script types into #name and #secret.
TEXT_VALUE = "alice"
PASSWORD_VALUE = "s3cret"


def filler(label, count):
    return "".join("<p>{} filler row {}</p>".format(label, index)
                   for index in range(count))


def page(title, body, head=""):
    return ("<!doctype html><html><head><title>{0}</title>{2}</head><body>"
            "<h1>{0}</h1>{1}</body></html>").format(title, body, head)


INDEX = page(
    INDEX_TITLE,
    '<p><a id="to-bottom" href="#bottom">jump to bottom</a></p>'
    '<div class="banner">styled banner</div>'
    + filler("index", 6)
    + '<div id="bottom"><p>bottom of page</p>'
      '<p><a id="to-form" href="/form.html">open form</a></p></div>'
    + filler("tail", 30),
    '<link rel="stylesheet" href="/style.css">'
    '<script src="/app.js"></script>')

STYLE = """
h1 { color: #003366; }
a { color: #0055aa; }
.banner { background-color: #ffcc00; color: #331100; }
#bottom { background-color: #ddeeff; }
"""

# Only DOM calls the frozen tests/reference/runtime.js defines: it has no
# document.title, so the script retitles through the <title> element.
APP = (
    "var added = document.createElement('p');\n"
    "added.setAttribute('id', 'added');\n"
    "added.innerHTML = 'added by app.js';\n"
    "document.querySelectorAll('body')[0].appendChild(added);\n"
    "document.querySelectorAll('title')[0].innerHTML = '" + READY_TITLE
    + "';\n"
    "document.cookie = '" + COOKIE + "';\n")

FORM = page(
    FORM_TITLE,
    '<form method="post" action="/submit">'
    '<p>Name <input id="name" name="name"></p>'
    '<p>Secret <input id="secret" type="password" name="secret"></p>'
    '<p><input id="agree" type="checkbox" name="agree"> agree</p>'
    '<p><button id="send">Send</button></p>'
    '</form>')

INLINE = page(
    INLINE_TITLE,
    '<div id="target">inline target</div>'
    "<script>document.querySelectorAll('#target')[0].style = "
    "'background-color:#00aa55;height:100px';</script>")

PAGES = {
    "/index.html": ("text/html", INDEX),
    "/style.css": ("text/css", STYLE),
    "/app.js": ("text/javascript", APP),
    "/form.html": ("text/html", FORM),
    "/inline.html": ("text/html", INLINE),
}


def submitted(body):
    rows = "".join(
        '<p id="field-{0}">{0} = {1}</p>'.format(html.escape(name),
                                                 html.escape(value))
        for name, value in parse_qsl(body, keep_blank_values=True))
    return page(SUBMITTED_TITLE, rows)


class E2EServer:
    def __init__(self):
        self.lock = threading.Condition()
        self.requests = []
        owner = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_args):
                pass

            def record(self, body):
                with owner.lock:
                    owner.requests.append(
                        {"method": self.command, "path": self.path,
                         "cookie": self.headers.get("Cookie"),
                         "body": body})
                    owner.lock.notify_all()

            def send(self, status, headers, payload):
                self.send_response(status)
                for name, value in headers:
                    self.send_header(name, value)
                self.send_header("Connection", "close")
                self.end_headers()
                try:
                    self.wfile.write(payload)
                except (BrokenPipeError, ConnectionResetError):
                    pass
                self.close_connection = True

            def do_GET(self):
                self.record(None)
                path = self.path.split("?", 1)[0]
                if path == "/":
                    # Absolute: the oracle resolves a relative Location
                    # against scheme and host only, dropping the port.
                    self.send(302, [("Location", owner.url("/index.html")),
                                    ("Content-Length", "0")], b"")
                    return
                entry = PAGES.get(path)
                if entry is None:
                    self.send(404, [("Content-Length", "0")], b"")
                    return
                content_type, text = entry
                payload = text.encode()
                headers = [("Content-Type", content_type + "; charset=utf-8")]
                if path == "/style.css":
                    payload = gzip.compress(payload, mtime=0)
                    headers.append(("Content-Encoding", "gzip"))
                if path == "/app.js":
                    headers.append(("Transfer-Encoding", "chunked"))
                    payload = chunked(payload)
                else:
                    headers.append(("Content-Length", str(len(payload))))
                self.send(200, headers, payload)

            def do_POST(self):
                length = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(length).decode("utf-8", "replace")
                self.record(body)
                if self.path.split("?", 1)[0] != "/submit":
                    self.send(404, [("Content-Length", "0")], b"")
                    return
                payload = submitted(body).encode()
                self.send(200, [("Content-Type", "text/html; charset=utf-8"),
                                ("Content-Length", str(len(payload)))],
                          payload)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.port = self.server.server_port
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       name="e2e-fixture", daemon=True)
        self.thread.start()

    def url(self, path):
        return "http://127.0.0.1:{}{}".format(self.port, path)

    def mark(self):
        with self.lock:
            return len(self.requests)

    def since(self, mark):
        with self.lock:
            return [dict(item) for item in self.requests[mark:]]

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


def chunked(payload, size=64):
    """Several chunks, so a reader that stops after the first one fails."""
    out = b""
    for start in range(0, len(payload), size):
        piece = payload[start:start + size]
        out += b"%x\r\n" % len(piece) + piece + b"\r\n"
    return out + b"0\r\n\r\n"

