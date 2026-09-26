"""Local HTTP fixture for the native bookmark tab-set integration test."""

from __future__ import annotations

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import queue
import subprocess
import sys
import threading
import time


requests: list[str] = []
condition = threading.Condition()
release_pending = threading.Event()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args: object) -> None:
        pass

    def do_GET(self) -> None:
        with condition:
            requests.append(self.path)
            condition.notify_all()
        path = self.path.split("?", 1)[0]
        if path == "/pending" and not release_pending.wait(timeout=10):
            self.close_connection = True
            return
        heading = {"/alpha": "alpha-page", "/zeta": "zeta-page",
                   "/pending": "pending-page",
                   "/persist": "persist-page"}.get(path, "unexpected-page")
        body = ("<!doctype html><html><body><h1>" + heading +
                "</h1></body></html>").encode()
        try:
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass
        self.close_connection = True


def wait_for_request(path: str, count: int = 1) -> None:
    deadline = time.monotonic() + 8
    with condition:
        while requests.count(path) < count:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(
                    f"expected {count} GETs for {path!r}; received {requests!r}"
                )
            condition.wait(remaining)


def stdout_reader(stream: object, lines: queue.Queue[str | None]) -> None:
    assert hasattr(stream, "readline")
    while True:
        line = stream.readline()
        if not line:
            lines.put(None)
            return
        lines.put(line.rstrip("\r\n"))


def expect_line(lines: queue.Queue[str | None], expected: str) -> None:
    try:
        line = lines.get(timeout=12)
    except queue.Empty as error:
        raise AssertionError(f"timed out waiting for {expected!r}") from error
    if line != expected:
        raise AssertionError(f"expected {expected!r}, got {line!r}")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} PATH_TO_TEST_TABSET_BOOKMARKS")

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    process: subprocess.Popen[str] | None = None
    reader: threading.Thread | None = None
    lines: queue.Queue[str | None] = queue.Queue()
    try:
        process = subprocess.Popen(
            [sys.argv[1], str(server.server_port)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        assert process.stdout is not None
        reader = threading.Thread(
            target=stdout_reader, args=(process.stdout, lines), daemon=True
        )
        reader.start()

        expect_line(lines, "BOOKMARK_PENDING")
        wait_for_request("/pending")
        release_pending.set()
        assert process.stdin is not None
        process.stdin.write("\n")
        process.stdin.flush()
        expect_line(lines, "bookmark tab-set integration passed")
        assert process.wait(timeout=5) == 0

        # The first alpha GET loads the second tab; the second is a real click
        # from about:bookmarks. The fragment stays client-side, while query '&'
        # must reach the server unchanged instead of becoming '&amp;'.
        assert requests.count("/alpha?x=1&y=2") == 2, requests
        assert requests.count("/zeta") == 1, requests
        assert requests.count("/pending") == 1, requests
        # Four tab-set restarts share one XDG data directory.
        assert requests.count("/persist") == 4, requests
        assert not any("&amp;" in path for path in requests), requests
        print("Bookmark HTTP integration: exact URL GET, shared tabs, list, history, and persistence passed")
        return 0
    finally:
        release_pending.set()
        if process is not None and process.poll() is None:
            try:
                if process.stdin is not None:
                    process.stdin.write("\n")
                    process.stdin.flush()
            except (BrokenPipeError, OSError):
                pass
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)
        if process is not None and process.stderr is not None:
            diagnostic = process.stderr.read()
            if diagnostic:
                sys.stderr.write(diagnostic)
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)
        if reader is not None:
            reader.join(timeout=0.1)


if __name__ == "__main__":
    raise SystemExit(main())
