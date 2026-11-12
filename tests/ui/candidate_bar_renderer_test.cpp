#include "modernime/core/candidate_model.h"
#include "modernime/ui/candidate_bar_layout.h"
#include "modernime/ui/candidate_bar_renderer.h"

#include <algorithm>
#include <cstdlib>
#include <cmath>
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
    std::vector<double> textX;
    std::vector<double> textBaseline;
    std::vector<double> textSizes;
    std::vector<modernime::ui::Color> textColors;
    std::vector<modernime::ui::Rect> rects;

    void roundedRect(const modernime::ui::Rect &bounds, double,
                     const modernime::ui::Color &, bool fill) override {
        operations.push_back(fill ? "rounded-fill" : "rounded-stroke");
        rects.push_back(bounds);
    }

    double textWidth(std::string_view value,
                     const modernime::ui::TextStyle &) const override {
        return value.find('.') == std::string_view::npos ? 20.0 : 8.0;
    }

    void text(std::string_view value, double x, double baseline,
              const modernime::ui::TextStyle &style,
              const modernime::ui::Color &color) override {
        operations.push_back("text:" + std::string(value));
        textX.push_back(x);
        textBaseline.push_back(baseline);
        textSizes.push_back(style.size);
        textColors.push_back(color);
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
    const auto style = modernime::ui::RenderStyle::reference();
    assertTrue(style.panelRadius == 10.0 && style.selectedRadius == 7.0,
               "reference radii follow the design proportions");
    assertTrue(style.shadowRadius == 6.0 && style.shadowOffsetY == 4.0,
               "reference shadow preserves the native window height");
    assertTrue(style.candidateNumberText.size == 16.0 &&
                   style.candidateText.size == 19.0 &&
                   style.preeditText.size == 20.0,
               "reference typography follows the design proportions");
    assertTrue(style.selected.red == 0.07 && style.selected.green == 0.40 &&
                   style.selected.blue == 0.93,
               "selected color matches the design");
    modernime::ui::CandidateBarRenderer::render(surface, layout, style);

    assertTrue(surface.operations.size() == 10,
               "one shadow, panel, border, pill and six text segments");
    assertTrue(surface.operations[0] == "rounded-fill", "shadow is drawn first");
    assertTrue(surface.operations[1] == "rounded-fill", "panel is drawn second");
    assertTrue(surface.operations[2] == "rounded-stroke", "border is drawn third");
    assertTrue(surface.operations[3] == "rounded-fill", "selected pill is drawn fourth");
    assertTrue(surface.operations[4] == "text:1.", "first candidate index is drawn");
    assertTrue(surface.operations[5] == "text:还", "first candidate text is drawn");
    assertTrue(surface.operations[6] == "text:2.", "second candidate index is drawn");
    assertTrue(surface.operations[7] == "text:海", "second candidate text is drawn");
    assertTrue(surface.operations[8] == "text:3.", "third candidate index is drawn");
    assertTrue(surface.operations[9] == "text:害", "third candidate text is drawn");
    assertTrue(surface.rects[0].x == 0.0 && surface.rects[0].y == 6.0,
               "shadow extends below and around the panel");
    assertTrue(surface.textX.size() == 6, "candidate text positions are recorded");
    assertTrue(surface.textX[0] == 14.0 && surface.textX[1] == 22.0,
               "selected candidate text is centered in its pill");
    assertTrue(surface.textX[2] == 82.0 && surface.textX[3] == 90.0,
               "normal candidate text is centered in its slot");
    assertTrue(surface.textSizes[0] == 16.0 && surface.textSizes[1] == 19.0,
               "candidate index is smaller than candidate text");
    assertTrue(surface.textColors[0].red == style.selectedText.red &&
                   surface.textColors[0].green == style.selectedText.green &&
                   surface.textColors[0].blue == style.selectedText.blue,
               "selected index retains the same contrast as its candidate");
    assertTrue(surface.textColors[2].red > surface.textColors[3].red &&
                   surface.textColors[2].green > surface.textColors[3].green &&
                   surface.textColors[2].blue > surface.textColors[3].blue,
               "unselected index is secondary to the candidate text");
    assertTrue(surface.textBaseline.size() == 6,
               "candidate baselines are recorded");
    assertTrue(surface.textBaseline[0] == 33.75 &&
                   surface.textBaseline[1] == 33.75 &&
                   surface.textBaseline[2] == 33.75 &&
                   surface.textBaseline[3] == 33.75 &&
                   surface.textBaseline[4] == 33.75 &&
                   surface.textBaseline[5] == 33.75,
               "candidate text is vertically centered in its slot");

    auto settings = modernime::core::defaultSettings();
    const auto luminance = [](const modernime::ui::Color &color) {
        const auto linear = [](double channel) {
            return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * linear(color.red) + 0.7152 * linear(color.green) + 0.0722 * linear(color.blue);
    };
    const auto contrast = [&](const modernime::ui::Color &a, const modernime::ui::Color &b) {
        const double left = luminance(a), right = luminance(b);
        return (std::max(left, right) + 0.05) / (std::min(left, right) + 0.05);
    };
    for (const auto &theme : modernime::core::kCandidateThemes) {
        settings.candidateTheme = theme.value;
        const auto palette = modernime::ui::RenderStyle::fromSettings(settings);
        assertTrue(contrast(palette.panel, palette.text) >= 4.5 &&
                       contrast(palette.selected, palette.selectedText) >= 4.5,
                   "every candidate palette keeps normal and selected text readable");
        RecordingSurface themed;
        modernime::ui::CandidateBarRenderer::render(themed, layout, palette);
        assertTrue(themed.operations == surface.operations,
                   "palette changes preserve rendered candidate content and order");
        for (const auto &preset : modernime::core::kCandidateStyles) {
            auto materialSettings = settings;
            materialSettings.candidateStyle = preset.value;
            const auto material = modernime::ui::RenderStyle::fromSettings(materialSettings);
            const auto illuminated = [](modernime::ui::Color base, const modernime::ui::Color &light) {
                base.red += (light.red - base.red) * light.alpha;
                base.green += (light.green - base.green) * light.alpha;
                base.blue += (light.blue - base.blue) * light.alpha;
                return base;
            };
            assertTrue(contrast(material.panelTop, material.text) >= 4.5 &&
                           contrast(material.panelBottom, material.text) >= 4.5 &&
                           contrast(material.selectedTop, material.selectedText) >= 4.5 &&
                           contrast(material.selectedBottom, material.selectedText) >= 4.5 &&
                           contrast(material.clipboardSubmitBackground, material.selectedText) >= 4.5,
                       "every material retains text contrast at both ends of its lighting");
            assertTrue(contrast(illuminated(material.panelTop, material.panelLight), material.text) >= 4.5 &&
                           contrast(illuminated(material.panelBottom, material.panelLight), material.text) >= 4.5 &&
                           contrast(illuminated(material.selectedTop, material.selectedLight), material.selectedText) >= 4.5 &&
                           contrast(illuminated(material.selectedBottom, material.selectedLight), material.selectedText) >= 4.5,
                       "material highlights retain readable contrast at their maximum intensity");
            RecordingSurface rendered;
            modernime::ui::CandidateBarRenderer::render(rendered, layout, material);
            assertTrue(rendered.textX == themed.textX && rendered.textBaseline == themed.textBaseline,
                       "material changes preserve candidate positions and selection geometry");
        }
    }
    settings.candidateShape = modernime::core::CandidateShape::Square;
    const auto square = modernime::ui::RenderStyle::fromSettings(settings);
    settings.candidateShape = modernime::core::CandidateShape::Capsule;
    const auto capsule = modernime::ui::RenderStyle::fromSettings(settings);
    assertTrue(capsule.panelRadius > square.panelRadius && capsule.selectedRadius > square.selectedRadius &&
                   capsule.clipboardSelectedRadius > square.clipboardSelectedRadius,
               "shape changes reach both candidate and clipboard rendering");
    settings = modernime::core::defaultSettings();
    const auto normalMetrics = modernime::ui::CandidateBarMetrics::fromSettings(settings);
    settings.candidateFontFamily = "DejaVu Sans";
    settings.candidateTheme = modernime::core::CandidateTheme::Dark;
    settings.candidateSpacing = modernime::core::CandidateSpacing::Compact;
    const auto darkStyle = modernime::ui::RenderStyle::fromSettings(settings);
    const auto compactMetrics = modernime::ui::CandidateBarMetrics::fromSettings(settings);
    assertTrue(darkStyle.panel.red < darkStyle.text.red &&
                   darkStyle.selected.red > darkStyle.selectedText.red,
               "dark theme keeps candidate and selected text legible");
    assertTrue(darkStyle.candidateText.family == settings.candidateFontFamily &&
                   darkStyle.clipboardText.family == settings.candidateFontFamily &&
                   compactMetrics.fontFamily == settings.candidateFontFamily,
               "candidate and clipboard use the selected font");
    assertTrue(compactMetrics.panelWidth < normalMetrics.panelWidth &&
                   compactMetrics.candidateGap < normalMetrics.candidateGap,
               "compact spacing reduces the actual bar width");
    settings.candidateSpacing = modernime::core::CandidateSpacing::Spacious;
    assertTrue(modernime::ui::CandidateBarMetrics::fromSettings(settings).panelWidth > normalMetrics.panelWidth,
               "spacious spacing expands the actual bar width");

    modernime::core::CandidatePage clipboardPage;
    clipboardPage.mode = modernime::core::CandidatePageMode::Clipboard;
    clipboardPage.items = {{"first clipboard", {}, 0},
                           {"second clipboard", {}, 1},
                           {"third clipboard", {}, 2}};
    const auto clipboardLayout = modernime::ui::CandidateBarLayout::measure(
        clipboardPage, modernime::ui::CandidateBarMetrics::reference(),
        [](std::string_view value) {
            return static_cast<double>(value.size() * 4);
        });
    RecordingSurface clipboardSurface;
    modernime::ui::CandidateBarRenderer::render(clipboardSurface,
                                                 clipboardLayout, style);
    assertTrue(clipboardSurface.operations.size() > surface.operations.size(),
               "clipboard rendering includes rows and separators");
    assertTrue(std::find(clipboardSurface.operations.begin(),
                         clipboardSurface.operations.end(),
                         "text:first clipboard") !=
                   clipboardSurface.operations.end(),
               "clipboard rendering draws the row text without a number");
    assertTrue(std::find(clipboardSurface.operations.begin(),
                         clipboardSurface.operations.end(), "text:1.") ==
                   clipboardSurface.operations.end(),
               "clipboard rendering does not draw candidate number prefixes");

    modernime::core::CandidatePage annotatedPage;
    annotatedPage.items = {{"dee", "dee", 0, modernime::core::CandidateSource::Raw, 3, "马具；电极"},
                           {"的", "de", 1, modernime::core::CandidateSource::Engine, 2, ""}};
    annotatedPage.cursor = 0;
    const auto annotatedLayout = modernime::ui::CandidateBarLayout::measure(
        annotatedPage, modernime::ui::CandidateBarMetrics::reference());
    RecordingSurface annotatedSurface;
    modernime::ui::CandidateBarRenderer::render(annotatedSurface, annotatedLayout, style);
    assertTrue(std::find(annotatedSurface.operations.begin(),
                         annotatedSurface.operations.end(), "text:1.") !=
                   annotatedSurface.operations.end(),
               "annotated candidate renders number");
    assertTrue(std::find(annotatedSurface.operations.begin(),
                         annotatedSurface.operations.end(), "text:dee") !=
                   annotatedSurface.operations.end(),
               "annotated candidate renders main word");
    assertTrue(std::find(annotatedSurface.operations.begin(),
                         annotatedSurface.operations.end(), "text: (马具；电极)") !=
                   annotatedSurface.operations.end(),
               "annotated candidate renders annotation as secondary text");

    modernime::core::CandidatePage indicatorPage;
    indicatorPage.items = {{"第一项", "dyx", 0}, {"第二项", "dex", 1}};
    indicatorPage.pageBoundaries = {{0, 2}, {2, 4}};
    indicatorPage.cursor = 0;
    const auto indicatorLayout = modernime::ui::CandidateBarLayout::measure(
        indicatorPage, modernime::ui::CandidateBarMetrics::reference());
    RecordingSurface indicatorSurface;
    modernime::ui::CandidateBarRenderer::render(indicatorSurface, indicatorLayout, style);
    indicatorPage.pageBoundaries.clear();
    const auto plainLayout = modernime::ui::CandidateBarLayout::measure(
        indicatorPage, modernime::ui::CandidateBarMetrics::reference());
    RecordingSurface plainSurface;
    modernime::ui::CandidateBarRenderer::render(plainSurface, plainLayout, style);
    assertTrue(indicatorSurface.operations == plainSurface.operations &&
                   indicatorSurface.textX == plainSurface.textX &&
                   indicatorSurface.textBaseline == plainSurface.textBaseline,
               "page metadata draws no extra text and preserves candidate positions");

    return EXIT_SUCCESS;
}
