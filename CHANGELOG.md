# Changelog

All notable changes to dvv-base. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow semantic versioning with
the dvv rules: compatible = the same major and at least the minimum version, also for 0.x; breaking
changes only in a new major, after a deprecation. This file is the release notes (there are no
GitHub Releases).

## [0.1.0] - 2026-10-28

First versioned release.

### Added
- Errors: `dvv::Result`, `DVV_TRY` and `co_try`.
- Factory-only creation of managed objects (`CreationKey`) with a lifetime registry and a leak
  check.
- Coroutine tasks and executors.
- Logging, a JSON wrapper (nlohmann/json), a URI parser, per-user paths and UTF-8
  environment access.
- The CMake package `dvv_base` (`find_package(dvv_base 0.1)`, `SameMajorVersion`); it installs
  dvv-build's `DvvCompilerOptions.cmake` and `run-compile-fail.cmake` and points `DVV_CMAKE_DIR`
  to them.
