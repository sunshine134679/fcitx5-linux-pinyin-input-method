#include "modernime/core/input_state.h"

#include <algorithm>

namespace modernime::core {

bool InputState::append(std::string_view value) {
    if (value.empty() || std::any_of(value.begin(), value.end(), [](char byte) {
            return static_cast<unsigned char>(byte) >= 0x80;
        })) {
        return false;
    }
    text_.append(value);
    generation_ += static_cast<std::uint64_t>(value.size());
    return true;
}

bool InputState::eraseLast() {
    if (text_.empty()) {
        return false;
    }
    text_.pop_back();
    ++generation_;
    return true;
}

void InputState::clear() {
    text_.clear();
    ++generation_;
}

} // namespace modernime::core
