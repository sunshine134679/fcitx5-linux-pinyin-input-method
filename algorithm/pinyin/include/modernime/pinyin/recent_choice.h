#pragma once

#include <deque>
#include <string>
#include <string_view>

namespace modernime::pinyin {

// Per-Provider selection feedback. No disk, shared resources, surrounding
// text, or implicit learning events. Only confirmed choices enter the window.
class RecentChoices final {
public:
    void record(std::string_view input, std::string_view text,
                std::string_view pinyin, bool partial);
    double bonus(std::string_view input, std::string_view text,
                 std::string_view pinyin, bool partial) const;
    void clear() { groups_.clear(); }
private:
    struct Group {
        std::string input;
        bool partial;
        std::deque<std::string> choices;
    };
    std::deque<Group> groups_;
};
} // namespace modernime::pinyin
