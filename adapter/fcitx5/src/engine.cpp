#include "modernime/fcitx5/engine.h"

#include <array>
#include <algorithm>
#include <cstddef>

namespace modernime::fcitx5 {
namespace {

const std::array<std::string_view, 9> sampleCandidates{
    "还", "海", "害", "嗨", "咳", "亥", "孩", "骇", "氦"};

} // namespace

ModernIMEController::ModernIMEController(EngineHost &host,
                                         core::CandidateProvider *provider)
    : host_(host), provider_(provider) {}

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
        if (event.character < 'a' || event.character > 'z') {
            return false;
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
        if (event.digit < '1' || event.digit > '9') {
            return false;
        }
        const auto index = static_cast<std::size_t>(event.digit - '1');
        const auto pageStart = currentPageIndex() * kCandidatePageSize;
        return select(pageStart + index);
    }
    case KeyKind::PreviousCandidate:
        return moveCursor(-1);
    case KeyKind::NextCandidate:
        return moveCursor(1);
    case KeyKind::PreviousPage:
        return movePage(-1);
    case KeyKind::NextPage:
        return movePage(1);
    case KeyKind::Toggle:
        break;
    }
    return false;
}

bool ModernIMEController::select(std::size_t index) {
    if (!active_ || index >= page_.items.size()) {
        return false;
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
    if (!active_ || page_.items.empty() || provider_ == nullptr) {
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
        (page_.items.size() - 1) / kCandidatePageSize);
    const auto next = std::clamp(current + delta, std::ptrdiff_t{0}, last);
    if (next == current) {
        return false;
    }
    page_.cursor = static_cast<std::size_t>(next) * kCandidatePageSize;
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

std::size_t ModernIMEController::currentPageIndex() const {
    return page_.items.empty() ? 0 : page_.cursor / kCandidatePageSize;
}

std::size_t ModernIMEController::pageSize() const {
    return kCandidatePageSize;
}

void ModernIMEController::refreshPage() {
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
