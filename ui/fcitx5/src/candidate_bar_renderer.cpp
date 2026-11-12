#include "modernime/ui/candidate_bar_renderer.h"

#include <algorithm>
#include <string_view>

namespace modernime::ui {
namespace {

Color mix(const Color &from, const Color &to, double amount) {
    return {from.red + (to.red - from.red) * amount,
            from.green + (to.green - from.green) * amount,
            from.blue + (to.blue - from.blue) * amount,
            from.alpha + (to.alpha - from.alpha) * amount};
}

void renderPanel(RenderSurface &surface, const CandidateBarLayout &layout,
                 const RenderStyle &style, double radius) {
    const Rect shadowBounds{layout.panel.x - style.shadowSpread,
        layout.panel.y + style.shadowOffsetY,
        layout.panel.width + 2.0 * style.shadowSpread, layout.panel.height + style.shadowSpread};
    surface.shadowRoundedRect(shadowBounds, radius + style.shadowSpread,
                              style.shadow, style.shadowRadius);
    surface.illuminatedRoundedRect(layout.panel, radius, style.panelTop, style.panelBottom, style.panelLight);
    const Rect edge{layout.panel.x + 0.5, layout.panel.y + 0.5,
                    std::max(0.0, layout.panel.width - 1), std::max(0.0, layout.panel.height - 1)};
    surface.gradientRoundedRect(edge, std::max(0.0, radius - 0.5), style.borderTop, style.borderBottom, false);
}

void renderSelection(RenderSurface &surface, const Rect &bounds, const RenderStyle &style,
                     double radius) {
    if (bounds.width <= 0 || bounds.height <= 0) return;
    if (style.selectedShadow.alpha > 0) {
        const Rect shadow{bounds.x - 2, bounds.y, bounds.width + 4, bounds.height + 4};
        surface.shadowRoundedRect(shadow, radius + 2, style.selectedShadow, 4.0);
    }
    surface.illuminatedRoundedRect(bounds, radius, style.selectedTop, style.selectedBottom, style.selectedLight);
    if (style.selectedEdgeTop.alpha > 0 || style.selectedEdgeBottom.alpha > 0) {
        const Rect edge{bounds.x + 0.5, bounds.y + 0.5,
                        std::max(0.0, bounds.width - 1), std::max(0.0, bounds.height - 1)};
        surface.gradientRoundedRect(edge, std::max(0.0, radius - 0.5),
                                    style.selectedEdgeTop, style.selectedEdgeBottom, false);
    }
}

void renderClipboard(RenderSurface &surface, const CandidateBarLayout &layout,
                     const RenderStyle &style) {
    renderPanel(surface, layout, style, style.clipboardPanelRadius);

    for (const auto &separator : layout.separators) {
        surface.roundedRect(separator, 0.0, style.clipboardSeparator, true);
    }
    renderSelection(surface, layout.selectedPill, style, style.clipboardSelectedRadius);

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
    style.shadow = {0.10, 0.14, 0.20, 0.09};
    style.panel = {1.0, 1.0, 1.0, 0.99};
    style.border = {0.86, 0.89, 0.93, 0.75};
    style.selected = {0.07, 0.40, 0.93, 1.0};
    style.text = {0.05, 0.09, 0.15, 1.0};
    style.selectedText = {1.0, 1.0, 1.0, 1.0};
    style.preedit = {0.27, 0.32, 0.40, 1.0};
    style.panelRadius = 10.0;
    style.selectedRadius = 7.0;
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
    style.clipboardSeparator = {0.90, 0.92, 0.95, 1.0};
    style.clipboardSubmitBackground = {0.04, 0.32, 0.82, 1.0};
    style.clipboardPanelRadius = 14.0;
    style.clipboardSelectedRadius = 8.0;
    style.clipboardSubmitRadius = 7.0;
    style.panelTop = style.panelBottom = style.panel;
    style.borderTop = style.borderBottom = style.border;
    style.selectedTop = style.selectedBottom = style.selected;
    return style;
}

RenderStyle RenderStyle::fromSettings(const core::ModernIMESettings &settings) {
    auto style = reference();
    for (auto *text : {&style.candidateText, &style.candidateNumberText, &style.preeditText,
                       &style.annotationText, &style.clipboardText, &style.clipboardSubmitIconText})
        text->family = settings.candidateFontFamily;
    style.candidateText.size = settings.candidateFontSize;
    style.candidateNumberText.size = std::max(10, settings.candidateFontSize - 4);
    style.preeditText.size = std::max(12, settings.candidateFontSize - 2);
    style.annotationText.size = std::max(10, settings.candidateFontSize - 6);
    style.clipboardText.size = settings.candidateFontSize;
    style.clipboardSubmitIconText.size = std::max(12, settings.candidateFontSize - 2);
    if (settings.candidateTheme != core::CandidateTheme::ClassicBlue)
        style.shadow = {0.0, 0.0, 0.0, 0.10};
    if (settings.candidateTheme == core::CandidateTheme::Dark) {
        style.panel = {0.12, 0.13, 0.14, 1.0};
        style.border = {0.29, 0.30, 0.31, 1.0};
        style.selected = {0.88, 0.89, 0.90, 1.0};
        style.text = {0.94, 0.95, 0.96, 1.0};
        style.selectedText = {0.10, 0.11, 0.12, 1.0};
        style.preedit = style.annotation = {0.68, 0.69, 0.70, 1.0};
        style.selectedAnnotation = {0.28, 0.29, 0.30, 1.0};
        style.clipboardSeparator = {0.25, 0.26, 0.27, 1.0};
        style.clipboardSubmitBackground = {0.77, 0.78, 0.79, 1.0};
    } else if (settings.candidateTheme == core::CandidateTheme::Light) {
        style.panel = {1.0, 1.0, 1.0, 0.99};
        style.border = {0.86, 0.86, 0.86, 1.0};
        style.selected = {0.17, 0.17, 0.17, 1.0};
        style.text = {0.13, 0.13, 0.13, 1.0};
        style.selectedText = {1.0, 1.0, 1.0, 1.0};
        style.preedit = style.annotation = {0.43, 0.43, 0.43, 1.0};
        style.selectedAnnotation = {0.85, 0.85, 0.85, 1.0};
        style.clipboardSeparator = {0.90, 0.90, 0.90, 1.0};
        style.clipboardSubmitBackground = {0.29, 0.29, 0.29, 1.0};
    }
    const auto color = [](unsigned int hex) -> Color {
        return {((hex >> 16) & 255) / 255.0, ((hex >> 8) & 255) / 255.0,
                (hex & 255) / 255.0, 1.0};
    };
    const auto palette = [&](unsigned int panel, unsigned int ink, unsigned int accent,
                             unsigned int selectedInk, unsigned int muted, unsigned int border) {
        style.panel = color(panel); style.text = color(ink); style.selected = color(accent);
        style.selectedText = style.selectedAnnotation = color(selectedInk);
        style.preedit = style.annotation = color(muted); style.border = color(border);
        style.clipboardSeparator = style.border;
        // The clipboard submit mark uses selectedText, so keep its surface in
        // the same contrast range as the selected candidate.
        style.clipboardSubmitBackground = style.selected;
    };
    switch (settings.candidateTheme) {
    case core::CandidateTheme::Jade: palette(0xf2faf6, 0x163c31, 0x087f63, 0xffffff, 0x527469, 0xc2ddcf); break;
    case core::CandidateTheme::Amber: palette(0xfff8ed, 0x3c2b1b, 0xa65f16, 0xffffff, 0x7c654d, 0xe4cdaa); break;
    case core::CandidateTheme::Rose: palette(0xfff4f7, 0x452634, 0xa83d63, 0xffffff, 0x81586b, 0xe7c4d0); break;
    case core::CandidateTheme::Lavender: palette(0xf7f3ff, 0x33284b, 0x7052ad, 0xffffff, 0x706080, 0xd9ccec); break;
    case core::CandidateTheme::Midnight: palette(0x182536, 0xedf4ff, 0x89b9ff, 0x142339, 0xb0c1d8, 0x405570); break;
    case core::CandidateTheme::HighContrast: palette(0x090909, 0xffffff, 0xffe066, 0x111111, 0xd8d8d8, 0xeeeeee); break;
    default: break;
    }
    if (settings.candidateShape == core::CandidateShape::Square) {
        style.panelRadius = style.clipboardPanelRadius = 3.0;
        style.selectedRadius = style.clipboardSelectedRadius = style.clipboardSubmitRadius = 2.0;
        style.shadow.alpha = 0.0;
    } else if (settings.candidateShape == core::CandidateShape::Capsule) {
        style.panelRadius = 28.0;
        style.selectedRadius = 24.0;
        style.clipboardPanelRadius = 20.0;
        style.clipboardSelectedRadius = style.clipboardSubmitRadius = 16.0;
    }
    if (settings.candidateTheme == core::CandidateTheme::HighContrast) style.shadow.alpha = 0.0;
    style.panelTop = style.panelBottom = style.panel;
    style.borderTop = style.borderBottom = style.border;
    style.selectedTop = style.selectedBottom = style.selected;
    // High contrast keeps solid, unambiguous surfaces for every saved style.
    if (settings.candidateTheme == core::CandidateTheme::HighContrast) return style;
    const bool dark = style.panel.red + style.panel.green + style.panel.blue < 1.5;
    const Color white{1, 1, 1, 1}, black{0, 0, 0, 1};
    if (settings.candidateStyle == core::CandidateStyle::Minimal) {
        const auto accent = style.selected;
        // Matte finish: one low-relief tonal selection, a precise edge, and a
        // restrained contact shadow. Simplicity retains the same craft as the
        // other materials instead of removing all surface definition.
        style.shadow.alpha = settings.candidateShape == core::CandidateShape::Square ? 0 : (dark ? 0.16 : 0.08);
        style.selected = mix(style.panel, accent, dark ? 0.14 : 0.08);
        style.selected.alpha = 1;
        style.selectedText = mix(accent, style.text, 0.30);
        style.selectedAnnotation = style.selectedText;
        style.clipboardSubmitBackground = mix(style.panel, accent, dark ? 0.18 : 0.10);
        style.panel.alpha = 1;
        style.border = mix(style.panel, style.text, dark ? 0.20 : 0.12);
        style.panelTop = style.panel;
        style.panelBottom = mix(style.panel, accent, dark ? 0.012 : 0.008);
        style.borderTop = mix(style.panel, style.text, dark ? 0.26 : 0.10);
        style.borderBottom = style.border;
        style.selectedTop = style.selectedBottom = style.selected;
        style.selectedEdgeTop = mix(style.panel, accent, dark ? 0.25 : 0.16);
        style.selectedEdgeBottom = mix(style.panel, accent, dark ? 0.20 : 0.12);
    } else if (settings.candidateStyle == core::CandidateStyle::Textured) {
        // Frosted ceramic rather than see-through glass: the dense surface
        // protects small text, while a broad light and inset rim express depth.
        style.shadow = mix(style.panel, black, dark ? 0.80 : 0.88);
        style.shadow.alpha = settings.candidateShape == core::CandidateShape::Square ? 0 : (dark ? 0.32 : 0.22);
        style.panelTop = mix(style.panel, white, dark ? 0.035 : 0.12);
        style.panelBottom = mix(style.panel, style.selected, dark ? 0.025 : 0.045);
        style.panelLight = {1, 1, 1, dark ? 0.055 : 0.55};
        style.borderTop = mix(style.panel, white, dark ? 0.30 : 0.75);
        style.borderBottom = mix(style.border, style.text, dark ? 0.12 : 0.08);
        // Dark palettes have pale selections. Reverse the relief's lightness
        // range to keep their dark foreground ink as crisp as light palettes.
        style.selectedTop = mix(style.selected, dark ? white : black, dark ? 0.10 : 0.025);
        style.selectedBottom = mix(style.selected, dark ? white : black, dark ? 0.025 : 0.15);
        style.selectedLight = {1, 1, 1, dark ? 0.18 : 0.06};
        style.selectedShadow = mix(style.selected, black, dark ? 0.75 : 0.70);
        style.selectedShadow.alpha = dark ? 0.28 : 0.22;
        style.selectedEdgeTop = {1, 1, 1, dark ? 0.70 : 0.40};
        style.selectedEdgeBottom = {1, 1, 1, dark ? 0.08 : 0.04};
    }
    return style;
}

void CandidateBarRenderer::render(RenderSurface &surface,
                                  const CandidateBarLayout &layout,
                                  const RenderStyle &style) {
    if (layout.clipboardMode) {
        renderClipboard(surface, layout, style);
        return;
    }

    renderPanel(surface, layout, style, style.panelRadius);
    renderSelection(surface, layout.selectedPill, style, style.selectedRadius);

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
        const auto numberColor = candidate.selected ? style.selectedText
                                                     : style.preedit;
        const auto annotationColor =
            candidate.selected ? style.selectedAnnotation : style.annotation;
        surface.text(indexText, textX, baseline, style.candidateNumberText,
                     numberColor);
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
}

} // namespace modernime::ui
