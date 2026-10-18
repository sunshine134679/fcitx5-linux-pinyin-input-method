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

GtkWidget *SettingsWindow::window() const {
    return shell_ ? shell_->window() : nullptr;
}

void SettingsWindow::show(SettingsPageId page, std::string_view target) {
    shell_->show(page, target);
}

void SettingsWindow::showOverviewPage() {
    show(SettingsPageId::Overview);
}

void SettingsWindow::showBasicPage() {
    show(SettingsPageId::Input);
}

void SettingsWindow::showCandidatePage() {
    show(SettingsPageId::Candidates);
}

void SettingsWindow::showClipboardPage() {
    show(SettingsPageId::Clipboard);
}

void SettingsWindow::showLearningPage() {
    show(SettingsPageId::Learning);
}

void SettingsWindow::showDictionaryPage() {
    show(SettingsPageId::Dictionary);
}

void SettingsWindow::showStatusPage() {
    show(SettingsPageId::Diagnostics);
}

void SettingsWindow::showUpdatesPage() {
    show(SettingsPageId::Updates);
}

void SettingsWindow::presentError(std::string_view message) {
    shell_->presentError(message);
}

} // namespace modernime::settings
