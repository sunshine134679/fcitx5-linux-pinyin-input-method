#pragma once

#include <gtk/gtk.h>

#include <initializer_list>
#include <array>
#include <string_view>

namespace modernime::settings {

enum class SettingsPageId;

inline constexpr std::string_view kSettingsWindowClass = "modernime-settings";
inline constexpr std::string_view kSettingsSidebarClass = "modernime-sidebar";
inline constexpr std::string_view kSettingsSectionClass = "modernime-section";
inline constexpr std::string_view kSettingsPrimaryButtonClass =
    "suggested-action";
inline constexpr std::string_view kSettingsDangerButtonClass =
    "destructive-action";
inline constexpr std::string_view kSettingsFocusFallbackClass =
    "modernime-focus-fallback";
inline constexpr std::string_view kSettingsShortcutButtonClass =
    "modernime-shortcut-btn";

void installSettingsStyles();
void bindSettingsFocusVisibility(GtkWindow *window);
GtkWidget *createSettingsPopover(GtkWidget *relativeTo);
void applySettingsTheme(std::string_view theme);
GtkWidget *createSettingsChoice();
void appendSettingsChoice(GtkWidget *choice, std::string_view text);
void appendSettingsPaletteChoice(GtkWidget *choice, std::string_view text,
                                 const std::array<GdkRGBA, 3> &colors);
std::array<GdkRGBA, 3> settingsThemeColors(std::string_view theme);
int settingsChoiceActive(GtkWidget *choice);
void setSettingsChoiceActive(GtkWidget *choice, int index);
enum class SettingsStatusArt { Release, Input };
GtkWidget *createSettingsStatusArt(SettingsStatusArt kind);
GtkWidget *createSettingsPageIcon(SettingsPageId page, int size = 16);
std::string_view settingsStyles();
void setAccessibleWidgetText(GtkWidget *widget, std::string_view name,
                             std::string_view description);
void setSettingsFocusChain(
    GtkWidget *container,
    std::initializer_list<GtkWidget *> focusableWidgets);
void prependSettingsFocusChainChild(GtkWidget *container, GtkWidget *child);
void setDialogResponseAccessibility(GtkDialog *dialog, int response,
                                    std::string_view name,
                                    std::string_view description);
GtkWidget *createPageShell(std::string_view title, std::string_view subtitle,
                           GtkWidget *headerAction = nullptr);
void setPageHeaderAction(GtkWidget *page, GtkWidget *action);
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
