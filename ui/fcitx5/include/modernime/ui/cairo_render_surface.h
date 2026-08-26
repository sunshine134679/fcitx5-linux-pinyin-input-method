#pragma once

#include "modernime/ui/candidate_bar_renderer.h"

#include <cairo/cairo.h>

namespace modernime::ui {

class CairoRenderSurface final : public RenderSurface {
public:
    explicit CairoRenderSurface(cairo_surface_t *surface);
    ~CairoRenderSurface() override;

    CairoRenderSurface(const CairoRenderSurface &) = delete;
    CairoRenderSurface &operator=(const CairoRenderSurface &) = delete;

    void roundedRect(const Rect &bounds, double radius, const Color &color,
                     bool fill) override;
    void shadowRoundedRect(const Rect &bounds, double radius, const Color &color,
                           double blurRadius) override;
    double textWidth(std::string_view value,
                     const TextStyle &style) const override;
    void text(std::string_view value, double x, double baseline,
              const TextStyle &style, const Color &color) override;

private:
    cairo_t *context_;
};

} // namespace modernime::ui
