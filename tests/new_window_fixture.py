"""127.0.0.1 HTTP fixture shared by the new-window oracle probe and native test.

Every page has a unique <h1> so waiting for a heading cannot succeed early.
The server records each request's method, path and Cookie header. /cookie-set
answers with a session cookie and /cookie-check echoes the Cookie header it
received, so a later request from another window shows whether the cookie jar
is shared. A gate holds /delay until the test releases
it, which keeps a load in flight while its window closes.
"""

import re
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


PORT_TOKEN = "<PORT>"
GATE_PATHS = ("/delay",)
COOKIE = "nw=shared"


def filler(label, count):
    return "".join("<p>{} filler row {}</p>".format(label, index)
                   for index in range(count))


def page(title, body=""):
    return ("<!doctype html><html><head><title>{0}</title></head><body>"
            "<h1>{0}</h1>{1}{2}</body></html>").format(
                title, body, filler(title, 60))


PAGES = {
    "/home": page("home"),
    "/a": page("page-a"),
    "/b": page("page-b"),
    "/c": page("page-c"),
    "/links": page("links", "<p><a href=\"/b\">to b</a></p>"),
    "/cookie-set": page("cookie-set"),
    "/cookie-check": page("cookie-check"),
    "/delay": page("delay"),
}


class NewWindowServer:
    def __init__(self):
        self.lock = threading.Condition()
        self.requests = []
        self.gates = {path: threading.Event() for path in GATE_PATHS}
        owner = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_args):
                pass

            def do_GET(self):
                path = self.path.split("?", 1)[0]
                with owner.lock:
                    owner.requests.append(
                        {"method": self.command, "path": self.path,
                         "cookie": self.headers.get("Cookie")})
                    owner.lock.notify_all()
                gate = owner.gates.get(path)
                if gate is not None and not gate.wait(timeout=15):
                    path = None
                if path not in PAGES:
                    self.close_connection = True
                    try:
                        self.connection.shutdown(2)
                    except OSError:
                        pass
                    self.connection.close()
                    return
                payload = PAGES[path]
                if path == "/cookie-check":
                    # Echo the cookie so a native test can read it on the page.
                    payload = page("cookie-check", "<p>cookie=[{}]</p>".format(
                        self.headers.get("Cookie") or ""))
                payload = payload.encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(payload)))
                if path == "/cookie-set":
                    self.send_header("Set-Cookie", COOKIE)
                self.send_header("Connection", "close")
                self.end_headers()
                try:
                    self.wfile.write(payload)
                except (BrokenPipeError, ConnectionResetError):
                    pass
                self.close_connection = True

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.port = self.server.server_port
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       name="new-window-fixture", daemon=True)
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
            return [{"method": item["method"], "path": item["path"],
                     "cookie": item["cookie"]}
                    for item in self.requests[mark:]]

    def wait_seen(self, path, mark=0, timeout=8):
        with self.lock:
            if not self.lock.wait_for(
                    lambda: any(item["path"] == path
                                for item in self.requests[mark:]),
                    timeout=timeout):
                raise AssertionError("timed out waiting for {}".format(path))

    def release(self, path):
        self.gates[path].set()

    def close(self):
        for gate in self.gates.values():
            gate.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
