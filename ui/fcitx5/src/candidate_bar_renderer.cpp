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
    style.annotationText = {"Noto Sans CJK SC", 14.0, 400};
    style.annotation = {0.42, 0.47, 0.55, 1.0};
    style.selectedAnnotation = {0.90, 0.94, 1.0, 0.90};
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
        const std::string_view displayText = candidate.displayText;
        const auto separator = displayText.find('.');
        const auto indexText = separator != std::string_view::npos
                                   ? displayText.substr(0, separator + 1)
                                   : std::string_view{};
        const auto valueText = separator != std::string_view::npos
                                   ? displayText.substr(separator + 1)
                                   : displayText;
        // 易错音内联读音提示：在词部分第 hintAfterChar 个 UTF-8 码点之后切分
        // （前缀含 hintAfterChar+1 个字），以小号注音字体绘制该字正确读音
        // （如 卡qiǎ住）。
        std::size_t hintBytes = 0;
        std::size_t chars = 0;
        while (!candidate.hint.empty() && hintBytes < valueText.size() &&
               chars <= candidate.hintAfterChar) {
            const auto lead = static_cast<unsigned char>(valueText[hintBytes]);
            hintBytes += lead < 0x80 ? 1
                        : (lead & 0xE0) == 0xC0 ? 2
                        : (lead & 0xF0) == 0xE0 ? 3
                        : (lead & 0xF8) == 0xF0 ? 4 : 1;
            ++chars;
        }
        const auto valuePrefix =
            candidate.hint.empty() ? valueText : valueText.substr(0, hintBytes);
        const auto valueSuffix =
            candidate.hint.empty() ? std::string_view{}
                                   : valueText.substr(hintBytes);
        const auto indexMetrics =
            surface.textMetrics(indexText, style.candidateNumberText);
        const auto prefixMetrics =
            surface.textMetrics(valuePrefix, style.candidateText);
        const auto suffixMetrics =
            valueSuffix.empty()
                ? TextMetrics{}
                : surface.textMetrics(valueSuffix, style.candidateText);
        const auto hintMetrics =
            candidate.hint.empty()
                ? TextMetrics{}
                : surface.textMetrics(candidate.hint, style.annotationText);
        const auto annotationMetrics =
            candidate.annotation.empty()
                ? TextMetrics{}
                : surface.textMetrics(candidate.annotation, style.annotationText);
        const auto valueWidth = prefixMetrics.width + hintMetrics.width +
                                suffixMetrics.width;
        const auto textWidth = indexMetrics.width + valueWidth +
                               (candidate.annotation.empty() ? 0.0 : annotationMetrics.width);
        const auto textX = textWidth <= textBounds.width
                               ? textBounds.x +
                                     (textBounds.width - textWidth) / 2.0
                               : textBounds.x;
        const auto valueHeight = std::max(
            {prefixMetrics.height, hintMetrics.height, suffixMetrics.height});
        const auto valueBaseline = std::max(
            {prefixMetrics.baseline, hintMetrics.baseline, suffixMetrics.baseline});
        const auto textHeight =
            candidate.annotation.empty()
                ? std::max(indexMetrics.height, valueHeight)
                : std::max({indexMetrics.height, valueHeight,
                            annotationMetrics.height});
        const auto baseline =
            textBounds.y + (textBounds.height - textHeight) / 2.0 +
            (candidate.annotation.empty()
                 ? std::max(indexMetrics.baseline, valueBaseline)
                 : std::max({indexMetrics.baseline, valueBaseline,
                             annotationMetrics.baseline}));
        const auto color = candidate.selected ? style.selectedText : style.text;
        const auto annotationColor =
            candidate.selected ? style.selectedAnnotation : style.annotation;
        surface.text(indexText, textX, baseline, style.candidateNumberText,
                     color);
        double valueX = textX + indexMetrics.width;
        surface.text(valuePrefix, valueX, baseline, style.candidateText, color);
        valueX += prefixMetrics.width;
        if (!candidate.hint.empty()) {
            surface.text(candidate.hint, valueX, baseline,
                         style.annotationText, annotationColor);
            valueX += hintMetrics.width;
        }
        if (!valueSuffix.empty()) {
            surface.text(valueSuffix, valueX, baseline, style.candidateText,
                         color);
            valueX += suffixMetrics.width;
        }
        if (!candidate.annotation.empty()) {
            surface.text(candidate.annotation, valueX, baseline,
                         style.annotationText, annotationColor);
        }
    }

    if (!layout.pageIndicatorText.empty() && layout.pageIndicator.width > 0.0) {
        const auto indicatorMetrics =
            surface.textMetrics(layout.pageIndicatorText, style.annotationText);
        const auto textX = layout.pageIndicator.x +
                           std::max(0.0, (layout.pageIndicator.width - indicatorMetrics.width) / 2.0);
        const auto baseline = layout.pageIndicator.y +
                              (layout.pageIndicator.height - indicatorMetrics.height) / 2.0 +
                              indicatorMetrics.baseline;
        surface.text(layout.pageIndicatorText, textX, baseline, style.annotationText,
                     style.annotation);
    }
}

} // namespace modernime::ui
