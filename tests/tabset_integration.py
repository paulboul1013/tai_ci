"""Deterministic localhost driver for the native tab-set integration test."""

from __future__ import annotations

import queue
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


GATE_PATHS = {
    "/initial-delay": threading.Event(),
    "/secondary-delay": threading.Event(),
    "/slow.css": threading.Event(),
    "/cancel-delay": threading.Event(),
    "/replace-delay": threading.Event(),
    "/window-delay": threading.Event(),
    "/window-slow.css": threading.Event(),
    "/window-close-delay": threading.Event(),
}
REQUESTS: list[str] = []
REQUEST_COUNTS: dict[str, int] = {}
REQUEST_REFERERS: list[tuple[str, str | None]] = []
REQUEST_CONDITION = threading.Condition()
SERVER_ERRORS: list[str] = []


def long_content() -> bytes:
    return b"".join(
        f"<p>scroll fixture row {index} provides stable document height</p>".encode()
        for index in range(48)
    )


def page(title: str, links: bytes = b"", head: bytes = b"") -> bytes:
    return (
        b"<!doctype html><html><head><title>tab fixture</title>"
        + head
        + b"</head><body><h1>"
        + title.encode()
        + b"</h1>"
        + links
        + long_content()
        + b"</body></html>"
    )


PAGES = {
    "/initial-delay": page(
        "initial-page",
        b'<a href="/first-next">first next</a>',
    ),
    "/secondary-delay": page("secondary-page"),
    "/first-next": page(
        "first-next",
        b'<a href="/with-css">with css</a>'
        b'<a href="/later-fail">later failure</a>',
    ),
    "/with-css": page(
        "styled-page",
        b'<p id="styled">computed red text</p>',
        head=b'<link rel="stylesheet" href="/slow.css">',
    ),
    "/replacement-fast": page("replacement-fast"),
    "/history-stable": page("history-stable"),
    "/window-home": page("window-home"),
    "/window-css-home": page("window-css-home"),
    "/window-delay": page("window-initial"),
    "/window-close-home": page("close-home"),
    "/window-close-delay": page("close-initial"),
    "/window-subresource": page(
        "subresource-document",
        b'<p id="styled">computed style</p>',
        head=b'<link rel="stylesheet" href="/window-slow.css">',
    ),
}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args: object) -> None:
        pass

    def do_GET(self) -> None:
        path = self.path.split("?", 1)[0]
        with REQUEST_CONDITION:
            REQUESTS.append(path)
            REQUEST_COUNTS[path] = REQUEST_COUNTS.get(path, 0) + 1
            REQUEST_REFERERS.append((path, self.headers.get("Referer")))
            request_count = REQUEST_COUNTS[path]
            REQUEST_CONDITION.notify_all()

        gate = GATE_PATHS.get(path)
        if gate and not gate.wait(timeout=12):
            SERVER_ERRORS.append(f"timed out waiting to release {path}")
            return

        if path in ("/first-fail", "/later-fail") or (
            path == "/flaky" and request_count > 1
        ):
            # A closed connection exercises the transport-failure path, rather
            # than treating an HTTP error response as a successfully loaded page.
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.connection.close()
            self.close_connection = True
            return

        if path in ("/slow.css", "/window-slow.css"):
            body = b"#styled { color: red; }"
            content_type = "text/css; charset=utf-8"
        elif path == "/replace-delay":
            body = page("superseded-old")
            content_type = "text/html; charset=utf-8"
        elif path == "/flaky":
            body = page("flaky-page")
            content_type = "text/html; charset=utf-8"
        else:
            body = PAGES.get(path, b"<html><body><h1>unexpected path</h1></body></html>")
            content_type = "text/html; charset=utf-8"

        try:
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass
        self.close_connection = True


def wait_for_paths(paths: set[str], timeout: float = 6.0) -> None:
    deadline = time.monotonic() + timeout
    with REQUEST_CONDITION:
        while not paths.issubset(REQUESTS):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                missing = sorted(paths.difference(REQUESTS))
                raise AssertionError(f"server did not receive requests: {missing}")
            REQUEST_CONDITION.wait(remaining)


def assert_referer(path: str, expected: str | None) -> None:
    values = [referer for request_path, referer in REQUEST_REFERERS
              if request_path == path]
    if expected is None:
        if not values or any(values):
            raise AssertionError(f"expected no Referer for {path}, got {values}")
    elif expected not in values:
        raise AssertionError(
            f"expected Referer {expected!r} for {path}, got {values}"
        )


def stdout_reader(stream: object, output: queue.Queue[str | None]) -> None:
    assert hasattr(stream, "readline")
    while True:
        value = stream.readline()
        if not value:
            output.put(None)
            return
        output.put(value.rstrip("\r\n"))


def expect_line(
    process: subprocess.Popen[str], output: queue.Queue[str | None], expected: str,
    *, allow_exit: bool = False,
) -> None:
    try:
        line = output.get(timeout=12)
    except queue.Empty as error:
        raise AssertionError(f"timed out waiting for child output {expected!r}") from error
    if line != expected:
        raise AssertionError(f"expected child output {expected!r}, got {line!r}")
    if not allow_exit and process.poll() is not None:
        raise AssertionError(f"child exited before checkpoint {expected!r}")


def release_checkpoint(process: subprocess.Popen[str]) -> None:
    assert process.stdin is not None
    process.stdin.write("\n")
    process.stdin.flush()


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            f"usage: {sys.argv[0]} PATH_TO_TEST_TABSET PATH_TO_TEST_TABS_WINDOW"
        )

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()

    process: subprocess.Popen[str] | None = None
    window_process: subprocess.Popen[str] | None = None
    stdout_lines: queue.Queue[str | None] = queue.Queue()
    reader: threading.Thread | None = None
    window_stdout_lines: queue.Queue[str | None] = queue.Queue()
    window_reader: threading.Thread | None = None
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
            target=stdout_reader, args=(process.stdout, stdout_lines), daemon=True
        )
        reader.start()

        expect_line(process, stdout_lines, "DOCS_PENDING")
        wait_for_paths({"/initial-delay", "/secondary-delay"})
        GATE_PATHS["/secondary-delay"].set()
        release_checkpoint(process)

        expect_line(process, stdout_lines, "SECONDARY_COMMITTED")
        GATE_PATHS["/initial-delay"].set()
        release_checkpoint(process)

        expect_line(process, stdout_lines, "CSS_PENDING")
        wait_for_paths({"/with-css", "/slow.css"})
        release_checkpoint(process)

        expect_line(process, stdout_lines, "CSS_RESIZE_DONE")
        GATE_PATHS["/slow.css"].set()
        release_checkpoint(process)

        expect_line(process, stdout_lines, "REPLACE_PENDING")
        wait_for_paths({"/replace-delay"})
        release_checkpoint(process)

        wait_for_paths({"/replacement-fast"})
        expect_line(process, stdout_lines, "REPLACED_COMMITTED")
        GATE_PATHS["/replace-delay"].set()
        release_checkpoint(process)

        expect_line(process, stdout_lines, "REPLACE_LATE_DONE")
        release_checkpoint(process)

        expect_line(process, stdout_lines, "DESTROY_PENDING")
        wait_for_paths({"/cancel-delay"})
        release_checkpoint(process)

        expect_line(process, stdout_lines, "DESTROYED_PENDING_LOAD")
        GATE_PATHS["/cancel-delay"].set()
        expect_line(
            process,
            stdout_lines,
            "tab-set integration passed: pending routing, CSS, failures, history, resize/scroll isolation, and destroy cancellation",
        )
        return_code = process.wait(timeout=4)
        if return_code != 0:
            raise AssertionError(f"tab-set test exited with status {return_code}")
        origin = f"http://127.0.0.1:{server.server_port}"
        assert_referer("/secondary-delay", None)
        assert_referer("/first-next", origin + "/initial-delay")
        assert_referer("/with-css", origin + "/first-next")
        assert_referer("/later-fail", origin + "/with-css")
        assert_referer("/later-fail", origin + "/first-next")
        assert_referer("/replacement-fast", origin + "/replace-delay")
        assert_referer("/flaky", origin + "/history-stable")
        if SERVER_ERRORS:
            raise AssertionError("; ".join(SERVER_ERRORS))

        window_process = subprocess.Popen(
            [sys.argv[2], str(server.server_port)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        assert window_process.stdout is not None
        window_reader = threading.Thread(
            target=stdout_reader,
            args=(window_process.stdout, window_stdout_lines),
            daemon=True,
        )
        window_reader.start()

        wait_for_paths({"/window-delay", "/window-home"})
        if GATE_PATHS["/window-delay"].is_set():
            raise AssertionError("initial window response was released too early")
        GATE_PATHS["/window-delay"].set()
        expect_line(
            window_process,
            window_stdout_lines,
            "tabbed SDL remained interactive during delayed document load",
        )

        wait_for_paths({"/window-subresource", "/window-slow.css",
                        "/window-css-home"})
        if GATE_PATHS["/window-slow.css"].is_set():
            raise AssertionError("stylesheet response was released too early")
        GATE_PATHS["/window-slow.css"].set()
        expect_line(
            window_process,
            window_stdout_lines,
            "tabbed SDL remained interactive during delayed CSS load",
        )
        wait_for_paths({"/window-close-delay", "/window-close-home"})
        expect_line(
            window_process,
            window_stdout_lines,
            "tabbed SDL closed while a document remained pending",
            allow_exit=True,
        )
        GATE_PATHS["/window-close-delay"].set()
        expect_line(
            window_process,
            window_stdout_lines,
            "tabbed SDL delayed-load integration passed",
            allow_exit=True,
        )
        return_code = window_process.wait(timeout=4)
        if return_code != 0:
            raise AssertionError(f"tab-window test exited with status {return_code}")
        if window_process.stderr is not None:
            diagnostic = window_process.stderr.read()
            if diagnostic:
                sys.stderr.write(diagnostic)
        print("Tab-set HTTP barriers: pending routing, CSS, failure rollback, Referer, superseding, and destroy cancellation passed")
        print("Tabbed SDL HTTP barriers: delayed document and CSS remained interactive")
        return 0
    finally:
        for gate in GATE_PATHS.values():
            gate.set()
        if process is not None and process.poll() is None:
            try:
                release_checkpoint(process)
            except (BrokenPipeError, OSError):
                pass
            try:
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)
        if window_process is not None and window_process.poll() is None:
            try:
                window_process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                window_process.kill()
                window_process.wait(timeout=2)
        if process is not None and process.stderr is not None:
            diagnostic = process.stderr.read()
            if diagnostic:
                sys.stderr.write(diagnostic)
        if window_process is not None and window_process.stderr is not None:
            diagnostic = window_process.stderr.read()
            if diagnostic:
                sys.stderr.write(diagnostic)
        server.shutdown()
        server.server_close()
        server_thread.join(timeout=2)
        if reader is not None:
            reader.join(timeout=0.1)
        if window_reader is not None:
            window_reader.join(timeout=0.1)


if __name__ == "__main__":
    raise SystemExit(main())
