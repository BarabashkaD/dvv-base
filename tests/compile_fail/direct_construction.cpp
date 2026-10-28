// Compile-fail cases for the factory-only rule (spec §9.4). DVV_COMPILE_FAIL_CASE selects one:
//   0  control: creation through the authority compiles
//   1  constructing a CreationKey outside its authority
//   2  default-constructing a managed object
//   3  copying a managed object
#include <memory>
#include <utility>

#include "dvv/base/lifetime.h"
#include "dvv/base/testing/test_authority.h"

namespace {

class Widget final : public dvv::Managed<Widget> {
public:
    explicit Widget(dvv::CreationKey<dvv::test::TestAuthority> key) noexcept
        : Managed(std::move(key)) {}
};

}  // namespace

int main() {
    auto widget = dvv::test::TestAuthority::makeUnique<Widget>();
#if DVV_COMPILE_FAIL_CASE == 1
    dvv::CreationKey<dvv::test::TestAuthority> forged(
        dvv::LifetimeRegistry::instance().issue<Widget>());
    Widget direct(std::move(forged));
#elif DVV_COMPILE_FAIL_CASE == 2
    Widget direct;
#elif DVV_COMPILE_FAIL_CASE == 3
    Widget copy(*widget);
#endif
    return widget->objectId() != dvv::NO_OWNER ? 0 : 1;
}
