#include "modernime/fcitx5/engine.h"

#include "modernime/core/pinyin_match.h"
#include "modernime/core/punctuation.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
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

ModernIMEController::ModernIMEController(EngineHost &host,
                                         core::CandidateProvider *provider,
                                         ControllerOptions options)
    : host_(host), provider_(provider), options_(options) {}

void ModernIMEController::setContext(std::string before, std::string after) {
    if (!before.empty() || !after.empty()) {
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
        contextBefore_ = fallbackContextBefore_;
        contextAfter_.clear();
    }
    if (provider_ != nullptr) {
        provider_->setContext(contextBefore_, contextAfter_);
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

    switch (event.kind) {
    case KeyKind::Character:
        if ((event.character < 'a' || event.character > 'z') &&
            event.character != '\'') {
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
    case KeyKind::Backspace:
        if (clipboardMode_) {
            clearComposition();
            return true;
        }
        if (compositionInput_.empty()) {
            return false;
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
            return false;
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
    case KeyKind::Punctuation:
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
        if (!page_.items.empty()) {
            if (!commitCurrent()) {
                return false;
            }
        } else if (!commitRawPreedit()) {
            return false;
        }
        host_.commit(std::string_view(&event.character, 1));
        return true;
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
        return openClipboard();
    case KeyKind::Toggle:
        break;
    }
    return false;
}

bool ModernIMEController::select(std::size_t index) {
    if (!active() || index >= page_.items.size()) {
        return false;
    }
    if (clipboardMode_) {
        if (page_.items[index].source == core::CandidateSource::Command) {
            clearComposition();
            return true;
        }
        const auto text = page_.items[index].text;
        host_.commit(text);
        clearComposition();
        return true;
    }
    if (provider_) {
        const auto candidate = page_.items[index];
        if (candidate.source == core::CandidateSource::Command) {
            provider_->select(index);
            return openClipboard();
        }
        const auto text = candidate.text;
        const bool partialSelection = candidate.consumedInputBytes > 0 &&
                                      candidate.consumedInputBytes <
                                          page_.rawInput.size();
        if (!provider_->select(index)) {
            return false;
        }
        host_.commit(text);
        appendFallbackContext(text);
        if (partialSelection && !provider_->page().rawInput.empty()) {
            publishProviderPage(true);
            return true;
        }
        clearComposition();
        return true;
    }
    if (page_.items[index].source == core::CandidateSource::Command) {
        clearComposition();
        return openClipboard();
    }
    page_.cursor = index;
    return commitCurrent();
}

bool ModernIMEController::removeCurrent() {
    if (!active() || clipboardMode_ || page_.items.empty() ||
        provider_ == nullptr) {
        return false;
    }
    if (!provider_->remove(page_.cursor)) {
        return false;
    }
    publishProviderPage();
    return true;
}

bool ModernIMEController::commitPunctuation(char ascii) {
    // Punctuation inside an ASCII run stays half-width so inputs such as
    // "3.14", "1,000" and English fragments survive. During composition the
    // pending candidate decides: committing Chinese goes with full-width,
    // committing a raw ASCII fragment goes half-width.
    bool afterAscii = endsWithAsciiAlnum(contextBefore_, contextAfter_);
    if (!page_.preedit.empty()) {
        std::string_view pending = page_.preedit;
        if (!page_.items.empty()) {
            const auto index = std::min(page_.cursor, page_.items.size() - 1);
            pending = page_.items[index].text;
        }
        afterAscii = afterAscii || isAsciiText(pending);
    }
    std::optional<std::string> converted;
    if (!afterAscii) {
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
    if (!page_.preedit.empty()) {
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

bool ModernIMEController::commitRawPreedit(std::string_view suffix) {
    if (page_.preedit.empty()) {
        return false;
    }
    const auto preedit = compositionInput_.empty() ? page_.preedit
                                                   : compositionInput_;
    if (provider_ != nullptr) {
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
    contextBefore_ = fallbackContextBefore_;
    contextAfter_.clear();
    clipboardMode_ = false;
    clipboardEntries_.clear();
    compositionInput_.clear();
    compositionCursor_ = 0;
    if (provider_ != nullptr) {
        provider_->setContext(contextBefore_, contextAfter_);
    }
    if (provider_) {
        provider_->reset();
        publishProviderPage();
    } else {
        input_.clear();
        page_.clear();
        host_.publishPage(page_);
    }
}

void ModernIMEController::reset() {
    fallbackContextBefore_.clear();
    if (provider_ != nullptr) {
        provider_->resetCommitHistory();
    }
    clearComposition();
    doubleQuoteOpen_ = false;
    singleQuoteOpen_ = false;
}

void ModernIMEController::setOptions(ControllerOptions options) {
    options_ = options;
    if (!options_.inputEnabled) {
        reset();
    }
}

void ModernIMEController::setActive(bool active) {
    active_ = active;
    if (!this->active()) {
        reset();
    }
}

void ModernIMEController::setClipboardEntries(
    std::vector<std::string> entries) {
    clipboardEntries_ = std::move(entries);
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

void ModernIMEController::publishProviderPage(bool adoptRemainingComposition) {
    page_ = provider_->page();
    if (adoptRemainingComposition && !page_.rawInput.empty()) {
        compositionInput_ = page_.rawInput;
        compositionCursor_ = compositionInput_.size();
    }
    updatePreeditCursor();
    ensurePageBoundaries();
    host_.publishPage(page_);
}

void ModernIMEController::refreshPage() {
    if (clipboardMode_) {
        return;
    }
    if (provider_) {
        publishProviderPage();
        return;
    }
    page_.clear();
    page_.preedit = input_.text();
    page_.generation = input_.generation();
    updatePreeditCursor();
    // 无 provider 的降级路径（正常安装不会出现）：唯一候选就是原始
    // 输入本身，保持行为可预期，不再注入开发期示例候选。
    if (!input_.text().empty()) {
        page_.items.push_back({input_.text(), input_.text(), 0});
    }
    ensurePageBoundaries();
    host_.publishPage(page_);
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
    if (!active()) {
        return false;
    }

    if (provider_ != nullptr) {
        provider_->reset();
    } else {
        input_.clear();
    }
    page_.clear();
    page_.mode = core::CandidatePageMode::Clipboard;
    compositionInput_.clear();
    compositionCursor_ = 0;
    clipboardMode_ = true;
    page_.items.reserve(clipboardEntries_.size());
    if (clipboardEntries_.empty()) {
        core::CandidateItem emptyItem;
        emptyItem.text = "(剪贴板为空)";
        emptyItem.source = core::CandidateSource::Command;
        page_.items.push_back(std::move(emptyItem));
    } else {
        for (std::size_t index = 0; index < clipboardEntries_.size(); ++index) {
            page_.items.push_back({clipboardEntries_[index], {}, index});
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
    if (clipboardMode_ || provider_ != nullptr) {
        return select(page_.cursor);
    }
    const auto text = page_.items[page_.cursor].text;
    host_.commit(text);
    clearComposition();
    return true;
}

} // namespace modernime::fcitx5
