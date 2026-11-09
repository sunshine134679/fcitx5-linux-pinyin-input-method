#include "modernime/settings/settings_widgets.h"

#include <string>
#include <vector>

namespace {

// A native menu button keeps its popover attached to the control. Selection is
// owned here; the label and radio marks are views of the same selected index.
struct ChoiceState {
    GtkWidget *label = nullptr;
    GtkWidget *swatch = nullptr;
    GtkWidget *items = nullptr;
    GtkWidget *popover = nullptr;
    GtkWidget *scroll = nullptr;
    std::vector<std::string> labels;
    std::vector<GtkWidget *> buttons;
    std::vector<GdkPixbuf *> previews;
    bool changing = false;
    int selected = -1;
};

struct ModernimeChoice { GtkMenuButton parent; ChoiceState *state; };
struct ModernimeChoiceClass { GtkMenuButtonClass parent; };
G_DEFINE_TYPE(ModernimeChoice, modernime_choice, GTK_TYPE_MENU_BUTTON)
guint changedSignal = 0;

void modernime_choice_finalize(GObject *object) {
    for (auto *preview : reinterpret_cast<ModernimeChoice *>(object)->state->previews)
        if (preview) g_object_unref(preview);
    delete reinterpret_cast<ModernimeChoice *>(object)->state;
    G_OBJECT_CLASS(modernime_choice_parent_class)->finalize(object);
}

void modernime_choice_class_init(ModernimeChoiceClass *klass) {
    G_OBJECT_CLASS(klass)->finalize = modernime_choice_finalize;
    changedSignal = g_signal_new("changed", G_TYPE_FROM_CLASS(klass),
                                 G_SIGNAL_RUN_LAST, 0, nullptr, nullptr,
                                 nullptr, G_TYPE_NONE, 0);
}

void modernime_choice_init(ModernimeChoice *self) {
    self->state = new ChoiceState;
    auto &state = *self->state;
    auto *widget = GTK_WIDGET(self);
    gtk_style_context_add_class(gtk_widget_get_style_context(widget), "modernime-choice");
    auto *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    state.swatch = gtk_image_new();
    gtk_widget_set_no_show_all(state.swatch, TRUE);
    gtk_box_pack_start(GTK_BOX(content), state.swatch, FALSE, FALSE, 0);
    state.label = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(content), state.label, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(content),
        gtk_image_new_from_icon_name("pan-down-symbolic", GTK_ICON_SIZE_MENU), FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(self), content);
    state.popover = gtk_popover_new(widget);
    gtk_style_context_add_class(gtk_widget_get_style_context(state.popover), "modernime-choice-popover");
    gtk_popover_set_position(GTK_POPOVER(state.popover), GTK_POS_BOTTOM);
    state.items = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_container_set_border_width(GTK_CONTAINER(state.items), 6);
    auto *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    state.scroll = scroll;
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 340);
    gtk_container_add(GTK_CONTAINER(scroll), state.items);
    gtk_container_add(GTK_CONTAINER(state.popover), scroll);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(self), state.popover);
    g_signal_connect_object(state.popover, "map", G_CALLBACK(+[](GtkWidget *, gpointer owner) {
        // Popover mapping assigns initial focus after the map handlers. Wait
        // until that finishes, then reveal the selected swatch in long menus.
        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, +[](gpointer value) -> gboolean {
            auto *control = GTK_WIDGET(value);
            auto *popover = gtk_menu_button_get_popover(GTK_MENU_BUTTON(control));
            if (popover == nullptr || !gtk_widget_get_mapped(GTK_WIDGET(popover))) return G_SOURCE_REMOVE;
            auto &current = *reinterpret_cast<ModernimeChoice *>(control)->state;
            if (current.selected < 0) return G_SOURCE_REMOVE;
            auto *selected = current.buttons[current.selected];
            gtk_widget_grab_focus(selected);
            int x = 0, y = 0;
            if (gtk_widget_translate_coordinates(selected, current.items, 0, 0, &x, &y)) {
                auto *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(current.scroll));
                gtk_adjustment_clamp_page(adjustment, y, y + gtk_widget_get_allocated_height(selected) + 6);
            }
            return G_SOURCE_REMOVE;
        }, g_object_ref(owner), g_object_unref);
    }), self, G_CONNECT_AFTER);
    // The toolkit handles Escape, outside clicks, focus return and animations.
    gtk_widget_show_all(content);
    gtk_widget_show_all(scroll);
}

ChoiceState &choiceState(GtkWidget *choice) {
    return *reinterpret_cast<ModernimeChoice *>(choice)->state;
}

} // namespace

namespace modernime::settings {

GtkWidget *createSettingsChoice() {
    return GTK_WIDGET(g_object_new(modernime_choice_get_type(), nullptr));
}

void appendSettingsChoice(GtkWidget *choice, std::string_view text) {
    auto &state = choiceState(choice);
    const int index = static_cast<int>(state.labels.size());
    state.labels.emplace_back(text);
    auto *item = gtk_model_button_new();
    gtk_widget_set_can_default(item, TRUE);
    g_object_set(item, "text", state.labels.back().c_str(),
                 "role", GTK_BUTTON_ROLE_RADIO, "active", FALSE, nullptr);
    g_object_set_data(G_OBJECT(item), "modernime-choice-index", GINT_TO_POINTER(index));
    g_signal_connect_object(item, "clicked", G_CALLBACK(+[](GtkButton *button, gpointer owner) {
        auto *control = GTK_WIDGET(owner);
        setSettingsChoiceActive(control, GPOINTER_TO_INT(
            g_object_get_data(G_OBJECT(button), "modernime-choice-index")));
        gtk_popover_popdown(GTK_POPOVER(choiceState(control).popover));
    }), choice, GConnectFlags(0));
    state.buttons.push_back(item);
    state.previews.push_back(nullptr);
    gtk_box_pack_start(GTK_BOX(state.items), item, FALSE, FALSE, 0);
    gtk_widget_show(item);
}

int settingsChoiceActive(GtkWidget *choice) { return choiceState(choice).selected; }

void appendSettingsPaletteChoice(GtkWidget *choice, std::string_view text,
                                 const std::array<GdkRGBA, 3> &colors) {
    auto &state = choiceState(choice);
    const int index = static_cast<int>(state.labels.size());
    state.labels.emplace_back(text);
    auto *item = gtk_radio_button_new_from_widget(state.buttons.empty()
        ? nullptr : GTK_RADIO_BUTTON(state.buttons.front()));
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(item), FALSE);
    gtk_widget_set_can_default(item, TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(item), "modernime-palette-option");
    auto *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    auto *canvas = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 52, 28);
    auto *cr = cairo_create(canvas);
    gdk_cairo_set_source_rgba(cr, &colors[0]);
    cairo_paint(cr);
    gdk_cairo_set_source_rgba(cr, &colors[2]);
    cairo_rectangle(cr, 4, 4, 17, 20); cairo_fill(cr);
    gdk_cairo_set_source_rgba(cr, &colors[1]);
    cairo_rectangle(cr, 27, 8, 19, 2); cairo_rectangle(cr, 27, 14, 13, 2);
    cairo_rectangle(cr, 27, 20, 16, 2); cairo_fill(cr);
    auto *preview = gdk_pixbuf_get_from_surface(canvas, 0, 0, 52, 28);
    cairo_destroy(cr); cairo_surface_destroy(canvas);
    state.previews.push_back(preview);
    gtk_box_pack_start(GTK_BOX(row), gtk_image_new_from_pixbuf(preview), FALSE, FALSE, 0);
    auto *label = gtk_label_new(state.labels.back().c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    gtk_box_pack_start(GTK_BOX(row), label, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(item), row);
    setAccessibleWidgetText(item, text, "选择此配色，立即预览");
    g_object_set_data(G_OBJECT(item), "modernime-choice-index", GINT_TO_POINTER(index));
    g_signal_connect_object(item, "toggled", G_CALLBACK(+[](GtkToggleButton *button, gpointer owner) {
        auto *control = GTK_WIDGET(owner);
        if (choiceState(control).changing || !gtk_toggle_button_get_active(button)) return;
        setSettingsChoiceActive(control, GPOINTER_TO_INT(
            g_object_get_data(G_OBJECT(button), "modernime-choice-index")));
        gtk_popover_popdown(GTK_POPOVER(choiceState(control).popover));
    }), choice, GConnectFlags(0));
    state.buttons.push_back(item);
    gtk_box_pack_start(GTK_BOX(state.items), item, FALSE, FALSE, 0);
    gtk_widget_show_all(item);
}

void setSettingsChoiceActive(GtkWidget *choice, int index) {
    auto &state = choiceState(choice);
    if (index < 0 || index >= static_cast<int>(state.labels.size()) || index == state.selected) return;
    state.selected = index;
    state.changing = true;
    gtk_label_set_text(GTK_LABEL(state.label), state.labels[index].c_str());
    gtk_popover_set_default_widget(GTK_POPOVER(state.popover), state.buttons[index]);
    for (std::size_t i = 0; i < state.buttons.size(); ++i) {
        g_object_set(state.buttons[i], "active", static_cast<int>(i) == index, nullptr);
    }
    state.changing = false;
    if (state.previews[index]) {
        auto *small = gdk_pixbuf_scale_simple(state.previews[index], 37, 20, GDK_INTERP_BILINEAR);
        gtk_image_set_from_pixbuf(GTK_IMAGE(state.swatch), small);
        g_object_unref(small);
        gtk_widget_show(state.swatch);
    } else {
        gtk_widget_hide(state.swatch);
    }
    gtk_widget_set_tooltip_text(choice, state.labels[index].c_str());
    g_signal_emit(choice, changedSignal, 0);
}

} // namespace modernime::settings
