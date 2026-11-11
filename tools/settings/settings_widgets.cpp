#include "modernime/settings/settings_widgets.h"

#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/page_registry.h"
#include "modernime/settings/settings_shell_state.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace modernime::settings {
namespace {

struct ClientPalette {
    std::string_view key;
    const char *canvas, *surface, *sidebar, *inset, *control;
    const char *ink, *muted, *border, *controlBorder, *accent, *selectedInk;
    bool dark;
};
const ClientPalette &clientPalette(std::string_view key) {
    static constexpr ClientPalette palettes[]{
        {"blue", "#f3f6fb", "#ffffff", "#eaf0f7", "#edf2f8", "#ffffff",
         "#202938", "#5d697a", "#dde3ec", "#bac6d6", "#1262ff", "#ffffff", false},
        {"jade", "#f2f6f4", "#fdfefd", "#e9f0ec", "#edf2ef", "#ffffff",
         "#26322e", "#606d68", "#dbe3df", "#bdccc5", "#087f73", "#ffffff", false},
        {"night", "#162231", "#243449", "#1d2c3e", "#1b2a3c", "#2c3f56",
         "#edf3fb", "#b5c3d5", "#43556c", "#637892", "#296ad8", "#ffffff", true},
        {"amber", "#f7f4ef", "#fffdfa", "#f0ece5", "#f3efe9", "#fffefd",
         "#332e28", "#70665d", "#e1ddd6", "#c9c0b3", "#a65f16", "#ffffff", false},
        {"rose", "#f8f3f5", "#fffefe", "#f1eaee", "#f4eef1", "#ffffff",
         "#332c31", "#71636b", "#e3dce0", "#cdbfc5", "#a83d63", "#ffffff", false},
        {"lavender", "#f3f1f8", "#fdfcff", "#ece9f3", "#efedf6", "#fefeff",
         "#302d3c", "#6c6577", "#e0dce8", "#c7c0d5", "#7052ad", "#ffffff", false},
        {"contrast", "#090909", "#141414", "#101010", "#090909", "#1c1c1c",
         "#ffffff", "#ededed", "#bdbdbd", "#ededed", "#ffe066", "#111111", true},
    };
    for (const auto &palette : palettes) if (palette.key == key) return palette;
    return palettes[0];
}

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
    // Application-owned prompts use the same theme as their parent window.
    // Keep native file chooser presentation and behavior under GTK's control.
    if (!GTK_IS_FILE_CHOOSER(dialog)) {
        addStyleClass(GTK_WIDGET(dialog), kSettingsWindowClass);
        addStyleClass(GTK_WIDGET(dialog), "modernime-dialog");
    }
    auto *button = gtk_dialog_get_widget_for_response(dialog, response);
    if (GTK_IS_BUTTON(button) && !name.empty()) {
        gtk_button_set_label(GTK_BUTTON(button), std::string(name).c_str());
    }
    setAccessibleWidgetText(button, name, description);
}

GtkWidget *createSettingsPageIcon(SettingsPageId page, int size) {
    const char *name = nullptr;
    switch (page) {
    case SettingsPageId::Overview: name = "go-home-symbolic"; break;
    case SettingsPageId::Input: name = "input-keyboard-symbolic"; break;
    case SettingsPageId::Dictionary: name = "accessories-dictionary-symbolic"; break;
    case SettingsPageId::Clipboard: name = "edit-paste-symbolic"; break;
    case SettingsPageId::Updates: name = "software-update-available-symbolic"; break;
    case SettingsPageId::Diagnostics: name = "utilities-system-monitor-symbolic"; break;
    case SettingsPageId::Candidates:
    case SettingsPageId::Learning: break;
    }
    if (name) {
        auto *icon = gtk_image_new_from_icon_name(name, GTK_ICON_SIZE_BUTTON);
        gtk_image_set_pixel_size(GTK_IMAGE(icon), size);
        return icon;
    }

    auto *icon = gtk_drawing_area_new();
    gtk_widget_set_size_request(icon, size, size);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
    gtk_widget_set_has_window(icon, FALSE);
    gtk_widget_set_can_focus(icon, FALSE);
    atk_object_set_role(gtk_widget_get_accessible(icon), ATK_ROLE_REDUNDANT_OBJECT);
    g_signal_connect(icon, "draw", G_CALLBACK(+[](GtkWidget *widget, cairo_t *cr, gpointer data) -> gboolean {
        GdkRGBA ink{};
        gtk_style_context_get_color(gtk_widget_get_style_context(widget),
                                    gtk_widget_get_state_flags(widget), &ink);
        int pixels = 16;
        gtk_widget_get_size_request(widget, &pixels, nullptr);
        cairo_save(cr);
        cairo_translate(cr, (gtk_widget_get_allocated_width(widget) - pixels) / 2.0,
                        (gtk_widget_get_allocated_height(widget) - pixels) / 2.0);
        gdk_cairo_set_source_rgba(cr, &ink);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        if (static_cast<SettingsPageId>(GPOINTER_TO_INT(data)) == SettingsPageId::Candidates) {
            cairo_scale(cr, pixels / 24.0, pixels / 24.0);
            cairo_set_line_width(cr, 1.4);
            // A selected candidate and two alternatives, above three palette dots.
            cairo_new_sub_path(cr);
            cairo_arc(cr, 19, 6, 2, -G_PI / 2, 0);
            cairo_arc(cr, 19, 12, 2, 0, G_PI / 2);
            cairo_arc(cr, 5, 12, 2, G_PI / 2, G_PI);
            cairo_arc(cr, 5, 6, 2, G_PI, 3 * G_PI / 2);
            cairo_close_path(cr);
            cairo_stroke(cr);
            cairo_rectangle(cr, 5.5, 6.5, 4.5, 5);
            cairo_fill(cr);
            cairo_move_to(cr, 12.5, 9); cairo_line_to(cr, 14, 9);
            cairo_move_to(cr, 17, 9); cairo_line_to(cr, 18.5, 9);
            cairo_stroke(cr);
            cairo_arc(cr, 7, 19, 1.4, 0, 2 * G_PI); cairo_fill(cr);
            cairo_arc(cr, 12, 19, 1.4, 0, 2 * G_PI); cairo_stroke(cr);
            cairo_arc(cr, 17, 19, 1.4, 0, 2 * G_PI); cairo_stroke(cr);
            cairo_move_to(cr, 17, 17.6); cairo_line_to(cr, 17, 20.4); cairo_stroke(cr);
        } else {
            cairo_scale(cr, pixels / 16.0, pixels / 16.0);
            cairo_set_line_width(cr, 1.2);
            // Reuse the same compact brain in the sidebar and overview.
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
        }
        cairo_restore(cr);
        return FALSE;
    }), GINT_TO_POINTER(static_cast<int>(page)));
    return icon;
}

GtkWidget *createSettingsStatusArt(SettingsStatusArt kind) {
    auto *art = gtk_drawing_area_new();
    gtk_widget_set_size_request(art, 72, 64);
    gtk_widget_set_valign(art, GTK_ALIGN_CENTER);
    addStyleClass(art, "modernime-status-art");
    setAccessibleWidgetText(art, kind == SettingsStatusArt::Release ? "版本更新" : "输入服务", {});
    g_signal_connect(art, "draw", G_CALLBACK(+[](GtkWidget *widget, cairo_t *cr, gpointer data) -> gboolean {
        auto *context = gtk_widget_get_style_context(widget);
        GdkRGBA ink{};
        GdkRGBA paper{};
        gtk_style_context_get_color(context, gtk_widget_get_state_flags(widget), &ink);
        gtk_style_context_lookup_color(context, "theme_base_color", &paper);
        cairo_save(cr);
        cairo_translate(cr, (gtk_widget_get_allocated_width(widget) - 72) / 2.0,
                        (gtk_widget_get_allocated_height(widget) - 64) / 2.0);
        cairo_set_line_width(cr, 1.8);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        const auto color = [&](double alpha) {
            cairo_set_source_rgba(cr, ink.red, ink.green, ink.blue, alpha);
        };
        const auto rounded = [&](double x, double y, double w, double h, double radius) {
            constexpr double pi = 3.141592653589793;
            cairo_new_sub_path(cr);
            cairo_arc(cr, x+w-radius, y+radius, radius, -pi/2, 0);
            cairo_arc(cr, x+w-radius, y+h-radius, radius, 0, pi/2);
            cairo_arc(cr, x+radius, y+h-radius, radius, pi/2, pi);
            cairo_arc(cr, x+radius, y+radius, radius, pi, 3*pi/2);
            cairo_close_path(cr);
        };
        if (GPOINTER_TO_INT(data) == static_cast<int>(SettingsStatusArt::Release)) {
            // Successive release sheets; the download mark describes updates,
            // not a claim that the remote repository is already current.
            for (int layer = 0; layer < 3; ++layer) {
                rounded(20-layer*5, 5+layer*6, 34, 40, 6);
                gdk_cairo_set_source_rgba(cr, &paper); cairo_fill_preserve(cr);
                color(0.07 + layer*0.04); cairo_fill_preserve(cr);
                color(0.25 + layer*0.25); cairo_stroke(cr);
            }
            color(0.85);
            cairo_move_to(cr, 19, 29); cairo_line_to(cr, 35, 29);
            cairo_move_to(cr, 19, 36); cairo_line_to(cr, 30, 36);
            cairo_move_to(cr, 19, 43); cairo_line_to(cr, 26, 43); cairo_stroke(cr);
            cairo_arc(cr, 53, 47, 13, 0, 6.283185307);
            gdk_cairo_set_source_rgba(cr, &paper); cairo_fill_preserve(cr);
            color(0.12); cairo_fill_preserve(cr); color(0.8); cairo_stroke(cr);
            cairo_move_to(cr, 53, 40); cairo_line_to(cr, 53, 49);
            cairo_move_to(cr, 49, 46); cairo_line_to(cr, 53, 50); cairo_line_to(cr, 57, 46);
            cairo_move_to(cr, 48, 54); cairo_line_to(cr, 58, 54); cairo_stroke(cr);
        } else {
            // Composition strip above a keyboard ties diagnostics to typing.
            rounded(11, 6, 45, 17, 5); color(0.09); cairo_fill_preserve(cr);
            color(0.5); cairo_stroke(cr);
            color(0.9);
            cairo_move_to(cr, 19, 15); cairo_line_to(cr, 28, 15);
            cairo_move_to(cr, 33, 15); cairo_line_to(cr, 39, 15);
            cairo_move_to(cr, 46, 11); cairo_line_to(cr, 46, 18); cairo_stroke(cr);
            rounded(4, 30, 62, 28, 7); color(0.08); cairo_fill_preserve(cr);
            color(0.65); cairo_stroke(cr);
            for (int key = 0; key < 6; ++key) {
                rounded(12+key*7.5, 37, 4.5, 4.5, 1); color(0.65); cairo_fill(cr);
            }
            rounded(23, 47, 23, 4, 1.5); color(0.65); cairo_fill(cr);
            cairo_arc(cr, 61, 12, 5, 0, 6.283185307);
            gdk_cairo_set_source_rgba(cr, &paper); cairo_fill_preserve(cr);
            color(1.0); cairo_fill(cr);
        }
        cairo_restore(cr);
        return FALSE;
    }), GINT_TO_POINTER(static_cast<int>(kind)));
    return art;
}

std::string_view settingsStyles() {
    return R"css(
        /* Shared surface hierarchy: canvas, raised cards, interactive controls. */
        @define-color theme_bg_color #f3f6fb;
        @define-color theme_base_color #ffffff;
        @define-color theme_fg_color #202938;
        @define-color insensitive_fg_color #5d697a;
        @define-color borders #dde3ec;
        @define-color theme_selected_bg_color #1262ff;
        @define-color theme_selected_fg_color #ffffff;
        @define-color modernime_sidebar_color #eaf0f7;
        @define-color modernime_control_border #bac6d6;
        @define-color modernime_accent_surface #e5eeff;
        @define-color modernime_nav_fg_color #0b55eb;
        @define-color modernime_shadow #203a64;
        @define-color warning_color #805800;
        @define-color error_color #b83042;
        @define-color success_color #147452;
        @define-color modernime_hover_color #f7faff;
        @define-color modernime_pressed_color #dce8ff;
        @define-color modernime_inset_color #edf2f8;
        @define-color modernime_accent_hover #0957ed;
        @define-color modernime_accent_pressed #084bd0;
        @define-color modernime_muted_control #9aa9bd;
        @define-color modernime_control_surface #ffffff;
        @define-color modernime_switch_thumb #ffffff;
        @define-color modernime_disabled_fg #748195;
        @define-color modernime_nav_border_color alpha(@theme_selected_bg_color, 0.32);
        @define-color modernime_nav_marker_color transparent;
        @define-color modernime_focus_hint_fill alpha(@theme_selected_bg_color, 0.08);
        @define-color modernime_focus_hint_border alpha(@theme_selected_bg_color, 0.55);
        .modernime-settings {
            background-color: @theme_bg_color;
            color: @theme_fg_color;
            border-radius: 12px;
            font-family: "Noto Sans", "Noto Sans CJK SC", sans-serif;
            font-size: 14px;
        }

        .modernime-sidebar {
            border-right: 1px solid @borders;
            background-color: @modernime_sidebar_color;
            box-shadow: none;
            padding: 10px 8px;
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
            background-color: @theme_base_color;
            border: 1px solid @modernime_control_border;
            box-shadow: none;
            color: @theme_fg_color;
            padding: 7px 10px;
            margin-bottom: 6px;
        }

        .modernime-nav-button {
            border-radius: 10px;
            background-image: none;
            background-color: transparent;
            box-shadow: none;
            padding: 8px 8px;
            margin: 2px 0;
            border: 1px solid transparent;
            color: @theme_fg_color;
            transition: none;
        }

        .modernime-settings .modernime-nav-button:hover {
            background-color: @modernime_hover_color;
            border-color: @borders;
            box-shadow: none;
        }

        .modernime-settings .modernime-nav-button:active {
            background-color: @modernime_pressed_color;
            box-shadow: none;
        }

        .modernime-settings .modernime-nav-button.modernime-nav-selected {
            background-color: @modernime_accent_surface;
            border-color: @modernime_nav_border_color;
            background-image: none;
            color: @modernime_nav_fg_color;
            outline-color: @theme_selected_bg_color;
            box-shadow: inset 3px 0 0 @modernime_nav_marker_color;
            font-weight: 600;
        }
        .modernime-settings .modernime-nav-button.modernime-nav-selected:hover {
            background-color: @modernime_pressed_color;
            border-color: @modernime_nav_border_color;
        }
        .modernime-settings .modernime-nav-button.modernime-nav-selected:active {
            background-color: @modernime_pressed_color;
            box-shadow: inset 3px 0 0 @modernime_nav_marker_color;
        }
        .modernime-nav-button.modernime-nav-selected * {
            color: @modernime_nav_fg_color;
        }
        .modernime-settings .modernime-nav-button,
        .modernime-settings .modernime-nav-button * {
            transition: none;
        }
        /* The moving selection is painted once by the navigation container. */
        .modernime-settings .modernime-nav-motion .modernime-nav-button.modernime-nav-selected,
        .modernime-settings .modernime-nav-motion .modernime-nav-button.modernime-nav-selected:hover,
        .modernime-settings .modernime-nav-motion .modernime-nav-button.modernime-nav-selected:active {
            background-color: transparent;
            background-image: none;
            border-color: transparent;
            box-shadow: none;
        }

        .modernime-code-block {
            background-color: @modernime_inset_color;
            border: 1px solid @borders;
            border-radius: 8px;
            padding: 12px;
            color: @theme_fg_color;
            font-family: monospace;
            font-size: 12px;
        }

        .modernime-update-notice {
            background-color: @modernime_accent_surface;
            border: 1px solid @borders;
            border-radius: 8px;
            padding: 12px;
            color: @theme_fg_color;
        }
        .modernime-update-notice label, .modernime-update-notice image {
            color: @theme_fg_color;
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
            font-size: 25px;
            font-weight: 700;
            color: @theme_fg_color;
            letter-spacing: -0.2px;
        }

        .modernime-page-subtitle {
            font-size: 13px;
            color: @insensitive_fg_color;
            margin-top: 2px;
            margin-bottom: 0;
        }

        .modernime-description,
        .modernime-path {
            color: @insensitive_fg_color;
            font-size: 12.5px;
        }

        .modernime-settings .modernime-dictionary-switcher button {
            background-color: @modernime_control_surface;
            padding: 7px 16px;
        }
        .modernime-settings .modernime-dictionary-switcher button:checked {
            background-color: @modernime_accent_surface;
            border-color: @modernime_nav_border_color;
            color: @modernime_nav_fg_color;
            font-weight: 600;
        }

        .modernime-settings .modernime-details-button,
        .modernime-settings .modernime-details-button:hover,
        .modernime-settings .modernime-details-button:checked {
            padding: 14px 10px;
            font-size: 15px;
            font-weight: 500;
            background-color: transparent;
            border: none;
            box-shadow: none;
        }
        .modernime-details {
            border-bottom: 1px solid @borders;
        }
        .modernime-details:last-child { border-bottom: none; }
        .modernime-details-content { padding: 4px 10px 18px; }
        .modernime-update-details .modernime-details { border: none; }
        .modernime-update-details .modernime-details-content { padding: 14px 12px 18px; }
        .modernime-update-details .modernime-release-content { padding-bottom: 8px; }
        .modernime-release-notes-scroll {
            border: 1px solid @borders;
            border-radius: 8px;
            background-color: @theme_base_color;
        }
        .modernime-release-notes-scroll:focus {
            border-color: transparent;
            outline: 2px solid alpha(@theme_selected_bg_color, 0.6);
            outline-offset: -2px;
        }
        .modernime-release-notes-body { padding: 14px; }
        .modernime-settings .modernime-update-details .modernime-details-button {
            padding: 12px 10px;
        }
        .modernime-update-summary { padding: 20px 22px; }
        .modernime-update-progress trough { min-height: 6px; border-radius: 6px; background: @modernime_accent_surface; border: none; }
        .modernime-update-progress progress { min-height: 6px; border-radius: 6px; background: @theme_selected_bg_color; border: none; }
        .modernime-current-version { font-size: 26px; font-weight: 600; }
        .modernime-update-status { font-size: 13px; color: @insensitive_fg_color; }
        .modernime-diagnostics-card { padding: 18px 22px; }
        .modernime-diagnostics-card .modernime-setting-row { padding: 16px 0; }
        .modernime-diagnostics-card .modernime-setting-title { font-size: 14px; }
        .modernime-runtime-summary {
            background-color: transparent;
            border: none;
            box-shadow: none;
            padding: 12px 0 20px;
        }
        .modernime-runtime-summary label {
            font-size: 18px;
            font-weight: 600;
            color: @theme_fg_color;
        }
        .modernime-runtime-summary image { color: @theme_selected_bg_color; }
        .modernime-runtime-summary.warning label,
        .modernime-runtime-summary.warning image { color: @warning_color; }
        .modernime-runtime-summary.error label,
        .modernime-runtime-summary.error image { color: @error_color; }
        .modernime-diagnostics-card .modernime-diagnostics-value {
            font-size: 15px;
            font-weight: 500;
            color: @theme_fg_color;
            background-color: transparent;
            border: none;
            border-radius: 0;
            padding: 0;
        }
        .modernime-diagnostics-card .modernime-badge-amber { color: @warning_color; }
        .modernime-diagnostics-card .modernime-badge-red { color: @error_color; }
        .modernime-diagnostics-card button { padding: 7px 13px; }

        .modernime-section {
            border: 1px solid @borders;
            border-radius: 12px;
            background-color: @theme_base_color;
            box-shadow: none;
            padding: 16px 20px;
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
            border-bottom: 1px solid @borders;
        }

        .modernime-setting-row:last-child {
            border-bottom: none;
        }

        .modernime-setting-title {
            font-size: 14px;
            font-weight: 500;
            color: @theme_fg_color;
        }

        .modernime-setting-desc {
            font-size: 12.5px;
            color: @insensitive_fg_color;
        }

        /* Overview: one input workbench, two shortcuts, one content library. */
        .modernime-overview-workbench {
            border: none;
            background-color: transparent;
            padding: 0 0 8px;
        }
        .modernime-overview-status { font-size: 14px; font-weight: 500; }
        .modernime-settings button.modernime-overview-diagnostics {
            padding: 6px 10px;
            background-color: transparent;
            border-color: transparent;
            box-shadow: none;
            color: @modernime_nav_fg_color;
        }
        .modernime-settings button.modernime-overview-diagnostics:hover {
            background-color: @modernime_pressed_color;
        }
        .modernime-settings entry.modernime-overview-trial {
            font-size: 18px;
            min-height: 52px;
            padding: 12px 20px;
            border: 1px solid @borders;
            border-radius: 12px;
            background-color: @theme_base_color;
            background-image: none;
            box-shadow: inset 0 1px 2px alpha(@modernime_shadow, 0.035),
                        0 3px 8px alpha(@modernime_shadow, 0.035);
            color: @theme_fg_color;
        }
        .modernime-settings entry.modernime-overview-trial:hover {
            border-color: mix(@borders, @theme_selected_bg_color, 0.35);
        }
        .modernime-settings entry.modernime-overview-trial:focus {
            border-color: @theme_selected_bg_color;
            box-shadow: 0 0 0 3px alpha(@theme_selected_bg_color, 0.12),
                        inset 0 1px 2px alpha(@modernime_shadow, 0.025);
        }
        .modernime-settings entry.modernime-overview-trial:drop(active) {
            border-color: @theme_selected_bg_color;
        }
        .modernime-overview-shortcuts > flowboxchild {
            padding: 0;
            margin: 0;
            outline-width: 0;
        }
        .modernime-settings button.modernime-overview-shortcut {
            padding: 14px;
            border-radius: 10px;
            box-shadow: none;
        }
        .modernime-overview-shortcut-icon {
            padding: 8px;
            border-radius: 8px;
            background-color: @modernime_accent_surface;
            color: @modernime_nav_fg_color;
        }
        .modernime-overview-library {
            border: 1px solid @borders;
            border-radius: 12px;
            background-color: @theme_base_color;
            padding: 12px 6px 6px;
        }
        .modernime-overview-library-title {
            font-size: 13px;
            font-weight: 600;
            color: @insensitive_fg_color;
            margin: 0 10px 2px;
        }
        .modernime-settings button.modernime-overview-content-row {
            padding: 10px;
            border: 1px solid transparent;
            border-radius: 9px;
            box-shadow: none;
            background-color: transparent;
            transition: none;
        }
        .modernime-settings button.modernime-overview-content-row:hover {
            background-color: transparent;
            border-color: transparent;
        }
        .modernime-settings button.modernime-overview-content-row:active {
            background-color: @modernime_pressed_color;
        }
        .modernime-overview-content-icon {
            color: @insensitive_fg_color;
            padding: 6px;
            border-radius: 8px;
        }
        .modernime-overview-content-arrow {
            color: @insensitive_fg_color;
            padding: 5px;
            border-radius: 7px;
        }
        .modernime-overview-content-row:hover .modernime-overview-content-icon,
        .modernime-overview-content-row:hover .modernime-overview-content-arrow {
            color: @modernime_nav_fg_color;
            background-color: @modernime_accent_surface;
        }
        .modernime-overview-content-row:hover .modernime-overview-content-title {
            color: @modernime_nav_fg_color;
        }
        .modernime-overview-count {
            font-size: 13px;
            color: @theme_fg_color;
            padding: 4px 8px;
            border-radius: 6px;
            background-color: @modernime_inset_color;
        }
        .modernime-overview-notices {
            border: 1px solid alpha(@warning_color, 0.4);
            border-radius: 9px;
            background-color: alpha(@warning_color, 0.06);
        }
        .modernime-settings button.modernime-overview-notice {
            padding: 10px 12px;
            border: none;
            box-shadow: none;
            background-color: transparent;
        }
        .modernime-settings button.modernime-overview-notice:hover {
            background-color: @modernime_hover_color;
        }
        .modernime-overview-notice image { color: @warning_color; }

        /* Status & Badges */
        .modernime-status {
            border-radius: 6px;
            color: @insensitive_fg_color;
            transition: color 150ms ease;
        }
        .modernime-status label, .modernime-status image { transition: color 150ms ease; }

        .modernime-status-banner {
            background-color: @modernime_accent_surface;
            border: 1px solid @borders;
            border-radius: 8px;
            padding: 9px 13px;
            margin-bottom: 14px;
        }
        .modernime-status-banner label {
            color: @modernime_nav_fg_color;
            font-size: 13px;
            font-weight: 500;
        }
        .modernime-status-banner image {
            color: @theme_selected_bg_color;
        }
        .modernime-status-banner.error {
            background-color: alpha(@error_color, 0.10);
            border-color: alpha(@error_color, 0.3);
        }
        .modernime-status-banner.error label {
            color: @error_color;
        }
        .modernime-status-banner.error image {
            color: @error_color;
        }
        .modernime-status-banner.warning {
            background-color: alpha(@warning_color, 0.12);
            border-color: alpha(@warning_color, 0.12);
        }
        .modernime-status-banner.warning label {
            color: @warning_color;
        }
        .modernime-status-banner.warning image {
            color: @warning_color;
        }
        .modernime-status-banner.success {
            background-color: alpha(@success_color, 0.12);
            border-color: alpha(@success_color, 0.12);
        }
        .modernime-status-banner.success label {
            color: @success_color;
        }
        .modernime-status-banner.success image {
            color: @success_color;
        }

        .modernime-badge-green {
            background-color: alpha(@success_color, 0.12);
            color: @success_color;
            font-weight: 600;
            font-size: 12.5px;
            border-radius: 6px;
            padding: 3px 8px;
            border: 1px solid transparent;
        }

        .modernime-badge-amber {
            background-color: alpha(@warning_color, 0.12);
            color: @warning_color;
            font-weight: 600;
            font-size: 12.5px;
            border-radius: 6px;
            padding: 3px 8px;
            border: 1px solid transparent;
        }

        .modernime-badge-red {
            background-color: alpha(@error_color, 0.12);
            color: @error_color;
            font-weight: 600;
            font-size: 12.5px;
            border-radius: 6px;
            padding: 3px 8px;
            border: 1px solid transparent;
        }

        .modernime-badge-neutral {
            background-color: @modernime_inset_color;
            color: @theme_fg_color;
            font-weight: 500;
            font-size: 12.5px;
            border-radius: 6px;
            padding: 3px 8px;
            border: 1px solid transparent;
        }

        .modernime-status-dirty {
            color: @modernime_nav_fg_color;
            font-weight: 600;
        }

        .modernime-status-error,
        entry.error,
        .modernime-settings button.modernime-shortcut-btn.error {
            color: @error_color;
        }

        .modernime-focus-fallback {
            border-radius: 8px;
            box-shadow: inset 0 0 0 2px @theme_selected_bg_color;
        }

        entry.error,
        .modernime-settings button.modernime-shortcut-btn.error {
            border-color: @error_color;
        }

        /* Bottom Bar */
        .modernime-bottom-bar {
            border-top: 1px solid @borders;
            background-color: @theme_base_color;
            box-shadow: none;
            border-radius: 0 0 12px 12px;
            padding: 12px 24px;
        }

        /* Preview Box */
        .modernime-preview-bar {
            background-color: @modernime_inset_color;
            border: 1px solid @borders;
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
            color: @theme_fg_color;
        }
        .modernime-clipboard-list row:selected { background-color: transparent; }
        .modernime-clipboard-card {
            border: 1px solid @borders;
            border-left: 1px solid @borders;
            border-radius: 8px;
            background-color: @theme_base_color;
            padding: 14px 16px;
            margin: 1px 2px;
            transition: background-color 140ms ease, border-color 140ms ease;
        }
        .modernime-clipboard-card:hover {
            background-color: @modernime_hover_color;
            border-color: @borders;
        }
        .modernime-clipboard-list row:selected .modernime-clipboard-card {
            border-color: @modernime_control_border;
            border-left-color: @theme_selected_bg_color;
            background-color: @modernime_accent_surface;
            box-shadow: none;
        }
        .modernime-card-index {
            min-width: 20px;
            font-size: 11px;
            font-weight: 600;
            color: @insensitive_fg_color;
        }
        .modernime-badge-code, .modernime-badge-url,
        .modernime-badge-multiline, .modernime-badge-text {
            font-size: 12px;
            color: @insensitive_fg_color;
        }
        .modernime-card-meta { font-size: 11px; color: @insensitive_fg_color; }
        .modernime-card-preview { font-size: 14px; color: @theme_fg_color; }
        .modernime-card-expanded-box {
            border-radius: 6px;
            border: 1px solid @borders;
            background-color: @theme_base_color;
        }
        .modernime-clipboard-reader, .modernime-clipboard-reader text {
            font-size: 14px;
            color: @theme_fg_color;
            background-color: @theme_base_color;
        }
        .modernime-card-code {
            font-family: monospace;
            font-size: 13px;
            color: @theme_fg_color;
        }
        .modernime-toggle-btn, .modernime-card-btn {
            font-size: 12px;
            min-height: 22px;
            padding: 2px 8px;
            border-radius: 5px;
            color: @theme_selected_bg_color;
            border: 1px solid transparent;
            background-image: none;
            background-color: transparent;
            box-shadow: none;
        }
        .modernime-toggle-btn:hover, .modernime-card-btn:hover {
            background-color: @modernime_accent_surface;
            border-color: @modernime_control_border;
        }
        .modernime-toggle-btn:focus, .modernime-card-btn:focus {
            border-color: @theme_selected_bg_color;
        }
        .modernime-toggle-btn label, .modernime-card-btn label { color: @theme_selected_bg_color; }
        .modernime-clipboard-toolbar { margin: 4px 0 8px; }

        .modernime-shortcut-btn {
            font-family: monospace, DejaVu Sans Mono, Liberation Mono, sans-serif;
            font-size: 13px;
            font-weight: 600;
            padding: 7px 13px;
            min-width: 140px;
            border-radius: 8px;
            border: 1px solid @borders;
            background-color: @modernime_control_surface;
            color: @theme_fg_color;
            transition: all 120ms ease;
        }

        .modernime-shortcut-btn:hover {
            background-color: @modernime_hover_color;
            border-color: @modernime_control_border;
        }

        .modernime-settings .modernime-shortcut-btn.recording {
            border-color: @theme_selected_bg_color;
            background-color: @modernime_accent_surface;
            color: @modernime_nav_fg_color;
            box-shadow: inset 0 0 0 1px @theme_selected_bg_color;
        }

        /* Candidates Hero Preview & Controls matching layout-1.png */
        .modernime-preview-card {
            border: 1px solid @borders;
            border-radius: 10px;
            background-color: @modernime_inset_color;
            box-shadow: none;
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
            background-color: @modernime_inset_color;
            border-radius: 8px;
            padding: 10px 12px;
            margin-top: 4px;
        }

        .modernime-sidebar-footer {
            font-size: 11.5px;
            color: @insensitive_fg_color;
            border-top: 1px solid @borders;
            padding-top: 14px;
        }

        button {
            border-radius: 8px;
            background-image: none;
            background-color: @modernime_control_surface;
            box-shadow: none;
            border: 1px solid @modernime_control_border;
            color: @theme_fg_color;
            min-height: 24px;
            padding: 7px 13px;
            transition: background-color 120ms ease, border-color 120ms ease,
                        box-shadow 120ms ease;
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
            border-color: @modernime_accent_hover;
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

        .modernime-nav-label { font-size: 14px; }
        .modernime-brand { font-size: 22px; font-weight: 800; color: @modernime_nav_fg_color; }
        .modernime-settings entry,
        .modernime-settings spinbutton,
        .modernime-settings treeview,
        .modernime-settings textview text {
            background-image: none;
            background-color: @modernime_control_surface;
            color: @theme_fg_color;
            border-color: @modernime_control_border;
        }
        .modernime-settings entry {
            min-height: 24px;
            padding: 7px 10px;
            border: 1px solid @modernime_control_border;
            border-radius: 8px;
            box-shadow: none;
        }
        .modernime-settings entry:disabled,
        .modernime-settings spinbutton:disabled,
        .modernime-settings textview:disabled text {
            background-color: @modernime_inset_color;
            color: @modernime_disabled_fg;
            border-color: @borders;
        }
        .modernime-settings entry selection,
        .modernime-settings textview text selection,
        .modernime-settings treeview:selected {
            background-color: @theme_selected_bg_color;
            color: @theme_selected_fg_color;
        }
        .modernime-settings entry:focus,
        .modernime-settings spinbutton:focus {
            border-color: @theme_selected_bg_color;
            box-shadow: inset 0 0 0 1px alpha(@theme_selected_bg_color, 0.15);
        }
        .modernime-settings button:hover {
            background-color: @modernime_hover_color;
            border-color: @modernime_control_border;
        }
        .modernime-settings button:active {
            background-color: @modernime_pressed_color;
            box-shadow: none;
        }
        .modernime-settings button:focus {
            outline: 2px solid alpha(@theme_selected_bg_color, 0.6);
            /* Keep the focus stroke inside the control's paint/allocation box.
               An outset ring doubles its border and can survive partial redraws. */
            outline-offset: -2px;
        }
        .modernime-settings button.suggested-action:hover {
            background-color: @modernime_accent_hover;
            border-color: @modernime_accent_pressed;
        }
        .modernime-settings button.suggested-action:active {
            background-color: @modernime_accent_pressed;
            border-color: @modernime_accent_pressed;
            box-shadow: none;
        }
        .modernime-settings button.destructive-action {
            color: @error_color;
            background-color: alpha(@error_color, 0.10);
            border-color: alpha(@error_color, 0.3);
        }
        .modernime-settings button.destructive-action:hover {
            background-color: alpha(@error_color, 0.10);
            border-color: alpha(@error_color, 0.3);
        }
        .modernime-settings button.destructive-action:active {
            background-color: alpha(@error_color, 0.10);
        }
        .modernime-settings button:disabled {
            background-color: @modernime_inset_color;
            border-color: @borders;
            color: @modernime_disabled_fg;
            box-shadow: none;
        }
        .modernime-settings button.suggested-action:disabled {
            background-color: @modernime_accent_surface;
            border-color: @modernime_accent_surface;
            color: @modernime_disabled_fg;
        }
        .modernime-settings button:disabled label { color: inherit; }
        .modernime-settings .modernime-bottom-bar button { min-width: 84px; }
        .modernime-settings switch {
            background-image: none;
            background-color: @modernime_muted_control;
            border: 1px solid @modernime_muted_control;
            box-shadow: none;
            border-radius: 16px;
            min-width: 42px;
            min-height: 24px;
            color: transparent;
        }
        .modernime-settings switch:checked {
            background-color: @theme_selected_bg_color;
            border-color: @modernime_accent_hover;
        }
        .modernime-settings switch slider {
            background-image: none;
            background-color: @modernime_switch_thumb;
            border: none;
            border-radius: 50%;
            min-width: 20px;
            min-height: 20px;
            margin: 2px;
            box-shadow: 0 1px 2px alpha(@theme_fg_color, 0.12);
        }
        .modernime-settings switch:checked slider { background-color: @theme_selected_fg_color; }
        .modernime-settings switch:disabled {
            background-color: @modernime_inset_color;
            border-color: @modernime_control_border;
        }
        .modernime-settings switch:disabled slider {
            background-color: @modernime_disabled_fg;
            box-shadow: none;
        }
        .modernime-settings switch:focus,
        .modernime-settings checkbutton:focus check,
        .modernime-settings radiobutton:focus radio {
            outline: 2px solid @theme_selected_bg_color;
            outline-offset: -2px;
        }
        .modernime-settings checkbutton check,
        .modernime-settings radiobutton radio {
            background-image: none;
            background-color: @modernime_control_surface;
            border: 1px solid @modernime_control_border;
            color: @theme_fg_color;
        }
        .modernime-settings checkbutton check:checked,
        .modernime-settings radiobutton radio:checked {
            background-image: none;
            background-color: @theme_selected_bg_color;
            border-color: @theme_selected_bg_color;
            color: @theme_selected_fg_color;
        }

        /* Anchored choices share the application's surface, typography and depth. */
        popover, popover.background, menu {
            background-color: @theme_base_color;
            background-image: none;
            color: @theme_fg_color;
            border: 1px solid @borders;
            border-radius: 12px;
            box-shadow: 0 8px 24px alpha(@modernime_shadow, 0.18);
        }
        popover label, popover modelbutton, menu menuitem { color: @theme_fg_color; }
        popover modelbutton, menu menuitem {
            border-radius: 7px;
            min-height: 24px;
            padding: 7px 12px;
        }
        popover modelbutton:hover, popover modelbutton:focus,
        menu menuitem:hover {
            background-color: @modernime_accent_surface;
            color: @modernime_nav_fg_color;
        }
        popover modelbutton:hover label, popover modelbutton:focus label,
        menu menuitem:hover label { color: @modernime_nav_fg_color; }
        popover button:focus, popover modelbutton:focus {
            outline: 2px solid @theme_selected_bg_color;
            outline-offset: -2px;
        }
        popover button:disabled, popover modelbutton:disabled,
        popover button:disabled label, popover modelbutton:disabled label,
        menu menuitem:disabled, menu menuitem:disabled label {
            color: @modernime_disabled_fg;
        }
        popover separator, menu separator { background-color: @borders; }
        popover modelbutton:checked { color: @modernime_nav_fg_color; }
        popover radio, popover check, menu radio, menu check {
            background-image: none;
            background-color: @modernime_control_surface;
            border: 1px solid @modernime_control_border;
            color: @theme_fg_color;
        }
        popover radio:checked, popover check:checked,
        menu radio:checked, menu check:checked {
            background-image: none;
            background-color: @theme_selected_bg_color;
            border-color: @theme_selected_bg_color;
            color: @theme_selected_fg_color;
        }
        .modernime-settings .modernime-choice { min-width: 84px; }
        .modernime-settings .modernime-choice:checked {
            background-color: @modernime_accent_surface;
            border-color: @theme_selected_bg_color;
            box-shadow: none;
        }
        .modernime-choice-popover .modernime-palette-option {
            background-image: none;
            background-color: transparent;
            border: 1px solid transparent;
            box-shadow: none;
            border-radius: 7px;
            padding: 8px 12px;
            min-width: 180px;
        }
        .modernime-choice-popover .modernime-palette-option:hover,
        .modernime-choice-popover .modernime-palette-option:focus {
            background-color: @modernime_hover_color;
        }
        .modernime-choice-popover .modernime-palette-option:checked {
            background-color: @modernime_accent_surface;
            border-color: @theme_selected_bg_color;
        }
        .modernime-choice-popover .modernime-palette-option:checked label {
            color: @modernime_nav_fg_color;
            font-weight: 700;
        }
        .modernime-choice image { color: @insensitive_fg_color; }
        .modernime-settings .modernime-details-button {
            border: 1px solid transparent;
            border-radius: 8px;
            background-image: none;
            outline-offset: -2px;
        }
        .modernime-settings .modernime-details-button:hover,
        .modernime-settings .modernime-details-button:checked {
            background-color: @modernime_hover_color;
            border: 1px solid transparent;
            box-shadow: none;
        }
        .modernime-settings .modernime-details-button:checked image {
            color: @modernime_nav_fg_color;
        }
        .modernime-settings .modernime-details-button:active {
            background-color: @modernime_pressed_color;
            box-shadow: none;
        }
        .modernime-setting-desc { margin-top: 3px; }
        .modernime-settings spinbutton {
            min-height: 38px;
            padding: 0;
            border: 1px solid @modernime_control_border;
            border-radius: 8px;
        }
        .modernime-settings spinbutton entry {
            min-height: 24px;
            padding: 7px 10px;
            border: none;
            border-radius: 8px 0 0 8px;
        }
        .modernime-settings spinbutton button {
            min-height: 24px;
            padding: 7px 8px;
            border: none;
            border-left: 1px solid @modernime_control_border;
            border-radius: 0;
            box-shadow: none;
        }
        .modernime-settings spinbutton button:last-child { border-radius: 0 8px 8px 0; }
        .modernime-settings .modernime-badge-code,
        .modernime-settings .modernime-badge-url,
        .modernime-settings .modernime-badge-multiline,
        .modernime-settings .modernime-badge-text {
            border-radius: 5px;
            padding: 3px 7px;
            font-size: 11px;
            font-weight: 600;
        }
        .modernime-settings .modernime-badge-code {
            background-color: alpha(@warning_color, 0.12); color: @warning_color;
        }
        .modernime-settings .modernime-badge-url {
            background-color: alpha(@success_color, 0.12); color: @success_color;
        }
        .modernime-settings .modernime-badge-text,
        .modernime-settings .modernime-badge-multiline {
            background-color: @modernime_accent_surface; color: @modernime_nav_fg_color;
        }
        .modernime-clipboard-type-icon { color: @insensitive_fg_color; }
        .modernime-card-url { color: @modernime_nav_fg_color; }
        .modernime-clipboard-toolbar { border-top: 1px solid @borders; padding-top: 12px; }
        .modernime-settings .modernime-card-btn,
        .modernime-settings .modernime-toggle-btn {
            background: transparent; border-color: transparent; box-shadow: none;
            color: @modernime_nav_fg_color;
        }
        .modernime-settings .modernime-card-btn:hover,
        .modernime-settings .modernime-toggle-btn:hover {
            background-color: @modernime_accent_surface;
        }
        .modernime-settings .modernime-clipboard-list row:selected .modernime-clipboard-card {
            border-color: @theme_selected_bg_color;
            background-color: @modernime_accent_surface;
        }
        .modernime-settings scrollbar slider { background-color: @modernime_muted_control; }
        .modernime-settings separator { background-color: @borders; min-height: 1px; }
        .modernime-settings .modernime-status-heading {
            font-size: 22px;
            font-weight: 700;
            color: @theme_fg_color;
        }
        .modernime-settings .modernime-status-hint,
        .modernime-settings .modernime-version-meta,
        .modernime-settings .modernime-check-details-message {
            font-size: 13px;
            font-weight: 400;
            color: @insensitive_fg_color;
        }
        .modernime-status-art {
            color: @modernime_nav_fg_color;
        }
        .modernime-settings .success .modernime-status-art {
            color: @success_color;
        }
        .modernime-settings .warning .modernime-status-art {
            color: @warning_color;
        }
        .modernime-settings .error .modernime-status-art {
            color: @error_color;
        }
        .modernime-settings .modernime-release-version {
            font-size: 28px;
            font-weight: 700;
            color: @theme_fg_color;
        }
        .modernime-settings .modernime-release-status {
            font-size: 13px;
            font-weight: 400;
            color: @insensitive_fg_color;
            margin-top: 4px;
        }
        .modernime-settings .warning .modernime-release-status {
            color: @warning_color;
        }
        .modernime-settings button.modernime-source-link {
            background-color: @modernime_accent_surface;
            border: 1px solid transparent;
            box-shadow: none;
            padding: 8px 14px;
            color: @modernime_nav_fg_color;
        }
        .modernime-settings button.modernime-source-link:hover {
            border-color: alpha(@theme_selected_bg_color, 0.45);
            background-color: @modernime_pressed_color;
        }
        .modernime-settings button.modernime-source-link label,
        .modernime-settings button.modernime-source-link drawingarea { color: inherit; }
        .modernime-settings .modernime-update-details button.modernime-source-link {
            background-color: transparent;
            padding: 8px 10px;
        }
        .modernime-settings .modernime-update-details button.modernime-source-link:hover {
            background-color: @modernime_hover_color;
        }
        .modernime-settings button.modernime-check-button image { margin-right: 6px; }

        /* Application prompts and FontButton windows are separate toplevels. */
        .modernime-dialog headerbar {
            background-image: none;
            background-color: @theme_base_color;
            color: @theme_fg_color;
            border-bottom: 1px solid @borders;
            box-shadow: none;
        }
        .modernime-dialog .dialog-action-area {
            padding: 12px;
        }
        .modernime-dialog .dim-label { color: @insensitive_fg_color; opacity: 1; }
        .modernime-settings tooltip, tooltip {
            background-image: none;
            background-color: @theme_base_color;
            color: @theme_fg_color;
            border: 1px solid @modernime_control_border;
            border-radius: 6px;
            box-shadow: none;
        }
        tooltip label { color: @theme_fg_color; }
        .modernime-font-dialog { border-radius: 12px; }
        .modernime-font-dialog headerbar {
            background-image: none;
            background-color: @theme_base_color;
            border-bottom: 1px solid @borders;
            box-shadow: none;
            padding: 8px 10px;
            min-height: 34px;
            color: @theme_fg_color;
        }
        .modernime-font-dialog headerbar .title { font-weight: 700; }
        .modernime-font-dialog .modernime-font-chooser { font-size: 12px; }
        .modernime-font-dialog .modernime-font-search {
            font-size: 13px;
            min-height: 24px;
            padding: 7px 10px;
            margin-bottom: 6px;
            border-radius: 8px;
        }
        .modernime-font-dialog .modernime-font-list {
            background-color: @theme_base_color;
            color: @theme_fg_color;
            -GtkTreeView-vertical-separator: 4;
            -GtkTreeView-horizontal-separator: 12;
        }
        .modernime-font-dialog .modernime-font-list:selected,
        .modernime-font-dialog .modernime-font-list:selected:focus {
            background-color: @modernime_accent_surface;
            color: @modernime_nav_fg_color;
        }
        .modernime-font-dialog .modernime-font-list:hover:not(:selected) {
            background-color: @modernime_hover_color;
        }
        .modernime-font-dialog .frame { border-color: @borders; }
        .modernime-font-dialog .modernime-font-preview {
            font-size: 18px;
            background-color: @modernime_inset_color;
            border: 1px solid @borders;
            border-radius: 10px;
            min-height: 32px;
            padding: 8px 10px;
            margin-top: 6px;
        }
        .modernime-settings .modernime-diagnostics-value {
            border-radius: 6px;
            padding: 5px 9px;
            font-size: 13px;
        }
        .modernime-settings .modernime-diagnostics-value.modernime-badge-green {
            background-color: alpha(@success_color, 0.12); color: @success_color;
        }
        .modernime-settings .modernime-diagnostics-value.modernime-badge-neutral {
            background-color: @modernime_inset_color; color: @insensitive_fg_color;
        }
        .modernime-settings .modernime-diagnostics-value.modernime-badge-amber {
            background-color: alpha(@warning_color, 0.12); color: @warning_color;
        }
        .modernime-settings .modernime-diagnostics-value.modernime-badge-red {
            background-color: alpha(@error_color, 0.12); color: @error_color;
        }
        .modernime-clipboard-type label, .modernime-clipboard-type image { color: inherit; }
        /* The reveal composites complete old/new frames; inner CSS transitions
           would otherwise leave controls halfway between their two palettes. */
        .modernime-settings.modernime-theme-static,
        .modernime-settings.modernime-theme-static *,
        .modernime-settings.modernime-theme-static *:hover,
        .modernime-settings.modernime-theme-static *:active,
        .modernime-settings.modernime-theme-static *:checked,
        .modernime-settings.modernime-theme-static *:focus,
        .modernime-theme-reveal,
        .modernime-theme-reveal *,
        .modernime-settings.modernime-theme-reveal,
        .modernime-settings.modernime-theme-reveal *,
        .modernime-settings.modernime-theme-reveal *:hover,
        .modernime-settings.modernime-theme-reveal *:active,
        .modernime-settings.modernime-theme-reveal *:checked,
        .modernime-settings.modernime-theme-reveal *:focus {
            transition: none;
        }
    )css";
}

void installSettingsStyles() { applySettingsTheme("blue"); }

std::array<GdkRGBA, 3> settingsThemeColors(std::string_view theme) {
    const auto &palette = clientPalette(theme);
    std::array<GdkRGBA, 3> colors{};
    gdk_rgba_parse(&colors[0], palette.surface);
    gdk_rgba_parse(&colors[1], palette.ink);
    gdk_rgba_parse(&colors[2], palette.accent);
    return colors;
}

void applySettingsTheme(std::string_view theme) {
    auto *screen = gdk_screen_get_default();
    if (screen == nullptr) return;
    constexpr const char *providerKey = "modernime-settings-css-provider";
    constexpr const char *themeKey = "modernime-settings-css-theme";
    const auto *previous = static_cast<const char *>(g_object_get_data(G_OBJECT(screen), themeKey));
    if (previous != nullptr && theme == previous) return;
    auto *provider = static_cast<GtkCssProvider *>(g_object_get_data(G_OBJECT(screen), providerKey));
    if (provider == nullptr) {
        provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(provider),
                                                  GTK_STYLE_PROVIDER_PRIORITY_USER);
        g_object_set_data_full(G_OBJECT(screen), providerKey, provider, g_object_unref);
    }
    std::string css(settingsStyles());
    const auto &palette = clientPalette(theme);
    const auto define = [&](const char *name, const std::string &value) {
        css += "\n@define-color " + std::string(name) + " " + value + ";";
    };
    define("theme_bg_color", palette.canvas);
    define("theme_base_color", palette.surface);
    define("theme_fg_color", palette.ink);
    define("insensitive_fg_color", palette.muted);
    define("borders", palette.border);
    define("theme_selected_bg_color", palette.accent);
    define("theme_selected_fg_color", palette.selectedInk);
    define("modernime_control_border", palette.controlBorder);
    define("modernime_control_surface", palette.control);
    define("modernime_sidebar_color", palette.sidebar);
    define("modernime_inset_color", palette.inset);
    define("modernime_accent_surface", "mix(@theme_base_color, @theme_selected_bg_color, 0.12)");
    define("modernime_hover_color", "mix(@modernime_control_surface, @theme_selected_bg_color, 0.06)");
    define("modernime_pressed_color", "mix(@theme_base_color, @theme_selected_bg_color, 0.18)");
    define("modernime_accent_hover", "shade(@theme_selected_bg_color, 0.92)");
    define("modernime_accent_pressed", "shade(@theme_selected_bg_color, 0.84)");
    define("modernime_shadow", palette.ink);
    define("modernime_muted_control", "mix(@modernime_control_border, @insensitive_fg_color, 0.3)");
    define("modernime_disabled_fg", "mix(@insensitive_fg_color, @theme_base_color, 0.18)");
    define("modernime_nav_border_color", "alpha(@theme_selected_bg_color, 0.32)");
    define("modernime_nav_marker_color", "transparent");
    define("modernime_focus_hint_fill", "alpha(@theme_selected_bg_color, 0.08)");
    define("modernime_focus_hint_border", "alpha(@theme_selected_bg_color, 0.55)");
    define("modernime_nav_fg_color", palette.dark
        ? "mix(@theme_selected_bg_color, @theme_fg_color, 0.55)" : "@theme_selected_bg_color");
    define("success_color", "#147452");
    define("warning_color", "#805800");
    define("error_color", "#b83042");
    if (palette.dark) {
        define("modernime_shadow", "#070e19");
        define("modernime_switch_thumb", "#edf3fb");
        define("modernime_disabled_fg", "#91a2b9");
        define("modernime_nav_border_color", "#6495e6");
        define("success_color", "#6bd3af");
        define("warning_color", "#efc16b");
        define("error_color", "#ff9399");
    }
    if (theme == "contrast") {
        define("modernime_accent_surface", "#252525");
        define("modernime_hover_color", "#303030");
        define("modernime_pressed_color", "#3c3c3c");
        define("modernime_accent_hover", "#ffeb99");
        define("modernime_accent_pressed", "#e6c84f");
        define("modernime_nav_fg_color", "@theme_selected_bg_color");
        define("modernime_nav_border_color", "@theme_selected_bg_color");
        define("modernime_nav_marker_color", "@theme_selected_bg_color");
        define("modernime_focus_hint_fill", "transparent");
        define("modernime_focus_hint_border", "@theme_selected_bg_color");
        define("modernime_muted_control", "#808080");
        define("modernime_disabled_fg", "#bdbdbd");
        define("modernime_switch_thumb", "#ffffff");
        define("success_color", "#72f0bc");
        define("warning_color", "#ffe066");
        define("error_color", "#ff9aab");
        css += R"css(
            .modernime-settings .modernime-section,
            .modernime-settings .modernime-preview-card,
            popover, popover.background, menu { box-shadow: none; border-color: @borders; }
            .modernime-settings button:focus,
            popover button:focus, popover modelbutton:focus { outline: 2px solid @theme_selected_bg_color; }
            .modernime-settings entry:focus, .modernime-settings spinbutton:focus {
                box-shadow: inset 0 0 0 1px @theme_selected_bg_color;
            }
            .modernime-settings .modernime-setting-desc, .modernime-settings .modernime-description { opacity: 1; }
            .modernime-settings .modernime-status-banner,
            .modernime-settings button.destructive-action,
            .modernime-settings .modernime-badge-green,
            .modernime-settings .modernime-badge-amber,
            .modernime-settings .modernime-badge-red,
            .modernime-settings .modernime-badge-neutral,
            .modernime-settings .modernime-diagnostics-value.modernime-badge-green,
            .modernime-settings .modernime-diagnostics-value.modernime-badge-amber,
            .modernime-settings .modernime-diagnostics-value.modernime-badge-red,
            .modernime-settings .modernime-diagnostics-value.modernime-badge-neutral,
            .modernime-settings .modernime-badge-code,
            .modernime-settings .modernime-badge-url,
            .modernime-settings .modernime-badge-text,
            .modernime-settings .modernime-badge-multiline {
                background-color: @modernime_inset_color;
                border-color: currentColor;
            }
            .modernime-settings .modernime-status-banner.success { border-color: @success_color; }
            .modernime-settings .modernime-status-banner.warning { border-color: @warning_color; }
            .modernime-settings .modernime-status-banner.error { border-color: @error_color; }
            .modernime-settings button:disabled,
            .modernime-settings button.suggested-action:disabled {
                border-color: @borders;
                background-color: @modernime_inset_color;
            }
        )css";
    }
    GError *error = nullptr;
    if (!gtk_css_provider_load_from_data(provider, css.data(), static_cast<gssize>(css.size()), &error)) {
        g_warning("ModernIME theme: %s", error->message);
        g_clear_error(&error);
        return;
    }
    g_object_set_data_full(G_OBJECT(screen), themeKey, g_strdup(std::string(theme).c_str()), g_free);
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

    auto *textVBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(textVBox, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(textVBox, TRUE);

    auto *label = gtk_label_new(std::string(title).c_str());
    addStyleClass(label, "modernime-setting-title");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    setAccessibleWidgetText(label, title, "设置名称");
    gtk_box_pack_start(GTK_BOX(textVBox), label, FALSE, FALSE, 0);
    if (!description.empty()) {
        auto *hint = gtk_label_new(std::string(description).c_str());
        addStyleClass(hint, "modernime-setting-desc");
        gtk_label_set_xalign(GTK_LABEL(hint), 0.0F);
        gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(hint), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_max_width_chars(GTK_LABEL(hint), 1);
        gtk_widget_set_hexpand(hint, TRUE);
        gtk_box_pack_start(GTK_BOX(textVBox), hint, FALSE, FALSE, 0);
    }


    gtk_box_pack_start(GTK_BOX(row), textVBox, TRUE, TRUE, 0);

    if (control != nullptr) {
        gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(control, GTK_ALIGN_END);
        if (GTK_IS_LABEL(control)) {
            // Read-only values must retain their dynamic text as the ATK name.
            setAccessibleWidgetText(control, {}, title);
        } else {
            setAccessibleWidgetText(control, title, description);
        }
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
