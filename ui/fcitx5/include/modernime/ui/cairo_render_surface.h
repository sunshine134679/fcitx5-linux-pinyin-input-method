#pragma once

#include "modernime/ui/candidate_bar_renderer.h"

#include <cairo/cairo.h>

struct _PangoLayout;
typedef struct _PangoLayout PangoLayout;
struct _PangoFontDescription;
typedef struct _PangoFontDescription PangoFontDescription;

namespace modernime::ui {

class CairoRenderSurface final : public RenderSurface {
public:
    explicit CairoRenderSurface(cairo_surface_t *surface);
    explicit CairoRenderSurface(cairo_t *context);
    ~CairoRenderSurface() override;

    CairoRenderSurface(const CairoRenderSurface &) = delete;
    CairoRenderSurface &operator=(const CairoRenderSurface &) = delete;

    void roundedRect(const Rect &bounds, double radius, const Color &color,
                     bool fill) override;
    void shadowRoundedRect(const Rect &bounds, double radius, const Color &color,
                           double blurRadius) override;
    double textWidth(std::string_view value,
                     const TextStyle &style) const override;
    TextMetrics textMetrics(std::string_view value,
                            const TextStyle &style) const override;
    void text(std::string_view value, double x, double baseline,
              const TextStyle &style, const Color &color) override;

private:
    PangoLayout *prepareLayout(std::string_view value,
                               const TextStyle &style) const;

    cairo_t *context_;
    bool ownsContext_ = true;
    mutable PangoLayout *layout_ = nullptr;
    mutable PangoFontDescription *fontDesc_ = nullptr;
    mutable TextStyle lastStyle_{};
};

} // namespace modernime::ui
