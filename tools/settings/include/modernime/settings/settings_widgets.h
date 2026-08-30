#pragma once

#include <gtk/gtk.h>

#include <initializer_list>
#include <string_view>

namespace modernime::settings {

inline constexpr std::string_view kSettingsWindowClass = "modernime-settings";
inline constexpr std::string_view kSettingsSidebarClass = "modernime-sidebar";
inline constexpr std::string_view kSettingsSectionClass = "modernime-section";
inline constexpr std::string_view kSettingsPrimaryButtonClass =
    "suggested-action";
inline constexpr std::string_view kSettingsDangerButtonClass =
    "destructive-action";
inline constexpr std::string_view kSettingsFocusFallbackClass =
    "modernime-focus-fallback";

void installSettingsStyles();
std::string_view settingsStyles();
void setAccessibleWidgetText(GtkWidget *widget, std::string_view name,
                             std::string_view description);
void setSettingsFocusChain(
    GtkWidget *container,
    std::initializer_list<GtkWidget *> focusableWidgets);

GtkWidget *createPageShell(std::string_view title, std::string_view subtitle);
GtkWidget *createSectionCard(std::string_view title,
                             std::string_view description);
GtkWidget *createScrollablePage(GtkWidget *page);
GtkWidget *createSettingRow(std::string_view title,
                            std::string_view description,
                            GtkWidget *control);
GtkWidget *createStatusPill(std::string_view text);
GtkWidget *createEmptyState(std::string_view title,
                            std::string_view description);
bool focusWidgetOrFallback(GtkWidget *target, GtkWidget *fallback = nullptr);

} // namespace modernime::settings
