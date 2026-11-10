# dvv-base

Foundation library of the dvv stack used by rawreel: errors (`Result`, `DVV_TRY`, `co_try`),
factory-only creation with a lifetime registry and leak check, coroutine tasks and executors,
logging, a JSON wrapper, a URI parser and per-user paths. C++23, no dependencies beyond the standard
library and nlohmann/json.

`dvv::getEnvironment` and `dvv::setEnvironment` take and return UTF-8 names and values; child
processes see the values they set. Invalid UTF-8, empty names and names with `=` are rejected.

## Logging

`DVV_LOG_TRACE`, `DVV_LOG_DEBUG`, `DVV_LOG_INFO`, `DVV_LOG_WARN` and `DVV_LOG_ERROR` take a
`std::format` string literal, checked at compile time, and arguments. The level gate comes first: a
call below the level evaluates none of its arguments. `DVV_LOG_TEXT(level, text)` logs text that is
not a format. A call whose whole format is a bare `"{}"` logs a warning once per call site; use
`DVV_LOG_TEXT` or pass real arguments.

```cpp
auto& log = dvv::Logger::instance();
log.addSink(std::make_shared<dvv::StderrSink>());
log.addSink(std::make_shared<dvv::SystemLogSink>("com.example.app"), dvv::LogLevel::Warn);
log.start();
DVV_LOG_INFO("opened {} in {} ms", path, elapsed);
// ... before the program exits:
log.stop();
```

### Sinks and levels

A sink implements `ILogSink` (`write`, and optionally `batchEnd` and `flush`). Shipped sinks:
`StderrSink`, `MemorySink`, `RotatingFileSink`, `SplitFileSink` and `SystemLogSink`.
`addSink(sink, level)` gives the sink its own minimum level; `setSinkLevel` changes it at any time.
`setLevel` sets the global minimum. `enabled(level)` is true when a call can reach a sink: the level is not `Off`, is at or
above `level()` and at or above the lowest sink level. Without sinks the gate is closed and every
call is a single atomic load.

`SystemLogSink(identity)` writes to the OS log: os_log on macOS, syslog on Linux and
`OutputDebugStringW` on Windows. On Linux `openlog` and `closelog` are process-global, so keep one
`SystemLogSink` per process.

### Marks

`Logger::mark(text)` delivers `text` as one record of level Info with `LogRecord::mark` set. It goes
to every sink whose level is not `Off`, whatever `level()` and the sinks' levels are, and does
nothing without sinks. Synchronous: in order with the other records, under the logger's lock.
Asynchronous: through the queue with the priority of Warn, so a full ring drops it only when it
would drop a warning (counted as a dropped Info message). `RotatingFileSink` and `SplitFileSink`
flush a mark at once; `SystemLogSink` writes it at the OS default level (`OS_LOG_TYPE_DEFAULT` on
macOS, `LOG_NOTICE` on Linux). In a crash snapshot a mark is flag bit 1 of its record.

`formatLogRecord(record)` gives `2026-10-20T12:34:56.789Z INFO  message [file.cpp:42]`;
`formatLogRecord(record, false)` leaves out the ` [file.cpp:42]` suffix.

### SplitFileSink

`SplitFileSink::open(folder, prefix, max_bytes, append_newest)` writes
`<folder>/<prefix>-YYYYMMDD-HHMMSS[-N].log`, named by the local time of the file's creation; a file
is created exclusively and a taken name moves on to `-2`, `-3`, .... With `append_newest` the sink
appends to the newest matching file below `max_bytes` (newest by timestamp, then N), otherwise it
creates one. A write that would pass `max_bytes` starts a new file first, unless the current file
holds no line yet, so a line is never divided. It never renames or deletes a file, and a name that
does not match the pattern is never touched. `setHeader` sets the first line of each new file;
`currentFile`, `totalBytes` and `splits` report its state. Warn and above and marks are flushed at
once; Info and below at `flush()`, `batchEnd()` or destruction. When creating a file or writing
fails, a note goes to stderr (never to the `Logger`); lines are dropped while no file is open, and a
new file is tried after a back-off of `max_bytes / 4` bytes.

### Synchronous and asynchronous mode

Until `start()` the logger is synchronous: a call formats the message and runs the sinks under the
logger's lock on the calling thread. `addSink` and `clearSinks` work only in this mode; while the
log thread runs they do nothing and log an Error line.

`start()` starts the log thread `dvv-log`. From then on a call encodes its arguments into a 4 MiB
lock-free ring and returns; the log thread formats the message and calls the sinks one at a time,
and calls `batchEnd()` after each pass. A second `start()` does nothing.

- `flush()` returns when everything logged before the call is in every sink and every sink's
  `flush()` has run. Called from a sink it returns at once. Without a log thread it flushes the
  sinks.
- `stop()` hands every queued message to the sinks, joins the log thread, flushes every sink and
  returns to synchronous mode. It always flushes, also without a log thread.
  `finishWithLeakCheck` calls it.

Deferred arguments: `bool`, `char`, integers of 1, 2, 4 and 8 bytes, `float`, `double`,
`long double`, strings (`std::string`, `std::string_view`, `const char*`, `char` arrays) and
pointers (`void*`, `nullptr`). They are copied at the call and formatted on the log thread. A call
with any other argument type (for example `std::filesystem::path`, an enum or a class with a
`std::formatter`) is formatted on the calling thread with `std::vformat` and queued as text, so the
result is the same and the call is slower.

A record is at most 256 KiB. A string that does not fit is cut on a UTF-8 boundary and ends with
`…[cut]`.

When the ring is full the call does not wait: the message is dropped and counted. Warn and Error can
fill the whole ring; Trace, Debug and Info only three quarters of it. The log thread reports the
count as one line, `N log messages dropped (X error, Y warn, ...)`.

### Crash snapshot

`Logger::writeCrashSnapshot(writer, context)` is for a crash handler. It passes `writer` the queued
records that the sinks may not have made durable yet, the dropped counts and the call sites those
records name. It is async-signal-safe: no allocation, no lock, no formatting. Only the first call
writes. Without `start()` it writes no records. `Logger::crashSnapshotCapacity()` is the most bytes
it writes, so a handler can reserve a buffer or a file range up front.

`dvv::decodeCrashSnapshot(bytes)` turns a snapshot back into lines, each followed by
` (from crash snapshot)`. It needs neither the program nor its symbols. It treats the bytes as
untrusted: a cut-short snapshot gives the lines before the cut, and its output is bounded (a message
over 1 MiB gives the format text and values instead; past about 4 x `crashSnapshotCapacity()` bytes
of text the decode stops with the line `decode stopped: output limit reached`). The format is in
the comment above `LOG_SNAPSHOT_SITES` in `dvv/base/log.h`; integers are in the byte order of the
writing machine.

### Rules

- Call `flush()` before unloading a library that logged: queued records refer to its format strings
  and file names.
- A sink must not block for long and must not call back into the `Logger` (no logging, `addSink`,
  `clearSinks`, `start`, `stop` or `flush`).
- Do not log from a signal handler. Use `writeCrashSnapshot`.
- Call `stop()` (or `finishWithLeakCheck`) on every exit path of a program that called `start()`.
- Do not call `fork()` while the log thread runs.

Cost of a call in asynchronous mode (Release, Apple Silicon, 10 hardware threads, one counting sink,
ns per call). Each round of 1500 calls per thread fits the ring and the log thread is drained
between rounds, so no message was dropped: every row measures queued calls. The call cost rises
with the number of threads logging at once, well before they outnumber the cores: they share one
ring and its atomics.

| Threads | Disabled level | `"{} {} {}"`, 3 integers | One 40-byte string |
|--------:|---------------:|-------------------------:|-------------------:|
| 1       | 2.8            | 28.3                     | 31.6               |
| 4       | 2.2            | 396.3                    | 537.4              |
| 16      | 1.8            | 2697.5                   | 2643.8             |

Build and run it with `cmake --build --preset ninja-macos-release --target dvv_base_log_bench`.

## Build

Builds and tests on Windows (Visual Studio 2026) and Linux (Clang 19 with libstdc++ 14) with
[dvv-build](https://github.com/BarabashkaD/dvv-build), which provides the presets, the shared CMake
modules, the third-party dependencies and the CI workflow; its README lists the prerequisites.
dvv-build is cloned next to dvv-base at the version of the `dvv-build` line in `dvv-deps.txt`:

```powershell
git clone https://github.com/BarabashkaD/dvv-base
git clone --branch v0.4.0 https://github.com/BarabashkaD/dvv-build
python -m pip install -r dvv-build/tools/requirements.txt
cmake -DREPO=dvv-base -P dvv-build/tools/fetch-deps.cmake
./dvv-build/bootstrap.ps1
cd dvv-base
cmake --workflow --preset vs2026
```

On Linux: `./dvv-build/bootstrap.sh`, then `cmake --workflow --preset ninja-clang`.
Applications use dvv-base as a git submodule in their `libs/` folder, next to dvv-build.

Licence: MIT.
