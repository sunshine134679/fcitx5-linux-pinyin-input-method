#pragma once

#include "modernime/core/candidate_ranker.h"
#include <libime/core/languagemodel.h>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace libime { class PinyinContext; class SentenceResult; }
namespace modernime::core { class LearningSnapshot; }
namespace modernime::pinyin {

// Request-local model evidence and learned vocabulary. Never updates LibIME history.
class ContextDecoder final {
public:
    ContextDecoder(libime::PinyinContext &context, std::string_view before,
                   const core::LearningSnapshot *learning, std::int64_t nowMs,
                   std::shared_ptr<const libime::StaticLanguageModelFile> contextModel = nullptr);
    double gain(const libime::SentenceResult &sentence) const;
    std::vector<core::CandidateScore> candidates() const;
private:
    libime::PinyinContext &context_;
    libime::LanguageModel model_;
    libime::State baseline_;
    libime::State state_;
    libime::LanguageModel evidenceModel_;
    std::vector<libime::State> evidenceStates_;
    bool active_ = false;
    const core::LearningSnapshot *learning_;
    std::int64_t nowMs_;
};

} // namespace modernime::pinyin
