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
    style.panelRadius = 42.0;
    style.selectedRadius = 48.0;
    style.shadowRadius = 18.0;
    style.borderWidth = 2.0;
    style.preeditText = {"Noto Sans CJK SC", 56.0, 400};
    style.candidateText = {"Noto Sans CJK SC", 52.0, 400};
    return style;
}

void CandidateBarRenderer::render(RenderSurface &surface,
                                  const CandidateBarLayout &layout,
                                  const RenderStyle &style) {
    surface.roundedRect(layout.panel, style.panelRadius, style.shadow, true);
    surface.roundedRect(layout.panel, style.panelRadius, style.panel, true);
    surface.roundedRect(layout.panel, style.panelRadius, style.border, false);

    if (layout.selectedPill.width > 0.0 && layout.selectedPill.height > 0.0) {
        surface.roundedRect(layout.selectedPill, style.selectedRadius,
                            style.selected, true);
    }

    if (!layout.preedit.empty()) {
        surface.text(layout.preedit, layout.preeditX, layout.preeditBaseline,
                     style.preeditText, style.preedit);
    }
    for (const auto &candidate : layout.candidates) {
        surface.text(candidate.displayText, candidate.bounds.x,
                     layout.candidateBaseline, style.candidateText,
                     candidate.selected ? style.selectedText : style.text);
    }
}

} // namespace modernime::ui
