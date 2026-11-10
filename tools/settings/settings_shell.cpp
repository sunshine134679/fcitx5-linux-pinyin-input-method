#include "modernime/settings/settings_shell.h"

#include "modernime/settings/detail/diagnostics_lifetime.h"
#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/overview_model.h"
#include "modernime/settings/pages/candidates_page.h"
#include "modernime/settings/pages/clipboard_page.h"
#include "modernime/settings/pages/diagnostics_page.h"
#include "modernime/settings/pages/dictionary_page.h"
#include "modernime/settings/pages/input_page.h"
#include "modernime/settings/pages/learning_page.h"
#include "modernime/settings/pages/overview_page.h"
#include "modernime/settings/pages/updates_page.h"
#include "modernime/settings/runtime_controller.h"
#include "modernime/settings/runtime_environment.h"
#include "modernime/settings/settings_model.h"
#include "modernime/settings/settings_motion.h"
#include "modernime/settings/settings_shell_state.h"
#include "modernime/settings/settings_ui_contract.h"
#include "modernime/settings/settings_widgets.h"
#include "modernime/settings/update_controller.h"

#include <gio/gio.h>
#include <gtk/gtk.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace modernime::settings {
namespace {

void addStyleClass(GtkWidget *widget, std::string_view className) {
    gtk_style_context_add_class(gtk_widget_get_style_context(widget),
                                std::string(className).c_str());
}

void removeStyleClass(GtkWidget *widget, std::string_view className) {
    gtk_style_context_remove_class(gtk_widget_get_style_context(widget),
                                   std::string(className).c_str());
}

Environment currentEnvironment() {
    std::filesystem::path fallback;
#ifdef MODERNIME_INSTALL_PREFIX
    fallback = MODERNIME_INSTALL_PREFIX;
#endif
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    const auto prefix = installationPrefix(error ? std::filesystem::path{} : executable,
                                           fallback);
    std::string systemAddonDirectory;
#ifdef MODERNIME_SYSTEM_FCITX5_ADDON_DIR
    systemAddonDirectory = MODERNIME_SYSTEM_FCITX5_ADDON_DIR;
#endif
    return runtimeEnvironment(prefix, systemAddonDirectory);
}

std::filesystem::path executablePath(std::string_view name) {
    auto *path = g_find_program_in_path(std::string(name).c_str());
    if (path == nullptr) {
        return std::filesystem::path(name);
    }
    const std::filesystem::path result(path);
    g_free(path);
    return result;
}

const SettingsPageDefinition &pageDefinition(SettingsPageId page) {
    for (const auto &definition : settingsPageDefinitions()) {
        if (definition.id == page) {
            return definition;
        }
    }
    return settingsPageDefinitions().front();
}

void clearContainer(GtkWidget *container) {
    auto *children = gtk_container_get_children(GTK_CONTAINER(container));
    for (auto *item = children; item != nullptr; item = item->next) {
        gtk_widget_destroy(GTK_WIDGET(item->data));
    }
    g_list_free(children);
}

GtkWidget *findSettingsTarget(GtkWidget *widget, std::string_view target) {
    const auto *id = static_cast<const char *>(g_object_get_data(G_OBJECT(widget), "modernime-settings-target"));
    if (id && target == id) return widget;
    if (!GTK_IS_CONTAINER(widget)) return nullptr;
    auto *children = gtk_container_get_children(GTK_CONTAINER(widget));
    GtkWidget *found = nullptr;
    for (auto *item = children; item && !found; item = item->next)
        found = findSettingsTarget(GTK_WIDGET(item->data), target);
    g_list_free(children);
    return found;
}

struct OverviewTask final {
    OverviewRefreshState::Generation generation;
    core::SettingsPaths paths;
    core::ModernIMESettings settings;
    std::filesystem::path remoteExecutable;
    Environment environment;
    std::string savedContentHash;
    std::filesystem::path fcitxExecutable;
    OverviewSetupAction setupAction = OverviewSetupAction::None;
};

struct OverviewResult final {
    OverviewSnapshot snapshot;
    std::string applicationMessage;
    std::string setupMessage;
};

struct GObjectUnref final {
    void operator()(GObject *object) const {
        if (object != nullptr) {
            g_object_unref(object);
        }
    }
};

struct GtkWidgetDestroy final {
    void operator()(GtkWidget *widget) const {
        if (widget != nullptr) {
            gtk_widget_destroy(widget);
        }
    }
};

using GObjectOwner = std::unique_ptr<GObject, GObjectUnref>;
using GtkWidgetOwner = std::unique_ptr<GtkWidget, GtkWidgetDestroy>;

void overviewTaskFunction(GTask *task, gpointer, gpointer data,
                          GCancellable *) {
    const auto *request = static_cast<const OverviewTask *>(data);
    std::string setupMessage;
    if (request->setupAction == OverviewSetupAction::Activate) {
        setupMessage = RuntimeController::activate(request->remoteExecutable,
                                                    request->environment).message;
    } else if (request->setupAction == OverviewSetupAction::Start ||
               request->setupAction == OverviewSetupAction::Reload) {
        setupMessage = RuntimeController::reload(request->fcitxExecutable,
            request->remoteExecutable, request->environment).message;
    }
    auto runtime = RuntimeController::probe(request->remoteExecutable,
                                                  request->paths.runtimeState,
                                                  request->environment);
    runtime.desktopPanel = RuntimeController::probeDesktopPanel(request->environment);
    g_task_return_pointer(
        task,
        new OverviewResult{
            collectOverviewSnapshot(request->paths, request->settings, runtime),
            request->savedContentHash.empty()
                ? std::string{}
                : describeSettingsApplication(runtime, request->savedContentHash),
            std::move(setupMessage)},
        [](gpointer value) { delete static_cast<OverviewResult *>(value); });
}

const char *iconForPage(SettingsPageId page) {
    switch (page) {
    case SettingsPageId::Overview:
        return "go-home-symbolic";
    case SettingsPageId::Input:
        return "input-keyboard-symbolic";
    case SettingsPageId::Candidates:
        return "video-display-symbolic";
    case SettingsPageId::Dictionary:
        return "accessories-dictionary-symbolic";
    case SettingsPageId::Clipboard:
        return "edit-paste-symbolic";
    case SettingsPageId::Learning:
        return nullptr;
    case SettingsPageId::Updates:
        return "software-update-available-symbolic";
    case SettingsPageId::Diagnostics:
        return "utilities-system-monitor-symbolic";
    }
    return "emblem-default-symbolic";
}

GtkWidget *createNavigationIcon(SettingsPageId page) {
    if (const auto *name = iconForPage(page))
        return gtk_image_new_from_icon_name(name, GTK_ICON_SIZE_BUTTON);

    auto *icon = gtk_drawing_area_new();
    gtk_widget_set_size_request(icon, 16, 16);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
    gtk_widget_set_has_window(icon, FALSE);
    gtk_widget_set_can_focus(icon, FALSE);
    atk_object_set_role(gtk_widget_get_accessible(icon), ATK_ROLE_REDUNDANT_OBJECT);
    g_signal_connect(icon, "draw", G_CALLBACK(+[](GtkWidget *widget, cairo_t *cr, gpointer) -> gboolean {
        GdkRGBA ink{};
        gtk_style_context_get_color(gtk_widget_get_style_context(widget),
                                    gtk_widget_get_state_flags(widget), &ink);
        cairo_save(cr);
        cairo_translate(cr, (gtk_widget_get_allocated_width(widget) - 16) / 2.0,
                        (gtk_widget_get_allocated_height(widget) - 16) / 2.0);
        gdk_cairo_set_source_rgba(cr, &ink);
        cairo_set_line_width(cr, 1.2);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        // Two compact hemispheres, with room around the inner memory folds.
        cairo_move_to(cr, 8, 3.2);
        cairo_curve_to(cr, 7.5, 1.1, 4.5, 1.1, 4, 3.1);
        cairo_curve_to(cr, 1.6, 3.1, 1, 5.9, 2.5, 7.3);
        cairo_curve_to(cr, .9, 9.3, 2, 11.9, 4.1, 12);
        cairo_curve_to(cr, 4.5, 14.4, 7.4, 14.5, 8, 12.5);
        cairo_curve_to(cr, 8.6, 14.5, 11.5, 14.4, 11.9, 12);
        cairo_curve_to(cr, 14, 11.9, 15.1, 9.3, 13.5, 7.3);
        cairo_curve_to(cr, 15, 5.9, 14.4, 3.1, 12, 3.1);
        cairo_curve_to(cr, 11.5, 1.1, 8.5, 1.1, 8, 3.2);
        cairo_line_to(cr, 8, 12.5);
        cairo_move_to(cr, 4.2, 5.1);
        cairo_curve_to(cr, 5.9, 5.2, 6.2, 6.4, 5.6, 7.5);
        cairo_move_to(cr, 11.8, 5.1);
        cairo_curve_to(cr, 10.1, 5.2, 9.8, 6.4, 10.4, 7.5);
        cairo_move_to(cr, 4.6, 10.4);
        cairo_curve_to(cr, 5.8, 9.4, 7.3, 9.8, 8, 11);
        cairo_curve_to(cr, 8.7, 9.8, 10.2, 9.4, 11.4, 10.4);
        cairo_stroke(cr);
        cairo_restore(cr);
        return FALSE;
    }), nullptr);
    return icon;
}

} // namespace

class SettingsShell::Impl final {
public:
    Impl(GtkApplication *gtkApplication, core::SettingsPaths settingsPaths)
        : application(gtkApplication), paths(std::move(settingsPaths)),
          model(paths.settingsFile), fcitx(executablePath("fcitx5")),
          remote(executablePath("fcitx5-remote")),
          environment(currentEnvironment()),
          overviewLifetime(G_OBJECT(g_object_new(G_TYPE_OBJECT, nullptr))),
          lifetimeGuard(std::make_shared<Lifetime>(this)) {
        g_object_set_data_full(
            overviewLifetime.get(), kOverviewStateKey,
            new SharedLifetime(lifetimeGuard.state()),
            [](gpointer value) { delete static_cast<SharedLifetime *>(value); });
        buildWindow();
        show(SettingsPageId::Overview, {});
        showLoadDiagnostics();
    }

    ~Impl() {
        motion.reset();
        if (taskStatusTimeout != 0) g_source_remove(taskStatusTimeout);
    }

    GtkWidget *window() const { return windowOwner.get(); }

    void present() {
        if (overviewPresentation.presentedAndNeedsRefresh()) {
            requestOverviewRefresh();
        }
        gtk_widget_show_all(windowOwner.get());
        gtk_window_present(GTK_WINDOW(windowOwner.get()));
    }

    void show(SettingsPageId page, std::string_view target) {
        if (motion) { motion->finishTheme(); motion->cancelHint(); }
        const auto &definition = pageDefinition(page);
        gtk_stack_set_visible_child_name(GTK_STACK(stack),
                                         std::string(definition.name).c_str());
        updateNavigation(page);

        switch (page) {
        case SettingsPageId::Overview:
            requestOverviewRefresh();
            break;
        case SettingsPageId::Input:
            inputPage->refresh();
            break;
        case SettingsPageId::Candidates:
            candidatesPage->refresh();
            break;
        case SettingsPageId::Dictionary:
            dictionaryPage->refresh();
            break;
        case SettingsPageId::Clipboard:
            clipboardPage->refresh(false);
            break;
        case SettingsPageId::Learning:
            learningPage->refresh(false);
            break;
        case SettingsPageId::Updates:
            updatesPage->refresh();
            break;
        case SettingsPageId::Diagnostics:
            diagnosticsPage->refresh();
            break;
        }

        if (!target.empty()) {
            const bool focused = focusPageTarget(page, target);
            if (motion) {
                auto *control = focused ? gtk_window_get_focus(GTK_WINDOW(windowOwner.get()))
                    : findSettingsTarget(gtk_stack_get_visible_child(GTK_STACK(stack)), target);
                motion->hintFocus(control);
            }
        }
    }

    void presentError(std::string_view message) {
        if (taskStatusTimeout != 0) {
            g_source_remove(taskStatusTimeout);
            taskStatusTimeout = 0;
        }
        std::string compact(message);
        for (auto &character : compact) {
            if (character == '\n' || character == '\r' || character == '\t') character = ' ';
        }
        gtk_label_set_text(GTK_LABEL(taskStatus), compact.c_str());
        gtk_widget_set_tooltip_text(taskStatus, std::string(message).c_str());
        if (motion) motion->showMessage();
        if (message.empty()) return;
        taskStatusTimeout = g_timeout_add(5000, +[](gpointer data) -> gboolean {
            auto &owner = *static_cast<Impl *>(data);
            owner.taskStatusTimeout = 0;
            if (owner.motion) owner.motion->hideMessage();
            else gtk_label_set_text(GTK_LABEL(owner.taskStatus), "");
            gtk_widget_set_tooltip_text(owner.taskStatus, nullptr);
            return G_SOURCE_REMOVE;
        }, this);
    }

private:
    using Lifetime = detail::DiagnosticsLifetime<Impl>;
    using SharedLifetime = std::shared_ptr<Lifetime>;

    static constexpr const char *kOverviewStateKey =
        "modernime-overview-async-state";

    struct NavigationAction final {
        Impl *owner;
        SettingsPageId page;
    };

    struct SearchAction final {
        Impl *owner;
        SettingsSearchEntry entry;
    };

    static void destroyNavigationAction(gpointer data, GClosure *) {
        delete static_cast<NavigationAction *>(data);
    }

    static void destroySearchAction(gpointer data, GClosure *) {
        delete static_cast<SearchAction *>(data);
    }

    static void onNavigate(GtkButton *, gpointer data) {
        const auto *action = static_cast<const NavigationAction *>(data);
        action->owner->show(action->page, {});
    }

    static void onSearchChanged(GtkEditable *, gpointer data) {
        static_cast<Impl *>(data)->updateSearchResults();
    }

    static void onSearchResult(GtkButton *, gpointer data) {
        const auto *action = static_cast<const SearchAction *>(data);
        auto *owner = action->owner;
        const auto entry = action->entry;
        owner->show(entry.page, entry.target);
        gtk_popover_popdown(GTK_POPOVER(owner->searchPopover));
        gtk_entry_set_text(GTK_ENTRY(owner->searchEntry), "");
    }

    static void onRestoreEdits(GtkButton *, gpointer data) {
        static_cast<Impl *>(data)->restoreEdits();
    }

    static void onSave(GtkButton *, gpointer data) {
        static_cast<Impl *>(data)->saveEditedSettings(false);
    }

    static void onEditDefaults(GtkWidget *button, gpointer data) {
        auto *popover = gtk_widget_get_ancestor(button, GTK_TYPE_POPOVER);
        if (popover) gtk_popover_popdown(GTK_POPOVER(popover));
        static_cast<Impl *>(data)->editDefaults();
    }

    static gboolean onWindowDelete(GtkWidget *, GdkEvent *, gpointer data) {
        static_cast<Impl *>(data)->requestClose();
        return TRUE;
    }

    static gboolean onWindowKeyPress(GtkWidget *, GdkEventKey *event,
                                     gpointer data) {
        auto *owner = static_cast<Impl *>(data);
        const auto modifiers = event->state & gtk_accelerator_get_default_mod_mask();
        if (modifiers == GDK_CONTROL_MASK &&
            (event->keyval == GDK_KEY_f || event->keyval == GDK_KEY_F)) {
            gtk_widget_grab_focus(owner->searchEntry);
            return TRUE;
        }
        if (modifiers == GDK_CONTROL_MASK &&
            (event->keyval == GDK_KEY_Return ||
             event->keyval == GDK_KEY_KP_Enter)) {
            if (gtk_widget_is_sensitive(owner->saveButton)) {
                gtk_button_clicked(GTK_BUTTON(owner->saveButton));
            }
            return TRUE;
        }
        if (modifiers == 0 &&
            (event->keyval == GDK_KEY_Down || event->keyval == GDK_KEY_Tab) &&
            gtk_widget_has_focus(owner->searchEntry) &&
            gtk_widget_get_visible(owner->searchPopover) &&
            gtk_widget_child_focus(owner->searchResults, GTK_DIR_TAB_FORWARD)) {
            return TRUE;
        }
        if (modifiers == 0 && event->keyval == GDK_KEY_Escape &&
            gtk_widget_get_visible(owner->searchPopover)) {
            gtk_popover_popdown(GTK_POPOVER(owner->searchPopover));
            return TRUE;
        }
        return FALSE;
    }

    static void overviewTaskFinished(GObject *source, GAsyncResult *result,
                                     gpointer) {
        GError *error = nullptr;
        auto *snapshot = static_cast<OverviewResult *>(
            g_task_propagate_pointer(G_TASK(result), &error));
        const auto *request =
            static_cast<const OverviewTask *>(g_task_get_task_data(G_TASK(result)));
        const auto generation = request->generation;
        auto *state = static_cast<SharedLifetime *>(
            g_object_get_data(source, kOverviewStateKey));
        if (state != nullptr) {
            (*state)->withOwner([snapshot, error, generation, request](Impl &owner) {
                owner.finishOverviewRefresh(generation, request->savedContentHash,
                                            snapshot, error, request->setupAction);
            });
        }
        delete snapshot;
        g_clear_error(&error);
    }

    void buildWindow() {
        windowOwner.reset(gtk_application_window_new(application));
        gtk_window_set_title(GTK_WINDOW(windowOwner.get()), "ModernIME 设置");
        gtk_window_set_icon_name(GTK_WINDOW(windowOwner.get()), "modernime");
        const auto defaultSize = settingsDefaultWindowSize();
        gtk_window_set_default_size(GTK_WINDOW(windowOwner.get()),
                                    defaultSize.width, defaultSize.height);
        const auto minimumSize = settingsMinimumWindowSize();
        gtk_widget_set_size_request(windowOwner.get(), minimumSize.width,
                                    minimumSize.height);
        addStyleClass(windowOwner.get(), kSettingsWindowClass);
        installSettingsStyles();
        applySettingsTheme(model.settings().clientTheme);
        appliedTheme = model.settings().clientTheme;
        g_signal_connect(windowOwner.get(), "delete-event",
                         G_CALLBACK(onWindowDelete), this);
        g_signal_connect(windowOwner.get(), "key-press-event",
                         G_CALLBACK(onWindowKeyPress), this);

        detail::GtkWidgetGuard rootGuard(
            gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
        auto *root = rootGuard.get();
        auto *overlay = gtk_overlay_new();
        gtk_container_add(GTK_CONTAINER(windowOwner.get()), overlay);
        gtk_container_add(GTK_CONTAINER(overlay), root);
        rootGuard.release();

        detail::GtkWidgetGuard bodyGuard(
            gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
        auto *body = bodyGuard.get();
        gtk_box_pack_start(GTK_BOX(root), body, TRUE, TRUE, 0);
        bodyGuard.release();

        gtk_box_pack_start(GTK_BOX(body), buildSidebar(), FALSE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(body), buildPageStack(), TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(root), buildBottomBar(), FALSE, FALSE, 0);
        motion = std::make_unique<SettingsMotion>(windowOwner.get(), overlay, root,
                                                  navigationBox, editState, taskStatus);
        updateActionState();
    }

    GtkWidget *buildSidebar() {
        detail::GtkWidgetGuard sidebarGuard(
            gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
        auto *sidebar = sidebarGuard.get();
        addStyleClass(sidebar, kSettingsSidebarClass);
        gtk_widget_set_size_request(sidebar, 220, -1);
        gtk_widget_set_hexpand(sidebar, FALSE);

        // The wordmark follows the chosen client accent.
        auto *brandBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_margin_start(brandBox, 16);
        gtk_widget_set_margin_top(brandBox, 16);
        gtk_widget_set_margin_bottom(brandBox, 14);
        auto *brandLabel = gtk_label_new(nullptr);
        addStyleClass(brandLabel, "modernime-brand");
        gtk_label_set_text(GTK_LABEL(brandLabel), "ModernIME");
        gtk_widget_set_halign(brandLabel, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(brandBox), brandLabel, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(sidebar), brandBox, FALSE, FALSE, 0);

        searchEntry = gtk_search_entry_new();
        addStyleClass(searchEntry, "modernime-search-box");
        gtk_entry_set_placeholder_text(GTK_ENTRY(searchEntry), "搜索设置");
        gtk_entry_set_width_chars(GTK_ENTRY(searchEntry), 14);
        gtk_entry_set_max_width_chars(GTK_ENTRY(searchEntry), 18);
        gtk_widget_set_tooltip_text(searchEntry,
                                    "仅在本地搜索设置名称和说明");
        setAccessibleWidgetText(searchEntry, "搜索设置",
                                "仅在本地搜索设置名称和说明，按 Ctrl+F 聚焦");
        gtk_widget_set_margin_start(searchEntry, 10);
        gtk_widget_set_margin_end(searchEntry, 10);
        gtk_widget_set_margin_bottom(searchEntry, 10);
        gtk_box_pack_start(GTK_BOX(sidebar), searchEntry, FALSE, FALSE, 0);

        searchPopover = gtk_popover_new(searchEntry);
        g_object_set_data_full(G_OBJECT(searchPopover), "modernime-settings-target",
                               g_strdup("settings-search-popover"), g_free);
        // Search suggestions must leave the entry usable for continuous typing.
        gtk_popover_set_modal(GTK_POPOVER(searchPopover), FALSE);
        gtk_popover_set_position(GTK_POPOVER(searchPopover), GTK_POS_RIGHT);
        searchResults = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_size_request(searchResults, 360, -1);
        gtk_container_set_border_width(GTK_CONTAINER(searchResults), 8);
        gtk_container_add(GTK_CONTAINER(searchPopover), searchResults);
        // This small local index needs no delayed query: a queued search-changed
        // signal could reopen suggestions after Escape or a focus change.
        g_signal_connect(searchEntry, "changed",
                         G_CALLBACK(onSearchChanged), this);
        g_signal_connect(windowOwner.get(), "set-focus",
                         G_CALLBACK(+[](GtkWindow *, GtkWidget *focus, gpointer data) {
                             auto *owner = static_cast<Impl *>(data);
                             if (focus != owner->searchEntry &&
                                 (focus == nullptr ||
                                  !gtk_widget_is_ancestor(focus, owner->searchPopover))) {
                                 gtk_popover_popdown(GTK_POPOVER(owner->searchPopover));
                             }
                         }), this);

        auto *navBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        navigationBox = navBox;
        for (const auto &definition : settingsPageDefinitions()) {
            auto *button = gtk_button_new();
            addStyleClass(button, "modernime-nav-button");
            gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
            gtk_widget_set_focus_on_click(button, FALSE);
            gtk_widget_set_hexpand(button, TRUE);
            gtk_widget_set_margin_start(button, 6);
            gtk_widget_set_margin_end(button, 6);
            gtk_widget_set_margin_top(button, 1);
            gtk_widget_set_margin_bottom(button, 1);

            auto *btnBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            gtk_widget_set_margin_start(btnBox, 8);
            gtk_widget_set_margin_end(btnBox, 8);
            gtk_widget_set_margin_top(btnBox, 3);
            gtk_widget_set_margin_bottom(btnBox, 3);

            auto *icon = createNavigationIcon(definition.id);
            addStyleClass(icon, "modernime-nav-icon");
            gtk_box_pack_start(GTK_BOX(btnBox), icon, FALSE, FALSE, 0);

            auto *label = gtk_label_new(std::string(definition.title).c_str());
            addStyleClass(label, "modernime-nav-label");
            gtk_widget_set_halign(label, GTK_ALIGN_START);
            gtk_box_pack_start(GTK_BOX(btnBox), label, TRUE, TRUE, 0);

            gtk_container_add(GTK_CONTAINER(button), btnBox);

            gtk_widget_set_tooltip_text(
                button, std::string(definition.subtitle).c_str());
            setAccessibleWidgetText(button, definition.title,
                                    definition.subtitle);
            gtk_box_pack_start(GTK_BOX(navBox), button, FALSE, FALSE, 0);
            navigationButtons.emplace_back(definition.id, button);
            g_signal_connect_data(
                button, "clicked", G_CALLBACK(onNavigate),
                new NavigationAction{this, definition.id},
                destroyNavigationAction, static_cast<GConnectFlags>(0));
        }

        auto *navScroller = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(navScroller),
                                       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(navScroller),
                                            GTK_SHADOW_NONE);
        gtk_container_add(GTK_CONTAINER(navScroller), navBox);
        gtk_box_pack_start(GTK_BOX(sidebar), navScroller, TRUE, TRUE, 0);

        // Sidebar bottom footer matching layout-1.png
        const auto channel = UpdateController::detectInstallChannel(environment);
        std::string channelText = "手动安装";
        if (channel == "apt" || channel == "deb") {
            channelText = "APT 安装";
        } else if (channel == "dnf" || channel == "rpm") {
            channelText = "DNF 安装";
        } else if (channel == "script") {
            channelText = "脚本安装";
        }
        const std::string versionFooter = "v" MODERNIME_VERSION " · " + channelText;
        auto *footerLabel = gtk_label_new(versionFooter.c_str());
        addStyleClass(footerLabel, "modernime-sidebar-footer");
        gtk_widget_set_halign(footerLabel, GTK_ALIGN_START);
        gtk_widget_set_margin_start(footerLabel, 16);
        gtk_widget_set_margin_bottom(footerLabel, 16);
        gtk_widget_set_margin_top(footerLabel, 8);
        gtk_box_pack_end(GTK_BOX(sidebar), footerLabel, FALSE, FALSE, 0);

        return sidebarGuard.release();
    }

    GtkWidget *buildPageStack() {
        detail::GtkWidgetGuard stackGuard(gtk_stack_new());
        stack = stackGuard.get();
        addStyleClass(stack, "modernime-page-stack");
        gtk_stack_set_transition_type(GTK_STACK(stack),
                                      GTK_STACK_TRANSITION_TYPE_CROSSFADE);
        gtk_stack_set_transition_duration(GTK_STACK(stack), 160);

        const auto notify = [this](std::string message) {
            onPageMessage(std::move(message));
        };
        overviewPage = std::make_unique<OverviewPage>(
            [this](SettingsPageId page) { show(page, {}); },
            [this](OverviewSetupAction action) {
                if (action == OverviewSetupAction::Install) {
                    show(SettingsPageId::Updates, {});
                } else if (action == OverviewSetupAction::Configure) {
                    show(SettingsPageId::Input, "input-enabled");
                } else if (action == OverviewSetupAction::DesktopPanel) {
                    show(SettingsPageId::Diagnostics, "diagnostics-panel-enable");
                } else if (action != OverviewSetupAction::None) {
                    pendingSetupAction = action;
                    overviewPage->setSetupBusy(true);
                    requestOverviewRefresh();
                }
            },
            [this] { requestOverviewRefresh(); });
        addPage(SettingsPageId::Overview, overviewPage->widget());
        inputPage = std::make_unique<InputPage>(model, [this] {
            updateActionState();
        });
        addPage(SettingsPageId::Input, inputPage->widget());
        addInputPageMenu();
        candidatesPage = std::make_unique<CandidatesPage>(model, [this] {
            updateActionState();
        });
        addPage(SettingsPageId::Candidates, candidatesPage->widget());
        dictionaryPage =
            std::make_unique<DictionaryPage>(paths.userDictionary, notify);
        addPage(SettingsPageId::Dictionary, dictionaryPage->widget());
        clipboardPage = std::make_unique<ClipboardPage>(
            model, paths.clipboardHistory,
            [this] { updateActionState(); }, notify, paths.clipboardFavorites);
        addPage(SettingsPageId::Clipboard, clipboardPage->widget());
        learningPage = std::make_unique<LearningPage>(
            model, paths.learningStore,
            [this] { updateActionState(); }, notify);
        addPage(SettingsPageId::Learning, learningPage->widget());
        updatesPage = std::make_unique<UpdatesPage>(environment, notify,
            [this] { return prepareUpdateRestart(); }, [this] { hideWindow(); });
        addPage(SettingsPageId::Updates, updatesPage->widget());
        diagnosticsPage = std::make_unique<DiagnosticsPage>(
            fcitx, remote, environment, notify,
            [this] { return model.savedRevision(); },
            [this](SettingsWindowModel::Revision revision) {
                model.markReloaded(revision);
                updateActionState();
                if (revision != model.savedRevision()) {
                    savedStatusHash = model.savedContentHash();
                    presentError("已保存，应用状态待确认");
                    requestOverviewRefresh();
                }
            });
        addPage(SettingsPageId::Diagnostics, diagnosticsPage->widget());

        return stackGuard.release();
    }

    void addPage(SettingsPageId page, GtkWidget *widget) {
        detail::GtkWidgetGuard pageGuard(widget);
        const auto &definition = pageDefinition(page);
        const auto name = std::string(definition.name);
        detail::GtkWidgetGuard scrolledGuard(
            createScrollablePage(pageGuard.release()));
        gtk_stack_add_named(GTK_STACK(stack), scrolledGuard.get(),
                            name.c_str());
        scrolledGuard.release();
    }

    void addInputPageMenu() {
        detail::GtkWidgetGuard menuGuard(gtk_popover_new(nullptr));
        auto *menu = menuGuard.get();
        auto *defaults = gtk_model_button_new();
        g_object_set(defaults, "text", "恢复默认", nullptr);
        gtk_container_set_border_width(GTK_CONTAINER(menu), 6);
        gtk_widget_set_tooltip_text(
            defaults, "只修改当前设置草稿，不删除个人词典或学习数据");
        setAccessibleWidgetText(
            defaults, "恢复默认",
            "只修改当前设置草稿，不删除个人词典或学习数据");
        gtk_container_add(GTK_CONTAINER(menu), defaults);
        gtk_widget_show(defaults);
        g_signal_connect(defaults, "clicked", G_CALLBACK(onEditDefaults),
                         this);

        detail::GtkWidgetGuard menuButtonGuard(gtk_menu_button_new());
        auto *menuButton = menuButtonGuard.get();
        gtk_button_set_label(GTK_BUTTON(menuButton), "更多");
        gtk_widget_set_halign(menuButton, GTK_ALIGN_END);
        gtk_widget_set_tooltip_text(menuButton, "打开输入体验的更多操作");
        setAccessibleWidgetText(menuButton, "更多",
                                "打开输入体验的更多操作菜单");
        g_object_set_data_full(G_OBJECT(menuButton),
                               "modernime-settings-target",
                               g_strdup("input-more"), g_free);
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(menuButton), menu);
        menuGuard.release();
        setPageHeaderAction(inputPage->widget(), menuButton);
        prependSettingsFocusChainChild(inputPage->widget(), menuButton);
        menuButtonGuard.release();
    }

    GtkWidget *buildBottomBar() {
        detail::GtkWidgetGuard barGuard(
            gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8));
        auto *bar = barGuard.get();
        addStyleClass(bar, "modernime-bottom-bar");

        editState = gtk_label_new(nullptr);
        gtk_label_set_ellipsize(GTK_LABEL(editState), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(editState), 22);
        gtk_label_set_width_chars(GTK_LABEL(editState), 22);
        gtk_label_set_xalign(GTK_LABEL(editState), 0);
        gtk_label_set_text(GTK_LABEL(editState), "● 所有设置已保存");
        addStyleClass(editState, "modernime-status");
        gtk_widget_set_halign(editState, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(bar), editState, FALSE, FALSE, 0);

        taskStatus = gtk_label_new("");
        g_object_set_data_full(G_OBJECT(taskStatus), "modernime-settings-target",
                              g_strdup("settings-task-status"), g_free);
        addStyleClass(taskStatus, "modernime-status");
        gtk_label_set_ellipsize(GTK_LABEL(taskStatus), PANGO_ELLIPSIZE_END);
        gtk_label_set_single_line_mode(GTK_LABEL(taskStatus), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(taskStatus), 40);
        gtk_label_set_xalign(GTK_LABEL(taskStatus), 0);
        gtk_widget_set_valign(taskStatus, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(taskStatus, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(bar), taskStatus, TRUE, TRUE, 0);

        auto *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        const auto labels = settingsActionLabels();
        restoreButton = gtk_button_new_with_label(labels[0].data());
        saveButton = gtk_button_new_with_label(labels[1].data());
        addStyleClass(saveButton, kSettingsPrimaryButtonClass);
        setAccessibleWidgetText(restoreButton, labels[0],
                                "放弃当前会话内尚未保存的设置修改");
        setAccessibleWidgetText(saveButton, labels[1],
                                "保存设置但保持窗口打开，可按 Ctrl+Enter");
        for (auto *button : {restoreButton, saveButton}) {
            gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
            gtk_box_pack_start(GTK_BOX(buttons), button, FALSE, FALSE, 0);
        }
        gtk_box_pack_end(GTK_BOX(bar), buttons, FALSE, FALSE, 0);
        g_signal_connect(restoreButton, "clicked", G_CALLBACK(onRestoreEdits),
                         this);
        g_signal_connect(saveButton, "clicked", G_CALLBACK(onSave), this);
        return barGuard.release();
    }

    void updateNavigation(SettingsPageId selected) {
        for (const auto &[page, button] : navigationButtons) {
            removeStyleClass(button, "modernime-nav-selected");
            gtk_widget_unset_state_flags(button, GTK_STATE_FLAG_SELECTED);
            if (page == selected) {
                if (motion) motion->selectNavigation(button);
                addStyleClass(button, "modernime-nav-selected");
                gtk_widget_set_state_flags(button, GTK_STATE_FLAG_SELECTED, FALSE);
            }
        }
    }

    void updateSearchResults() {
        clearContainer(searchResults);
        const auto query = gtk_entry_get_text(GTK_ENTRY(searchEntry));
        const std::string_view queryText = query == nullptr ? "" : query;
        if (queryText.find_first_not_of(" \t\n\r\f\v") == std::string_view::npos) {
            gtk_popover_popdown(GTK_POPOVER(searchPopover));
            return;
        }
        const auto results = searchSettings(queryText);
        if (results.empty()) {
            auto *title = gtk_label_new("未找到匹配的设置");
            gtk_widget_set_halign(title, GTK_ALIGN_START);
            auto *hint = gtk_label_new("试试其他关键词，如「候选」或「学习」");
            addStyleClass(hint, "modernime-description");
            gtk_widget_set_halign(hint, GTK_ALIGN_START);
            gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
            gtk_box_pack_start(GTK_BOX(searchResults), title, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(searchResults), hint, FALSE, FALSE, 0);
        }

        constexpr std::size_t kMaximumVisibleResults = 10;
        const auto count = std::min(results.size(), kMaximumVisibleResults);
        for (std::size_t index = 0; index < count; ++index) {
            const auto &entry = results[index];
            auto *button = gtk_button_new();
            gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
            gtk_widget_set_focus_on_click(button, FALSE);
            auto *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            auto *title = gtk_label_new(std::string(entry.title).c_str());
            gtk_widget_set_halign(title, GTK_ALIGN_START);
            auto descriptionText = std::string(pageDefinition(entry.page).title);
            descriptionText += " · ";
            descriptionText += entry.description;
            setAccessibleWidgetText(button, entry.title, descriptionText);
            auto *description = gtk_label_new(descriptionText.c_str());
            addStyleClass(description, "modernime-description");
            gtk_widget_set_halign(description, GTK_ALIGN_START);
            gtk_label_set_line_wrap(GTK_LABEL(description), TRUE);
            gtk_box_pack_start(GTK_BOX(content), title, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(content), description, FALSE, FALSE, 0);
            gtk_container_add(GTK_CONTAINER(button), content);
            gtk_box_pack_start(GTK_BOX(searchResults), button, FALSE, FALSE, 0);
            g_signal_connect_data(
                button, "clicked", G_CALLBACK(onSearchResult),
                new SearchAction{this, entry}, destroySearchAction,
                static_cast<GConnectFlags>(0));
        }
        gtk_widget_show_all(searchPopover);
        gtk_popover_popup(GTK_POPOVER(searchPopover));
    }

    bool focusPageTarget(SettingsPageId page, std::string_view target) {
        switch (page) {
        case SettingsPageId::Overview:
            return false;
        case SettingsPageId::Input:
            return inputPage->focusTarget(target);
        case SettingsPageId::Candidates:
            return candidatesPage->focusTarget(target);
        case SettingsPageId::Dictionary:
            if (dictionaryPage->focusTarget(target)) {
                return true;
            }
            if (target == "dictionary-edit" || target == "dictionary-delete") {
                return dictionaryPage->focusTarget("dictionary-search");
            }
            if (target == "dictionary-import" ||
                target == "dictionary-export") {
                return dictionaryPage->focusTarget("dictionary-import-export");
            }
            return false;
        case SettingsPageId::Clipboard:
            if (clipboardPage->focusTarget(target)) {
                return true;
            }
            if (target == "clipboard-copy" || target == "clipboard-delete" ||
                target == "clipboard-clear" ||
                target == "clipboard-refresh") {
                return clipboardPage->focusTarget("clipboard-history");
            }
            return false;
        case SettingsPageId::Learning:
            if (learningPage->focusTarget(target)) {
                return true;
            }
            if (target == "learning-clear") {
                return learningPage->focusTarget("learning-data");
            }
            return false;
        case SettingsPageId::Updates:
            return updatesPage->focusTarget(target);
        case SettingsPageId::Diagnostics:
            return diagnosticsPage->focusTarget(target);
        }
        return false;
    }

    void updateActionState() {
        const auto &theme = model.settings().clientTheme;
        if (theme != appliedTheme) {
            auto *origin = restoringTheme ? restoreButton : findSettingsTarget(candidatesPage->widget(), "client-theme");
            if (motion) motion->changeTheme(appliedTheme, theme, origin, restoringTheme ? 300 : 440);
            else applySettingsTheme(theme);
            appliedTheme = theme;
        }
        const auto validation = model.validation();
        const bool canSave = model.dirty() && validation.valid && !actionBusy;
        gtk_widget_set_sensitive(saveButton, canSave);
        gtk_widget_set_sensitive(restoreButton,
                                 model.dirty() && !actionBusy);

        std::string stateClass;
        std::string status;
        if (!validation.valid) {
            stateClass = "modernime-status-error";
            const auto message = validation.issues.empty()
                                     ? std::string("当前设置无法保存")
                                     : validation.issues.front().message;
            status = "! " + message;
        } else if (model.dirty()) {
            stateClass = "modernime-status-dirty";
            status = "● 有未保存的修改";
        } else {
            status = savedFeedback ? "✓ 所有设置已保存" : "● 所有设置已保存";
        }
        if (status != editStatusText) {
            if (motion && !editStatusText.empty()) motion->transitionStatus(!model.dirty() && savedFeedback ? 180 : 150);
            removeStyleClass(editState, "modernime-status-dirty");
            removeStyleClass(editState, "modernime-status-error");
            if (!stateClass.empty()) addStyleClass(editState, stateClass);
            gtk_label_set_text(GTK_LABEL(editState), status.c_str());
            editStatusText = status;
        }
    }

    void setActionBusy(bool busy) {
        actionBusy = busy;
        updateActionState();
    }

    bool saveEditedSettings(bool closeAfterSave) {
        const auto validation = model.validation();
        if (!validation.valid) {
            if (validation.issues.empty()) {
                presentError("当前设置无法保存");
            } else {
                const auto &issue = validation.issues.front();
                presentError(issue.message);
                focusValidationIssue(issue.key);
            }
            updateActionState();
            return false;
        }
        if (!model.dirty()) {
            presentError("没有需要保存的修改");
            if (closeAfterSave) {
                hideWindow();
            }
            return true;
        }

        setActionBusy(true);
        std::string error;
        const bool saved = model.save(&error);
        if (saved) savedFeedback = true;
        setActionBusy(false);
        if (!saved) {
            presentError(error.empty() ? "设置保存失败，修改仍保留" : error);
            return false;
        }

        updateActionState();
        savedStatusHash = model.savedContentHash();
        presentError("已保存，应用状态待确认");
        requestOverviewRefresh();
        if (closeAfterSave) {
            hideWindow();
        }
        return true;
    }

    void restoreEdits() {
        restoringTheme = true;
        model.resetEdits();
        refreshSettingsPages();
        requestOverviewRefresh();
        updateActionState();
        restoringTheme = false;
        presentError("已恢复未保存的修改");
    }

    void editDefaults() {
        auto *dialog = gtk_message_dialog_new(
            GTK_WINDOW(windowOwner.get()), GTK_DIALOG_MODAL,
            GTK_MESSAGE_WARNING,
            GTK_BUTTONS_YES_NO,
            "这只会修改 ModernIME 设置草稿，不会删除学习记录或个人词典。继续吗？");
        setDialogResponseAccessibility(
            GTK_DIALOG(dialog), GTK_RESPONSE_YES, "恢复默认",
            "将当前设置草稿恢复为默认值但不立即保存");
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_NO,
                                       "取消", "保留当前设置草稿");
        const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (response != GTK_RESPONSE_YES) {
            return;
        }
        model.editDefaults();
        refreshSettingsPages();
        updateActionState();
        presentError("ModernIME 设置已恢复默认值，请保存后生效");
    }

    void refreshSettingsPages() {
        inputPage->refresh();
        candidatesPage->refresh();
        clipboardPage->refreshSettings();
        learningPage->refreshSettings();
    }

    bool prepareUpdateRestart() {
        if (!dictionaryPage->confirmLeave()) return false;
        if (!model.dirty()) return true;
        auto *dialog = gtk_message_dialog_new(GTK_WINDOW(windowOwner.get()), GTK_DIALOG_MODAL,
            GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "更新前如何处理未保存的设置？");
        gtk_dialog_add_button(GTK_DIALOG(dialog), "暂不更新", GTK_RESPONSE_CANCEL);
        gtk_dialog_add_button(GTK_DIALOG(dialog), "放弃修改并更新", GTK_RESPONSE_REJECT);
        gtk_dialog_add_button(GTK_DIALOG(dialog), "保存并更新", GTK_RESPONSE_ACCEPT);
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL,
            "暂不更新", "保留设置草稿，暂不安装或重启");
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_REJECT,
            "放弃修改并更新", "放弃尚未保存的设置，继续更新");
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT,
            "保存并更新", "保存当前设置，成功后继续更新");
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
        const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (response == GTK_RESPONSE_ACCEPT) return saveEditedSettings(false);
        if (response == GTK_RESPONSE_REJECT) {
            model.resetEdits();
            refreshSettingsPages();
            updateActionState();
            return true;
        }
        return false;
    }

    void requestClose() {
        if (!dictionaryPage->confirmLeave()) return;
        if (!model.dirty()) {
            hideWindow();
            return;
        }

        auto *dialog = gtk_message_dialog_new(
            GTK_WINDOW(windowOwner.get()), GTK_DIALOG_MODAL,
            GTK_MESSAGE_WARNING,
            GTK_BUTTONS_NONE, "有未保存的修改，确定要放弃并退出吗？");
        gtk_dialog_add_button(GTK_DIALOG(dialog), "继续编辑",
                              GTK_RESPONSE_CANCEL);
        gtk_dialog_add_button(GTK_DIALOG(dialog), "放弃修改",
                              GTK_RESPONSE_REJECT);
        gtk_dialog_add_button(GTK_DIALOG(dialog), "保存并退出",
                              GTK_RESPONSE_ACCEPT);
        setDialogResponseAccessibility(
            GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL, "继续编辑",
            "关闭提示并继续编辑当前设置草稿");
        setDialogResponseAccessibility(
            GTK_DIALOG(dialog), GTK_RESPONSE_REJECT, "放弃修改",
            "丢弃当前会话内尚未保存的设置修改并关闭窗口");
        setDialogResponseAccessibility(
            GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, "保存并退出",
            "保存当前设置草稿，成功后关闭窗口");
        const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);

        if (response == GTK_RESPONSE_REJECT) {
            model.resetEdits();
            refreshSettingsPages();
            requestOverviewRefresh();
            updateActionState();
            hideWindow();
        } else if (response == GTK_RESPONSE_ACCEPT) {
            saveEditedSettings(true);
        }
    }

    void hideWindow() {
        gtk_popover_popdown(GTK_POPOVER(searchPopover));
        gtk_widget_hide(windowOwner.get());
        g_application_quit(G_APPLICATION(application));
    }

    void onPageMessage(std::string message) {
        presentError(message);
    }

    void focusValidationIssue(std::string_view issueKey) {
        const auto route = settingsFocusRouteForIssue(issueKey);
        if (route.has_value()) {
            show(route->page, route->target);
        }
    }

    void requestOverviewRefresh() {
        const auto generation = overviewRefresh.request();
        if (generation.has_value()) {
            launchOverviewRefresh(*generation);
        }
    }

    void launchOverviewRefresh(OverviewRefreshState::Generation generation) {
        GObjectOwner taskOwner(G_OBJECT(g_task_new(
            overviewLifetime.get(), nullptr, overviewTaskFinished, nullptr)));
        auto request = std::make_unique<OverviewTask>(OverviewTask{
            generation, paths, model.settings(), remote, environment,
            savedStatusHash, fcitx,
            std::exchange(pendingSetupAction, OverviewSetupAction::None)});
        g_task_set_task_data(G_TASK(taskOwner.get()), request.release(),
                             [](gpointer value) {
            delete static_cast<OverviewTask *>(value);
        });
        g_task_run_in_thread(G_TASK(taskOwner.get()), overviewTaskFunction);
    }

    void finishOverviewRefresh(OverviewRefreshState::Generation generation,
                               const std::string &savedHash,
                               const OverviewResult *result,
                               const GError *error, OverviewSetupAction action) {
        const bool current = overviewRefresh.complete(generation);
        if (current && result != nullptr) {
            overviewPage->setSnapshot(result->snapshot);
            if (!savedHash.empty() && savedHash == savedStatusHash &&
                savedHash == model.savedContentHash()) {
                presentError(result->applicationMessage);
                savedStatusHash.clear();
            }
        } else if (current) {
            presentError(error == nullptr || error->message == nullptr
                             ? "无法读取概览"
                             : error->message);
        }
        if (action != OverviewSetupAction::None) {
            overviewPage->setSetupBusy(false);
            if (result != nullptr && !result->setupMessage.empty()) {
                presentError(result->setupMessage);
            }
        }
        const auto pending = overviewRefresh.startPending();
        if (pending.has_value()) {
            launchOverviewRefresh(*pending);
        }
    }

    void showLoadDiagnostics() {
        if (model.loadDiagnostics().empty()) {
            return;
        }
        std::ostringstream warning;
        warning << "配置读取警告：";
        for (std::size_t index = 0; index < model.loadDiagnostics().size();
             ++index) {
            if (index != 0) {
                warning << "；";
            }
            warning << model.loadDiagnostics()[index];
        }
        presentError(warning.str());
    }

    GtkApplication *application;
    core::SettingsPaths paths;
    SettingsWindowModel model;
    std::filesystem::path fcitx;
    std::filesystem::path remote;
    Environment environment;

    bool actionBusy = false;
    bool restoringTheme = false;
    bool savedFeedback = false;
    std::string appliedTheme;
    std::string editStatusText;
    std::string savedStatusHash;

    GtkWidget *stack = nullptr;
    GtkWidget *navigationBox = nullptr;
    GtkWidget *searchEntry = nullptr;
    GtkWidget *searchPopover = nullptr;
    GtkWidget *searchResults = nullptr;
    GtkWidget *editState = nullptr;
    GtkWidget *taskStatus = nullptr;
    guint taskStatusTimeout = 0;
    GtkWidget *restoreButton = nullptr;
    GtkWidget *saveButton = nullptr;
    std::vector<std::pair<SettingsPageId, GtkWidget *>> navigationButtons;

    std::unique_ptr<OverviewPage> overviewPage;
    std::unique_ptr<InputPage> inputPage;
    std::unique_ptr<CandidatesPage> candidatesPage;
    std::unique_ptr<DictionaryPage> dictionaryPage;
    std::unique_ptr<ClipboardPage> clipboardPage;
    std::unique_ptr<LearningPage> learningPage;
    std::unique_ptr<UpdatesPage> updatesPage;
    std::unique_ptr<DiagnosticsPage> diagnosticsPage;
    GtkWidgetOwner windowOwner;
    std::unique_ptr<SettingsMotion> motion;
    OverviewRefreshState overviewRefresh;
    OverviewSetupAction pendingSetupAction = OverviewSetupAction::None;
    OverviewPresentationState overviewPresentation;
    GObjectOwner overviewLifetime;
    ScopedLifetimeDeactivation<Lifetime> lifetimeGuard;
};

SettingsShell::SettingsShell(GtkApplication *application,
                             core::SettingsPaths paths)
    : impl_(std::make_unique<Impl>(application, std::move(paths))) {}

SettingsShell::~SettingsShell() = default;

GtkWidget *SettingsShell::window() const {
    return impl_->window();
}

void SettingsShell::present() {
    impl_->present();
}

void SettingsShell::show(SettingsPageId page, std::string_view target) {
    impl_->show(page, target);
}

void SettingsShell::presentError(std::string_view message) {
    impl_->presentError(message);
}

} // namespace modernime::settings
