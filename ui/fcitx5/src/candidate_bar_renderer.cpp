#include "modernime/ui/candidate_bar_renderer.h"

#include <algorithm>
#include <string_view>

namespace modernime::ui {
namespace {

void renderClipboard(RenderSurface &surface, const CandidateBarLayout &layout,
                     const RenderStyle &style) {
    const Rect shadowBounds{
        layout.panel.x - style.shadowSpread,
        layout.panel.y + style.shadowOffsetY,
        layout.panel.width + 2.0 * style.shadowSpread,
        layout.panel.height + style.shadowSpread};
    surface.shadowRoundedRect(shadowBounds,
                              style.clipboardPanelRadius + style.shadowSpread,
                              style.shadow, style.shadowRadius);
    surface.roundedRect(layout.panel, style.clipboardPanelRadius, style.panel,
                        true);
    surface.roundedRect(layout.panel, style.clipboardPanelRadius, style.border,
                        false);

    for (const auto &separator : layout.separators) {
        surface.roundedRect(separator, 0.0, style.clipboardSeparator, true);
    }
    if (layout.selectedPill.width > 0.0 && layout.selectedPill.height > 0.0) {
        surface.roundedRect(layout.selectedPill, style.clipboardSelectedRadius,
                            style.selected, true);
    }

    for (const auto &candidate : layout.candidates) {
        const auto &bounds = candidate.bounds;
        const auto textStyle = style.clipboardText;
        const auto textMetrics = surface.textMetrics(candidate.displayText,
                                                     textStyle);
        const auto textX = bounds.x + layout.clipboardTextPadding;
        const auto baseline = bounds.y +
                              (bounds.height - textMetrics.height) / 2.0 +
                              textMetrics.baseline;
        const auto color = candidate.selected ? style.selectedText : style.text;
        surface.text(candidate.displayText, textX, baseline, textStyle, color);

        if (candidate.selected && layout.submitIcon.width > 0.0 &&
            layout.submitIcon.height > 0.0) {
            surface.roundedRect(layout.submitIcon, style.clipboardSubmitRadius,
                                style.clipboardSubmitBackground, true);
            constexpr std::string_view submitIcon = "↵";
            const auto iconMetrics = surface.textMetrics(
                submitIcon, style.clipboardSubmitIconText);
            const auto iconX = layout.submitIcon.x +
                               (layout.submitIcon.width - iconMetrics.width) /
                                   2.0;
            const auto iconBaseline = layout.submitIcon.y +
                                      (layout.submitIcon.height -
                                       iconMetrics.height) /
                                          2.0 +
                                      iconMetrics.baseline;
            surface.text(submitIcon, iconX, iconBaseline,
                         style.clipboardSubmitIconText, style.selectedText);
        }
    }
}

} // namespace

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
    style.clipboardText = {"Noto Sans CJK SC", 20.0, 400};
    style.clipboardSubmitIconText = {"Noto Sans CJK SC", 18.0, 400};
    style.clipboardSeparator = {0.86, 0.88, 0.91, 1.0};
    style.clipboardSubmitBackground = {0.04, 0.32, 0.82, 1.0};
    style.clipboardPanelRadius = 14.0;
    style.clipboardSelectedRadius = 8.0;
    style.clipboardSubmitRadius = 7.0;
    return style;
}

void CandidateBarRenderer::render(RenderSurface &surface,
                                  const CandidateBarLayout &layout,
                                  const RenderStyle &style) {
    if (layout.clipboardMode) {
        renderClipboard(surface, layout, style);
        return;
    }

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
