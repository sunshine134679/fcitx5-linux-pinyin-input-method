#pragma once

#include "modernime/core/candidate_provider.h"

#include <memory>
#include <string>

namespace modernime::pinyin {

struct PinyinDataPaths final {
    std::string dictionary = "/usr/share/libime/sc.dict";
    std::string languageModel = "/usr/lib/x86_64-linux-gnu/libime/zh_CN.lm";
};

class PinyinCandidateProvider final : public core::CandidateProvider {
public:
    explicit PinyinCandidateProvider(PinyinDataPaths paths = {});
    ~PinyinCandidateProvider() override;

    bool append(std::string_view input) override;
    bool eraseLast() override;
    bool select(std::size_t index) override;
    void reset() override;
    const core::CandidatePage &page() const override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace modernime::pinyin
