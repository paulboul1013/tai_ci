"""127.0.0.1 HTTP fixture shared by the stale-completion oracle probe and the
native test (docs/stale-callback-plan.md).

Every document has a unique <h1>. A gated path holds its response until the
test releases it, so a test can order "request sent", "newer navigation
committed" and "late response arrives" without sleeping. ``wait_served``
returns once the server answered a released path (response written, or the
client already gone); an expired gate never counts, so "the late response
arrived" is an observable event.
"""

import re
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


PORT_TOKEN = "<PORT>"
GATE_PATHS = ("/slow.css", "/slow.js", "/slow-a", "/slow-b", "/slow-c",
              "/links")


def page(title, head="", body=""):
    return ("<!doctype html><html><head><title>{0}</title>{1}</head><body>"
            "<h1>{0}</h1>{2}<p>{0} body</p></body></html>").format(
                title, head, body)


HTML = "text/html; charset=utf-8"

# If the late script ever ran it would leave two marks: a request for
# /stale-beacon and a rewritten <h1>.
STALE_SCRIPT = (
    "var request = new XMLHttpRequest();\n"
    "request.open('GET', '/stale-beacon', false);\n"
    "request.send();\n"
    "document.querySelectorAll('h1')[0].innerHTML = 'stale-script-ran';\n")

# Banner colours let a screenshot tell the pages apart
# (tests/stale_window_integration.py); the DOM probes ignore them.
B_BANNER = 0x00aa55ff
A_BANNER = 0xffcc00ff


def banner(rgba):
    return '<div style="background-color:#{:06x}">banner</div>'.format(
        rgba >> 8)


RESOURCES = {
    "/start": (HTML, page("start")),
    "/b": (HTML, page("page-b", body=banner(B_BANNER))),
    "/x": (HTML, page("page-x")),
    "/css-a": (HTML, page("css-a",
                          head='<link rel="stylesheet" href="/slow.css">',
                          body=banner(A_BANNER))),
    "/js-a": (HTML, page("js-a", body=banner(A_BANNER)
                         + '<script src="/slow.js"></script>')),
    "/slow-a": (HTML, page("slow-a")),
    "/slow-b": (HTML, page("slow-b")),
    "/slow-c": (HTML, page("slow-c")),
    "/links": (HTML, page("links", body='<p><a href="/x">to x</a></p>')),
    "/slow.css": ("text/css", "h1 { color: red; }\n"),
    "/slow.js": ("application/javascript", STALE_SCRIPT),
}


class StaleServer:
    def __init__(self):
        self.lock = threading.Condition()
        self.requests = []
        self.served = []
        self.gates = {path: threading.Event() for path in GATE_PATHS}
        owner = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_args):
                pass

            def do_GET(self):
                path = self.path.split("?", 1)[0]
                with owner.lock:
                    owner.requests.append(self.path)
                    owner.lock.notify_all()
                if self.respond(path):
                    with owner.lock:
                        owner.served.append(self.path)
                        owner.lock.notify_all()

            def respond(self, path):
                """Answer once the gate opens; False when the gate expired
                or the path is unknown, so wait_served never matches it."""
                gate = owner.gates.get(path)
                failed = gate is not None and not gate.wait(timeout=60)
                if failed or path not in RESOURCES:
                    self.close_connection = True
                    try:
                        self.connection.shutdown(2)
                    except OSError:
                        pass
                    self.connection.close()
                    return False
                content_type, text = RESOURCES[path]
                payload = text.encode()
                try:
                    self.send_response(200)
                    self.send_header("Content-Type", content_type)
                    self.send_header("Content-Length", str(len(payload)))
                    self.send_header("Connection", "close")
                    self.end_headers()
                    self.wfile.write(payload)
                except (BrokenPipeError, ConnectionResetError):
                    pass
                self.close_connection = True
                return True

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.port = self.server.server_port
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       name="stale-fixture", daemon=True)
        self.thread.start()

    def url(self, path):
        return "http://127.0.0.1:{}{}".format(self.port, path)

    def normalize(self, value):
        return re.sub(r"http://127\.0\.0\.1:\d+",
                      "http://127.0.0.1:" + PORT_TOKEN, value)

    def mark(self):
        with self.lock:
            return len(self.requests)

    def since(self, mark):
        with self.lock:
            return list(self.requests[mark:])

    def _wait(self, log, path, mark, label, timeout):
        with self.lock:
            if not self.lock.wait_for(lambda: path in log[mark:],
                                      timeout=timeout):
                raise AssertionError("timed out waiting for {} {}".format(
                    label, path))

    def wait_seen(self, path, mark=0, timeout=8):
        self._wait(self.requests, path, mark, "request", timeout)

    def wait_served(self, path, timeout=8):
        self._wait(self.served, path, 0, "response", timeout)

    def release(self, path):
        self.gates[path].set()

    def reset_gate(self, path):
        with self.lock:
            self.served = [item for item in self.served if item != path]
        self.gates[path] = threading.Event()

    def close(self):
        for gate in self.gates.values():
            gate.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
