// Manual diagnostic replay; build/run instructions: docs/ALGORITHM_USAGE_SIMULATION_2026-10-15.md.
// Ranking rows are observations, not assertions that the new UX contracts pass.
#include "modernime/fcitx5/engine.h"
#include "modernime/pinyin/pinyin_candidate_provider.h"
#include "modernime/core/learning_store.h"
#include "modernime/ui/candidate_bar_layout.h"
#include "modernime/ui/candidate_pagination.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace modernime;
namespace fs = std::filesystem;
using Controller = fcitx5::ModernIMEController;
using Kind = fcitx5::KeyKind;
struct Host : fcitx5::EngineHost {
    std::vector<std::string> commits;
    void publishPage(const core::CandidatePage &) override {}
    void commit(std::string_view text) override { commits.emplace_back(text); }
};
void check(bool ok, const std::string &message) {
    if (!ok) throw std::runtime_error(message);
}
std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
pinyin::PinyinDataPaths paths(const fs::path &root) {
    fs::create_directories(root);
    std::ofstream(root / "empty-user.txt");
    pinyin::PinyinDataPaths p;
    p.learningStore = (root / "learning.sqlite3").string();
    p.userDictionary = (root / "empty-user.txt").string();
    p.extensionDictionary = (fs::current_path() / "build/install-debug/data/pinyin/modernime-knowledge.dict").string();
    p.hotwordDictionary = (fs::current_path() / "build/install-debug/data/pinyin/modernime-hotwords.dict").string();
    return p;
}
void type(Controller &c, const std::string &input) {
    c.setContext("", "");
    for (char key : input) check(c.handle({Kind::Character, key, 0}), "key rejected");
    check(c.page().rawInput == input, "raw input changed");
}
std::size_t index(const core::CandidatePage &p, const std::string &word) {
    for (std::size_t i = 0; i < p.items.size(); ++i)
        if (p.items[i].text == word) return i;
    throw std::runtime_error("missing candidate " + word);
}
double width(std::string_view text, double font) {
    double result = 0;
    for (unsigned char ch : text)
        if ((ch & 0xc0) != 0x80) result += (ch < 128 ? 0.55 : 1.0) * font;
    return result;
}
std::pair<std::size_t, std::size_t> physicalPosition(
    const core::CandidatePage &page, std::size_t global, double font = 20, double panel = 610) {
    auto metrics = ui::CandidateBarMetrics::reference(font, 9);
    metrics.panelWidth = panel;
    const auto boundaries = ui::CandidatePagination::partition(page.items, metrics,
        [font](std::string_view text) { return width(text, font); });
    for (std::size_t n = 0; n < boundaries.size(); ++n) {
        const auto b = boundaries[n];
        if (global >= b.begin && global < b.end) return {n, global - b.begin};
    }
    throw std::runtime_error("learned candidate lost during pagination");
}
void chooseFromPhysicalPage(Controller &c, const std::string &target, bool digit) {
    auto metrics = ui::CandidateBarMetrics::reference(20, 9);
    metrics.panelWidth = 610;
    auto measure = [](std::string_view text) { return width(text, 20); };
    const auto boundaries = ui::CandidatePagination::partition(c.page().items, metrics, measure);
    const auto [page, slot] = physicalPosition(c.page(), index(c.page(), target));
    c.setPageBoundaries(boundaries);
    if (page) check(c.movePage(page), "learned target page move");
    if (digit) {
        check(c.handle({Kind::Digit, 0, char('1' + slot)}), "learned target digit select");
    } else {
        core::CandidatePage local;
        const auto b = boundaries[page];
        local.items.assign(c.page().items.begin() + b.begin, c.page().items.begin() + b.end);
        const auto view = ui::CandidateBarLayout::measure(local, metrics, measure);
        const auto &r = view.candidates.at(slot).bounds;
        const int hit = ui::hitTestCandidate(view, metrics, r.x + r.width / 2, r.y + r.height / 2);
        check(hit == static_cast<int>(slot) && c.select(b.begin + hit), "learned target click mapping");
    }
}
void train(const fs::path &root, const std::string &input,
           const std::string &target, const std::string &competitor = {}) {
    auto p = paths(root);
    if (!competitor.empty()) {
        core::LearningStore store(p.learningStore);
        check(store.open(), "seed open");
        std::vector<core::LearningEvent> events(200,
            {core::LearningEvent::Kind::Selection, competitor, input, {}, {}, now()});
        check(store.recordBatch(events), "seed write");
    }
    {
        pinyin::PinyinCandidateProvider provider(p);
        Host host;
        Controller c(host, &provider);
        type(c, input);
        std::cout << "CONTINUOUS input=" << input << " target=" << target
                  << " seed=" << competitor << " initial=" << index(c.page(), target) + 1 << '\n';
        for (int n = 1; n <= 50; ++n) {
            const auto selected = c.page().items.at(index(c.page(), target));
            chooseFromPhysicalPage(c, target, n % 2 == 0);
            check(host.commits.back() == target, "committed wrong text");
            const bool partial = selected.consumedInputBytes > 0 &&
                                 selected.consumedInputBytes < input.size();
            if (partial) {
                check(c.page().rawInput == input.substr(selected.consumedInputBytes), "partial remainder");
                check(c.handle({Kind::Escape}), "cancel remainder");
            } else check(c.page().rawInput.empty(), "full commit left composition");
            type(c, input);
            if (n == 1 || n == 3 || n == 10 || n == 20 || n == 50) {
                check(provider.flush(), "flush");
                const auto &item = c.page().items.at(index(c.page(), target));
                core::LearningStore store(p.learningStore);
                check(store.open(), "read store");
                auto snapshot = store.snapshot();
                auto entry = snapshot->entry(target, item.fullPinyin, {}, {});
                check(entry && entry->frequency == n, "selection frequency mismatch");
                std::cout << " n=" << n << " rank=" << index(c.page(), target) + 1
                          << " freq=" << entry->frequency
                          << " stored_boost=" << snapshot->boostAt(target, item.fullPinyin, now())
                          << " item_boost=" << item.learningBoost
                          << " consumed=" << item.consumedInputBytes;
                const auto [physicalPage, physicalSlot] = physicalPosition(c.page(), index(c.page(), target));
                std::cout << " physical_page=" << physicalPage + 1 << " local_slot=" << physicalSlot + 1 << '\n';
                for (double font : {16.0, 20.0, 28.0}) for (double panel : {360.0, 610.0, 900.0}) {
                    const auto position = physicalPosition(c.page(), index(c.page(), target), font, panel);
                    if (n >= 10) check(position.first == 0, "learned target did not reach physical first page");
                }
            }
        }
        check(host.commits.size() == 50, "commit count");
    }
    pinyin::PinyinCandidateProvider reopened(p);
    Host host;
    Controller c(host, &reopened);
    type(c, input);
    const auto &item = c.page().items.at(index(c.page(), target));
    std::cout << " reopened_rank=" << index(c.page(), target) + 1
              << " item_boost=" << item.learningBoost << '\n';
}
void layout(const fs::path &root) {
    auto p = paths(root);
    pinyin::PinyinCandidateProvider provider(p);
    Host host;
    fcitx5::ControllerOptions options;
    options.learningEnabled = false;
    options.contextLearningEnabled = false;
    Controller c(host, &provider, options);
    // Keep Provider and Controller flags aligned, as the Fcitx adapter does.
    c.setOptions(options);
    std::size_t geometryChecks = 0, selectionChecks = 0, configurations = 0;
    for (const std::string input : {"shi", "jian", "nihaoshijie", "gara"}) {
        c.reset(); type(c, input);
        const auto original = c.page();
        for (double font : {16.0, 20.0, 28.0}) for (double panelWidth : {360.0, 610.0, 900.0}) {
            ++configurations;
            auto metrics = ui::CandidateBarMetrics::reference(font, 9);
            metrics.panelWidth = panelWidth;
            auto measure = [font](std::string_view s) { return width(s, font); };
            const auto boundaries = ui::CandidatePagination::partition(original.items, metrics, measure);
            std::size_t expected = 0;
            for (const auto &b : boundaries) {
                check(b.begin == expected && b.begin < b.end && b.end <= original.items.size(), "bad coverage");
                expected = b.end;
                core::CandidatePage local;
                local.items.assign(original.items.begin() + b.begin, original.items.begin() + b.end);
                for (std::size_t cursor = 0; cursor < local.items.size(); ++cursor) {
                    local.cursor = cursor;
                    auto view = ui::CandidateBarLayout::measure(local, metrics, measure);
                    check(view.candidates.size() == local.items.size(), "pagination/render count mismatch");
                    double right = metrics.panelX + metrics.horizontalPadding;
                    for (std::size_t i = 0; i < view.candidates.size(); ++i) {
                        const auto &r = view.candidates[i].bounds;
                        check(r.x + 0.001 >= right, "overlap");
                        check(r.x + r.width <= metrics.panelX + panelWidth - metrics.horizontalPadding + 0.001, "overflow");
                        check(ui::hitTestCandidate(view, metrics, r.x + r.width / 2, r.y + r.height / 2) == static_cast<int>(i), "hit mapping");
                        right = r.x + r.width;
                        ++geometryChecks;
                    }
                }
            }
            check(expected == original.items.size(), "lost candidate");
            for (std::size_t page : {std::size_t{0}, boundaries.size() / 2, boundaries.size() - 1}) {
                const auto b = boundaries[page];
                for (bool digit : {false, true}) {
                    c.reset(); type(c, input);
                    check(c.page().items.size() == original.items.size(), "disabled-learning changed count");
                    for (std::size_t i = 0; i < original.items.size(); ++i)
                        check(c.page().items[i].text == original.items[i].text, "disabled-learning changed order");
                    c.setPageBoundaries(boundaries);
                    if (page) check(c.movePage(page), "page move");
                    const std::size_t localIndex = digit ? b.end - b.begin - 1 : 0;
                    const auto expectedText = original.items[b.begin + localIndex].text;
                    if (digit) check(c.handle({Kind::Digit, 0, char('1' + localIndex)}), "digit select");
                    else {
                        core::CandidatePage local;
                        local.items.assign(original.items.begin() + b.begin, original.items.begin() + b.end);
                        auto view = ui::CandidateBarLayout::measure(local, metrics, measure);
                        const auto &r = view.candidates.front().bounds;
                        const int hit = ui::hitTestCandidate(view, metrics, r.x + r.width / 2, r.y + r.height / 2);
                        check(hit >= 0 && c.select(b.begin + hit), "click select");
                    }
                    check(host.commits.back() == expectedText, "page selection committed wrong candidate");
                    ++selectionChecks;
                }
            }
            std::cout << "LAYOUT input=" << input << " font=" << font << " width=" << panelWidth
                      << " items=" << original.items.size() << " pages=" << boundaries.size()
                      << " first_page=" << boundaries.front().end << '\n';
        }
    }
    std::cout << "LAYOUT PASS configurations=" << configurations << " geometry_checks=" << geometryChecks
              << " real_controller_selections=" << selectionChecks << '\n';
}
int main(int argc, char **argv) {
    if (argc != 2 && argc != 3) return 2;
    std::cout.setf(std::ios::unitbuf);
    try {
        const fs::path root = argv[1];
        check(!fs::exists(root) || fs::is_empty(root), "use a new empty directory");
        if (argc == 3) { layout(root / "layout"); return 0; }
        train(root / "shi-fresh", "shi", "试");
        train(root / "jian-fresh", "jian", "键");
        train(root / "shi-competing", "shi", "试", "是");
        train(root / "jian-competing", "jian", "见", "键");
        train(root / "partial-single", "nihaoshijie", "你");
        train(root / "partial-derived", "nihaoma", "你好");
        layout(root / "layout");
        std::cout << "Controller commit/consumption/persistence/layout checks PASS; ranking observations above are not UX acceptance passes.\n";
    } catch (const std::exception &e) { std::cerr << "FAILED: " << e.what() << '\n'; return 1; }
}
