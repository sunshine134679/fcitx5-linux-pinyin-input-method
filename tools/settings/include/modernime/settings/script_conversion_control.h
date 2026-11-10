#pragma once

#include <gtk/gtk.h>
#include <memory>
#include <string_view>

namespace modernime::settings {

// Fcitx owns this immediate setting; it is deliberately outside the settings draft.
class ScriptConversionControl final {
public:
    ScriptConversionControl();
    ~ScriptConversionControl();
    GtkWidget *widget() const;
    void refresh();
    bool focusTarget(std::string_view target);
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace modernime::settings
