#include "modernime/settings/candidate_pin_dialog.h"
#include "modernime/core/pinyin_match.h"
#include "modernime/pinyin/pinyin_candidate_provider.h"
#include <algorithm>
#include <exception>
#include <utility>

namespace modernime::settings {
PinCandidateResult queryPinCandidates(const core::SettingsPaths &paths,
    const core::ModernIMESettings &settings, core::PinyinScheme scheme,
    const std::string &input, std::size_t limit) {
    PinCandidateResult result;
    std::string normalized = input;
    for (auto &c : normalized) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (normalized.empty() || normalized.size() > 128 || !core::PinyinMatchPolicy::validComposition(normalized)) {
        result.error = "请输入 1–128 字节的拼音或双拼按键，可用英文单引号分隔。";
        return result;
    }
    try {
        pinyin::PinyinDataPaths data;
        data.userDictionary = paths.userDictionary.string();
        data.learningStore = paths.learningStore.string();
        pinyin::PinyinProviderOptions options;
        options.learningEnabled = false;
        options.contextLearningEnabled = false;
        options.clipboardEnabled = false;
        options.englishDefinitionEnabled = false;
        options.fuzzyPinyin = settings.fuzzyPinyin;
        options.pinyinScheme = scheme;
        pinyin::PinyinCandidateProvider provider(data, options);
        if (!provider.append(normalized)) { result.error = "未能解析输入，请检查所选方案和按键。"; return result; }
        limit = std::clamp<std::size_t>(limit, 32, 1024);
        while (provider.page().items.size() < limit && provider.page().hasMoreCandidates)
            if (!provider.loadMoreCandidates()) break;
        const auto &page = provider.page();
        for (const auto &item : page.items) {
            const auto identity = core::candidateIdentity(item, page);
            if (item.source == core::CandidateSource::None || identity.consumedInputBytes != page.rawInput.size() ||
                (identity.role != core::CommitCandidateRole::Chinese && identity.role != core::CommitCandidateRole::UserDictionary)) continue;
            // SettingsStore requires canonical full pinyin; the composer uses
            // this same normalization when matching the original identity.
            core::CandidatePin pin{scheme, core::PinyinMatchPolicy::canonical(input), identity.text,
                core::PinyinMatchPolicy::canonical(identity.fullPinyin)};
            auto validation = core::defaultSettings(); validation.candidatePins = {pin};
            if (!core::validateSettings(validation).valid || std::find(result.candidates.begin(), result.candidates.end(), pin) != result.candidates.end()) continue;
            result.candidates.push_back(std::move(pin));
        }
        result.hasMore = page.hasMoreCandidates;
    } catch (const std::exception &error) { result.error = error.what(); }
    return result;
}
}
