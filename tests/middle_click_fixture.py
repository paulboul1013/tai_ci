"""127.0.0.1 HTTP fixture shared by the middle-click oracle probe and test.

Every page has a unique <h1> so waiting for a heading cannot succeed early.
/links holds one link of each kind in LINKS (the absolute one names this
server's own port), a paragraph of plain text and a text input; /cancel runs
/cancel.js, whose click listener cancels its link. The server records each
request's method and path, so a test can assert which loads a step started.
"""

import re
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


PORT_TOKEN = "<PORT>"

# Link text names the key; "absolute" is formatted with the server's port.
LINKS = {
    "absolute": "http://127.0.0.1:{port}/b",
    "relative": "/c",
    "fragment": "#part",
    "mailto": "mailto:frag@example.com#part",
}

CANCEL_HREF = "/b"


def page(heading, body="", head=""):
    return ("<!doctype html><html><head><title>{0}</title>{2}</head><body>"
            "<h1>{0}</h1>{1}</body></html>").format(heading, body, head)


def links_page(port):
    return page("links", "".join(
        '<p><a href="{}">{} link</a></p>'.format(href.format(port=port), name)
        for name, href in LINKS.items())
        + '<p id="part">plain text</p><input name="q" value="hi">')


def pages(port):
    return {
        "/links": ("text/html", links_page(port)),
        "/b": ("text/html", page("page-b")),
        "/c": ("text/html", page("page-c")),
        "/cancel": ("text/html", page(
            "cancel",
            '<p><a href="{}">cancel link</a></p>'.format(CANCEL_HREF),
            '<script src="/cancel.js"></script>')),
        "/cancel.js": ("text/javascript",
                       "document.querySelectorAll('a')[0].addEventListener("
                       "'click', function (e) { e.preventDefault(); });"),
    }


class MiddleClickServer:
    def __init__(self):
        self.lock = threading.Lock()
        self.requests = []
        owner = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_args):
                pass

            def do_GET(self):
                with owner.lock:
                    owner.requests.append(
                        {"method": self.command, "path": self.path})
                entry = owner.pages.get(self.path.split("?", 1)[0])
                if entry is None:
                    self.send_error(404)
                    return
                payload = entry[1].encode()
                self.send_response(200)
                self.send_header("Content-Type",
                                 entry[0] + "; charset=utf-8")
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
        self.pages = pages(self.port)
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       name="middle-click-fixture",
                                       daemon=True)
        self.thread.start()

    def url(self, path):
        return "http://127.0.0.1:{}{}".format(self.port, path)

    def href(self, name):
        return LINKS[name].format(port=self.port)

    def normalize(self, value):
        return re.sub(r"http://127\.0\.0\.1:\d+",
                      "http://127.0.0.1:" + PORT_TOKEN, value)

    def mark(self):
        with self.lock:
            return len(self.requests)

    def since(self, mark):
        with self.lock:
            return ["{} {}".format(item["method"], item["path"])
                    for item in self.requests[mark:]]

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
