#include "modernime/ui/candidate_bar_layout.h"

#include <algorithm>

namespace modernime::ui {

CandidateBarMetrics CandidateBarMetrics::reference() {
    CandidateBarMetrics metrics;
    metrics.canvasWidth = 2172.0;
    metrics.canvasHeight = 724.0;
    metrics.panelX = 216.0;
    metrics.panelY = 330.0;
    metrics.panelWidth = 1778.0;
    metrics.panelHeight = 153.0;
    metrics.panelRadius = 42.0;
    metrics.borderWidth = 2.0;
    metrics.shadowRadius = 18.0;
    metrics.shadowOpacity = 0.12;
    metrics.horizontalPadding = 33.0;
    metrics.candidateAdvance = 200.0;
    metrics.candidateWidth = 172.0;
    metrics.candidateHeight = 95.0;
    metrics.selectedWidth = 180.0;
    metrics.selectedHeight = 95.0;
    metrics.selectedRadius = 48.0;
    metrics.preeditX = 247.0;
    metrics.preeditBaseline = 285.0;
    metrics.candidateBaseline = 421.0;
    metrics.fontFamily = "Noto Sans CJK SC";
    metrics.preeditFontSize = 56.0;
    metrics.candidateFontSize = 52.0;
    metrics.fontWeight = 400;
    return metrics;
}

std::size_t CandidateBarLayout::visibleItems(const core::CandidatePage &page) {
    return std::min<std::size_t>(9, page.items.size());
}

CandidateBarLayout CandidateBarLayout::measure(
    const core::CandidatePage &page, const CandidateBarMetrics &metrics) {
    CandidateBarLayout layout;
    layout.panel = {metrics.panelX, metrics.panelY, metrics.panelWidth,
                    metrics.panelHeight};
    layout.preeditX = metrics.preeditX;
    layout.preeditBaseline = metrics.preeditBaseline;
    layout.candidateBaseline = metrics.candidateBaseline;

    const auto count = visibleItems(page);
    layout.candidates.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto selected = index == page.cursor;
        const auto x = metrics.panelX + metrics.horizontalPadding +
                       static_cast<double>(index) * metrics.candidateAdvance;
        const auto y = metrics.panelY +
                       (metrics.panelHeight - metrics.candidateHeight) / 2.0;
        layout.candidates.push_back(
            {{x, y, metrics.candidateWidth, metrics.candidateHeight},
             std::to_string(index + 1) + "." + page.items[index].text, selected});
        if (selected) {
            layout.selectedPill = {x, y, metrics.selectedWidth,
                                   metrics.selectedHeight};
        }
    }
    return layout;
}

} // namespace modernime::ui
