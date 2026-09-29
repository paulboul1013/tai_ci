"""127.0.0.1 HTTP fixture shared by the window-title oracle probe and native test.

Every page has a unique <h1> so waiting for a heading cannot succeed early.
The /t/* pages cover the <title> markup rules (whitespace, several titles,
entities, nested elements, non-ASCII text, placement). /retitle runs
/retitle.js, which rewrites the title through innerHTML. A gate holds /delay
until the test releases it, which keeps a navigation pending. /fail closes
the connection without a response, producing the Network Error page.
"""

import re
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


PORT_TOKEN = "<PORT>"
GATE_PATHS = ("/delay",)


def page(title_markup, heading, head_extra=""):
    return ("<!doctype html><html><head>{0}{2}</head><body><h1>{1}</h1>"
            "<p>{1} body</p></body></html>").format(
                title_markup, heading, head_extra)


# Path -> (markup, heading). Markup variants keep the heading outside <title>.
MARKUP_PAGES = {
    "/t/basic": (page("<title>Page A</title>", "basic"), "basic"),
    "/t/none": (page("", "none"), "none"),
    "/t/padded": (page("<title>  padded  </title>", "padded"), "padded"),
    "/t/empty-first": (page("<title></title><title>Second</title>",
                            "empty-first"), "empty-first"),
    "/t/blank": (page("<title>   </title>", "blank"), "blank"),
    "/t/blank-then": (page("<title> \n\t </title><title>After blank</title>",
                           "blank-then"), "blank-then"),
    "/t/two": (page("<title>First</title><title>Second</title>", "two"),
               "two"),
    "/t/amp": (page("<title>A &amp; B</title>", "amp"), "amp"),
    "/t/lt": (page("<title>&lt;b&gt;</title>", "lt"), "lt"),
    "/t/nested": (page("<title>x<b>y</b>z</title>", "nested"), "nested"),
    "/t/cjk": (page("<title>中文標題</title>", "cjk"), "cjk"),
    "/t/newline": (page("<title>line one\nline two</title>", "newline"),
                   "newline"),
    "/t/controls": (page("<title>\t\r\n spaced \x0b\x0c</title>", "controls"),
                    "controls"),
    "/t/nbsp-entity": (page("<title>&nbsp;nbsp&nbsp;</title>", "nbsp-entity"),
                       "nbsp-entity"),
    "/t/nbsp-literal": (page("<title> lit </title>", "nbsp-literal"),
                        "nbsp-literal"),
    "/t/ideographic": (page("<title>　wide　</title>", "ideographic"),
                       "ideographic"),
    "/t/body": ("<!doctype html><html><head></head><body><h1>body</h1>"
                "<title>In Body</title><p>body body</p></body></html>",
                "body"),
    "/t/headless": ("<title>Headless</title><h1>headless</h1><p>x</p>",
                    "headless"),
}

PAGES = {
    "/a": (page("<title>Page A</title>", "page-a"), "page-a"),
    "/b": (page("<title>Page B</title>", "page-b"), "page-b"),
    "/c": (page("<title>Page C</title>", "page-c"), "page-c"),
    "/home": (page("<title>Home</title>", "home"), "home"),
    "/delay": (page("<title>Delayed</title>", "delay"), "delay"),
    "/retitle": (page("<title>Before script</title>", "retitle",
                      "<script src=\"/retitle.js\"></script>"), "retitle"),
}
PAGES.update(MARKUP_PAGES)

RETITLE_JS = ('document.querySelectorAll("title")[0].innerHTML = '
              '"After script";\n')


class TitleServer:
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
                    owner.requests.append(path)
                    owner.lock.notify_all()
                gate = owner.gates.get(path)
                if gate is not None and not gate.wait(timeout=15):
                    path = None
                if path == "/retitle.js":
                    self._respond(RETITLE_JS.encode(), "text/javascript")
                    return
                if path not in PAGES:
                    self.close_connection = True
                    try:
                        self.connection.shutdown(2)
                    except OSError:
                        pass
                    self.connection.close()
                    return
                self._respond(PAGES[path][0].encode(),
                              "text/html; charset=utf-8")

            def _respond(self, payload, content_type):
                self.send_response(200)
                self.send_header("Content-Type", content_type)
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
                                       name="title-fixture", daemon=True)
        self.thread.start()

    def url(self, path):
        return "http://127.0.0.1:{}{}".format(self.port, path)

    def normalize(self, value):
        return re.sub(r"http://127\.0\.0\.1:\d+",
                      "http://127.0.0.1:" + PORT_TOKEN, value)

    def wait_seen(self, path, timeout=8):
        with self.lock:
            if not self.lock.wait_for(lambda: path in self.requests,
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
