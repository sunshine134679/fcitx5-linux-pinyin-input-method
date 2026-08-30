#include "modernime/settings/settings_window.h"

#include "modernime/settings/settings_shell.h"

#include <gtk/gtk.h>

#include <utility>

namespace modernime::settings {

SettingsWindow::SettingsWindow(void *application, core::SettingsPaths paths)
    : shell_(std::make_unique<SettingsShell>(
          GTK_APPLICATION(application), std::move(paths))) {}

SettingsWindow::~SettingsWindow() = default;

void SettingsWindow::present() {
    shell_->present();
}

void SettingsWindow::showBasicPage() {
    shell_->show(SettingsPageId::Input);
}

void SettingsWindow::showCandidatePage() {
    shell_->show(SettingsPageId::Input);
}

void SettingsWindow::showClipboardPage() {
    shell_->show(SettingsPageId::Clipboard);
}

void SettingsWindow::showLearningPage() {
    shell_->show(SettingsPageId::Learning);
}

void SettingsWindow::showDictionaryPage() {
    shell_->show(SettingsPageId::Dictionary);
}

void SettingsWindow::showStatusPage() {
    shell_->show(SettingsPageId::Diagnostics);
}

void SettingsWindow::presentError(std::string_view message) {
    shell_->presentError(message);
}

} // namespace modernime::settings
