#pragma once

#include "modernime/pinyin/pinyin_candidate_provider.h"
#include "modernime/pinyin/user_dictionary.h"
#include "modernime/pinyin/dictionary_pack.h"
#include "modernime/pinyin/mixed_word_index.h"

#include "modernime/core/english_definition_dictionary.h"
#include "modernime/core/learning_store.h"
#include "modernime/core/learning_writer.h"
#include "modernime/core/phrase_formation.h"

#include <libime/pinyin/pinyinime.h>
#include <libime/core/languagemodel.h>

#include <cstdint>
#include <atomic>
#include <utility>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>

namespace modernime::pinyin {

// PinyinSharedResources manages heavy engine-wide resources (PinyinIME,
// UserDictionary, LearningWriter/LearningStore, EnglishDefinitionDictionary).
//
// Threading contract:
// LibIME is not thread-safe. All PinyinIME operations (typing, candidate matching,
// dictionary modification) must be serialized on the engine/input thread.
// Shared resources must be loaded during engine initialization or on the dedicated
// engine thread. Concurrent calls to PinyinIME from multiple threads are not supported
// by LibIME; PinyinCandidateProvider instances sharing these resources must be accessed
// sequentially on the input processing thread.
class PinyinSharedResources {
public:
    static std::shared_ptr<PinyinSharedResources> create(
        const PinyinDataPaths &paths, const PinyinProviderOptions &options,
        bool initializeLearning = true);

    PinyinSharedResources();
    virtual ~PinyinSharedResources();

    ResourceChange reloadUserDictionary();
    ResourceChange reloadDictionaryPacks();
    // Timer/engine-thread entry points: the worker owns all preparation I/O;
    // consume only swaps ready resources and advances the dictionary generation.
    void requestDictionaryPackRefresh();
    ResourceChange consumeDictionaryPackRefresh();
    void retirePinyinEngine(std::shared_ptr<libime::PinyinIME> engine) {
        retiredPinyinEngines_.push_back(std::move(engine));
    }
    std::vector<DictionaryPackManifest> dictionaryPacks() const;
    const std::filesystem::path &dictionaryPacksPath() const noexcept { return dictionaryPacksPath_; }
    const std::filesystem::path &bundledDictionaryPacksPath() const noexcept { return bundledDictionaryPacksPath_; }
    bool importDictionaryPack(const std::filesystem::path &directory, bool replace = false,
                              std::string *error = nullptr);
    bool setDictionaryPackEnabled(std::string_view id, bool enabled, std::string *error = nullptr);
    bool removeDictionaryPack(std::string_view id, std::string *error = nullptr);
    MixedWordIndex mixedWords;
    ResourceChange reloadLearningStore();
    ResourceChange refreshLearningStore();

    // Engine-wide parsing configuration; read/written only on the input thread.
    bool setFuzzyOptions(const core::FuzzyPinyinOptions &options);
    bool setPinyinScheme(core::PinyinScheme scheme);
    core::PinyinScheme pinyinScheme() const noexcept { return pinyinScheme_; }
    bool usesShuangpin() const noexcept { return pinyinScheme_ != core::PinyinScheme::Full; }
    std::uint64_t inputConfigurationGeneration() const noexcept {
        return inputConfigurationGeneration_;
    }

    std::shared_ptr<const core::LearningSnapshot> visibleLearning() const;
    // Input-thread boundary: reconcile shared formation counts once, before a
    // Provider records new commits. No storage I/O or per-Provider state here.
    std::uint64_t refreshLearningForgetState();
    core::ForgottenPreferences forgottenSince(std::uint64_t generation) const;
    core::LearningWriter *ensureLearningWriter(bool synchronousInitialLoad = true);
    core::EnglishDefinitionDictionary *ensureEnglishDefinitionDictionary();
    bool reloadVisibility();

    core::EpochViewState epochViewState() const;
    void prepareLearningWriterAsync();
    void setOnReadyCallback(std::function<void()> callback);

    std::uint64_t dictionaryGeneration() const noexcept {
        return dictionaryGeneration_.load(std::memory_order_acquire);
    }
    std::int64_t learningRevision() const noexcept {
        return learningRevision_.load(std::memory_order_acquire);
    }

    std::shared_ptr<libime::PinyinIME> ime;
    std::shared_ptr<const libime::StaticLanguageModelFile> contextLanguageModel;
    std::filesystem::path contextLanguageModelPath;
    std::unique_ptr<core::LearningWriter> learning;
    UserDictionary userDictionary;
    std::filesystem::path userDictionaryPath;
    std::filesystem::path learningStorePath;
    core::EnglishDefinitionDictionary englishDefinitionDict;
    std::filesystem::path englishDefinitionDictPath;
    std::shared_ptr<core::PhraseFormationStats> phraseStats =
        std::make_shared<core::PhraseFormationStats>();

    std::unique_ptr<core::LearningStore> visibilityReader;
    std::shared_ptr<const core::LearningSnapshot> visibilitySnapshot;
    std::int64_t visibilityRevision = 0;

private:
    std::uint64_t inputConfigurationGeneration_ = 0;
    core::PinyinScheme pinyinScheme_ = core::PinyinScheme::Full;
    std::atomic<std::uint64_t> dictionaryGeneration_{1};
    std::atomic<std::int64_t> learningRevision_{0};
    std::uint64_t phraseStatsForgetGeneration_ = 0;
    std::function<void()> onReadyCallback_;
    mutable std::recursive_mutex resourceMutex_;
    bool preparingWriter_ = false;
    std::filesystem::path dictionaryPacksPath_, bundledDictionaryPacksPath_;
    std::size_t dictionaryPackLayer_ = 0;
    std::vector<DictionaryPack> activeDictionaryPacks_;
    PinyinDataPaths enginePaths_;
    struct PreparedPackRefresh {
        std::uint64_t stamp = 0, baselineGeneration = 0;
        std::shared_ptr<PinyinSharedResources> resources;
    };
    std::future<PreparedPackRefresh> pendingPackRefresh_;
    std::uint64_t dictionaryPackStamp_ = 0;
    std::vector<std::shared_ptr<PinyinSharedResources>> retiredPackResources_;
    std::vector<std::shared_ptr<libime::PinyinIME>> retiredPinyinEngines_;
};

class PinyinCandidateProvider::SharedResources final : public PinyinSharedResources {
public:
    using PinyinSharedResources::PinyinSharedResources;
};

} // namespace modernime::pinyin
