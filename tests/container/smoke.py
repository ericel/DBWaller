"""Test the actual daemon/client images and documented Compose deployment locally."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import uuid

ROOT = Path(__file__).resolve().parents[2]


def run(command, *, env=None, success=True, timeout=120):
    result = subprocess.run(command, env=env, text=True, capture_output=True, timeout=timeout)
    if (result.returncode == 0) != success:
        raise RuntimeError(f"Unexpected exit {result.returncode}: {' '.join(command)}\n{result.stdout}\n{result.stderr}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", default="dbwaller-daemon:local")
    parser.add_argument("--client-image", default="dbwaller-client:local")
    args = parser.parse_args()
    project = "dbwaller-test-" + uuid.uuid4().hex[:12]
    local = ROOT / ".local"
    local.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="container-test-", dir=local) as temporary:
        config = Path(temporary) / "config"
        init = [sys.executable, str(ROOT / "deploy/container/init_local.py"), str(config)]
        run(init)
        key = (config / "server/server.key").read_bytes()
        run(init, success=False)
        if key != (config / "server/server.key").read_bytes():
            raise RuntimeError("initialization overwrote existing credentials")
        for path in config.rglob("*"):
            if path.stat().st_mode & 0o077:
                raise RuntimeError("development credentials have group/other permissions")
        env = dict(os.environ, DBWALLER_IMAGE=args.image, DBWALLER_CLIENT_IMAGE=args.client_image,
                   DBWALLER_PORT="0")
        compose = ["docker", "compose", "--project-name", project, "--env-file", str(config / ".env"),
                   "-f", str(ROOT / "deploy/container/compose.yaml")]
        client = compose + ["run", "--rm", "--no-deps", "client"]
        probe = compose + ["run", "--rm", "--no-deps", "--entrypoint", "/usr/local/bin/dbwaller-probe", "client"]

        def check(mode, success=True):
            return run(probe + [mode, "daemon:7447", "/etc/dbwaller-client/server.crt",
                                "/etc/dbwaller-client/client.token", "public"], env=env, success=success)

        try:
            run(compose + ["up", "-d", "--wait", "--wait-timeout", "90", "daemon"], env=env)
            container = run(compose + ["ps", "-q", "daemon"], env=env).strip()
            info = json.loads(run(["docker", "inspect", container]))[0]
            host = info["HostConfig"]
            if (info["Config"]["User"].split(":")[0] in ("", "0", "root") or
                    not host["ReadonlyRootfs"] or not host["Memory"] or not host["PidsLimit"] or
                    "ALL" not in host["CapDrop"] or
                    not any(x.startswith("no-new-privileges") for x in host["SecurityOpt"])):
                raise RuntimeError("container isolation/resource limits are missing")
            if any(binding["HostIp"] != "127.0.0.1" for bindings in host["PortBindings"].values() for binding in bindings):
                raise RuntimeError("quickstart port is not bound to localhost")
            online = run(client, env=env)
            if "backend fetches: 1" not in online or "Updated value read successfully" not in online:
                raise RuntimeError("packaged SDK did not share and invalidate cache values")
            check("seed")
            check("read")  # Separate client process observes the previous publication.
            check("health", success=False)  # Ordinary client credentials are not admin credentials.
            token_path = config / "client/client.token"
            token = token_path.read_text()
            try:
                token_path.write_text("0" * 64)
                check("read", success=False)
            finally:
                token_path.write_text(token)
            ca_path = config / "client/server.crt"
            ca = ca_path.read_text()
            try:
                ca_path.write_text("")  # System trust must not accept the private development CA.
                check("read", success=False)
            finally:
                ca_path.write_text(ca)
            run(compose + ["stop", "daemon"], env=env)
            stopped = json.loads(run(["docker", "inspect", container]))[0]
            if stopped["State"]["ExitCode"] != 0 or stopped["State"]["OOMKilled"]:
                raise RuntimeError("daemon did not stop gracefully under its memory budget")
            logs = run(compose + ["logs", "--no-color", "daemon"], env=env)
            if "dbwallerd stopped" not in logs or token in logs or key.decode() in logs:
                raise RuntimeError("shutdown log missing or sensitive material logged")
            offline = run(client, env=env)
            if "backend fetches: 2" not in offline or "Invalidation unavailable" not in offline:
                raise RuntimeError("packaged example did not fall back during outage")
            run(compose + ["up", "-d", "--wait", "--wait-timeout", "90", "daemon"], env=env)
            check("cold")
            check("seed")
            check("read")
            run(compose + ["run", "--rm", "--no-deps", "daemon", "--config", "/etc/dbwaller/absent.json"],
                env=env, success=False)
            disabled = run(client + ["--mode", "disabled"], env=env)
            if "backend fetches: 2" not in disabled:
                raise RuntimeError("disabled mode did not bypass cache")
        finally:
            run(compose + ["down", "--remove-orphans", "--timeout", "15"], env=env)
    print("Container smoke passed: non-root/read-only/limits, TLS, auth, sharing, invalidation, SIGTERM, cold restart, outage fallback, disabled mode")


if __name__ == "__main__":
    main()
