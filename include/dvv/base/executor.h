#pragma once

#include "dvv/base/move_only_function.h"

namespace dvv {

// Runs work items. Implementations never run posted work inline in post().
class IExecutor {
public:
    virtual ~IExecutor() = default;

    // Queues work. The work must not throw. If post throws, the work was not queued and will never
    // run.
    virtual void post(MoveOnlyFunction<void()> work) = 0;
};

}  // namespace dvv
