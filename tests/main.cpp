#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "dvv/base/leak_check.h"
#include "dvv/base/lifetime.h"

int main(int argc, char** argv) {
    dvv::LifetimeRegistry::instance().setDetailed(true);
    doctest::Context context(argc, argv);
    const int result = context.run();
    if (context.shouldExit()) {
        return result;
    }
    return dvv::finishWithLeakCheck(result);
}
