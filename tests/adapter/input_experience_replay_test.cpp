#include "modernime/fcitx5/engine.h"
#include "modernime/pinyin/pinyin_candidate_provider.h"
#include "modernime/core/learning_store.h"
#include "modernime/ui/candidate_bar_layout.h"
#include "modernime/ui/candidate_pagination.h"
#include "native_pinyin_test_baseline.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

using namespace modernime;
namespace fs = std::filesystem;
using Controller = fcitx5::ModernIMEController;
using Kind = fcitx5::KeyKind;
namespace {
struct Scenario {
    std::string id, category, input, target, scheme, remaining, prelude, origin;
    std::size_t maxRank = 0;
};
struct Host final : fcitx5::EngineHost {
    std::vector<std::string> commits;
    void publishPage(const core::CandidatePage &) override {}
    void commit(std::string_view s) override { commits.emplace_back(s); }
};
struct Counters {
    std::size_t events = 0, phases = 0, passed = 0, failed = 0;
    std::map<std::string, std::size_t> kinds, categories;
    std::map<std::string, std::vector<double>> latency;
    std::map<std::string, double> peakLatency;
    std::map<std::string, std::string> peakContext;
    double maxKeyMs = 0;
} totals;
std::ofstream report;
std::string phaseContext;
void sampleLatency(const std::string &group, double ms, const std::string &event) {
    totals.latency[group].push_back(ms);
    if (!totals.peakContext.contains(group) || ms > totals.peakLatency[group]) {
        totals.peakLatency[group] = ms;
        totals.peakContext[group] = phaseContext + ":" + event;
    }
}
void require(bool ok, const std::string &message) {
    if (!ok) throw std::runtime_error(message);
}
std::vector<std::string> split(const std::string &s) {
    std::vector<std::string> result;
    std::size_t start = 0, end;
    while ((end = s.find('\t', start)) != std::string::npos) {
        result.push_back(s.substr(start, end - start)); start = end + 1;
    }
    result.push_back(s.substr(start)); return result;
}
std::string safe(std::string s) {
    for (char &c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}
std::vector<Scenario> scenarios() {
    std::ifstream in(MODERNIME_INPUT_REPLAY_DATA_FILE);
    require(in.good(), "missing scenario corpus");
    std::vector<Scenario> result;
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') continue;
        const auto f = split(line);
        require(f.size() == 9, "bad corpus row");
        result.push_back({f[0], f[1], f[2], f[3], f[5], f[6], f[7], f[8], std::stoul(f[4])});
    }
    require(result.size() >= 200, "less than 200 independent scenarios");
    return result;
}
std::size_t find(const core::CandidatePage &p, const std::string &target) {
    for (std::size_t i = 0; i < p.items.size(); ++i) if (p.items[i].text == target) return i;
    return p.items.size();
}
std::string eventName(Kind kind) {
    switch (kind) {
    case Kind::Character: return "key";
    case Kind::Backspace: return "backspace";
    case Kind::MoveCompositionLeft: return "left";
    case Kind::MoveCompositionRight: return "right";
    case Kind::Escape: return "escape";
    case Kind::Enter: return "enter-raw";
    case Kind::Space: return "space-select";
    case Kind::Digit: return "digit-select";
    case Kind::NextPage: return "next-page";
    case Kind::PreviousPage: return "previous-page";
    case Kind::ToggleTemporaryEnglish: return "temporary-english";
    case Kind::OpenCalculator: return "open-calculator";
    case Kind::OpenSymbols: return "open-symbols";
    case Kind::OpenClipboard: return "open-clipboard";
    default: return "other";
    }
}
struct Replay {
    const Scenario &s;
    Controller &c;
    Host &host;
    std::string sequence;
    std::size_t startEvents = totals.events;
    bool editingCharacter = false;
    void record(const std::string &kind, const std::string &detail = {}) {
        ++totals.events; ++totals.kinds[kind];
        if (!sequence.empty()) sequence += ',';
        sequence += kind + (detail.empty() ? "" : ":" + detail);
    }
    void key(Kind kind, char character = 0, char digit = 0) {
        const auto name = eventName(kind);
        record(name, character ? std::string(1, character) : digit ? std::string(1, digit) : "");
        const auto begin = std::chrono::steady_clock::now();
        const bool handled = c.handle({kind, character, digit});
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        totals.maxKeyMs = std::max(totals.maxKeyMs, ms);
        const bool numericMacro = s.input.size() > 1 && s.input[0] == 'v' && s.input[1] >= '0' && s.input[1] <= '9';
        const std::string group = editingCharacter || kind == Kind::Backspace || kind == Kind::MoveCompositionLeft || kind == Kind::MoveCompositionRight || kind == Kind::DeleteForward ? "editing" :
            kind == Kind::Character && !c.auxiliaryInputActive() && !c.clipboardMode() && !numericMacro ? "ordinary_character" : "explicit_operation";
        sampleLatency(group, ms, name + (character ? std::string(":") + character : ""));
        require(handled, "event rejected: " + name);
    }
    void type(const std::string &text, bool exact = true) {
        const auto initial = c.page().rawInput;
        for (std::size_t i = 0; i < text.size(); ++i) {
            key(Kind::Character, text[i]);
            if (exact) require(c.page().rawInput == initial + text.substr(0, i + 1), "per-key raw input mismatch");
        }
    }
    void auxiliary() {
        if (s.prelude == "none") return;
        const auto before = host.commits.size();
        if (s.prelude == "temporary") {
            key(Kind::ToggleTemporaryEnglish); type("User42");
            require(c.temporaryEnglishMode(), "temporary mode not entered");
        } else if (s.prelude == "calculator") {
            key(Kind::OpenCalculator); type("6*7");
            require(c.page().items.front().text == "42", "calculator result mismatch");
        } else if (s.prelude == "symbols") {
            key(Kind::OpenSymbols); type("jiantou");
            require(!c.page().items.empty(), "symbol query missing");
        } else if (s.prelude == "clipboard") {
            c.setClipboardEntries({"隔离剪贴板一", "isolated-clipboard-two"});
            key(Kind::OpenClipboard); require(c.clipboardMode(), "clipboard not entered");
        }
        key(Kind::Escape);
        require(!c.auxiliaryInputActive() && !c.clipboardMode() && c.page().rawInput.empty(), "auxiliary cancel leaked mode or input");
        require(host.commits.size() == before, "cancel unexpectedly committed auxiliary input");
    }
    void choose(std::size_t index, bool digit) {
        auto metrics = ui::CandidateBarMetrics::reference(20, 9);
        metrics.panelWidth = 610;
        const auto measure = [](std::string_view t) {
            double width = 0;
            for (const unsigned char ch : t) if ((ch & 0xc0) != 0x80) width += ch < 128 ? 11 : 20;
            return width;
        };
        const auto boundaries = ui::CandidatePagination::partition(c.page().items, metrics, measure);
        c.setPageBoundaries(boundaries);
        std::size_t page = 0;
        while (page < boundaries.size() && index >= boundaries[page].end) ++page;
        require(page < boundaries.size(), "selection omitted by pagination");
        for (std::size_t p = 0; p < page; ++p) key(Kind::NextPage);
        require(c.currentPageIndex() == page, "physical page cursor mismatch");
        const auto slot = index - boundaries[page].begin;
        if (digit && !s.input.starts_with("v")) {
            require(slot < 9, "physical slot exceeds number key range");
            key(Kind::Digit, 0, static_cast<char>('1' + slot));
        } else if (digit && s.input.starts_with("v")) {
            require(index == 0, "numeric macro keyboard selection expects first candidate");
            key(Kind::Space);
        } else {
            core::CandidatePage local;
            const auto b = boundaries[page];
            local.items.assign(c.page().items.begin() + b.begin, c.page().items.begin() + b.end);
            const auto layout = ui::CandidateBarLayout::measure(local, metrics, measure);
            const auto rect = layout.candidates.at(slot).bounds;
            const auto hit = ui::hitTestCandidate(layout, metrics, rect.x + rect.width / 2, rect.y + rect.height / 2);
            require(hit == static_cast<int>(slot), "candidate hit target mismatch");
            record("pointer-select", std::to_string(index));
            const auto begin = std::chrono::steady_clock::now();
            const bool selected = c.select(b.begin + static_cast<std::size_t>(hit));
            sampleLatency("explicit_operation", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count(), "pointer-select");
            require(selected, "pointer selection rejected");
        }
    }
};
pinyin::PinyinDataPaths paths(const fs::path &directory, const std::string &backend) {
    fs::create_directories(directory);
    std::ofstream(directory / "user.txt");
    pinyin::PinyinDataPaths p;
    p.learningStore = (directory / (backend == "lmdb" ? "learning.lmdb" : "learning.sqlite3")).string();
    p.userDictionary = (directory / "user.txt").string();
    p.extensionDictionary = MODERNIME_PINYIN_KNOWLEDGE_BUILD_BINARY;
    p.hotwordDictionary = MODERNIME_PINYIN_HOTWORDS_BUILD_BINARY;
    p.dictionaryPacks = (directory / "absent-packs").string();
    p.bundledDictionaryPacks = (directory / "absent-bundled-packs").string();
    return p;
}
void run(const Scenario &original, const std::string &backend, const fs::path &directory,
         std::size_t ordinal, NativePinyinTestBaseline &native) {
    Scenario s = original;
    if (s.target.starts_with("@native:")) s.target = native.candidate(s.target.substr(8));
    if (native.hasAuditedLegacyProfile() && s.origin == "daily") {
        if (s.target == "死锁") s.maxRank = 3;
        if (s.target == "破折号") s.maxRank = 15;
    }
    const auto p = paths(directory, backend);
    const std::string policy = s.prelude != "none" ? "off" : ordinal % 3 == 0 ? "on" : ordinal % 3 == 1 ? "off" : "sensitive";
    pinyin::PinyinProviderOptions opts;
    opts.learningEnabled = policy != "off";
    opts.contextLearningEnabled = false;
    opts.pinyinScheme = s.scheme == "xiaohe" ? core::PinyinScheme::Xiaohe : s.scheme == "ziranma" ? core::PinyinScheme::Ziranma : core::PinyinScheme::Full;
    fcitx5::ControllerOptions copts;
    copts.learningEnabled = opts.learningEnabled;
    copts.contextLearningEnabled = false;
    for (const std::string phase : {"cold-edit", "independent-reopen"}) {
        phaseContext = s.id + ":" + backend + ":" + phase;
        pinyin::PinyinCandidateProvider provider(p, opts);
        Host host;
        Controller controller(host, &provider, copts);
        controller.setOptions(copts);
        Replay r{s, controller, host, {}};
        ++totals.phases; ++totals.categories[s.category];
        std::string error, candidates, remainder, top, actualPinyin, paginationProbe;
        std::size_t extraCommits = 0;
        std::size_t index = 0, consumed = 0, physicalPage = 0, physicalSlot = 0;
        int source = -1;
        try {
            if (phase == "cold-edit") r.auxiliary();
            if (policy == "sensitive") controller.setSensitive(true);
            controller.setContext("", "");
            r.type(s.input);
            require(!controller.page().items.empty(), "no candidates");
            if (phase == "cold-edit") {
                const auto old = controller.page();
                const core::CandidateSelectionRequest stale{old.sessionId, old.displayGeneration, 0, core::candidateIdentity(old.items[0], old)};
                r.key(Kind::MoveCompositionLeft);
                require(controller.page().rawInput == s.input, "left changed composition");
                r.key(Kind::MoveCompositionRight);
                require(controller.page().rawInput == s.input, "right changed composition");
                const char editKey = s.input.starts_with("v") && s.input.size() > 1 && s.input[1] >= '0' && s.input[1] <= '9' ? '0' : 'x';
                r.editingCharacter = true;
                r.key(Kind::Character, editKey);
                r.editingCharacter = false;
                require(controller.page().rawInput == s.input + editKey, "insert did not preserve raw spelling");
                r.key(Kind::Backspace);
                require(controller.page().rawInput == s.input, "backspace failed to restore raw spelling");
                r.record("stale-selection-reject");
                const auto rejectBegin = std::chrono::steady_clock::now();
                const bool rejected = !controller.select(stale);
                sampleLatency("explicit_operation", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - rejectBegin).count(), "stale-selection-reject");
                require(rejected && host.commits.empty(), "stale display selected a current candidate");
            }
            if (phase == "cold-edit" && policy == "off") {
                const auto before = controller.page();
                auto m = ui::CandidateBarMetrics::reference(20, 9); m.panelWidth = 610;
                const auto pages = ui::CandidatePagination::partition(before.items, m, [](std::string_view t) { double w=0; for (const unsigned char ch:t) if((ch&0xc0)!=0x80) w+=ch<128?11:20; return w; });
                if (pages.size() > 1) {
                    const auto global = pages[1].begin;
                    const auto displayed = before.items.at(global);
                    const auto used = displayed.consumedInputBytes ? displayed.consumedInputBytes : s.input.size();
                    require(used <= s.input.size(), "second page candidate has invalid consumption");
                    r.choose(global, ordinal % 2 == 0);
                    require(host.commits.size() == 1 && host.commits.back() == displayed.text, "second page displayed candidate committed different text");
                    require(controller.page().rawInput == s.input.substr(used), "second page selection consumed wrong raw range");
                    paginationProbe = "page=2;slot=1;global=" + std::to_string(global) + ";text=" + displayed.text + ";source=" + std::to_string(static_cast<int>(displayed.source)) + ";consumed=" + std::to_string(used) + ";remaining=" + controller.page().rawInput;
                    if (!controller.page().rawInput.empty()) r.key(Kind::Escape);
                    require(!controller.auxiliaryInputActive() && controller.page().rawInput.empty(), "second page selection leaked composition");
                    extraCommits = 1;
                    controller.reset(); controller.setContext("", "");
                    r.type(s.input);
                }
            }
            const auto page = controller.page();
            top = page.items.front().text;
            index = find(page, s.target);
            for (std::size_t n = 0; n < page.items.size(); ++n) {
                if (!candidates.empty()) candidates += '|';
                candidates += std::to_string(n) + ':' + page.items[n].text + ':' + std::to_string(static_cast<int>(page.items[n].source)) + ':' + std::to_string(page.items[n].consumedInputBytes);
            }
            require(index < page.items.size(), "target missing: " + s.target + " top=" + top);
            require(index <= s.maxRank, "rank " + std::to_string(index) + " exceeds original limit " + std::to_string(s.maxRank) + " top=" + top);
            const auto selected = page.items[index];
            source = static_cast<int>(selected.source);
            actualPinyin = selected.fullPinyin;
            require(selected.source != core::CandidateSource::None && selected.action == core::SelectionAction::CommitText, "target is not committable");
            consumed = selected.consumedInputBytes ? selected.consumedInputBytes : s.input.size();
            require(consumed <= s.input.size() && s.input.substr(consumed) == s.remaining, "candidate consumption differs from independent expected suffix");
            // Record the same default reference geometry used for the actual number/hit mapping.
            auto metrics = ui::CandidateBarMetrics::reference(20, 9); metrics.panelWidth = 610;
            const auto b = ui::CandidatePagination::partition(page.items, metrics, [](std::string_view t) { double w=0; for (const unsigned char ch:t) if((ch&0xc0)!=0x80) w+=ch<128?11:20; return w; });
            while (physicalPage < b.size() && index >= b[physicalPage].end) ++physicalPage;
            physicalSlot = index - b.at(physicalPage).begin;
            r.choose(index, (ordinal + (phase == "cold-edit" ? 0 : 1)) % 2 == 0);
            require(host.commits.size() == extraCommits + 1 && host.commits.back() == s.target, "selected displayed text did not commit exactly once");
            remainder = controller.page().rawInput;
            require(remainder == s.remaining, "committed candidate consumed wrong raw range");
            if (!remainder.empty()) {
                r.key(Kind::Escape);
                require(controller.page().rawInput.empty() && host.commits.size() == extraCommits + 1, "cancel suffix changed committed text");
            }
            require(provider.flush(), "flush failed");
            if (policy != "on") {
                core::LearningStore store(p.learningStore);
                if (fs::exists(p.learningStore)) {
                    require(store.openReadOnly(), "isolated no-learning store cannot open");
                    require(store.entryCount() == std::optional<std::size_t>(0), "disabled/sensitive selection persisted learning");
                }
            }
            if (phase == "independent-reopen") {
                controller.reset(); controller.setContext("", "");
                r.type(s.input);
                r.key(Kind::Enter);
                require(host.commits.size() == extraCommits + 2 && host.commits.back() == s.input && controller.page().rawInput.empty(), "Enter did not commit original raw input");
                require(provider.flush(), "raw commit flush failed");
            }
        } catch (const std::exception &e) { error = e.what(); }
        const bool passed = error.empty();
        if (passed) ++totals.passed; else ++totals.failed;
        report << s.id << '\t' << s.category << '\t' << backend << '\t' << policy << '\t' << phase << '\t' << s.scheme << '\t' << s.input << '\t' << s.target << '\t' << s.maxRank << '\t' << index << '\t' << source << '\t' << physicalPage+1 << '\t' << physicalSlot+1 << '\t' << consumed << '\t' << safe(remainder) << '\t' << safe(top) << '\t' << safe(actualPinyin) << '\t' << safe(host.commits.size() <= extraCommits ? "" : host.commits[extraCommits]) << '\t' << totals.events-r.startEvents << '\t' << (passed?"PASS":"FAIL") << '\t' << safe(error) << '\t' << safe(r.sequence) << '\t' << safe(candidates) << '\t' << safe(paginationProbe) << '\n';
        report.flush();
        if (!passed) std::cerr << s.id << ' ' << backend << ' ' << phase << " FAIL " << error << '\n';
    }
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc != 2 && argc != 3) throw std::runtime_error("usage: replay NEW_EMPTY_EVIDENCE_DIR [CASE_ID]");
        const fs::path parent = fs::absolute(argv[1]);
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path root = parent / ("run-" + std::to_string(stamp) + "-" + std::to_string(getpid()));
        require(!fs::exists(root), "unique evidence directory already exists");
        std::cout << "EVIDENCE " << root << std::endl;
        fs::create_directories(root);
        for (const auto &name : {"home", "config", "data", "cache", "state"}) fs::create_directories(root/name);
        for (const auto &[name, dir] : std::vector<std::pair<std::string,std::string>>{{"HOME","home"},{"XDG_CONFIG_HOME","config"},{"XDG_DATA_HOME","data"},{"XDG_CACHE_HOME","cache"},{"XDG_STATE_HOME","state"}})
            require(setenv(name.c_str(), (root/dir).c_str(), 1) == 0, "cannot isolate " + name);
        report.open(root/"回放明细.tsv");
        report << "case\tcategory\tbackend\tpolicy\tphase\tscheme\tinput\ttarget\tmax_rank_zero_based\tglobal_rank_zero_based\tsource\tphysical_page\tphysical_slot\tconsumed\tremaining\ttop\tpinyin\tcommit\tactual_events\tresult\terror\tevent_sequence\tcandidates\tpagination_probe\n";
        const auto corpus = scenarios();
        NativePinyinTestBaseline native;
        std::size_t executedCases = 0;
        for (std::size_t i=0;i<corpus.size();++i) {
            const auto &s = corpus[i];
            if (argc == 3 && s.id != argv[2]) continue;
            ++executedCases;
            for (const std::string backend : {"sqlite", "lmdb"}) run(s, backend, root/backend/s.id, i, native);
            if (executedCases % 20 == 0) std::cout << "cases=" << executedCases << " phases=" << totals.phases << " events=" << totals.events << " failures=" << totals.failed << std::endl;
        }
        require(executedCases != 0, "unknown case id");
        std::ofstream out(root/"汇总.json");
        out << "{\n  \"seed\": 20261113,\n  \"scenario_count\": " << executedCases << ",\n  \"backend_count\": 2,\n  \"phase_count\": " << totals.phases << ",\n  \"passed\": " << totals.passed << ",\n  \"failed\": " << totals.failed << ",\n  \"actual_events\": " << totals.events << ",\n  \"max_controller_event_ms\": " << totals.maxKeyMs << ",\n  \"event_counts\": {";
        bool first=true; for (const auto &[kind,count]:totals.kinds) { if(!first)out<<','; first=false; out << "\n    \"" << kind << "\": " << count; }
        out << "\n  },\n  \"category_phase_counts\": {";
        first=true; for (const auto &[kind,count]:totals.categories) { if(!first)out<<','; first=false; out << "\n    \"" << kind << "\": " << count; }
        out << "\n  },\n  \"latency_ms\": {";
        first=true;
        for (auto &[group, samples] : totals.latency) {
            std::sort(samples.begin(), samples.end());
            const auto percentile = [&](std::size_t percent) { return samples.at((samples.size() * percent + 99) / 100 - 1); };
            if (!first) out << ',';
            first=false;
            out << "\n    \"" << group << "\": {\"count\":" << samples.size() << ",\"p50\":" << percentile(50) << ",\"p95\":" << percentile(95) << ",\"p99\":" << percentile(99) << ",\"max\":" << samples.back() << ",\"max_context\":\"" << totals.peakContext[group] << "\"}";
        }
        out << "\n  },\n  \"physical_geometry\": \"reference 20px 610px, headless pagination and hit test\",\n  \"desktop_application_test\": false\n}\n";
        std::cout << "SUMMARY scenarios=" << executedCases << " phases=" << totals.phases << " events=" << totals.events << " passed=" << totals.passed << " failed=" << totals.failed << '\n';
        if (argc == 2) require(totals.events >= 10000, "fewer than 10000 meaningful replayed events");
        return totals.failed ? 1 : 0;
    } catch (const std::exception &e) { std::cerr << "replay fatal: " << e.what() << '\n'; return 2; }
}
