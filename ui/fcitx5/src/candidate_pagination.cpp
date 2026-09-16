#include "modernime/ui/candidate_pagination.h"

#include "modernime/ui/candidate_bar_layout.h"

#include <algorithm>
#include <string>

namespace modernime::ui {
namespace {

constexpr std::size_t kMaximumNumberedCandidates = 9;

} // namespace

std::vector<core::PageBoundary> CandidatePagination::partition(
    const std::vector<core::CandidateItem> &items,
    const CandidateBarMetrics &metrics,
    const std::function<double(std::string_view)> &textWidth) {
    std::vector<core::PageBoundary> boundaries;
    const auto availableWidth = std::max(
        0.0, metrics.panelWidth - 2.0 * metrics.horizontalPadding);

    for (std::size_t begin = 0; begin < items.size();) {
        auto end = begin;
        double baseWidth = 0.0;
        double maximumSelectedExtra = 0.0;
        const auto maxNumbered = metrics.maxCandidates > 0
                                     ? metrics.maxCandidates
                                     : kMaximumNumberedCandidates;
        while (end < items.size() &&
               end - begin < maxNumbered) {
            const auto localIndex = end - begin;
            const auto displayText = std::to_string(localIndex + 1) + "." +
                                     items[end].text;
            const auto measuredWidth =
                textWidth ? std::max(0.0, textWidth(displayText)) : 0.0;
            const auto itemWidth = std::max(
                metrics.candidateWidth,
                measuredWidth + 2.0 * metrics.candidateTextPadding);
            const auto selectedWidth = std::max(
                metrics.selectedWidth,
                measuredWidth + 2.0 * metrics.selectedTextPadding);
            const auto nextBaseWidth =
                baseWidth +
                (localIndex == 0 ? 0.0 : metrics.candidateGap) + itemWidth;
            const auto nextSelectedExtra = std::max(
                maximumSelectedExtra, selectedWidth - itemWidth);
            if (localIndex > 0 &&
                nextBaseWidth + nextSelectedExtra > availableWidth) {
                break;
            }
            baseWidth = nextBaseWidth;
            maximumSelectedExtra = nextSelectedExtra;
            ++end;
            if (localIndex == 0 &&
                baseWidth + maximumSelectedExtra > availableWidth) {
                break;
            }
        }
        boundaries.push_back({begin, end});
        begin = end;
    }
    return boundaries;
}

} // namespace modernime::ui
