#include "modernime/pinyin/recent_choice.h"
#include "modernime/core/candidate_model.h"

#include <algorithm>

namespace modernime::pinyin {

void RecentChoices::record(std::string_view input, std::string_view text,
                           std::string_view pinyin, bool partial) {
    // Bound bytes as well as counts, including callers outside the decoder.
    if (input.empty() || text.empty() || pinyin.empty() ||
        input.size() > 128 || text.size() > 256 || pinyin.size() > 128) return;
    auto it = std::find_if(groups_.begin(), groups_.end(), [&](const Group &g) {
        return g.input == input && g.partial == partial;
    });
    Group next{std::string(input), partial, {}};
    if (it != groups_.end()) {
        next = std::move(*it);
        groups_.erase(it);
    }
    next.choices.push_back(core::candidateOrderKey(text, pinyin));
    if (next.choices.size() > 8) next.choices.pop_front();
    groups_.push_back(std::move(next));
    if (groups_.size() > 64) groups_.pop_front();
}

double RecentChoices::bonus(std::string_view input, std::string_view text,
                            std::string_view pinyin, bool partial) const {
    const auto it = std::find_if(groups_.begin(), groups_.end(), [&](const Group &g) {
        return g.input == input && g.partial == partial;
    });
    if (it == groups_.end()) return 0.0;
    const auto identity = core::candidateOrderKey(text, pinyin);
    if (it->choices.empty() || it->choices.back() != identity) return 0.0;
    const auto count = static_cast<std::size_t>(std::count(it->choices.begin(), it->choices.end(), identity));
    std::size_t streak = 0;
    for (auto choice = it->choices.rbegin(); choice != it->choices.rend() && *choice == identity; ++choice) ++streak;
    // Keep the established three-consecutive-corrections behavior even when
    // the preceding word still has a majority in the bounded window.
    if (count < 3 || (count * 2 <= it->choices.size() && streak < 3)) return 0.0;
    return 24.0 * static_cast<double>(count - 2); // at most 144 in an eight-choice window
}
} // namespace modernime::pinyin
