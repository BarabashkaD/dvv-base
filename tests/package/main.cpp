#include <cstdio>

#include "dvv/base/error.h"

int main() {
    const dvv::Result<int> result = dvv::makeError(dvv::Errc::Format, "probe");
    if (result.has_value() || result.error().code != dvv::Errc::Format) {
        return 1;
    }
    std::puts("dvv_base package works");
    return 0;
}
