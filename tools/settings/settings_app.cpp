#include "modernime/settings/settings_window.h"

#include <gtk/gtk.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct ApplicationState final {
    explicit ApplicationState(modernime::core::SettingsPaths paths,
                             std::string targetPage = {})
        : settingsPaths(std::move(paths)), initialPage(std::move(targetPage)) {}

    modernime::core::SettingsPaths settingsPaths;
    std::string initialPage;
    std::unique_ptr<modernime::settings::SettingsWindow> window;
};

void activate(GtkApplication *application, gpointer data) {
    auto *state = static_cast<ApplicationState *>(data);
    if (state->window == nullptr) {
        state->window = std::make_unique<modernime::settings::SettingsWindow>(
            application, state->settingsPaths);
    }
    state->window->present();
    if (state->initialPage == "clipboard") {
        state->window->showClipboardPage();
    } else if (state->initialPage == "learning") {
        state->window->showLearningPage();
    } else if (state->initialPage == "dictionary") {
        state->window->showDictionaryPage();
    } else if (state->initialPage == "input") {
        state->window->showBasicPage();
    } else if (state->initialPage == "diagnostics" ||
               state->initialPage == "status") {
        state->window->showStatusPage();
    }
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

    std::string targetPage;
    std::vector<char *> filteredArgs;
    filteredArgs.reserve(static_cast<std::size_t>(argc));
    filteredArgs.push_back(argv[0]);

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--page" && i + 1 < argc) {
            targetPage = argv[++i];
        } else if (arg == "--clipboard" || arg == "-c") {
            targetPage = "clipboard";
        } else if (arg == "--learning" || arg == "-l") {
            targetPage = "learning";
        } else if (arg == "--dictionary" || arg == "-d") {
            targetPage = "dictionary";
        } else if (arg == "--input" || arg == "-i") {
            targetPage = "input";
        } else if (arg == "--diagnostics") {
            targetPage = "diagnostics";
        } else {
            filteredArgs.push_back(argv[i]);
        }
    }

    const auto paths = settingsPaths();
    ApplicationState state(paths, targetPage);
    auto *application = gtk_application_new("com.modernime.Settings",
                                             G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(activate), &state);
    int filteredArgc = static_cast<int>(filteredArgs.size());
    const auto result = g_application_run(G_APPLICATION(application), filteredArgc,
                                           filteredArgs.data());
    g_object_unref(application);
    return result;
}
