#pragma once

#include "modernime/pinyin/pinyin_candidate_provider.h"

#include <libime/core/userlanguagemodel.h>
#include <libime/pinyin/pinyincontext.h>
#include <libime/pinyin/pinyindictionary.h>
#include <libime/pinyin/pinyinime.h>

#include <memory>
#include <cstddef>
#include <string>
#include <string_view>

// Read the installed system model directly, without Provider scoring, user
// dictionaries, learning or composition. Jammy's model prefers yi/一, zi/子,
// ge/个 and jia/加; newer models prefer 以, 自, 各 and 家. These specific
// fixtures check preservation of the model prior rather than a model version.
class NativePinyinTestBaseline final {
public:
    NativePinyinTestBaseline() {
        auto dictionary = std::make_unique<libime::PinyinDictionary>();
        dictionary->load(0, modernime::pinyin::defaultDictionaryPath().c_str(),
                         libime::PinyinDictFormat::Binary);
        auto model = std::make_unique<libime::UserLanguageModel>(
            modernime::pinyin::defaultLanguageModelPath().c_str());
        ime_ = std::make_unique<libime::PinyinIME>(std::move(dictionary),
                                                std::move(model));
    }

    std::string candidate(std::string_view input, std::size_t index = 0) {
        libime::PinyinContext context(ime_.get());
        context.type(input);
        return context.candidates().at(index).toString();
    }

    // This is the independently audited behavior profile, not a library
    // version check or a security fingerprint. Unknown profiles keep the
    // default quality assertions instead of acquiring relaxed expectations.
    bool hasAuditedLegacyProfile() {
        return candidate("yi") == "一" && candidate("zi") == "子" &&
               candidate("ge") == "个" && candidate("jia") == "加" &&
               candidate("ni", 1) == "尼";
    }

private:
    std::unique_ptr<libime::PinyinIME> ime_;
};
