#include "dvv/base/leak_check.h"

#include <cstdio>
#include <format>

#include "dvv/base/environment.h"
#include "dvv/base/error.h"
#include "dvv/base/log.h"

namespace dvv {

LeakPolicy defaultLeakPolicy() {
    if (const auto value = getEnvironment("DVV_LEAK_CHECK")) {
        if (*value == "fail") {
            return LeakPolicy::Fail;
        }
        if (*value == "warn") {
            return LeakPolicy::Warn;
        }
    }
#if defined(NDEBUG)
    return LeakPolicy::Warn;
#else
    return LeakPolicy::Fail;
#endif
}

std::string formatLeakReport(std::span<const LiveObjectInfo> leaks) {
    std::string report = std::format("{} managed object(s) still alive at exit:\n", leaks.size());
    for (const LiveObjectInfo& leak : leaks) {
        report += std::format("  #{} {} (owner #{})", leak.id, leak.type, leak.owner);
        if (leak.createdAt) {
            report += std::format(" created at {}:{}", sourceFileName(*leak.createdAt),
                                  leak.createdAt->line());
        }
        report += '\n';
    }
    return report;
}

int finishWithLeakCheck(int exit_code, LeakPolicy policy) {
    Logger::instance().flush();
    const auto leaks = LifetimeRegistry::instance().liveObjects();
    if (leaks.empty()) {
        return exit_code;
    }
    std::fputs(formatLeakReport(leaks).c_str(), stderr);
    if (policy == LeakPolicy::Warn) {
        return exit_code;
    }
    return exit_code != 0 ? exit_code : 1;
}

}  // namespace dvv
