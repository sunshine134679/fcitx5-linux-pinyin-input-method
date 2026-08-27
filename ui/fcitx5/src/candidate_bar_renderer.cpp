#include "modernime/ui/candidate_bar_renderer.h"

namespace modernime::ui {

RenderStyle RenderStyle::reference() {
    RenderStyle style;
    style.shadow = {0.10, 0.14, 0.20, 0.12};
    style.panel = {1.0, 1.0, 1.0, 0.94};
    style.border = {0.88, 0.90, 0.93, 0.90};
    style.selected = {0.08, 0.42, 0.94, 1.0};
    style.text = {0.06, 0.10, 0.16, 1.0};
    style.selectedText = {1.0, 1.0, 1.0, 1.0};
    style.preedit = {0.27, 0.32, 0.40, 1.0};
    style.panelRadius = 14.0;
    style.selectedRadius = 19.0;
    style.shadowRadius = 4.0;
    style.shadowSpread = 2.0;
    style.shadowOffsetY = 4.0;
    style.borderWidth = 1.0;
    style.preeditText = {"Noto Sans CJK SC", 18.0, 400};
    style.candidateText = {"Noto Sans CJK SC", 20.0, 400};
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

    for (const auto &candidate : layout.candidates) {
        const Rect textBounds = candidate.selected ? layout.selectedPill
                                                   : candidate.bounds;
        const double width =
            surface.textWidth(candidate.displayText, style.candidateText);
        const double textX = width > 0.0 && width <= textBounds.width
                                 ? textBounds.x + (textBounds.width - width) / 2.0
                                 : textBounds.x;
        surface.text(candidate.displayText, textX,
                     layout.candidateBaseline, style.candidateText,
                     candidate.selected ? style.selectedText : style.text);
    }
}

} // namespace modernime::ui
