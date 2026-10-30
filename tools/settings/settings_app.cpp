#include "modernime/settings/page_registry.h"
#include "modernime/settings/settings_window.h"
#include "modernime/settings/update_controller.h"
#include "modernime/settings/detail/process_runner.h"

#include <gtk/gtk.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

struct ApplicationState final {
    explicit ApplicationState(modernime::core::SettingsPaths paths)
        : settingsPaths(std::move(paths)) {}

    modernime::core::SettingsPaths settingsPaths;
    std::unique_ptr<modernime::settings::SettingsWindow> window;
    uint64_t sequenceNumber = 0;
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

struct WidgetTypeSearch final {
    GType type;
    GtkWidget *result = nullptr;
};

GtkWidget *findWidgetByType(GtkWidget *widget, GType type);

void findWidgetByTypeChild(GtkWidget *child, gpointer data) {
    auto *search = static_cast<WidgetTypeSearch *>(data);
    if (search->result == nullptr) {
        search->result = findWidgetByType(child, search->type);
    }
}

GtkWidget *findWidgetByType(GtkWidget *widget, GType type) {
    if (widget == nullptr) {
        return nullptr;
    }
    if (g_type_is_a(G_OBJECT_TYPE(widget), type)) {
        return widget;
    }
    if (!GTK_IS_CONTAINER(widget)) {
        return nullptr;
    }
    WidgetTypeSearch search{type};
    gtk_container_forall(GTK_CONTAINER(widget), findWidgetByTypeChild, &search);
    return search.result;
}

const char *findTargetInWidget(GtkWidget *widget) {
    if (widget == nullptr) {
        return nullptr;
    }
    const void *data =
        g_object_get_data(G_OBJECT(widget), "modernime-settings-target");
    if (data != nullptr) {
        return static_cast<const char *>(data);
    }
    if (GTK_IS_CONTAINER(widget)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
        const char *result = nullptr;
        for (GList *l = children; l != nullptr; l = l->next) {
            result = findTargetInWidget(GTK_WIDGET(l->data));
            if (result != nullptr) {
                break;
            }
        }
        g_list_free(children);
        return result;
    }
    return nullptr;
}

void dumpSettingsStateIfConfigured(ApplicationState *state) {
    const char *stateFile = std::getenv("MODERNIME_SETTINGS_STATE_FILE");
    if (stateFile == nullptr || stateFile[0] == '\0') {
        return;
    }
    if (state == nullptr || state->window == nullptr) {
        return;
    }
    GtkWidget *win = state->window->window();
    if (win == nullptr) {
        return;
    }

    while (g_main_context_iteration(nullptr, FALSE)) {}

    std::string visiblePage;
    GtkWidget *stack = findWidgetByType(win, GTK_TYPE_STACK);
    if (stack != nullptr) {
        const char *name = gtk_stack_get_visible_child_name(GTK_STACK(stack));
        if (name != nullptr) {
            visiblePage = name;
        }
    }

    std::string focusedTarget;
    GtkWidget *f = gtk_window_get_focus(GTK_WINDOW(win));
    if (f != nullptr) {
        for (GtkWidget *w = f; w != nullptr; w = gtk_widget_get_parent(w)) {
            const void *targetData =
                g_object_get_data(G_OBJECT(w), "modernime-settings-target");
            if (targetData != nullptr) {
                focusedTarget = static_cast<const char *>(targetData);
                break;
            }
        }
        if (focusedTarget.empty()) {
            const char *targetData = findTargetInWidget(f);
            if (targetData != nullptr) {
                focusedTarget = targetData;
            }
        }
    }

    int windowCount = 1;
    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(win));
    if (app != nullptr) {
        GList *windows = gtk_application_get_windows(app);
        windowCount = static_cast<int>(g_list_length(windows));
    }

    state->sequenceNumber++;

    const std::string tmpFile = std::string(stateFile) + ".tmp";
    std::ofstream ofs(tmpFile);
    if (ofs) {
        ofs << "{\n"
            << "  \"pid\": " << getpid() << ",\n"
            << "  \"sequence\": " << state->sequenceNumber << ",\n"
            << "  \"window_count\": " << windowCount << ",\n"
            << "  \"visible_page\": \"" << visiblePage << "\",\n"
            << "  \"focused_target\": \"" << focusedTarget << "\",\n"
            << "  \"ready\": true\n"
            << "}\n";
        ofs.close();
        std::rename(tmpFile.c_str(), stateFile);
    }
}

void activate(GtkApplication *application, gpointer data) {
    modernime::settings::UpdateController::ensureDesktopShortcut();
    auto *state = static_cast<ApplicationState *>(data);
    if (state->window == nullptr) {
        state->window = std::make_unique<modernime::settings::SettingsWindow>(
            application, state->settingsPaths);
    }
    state->window->present();
    dumpSettingsStateIfConfigured(state);
}

int commandLine(GApplication *application,
                GApplicationCommandLine *cmdline,
                gpointer data) {
    auto *state = static_cast<ApplicationState *>(data);

    int argc = 0;
    char **argv = g_application_command_line_get_arguments(cmdline, &argc);
    const auto req = parseArguments(argc, argv);
    g_strfreev(argv);

    if (req.showVersion) {
        g_application_command_line_print(
            cmdline, "ModernIME settings %s\n", MODERNIME_VERSION);
        return EXIT_SUCCESS;
    }

    modernime::settings::UpdateController::ensureDesktopShortcut();

    std::optional<modernime::settings::SettingsPageId> parsedPage;
    if (!req.pageName.empty()) {
        parsedPage = modernime::settings::parseSettingsPage(req.pageName);
        if (!parsedPage.has_value()) {
            const std::string err = "Unknown settings page: " + req.pageName;
            g_application_command_line_printerr(cmdline, "%s\n", err.c_str());
            g_application_command_line_set_exit_status(cmdline, EXIT_FAILURE);
            if (state->window != nullptr) {
                dumpSettingsStateIfConfigured(state);
            }
            return EXIT_FAILURE;
        }
    }

    if (state->window == nullptr) {
        state->window = std::make_unique<modernime::settings::SettingsWindow>(
            application, state->settingsPaths);
    }
    state->window->present();

    if (parsedPage.has_value()) {
        state->window->show(*parsedPage, req.target);
    } else if (req.hasTarget) {
        state->window->show(modernime::settings::SettingsPageId::Overview, req.target);
    }

    dumpSettingsStateIfConfigured(state);

    return EXIT_SUCCESS;
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
