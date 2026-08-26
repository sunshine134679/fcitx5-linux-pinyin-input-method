#include "modernime/core/candidate_model.h"
#include "modernime/ui/cairo_render_surface.h"
#include "modernime/ui/candidate_bar_layout.h"
#include "modernime/ui/candidate_bar_renderer.h"

#include <cairo/cairo.h>

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
    const std::string output =
        argc > 1 ? argv[1] : "/tmp/modernime-candidate-bar.png";
    const auto metrics = modernime::ui::CandidateBarMetrics::reference();

    modernime::core::CandidatePage page;
    page.preedit = "hail";
    constexpr std::array<std::string_view, 9> candidates{
        "还", "海", "害", "嗨", "咳", "亥", "孩", "骇", "氦"};
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        page.items.push_back({std::string(candidates[index]), "hail", index});
    }

    const auto layout = modernime::ui::CandidateBarLayout::measure(page, metrics);
    const auto style = modernime::ui::RenderStyle::reference();
    cairo_surface_t *surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, static_cast<int>(metrics.canvasWidth),
        static_cast<int>(metrics.canvasHeight));
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        std::cerr << "unable to create cairo image surface\n";
        return EXIT_FAILURE;
    }

    cairo_t *background = cairo_create(surface);
    cairo_set_source_rgb(background, 1.0, 1.0, 1.0);
    cairo_paint(background);
    cairo_destroy(background);

    modernime::ui::CairoRenderSurface renderer(surface);
    modernime::ui::CandidateBarRenderer::render(renderer, layout, style);
    cairo_status_t status = cairo_surface_write_to_png(surface, output.c_str());
    cairo_surface_destroy(surface);
    if (status != CAIRO_STATUS_SUCCESS) {
        std::cerr << "unable to write snapshot: " << output << '\n';
        return EXIT_FAILURE;
    }
    std::cout << output << '\n';
    return EXIT_SUCCESS;
}
