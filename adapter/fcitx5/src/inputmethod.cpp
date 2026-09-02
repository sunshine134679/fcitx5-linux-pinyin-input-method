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
#include <fcitx-utils/log.h>
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

core::SettingsPaths settingsPaths() {
    const auto *xdgConfigHome = std::getenv("XDG_CONFIG_HOME");
    const auto *xdgDataHome = std::getenv("XDG_DATA_HOME");
    const auto *home = std::getenv("HOME");
    return core::SettingsPaths::fromEnvironment(
        xdgConfigHome == nullptr ? std::string_view{}
                                 : std::string_view(xdgConfigHome),
        xdgDataHome == nullptr ? std::string_view{}
                               : std::string_view(xdgDataHome),
        home == nullptr ? std::string_view{} : std::string_view(home));
}

core::ModernIMESettings loadSettings() {
    return core::SettingsStore::load(settingsPaths().settingsFile).settings;
}

ControllerOptions controllerOptions(const core::ModernIMESettings &settings) {
    return {settings.inputEnabled, settings.numberSelection,
            settings.arrowNavigation, settings.pageNavigation,
            settings.punctuationEnabled};
}

KeyBindings keyBindings(const core::ModernIMESettings &settings) {
    return {settings.toggleKey, settings.numberSelection,
            settings.arrowNavigation, settings.pageNavigation,
            settings.clipboardEnabled, settings.clipboardTrigger};
}

} // namespace

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

bool clipboardTriggerFire(std::string_view preedit, const fcitx::Key &key,
                          std::string_view trigger) {
    if (trigger.size() != 3 || trigger[1] != '+' || preedit.size() != 1) {
        return false;
    }
    const auto first = static_cast<unsigned char>(trigger[0]);
    const bool letter = (first >= 'A' && first <= 'Z') ||
                        (first >= 'a' && first <= 'z');
    if (!letter) {
        return false;
    }
    const auto expected = static_cast<char>(std::tolower(first));
    if (std::tolower(static_cast<unsigned char>(preedit.front())) !=
        expected) {
        return false;
    }
    return clipboardTriggerDigit(key, trigger).has_value();
}

#ifdef MODERNIME_HAS_LIBIME_PINYIN
pinyin::PinyinDataPaths pinyinPaths() {
    const auto paths = settingsPaths();
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
    const auto requestedCursor =
        page.preeditCursor == core::CandidatePage::kCursorAtEnd
            ? preedit.textLength()
            : std::min(page.preeditCursor, preedit.textLength());
    preedit.setCursor(static_cast<int>(requestedCursor));
    if (page.items.empty()) {
        inputContext_->inputPanel().reset();
        inputContext_->inputPanel().setPreedit(preedit);
        inputContext_->inputPanel().setClientPreedit(preedit);
        inputContext_->updatePreedit();
        inputContext_->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        return;
    }

    auto candidates = std::make_unique<FcitxCandidateList>(controller_);
    const bool clipboard = page.mode == core::CandidatePageMode::Clipboard;
    candidates->setLayoutHint(clipboard ? fcitx::CandidateLayoutHint::Vertical
                                        : fcitx::CandidateLayoutHint::Horizontal);
    candidates->setCursorIncludeUnselected(true);
    for (std::size_t index = 0; index < page.items.size(); ++index) {
        candidates->append<FcitxCandidateWord>(page.items[index].text,
                                               controller_, index);
    }
    // Fcitx5's size() is page-local, so clamp against the full published list
    // before selecting the requested page.
    const auto cursor = std::min<std::size_t>(page.cursor, page.items.size() - 1);
    candidates->setGlobalCursorIndex(static_cast<int>(cursor));
    auto boundaries = page.pageBoundaries;
    if (clipboard && boundaries.empty()) {
        for (std::size_t begin = 0; begin < page.items.size();
             begin += kClipboardPageSize) {
            boundaries.push_back(
                {begin,
                 std::min(begin + kClipboardPageSize, page.items.size())});
        }
    }
    candidates->setPageBoundaries(std::move(boundaries), false);

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
    fcitx::InputContext &inputContext, const core::ModernIMESettings &settings,
    const FcitxEngineResources &resources, std::uint64_t settingsGeneration)
    : host_(inputContext)
#ifdef MODERNIME_HAS_LIBIME_PINYIN
      ,
      provider_(resources.pinyin != nullptr
                    ? std::make_unique<pinyin::PinyinCandidateProvider>(
                          resources.pinyin, pinyinOptions(settings))
                    : std::make_unique<pinyin::PinyinCandidateProvider>(
                          pinyinPaths(), pinyinOptions(settings)))
      , controller_(host_, provider_.get(), controllerOptions(settings))
      , settingsGeneration_(settingsGeneration) {
#else
      , controller_(host_, nullptr, controllerOptions(settings))
      , settingsGeneration_(settingsGeneration) {
        (void)resources;
#endif
    host_.setController(controller_);
    controller_.setActive(settings.inputEnabled &&
                          settings.defaultMode == core::InputMode::Chinese);
}

void FcitxInputContextState::applySettings(
    const core::ModernIMESettings &settings, std::uint64_t generation) {
    controller().setOptions(controllerOptions(settings));
#ifdef MODERNIME_HAS_LIBIME_PINYIN
    if (provider_ != nullptr) {
        provider_->setLearningEnabled(settings.learningEnabled);
        provider_->setContextLearningEnabled(
            settings.contextLearningEnabled);
    }
#else
    (void)settings;
#endif
    settingsGeneration_ = generation;
}

ModernIMEInputMethod::ModernIMEInputMethod(fcitx::AddonManager *manager)
    : manager_(manager),
      instance_(manager == nullptr ? nullptr : manager->instance()),
      clipboardHistory_(settingsPaths().clipboardHistory),
      settings_(loadSettings()), keyBindings_(keyBindings(settings_)),
      stateFactory_([this](fcitx::InputContext &inputContext) {
          return new FcitxInputContextState(inputContext, settings_,
                                            resources_, settingsGeneration_);
      }) {
#ifdef MODERNIME_HAS_LIBIME_PINYIN
    // One dictionary/language-model/learning-writer set for the whole engine;
    // every input context only adds its own composition state.
    resources_.pinyin = pinyin::PinyinCandidateProvider::createSharedResources(
        pinyinPaths(), pinyinOptions(settings_));
#endif
    {
        std::error_code mtimeError;
        settingsMtime_ = std::filesystem::last_write_time(
            settingsPaths().settingsFile, mtimeError);
        if (mtimeError) {
            settingsMtime_ = {};
        }
        userDictionaryMtime_ = std::filesystem::last_write_time(
            settingsPaths().userDictionary, mtimeError);
        if (mtimeError) {
            userDictionaryMtime_ = {};
        }
    }
    std::string historyError;
    if (!clipboardHistory_.load(&historyError)) {
        FCITX_ERROR() << "Failed to load ModernIME clipboard history: "
                      << historyError;
    }
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
        fileTimer_ = instance_->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 2000000, 2000000,
            [this](fcitx::EventSourceTime *source, std::uint64_t) {
                pollFileChanges();
                source->setNextInterval(2000000);
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

void ModernIMEInputMethod::save() {
    std::string historyError;
    if (!clipboardHistory_.flush(&historyError)) {
        FCITX_ERROR() << "Failed to save ModernIME clipboard history: "
                      << historyError;
    }
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
        event.kind = KeyKind::DeleteForward;
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
    if (bindings.arrowNavigation && key.check(FcitxKey_Up)) {
        event.kind = KeyKind::PreviousClipboardItem;
        return event;
    }
    if (bindings.arrowNavigation && key.check(FcitxKey_Down)) {
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
        event.kind = KeyKind::MoveCompositionLeft;
        return event;
    }
    if (bindings.arrowNavigation && key.check(FcitxKey_Right)) {
        event.kind = KeyKind::MoveCompositionRight;
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
            return std::nullopt;
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

bool shouldCommitCompositionBeforePassThrough(std::string_view preedit,
                                              const fcitx::Key &key) {
    if (preedit.empty() || hasNonShiftModifier(key)) {
        return false;
    }
    const auto unicode = fcitx::Key::keySymToUnicode(key.sym());
    return (unicode >= 'A' && unicode <= 'Z') ||
           (unicode >= '0' && unicode <= '9');
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
    // Collect only while ModernIME is the active input method: clipboard
    // content copied under other input methods or keyboard layouts must not
    // end up in the history file.
    if (instance_->inputMethod(inputContext) != "modernime") {
        return;
    }
    // The settings client may have deleted or cleared the history file since
    // our last write; adopt the on-disk state before recording new content.
    {
        std::string reloadError;
        if (!clipboardHistory_.reloadIfExternallyChanged(&reloadError) &&
            !reloadError.empty()) {
            FCITX_ERROR() << "Failed to reload ModernIME clipboard history: "
                          << reloadError;
        }
    }
    try {
        const auto text = clipboardAddon_->callWithSignature<
            std::string(const fcitx::InputContext *)>(
            "Clipboard::clipboard", inputContext);
        std::string historyError;
        clipboardHistory_.observe(text, &historyError);
        if (!historyError.empty()) {
            // 剪贴板每 50ms 轮询一次；同一错误只在首次出现时记录，
            // 避免日志刷屏。
            if (historyError != lastHistoryError_) {
                FCITX_ERROR() << "Failed to save ModernIME clipboard history: "
                              << historyError;
                lastHistoryError_ = historyError;
            }
        } else if (!lastHistoryError_.empty()) {
            lastHistoryError_.clear();
        }
    } catch (const std::exception &) {
        // A third-party or older clipboard addon may not expose this optional
        // function. Clipboard mode remains harmlessly empty in that case.
    }
}

void ModernIMEInputMethod::pollFileChanges() {
    if (instance_ == nullptr) {
        return;
    }
    std::error_code error;
    const auto settingsFile = settingsPaths().settingsFile;
    const auto settingsTime =
        std::filesystem::last_write_time(settingsFile, error);
    if (!error && settingsTime != settingsMtime_) {
        settingsMtime_ = settingsTime;
        settings_ = core::SettingsStore::load(settingsFile).settings;
        keyBindings_ = keyBindings(settings_);
        ++settingsGeneration_;
        FCITX_INFO() << "ModernIME settings reloaded from "
                     << settingsFile.string();
    }
    const auto dictionaryFile = settingsPaths().userDictionary;
    const auto dictionaryTime =
        std::filesystem::last_write_time(dictionaryFile, error);
    if (!error && dictionaryTime != userDictionaryMtime_) {
        userDictionaryMtime_ = dictionaryTime;
#ifdef MODERNIME_HAS_LIBIME_PINYIN
        if (pinyin::PinyinCandidateProvider::reloadUserDictionary(
                resources_.pinyin)) {
            FCITX_INFO() << "ModernIME user dictionary reloaded from "
                         << dictionaryFile.string();
        }
#endif
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
    if (contextState->settingsGeneration() != settingsGeneration_) {
        contextState->applySettings(settings_, settingsGeneration_);
    }
    const auto context = extractSurroundingContext(
        event.inputContext()->surroundingText(), 32);
    contextState->controller().setContext(context.first, context.second);
    auto modernEvent = translateKey(event.key(), keyBindings_);
    const bool clipboardActive = keyBindings_.clipboardEnabled &&
                                  contextState->controller().active();
    // 两段式剪贴板触发：拼音组合恰好是触发字母（默认 v）时，下一键为
    // 触发数字（默认 2）则丢弃组合并打开剪贴板。触发字母本身始终按
    // 正常输入处理，不会被消费或改写；数字选择关闭时同样生效。
    if (clipboardActive &&
        clipboardTriggerFire(contextState->controller().page().preedit,
                             event.key(), keyBindings_.clipboardTrigger)) {
        contextState->setClipboardEntries(clipboardHistory_.entries());
        if (contextState->controller().handle(
                {KeyKind::OpenClipboard, 0, 0})) {
            event.filterAndAccept();
        }
        return;
    }
    if (!modernEvent.has_value()) {
        if (shouldCommitCompositionBeforePassThrough(
                contextState->controller().page().preedit, event.key())) {
            contextState->controller().handle({KeyKind::Enter, 0, 0});
        }
        return;
    }

    if (contextState->controller().handle(*modernEvent)) {
        event.filterAndAccept();
    }
}

void ModernIMEInputMethod::activate(const fcitx::InputMethodEntry &,
                                    fcitx::InputContextEvent &event) {
    if (auto *contextState = state(event.inputContext()); contextState != nullptr) {
        if (contextState->settingsGeneration() != settingsGeneration_) {
            contextState->applySettings(settings_, settingsGeneration_);
        }
        contextState->controller().setActive(
            settings_.inputEnabled &&
            settings_.defaultMode == core::InputMode::Chinese);
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
