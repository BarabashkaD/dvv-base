#include <chrono>
#include <cstdio>
#include <string_view>

#include "dvv/base/error.h"
#include "dvv/base/local_time.h"
#include "dvv/base/log.h"
#include "dvv/base/utf8.h"

const char* osProbe();

int main() {
    const dvv::Result<int> result = dvv::makeError(dvv::Errc::Format, "probe");
    if (result.has_value() || result.error().code != dvv::Errc::Format) {
        return 1;
    }
    const std::string_view os = osProbe();
    if (os != "posix" && os != "win32") {
        return 1;
    }
    if (dvv::pathText(dvv::pathFromText("a")) != "a") {
        return 1;
    }
    if (!dvv::localTime(std::chrono::system_clock::from_time_t(0)).has_value()) {
        return 1;
    }
    if (!dvv::parseSplitLogName("p-20261110-153126.log", "p").has_value()) {
        return 1;
    }
    std::puts("dvv_base package works");
    return 0;
}
