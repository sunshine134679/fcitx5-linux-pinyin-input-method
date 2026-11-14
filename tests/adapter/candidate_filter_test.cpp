#include "modernime/fcitx5/engine.h"
#include "modernime/pinyin/pinyin_candidate_provider.h"
#include "modernime/pinyin/pinyin_shared_resources.h"
#include "modernime/core/candidate_filter.h"
#include "modernime/core/learning_writer.h"
#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <unistd.h>
using namespace modernime;
struct Host final : fcitx5::EngineHost {
    std::string committed;
    void publishPage(const core::CandidatePage &) override {}
    void commit(std::string_view s) override { committed += s; }
};

void testFilterPrefixEditing(fcitx5::ModernIMEController &controller) {
    for (const auto &[query, suffix] : {
             std::pair{"s:n", "nzhh"}, std::pair{"c:y", "yanshi"}}) {
        assert(controller.setCandidateFilter(query));
        assert(controller.handle({fcitx5::KeyKind::Backspace}));
        const auto prefix = std::string(query).substr(0, 2);
        assert(controller.page().preedit.ends_with("〔筛选：" + prefix + "〕"));
        assert(controller.page().rawInput == "shi");
        assert(controller.page().items.front().text == "是");
        for (const char c : std::string(suffix))
            assert(controller.handle({fcitx5::KeyKind::Character, c}));
        assert(controller.page().items.front().text == "试");
        for (std::size_t i = 0; i < std::string(suffix).size() + 2; ++i)
            assert(controller.handle({fcitx5::KeyKind::Backspace}));
        assert(controller.page().preedit.ends_with("〔筛选：〕"));
        assert(controller.handle({fcitx5::KeyKind::Backspace}));
        assert(!controller.candidateFiltering());
    }
}

void testFilteredCoreDefaultLearning(const std::filesystem::path &root) {
    std::filesystem::create_directories(root);
    pinyin::PinyinDataPaths paths;
    paths.userDictionary = (root / "dictionary.tsv").string();
    paths.learningStore = (root / "learning.sqlite3").string();
    paths.dictionaryPacks = (root / "packs").string();
    pinyin::PinyinProviderOptions options;
    options.contextLearningEnabled = false;
    pinyin::PinyinCandidateProvider provider(paths, options);
    Host host;
    fcitx5::ControllerOptions controllerOptions;
    controllerOptions.contextLearningEnabled = false;
    fcitx5::ModernIMEController controller(host, &provider, controllerOptions);
    controller.setCandidateFilterIndex(std::make_shared<const core::CandidateFilterIndex>());
    auto *writer = provider.sharedResources()->ensureLearningWriter(false);
    assert(writer != nullptr);
    const auto frequency = [&] {
        const auto snapshot = writer->snapshot();
        const auto *entry = snapshot->entry("你", "ni", "", "");
        return entry ? entry->frequency : 0;
    };
    const auto type = [&] {
        for (const char c : std::string("ni"))
            assert(controller.handle({fcitx5::KeyKind::Character, c}));
    };
    const auto find = [&](std::string_view text) {
        const auto &items = controller.page().items;
        std::size_t index = 0;
        while (index < items.size() && items[index].text != text) ++index;
        assert(index < items.size());
        return index;
    };

    // Ordinary default selection and merely opening an empty filter must
    // preserve the existing no-context core-default learning policy.
    for (const bool emptyFilter : {false, true}) {
        type();
        assert(controller.page().items.front().text == "你");
        if (emptyFilter) assert(controller.handle({fcitx5::KeyKind::OpenCandidateFilter}));
        assert(controller.handle({fcitx5::KeyKind::Space}));
        assert(provider.flush());
        assert(frequency() == 0);
    }

    for (int i = 0; i < 20; ++i) {
        type();
        assert(controller.select(find("泥")));
    }
    controller.reset();
    type();
    assert(controller.page().items.front().text == "泥");
    assert(find("你") > 0);

    for (int i = 1; i <= 10; ++i) {
        if (i > 1) type();
        assert(controller.setCandidateFilter("c:rener"));
        assert(controller.page().items.front().text == "你");
        assert(controller.page().items.front().consumedInputBytes == 2);
        host.committed.clear();
        assert(controller.handle({fcitx5::KeyKind::Space}));
        assert(host.committed == "你" && controller.page().rawInput.empty());
        assert(provider.flush());
        assert(frequency() == i);
    }
    type();
    assert(controller.page().items.front().text == "你");
    assert(controller.currentPageIndex() == 0);
    host.committed.clear();
    assert(controller.handle({fcitx5::KeyKind::Digit, '\0', '1'}));
    assert(host.committed == "你" && controller.page().rawInput.empty());
    assert(provider.flush());
}

int main() {
    auto root = std::filesystem::temp_directory_path() / ("modernime-filter-" + std::to_string(getpid()));
    std::filesystem::create_directories(root);
    pinyin::PinyinDataPaths paths;
    paths.userDictionary = (root / "dictionary.tsv").string();
    paths.learningStore = (root / "learning.sqlite3").string();
    pinyin::PinyinCandidateProvider provider(paths);
    Host host;
    fcitx5::ModernIMEController controller(host, &provider);
    controller.setCandidateFilterIndex(std::make_shared<const core::CandidateFilterIndex>());
    for(char c : std::string("shi")) assert(controller.handle({fcitx5::KeyKind::Character,c}));
    assert(controller.page().items.front().text == "是");
    testFilterPrefixEditing(controller);
    auto old = controller.page().generation;
    assert(controller.handle({fcitx5::KeyKind::OpenCandidateFilter}));
    for(char c : std::string("nzhh")) assert(controller.handle({fcitx5::KeyKind::Character,c}));
    assert(controller.candidateFiltering());
    assert(controller.page().rawInput == "shi");
    assert(controller.page().generation != old);
    assert(!controller.page().items.empty());
    assert(controller.page().items.front().text == "试");
    assert(controller.page().items.front().consumedInputBytes == 3);
    assert(controller.handle({fcitx5::KeyKind::Escape}));
    assert(!controller.candidateFiltering() && controller.page().rawInput == "shi");
    assert(controller.page().items.front().text == "是");
    assert(controller.handle({fcitx5::KeyKind::OpenCandidateFilter}));
    assert(controller.setCandidateFilter("s:nzhh"));
    assert(controller.handle({fcitx5::KeyKind::Space}));
    assert(host.committed == "试" && controller.page().rawInput.empty());
    for(char c : std::string("shi")) controller.handle({fcitx5::KeyKind::Character,c});
    assert(controller.setCandidateFilter("s:nzhh"));
    controller.setSensitive(true);
    assert(!controller.candidateFiltering());
    assert(!controller.setCandidateFilter("s:nzhh"));
    assert(provider.flush());

    // Restore updates the same Writer before the host's ready notification.
    // Its new reset generation must still reach every existing Provider.
    provider.replaceInput("shi");
    const auto beforeRestore = provider.page();
    auto shared = provider.sharedResources();
    auto *writer = shared->ensureLearningWriter(false);
    assert(writer != nullptr);
    assert(writer->restoreTrustedEntries({{"试", "shi", {}, {}, 3, 1000, 0, true}}));
    const auto changed = shared->refreshLearningStore();
    assert(changed.changed && changed.learningRevision == writer->resetRevision());
    assert(!provider.select(0)); // The old displayed page has lost authorization.
    const auto &afterRestore = provider.page();
    assert(afterRestore.generation != beforeRestore.generation);
    assert(afterRestore.rawInput == "shi");
    for (const auto &item : afterRestore.items) assert(item.text != "试");
    testFilteredCoreDefaultLearning(root / "filtered-learning");
    std::filesystem::remove_all(root);
    std::cout << "controller candidate filtering passed\n";
}
