#include "modernime/fcitx5/fcitx_engine.h"

#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/surroundingtext.h>
#include <fcitx-utils/keysymgen.h>
#include <fcitx-utils/utf8.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace modernime::fcitx5 {
namespace {

class FcitxCandidateWord final : public fcitx::CandidateWord {
public:
    FcitxCandidateWord(std::string text, ModernIMEController *controller,
                       std::size_t index,
                       std::function<void()> beforeSelection)
        : CandidateWord(fcitx::Text(std::move(text))), controller_(controller),
          index_(index), beforeSelection_(std::move(beforeSelection)) {}

    void select(fcitx::InputContext *) const override {
        if (beforeSelection_) {
            beforeSelection_();
        }
        if (controller_ != nullptr) {
            controller_->select(index_);
        }
    }

private:
    ModernIMEController *controller_;
    std::size_t index_;
    std::function<void()> beforeSelection_;
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

bool matchesToggleKey(const fcitx::Key &key, std::string_view binding) {
    if (binding == "Ctrl+Space") {
        return key.check(FcitxKey_space,
                         fcitx::KeyStates(fcitx::KeyState::Ctrl));
    }
    if (binding == "Alt+Space") {
        return key.check(FcitxKey_space,
                         fcitx::KeyStates(fcitx::KeyState::Alt));
    }
    if (binding == "Super+Space") {
        return key.check(FcitxKey_space,
                         fcitx::KeyStates(fcitx::KeyState::Super));
    }
    if (binding == "Ctrl+Shift+Space") {
        return key.check(
            FcitxKey_space,
            fcitx::KeyStates({fcitx::KeyState::Ctrl, fcitx::KeyState::Shift}));
    }
    return false;
}

core::ModernIMESettings loadSettings() {
    const auto *xdgConfigHome = std::getenv("XDG_CONFIG_HOME");
    const auto *xdgDataHome = std::getenv("XDG_DATA_HOME");
    const auto *home = std::getenv("HOME");
    const auto paths = core::SettingsPaths::fromEnvironment(
        xdgConfigHome == nullptr ? std::string_view{}
                                 : std::string_view(xdgConfigHome),
        xdgDataHome == nullptr ? std::string_view{}
                               : std::string_view(xdgDataHome),
        home == nullptr ? std::string_view{} : std::string_view(home));
    return core::SettingsStore::load(paths.settingsFile).settings;
}

ControllerOptions controllerOptions(const core::ModernIMESettings &settings) {
    return {settings.inputEnabled, settings.numberSelection,
            settings.arrowNavigation, settings.pageNavigation};
}

KeyBindings keyBindings(const core::ModernIMESettings &settings) {
    return {settings.toggleKey, settings.numberSelection,
            settings.arrowNavigation, settings.pageNavigation,
            settings.clipboardEnabled, settings.clipboardTrigger};
}

std::optional<char> clipboardTriggerDigit(const fcitx::Key &key,
                                          std::string_view trigger) {
    if (trigger.size() != 3 || trigger[1] != '+' ||
        hasNonShiftModifier(key)) {
        return std::nullopt;
    }
    const auto second = static_cast<unsigned char>(trigger[2]);
    if (second < '1' || second > '9' ||
        key.digitSelection() != static_cast<int>(second - '1')) {
        return std::nullopt;
    }
    return static_cast<char>(second);
}

#ifdef MODERNIME_HAS_LIBIME_PINYIN
pinyin::PinyinDataPaths pinyinPaths() {
    const auto *xdgConfigHome = std::getenv("XDG_CONFIG_HOME");
    const auto *xdgDataHome = std::getenv("XDG_DATA_HOME");
    const auto *home = std::getenv("HOME");
    const auto paths = core::SettingsPaths::fromEnvironment(
        xdgConfigHome == nullptr ? std::string_view{}
                                 : std::string_view(xdgConfigHome),
        xdgDataHome == nullptr ? std::string_view{}
                               : std::string_view(xdgDataHome),
        home == nullptr ? std::string_view{} : std::string_view(home));
    pinyin::PinyinDataPaths result;
    result.userDictionary = paths.userDictionary.string();
    result.learningStore = paths.learningStore.string();
    return result;
}

pinyin::PinyinProviderOptions pinyinOptions(
    const core::ModernIMESettings &settings) {
    return {settings.learningEnabled, settings.contextLearningEnabled};
}
#endif

} // namespace

std::pair<std::string, std::string>
extractSurroundingContext(const fcitx::SurroundingText &text,
                          std::size_t maxChars) {
    if (!text.isValid() || maxChars == 0 ||
        !fcitx::utf8::validate(text.text())) {
        return {};
    }

    const auto &value = text.text();
    const auto length = fcitx::utf8::length(value);
    const auto cursor = std::min<std::size_t>(text.cursor(), length);
    const auto beforeCount = std::min(maxChars, cursor);
    const auto afterCount = std::min(maxChars, length - cursor);
    const auto advance = [](auto iterator, std::size_t count) {
        return count == 0 ? iterator : fcitx::utf8::nextNChar(iterator, count);
    };
    const auto begin = advance(value.cbegin(), cursor - beforeCount);
    const auto cursorIterator = advance(value.cbegin(), cursor);
    const auto end = advance(cursorIterator, afterCount);
    return {std::string(begin, cursorIterator),
            std::string(cursorIterator, end)};
}

FcitxEngineHost::FcitxEngineHost(fcitx::InputContext &inputContext)
    : inputContext_(&inputContext) {}

void FcitxEngineHost::publishPage(const core::CandidatePage &page) {
    fcitx::Text preedit(page.preedit);
    preedit.setCursor(static_cast<int>(preedit.textLength()));
    if (page.items.empty()) {
        inputContext_->inputPanel().reset();
        inputContext_->inputPanel().setPreedit(preedit);
        inputContext_->inputPanel().setClientPreedit(preedit);
        inputContext_->updatePreedit();
        inputContext_->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        return;
    }

    auto candidates = std::make_unique<fcitx::CommonCandidateList>();
    const bool clipboard = page.mode == core::CandidatePageMode::Clipboard;
    const auto pageSize = clipboard ? kClipboardPageSize : kCandidatePageSize;
    candidates->setPageSize(static_cast<int>(pageSize));
    candidates->setLayoutHint(clipboard ? fcitx::CandidateLayoutHint::Vertical
                                        : fcitx::CandidateLayoutHint::Horizontal);
    candidates->setCursorIncludeUnselected(true);
    for (std::size_t index = 0; index < page.items.size(); ++index) {
        candidates->append<FcitxCandidateWord>(page.items[index].text,
                                               controller_, index,
                                               beforeCandidateSelection_);
    }
    if (page.mode == core::CandidatePageMode::FunctionMenu) {
        std::vector<std::string> labels;
        labels.reserve(page.items.size());
        for (const auto &item : page.items) {
            labels.push_back(std::to_string(item.sourceIndex + 1));
        }
        candidates->setLabels(labels);
    }
    // Fcitx5's size() is page-local, so clamp against the full published list
    // before selecting the requested page.
    const auto cursor = std::min<std::size_t>(page.cursor, page.items.size() - 1);
    candidates->setGlobalCursorIndex(static_cast<int>(cursor));
    candidates->setPage(static_cast<int>(cursor / pageSize));

    inputContext_->inputPanel().setPreedit(preedit);
    inputContext_->inputPanel().setClientPreedit(preedit);
    inputContext_->inputPanel().setCandidateList(std::move(candidates));
    inputContext_->updatePreedit();
    inputContext_->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void FcitxEngineHost::commit(std::string_view text) {
    inputContext_->commitString(std::string(text));
}

FcitxInputContextState::FcitxInputContextState(
    fcitx::InputContext &inputContext,
    const core::ModernIMESettings &settings)
    : host_(inputContext)
#ifdef MODERNIME_HAS_LIBIME_PINYIN
      , provider_(std::make_unique<pinyin::PinyinCandidateProvider>(
            pinyinPaths(), pinyinOptions(settings)))
      , clipboardTrigger_(settings.clipboardTrigger)
      , controller_(host_, provider_.get(), controllerOptions(settings)) {
#else
      , clipboardTrigger_(settings.clipboardTrigger)
      , controller_(host_, nullptr, controllerOptions(settings)) {
#endif
    host_.setController(controller_);
    host_.setBeforeCandidateSelection(
        [this] { clipboardTrigger_.reset(); });
    controller_.setActive(settings.inputEnabled &&
                           settings.defaultMode == core::InputMode::Chinese);
}

ModernIMEInputMethod::ModernIMEInputMethod(fcitx::AddonManager *manager)
    : manager_(manager),
      instance_(manager == nullptr ? nullptr : manager->instance()),
      settings_(loadSettings()), keyBindings_(keyBindings(settings_)),
      stateFactory_([this](fcitx::InputContext &inputContext) {
          return new FcitxInputContextState(inputContext, settings_);
      }) {
    if (instance_ != nullptr) {
        instance_->inputContextManager().registerProperty(
            std::string(statePropertyName), &stateFactory_);
        clipboardTimer_ = instance_->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 50000, 50000,
            [this](fcitx::EventSourceTime *source, std::uint64_t) {
                pollClipboard();
                source->setNextInterval(50000);
                source->setEnabled(true);
                return true;
            });
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

std::optional<KeyEvent> translateKey(const fcitx::Key &key,
                                     const KeyBindings &bindings) {
    KeyEvent event;
    if (matchesToggleKey(key, bindings.toggleKey)) {
        event.kind = KeyKind::Toggle;
        return event;
    }
    if (key.check(FcitxKey_Delete,
                  fcitx::KeyStates(fcitx::KeyState::Ctrl)) ||
        key.check(FcitxKey_Delete,
                  fcitx::KeyStates(fcitx::KeyState::Shift))) {
        event.kind = KeyKind::DeleteCandidate;
        return event;
    }
    if (key.check(FcitxKey_Delete)) {
        event.kind = KeyKind::CloseClipboard;
        return event;
    }
    if (key.check(FcitxKey_BackSpace)) {
        event.kind = KeyKind::Backspace;
        return event;
    }
    if (key.check(FcitxKey_Escape)) {
        event.kind = KeyKind::Escape;
        return event;
    }
    if (key.check(FcitxKey_Return) || key.check(FcitxKey_KP_Enter)) {
        event.kind = KeyKind::Enter;
        return event;
    }
    if (bindings.pageNavigation && key.check(FcitxKey_Page_Up)) {
        event.kind = KeyKind::PreviousPage;
        return event;
    }
    if (bindings.pageNavigation && key.check(FcitxKey_Page_Down)) {
        event.kind = KeyKind::NextPage;
        return event;
    }
    if (bindings.pageNavigation && key.check(FcitxKey_Up)) {
        event.kind = KeyKind::PreviousClipboardItem;
        return event;
    }
    if (bindings.pageNavigation && key.check(FcitxKey_Down)) {
        event.kind = KeyKind::NextClipboardItem;
        return event;
    }
    if (bindings.pageNavigation &&
        (key.check(FcitxKey_equal) || key.check(FcitxKey_plus) ||
         key.check(FcitxKey_KP_Add))) {
        event.kind = KeyKind::NextPage;
        return event;
    }
    if (bindings.arrowNavigation && key.check(FcitxKey_Left)) {
        event.kind = KeyKind::PreviousCandidate;
        return event;
    }
    if (bindings.arrowNavigation && key.check(FcitxKey_Right)) {
        event.kind = KeyKind::NextCandidate;
        return event;
    }
    if (bindings.arrowNavigation &&
        key.check(FcitxKey_Tab,
                  fcitx::KeyStates(fcitx::KeyState::Shift))) {
        event.kind = KeyKind::PreviousCandidate;
        return event;
    }
    if (bindings.arrowNavigation && key.check(FcitxKey_Tab)) {
        event.kind = KeyKind::NextCandidate;
        return event;
    }
    if (key.check(FcitxKey_space)) {
        event.kind = KeyKind::Space;
        return event;
    }
    const int selection = key.digitSelection();
    if (bindings.numberSelection && selection >= 0 && selection < 9) {
        event.kind = KeyKind::Digit;
        event.digit = static_cast<char>('1' + selection);
        return event;
    }
    if (!hasNonShiftModifier(key)) {
        const auto unicode = fcitx::Key::keySymToUnicode(key.sym());
        if (unicode >= 'A' && unicode <= 'Z') {
            event.kind = KeyKind::Character;
            event.character = static_cast<char>(unicode - 'A' + 'a');
            return event;
        }
        if (unicode >= 'a' && unicode <= 'z') {
            event.kind = KeyKind::Character;
            event.character = static_cast<char>(unicode);
            return event;
        }
        if (unicode == '\'') {
            event.kind = KeyKind::Character;
            event.character = static_cast<char>(unicode);
            return event;
        }
        if (isAsciiPunctuation(unicode)) {
            event.kind = KeyKind::Punctuation;
            event.character = static_cast<char>(unicode);
            return event;
        }
    }
    return std::nullopt;
}

void ModernIMEInputMethod::pollClipboard() {
    if (!keyBindings_.clipboardEnabled || manager_ == nullptr ||
        instance_ == nullptr) {
        return;
    }
    if (!clipboardAddonLookupAttempted_) {
        clipboardAddon_ = manager_->addon("clipboard", true);
        clipboardAddonLookupAttempted_ = true;
    }
    if (clipboardAddon_ == nullptr) {
        return;
    }
    auto *inputContext =
        instance_->inputContextManager().mostRecentInputContext();
    if (inputContext == nullptr) {
        return;
    }
    try {
        const auto text = clipboardAddon_->callWithSignature<
            std::string(const fcitx::InputContext *)>(
            "Clipboard::clipboard", inputContext);
        clipboardHistory_.observe(text);
    } catch (const std::exception &) {
        // A third-party or older clipboard addon may not expose this optional
        // function. Clipboard mode remains harmlessly empty in that case.
    }
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
    const auto context = extractSurroundingContext(
        event.inputContext()->surroundingText(), 32);
    contextState->controller().setContext(context.first, context.second);
    auto modernEvent = translateKey(event.key(), keyBindings_);
    const bool clipboardActive = keyBindings_.clipboardEnabled &&
                                  contextState->controller().active();
    if (!modernEvent.has_value() && clipboardActive) {
        if (const auto digit =
                clipboardTriggerDigit(event.key(), keyBindings_.clipboardTrigger);
            digit.has_value()) {
            modernEvent = KeyEvent{KeyKind::Digit, 0, *digit};
        }
    }
    if (!modernEvent.has_value()) {
        return;
    }

    if (clipboardActive) {
        const auto trigger = contextState->processClipboardTrigger(
            *modernEvent, contextState->controller().page().preedit.empty());
        if (trigger.replay.has_value()) {
            contextState->controller().handle(*trigger.replay);
        }
        if (trigger.openFeatureMenu) {
            contextState->setClipboardEntries(clipboardHistory_.entries());
            if (contextState->controller().handle(
                    {KeyKind::OpenFeatureMenu, trigger.featurePrefix,
                     trigger.featureDigit})) {
                event.filterAndAccept();
            }
            return;
        }
        if (trigger.openClipboard) {
            contextState->setClipboardEntries(clipboardHistory_.entries());
            if (contextState->controller().handle(
                    {KeyKind::OpenClipboard, 0, 0})) {
                event.filterAndAccept();
            }
            return;
        }
        if (trigger.consumed) {
            event.filterAndAccept();
            return;
        }
    }

    if (contextState->controller().handle(*modernEvent)) {
        event.filterAndAccept();
    }
}

void ModernIMEInputMethod::activate(const fcitx::InputMethodEntry &,
                                    fcitx::InputContextEvent &event) {
    if (auto *contextState = state(event.inputContext()); contextState != nullptr) {
        contextState->resetClipboardTrigger();
        contextState->controller().setActive(
            settings_.inputEnabled &&
            settings_.defaultMode == core::InputMode::Chinese);
    }
}

void ModernIMEInputMethod::deactivate(const fcitx::InputMethodEntry &entry,
                                      fcitx::InputContextEvent &event) {
    reset(entry, event);
    if (auto *contextState = state(event.inputContext()); contextState != nullptr) {
        contextState->resetClipboardTrigger();
        contextState->controller().setActive(false);
    }
}

void ModernIMEInputMethod::reset(const fcitx::InputMethodEntry &,
                                 fcitx::InputContextEvent &event) {
    if (auto *contextState = state(event.inputContext()); contextState != nullptr) {
        contextState->resetClipboardTrigger();
        contextState->controller().reset();
    }
}

} // namespace modernime::fcitx5
