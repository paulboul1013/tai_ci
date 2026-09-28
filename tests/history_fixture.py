"""127.0.0.1 HTTP fixture shared by the history oracle probe and native test.

Every page has a unique <h1> so waiting for a heading cannot succeed early.
The server records each request's method, path and body. A gate holds one
document until the test releases it, and paths listed in ``fail_paths`` close
the connection without a response so the browser shows its Network Error page.
"""

import re
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


PORT_TOKEN = "<PORT>"
GATE_PATHS = ("/delay-b", "/delay-c")
ALWAYS_FAIL = ("/fail",)


def filler(label, count):
    return "".join("<p>{} filler row {}</p>".format(label, index)
                   for index in range(count))


def page(title, body=""):
    return ("<!doctype html><html><head><title>{0}</title></head><body>"
            "<h1>{0}</h1>{1}{2}</body></html>").format(
                title, body, filler(title, 30))


def fragment_page():
    return ("<!doctype html><html><head><title>frag</title></head><body>"
            "<h1>frag</h1><p><a href=\"#target\">to target</a></p>"
            "{}<h2 id=\"target\">target</h2>{}</body></html>").format(
                filler("before", 60), filler("after", 60))


def form_page():
    return ("<!doctype html><html><head><title>form</title></head><body>"
            "<h1>form</h1><form action=\"/posted\" method=\"post\">"
            "<input name=\"q\" value=\"hi\"><button>send</button></form>"
            "</body></html>")


PAGES = {
    "/a": page("page-a"),
    "/b": page("page-b"),
    "/c": page("page-c"),
    "/d": page("page-d"),
    "/e": page("page-e"),
    "/delay-b": page("delay-b"),
    "/delay-c": page("delay-c"),
    "/frag": fragment_page(),
    "/form": form_page(),
    "/posted": page("posted"),
    "/other": page("page-other"),
}


class HistoryServer:
    def __init__(self):
        self.lock = threading.Condition()
        self.requests = []
        self.gates = {path: threading.Event() for path in GATE_PATHS}
        self.fail_paths = set(ALWAYS_FAIL)
        owner = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_args):
                pass

            def do_POST(self):
                self.do_GET()

            def do_GET(self):
                length = int(self.headers.get("Content-Length", "0") or "0")
                body = self.rfile.read(length).decode("utf-8", "replace")
                path = self.path.split("?", 1)[0]
                with owner.lock:
                    owner.requests.append(
                        {"method": self.command, "path": self.path,
                         "body": body})
                    fail = path in owner.fail_paths
                    owner.lock.notify_all()
                gate = owner.gates.get(path)
                if gate is not None and not gate.wait(timeout=15):
                    fail = True
                if fail or path not in PAGES:
                    self.close_connection = True
                    try:
                        self.connection.shutdown(2)
                    except OSError:
                        pass
                    self.connection.close()
                    return
                payload = PAGES[path].encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(payload)))
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
                                       name="history-fixture", daemon=True)
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
            return [dict(item) for item in self.requests[mark:]]

    def wait_seen(self, path, mark=0, timeout=8):
        with self.lock:
            if not self.lock.wait_for(
                    lambda: any(item["path"] == path
                                for item in self.requests[mark:]),
                    timeout=timeout):
                raise AssertionError("timed out waiting for {}".format(path))

    def release(self, path):
        self.gates[path].set()

    def reset_gate(self, path):
        self.gates[path] = threading.Event()

    def close(self):
        for gate in self.gates.values():
            gate.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
