# syntax=docker/dockerfile:1
# Pin the multi-architecture Ubuntu index. Refresh deliberately with validation.
ARG UBUNTU_IMAGE=ubuntu:24.04@sha256:2198e4c65076e9cb0b3d695219df667c0e3b0072da64ca3c2648aca17dcecaf5
FROM ${UBUNTU_IMAGE} AS dependencies
ARG BUILD_JOBS=2
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build python3 python3-venv pkg-config \
      ca-certificates curl git perl && rm -rf /var/lib/apt/lists/*
RUN python3 -m venv /opt/conan && /opt/conan/bin/pip install --no-cache-dir 'conan==2.34.0'
ENV PATH="/opt/conan/bin:${PATH}"
WORKDIR /src
COPY conanfile.py VERSION ./
COPY deploy/container/conan.lock /src/deploy/container/conan.lock
RUN mkdir -p /build && conan profile detect --force && \
    conan install . --output-folder=/build --build=missing -r conancenter \
      --lockfile=deploy/container/conan.lock -s:a build_type=Release -s:a compiler.cppstd=20 \
      -o '&:with_daemon=True' -o '&:with_tests=False' \
      -o '&:with_benchmarks=False' -o '&:build_apps=False' \
      -c tools.cmake.cmaketoolchain:generator=Ninja -c tools.build:jobs=${BUILD_JOBS} \
      --format=json > /build/dependencies.json

FROM dependencies AS build
COPY . /src
RUN cmake -S /src -B /build/build/Release \
      -DCMAKE_TOOLCHAIN_FILE=/build/build/Release/generators/conan_toolchain.cmake \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/install && \
    cmake --build /build/build/Release --target dbwallerd -j ${BUILD_JOBS} && \
    cmake --install /build/build/Release
# Build the SDK as an independent installed package, then consume only its public API.
RUN conan create sdk/cpp --build=missing -r conancenter \
      --lockfile=deploy/container/conan.lock -s:a build_type=Release -s:a compiler.cppstd=20 \
      -c tools.cmake.cmaketoolchain:generator=Ninja -c tools.build:jobs=${BUILD_JOBS} && \
    conan install examples/remote_cpp --output-folder=/consumer --build=missing -r conancenter \
      --lockfile=deploy/container/conan.lock --lockfile-partial \
      -s:a build_type=Release -s:a compiler.cppstd=20 -c tools.cmake.cmaketoolchain:generator=Ninja && \
    cmake -S deploy/container/client -B /consumer/build/Release \
      -DCMAKE_TOOLCHAIN_FILE=/consumer/build/Release/generators/conan_toolchain.cmake \
      -DCMAKE_BUILD_TYPE=Release && \
    cmake --build /consumer/build/Release -j ${BUILD_JOBS}
RUN python3 deploy/container/stage_runtime.py /build/dependencies.json /install /consumer/build/Release /out

FROM ${UBUNTU_IMAGE} AS runtime-base
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates libstdc++6 \
      && rm -rf /var/lib/apt/lists/*
COPY --from=build /out/ /usr/local/
USER 65532:65532
WORKDIR /tmp

# This companion is for the public tutorial and container integration tests.
FROM runtime-base AS client
ENTRYPOINT ["/usr/local/bin/dbwaller_remote_example"]
CMD ["--mode", "disabled"]

FROM runtime-base AS daemon
ARG VERSION
ARG REVISION=unknown
RUN test -n "${VERSION}" && test "${VERSION}" = "$(cat /usr/local/share/dbwaller/VERSION)"
LABEL org.opencontainers.image.title="DBWaller daemon" \
      org.opencontainers.image.description="Experimental standalone cache; application data remains in its durable backend" \
      org.opencontainers.image.source="https://github.com/ericel/DBWaller" \
      org.opencontainers.image.licenses="MIT" \
      org.opencontainers.image.version=${VERSION} \
      org.opencontainers.image.revision=${REVISION}
EXPOSE 7447
STOPSIGNAL SIGTERM
# No default health credential is embedded. The Compose quickstart configures
# an authenticated readiness probe; process state remains the liveness signal.
ENTRYPOINT ["/usr/local/bin/dbwallerd"]
CMD ["--config", "/etc/dbwaller/daemon.json"]
