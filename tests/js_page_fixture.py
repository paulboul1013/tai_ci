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

The XHR pages (slice 5) record each outcome in a data-v attribute. A bridge
error is reduced in the page script to csp, cors or other, because Python
wraps it as "Error while calling Python Function ...". localhost:<port> is
the cross-origin peer of 127.0.0.1:<port>; "<PORT>" in scripts and headers
is replaced with the server port when served. /echo answers
m=METHOD|o=ORIGIN|r=REFERER|c=COOKIE|b=BODY ("-" when absent) and, for
?acao=page|star|wrong, sends Access-Control-Allow-Origin.
"""

import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


PORT_TOKEN = "<PORT>"


def page(title, heading, body, script):
    return ("<!doctype html><html><head><title>{0}</title>"
            "<script src=\"/{3}.js\"></script></head>"
            "<body><h1>{1}</h1>{2}</body></html>").format(
                title, heading, body, script)


XHR_HELPERS = """
function outcome(f) {
  try { return 'ok:' + f(); }
  catch (e) {
    var m = String(e);
    return 'error:' + (m.indexOf('blocked by CSP') >= 0 ? 'csp'
        : m.indexOf('blocked by CORS') >= 0 ? 'cors' : 'other');
  }
}
function send(method, url, body) {
  var x = new XMLHttpRequest();
  x.open(method, url, false);
  x.send(body);
  return x.responseText;
}
function record(label, value) {
  var p = document.createElement('p');
  p.setAttribute('class', label);
  p.setAttribute('data-v', value);
  out.appendChild(p);
}
"""

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
    "/xhr": page("XHR", "xhr", "<div id=out></div>", "xhr"),
    "/xhr-click": page("XHR click", "xhr click",
                       "<p id=target>Fetch</p><div id=out></div>",
                       "xhr-click"),
    "/xhr-csp": page("XHR CSP", "xhr csp", "<div id=out></div>", "xhr-csp"),
    "/xhr-norefer": page("XHR no referrer", "xhr no referrer",
                         "<div id=out></div>", "xhr-norefer"),
    "/xhr-slow": page("XHR slow", "xhr slow", "<div id=out></div>",
                      "xhr-slow"),
    # Native-only pages for tests/test_tabset_xhr.c (no oracle scenario).
    "/xhr-hang": page("XHR hang", "xhr hang", "<div id=out></div>",
                      "xhr-hang"),
    "/xhr-wait": page("XHR wait", "xhr wait", "<div id=out></div>",
                      "xhr-wait"),
}

# Extra response headers per page path.
PAGE_HEADERS = {
    "/xhr-csp": (("Content-Security-Policy",
                  "default-src http://127.0.0.1:<PORT>"),),
    "/xhr-norefer": (("Referrer-Policy", " No-Referrer "),),
}

# /xhr-slow waits this long: more than the 2 s script limit.
SLOW_SECONDS = 2.3

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
    "/xhr.js": XHR_HELPERS + """
var cross = 'http://localhost:<PORT>/echo';
record('get', outcome(function () { return send('GET', '/echo'); }));
record('post', outcome(function () { return send('POST', '/echo', 'a=1&b=2'); }));
record('post-empty', outcome(function () { return send('POST', '/echo', ''); }));
record('relative', outcome(function () { return send('GET', 'echo?q=1'); }));
record('fragment', outcome(function () { return send('GET', '/echo#frag'); }));
document.cookie = 'k=v; SameSite=Lax';
record('js-cookie', outcome(function () { return send('GET', '/echo'); }));
record('set-cookie', outcome(function () { return send('GET', '/setcookie'); }));
record('cookie-after', document.cookie);
record('http-only', outcome(function () { return send('GET', '/setcookie?httponly=1'); }));
record('cookie-hidden', document.cookie);
document.cookie = 'x=1';
record('cookie-kept', outcome(function () { return send('GET', '/echo'); }));
record('cross-page', outcome(function () { return send('GET', cross + '?acao=page'); }));
record('cross-star', outcome(function () { return send('POST', cross + '?acao=star', 'p'); }));
record('cross-wrong', outcome(function () { return send('GET', cross + '?acao=wrong'); }));
record('cross-none', outcome(function () { return send('GET', cross); }));
record('redirect', outcome(function () { return send('GET', '/redirect'); }));
record('redirect-absolute', outcome(function () {
  return send('GET', '/redirect?absolute=1');
}));
record('missing', outcome(function () { return send('GET', '/missing'); }));
record('refused', outcome(function () { return send('GET', 'http://127.0.0.1:1/'); }));
record('data', outcome(function () { return send('GET', 'data:text/plain,hi'); }));
record('no-open', outcome(function () { return new XMLHttpRequest().send(); }));
record('async', outcome(function () { new XMLHttpRequest().open('GET', '/echo', true); }));
record('label', outcome(function () { return send('DELETE', '/echo'); }));
""",
    "/xhr-click.js": XHR_HELPERS + """
var clicks = 0;
target.addEventListener('click', function () {
  clicks = clicks + 1;
  record('click', outcome(function () { return send('GET', '/echo?n=' + clicks); }));
  record('cross', outcome(function () {
    return send('GET', 'http://localhost:<PORT>/echo?acao=star');
  }));
  record('cookie', document.cookie);
  document.querySelectorAll('title')[0].innerHTML = 'Fetched ' + clicks;
});
document.cookie = 'tab=1';
""",
    "/xhr-csp.js": XHR_HELPERS + """
record('same', outcome(function () { return send('GET', '/echo'); }));
record('cross', outcome(function () {
  return send('GET', 'http://localhost:<PORT>/echo?acao=star');
}));
""",
    "/xhr-norefer.js": XHR_HELPERS + """
record('same', outcome(function () { return send('GET', '/echo'); }));
record('cross', outcome(function () {
  return send('GET', 'http://localhost:<PORT>/echo?acao=star');
}));
""",
    "/xhr-slow.js": XHR_HELPERS + """
record('slow', outcome(function () { return send('GET', '/slow'); }));
record('after', 'done');
""",
    "/xhr-hang.js": XHR_HELPERS + """
while (true) { try { send('GET', '/slow?seconds=30'); } catch (e) {} }
""",
    "/xhr-wait.js": XHR_HELPERS + """
record('slow', outcome(function () { return send('GET', '/slow?seconds=4'); }));
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
    ("xhr", "/xhr", "xhr", (("state", "loaded"),)),
    ("xhr_click", "/xhr-click", "xhr click", (
        ("state", "loaded"),
        ("click", "target"), ("state", "first_click"),
        ("click", "target"), ("state", "second_click"),
    )),
    ("xhr_csp", "/xhr-csp", "xhr csp", (("state", "loaded"),)),
    ("xhr_norefer", "/xhr-norefer", "xhr no referrer", (("state", "loaded"),)),
    ("xhr_slow", "/xhr-slow", "xhr slow", (("state", "loaded"),)),
)


class JsPageServer:
    def __init__(self):
        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_args):
                pass

            def do_GET(self):
                self._route(None)

            def do_POST(self):
                length = int(self.headers.get("Content-Length") or 0)
                self._route(self.rfile.read(length).decode("utf-8"))

            def _route(self, body):
                path, _, query = self.path.partition("?")
                port = str(self.server.server_port)
                if path in SCRIPTS:
                    self._respond(SCRIPTS[path].replace(PORT_TOKEN, port)
                                  .encode(), "text/javascript")
                elif path in PAGES:
                    headers = [(key, value.replace(PORT_TOKEN, port))
                               for key, value in PAGE_HEADERS.get(path, ())]
                    self._respond(PAGES[path].encode(),
                                  "text/html; charset=utf-8", headers=headers)
                elif path == "/echo":
                    self._echo(body, query)
                elif path == "/setcookie":
                    cookie = ("sid=s; HttpOnly" if "httponly" in query
                              else "srv=1; Path=/")
                    self._respond(b"set", "text/plain",
                                  headers=[("Set-Cookie", cookie)])
                elif path == "/redirect":
                    # Python drops the port of a relative Location, so the
                    # relative form fails in both browsers.
                    target = "/echo?from=redirect"
                    if "absolute" in query:
                        target = "http://127.0.0.1:{}{}".format(port, target)
                    self._respond(b"", "text/plain", 302, headers=[
                        ("Location", target)])
                elif path == "/slow":
                    seconds = dict(part.partition("=")[::2]
                                   for part in query.split("&")).get("seconds")
                    time.sleep(float(seconds) if seconds else SLOW_SECONDS)
                    self._respond(b"slow", "text/plain")
                elif path == "/missing":
                    self._respond(b"missing", "text/plain", 404)
                else:
                    self._respond(b"", "text/plain", 404)

            def _echo(self, body, query):
                origin = self.headers.get("Origin")
                fields = [("m", self.command), ("o", origin),
                          ("r", self.headers.get("Referer")),
                          ("c", self.headers.get("Cookie")), ("b", body),
                          ("q", query or None)]
                text = "|".join("{}={}".format(key, "-" if value is None
                                               else value)
                                for key, value in fields)
                acao = dict(part.partition("=")[::2]
                            for part in query.split("&")).get("acao")
                allow = {"page": origin, "star": "*",
                         "wrong": "http://evil.test"}.get(acao)
                headers = [("Access-Control-Allow-Origin", allow)] if allow \
                    else []
                self._respond(text.encode(), "text/plain", headers=headers)

            def _respond(self, payload, content_type, status=200,
                         headers=()):
                self.send_response(status)
                self.send_header("Content-Type", content_type)
                for key, value in headers:
                    self.send_header(key, value)
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
        return re.sub(r"http://(127\.0\.0\.1|localhost):\d+",
                      r"http://\1:" + PORT_TOKEN, value)

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
