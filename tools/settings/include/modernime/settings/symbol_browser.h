#pragma once

#include <gtk/gtk.h>
#include <functional>

namespace modernime::settings {

class SettingsWindowModel;

// Shows a native dialog with a private favorite draft. Acceptance updates only
// model.settings().symbolFavorites and notifies changed; the main window still
// owns saving. The caller keeps model alive and may track/destroy the dialog.
GtkWidget *openSymbolBrowser(GtkWindow *parent, SettingsWindowModel &model,
                             std::function<void()> changed);

} // namespace modernime::settings
