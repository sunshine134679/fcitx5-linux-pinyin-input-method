#include "modernime/settings/settings_shell.h"
#include "modernime/settings/settings_widgets.h"
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
    ::setenv("MODERNIME_TEST_ALLOW_MOCK_INSTANCE", "1", 1);
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
        assert(waitUntil([window] { return hasLabel(window, "ModernIME 正在运行"); }));
        std::ofstream(slowRemote) << "delay\n";
        const bool initial = gtk_switch_get_active(GTK_SWITCH(enabled));
        gtk_switch_set_active(GTK_SWITCH(enabled), !initial);
        saveWithoutBlocking(save);
        const auto firstHash = modernime::core::computeFileContentHash(paths.settingsFile);
        std::ofstream(paths.runtimeState) << "applied_content_hash=" << firstHash << "\npid=" << getpid() << '\n';

        // A second save while the first probe runs must report the second hash.
        gtk_switch_set_active(GTK_SWITCH(enabled), initial);
        saveWithoutBlocking(save);
        assert(modernime::core::computeFileContentHash(paths.settingsFile) != firstHash);
        assert(waitUntil([window] { return hasLabel(window, "已保存，等待输入法读取"); }));

        gtk_switch_set_active(GTK_SWITCH(enabled), !initial);
        saveWithoutBlocking(save);
        assert(modernime::core::computeFileContentHash(paths.settingsFile) == firstHash);
        assert(waitUntil([window] { return hasLabel(window, "已保存并应用"); }));
        gtk_switch_set_active(GTK_SWITCH(enabled), true);
        auto *scheme = findWidget(window, [](GtkWidget *widget) {
            const auto *target = static_cast<const char *>(g_object_get_data(G_OBJECT(widget), "modernime-settings-target"));
            return target && std::string_view(target) == "pinyin.scheme";
        });
        assert(scheme && GTK_IS_MENU_BUTTON(scheme));
        modernime::settings::setSettingsChoiceActive(scheme, 1);
        for (const auto &option : modernime::core::kFuzzyPinyinOptions) {
            auto *control = findWidget(window, [&](GtkWidget *widget) {
                const auto *target = static_cast<const char *>(g_object_get_data(
                    G_OBJECT(widget), "modernime-settings-target"));
                return target && std::string_view(target) == option.key;
            });
            assert(control && GTK_IS_SWITCH(control) && gtk_widget_get_sensitive(control));
            assert(gtk_switch_get_active(GTK_SWITCH(control)));
            gtk_switch_set_active(GTK_SWITCH(control), false);
        }
        const auto targetWidget = [&](std::string_view name) {
            return findWidget(window, [&](GtkWidget *widget) {
                const auto *target = static_cast<const char *>(g_object_get_data(G_OBJECT(widget), "modernime-settings-target"));
                return target && name == target;
            });
        };
        auto *font = targetWidget("candidate-font-family");
        auto *theme = targetWidget("candidate-theme");
        auto *spacing = targetWidget("candidate-spacing");
        auto *clientTheme = targetWidget("client-theme");
        auto *shape = targetWidget("candidate-shape");
        assert(font && theme && spacing && clientTheme && shape);
        auto *description = pango_font_description_from_string("DejaVu Sans 20");
        gtk_font_chooser_set_font_desc(GTK_FONT_CHOOSER(font), description);
        pango_font_description_free(description);
        g_signal_emit_by_name(font, "font-set");
        modernime::settings::setSettingsChoiceActive(theme, 1);
        modernime::settings::setSettingsChoiceActive(spacing, 0);
        modernime::settings::setSettingsChoiceActive(clientTheme, 2);
        saveWithoutBlocking(save);
        const auto saved = modernime::core::SettingsStore::load(paths.settingsFile);
        assert(saved.isSuccess());
        assert(saved.settings.clientTheme == "night");
        assert(saved.settings.candidateFontFamily == "DejaVu Sans");
        assert(saved.settings.candidateTheme == modernime::core::CandidateTheme::Dark);
        assert(saved.settings.candidateSpacing == modernime::core::CandidateSpacing::Compact);
        assert(saved.settings.pinyinScheme == modernime::core::PinyinScheme::Xiaohe);
        for (const auto &option : modernime::core::kFuzzyPinyinOptions)
            assert(!(saved.settings.fuzzyPinyin.*option.member));
        // Select a swatch through the actual palette row, not only its public setter.
        auto *paletteRow = findWidget(GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(theme))),
            [](GtkWidget *widget) {
                return GTK_IS_RADIO_BUTTON(widget) && GPOINTER_TO_INT(
                    g_object_get_data(G_OBJECT(widget), "modernime-choice-index")) == 5;
            });
        assert(paletteRow);
        gtk_button_clicked(GTK_BUTTON(paletteRow));
        modernime::settings::setSettingsChoiceActive(shape, 2);
        modernime::settings::setSettingsChoiceActive(clientTheme, 4);
        saveWithoutBlocking(save);
        const auto preset = modernime::core::SettingsStore::load(paths.settingsFile);
        assert(preset.isSuccess() && preset.settings.clientTheme == "rose");
        assert(preset.settings.candidateTheme == modernime::core::CandidateTheme::Rose);
        assert(preset.settings.candidateShape == modernime::core::CandidateShape::Capsule);

        // Exercise the real native chooser: Chinese coverage filtering, the
        // existing search, viewing all fonts, and cancel preserving settings.
        shell.present();
        shell.show(modernime::settings::SettingsPageId::Candidates);
        assert(waitUntil([window] { return gtk_widget_get_mapped(window); }));
        gtk_button_clicked(GTK_BUTTON(font));
        GtkWidget *fontDialog = nullptr;
        auto *windows = gtk_window_list_toplevels();
        for (auto *item = windows; item; item = item->next)
            if (GTK_IS_FONT_CHOOSER_DIALOG(item->data) && gtk_widget_get_visible(GTK_WIDGET(item->data)))
                fontDialog = GTK_WIDGET(item->data);
        g_list_free(windows);
        assert(fontDialog);
        auto *onlyChinese = findWidget(fontDialog, [](GtkWidget *widget) {
            const auto *id = static_cast<const char *>(g_object_get_data(G_OBJECT(widget), "modernime-settings-target"));
            return id && std::string_view(id) == "candidate-font-chinese-only";
        });
        assert(onlyChinese);
        assert(waitUntil([onlyChinese] { return gtk_widget_get_sensitive(onlyChinese); }));
        auto *fontList = findWidget(fontDialog, [](GtkWidget *widget) { return GTK_IS_TREE_VIEW(widget); });
        auto *fontSearch = findWidget(fontDialog, [](GtkWidget *widget) { return GTK_IS_SEARCH_ENTRY(widget); });
        assert(fontList && fontSearch);
        PangoFontFamily **families = nullptr;
        int familyCount = 0;
        pango_context_list_families(gtk_widget_get_pango_context(font), &families, &familyCount);
        bool hasDejaVu = false;
        for (int i = 0; i < familyCount; ++i)
            hasDejaVu |= std::string_view(pango_font_family_get_name(families[i])) == "DejaVu Sans";
        g_free(families);
        if (hasDejaVu) {
            assert(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(onlyChinese)));
            gtk_entry_set_text(GTK_ENTRY(fontSearch), "DejaVu Sans");
            assert(waitUntil([fontList] {
                return gtk_tree_model_iter_n_children(gtk_tree_view_get_model(GTK_TREE_VIEW(fontList)), nullptr) == 0;
            }));
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(onlyChinese), FALSE);
            assert(waitUntil([fontList] {
                return gtk_tree_model_iter_n_children(gtk_tree_view_get_model(GTK_TREE_VIEW(fontList)), nullptr) > 0;
            }));
        }
        gtk_dialog_response(GTK_DIALOG(fontDialog), GTK_RESPONSE_CANCEL);
        assert(!gtk_widget_get_visible(fontDialog));
        assert(!gtk_widget_get_sensitive(save));
        assert(modernime::core::SettingsStore::load(paths.settingsFile).settings.candidateFontFamily == "DejaVu Sans");
    }
    g_object_unref(application);
    std::filesystem::remove_all(root);
}
