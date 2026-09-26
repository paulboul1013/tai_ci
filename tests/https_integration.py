#!/usr/bin/env python3
"""Native HTTPS trust and tab-set security driver.

Generates a per-run test CA, serves 127.0.0.1 HTTP, trusted HTTPS and
untrusted HTTPS fixtures, then checks:

* test_network trusts the test CA only when given --ca, and reports a
  certificate error otherwise or for the untrusted server;
* test_tabset_secure's TaiTabSetView.secure transitions;
* test_tabs_secure_window's address-field geometry against the frozen oracle
  and its lock-slot, field and star clicks under SDL's dummy driver.

    python3 tests/https_integration.py TEST_NETWORK TEST_TABSET_SECURE \
        TEST_TABS_SECURE_WINDOW
"""

import json
import pathlib
import queue
import subprocess
import sys
import tempfile
import threading

ROOT = pathlib.Path(__file__).resolve().parents[1]
ORACLE = ROOT / "tests" / "fixtures" / "https_oracle.json"
# Widths where the native address row sits at a different y than the oracle
# for reasons outside this slice: below 125px native always reserves the
# second tab-strip row that Python adds only once a second tab wraps
# (docs/reference-presentation.md). x extents and the lock's offset from the
# field are still compared there.
KNOWN_Y_DIFFERENCES = {120}

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import https_fixture  # noqa: E402


def fetch(test_network, url, ca=None):
    args = [test_network] + (["--ca", str(ca)] if ca else []) + [url]
    completed = subprocess.run(args, check=True, capture_output=True, text=True,
                               timeout=20)
    return json.loads(completed.stdout)


def check_network(test_network, servers, ca):
    trusted = fetch(test_network, servers.https_url("/secure-home"), ca)
    assert not trusted["error"] and not trusted["certificate_error"], trusted
    assert "<h1>secure-home</h1>" in trusted["body"], trusted
    redirected = fetch(test_network, servers.http_url("/to-https"), ca)
    assert "<h1>redirected-secure</h1>" in redirected["body"], redirected

    default_trust = fetch(test_network, servers.https_url("/secure-home"))
    assert default_trust["error"] and default_trust["certificate_error"], default_trust
    untrusted = fetch(test_network, servers.untrusted_url("/secure-home"), ca)
    assert untrusted["error"] and untrusted["certificate_error"], untrusted
    # A missing CA file fails closed instead of falling back to system roots.
    missing = fetch(test_network, servers.https_url("/secure-home"),
                    pathlib.Path(ca).with_name("missing-ca.pem"))
    assert missing["error"] and missing["certificate_error"], missing
    transport = fetch(test_network, servers.https_url("/secure-fail"), ca)
    assert transport["error"] and not transport["certificate_error"], transport


def check_geometry(test_window):
    native = json.loads(subprocess.run(
        [test_window, "--geometry"], check=True, capture_output=True,
        text=True, timeout=20).stdout)
    oracle = json.loads(ORACLE.read_text(encoding="utf-8"))["geometry"]
    assert set(native) == set(oracle), (sorted(native), sorted(oracle))
    for key, expected in oracle.items():
        width = expected["window_width"]
        got = native[key]
        ex, ey, er, eb = expected["address_rect"]
        gx, gy, gr, gb = got["address_rect"]
        # Native keeps the field inside the window (PORTING_PLAN.md).
        want = [ex, ey, min(er, width), eb]
        if width in KNOWN_Y_DIFFERENCES:
            want[1], want[3] = gy, gb
        assert [gx, gy, gr, gb] == want, (key, got, expected)
        if expected["lock_rect"] is None:
            assert got["lock_rect"] is None, (key, got)
            continue
        lx, ly, lr, lb = expected["lock_rect"]
        glx, gly, glr, glb = got["lock_rect"]
        assert [glx, glr] == [lx, lr], (key, got, expected)
        assert [round(gly - gy, 3), round(glb - gy, 3)] == \
            [round(ly - ey, 3), round(lb - ey, 3)], (key, got, expected)


def check_window(test_window, servers, ca):
    completed = subprocess.run(
        [test_window, str(servers.http.server_port),
         str(servers.https.server_port), str(ca)],
        capture_output=True, text=True, timeout=120)
    if completed.returncode != 0:
        sys.stderr.write(completed.stderr)
        raise AssertionError("test_tabs_secure_window exited with {}".format(
            completed.returncode))
    assert completed.stdout.strip() == (
        "tabbed SDL HTTPS chrome passed: lock slot, shifted field, and star "
        "at 800/232/120/70px"), completed.stdout


def reader(stream, lines):
    for line in iter(stream.readline, ""):
        lines.put(line.rstrip("\r\n"))
    lines.put(None)


def expect(lines, expected):
    try:
        line = lines.get(timeout=20)
    except queue.Empty as error:
        raise AssertionError("timed out waiting for {!r}".format(expected)) from error
    if line != expected:
        raise AssertionError("expected {!r}, got {!r}".format(expected, line))


def check_tabset(test_tabset, servers, ca):
    process = subprocess.Popen(
        [test_tabset, str(servers.http.server_port), str(servers.https.server_port),
         str(servers.untrusted.server_port), str(ca)],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1,
    )
    lines = queue.Queue()
    thread = threading.Thread(target=reader, args=(process.stdout, lines), daemon=True)
    thread.start()
    try:
        expect(lines, "SECURE_PENDING")
        servers.wait_seen("/secure-delay")
        process.stdin.write("\n")
        process.stdin.flush()
        expect(lines, "SECURE_PENDING_CHECKED")
        servers.gates["/secure-delay"].set()
        process.stdin.write("\n")
        process.stdin.flush()
        expect(lines, "tab-set HTTPS security passed: success, pending, failures, "
                      "redirects, history, and per-tab state")
        if process.wait(timeout=20) != 0:
            raise AssertionError("test_tabset_secure exited with {}".format(process.returncode))
    finally:
        servers.gates["/secure-delay"].set()
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        thread.join(timeout=1)


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    check_geometry(sys.argv[3])
    with tempfile.TemporaryDirectory(prefix="tai-https-native-") as directory:
        material = https_fixture.make_material(pathlib.Path(directory))
        ca = material["trusted"]["ca"]
        servers = https_fixture.FixtureServers(material)
        try:
            check_network(sys.argv[1], servers, ca)
            check_tabset(sys.argv[2], servers, ca)
            check_window(sys.argv[3], servers, ca)
        finally:
            servers.close()
    print("HTTPS integration: test CA trust, certificate errors, tab-set "
          "security transitions, oracle address geometry, and dummy-SDL lock "
          "chrome passed")


if __name__ == "__main__":
    main()
