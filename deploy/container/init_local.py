"""Create private development TLS files for the Compose quickstart; never overwrite."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess


def create(directory):
    directory = directory.resolve()
    if os.getuid() == 0:
        raise RuntimeError("run this local quickstart as an ordinary user")
    if any(c in str(directory) for c in "\n\r'"):
        raise ValueError("directory cannot contain a newline or single quote")
    directory.mkdir(mode=0o700, exist_ok=False)
    server, client = directory / "server", directory / "client"
    server.mkdir(mode=0o700)
    client.mkdir(mode=0o700)
    # Use restrictive creation permissions, including while OpenSSL is writing.
    old_mask = os.umask(0o077)
    try:
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "7",
                        "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,DNS:daemon,IP:127.0.0.1",
                        "-keyout", str(server / "server.key"), "-out", str(server / "server.crt")],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)
        token, health = secrets.token_hex(32), secrets.token_hex(32)
        (client / "client.token").write_text(token)
        (server / "health.token").write_text(health)
        shutil.copyfile(server / "server.crt", client / "server.crt")
        (server / "credentials.json").write_text(json.dumps({"principals": [
            {"name": "local-client", "token_sha256": hashlib.sha256(token.encode()).hexdigest(),
             "namespaces": ["public"], "write": True, "invalidate": True, "admin": False},
            {"name": "local-health", "token_sha256": hashlib.sha256(health.encode()).hexdigest(),
             "namespaces": [], "write": False, "invalidate": False, "admin": True},
        ]}, indent=2) + "\n")
        (server / "daemon.json").write_text(json.dumps({
            "listen": "0.0.0.0:7447", "certificate_file": "server.crt", "private_key_file": "server.key",
            "credentials_file": "credentials.json",
            "cache": {"shards": 8, "value_bytes": 67108864, "entries": 10000},
            "rpc": {"workers": 2, "queue": 64, "active": 128, "connections": 32,
                    "transport_memory_bytes": 33554432, "shutdown_grace_ms": 5000},
        }, indent=2) + "\n")
        (directory / ".env").write_text(f"DBWALLER_LOCAL_DIR='{directory}'\nDBWALLER_UID={os.getuid()}\nDBWALLER_GID={os.getgid()}\n")
    finally:
        os.umask(old_mask)
    print(f"Local configuration created in {directory}; development certificate expires in 7 days.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    create(parser.parse_args().directory)
