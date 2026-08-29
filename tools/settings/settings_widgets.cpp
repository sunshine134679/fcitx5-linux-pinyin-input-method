#include "modernime/settings/settings_widgets.h"

#include <string>

namespace modernime::settings {
namespace {

void addStyleClass(GtkWidget *widget, std::string_view className) {
    gtk_style_context_add_class(gtk_widget_get_style_context(widget),
                                std::string(className).c_str());
}

void setAccessibleText(GtkWidget *widget, std::string_view name,
                       std::string_view description = {}) {
    auto *accessible = gtk_widget_get_accessible(widget);
    if (!name.empty()) {
        atk_object_set_name(accessible, std::string(name).c_str());
    }
    if (!description.empty()) {
        atk_object_set_description(accessible, std::string(description).c_str());
    }
}

} // namespace

std::string_view settingsStyles() {
    return R"css(
        .modernime-sidebar {
            border-right: 1px solid @borders;
            background-color: @theme_bg_color;
            padding: 12px;
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
            font-size: 20px;
            font-weight: 600;
        }
        .modernime-page-subtitle,
        .modernime-description,
        .modernime-path {
            color: @insensitive_fg_color;
        }
        .modernime-section {
            border: 1px solid @borders;
            border-radius: 12px;
            background-color: @theme_base_color;
            padding: 16px;
        }
        .modernime-section-title {
            font-weight: 600;
        }
        .modernime-status {
            padding: 4px 8px;
        }
        .modernime-status-dirty {
            color: @warning_color;
            font-weight: 600;
        }
        .modernime-status-error,
        entry.error {
            color: @error_color;
        }
        entry.error {
            border-color: @error_color;
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
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

GtkWidget *createPageShell(std::string_view title, std::string_view subtitle) {
    auto *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    addStyleClass(page, "modernime-page");
    gtk_widget_set_margin_start(page, 24);
    gtk_widget_set_margin_end(page, 24);
    gtk_widget_set_margin_top(page, 24);
    gtk_widget_set_margin_bottom(page, 24);
    setAccessibleText(page, title, subtitle);

    auto *heading = gtk_label_new(std::string(title).c_str());
    addStyleClass(heading, "modernime-page-title");
    gtk_widget_set_halign(heading, GTK_ALIGN_START);
    setAccessibleText(heading, title);
    gtk_box_pack_start(GTK_BOX(page), heading, FALSE, FALSE, 0);

    auto *description = gtk_label_new(std::string(subtitle).c_str());
    addStyleClass(description, "modernime-page-subtitle");
    gtk_widget_set_halign(description, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(description), TRUE);
    setAccessibleText(description, subtitle);
    gtk_box_pack_start(GTK_BOX(page), description, FALSE, FALSE, 0);
    return page;
}

GtkWidget *createSectionCard(std::string_view title,
                             std::string_view description) {
    auto *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    addStyleClass(card, kSettingsSectionClass);
    setAccessibleText(card, title, description);
    if (!title.empty()) {
        auto *heading = gtk_label_new(std::string(title).c_str());
        addStyleClass(heading, "modernime-section-title");
        gtk_widget_set_halign(heading, GTK_ALIGN_START);
        setAccessibleText(heading, title);
        gtk_box_pack_start(GTK_BOX(card), heading, FALSE, FALSE, 0);
    }
    if (!description.empty()) {
        auto *help = gtk_label_new(std::string(description).c_str());
        addStyleClass(help, "modernime-description");
        gtk_widget_set_halign(help, GTK_ALIGN_START);
        gtk_label_set_line_wrap(GTK_LABEL(help), TRUE);
        setAccessibleText(help, description);
        gtk_box_pack_start(GTK_BOX(card), help, FALSE, FALSE, 0);
    }
    return card;
}

GtkWidget *createScrollablePage(GtkWidget *page) {
    auto *surface = gtk_event_box_new();
    addStyleClass(surface, "modernime-page-surface");
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(surface), TRUE);
    gtk_widget_set_hexpand(surface, TRUE);
    gtk_widget_set_vexpand(surface, TRUE);

    auto *scrolled = gtk_scrolled_window_new(nullptr, nullptr);
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
    gtk_container_add(GTK_CONTAINER(surface), page);
    gtk_container_add(GTK_CONTAINER(scrolled), surface);
    auto *viewport = gtk_bin_get_child(GTK_BIN(scrolled));
    if (viewport != nullptr) {
        addStyleClass(viewport, "modernime-page-viewport");
    }
    return scrolled;
}

GtkWidget *createSettingRow(std::string_view title,
                            std::string_view description,
                            GtkWidget *control) {
    auto *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    setAccessibleText(row, title, description);
    auto *label = gtk_label_new(std::string(title).c_str());
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    setAccessibleText(label, title);
    gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
    if (!description.empty()) {
        auto *help = gtk_label_new(std::string(description).c_str());
        addStyleClass(help, "modernime-description");
        gtk_widget_set_halign(help, GTK_ALIGN_START);
        gtk_label_set_line_wrap(GTK_LABEL(help), TRUE);
        setAccessibleText(help, description);
        gtk_box_pack_start(GTK_BOX(row), help, FALSE, FALSE, 0);
    }
    if (control != nullptr) {
        setAccessibleText(control, title, description);
        gtk_box_pack_start(GTK_BOX(row), control, FALSE, FALSE, 0);
    }
    return row;
}

GtkWidget *createStatusPill(std::string_view text) {
    auto *status = gtk_label_new(std::string(text).c_str());
    addStyleClass(status, "modernime-status");
    gtk_widget_set_halign(status, GTK_ALIGN_START);
    setAccessibleText(status, text);
    return status;
}

GtkWidget *createEmptyState(std::string_view title,
                            std::string_view description) {
    auto *state = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    setAccessibleText(state, title, description);
    auto *heading = gtk_label_new(std::string(title).c_str());
    gtk_widget_set_halign(heading, GTK_ALIGN_START);
    setAccessibleText(heading, title);
    gtk_box_pack_start(GTK_BOX(state), heading, FALSE, FALSE, 0);
    if (!description.empty()) {
        auto *help = gtk_label_new(std::string(description).c_str());
        addStyleClass(help, "modernime-description");
        gtk_widget_set_halign(help, GTK_ALIGN_START);
        gtk_label_set_line_wrap(GTK_LABEL(help), TRUE);
        setAccessibleText(help, description);
        gtk_box_pack_start(GTK_BOX(state), help, FALSE, FALSE, 0);
    }
    return state;
}

} // namespace modernime::settings
