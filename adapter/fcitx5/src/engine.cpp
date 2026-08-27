#include "modernime/fcitx5/engine.h"

#include "modernime/core/pinyin_match.h"

#include <array>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <utility>

namespace modernime::fcitx5 {
namespace {

const std::array<std::string_view, 9> sampleCandidates{
    "还", "海", "害", "嗨", "咳", "亥", "孩", "骇", "氦"};

} // namespace

ClipboardTrigger::ClipboardTrigger(std::string_view trigger) {
    if (trigger.size() != 3 || trigger[1] != '+') {
        return;
    }
    const auto first = static_cast<unsigned char>(trigger[0]);
    const auto second = static_cast<unsigned char>(trigger[2]);
    const bool letter = (first >= 'A' && first <= 'Z') ||
                        (first >= 'a' && first <= 'z');
    if (!letter || second < '1' || second > '9') {
        return;
    }
    first_ = static_cast<char>(std::tolower(first));
    second_ = static_cast<char>(second);
}

bool ClipboardTrigger::isFirst(const KeyEvent &event) const {
    if (event.kind != KeyKind::Character) {
        return false;
    }
    return static_cast<char>(std::tolower(
               static_cast<unsigned char>(event.character))) == first_;
}

bool ClipboardTrigger::isSecond(const KeyEvent &event) const {
    return event.kind == KeyKind::Digit && event.digit == second_;
}

ClipboardTriggerResult ClipboardTrigger::feed(const KeyEvent &event,
                                              std::uint64_t nowMs,
                                              bool eligible) {
    ClipboardTriggerResult result;
    if (!valid()) {
        return result;
    }

    if (pending_ && nowMs >= pendingSinceMs_ &&
        nowMs - pendingSinceMs_ >= kTimeoutMs) {
        result.replay = replayEvent();
        pending_ = false;
    }

    if (pending_) {
        if (isSecond(event)) {
            pending_ = false;
            result.consumed = true;
            result.openClipboard = true;
            return result;
        }
        result.replay = replayEvent();
        pending_ = false;
    }

    if (eligible && isFirst(event)) {
        pending_ = true;
        pendingSinceMs_ = nowMs;
        result.consumed = true;
    }
    return result;
}

ClipboardTriggerResult ClipboardTrigger::expire(std::uint64_t nowMs) {
    ClipboardTriggerResult result;
    if (!valid() || !pending_ || nowMs < pendingSinceMs_ ||
        nowMs - pendingSinceMs_ < kTimeoutMs) {
        return result;
    }
    pending_ = false;
    result.consumed = true;
    result.replay = replayEvent();
    return result;
}

void ClipboardTrigger::reset() {
    pending_ = false;
    pendingSinceMs_ = 0;
}

ModernIMEController::ModernIMEController(EngineHost &host,
                                         core::CandidateProvider *provider,
                                         ControllerOptions options)
    : host_(host), provider_(provider), options_(options),
      active_(options.inputEnabled) {}

void ModernIMEController::setContext(std::string before, std::string after) {
    contextBefore_ = std::move(before);
    contextAfter_ = std::move(after);
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
        if (provider_ ? !provider_->eraseLast() : !input_.eraseLast()) {
            return false;
        }
        refreshPage();
        return true;
    case KeyKind::DeleteCandidate:
        return removeCurrent();
    case KeyKind::Escape:
        reset();
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
        return select(pageStart + index);
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
        reset();
        return true;
    }
    if (provider_) {
        const auto text = page_.items[index].text;
        if (!provider_->select(index)) {
            return false;
        }
        host_.commit(text);
        reset();
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
        return false;
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
        return false;
    }
    page_.cursor = static_cast<std::size_t>(next) * pageSize();
    host_.publishPage(page_);
    return true;
}

bool ModernIMEController::commitRawPreedit(std::string_view suffix) {
    if (page_.preedit.empty()) {
        return false;
    }
    const auto preedit = page_.preedit;
    reset();
    host_.commit(preedit);
    if (!suffix.empty()) {
        host_.commit(suffix);
    }
    return true;
}

void ModernIMEController::reset() {
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
    if (provider_ && !provider_->select(page_.cursor)) {
        return false;
    }
    host_.commit(text);
    reset();
    return true;
}

} // namespace modernime::fcitx5
