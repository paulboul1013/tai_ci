#!/usr/bin/env python3
"""Headless check of script execution order and CSP (intentional difference D6).

Native runs inline <script> elements (Python only runs src scripts) in source
order with external ones, and a valid CSP default-src blocks every inline
script while external scripts follow the origin allowlist.

    python3 tests/inline_script_integration.py build/tai-browser
"""
import json
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

APPEND = "t.setAttribute('data-log', t.getAttribute('data-log') + '{}');"


def page(port):
    return ("<p id=t data-log=''>x</p>"
            "<script>" + APPEND.format("a") + "</script>"
            "<script src='/b.js'></script>"
            "<script>" + APPEND.format("c") + "</script>"
            "<script src='http://localhost:{}/d.js'></script>".format(port))


# Python JSContext.run prints "Script <src> crashed <error>" and the load goes
# on; native writes it to stderr (D1). The inline script is native only (D6).
CRASH_PAGE = ("<p id=t data-log=''>x</p>"
              "<script src='/boom.js'></script>"
              "<script>throw Error('inline');</script>"
              "<script src='/b.js'></script>")
CRASH_REPORTS = ["Script /boom.js crashed Error: boom",
                 "Script <inline-script> crashed Error: inline"]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def do_GET(self):
        port = self.server.server_port
        own = "http://127.0.0.1:{}".format(port)
        policies = {
            "/plain": None,
            "/csp": "default-src " + own,
            "/csp-both": "default-src {} http://localhost:{}".format(own, port),
            "/csp-none": "default-src",
            "/csp-other": "script-src 'self'",
        }
        headers = {}
        if self.path == "/crash":
            body, kind = CRASH_PAGE, "text/html"
        elif self.path == "/boom.js":
            body, kind = "throw Error('boom');", "text/javascript"
        elif self.path == "/b.js":
            body, kind = APPEND.format("b"), "text/javascript"
        elif self.path == "/d.js":
            body, kind = APPEND.format("d"), "text/javascript"
        elif self.path in policies:
            body, kind = page(port), "text/html"
            if policies[self.path]:
                headers["Content-Security-Policy"] = policies[self.path]
        else:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        data = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(data)))
        for key, value in headers.items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(data)


def find(node, tag):
    if node.get("tag") == tag:
        return node
    for child in node.get("children", []):
        found = find(child, tag)
        if found:
            return found
    return None


def main():
    browser = sys.argv[1]
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    expected = {
        "/plain": "abcd",      # inline and external, in source order
        "/csp": "b",           # inline blocked, localhost not allowed
        "/csp-both": "bd",     # inline blocked, both origins allowed
        "/csp-none": "",       # empty allowlist blocks everything
        "/csp-other": "abcd",  # not default-src: no policy applies
    }
    try:
        for path, log in expected.items():
            url = "http://127.0.0.1:{}{}".format(server.server_port, path)
            output = subprocess.run([browser, "--headless", url], check=True,
                                    capture_output=True, text=True,
                                    timeout=30).stdout
            paragraph = find(json.loads(output)["dom"], "p")
            actual = paragraph["attributes"]["data-log"]
            assert actual == log, (path, log, actual)
        url = "http://127.0.0.1:{}/crash".format(server.server_port)
        result = subprocess.run([browser, "--headless", url], check=True,
                                capture_output=True, text=True, timeout=30)
        paragraph = find(json.loads(result.stdout)["dom"], "p")
        assert paragraph["attributes"]["data-log"] == "b", paragraph
        reports = [line for line in result.stderr.splitlines()
                   if " crashed " in line]
        assert reports == CRASH_REPORTS, reports
    finally:
        server.shutdown()
        server.server_close()
    print("Inline script integration: {} pages passed".format(len(expected) + 1))


if __name__ == "__main__":
    main()
