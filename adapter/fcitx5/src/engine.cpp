#include "modernime/fcitx5/engine.h"

#include "modernime/core/pinyin_match.h"
#include "modernime/core/punctuation.h"
#include "modernime/core/calculator.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace modernime::fcitx5 {
namespace {

bool endsWithAsciiAlnum(std::string_view text, std::string_view after) {
    if (text.empty()) {
        return false;
    }
    const auto last = static_cast<unsigned char>(text.back());
    if ((last >= 'a' && last <= 'z') || (last >= 'A' && last <= 'Z')) {
        return true;
    }
    if (last < '0' || last > '9') {
        return false;
    }

    auto digitStart = text.size() - 1;
    while (digitStart > 0 && text[digitStart - 1] >= '0' &&
           text[digitStart - 1] <= '9') {
        --digitStart;
    }
    const bool precededByChinese =
        digitStart > 0 &&
        static_cast<unsigned char>(text[digitStart - 1]) >= 0x80;
    const bool followedByChinese =
        !after.empty() && static_cast<unsigned char>(after.front()) >= 0x80;
    return !precededByChinese && !followedByChinese;
}

bool isAsciiText(std::string_view text) {
    for (const char character : text) {
        if (static_cast<unsigned char>(character) >= 0x80) {
            return false;
        }
    }
    return true;
}

bool inAsciiToken(std::string_view before, std::string_view after) {
    const auto alnum = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    };
    if (!after.empty() && alnum(static_cast<unsigned char>(after.front()))) return true;
    bool separator = false;
    for (auto it = before.rbegin(); it != before.rend(); ++it) {
        const auto c = static_cast<unsigned char>(*it);
        if (alnum(c)) return separator;
        if (c != '_' && c != '/' && c != '\\' && c != '.' && c != ':' && c != '-' && c != '@') break;
        separator = true;
    }
    return false;
}

std::size_t codepoints(std::string_view text) {
    return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](unsigned char c) {
        return (c & 0xc0) != 0x80;
    }));
}

TextEditContext afterInsertion(TextEditContext context, std::string_view left, std::string_view right = {}) {
    context.before += left;
    context.after.insert(0, right);
    context.cursor += codepoints(left);
    while (codepoints(context.before) > 256) {
        std::size_t end = 1;
        while (end < context.before.size() && (static_cast<unsigned char>(context.before[end]) & 0xc0) == 0x80) ++end;
        context.before.erase(0, end);
    }
    std::size_t end = 0, count = 0;
    while (end < context.after.size()) {
        if ((static_cast<unsigned char>(context.after[end]) & 0xc0) != 0x80 && ++count > 256) break;
        ++end;
    }
    context.after.resize(end);
    return context;
}

bool insidePair(const TextEditContext &current, const TextEditContext &anchor) {
    if (current.after != anchor.after || current.cursor < anchor.cursor) return false;
    const auto added = current.cursor - anchor.cursor;
    const auto count = codepoints(current.before);
    if (added >= count) return false;
    std::size_t end = 0, prefixChars = count - added;
    while (end < current.before.size() && prefixChars) {
        ++end;
        while (end < current.before.size() && (static_cast<unsigned char>(current.before[end]) & 0xc0) == 0x80) ++end;
        --prefixChars;
    }
    return anchor.before.ends_with(std::string_view(current.before).substr(0, end));
}

bool quoteIsOpenAtCursor(std::string_view before, std::string_view after,
                         std::string_view left, std::string_view right) {
    const auto lastLeft = before.rfind(left);
    const auto lastRight = before.rfind(right);
    if (lastLeft != std::string_view::npos ||
        lastRight != std::string_view::npos) {
        return lastLeft != std::string_view::npos &&
               (lastRight == std::string_view::npos || lastLeft > lastRight);
    }

    const auto firstLeft = after.find(left);
    const auto firstRight = after.find(right);
    return firstRight != std::string_view::npos &&
           (firstLeft == std::string_view::npos || firstRight < firstLeft);
}

std::size_t displayedPreeditCursor(std::string_view displayed,
                                   std::string_view raw,
                                   std::size_t rawCursor) {
    rawCursor = std::min(rawCursor, raw.size());
    std::size_t displayedOffset = 0;
    std::size_t rawOffset = 0;
    while (displayedOffset < displayed.size() && rawOffset < rawCursor) {
        if (rawOffset < raw.size() &&
            displayed[displayedOffset] == raw[rawOffset]) {
            ++displayedOffset;
            ++rawOffset;
        } else {
            ++displayedOffset;
        }
    }
    // An automatically inserted apostrophe belongs to the syllable boundary
    // before the next raw letter, so place the caret after it. A user-typed
    // apostrophe remains on the right side until that raw byte is consumed.
    while (displayedOffset < displayed.size() &&
           displayed[displayedOffset] == '\'' &&
           (rawOffset >= raw.size() || raw[rawOffset] != '\'')) {
        ++displayedOffset;
    }
    return displayedOffset;
}

} // namespace

namespace {
std::atomic<std::uint64_t> gNextSessionId{1};
} // namespace

class ModernIMEController::ControllerEndpoint final : public core::SelectionEndpoint {
public:
    explicit ControllerEndpoint(ModernIMEController *controller)
        : controller_(controller) {}

    void detach() noexcept {
        controller_ = nullptr;
    }

    bool select(const core::CandidateSelectionRequest &request) override {
        if (!controller_) {
            return false;
        }
        return controller_->select(request);
    }

    bool setPageBoundaries(std::uint64_t displayGeneration,
                           std::uint64_t layoutGeneration,
                           std::vector<core::PageBoundary> boundaries) override {
        if (!controller_) {
            return false;
        }
        return controller_->setPageBoundaries(displayGeneration, layoutGeneration,
                                              std::move(boundaries));
    }

    bool moveCursor(std::ptrdiff_t delta) override {
        if (!controller_) {
            return false;
        }
        return controller_->moveCursor(delta);
    }

    bool movePage(std::ptrdiff_t delta) override {
        if (!controller_) {
            return false;
        }
        return controller_->movePage(delta);
    }

    std::size_t currentPageIndex() const override {
        if (!controller_) {
            return 0;
        }
        return controller_->currentPageIndex();
    }

private:
    ModernIMEController *controller_ = nullptr;
};

ModernIMEController::ModernIMEController(EngineHost &host,
                                         core::CandidateProvider *provider,
                                         ControllerOptions options)
    : host_(host),
      provider_(provider),
      options_(options),
      sessionId_(gNextSessionId.fetch_add(1, std::memory_order_relaxed)),
      displayGeneration_(1),
      layoutGeneration_(1),
      providerGeneration_(0),
      endpoint_(std::make_shared<ControllerEndpoint>(this)) {}

ModernIMEController::~ModernIMEController() {
    if (endpoint_) {
        endpoint_->detach();
    }
}

std::weak_ptr<core::SelectionEndpoint> ModernIMEController::endpoint() const noexcept {
    return endpoint_;
}

void ModernIMEController::initRecentCommit(
    const core::CandidateItem &candidate, const std::string &basePrefix,
    const std::string &contextAfter) {
    recentCommit_.valid = false;
    recentCommit_.originalChars.clear();
    recentCommit_.originalSyllables.clear();

    if (candidate.text.empty()) {
        return;
    }

    std::vector<std::string> utf8Chars;
    for (std::size_t i = 0; i < candidate.text.size(); ) {
        unsigned char c = static_cast<unsigned char>(candidate.text[i]);
        std::size_t len = 1;
        if ((c & 0x80) == 0) {
            return;
        } else if ((c & 0xE0) == 0xC0) {
            return;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
        } else {
            return;
        }
        if (i + len > candidate.text.size()) {
            return;
        }
        utf8Chars.push_back(candidate.text.substr(i, len));
        i += len;
    }

    if (utf8Chars.size() < 2) {
        return;
    }

    std::string fullPinyin =
        candidate.fullPinyin.empty() ? page_.rawInput : candidate.fullPinyin;
    std::string segmentedPinyin;
    if (fullPinyin.find('\'') != std::string::npos) {
        segmentedPinyin = fullPinyin;
    } else {
        segmentedPinyin = core::PinyinMatchPolicy::segmentPinyin(fullPinyin);
    }

    std::string_view pView = segmentedPinyin;
    while (!pView.empty() && pView.front() == '\'') {
        pView.remove_prefix(1);
    }
    while (!pView.empty() && pView.back() == '\'') {
        pView.remove_suffix(1);
    }

    std::vector<std::string> syllables;
    std::size_t start = 0;
    while (start < pView.size()) {
        auto pos = pView.find('\'', start);
        if (pos == std::string_view::npos) {
            syllables.push_back(std::string(pView.substr(start)));
            break;
        }
        if (pos > start) {
            syllables.push_back(std::string(pView.substr(start, pos - start)));
        }
        start = pos + 1;
    }

    if (syllables.size() != utf8Chars.size()) {
        return;
    }

    recentCommit_.originalText = candidate.text;
    recentCommit_.originalPinyin = segmentedPinyin;
    recentCommit_.currentText = candidate.text;
    recentCommit_.currentPinyin = segmentedPinyin;
    recentCommit_.basePrefix = basePrefix;
    recentCommit_.contextAfter = contextAfter;
    recentCommit_.timestamp = std::chrono::steady_clock::now();
    recentCommit_.originalChars = std::move(utf8Chars);
    recentCommit_.originalSyllables = std::move(syllables);
    recentCommit_.sawDocumentText = !basePrefix.empty() || !contextAfter.empty();
    recentCommit_.valid = true;
}

bool ModernIMEController::checkRecentCommitDeletionFeedback(
    const std::string &before, const std::string &after) {
    if (!recentCommit_.valid || provider_ == nullptr || sensitive_ ||
        !options_.learningEnabled) {
        recentCommit_.valid = false;
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - recentCommit_.timestamp);
    if (elapsed > std::chrono::milliseconds(15000)) {
        recentCommit_.valid = false;
        return false;
    }

    const auto &chars = recentCommit_.originalChars;
    const auto &sylls = recentCommit_.originalSyllables;
    const std::size_t k = chars.size();
    if (k < 2 || sylls.size() != k) {
        recentCommit_.valid = false;
        return false;
    }

    // Compare both sides of the caret against the complete local edit window.
    // A shorter "before" alone proves no deletion: the missing text may simply
    // have moved to "after". Only an otherwise unchanged window is evidence.
    const std::string document = before + after;
    if (!document.empty()) {
        recentCommit_.sawDocumentText = true;
    }
    const auto matchesWindow = [&](std::string_view remaining) {
        const std::string expected = recentCommit_.basePrefix +
                                     std::string(remaining) +
                                     recentCommit_.contextAfter;
        // The frontend limits the caret's left window. A newly committed word
        // can displace the beginning of the old prefix; the complete remainder
        // and right context must still be visible before inferring an edit.
        return expected == document ||
               (document.size() >= remaining.size() +
                                       recentCommit_.contextAfter.size() &&
                expected.ends_with(document));
    };
    if (matchesWindow(recentCommit_.currentText)) {
        return false;
    }
    if (matchesWindow("")) {
        // An empty document is only evidence of "deleted everything" when the
        // committed text was actually observed in the document first. A first
        // observation that is already empty is a window jump (for example the
        // caret moved to a fresh empty field): drop tracking without punishing
        // the word (F2, 2026-10-24 容错专项).
        if (!recentCommit_.sawDocumentText) {
            recentCommit_.valid = false;
            return false;
        }
        provider_->onNegativeFeedback(recentCommit_.originalText,
                                     recentCommit_.originalPinyin);
        provider_->resetCommitHistory();
        recentCommit_.valid = false;
        return true;
    }

    // Search only proper subsegments of the current remainder. This preserves
    // middle-character extraction while refusing unrelated replacements,
    // document jumps, additions and undo operations.
    for (std::size_t length = k - 1; length > 0; --length) {
        for (std::size_t first = 0; first + length <= k; ++first) {
            std::string text;
            std::string pinyin;
            for (std::size_t index = first; index < first + length; ++index) {
                text += chars[index];
                if (!pinyin.empty()) { pinyin += '\''; }
                pinyin += sylls[index];
            }
            if (text.size() >= recentCommit_.currentText.size() ||
                recentCommit_.currentText.find(text) == std::string::npos ||
                !matchesWindow(text)) {
                continue;
            }
            // Even a single remaining character is tentative until the next
            // non-deletion input. A second deletion can still remove it.
            recentCommit_.currentText = std::move(text);
            recentCommit_.currentPinyin = std::move(pinyin);
            recentCommit_.timestamp = now;
            return true;
        }
    }
    recentCommit_.valid = false;
    provider_->resetCommitHistory();
    return false;
}

void ModernIMEController::finalizeRecentCommitDeletion() {
    if (!recentCommit_.valid || provider_ == nullptr || sensitive_ ||
        !options_.learningEnabled) {
        recentCommit_.valid = false;
        return;
    }
    if (std::chrono::steady_clock::now() - recentCommit_.timestamp >
        std::chrono::seconds(15)) {
        recentCommit_.valid = false;
        return;
    }
    if (!recentCommit_.currentText.empty() &&
        recentCommit_.currentText != recentCommit_.originalText) {
        provider_->onNegativeFeedback(recentCommit_.originalText,
                                     recentCommit_.originalPinyin);
        core::CommitEvent event;
        event.text = recentCommit_.currentText;
        event.fullPinyin = recentCommit_.currentPinyin;
        event.rawKeystrokes = recentCommit_.currentPinyin;
        event.role = core::CommitCandidateRole::Chinese;
        event.consumedInputBytes = recentCommit_.currentPinyin.size();
        event.timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        event.isDeviation = true;
        const bool singleCharacter =
            recentCommit_.currentPinyin.find('\'') == std::string::npos;
        if (singleCharacter) {
            // General single-character learning drives immediate promotion,
            // independently of the document's surrounding context.
            for (int repetition = 0; repetition < 3; ++repetition) {
                provider_->onCommitEvent(event);
            }
        }
        if (!singleCharacter || !recentCommit_.basePrefix.empty()) {
            event.contextBefore = recentCommit_.basePrefix;
            event.contextAfter = recentCommit_.contextAfter;
            provider_->onCommitEvent(event);
        }
        provider_->resetCommitHistory();
        // Learning sequence cleanup also clears the provider's context. The
        // host has already reported the edited document, and unchanged later
        // updates are deduplicated by setContext(), so restore that valid fact
        // before the next composition starts.
        if (contextAvailability_ == ContextAvailability::Valid) {
            provider_->setContext(contextBefore_, contextAfter_);
        }
    }
    recentCommit_.valid = false;
}

void ModernIMEController::setContext(std::string before, std::string after,
                                     ContextAvailability availability) {
    if (inSetContext_) {
        return;
    }
    inSetContext_ = true;
    struct ReentrancyGuard {
        bool &flag;
        ~ReentrancyGuard() { flag = false; }
    } guard{inSetContext_};

    if (sensitive_) { availability = ContextAvailability::Invalidated; }
    if (undoCommit_ && (availability != ContextAvailability::Valid ||
                       host_.textEditContext() != undoCommit_->expected)) undoCommit_.reset();

    const bool availabilityChanged = (availability != contextAvailability_);
    const bool contentChanged = (availability == ContextAvailability::Valid)
                                    ? (before != contextBefore_ || after != contextAfter_)
                                    : false;
    if (!availabilityChanged && !contentChanged) {
        return;
    }

    if (availability == ContextAvailability::Valid) {
        checkRecentCommitDeletionFeedback(before, after);
        contextBefore_ = std::move(before);
        contextAfter_ = std::move(after);
        fallbackContextBefore_ = contextBefore_;
        doubleQuoteOpen_ = quoteIsOpenAtCursor(
            contextBefore_, contextAfter_, core::kLeftDoubleQuote,
            core::kRightDoubleQuote);
        singleQuoteOpen_ = quoteIsOpenAtCursor(
            contextBefore_, contextAfter_, core::kLeftSingleQuote,
            core::kRightSingleQuote);
    } else {
        recentCommit_.valid = false;
        if (availability == ContextAvailability::Invalidated ||
            contextAvailability_ != ContextAvailability::Unavailable) {
            fallbackContextBefore_.clear();
            doubleQuoteOpen_ = false;
            singleQuoteOpen_ = false;
            if (provider_ != nullptr) { provider_->resetCommitHistory(); }
        }
        contextBefore_ = fallbackContextBefore_;
        contextAfter_.clear();
    }
    contextAvailability_ = availability;
    if (provider_ != nullptr) {
        provider_->setContext(contextBefore_, contextAfter_);
    }
    if (auxiliaryInputActive()) return;
    if (!page_.items.empty()) {
        if (clipboardMode_) {
            displayGeneration_++;
            page_.displayGeneration = displayGeneration_;
            host_.publishPage(page_);
        } else if (provider_ != nullptr) {
            publishProviderPage();
        } else {
            refreshPage();
        }
    }
}

void ModernIMEController::appendFallbackContext(std::string_view text) {
    if (text.empty()) {
        return;
    }
    fallbackContextBefore_.append(text);
    constexpr std::size_t maxFallbackChars = 32;
    std::size_t charCount = 0;
    std::size_t end = fallbackContextBefore_.size();
    while (end > 0 && charCount < maxFallbackChars) {
        std::size_t start = end - 1;
        while (start > 0 && (static_cast<unsigned char>(fallbackContextBefore_[start]) & 0xc0U) == 0x80U) {
            --start;
        }
        end = start;
        ++charCount;
    }
    if (end > 0) {
        fallbackContextBefore_.erase(0, end);
    }
    contextBefore_ = fallbackContextBefore_;
}

bool ModernIMEController::handle(const KeyEvent &event) {
    if (event.kind == KeyKind::Toggle) {
        if (!options_.inputEnabled) {
            return false;
        }
        setActive(!active_);
        return true;
    }
    if (!active()) {
        return false;
    }
    if (event.kind == KeyKind::ToggleTemporaryEnglish) {
        if (!options_.temporaryEnglishEnabled) return false;
        if (temporaryEnglishMode()) return finishTemporaryEnglish(true);
        return openAuxiliaryInput(AuxiliaryInputMode::TemporaryEnglish, "");
    }
    if (event.kind == KeyKind::OpenComponentLookup) {
        if (!options_.componentLookupEnabled) return false;
        return openAuxiliaryInput(AuxiliaryInputMode::ComponentLookup, "u");
    }
    if (event.kind == KeyKind::OpenCalculator) {
        if (!options_.calculatorEnabled) return false;
        return openAuxiliaryInput(AuxiliaryInputMode::Calculator, "v");
    }
    if (auxiliaryInputActive()) return handleAuxiliaryInput(event);
    if (event.kind == KeyKind::UndoCommit) return undoLastCommit();
    if (event.kind != KeyKind::PreviousCandidate && event.kind != KeyKind::NextCandidate &&
        event.kind != KeyKind::PreviousPage && event.kind != KeyKind::NextPage &&
        event.kind != KeyKind::PreviousClipboardItem && event.kind != KeyKind::NextClipboardItem)
        undoCommit_.reset();

    const bool isNavigationOrDeletion =
        (event.kind == KeyKind::Backspace || event.kind == KeyKind::DeleteForward ||
         event.kind == KeyKind::MoveCompositionLeft ||
         event.kind == KeyKind::MoveCompositionRight ||
         event.kind == KeyKind::PreviousCandidate ||
         event.kind == KeyKind::NextCandidate ||
         event.kind == KeyKind::PreviousClipboardItem ||
         event.kind == KeyKind::NextClipboardItem ||
         event.kind == KeyKind::PreviousPage || event.kind == KeyKind::NextPage);
    if (compositionInput_.empty() &&
        contextAvailability_ != ContextAvailability::Valid &&
        (isNavigationOrDeletion || event.kind == KeyKind::Enter ||
         event.kind == KeyKind::Escape)) {
        setContext("", "", ContextAvailability::Invalidated);
    }
    if (event.kind == KeyKind::Escape) {
        recentCommit_.valid = false;
    } else if (!isNavigationOrDeletion || !compositionInput_.empty()) {
        finalizeRecentCommitDeletion();
    }

    switch (event.kind) {
    case KeyKind::Character: {
        const bool isVMode = !compositionInput_.empty() && compositionInput_.front() == 'v';
        const bool isLegalChar = (event.character >= 'a' && event.character <= 'z') || event.character == '\'';
        const bool isLegalVChar = isVMode && ((event.character >= '0' && event.character <= '9') || event.character == '.');
        if (!isLegalChar && !isLegalVChar) {
            return false;
        }
        {
            std::string nextInput = compositionInput_;
            nextInput.insert(compositionCursor_, 1, event.character);
            if (!core::PinyinMatchPolicy::validComposition(nextInput)) {
                return false;
            }
            const bool appending = compositionCursor_ == compositionInput_.size();
            const bool changed = provider_
                                     ? (appending
                                            ? provider_->append(std::string_view(
                                                  &event.character, 1))
                                            : provider_->replaceInput(nextInput))
                                     : (appending
                                            ? input_.append(std::string_view(
                                                  &event.character, 1))
                                            : input_.replace(nextInput));
            if (!changed) {
                return false;
            }
            compositionInput_ = std::move(nextInput);
            ++compositionCursor_;
        }
        refreshPage();
        return true;
    }
    case KeyKind::Backspace:
        if (clipboardMode_) {
            clearComposition();
            return true;
        }
        if (compositionInput_.empty()) {
            return eraseEmptyPair();
        }
        if (compositionCursor_ == 0) {
            return true;
        }
        {
            auto nextInput = compositionInput_;
            nextInput.erase(compositionCursor_ - 1, 1);
            const auto nextCursor = compositionCursor_ - 1;
            const bool erasingLast = compositionCursor_ == compositionInput_.size();
            const bool changed = provider_
                                     ? (erasingLast ? provider_->eraseLast()
                                                    : provider_->replaceInput(nextInput))
                                     : (erasingLast ? input_.eraseLast()
                                                    : input_.replace(nextInput));
            if (!changed) {
                return false;
            }
            compositionInput_ = std::move(nextInput);
            compositionCursor_ = nextCursor;
        }
        refreshPage();
        return true;
    case KeyKind::DeleteCandidate:
        return removeCurrent();
    case KeyKind::CloseClipboard:
        if (!clipboardMode_) {
            return false;
        }
        clearComposition();
        return true;
    case KeyKind::DeleteForward:
        if (clipboardMode_) {
            clearComposition();
            return true;
        }
        if (compositionInput_.empty()) {
            return eraseEmptyPair();
        }
        if (compositionCursor_ >= compositionInput_.size()) {
            return true;
        }
        {
            auto nextInput = compositionInput_;
            nextInput.erase(compositionCursor_, 1);
            if (!replaceComposition(std::move(nextInput), compositionCursor_)) {
                return false;
            }
        }
        refreshPage();
        return true;
    case KeyKind::Escape:
        // 只在有内容需要取消时（拼音组合或剪贴板模式）消费 Escape；
        // 空闲状态下必须放行给应用（vim 退出插入模式、对话框取消等）。
        if (page_.preedit.empty() && !clipboardMode_) {
            return false;
        }
        if (provider_ != nullptr) {
            provider_->resetCommitHistory();
        }
        clearComposition();
        return true;
    case KeyKind::Enter:
        // 拼音组合中回车一律上屏原始拼音（主流拼音输入法惯例），
        // 不再提交第一个候选；剪贴板模式保持回车提交当前条目；
        // 空闲状态放行回车给应用。
        if (clipboardMode_) {
            return page_.items.empty() ? false : commitCurrent();
        }
        if (page_.preedit.empty()) {
            return false;
        }
        return commitRawPreedit();
    case KeyKind::Space:
        return page_.items.empty() ? commitRawPreedit(" ") : commitCurrent();
    case KeyKind::Punctuation: {
        if (event.character == '_' && !compositionInput_.empty() && !clipboardMode_) {
            const auto mapping = options_.punctuationOverrides.find('_');
            const bool literal = !options_.punctuationEnabled ||
                (mapping != options_.punctuationOverrides.end() ? mapping->second == "_" :
                 options_.underscoreStyle == core::UnderscoreStyle::Ascii ||
                 (options_.underscoreStyle == core::UnderscoreStyle::Smart && options_.smartPunctuation));
            const bool englishPending = !page_.items.empty() &&
                page_.items[page_.cursor].source != core::CandidateSource::None &&
                isAsciiText(page_.items[page_.cursor].text);
            // '_' continues an identifier. Completing a word here changes the
            // user's spelling (inter -> interface_) before their separator.
            if (literal && (englishPending ||
                    (options_.smartPunctuation && inAsciiToken(contextBefore_, contextAfter_))))
                return commitRawPreedit("_");
        }
        if (!compositionInput_.empty() && compositionInput_.front() == 'v' &&
            event.character == '.' && compositionInput_.find('.') == std::string::npos) {
            std::string nextInput = compositionInput_;
            nextInput.insert(compositionCursor_, 1, event.character);
            if (core::PinyinMatchPolicy::validComposition(nextInput)) {
                const bool appending = compositionCursor_ == compositionInput_.size();
                const bool changed = provider_
                                         ? (appending
                                                ? provider_->append(std::string_view(&event.character, 1))
                                                : provider_->replaceInput(nextInput))
                                         : (appending
                                                ? input_.append(std::string_view(&event.character, 1))
                                                : input_.replace(nextInput));
                if (changed) {
                    compositionInput_ = std::move(nextInput);
                    ++compositionCursor_;
                    refreshPage();
                    return true;
                }
            }
        }
        if (options_.punctuationEnabled &&
            commitPunctuation(event.character)) {
            return true;
        }
        if (page_.preedit.empty()) {
            return false;
        }
        const bool hasNoneCandidate = !page_.items.empty() &&
            page_.items[0].source == core::CandidateSource::None;
        if (hasNoneCandidate) {
            return false;
        }
        if (!page_.items.empty()) {
            if (!commitCurrent()) {
                return false;
            }
        } else if (!commitRawPreedit()) {
            return false;
        }
        host_.commit(std::string_view(&event.character, 1));
        return true;
    }
    case KeyKind::Digit: {
        char triggerChar = 'v';
        char triggerDigit = '2';
        if (options_.clipboardTrigger.size() == 3 && options_.clipboardTrigger[1] == '+') {
            triggerChar = static_cast<char>(std::tolower(static_cast<unsigned char>(options_.clipboardTrigger[0])));
            triggerDigit = options_.clipboardTrigger[2];
        }
        if (options_.clipboardEnabled &&
            compositionInput_.size() == 1 &&
            std::tolower(static_cast<unsigned char>(compositionInput_.front())) == triggerChar &&
            event.digit == triggerDigit) {
            return openClipboard();
        }
        if (!compositionInput_.empty() && compositionInput_.front() == 'v') {
            std::string nextInput = compositionInput_;
            nextInput.insert(compositionCursor_, 1, event.digit);
            if (core::PinyinMatchPolicy::validComposition(nextInput)) {
                const bool appending = compositionCursor_ == compositionInput_.size();
                const bool changed = provider_
                                         ? (appending
                                                ? provider_->append(std::string_view(&event.digit, 1))
                                                : provider_->replaceInput(nextInput))
                                         : (appending
                                                ? input_.append(std::string_view(&event.digit, 1))
                                                : input_.replace(nextInput));
                if (changed) {
                    compositionInput_ = std::move(nextInput);
                    ++compositionCursor_;
                    refreshPage();
                    return true;
                }
            }
        }
        if (!options_.numberSelection || event.digit < '1' || event.digit > '9') {
            commitRawPreedit();
            return false;
        }
        const auto index = static_cast<std::size_t>(event.digit - '1');
        const auto boundary = currentPageBoundary();
        if (page_.items.empty()) {
            if (!page_.preedit.empty()) {
                commitRawPreedit();
            }
            return false;
        }
        const auto candidateIndex = boundary.begin + index;
        if (candidateIndex >= boundary.end) {
            return true;
        }
        return select(candidateIndex);
    }
    case KeyKind::PreviousCandidate:
        if (!options_.arrowNavigation) {
            return false;
        }
        return moveCursor(-1);
    case KeyKind::NextCandidate:
        if (!options_.arrowNavigation) {
            return false;
        }
        return moveCursor(1);
    case KeyKind::MoveCompositionLeft:
        if (compositionInput_.empty()) {
            return false;
        }
        if (compositionCursor_ > 0) {
            --compositionCursor_;
            updatePreeditCursor();
            host_.publishPage(page_);
        }
        return true;
    case KeyKind::MoveCompositionRight:
        if (compositionInput_.empty()) {
            return false;
        }
        if (compositionCursor_ < compositionInput_.size()) {
            ++compositionCursor_;
            updatePreeditCursor();
            host_.publishPage(page_);
        }
        return true;
    case KeyKind::PreviousClipboardItem:
        if (!options_.arrowNavigation) {
            return false;
        }
        if (clipboardMode_) {
            return moveCursor(-1);
        }
        return movePage(-1);
    case KeyKind::NextClipboardItem:
        if (!options_.arrowNavigation) {
            return false;
        }
        if (clipboardMode_) {
            return moveCursor(1);
        }
        return movePage(1);
    case KeyKind::PreviousPage:
        if (!options_.pageNavigation) {
            return false;
        }
        return movePage(-1);
    case KeyKind::NextPage:
        if (!options_.pageNavigation) {
            return false;
        }
        return movePage(1);
    case KeyKind::OpenClipboard:
        if (!options_.clipboardEnabled) {
            return false;
        }
        return openClipboard();
    case KeyKind::CommitFirstCharacter:
        if (options_.characterExtractionEnabled && extractCharacter(true)) {
            return true;
        }
        if (page_.preedit.empty()) {
            if (options_.punctuationEnabled && commitPunctuation('[')) {
                return true;
            }
            return false;
        }
        return false;
    case KeyKind::CommitLastCharacter:
        if (options_.characterExtractionEnabled && extractCharacter(false)) {
            return true;
        }
        if (page_.preedit.empty()) {
            if (options_.punctuationEnabled && commitPunctuation(']')) {
                return true;
            }
            return false;
        }
        return false;
    case KeyKind::Toggle:
    case KeyKind::UndoCommit:
    case KeyKind::ToggleTemporaryEnglish:
    case KeyKind::OpenComponentLookup:
    case KeyKind::OpenCalculator:
        break;
    }
    return false;
}

bool ModernIMEController::select(const core::CandidateSelectionRequest &request) {
    if (!active()) {
        return false;
    }
    if (request.sessionId != sessionId_ ||
        request.displayGeneration != displayGeneration_) {
        return false;
    }
    if (request.globalIndex >= page_.items.size()) {
        return false;
    }
    if (page_.items[request.globalIndex].source == core::CandidateSource::None) {
        return false;
    }
    const auto currentId = core::candidateIdentity(page_.items[request.globalIndex], page_);
    if (!(currentId == request.expected)) {
        return false;
    }

    if (auxiliaryInputActive()) {
        const auto text = page_.items[request.globalIndex].text;
        host_.commit(text);
        appendFallbackContext(text);
        clearComposition();
        return true;
    }

    if (clipboardMode_) {
        if (page_.items[request.globalIndex].source == core::CandidateSource::Command) {
            clearComposition();
            return true;
        }
        const auto text = page_.items[request.globalIndex].text;
        host_.commit(text);
        clearComposition();
        return true;
    }

    if (provider_) {
        const auto basePrefix = contextBefore_;
        const auto editContext = !sensitive_ && options_.undoCommitEnabled
            ? host_.textEditContext() : std::nullopt;
        const auto originalInput = compositionInput_;
        auto outcome = provider_->selectConfirmed(request, providerGeneration_);
        if (!outcome.has_value()) {
            return false;
        }
        if (outcome->action == core::SelectionAction::OpenClipboard) {
            if (!options_.clipboardEnabled) {
                return false;
            }
            return openClipboard();
        }
        if (outcome->action == core::SelectionAction::OpenCalculator) {
            if (!options_.calculatorEnabled) return false;
            return openAuxiliaryInput(AuxiliaryInputMode::Calculator, "v");
        }
        if (outcome->action == core::SelectionAction::NoCommit) {
            return true;
        }

        const auto text = outcome->selected.text;
        host_.commit(text);
        rememberUndo(editContext, outcome->selected, originalInput, outcome->remainingInput);
        appendFallbackContext(text);

        if (!outcome->remainingInput.empty()) {
            publishProviderPage(true);
            return true;
        }

        if (!sensitive_ && options_.learningEnabled &&
            (outcome->selected.role == core::CommitCandidateRole::Chinese ||
             outcome->selected.role == core::CommitCandidateRole::UserDictionary) &&
            !outcome->selected.text.empty()) {
            initRecentCommit(page_.items[request.globalIndex], basePrefix, contextAfter_);
        } else {
            finalizeRecentCommitDeletion();
        }
        clearComposition();
        return true;
    }

    if (page_.items[request.globalIndex].source == core::CandidateSource::Command) {
        if (core::candidateIdentity(page_.items[request.globalIndex], page_).action ==
            core::SelectionAction::OpenCalculator) {
            if (!options_.calculatorEnabled) return false;
            return openAuxiliaryInput(AuxiliaryInputMode::Calculator, "v");
        }
        if (!options_.clipboardEnabled) {
            return false;
        }
        clearComposition();
        return openClipboard();
    }
    page_.cursor = request.globalIndex;
    const auto text = page_.items[request.globalIndex].text;
    const auto context = !sensitive_ && options_.undoCommitEnabled ? host_.textEditContext() : std::nullopt;
    const auto identity = core::candidateIdentity(page_.items[request.globalIndex], page_);
    const auto originalInput = compositionInput_;
    host_.commit(text);
    rememberUndo(context, identity, originalInput, "");
    clearComposition();
    return true;
}

bool ModernIMEController::select(std::size_t index) {
    if (!active() || index >= page_.items.size()) {
        return false;
    }
    if (page_.items[index].source == core::CandidateSource::None) {
        return false;
    }
    core::CandidateSelectionRequest req;
    req.sessionId = sessionId_;
    req.displayGeneration = displayGeneration_;
    req.globalIndex = index;
    req.expected = core::candidateIdentity(page_.items[index], page_);
    return select(req);
}

bool ModernIMEController::extractCharacter(bool firstChar) {
    finalizeRecentCommitDeletion();
    if (!active() || clipboardMode_ || page_.items.empty() || provider_ == nullptr) {
        return false;
    }
    if (page_.cursor >= page_.items.size() ||
        page_.items[page_.cursor].source == core::CandidateSource::None) {
        return false;
    }
    const auto candidateIndex = page_.cursor;
    std::string committedText;
    if (!provider_->extractCharacter(candidateIndex, firstChar, &committedText)) {
        return false;
    }
    host_.commit(committedText);
    appendFallbackContext(committedText);
    clearComposition();
    return true;
}

bool ModernIMEController::removeCurrent() {
    if (!active() || clipboardMode_ || page_.items.empty() ||
        provider_ == nullptr) {
        return false;
    }
    if (page_.cursor >= page_.items.size() ||
        page_.items[page_.cursor].source == core::CandidateSource::None) {
        return false;
    }
    if (!provider_->remove(page_.cursor)) {
        return false;
    }
    publishProviderPage();
    return true;
}

bool ModernIMEController::commitPunctuation(char ascii) {
    const auto override = options_.punctuationOverrides.find(ascii);
    const bool asciiContext = endsWithAsciiAlnum(contextBefore_, contextAfter_) ||
        (options_.smartPunctuation && inAsciiToken(contextBefore_, contextAfter_));
    if (override == options_.punctuationOverrides.end() &&
        options_.autoPairPunctuation && editPairedPunctuation(ascii)) return true;
    // Punctuation inside an ASCII run stays half-width so inputs such as
    // "3.14", "1,000" and English fragments survive. During composition the
    // pending candidate decides: committing Chinese goes with full-width,
    // committing a raw ASCII fragment goes half-width.
    bool afterAscii = asciiContext;
    if (!page_.preedit.empty()) {
        std::string_view pending = page_.preedit;
        if (!page_.items.empty()) {
            const auto index = std::min(page_.cursor, page_.items.size() - 1);
            pending = page_.items[index].text;
        }
        afterAscii = afterAscii || isAsciiText(pending);
    }
    std::optional<std::string> converted;
    if (override != options_.punctuationOverrides.end()) {
        converted = override->second;
    } else if (ascii == '_' && options_.underscoreStyle != core::UnderscoreStyle::Smart) {
        converted = options_.underscoreStyle == core::UnderscoreStyle::Ascii ? "_" : "——";
    } else if (!afterAscii) {
        switch (ascii) {
        case '"':
            converted = std::string(doubleQuoteOpen_
                                        ? core::kRightDoubleQuote
                                        : core::kLeftDoubleQuote);
            break;
        case '\'':
            converted = std::string(singleQuoteOpen_
                                        ? core::kRightSingleQuote
                                        : core::kLeftSingleQuote);
            break;
        default:
            converted = core::fullWidthPunctuation(ascii);
            break;
        }
    }
    if (!converted.has_value()) {
        return false;
    }
    finalizeRecentCommitDeletion();
    if (!page_.preedit.empty()) {
        const bool hasNoneCandidate = !page_.items.empty() &&
            page_.items[0].source == core::CandidateSource::None;
        if (hasNoneCandidate) {
            return false;
        }
        const bool committed = !page_.items.empty() ? commitCurrent()
                                                    : commitRawPreedit();
        if (!committed) {
            return false;
        }
    }
    host_.commit(*converted);
    fallbackContextBefore_.clear();
    if (provider_ != nullptr) {
        provider_->resetCommitHistory();
    }
    if (ascii == '"') {
        doubleQuoteOpen_ = !doubleQuoteOpen_;
    } else if (ascii == '\'') {
        singleQuoteOpen_ = !singleQuoteOpen_;
    }
    return true;
}

bool ModernIMEController::editPairedPunctuation(char ascii) {
    if (sensitive_) return false;
    const auto context = host_.textEditContext();
    if (!context) { autoPairs_.clear(); return false; }
    if (!autoPairs_.empty() && !insidePair(*context, autoPairs_.back().expected)) autoPairs_.clear();
    const auto commitPending = [&]() {
        if (page_.preedit.empty()) return true;
        if (page_.items.empty()) return commitRawPreedit();
        const auto &item = page_.items[page_.cursor];
        if (item.source == core::CandidateSource::None ||
            (item.consumedInputBytes && item.consumedInputBytes < compositionInput_.size())) return false;
        return commitCurrent();
    };
    if (!autoPairs_.empty()) {
        const auto pair = autoPairs_.back();
        const bool closing = (ascii == ')' && pair.closer == "）") ||
            (ascii == ']' && pair.closer == "】") || (ascii == '>' && pair.closer == "》") ||
            (ascii == '"' && pair.closer == "”") || (ascii == '\'' && pair.closer == "’");
        if (closing && commitPending()) {
            autoPairs_.pop_back();
            undoCommit_.reset();
            recentCommit_.valid = false;
            if (provider_) provider_->resetCommitHistory();
            return host_.replaceSurrounding("", pair.closer, pair.closer);
        }
    }
    if (endsWithAsciiAlnum(contextBefore_, contextAfter_) ||
        (options_.smartPunctuation && inAsciiToken(contextBefore_, contextAfter_))) return false;
    if (!page_.preedit.empty()) {
        if (page_.items.empty() || page_.items[page_.cursor].source == core::CandidateSource::None ||
            isAsciiText(page_.items[page_.cursor].text)) return false;
        // A punctuation key cannot silently consume only a prefix and leave
        // the caret inside an unrelated document edit.
        const auto &item = page_.items[page_.cursor];
        if (item.consumedInputBytes && item.consumedInputBytes < compositionInput_.size()) return false;
    }
    std::string opener, closer;
    switch (ascii) {
    case '(': opener = "（"; closer = "）"; break;
    case '[': opener = "【"; closer = "】"; break;
    case '<': opener = "《"; closer = "》"; break;
    case '"': opener = "“"; closer = "”"; break;
    case '\'': opener = "‘"; closer = "’"; break;
    default: return false;
    }
    if (context->after.starts_with(closer)) return false;
    if (!page_.preedit.empty() && !commitCurrent()) return false;
    const auto insertion = host_.textEditContext();
    if (!insertion || !host_.insertPair(opener, closer)) return false;
    if (autoPairs_.size() == 32) autoPairs_.erase(autoPairs_.begin());
    autoPairs_.push_back({opener, closer, afterInsertion(*insertion, opener, closer)});
    undoCommit_.reset();
    recentCommit_.valid = false;
    if (provider_) provider_->resetCommitHistory();
    fallbackContextBefore_.clear();
    return true;
}

bool ModernIMEController::eraseEmptyPair() {
    if (!options_.autoPairPunctuation || sensitive_ || autoPairs_.empty()) return false;
    const auto context = host_.textEditContext();
    const auto pair = autoPairs_.back();
    if (!context || !insidePair(*context, pair.expected)) { autoPairs_.clear(); return false; }
    if (*context != pair.expected) return false;
    autoPairs_.pop_back();
    return host_.replaceSurrounding(pair.opener, pair.closer, "");
}

void ModernIMEController::invalidateTextActions() { autoPairs_.clear(); undoCommit_.reset(); }

void ModernIMEController::rememberUndo(const std::optional<TextEditContext> &before,
                                      const core::CandidateIdentity &candidate,
                                      std::string rawInput, std::string remainingInput) {
    undoCommit_.reset();
    if (!before || sensitive_ || !options_.undoCommitEnabled || candidate.text.empty() || rawInput.empty() ||
        candidate.text.find_first_of("\r\n") != std::string::npos ||
        candidate.role == core::CommitCandidateRole::Generated || candidate.role == core::CommitCandidateRole::Command)
        return;
    undoCommit_ = UndoCommit{afterInsertion(*before, candidate.text), candidate,
                            std::move(rawInput), std::move(remainingInput), std::chrono::steady_clock::now()};
}

bool ModernIMEController::undoLastCommit() {
    if (!active() || sensitive_ || !options_.undoCommitEnabled || !undoCommit_ || clipboardMode_) return false;
    const auto undo = std::move(*undoCommit_);
    undoCommit_.reset();
    if (std::chrono::steady_clock::now() - undo.timestamp > std::chrono::seconds(15) ||
        compositionInput_ != undo.remainingInput || host_.textEditContext() != undo.expected) return false;
    recentCommit_.valid = false;
    if (!host_.replaceSurrounding(undo.candidate.text, "", "")) return false;
    autoPairs_.clear();
    if (provider_) provider_->resetCommitHistory();
    if (!replaceComposition(undo.rawInput, undo.rawInput.size())) {
        host_.commit(undo.candidate.text);
        replaceComposition(undo.remainingInput, undo.remainingInput.size());
        refreshPage();
        return false;
    }
    if (const auto context = host_.textEditContext())
        setContext(context->before, context->after, ContextAvailability::Valid);
    if (provider_ && options_.learningEnabled &&
        (undo.candidate.role == core::CommitCandidateRole::Chinese ||
         undo.candidate.role == core::CommitCandidateRole::UserDictionary))
        provider_->onNegativeFeedback(undo.candidate.text, undo.candidate.fullPinyin);
    refreshPage();
    return true;
}

bool ModernIMEController::commitRawPreedit(std::string_view suffix) {
    finalizeRecentCommitDeletion();
    if (page_.preedit.empty()) {
        return false;
    }
    const auto preedit = compositionInput_.empty() ? page_.preedit
                                                   : compositionInput_;
    if (!sensitive_ && provider_ != nullptr) {
        core::CommitEvent event;
        event.text = preedit;
        event.rawKeystrokes = compositionInput_.empty() ? page_.rawInput : compositionInput_;
        event.fullPinyin = event.rawKeystrokes;
        event.role = core::CommitCandidateRole::Raw;
        event.isPartial = false;
        event.consumedInputBytes = event.rawKeystrokes.size();
        event.contextBefore = contextBefore_;
        event.contextAfter = contextAfter_;
        event.timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        provider_->onCommitEvent(event);
    }
    fallbackContextBefore_.clear();
    clearComposition();
    host_.commit(preedit);
    if (!suffix.empty()) {
        host_.commit(suffix);
    }
    return true;
}

void ModernIMEController::clearComposition() {
    auxiliaryMode_ = AuxiliaryInputMode::None;
    auxiliaryInput_.clear(); auxiliaryCursor_ = 0;
    contextBefore_ = fallbackContextBefore_;
    contextAfter_.clear();
    clipboardMode_ = false;
    clipboardEntries_.reset();
    compositionInput_.clear();
    compositionCursor_ = 0;
    if (provider_ != nullptr) {
        provider_->setContext(contextBefore_, contextAfter_);
    }
    if (provider_) {
        provider_->reset();
        publishProviderPage();
    } else {
        displayGeneration_++;
        input_.clear();
        page_.clear();
        page_.sessionId = sessionId_;
        page_.displayGeneration = displayGeneration_;
        host_.publishPage(page_);
    }
}

void ModernIMEController::reset() {
    invalidateTextActions();
    displayGeneration_++;
    recentCommit_.valid = false;
    contextAvailability_ = ContextAvailability::Unavailable;
    fallbackContextBefore_.clear();
    if (provider_ != nullptr) {
        provider_->resetCommitHistory();
    }
    clearComposition();
    doubleQuoteOpen_ = false;
    singleQuoteOpen_ = false;
}

void ModernIMEController::setSensitive(bool sensitive) {
    if (sensitive_ == sensitive) {
        return;
    }
    sensitive_ = sensitive;
    if (sensitive_) {
        if (auxiliaryInputActive()) cancelAuxiliaryInput();
        invalidateTextActions();
        recentCommit_.valid = false;
        contextAvailability_ = ContextAvailability::Invalidated;
        fallbackContextBefore_.clear();
        contextBefore_.clear();
        contextAfter_.clear();
        if (provider_ != nullptr) {
            provider_->setLearningEnabled(false);
            provider_->setContextLearningEnabled(false);
            provider_->resetCommitHistory();
        }
    } else if (provider_ != nullptr) {
        provider_->setLearningEnabled(options_.learningEnabled);
        provider_->setContextLearningEnabled(options_.contextLearningEnabled);
    }
    if (!page_.items.empty()) {
        if (clipboardMode_) {
            displayGeneration_++;
            page_.displayGeneration = displayGeneration_;
            host_.publishPage(page_);
        } else if (provider_ != nullptr) {
            publishProviderPage();
        } else {
            refreshPage();
        }
    } else {
        displayGeneration_++;
    }
}

void ModernIMEController::setOptions(ControllerOptions options) {
    if (options.autoPairPunctuation != options_.autoPairPunctuation ||
        options.punctuationEnabled != options_.punctuationEnabled ||
        options.punctuationOverrides != options_.punctuationOverrides ||
        options.undoCommitEnabled != options_.undoCommitEnabled) invalidateTextActions();
    options_ = options;
    if (temporaryEnglishMode() && !options_.temporaryEnglishEnabled) finishTemporaryEnglish(true);
    if ((auxiliaryMode_ == AuxiliaryInputMode::ComponentLookup && !options_.componentLookupEnabled) ||
        (auxiliaryMode_ == AuxiliaryInputMode::Calculator && !options_.calculatorEnabled)) cancelAuxiliaryInput();
    if (!options_.learningEnabled) { recentCommit_.valid = false; }
    if (provider_ != nullptr && !sensitive_) {
        provider_->setLearningEnabled(options_.learningEnabled);
        provider_->setContextLearningEnabled(options_.contextLearningEnabled);
    }
    if (!options_.inputEnabled) {
        reset();
    } else if (auxiliaryInputActive()) {
        publishAuxiliaryPage();
    } else if (!page_.items.empty()) {
        if (clipboardMode_) {
            displayGeneration_++;
            page_.displayGeneration = displayGeneration_;
            host_.publishPage(page_);
        } else if (provider_ != nullptr) {
            publishProviderPage();
        } else {
            refreshPage();
        }
    } else {
        displayGeneration_++;
    }
}

void ModernIMEController::setActive(bool active) {
    active_ = active;
    recentCommit_.valid = false;
    if (!this->active()) {
        reset();
    }
}

void ModernIMEController::setClipboardEntries(std::vector<std::string> entries) {
    setClipboardSnapshot(std::make_shared<const std::vector<std::string>>(std::move(entries)),
                         core::ClipboardAvailability::Ready);
}

void ModernIMEController::setClipboardSnapshot(
    std::shared_ptr<const std::vector<std::string>> entries,
    core::ClipboardAvailability availability) {
    const bool refreshPlaceholder = clipboardMode_ && !sensitive_ &&
        clipboardPageAvailability_ != core::ClipboardAvailability::Ready &&
        clipboardPageAvailability_ != availability;
    if (entries == clipboardEntries_ && availability == clipboardAvailability_ && !refreshPlaceholder) return;
    clipboardEntries_ = std::move(entries);
    clipboardAvailability_ = availability;
    if (refreshPlaceholder) openClipboard();
}

std::size_t ModernIMEController::pageSize() const {
    const auto boundary = currentPageBoundary();
    return boundary.end - boundary.begin;
}

core::PageBoundary ModernIMEController::currentPageBoundary() const {
    if (page_.items.empty()) {
        return {};
    }
    if (page_.pageBoundaries.empty()) {
        return {0, page_.items.size()};
    }
    return page_.pageBoundaries[currentPageIndex()];
}

void ModernIMEController::ensurePageBoundaries() {
    if (!page_.pageBoundaries.empty()) {
        return;
    }
    const auto step = options_.pageSize > 0 ? options_.pageSize : kFallbackCandidatePageSize;
    for (std::size_t begin = 0; begin < page_.items.size();
         begin += step) {
        page_.pageBoundaries.push_back(
            {begin, std::min(begin + step,
                             page_.items.size())});
    }
}

bool ModernIMEController::setPageBoundaries(std::uint64_t displayGeneration,
                                           std::uint64_t layoutGeneration,
                                           std::vector<core::PageBoundary> boundaries) {
    if (displayGeneration != displayGeneration_) {
        return false;
    }
    if (layoutGeneration < layoutGeneration_) {
        return false;
    }
    const auto total = page_.items.size();
    if (boundaries.empty()) {
        if (total != 0) {
            return false;
        }
    } else {
        std::size_t expected = 0;
        for (const auto &b : boundaries) {
            if (b.begin != expected || b.begin >= b.end || b.end > total) {
                return false;
            }
            expected = b.end;
        }
        if (expected != total) {
            return false;
        }
    }
    layoutGeneration_ = layoutGeneration;
    page_.pageBoundaries = std::move(boundaries);
    if (!page_.items.empty()) {
        page_.cursor = std::min(page_.cursor, page_.items.size() - 1);
    }
    return true;
}

void ModernIMEController::setPageBoundaries(std::vector<core::PageBoundary> boundaries) {
    setPageBoundaries(displayGeneration_, layoutGeneration_, std::move(boundaries));
}

bool ModernIMEController::loadMoreCandidates() {
    if (clipboardMode_ || !provider_ || !provider_->loadMoreCandidates()) return false;
    const auto &extended = provider_->page();
    const auto oldSize = page_.items.size();
    const bool samePrefix = page_.rawInput == extended.rawInput && extended.items.size() >= oldSize &&
        std::equal(page_.items.begin(), page_.items.end(), extended.items.begin(),
            [&](const auto &a, const auto &b) {
                return core::candidateIdentity(a, page_) == core::candidateIdentity(b, extended);
            });
    const auto cursor = samePrefix
        ? (extended.items.size() > oldSize ? oldSize : page_.cursor) : std::size_t{0};
    publishProviderPage(false, cursor);
    return true;
}

void ModernIMEController::publishProviderPage(bool adoptRemainingComposition,
                                             std::optional<std::size_t> cursor) {
    displayGeneration_++;
    page_ = provider_->page();
    page_.sessionId = sessionId_;
    page_.displayGeneration = displayGeneration_;
    providerGeneration_ = page_.generation;
    if (cursor && !page_.items.empty()) page_.cursor = std::min(*cursor, page_.items.size() - 1);
    if (adoptRemainingComposition && !page_.rawInput.empty()) {
        compositionInput_ = page_.rawInput;
        compositionCursor_ = compositionInput_.size();
    }
    updatePreeditCursor();
    ensurePageBoundaries();
    host_.publishPage(page_);
}

void ModernIMEController::refreshPage() {
    if (auxiliaryInputActive()) { publishAuxiliaryPage(); return; }
    if (clipboardMode_) {
        return;
    }
    if (provider_) {
        publishProviderPage();
        return;
    }
    displayGeneration_++;
    page_.clear();
    page_.sessionId = sessionId_;
    page_.displayGeneration = displayGeneration_;
    page_.preedit = input_.text();
    page_.generation = input_.generation();
    providerGeneration_ = page_.generation;
    updatePreeditCursor();
    // 无 provider 的降级路径（正常安装不会出现）：唯一候选就是原始
    // 输入本身，保持行为可预期，不再注入开发期示例候选。
    if (!input_.text().empty()) {
        core::CandidateItem item;
        item.text = input_.text();
        item.fullPinyin = input_.text();
        item.consumedInputBytes = input_.text().size();
        item.source = core::CandidateSource::Raw;
        item.commitRole = core::CommitCandidateRole::Raw;
        page_.items.push_back(std::move(item));
    }
    ensurePageBoundaries();
    host_.publishPage(page_);
}

bool ModernIMEController::openAuxiliaryInput(AuxiliaryInputMode mode, std::string_view seed) {
    if (sensitive_ || auxiliaryInputActive() || clipboardMode_ ||
        (!compositionInput_.empty() && compositionInput_ != seed)) return false;
    finalizeRecentCommitDeletion();
    invalidateTextActions();
    if (provider_) { provider_->resetCommitHistory(); provider_->reset(); }
    input_.clear(); compositionInput_.clear(); compositionCursor_ = 0;
    auxiliaryMode_ = mode;
    auxiliaryInput_.clear(); auxiliaryCursor_ = 0;
    publishAuxiliaryPage();
    return true;
}

void ModernIMEController::publishAuxiliaryPage() {
    page_.clear();
    page_.sessionId = sessionId_;
    page_.displayGeneration = ++displayGeneration_;
    page_.generation = displayGeneration_;
    page_.rawInput = auxiliaryInput_;
    const std::string_view prefix = auxiliaryMode_ == AuxiliaryInputMode::ComponentLookup ? "拆字：" :
        auxiliaryMode_ == AuxiliaryInputMode::Calculator ? "计算：" : "";
    page_.preedit = std::string(prefix) + auxiliaryInput_;
    if (auxiliaryMode_ == AuxiliaryInputMode::ComponentLookup) {
        if (componentLookup_) {
            for (const auto &match : componentLookup_->lookup(auxiliaryInput_)) {
                core::CandidateItem item;
                item.text = match.text;
                item.annotation = match.components;
                item.source = core::CandidateSource::Engine;
                item.commitRole = core::CommitCandidateRole::Generated;
                item.consumedInputBytes = auxiliaryInput_.size();
                page_.items.push_back(std::move(item));
            }
        }
        if (page_.items.empty()) {
            core::CandidateItem hint;
            hint.text = !componentLookup_ || !componentLookup_->available() ? "拆字数据不可用" :
                auxiliaryInput_.empty() ? "输入部件拼音，如 mumu → 林" : "没有匹配的拆字";
            hint.source = core::CandidateSource::None;
            page_.items.push_back(std::move(hint));
        }
    }
    if (auxiliaryMode_ == AuxiliaryInputMode::Calculator) {
        const auto result = core::calculate(auxiliaryInput_);
        core::CandidateItem item;
        item.text = result.valid ? result.text : auxiliaryInput_.empty() ? "输入算式，如 (2+3)*4" : result.error;
        item.source = result.valid ? core::CandidateSource::Engine : core::CandidateSource::None;
        item.commitRole = core::CommitCandidateRole::Generated;
        item.consumedInputBytes = auxiliaryInput_.size();
        if (result.valid) item.annotation = "计算结果";
        page_.items.push_back(std::move(item));
    }
    page_.preeditCursor = auxiliaryCursor_ + prefix.size();
    ensurePageBoundaries();
    host_.publishPage(page_);
}

bool ModernIMEController::finishTemporaryEnglish(bool commitText) {
    if (!temporaryEnglishMode()) return false;
    const auto text = auxiliaryInput_;
    fallbackContextBefore_.clear();
    clearComposition();
    if (commitText && !text.empty()) host_.commit(text);
    return true;
}

bool ModernIMEController::handleAuxiliaryInput(const KeyEvent &event) {
    if (!temporaryEnglishMode()) {
        if (event.kind == KeyKind::Space || event.kind == KeyKind::Enter ||
            (auxiliaryMode_ == AuxiliaryInputMode::Calculator && event.kind == KeyKind::Character && event.character == '=')) {
            select(page_.cursor); return true;
        }
        if (auxiliaryMode_ == AuxiliaryInputMode::ComponentLookup && (event.kind == KeyKind::Digit ||
            (event.kind == KeyKind::Character && event.character >= '1' && event.character <= '9'))) {
            if (options_.numberSelection) {
                const auto digit = event.kind == KeyKind::Digit ? event.digit : event.character;
                if (digit >= '1' && digit <= '9') {
                    const auto boundary = currentPageBoundary();
                    const auto index = boundary.begin + static_cast<std::size_t>(digit - '1');
                    if (index < boundary.end) select(index);
                }
            }
            return true;
        }
        if (event.kind == KeyKind::PreviousCandidate) return moveCursor(-1);
        if (event.kind == KeyKind::NextCandidate) return moveCursor(1);
        if (event.kind == KeyKind::PreviousPage) return movePage(-1);
        if (event.kind == KeyKind::NextPage) return movePage(1);
    }
    switch (event.kind) {
    case KeyKind::Character:
        if (event.character < 0x21 || event.character > 0x7e) return false;
        if (auxiliaryInput_.size() < (auxiliaryMode_ == AuxiliaryInputMode::Calculator ? 512U : 128U))
            auxiliaryInput_.insert(auxiliaryCursor_++, 1, event.character);
        break;
    case KeyKind::Digit:
        if (auxiliaryMode_ != AuxiliaryInputMode::Calculator) return false;
        if (auxiliaryInput_.size() < 512) auxiliaryInput_.insert(auxiliaryCursor_++, 1, event.digit);
        break;
    case KeyKind::Backspace:
        if (auxiliaryInput_.empty()) { cancelAuxiliaryInput(); return true; }
        if (auxiliaryCursor_) auxiliaryInput_.erase(--auxiliaryCursor_, 1);
        break;
    case KeyKind::DeleteForward:
        if (auxiliaryCursor_ < auxiliaryInput_.size()) auxiliaryInput_.erase(auxiliaryCursor_, 1);
        break;
    case KeyKind::MoveCompositionLeft:
        if (auxiliaryCursor_) --auxiliaryCursor_;
        break;
    case KeyKind::MoveCompositionRight:
        if (auxiliaryCursor_ < auxiliaryInput_.size()) ++auxiliaryCursor_;
        break;
    case KeyKind::Enter: return finishTemporaryEnglish(true);
    case KeyKind::Space:
        finishTemporaryEnglish(true); host_.commit(" "); return true;
    case KeyKind::Escape: cancelAuxiliaryInput(); return true;
    default: return false;
    }
    publishAuxiliaryPage();
    return true;
}

bool ModernIMEController::replaceComposition(std::string nextInput,
                                             std::size_t nextCursor) {
    if (!nextInput.empty() &&
        !core::PinyinMatchPolicy::validComposition(nextInput)) {
        return false;
    }
    const bool changed = provider_ ? provider_->replaceInput(nextInput)
                                   : input_.replace(nextInput);
    if (!changed) {
        return false;
    }
    compositionInput_ = std::move(nextInput);
    compositionCursor_ = std::min(nextCursor, compositionInput_.size());
    return true;
}

void ModernIMEController::updatePreeditCursor() {
    page_.preeditCursor = displayedPreeditCursor(
        page_.preedit, compositionInput_, compositionCursor_);
}

bool ModernIMEController::openClipboard() {
    if (!active() || !options_.clipboardEnabled) {
        return false;
    }

    if (provider_ != nullptr) {
        provider_->reset();
    } else {
        input_.clear();
    }
    displayGeneration_++;
    page_.clear();
    page_.sessionId = sessionId_;
    page_.displayGeneration = displayGeneration_;
    page_.mode = core::CandidatePageMode::Clipboard;
    compositionInput_.clear();
    compositionCursor_ = 0;
    clipboardMode_ = true;
    clipboardPageAvailability_ = clipboardAvailability_;
    const auto count = clipboardEntries_ ? clipboardEntries_->size() : 0;
    page_.items.reserve(count);
    if (clipboardAvailability_ != core::ClipboardAvailability::Ready || count == 0) {
        core::CandidateItem emptyItem;
        emptyItem.text = clipboardAvailability_ == core::ClipboardAvailability::Loading
            ? "(正在读取剪贴板…)"
            : clipboardAvailability_ == core::ClipboardAvailability::Unavailable
                ? "(剪贴板暂不可用)" : "(剪贴板为空)";
        emptyItem.source = core::CandidateSource::Command;
        emptyItem.commitRole = core::CommitCandidateRole::Command;
        page_.items.push_back(std::move(emptyItem));
    } else {
        for (std::size_t index = 0; index < count; ++index) {
            core::CandidateItem item;
            item.text = (*clipboardEntries_)[index];
            item.consumedInputBytes = index;
            item.commitRole = core::CommitCandidateRole::Chinese;
            page_.items.push_back(std::move(item));
        }
    }
    for (std::size_t begin = 0; begin < page_.items.size();
         begin += kClipboardPageSize) {
        page_.pageBoundaries.push_back(
            {begin, std::min(begin + kClipboardPageSize, page_.items.size())});
    }
    host_.publishPage(page_);
    return true;
}

bool ModernIMEController::commitCurrent() {
    if (page_.items.empty() || page_.cursor >= page_.items.size()) {
        return false;
    }
    if (page_.items[page_.cursor].source == core::CandidateSource::None) {
        return false;
    }
    return select(page_.cursor);
}

} // namespace modernime::fcitx5
