#pragma once

#include "modernime/settings/quick_phrase_store.h"

#include <functional>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

struct _GtkWidget;

namespace modernime::settings {

// A child view of Personal Dictionary, with its own file and explicit save action.
class QuickPhraseEditor final {
public:
    QuickPhraseEditor(std::filesystem::path path,
                      std::function<void(std::string)> notify,
                      std::function<QuickPhraseReloadResult()> reload = reloadQuickPhrases);
    ~QuickPhraseEditor();
    QuickPhraseEditor(const QuickPhraseEditor &) = delete;
    QuickPhraseEditor &operator=(const QuickPhraseEditor &) = delete;

    _GtkWidget *widget() const;
    // Lazy and non-destructive: never replaces an unsaved draft or an active operation.
    void refresh();
    bool focusTarget(std::string_view target);
    bool hasUnsavedChanges() const;
    bool hasPendingOperations() const;
    bool confirmLeave();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace modernime::settings
