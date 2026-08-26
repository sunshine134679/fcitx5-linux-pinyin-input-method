#include "modernime/core/candidate_model.h"
#include "modernime/ui/candidate_bar_layout.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "candidate bar layout test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    modernime::core::CandidatePage page;
    page.preedit = "hail";
    for (std::size_t index = 0; index < 12; ++index) {
        page.items.push_back({"候选" + std::to_string(index), "hail", index});
    }
    page.cursor = 0;

    const auto metrics = modernime::ui::CandidateBarMetrics::reference();
    const auto layout = modernime::ui::CandidateBarLayout::measure(page, metrics);

    assertTrue(layout.candidates.size() == 9, "layout caps candidates at nine");
    assertTrue(layout.panel.x == 216.0 && layout.panel.y == 330.0,
               "panel starts at reference position");
    assertTrue(layout.panel.width == 1778.0 && layout.panel.height == 153.0,
               "panel uses reference dimensions");
    assertTrue(layout.preeditBaseline < layout.panel.y,
               "preedit baseline is above panel");
    assertTrue(layout.candidateBaseline > layout.panel.y &&
                   layout.candidateBaseline < layout.panel.y + layout.panel.height,
               "candidate baseline is inside panel");
    assertTrue(layout.candidates.front().selected,
               "first candidate is selected");
    assertTrue(layout.selectedPill.x == 249.0 && layout.selectedPill.y == 359.0 &&
                   layout.selectedPill.width == 180.0 &&
                   layout.selectedPill.height == 95.0,
               "selected pill uses reference bounds");
    for (std::size_t index = 1; index < layout.candidates.size(); ++index) {
        assertTrue(layout.candidates[index - 1].bounds.x <
                       layout.candidates[index].bounds.x,
                   "candidate slots are ordered left to right");
    }
    return EXIT_SUCCESS;
}
