#include "modernime/settings/settings_disclosure.h"

#include <algorithm>

namespace modernime::settings {
namespace {

constexpr const char *kDisclosure = "modernime-settings-disclosure";

struct Disclosure {
    GtkWidget *controller;
    GtkWidget *revealer;
    GtkWidget *arrow;
    bool animate = true;
    bool followReveal = false;

    ~Disclosure() {
        if (revealer != nullptr) {
            g_object_remove_weak_pointer(G_OBJECT(revealer),
                reinterpret_cast<gpointer *>(&revealer));
        }
        if (arrow != nullptr) {
            g_object_remove_weak_pointer(G_OBJECT(arrow),
                reinterpret_cast<gpointer *>(&arrow));
        }
    }
};

bool animationsEnabled(GtkWidget *widget) {
    gboolean enabled = TRUE;
    g_object_get(gtk_widget_get_settings(widget), "gtk-enable-animations",
                 &enabled, nullptr);
    return enabled;
}

bool isExpanded(GtkWidget *controller) {
    return GTK_IS_EXPANDER(controller)
        ? gtk_expander_get_expanded(GTK_EXPANDER(controller))
        : gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(controller));
}

void keepDisclosureVisible(Disclosure *state) {
    if (!state || !state->revealer || !state->followReveal ||
        !gtk_widget_has_focus(state->controller) || !isExpanded(state->controller)) return;
    auto *scroll = gtk_widget_get_ancestor(state->controller, GTK_TYPE_SCROLLED_WINDOW);
    if (!scroll) return;
    auto *viewport = gtk_bin_get_child(GTK_BIN(scroll));
    auto *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
    const double page = gtk_adjustment_get_page_size(adjustment);
    int x = 0, y = 0;
    if (page <= 0 || !gtk_widget_translate_coordinates(state->controller, viewport, 0, 0, &x, &y)) return;
    const double value = gtk_adjustment_get_value(adjustment);
    const double top = value + y - 8;
    const double bottom = top + gtk_widget_get_allocated_height(state->controller) +
        gtk_widget_get_allocated_height(state->revealer) + 16;
    double next = value;
    if (bottom - top > page || top < value) next = top;
    else if (bottom > value + page) next = bottom - page;
    gtk_adjustment_set_value(adjustment, std::clamp(next,
        gtk_adjustment_get_lower(adjustment),
        std::max(gtk_adjustment_get_lower(adjustment), gtk_adjustment_get_upper(adjustment) - page)));
}

void setClass(GtkWidget *widget, const char *name, bool present) {
    auto *context = gtk_widget_get_style_context(widget);
    if (present) gtk_style_context_add_class(context, name);
    else gtk_style_context_remove_class(context, name);
}

void installArrowStyle(GtkWidget *widget) {
    auto *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, R"CSS(
        .modernime-disclosure-arrow {
            -gtk-icon-transform: rotate(0deg);
            transition: -gtk-icon-transform 140ms ease-out;
        }
        .modernime-disclosure-arrow.expanded {
            -gtk-icon-transform: rotate(90deg);
            transition-duration: 150ms;
        }
        .modernime-disclosure-arrow.half-turn.expanded {
            -gtk-icon-transform: rotate(180deg);
        }
        .modernime-disclosure-expander > title > arrow {
            transition: -gtk-icon-transform 140ms ease-out;
        }
        .modernime-disclosure-expander > title > arrow:checked {
            transition-duration: 150ms;
        }
        .modernime-disclosure-arrow.immediate,
        .modernime-disclosure-expander.immediate > title > arrow {
            transition-duration: 0ms;
        }
    )CSS", -1, nullptr);
    gtk_style_context_add_provider(gtk_widget_get_style_context(widget),
        GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    g_object_unref(provider);
}

void updateDisclosure(Disclosure *state) {
    if (state->revealer == nullptr) return;
    const bool expanded = isExpanded(state->controller);
    const bool animate = state->animate && animationsEnabled(state->controller) &&
                         gtk_widget_get_mapped(state->controller);
    // Follow a focused disclosure while its native reveal grows. Search opens
    // synchronously and keeps owning its target/focus positioning.
    state->followReveal = expanded && state->animate && gtk_widget_get_mapped(state->controller);
    if (!expanded) {
        auto *window = gtk_widget_get_toplevel(state->controller);
        auto *focus = GTK_IS_WINDOW(window) ? gtk_window_get_focus(GTK_WINDOW(window)) : nullptr;
        if (focus != nullptr && gtk_widget_is_ancestor(focus, state->revealer)) {
            gtk_widget_grab_focus(state->controller);
        }
    }
    auto *arrow = state->arrow != nullptr ? state->arrow : state->controller;
    setClass(arrow, "immediate", !animate);
    if (state->arrow != nullptr) setClass(arrow, "expanded", expanded);
    setSettingsRevealed(state->revealer, expanded, animate);
    atk_object_notify_state_change(gtk_widget_get_accessible(state->controller),
                                   ATK_STATE_EXPANDED, expanded);
}

} // namespace

GtkWidget *createSettingsDisclosure(GtkWidget *controller, GtkWidget *content) {
    auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    auto *revealer = gtk_revealer_new();
    gtk_container_add(GTK_CONTAINER(revealer), content);
    gtk_box_pack_start(GTK_BOX(box), controller, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), revealer, FALSE, FALSE, 0);
    bindSettingsDisclosure(controller, revealer);
    return box;
}

void setSettingsRevealed(GtkWidget *widget, bool expanded, bool animate) {
    auto *revealer = GTK_REVEALER(widget);
    animate = animate && animationsEnabled(widget) && gtk_widget_get_mapped(widget);
    gtk_revealer_set_transition_type(revealer, GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(revealer, animate ? (expanded ? 150 : 140) : 0);
    auto *child = gtk_bin_get_child(GTK_BIN(revealer));
    const bool settled = expanded ? gtk_revealer_get_child_revealed(revealer)
        : child == nullptr || !gtk_widget_get_child_visible(child);
    if (!animate && !settled && gtk_revealer_get_reveal_child(revealer) == expanded) {
        // A repeated search or an unmap may arrive halfway through a transition.
        // Retarget synchronously so the child is fully visible before focusing.
        gtk_revealer_set_reveal_child(revealer, !expanded);
    }
    gtk_revealer_set_reveal_child(revealer, expanded);
}

void bindSettingsDisclosure(GtkWidget *controller, GtkWidget *revealer,
                            GtkWidget *arrow, bool halfTurnArrow) {
    auto *state = new Disclosure{controller, revealer, arrow};
    g_object_add_weak_pointer(G_OBJECT(revealer),
        reinterpret_cast<gpointer *>(&state->revealer));
    if (arrow != nullptr) {
        g_object_add_weak_pointer(G_OBJECT(arrow),
            reinterpret_cast<gpointer *>(&state->arrow));
        setClass(arrow, "modernime-disclosure-arrow", true);
        setClass(arrow, "half-turn", halfTurnArrow);
        installArrowStyle(arrow);
    } else if (GTK_IS_EXPANDER(controller)) {
        setClass(controller, "modernime-disclosure-expander", true);
        installArrowStyle(controller);
    }
    g_object_set_data_full(G_OBJECT(controller), kDisclosure, state,
        +[](gpointer data) { delete static_cast<Disclosure *>(data); });
    if (GTK_IS_BUTTON(controller)) gtk_widget_set_focus_on_click(controller, TRUE);
    g_signal_connect_object(revealer, "size-allocate", G_CALLBACK(+[](GtkWidget *, GtkAllocation *, gpointer data) {
        keepDisclosureVisible(static_cast<Disclosure *>(g_object_get_data(G_OBJECT(data), kDisclosure)));
    }), controller, G_CONNECT_AFTER);
    g_signal_connect_object(revealer, "notify::child-revealed", G_CALLBACK(+[](GObject *, GParamSpec *, gpointer data) {
        keepDisclosureVisible(static_cast<Disclosure *>(g_object_get_data(G_OBJECT(data), kDisclosure)));
    }), controller, GConnectFlags(0));
    g_signal_connect(controller, "focus-out-event", G_CALLBACK(+[](GtkWidget *, GdkEventFocus *, gpointer data) -> gboolean {
        static_cast<Disclosure *>(data)->followReveal = false;
        return FALSE;
    }), state);
    if (GTK_IS_EXPANDER(controller)) {
        g_signal_connect(controller, "notify::expanded",
            G_CALLBACK(+[](GObject *, GParamSpec *, gpointer data) {
                updateDisclosure(static_cast<Disclosure *>(data));
            }), state);
    } else {
        g_signal_connect(controller, "toggled",
            G_CALLBACK(+[](GtkToggleButton *, gpointer data) {
                updateDisclosure(static_cast<Disclosure *>(data));
            }), state);
    }
    g_signal_connect(controller, "unmap",
        G_CALLBACK(+[](GtkWidget *, gpointer data) {
            auto *state = static_cast<Disclosure *>(data);
            state->animate = false;
            updateDisclosure(state);
            state->animate = true;
        }), state);
    g_signal_connect_object(gtk_widget_get_settings(controller),
        "notify::gtk-enable-animations",
        G_CALLBACK(+[](GObject *, GParamSpec *, gpointer widget) {
            if (!animationsEnabled(GTK_WIDGET(widget))) {
                updateDisclosure(static_cast<Disclosure *>(g_object_get_data(
                    G_OBJECT(widget), kDisclosure)));
            }
        }), controller, GConnectFlags(0));
    auto *accessible = gtk_widget_get_accessible(controller);
    atk_object_add_relationship(accessible, ATK_RELATION_CONTROLLER_FOR,
                                gtk_widget_get_accessible(revealer));
    updateDisclosure(state);
}

void setSettingsDisclosureExpanded(GtkWidget *controller, bool expanded,
                                   bool animate) {
    auto *state = static_cast<Disclosure *>(g_object_get_data(G_OBJECT(controller), kDisclosure));
    state->animate = animate;
    const bool changed = isExpanded(controller) != expanded;
    if (GTK_IS_EXPANDER(controller)) {
        gtk_expander_set_expanded(GTK_EXPANDER(controller), expanded);
    } else {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(controller), expanded);
    }
    // Also finish an already requested expansion when search needs it now.
    if (!changed) updateDisclosure(state);
    state->animate = true;
}

} // namespace modernime::settings
