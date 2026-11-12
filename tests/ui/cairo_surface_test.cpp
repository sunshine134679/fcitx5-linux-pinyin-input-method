#include "modernime/ui/cairo_render_surface.h"

#include <cairo/cairo.h>

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "cairo surface test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    // Shadow bounds include their full fade. The requested opacity belongs to
    // the core, not to a stack of partially transparent rounded rectangles.
    auto *shadow = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 96, 80);
    {
        modernime::ui::CairoRenderSurface renderer(shadow);
        renderer.shadowRoundedRect({16, 16, 64, 48}, 12, {0, 0, 0, 0.5}, 8);
    }
    cairo_surface_flush(shadow);
    const auto alpha = [&](int x, int y) {
        const auto *row = reinterpret_cast<const unsigned int *>(
            cairo_image_surface_get_data(shadow) + y * cairo_image_surface_get_stride(shadow));
        return row[x] >> 24;
    };
    assertTrue(alpha(48, 40) >= 126 && alpha(48, 40) <= 129,
               "shadow core retains the requested opacity");
    assertTrue(alpha(15, 40) == 0 && alpha(16, 40) <= 2 && alpha(79, 40) <= 2 && alpha(80, 40) == 0,
               "shadow fades to transparent inside its complete bounds");
    assertTrue(alpha(17, 40) < alpha(19, 40) && alpha(19, 40) < alpha(21, 40) &&
                   alpha(21, 40) < alpha(24, 40),
               "shadow falloff is smooth and monotonic");
    cairo_surface_destroy(shadow);

    auto *material = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 96, 80);
    {
        modernime::ui::CairoRenderSurface renderer(material);
        renderer.illuminatedRoundedRect({16, 16, 64, 48}, 10,
                                        {0.2, 0.2, 0.2, 1}, {0.2, 0.2, 0.2, 1}, {1, 1, 1, 0.5});
    }
    cairo_surface_flush(material);
    const auto pixel = [&](int x, int y) {
        const auto *row = reinterpret_cast<const unsigned int *>(
            cairo_image_surface_get_data(material) + y * cairo_image_surface_get_stride(material));
        return row[x];
    };
    assertTrue((pixel(30, 22) & 255) > (pixel(70, 54) & 255),
               "material light falls from upper left towards lower right");
    assertTrue(pixel(15, 40) == 0 && pixel(16, 16) == 0 && (pixel(48, 40) >> 24) == 255,
               "material light stays inside the rounded opaque surface");
    cairo_surface_destroy(material);

    cairo_surface_t *surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, 2172, 724);
    assertTrue(surface != nullptr, "cairo surface exists");
    modernime::ui::CairoRenderSurface renderer(surface);

    renderer.roundedRect({216.0, 330.0, 1778.0, 153.0}, 42.0,
                         {1.0, 1.0, 1.0, 1.0}, true);
    renderer.text("1.还", 249.0, 421.0,
                  {"Noto Sans CJK SC", 52.0, 400},
                  {0.06, 0.10, 0.16, 1.0});
    cairo_surface_flush(surface);
    unsigned char *pixels = cairo_image_surface_get_data(surface);
    assertTrue(pixels != nullptr, "cairo pixels exist");
    bool hasInk = false;
    for (int index = 0; index < 2172 * 724 * 4; ++index) {
        if (pixels[index] != 0) {
            hasInk = true;
            break;
        }
    }
    assertTrue(hasInk, "renderer writes pixels");
    cairo_surface_destroy(surface);
    return EXIT_SUCCESS;
}
