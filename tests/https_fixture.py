"""Local HTTPS fixture material shared by the oracle probe and native tests.

Certificates are generated per run with the openssl CLI into a caller-owned
temporary directory. The trusted CA signs the certificate served for
127.0.0.1; a second, never-trusted CA signs the certificate used to provoke a
real verification failure. Nothing here reads or writes outside that
directory, and every server binds only to 127.0.0.1.
"""

from __future__ import annotations

import datetime
import pathlib
import re
import socket
import ssl
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def _openssl(*args: str, cwd: pathlib.Path) -> None:
    subprocess.run(
        ["openssl", *args],
        cwd=cwd,
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )


def _validity() -> tuple[str, str]:
    """A window that starts two days back: WSL2 clocks can step backwards
    after certificates are issued, which would make them "not yet valid"."""
    now = datetime.datetime.now(datetime.timezone.utc)
    fmt = "%y%m%d%H%M%SZ"
    return ((now - datetime.timedelta(days=2)).strftime(fmt),
            (now + datetime.timedelta(days=2)).strftime(fmt))


_CA_CONFIG = """\
[ca]
default_ca = fixture_ca
[fixture_ca]
database = {name}-index.txt
new_certs_dir = .
serial = {name}-serial
default_md = sha256
policy = fixture_policy
unique_subject = no
[fixture_policy]
commonName = supplied
"""


def _issue(directory: pathlib.Path, name: str, common_name: str) -> dict[str, pathlib.Path]:
    """Create a CA named name-ca and a 127.0.0.1 server certificate it signs."""
    ca_key = directory / f"{name}-ca.key"
    ca_csr = directory / f"{name}-ca.csr"
    ca_cert = directory / f"{name}-ca.pem"
    ca_extensions = directory / f"{name}-ca.ext"
    key = directory / f"{name}-server.key"
    csr = directory / f"{name}-server.csr"
    cert = directory / f"{name}-server.pem"
    extensions = directory / f"{name}-server.ext"
    config = directory / f"{name}-ca.cnf"
    config.write_text(_CA_CONFIG.format(name=name), encoding="ascii")
    (directory / f"{name}-index.txt").write_text("", encoding="ascii")
    (directory / f"{name}-serial").write_text("01\n", encoding="ascii")
    ca_extensions.write_text(
        "basicConstraints=critical,CA:TRUE\n"
        "keyUsage=critical,keyCertSign,cRLSign\n"
        "subjectKeyIdentifier=hash\n",
        encoding="ascii",
    )
    extensions.write_text(
        "subjectAltName=IP:127.0.0.1\n"
        "basicConstraints=CA:FALSE\n"
        "keyUsage=critical,digitalSignature\n"
        "extendedKeyUsage=serverAuth\n",
        encoding="ascii",
    )
    start, end = _validity()
    dates = ("-startdate", start, "-enddate", end)
    curve = ("-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1")
    _openssl("req", *curve, "-nodes", "-keyout", ca_key.name,
             "-out", ca_csr.name, "-subj", f"/CN={common_name}", cwd=directory)
    _openssl("ca", "-batch", "-notext", "-config", config.name, "-selfsign",
             "-keyfile", ca_key.name, "-in", ca_csr.name, "-out", ca_cert.name,
             "-extfile", ca_extensions.name, *dates, cwd=directory)
    _openssl("req", *curve, "-nodes", "-keyout", key.name, "-out", csr.name,
             "-subj", "/CN=127.0.0.1", cwd=directory)
    _openssl("ca", "-batch", "-notext", "-config", config.name,
             "-cert", ca_cert.name, "-keyfile", ca_key.name, "-in", csr.name,
             "-out", cert.name, "-extfile", extensions.name, *dates,
             cwd=directory)
    return {"ca": ca_cert, "cert": cert, "key": key}


def make_material(directory: pathlib.Path) -> dict[str, dict[str, pathlib.Path]]:
    """Return {"trusted": {...}, "untrusted": {...}} PEM paths."""
    return {
        "trusted": _issue(directory, "trusted", "Tai Test Root CA"),
        "untrusted": _issue(directory, "untrusted", "Tai Untrusted CA"),
    }


class TLSServer(ThreadingHTTPServer):
    """ThreadingHTTPServer whose handshake runs on the request thread.

    A client that rejects the certificate aborts the handshake; that failure
    stays in its own request thread instead of blocking the accept loop.
    """

    daemon_threads = True

    def __init__(self, handler, cert: pathlib.Path, key: pathlib.Path):
        self.tls_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.tls_context.load_cert_chain(str(cert), str(key))
        super().__init__(("127.0.0.1", 0), handler)

    def finish_request(self, request, client_address):
        # wrap_socket detaches request; the base class then shuts down an
        # already-detached socket, so the TLS socket is closed here.
        try:
            tls = self.tls_context.wrap_socket(request, server_side=True)
        except (ssl.SSLError, OSError):
            request.close()
            return
        try:
            super().finish_request(tls, client_address)
        except (ssl.SSLError, OSError):
            pass
        finally:
            try:
                tls.shutdown(socket.SHUT_WR)
            except OSError:
                pass
            tls.close()


def plain_server(handler) -> ThreadingHTTPServer:
    server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
    server.daemon_threads = True
    return server


def page(heading):
    return (
        "<!doctype html><html><head><title>https fixture</title></head>"
        "<body><h1>{}</h1><p>{} body</p></body></html>".format(heading, heading)
    ).encode()


class FixtureServers:
    """HTTP, trusted HTTPS and untrusted HTTPS servers with one handler.

    /secure-delay waits for gates[path].set(); /secure-fail closes the
    connection without a response; /to-https and /to-http redirect across
    schemes. Every other known path answers with an <h1> naming the page.
    """

    def __init__(self, material):
        self.seen = {}
        self.lock = threading.Lock()
        self.gates = {"/secure-delay": threading.Event()}
        owner = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_args):
                pass

            def do_GET(self):
                path = self.path.split("?", 1)[0]
                with owner.lock:
                    owner.seen.setdefault(path, threading.Event()).set()
                gate = owner.gates.get(path)
                if gate is not None and not gate.wait(timeout=15):
                    self.close_connection = True
                    return
                if path == "/secure-fail":
                    self.close_connection = True
                    try:
                        self.connection.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass
                    return
                if path == "/to-https":
                    self._redirect(owner.https_url("/redirected-secure"))
                    return
                if path == "/to-http":
                    self._redirect(owner.http_url("/redirected-plain"))
                    return
                heading = {
                    "/secure-home": "secure-home",
                    "/secure-delay": "secure-delayed",
                    "/secure-next": "secure-next",
                    "/plain-home": "plain-home",
                    "/redirected-secure": "redirected-secure",
                    "/redirected-plain": "redirected-plain",
                }.get(path, "unexpected")
                self._respond(200, page(heading))

            def _redirect(self, location):
                self._respond(302, b"", {"Location": location})

            def _respond(self, status, body, headers=None):
                self.send_response(status)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                for key, value in (headers or {}).items():
                    self.send_header(key, value)
                self.send_header("Connection", "close")
                self.end_headers()
                try:
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError):
                    pass
                self.close_connection = True

        self.http = plain_server(Handler)
        self.https = TLSServer(
            Handler, material["trusted"]["cert"], material["trusted"]["key"])
        self.untrusted = TLSServer(
            Handler, material["untrusted"]["cert"], material["untrusted"]["key"])
        self.threads = []
        for server in (self.http, self.https, self.untrusted):
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            self.threads.append(thread)

    def http_url(self, path):
        return "http://127.0.0.1:{}{}".format(self.http.server_port, path)

    def https_url(self, path):
        return "https://127.0.0.1:{}{}".format(self.https.server_port, path)

    def untrusted_url(self, path):
        return "https://127.0.0.1:{}{}".format(self.untrusted.server_port, path)

    def normalize(self, value):
        for server, token in ((self.http, "<HTTP_PORT>"),
                              (self.https, "<HTTPS_PORT>"),
                              (self.untrusted, "<UNTRUSTED_PORT>")):
            value = re.sub(r"127\.0\.0\.1:{}\b".format(server.server_port),
                           "127.0.0.1:" + token, value)
        return value

    def wait_seen(self, path):
        with self.lock:
            event = self.seen.setdefault(path, threading.Event())
        if not event.wait(timeout=8):
            raise AssertionError("timed out waiting for HTTP request {}".format(path))

    def close(self):
        for gate in self.gates.values():
            gate.set()
        for server in (self.http, self.https, self.untrusted):
            server.shutdown()
            server.server_close()
        for thread in self.threads:
            thread.join(timeout=2)
