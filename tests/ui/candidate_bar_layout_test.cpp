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

    assertTrue(metrics.canvasWidth == 614.0 && metrics.canvasHeight == 62.0,
               "reference canvas matches native Fcitx5 candidate window");
    assertTrue(metrics.panelX == 2.0 && metrics.panelY == 2.0 &&
                   metrics.panelWidth == 610.0 && metrics.panelHeight == 54.0,
               "candidate panel fits the native Fcitx5 window");
    assertTrue(layout.candidates.size() == 9, "layout caps candidates at nine");
    assertTrue(layout.panel.x == 2.0 && layout.panel.y == 2.0,
               "panel starts at reference position");
    assertTrue(layout.panel.width == 610.0 && layout.panel.height == 54.0,
               "panel uses reference dimensions");
    assertTrue(layout.preeditBaseline < layout.panel.y,
               "preedit baseline is above panel");
    assertTrue(layout.candidateBaseline > layout.panel.y &&
                   layout.candidateBaseline < layout.panel.y + layout.panel.height,
               "candidate baseline is inside panel");
    assertTrue(layout.candidates.front().selected,
               "first candidate is selected");
    assertTrue(layout.selectedPill.x == 10.0 && layout.selectedPill.y == 10.0 &&
                   layout.selectedPill.width == 36.0 &&
                   layout.selectedPill.height == 38.0,
               "selected pill uses reference bounds");
    for (std::size_t index = 1; index < layout.candidates.size(); ++index) {
        assertTrue(layout.candidates[index - 1].bounds.x <
                       layout.candidates[index].bounds.x,
                   "candidate slots are ordered left to right");
    }

    modernime::core::CandidatePage singleCharPage;
    singleCharPage.preedit = "h";
    for (std::size_t index = 0; index < 9; ++index) {
        singleCharPage.items.push_back({"还", "h", index});
    }
    const auto singleCharLayout = modernime::ui::CandidateBarLayout::measure(
        singleCharPage, metrics, [](std::string_view) { return 33.0; });
    assertTrue(singleCharLayout.candidates.size() == 9,
               "nine short candidates fit the fixed panel");

    modernime::core::CandidatePage longWordPage;
    longWordPage.preedit = "df";
    for (std::size_t index = 0; index < 9; ++index) {
        longWordPage.items.push_back({"地方", "df", index});
    }
    longWordPage.cursor = 0;
    const auto longWordLayout = modernime::ui::CandidateBarLayout::measure(
        longWordPage, metrics,
        [](std::string_view) { return 47.0; });
    assertTrue(longWordLayout.panel.width == metrics.panelWidth,
               "panel keeps the fixed reference width");
    assertTrue(longWordLayout.candidates.size() == 7,
               "only candidates that fit the fixed panel are displayed");
    assertTrue(longWordLayout.selectedPill.width == 63.0,
               "selected pill has wider horizontal padding");
    for (std::size_t index = 1; index < longWordLayout.candidates.size();
         ++index) {
        const auto &previous = longWordLayout.candidates[index - 1].bounds;
        const auto &current = longWordLayout.candidates[index].bounds;
        assertTrue(previous.x + previous.width <= current.x,
                   "multi-character candidate slots do not overlap");
    }
    const auto &lastCandidate = longWordLayout.candidates.back().bounds;
    assertTrue(lastCandidate.x + lastCandidate.width <=
                   metrics.panelX + metrics.panelWidth - metrics.horizontalPadding,
               "last displayed candidate stays inside the panel");

    modernime::core::CandidatePage clipboardPage;
    clipboardPage.mode = modernime::core::CandidatePageMode::Clipboard;
    clipboardPage.items = {
        {"cd /home/wsl/ModernIME ./install.sh", {}, 0},
        {"line one\nline two", {}, 1},
    };
    const auto clipboardLayout = modernime::ui::CandidateBarLayout::measure(
        clipboardPage, metrics,
        [](std::string_view value) {
            return static_cast<double>(value.size() * 10);
        });
    assertTrue(clipboardLayout.panel.height > metrics.panelHeight,
               "clipboard layout grows into a vertical list");
    assertTrue(clipboardLayout.candidates.size() == 2 &&
                   clipboardLayout.candidates[0].displayText ==
                       "cd /home/wsl/ModernIME ./install.sh",
               "clipboard rows do not add candidate number prefixes");
    assertTrue(clipboardLayout.candidates[1].bounds.y >
                   clipboardLayout.candidates[0].bounds.y,
               "clipboard rows are stacked vertically");
    assertTrue(clipboardLayout.candidates[1].displayText.find('\n') ==
                   std::string::npos,
               "clipboard rows remain single-line");

    modernime::core::CandidatePage longClipboardPage;
    longClipboardPage.mode = modernime::core::CandidatePageMode::Clipboard;
    longClipboardPage.items = {
        {"a very long clipboard entry that must be shortened before it reaches the edge of the popup", {}, 0}};
    const auto longClipboardLayout = modernime::ui::CandidateBarLayout::measure(
        longClipboardPage, metrics,
        [](std::string_view value) {
            return static_cast<double>(value.size() * 10);
        });
    assertTrue(longClipboardLayout.candidates.front().displayText.size() <
                   longClipboardPage.items.front().text.size() &&
                   longClipboardLayout.candidates.front().displayText.ends_with(
                       "…"),
               "long clipboard rows use an ellipsis instead of wrapping");
    return EXIT_SUCCESS;
}
