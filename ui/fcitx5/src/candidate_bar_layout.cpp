#include "modernime/ui/candidate_bar_layout.h"

#include <algorithm>

namespace modernime::ui {

CandidateBarMetrics CandidateBarMetrics::reference() {
    CandidateBarMetrics metrics;
    metrics.canvasWidth = 360.0;
    metrics.canvasHeight = 62.0;
    metrics.panelX = 2.0;
    metrics.panelY = 2.0;
    metrics.panelWidth = 356.0;
    metrics.panelHeight = 54.0;
    metrics.panelRadius = 14.0;
    metrics.borderWidth = 1.0;
    metrics.shadowRadius = 4.0;
    metrics.shadowOpacity = 0.12;
    metrics.horizontalPadding = 8.0;
    metrics.candidateTextPadding = 0.0;
    metrics.selectedTextPadding = 6.0;
    metrics.candidateGap = 2.0;
    metrics.candidateAdvance = 38.0;
    metrics.candidateWidth = 34.0;
    metrics.candidateHeight = 38.0;
    metrics.selectedWidth = 36.0;
    metrics.selectedHeight = 38.0;
    metrics.selectedRadius = 19.0;
    metrics.preeditX = 8.0;
    metrics.preeditBaseline = 0.0;
    metrics.candidateBaseline = 43.0;
    metrics.fontFamily = "Noto Sans CJK SC";
    metrics.preeditFontSize = 18.0;
    metrics.candidateFontSize = 20.0;
    metrics.fontWeight = 400;
    return metrics;
}

std::size_t CandidateBarLayout::visibleItems(const core::CandidatePage &page) {
    return std::min<std::size_t>(9, page.items.size());
}

CandidateBarLayout CandidateBarLayout::measure(
    const core::CandidatePage &page, const CandidateBarMetrics &metrics) {
    return measure(page, metrics, {});
}

CandidateBarLayout CandidateBarLayout::measure(
    const core::CandidatePage &page, const CandidateBarMetrics &metrics,
    const std::function<double(std::string_view)> &textWidth) {
    CandidateBarLayout layout;
    layout.panel = {metrics.panelX, metrics.panelY, metrics.panelWidth,
                    metrics.panelHeight};
    layout.preedit = page.preedit;
    layout.preeditX = metrics.preeditX;
    layout.preeditBaseline = metrics.preeditBaseline;
    layout.candidateBaseline = metrics.candidateBaseline;

    const auto count = visibleItems(page);
    layout.candidates.reserve(count);
    double nextX = metrics.panelX + metrics.horizontalPadding;
    const double rightEdge = metrics.panelX + metrics.panelWidth -
                             metrics.horizontalPadding;
    for (std::size_t index = 0; index < count; ++index) {
        const auto selected = index == page.cursor;
        const auto displayText =
            std::to_string(index + 1) + "." + page.items[index].text;
        double slotWidth = metrics.candidateWidth;
        double selectedWidth = metrics.selectedWidth;
        if (textWidth) {
            const auto measuredWidth = std::max(0.0, textWidth(displayText));
            slotWidth = std::max(
                slotWidth, measuredWidth + 2.0 * metrics.candidateTextPadding);
            selectedWidth = std::max(
                selectedWidth, measuredWidth + 2.0 * metrics.selectedTextPadding);
        }
        const auto x = nextX;
        const auto occupiedWidth =
            selected ? std::max(slotWidth, selectedWidth) : slotWidth;
        if (x + occupiedWidth > rightEdge) {
            break;
        }
        const auto y = metrics.panelY +
                       (metrics.panelHeight - metrics.candidateHeight) / 2.0;
        layout.candidates.push_back(
            {{x, y, slotWidth, metrics.candidateHeight}, displayText, selected});
        if (selected) {
            layout.selectedPill = {x, y, selectedWidth, metrics.selectedHeight};
        }
        const auto advance = textWidth
                                 ? occupiedWidth + metrics.candidateGap
                                 : std::max(metrics.candidateAdvance,
                                            occupiedWidth + metrics.candidateGap);
        nextX = x + advance;
    }
    return layout;
}

} // namespace modernime::ui
