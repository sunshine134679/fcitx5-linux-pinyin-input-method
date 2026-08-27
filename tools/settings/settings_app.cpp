#include "modernime/settings/settings_window.h"

#include <gtk/gtk.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>
#include <utility>

namespace {

struct ApplicationState final {
    explicit ApplicationState(modernime::core::SettingsPaths paths)
        : settingsPaths(std::move(paths)) {}

    modernime::core::SettingsPaths settingsPaths;
    std::unique_ptr<modernime::settings::SettingsWindow> window;
};

void activate(GtkApplication *application, gpointer data) {
    auto *state = static_cast<ApplicationState *>(data);
    if (state->window == nullptr) {
        state->window = std::make_unique<modernime::settings::SettingsWindow>(
            application, state->settingsPaths);
    }
    state->window->present();
}

modernime::core::SettingsPaths settingsPaths() {
    const auto *xdgConfigHome = std::getenv("XDG_CONFIG_HOME");
    const auto *xdgDataHome = std::getenv("XDG_DATA_HOME");
    const auto *home = std::getenv("HOME");
    return modernime::core::SettingsPaths::fromEnvironment(
        xdgConfigHome == nullptr ? std::string_view{}
                                 : std::string_view(xdgConfigHome),
        xdgDataHome == nullptr ? std::string_view{}
                               : std::string_view(xdgDataHome),
        home == nullptr ? std::string_view{} : std::string_view(home));
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "ModernIME settings " << MODERNIME_VERSION << '\n';
        return EXIT_SUCCESS;
    }

    const auto paths = settingsPaths();
    ApplicationState state(paths);
    auto *application = gtk_application_new("com.modernime.Settings",
                                             G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(activate), &state);
    const auto result = g_application_run(G_APPLICATION(application), argc,
                                           argv);
    g_object_unref(application);
    return result;
}
