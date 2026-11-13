"""Stage only runtime executables, public metadata and third-party license files."""
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

graph_path, install, consumer, output = map(Path, sys.argv[1:])
(output / "bin").mkdir(parents=True)
share = output / "share" / "dbwaller"
share.mkdir(parents=True)
for binary in (install / "bin/dbwallerd", consumer / "dbwaller_remote_example", consumer / "dbwaller-probe"):
    linked = subprocess.run(["ldd", str(binary)], check=True, text=True, capture_output=True).stdout
    # All Conan libraries must be static. Only the base OS C/C++ runtime is dynamic.
    allowed = {"libstdc++.so.6", "libgcc_s.so.1", "libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1"}
    for line in linked.splitlines():
        if "not found" in line:
            raise RuntimeError("missing runtime library")
        if "=>" in line and line.split()[0] not in allowed:
            raise RuntimeError(f"unexpected dynamic dependency: {line.split()[0]}")
    shutil.copy2(binary, output / "bin" / binary.name)
    subprocess.run(["strip", str(output / "bin" / binary.name)], check=True)

for name in ("LICENSE", "VERSION"):
    shutil.copy2(Path("/src") / name, share / name)
shutil.copytree(install / "share/DBWaller/proto", share / "proto")
shutil.copy2(Path("/src/deploy/container/conan.lock"), share / "conan.lock")
nodes = json.loads(graph_path.read_text())["graph"]["nodes"].values()
manifest = []
for node in nodes:
    ref = node.get("ref")
    if not ref or node.get("context") != "host" or node.get("recipe") == "Consumer":
        continue
    package = node.get("package_folder")
    if not package:
        raise RuntimeError(f"missing dependency package: {ref}")
    licenses = Path(package) / "licenses"
    if not licenses.is_dir() or not any(licenses.rglob("*")):
        raise RuntimeError(f"missing dependency notices: {ref}")
    name = ref.split("/", 1)[0]
    if not re.fullmatch(r"[a-zA-Z0-9_.-]+", name):
        raise RuntimeError("invalid dependency name")
    shutil.copytree(licenses, share / "licenses" / name)
    manifest.append({key: node.get(key) for key in ("ref", "package_id", "prev", "settings", "options")})
(share / "dependencies.json").write_text(json.dumps(manifest, indent=2) + "\n")

