#include "modernime/ui/cairo_render_surface.h"

#include <pango/pangocairo.h>

#include <algorithm>
#include <array>

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

void CairoRenderSurface::gradientRoundedRect(const Rect &bounds, double radius,
                                             const Color &top, const Color &bottom, bool fill) {
    if (top == bottom) {
        roundedRect(bounds, radius, top, fill);
        return;
    }
    addRoundedRectPath(context_, bounds, radius);
    auto *gradient = cairo_pattern_create_linear(bounds.x, bounds.y, bounds.x, bounds.y + bounds.height);
    cairo_pattern_add_color_stop_rgba(gradient, 0, top.red, top.green, top.blue, top.alpha);
    cairo_pattern_add_color_stop_rgba(gradient, 1, bottom.red, bottom.green, bottom.blue, bottom.alpha);
    cairo_set_source(context_, gradient);
    if (fill) cairo_fill(context_);
    else { cairo_set_line_width(context_, 1.0); cairo_stroke(context_); }
    cairo_pattern_destroy(gradient);
}

void CairoRenderSurface::illuminatedRoundedRect(const Rect &bounds, double radius,
                                                const Color &top, const Color &bottom,
                                                const Color &light) {
    gradientRoundedRect(bounds, radius, top, bottom, true);
    if (light.alpha <= 0 || bounds.width <= 0 || bounds.height <= 0) return;
    cairo_save(context_);
    addRoundedRectPath(context_, bounds, radius);
    cairo_clip(context_);
    // A broad, elliptical light source gives the surface one coherent light
    // direction. Its highlight stays behind the text and inside the shape.
    cairo_translate(context_, bounds.x + bounds.width * 0.16,
                    bounds.y - bounds.height * 0.4);
    cairo_scale(context_, bounds.width * 0.9, bounds.height * 1.7);
    auto *glow = cairo_pattern_create_radial(0, 0, 0, 0, 0, 1);
    cairo_pattern_add_color_stop_rgba(glow, 0, light.red, light.green, light.blue, light.alpha);
    cairo_pattern_add_color_stop_rgba(glow, 0.55, light.red, light.green, light.blue, light.alpha * 0.35);
    cairo_pattern_add_color_stop_rgba(glow, 1, light.red, light.green, light.blue, 0);
    cairo_set_source(context_, glow);
    cairo_paint(context_);
    cairo_pattern_destroy(glow);
    cairo_restore(context_);
}

void CairoRenderSurface::shadowRoundedRect(const Rect &bounds, double radius,
                                           const Color &color,
                                           double blurRadius) {
    if (color.alpha <= 0 || bounds.width <= 0 || bounds.height <= 0) return;
    const double feather = std::clamp(blurRadius, 0.0, std::min(bounds.width, bounds.height) / 2.0);
    if (feather == 0) { roundedRect(bounds, radius, color, true); return; }
    const double corner = std::clamp(std::max(radius, feather), feather,
                                     std::min(bounds.width, bounds.height) / 2.0);
    const double left = bounds.x + corner, right = bounds.x + bounds.width - corner;
    const double top = bounds.y + corner, bottom = bounds.y + bounds.height - corner;
    // Nine bounded gradient patches form a continuous soft shadow, without a
    // per-pixel blur/allocation on every keystroke or overlapping alpha bands.
    const auto stops = [&](cairo_pattern_t *pattern, bool radial) {
        constexpr std::array ramp{0.0, 0.0625, 0.125, 0.25, 0.5, 0.75, 0.875, 0.9375, 1.0};
        for (const double t : ramp) {
            const double opacity = t * t * (3.0 - 2.0 * t);
            const double position = radial ? 1.0 - t * feather / corner : t;
            cairo_pattern_add_color_stop_rgba(pattern, position, color.red, color.green,
                                              color.blue, color.alpha * opacity);
        }
    };
    const auto patch = [&](const Rect &rect, cairo_pattern_t *pattern) {
        cairo_set_source(context_, pattern);
        cairo_rectangle(context_, rect.x, rect.y, rect.width, rect.height);
        cairo_fill(context_);
        cairo_pattern_destroy(pattern);
    };
    const auto edge = [&](const Rect &rect, double x1, double y1, double x2, double y2) {
        auto *pattern = cairo_pattern_create_linear(x1, y1, x2, y2);
        stops(pattern, false);
        patch(rect, pattern);
    };
    cairo_save(context_);
    cairo_set_antialias(context_, CAIRO_ANTIALIAS_NONE);
    setSource(context_, color);
    cairo_rectangle(context_, left, top, right - left, bottom - top);
    cairo_fill(context_);
    edge({left, bounds.y, right - left, corner}, 0, bounds.y, 0, bounds.y + feather);
    edge({left, bottom, right - left, corner}, 0, bounds.y + bounds.height, 0, bounds.y + bounds.height - feather);
    edge({bounds.x, top, corner, bottom - top}, bounds.x, 0, bounds.x + feather, 0);
    edge({right, top, corner, bottom - top}, bounds.x + bounds.width, 0, bounds.x + bounds.width - feather, 0);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            const double cx = x ? right : left, cy = y ? bottom : top;
            auto *pattern = cairo_pattern_create_radial(cx, cy, 0, cx, cy, corner);
            stops(pattern, true);
            patch({x ? right : bounds.x, y ? bottom : bounds.y, corner, corner}, pattern);
        }
    }
    cairo_restore(context_);
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
