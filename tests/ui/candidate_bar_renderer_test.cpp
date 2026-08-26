#include "modernime/core/candidate_model.h"
#include "modernime/ui/candidate_bar_layout.h"
#include "modernime/ui/candidate_bar_renderer.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "candidate bar renderer test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct RecordingSurface final : modernime::ui::RenderSurface {
    std::vector<std::string> operations;

    void roundedRect(const modernime::ui::Rect &, double,
                     const modernime::ui::Color &, bool fill) override {
        operations.push_back(fill ? "rounded-fill" : "rounded-stroke");
    }

    void text(std::string_view value, double, double,
              const modernime::ui::TextStyle &, const modernime::ui::Color &) override {
        operations.push_back("text:" + std::string(value));
    }
};

} // namespace

int main() {
    modernime::core::CandidatePage page;
    page.preedit = "hail";
    page.items = {{"还", "hail", 0}, {"海", "hail", 1}, {"害", "hail", 2}};
    page.cursor = 0;
    const auto layout = modernime::ui::CandidateBarLayout::measure(
        page, modernime::ui::CandidateBarMetrics::reference());

    RecordingSurface surface;
    modernime::ui::CandidateBarRenderer::render(
        surface, layout, modernime::ui::RenderStyle::reference());

    assertTrue(surface.operations.size() == 8,
               "one shadow, panel, border, pill, preedit and three candidates");
    assertTrue(surface.operations[0] == "rounded-fill", "shadow is drawn first");
    assertTrue(surface.operations[1] == "rounded-fill", "panel is drawn second");
    assertTrue(surface.operations[2] == "rounded-stroke", "border is drawn third");
    assertTrue(surface.operations[3] == "rounded-fill", "selected pill is drawn fourth");
    assertTrue(surface.operations[4] == "text:hail", "preedit is drawn fifth");
    assertTrue(surface.operations[5] == "text:1.还", "first candidate is drawn");
    assertTrue(surface.operations[6] == "text:2.海", "second candidate is drawn");
    assertTrue(surface.operations[7] == "text:3.害", "third candidate is drawn");
    return EXIT_SUCCESS;
}
