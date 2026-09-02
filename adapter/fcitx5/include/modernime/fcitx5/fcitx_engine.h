#pragma once

#include "modernime/core/persistent_clipboard_history.h"
#include "modernime/fcitx5/engine.h"

#ifdef MODERNIME_HAS_LIBIME_PINYIN
#include "modernime/pinyin/pinyin_candidate_provider.h"
#endif

#include <fcitx/candidatelist.h>
#include <fcitx/inputpanel.h>
#include <fcitx-utils/event.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/surroundingtext.h>

#include <algorithm>
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

// 中英混输边界：大写字母或直通数字到来时，先提交已有拼音组合，再把
// 当前按键原样交给应用；带 Ctrl/Alt/Super 的快捷键不触发提交。
bool shouldCommitCompositionBeforePassThrough(std::string_view preedit,
                                              const fcitx::Key &key);

// 两段式剪贴板触发判定：拼音组合恰好是触发字母（默认 v）且当前按键是
// 触发数字（默认 2、无 Ctrl/Alt/Super 修饰）时返回 true。
bool clipboardTriggerFire(std::string_view preedit, const fcitx::Key &key,
                          std::string_view trigger);

// Every published word keeps its original global index. Page-local labels and
// mouse selection therefore share the controller's global candidate model.
class FcitxCandidateWord final : public fcitx::CandidateWord {
public:
    FcitxCandidateWord(std::string text, ModernIMEController *controller,
                       std::size_t index)
        : CandidateWord(fcitx::Text(std::move(text))), controller_(controller),
          index_(index) {}

    void select(fcitx::InputContext *) const override {
        if (controller_ != nullptr) {
            controller_->select(index_);
        }
    }
private:
    ModernIMEController *controller_;
    std::size_t index_;
};

// CommonCandidateList stores every candidate, while this subclass exposes the
// current variable [begin, end) slice through the normal Fcitx CandidateList
// interface. This preserves Fcitx's pageable and bulk-list contracts without
// padding or hiding candidates.
class FcitxCandidateList final : public fcitx::CommonCandidateList {
public:
    explicit FcitxCandidateList(ModernIMEController *controller)
        : controller_(controller) {}

    void setPageBoundaries(std::vector<core::PageBoundary> boundaries,
                           bool notifyController = true) {
        const auto total = static_cast<std::size_t>(totalSize());
        if (!validBoundaries(boundaries, total)) {
            boundaries.clear();
        }
        const bool explicitBoundaries = !boundaries.empty();
        if (boundaries.empty() && total > 0) {
            boundaries.push_back({0, total});
        }
        boundaries_ = std::move(boundaries);
        if (notifyController && controller_ != nullptr &&
            explicitBoundaries) {
            controller_->setPageBoundaries(boundaries_);
        }
    }

    std::size_t pageBegin() const { return currentBoundary().begin; }

    const fcitx::CandidateWord &candidate(int index) const override {
        return candidateFromAll(
            static_cast<int>(currentBoundary().begin) + index);
    }

    int size() const override {
        const auto boundary = currentBoundary();
        return static_cast<int>(boundary.end - boundary.begin);
    }

    int cursorIndex() const override {
        const auto global = globalCursorIndex();
        const auto boundary = currentBoundary();
        if (global < 0 || static_cast<std::size_t>(global) < boundary.begin ||
            static_cast<std::size_t>(global) >= boundary.end) {
            return -1;
        }
        return global - static_cast<int>(boundary.begin);
    }

    bool hasPrev() const override { return currentPage() > 0; }
    bool hasNext() const override {
        return currentPage() + 1 < totalPages();
    }
    void prev() override {
        if (hasPrev()) {
            if (controller_ != nullptr) {
                auto *controller = controller_;
                controller->movePage(-1);
                return;
            }
            setGlobalCursorIndex(static_cast<int>(
                boundaries_[static_cast<std::size_t>(currentPage() - 1)]
                    .begin));
        }
    }
    void next() override {
        if (hasNext()) {
            usedNextBefore_ = true;
            if (controller_ != nullptr) {
                auto *controller = controller_;
                controller->movePage(1);
                return;
            }
            setGlobalCursorIndex(static_cast<int>(
                boundaries_[static_cast<std::size_t>(currentPage() + 1)]
                    .begin));
        }
    }
    bool usedNextBefore() const override { return usedNextBefore_; }
    int totalPages() const override {
        return static_cast<int>(boundaries_.size());
    }
    int currentPage() const override {
        if (boundaries_.empty()) {
            return 0;
        }
        const auto global = std::max(0, globalCursorIndex());
        for (std::size_t index = 0; index < boundaries_.size(); ++index) {
            if (static_cast<std::size_t>(global) >= boundaries_[index].begin &&
                static_cast<std::size_t>(global) < boundaries_[index].end) {
                return static_cast<int>(index);
            }
        }
        return static_cast<int>(boundaries_.size() - 1);
    }
    void setPage(int page) override {
        if (boundaries_.empty()) {
            return;
        }
        page = std::clamp(page, 0, totalPages() - 1);
        if (controller_ != nullptr) {
            auto *controller = controller_;
            const auto delta = static_cast<std::ptrdiff_t>(page) -
                               static_cast<std::ptrdiff_t>(
                                   controller->currentPageIndex());
            controller->movePage(delta);
            return;
        }
        setGlobalCursorIndex(
            static_cast<int>(boundaries_[static_cast<std::size_t>(page)].begin));
    }

    void prevCandidate() override {
        const auto global = globalCursorIndex();
        if (global > 0) {
            if (controller_ != nullptr) {
                auto *controller = controller_;
                controller->moveCursor(-1);
                return;
            }
            setGlobalCursorIndex(global - 1);
        }
    }
    void nextCandidate() override {
        const auto global = globalCursorIndex();
        if (global >= 0 && global + 1 < totalSize()) {
            if (controller_ != nullptr) {
                auto *controller = controller_;
                controller->moveCursor(1);
                return;
            }
            setGlobalCursorIndex(global + 1);
        }
    }

private:
    static bool validBoundaries(const std::vector<core::PageBoundary> &values,
                                std::size_t total) {
        if (values.empty()) {
            return total == 0;
        }
        std::size_t expected = 0;
        for (const auto &value : values) {
            if (value.begin != expected || value.begin >= value.end ||
                value.end > total) {
                return false;
            }
            expected = value.end;
        }
        return expected == total;
    }

    core::PageBoundary currentBoundary() const {
        if (boundaries_.empty()) {
            return {};
        }
        return boundaries_[static_cast<std::size_t>(currentPage())];
    }

    ModernIMEController *controller_ = nullptr;
    std::vector<core::PageBoundary> boundaries_;
    bool usedNextBefore_ = false;
};

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
