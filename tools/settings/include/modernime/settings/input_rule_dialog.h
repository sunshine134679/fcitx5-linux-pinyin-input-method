#pragma once

#include "modernime/core/settings.h"
#include <gtk/gtk.h>
#include <functional>

namespace modernime::settings {

enum class InputRuleKind { Application, Punctuation };

// Edits a private copy. Accept updates the caller's draft, never the file.
GtkWidget *createInputRuleDialog(GtkWindow *parent,
    const core::ModernIMESettings &settings, InputRuleKind kind,
    std::function<void(core::ModernIMESettings)> accept);

} // namespace modernime::settings
