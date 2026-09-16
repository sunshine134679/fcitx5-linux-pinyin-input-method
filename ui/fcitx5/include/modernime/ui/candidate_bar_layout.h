#pragma once

#include "modernime/core/candidate_model.h"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace modernime::ui {

struct Rect final {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

struct CandidateBarMetrics final {
    double canvasWidth = 0.0;
    double canvasHeight = 0.0;
    double panelX = 0.0;
    double panelY = 0.0;
    double panelWidth = 0.0;
    double panelHeight = 0.0;
    double panelRadius = 0.0;
    double borderWidth = 0.0;
    double shadowRadius = 0.0;
    double shadowOpacity = 0.0;
    double horizontalPadding = 0.0;
    double candidateTextPadding = 0.0;
    double selectedTextPadding = 0.0;
    double candidateGap = 0.0;
    double candidateAdvance = 0.0;
    double candidateWidth = 0.0;
    double candidateHeight = 0.0;
    double selectedWidth = 0.0;
    double selectedHeight = 0.0;
    double selectedRadius = 0.0;
    double preeditX = 0.0;
    double preeditBaseline = 0.0;
    double candidateBaseline = 0.0;
    std::string fontFamily;
    double preeditFontSize = 0.0;
    double candidateFontSize = 0.0;
    int fontWeight = 0;

    double clipboardPanelRadius = 0.0;
    double clipboardRowHeight = 0.0;
    double clipboardSelectedHeight = 0.0;
    double clipboardHorizontalPadding = 0.0;
    double clipboardVerticalPadding = 0.0;
    double clipboardTextPadding = 0.0;
    double clipboardSubmitIconWidth = 0.0;
    double clipboardSubmitIconHeight = 0.0;
    double clipboardSubmitIconGap = 0.0;
    double clipboardSeparatorHeight = 0.0;
    std::size_t maxCandidates = 9;

    static CandidateBarMetrics reference(double fontSize = 20.0,
                                         std::size_t maxCandidates = 9);
};

std::function<double(std::string_view)> candidateTextWidthForMode(
    core::CandidatePageMode mode,
    std::function<double(std::string_view)> pinyinTextWidth,
    std::function<double(std::string_view)> clipboardTextWidth);

struct CandidateGeometry final {
    Rect bounds;
    std::string displayText;
    bool selected = false;
};

struct CandidateBarLayout final {
    Rect panel;
    Rect selectedPill;
    Rect submitIcon;
    std::string preedit;
    double preeditX = 0.0;
    double preeditBaseline = 0.0;
    double candidateBaseline = 0.0;
    double clipboardTextPadding = 0.0;
    bool clipboardMode = false;
    std::vector<Rect> separators;
    std::vector<CandidateGeometry> candidates;

    static CandidateBarLayout measure(const core::CandidatePage &page,
                                      const CandidateBarMetrics &metrics);
    static CandidateBarLayout measure(
        const core::CandidatePage &page, const CandidateBarMetrics &metrics,
        const std::function<double(std::string_view)> &textWidth);
    static std::size_t visibleItems(const core::CandidatePage &page, const CandidateBarMetrics *metrics = nullptr);
};

} // namespace modernime::ui
