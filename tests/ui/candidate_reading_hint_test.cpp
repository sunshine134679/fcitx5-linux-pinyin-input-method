// 易错音内联读音提示（卡qiǎ住）布局与渲染测试（2026-10-25）。
#include "modernime/core/candidate_model.h"
#include "modernime/ui/candidate_bar_layout.h"
#include "modernime/ui/candidate_bar_renderer.h"
#include "modernime/ui/candidate_pagination.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "candidate reading hint test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

// CJK 宽度按码点计：把 UTF-8 串按非续字节数 * 20 计算。
double cjkAwareWidth(std::string_view value) {
    double width = 0.0;
    for (const char c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x80) {
            width += 10.0;
        } else if ((byte & 0xC0) != 0x80) {
            width += 20.0;
        }
    }
    return width;
}

struct WidthSurface final : modernime::ui::RenderSurface {
    std::vector<std::string> operations;
    std::vector<double> textX;
    std::vector<double> textSizes;

    void roundedRect(const modernime::ui::Rect &, double,
                     const modernime::ui::Color &, bool) override {}

    double textWidth(std::string_view value,
                     const modernime::ui::TextStyle &) const override {
        return cjkAwareWidth(value);
    }

    void text(std::string_view value, double x, double,
              const modernime::ui::TextStyle &style,
              const modernime::ui::Color &) override {
        operations.push_back("text:" + std::string(value));
        textX.push_back(x);
        textSizes.push_back(style.size);
    }
};

modernime::ui::CandidateBarMetrics referenceMetrics() {
    auto metrics = modernime::ui::CandidateBarMetrics::reference();
    metrics.panelWidth = 600.0;
    return metrics;
}

modernime::core::CandidatePage hintPage() {
    modernime::core::CandidatePage page;
    page.preedit = "kazhu";
    modernime::core::CandidateItem item;
    item.text = "卡住";
    item.fullPinyin = "ka'zhu";
    item.isLexical = true;
    item.readingHint = "qiǎ";
    item.readingHintAfterCharIndex = 0;
    page.items.push_back(item);
    modernime::core::CandidateItem plain;
    plain.text = "卡着";
    plain.fullPinyin = "ka'zhe";
    page.items.push_back(plain);
    page.cursor = 0;
    return page;
}

} // namespace

int main() {
    // 1. 布局：提示计入候选宽度并随几何下发。
    {
        auto page = hintPage();
        const auto hinted = modernime::ui::CandidateBarLayout::measure(
            page, referenceMetrics(), cjkAwareWidth);
        assertTrue(hinted.candidates[0].hint == "qiǎ" &&
                       hinted.candidates[0].hintAfterChar == 0,
                   "hinted candidate carries hint geometry");
        assertTrue(hinted.candidates[1].hint.empty(),
                   "unhinted candidate carries no hint");

        page.items[0].readingHint.clear();
        const auto plain = modernime::ui::CandidateBarLayout::measure(
            page, referenceMetrics(), cjkAwareWidth);
        assertTrue(hinted.candidates[0].bounds.width >
                       plain.candidates[0].bounds.width,
                   "hint widens the candidate slot to fit the reading");
    }

    // 2. 渲染：提示按 序号->前缀->qiǎ->后缀 顺序内联绘制，注音小字号。
    {
        const auto page = hintPage();
        const auto layout = modernime::ui::CandidateBarLayout::measure(
            page, referenceMetrics(), cjkAwareWidth);
        WidthSurface surface;
        const auto style = modernime::ui::RenderStyle::reference();
        modernime::ui::CandidateBarRenderer::render(surface, layout, style);
        const auto begin = surface.operations.begin();
        const auto end = surface.operations.end();
        const auto prefix = std::find(begin, end, std::string("text:卡"));
        const auto hint = std::find(begin, end, std::string("text:qiǎ"));
        const auto suffix = std::find(begin, end, std::string("text:住"));
        assertTrue(prefix != end && hint != end && suffix != end,
                   "inline hint splits the word into prefix/hint/suffix");
        assertTrue(prefix < hint && hint < suffix,
                   "hint renders between the prefix and suffix characters");
        const auto hintIndex = static_cast<std::size_t>(hint - begin);
        const auto prefixIndex = static_cast<std::size_t>(prefix - begin);
        const auto suffixIndex = static_cast<std::size_t>(suffix - begin);
        assertTrue(surface.textSizes[hintIndex] == style.annotationText.size,
                   "hint uses the smaller annotation font");
        assertTrue(surface.textSizes[prefixIndex] == style.candidateText.size &&
                       surface.textSizes[suffixIndex] == style.candidateText.size,
                   "word characters keep the candidate font");
        assertTrue(surface.textX[hintIndex] > surface.textX[prefixIndex] &&
                       surface.textX[suffixIndex] > surface.textX[hintIndex],
                   "hint is drawn inline between the two characters");
    }

    // 3. 溢出回退：空间不足时提示与尾注一并放弃，不产生越界几何。
    {
        auto page = hintPage();
        auto metrics = referenceMetrics();
        metrics.panelWidth = 40.0;
        const auto layout = modernime::ui::CandidateBarLayout::measure(
            page, metrics, cjkAwareWidth);
        for (const auto &geometry : layout.candidates) {
            assertTrue(geometry.hint.empty(),
                       "overflow fallback drops the reading hint");
        }
    }

    // 4. 分页/布局一致性：提示宽度必须同时计入分页与布局度量
    //    （CAND-PAGES 契约，2026-10-25 容错冒烟曾因两者失配红灯）。
    {
        const auto page = hintPage();
        for (double panelWidth : {160.0, 220.0, 280.0, 600.0}) {
            auto metrics = referenceMetrics();
            metrics.panelWidth = panelWidth;
            const auto measure = [](std::string_view value) {
                return cjkAwareWidth(value);
            };
            const auto boundaries = modernime::ui::CandidatePagination::partition(
                page.items, metrics, measure);
            std::size_t covered = 0;
            for (const auto &boundary : boundaries) {
                assertTrue(boundary.begin == covered && boundary.begin < boundary.end,
                           "partition covers every item exactly once");
                covered = boundary.end;
                modernime::core::CandidatePage local = page;
                local.items.assign(page.items.begin() + boundary.begin,
                                   page.items.begin() + boundary.end);
                for (std::size_t cursor = 0; cursor < local.items.size(); ++cursor) {
                    local.cursor = cursor;
                    const auto view = modernime::ui::CandidateBarLayout::measure(
                        local, metrics, measure);
                    assertTrue(view.candidates.size() == local.items.size(),
                               "pagination and layout agree with hint widths");
                }
            }
            assertTrue(covered == page.items.size(),
                       "partition covers the whole page");
        }
    }

    return EXIT_SUCCESS;
}
