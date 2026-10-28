#pragma once

// Creation authority (spec §9.4). Tests only; product code must never include this header.

#include <memory>
#include <utility>

#include "dvv/base/lifetime.h"

namespace dvv::test {

class TestAuthority {
public:
    template <ManagedType T, class... Args>
    [[nodiscard]] static std::unique_ptr<T> makeUnique(Args&&... args) {
        return std::make_unique<T>(key<T>(), std::forward<Args>(args)...);
    }

    template <ManagedType T, class... Args>
    [[nodiscard]] static std::shared_ptr<T> makeShared(Args&&... args) {
        return std::make_shared<T>(key<T>(), std::forward<Args>(args)...);
    }

private:
    template <class T>
    [[nodiscard]] static CreationKey<TestAuthority> key() {
        return CreationKey<TestAuthority>(LifetimeRegistry::instance().issue<T>());
    }
};

}  // namespace dvv::test
