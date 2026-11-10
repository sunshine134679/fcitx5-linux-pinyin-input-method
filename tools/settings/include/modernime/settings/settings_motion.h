#pragma once

#include <gtk/gtk.h>

#include <memory>
#include <string_view>

namespace modernime::settings {

// Presentation only: the model and native controls keep owning their state.
class SettingsMotion final {
public:
    SettingsMotion(GtkWidget *window, GtkWidget *overlay, GtkWidget *content,
                   GtkWidget *navigation, GtkWidget *editStatus, GtkWidget *taskStatus);
    ~SettingsMotion();
    SettingsMotion(const SettingsMotion &) = delete;
    SettingsMotion &operator=(const SettingsMotion &) = delete;

    void changeTheme(std::string_view previous, std::string_view next,
                     GtkWidget *origin, unsigned duration = 440);
    void finishTheme();
    void selectNavigation(GtkWidget *button);
    void hintFocus(GtkWidget *target);
    void cancelHint();
    void transitionStatus(unsigned duration);
    void showMessage();
    void hideMessage();
    void finish();
    bool animating() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace modernime::settings
