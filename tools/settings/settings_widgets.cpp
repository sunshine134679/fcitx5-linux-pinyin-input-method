#include "modernime/settings/settings_widgets.h"

#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/settings_shell_state.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace modernime::settings {
namespace {

void addStyleClass(GtkWidget *widget, std::string_view className) {
    gtk_style_context_add_class(gtk_widget_get_style_context(widget),
                                std::string(className).c_str());
}

void restoreFocusFallback(GtkWidget *widget) {
    gtk_style_context_remove_class(
        gtk_widget_get_style_context(widget),
        std::string(kSettingsFocusFallbackClass).c_str());
    gtk_widget_set_can_focus(widget, FALSE);
}

gboolean onFocusFallbackOut(GtkWidget *widget, GdkEventFocus *, gpointer) {
    restoreFocusFallback(widget);
    return FALSE;
}

void onFocusFallbackUnmap(GtkWidget *widget, gpointer) {
    restoreFocusFallback(widget);
}

void ensureFocusFallbackCleanup(GtkWidget *widget) {
    constexpr const char *kCleanupInstalled =
        "modernime-focus-fallback-cleanup-installed";
    if (g_object_get_data(G_OBJECT(widget), kCleanupInstalled) != nullptr) {
        return;
    }
    g_object_set_data(G_OBJECT(widget), kCleanupInstalled,
                      GINT_TO_POINTER(1));
    g_signal_connect(widget, "focus-out-event",
                     G_CALLBACK(onFocusFallbackOut), nullptr);
    g_signal_connect(widget, "unmap", G_CALLBACK(onFocusFallbackUnmap),
                     nullptr);
}

} // namespace

void setAccessibleWidgetText(GtkWidget *widget, std::string_view name,
                             std::string_view description) {
    if (widget == nullptr) {
        return;
    }
    auto *accessible = gtk_widget_get_accessible(widget);
    if (!name.empty()) {
        atk_object_set_name(accessible, std::string(name).c_str());
    }
    if (!description.empty()) {
        atk_object_set_description(accessible,
                                   std::string(description).c_str());
    }
}

void setSettingsFocusChain(
    GtkWidget *container,
    std::initializer_list<GtkWidget *> focusableWidgets) {
    if (!GTK_IS_CONTAINER(container)) {
        return;
    }

    std::unordered_map<GtkWidget *, std::vector<GtkWidget *>> chains;
    for (auto *focusable : focusableWidgets) {
        if (focusable == nullptr || focusable == container) {
            continue;
        }
        std::vector<GtkWidget *> path;
        auto *current = focusable;
        while (current != nullptr && current != container) {
            path.push_back(current);
            current = gtk_widget_get_parent(current);
        }
        if (current != container) {
            continue;
        }

        auto *parent = container;
        for (auto item = path.rbegin(); item != path.rend(); ++item) {
            auto *child = *item;
            if (!GTK_IS_CONTAINER(parent)) {
                break;
            }
            auto &chain = chains[parent];
            if (std::find(chain.begin(), chain.end(), child) == chain.end()) {
                chain.push_back(child);
            }
            parent = child;
        }
    }

    for (const auto &[focusContainer, widgets] : chains) {
        GList *chain = nullptr;
        for (auto *widget : widgets) {
            chain = g_list_append(chain, widget);
        }
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        gtk_container_set_focus_chain(GTK_CONTAINER(focusContainer), chain);
        G_GNUC_END_IGNORE_DEPRECATIONS
        g_list_free(chain);
    }
}

void prependSettingsFocusChainChild(GtkWidget *container, GtkWidget *child) {
    if (!GTK_IS_CONTAINER(container) || child == nullptr ||
        gtk_widget_get_parent(child) != container) {
        return;
    }
    GList *chain = nullptr;
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    const bool hasChain =
        gtk_container_get_focus_chain(GTK_CONTAINER(container), &chain);
    G_GNUC_END_IGNORE_DEPRECATIONS
    if (!hasChain) {
        g_list_free(chain);
        return;
    }
    chain = g_list_remove(chain, child);
    chain = g_list_prepend(chain, child);
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gtk_container_set_focus_chain(GTK_CONTAINER(container), chain);
    G_GNUC_END_IGNORE_DEPRECATIONS
    g_list_free(chain);
}

void setDialogResponseAccessibility(GtkDialog *dialog, int response,
                                    std::string_view name,
                                    std::string_view description) {
    if (dialog == nullptr) {
        return;
    }
    auto *button = gtk_dialog_get_widget_for_response(dialog, response);
    if (GTK_IS_BUTTON(button) && !name.empty()) {
        gtk_button_set_label(GTK_BUTTON(button), std::string(name).c_str());
    }
    setAccessibleWidgetText(button, name, description);
}

std::string_view settingsStyles() {
    return R"css(
        .modernime-settings {
            background-color: @theme_bg_color;
            color: @theme_fg_color;
        }

        .modernime-sidebar {
            border-right: 1px solid @borders;
            background-color: @theme_bg_color;
            padding: 8px 8px;
        }

        .modernime-sidebar-group {
            font-size: 11px;
            font-weight: 700;
            color: @insensitive_fg_color;
            margin-top: 6px;
            margin-bottom: 2px;
            margin-left: 8px;
            letter-spacing: 0.5px;
        }

        .modernime-search-box {
            border-radius: 8px;
            padding: 3px 6px;
            margin-bottom: 4px;
        }

        .modernime-nav-button {
            border-radius: 8px;
            padding: 3px 6px;
            margin: 1px 0;
            border: 1px solid transparent;
            color: @theme_fg_color;
            transition: all 120ms ease;
        }

        .modernime-nav-button:hover {
            background-color: rgba(128, 128, 128, 0.12);
        }

        .modernime-nav-button.suggested-action,
        .modernime-nav-button.suggested-action:hover,
        .modernime-nav-button.suggested-action:active,
        .modernime-nav-button.suggested-action:focus,
        .modernime-nav-button.suggested-action:focus:hover,
        .modernime-nav-button.suggested-action:focus:active,
        .modernime-nav-button:checked,
        .modernime-nav-button:checked:hover,
        .modernime-nav-button:checked:active,
        .modernime-nav-button:checked:focus {
            background-color: @theme_selected_bg_color;
            background-image: none;
            color: @theme_selected_fg_color;
            border: 1px solid transparent;
            outline: none;
            box-shadow: none;
            font-weight: 600;
        }

        .modernime-nav-button.suggested-action image,
        .modernime-nav-button.suggested-action label,
        .modernime-nav-button.suggested-action *,
        .modernime-nav-button:checked image,
        .modernime-nav-button:checked label,
        .modernime-nav-button:checked * {
            color: @theme_selected_fg_color;
        }

        .modernime-page,
        .modernime-page-scroller,
        .modernime-page-viewport,
        .modernime-page-scroller > viewport,
        .modernime-page-stack,
        .modernime-page-surface {
            background-color: @theme_bg_color;
            border: none;
        }

        .modernime-page-title {
            font-size: 22px;
            font-weight: 700;
            color: @theme_fg_color;
        }

        .modernime-page-subtitle {
            font-size: 13px;
            color: @insensitive_fg_color;
            margin-top: 2px;
            margin-bottom: 6px;
        }

        .modernime-description,
        .modernime-path {
            color: @insensitive_fg_color;
            font-size: 12.5px;
        }

        .modernime-section {
            border: 1px solid @borders;
            border-radius: 12px;
            background-color: @theme_base_color;
            padding: 18px 20px;
            margin-bottom: 14px;
        }

        .modernime-section-title {
            font-size: 15px;
            font-weight: 700;
            color: @theme_fg_color;
        }

        .modernime-setting-row {
            padding: 10px 4px;
            border-bottom: 1px solid rgba(128, 128, 128, 0.12);
        }

        .modernime-setting-row:last-child {
            border-bottom: none;
        }

        .modernime-setting-title {
            font-size: 13.5px;
            font-weight: 600;
            color: @theme_fg_color;
        }

        .modernime-setting-desc {
            font-size: 12px;
            color: @insensitive_fg_color;
        }

        /* Overview Bento Cards */
        .modernime-hero-card {
            border: 1px solid @borders;
            border-radius: 14px;
            background-color: @theme_base_color;
            padding: 12px 18px;
            margin-bottom: 10px;
        }

        .modernime-hero-title {
            font-size: 16px;
            font-weight: 700;
            color: @theme_fg_color;
        }

        .modernime-bento-card {
            border: 1px solid @borders;
            border-radius: 12px;
            background-color: @theme_base_color;
            padding: 12px 16px;
        }

        .modernime-bento-title {
            font-size: 12px;
            font-weight: 600;
            color: @insensitive_fg_color;
        }

        .modernime-bento-value {
            font-size: 18px;
            font-weight: 700;
            color: @theme_fg_color;
            margin: 3px 0 2px 0;
        }

        .modernime-bento-desc {
            font-size: 11.5px;
            color: @insensitive_fg_color;
            margin-bottom: 8px;
        }

        /* Status & Badges */
        .modernime-status {
            padding: 4px 10px;
            border-radius: 8px;
        }

        .modernime-badge-green {
            background-color: rgba(34, 197, 94, 0.15);
            color: #16a34a;
            font-weight: 600;
            border-radius: 16px;
            padding: 4px 12px;
        }

        .modernime-badge-amber {
            background-color: rgba(245, 158, 11, 0.15);
            color: #d97706;
            font-weight: 600;
            border-radius: 16px;
            padding: 4px 12px;
        }

        .modernime-badge-neutral {
            background-color: rgba(128, 128, 128, 0.12);
            color: @theme_fg_color;
            font-weight: 500;
            border-radius: 8px;
            padding: 4px 10px;
        }

        .modernime-status-dirty {
            color: @warning_color;
            font-weight: 600;
        }

        .modernime-status-error,
        entry.error {
            color: @error_color;
        }

        .modernime-focus-fallback {
            border-radius: 8px;
            box-shadow: inset 0 0 0 2px @theme_selected_bg_color;
        }

        entry.error {
            border-color: @error_color;
        }

        /* Bottom Bar */
        .modernime-bottom-bar {
            border-top: 1px solid @borders;
            background-color: @theme_bg_color;
            padding: 4px 14px;
        }

        /* Preview Box */
        .modernime-preview-bar {
            background-color: rgba(128, 128, 128, 0.08);
            border: 1px dashed @borders;
            border-radius: 10px;
            padding: 12px 16px;
            margin-top: 8px;
        }

        button {
            border-radius: 8px;
            padding: 5px 12px;
            transition: all 120ms ease;
        }

        button.suggested-action,
        button.suggested-action:hover,
        button.suggested-action:active,
        button.suggested-action:focus,
        button.suggested-action:focus:hover,
        button.suggested-action:focus:active {
            background-color: @theme_selected_bg_color;
            background-image: none;
            color: @theme_selected_fg_color;
            border-color: transparent;
            outline: none;
            box-shadow: none;
            font-weight: 600;
        }

        button.suggested-action *,
        button.suggested-action:hover *,
        button.suggested-action:active *,
        button.suggested-action:focus * {
            color: @theme_selected_fg_color;
        }
    )css";
}

void installSettingsStyles() {
    auto *screen = gdk_screen_get_default();
    if (screen == nullptr) {
        return;
    }
    auto *provider = gtk_css_provider_new();
    const auto styles = settingsStyles();
    gtk_css_provider_load_from_data(provider, styles.data(),
                                    static_cast<gssize>(styles.size()),
                                    nullptr);
    gtk_style_context_add_provider_for_screen(
        screen, GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_USER);
    g_object_unref(provider);
}

GtkWidget *createPageShell(std::string_view title, std::string_view subtitle) {
    detail::GtkWidgetGuard pageGuard(
        gtk_box_new(GTK_ORIENTATION_VERTICAL, 12));
    auto *page = pageGuard.get();
    addStyleClass(page, "modernime-page");
    gtk_widget_set_margin_start(page, 24);
    gtk_widget_set_margin_end(page, 24);
    gtk_widget_set_margin_top(page, 16);
    gtk_widget_set_margin_bottom(page, 16);
    setAccessibleWidgetText(page, title, subtitle);

    auto *heading = gtk_label_new(std::string(title).c_str());
    addStyleClass(heading, "modernime-page-title");
    gtk_widget_set_halign(heading, GTK_ALIGN_START);
    setAccessibleWidgetText(heading, title, "页面标题");
    gtk_box_pack_start(GTK_BOX(page), heading, FALSE, FALSE, 0);

    auto *description = gtk_label_new(std::string(subtitle).c_str());
    addStyleClass(description, "modernime-page-subtitle");
    gtk_widget_set_halign(description, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(description), TRUE);
    setAccessibleWidgetText(description, subtitle, "页面说明");
    gtk_box_pack_start(GTK_BOX(page), description, FALSE, FALSE, 0);
    return pageGuard.release();
}

GtkWidget *createSectionCard(std::string_view title,
                             std::string_view description) {
    detail::GtkWidgetGuard cardGuard(
        gtk_box_new(GTK_ORIENTATION_VERTICAL, 12));
    auto *card = cardGuard.get();
    addStyleClass(card, kSettingsSectionClass);
    setAccessibleWidgetText(card, title, description);
    if (!title.empty()) {
        auto *heading = gtk_label_new(std::string(title).c_str());
        addStyleClass(heading, "modernime-section-title");
        gtk_widget_set_halign(heading, GTK_ALIGN_START);
        setAccessibleWidgetText(heading, title, "设置分组标题");
        gtk_box_pack_start(GTK_BOX(card), heading, FALSE, FALSE, 0);
    }
    if (!description.empty()) {
        auto *help = gtk_label_new(std::string(description).c_str());
        addStyleClass(help, "modernime-description");
        gtk_widget_set_halign(help, GTK_ALIGN_START);
        gtk_label_set_line_wrap(GTK_LABEL(help), TRUE);
        setAccessibleWidgetText(help, description, "设置分组说明");
        gtk_box_pack_start(GTK_BOX(card), help, FALSE, FALSE, 0);
    }
    return cardGuard.release();
}

GtkWidget *createScrollablePage(GtkWidget *page) {
    detail::GtkWidgetGuard pageGuard(page);
    detail::GtkWidgetGuard surfaceGuard(gtk_event_box_new());
    auto *surface = surfaceGuard.get();
    addStyleClass(surface, "modernime-page-surface");
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(surface), TRUE);
    gtk_widget_set_hexpand(surface, TRUE);
    gtk_widget_set_vexpand(surface, TRUE);

    detail::GtkWidgetGuard scrolledGuard(
        gtk_scrolled_window_new(nullptr, nullptr));
    auto *scrolled = scrolledGuard.get();
    addStyleClass(scrolled, "modernime-page-scroller");
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scrolled),
                                        GTK_SHADOW_NONE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand(scrolled, TRUE);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_widget_set_hexpand(page, TRUE);
    gtk_widget_set_vexpand(page, TRUE);
    gtk_widget_set_halign(page, GTK_ALIGN_FILL);
    gtk_widget_set_valign(page, GTK_ALIGN_FILL);
    gtk_container_add(GTK_CONTAINER(surface), pageGuard.get());
    pageGuard.release();
    gtk_container_add(GTK_CONTAINER(scrolled), surface);
    surfaceGuard.release();
    auto *viewport = gtk_bin_get_child(GTK_BIN(scrolled));
    if (viewport != nullptr) {
        addStyleClass(viewport, "modernime-page-viewport");
    }
    return scrolledGuard.release();
}

GtkWidget *createSettingRow(std::string_view title,
                            std::string_view description,
                            GtkWidget *control) {
    detail::GtkWidgetGuard controlGuard(control);
    detail::GtkWidgetGuard rowGuard(
        gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16));
    auto *row = rowGuard.get();
    addStyleClass(row, "modernime-setting-row");
    setAccessibleWidgetText(row, title, description);

    auto *textVBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_valign(textVBox, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(textVBox, TRUE);

    auto *label = gtk_label_new(std::string(title).c_str());
    addStyleClass(label, "modernime-setting-title");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    setAccessibleWidgetText(label, title, "设置名称");
    gtk_box_pack_start(GTK_BOX(textVBox), label, FALSE, FALSE, 0);

    if (!description.empty()) {
        auto *help = gtk_label_new(std::string(description).c_str());
        addStyleClass(help, "modernime-setting-desc");
        gtk_widget_set_halign(help, GTK_ALIGN_START);
        gtk_label_set_line_wrap(GTK_LABEL(help), TRUE);
        setAccessibleWidgetText(help, description, "设置说明");
        gtk_box_pack_start(GTK_BOX(textVBox), help, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(row), textVBox, TRUE, TRUE, 0);

    if (control != nullptr) {
        gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(control, GTK_ALIGN_END);
        setAccessibleWidgetText(control, title, description);
        gtk_box_pack_end(GTK_BOX(row), control, FALSE, FALSE, 0);
        controlGuard.release();
    }
    return rowGuard.release();
}

GtkWidget *createStatusPill(std::string_view text) {
    detail::GtkWidgetGuard statusGuard(
        gtk_label_new(std::string(text).c_str()));
    auto *status = statusGuard.get();
    addStyleClass(status, "modernime-status");
    gtk_widget_set_halign(status, GTK_ALIGN_START);
    setAccessibleWidgetText(status, text, "当前状态");
    return statusGuard.release();
}

GtkWidget *createEmptyState(std::string_view title,
                            std::string_view description) {
    detail::GtkWidgetGuard stateGuard(
        gtk_box_new(GTK_ORIENTATION_VERTICAL, 12));
    auto *state = stateGuard.get();
    setAccessibleWidgetText(state, title, description);
    auto *heading = gtk_label_new(std::string(title).c_str());
    gtk_widget_set_halign(heading, GTK_ALIGN_START);
    setAccessibleWidgetText(heading, title, "空状态标题");
    gtk_box_pack_start(GTK_BOX(state), heading, FALSE, FALSE, 0);
    if (!description.empty()) {
        auto *help = gtk_label_new(std::string(description).c_str());
        addStyleClass(help, "modernime-description");
        gtk_widget_set_halign(help, GTK_ALIGN_START);
        gtk_label_set_line_wrap(GTK_LABEL(help), TRUE);
        setAccessibleWidgetText(help, description, "空状态说明");
        gtk_box_pack_start(GTK_BOX(state), help, FALSE, FALSE, 0);
    }
    return stateGuard.release();
}

bool focusWidgetOrFallback(GtkWidget *target, GtkWidget *fallback) {
    const auto focusable = [](GtkWidget *widget) {
        return widget != nullptr && gtk_widget_get_visible(widget) &&
               gtk_widget_is_sensitive(widget) &&
               gtk_widget_get_can_focus(widget);
    };
    const auto grabAndVerify = [](GtkWidget *widget) {
        gtk_widget_grab_focus(widget);
        return gtk_widget_has_focus(widget);
    };

    const auto fallbackAvailable = [](GtkWidget *widget) {
        return widget != nullptr && gtk_widget_get_visible(widget) &&
               gtk_widget_is_sensitive(widget);
    };
    const auto destination = chooseSettingsFocusDestination(
        focusable(target), fallbackAvailable(fallback));
    if (destination == SettingsFocusDestination::Target &&
        grabAndVerify(target)) {
        return true;
    }
    if (fallbackAvailable(fallback)) {
        gtk_widget_set_can_focus(fallback, TRUE);
        addStyleClass(fallback, kSettingsFocusFallbackClass);
        if (grabAndVerify(fallback)) {
            ensureFocusFallbackCleanup(fallback);
            return true;
        }
        restoreFocusFallback(fallback);
    }
    return false;
}

} // namespace modernime::settings
