"""Observe actual exit handlers in fresh processes, without a timing threshold."""
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="dbwaller-tls-policy-") as temporary:
    for mode, expected_cleanup in (("baseline", True), ("early", False), ("late", True)):
        marker = Path(temporary) / mode
        subprocess.run([sys.argv[1], mode, str(marker)], check=True, timeout=20)
        if marker.exists() != expected_cleanup:
            raise RuntimeError(f"unexpected OpenSSL cleanup policy for {mode}")
print("TLS policy passed: early initialization disables cleanup; late initialization cannot undo it")
