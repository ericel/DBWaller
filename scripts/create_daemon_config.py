"""Create a fresh loopback TLS configuration. Never prints bearer tokens or overwrites files."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="new private directory for local development credentials")
    parser.add_argument("--openssl", default="openssl")
    args = parser.parse_args()
    directory = args.directory.resolve()
    # Exclusive creation avoids overwriting keys or silently rotating live credentials.
    directory.mkdir(mode=0o700, parents=False, exist_ok=False)
    os.chmod(directory, 0o700)
    subprocess.run([
        args.openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "7",
        "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
        "-keyout", str(directory / "server.key"), "-out", str(directory / "server.crt"),
    ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)
    token = secrets.token_hex(32)
    files = {
        "client.token": token,
        "credentials.json": json.dumps({"principals": [{
            "name": "local-development", "token_sha256": hashlib.sha256(token.encode()).hexdigest(),
            "namespaces": ["default"], "write": True, "invalidate": True, "admin": True,
        }]}, indent=2),
        "daemon.json": json.dumps({
            "listen": "127.0.0.1:7447", "certificate_file": "server.crt",
            "private_key_file": "server.key", "credentials_file": "credentials.json",
        }, indent=2),
    }
    for name, value in files.items():
        path = directory / name
        with path.open("x") as output:
            output.write(value)
        os.chmod(path, 0o600)
    os.chmod(directory / "server.key", 0o600)
    print(f"Configuration: {directory / 'daemon.json'}")
    print(f"Client trust: {directory / 'server.crt'}")
    print(f"Client token file: {directory / 'client.token'} (keep private)")
    print("Local-development certificate expires in 7 days; use your own certificates for deployment.")


if __name__ == "__main__":
    main()
