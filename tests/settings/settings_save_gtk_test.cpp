#include "modernime/settings/settings_shell.h"
#include "modernime/core/settings.h"

#include <gtk/gtk.h>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace {

template <typename Predicate>
GtkWidget *findWidget(GtkWidget *widget, const Predicate &predicate) {
    if (predicate(widget)) {
        return widget;
    }
    if (!GTK_IS_CONTAINER(widget)) {
        return nullptr;
    }
    auto *children = gtk_container_get_children(GTK_CONTAINER(widget));
    GtkWidget *result = nullptr;
    for (auto *child = children; child != nullptr && result == nullptr;
         child = child->next) {
        result = findWidget(GTK_WIDGET(child->data), predicate);
    }
    g_list_free(children);
    return result;
}

template <typename Predicate>
bool waitUntil(const Predicate &predicate) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    do {
        while (g_main_context_iteration(nullptr, FALSE)) {}
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

bool hasLabel(GtkWidget *window, std::string_view expected) {
    return findWidget(window, [expected](GtkWidget *widget) {
        return GTK_IS_LABEL(widget) &&
               expected == gtk_label_get_text(GTK_LABEL(widget));
    }) != nullptr;
}

void saveWithoutBlocking(GtkWidget *save) {
    assert(gtk_widget_get_sensitive(save));
    const auto started = std::chrono::steady_clock::now();
    gtk_button_clicked(GTK_BUTTON(save));
    assert(std::chrono::steady_clock::now() - started <
           std::chrono::milliseconds(250));
    assert(!gtk_widget_get_sensitive(save));
}

} // namespace

int main(int argc, char **argv) {
    if (!gtk_init_check(&argc, &argv)) {
        return 77;
    }
    const auto root = std::filesystem::temp_directory_path() /
                      ("modernime-settings-save-gtk-" +
                       std::to_string(static_cast<long long>(getpid())));
    std::filesystem::create_directories(root / "bin");
    const auto remote = root / "bin/fcitx5-remote";
    const auto slowRemote = root / "slow-remote";
    {
        std::ofstream script(remote);
        script << "#!/bin/sh\n"
                  "if [ -e \"" << slowRemote.string()
               << "\" ]; then sleep 0.35; fi\n"
                  "case \"$1\" in\n"
                  "  -n|-m) printf 'modernime\\n';;\n"
                  "  *) printf '2\\n';;\n"
                  "esac\n";
    }
    std::filesystem::permissions(remote, std::filesystem::perms::owner_all);
    const auto *oldPath = std::getenv("PATH");
    const auto path = (root / "bin").string() + ":" +
                      (oldPath == nullptr ? "" : oldPath);
    assert(g_setenv("PATH", path.c_str(), TRUE));
    auto *application = gtk_application_new(
        nullptr, G_APPLICATION_NON_UNIQUE);
    GError *error = nullptr;
    assert(g_application_register(G_APPLICATION(application), nullptr, &error));
    assert(error == nullptr);
    const modernime::core::SettingsPaths paths{
        root / "config/settings.conf", root / "data/user.dict",
        root / "data/learning.db", root / "data/clipboard.history",
        root / "config/runtime.state"};
    {
        modernime::settings::SettingsShell shell(application, paths);
        auto *window = shell.window();
        auto *enabled = findWidget(window, [](GtkWidget *widget) {
            const auto *target = static_cast<const char *>(g_object_get_data(
                G_OBJECT(widget), "modernime-settings-target"));
            return target != nullptr && std::string_view(target) == "input-enabled";
        });
        auto *save = findWidget(window, [](GtkWidget *widget) {
            if (!GTK_IS_BUTTON(widget)) {
                return false;
            }
            const auto *name = atk_object_get_name(gtk_widget_get_accessible(widget));
            return name != nullptr && std::string_view(name) == "保存设置";
        });
        assert(enabled != nullptr && save != nullptr);
        assert(waitUntil([window] { return hasLabel(window, "● ModernIME 正在运行"); }));
        std::ofstream(slowRemote) << "delay\n";
        const bool initial = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(enabled));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(enabled), !initial);
        saveWithoutBlocking(save);
        const auto firstHash = modernime::core::computeFileContentHash(paths.settingsFile);
        std::ofstream(paths.runtimeState) << "applied_content_hash=" << firstHash << '\n';

        // A second save while the first probe runs must report the second hash.
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(enabled), initial);
        saveWithoutBlocking(save);
        assert(modernime::core::computeFileContentHash(paths.settingsFile) != firstHash);
        assert(waitUntil([window] { return hasLabel(window, "已保存，等待输入法读取"); }));

        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(enabled), !initial);
        saveWithoutBlocking(save);
        assert(modernime::core::computeFileContentHash(paths.settingsFile) == firstHash);
        assert(waitUntil([window] { return hasLabel(window, "已保存并应用"); }));
    }
    g_object_unref(application);
    std::filesystem::remove_all(root);
}
