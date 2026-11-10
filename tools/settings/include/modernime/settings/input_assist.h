#pragma once
#include "modernime/settings/settings_model.h"
#include <gtk/gtk.h>
#include <functional>
#include <memory>
#include <string_view>

namespace modernime::settings {
class InputAssist final {
public:
    InputAssist(SettingsWindowModel &model, std::function<void()> changed);
    ~InputAssist();
    GtkWidget *widget() const;
    void refresh();
    bool focusTarget(std::string_view target);
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
