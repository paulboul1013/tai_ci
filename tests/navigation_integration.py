import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


requests = []


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def _handle(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8") if length else ""
        requests.append({
            "method": self.command,
            "path": self.path,
            "body": body,
            "headers": {key.lower(): value for key, value in self.headers.items()},
        })
        if self.path == "/drop":
            self.close_connection = True
            self.connection.close()
            return

        if self.path == "/source":
            response = b'<html><body><a href="/fragment#target">next</a></body></html>'
        elif self.path == "/fragment":
            content = b"".join(b"<p>filler</p>" for _ in range(16))
            response = b"<html><body>" + content + b'<p id="target">target</p></body></html>'
        else:
            response = b"<html><body><p>submitted</p></body></html>"
        self.send_response(200)
        self.send_header("Content-Length", str(len(response)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(response)
        self.close_connection = True

    do_GET = _handle
    do_POST = _handle


server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
server.daemon_threads = True
worker = threading.Thread(target=server.serve_forever, daemon=True)
worker.start()
try:
    subprocess.run([sys.argv[1], str(server.server_port)], check=True)
finally:
    server.shutdown()
    server.server_close()
    worker.join()

assert [entry["method"] for entry in requests] == [
    "GET", "GET", "GET", "POST", "GET"
]
assert [entry["path"] for entry in requests] == [
    "/get?old=1&a+b=hello+world&flag=on&empty=",
    "/source",
    "/fragment",
    "/post",
    "/drop",
]
assert [entry["body"] for entry in requests] == [
    "", "", "", "q=%C3%A9+%26", ""
]
assert requests[2]["headers"].get("referer") == (
    f"http://127.0.0.1:{server.server_port}/source"
)
assert "content-type" not in requests[3]["headers"]
assert requests[3]["headers"].get("content-length") == str(
    len("q=%C3%A9+%26".encode("utf-8"))
)
print("Browser navigation HTTP fixture: GET, POST, referrer load, and failed replacement passed")
