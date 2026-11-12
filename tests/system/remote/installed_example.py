"""Exercise a separately built SDK consumer against a local daemon and an outage."""
import argparse
from pathlib import Path
import selectors
import subprocess
import tempfile
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "daemon"))
from process_tests import fixtures


def run(args):
    with tempfile.TemporaryDirectory(prefix="dbwaller-sdk-") as temporary:
        directory = Path(temporary)
        fixtures(directory, args.openssl)
        base = [args.example, "--mode", "remote", "--namespace", "public",
                "--ca", str(directory / "server.crt"), "--token-file", str(directory / "client.token")]
        with (directory / "stderr.log").open("w") as log:
            child = subprocess.Popen([args.daemon, "--config", str(directory / "daemon.json")],
                                     stdout=subprocess.PIPE, stderr=log, text=True)
            try:
                with selectors.DefaultSelector() as selector:
                    selector.register(child.stdout, selectors.EVENT_READ)
                    if not selector.select(timeout=20):
                        raise RuntimeError("daemon readiness watchdog expired")
                    ready = child.stdout.readline().strip()
                prefix = "dbwallerd ready on "
                if not ready.startswith(prefix):
                    raise RuntimeError("daemon did not become ready")
                endpoint = ready[len(prefix):]
                online = subprocess.run(base + ["--endpoint", endpoint], check=True, text=True,
                                        capture_output=True, timeout=20).stdout
                if "backend fetches: 1" not in online or "Updated value read successfully" not in online:
                    raise RuntimeError("installed consumer did not reuse and invalidate cached data")
                child.terminate()
                stdout, _ = child.communicate(timeout=20)
                if child.returncode != 0 or "dbwallerd stopped" not in stdout:
                    raise RuntimeError("daemon shutdown failed")
                offline = subprocess.run(base + ["--endpoint", endpoint], check=True, text=True,
                                         capture_output=True, timeout=20).stdout
                if "backend fetches: 2" not in offline or "Invalidation unavailable" not in offline:
                    raise RuntimeError("installed consumer did not fall back during an outage")
            finally:
                if child.poll() is None:
                    child.kill()
                    child.wait(timeout=5)
        disabled = subprocess.run([args.example, "--mode", "disabled"], check=True, text=True,
                                  capture_output=True, timeout=20).stdout
        if "backend fetches: 2" not in disabled:
            raise RuntimeError("disabled mode unexpectedly cached a value")
    print("Installed SDK example passed: TLS sharing, invalidation, outage fallback, disabled mode")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--daemon", required=True)
    parser.add_argument("--example", required=True)
    parser.add_argument("--openssl", default="openssl")
    run(parser.parse_args())
