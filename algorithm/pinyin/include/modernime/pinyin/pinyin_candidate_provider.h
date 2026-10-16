#pragma once

#include "modernime/core/candidate_provider.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace modernime::pinyin {

// 返回当前系统探测到的默认拼音词典路径（支持多发行版探测与环境变量覆盖）
std::string defaultDictionaryPath();

// 返回当前系统探测到的默认拼音语言模型路径（支持多发行版探测与环境变量覆盖）
std::string defaultLanguageModelPath();

struct PinyinDataPaths final {
    std::string dictionary = defaultDictionaryPath();
    std::string languageModel = defaultLanguageModelPath();
    std::string extensionDictionary;
    // 网络热词词典（带正 cost 的独立小表）；简拼命中时排在组合候选前。
    std::string hotwordDictionary;
    std::string userDictionary;
    std::string learningStore;
    std::string englishDefinitionDictionary;
};

struct PinyinProviderOptions final {
    bool learningEnabled = true;
    bool contextLearningEnabled = true;
    bool englishDefinitionEnabled = false;
    bool clipboardEnabled = true;
    std::string clipboardTrigger = "V+2";
};

class PinyinCandidateProvider final : public core::CandidateProvider {
public:
    // Heavyweight state (system/user/extension dictionaries, language model,
    // learning writer) intended to be created once per engine and shared by
    // every provider instance. Defined in the source file so libime stays out
    // of this header.
    class SharedResources;

    // Builds the shared resources; never returns null. options.learningEnabled
    // decides whether the shared learning writer exists. Disabled resources
    // load existing suppression through a read-only store without creating a DB.
    static std::shared_ptr<SharedResources> createSharedResources(
        const PinyinDataPaths &paths, const PinyinProviderOptions &options);

    // Re-reads the user dictionary file into the shared resources so
    // settings-client edits apply without restarting; false when null.
    static bool reloadUserDictionary(
        std::shared_ptr<SharedResources> &resources);

    // Re-reads the learning store from disk into the shared resources so
    // external clears/edits apply without restarting; false when null.
    static bool reloadLearningStore(
        std::shared_ptr<SharedResources> &resources);

    // Polls the SQLite reset marker; returns true only for an applied external clear.
    static bool refreshLearningStore(
        std::shared_ptr<SharedResources> &resources);

    // Constructs a provider that owns all of its resources. Convenience for
    // tests and single-instance tools.
    explicit PinyinCandidateProvider(PinyinDataPaths paths = {},
                                     PinyinProviderOptions options = {});
    // Shares the given resources across provider instances; each instance only
    // keeps per-context composition state. resources must not be null.
    explicit PinyinCandidateProvider(std::shared_ptr<SharedResources> shared,
                                     PinyinProviderOptions options = {});
    ~PinyinCandidateProvider() override;

    bool append(std::string_view input) override;
    bool eraseLast() override;
    bool replaceInput(std::string_view input) override;
    bool select(std::size_t index) override;
    std::optional<core::CandidateSelectionOutcome> selectConfirmed(
        const core::CandidateSelectionRequest &request,
        std::uint64_t providerGeneration) override;
    bool extractCharacter(std::size_t index, bool firstChar,
                          std::string *outText = nullptr) override;
    bool remove(std::size_t index) override;
    void onCommitEvent(const core::CommitEvent &event) override;
    void onNegativeFeedback(std::string_view phrase,
                            std::string_view pinyin) override;
    void reset() override;
    void resetCommitHistory() override;
    const core::CandidatePage &page() const override;

    bool flush();

    void setContext(std::string_view before, std::string_view after);
    void applyOptions(const PinyinProviderOptions &options);
    // Runtime toggles for settings hot-reload; learning writer is created
    // lazily on first use after being enabled.
    void setLearningEnabled(bool enabled);
    void setContextLearningEnabled(bool enabled);
    void setEnglishDefinitionEnabled(bool enabled);
    void setClipboardEnabled(bool enabled);
    void setClipboardTrigger(std::string trigger);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace modernime::pinyin
