#pragma once

#include "modernime/ui/candidate_bar_layout.h"

#include <string>
#include <string_view>

namespace modernime::ui {

struct Color final {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double alpha = 1.0;
};

struct TextStyle final {
    std::string family;
    double size = 0.0;
    int weight = 400;
};

struct RenderStyle final {
    Color shadow;
    Color panel;
    Color border;
    Color selected;
    Color text;
    Color selectedText;
    Color preedit;
    double panelRadius = 0.0;
    double selectedRadius = 0.0;
    double shadowRadius = 0.0;
    double borderWidth = 0.0;
    TextStyle preeditText;
    TextStyle candidateText;

    static RenderStyle reference();
};

class RenderSurface {
public:
    virtual ~RenderSurface() = default;

    virtual void roundedRect(const Rect &bounds, double radius,
                             const Color &color, bool fill) = 0;
    virtual void text(std::string_view value, double x, double baseline,
                      const TextStyle &style, const Color &color) = 0;
};

class CandidateBarRenderer final {
public:
    static void render(RenderSurface &surface, const CandidateBarLayout &layout,
                       const RenderStyle &style);
};

} // namespace modernime::ui
