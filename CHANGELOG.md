# Changelog

All notable changes to dvv-base. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow semantic versioning with
the dvv rules: compatible = the same major and at least the minimum version, also for 0.x; breaking
changes only in a new major, after a deprecation. This file is the release notes (there are no
GitHub Releases).

## [0.3.0] - 2026-11-09

### Added
- Asynchronous logging: `Logger::start()` starts the log thread; a call queues its message in a
  4 MiB ring and the sinks run on the log thread. `stop()` (always flushes) and `flush()`.
- Sink levels: `addSink(sink, level)` and `setSinkLevel`.
- `ILogSink::batchEnd()`, called after each pass of the log thread.
- `DVV_LOG_TEXT` for messages that are plain text.
- `SystemLogSink`: os_log on macOS, syslog on Linux, `OutputDebugStringW` on Windows.
- Crash snapshot: `Logger::writeCrashSnapshot` (async-signal-safe), `crashSnapshotCapacity` and
  `dvv::decodeCrashSnapshot`.
- A benchmark target, `dvv_base_log_bench` (not built by default).

### Changed
- The log macros encode bool, char, integer, floating-point, string and pointer arguments and format
  them later; other types are formatted on the calling thread.
- In asynchronous mode the sinks run on the log thread; `addSink` and `clearSinks` are ignored
  (with an Error line) while it runs.
- Long string arguments are cut on a UTF-8 boundary and end with `…[cut]`. When the ring is full,
  messages are dropped and counted.
- `finishWithLeakCheck` stops the log thread.
- A log call whose format is a bare `"{}"` warns once per call site.
- The gate is closed without sinks: `enabled()` is false and calls do nothing.

## [0.2.0] - 2026-11-07

macOS on Apple Silicon.

### Added
- `dvv::MoveOnlyFunction`: `std::move_only_function` where the standard library has it, an own
  implementation elsewhere (e.g. Apple libc++).
- macOS per-user paths (`~/Library/Application Support`, `~/Library/Caches`) and
  `executablePath()` on macOS.

### Changed
- The executor types take `dvv::MoveOnlyFunction`; source-compatible where
  `std::move_only_function` exists.

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
