#include "modernime/settings/settings_shell.h"

#include "modernime/settings/detail/diagnostics_lifetime.h"
#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/overview_model.h"
#include "modernime/settings/pages/clipboard_page.h"
#include "modernime/settings/pages/diagnostics_page.h"
#include "modernime/settings/pages/dictionary_page.h"
#include "modernime/settings/pages/input_page.h"
#include "modernime/settings/pages/learning_page.h"
#include "modernime/settings/pages/overview_page.h"
#include "modernime/settings/runtime_controller.h"
#include "modernime/settings/settings_model.h"
#include "modernime/settings/settings_shell_state.h"
#include "modernime/settings/settings_ui_contract.h"
#include "modernime/settings/settings_widgets.h"

#include <gio/gio.h>
#include <gtk/gtk.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
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

std::string modernimeAddonDirectories() {
    std::vector<std::string> directories;
#ifdef MODERNIME_INSTALL_PREFIX
    directories.emplace_back(
        (std::filesystem::path(MODERNIME_INSTALL_PREFIX) / "lib/fcitx5")
            .string());
#endif
#ifdef MODERNIME_SYSTEM_FCITX5_ADDON_DIR
    if (std::string_view(MODERNIME_SYSTEM_FCITX5_ADDON_DIR).size() != 0) {
        directories.emplace_back(MODERNIME_SYSTEM_FCITX5_ADDON_DIR);
    }
#endif
    if (const auto *existing = std::getenv("FCITX_ADDON_DIRS");
        existing != nullptr && *existing != '\0') {
        directories.emplace_back(existing);
    }

    std::ostringstream result;
    for (std::size_t index = 0; index < directories.size(); ++index) {
        if (index != 0) {
            result << ':';
        }
        result << directories[index];
    }
    return result.str();
}

Environment currentEnvironment() {
    Environment environment;
    for (const char *name : {"DISPLAY", "WAYLAND_DISPLAY",
                             "DBUS_SESSION_BUS_ADDRESS", "XDG_RUNTIME_DIR"}) {
        const auto *value = std::getenv(name);
        if (value != nullptr && *value != '\0') {
            environment.emplace_back(name, value);
        }
    }
    const auto addonDirectories = modernimeAddonDirectories();
    if (!addonDirectories.empty()) {
        environment.emplace_back("FCITX_ADDON_DIRS", addonDirectories);
    }
    return environment;
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

struct OverviewTask final {
    OverviewRefreshState::Generation generation;
    core::SettingsPaths paths;
    core::ModernIMESettings settings;
    std::filesystem::path remoteExecutable;
    Environment environment;
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
    const auto runtime = RuntimeController::probe(request->remoteExecutable,
                                                  request->environment);
    g_task_return_pointer(
        task,
        new OverviewSnapshot(collectOverviewSnapshot(
            request->paths, request->settings, runtime)),
        [](gpointer value) { delete static_cast<OverviewSnapshot *>(value); });
}

const char *iconForPage(SettingsPageId page) {
    switch (page) {
    case SettingsPageId::Overview:
        return "go-home-symbolic";
    case SettingsPageId::Input:
        return "input-keyboard-symbolic";
    case SettingsPageId::Dictionary:
        return "accessories-dictionary-symbolic";
    case SettingsPageId::Clipboard:
        return "edit-paste-symbolic";
    case SettingsPageId::Learning:
        return "starred-symbolic";
    case SettingsPageId::Diagnostics:
        return "utilities-system-monitor-symbolic";
    }
    return "emblem-default-symbolic";
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

    ~Impl() = default;

    GtkWidget *window() const { return windowOwner.get(); }

    void present() {
        if (overviewPresentation.presentedAndNeedsRefresh()) {
            requestOverviewRefresh();
        }
        gtk_widget_show_all(windowOwner.get());
        gtk_window_present(GTK_WINDOW(windowOwner.get()));
    }

    void show(SettingsPageId page, std::string_view target) {
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
        case SettingsPageId::Dictionary:
            dictionaryPage->refresh();
            break;
        case SettingsPageId::Clipboard:
            clipboardPage->refresh(false);
            break;
        case SettingsPageId::Learning:
            learningPage->refresh(false);
            break;
        case SettingsPageId::Diagnostics:
            diagnosticsPage->refresh();
            break;
        }

        if (!target.empty()) {
            focusPageTarget(page, target);
        }
    }

    void presentError(std::string_view message) {
        gtk_label_set_text(GTK_LABEL(taskStatus), std::string(message).c_str());
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

    static void onApply(GtkButton *, gpointer data) {
        static_cast<Impl *>(data)->saveEditedSettings(false);
    }

    static void onSaveAndClose(GtkButton *, gpointer data) {
        static_cast<Impl *>(data)->saveEditedSettings(true);
    }

    static void onEditDefaults(GtkMenuItem *, gpointer data) {
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
            if (gtk_widget_is_sensitive(owner->applyButton)) {
                gtk_button_clicked(GTK_BUTTON(owner->applyButton));
            }
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
        auto *snapshot = static_cast<OverviewSnapshot *>(
            g_task_propagate_pointer(G_TASK(result), &error));
        const auto *request =
            static_cast<const OverviewTask *>(g_task_get_task_data(G_TASK(result)));
        const auto generation = request->generation;
        auto *state = static_cast<SharedLifetime *>(
            g_object_get_data(source, kOverviewStateKey));
        if (state != nullptr) {
            (*state)->withOwner([snapshot, error, generation](Impl &owner) {
                owner.finishOverviewRefresh(generation, snapshot, error);
            });
        }
        delete snapshot;
        g_clear_error(&error);
    }

    void buildWindow() {
        windowOwner.reset(gtk_application_window_new(application));
        gtk_window_set_title(GTK_WINDOW(windowOwner.get()), "ModernIME 设置");
        const auto defaultSize = settingsDefaultWindowSize();
        gtk_window_set_default_size(GTK_WINDOW(windowOwner.get()),
                                    defaultSize.width, defaultSize.height);
        const auto minimumSize = settingsMinimumWindowSize();
        gtk_widget_set_size_request(windowOwner.get(), minimumSize.width,
                                    minimumSize.height);
        addStyleClass(windowOwner.get(), kSettingsWindowClass);
        installSettingsStyles();
        g_signal_connect(windowOwner.get(), "delete-event",
                         G_CALLBACK(onWindowDelete), this);
        g_signal_connect(windowOwner.get(), "key-press-event",
                         G_CALLBACK(onWindowKeyPress), this);

        detail::GtkWidgetGuard rootGuard(
            gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
        auto *root = rootGuard.get();
        gtk_container_add(GTK_CONTAINER(windowOwner.get()), root);
        rootGuard.release();

        detail::GtkWidgetGuard bodyGuard(
            gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
        auto *body = bodyGuard.get();
        gtk_box_pack_start(GTK_BOX(root), body, TRUE, TRUE, 0);
        bodyGuard.release();

        gtk_box_pack_start(GTK_BOX(body), buildSidebar(), FALSE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(body), buildPageStack(), TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(root), buildBottomBar(), FALSE, FALSE, 0);
        updateActionState();
    }

    GtkWidget *buildSidebar() {
        detail::GtkWidgetGuard sidebarGuard(
            gtk_box_new(GTK_ORIENTATION_VERTICAL, 10));
        auto *sidebar = sidebarGuard.get();
        addStyleClass(sidebar, kSettingsSidebarClass);
        gtk_widget_set_size_request(sidebar, 220, -1);

        searchEntry = gtk_search_entry_new();
        addStyleClass(searchEntry, "modernime-search-box");
        gtk_entry_set_placeholder_text(GTK_ENTRY(searchEntry), "搜索设置");
        gtk_widget_set_tooltip_text(searchEntry,
                                    "仅在本地搜索设置名称和说明");
        setAccessibleWidgetText(searchEntry, "搜索设置",
                                "仅在本地搜索设置名称和说明，按 Ctrl+F 聚焦");
        gtk_box_pack_start(GTK_BOX(sidebar), searchEntry, FALSE, FALSE, 0);

        searchPopover = gtk_popover_new(searchEntry);
        gtk_popover_set_position(GTK_POPOVER(searchPopover), GTK_POS_RIGHT);
        searchResults = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_size_request(searchResults, 360, -1);
        gtk_container_set_border_width(GTK_CONTAINER(searchResults), 8);
        gtk_container_add(GTK_CONTAINER(searchPopover), searchResults);
        g_signal_connect(searchEntry, "search-changed",
                         G_CALLBACK(onSearchChanged), this);

        std::string_view previousGroup;
        for (const auto &definition : settingsPageDefinitions()) {
            if (definition.group != previousGroup) {
                auto *group = gtk_label_new(std::string(definition.group).c_str());
                addStyleClass(group, "modernime-sidebar-group");
                gtk_widget_set_halign(group, GTK_ALIGN_START);
                gtk_box_pack_start(GTK_BOX(sidebar), group, FALSE, FALSE, 0);
                previousGroup = definition.group;
            }
            auto *button = gtk_button_new();
            addStyleClass(button, "modernime-nav-button");
            gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
            gtk_widget_set_hexpand(button, TRUE);

            auto *btnBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            gtk_widget_set_margin_start(btnBox, 4);
            gtk_widget_set_margin_end(btnBox, 4);
            gtk_widget_set_margin_top(btnBox, 1);
            gtk_widget_set_margin_bottom(btnBox, 1);

            auto *icon = gtk_image_new_from_icon_name(
                iconForPage(definition.id), GTK_ICON_SIZE_BUTTON);
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
            gtk_box_pack_start(GTK_BOX(sidebar), button, FALSE, FALSE, 0);
            navigationButtons.emplace_back(definition.id, button);
            g_signal_connect_data(
                button, "clicked", G_CALLBACK(onNavigate),
                new NavigationAction{this, definition.id},
                destroyNavigationAction, static_cast<GConnectFlags>(0));
        }
        return sidebarGuard.release();
    }

    GtkWidget *buildPageStack() {
        detail::GtkWidgetGuard stackGuard(gtk_stack_new());
        stack = stackGuard.get();
        addStyleClass(stack, "modernime-page-stack");
        gtk_stack_set_transition_type(GTK_STACK(stack),
                                      GTK_STACK_TRANSITION_TYPE_NONE);

        const auto notify = [this](std::string message) {
            onPageMessage(std::move(message));
        };
        overviewPage = std::make_unique<OverviewPage>(
            [this](SettingsPageId page) { show(page, {}); });
        addPage(SettingsPageId::Overview, overviewPage->widget());
        inputPage = std::make_unique<InputPage>(model, [this] {
            updateActionState();
        });
        addPage(SettingsPageId::Input, inputPage->widget());
        addInputPageMenu();
        dictionaryPage =
            std::make_unique<DictionaryPage>(paths.userDictionary, notify);
        addPage(SettingsPageId::Dictionary, dictionaryPage->widget());
        clipboardPage = std::make_unique<ClipboardPage>(
            model, paths.clipboardHistory,
            [this] { updateActionState(); }, notify);
        addPage(SettingsPageId::Clipboard, clipboardPage->widget());
        learningPage = std::make_unique<LearningPage>(
            model, paths.learningStore,
            [this] { updateActionState(); }, notify);
        addPage(SettingsPageId::Learning, learningPage->widget());
        diagnosticsPage = std::make_unique<DiagnosticsPage>(
            fcitx, remote, environment, notify,
            [this] { return model.savedRevision(); },
            [this](SettingsWindowModel::Revision revision) {
                model.markReloaded(revision);
                updateActionState();
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
        detail::GtkWidgetGuard menuGuard(gtk_menu_new());
        auto *menu = menuGuard.get();
        auto *defaults = gtk_menu_item_new_with_label("恢复默认");
        gtk_widget_set_tooltip_text(
            defaults, "只修改当前设置草稿，不删除个人词典或学习数据");
        setAccessibleWidgetText(
            defaults, "恢复默认",
            "只修改当前设置草稿，不删除个人词典或学习数据");
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), defaults);
        gtk_widget_show(defaults);
        g_signal_connect(defaults, "activate", G_CALLBACK(onEditDefaults),
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
        gtk_menu_button_set_popup(GTK_MENU_BUTTON(menuButton), menu);
        menuGuard.release();
        gtk_box_pack_start(GTK_BOX(inputPage->widget()), menuButton, FALSE,
                           FALSE, 0);
        prependSettingsFocusChainChild(inputPage->widget(), menuButton);
        menuButtonGuard.release();
        gtk_box_reorder_child(GTK_BOX(inputPage->widget()), menuButton, 2);
    }

    GtkWidget *buildBottomBar() {
        detail::GtkWidgetGuard barGuard(
            gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8));
        auto *bar = barGuard.get();
        addStyleClass(bar, "modernime-bottom-bar");
        gtk_widget_set_margin_start(bar, 12);
        gtk_widget_set_margin_end(bar, 12);
        gtk_widget_set_margin_top(bar, 4);
        gtk_widget_set_margin_bottom(bar, 4);

        editState = gtk_label_new("所有设置已保存");
        addStyleClass(editState, "modernime-status");
        gtk_widget_set_halign(editState, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(bar), editState, FALSE, FALSE, 0);

        taskStatus = gtk_label_new("");
        addStyleClass(taskStatus, "modernime-status");
        gtk_label_set_ellipsize(GTK_LABEL(taskStatus), PANGO_ELLIPSIZE_END);
        gtk_widget_set_halign(taskStatus, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(bar), taskStatus, TRUE, TRUE, 0);

        auto *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        const auto labels = settingsActionLabels();
        restoreButton = gtk_button_new_with_label(labels[0].data());
        applyButton = gtk_button_new_with_label(labels[1].data());
        saveButton = gtk_button_new_with_label(labels[2].data());
        addStyleClass(saveButton, kSettingsPrimaryButtonClass);
        setAccessibleWidgetText(restoreButton, labels[0],
                                "放弃当前会话内尚未保存的设置修改");
        setAccessibleWidgetText(applyButton, labels[1],
                                "保存设置但保持窗口打开，可按 Ctrl+Enter");
        setAccessibleWidgetText(saveButton, labels[2],
                                "保存设置并关闭窗口");
        for (auto *button : {restoreButton, applyButton, saveButton}) {
            gtk_box_pack_start(GTK_BOX(buttons), button, FALSE, FALSE, 0);
        }
        gtk_box_pack_end(GTK_BOX(bar), buttons, FALSE, FALSE, 0);
        g_signal_connect(restoreButton, "clicked", G_CALLBACK(onRestoreEdits),
                         this);
        g_signal_connect(applyButton, "clicked", G_CALLBACK(onApply), this);
        g_signal_connect(saveButton, "clicked", G_CALLBACK(onSaveAndClose),
                         this);
        return barGuard.release();
    }

    void updateNavigation(SettingsPageId selected) {
        for (const auto &[page, button] : navigationButtons) {
            removeStyleClass(button, kSettingsPrimaryButtonClass);
            if (page == selected) {
                addStyleClass(button, kSettingsPrimaryButtonClass);
            }
        }
    }

    void updateSearchResults() {
        clearContainer(searchResults);
        const auto query = gtk_entry_get_text(GTK_ENTRY(searchEntry));
        const auto results = searchSettings(query == nullptr ? "" : query);
        if (results.empty()) {
            gtk_popover_popdown(GTK_POPOVER(searchPopover));
            return;
        }

        constexpr std::size_t kMaximumVisibleResults = 10;
        const auto count = std::min(results.size(), kMaximumVisibleResults);
        for (std::size_t index = 0; index < count; ++index) {
            const auto &entry = results[index];
            auto *button = gtk_button_new();
            gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
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
        case SettingsPageId::Diagnostics:
            return diagnosticsPage->focusTarget(target);
        }
        return false;
    }

    void updateActionState() {
        const auto validation = model.validation();
        const bool canSave = model.dirty() && validation.valid && !actionBusy;
        gtk_widget_set_sensitive(applyButton, canSave);
        gtk_widget_set_sensitive(saveButton, canSave);
        gtk_widget_set_sensitive(restoreButton,
                                 model.dirty() && !actionBusy);

        removeStyleClass(editState, "modernime-status-dirty");
        removeStyleClass(editState, "modernime-status-error");
        if (!validation.valid) {
            addStyleClass(editState, "modernime-status-error");
            const auto message = validation.issues.empty()
                                     ? std::string("当前设置无法保存")
                                     : validation.issues.front().message;
            gtk_label_set_text(GTK_LABEL(editState), message.c_str());
        } else if (model.dirty()) {
            addStyleClass(editState, "modernime-status-dirty");
            gtk_label_set_text(GTK_LABEL(editState), "有未保存修改");
        } else if (model.reloadRequired()) {
            gtk_label_set_text(GTK_LABEL(editState),
                               "设置已保存，需要重新加载 ModernIME");
        } else {
            gtk_label_set_text(GTK_LABEL(editState), "所有设置已保存");
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
            presentError(model.reloadRequired()
                             ? "设置已保存，需要重新加载 ModernIME"
                             : "没有需要保存的修改");
            if (closeAfterSave) {
                hideWindow();
            }
            return true;
        }

        setActionBusy(true);
        std::string error;
        const bool saved = model.save(&error);
        setActionBusy(false);
        if (!saved) {
            presentError(error.empty() ? "设置保存失败，修改仍保留" : error);
            return false;
        }

        requestOverviewRefresh();
        presentError(model.reloadRequired()
                         ? "设置已保存，需要重新加载 ModernIME"
                         : "设置已保存");
        if (closeAfterSave) {
            hideWindow();
        }
        return true;
    }

    void restoreEdits() {
        model.resetEdits();
        refreshSettingsPages();
        requestOverviewRefresh();
        updateActionState();
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
        clipboardPage->refreshSettings();
        learningPage->refreshSettings();
    }

    void requestClose() {
        if (!model.dirty()) {
            hideWindow();
            return;
        }

        auto *dialog = gtk_message_dialog_new(
            GTK_WINDOW(windowOwner.get()), GTK_DIALOG_MODAL,
            GTK_MESSAGE_WARNING,
            GTK_BUTTONS_NONE, "当前有未保存的修改。");
        gtk_dialog_add_button(GTK_DIALOG(dialog), "继续编辑",
                              GTK_RESPONSE_CANCEL);
        gtk_dialog_add_button(GTK_DIALOG(dialog), "放弃修改",
                              GTK_RESPONSE_REJECT);
        gtk_dialog_add_button(GTK_DIALOG(dialog), "保存并关闭",
                              GTK_RESPONSE_ACCEPT);
        setDialogResponseAccessibility(
            GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL, "继续编辑",
            "关闭提示并继续编辑当前设置草稿");
        setDialogResponseAccessibility(
            GTK_DIALOG(dialog), GTK_RESPONSE_REJECT, "放弃修改",
            "丢弃当前会话内尚未保存的设置修改并关闭窗口");
        setDialogResponseAccessibility(
            GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, "保存并关闭",
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
            generation, paths, model.settings(), remote, environment});
        g_task_set_task_data(G_TASK(taskOwner.get()), request.release(),
                             [](gpointer value) {
            delete static_cast<OverviewTask *>(value);
        });
        g_task_run_in_thread(G_TASK(taskOwner.get()), overviewTaskFunction);
    }

    void finishOverviewRefresh(OverviewRefreshState::Generation generation,
                               const OverviewSnapshot *snapshot,
                               const GError *error) {
        const bool current = overviewRefresh.complete(generation);
        if (current && snapshot != nullptr) {
            overviewPage->setSnapshot(*snapshot);
        } else if (current) {
            presentError(error == nullptr || error->message == nullptr
                             ? "无法读取概览"
                             : error->message);
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

    GtkWidget *stack = nullptr;
    GtkWidget *searchEntry = nullptr;
    GtkWidget *searchPopover = nullptr;
    GtkWidget *searchResults = nullptr;
    GtkWidget *editState = nullptr;
    GtkWidget *taskStatus = nullptr;
    GtkWidget *restoreButton = nullptr;
    GtkWidget *applyButton = nullptr;
    GtkWidget *saveButton = nullptr;
    std::vector<std::pair<SettingsPageId, GtkWidget *>> navigationButtons;

    std::unique_ptr<OverviewPage> overviewPage;
    std::unique_ptr<InputPage> inputPage;
    std::unique_ptr<DictionaryPage> dictionaryPage;
    std::unique_ptr<ClipboardPage> clipboardPage;
    std::unique_ptr<LearningPage> learningPage;
    std::unique_ptr<DiagnosticsPage> diagnosticsPage;
    GtkWidgetOwner windowOwner;
    OverviewRefreshState overviewRefresh;
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
