#include "modernime/ui/candidate_bar_renderer.h"

#include <algorithm>
#include <string_view>

namespace modernime::ui {

RenderStyle RenderStyle::reference() {
    RenderStyle style;
    style.shadow = {0.10, 0.14, 0.20, 0.12};
    style.panel = {1.0, 1.0, 1.0, 0.96};
    style.border = {0.88, 0.90, 0.93, 0.90};
    style.selected = {0.07, 0.40, 0.93, 1.0};
    style.text = {0.05, 0.09, 0.15, 1.0};
    style.selectedText = {1.0, 1.0, 1.0, 1.0};
    style.preedit = {0.27, 0.32, 0.40, 1.0};
    style.panelRadius = 9.0;
    style.selectedRadius = 6.0;
    style.shadowRadius = 6.0;
    style.shadowSpread = 2.0;
    style.shadowOffsetY = 4.0;
    style.borderWidth = 1.0;
    style.preeditText = {"Noto Sans CJK SC", 20.0, 400};
    style.candidateNumberText = {"Noto Sans CJK SC", 16.0, 400};
    style.candidateText = {"Noto Sans CJK SC", 19.0, 400};
    return style;
}

void CandidateBarRenderer::render(RenderSurface &surface,
                                  const CandidateBarLayout &layout,
                                  const RenderStyle &style) {
    const Rect shadowBounds{
        layout.panel.x - style.shadowSpread,
        layout.panel.y + style.shadowOffsetY,
        layout.panel.width + 2.0 * style.shadowSpread,
        layout.panel.height + style.shadowSpread};
    surface.shadowRoundedRect(shadowBounds,
                              style.panelRadius + style.shadowSpread,
                              style.shadow, style.shadowRadius);
    surface.roundedRect(layout.panel, style.panelRadius, style.panel, true);
    surface.roundedRect(layout.panel, style.panelRadius, style.border, false);

    if (layout.selectedPill.width > 0.0 && layout.selectedPill.height > 0.0) {
        surface.roundedRect(layout.selectedPill, style.selectedRadius,
                            style.selected, true);
    }

    if (!layout.modePrompt.empty()) {
        const auto metrics =
            surface.textMetrics(layout.modePrompt, style.candidateText);
        const auto textX = layout.panel.x +
                           (layout.panel.width - metrics.width) / 2.0;
        const auto baseline = layout.panel.y +
                              (layout.panel.height - metrics.height) / 2.0 +
                              metrics.baseline;
        surface.text(layout.modePrompt, textX, baseline, style.candidateText,
                     style.text);
    }

    for (const auto &candidate : layout.candidates) {
        const Rect textBounds = candidate.selected ? layout.selectedPill
                                                   : candidate.bounds;
        const auto separator = candidate.displayText.find('.');
        const auto indexText = candidate.displayText.substr(0, separator + 1);
        const auto valueText = candidate.displayText.substr(separator + 1);
        const auto indexMetrics =
            surface.textMetrics(indexText, style.candidateNumberText);
        const auto valueMetrics =
            surface.textMetrics(valueText, style.candidateText);
        const auto textWidth = indexMetrics.width + valueMetrics.width;
        const auto textX = textWidth <= textBounds.width
                               ? textBounds.x +
                                     (textBounds.width - textWidth) / 2.0
                               : textBounds.x;
        const auto textHeight = std::max(indexMetrics.height, valueMetrics.height);
        const auto baseline = textBounds.y +
                              (textBounds.height - textHeight) / 2.0 +
                              std::max(indexMetrics.baseline, valueMetrics.baseline);
        const auto color = candidate.selected ? style.selectedText : style.text;
        surface.text(indexText, textX, baseline, style.candidateNumberText, color);
        surface.text(valueText, textX + indexMetrics.width, baseline,
                     style.candidateText, color);
    }
}

} // namespace modernime::ui
