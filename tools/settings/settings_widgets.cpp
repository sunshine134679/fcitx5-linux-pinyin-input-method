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
    if (!GTK_IS_CONTAINER(container) || child == nullptr) {
        return;
    }
    std::vector<GtkWidget *> path;
    auto *current = child;
    while (current != nullptr && current != container) {
        path.push_back(current);
        current = gtk_widget_get_parent(current);
    }
    if (current != container || path.empty()) {
        return;
    }
    auto *parent = container;
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        auto *item = *it;
        if (!GTK_IS_CONTAINER(parent)) {
            break;
        }
        GList *chain = nullptr;
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        const bool hasChain =
            gtk_container_get_focus_chain(GTK_CONTAINER(parent), &chain);
        G_GNUC_END_IGNORE_DEPRECATIONS
        if (hasChain && chain != nullptr) {
            chain = g_list_remove(chain, item);
            chain = g_list_prepend(chain, item);
        } else {
            chain = g_list_append(nullptr, item);
        }
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        gtk_container_set_focus_chain(GTK_CONTAINER(parent), chain);
        G_GNUC_END_IGNORE_DEPRECATIONS
        g_list_free(chain);
        parent = item;
    }
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
        /* Fixed light palette from the approved client references. */
        @define-color theme_bg_color #ffffff;
        @define-color theme_base_color #ffffff;
        @define-color theme_fg_color #182031;
        @define-color insensitive_fg_color #657184;
        @define-color borders #dfe4ec;
        @define-color theme_selected_bg_color #1262ff;
        @define-color theme_selected_fg_color #ffffff;
        @define-color warning_color #966400;
        @define-color error_color #c23838;
        .modernime-settings {
            background-color: @theme_bg_color;
            color: @theme_fg_color;
            border-radius: 12px;
            font-family: "Noto Sans", "Noto Sans CJK SC", sans-serif;
            font-size: 14px;
        }

        .modernime-sidebar {
            border-right: 1px solid @borders;
            background-color: #f6f7f9;
            padding: 8px 8px;
        }

        .modernime-sidebar viewport {
            background-color: transparent;
            border: none;
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
            background-image: none;
            background-color: #ffffff;
            border: 1px solid #d7dee8;
            color: @theme_fg_color;
            padding: 7px 8px;
            margin-bottom: 4px;
        }

        .modernime-nav-button {
            border-radius: 8px;
            background-image: none;
            background-color: transparent;
            box-shadow: none;
            padding: 6px 6px;
            margin: 1px 0;
            border: 1px solid transparent;
            color: @theme_fg_color;
            transition: background-color 150ms ease, border-color 150ms ease;
        }

        .modernime-nav-button:hover {
            background-color: #edf0f5;
        }

        .modernime-settings .modernime-nav-button.modernime-nav-selected {
            background-color: #e5edff;
            background-image: none;
            color: @theme_selected_bg_color;
            outline-color: @theme_selected_bg_color;
            box-shadow: none;
            font-weight: 600;
        }
        .modernime-settings .modernime-nav-button.modernime-nav-selected:hover,
        .modernime-settings .modernime-nav-button.modernime-nav-selected:active {
            background-color: #dbe7ff;
        }
        .modernime-nav-button.modernime-nav-selected * {
            color: @theme_selected_bg_color;
        }

        .modernime-code-block {
            background-color: #f6f7f9;
            border: 1px solid @borders;
            border-radius: 8px;
            padding: 12px;
            color: #34445c;
            font-family: monospace;
            font-size: 12px;
        }

        .modernime-update-notice {
            background-color: #eef4ff;
            border: 1px solid #dce7ff;
            border-radius: 8px;
            padding: 12px;
            color: #284870;
        }
        .modernime-update-notice label, .modernime-update-notice image {
            color: #284870;
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
            font-size: 26px;
            font-weight: 700;
            color: @theme_fg_color;
            letter-spacing: -0.2px;
        }

        .modernime-page-subtitle {
            font-size: 13px;
            color: @insensitive_fg_color;
            margin-top: 1px;
            margin-bottom: 0;
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
            padding: 14px 18px;
            margin-bottom: 4px;
        }

        .modernime-section > .modernime-description {
            margin-bottom: 12px;
        }

        .modernime-section-title {
            margin-bottom: 4px;
            font-size: 16px;
            font-weight: 600;
            color: @theme_fg_color;
        }

        .modernime-setting-row {
            padding: 10px 0;
            border-bottom: 1px solid #e6eaf0;
        }

        .modernime-setting-row:last-child {
            border-bottom: none;
        }

        .modernime-setting-title {
            font-size: 15px;
            font-weight: 600;
            color: @theme_fg_color;
        }

        .modernime-setting-desc {
            font-size: 12.5px;
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
            border-radius: 6px;
        }

        .modernime-status-banner {
            background-color: #f0f6ff;
            border: 1px solid #d0e1fd;
            border-radius: 8px;
            padding: 9px 13px;
            margin-bottom: 14px;
        }
        .modernime-status-banner label {
            color: #1e3a8a;
            font-size: 13px;
            font-weight: 500;
        }
        .modernime-status-banner image {
            color: #2563eb;
        }
        .modernime-status-banner.error {
            background-color: #fef2f2;
            border-color: #fecaca;
        }
        .modernime-status-banner.error label {
            color: #991b1b;
        }
        .modernime-status-banner.error image {
            color: #dc2626;
        }
        .modernime-status-banner.warning {
            background-color: #fffbeb;
            border-color: #fde68a;
        }
        .modernime-status-banner.warning label {
            color: #92400e;
        }
        .modernime-status-banner.warning image {
            color: #d97706;
        }
        .modernime-status-banner.success {
            background-color: #f0fdf4;
            border-color: #bbf7d0;
        }
        .modernime-status-banner.success label {
            color: #166534;
        }
        .modernime-status-banner.success image {
            color: #16a34a;
        }

        .modernime-badge-green {
            background-color: rgba(34, 197, 94, 0.12);
            color: #15803d;
            font-weight: 600;
            font-size: 12.5px;
            border-radius: 6px;
            padding: 3px 8px;
        }

        .modernime-badge-amber {
            background-color: rgba(245, 158, 11, 0.12);
            color: #b45309;
            font-weight: 600;
            font-size: 12.5px;
            border-radius: 6px;
            padding: 3px 8px;
        }

        .modernime-badge-red {
            background-color: rgba(239, 68, 68, 0.12);
            color: #b91c1c;
            font-weight: 600;
            font-size: 12.5px;
            border-radius: 6px;
            padding: 3px 8px;
        }

        .modernime-badge-neutral {
            background-color: rgba(100, 116, 139, 0.10);
            color: #334155;
            font-weight: 500;
            font-size: 12.5px;
            border-radius: 6px;
            padding: 3px 8px;
        }

        .modernime-status-dirty {
            color: @theme_selected_bg_color;
            font-weight: 600;
        }

        .modernime-status-error,
        entry.error,
        button.modernime-shortcut-btn.error {
            color: @error_color;
        }

        .modernime-focus-fallback {
            border-radius: 8px;
            box-shadow: inset 0 0 0 2px @theme_selected_bg_color;
        }

        entry.error,
        button.modernime-shortcut-btn.error {
            border-color: @error_color;
        }

        /* Bottom Bar */
        .modernime-bottom-bar {
            border-top: 1px solid @borders;
            background-color: @theme_bg_color;
            border-radius: 0 0 12px 12px;
            padding: 14px 28px;
        }

        /* Preview Box */
        .modernime-preview-bar {
            background-color: rgba(128, 128, 128, 0.08);
            border: 1px dashed @borders;
            border-radius: 10px;
            padding: 12px 16px;
            margin-top: 8px;
        }

        /* Clipboard History Cards & Color Blocks */
        .modernime-clipboard-list { background-color: transparent; }
        .modernime-clipboard-list row {
            background-color: transparent;
            padding: 3px 0;
            border: none;
            color: #172b4d;
        }
        .modernime-clipboard-list row:selected { background-color: transparent; }
        .modernime-clipboard-card {
            border: 1px solid #e4eaf3;
            border-left: 3px solid transparent;
            border-radius: 8px;
            background-color: #ffffff;
            padding: 9px 12px;
            margin: 1px 2px;
            transition: background-color 140ms ease, border-color 140ms ease;
        }
        .modernime-clipboard-card:hover {
            background-color: #f7f9fd;
            border-color: #dbe4f3;
        }
        .modernime-clipboard-list row:selected .modernime-clipboard-card {
            border-color: #d5e2ff;
            border-left-color: #1262ff;
            background-color: #f2f6ff;
            box-shadow: none;
        }
        .modernime-card-index {
            min-width: 20px;
            font-size: 11px;
            font-weight: 600;
            color: #7a89a0;
        }
        .modernime-badge-code, .modernime-badge-url,
        .modernime-badge-multiline, .modernime-badge-text {
            font-size: 12px;
            color: #627693;
        }
        .modernime-card-meta { font-size: 11px; color: #7a89a0; }
        .modernime-card-preview { font-size: 14px; color: #172b4d; }
        .modernime-card-expanded-box {
            border-radius: 6px;
            border: 1px solid #dce5f3;
            background-color: #ffffff;
        }
        .modernime-clipboard-reader, .modernime-clipboard-reader text {
            font-size: 14px;
            color: #172b4d;
            background-color: #ffffff;
        }
        .modernime-card-code {
            font-family: monospace;
            font-size: 13px;
            color: #172b4d;
        }
        .modernime-toggle-btn, .modernime-card-btn {
            font-size: 12px;
            min-height: 22px;
            padding: 2px 8px;
            border-radius: 5px;
            color: #1262ff;
            border: 1px solid transparent;
            background-image: none;
            background-color: transparent;
            box-shadow: none;
        }
        .modernime-toggle-btn:hover, .modernime-card-btn:hover {
            background-color: #e7eeff;
            border-color: #cbdcff;
        }
        .modernime-toggle-btn:focus, .modernime-card-btn:focus {
            border-color: #1262ff;
        }
        .modernime-toggle-btn label, .modernime-card-btn label { color: #1262ff; }
        .modernime-clipboard-toolbar { margin: 4px 0 8px; }

        .modernime-shortcut-btn {
            font-family: monospace, DejaVu Sans Mono, Liberation Mono, sans-serif;
            font-size: 13px;
            font-weight: 600;
            padding: 6px 16px;
            min-width: 140px;
            border-radius: 8px;
            border: 1px solid @borders;
            background-color: rgba(128, 128, 128, 0.08);
            color: @theme_fg_color;
            transition: all 120ms ease;
        }

        .modernime-shortcut-btn:hover {
            background-color: rgba(128, 128, 128, 0.15);
            border-color: rgba(128, 128, 128, 0.4);
        }

        .modernime-shortcut-btn.recording {
            border-color: @theme_selected_bg_color;
            background-color: rgba(59, 130, 246, 0.12);
            color: @theme_selected_bg_color;
            box-shadow: inset 0 0 0 1px @theme_selected_bg_color;
        }

        /* Candidates Hero Preview & Controls matching layout-1.png */
        .modernime-preview-card {
            border: 1px solid @borders;
            border-radius: 12px;
            background-color: #f6f7f9;
            padding: 14px 18px;
            margin-bottom: 4px;
        }

        .modernime-preview-header {
            font-size: 13px;
            font-weight: 600;
            color: @insensitive_fg_color;
            margin-bottom: 6px;
        }

        .modernime-composition-text {
            font-size: 20px;
            font-weight: 500;
            color: @theme_fg_color;
            margin-bottom: 8px;
        }

        .modernime-tip-note {
            font-size: 12px;
            color: @insensitive_fg_color;
            background-color: #f6f7f9;
            border-radius: 8px;
            padding: 10px 12px;
            margin-top: 4px;
        }

        .modernime-sidebar-footer {
            font-size: 11.5px;
            color: @insensitive_fg_color;
        }

        button {
            border-radius: 8px;
            background-image: none;
            background-color: #ffffff;
            border: 1px solid @borders;
            color: @theme_fg_color;
            padding: 6px 12px;
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
            outline-color: @theme_selected_bg_color;
            box-shadow: none;
            font-weight: 600;
        }

        button.suggested-action *,
        button.suggested-action:hover *,
        button.suggested-action:active *,
        button.suggested-action:focus * {
            color: @theme_selected_fg_color;
        }

        .modernime-nav-label { font-size: 15px; }
        .modernime-brand { font-size: 24px; font-weight: 700; }
        .modernime-settings entry,
        .modernime-settings spinbutton,
        .modernime-settings treeview,
        .modernime-settings textview text {
            background-image: none;
            background-color: #ffffff;
            color: @theme_fg_color;
            border-color: @borders;
        }
        .modernime-settings entry:focus,
        .modernime-settings spinbutton:focus {
            border-color: @theme_selected_bg_color;
            box-shadow: inset 0 0 0 1px alpha(@theme_selected_bg_color, 0.15);
        }
        .modernime-settings button:hover { background-color: #f1f4f9; }
        .modernime-settings button.suggested-action:hover,
        .modernime-settings button.suggested-action:active {
            background-color: @theme_selected_bg_color;
        }
        .modernime-settings button:disabled {
            background-color: #f4f6f9;
            border-color: #e5e9ef;
            color: #9aa3b2;
        }
        .modernime-settings button.suggested-action:disabled {
            background-color: #e7efff;
            border-color: #e7efff;
            color: #7193d5;
        }
        .modernime-settings button:disabled label { color: inherit; }
        .modernime-settings .modernime-bottom-bar button { min-width: 84px; }
        .modernime-settings switch {
            background-image: none;
            background-color: #b7bec9;
            border: none;
            border-radius: 16px;
            min-width: 42px;
            min-height: 24px;
            color: transparent;
        }
        .modernime-settings switch:checked { background-color: @theme_selected_bg_color; }
        .modernime-settings switch slider {
            background-image: none;
            background-color: #ffffff;
            border: none;
            border-radius: 50%;
            min-width: 20px;
            min-height: 20px;
            margin: 2px;
            box-shadow: 0 1px 2px alpha(#182031, 0.12);
        }
        .modernime-settings checkbutton check:checked,
        .modernime-settings radiobutton radio:checked {
            background-image: none;
            background-color: @theme_selected_bg_color;
            border-color: @theme_selected_bg_color;
            color: #ffffff;
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

GtkWidget *createPageShell(std::string_view title, std::string_view subtitle,
                           GtkWidget *headerAction) {
    detail::GtkWidgetGuard actionGuard(headerAction);
    detail::GtkWidgetGuard pageGuard(
        gtk_box_new(GTK_ORIENTATION_VERTICAL, 12));
    auto *page = pageGuard.get();
    addStyleClass(page, "modernime-page");
    gtk_widget_set_margin_start(page, 24);
    gtk_widget_set_margin_end(page, 24);
    gtk_widget_set_margin_top(page, 20);
    gtk_widget_set_margin_bottom(page, 18);
    setAccessibleWidgetText(page, title, subtitle);

    auto *headerBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_bottom(headerBox, 6);
    g_object_set_data(G_OBJECT(page), "modernime-page-header", headerBox);

    auto *titlesBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand(titlesBox, TRUE);

    auto *heading = gtk_label_new(std::string(title).c_str());
    addStyleClass(heading, "modernime-page-title");
    gtk_widget_set_halign(heading, GTK_ALIGN_START);
    setAccessibleWidgetText(heading, title, "页面标题");
    gtk_box_pack_start(GTK_BOX(titlesBox), heading, FALSE, FALSE, 0);

    if (!subtitle.empty()) {
        auto *description = gtk_label_new(std::string(subtitle).c_str());
        addStyleClass(description, "modernime-page-subtitle");
        gtk_widget_set_halign(description, GTK_ALIGN_START);
        gtk_label_set_line_wrap(GTK_LABEL(description), TRUE);
        setAccessibleWidgetText(description, subtitle, "页面说明");
        gtk_box_pack_start(GTK_BOX(titlesBox), description, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(headerBox), titlesBox, TRUE, TRUE, 0);

    if (headerAction != nullptr) {
        gtk_widget_set_valign(headerAction, GTK_ALIGN_START);
        gtk_widget_set_halign(headerAction, GTK_ALIGN_END);
        gtk_box_pack_end(GTK_BOX(headerBox), actionGuard.release(), FALSE, FALSE, 0);
    }

    gtk_box_pack_start(GTK_BOX(page), headerBox, FALSE, FALSE, 0);
    return pageGuard.release();
}

void setPageHeaderAction(GtkWidget *page, GtkWidget *action) {
    if (page == nullptr || action == nullptr) {
        return;
    }
    auto *headerBox = static_cast<GtkWidget *>(
        g_object_get_data(G_OBJECT(page), "modernime-page-header"));
    if (headerBox != nullptr && GTK_IS_BOX(headerBox)) {
        gtk_widget_set_valign(action, GTK_ALIGN_START);
        gtk_widget_set_halign(action, GTK_ALIGN_END);
        gtk_box_pack_end(GTK_BOX(headerBox), action, FALSE, FALSE, 0);
    }
}

GtkWidget *createSectionCard(std::string_view title,
                             std::string_view description) {
    detail::GtkWidgetGuard cardGuard(
        gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
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
        gtk_widget_set_halign(help, GTK_ALIGN_FILL);
        gtk_label_set_xalign(GTK_LABEL(help), 0);
        gtk_label_set_line_wrap(GTK_LABEL(help), TRUE);
        gtk_widget_set_margin_bottom(help, 10);
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
        gtk_widget_set_halign(help, GTK_ALIGN_FILL);
        gtk_label_set_xalign(GTK_LABEL(help), 0);
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
        gtk_widget_set_halign(help, GTK_ALIGN_FILL);
        gtk_label_set_xalign(GTK_LABEL(help), 0);
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
