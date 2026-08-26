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
    candidates->setGlobalCursorIndex(static_cast<int>(page.cursor));
    for (std::size_t index = 0; index < page.items.size() && index < 9;
         ++index) {
        candidates->append<FcitxCandidateWord>(page.items[index].text,
                                               *controller_, index);
    }

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
    if (key.isLAZ()) {
        event.kind = KeyKind::Character;
        event.character = static_cast<char>(key.sym());
        return true;
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
