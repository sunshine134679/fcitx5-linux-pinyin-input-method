#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace modernime::core {

class InputState final {
public:
    bool append(std::string_view value);
    bool eraseLast();
    void clear();

    const std::string &text() const { return text_; }
    std::uint64_t generation() const { return generation_; }

private:
    std::string text_;
    std::uint64_t generation_ = 0;
};

} // namespace modernime::core
