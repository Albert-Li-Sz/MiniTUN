#!/usr/bin/env python3
"""Exercise TLS admission over loopback, including bounded malformed/valid floods."""

import concurrent.futures
import contextlib
import http.client
import json
import math
import pathlib
import resource
import socket
import ssl
import subprocess
import sys
import tempfile
import time


def wait_for(check, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(0.02)
    raise AssertionError("timed out waiting for server state")


def free_ports():
    with socket.socket() as control, socket.socket() as admin:
        control.bind(("127.0.0.1", 0))
        admin.bind(("127.0.0.1", 0))
        return control.getsockname()[1], admin.getsockname()[1]


class Probe:
    def __init__(self, root, control_port, admin_port):
        self.root = root
        self.control_port = control_port
        self.admin_port = admin_port

    def connect(self):
        return socket.create_connection(("127.0.0.1", self.control_port), timeout=1)

    def metrics(self):
        connection = http.client.HTTPConnection("127.0.0.1", self.admin_port, timeout=1)
        try:
            connection.request("GET", "/metrics")
            response = connection.getresponse()
            assert response.status == 200
            return {
                fields[0]: float(fields[1])
                for line in response.read().decode().splitlines()
                if (fields := line.split()) and not line.startswith("#") and len(fields) == 2
            }
        finally:
            connection.close()

    def ready(self):
        try:
            self.metrics()
            return True
        except OSError:
            return False

    def client_command(self, *extra):
        return [
            sys.argv[2], "--endpoint", f"127.0.0.1:{self.control_port}",
            "--ca-cert", str(self.root / "server.crt"),
            "--token-file", str(self.root / "token"), *extra,
        ]


@contextlib.contextmanager
def running(root, name, *options):
    control_port, admin_port = free_ports()
    probe = Probe(root, control_port, admin_port)
    log_path = root / f"{name}.log"
    with log_path.open("w") as log:
        server = subprocess.Popen([
            sys.argv[1], "--foreground", "--listen", f"127.0.0.1:{control_port}",
            "--admin-listen", f"127.0.0.1:{admin_port}",
            "--tls-cert", str(root / "server.crt"),
            "--tls-key", str(root / "server.key"),
            "--clients-config", str(root / "clients.json"),
            "--handshake-timeout", "2", "--heartbeat-interval", "1",
            "--heartbeat-timeout", "3", "--shutdown-timeout", "1", "--io-threads", "2",
            *options,
        ], stdout=log, stderr=log)
        try:
            wait_for(probe.ready)
            yield probe
        except BaseException:
            print(log_path.read_text(), file=sys.stderr)
            raise
        finally:
            server.terminate()
            try:
                assert server.wait(timeout=4) == 0
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
                raise AssertionError("server failed to stop during TLS admission backoff")


def pending_limits(root, context):
    with running(root, "per-ip", "--max-pending-handshakes", "4",
                 "--max-pending-handshakes-per-ip", "2") as probe:
        with contextlib.ExitStack() as sockets:
            for _ in range(10):
                sockets.enter_context(probe.connect())
            wait_for(lambda: probe.metrics()["minitun_tls_admission_rejections_total"] == 8)
            assert probe.metrics()["minitun_pending_handshakes"] == 2
        wait_for(lambda: probe.metrics()["minitun_pending_handshakes"] == 0)
        # Failed sessions release their reservations; normal authentication and
        # heartbeat remain possible after the pending slots were saturated.
        subprocess.run(probe.client_command("--heartbeat-count", "1"), check=True, timeout=5)

    with running(root, "global", "--max-pending-handshakes", "2",
                 "--max-pending-handshakes-per-ip", "2") as probe:
        with contextlib.ExitStack() as sockets:
            for _ in range(8):
                sockets.enter_context(probe.connect())
            wait_for(lambda: probe.metrics()["minitun_pending_handshakes"] == 2)
            time.sleep(0.15)
            # A full global quota pauses accept, leaving additional sockets in
            # the kernel backlog instead of allocating SSL objects or spinning.
            assert probe.metrics()["minitun_connections_total"] == 2

    with contextlib.ExitStack() as sockets:
        with running(root, "shutdown", "--max-pending-handshakes", "1",
                     "--max-pending-handshakes-per-ip", "1") as probe:
            sockets.enter_context(probe.connect())
            wait_for(lambda: probe.metrics()["minitun_pending_handshakes"] == 1)
        assert "TLS handshake failed" not in (root / "shutdown.log").read_text()

    with running(root, "post-tls", "--max-pending-handshakes", "4",
                 "--max-pending-handshakes-per-ip", "2") as probe:
        with contextlib.ExitStack() as sockets:
            for _ in range(2):
                raw = sockets.enter_context(probe.connect())
                sockets.enter_context(context.wrap_socket(raw, server_hostname="localhost"))
            wait_for(lambda: probe.metrics()["minitun_pending_handshakes"] == 2)
            with probe.connect() as extra:
                extra.sendall(b"GET / HTTP/1.0\r\n\r\n")
                wait_for(lambda: probe.metrics()["minitun_tls_admission_rejections_total"] == 1)
            # Completing TLS alone does not release the quota. The deadline
            # releases stalled application handshakes without another TLS error.
            wait_for(lambda: probe.metrics()["minitun_pending_handshakes"] == 0)
            assert probe.metrics()["minitun_tls_handshake_failures_total"] == 0
        subprocess.run(probe.client_command("--heartbeat-count", "1"), check=True, timeout=5)


def absolute_deadline(root, context):
    with running(root, "deadline") as probe:
        with probe.connect() as raw:
            wait_for(lambda: probe.metrics()["minitun_pending_handshakes"] == 1)
            started = time.monotonic()
            time.sleep(1.2)
            with context.wrap_socket(raw, server_hostname="localhost") as stream:
                stream.settimeout(3)
                assert stream.recv(1) == b""
            # TLS success must not reset the two-second authentication budget.
            assert time.monotonic() - started < 2.8
        wait_for(lambda: probe.metrics()["minitun_pending_handshakes"] == 0)


def flood(root, context, malformed):
    name = "malformed-flood" if malformed else "valid-flood"
    cpu_before = resource.getrusage(resource.RUSAGE_CHILDREN)
    companion = None
    try:
        with running(root, name, "--max-pending-handshakes", "8",
                     "--max-pending-handshakes-per-ip", "4",
                     "--max-handshakes-per-second", "20") as probe:
            companion = subprocess.Popen(probe.client_command("--expect-goaway"))
            wait_for(lambda: probe.metrics()['minitun_authentication_total{result="success"}'] == 1)
            wait_for(lambda: probe.metrics()["minitun_pending_handshakes"] == 0)
            started = time.monotonic()
            deadline = started + 2.5

            def sender():
                while time.monotonic() < deadline:
                    try:
                        with probe.connect() as raw:
                            # Eight senders at 20 accepts/s may wait 400ms in
                            # the backlog. Do not turn pacing into TLS aborts.
                            raw.settimeout(1)
                            if malformed:
                                raw.sendall(b"GET / HTTP/1.0\r\n\r\n")
                                raw.recv(1)
                            else:
                                with context.wrap_socket(raw, server_hostname="localhost"):
                                    pass
                    except OSError:
                        pass

            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                jobs = [pool.submit(sender) for _ in range(8)]
                # Metrics requests also prove the event loop remains responsive
                # while the unauthenticated connection budget is being consumed.
                while time.monotonic() < deadline:
                    assert probe.metrics()["minitun_pending_handshakes"] <= 4
                    time.sleep(0.1)
                for job in jobs:
                    job.result()
            elapsed = time.monotonic() - started
            stats = probe.metrics()
            accepted = stats["minitun_connections_total"]
            assert 25 <= accepted <= math.ceil(20 * (1 + elapsed)) + 2, stats
            assert companion.poll() is None, "existing authenticated session lost heartbeat"
            if malformed:
                # Concurrent attempts can finish after the fifth failure, but
                # subsequent accepts from the blocked IP never reach TLS.
                assert 5 <= stats["minitun_tls_handshake_failures_total"] <= 8, stats
                assert stats["minitun_tls_admission_rejections_total"] > 10, stats
            else:
                assert stats["minitun_tls_handshake_failures_total"] < 5, stats
            print(f"{name}: {int(accepted)} accepts in {elapsed:.2f}s; "
                  f"{int(stats['minitun_tls_handshake_failures_total'])} TLS failures")
        assert companion.wait(timeout=3) == 0, "authenticated session failed graceful shutdown"
        cpu_after = resource.getrusage(resource.RUSAGE_CHILDREN)
        cpu_seconds = (cpu_after.ru_utime + cpu_after.ru_stime
                       - cpu_before.ru_utime - cpu_before.ru_stime)
        print(f"{name}: server + authenticated client CPU {cpu_seconds:.3f}s")
        warnings = (root / f"{name}.log").read_text().count("TLS handshake failed")
        assert warnings <= 2, f"TLS warning flood: {warnings} log records"
    finally:
        if companion is not None and companion.poll() is None:
            companion.kill()
            companion.wait()


def main():
    with tempfile.TemporaryDirectory(prefix="minitun-tls-admission-") as directory:
        root = pathlib.Path(directory)
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-sha256", "-days", "1",
            "-nodes", "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
            "-keyout", str(root / "server.key"), "-out", str(root / "server.crt"),
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        (root / "server.key").chmod(0o600)
        (root / "token").write_text("tls-admission-test-token\n")
        (root / "token").chmod(0o600)
        (root / "clients.json").write_text(json.dumps({"format_version": 1, "clients": [{
            "client_id": "client_00000000000000000000000000000001", "enabled": True,
            "psk_file": "token", "allowed_ports": ["1024-65535"], "max_tunnels": 4,
            "max_connections": 4, "max_idle_workers": 2,
        }]}))
        (root / "clients.json").chmod(0o640)
        context = ssl.create_default_context(cafile=str(root / "server.crt"))
        pending_limits(root, context)
        absolute_deadline(root, context)
        flood(root, context, malformed=True)
        # Cover full TLS 1.2 handshakes as well as the TLS 1.3 slow-client cases.
        context.maximum_version = ssl.TLSVersion.TLSv1_2
        flood(root, context, malformed=False)
    print("TLS admission integration passed")


if __name__ == "__main__":
    main()
