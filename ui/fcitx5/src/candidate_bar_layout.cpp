#include "modernime/ui/candidate_bar_layout.h"

#include <algorithm>
#include <string>
#include <utility>

namespace modernime::ui {
namespace {

constexpr std::size_t kClipboardVisibleRows = 5;

std::size_t nextUtf8Boundary(std::string_view value, std::size_t offset) {
    if (offset >= value.size()) {
        return offset;
    }
    const auto first = static_cast<unsigned char>(value[offset]);
    std::size_t length = 1;
    if (first >= 0xc2 && first <= 0xdf) {
        length = 2;
    } else if (first >= 0xe0 && first <= 0xef) {
        length = 3;
    } else if (first >= 0xf0 && first <= 0xf4) {
        length = 4;
    }
    if (offset + length > value.size()) {
        return offset + 1;
    }
    for (std::size_t index = offset + 1; index < offset + length; ++index) {
        const auto continuation = static_cast<unsigned char>(value[index]);
        if ((continuation & 0xc0) != 0x80) {
            return offset + 1;
        }
    }
    return offset + length;
}

std::string singleLine(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    bool previousWasSpace = false;
    for (const char character : value) {
        const bool whitespace = character == '\n' || character == '\r' ||
                                character == '\t';
        if (whitespace) {
            if (!previousWasSpace) {
                result.push_back(' ');
            }
            previousWasSpace = true;
            continue;
        }
        result.push_back(character);
        previousWasSpace = character == ' ';
    }
    return result;
}

std::string ellipsize(std::string_view value, double maxWidth,
                      const std::function<double(std::string_view)> &textWidth) {
    const auto normalized = singleLine(value);
    if (!textWidth || textWidth(normalized) <= maxWidth) {
        return normalized;
    }
    constexpr std::string_view marker = "…";
    if (maxWidth <= 0.0 || textWidth(marker) > maxWidth) {
        return {};
    }

    std::string prefix;
    for (std::size_t offset = 0; offset < normalized.size();) {
        const auto next = nextUtf8Boundary(normalized, offset);
        const auto character = std::string_view(normalized).substr(offset,
                                                                       next - offset);
        std::string candidate = prefix;
        candidate.append(character);
        candidate.append(marker);
        if (textWidth(candidate) > maxWidth) {
            break;
        }
        prefix = std::move(candidate);
        prefix.erase(prefix.size() - marker.size());
        offset = next;
    }
    return prefix + std::string(marker);
}

CandidateBarLayout measureClipboard(
    const core::CandidatePage &page, const CandidateBarMetrics &metrics,
    const std::function<double(std::string_view)> &textWidth) {
    CandidateBarLayout layout;
    layout.clipboardMode = true;
    layout.clipboardTextPadding = metrics.clipboardTextPadding;
    layout.preedit = page.preedit;
    const auto count = std::min<std::size_t>(kClipboardVisibleRows,
                                             page.items.size());
    layout.panel = {
        metrics.panelX,
        metrics.panelY,
        metrics.panelWidth,
        2.0 * metrics.clipboardVerticalPadding +
            static_cast<double>(count) * metrics.clipboardRowHeight +
            static_cast<double>(count > 0 ? count - 1 : 0) *
                metrics.clipboardSeparatorHeight};
    layout.candidates.reserve(count);
    if (count == 0) {
        return layout;
    }

    const auto rowWidth = metrics.panelWidth -
                          2.0 * metrics.clipboardHorizontalPadding;
    for (std::size_t index = 0; index < count; ++index) {
        const auto slotY = metrics.panelY + metrics.clipboardVerticalPadding +
                           static_cast<double>(index) *
                               (metrics.clipboardRowHeight +
                                metrics.clipboardSeparatorHeight);
        const auto selected = index == page.cursor;
        const auto rowHeight = selected ? metrics.clipboardSelectedHeight
                                        : metrics.clipboardRowHeight;
        const Rect row{
            metrics.panelX + metrics.clipboardHorizontalPadding,
            slotY + (metrics.clipboardRowHeight - rowHeight) / 2.0,
            rowWidth,
            rowHeight};
        const auto rightReserved = selected
                                       ? metrics.clipboardSubmitIconWidth +
                                             metrics.clipboardSubmitIconGap
                                       : metrics.clipboardTextPadding;
        const auto maxTextWidth = row.width - metrics.clipboardTextPadding -
                                  rightReserved;
        layout.candidates.push_back(
            {row, ellipsize(page.items[index].text, maxTextWidth, textWidth),
             selected});
        if (selected) {
            layout.selectedPill = row;
            layout.submitIcon = {
                row.x + row.width - metrics.clipboardSubmitIconWidth -
                    metrics.clipboardTextPadding,
                row.y + (row.height - metrics.clipboardSubmitIconHeight) / 2.0,
                metrics.clipboardSubmitIconWidth,
                metrics.clipboardSubmitIconHeight};
        }
        if (index + 1 < count) {
            layout.separators.push_back(
                {metrics.panelX, slotY + metrics.clipboardRowHeight,
                 metrics.panelWidth, metrics.clipboardSeparatorHeight});
        }
    }
    return layout;
}

} // namespace

CandidateBarMetrics CandidateBarMetrics::reference() {
    CandidateBarMetrics metrics;
    metrics.canvasWidth = 614.0;
    metrics.canvasHeight = 62.0;
    metrics.panelX = 2.0;
    metrics.panelY = 2.0;
    metrics.panelWidth = 610.0;
    metrics.panelHeight = 54.0;
    metrics.panelRadius = 14.0;
    metrics.borderWidth = 1.0;
    metrics.shadowRadius = 4.0;
    metrics.shadowOpacity = 0.12;
    metrics.horizontalPadding = 8.0;
    metrics.candidateTextPadding = 0.0;
    metrics.selectedTextPadding = 8.0;
    metrics.candidateGap = 33.0;
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
    metrics.clipboardPanelRadius = 14.0;
    metrics.clipboardRowHeight = 56.0;
    metrics.clipboardSelectedHeight = 48.0;
    metrics.clipboardHorizontalPadding = 8.0;
    metrics.clipboardVerticalPadding = 8.0;
    metrics.clipboardTextPadding = 14.0;
    metrics.clipboardSubmitIconWidth = 32.0;
    metrics.clipboardSubmitIconHeight = 28.0;
    metrics.clipboardSubmitIconGap = 10.0;
    metrics.clipboardSeparatorHeight = 1.0;
    return metrics;
}

std::function<double(std::string_view)> candidateTextWidthForMode(
    core::CandidatePageMode mode,
    std::function<double(std::string_view)> pinyinTextWidth,
    std::function<double(std::string_view)> clipboardTextWidth) {
    return mode == core::CandidatePageMode::Clipboard
               ? std::move(clipboardTextWidth)
               : std::move(pinyinTextWidth);
}

std::size_t CandidateBarLayout::visibleItems(const core::CandidatePage &page) {
    const auto limit = page.mode == core::CandidatePageMode::Clipboard
                           ? kClipboardVisibleRows
                           : std::size_t{9};
    return std::min(limit, page.items.size());
}

CandidateBarLayout CandidateBarLayout::measure(
    const core::CandidatePage &page, const CandidateBarMetrics &metrics) {
    return measure(page, metrics, {});
}

CandidateBarLayout CandidateBarLayout::measure(
    const core::CandidatePage &page, const CandidateBarMetrics &metrics,
    const std::function<double(std::string_view)> &textWidth) {
    if (page.mode == core::CandidatePageMode::Clipboard) {
        return measureClipboard(page, metrics, textWidth);
    }

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
        const auto number = index + 1;
        const auto displayText =
            std::to_string(number) + "." + page.items[index].text;
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
