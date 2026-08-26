#include "modernime/fcitx5/engine.h"

#include <array>

namespace modernime::fcitx5 {
namespace {

const std::array<std::string_view, 9> sampleCandidates{
    "还", "海", "害", "嗨", "咳", "亥", "孩", "骇", "氦"};

} // namespace

ModernIMEController::ModernIMEController(EngineHost &host) : host_(host) {}

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
        if (event.character < 'a' || event.character > 'z' ||
            !input_.append(std::string_view(&event.character, 1))) {
            return false;
        }
        refreshPage();
        return true;
    case KeyKind::Backspace:
        if (!input_.eraseLast()) {
            return false;
        }
        refreshPage();
        return true;
    case KeyKind::Escape:
        reset();
        return true;
    case KeyKind::Enter:
        if (!page_.items.empty()) {
            return commitCurrent();
        }
        if (input_.text().empty()) {
            return false;
        }
        host_.commit(input_.text());
        reset();
        return true;
    case KeyKind::Space:
        return page_.items.empty() ? false : commitCurrent();
    case KeyKind::Digit: {
        if (event.digit < '1' || event.digit > '9') {
            return false;
        }
        const auto index = static_cast<std::size_t>(event.digit - '1');
        return select(index);
    }
    case KeyKind::Toggle:
        break;
    }
    return false;
}

bool ModernIMEController::select(std::size_t index) {
    if (!active_ || index >= page_.items.size()) {
        return false;
    }
    page_.cursor = index;
    return commitCurrent();
}

void ModernIMEController::reset() {
    input_.clear();
    page_.clear();
    host_.publishPage(page_);
}

void ModernIMEController::setActive(bool active) {
    active_ = active;
    if (!active_) {
        reset();
    }
}

void ModernIMEController::refreshPage() {
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
    host_.commit(page_.items[page_.cursor].text);
    reset();
    return true;
}

} // namespace modernime::fcitx5
