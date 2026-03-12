# Changelog

All notable changes to this project will be documented in this file.

The format is based on Keep a Changelog, and DBWaller follows Semantic Versioning.

## [Unreleased]

### Added

- GitHub Actions workflows for pull request, release-tag, and nightly verification.
- Conan package install and export metadata plus a downstream `test_package/`.
- A documented CI and release roadmap for versioning and configuration control.

### Changed

- Project versioning now flows from the tracked `VERSION` file.
- Local and CI build instructions are aligned around Conan-managed dependencies and tracked CMake presets.
