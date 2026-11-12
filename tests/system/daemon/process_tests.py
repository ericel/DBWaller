"""Local TLS fixtures and process lifecycle tests; no service credentials/network required."""
import hashlib
import json
import os
from pathlib import Path
import selectors
import subprocess
import sys


def fixtures(directory, openssl):
    directory.mkdir(parents=True, exist_ok=True)
    os.chmod(directory, 0o700)
    subprocess.run([
        openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2",
        "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
        "-keyout", str(directory / "server.key"), "-out", str(directory / "server.crt"),
    ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=20)
    os.chmod(directory / "server.key", 0o600)
    token = "a" * 64  # Deliberately synthetic, used only for isolated loopback tests.
    (directory / "client.token").write_text(token)
    os.chmod(directory / "client.token", 0o600)
    (directory / "credentials.json").write_text(json.dumps({"principals": [{
        "name": "probe", "token_sha256": hashlib.sha256(token.encode()).hexdigest(),
        "namespaces": ["public"], "write": True, "invalidate": True, "admin": True,
    }]}))
    (directory / "daemon.json").write_text(json.dumps({
        "listen": "127.0.0.1:0", "certificate_file": "server.crt", "private_key_file": "server.key",
        "credentials_file": "credentials.json", "rpc": {"shutdown_grace_ms": 1000},
    }))


def process(directory, server, probe):
    subprocess.run([server, "--config", str(directory / "daemon.json"), "--check-config"], check=True, timeout=15)
    for modes in [("seed", "read"), ("cold",)]:
        with (directory / "daemon-stderr.log").open("w") as log:
            child = subprocess.Popen([server, "--config", str(directory / "daemon.json")], stdout=subprocess.PIPE,
                                     stderr=log, text=True)
            try:
                with selectors.DefaultSelector() as selector:
                    selector.register(child.stdout, selectors.EVENT_READ)
                    if not selector.select(timeout=15):
                        raise RuntimeError("daemon readiness watchdog expired")
                    ready = child.stdout.readline().strip()
                prefix = "dbwallerd ready on "
                if not ready.startswith(prefix):
                    raise RuntimeError("daemon did not become ready")
                endpoint = ready[len(prefix):]
                for mode in modes:
                    subprocess.run([probe, endpoint, str(directory / "server.crt"),
                                    str(directory / "client.token"), mode], check=True, timeout=15)
                child.terminate()
                stdout, _ = child.communicate(timeout=15)
                if child.returncode != 0 or "dbwallerd stopped" not in stdout:
                    raise RuntimeError("SIGTERM did not shut down cleanly")
            finally:
                if child.poll() is None:
                    child.kill()
                    child.wait(timeout=5)
    print("TLS process test passed: separate clients, SIGTERM, cold restart")


if __name__ == "__main__":
    directory = Path(sys.argv[2])
    if sys.argv[1] == "fixtures":
        fixtures(directory, sys.argv[3])
    else:
        process(directory, sys.argv[3], sys.argv[4])
