#pragma once

#include "modernime/core/candidate_ranker.h"
#include <libime/core/languagemodel.h>
#include <string_view>
#include <vector>

namespace libime { class PinyinContext; class SentenceResult; }
namespace modernime::pinyin {

// Request-local, read-only model evidence. Never updates LibIME history.
class ContextDecoder final {
public:
    ContextDecoder(libime::PinyinContext &context, std::string_view before);
    double gain(const libime::SentenceResult &sentence) const;
    std::vector<core::CandidateScore> candidates() const;
private:
    libime::PinyinContext &context_;
    libime::LanguageModel model_;
    libime::State baseline_;
    libime::State state_;
    bool active_ = false;
};

} // namespace modernime::pinyin
