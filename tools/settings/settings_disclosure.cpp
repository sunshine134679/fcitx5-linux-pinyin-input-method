#include "modernime/settings/settings_disclosure.h"

namespace modernime::settings {
namespace {

constexpr const char *kDisclosure = "modernime-settings-disclosure";

struct Disclosure {
    GtkWidget *controller;
    GtkWidget *revealer;
    GtkWidget *arrow;
    bool animate = true;

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
            transition: -gtk-icon-transform 170ms ease-out;
        }
        .modernime-disclosure-arrow.expanded {
            -gtk-icon-transform: rotate(90deg);
            transition-duration: 220ms;
        }
        .modernime-disclosure-arrow.half-turn.expanded {
            -gtk-icon-transform: rotate(180deg);
        }
        .modernime-disclosure-expander > title > arrow {
            transition: -gtk-icon-transform 170ms ease-out;
        }
        .modernime-disclosure-expander > title > arrow:checked {
            transition-duration: 220ms;
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

void setSettingsRevealed(GtkWidget *widget, bool expanded, bool animate) {
    auto *revealer = GTK_REVEALER(widget);
    animate = animate && animationsEnabled(widget) && gtk_widget_get_mapped(widget);
    gtk_revealer_set_transition_type(revealer, GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(revealer, animate ? (expanded ? 220 : 170) : 0);
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
