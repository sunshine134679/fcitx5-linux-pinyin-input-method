#include "modernime/pinyin/pinyin_candidate_provider.h"

#include "modernime/pinyin/candidate_mixer.h"
#include "modernime/pinyin/candidate_pipeline.h"
#include "modernime/pinyin/user_dictionary.h"

#include "modernime/core/pinyin_match.h"
#include "modernime/core/learning_writer.h"
#include "modernime/core/settings.h"

#include <libime/core/userlanguagemodel.h>
#include <libime/pinyin/pinyincontext.h>
#include <libime/pinyin/pinyindictionary.h>
#include <libime/pinyin/pinyinime.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace modernime::pinyin {

namespace {

std::filesystem::path defaultLearningPath() {
    const auto *xdgConfigHome = std::getenv("XDG_CONFIG_HOME");
    const auto *dataHome = std::getenv("XDG_DATA_HOME");
    const auto *home = std::getenv("HOME");
    return core::SettingsPaths::fromEnvironment(
               xdgConfigHome == nullptr ? std::string_view{}
                                        : std::string_view(xdgConfigHome),
               dataHome == nullptr ? std::string_view{}
                                   : std::string_view(dataHome),
               home == nullptr ? std::string_view{}
                               : std::string_view(home))
        .learningStore;
}

std::filesystem::path defaultUserDictionaryPath() {
    const auto *xdgConfigHome = std::getenv("XDG_CONFIG_HOME");
    const auto *dataHome = std::getenv("XDG_DATA_HOME");
    const auto *home = std::getenv("HOME");
    return core::SettingsPaths::fromEnvironment(
               xdgConfigHome == nullptr ? std::string_view{}
                                        : std::string_view(xdgConfigHome),
               dataHome == nullptr ? std::string_view{}
                                   : std::string_view(dataHome),
               home == nullptr ? std::string_view{}
                               : std::string_view(home))
        .userDictionary;
}

bool isRegularFile(const std::filesystem::path &path) {
    if (path.empty()) {
        return false;
    }
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

std::filesystem::path extensionDictionaryPath(
    std::string_view configuredPath) {
    if (!configuredPath.empty()) {
        return std::filesystem::path(configuredPath);
    }

    std::vector<std::filesystem::path> candidates;
    if (const auto *environment = std::getenv(
            "MODERNIME_PINYIN_KNOWLEDGE_DICTIONARY");
        environment != nullptr && *environment != '\0') {
        candidates.emplace_back(environment);
    }

    const auto *dataHome = std::getenv("XDG_DATA_HOME");
    if (dataHome != nullptr && *dataHome != '\0') {
        candidates.emplace_back(std::filesystem::path(dataHome) / "modernime" /
                                "pinyin" / "modernime-knowledge.dict");
    } else if (const auto *home = std::getenv("HOME"); home != nullptr &&
               *home != '\0') {
        candidates.emplace_back(std::filesystem::path(home) / ".local" /
                                "share" / "modernime" / "pinyin" /
                                "modernime-knowledge.dict");
    }

#ifdef MODERNIME_PINYIN_KNOWLEDGE_INSTALL_BINARY
    candidates.emplace_back(MODERNIME_PINYIN_KNOWLEDGE_INSTALL_BINARY);
#endif
#ifdef MODERNIME_PINYIN_KNOWLEDGE_BUILD_BINARY
    candidates.emplace_back(MODERNIME_PINYIN_KNOWLEDGE_BUILD_BINARY);
#endif
    candidates.emplace_back(
        "/usr/share/modernime/pinyin/modernime-knowledge.dict");

    for (const auto &candidate : candidates) {
        if (isRegularFile(candidate)) {
            return candidate;
        }
    }
    return {};
}

void loadExtensionDictionary(libime::PinyinDictionary &dictionary,
                             const std::filesystem::path &path) {
    if (!isRegularFile(path)) {
        return;
    }

    const auto index = dictionary.dictSize();
    dictionary.addEmptyDict();
    try {
        dictionary.load(index, path.c_str(), libime::PinyinDictFormat::Binary);
    } catch (...) {
        // An optional data package must never prevent the system dictionary
        // and user dictionary from starting.
        dictionary.clear(index);
    }
}

std::int64_t nowMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::size_t pinyinLetterCount(std::string_view input) {
    std::size_t count = 0;
    for (const char character : input) {
        if (character >= 'a' && character <= 'z') {
            ++count;
        }
    }
    return count;
}

std::string candidateKey(std::string_view pinyin, std::string_view text) {
    std::string key = core::normalizePinyin(pinyin);
    key.push_back('\x1f');
    key.append(text);
    return key;
}

std::vector<std::size_t> syllablePrefixEnds(std::string_view rawInput,
                                            std::string_view segmentedInput) {
    std::vector<std::size_t> result;
    std::size_t rawOffset = 0;
    for (const char character : segmentedInput) {
        if (character != '\'') {
            while (rawOffset < rawInput.size() && rawInput[rawOffset] == '\'') {
                ++rawOffset;
            }
            if (rawOffset < rawInput.size()) {
                ++rawOffset;
            }
            continue;
        }
        if (rawOffset < rawInput.size() && rawInput[rawOffset] == '\'') {
            ++rawOffset;
        }
        if (rawOffset > 0 && rawOffset < rawInput.size()) {
            result.push_back(rawOffset);
        }
    }
    return result;
}

std::string prefixInput(std::string_view rawInput, std::size_t consumedBytes) {
    auto prefix = std::string(rawInput.substr(0, consumedBytes));
    while (!prefix.empty() && prefix.back() == '\'') {
        prefix.pop_back();
    }
    return prefix;
}

bool coversPinyinInput(std::string_view userInput,
                       std::string_view fullPinyin) {
    // Keep short ASCII words such as "who" as the English fallback. Chinese
    // initialisms become a reliable signal once they contain at least four
    // initials, which also covers the normal four-character idiom case.
    if (userInput.size() >= 4 &&
        core::PinyinMatchPolicy::isAbbreviationInput(userInput) &&
        core::PinyinMatchPolicy::abbreviationKey(fullPinyin) == userInput) {
        return true;
    }

    const auto input = core::PinyinMatchPolicy::canonical(userInput);
    const auto candidate = core::PinyinMatchPolicy::canonical(fullPinyin);
    if (input.empty()) {
        return false;
    }
    if (candidate.size() >= input.size() &&
        candidate.compare(0, input.size(), input) == 0) {
        return true;
    }

    std::vector<std::string_view> syllables;
    std::size_t syllableStart = 0;
    while (syllableStart < fullPinyin.size()) {
        const auto separator = fullPinyin.find('\'', syllableStart);
        const auto syllableEnd = separator == std::string_view::npos
                                     ? fullPinyin.size()
                                     : separator;
        if (syllableEnd == syllableStart) {
            return false;
        }
        syllables.push_back(
            fullPinyin.substr(syllableStart, syllableEnd - syllableStart));
        if (separator == std::string_view::npos) {
            break;
        }
        syllableStart = separator + 1;
    }

    std::size_t inputOffset = 0;
    bool abbreviationMode = false;
    bool consumedFullSyllable = false;
    for (const auto syllable : syllables) {
        if (inputOffset == input.size()) {
            return consumedFullSyllable;
        }
        const auto remaining = input.substr(inputOffset);
        if (!abbreviationMode && remaining.starts_with(syllable)) {
            inputOffset += syllable.size();
            consumedFullSyllable = true;
            continue;
        }
        if (remaining.size() <= syllable.size() &&
            syllable.compare(0, remaining.size(), remaining) == 0) {
            return consumedFullSyllable;
        }
        if (remaining.front() == syllable.front()) {
            abbreviationMode = true;
            ++inputOffset;
            continue;
        }
        return false;
    }
    return inputOffset == input.size() && consumedFullSyllable;
}

struct PreeditAlignment final {
    std::string text;
    std::size_t fullSyllableCount = 0;
    std::size_t abbreviationSyllableCount = 0;
};

bool isBetterPreeditAlignment(const PreeditAlignment &candidate,
                              const PreeditAlignment &current) {
    if (candidate.fullSyllableCount != current.fullSyllableCount) {
        return candidate.fullSyllableCount > current.fullSyllableCount;
    }
    if (candidate.abbreviationSyllableCount !=
        current.abbreviationSyllableCount) {
        return candidate.abbreviationSyllableCount >
               current.abbreviationSyllableCount;
    }
    return candidate.text.size() > current.text.size();
}

std::optional<PreeditAlignment> alignPreeditToCandidate(
    std::string_view rawInput, std::string_view fullPinyin) {
    if (rawInput.empty() || fullPinyin.empty() ||
        rawInput.find('\'') != std::string_view::npos) {
        return std::nullopt;
    }

    std::vector<std::string_view> syllables;
    std::size_t syllableStart = 0;
    while (syllableStart < fullPinyin.size()) {
        const auto separator = fullPinyin.find('\'', syllableStart);
        const auto syllableEnd = separator == std::string_view::npos
                                     ? fullPinyin.size()
                                     : separator;
        if (syllableEnd == syllableStart) {
            return std::nullopt;
        }
        syllables.push_back(
            fullPinyin.substr(syllableStart, syllableEnd - syllableStart));
        if (separator == std::string_view::npos) {
            break;
        }
        syllableStart = separator + 1;
    }
    if (syllables.empty()) {
        return std::nullopt;
    }

    const auto stateCount = (syllables.size() + 1) * (rawInput.size() + 1);
    std::vector<std::optional<PreeditAlignment>> memo(stateCount);
    std::vector<bool> visited(stateCount, false);
    const auto stateIndex = [rawInput](std::size_t syllableIndex,
                                       std::size_t inputOffset) {
        return syllableIndex * (rawInput.size() + 1) + inputOffset;
    };

    std::function<std::optional<PreeditAlignment>(std::size_t, std::size_t)>
        align = [&](std::size_t syllableIndex,
                    std::size_t inputOffset)
        -> std::optional<PreeditAlignment> {
        const auto index = stateIndex(syllableIndex, inputOffset);
        if (visited[index]) {
            return memo[index];
        }
        visited[index] = true;

        if (inputOffset == rawInput.size()) {
            memo[index] = PreeditAlignment{};
            return memo[index];
        }
        if (syllableIndex == syllables.size()) {
            return std::nullopt;
        }

        const auto syllable = syllables[syllableIndex];
        const auto remaining = rawInput.substr(inputOffset);
        std::optional<PreeditAlignment> best;
        const auto consider = [&](std::size_t consumed,
                                  bool fullSyllable,
                                  bool abbreviation) {
            const auto tail = align(syllableIndex + 1,
                                    inputOffset + consumed);
            if (!tail.has_value()) {
                return;
            }

            PreeditAlignment alignment;
            alignment.text.assign(rawInput.substr(inputOffset, consumed));
            if (!tail->text.empty()) {
                alignment.text.push_back('\'');
                alignment.text.append(tail->text);
            }
            alignment.fullSyllableCount =
                tail->fullSyllableCount + (fullSyllable ? 1 : 0);
            alignment.abbreviationSyllableCount =
                tail->abbreviationSyllableCount + (abbreviation ? 1 : 0);
            if (!best.has_value() ||
                isBetterPreeditAlignment(alignment, *best)) {
                best = std::move(alignment);
            }
        };

        if (remaining.starts_with(syllable)) {
            consider(syllable.size(), true, false);
        }

        if (remaining.front() == syllable.front()) {
            consider(1, false, true);
        }

        // A shorter prefix is an unfinished final syllable. It must consume
        // all remaining input; otherwise it would swallow initials belonging
        // to later syllables (for example, smcg -> s'm'c'g).
        if (remaining.size() < syllable.size() &&
            syllable.starts_with(remaining)) {
            PreeditAlignment alignment;
            alignment.text = std::string(remaining);
            if (remaining.size() == rawInput.size() - inputOffset) {
                best = !best.has_value() ||
                               isBetterPreeditAlignment(alignment, *best)
                           ? std::optional<PreeditAlignment>(
                                 std::move(alignment))
                           : best;
            }
        }

        memo[index] = best;
        return memo[index];
    };

    auto alignment = align(0, 0);
    if (!alignment.has_value() || alignment->text.find('\'') ==
                                     std::string::npos) {
        return std::nullopt;
    }

    // A short all-initial input is intentionally left alone so ordinary
    // English fragments such as "who" are not rendered as Chinese initials.
    if (alignment->fullSyllableCount == 0 && rawInput.size() < 4) {
        return std::nullopt;
    }
    return alignment;
}

std::string automaticallySegmentedPreedit(
    std::string_view rawInput, const CandidatePipelineResult &result) {
    // An explicit separator is the user's segmentation choice.
    if (rawInput.find('\'') != std::string_view::npos) {
        return std::string(rawInput);
    }

    // Try candidates in their actual ranked order so the displayed boundary
    // follows the same phrase that the user sees first in the candidate list.
    for (const auto sourceIndex : result.order) {
        if (sourceIndex >= result.scored.size()) {
            continue;
        }
        const auto &candidate = result.scored[sourceIndex];
        const auto alignment =
            alignPreeditToCandidate(rawInput, candidate.full_pinyin);
        if (alignment.has_value()) {
            return alignment->text;
        }
    }
    return std::string(rawInput);
}

} // namespace

class PinyinCandidateProvider::SharedResources final {
public:
    std::unique_ptr<libime::PinyinIME> ime;
    std::unique_ptr<core::LearningWriter> learning;
    UserDictionary userDictionary;
    std::filesystem::path userDictionaryPath;
    std::filesystem::path learningStorePath;

    core::LearningWriter *ensureLearningWriter() {
        if (learning == nullptr && !learningStorePath.empty()) {
            learning =
                std::make_unique<core::LearningWriter>(learningStorePath);
        }
        return learning.get();
    }

    // Re-reads the user dictionary file and swaps the libime dictionary
    // layer, so settings-client edits apply without restarting fcitx5.
    bool reloadUserDictionary() {
        auto updated = UserDictionary::loadText(userDictionaryPath);
        ime->dict()->clear(1);
        updated.addTo(*ime->dict(), 1);
        userDictionary = std::move(updated);
        return true;
    }
};

std::shared_ptr<PinyinCandidateProvider::SharedResources>
PinyinCandidateProvider::createSharedResources(
    const PinyinDataPaths &paths, const PinyinProviderOptions &options) {
    auto resources = std::make_shared<SharedResources>();
    resources->userDictionaryPath =
        paths.userDictionary.empty()
            ? defaultUserDictionaryPath()
            : std::filesystem::path(paths.userDictionary);
    resources->userDictionary =
        UserDictionary::loadText(resources->userDictionaryPath);
    resources->learningStorePath =
        paths.learningStore.empty()
            ? defaultLearningPath()
            : std::filesystem::path(paths.learningStore);

    auto dictionary = std::make_unique<libime::PinyinDictionary>();
    if (std::filesystem::is_regular_file(paths.dictionary)) {
        dictionary->load(0, paths.dictionary.c_str(),
                         libime::PinyinDictFormat::Binary);
    }
    resources->userDictionary.addTo(*dictionary, 1);
    loadExtensionDictionary(
        *dictionary,
        extensionDictionaryPath(paths.extensionDictionary));

    std::unique_ptr<libime::UserLanguageModel> model;
    if (std::filesystem::is_regular_file(paths.languageModel)) {
        model = std::make_unique<libime::UserLanguageModel>(
            paths.languageModel.c_str());
    } else {
        model = std::make_unique<libime::UserLanguageModel>();
    }

    resources->ime = std::make_unique<libime::PinyinIME>(std::move(dictionary),
                                                         std::move(model));
    resources->ime->setFuzzyFlags(libime::PinyinFuzzyFlag::CommonTypo);
    resources->ime->setNBest(32);
    if (options.learningEnabled) {
        resources->learning =
            std::make_unique<core::LearningWriter>(resources->learningStorePath);
    }
    return resources;
}

bool PinyinCandidateProvider::reloadUserDictionary(
    std::shared_ptr<SharedResources> &resources) {
    if (resources == nullptr) {
        return false;
    }
    return resources->reloadUserDictionary();
}

class PinyinCandidateProvider::Impl final {
public:
    Impl(const PinyinDataPaths &paths, const PinyinProviderOptions &options)
        : Impl(createSharedResources(paths, options), options) {}

    Impl(std::shared_ptr<SharedResources> shared,
         const PinyinProviderOptions &options)
        : shared_(std::move(shared)),
          contextLearningEnabled_(options.contextLearningEnabled),
          learningEnabled_(options.learningEnabled) {
        context = std::make_unique<libime::PinyinContext>(shared_->ime.get());
        refresh();
    }

    bool append(std::string_view input) {
        std::string nextInput = context->userInput();
        nextInput.append(input);
        if (!core::PinyinMatchPolicy::validComposition(nextInput)) {
            return false;
        }
        if (!context->type(input)) {
            return false;
        }
        refresh();
        return true;
    }

    bool eraseLast() {
        if (!context->backspace()) {
            return false;
        }
        refresh();
        return true;
    }

    bool replaceInput(std::string_view input) {
        if (!input.empty() &&
            !core::PinyinMatchPolicy::validComposition(input)) {
            return false;
        }
        const auto previous = context->userInput();
        context->clear();
        if (!input.empty() && !context->type(input)) {
            context->clear();
            if (!previous.empty()) {
                context->type(previous);
            }
            refresh();
            return false;
        }
        refresh();
        return true;
    }

    bool select(std::size_t index) {
        if (index >= page_.items.size()) {
            return false;
        }
        const auto &candidate = page_.items[index];
        if (candidate.source == core::CandidateSource::Raw) {
            context->clear();
            refresh();
            return true;
        }
        if (auto *learning = learningWriter(); learning != nullptr) {
            learning->enqueueSelection(candidate.text, candidate.fullPinyin,
                                       contextBefore_, contextAfter_,
                                       nowMilliseconds());
        }
        const auto rawInput = context->userInput();
        if (candidate.consumedInputBytes > 0 &&
            candidate.consumedInputBytes < rawInput.size()) {
            auto remainder = rawInput.substr(candidate.consumedInputBytes);
            while (!remainder.empty() && remainder.front() == '\'') {
                remainder.erase(remainder.begin());
            }
            context->clear();
            if (!remainder.empty() && !context->type(remainder)) {
                context->clear();
                context->type(rawInput);
                refresh();
                return false;
            }
            refresh();
            return true;
        }
        context->select(page_.items[index].sourceIndex);
        refresh();
        return true;
    }

    bool remove(std::size_t index) {
        if (index >= page_.items.size()) {
            return false;
        }
        const auto candidate = page_.items[index];
        if (candidate.source == core::CandidateSource::Raw) {
            return false;
        }
        const auto rawInput = context->userInput();
        if (candidate.source == core::CandidateSource::UserDictionary) {
            auto updatedDictionary = userDictionary();
            if (!updatedDictionary.remove(candidate.fullPinyin,
                                          candidate.text) ||
                !updatedDictionary.saveText(userDictionaryPath())) {
                return false;
            }
            const bool removedFromLibime = userDictionary().removeFrom(
                *ime().dict(), 1, candidate.fullPinyin, candidate.text);
            if (!removedFromLibime) {
                // Restore the original file if the in-memory dictionary layer
                // could not be changed.
                userDictionary().saveText(userDictionaryPath());
                return false;
            }
            userDictionary() = std::move(updatedDictionary);
            rebuildContext(rawInput);
            return true;
        }

        if (candidate.source == core::CandidateSource::Learned) {
            suppressedLearned_.insert(
                candidateKey(candidate.fullPinyin, candidate.text));
        }
        if (learningWriter() == nullptr) {
            return false;
        }
        learningWriter()->enqueueSuppression(candidate.text,
                                             candidate.fullPinyin);
        refresh();
        return true;
    }

    void reset() {
        suppressedLearned_.clear();
        context->clear();
        refresh();
    }

    void setContext(std::string_view before, std::string_view after) {
        if (!contextLearningEnabled_) {
            contextBefore_.clear();
            contextAfter_.clear();
            return;
        }
        contextBefore_ = before;
        contextAfter_ = after;
    }

    void setLearningEnabled(bool enabled) { learningEnabled_ = enabled; }

    void setContextLearningEnabled(bool enabled) {
        contextLearningEnabled_ = enabled;
        if (!enabled) {
            contextBefore_.clear();
            contextAfter_.clear();
        }
    }

    const core::CandidatePage &page() const { return page_; }

private:
    libime::PinyinIME &ime() { return *shared_->ime; }
    core::LearningWriter *learningWriter() {
        if (!learningEnabled_) {
            return nullptr;
        }
        return shared_->ensureLearningWriter();
    }
    UserDictionary &userDictionary() { return shared_->userDictionary; }
    const std::filesystem::path &userDictionaryPath() const {
        return shared_->userDictionaryPath;
    }

    void rebuildContext(const std::string &rawInput) {
        context->clear();
        if (!rawInput.empty()) {
            context->type(rawInput);
        }
        refresh();
    }

    void refresh() {
        std::vector<std::string> previousOrder;
        previousOrder.reserve(page_.items.size());
        for (const auto &item : page_.items) {
            previousOrder.push_back(
                core::candidateOrderKey(item.text, item.fullPinyin));
        }
        page_.clear();
        const auto rawInput = context->userInput();
        page_.rawInput = rawInput;
        page_.preedit = rawInput;
        ++generation;
        page_.generation = generation;
        if (rawInput.empty()) {
            return;
        }

        const auto learning = [&] {
            auto *writer = learningWriter();
            return writer != nullptr ? writer->snapshot() : nullptr;
        }();
        const auto result = buildCandidatePipeline(
            *context, *ime().dict(), learning.get(), nowMilliseconds(),
            contextBefore_, contextAfter_, previousOrder);
        const auto segmentedInput =
            automaticallySegmentedPreedit(rawInput, result);
        const auto prefixEnds = syllablePrefixEnds(rawInput, segmentedInput);
        page_.items.reserve(result.order.size() + 4);
        const bool hasPinyinCoverage = std::any_of(
            result.scored.begin(), result.scored.end(),
            [&rawInput](const auto &candidate) {
                return coversPinyinInput(rawInput, candidate.full_pinyin);
            });
        const auto manualLimit =
            pinyinLetterCount(rawInput) < 3 ? std::size_t{2} : std::size_t{8};
        std::size_t manualCount = 0;
        std::unordered_set<std::string> seen;
        seen.reserve(result.order.size());
        std::vector<core::CandidateItem> fullItems;
        fullItems.reserve(result.order.size());
        std::vector<core::CandidateItem> partialPool;
        partialPool.reserve(result.order.size());
        for (const auto sourceIndex : result.order) {
            const auto &candidate = result.scored[sourceIndex];
            const bool isManual = userDictionary().contains(
                candidate.full_pinyin, candidate.text);
            const auto key = candidateKey(candidate.full_pinyin, candidate.text);
            if (!isManual && suppressedLearned_.contains(key)) {
                continue;
            }
            const bool isLearned = !isManual && learning != nullptr &&
                                   learning->hasPositiveFrequency(
                                       candidate.text, candidate.full_pinyin);
            if (isManual && manualCount >= manualLimit) {
                continue;
            }
            if (!seen.emplace(candidateKey(candidate.full_pinyin,
                                           candidate.text))
                     .second) {
                continue;
            }
            core::CandidateItem item;
            item.text = candidate.text;
            item.fullPinyin = candidate.full_pinyin;
            item.sourceIndex = candidate.source_index;
            item.source = isManual
                              ? core::CandidateSource::UserDictionary
                              : (isLearned ? core::CandidateSource::Learned
                                           : core::CandidateSource::Engine);
            if (isManual) {
                ++manualCount;
            }
            item.consumedInputBytes = rawInput.size();
            if (core::PinyinMatchPolicy::priority(
                    rawInput, candidate.full_pinyin) == -1) {
                const auto candidatePinyin =
                    core::PinyinMatchPolicy::canonical(candidate.full_pinyin);
                for (const auto prefixEnd : prefixEnds) {
                    if (core::PinyinMatchPolicy::canonical(
                            prefixInput(rawInput, prefixEnd)) == candidatePinyin) {
                        item.consumedInputBytes = prefixEnd;
                        break;
                    }
                }
            }
            if (item.consumedInputBytes < rawInput.size()) {
                partialPool.push_back(std::move(item));
            } else {
                fullItems.push_back(std::move(item));
            }
        }

        const auto *bestFullSentence =
            fullItems.empty() ? nullptr : &fullItems.front();
        page_.items = mixCandidateItems(fullItems, partialPool,
                                        bestFullSentence, prefixEnds, rawInput);

        core::CandidateItem rawCandidate;
        rawCandidate.text = rawInput;
        rawCandidate.source = core::CandidateSource::Raw;
        rawCandidate.consumedInputBytes = rawInput.size();
        const bool alreadyHasRawCandidate =
            std::any_of(page_.items.begin(), page_.items.end(),
                        [](const auto &item) {
                            return item.source == core::CandidateSource::Raw;
                        });
        if (alreadyHasRawCandidate) {
            // The mixer uses raw input as the fifth-slot fallback when no
            // distinct decoded homophone can enforce the full-sentence quota.
        } else if (!hasPinyinCoverage && rawInput.size() >= 3) {
            page_.items.insert(page_.items.begin(), std::move(rawCandidate));
        } else {
            page_.items.push_back(std::move(rawCandidate));
        }
        page_.preedit = segmentedInput;
    }

    std::shared_ptr<SharedResources> shared_;
    std::unique_ptr<libime::PinyinContext> context;
    bool contextLearningEnabled_ = true;
    bool learningEnabled_ = true;
    core::CandidatePage page_;
    std::string contextBefore_;
    std::string contextAfter_;
    std::unordered_set<std::string> suppressedLearned_;
    std::uint64_t generation = 0;
};

PinyinCandidateProvider::PinyinCandidateProvider(PinyinDataPaths paths,
                                                 PinyinProviderOptions options)
    : impl_(std::make_unique<Impl>(paths, options)) {}

PinyinCandidateProvider::PinyinCandidateProvider(
    std::shared_ptr<SharedResources> shared, PinyinProviderOptions options)
    : impl_(std::make_unique<Impl>(std::move(shared), options)) {}

PinyinCandidateProvider::~PinyinCandidateProvider() = default;

bool PinyinCandidateProvider::append(std::string_view input) {
    return impl_->append(input);
}

bool PinyinCandidateProvider::eraseLast() { return impl_->eraseLast(); }

bool PinyinCandidateProvider::replaceInput(std::string_view input) {
    return impl_->replaceInput(input);
}

bool PinyinCandidateProvider::select(std::size_t index) {
    return impl_->select(index);
}

bool PinyinCandidateProvider::remove(std::size_t index) {
    return impl_->remove(index);
}

void PinyinCandidateProvider::reset() { impl_->reset(); }

const core::CandidatePage &PinyinCandidateProvider::page() const {
    return impl_->page();
}

void PinyinCandidateProvider::setContext(std::string_view before,
                                         std::string_view after) {
    impl_->setContext(before, after);
}

void PinyinCandidateProvider::setLearningEnabled(bool enabled) {
    impl_->setLearningEnabled(enabled);
}

void PinyinCandidateProvider::setContextLearningEnabled(bool enabled) {
    impl_->setContextLearningEnabled(enabled);
}

} // namespace modernime::pinyin
