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
