#pragma once

#include <functional>

namespace dvv {

// Runs work items. Implementations never run posted work inline in post().
class IExecutor {
public:
    virtual ~IExecutor() = default;

    // Queues work. The work must not throw. If post throws, the work was not queued and will never
    // run.
    virtual void post(std::move_only_function<void()> work) = 0;
};

}  // namespace dvv
