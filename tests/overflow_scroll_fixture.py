"""127.0.0.1 HTTP fixture shared by the overflow-scroll oracle probe and test.

Every page has a unique <h1> and enough filler to scroll the page itself.
Containers are fixed-height overflow:scroll <div>s named in CONTAINERS, each
holding more lines than it shows. /cancel's click listener cancels clicks on
#cancel; /detach's listeners drop #gone from the document and turn #flat into
a plain block, each after the click has chosen it.
"""

import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


CONTAINERS = ("box", "outer", "inner", "cancel", "gone", "flat")


def page(heading, body="", head=""):
    return ("<!doctype html><html><head><title>{0}</title>{2}</head><body>"
            "<h1>{0}</h1>{1}</body></html>").format(heading, body, head)


def lines(prefix, count):
    return "".join("<p>{} line {}</p>".format(prefix, index)
                   for index in range(count))


def container(name, height, body):
    return ('<div id="{}" style="overflow:scroll;height:{}px">{}</div>'
            .format(name, height, body))


# The fragment target sits above the page's maximum scroll: Python sizes its
# tab viewport from a chrome bottom without the tab strip, so the two
# maximums differ while a fragment offset below them does not.
FILLER = (lines("filler", 10) + '<p id="middle">middle of page</p>'
          + lines("filler", 30))

BOX = container(
    "box", 100,
    '<p id="box-text">box text</p>'
    '<p><a id="fragment" href="#middle">fragment link</a></p>'
    '<p><a id="away" href="/after">away link</a></p>'
    + lines("box", 8))

PAGES = {
    "/box": ("text/html", page(
        "box", BOX + '<p id="outside">outside text</p>' + FILLER)),
    "/nested": ("text/html", page(
        "nested",
        container("outer", 150,
                  '<p id="outer-text">outer text</p>'
                  + container("inner", 60,
                              '<p id="inner-text">inner text</p>'
                              + lines("inner", 6))
                  + lines("outer", 8))
        + FILLER)),
    "/cancel": ("text/html", page(
        "cancel",
        BOX + container("cancel", 60,
                        '<p id="cancel-text">cancel text</p>'
                        + lines("cancel", 6))
        + FILLER,
        '<script src="/cancel.js"></script>')),
    "/cancel.js": ("text/javascript",
                   "document.querySelectorAll('#cancel')[0]"
                   ".addEventListener('click', function (e) {"
                   " e.preventDefault(); });"),
    "/detach": ("text/html", page(
        "detach",
        container("gone", 60,
                  '<p id="gone-text">gone text</p>' + lines("gone", 6))
        + container("flat", 60,
                    '<p id="flat-text">flat text</p>' + lines("flat", 6))
        + FILLER,
        '<script src="/detach.js"></script>')),
    "/detach.js": ("text/javascript",
                   "var gone = document.querySelectorAll('#gone')[0];"
                   "gone.addEventListener('click', function () {"
                   " document.querySelectorAll('body')[0]"
                   ".removeChild(gone); });"
                   "var flat = document.querySelectorAll('#flat')[0];"
                   "flat.addEventListener('click', function () {"
                   " flat.setAttribute('style', 'height:60px'); });"),
    "/after": ("text/html", page("after", FILLER)),
}


class OverflowScrollServer:
    def __init__(self):
        owner = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_args):
                pass

            def do_GET(self):
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

        self.pages = PAGES
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.port = self.server.server_port
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       name="overflow-scroll-fixture",
                                       daemon=True)
        self.thread.start()

    def url(self, path):
        return "http://127.0.0.1:{}{}".format(self.port, path)

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
