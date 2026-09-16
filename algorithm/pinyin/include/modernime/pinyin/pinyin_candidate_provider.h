#pragma once

#include "modernime/core/candidate_provider.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace modernime::pinyin {

struct PinyinDataPaths final {
    std::string dictionary = "/usr/share/libime/sc.dict";
    std::string languageModel = "/usr/lib/x86_64-linux-gnu/libime/zh_CN.lm";
    std::string extensionDictionary;
    // 网络热词词典（带正 cost 的独立小表）；简拼命中时排在组合候选前。
    std::string hotwordDictionary;
    std::string userDictionary;
    std::string learningStore;
};

struct PinyinProviderOptions final {
    bool learningEnabled = true;
    bool contextLearningEnabled = true;
};

class PinyinCandidateProvider final : public core::CandidateProvider {
public:
    // Heavyweight state (system/user/extension dictionaries, language model,
    // learning writer) intended to be created once per engine and shared by
    // every provider instance. Defined in the source file so libime stays out
    // of this header.
    class SharedResources;

    // Builds the shared resources; never returns null. options.learningEnabled
    // decides whether the shared learning writer exists.
    static std::shared_ptr<SharedResources> createSharedResources(
        const PinyinDataPaths &paths, const PinyinProviderOptions &options);

    // Re-reads the user dictionary file into the shared resources so
    // settings-client edits apply without restarting; false when null.
    static bool reloadUserDictionary(
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
    bool remove(std::size_t index) override;
    void reset() override;
    const core::CandidatePage &page() const override;

    void setContext(std::string_view before, std::string_view after);
    // Runtime toggles for settings hot-reload; learning writer is created
    // lazily on first use after being enabled.
    void setLearningEnabled(bool enabled);
    void setContextLearningEnabled(bool enabled);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace modernime::pinyin
