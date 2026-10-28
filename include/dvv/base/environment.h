#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace dvv {

// Reads an environment variable. Names are UTF-8 on every platform, and so are values on Windows.
// On Linux the value is returned as stored, which may not be valid UTF-8. On Windows the
// value comes from the process environment block (GetEnvironmentVariableW), which the CRT, child
// processes and the Vulkan loader share. nullopt when the variable is not set, when the name is
// empty, contains '=' or '\0' or is not valid UTF-8, or (Windows) when the value is not valid
// UTF-16 (an unpaired surrogate). Not synchronised with other threads that read or write the
// environment.
[[nodiscard]] std::optional<std::string> getEnvironment(std::string_view name);

// Sets the variable, or removes it when `value` is empty. On Windows it goes through the CRT's
// wide environment (_wputenv_s), which also updates the process environment block. False when the
// name is empty, contains '=' or '\0' or is not valid UTF-8, when the value contains '\0' or is not
// valid UTF-8, or when the platform call fails. On Windows, also false when the name or the value
// is 32767 UTF-16 units or longer. Not synchronised with other threads.
bool setEnvironment(std::string_view name, std::string_view value);

}  // namespace dvv
