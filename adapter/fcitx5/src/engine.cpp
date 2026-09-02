#include "modernime/fcitx5/engine.h"

#include "modernime/core/pinyin_match.h"
#include "modernime/core/punctuation.h"

#include <array>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace modernime::fcitx5 {
namespace {

const std::array<std::string_view, 9> sampleCandidates{
    "还", "海", "害", "嗨", "咳", "亥", "孩", "骇", "氦"};

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
    : host_(host), provider_(provider), options_(options),
      active_(options.inputEnabled) {}

void ModernIMEController::setContext(std::string before, std::string after) {
    contextBefore_ = std::move(before);
    contextAfter_ = std::move(after);
    // Empty/empty also means that the client does not expose surrounding
    // text (common in terminals and password fields). With no evidence about
    // the cursor location, keep the internal pair state instead of turning
    // every quote into an opening quote.
    if (!contextBefore_.empty() || !contextAfter_.empty()) {
        doubleQuoteOpen_ = quoteIsOpenAtCursor(
            contextBefore_, contextAfter_, core::kLeftDoubleQuote,
            core::kRightDoubleQuote);
        singleQuoteOpen_ = quoteIsOpenAtCursor(
            contextBefore_, contextAfter_, core::kLeftSingleQuote,
            core::kRightSingleQuote);
    }
    if (provider_ != nullptr) {
        provider_->setContext(contextBefore_, contextAfter_);
    }
}

bool ModernIMEController::handle(const KeyEvent &event) {
    if (event.kind == KeyKind::Toggle) {
        setActive(!active_);
        return true;
    }
    if (!active_) {
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
        clearComposition();
        return true;
    case KeyKind::Enter:
        if (!page_.items.empty()) {
            return commitCurrent();
        }
        if (page_.preedit.empty()) {
            return false;
        }
        return commitRawPreedit();
    case KeyKind::Space:
        return page_.items.empty() ? commitRawPreedit(" ") : commitCurrent();
    case KeyKind::Punctuation:
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
        if (!options_.numberSelection) {
            return false;
        }
        if (event.digit < '1' || event.digit > '9') {
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
        return moveCursor(-1);
    case KeyKind::NextClipboardItem:
        if (!options_.arrowNavigation) {
            return false;
        }
        return moveCursor(1);
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
    if (!active_ || index >= page_.items.size()) {
        return false;
    }
    if (clipboardMode_) {
        const auto text = page_.items[index].text;
        host_.commit(text);
        clearComposition();
        return true;
    }
    if (provider_) {
        const auto candidate = page_.items[index];
        const auto text = candidate.text;
        const bool partialSelection = candidate.consumedInputBytes > 0 &&
                                      candidate.consumedInputBytes <
                                          page_.rawInput.size();
        if (!provider_->select(index)) {
            return false;
        }
        host_.commit(text);
        if (partialSelection && !provider_->page().rawInput.empty()) {
            publishProviderPage(true);
            return true;
        }
        clearComposition();
        return true;
    }
    page_.cursor = index;
    return commitCurrent();
}

bool ModernIMEController::removeCurrent() {
    if (!active_ || clipboardMode_ || page_.items.empty() ||
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
    clearComposition();
    host_.commit(preedit);
    if (!suffix.empty()) {
        host_.commit(suffix);
    }
    return true;
}

void ModernIMEController::clearComposition() {
    contextBefore_.clear();
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
    clearComposition();
    doubleQuoteOpen_ = false;
    singleQuoteOpen_ = false;
}

void ModernIMEController::setActive(bool active) {
    active_ = active;
    if (!active_) {
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
    for (std::size_t begin = 0; begin < page_.items.size();
         begin += kFallbackCandidatePageSize) {
        page_.pageBoundaries.push_back(
            {begin, std::min(begin + kFallbackCandidatePageSize,
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
    if (input_.text() == "hail") {
        page_.items.reserve(sampleCandidates.size());
        for (std::size_t index = 0; index < sampleCandidates.size(); ++index) {
            page_.items.push_back(
                {std::string(sampleCandidates[index]), input_.text(), index});
        }
    } else if (!input_.text().empty()) {
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
    if (!active_) {
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
    for (std::size_t index = 0; index < clipboardEntries_.size(); ++index) {
        page_.items.push_back({clipboardEntries_[index], {}, index});
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
