#pragma once

#include "modernime/core/candidate_model.h"
#include "modernime/core/candidate_provider.h"
#include "modernime/core/input_state.h"
#include "modernime/core/settings.h"

#include <algorithm>
#include <cstddef>
#include <cctype>
#include <cstdint>
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
};

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
};

struct KeyBindings final {
    std::string toggleKey = "Ctrl+Shift+Space";
    bool numberSelection = true;
    bool arrowNavigation = true;
    bool pageNavigation = true;
    bool clipboardEnabled = true;
    std::string clipboardTrigger = "V+2";
};

class EngineHost {
public:
    virtual ~EngineHost() = default;

    virtual void publishPage(const core::CandidatePage &page) = 0;
    virtual void commit(std::string_view text) = 0;
};

class ModernIMEController final {
public:
    explicit ModernIMEController(EngineHost &host,
                                 core::CandidateProvider *provider = nullptr,
                                 ControllerOptions options = {});

    bool handle(const KeyEvent &event);
    bool select(std::size_t index);
    bool removeCurrent();
    void setContext(std::string before, std::string after);
    void reset();
    void setActive(bool active);

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
    void setPageBoundaries(std::vector<core::PageBoundary> boundaries) {
        if (!boundaries.empty()) {
            std::size_t expectedBegin = 0;
            for (const auto &boundary : boundaries) {
                if (boundary.begin != expectedBegin ||
                    boundary.begin >= boundary.end ||
                    boundary.end > page_.items.size()) {
                    page_.pageBoundaries.clear();
                    return;
                }
                expectedBegin = boundary.end;
            }
            if (expectedBegin != page_.items.size()) {
                page_.pageBoundaries.clear();
                return;
            }
        }
        page_.pageBoundaries = std::move(boundaries);
        if (!page_.items.empty()) {
            page_.cursor = std::min(page_.cursor, page_.items.size() - 1);
        }
    }
    bool moveCursor(std::ptrdiff_t delta) {
        if (page_.items.empty() || delta == 0) {
            return false;
        }

        const auto current = static_cast<std::ptrdiff_t>(page_.cursor);
        const auto last = static_cast<std::ptrdiff_t>(page_.items.size() - 1);
        const auto next =
            std::clamp(current + delta, std::ptrdiff_t{0}, last);
        if (next == current) {
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
    void setOptions(ControllerOptions options) { options_ = options; }
    bool clipboardMode() const { return clipboardMode_; }

    bool active() const { return active_; }
    const core::CandidatePage &page() const { return page_; }

private:
    void clearComposition();
    void refreshPage();
    bool commitCurrent();
    bool commitRawPreedit(std::string_view suffix = {});
    bool commitPunctuation(char ascii);
    bool openClipboard();
    bool replaceComposition(std::string nextInput,
                            std::size_t nextCursor);
    void publishProviderPage(bool adoptRemainingComposition = false);
    void updatePreeditCursor();
    void ensurePageBoundaries();
    core::PageBoundary currentPageBoundary() const;

    EngineHost &host_;
    core::CandidateProvider *provider_ = nullptr;
    core::InputState input_;
    core::CandidatePage page_;
    std::string contextBefore_;
    std::string contextAfter_;
    ControllerOptions options_;
    bool active_ = true;
    bool clipboardMode_ = false;
    bool doubleQuoteOpen_ = false;
    bool singleQuoteOpen_ = false;
    std::string compositionInput_;
    std::size_t compositionCursor_ = 0;
    std::vector<std::string> clipboardEntries_;
};

} // namespace modernime::fcitx5
