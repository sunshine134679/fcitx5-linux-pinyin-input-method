#include "modernime/settings/page_registry.h"
#include "modernime/settings/settings_window.h"
#include "modernime/settings/detail/process_runner.h"

#include <gtk/gtk.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct ApplicationState final {
    explicit ApplicationState(modernime::core::SettingsPaths paths)
        : settingsPaths(std::move(paths)) {}

    modernime::core::SettingsPaths settingsPaths;
    std::unique_ptr<modernime::settings::SettingsWindow> window;
};

struct NavigationRequest final {
    std::string pageName;
    std::string target;
    bool hasTarget = false;
    bool showVersion = false;
};

NavigationRequest parseArguments(int argc, char **argv) {
    NavigationRequest req;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--version") {
            req.showVersion = true;
        } else if (arg == "--page" && i + 1 < argc) {
            req.pageName = argv[++i];
        } else if (arg == "--target" && i + 1 < argc) {
            req.target = argv[++i];
            req.hasTarget = true;
        } else if (arg == "--overview" || arg == "-o") {
            req.pageName = "overview";
        } else if (arg == "--input" || arg == "-i") {
            req.pageName = "input";
        } else if (arg == "--candidates" || arg == "-k") {
            req.pageName = "candidates";
        } else if (arg == "--dictionary" || arg == "-d") {
            req.pageName = "dictionary";
        } else if (arg == "--clipboard" || arg == "-c") {
            req.pageName = "clipboard";
        } else if (arg == "--learning" || arg == "-l") {
            req.pageName = "learning";
        } else if (arg == "--updates" || arg == "-u") {
            req.pageName = "updates";
        } else if (arg == "--diagnostics" || arg == "--status") {
            req.pageName = "diagnostics";
        }
    }
    return req;
}

int applyNavigationRequest(ApplicationState *state,
                           const NavigationRequest &req,
                           GApplicationCommandLine *cmdline = nullptr) {
    if (req.showVersion) {
        if (cmdline != nullptr) {
            g_application_command_line_print(
                cmdline, "ModernIME settings %s\n", MODERNIME_VERSION);
        } else {
            std::cout << "ModernIME settings " << MODERNIME_VERSION << '\n';
        }
        return EXIT_SUCCESS;
    }

    if (state->window == nullptr) {
        return EXIT_FAILURE;
    }
    state->window->present();

    if (req.pageName.empty()) {
        if (req.hasTarget) {
            state->window->show(modernime::settings::SettingsPageId::Overview, req.target);
        }
        return EXIT_SUCCESS;
    }

    const auto parsedPage = modernime::settings::parseSettingsPage(req.pageName);
    if (!parsedPage.has_value()) {
        const std::string err = "Unknown settings page: " + req.pageName;
        if (cmdline != nullptr) {
            g_application_command_line_printerr(cmdline, "%s\n", err.c_str());
            g_application_command_line_set_exit_status(cmdline, EXIT_FAILURE);
        } else {
            std::cerr << err << '\n';
        }
        state->window->presentError(err);
        return EXIT_FAILURE;
    }

    state->window->show(*parsedPage, req.target);
    return EXIT_SUCCESS;
}

void activate(GtkApplication *application, gpointer data) {
    auto *state = static_cast<ApplicationState *>(data);
    if (state->window == nullptr) {
        state->window = std::make_unique<modernime::settings::SettingsWindow>(
            application, state->settingsPaths);
    }
    state->window->present();
}

int commandLine(GApplication *application,
                GApplicationCommandLine *cmdline,
                gpointer data) {
    auto *state = static_cast<ApplicationState *>(data);
    if (state->window == nullptr) {
        state->window = std::make_unique<modernime::settings::SettingsWindow>(
            application, state->settingsPaths);
    }

    int argc = 0;
    char **argv = g_application_command_line_get_arguments(cmdline, &argc);
    const auto req = parseArguments(argc, argv);
    const int status = applyNavigationRequest(state, req, cmdline);
    g_strfreev(argv);
    return status;
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
    if (argc >= 2 && std::string_view(argv[1]) == "--internal-capture-collector") {
        return modernime::settings::detail::runCommandCaptureChild(argc, argv);
    }

    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "ModernIME settings " << MODERNIME_VERSION << '\n';
        return EXIT_SUCCESS;
    }

    const auto paths = settingsPaths();
    ApplicationState state(paths);
#if GLIB_CHECK_VERSION(2, 74, 0)
    constexpr auto appFlags = static_cast<GApplicationFlags>(
        G_APPLICATION_DEFAULT_FLAGS | G_APPLICATION_HANDLES_COMMAND_LINE);
#else
    constexpr auto appFlags = static_cast<GApplicationFlags>(
        G_APPLICATION_FLAGS_NONE | G_APPLICATION_HANDLES_COMMAND_LINE);
#endif
    auto *application = gtk_application_new("com.modernime.Settings", appFlags);
    g_signal_connect(application, "activate", G_CALLBACK(activate), &state);
    g_signal_connect(application, "command-line", G_CALLBACK(commandLine), &state);
    const auto result = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return result;
}
