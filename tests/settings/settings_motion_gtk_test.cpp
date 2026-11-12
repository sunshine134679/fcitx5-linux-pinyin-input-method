#include "modernime/settings/settings_shell.h"
#include "modernime/settings/settings_widgets.h"
#include "modernime/core/settings.h"
#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

namespace {
GtkWidget *find(GtkWidget *widget, const std::string &key) {
    const auto *target = static_cast<const char *>(g_object_get_data(G_OBJECT(widget), "modernime-settings-target"));
    const auto *name = atk_object_get_name(gtk_widget_get_accessible(widget));
    if ((target && key == target) || (name && key == name)) return widget;
    if (!GTK_IS_CONTAINER(widget)) return nullptr;
    auto *children = gtk_container_get_children(GTK_CONTAINER(widget));
    GtkWidget *result = nullptr;
    for (auto *item = children; item && !result; item = item->next) result = find(GTK_WIDGET(item->data), key);
    g_list_free(children);
    return result;
}
template<class F> bool wait(F predicate, int milliseconds = 2000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    do {
        while (g_main_context_iteration(nullptr, FALSE)) {}
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < end);
    return false;
}
void frames(int milliseconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    assert(wait([&] { return std::chrono::steady_clock::now() >= end; }, milliseconds + 500));
}
void pointerClick(GtkWidget *widget) {
    assert(GTK_IS_BUTTON(widget));
#ifdef GDK_WINDOWING_X11
    auto *display = gtk_widget_get_display(widget);
    auto *xdotool = g_find_program_in_path("xdotool");
    if (GDK_IS_X11_DISPLAY(display) && xdotool) {
        auto *window = gtk_widget_get_toplevel(widget);
        int x = 0, y = 0, rootX = 0, rootY = 0;
        assert(gtk_widget_translate_coordinates(widget, window,
            gtk_widget_get_allocated_width(widget) / 2,
            gtk_widget_get_allocated_height(widget) / 2, &x, &y));
        gdk_window_get_origin(gtk_widget_get_window(window), &rootX, &rootY);
        auto *pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(display));
        gdk_device_warp(pointer, gtk_widget_get_screen(widget), rootX + x, rootY + y);
        frames(40);
        // XTest follows native input routing; widget signals and XSendEvent do not.
        char *arguments[]{xdotool, const_cast<char *>("click"), const_cast<char *>("1"), nullptr};
        int status = -1;
        GError *error = nullptr;
        assert(g_spawn_sync(nullptr, arguments, nullptr, G_SPAWN_DEFAULT, nullptr,
                            nullptr, nullptr, nullptr, &status, &error));
        assert(!error && g_spawn_check_wait_status(status, &error));
        g_free(xdotool);
        frames(50);
        return;
    }
    g_free(xdotool);
#endif
    static bool reported = false;
    if (!reported) std::cout << "Native pointer routing check not run: requires X11 and xdotool\n";
    reported = true;
    gtk_button_clicked(GTK_BUTTON(widget));
}
} // namespace

int main(int argc, char **argv) {
    if (!gtk_init_check(&argc, &argv)) return 77;
    using namespace modernime;
    const std::filesystem::path captures = argc > 1 ? argv[1] : "";
    if (!captures.empty()) std::filesystem::create_directories(captures);
    std::string pattern = "/tmp/modernime-motion-XXXXXX";
    assert(mkdtemp(pattern.data()));
    const std::filesystem::path root(pattern);
    std::filesystem::create_directories(root / "config");
    std::filesystem::create_directories(root / "data");
    const core::SettingsPaths paths{root / "config/settings.conf", root / "data/user.dict",
        root / "data/learning.db", root / "data/clipboard.history", root / "config/runtime.state"};
    auto *app = gtk_application_new(nullptr, G_APPLICATION_NON_UNIQUE);
    GError *error = nullptr;
    assert(g_application_register(G_APPLICATION(app), nullptr, &error));
    auto *gtkSettings = gtk_settings_get_default();
    gboolean originalAnimations = FALSE;
    g_object_get(gtkSettings, "gtk-enable-animations", &originalAnimations, nullptr);
    g_object_set(gtkSettings, "gtk-enable-animations", TRUE, nullptr);
    {
        settings::SettingsShell shell(app, paths);
        auto *window = shell.window();
        shell.present();
        shell.show(settings::SettingsPageId::Candidates);
        assert(wait([&] { return gtk_widget_get_mapped(window); }));
        frames(200);
        auto *theme = find(window, "client-theme");
        auto *candidate = find(window, "candidate-theme");
        auto *shape = find(window, "candidate-shape");
        auto *length = find(window, "candidate-page-size");
        auto *spacing = find(window, "candidate-spacing");
        auto *restore = find(window, "放弃修改");
        auto *save = find(window, "保存设置");
        assert(theme && candidate && shape && length && spacing && restore && save);
        auto *stack = gtk_widget_get_ancestor(theme, GTK_TYPE_STACK);
        assert(stack && wait([&] { return !gtk_stack_get_transition_running(GTK_STACK(stack)); }));
        pointerClick(find(window, "输入与按键"));
        assert(std::string(gtk_stack_get_visible_child_name(GTK_STACK(stack))) == "input");
        pointerClick(find(window, "候选与外观"));
        assert(std::string(gtk_stack_get_visible_child_name(GTK_STACK(stack))) == "candidates");
        assert(wait([&] { return !gtk_stack_get_transition_running(GTK_STACK(stack)); }));
        frames(30);
        const auto revealing = [&] { return gtk_style_context_has_class(
            gtk_widget_get_style_context(window), "modernime-theme-reveal"); };
        const auto record = [&](const std::string &name) {
            if (captures.empty()) return;
            auto *pixbuf = gdk_pixbuf_get_from_window(gtk_widget_get_window(window), 0, 0,
                gtk_widget_get_allocated_width(window), gtk_widget_get_allocated_height(window));
            assert(pixbuf);
            assert(gdk_pixbuf_save(pixbuf, (captures / (name + ".png")).c_str(), "png", &error, nullptr));
            assert(!error);
            g_object_unref(pixbuf);
        };
        record("blue");
        const int originalCandidate = settings::settingsChoiceActive(candidate);
        // Exercise the actual menu selection boundary, including popover cleanup.
        pointerClick(theme);
        assert(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(theme)));
        frames(160);
        auto *popover = GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(theme)));
        auto *jadeOption = find(popover, "青竹绿");
        assert(GTK_IS_RADIO_BUTTON(jadeOption));
        pointerClick(jadeOption);
        assert(!gtk_widget_get_visible(popover));
        assert(revealing());
        frames(50);
        record("主题揭幕-中间帧");
        settings::setSettingsChoiceActive(theme, 2); // Retarget, never queue old choices.
        assert(revealing());
        assert(wait([&] { return !revealing(); }));
        assert(settings::settingsChoiceActive(candidate) == originalCandidate);
        pointerClick(restore);
        assert(settings::settingsChoiceActive(theme) == 0 && revealing());
        assert(wait([&] { return !revealing(); }));
        for (std::size_t i = 1; i < core::kClientThemes.size(); ++i) {
            settings::setSettingsChoiceActive(theme, static_cast<int>(i));
            assert(revealing());
            if (core::kClientThemes[i].key == "contrast" && !captures.empty()) {
                frames(70);
                record("高对比-切入中间帧");
            }
            assert(wait([&] { return !revealing(); }));
            frames(30);
            record(std::string(core::kClientThemes[i].key));
            assert(settings::settingsChoiceActive(candidate) == originalCandidate);
        }
        settings::setSettingsChoiceActive(theme, 2);
        assert(revealing());
        if (!captures.empty()) { frames(70); record("高对比-切出中间帧"); }
        assert(wait([&] { return !revealing(); }));
        for (std::size_t i = 0; i < core::kCandidateThemes.size(); ++i) {
            settings::setSettingsChoiceActive(candidate, static_cast<int>(i));
            frames(230);
            assert(settings::settingsChoiceActive(theme) == 2);
        }
        settings::setSettingsChoiceActive(candidate, 0);
        frames(230);
        record("深海夜界面-浅色候选栏");
        settings::setSettingsChoiceActive(shape, 2);
        settings::setSettingsChoiceActive(length, 0);
        frames(40);
        settings::setSettingsChoiceActive(length, 6);
        settings::setSettingsChoiceActive(spacing, 2);
        frames(250);
        settings::setSettingsChoiceActive(theme, 1);
        assert(revealing());
        g_object_set(gtkSettings, "gtk-enable-animations", FALSE, nullptr);
        assert(!revealing());
        settings::setSettingsChoiceActive(theme, 6);
        assert(!revealing());
        settings::setSettingsChoiceActive(theme, 5);
        settings::setSettingsChoiceActive(shape, 1);
        assert(!revealing());
        frames(30);
        g_object_set(gtkSettings, "gtk-enable-animations", TRUE, nullptr);
        shell.show(settings::SettingsPageId::Input);
        frames(200);
        assert(GTK_IS_SWITCH(find(window, "input-enabled")));
        shell.show(settings::SettingsPageId::Candidates, "candidate-font-size");
        auto *font = find(window, "candidate-font-size");
        assert(wait([&] {
            auto *focus = gtk_window_get_focus(GTK_WINDOW(window));
            return focus == font || (focus && gtk_widget_is_ancestor(focus, font));
        }));
        frames(270);
        record("搜索定位");
        pointerClick(save);
        const auto saved = core::SettingsStore::load(paths.settingsFile);
        assert(saved.isSuccess() && saved.settings.clientTheme == "lavender");
        assert(saved.settings.candidateTheme == core::CandidateTheme::Light);
        assert(!gtk_widget_get_sensitive(save) && find(window, "✓ 所有设置已保存"));
        shell.show(settings::SettingsPageId::Updates, "updates-copy-command");
        frames(250);
        auto *checkUpdate = find(window, "updates-check");
        assert(checkUpdate && wait([&] { return gtk_widget_get_sensitive(checkUpdate); }));
        gtk_button_clicked(GTK_BUTTON(find(window, "更新与安装")));
        assert(gtk_widget_get_sensitive(checkUpdate));
        shell.show(settings::SettingsPageId::Diagnostics);
        frames(250);
        shell.show(settings::SettingsPageId::Candidates);
        frames(180);
        settings::setSettingsChoiceActive(theme, 2);
        assert(revealing());
        gtk_widget_hide(window);
        assert(!revealing());
    }
    frames(100); // Late callbacks must not access the destroyed shell.
    g_object_set(gtkSettings, "gtk-enable-animations", originalAnimations, nullptr);
    g_object_unref(app);
    std::filesystem::remove_all(root);
    std::cout << "motion, theme independence, discard/save and teardown passed\n";
}
