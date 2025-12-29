# Contributing to DBWaller

Thanks for your interest in contributing!

## Project Status

DBWaller is an experimental systems project exploring:
- in-process encrypted caching
- request coalescing (single-flight)
- batching / fan-out acceleration
- execution boundaries (embedded vs daemon vs shared memory)

Expect breaking changes as APIs stabilize.

## How to Build

### CMake (minimal)
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DDBWALLER_BUILD_TESTS=OFF -DDBWALLER_BUILD_BENCHMARKS=OFF
cmake --build build -j
