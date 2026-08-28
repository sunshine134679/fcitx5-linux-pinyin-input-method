#include "modernime/pinyin/pinyin_candidate_provider.h"

#include "modernime/pinyin/candidate_pipeline.h"
#include "modernime/pinyin/user_dictionary.h"

#include "modernime/core/pinyin_match.h"
#include "modernime/core/learning_writer.h"

#include <libime/core/userlanguagemodel.h>
#include <libime/pinyin/pinyincontext.h>
#include <libime/pinyin/pinyindictionary.h>
#include <libime/pinyin/pinyinime.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace modernime::pinyin {

namespace {

std::filesystem::path defaultLearningPath() {
    const auto *dataHome = std::getenv("XDG_DATA_HOME");
    if (dataHome != nullptr && *dataHome != '\0') {
        return std::filesystem::path(dataHome) / "modernime" /
               "learning.sqlite3";
    }
    const auto *home = std::getenv("HOME");
    if (home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".local" / "share" /
               "modernime" / "learning.sqlite3";
    }
    return {};
}

std::filesystem::path defaultUserDictionaryPath() {
    const auto *dataHome = std::getenv("XDG_DATA_HOME");
    if (dataHome != nullptr && *dataHome != '\0') {
        return std::filesystem::path(dataHome) / "modernime" /
               "user-dictionary.txt";
    }
    const auto *home = std::getenv("HOME");
    if (home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".local" / "share" /
               "modernime" / "user-dictionary.txt";
    }
    return {};
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

bool coversPinyinInput(std::string_view userInput,
                       std::string_view fullPinyin) {
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

} // namespace

class PinyinCandidateProvider::Impl final {
public:
    Impl(const PinyinDataPaths &paths, const PinyinProviderOptions &options)
        : userDictionaryPath_(paths.userDictionary.empty()
                                  ? defaultUserDictionaryPath()
                                  : std::filesystem::path(paths.userDictionary)),
          learning_(options.learningEnabled
                         ? std::make_unique<core::LearningWriter>(
                               paths.learningStore.empty()
                                   ? defaultLearningPath()
                                   : std::filesystem::path(paths.learningStore))
                         : nullptr),
          contextLearningEnabled_(options.contextLearningEnabled),
          userDictionary_(UserDictionary::loadText(userDictionaryPath_)) {
        auto dictionary = std::make_unique<libime::PinyinDictionary>();
        if (std::filesystem::is_regular_file(paths.dictionary)) {
            dictionary->load(0, paths.dictionary.c_str(),
                             libime::PinyinDictFormat::Binary);
        }
        userDictionary_.addTo(*dictionary, 1);

        std::unique_ptr<libime::UserLanguageModel> model;
        if (std::filesystem::is_regular_file(paths.languageModel)) {
            model = std::make_unique<libime::UserLanguageModel>(
                paths.languageModel.c_str());
        } else {
            model = std::make_unique<libime::UserLanguageModel>();
        }

        ime = std::make_unique<libime::PinyinIME>(std::move(dictionary),
                                                  std::move(model));
        ime->setNBest(32);
        context = std::make_unique<libime::PinyinContext>(ime.get());
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
        if (learning_ != nullptr) {
            learning_->enqueueSelection(candidate.text, candidate.fullPinyin,
                                         contextBefore_, contextAfter_,
                                         nowMilliseconds());
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
            auto updatedDictionary = userDictionary_;
            if (!updatedDictionary.remove(candidate.fullPinyin,
                                          candidate.text) ||
                !updatedDictionary.saveText(userDictionaryPath_)) {
                return false;
            }
            const bool removedFromLibime = userDictionary_.removeFrom(
                *ime->dict(), 1, candidate.fullPinyin, candidate.text);
            if (!removedFromLibime) {
                // Restore the original file if the in-memory dictionary layer
                // could not be changed.
                userDictionary_.saveText(userDictionaryPath_);
                return false;
            }
            userDictionary_ = std::move(updatedDictionary);
            rebuildContext(rawInput);
            return true;
        }

        if (candidate.source == core::CandidateSource::Learned) {
            suppressedLearned_.insert(
                candidateKey(candidate.fullPinyin, candidate.text));
        }
        if (learning_ == nullptr) {
            return false;
        }
        learning_->enqueueSuppression(candidate.text, candidate.fullPinyin);
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

    const core::CandidatePage &page() const { return page_; }

private:
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
        page_.preedit = context->userInput();
        ++generation;
        page_.generation = generation;
        if (context->userInput().empty()) {
            return;
        }

        const auto learning = learning_ != nullptr ? learning_->snapshot()
                                                   : nullptr;
        const auto result = buildCandidatePipeline(
            *context, *ime->dict(), learning.get(), nowMilliseconds(),
            contextBefore_, contextAfter_, previousOrder);
        page_.items.reserve(result.order.size() + 1);
        const bool hasPinyinCoverage = std::any_of(
            result.scored.begin(), result.scored.end(),
            [this](const auto &candidate) {
                return coversPinyinInput(page_.preedit, candidate.full_pinyin);
            });
        const auto manualLimit =
            pinyinLetterCount(page_.preedit) < 3 ? std::size_t{2}
                                                 : std::size_t{8};
        std::size_t manualCount = 0;
        std::unordered_set<std::string> seen;
        seen.reserve(result.order.size());
        for (const auto sourceIndex : result.order) {
            const auto &candidate = result.scored[sourceIndex];
            const bool isManual = userDictionary_.contains(
                candidate.full_pinyin, candidate.text);
            const auto learningEntry = learning != nullptr
                                           ? learning->entry(
                                                 candidate.text,
                                                 candidate.full_pinyin,
                                                 contextBefore_, contextAfter_)
                                           : nullptr;
            const auto key = candidateKey(candidate.full_pinyin, candidate.text);
            if (!isManual && suppressedLearned_.contains(key)) {
                continue;
            }
            const bool isLearned = !isManual && learning != nullptr &&
                                   !learning->isSuppressed(
                                       candidate.text, candidate.full_pinyin) &&
                                   learningEntry != nullptr &&
                                   learningEntry->frequency > 0;
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
            page_.items.push_back(std::move(item));
        }

        core::CandidateItem rawCandidate;
        rawCandidate.text = page_.preedit;
        rawCandidate.source = core::CandidateSource::Raw;
        if (!hasPinyinCoverage && page_.preedit.size() >= 3) {
            page_.items.insert(page_.items.begin(), std::move(rawCandidate));
        } else {
            page_.items.push_back(std::move(rawCandidate));
        }
    }

    std::unique_ptr<libime::PinyinIME> ime;
    std::unique_ptr<libime::PinyinContext> context;
    std::filesystem::path userDictionaryPath_;
    std::unique_ptr<core::LearningWriter> learning_;
    bool contextLearningEnabled_ = true;
    UserDictionary userDictionary_;
    core::CandidatePage page_;
    std::string contextBefore_;
    std::string contextAfter_;
    std::unordered_set<std::string> suppressedLearned_;
    std::uint64_t generation = 0;
};

PinyinCandidateProvider::PinyinCandidateProvider(PinyinDataPaths paths,
                                                 PinyinProviderOptions options)
    : impl_(std::make_unique<Impl>(paths, options)) {}

PinyinCandidateProvider::~PinyinCandidateProvider() = default;

bool PinyinCandidateProvider::append(std::string_view input) {
    return impl_->append(input);
}

bool PinyinCandidateProvider::eraseLast() { return impl_->eraseLast(); }

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

} // namespace modernime::pinyin
