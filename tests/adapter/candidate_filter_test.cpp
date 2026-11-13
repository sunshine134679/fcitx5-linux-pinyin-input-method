#include "modernime/fcitx5/engine.h"
#include "modernime/pinyin/pinyin_candidate_provider.h"
#include "modernime/pinyin/pinyin_shared_resources.h"
#include "modernime/core/candidate_filter.h"
#include "modernime/core/learning_writer.h"
#include <cassert>
#include <filesystem>
#include <iostream>
#include <unistd.h>
using namespace modernime;
struct Host final : fcitx5::EngineHost {
    std::string committed;
    void publishPage(const core::CandidatePage &) override {}
    void commit(std::string_view s) override { committed += s; }
};
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
    std::filesystem::remove_all(root);
    std::cout << "controller candidate filtering passed\n";
}
