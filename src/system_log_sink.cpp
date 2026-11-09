#include <climits>
#include <cstddef>
#include <string>
#include <utility>

#include "dvv/base/log.h"

#if defined(_WIN32)
// windows.h must come first.
// clang-format off
#include <windows.h>
// clang-format on
#elif defined(__APPLE__)
#include <os/log.h>
#else
#include <syslog.h>
#endif

namespace dvv {

#if !defined(_WIN32)
namespace {

int clampedSize(std::string_view line) noexcept {
    return static_cast<int>(line.size() > static_cast<std::size_t>(INT_MAX)
                                ? static_cast<std::size_t>(INT_MAX)
                                : line.size());
}

}  // namespace
#endif

SystemLogSink::SystemLogSink(std::string identity) : identity_(std::move(identity)) {
#if defined(__APPLE__)
    log_ = os_log_create(identity_.c_str(), "log");
#elif !defined(_WIN32)
    openlog(identity_.c_str(), LOG_PID, LOG_USER);
#endif
}

SystemLogSink::~SystemLogSink() {
#if !defined(__APPLE__) && !defined(_WIN32)
    closelog();
#endif
}

#if defined(_WIN32)

void SystemLogSink::write(const LogRecord& /*record*/, std::string_view line) {
    if (line.size() > static_cast<std::size_t>(INT_MAX)) {
        return;
    }
    std::wstring wide;
    if (!line.empty()) {
        const int length = static_cast<int>(line.size());
        const int size = MultiByteToWideChar(CP_UTF8, 0, line.data(), length, nullptr, 0);
        if (size <= 0) {
            return;
        }
        wide.assign(static_cast<std::size_t>(size), L'\0');
        if (MultiByteToWideChar(CP_UTF8, 0, line.data(), length, wide.data(), size) != size) {
            return;
        }
    }
    wide.push_back(L'\n');
    OutputDebugStringW(wide.c_str());
}

#elif defined(__APPLE__)

void SystemLogSink::write(const LogRecord& record, std::string_view line) {
    os_log_type_t type = OS_LOG_TYPE_DEBUG;
    switch (record.level) {
        case LogLevel::Error: type = OS_LOG_TYPE_ERROR; break;
        case LogLevel::Warn: type = OS_LOG_TYPE_DEFAULT; break;
        case LogLevel::Info: type = OS_LOG_TYPE_INFO; break;
        default: break;
    }
    // NOLINTBEGIN(bugprone-suspicious-stringview-data-usage,clang-diagnostic-unknown-attributes)
    os_log_with_type(static_cast<os_log_t>(log_), type, "%{public}.*s", clampedSize(line),
                     line.data());
    // NOLINTEND(bugprone-suspicious-stringview-data-usage,clang-diagnostic-unknown-attributes)
}

#else

void SystemLogSink::write(const LogRecord& record, std::string_view line) {
    int priority = LOG_DEBUG;
    switch (record.level) {
        case LogLevel::Error: priority = LOG_ERR; break;
        case LogLevel::Warn: priority = LOG_WARNING; break;
        case LogLevel::Info: priority = LOG_INFO; break;
        default: break;
    }
    // NOLINTBEGIN(bugprone-suspicious-stringview-data-usage)
    syslog(priority, "%.*s", clampedSize(line), line.data());
    // NOLINTEND(bugprone-suspicious-stringview-data-usage)
}

#endif

}  // namespace dvv
