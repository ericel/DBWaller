# Standalone daemon container

The container runs the experimental DBWaller cache independently of an application,
with the native daemon's TLS, authorization, invalidation tickets, and cold-start
behavior. The embedded library remains separately usable. Applications keep durable
data in their backend and must tolerate cache outages.

Source installation requires no cloud account or private repository. Release
automation prepares an image archive and GHCR image for Linux amd64; these exist
only after a new release passes its workflow. Merging this change does not publish
them. The Dockerfile also supports native Linux arm64 builds; the initial release
workflow publishes amd64 only. Windows containers are unsupported.

## Build and run locally

Use Docker with Linux containers, Compose v2 or later, Python 3, and host OpenSSL.
Run the quickstart as an ordinary user. The first C++ dependency build can take
tens of minutes and needs substantially more memory/disk than the running daemon.
Subsequent builds reuse Docker layers. `BUILD_JOBS` defaults to 2.

From the repository root:

```sh
docker build -f deploy/container/Dockerfile --target daemon \
  --build-arg VERSION="$(cat VERSION)" --build-arg REVISION="$(git rev-parse HEAD)" \
  -t dbwaller-daemon:local .
docker build -f deploy/container/Dockerfile --target client -t dbwaller-client:local .

mkdir -p .local
python3 deploy/container/init_local.py .local/container
docker compose -p dbwaller-local --env-file .local/container/.env \
  -f deploy/container/compose.yaml up -d --wait
docker compose -p dbwaller-local --env-file .local/container/.env \
  -f deploy/container/compose.yaml run --rm client
```

The example should report `backend fetches: 1` for two reads, followed by
`Updated value read successfully` after changing its simulated backend and
invalidating the cache. It consumes the separately installed public C++ SDK.

The initializer creates a new private directory and never overwrites existing
credentials. It generates a seven-day development certificate for localhost and
the Compose service name, an ordinary client credential, and a separate admin
credential used for readiness. The client cannot read the server private key or
readiness token. Secrets are mounted as files, not environment variables or image
build arguments. Use operator-managed certificates and credentials in deployment.

Compose runs under the host user's non-root UID/GID so owner-only bind mounts
work on Linux. The standalone image defaults to UID/GID 65532. Other deployments
must make mounted files readable by their runtime UID; do not make keys
world-readable to work around ownership errors.

The daemon listens on `0.0.0.0:7447` inside its container. Compose publishes only
**`127.0.0.1:7447` on the host** and uses an isolated network without external
egress. Set `DBWALLER_PORT` if that host port is occupied. The host C++ example
can use `127.0.0.1:7447`, namespace `public`, and files under `.local/container/client/`.
The companion client disables inherited DNS search suffixes so Compose's `daemon`
name resolves locally, including on hosts whose search domains need external DNS.

## Runtime limits and health

The sample uses a 64 MiB value budget, 10,000 entries, a 32 MiB gRPC transport
budget, 512 MiB container memory, one CPU, and 128 processes/threads. These are
example limits, not throughput guarantees or a promise that every workload fits.
Keys, tags, indexes, tickets, connections, and allocator/runtime overhead also
consume memory. Tune cache budgets below the container limit using real workloads.

The root filesystem and configuration mounts are read-only; capabilities are
dropped, privilege escalation is disabled, temporary space is bounded, and logs
rotate. There is no cache data volume. Restart starts empty; Docker volumes and
VM disks do not preserve process RAM. Neither GCS nor Filestore is required.

Running process state is the liveness signal. Compose readiness calls the
authenticated `Health` RPC using `dbwaller-probe`, checking TLS, credentials,
protocol compatibility, and daemon readiness. Unhealthy status does not itself
restart a running container. `unless-stopped` restarts exited containers;
applications still need bounded backend fallback.

The daemon is PID 1 and receives SIGTERM directly. Its five-second shutdown grace
fits within Compose's fifteen-second stop budget. Included executables follow
the [TLS startup contract](remote-cpp.md#process-wide-tls-startup-contract).

## Operate, upgrade, and remove

Use the same project and environment file for each operation:

```sh
docker compose -p dbwaller-local --env-file .local/container/.env \
  -f deploy/container/compose.yaml ps
docker compose -p dbwaller-local --env-file .local/container/.env \
  -f deploy/container/compose.yaml logs --tail=100 daemon
docker compose -p dbwaller-local --env-file .local/container/.env \
  -f deploy/container/compose.yaml stop daemon
docker compose -p dbwaller-local --env-file .local/container/.env \
  -f deploy/container/compose.yaml run --rm --no-deps client
```

During the outage, the example reports two backend fetches and unavailable
invalidation. Failed invalidation needs reconciliation before trusting cache hits
again; fallback on one request cannot repair other readers. Use `up -d --wait`
to start again. Old tickets and cached entries do not survive restart.

For upgrades, save the current image digest, obtain the new version, set
`DBWALLER_IMAGE` to its digest reference, and run `up -d --wait`. Roll back by
selecting the previous digest and repeating that command. Each replacement may
interrupt requests and starts a cold cache. Check protocol/configuration
compatibility, and never give previously fetched values new load tickets.

Use `down` with the same flags to remove only this Compose project's containers
and network. Host configuration under `.local/` remains; delete it separately
only when no longer needed. Global Docker pruning is not an uninstall command.

## Release artifacts and provenance

Release requires existing library checks and the container smoke test. The tested
amd64 image is exported as `dbwaller-daemon-VERSION-linux-amd64.tar.gz`, with a
notices/provenance tarball and `SHA256SUMS`. The protected `release` job verifies
these checksums, loads that image, and pushes
`ghcr.io/OWNER/dbwaller-daemon:VERSION-linux-amd64`. GitHub release assets include
the archives, checksums, and registry digest. Publication does not rebuild the
image or publish a moving `latest` tag.

After downloading published assets, substitute the actual version:

```sh
sha256sum -c SHA256SUMS       # macOS: shasum -a 256 -c SHA256SUMS
docker load --input dbwaller-daemon-VERSION-linux-amd64.tar.gz
```

Set `DBWALLER_IMAGE` to the loaded tag or published registry digest. Loading an
archive supports offline operation; the daemon does not need registry access.
The first GHCR upload defaults to private. Before advertising anonymous pulls,
the maintainer must make the package public and verify that route. Public GitHub
release archives are an independent installation route. See
[GitHub's registry documentation](https://docs.github.com/en/packages/working-with-a-github-packages-registry/working-with-the-container-registry).

The runtime image includes no compiler, Conan cache, source checkout, or credentials.
`/usr/local/share/dbwaller/` contains DBWaller's license/version, wire schema,
dependency notices and identities, and the Conan recipe lock. The build rejects
unexpected shared libraries; only the Ubuntu C/C++ runtime is dynamic. OS package
notices remain in the base image. The MIT label describes first-party code;
dependencies retain their respective licenses.

Ubuntu's multi-architecture digest, Conan version, and Conan recipe revisions are
pinned. APT and Python tool dependencies still come from their repositories; this
is not a claim of byte-for-byte reproducible builds. Promoting the tested image
and recording its digest identifies the exact release artifact. Refresh the base
and lock deliberately, review dependency changes, and rerun library/container checks.
See [Docker's build guidance](https://docs.docker.com/build/building/best-practices/).

## Validation

```sh
python3 tests/container/smoke.py \
  --image dbwaller-daemon:local --client-image dbwaller-client:local
```

This creates fresh local credentials and an isolated Compose project. It checks
actual runtime restrictions, TLS authorization, separate-client sharing,
invalidation, SIGTERM, cold restart, outage fallback, disabled mode, and failure
on missing configuration. It removes only its own containers/network. Subprocess
watchdogs bound failures without tight latency assertions. CI uses the same test
on Linux amd64, then exports, checksums, and reloads the release archive without
publishing it. Registry publication occurs only in the release workflow.

Google Cloud installation, snapshots, replication, and Python/Node.js clients
remain later work in the [bridge plan](daemon-bridge-plan.md).
