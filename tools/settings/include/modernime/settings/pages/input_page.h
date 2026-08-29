#pragma once

#include "modernime/settings/settings_model.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

struct _GtkWidget;

namespace modernime::settings {

using GtkWidget = ::_GtkWidget;

struct InputPageState final {
    bool dependentControlsSensitive = true;
    bool toggleKeyValid = true;
    std::string toggleKeyMessage;
    bool canApply = true;
};

inline InputPageState deriveInputPageState(const SettingsWindowModel &model) {
    InputPageState state;
    state.dependentControlsSensitive = model.settings().inputEnabled;
    const auto validation = model.validation();
    state.canApply = model.dirty() && validation.valid;
    for (const auto &issue : validation.issues) {
        if (issue.key == "input.toggle_key") {
            state.toggleKeyValid = false;
            state.toggleKeyMessage = issue.message;
            break;
        }
    }
    return state;
}

class InputPage final {
public:
    InputPage(SettingsWindowModel &model, std::function<void()> changed);
    ~InputPage();

    InputPage(const InputPage &) = delete;
    InputPage &operator=(const InputPage &) = delete;

    GtkWidget *widget() const;
    void refresh();
    bool focusTarget(std::string_view target);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace modernime::settings
