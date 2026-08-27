#pragma once

#include "modernime/fcitx5/engine.h"

#ifdef MODERNIME_HAS_LIBIME_PINYIN
#include "modernime/pinyin/pinyin_candidate_provider.h"
#endif

#include <fcitx/candidatelist.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>

#include <memory>
#include <optional>

namespace fcitx {
class AddonManager;
}

namespace modernime::fcitx5 {

std::optional<KeyEvent> translateKey(const fcitx::Key &key);

class FcitxEngineHost final : public EngineHost {
public:
    explicit FcitxEngineHost(fcitx::InputContext &inputContext);

    void setController(ModernIMEController &controller) {
        controller_ = &controller;
    }

    void publishPage(const core::CandidatePage &page) override;
    void commit(std::string_view text) override;

private:
    fcitx::InputContext *inputContext_;
    ModernIMEController *controller_ = nullptr;
};

class FcitxInputContextState final : public fcitx::InputContextProperty {
public:
    explicit FcitxInputContextState(fcitx::InputContext &inputContext);

    ModernIMEController &controller() { return controller_; }

private:
    FcitxEngineHost host_;
#ifdef MODERNIME_HAS_LIBIME_PINYIN
    std::unique_ptr<pinyin::PinyinCandidateProvider> provider_;
#endif
    ModernIMEController controller_;
};

class ModernIMEInputMethod final : public fcitx::InputMethodEngine {
public:
    explicit ModernIMEInputMethod(fcitx::AddonManager *manager);
    ~ModernIMEInputMethod() override = default;

    std::vector<fcitx::InputMethodEntry> listInputMethods() override;
    void keyEvent(const fcitx::InputMethodEntry &entry,
                  fcitx::KeyEvent &event) override;
    void activate(const fcitx::InputMethodEntry &entry,
                  fcitx::InputContextEvent &event) override;
    void deactivate(const fcitx::InputMethodEntry &entry,
                    fcitx::InputContextEvent &event) override;
    void reset(const fcitx::InputMethodEntry &entry,
               fcitx::InputContextEvent &event) override;

private:
    FcitxInputContextState *state(fcitx::InputContext *inputContext) const;

    fcitx::FactoryFor<FcitxInputContextState> stateFactory_;
};

} // namespace modernime::fcitx5
