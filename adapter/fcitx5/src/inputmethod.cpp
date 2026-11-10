#include "modernime/fcitx5/fcitx_engine.h"
#include "modernime/fcitx5/settings_shortcuts.h"

#ifdef MODERNIME_HAS_LIBIME_PINYIN
#include "modernime/pinyin/pinyin_shared_resources.h"
#endif

#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/statusarea.h>
#include <fcitx/surroundingtext.h>
#include <fcitx/userinterfacemanager.h>
#include <fcitx-utils/keysymgen.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/utf8.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace modernime::fcitx5 {
namespace {

// Fcitx < 5.1.2 has no cursor commit API. Keep its existing build support.
template <typename Context>
bool commitWithCursor(Context *ic, const std::string &text, std::size_t cursor) {
    if constexpr (requires { ic->commitStringWithCursor(text, cursor); }) {
        constexpr auto capability = static_cast<fcitx::CapabilityFlag>(1ull << 41);
        if (ic->capabilityFlags().test(capability)) {
            ic->commitStringWithCursor(text, cursor);
            return true;
        }
    }
    return false;
}

void writeRuntimeState(const std::filesystem::path &statePath,
                       std::string_view contentHash,
                       std::uint64_t generation) {
    if (statePath.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(statePath.parent_path(), ec);
    const auto tempPath = statePath.string() + ".tmp";
    std::ofstream out(tempPath, std::ios::trunc);
    if (!out) {
        return;
    }
    out << "pid=" << ::getpid() << "\n";
    out << "applied_content_hash=" << contentHash << "\n";
    out << "settings_generation=" << generation << "\n";
    out << "source=fcitx5-adapter\n";
    out.close();
    std::filesystem::rename(tempPath, statePath, ec);
}

constexpr std::string_view statePropertyName = "modernime-fcitx5-state";

bool hasNonShiftModifier(const fcitx::Key &key) {
    return key.states().testAny(fcitx::KeyStates(
        {fcitx::KeyState::Ctrl, fcitx::KeyState::Alt, fcitx::KeyState::Super,
         fcitx::KeyState::Meta}));
}

int keyDigitSelection(const fcitx::Key &key) {
    if (key.hasModifier()) {
        return -1;
    }
    const auto sym = key.sym();
    if (sym >= FcitxKey_1 && sym <= FcitxKey_9) {
        return static_cast<int>(sym - FcitxKey_1);
    }
    if (sym >= FcitxKey_KP_1 && sym <= FcitxKey_KP_9) {
        return static_cast<int>(sym - FcitxKey_KP_1);
    }
    if (sym == FcitxKey_0 || sym == FcitxKey_KP_0) {
        return 9;
    }
    return -1;
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

ControllerOptions controllerOptions(const core::ModernIMESettings &settings) {
    return {settings.inputEnabled, settings.numberSelection,
            settings.arrowNavigation, settings.pageNavigation,
            settings.punctuationEnabled,
            static_cast<std::size_t>(settings.candidatePageSize),
            settings.clipboardEnabled, settings.clipboardTrigger,
            settings.characterExtractionEnabled,
            settings.learningEnabled, settings.contextLearningEnabled,
            settings.autoPairPunctuation, settings.smartPunctuation,
            settings.underscoreStyle, settings.punctuationOverrides, settings.undoCommitEnabled,
            settings.temporaryEnglishEnabled, settings.componentLookupEnabled, settings.calculatorEnabled};
}

KeyBindings keyBindings(const core::ModernIMESettings &settings) {
    return {settings.toggleKey, settings.numberSelection,
            settings.arrowNavigation, settings.pageNavigation,
            settings.clipboardEnabled, settings.clipboardTrigger,
            settings.characterExtractionEnabled, settings.undoCommitEnabled, settings.temporaryEnglishEnabled, settings.componentLookupEnabled, settings.calculatorEnabled};
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
        keyDigitSelection(key) != static_cast<int>(second - '1')) {
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
    return {settings.learningEnabled, settings.contextLearningEnabled,
            settings.englishDefinitionEnabled,
            settings.clipboardEnabled, settings.clipboardTrigger, settings.fuzzyPinyin,
            settings.pinyinScheme, settings.candidatePins, settings.calculatorEnabled};
}
#endif

struct SurroundingContext final {
    std::string first;
    std::string second;
    ContextAvailability availability = ContextAvailability::Valid;
};

static SurroundingContext
extractSurroundingContextState(const fcitx::SurroundingText &text,
                          std::size_t maxChars) {
    if (!text.isValid() || maxChars == 0) {
        return {"", "", ContextAvailability::Invalidated};
    }

    const auto &value = text.text();
    if (value.empty()) {
        return {};
    }

    const auto targetCursor = static_cast<std::size_t>(text.cursor());
    if (targetCursor > value.size()) {
        return {"", "", ContextAvailability::Invalidated};
    }

    std::array<std::string::const_iterator, 64> stackRing;
    std::vector<std::string::const_iterator> heapRing;
    auto *ring = (maxChars < 64) ? stackRing.data()
                                 : (heapRing.resize(maxChars + 1), heapRing.data());

    auto it = value.cbegin();
    std::size_t charIndex = 0;
    ring[0] = it;

    while (it != value.cend() && charIndex < targetCursor) {
        it = fcitx::utf8::nextChar(it);
        ++charIndex;
        ring[charIndex % (maxChars + 1)] = it;
    }

    if (charIndex != targetCursor) {
        return {"", "", ContextAvailability::Invalidated};
    }
    const auto cursorIterator = it;
    const auto beforeCount = std::min(maxChars, charIndex);
    const auto begin = ring[(charIndex - beforeCount) % (maxChars + 1)];

    auto end = cursorIterator;
    std::size_t afterCount = 0;
    while (end != value.cend() && afterCount < maxChars) {
        end = fcitx::utf8::nextChar(end);
        ++afterCount;
    }

    std::string before(begin, cursorIterator);
    std::string after(cursorIterator, end);
    if (!fcitx::utf8::validate(before) || !fcitx::utf8::validate(after)) {
        return {"", "", ContextAvailability::Invalidated};
    }

    // 过滤换行符：上下文关联不应跨换行延续
    auto lastNl = before.find_last_of("\r\n");
    if (lastNl != std::string::npos) {
        before = before.substr(lastNl + 1);
    }
    auto firstNl = after.find_first_of("\r\n");
    if (firstNl != std::string::npos) {
        after = after.substr(0, firstNl);
    }

    // 过滤控制字符
    before.erase(std::remove_if(before.begin(), before.end(), [](unsigned char c) {
        return c < 0x20 && c != '\t';
    }), before.end());
    after.erase(std::remove_if(after.begin(), after.end(), [](unsigned char c) {
        return c < 0x20 && c != '\t';
    }), after.end());

    // A line boundary yielding an empty local window is not evidence that
    // the previous committed word was deleted. Drop its pending feedback.
    const auto availability =
        lastNl != std::string::npos && before.empty() && after.empty()
            ? ContextAvailability::Invalidated
            : ContextAvailability::Valid;
    return {std::move(before), std::move(after), availability};
}

std::pair<std::string, std::string>
extractSurroundingContext(const fcitx::SurroundingText &text,
                          std::size_t maxChars) {
    auto context = extractSurroundingContextState(text, maxChars);
    return {std::move(context.first), std::move(context.second)};
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
        // immediate=true 让候选面板更新在本键处理内同步送达 UI addon，
        // 不再等事件循环的 defer event 下一轮才 flush。
        inputContext_->updateUserInterface(
            fcitx::UserInterfaceComponent::InputPanel, true);
        return;
    }

    auto candidates = std::make_unique<FcitxCandidateList>(
        controller_ ? controller_->endpoint() : std::weak_ptr<core::SelectionEndpoint>{},
        controller_ ? controller_->displayGeneration() : page.displayGeneration,
        controller_ ? controller_->layoutGeneration() : 1, page.hasMoreCandidates);
    const bool clipboard = page.mode == core::CandidatePageMode::Clipboard;
    candidates->setLayoutHint(clipboard ? fcitx::CandidateLayoutHint::Vertical
                                        : fcitx::CandidateLayoutHint::Horizontal);
    candidates->setCursorIncludeUnselected(true);
    for (std::size_t index = 0; index < page.items.size(); ++index) {
        core::CandidateSelectionRequest req;
        req.sessionId = page.sessionId;
        req.displayGeneration = page.displayGeneration;
        req.globalIndex = index;
        req.expected = core::candidateIdentity(page.items[index], page);
        candidates->append<FcitxCandidateWord>(
            page.items[index].text,
            controller_ ? controller_->endpoint() : std::weak_ptr<core::SelectionEndpoint>{},
            std::move(req),
            page.items[index].annotation,
            page.items[index].readingHint,
            page.items[index].readingHintAfterCharIndex);
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
    // immediate=true 让候选面板更新在本键处理内同步送达 UI addon，
    // 不再等事件循环的 defer event 下一轮才 flush。
    inputContext_->updateUserInterface(
        fcitx::UserInterfaceComponent::InputPanel, true);
}

void FcitxEngineHost::commit(std::string_view text) {
    inputContext_->commitString(std::string(text));
}

std::optional<TextEditContext> FcitxEngineHost::textEditContext() const {
    const auto flags = inputContext_->capabilityFlags();
    const auto &text = inputContext_->surroundingText();
    if (!inputContext_->hasFocus() ||
        flags.testAny(fcitx::CapabilityFlag::PasswordOrSensitive) ||
        !flags.test(fcitx::CapabilityFlag::SurroundingText) || !text.isValid() ||
        text.cursor() != text.anchor()) return std::nullopt;
    auto window = extractSurroundingContextState(text, 256);
    if (window.availability != ContextAvailability::Valid) return std::nullopt;
    return TextEditContext{std::move(window.first), std::move(window.second), text.cursor()};
}

bool FcitxEngineHost::insertPair(std::string_view opener, std::string_view closer) {
    if (!textEditContext() || inputContext_->capabilityFlags().test(
            fcitx::CapabilityFlag::Terminal)) return false;
    const auto pair = std::string(opener) + std::string(closer);
    if (commitWithCursor(inputContext_, pair, fcitx::utf8::length(opener))) return true;
    inputContext_->commitString(pair);
    inputContext_->forwardKey(fcitx::Key(FcitxKey_Left));
    inputContext_->forwardKey(fcitx::Key(FcitxKey_Left), true);
    return true;
}

bool FcitxEngineHost::replaceSurrounding(std::string_view before, std::string_view after,
                                       std::string_view replacement) {
    const auto current = textEditContext();
    if (!current || !current->before.ends_with(before) ||
        !current->after.starts_with(after)) return false;
    const auto left = fcitx::utf8::length(before);
    const auto right = fcitx::utf8::length(after);
    inputContext_->deleteSurroundingText(-static_cast<int>(left),
                                       static_cast<unsigned>(left + right));
    if (!replacement.empty()) inputContext_->commitString(std::string(replacement));
    return true;
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
    controller_.setComponentLookup(resources.componentLookup);
    host_.setController(controller_);
    controller_.setActive(settings.defaultMode == core::InputMode::Chinese);
}

void FcitxInputContextState::applySettings(
    const core::ModernIMESettings &settings, std::uint64_t generation) {
    controller_.invalidateTextActions();
#ifdef MODERNIME_HAS_LIBIME_PINYIN
    if (provider_ != nullptr) {
        auto opts = pinyinOptions(settings);
        if (controller().isSensitive()) {
            opts.learningEnabled = false;
            opts.contextLearningEnabled = false;
        }
        provider_->applyOptions(opts);
    }
#else
    (void)settings;
#endif
    controller().setOptions(controllerOptions(settings));
    settingsGeneration_ = generation;
}

ModernIMEInputMethod::ModernIMEInputMethod(fcitx::AddonManager *manager)
    : ModernIMEInputMethod(manager, core::SettingsStore::load(settingsPaths().settingsFile)) {}

ModernIMEInputMethod::ModernIMEInputMethod(fcitx::AddonManager *manager,
                                           const core::SettingsLoadResult &initResult)
    : manager_(manager),
      instance_(manager == nullptr ? nullptr : manager->instance()),
      settingsStamp_(initResult.stamp),
      userDictionaryStamp_(core::SettingsFileStamp::fromPath(settingsPaths().userDictionary)),
      appliedVersion_({initResult.isSuccess() ? initResult.contentHash : std::string{}, 0}),
      settingsLoadFailed_(initResult.isIoError()),
      settingsRetryCount_(0),
      settingsGeneration_(0),
      settings_(initResult.settings),
      keyBindings_(keyBindings(settings_)),
      stateFactory_([this](fcitx::InputContext &inputContext) {
#ifdef MODERNIME_HAS_LIBIME_PINYIN
          // 首个输入上下文创建时加入后台预热（通常早已完成），
          // 保证首键路径不再承担全量词典加载。
          ensurePinyinResources();
#endif
          return new FcitxInputContextState(inputContext, settings_,
                                            resources_, settingsGeneration_);
      }) {
    // chttrans owns conversion/configuration and provides its native action.
    // Missing/disabled addons leave normal simplified input available.
    if (manager_) manager_->addon("chttrans", true);
    resources_.componentLookup = std::make_shared<const core::ComponentLookup>();
#ifdef MODERNIME_HAS_LIBIME_PINYIN
    // One dictionary/language-model/learning-writer set for the whole engine;
    // every input context only adds its own composition state. The load
    // takes roughly 400ms, so it runs on a detached background thread while
    // addon construction returns immediately; state creation joins it.
    {
        auto promise = std::make_shared<std::promise<
            std::shared_ptr<pinyin::PinyinCandidateProvider::SharedResources>>>();
        pinyinResourcesFuture_ = promise->get_future();
        const auto paths = pinyinPaths();
        const auto options = pinyinOptions(settings_);
        std::thread([promise, paths, options]() {
            try {
                promise->set_value(
                    pinyin::PinyinCandidateProvider::createSharedResources(
                        paths, options));
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
        }).detach();
    }
#endif
    if (initResult.isSuccess()) {
        writeRuntimeState(
            settingsPaths().runtimeState,
            initResult.contentHash,
            0);
    } else {
        writeRuntimeState(
            settingsPaths().runtimeState,
            "",
            0);
        if (initResult.isIoError()) {
            FCITX_WARN() << "ModernIME initial settings load failed: "
                         << settingsPaths().settingsFile.string();
        }
    }
    subModeAction_.setShortText("中");
    subModeAction_.setLongText("中文输入法");
    subModeAction_.setIcon("input-keyboard-symbolic");
    if (instance_) {
        subModeAction_.registerAction("modernime-mode", &instance_->userInterfaceManager());
        settingsAction_.registerAction("modernime-settings", &instance_->userInterfaceManager());
    }
    settingsAction_.setShortText("ModernIME 设置");
    settingsAction_.setLongText("打开设置、个人词典、剪贴板与诊断");
    settingsAction_.setIcon("preferences-system-symbolic");
    settingsAction_.setMenu(&settingsMenu_);
    for (const auto &shortcut : settingsShortcuts) {
        auto action = std::make_unique<fcitx::SimpleAction>();
        action->setShortText(shortcut.label);
        action->setLongText(shortcut.label);
        action->setIcon(shortcut.icon);
        if (instance_) {
            action->registerAction(std::string("modernime-settings-") + shortcut.page,
                                   &instance_->userInterfaceManager());
        }
        action->connect<fcitx::SimpleAction::Activated>(
            [page = shortcut.page](fcitx::InputContext *) {
                static const char moduleAnchor = 0;
                openSettingsPage(page, &moduleAnchor);
            });
        settingsMenu_.addAction(action.get());
        settingsShortcuts_.push_back(std::move(action));
    }
    subModeAction_.connect<fcitx::SimpleAction::Activated>([this](fcitx::InputContext *ic) {
        if (auto *contextState = state(ic)) {
            contextState->controller().handle({KeyKind::Toggle, 0, 0});
            rememberAppMode(ic);
            const bool active = contextState->controller().active();
            subModeAction_.setShortText(active ? "中" : "英");
            subModeAction_.setLongText(active ? "中文输入法" : "英文键盘");
            subModeAction_.update(ic);
            ic->updateUserInterface(fcitx::UserInterfaceComponent::StatusArea);
        }
    });
    readyEndpoint_ = std::make_shared<ReadyNotificationEndpoint>();
    readyEndpoint_->dispatcher = &dispatcher_;
    readyEndpoint_->owner = this;
    readyEndpoint_->instance = instance_;
    readyEndpoint_->active = true;
    readyEndpoint_->onDispatch = [this]() {
        if (auto *contextState = focusedActiveState()) {
            if (!contextState->controller().page().preedit.empty()) {
                contextState->controller().refreshPage();
            }
        }
    };
    // Clipboard completion has its own notification owner. It must not rebuild
    // a pinyin page or invalidate the user's current cursor/selection identity.
    clipboardEndpoint_ = std::make_shared<ReadyNotificationEndpoint>();
    clipboardEndpoint_->dispatcher = &dispatcher_;
    clipboardEndpoint_->owner = this;
    clipboardEndpoint_->instance = instance_;
    clipboardEndpoint_->onDispatch = [this]() {
        consumeClipboardSnapshot();
        if (keyBindings_.clipboardEnabled && settings_.inputEnabled) {
            if (auto *contextState = focusedActiveState()) {
                contextState->setClipboardSnapshot(clipboardSnapshot_);
            }
        }
    };
    if (instance_ != nullptr) {
        dispatcher_.attach(&instance_->eventLoop());
        instance_->inputContextManager().registerProperty(
            std::string(statePropertyName), &stateFactory_);
        surroundingWatcher_ = instance_->watchEvent(
            fcitx::EventType::InputContextSurroundingTextUpdated,
            fcitx::EventWatcherPhase::Default,
            [this](fcitx::Event &event) {
                auto *ic =
                    static_cast<fcitx::InputContextEvent &>(event).inputContext();
                if (ic == nullptr || instance_->inputMethod(ic) != "modernime") {
                    return;
                }
                auto *contextState = state(ic);
                if (contextState != nullptr) {
                    const bool isSensitive =
                        ic->capabilityFlags().test(fcitx::CapabilityFlag::Password) ||
                        ic->capabilityFlags().test(fcitx::CapabilityFlag::Sensitive);
                    contextState->controller().setSensitive(isSensitive);
                    if (!isSensitive && ic->surroundingText().isValid()) {
                        const auto context = extractSurroundingContextState(
                            ic->surroundingText(), 32);
                        contextState->controller().setContext(context.first,
                                                              context.second,
                                                              context.availability);
                    } else {
                        const auto availability =
                            !isSensitive && !ic->capabilityFlags().test(
                                fcitx::CapabilityFlag::SurroundingText)
                                ? ContextAvailability::Unavailable
                                : ContextAvailability::Invalidated;
                        contextState->controller().setContext("", "", availability);
                    }
                }
            });
        clipboardCapabilityWatcher_ = instance_->watchEvent(
            fcitx::EventType::InputContextCapabilityChanged,
            fcitx::EventWatcherPhase::Default,
            [this](fcitx::Event &event) {
                auto *ic = static_cast<fcitx::InputContextEvent &>(event).inputContext();
                if (ic != nullptr &&
                    ic == instance_->inputContextManager().mostRecentInputContext() &&
                    (ic->capabilityFlags().test(fcitx::CapabilityFlag::Password) ||
                     ic->capabilityFlags().test(fcitx::CapabilityFlag::Sensitive))) {
                    clipboardNeedsBaseline_ = true;
                }
            });
        clipboardTimer_ = instance_->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 500000, 500000,
            [this](fcitx::EventSourceTime *source, std::uint64_t) {
                pollClipboard();
                source->setNextInterval(500000);
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
    const std::weak_ptr<ReadyNotificationEndpoint> clipboardEndpoint = clipboardEndpoint_;
    clipboardHistory_ = std::make_unique<core::AsyncClipboardHistory>(
        settingsPaths().clipboardHistory, [clipboardEndpoint, hasInstance = instance_ != nullptr] {
            if (hasInstance) {
                if (auto endpoint = clipboardEndpoint.lock()) endpoint->notify();
            }
        });
}

ModernIMEInputMethod::~ModernIMEInputMethod() {
#ifdef MODERNIME_HAS_LIBIME_PINYIN
    if (resources_.pinyin) {
        resources_.pinyin->setOnReadyCallback(nullptr);
    }
#endif
    if (readyEndpoint_) {
        readyEndpoint_->deactivateAndDrain();
    }
    if (clipboardEndpoint_) {
        clipboardEndpoint_->deactivateAndDrain();
    }
    if (clipboardHistory_) {
        const auto error = clipboardHistory_->shutdown();
        if (!error.empty()) {
            FCITX_ERROR() << "Failed to close ModernIME clipboard history: " << error;
        }
    }
    dispatcher_.detach();
#ifdef MODERNIME_HAS_LIBIME_PINYIN
    // 后台线程只持有 promise/paths/options 的值拷贝，不引用 this；
    // 这里等待加载结果落地，避免析构先于 set_value 完成。
    if (pinyinResourcesFuture_.valid()) {
        pinyinResourcesFuture_.wait();
    }
#endif
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
    clipboardHistory_->requestSave();
}

FcitxInputContextState *ModernIMEInputMethod::focusedActiveState() const {
    if (instance_ == nullptr) return nullptr;
    auto *ic = instance_->inputContextManager().mostRecentInputContext();
    if (ic == nullptr || !ic->hasFocus() || instance_->inputMethod(ic) != "modernime" ||
        ic->capabilityFlags().test(fcitx::CapabilityFlag::Password) ||
        ic->capabilityFlags().test(fcitx::CapabilityFlag::Sensitive)) return nullptr;
    auto *contextState = state(ic);
    return contextState && contextState->controller().active() ? contextState : nullptr;
}

void ModernIMEInputMethod::consumeClipboardSnapshot() {
    if (auto snapshot = clipboardHistory_->takeSnapshot()) {
        clipboardSnapshot_ = std::move(*snapshot);
        if (!clipboardSnapshot_.error.empty() && clipboardSnapshot_.error != lastHistoryError_) {
            FCITX_ERROR() << "ModernIME clipboard history: " << clipboardSnapshot_.error;
        }
        lastHistoryError_ = clipboardSnapshot_.error;
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
    if (bindings.undoCommitEnabled && key.check(FcitxKey_BackSpace,
            fcitx::KeyStates({fcitx::KeyState::Ctrl, fcitx::KeyState::Shift}))) {
        event.kind = KeyKind::UndoCommit;
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
    if (bindings.pageNavigation &&
        (key.check(FcitxKey_Page_Up) || key.check(FcitxKey_minus) ||
         key.check(FcitxKey_KP_Subtract))) {
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
    const int selection = keyDigitSelection(key);
    if (selection >= 0 && selection < 9) {
        event.kind = KeyKind::Digit;
        event.digit = static_cast<char>('1' + selection);
        return event;
    }
    if (key.check(FcitxKey_0) || key.check(FcitxKey_KP_0)) {
        event.kind = KeyKind::Digit;
        event.digit = '0';
        return event;
    }
    if (bindings.characterExtractionEnabled) {
        if (key.check(FcitxKey_bracketleft, fcitx::KeyStates(fcitx::KeyState::Ctrl)) ||
            (!hasNonShiftModifier(key) && key.check(FcitxKey_bracketleft))) {
            event.kind = KeyKind::CommitFirstCharacter;
            event.character = '[';
            return event;
        }
        if (key.check(FcitxKey_bracketright, fcitx::KeyStates(fcitx::KeyState::Ctrl)) ||
            (!hasNonShiftModifier(key) && key.check(FcitxKey_bracketright))) {
            event.kind = KeyKind::CommitLastCharacter;
            event.character = ']';
            return event;
        }
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
    if (preedit.front() == 'v') {
        const auto unicode = fcitx::Key::keySymToUnicode(key.sym());
        if ((unicode >= '0' && unicode <= '9') || unicode == '.') {
            return false;
        }
    }
    const auto unicode = fcitx::Key::keySymToUnicode(key.sym());
    return (unicode >= 'A' && unicode <= 'Z') ||
           (unicode >= '0' && unicode <= '9');
}

void ModernIMEInputMethod::pollClipboard() {
    consumeClipboardSnapshot();
    if (!keyBindings_.clipboardEnabled || !settings_.inputEnabled || manager_ == nullptr ||
        instance_ == nullptr) {
        clipboardNeedsBaseline_ = true;
        return;
    }
    // 先做最廉价的短路判断再查 addon：ModernIME 非当前输入法时
    // 完全跳过后续的 addon 查找与调用，轮询空转成本降到最低。
    auto *inputContext =
        instance_->inputContextManager().mostRecentInputContext();
    // The Clipboard addon returns global content; it does not filter by ic.
    // Check eligibility before querying the addon or polling the history file.
    if (inputContext == nullptr || !inputContext->hasFocus() ||
        inputContext->capabilityFlags().test(fcitx::CapabilityFlag::Password) ||
        inputContext->capabilityFlags().test(fcitx::CapabilityFlag::Sensitive) ||
        instance_->inputMethod(inputContext) != "modernime") {
        clipboardNeedsBaseline_ = true;
        return;
    }
    if (!clipboardAddonLookupAttempted_) {
        clipboardAddon_ = manager_->addon("clipboard", true);
        clipboardAddonLookupAttempted_ = true;
    }
    if (clipboardAddon_ == nullptr) {
        return;
    }
    try {
        const auto text = clipboardAddon_->callWithSignature<
            std::string(const fcitx::InputContext *)>(
            "Clipboard::clipboard", inputContext);
        if (clipboardNeedsBaseline_) {
            // A value left during an excluded interval is not a new allowed
            // copy. Only later changes may enter history; dedup stays in core.
            clipboardHistory_->setObservationBaseline(text);
            clipboardNeedsBaseline_ = false;
        } else {
            clipboardHistory_->observe(text);
        }
        clipboardHistory_->requestRefresh();
    } catch (const std::exception &) {
        clipboardNeedsBaseline_ = true;
        // A third-party or older clipboard addon may not expose this optional
        // function. Clipboard mode remains harmlessly empty in that case.
    }
}

void ModernIMEInputMethod::pollFileChanges() {
    if (instance_ == nullptr) {
        return;
    }
    const auto settingsFile = settingsPaths().settingsFile;
    const auto currentStamp = core::SettingsFileStamp::fromPath(settingsFile);
    const bool stampChanged = (currentStamp != settingsStamp_);
    bool shouldRetry = false;
    if (stampChanged) {
        shouldRetry = true;
    } else if (settingsLoadFailed_) {
        // Immediate retries for first 5 attempts (every 2s poll),
        // then bounded backoff (every 2nd poll, i.e., every 4s) indefinitely.
        if (settingsRetryCount_ < 5) {
            shouldRetry = true;
        } else if ((settingsRetryCount_ - 5) % 2 == 0) {
            shouldRetry = true;
        }
    }
    if (settingsLoadFailed_) {
        ++settingsRetryCount_;
    }
    if (shouldRetry) {
        if (currentStamp.exists) {
            const auto loadResult = core::SettingsStore::load(settingsFile);
            if (loadResult.isSuccess()) {
                settingsLoadFailed_ = false;
                settingsRetryCount_ = 0;
                settingsStamp_ = loadResult.stamp;
                if (loadResult.contentHash != appliedVersion_.contentHash) {
                    if (settings_.appInputModes != loadResult.settings.appInputModes ||
                        settings_.rememberModePerApp != loadResult.settings.rememberModePerApp)
                        rememberedAppModes_.clear();
                    settings_ = loadResult.settings;
                    keyBindings_ = keyBindings(settings_);
                    if (!settings_.inputEnabled || !keyBindings_.clipboardEnabled) {
                        clipboardNeedsBaseline_ = true;
                    }
                    ++settingsGeneration_;
                    appliedVersion_ = {loadResult.contentHash, settingsGeneration_};
                    writeRuntimeState(settingsPaths().runtimeState, loadResult.contentHash,
                                      settingsGeneration_);
                    if (instance_ != nullptr) {
                        if (auto *ic = instance_->inputContextManager().mostRecentInputContext()) {
                            if (auto *contextState = state(ic)) {
                                if (contextState->settingsGeneration() != settingsGeneration_) {
                                    contextState->applySettings(settings_, settingsGeneration_);
                                }
                            }
                        }
                    }
                    FCITX_INFO() << "ModernIME settings reloaded from "
                                 << settingsFile.string();
                }
            } else if (loadResult.isIoError()) {
                settingsLoadFailed_ = true;
                settingsStamp_ = currentStamp;
                FCITX_WARN() << "ModernIME failed to read settings from "
                             << settingsFile.string();
            } else if (loadResult.isMissing()) {
                settingsStamp_ = currentStamp;
                settingsLoadFailed_ = false;
                settingsRetryCount_ = 0;
            }
        } else {
            settingsStamp_ = currentStamp;
            settingsLoadFailed_ = false;
            settingsRetryCount_ = 0;
        }
    }
    const auto dictionaryFile = settingsPaths().userDictionary;
    const auto dictStamp = core::SettingsFileStamp::fromPath(dictionaryFile);
    if (dictStamp != userDictionaryStamp_) {
        userDictionaryStamp_ = dictStamp;
#ifdef MODERNIME_HAS_LIBIME_PINYIN
        ensurePinyinResources();
        if (pinyin::PinyinCandidateProvider::reloadUserDictionary(
                resources_.pinyin)) {
            FCITX_INFO() << "ModernIME user dictionary reloaded from "
                         << dictionaryFile.string();
        }
#endif
    }
#ifdef MODERNIME_HAS_LIBIME_PINYIN
    ensurePinyinResources();
    if (pinyin::PinyinCandidateProvider::refreshLearningStore(resources_.pinyin)) {
        FCITX_INFO() << "ModernIME learning store reset from "
                     << settingsPaths().learningStore.string();
    }
#endif
}

#ifdef MODERNIME_HAS_LIBIME_PINYIN
void ModernIMEInputMethod::ensurePinyinResources() {
    if (pinyinResourcesResolved_ || !pinyinResourcesFuture_.valid()) {
        return;
    }
    pinyinResourcesResolved_ = true;
    try {
        resources_.pinyin = pinyinResourcesFuture_.get();
        if (resources_.pinyin) {
            auto endpoint = readyEndpoint_;
            resources_.pinyin->setOnReadyCallback([endpoint]() {
                if (endpoint) {
                    endpoint->notify();
                }
            });
        }
    } catch (const std::exception &error) {
        FCITX_ERROR() << "ModernIME background dictionary load failed: "
                      << error.what();
    }
}
#endif

void ModernIMEInputMethod::keyEvent(const fcitx::InputMethodEntry &,
                                    fcitx::KeyEvent &event) {
    consumeClipboardSnapshot();
    if (event.isRelease()) {
        if (event.key().check(FcitxKey_Shift_L) ||
            event.key().check(FcitxKey_Shift_R)) {
            if (shiftPressed_ && !otherKeyPressedWhileShift_) {
                shiftPressed_ = false;
                auto *contextState = state(event.inputContext());
                if (contextState != nullptr) {
                    if (contextState->settingsGeneration() != settingsGeneration_) {
                        contextState->applySettings(settings_, settingsGeneration_);
                    }
                    if (!contextState->controller().page().preedit.empty()) {
                        contextState->controller().handle({KeyKind::Enter, 0, 0});
                    }
                    if (contextState->controller().handle({KeyKind::Toggle, 0, 0})) {
                        rememberAppMode(event.inputContext());
                        const bool active = contextState->controller().active();
                        subModeAction_.setShortText(active ? "中" : "英");
                        subModeAction_.setLongText(active ? "中文输入法" : "英文键盘");
                        if (auto *ic = event.inputContext()) {
                            subModeAction_.update(ic);
                            ic->updateUserInterface(fcitx::UserInterfaceComponent::StatusArea);
                        }
                        event.filterAndAccept();
                        return;
                    }
                }
            }
            shiftPressed_ = false;
        }
        return;
    }

    if (event.key().check(FcitxKey_Shift_L) ||
        event.key().check(FcitxKey_Shift_R)) {
        shiftPressed_ = true;
        otherKeyPressedWhileShift_ = false;
    } else {
        otherKeyPressedWhileShift_ = true;
    }
    try {
        auto *contextState = state(event.inputContext());
        if (contextState == nullptr) {
            return;
        }
        if (contextState->settingsGeneration() != settingsGeneration_) {
            contextState->applySettings(settings_, settingsGeneration_);
        }
        const bool isSensitive =
            event.inputContext()->capabilityFlags().test(fcitx::CapabilityFlag::Password) ||
            event.inputContext()->capabilityFlags().test(fcitx::CapabilityFlag::Sensitive);
        contextState->controller().setSensitive(isSensitive);
        if (!isSensitive && event.inputContext()->surroundingText().isValid()) {
            const auto context = extractSurroundingContextState(
                event.inputContext()->surroundingText(), 32);
            contextState->controller().setContext(
                context.first, context.second, context.availability);
        } else {
            const auto availability =
                !isSensitive && !event.inputContext()->capabilityFlags().test(
                    fcitx::CapabilityFlag::SurroundingText)
                    ? ContextAvailability::Unavailable
                    : ContextAvailability::Invalidated;
            contextState->controller().setContext("", "", availability);
        }
        auto modernEvent = translateKey(event.key(), keyBindings_);
        if (keyBindings_.temporaryEnglishEnabled &&
            (event.rawKey().check(FcitxKey_e, fcitx::KeyStates({fcitx::KeyState::Ctrl, fcitx::KeyState::Shift})) ||
             event.rawKey().check(FcitxKey_E, fcitx::KeyStates({fcitx::KeyState::Ctrl, fcitx::KeyState::Shift})))) {
            modernEvent = KeyEvent{KeyKind::ToggleTemporaryEnglish};
        } else if (keyBindings_.componentLookupEnabled && !hasNonShiftModifier(event.key()) &&
                   !contextState->controller().auxiliaryInputActive() &&
                   contextState->controller().page().rawInput == "u" && event.key().check(FcitxKey_plus)) {
            modernEvent = KeyEvent{KeyKind::OpenComponentLookup};
        } else if (keyBindings_.calculatorEnabled && !hasNonShiftModifier(event.key()) &&
                   !contextState->controller().auxiliaryInputActive() &&
                   contextState->controller().page().rawInput == "v" && event.key().check(FcitxKey_equal)) {
            modernEvent = KeyEvent{KeyKind::OpenCalculator};
        } else if (contextState->controller().auxiliaryInputActive() && !hasNonShiftModifier(event.key())) {
            const auto c = fcitx::Key::keySymToUnicode(event.key().sym());
            if (c > 0x20 && c <= 0x7e) modernEvent = KeyEvent{KeyKind::Character, static_cast<char>(c)};
        }
        if (modernEvent && modernEvent->kind == KeyKind::Character && modernEvent->character == '\'' &&
            !contextState->controller().auxiliaryInputActive() &&
            contextState->controller().page().preedit.empty()) modernEvent->kind = KeyKind::Punctuation;
        if (modernEvent && contextState->controller().page().preedit.empty() &&
            !contextState->controller().clipboardMode() && !hasNonShiftModifier(event.key()) &&
            (modernEvent->kind == KeyKind::PreviousPage || modernEvent->kind == KeyKind::NextPage)) {
            const auto c = fcitx::Key::keySymToUnicode(event.key().sym());
            if (isAsciiPunctuation(c)) modernEvent = KeyEvent{KeyKind::Punctuation, static_cast<char>(c)};
        }
        // Fcitx normalizes Ctrl+Shift+BackSpace to Ctrl+BackSpace. Inspect the
        // unnormalized key so the application's word deletion remains intact.
        if (keyBindings_.undoCommitEnabled && event.rawKey().check(FcitxKey_BackSpace,
                fcitx::KeyStates({fcitx::KeyState::Ctrl, fcitx::KeyState::Shift})))
            modernEvent = KeyEvent{KeyKind::UndoCommit};
        const bool clipboardActive = keyBindings_.clipboardEnabled &&
                                      contextState->controller().active() &&
                                      !contextState->controller().auxiliaryInputActive();
        if (clipboardActive) {
            contextState->setClipboardSnapshot(clipboardSnapshot_);
        }
        // 两段式剪贴板触发：拼音组合恰好是触发字母（默认 v）时，下一键为
        // 触发数字（默认 2）则丢弃组合并打开剪贴板。触发字母本身始终按
        // 正常输入处理，不会被消费或改写；数字选择关闭时同样生效。
        if (clipboardActive &&
            clipboardTriggerFire(contextState->controller().page().preedit,
                                 event.key(), keyBindings_.clipboardTrigger)) {
            if (contextState->controller().handle(
                    {KeyKind::OpenClipboard, 0, 0})) {
                event.filterAndAccept();
            }
            return;
        }
        if (!modernEvent.has_value()) {
            if (contextState->controller().auxiliaryInputActive() && hasNonShiftModifier(event.key())) {
                if (contextState->controller().temporaryEnglishMode()) contextState->controller().finishTemporaryEnglish(true);
                else contextState->controller().cancelAuxiliaryInput();
            }
            if (hasNonShiftModifier(event.key())) contextState->controller().invalidateTextActions();
            if (contextState->controller().page().preedit.empty() &&
                !event.inputContext()->surroundingText().isValid()) {
                contextState->controller().setContext(
                    "", "", ContextAvailability::Invalidated);
            }
            if (shouldCommitCompositionBeforePassThrough(
                    contextState->controller().page().preedit, event.key())) {
                contextState->controller().handle({KeyKind::Enter, 0, 0});
            }
            return;
        }

        if (contextState->controller().handle(*modernEvent)) {
            if (modernEvent->kind == KeyKind::Toggle) {
                rememberAppMode(event.inputContext());
                const bool active = contextState->controller().active();
                subModeAction_.setShortText(active ? "中" : "英");
                subModeAction_.setLongText(active ? "中文输入法" : "英文键盘");
                if (auto *ic = event.inputContext()) {
                    subModeAction_.update(ic);
                    ic->updateUserInterface(fcitx::UserInterfaceComponent::StatusArea);
                }
            }
            event.filterAndAccept();
        } else if (modernEvent->kind == KeyKind::Backspace || modernEvent->kind == KeyKind::DeleteForward) {
            contextState->controller().invalidateUndo();
        } else if (modernEvent->kind == KeyKind::MoveCompositionLeft ||
                   modernEvent->kind == KeyKind::MoveCompositionRight ||
                   modernEvent->kind == KeyKind::PreviousClipboardItem || modernEvent->kind == KeyKind::NextClipboardItem ||
                   modernEvent->kind == KeyKind::PreviousPage || modernEvent->kind == KeyKind::NextPage) {
            contextState->controller().invalidateTextActions();
        }
    } catch (const std::exception &error) {
        FCITX_ERROR() << "ModernIME keyEvent caught exception: " << error.what();
    } catch (...) {
        FCITX_ERROR() << "ModernIME keyEvent caught unknown exception";
    }
}

void ModernIMEInputMethod::activate(const fcitx::InputMethodEntry &,
                                    fcitx::InputContextEvent &event) {
    clipboardNeedsBaseline_ = true;
    if (auto *contextState = state(event.inputContext()); contextState != nullptr) {
        if (contextState->settingsGeneration() != settingsGeneration_) {
            contextState->applySettings(settings_, settingsGeneration_);
        }
        const bool active = appStartsInChinese(event.inputContext());
        contextState->controller().setActive(active);
        subModeAction_.setShortText(active ? "中" : "英");
        subModeAction_.setLongText(active ? "中文输入法" : "英文键盘");
        if (auto *ic = event.inputContext()) {
            ic->statusArea().addAction(fcitx::StatusGroup::InputMethod, &subModeAction_);
            ic->statusArea().addAction(fcitx::StatusGroup::InputMethod, &settingsAction_);
            if (instance_) {
                if (auto *scriptAction = instance_->userInterfaceManager().lookupAction("chttrans"))
                    ic->statusArea().addAction(fcitx::StatusGroup::InputMethod, scriptAction);
            }
            subModeAction_.update(ic);
            ic->updateUserInterface(fcitx::UserInterfaceComponent::StatusArea);
        }
    }
}

void ModernIMEInputMethod::deactivate(const fcitx::InputMethodEntry &entry,
                                      fcitx::InputContextEvent &event) {
    clipboardNeedsBaseline_ = true;
    shiftPressed_ = false;
    otherKeyPressedWhileShift_ = false;
    if (readyEndpoint_) {
        std::lock_guard lock(readyEndpoint_->mutex);
        ++readyEndpoint_->notificationGeneration;
    }
    reset(entry, event);
    if (auto *contextState = state(event.inputContext()); contextState != nullptr) {
        contextState->controller().setActive(false);
    }
}

void ModernIMEInputMethod::rememberAppMode(fcitx::InputContext *context) {
    if (!settings_.rememberModePerApp || !settings_.inputEnabled || !context ||
        context->program().empty() || context->capabilityFlags().testAny(
            fcitx::CapabilityFlag::PasswordOrSensitive)) return;
    if (auto *contextState = state(context)) {
        if (!rememberedAppModes_.contains(context->program()) && rememberedAppModes_.size() == 128)
            rememberedAppModes_.erase(rememberedAppModes_.begin());
        rememberedAppModes_[context->program()] = contextState->controller().active()
            ? core::InputMode::Chinese : core::InputMode::English;
    }
}

bool ModernIMEInputMethod::appStartsInChinese(fcitx::InputContext *context) const {
    if (context && !context->program().empty()) {
        if (settings_.rememberModePerApp) {
            const auto remembered = rememberedAppModes_.find(context->program());
            if (remembered != rememberedAppModes_.end()) return remembered->second == core::InputMode::Chinese;
        }
        const auto rule = settings_.appInputModes.find(context->program());
        if (rule != settings_.appInputModes.end()) return rule->second == core::InputMode::Chinese;
    }
    return settings_.defaultMode == core::InputMode::Chinese;
}

void ModernIMEInputMethod::reset(const fcitx::InputMethodEntry &,
                                 fcitx::InputContextEvent &event) {
    shiftPressed_ = false;
    otherKeyPressedWhileShift_ = false;
    if (readyEndpoint_) {
        std::lock_guard lock(readyEndpoint_->mutex);
        ++readyEndpoint_->notificationGeneration;
    }
    if (auto *contextState = state(event.inputContext()); contextState != nullptr) {
        contextState->controller().reset();
    }
}

} // namespace modernime::fcitx5
