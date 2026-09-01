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
            std::string nextInput = provider_ ? provider_->page().preedit
                                               : input_.text();
            nextInput.push_back(event.character);
            if (!core::PinyinMatchPolicy::validComposition(nextInput)) {
                return false;
            }
        }
        if (provider_ &&
            !provider_->append(std::string_view(&event.character, 1))) {
            return false;
        }
        if (!provider_ &&
            !input_.append(std::string_view(&event.character, 1))) {
            return false;
        }
        refreshPage();
        return true;
    case KeyKind::Backspace:
        if (clipboardMode_) {
            clearComposition();
            return true;
        }
        if (provider_ ? !provider_->eraseLast() : !input_.eraseLast()) {
            return false;
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
        const auto pageStart = currentPageIndex() * pageSize();
        if (page_.items.empty()) {
            if (!page_.preedit.empty()) {
                commitRawPreedit();
            }
            return false;
        }
        const auto candidateIndex = pageStart + index;
        if (candidateIndex >= page_.items.size()) {
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
    case KeyKind::PreviousClipboardItem:
        if (!options_.pageNavigation) {
            return false;
        }
        return clipboardMode_ ? moveCursor(-1) : movePage(-1);
    case KeyKind::NextClipboardItem:
        if (!options_.pageNavigation) {
            return false;
        }
        return clipboardMode_ ? moveCursor(1) : movePage(1);
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
        const auto text = page_.items[index].text;
        if (!provider_->select(index)) {
            return false;
        }
        host_.commit(text);
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
    page_ = provider_->page();
    host_.publishPage(page_);
    return true;
}

bool ModernIMEController::moveCursor(std::ptrdiff_t delta) {
    if (page_.items.empty() || delta == 0) {
        return false;
    }

    const auto current = static_cast<std::ptrdiff_t>(page_.cursor);
    const auto last = static_cast<std::ptrdiff_t>(page_.items.size() - 1);
    const auto next = std::clamp(current + delta, std::ptrdiff_t{0}, last);
    if (next == current) {
        return true;
    }
    page_.cursor = static_cast<std::size_t>(next);
    host_.publishPage(page_);
    return true;
}

bool ModernIMEController::movePage(std::ptrdiff_t delta) {
    if (page_.items.empty() || delta == 0) {
        return false;
    }

    const auto current = static_cast<std::ptrdiff_t>(currentPageIndex());
    const auto last = static_cast<std::ptrdiff_t>(
        (page_.items.size() - 1) / pageSize());
    const auto next = std::clamp(current + delta, std::ptrdiff_t{0}, last);
    if (next == current) {
        return true;
    }
    page_.cursor = static_cast<std::size_t>(next) * pageSize();
    host_.publishPage(page_);
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
    const auto preedit = page_.preedit;
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
    if (provider_ != nullptr) {
        provider_->setContext(contextBefore_, contextAfter_);
    }
    if (provider_) {
        provider_->reset();
        page_ = provider_->page();
    } else {
        input_.clear();
        page_.clear();
    }
    host_.publishPage(page_);
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

std::size_t ModernIMEController::currentPageIndex() const {
    return page_.items.empty() ? 0 : page_.cursor / pageSize();
}

std::size_t ModernIMEController::pageSize() const {
    return clipboardMode_ ? kClipboardPageSize : kCandidatePageSize;
}

void ModernIMEController::refreshPage() {
    if (clipboardMode_) {
        return;
    }
    if (provider_) {
        page_ = provider_->page();
        host_.publishPage(page_);
        return;
    }
    page_.clear();
    page_.preedit = input_.text();
    page_.generation = input_.generation();
    if (input_.text() == "hail") {
        page_.items.reserve(sampleCandidates.size());
        for (std::size_t index = 0; index < sampleCandidates.size(); ++index) {
            page_.items.push_back(
                {std::string(sampleCandidates[index]), input_.text(), index});
        }
    } else if (!input_.text().empty()) {
        page_.items.push_back({input_.text(), input_.text(), 0});
    }
    host_.publishPage(page_);
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
    clipboardMode_ = true;
    page_.items.reserve(clipboardEntries_.size());
    for (std::size_t index = 0; index < clipboardEntries_.size(); ++index) {
        page_.items.push_back({clipboardEntries_[index], {}, index});
    }
    host_.publishPage(page_);
    return true;
}

bool ModernIMEController::commitCurrent() {
    if (page_.items.empty() || page_.cursor >= page_.items.size()) {
        return false;
    }
    const auto text = page_.items[page_.cursor].text;
    if (!clipboardMode_ && provider_ && !provider_->select(page_.cursor)) {
        return false;
    }
    host_.commit(text);
    clearComposition();
    return true;
}

} // namespace modernime::fcitx5
