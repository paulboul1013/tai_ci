"""127.0.0.1 HTTP fixture and scenarios for whole-page JS DOM checks.

Shared by tests/js_page_oracle_probe.py (the frozen Python browser) and
tests/js_dom_integration.py (native). Every page has a unique <h1> so waiting
for a heading cannot succeed early; external scripts only (Python does not
run inline scripts, D6).

A scenario is (name, path, heading, actions). Actions run in order:
    ("click", id)      click the middle of the element's first text box (an
                       input: its control box), in viewport coordinates
                       truncated to int like the SDL event path
    ("type", text)     one keypress per character
    ("state", label)   record a checkpoint
A checkpoint holds the DOM (no style), the page title (Python's fallback
name is the empty string), the scroll offset, the URL and the id of the
focused input (null when nothing is focused).
"""

import re
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


PORT_TOKEN = "<PORT>"


def page(title, heading, body, script):
    return ("<!doctype html><html><head><title>{0}</title>"
            "<script src=\"/{3}.js\"></script></head>"
            "<body><h1>{1}</h1>{2}</body></html>").format(
                title, heading, body, script)


PAGES = {
    "/mutate": page("Start", "mutate",
                    "<ul id=list><li>one</li></ul><p id=note>note</p>",
                    "mutate"),
    "/click": page("Before click", "click",
                   "<p id=target>Click me</p>", "click"),
    "/throw": page("Throw", "throw",
                   "<p id=target>Throw here</p><p id=other>Then here</p>",
                   "throw"),
    "/fragment": page("Fragment", "fragment",
                      "<a id=go href=\"#t\">Go down</a><p id=t>Target</p>",
                      "fragment"),
    "/focus": page("Focus", "focus",
                   "<div id=box><input id=field value=cat></div>"
                   "<p id=after>after</p>", "focus"),
}

# The scripts run at load; the <body> elements exist because each <script>
# sits in <head> but Python and native both run scripts after parsing.
SCRIPTS = {
    "/mutate.js": """
var li = document.createElement('li');
li.innerHTML = 'two <b>bold</b>';
list.appendChild(li);
var first = list.children[0];
var zero = document.createElement('li');
zero.innerHTML = 'zero';
list.insertBefore(zero, first);
list.removeChild(first);
note.setAttribute('class', 'done');
note.innerHTML = note.innerHTML + ' &amp; more';
document.querySelectorAll('title')[0].innerHTML = 'Mutated';
""",
    "/click.js": """
var clicks = 0;
target.addEventListener('click', function () {
  clicks = clicks + 1;
  var added = document.createElement('p');
  added.setAttribute('class', 'added');
  added.innerHTML = 'Added ' + clicks;
  document.querySelectorAll('body')[0].appendChild(added);
  document.querySelectorAll('title')[0].innerHTML = 'Clicked ' + clicks;
});
""",
    "/throw.js": """
target.addEventListener('click', function () {
  target.setAttribute('data-seen', 'yes');
  throw Error('boom');
});
other.addEventListener('click', function () {
  other.setAttribute('data-seen', 'yes');
  document.querySelectorAll('title')[0].innerHTML = 'Recovered';
});
""",
    "/fragment.js": """
go.addEventListener('click', function () {
  var gap = document.createElement('div');
  gap.setAttribute('style', 'height:900px');
  gap.innerHTML = 'gap';
  document.querySelectorAll('body')[0].insertBefore(gap, t);
});
""",
    "/focus.js": """
field.addEventListener('keydown', function () {
  box.removeChild(field);
  document.querySelectorAll('title')[0].innerHTML = 'Removed';
});
""",
}

SCENARIOS = (
    ("mutate", "/mutate", "mutate", (("state", "loaded"),)),
    ("click", "/click", "click", (
        ("state", "loaded"),
        ("click", "target"), ("state", "first_click"),
        ("click", "target"), ("state", "second_click"),
    )),
    ("throw", "/throw", "throw", (
        ("click", "target"), ("state", "after_throw"),
        ("click", "other"), ("state", "after_next"),
    )),
    ("fragment", "/fragment", "fragment", (
        ("click", "go"), ("state", "scrolled"),
    )),
    ("focus", "/focus", "focus", (
        ("click", "field"), ("state", "focused"),
        ("type", "x"), ("state", "after_key"),
    )),
)


class JsPageServer:
    def __init__(self):
        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_args):
                pass

            def do_GET(self):
                path = self.path.split("?", 1)[0]
                if path in SCRIPTS:
                    self._respond(SCRIPTS[path].encode(), "text/javascript")
                elif path in PAGES:
                    self._respond(PAGES[path].encode(),
                                  "text/html; charset=utf-8")
                else:
                    self._respond(b"", "text/plain", 404)

            def _respond(self, payload, content_type, status=200):
                self.send_response(status)
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
                                       name="js-page-fixture", daemon=True)
        self.thread.start()

    def url(self, path):
        return "http://127.0.0.1:{}{}".format(self.port, path)

    def normalize(self, value):
        return re.sub(r"http://127\.0\.0\.1:\d+",
                      "http://127.0.0.1:" + PORT_TOKEN, value)

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
