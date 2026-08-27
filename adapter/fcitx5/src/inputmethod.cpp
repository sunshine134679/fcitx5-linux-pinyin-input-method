#include "modernime/fcitx5/fcitx_engine.h"

#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx-utils/keysymgen.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <utility>

namespace modernime::fcitx5 {
namespace {

class FcitxCandidateWord final : public fcitx::CandidateWord {
public:
    FcitxCandidateWord(std::string text, ModernIMEController &controller,
                       std::size_t index)
        : CandidateWord(fcitx::Text(std::move(text))), controller_(&controller),
          index_(index) {}

    void select(fcitx::InputContext *) const override {
        controller_->select(index_);
    }

private:
    ModernIMEController *controller_;
    std::size_t index_;
};

constexpr std::string_view statePropertyName = "modernime-fcitx5-state";

bool hasNonShiftModifier(const fcitx::Key &key) {
    return key.states().testAny(fcitx::KeyStates(
        {fcitx::KeyState::Ctrl, fcitx::KeyState::Alt, fcitx::KeyState::Super,
         fcitx::KeyState::Meta}));
}

bool isAsciiPunctuation(std::uint32_t unicode) {
    if (unicode > 0x7f || unicode < 0x21) {
        return false;
    }
    return std::ispunct(static_cast<unsigned char>(unicode)) != 0;
}

} // namespace

FcitxEngineHost::FcitxEngineHost(fcitx::InputContext &inputContext)
    : inputContext_(&inputContext) {}

void FcitxEngineHost::publishPage(const core::CandidatePage &page) {
    if (page.items.empty()) {
        inputContext_->inputPanel().reset();
        inputContext_->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        return;
    }

    auto candidates = std::make_unique<fcitx::CommonCandidateList>();
    candidates->setPageSize(9);
    candidates->setLayoutHint(fcitx::CandidateLayoutHint::Horizontal);
    candidates->setCursorIncludeUnselected(true);
    for (std::size_t index = 0; index < page.items.size(); ++index) {
        candidates->append<FcitxCandidateWord>(page.items[index].text,
                                               *controller_, index);
    }
    // Fcitx5 validates the global cursor against the populated list.
    const auto cursor = std::min<std::size_t>(page.cursor, candidates->size() - 1);
    candidates->setGlobalCursorIndex(static_cast<int>(cursor));
    candidates->setPage(static_cast<int>(cursor / 9));

    inputContext_->inputPanel().setPreedit(fcitx::Text(page.preedit));
    inputContext_->inputPanel().setCandidateList(std::move(candidates));
    inputContext_->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void FcitxEngineHost::commit(std::string_view text) {
    inputContext_->commitString(std::string(text));
}

FcitxInputContextState::FcitxInputContextState(fcitx::InputContext &inputContext)
    : host_(inputContext)
#ifdef MODERNIME_HAS_LIBIME_PINYIN
      , provider_(std::make_unique<pinyin::PinyinCandidateProvider>())
      , controller_(host_, provider_.get()) {
#else
      , controller_(host_) {
#endif
    host_.setController(controller_);
}

ModernIMEInputMethod::ModernIMEInputMethod(fcitx::AddonManager *manager)
    : stateFactory_([](fcitx::InputContext &inputContext) {
          return new FcitxInputContextState(inputContext);
      }) {
    if (manager != nullptr && manager->instance() != nullptr) {
        manager->instance()->inputContextManager().registerProperty(
            std::string(statePropertyName), &stateFactory_);
    }
}

std::vector<fcitx::InputMethodEntry> ModernIMEInputMethod::listInputMethods() {
    std::vector<fcitx::InputMethodEntry> entries;
    entries.emplace_back("modernime", "ModernIME", "zh_CN", "modernime");
    entries.back()
        .setNativeName("ModernIME 拼音")
        .setIcon("fcitx-pinyin")
        .setLabel("拼")
        .setConfigurable(false);
    return entries;
}

FcitxInputContextState *
ModernIMEInputMethod::state(fcitx::InputContext *inputContext) const {
    if (inputContext == nullptr) {
        return nullptr;
    }
    return inputContext->propertyFor(&stateFactory_);
}

bool ModernIMEInputMethod::translateKey(const fcitx::Key &key,
                                        KeyEvent &event) const {
    if (key.check(FcitxKey_space, fcitx::KeyStates(fcitx::KeyState::Ctrl))) {
        event.kind = KeyKind::Toggle;
        return true;
    }
    if (key.check(FcitxKey_BackSpace)) {
        event.kind = KeyKind::Backspace;
        return true;
    }
    if (key.check(FcitxKey_Escape)) {
        event.kind = KeyKind::Escape;
        return true;
    }
    if (key.check(FcitxKey_Return)) {
        event.kind = KeyKind::Enter;
        return true;
    }
    if (key.check(FcitxKey_Page_Up) || key.check(FcitxKey_Up)) {
        event.kind = KeyKind::PreviousPage;
        return true;
    }
    if (key.check(FcitxKey_Page_Down) || key.check(FcitxKey_Down)) {
        event.kind = KeyKind::NextPage;
        return true;
    }
    if (key.check(FcitxKey_Left)) {
        event.kind = KeyKind::PreviousCandidate;
        return true;
    }
    if (key.check(FcitxKey_Right)) {
        event.kind = KeyKind::NextCandidate;
        return true;
    }
    if (key.check(FcitxKey_Tab,
                  fcitx::KeyStates(fcitx::KeyState::Shift))) {
        event.kind = KeyKind::PreviousCandidate;
        return true;
    }
    if (key.check(FcitxKey_Tab)) {
        event.kind = KeyKind::NextCandidate;
        return true;
    }
    if (key.check(FcitxKey_space)) {
        event.kind = KeyKind::Space;
        return true;
    }
    const int selection = key.digitSelection();
    if (selection >= 0 && selection < 9) {
        event.kind = KeyKind::Digit;
        event.digit = static_cast<char>('1' + selection);
        return true;
    }
    if (!hasNonShiftModifier(key)) {
        const auto unicode = fcitx::Key::keySymToUnicode(key.sym());
        if (unicode >= 'A' && unicode <= 'Z') {
            event.kind = KeyKind::Character;
            event.character = static_cast<char>(unicode - 'A' + 'a');
            return true;
        }
        if (unicode >= 'a' && unicode <= 'z') {
            event.kind = KeyKind::Character;
            event.character = static_cast<char>(unicode);
            return true;
        }
        if (unicode == '\'') {
            event.kind = KeyKind::Character;
            event.character = static_cast<char>(unicode);
            return true;
        }
        if (isAsciiPunctuation(unicode)) {
            event.kind = KeyKind::Punctuation;
            event.character = static_cast<char>(unicode);
            return true;
        }
    }
    return false;
}

void ModernIMEInputMethod::keyEvent(const fcitx::InputMethodEntry &,
                                    fcitx::KeyEvent &event) {
    if (event.isRelease()) {
        return;
    }
    auto *contextState = state(event.inputContext());
    if (contextState == nullptr) {
        return;
    }
    KeyEvent modernEvent;
    if (translateKey(event.key(), modernEvent) &&
        contextState->controller().handle(modernEvent)) {
        event.filterAndAccept();
    }
}

void ModernIMEInputMethod::activate(const fcitx::InputMethodEntry &,
                                    fcitx::InputContextEvent &event) {
    if (auto *contextState = state(event.inputContext()); contextState != nullptr) {
        contextState->controller().setActive(true);
    }
}

void ModernIMEInputMethod::deactivate(const fcitx::InputMethodEntry &entry,
                                      fcitx::InputContextEvent &event) {
    reset(entry, event);
    if (auto *contextState = state(event.inputContext()); contextState != nullptr) {
        contextState->controller().setActive(false);
    }
}

void ModernIMEInputMethod::reset(const fcitx::InputMethodEntry &,
                                 fcitx::InputContextEvent &event) {
    if (auto *contextState = state(event.inputContext()); contextState != nullptr) {
        contextState->controller().reset();
    }
}

} // namespace modernime::fcitx5
