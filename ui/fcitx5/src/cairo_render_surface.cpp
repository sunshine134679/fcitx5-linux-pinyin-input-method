#include "modernime/ui/cairo_render_surface.h"

#include <pango/pangocairo.h>

#include <algorithm>

namespace modernime::ui {
namespace {

void addRoundedRectPath(cairo_t *context, const Rect &bounds, double radius) {
    const double clampedRadius = std::min(
        radius, std::min(bounds.width, bounds.height) / 2.0);
    const double right = bounds.x + bounds.width;
    const double bottom = bounds.y + bounds.height;
    constexpr double halfPi = 1.57079632679489661923;

    cairo_new_path(context);
    cairo_arc(context, right - clampedRadius, bounds.y + clampedRadius,
              clampedRadius, -halfPi, 0.0);
    cairo_arc(context, right - clampedRadius, bottom - clampedRadius,
              clampedRadius, 0.0, halfPi);
    cairo_arc(context, bounds.x + clampedRadius, bottom - clampedRadius,
              clampedRadius, halfPi, 2.0 * halfPi);
    cairo_arc(context, bounds.x + clampedRadius, bounds.y + clampedRadius,
              clampedRadius, 2.0 * halfPi, 3.0 * halfPi);
    cairo_close_path(context);
}

void setSource(cairo_t *context, const Color &color) {
    cairo_set_source_rgba(context, color.red, color.green, color.blue,
                          color.alpha);
}

PangoWeight pangoWeight(int weight) {
    return weight >= 700 ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL;
}

} // namespace

CairoRenderSurface::CairoRenderSurface(cairo_surface_t *surface)
    : context_(cairo_create(surface)) {}

CairoRenderSurface::CairoRenderSurface(cairo_t *context)
    : context_(context), ownsContext_(false) {}

CairoRenderSurface::~CairoRenderSurface() {
    if (fontDesc_ != nullptr) {
        pango_font_description_free(fontDesc_);
    }
    if (layout_ != nullptr) {
        g_object_unref(layout_);
    }
    if (ownsContext_) {
        cairo_destroy(context_);
    }
}

void CairoRenderSurface::roundedRect(const Rect &bounds, double radius,
                                     const Color &color, bool fill) {
    addRoundedRectPath(context_, bounds, radius);
    setSource(context_, color);
    if (fill) {
        cairo_fill(context_);
    } else {
        cairo_set_line_width(context_, 1.0);
        cairo_stroke(context_);
    }
}

void CairoRenderSurface::shadowRoundedRect(const Rect &bounds, double radius,
                                           const Color &color,
                                           double blurRadius) {
    const int layerCount = std::max(4, static_cast<int>(blurRadius * 2.0));
    for (int layer = layerCount; layer >= 1; --layer) {
        const double t = static_cast<double>(layer) / layerCount;
        const double inset = (1.0 - t) * blurRadius / 4.0;
        const Rect layerBounds{bounds.x + inset, bounds.y + inset,
                               bounds.width - 2.0 * inset,
                               bounds.height - 2.0 * inset};
        const Color layerColor{color.red, color.green, color.blue,
                               color.alpha * 0.06 * t};
        addRoundedRectPath(context_, layerBounds, radius - inset);
        setSource(context_, layerColor);
        cairo_fill(context_);
    }
}

PangoLayout *CairoRenderSurface::prepareLayout(std::string_view value,
                                              const TextStyle &style) const {
    if (layout_ == nullptr) {
        layout_ = pango_cairo_create_layout(context_);
    }
    if (fontDesc_ == nullptr || lastStyle_.family != style.family ||
        lastStyle_.size != style.size || lastStyle_.weight != style.weight) {
        if (fontDesc_ != nullptr) {
            pango_font_description_free(fontDesc_);
        }
        fontDesc_ = pango_font_description_new();
        pango_font_description_set_family(fontDesc_, style.family.c_str());
        pango_font_description_set_absolute_size(fontDesc_, style.size * PANGO_SCALE);
        pango_font_description_set_weight(fontDesc_, pangoWeight(style.weight));
        pango_layout_set_font_description(layout_, fontDesc_);
        lastStyle_ = style;
    }
    pango_layout_set_text(layout_, value.data(), static_cast<int>(value.size()));
    return layout_;
}

double CairoRenderSurface::textWidth(std::string_view value,
                                     const TextStyle &style) const {
    PangoLayout *layout = prepareLayout(value, style);
    int width = 0;
    int height = 0;
    pango_layout_get_pixel_size(layout, &width, &height);
    (void)height;
    return static_cast<double>(width);
}

TextMetrics CairoRenderSurface::textMetrics(std::string_view value,
                                            const TextStyle &style) const {
    PangoLayout *layout = prepareLayout(value, style);
    int width = 0;
    int height = 0;
    pango_layout_get_pixel_size(layout, &width, &height);
    const double baseline =
        static_cast<double>(pango_layout_get_baseline(layout)) / PANGO_SCALE;
    return {static_cast<double>(width), static_cast<double>(height), baseline};
}

void CairoRenderSurface::text(std::string_view value, double x, double baseline,
                              const TextStyle &style, const Color &color) {
    PangoLayout *layout = prepareLayout(value, style);
    setSource(context_, color);
    const double layoutBaseline =
        static_cast<double>(pango_layout_get_baseline(layout)) / PANGO_SCALE;
    cairo_move_to(context_, x, baseline - layoutBaseline);
    pango_cairo_show_layout(context_, layout);
}

} // namespace modernime::ui
