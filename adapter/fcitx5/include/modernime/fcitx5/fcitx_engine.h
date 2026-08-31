#pragma once

#include "modernime/core/persistent_clipboard_history.h"
#include "modernime/fcitx5/engine.h"

#ifdef MODERNIME_HAS_LIBIME_PINYIN
#include "modernime/pinyin/pinyin_candidate_provider.h"
#endif

#include <fcitx/candidatelist.h>
#include <fcitx-utils/event.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/surroundingtext.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace fcitx {
class AddonInstance;
class AddonManager;
class Instance;
}

namespace modernime::fcitx5 {

std::pair<std::string, std::string>
extractSurroundingContext(const fcitx::SurroundingText &text,
                          std::size_t maxChars);
std::optional<KeyEvent> translateKey(const fcitx::Key &key,
                                     const KeyBindings &bindings = {});

// 两段式剪贴板触发判定：拼音组合恰好是触发字母（默认 v）且当前按键是
// 触发数字（默认 2、无 Ctrl/Alt/Super 修饰）时返回 true。
bool clipboardTriggerFire(std::string_view preedit, const fcitx::Key &key,
                          std::string_view trigger);

// Heavyweight resources created once per input method engine and shared by
// every input context.
struct FcitxEngineResources final {
#ifdef MODERNIME_HAS_LIBIME_PINYIN
    std::shared_ptr<pinyin::PinyinCandidateProvider::SharedResources> pinyin;
#endif
};

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
    FcitxInputContextState(fcitx::InputContext &inputContext,
                           const core::ModernIMESettings &settings,
                           const FcitxEngineResources &resources,
                           std::uint64_t settingsGeneration);

    // Brings this context in sync with reloaded settings; cheap enough to
    // call whenever the engine notices a stale generation.
    void applySettings(const core::ModernIMESettings &settings,
                       std::uint64_t generation);
    std::uint64_t settingsGeneration() const { return settingsGeneration_; }

    ModernIMEController &controller() { return controller_; }
    void setClipboardEntries(std::vector<std::string> entries) {
        controller_.setClipboardEntries(std::move(entries));
    }

private:
    FcitxEngineHost host_;
#ifdef MODERNIME_HAS_LIBIME_PINYIN
    std::unique_ptr<pinyin::PinyinCandidateProvider> provider_;
#endif
    ModernIMEController controller_;
    std::uint64_t settingsGeneration_ = 0;
};

class ModernIMEInputMethod final : public fcitx::InputMethodEngine {
public:
    explicit ModernIMEInputMethod(fcitx::AddonManager *manager);
    ~ModernIMEInputMethod() override = default;

    std::vector<fcitx::InputMethodEntry> listInputMethods() override;
    void save() override;
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
    void pollClipboard();
    void pollFileChanges();

    fcitx::AddonManager *manager_ = nullptr;
    fcitx::Instance *instance_ = nullptr;
    fcitx::AddonInstance *clipboardAddon_ = nullptr;
    bool clipboardAddonLookupAttempted_ = false;
    core::PersistentClipboardHistory clipboardHistory_;
    std::string lastHistoryError_;
    std::unique_ptr<fcitx::EventSourceTime> clipboardTimer_;
    std::unique_ptr<fcitx::EventSourceTime> fileTimer_;
    std::filesystem::file_time_type settingsMtime_{};
    std::filesystem::file_time_type userDictionaryMtime_{};
    std::uint64_t settingsGeneration_ = 0;
    core::ModernIMESettings settings_;
    KeyBindings keyBindings_;
    FcitxEngineResources resources_;
    fcitx::FactoryFor<FcitxInputContextState> stateFactory_;
};

} // namespace modernime::fcitx5
