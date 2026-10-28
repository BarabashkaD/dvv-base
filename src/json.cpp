#include "dvv/base/json.h"

namespace dvv::json {

Result<Value> parse(std::string_view text) {
    try {
        return Value::parse(text);
    } catch (const nlohmann::json::exception& error) {
        return makeError(Errc::Format, std::format("invalid JSON: {}", error.what()));
    }
}

std::string dump(const Value& value, int indent) {
    return value.dump(indent, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace dvv::json
