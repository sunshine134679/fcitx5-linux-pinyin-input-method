#pragma once

#include "modernime/pinyin/pinyin_candidate_provider.h"
#include "modernime/pinyin/user_dictionary.h"

#include "modernime/core/english_definition_dictionary.h"
#include "modernime/core/learning_store.h"
#include "modernime/core/learning_writer.h"
#include "modernime/core/phrase_formation.h"

#include <libime/pinyin/pinyinime.h>

#include <cstdint>
#include <filesystem>
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
        const PinyinDataPaths &paths, const PinyinProviderOptions &options);

    PinyinSharedResources();
    virtual ~PinyinSharedResources();

    ResourceChange reloadUserDictionary();
    ResourceChange reloadLearningStore();
    ResourceChange refreshLearningStore();

    std::shared_ptr<const core::LearningSnapshot> visibleLearning() const;
    core::LearningWriter *ensureLearningWriter();
    core::EnglishDefinitionDictionary *ensureEnglishDefinitionDictionary();
    bool reloadVisibility();

    core::EpochViewState epochViewState() const;
    void prepareLearningWriterAsync();

    std::uint64_t dictionaryGeneration() const noexcept { return dictionaryGeneration_; }
    std::int64_t learningRevision() const noexcept { return learningRevision_; }

    std::unique_ptr<libime::PinyinIME> ime;
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
    std::uint64_t dictionaryGeneration_ = 1;
    std::int64_t learningRevision_ = 0;
    mutable std::recursive_mutex resourceMutex_;
    std::thread writerInitThread_;
    bool preparingWriter_ = false;
};

class PinyinCandidateProvider::SharedResources final : public PinyinSharedResources {
public:
    using PinyinSharedResources::PinyinSharedResources;
};

} // namespace modernime::pinyin
