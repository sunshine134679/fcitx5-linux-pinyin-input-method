#pragma once

#include "modernime/core/candidate_model.h"
#include "modernime/core/async_clipboard_history.h"
#include "modernime/core/candidate_provider.h"
#include "modernime/core/input_state.h"
#include "modernime/core/settings.h"
#include "modernime/core/component_lookup.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cctype>
#include <cstdint>
#include <memory>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace modernime::fcitx5 {

inline constexpr std::size_t kClipboardPageSize = 5;
inline constexpr std::size_t kFallbackCandidatePageSize = 9;

enum class KeyKind {
    Character,
    Backspace,
    DeleteCandidate,
    CloseClipboard,
    DeleteForward,
    Escape,
    Enter,
    Space,
    Digit,
    Toggle,
    PreviousCandidate,
    NextCandidate,
    MoveCompositionLeft,
    MoveCompositionRight,
    PreviousClipboardItem,
    NextClipboardItem,
    PreviousPage,
    NextPage,
    OpenClipboard,
    Punctuation,
    CommitFirstCharacter,
    CommitLastCharacter,
    UndoCommit,
    ToggleTemporaryEnglish,
    OpenComponentLookup,
    OpenCalculator,
    OpenSymbols,
    OpenFavoriteSymbols,
    StartClipboardSearch,
    ToggleClipboardFavorite,
};

enum class AuxiliaryInputMode { None, TemporaryEnglish, ComponentLookup, Calculator, Symbols };

// Empty valid text is a real document state; unavailable clients may use the
// bounded continuous-commit fallback. Invalidated state drops that fallback.

enum class ContextAvailability { Valid, Unavailable, Invalidated };

struct KeyEvent final {
    KeyKind kind = KeyKind::Character;
    char character = 0;
    char digit = 0;
};

struct ControllerOptions final {
    bool inputEnabled = true;
    bool numberSelection = true;
    bool arrowNavigation = true;
    bool pageNavigation = true;
    bool punctuationEnabled = true;
    std::size_t pageSize = 9;
    bool clipboardEnabled = true;
    std::string clipboardTrigger = "V+2";
    bool characterExtractionEnabled = true;
    bool learningEnabled = true;
    bool contextLearningEnabled = true;
    bool autoPairPunctuation = true;
    bool smartPunctuation = true;
    core::UnderscoreStyle underscoreStyle = core::UnderscoreStyle::Smart;
    std::map<char, std::string> punctuationOverrides;
    bool undoCommitEnabled = true;
    bool temporaryEnglishEnabled = true;
    bool componentLookupEnabled = true;
    bool calculatorEnabled = true;
    bool symbolSearchEnabled = true;
    std::vector<std::string> symbolFavorites;
};

struct KeyBindings final {
    std::string toggleKey = "Ctrl+Shift+Space";
    bool numberSelection = true;
    bool arrowNavigation = true;
    bool pageNavigation = true;
    bool clipboardEnabled = true;
    std::string clipboardTrigger = "V+2";
    bool characterExtractionEnabled = true;
    bool undoCommitEnabled = true;
    bool temporaryEnglishEnabled = true;
    bool componentLookupEnabled = true;
    bool calculatorEnabled = true;
    bool symbolSearchEnabled = true;
};

struct TextEditContext final {
    std::string before;
    std::string after;
    std::size_t cursor = 0;
    bool operator==(const TextEditContext &) const = default;
};

class EngineHost {
public:
    virtual ~EngineHost() = default;

    virtual void publishPage(const core::CandidatePage &page) = 0;
    virtual void commit(std::string_view text) = 0;
    // Optional: only a focused, non-sensitive, selection-free document may
    // expose this window. Unsupported hosts retain ordinary input behavior.
    virtual std::optional<TextEditContext> textEditContext() const { return std::nullopt; }
    virtual bool insertPair(std::string_view, std::string_view) { return false; }
    virtual bool replaceSurrounding(std::string_view, std::string_view,
                                    std::string_view) { return false; }
};

class ModernIMEController final {
public:
    explicit ModernIMEController(EngineHost &host,
                                 core::CandidateProvider *provider = nullptr,
                                 ControllerOptions options = {});

    ~ModernIMEController();

    bool handle(const KeyEvent &event);
    bool select(const core::CandidateSelectionRequest &request);
    bool select(std::size_t index);
    bool extractCharacter(bool firstChar);
    bool removeCurrent();
    void setContext(std::string before, std::string after,
                    ContextAvailability availability = ContextAvailability::Valid);
    void reset();
    void invalidateTextActions();
    void invalidateUndo() { undoCommit_.reset(); }
    bool undoLastCommit();
    AuxiliaryInputMode auxiliaryInputMode() const { return auxiliaryMode_; }
    bool auxiliaryInputActive() const { return auxiliaryMode_ != AuxiliaryInputMode::None; }
    bool temporaryEnglishMode() const { return auxiliaryMode_ == AuxiliaryInputMode::TemporaryEnglish; }
    void setComponentLookup(std::shared_ptr<const core::ComponentLookup> lookup) { componentLookup_ = std::move(lookup); }
    void cancelAuxiliaryInput() { if (auxiliaryInputActive()) clearComposition(); }
    bool finishTemporaryEnglish(bool commitText);
    bool setSymbolFilter(std::string category, bool favoritesOnly);
    void setActive(bool active);
    void setSensitive(bool sensitive);
    bool isSensitive() const { return sensitive_; }

    std::weak_ptr<core::SelectionEndpoint> endpoint() const noexcept;
    std::uint64_t sessionId() const noexcept { return sessionId_; }
    std::uint64_t displayGeneration() const noexcept { return displayGeneration_; }
    std::uint64_t layoutGeneration() const noexcept { return layoutGeneration_; }

    std::size_t currentPageIndex() const {
        if (page_.items.empty() || page_.pageBoundaries.empty()) {
            return 0;
        }
        for (std::size_t index = 0; index < page_.pageBoundaries.size();
             ++index) {
            const auto &boundary = page_.pageBoundaries[index];
            if (page_.cursor >= boundary.begin &&
                page_.cursor < boundary.end) {
                return index;
            }
        }
        return page_.pageBoundaries.size() - 1;
    }
    std::size_t pageSize() const;
    bool setPageBoundaries(std::uint64_t displayGeneration,
                           std::uint64_t layoutGeneration,
                           std::vector<core::PageBoundary> boundaries);
    void setPageBoundaries(std::vector<core::PageBoundary> boundaries);
    bool moveCursor(std::ptrdiff_t delta) {
        if (page_.items.empty() || delta == 0) {
            return false;
        }

        const auto current = static_cast<std::ptrdiff_t>(page_.cursor);
        const auto last = static_cast<std::ptrdiff_t>(page_.items.size() - 1);
        const auto next =
            std::clamp(current + delta, std::ptrdiff_t{0}, last);
        if (next == current) {
            if (delta > 0 && page_.hasMoreCandidates) return loadMoreCandidates();
            return true;
        }
        page_.cursor = static_cast<std::size_t>(next);
        host_.publishPage(page_);
        return true;
    }
    bool movePage(std::ptrdiff_t delta) {
        if (page_.items.empty() || delta == 0) {
            return false;
        }

        const auto current = static_cast<std::ptrdiff_t>(currentPageIndex());
        const auto last = static_cast<std::ptrdiff_t>(
            page_.pageBoundaries.empty() ? 0
                                         : page_.pageBoundaries.size() - 1);
        const auto next =
            std::clamp(current + delta, std::ptrdiff_t{0}, last);
        if (next == current) {
            if (delta > 0 && page_.hasMoreCandidates) return loadMoreCandidates();
            return true;
        }
        page_.cursor = page_.pageBoundaries.empty()
                           ? 0
                           : page_.pageBoundaries[static_cast<std::size_t>(next)]
                                 .begin;
        host_.publishPage(page_);
        return true;
    }

    void setClipboardEntries(std::vector<std::string> entries);
    void setClipboardSnapshot(std::shared_ptr<const std::vector<std::string>> entries,
                              core::ClipboardAvailability availability);
    void setOptions(ControllerOptions options);
    bool clipboardMode() const { return clipboardMode_; }
    bool clipboardSearching() const { return clipboardMode_ && clipboardSearching_; }
    bool setClipboardQuery(std::string query, bool favoritesOnly = false);
    void setClipboardSnapshot(const core::ClipboardSnapshot &snapshot);
    void setClipboardFavoriteHandler(std::function<std::uint64_t(std::string_view, bool)> handler) { clipboardFavoriteHandler_ = std::move(handler); }
    bool favoriteCurrentClipboard();
    bool clipboardFavoritePending() const { return clipboardFavoritePendingId_ != 0; }

    bool active() const { return options_.inputEnabled && active_; }
    const core::CandidatePage &page() const { return page_; }
    void refreshPage();

private:
    bool loadMoreCandidates();
    void clearComposition();
    bool commitCurrent();
    bool commitRawPreedit(std::string_view suffix = {});
    bool commitPunctuation(char ascii);
    bool editPairedPunctuation(char ascii);
    bool eraseEmptyPair();
    bool handleAuxiliaryInput(const KeyEvent &event);
    bool openAuxiliaryInput(AuxiliaryInputMode mode, std::string_view seed);
    void publishAuxiliaryPage();
    void rememberUndo(const std::optional<TextEditContext> &before,
                      const core::CandidateIdentity &candidate,
                      std::string rawInput, std::string remainingInput);
    bool openClipboard();
    void publishClipboardPage();
    bool handleClipboardSearch(const KeyEvent &event);
    bool replaceComposition(std::string nextInput,
                            std::size_t nextCursor);
    void publishProviderPage(bool adoptRemainingComposition = false,
                             std::optional<std::size_t> cursor = std::nullopt);
    void updatePreeditCursor();
    void ensurePageBoundaries();
    core::PageBoundary currentPageBoundary() const;
    void appendFallbackContext(std::string_view text);

    struct RecentCommit final {
        std::string originalText;
        std::string originalPinyin;
        std::string currentText;
        std::string currentPinyin;
        std::string basePrefix;
        std::string contextAfter;
        std::chrono::steady_clock::time_point timestamp;
        bool valid = false;
        // 提交后是否观测到过非空文档窗口。真实"删光"必然先经过非空阶段；
        // 首次观测即为空只可能是窗口跳转（如切到空输入框），不构成删光证据。
        bool sawDocumentText = false;
        std::vector<std::string> originalChars;
        std::vector<std::string> originalSyllables;
    };
    void initRecentCommit(const core::CandidateItem &candidate,
                          const std::string &basePrefix,
                          const std::string &contextAfter);
    bool checkRecentCommitDeletionFeedback(const std::string &before,
                                           const std::string &after);
    void finalizeRecentCommitDeletion();

    EngineHost &host_;
    core::CandidateProvider *provider_ = nullptr;
    core::InputState input_;
    core::CandidatePage page_;
    std::string contextBefore_;
    std::string contextAfter_;
    std::string fallbackContextBefore_;
    ContextAvailability contextAvailability_ = ContextAvailability::Unavailable;
    ControllerOptions options_;
    bool active_ = true;
    bool clipboardMode_ = false;
    bool clipboardSearching_ = false;
    bool clipboardFavoritesOnly_ = false;
    std::string clipboardQuery_;
    std::size_t clipboardQueryCursor_ = 0;
    std::shared_ptr<const std::vector<std::string>> clipboardFavorites_;
    core::ClipboardAvailability clipboardFavoritesAvailability_ = core::ClipboardAvailability::Ready;
    std::string clipboardFavoriteError_;
    std::uint64_t clipboardFavoritePendingId_ = 0;
    std::function<std::uint64_t(std::string_view, bool)> clipboardFavoriteHandler_;
    bool doubleQuoteOpen_ = false;
    bool singleQuoteOpen_ = false;
    std::string compositionInput_;
    std::size_t compositionCursor_ = 0;
    std::shared_ptr<const std::vector<std::string>> clipboardEntries_;
    core::ClipboardAvailability clipboardAvailability_ = core::ClipboardAvailability::Ready;
    // The displayed page may intentionally lag the latest snapshot while a
    // sensitive context prevents asynchronous UI updates.
    core::ClipboardAvailability clipboardPageAvailability_ = core::ClipboardAvailability::Ready;
    RecentCommit recentCommit_;
    struct AutoPair final {
        std::string opener, closer;
        TextEditContext expected;
    };
    std::vector<AutoPair> autoPairs_;
    struct UndoCommit final {
        TextEditContext expected;
        core::CandidateIdentity candidate;
        std::string rawInput, remainingInput;
        std::chrono::steady_clock::time_point timestamp;
    };
    std::optional<UndoCommit> undoCommit_;
    AuxiliaryInputMode auxiliaryMode_ = AuxiliaryInputMode::None;
    std::shared_ptr<const core::ComponentLookup> componentLookup_;
    std::string symbolCategory_;
    bool symbolsFavoritesOnly_ = false;
    std::string auxiliaryInput_;
    std::size_t auxiliaryCursor_ = 0;
    std::uint64_t sessionId_ = 0;
    std::uint64_t displayGeneration_ = 1;
    std::uint64_t layoutGeneration_ = 1;
    std::uint64_t providerGeneration_ = 0;
    class ControllerEndpoint;
    std::shared_ptr<ControllerEndpoint> endpoint_;
    bool sensitive_ = false;
    bool inSetContext_ = false;
};

} // namespace modernime::fcitx5
