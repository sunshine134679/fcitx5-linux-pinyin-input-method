#include "modernime/settings/settings_motion.h"
#include "modernime/settings/settings_widgets.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace modernime::settings {
namespace {
constexpr double pi = 3.141592653589793;
struct Rect { double x = 0, y = 0, width = 0, height = 0; };
double ease(double value) { return 1 - std::pow(1 - std::clamp(value, 0.0, 1.0), 3); }
void rounded(cairo_t *cr, const Rect &r, double radius) {
    radius = std::min({radius, r.width / 2, r.height / 2});
    cairo_new_sub_path(cr);
    cairo_arc(cr, r.x + r.width - radius, r.y + radius, radius, -pi / 2, 0);
    cairo_arc(cr, r.x + r.width - radius, r.y + r.height - radius, radius, 0, pi / 2);
    cairo_arc(cr, r.x + radius, r.y + r.height - radius, radius, pi / 2, pi);
    cairo_arc(cr, r.x + radius, r.y + radius, radius, pi, 3 * pi / 2);
    cairo_close_path(cr);
}
bool bounds(GtkWidget *widget, GtkWidget *relative, Rect &rect) {
    int x = 0, y = 0;
    if (!widget || !gtk_widget_get_mapped(widget) ||
        !gtk_widget_translate_coordinates(widget, relative, 0, 0, &x, &y)) return false;
    rect = {static_cast<double>(x), static_cast<double>(y),
            static_cast<double>(gtk_widget_get_allocated_width(widget)),
            static_cast<double>(gtk_widget_get_allocated_height(widget))};
    return rect.width > 0 && rect.height > 0;
}
void color(cairo_t *cr, GtkWidget *widget, const char *name, double alpha = 1) {
    GdkRGBA rgba{};
    gtk_style_context_lookup_color(gtk_widget_get_style_context(widget), name, &rgba);
    cairo_set_source_rgba(cr, rgba.red, rgba.green, rgba.blue, rgba.alpha * alpha);
}
using Surface = std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)>;
Surface snapshot(GtkWidget *widget, bool background = false) {
    // A choice may just have changed its label. Resolve that queued allocation
    // at the existing size before drawing; never resize the window for a snapshot.
    GtkAllocation allocation{};
    gtk_widget_get_allocation(widget, &allocation);
    gtk_widget_size_allocate(widget, &allocation);
    const int width = gtk_widget_get_allocated_width(widget);
    const int height = gtk_widget_get_allocated_height(widget);
    const int scale = gtk_widget_get_scale_factor(widget);
    Surface surface(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width * scale,
                                               height * scale), cairo_surface_destroy);
    if (cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS) return {nullptr, cairo_surface_destroy};
    cairo_surface_set_device_scale(surface.get(), scale, scale);
    auto *cr = cairo_create(surface.get());
    if (background) { color(cr, widget, "theme_bg_color"); cairo_paint(cr); }
    gtk_widget_draw(widget, cr);
    cairo_destroy(cr);
    return surface;
}
} // namespace

class SettingsMotion::Impl final {
public:
    Impl(GtkWidget *window_, GtkWidget *overlay_, GtkWidget *content_, GtkWidget *navigation_,
         GtkWidget *edit_, GtkWidget *message_)
        : window(window_), overlay(overlay_), content(content_), navigation(navigation_),
          edit(edit_), message(message_) {
        g_weak_ref_init(&hintTarget, nullptr);
        layer = gtk_drawing_area_new();
        // Overlay pass-through applies to its wrapper, not child GdkWindows.
        // Keep this paint-only layer windowless so pointer events reach controls.
        gtk_widget_set_has_window(layer, FALSE);
        gtk_widget_set_hexpand(layer, TRUE);
        gtk_widget_set_vexpand(layer, TRUE);
        gtk_widget_set_can_focus(layer, FALSE);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), layer);
        gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(overlay), layer, TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(navigation), "modernime-nav-motion");
        g_signal_connect(layer, "draw", G_CALLBACK(+[](GtkWidget *, cairo_t *cr, gpointer data) -> gboolean {
            static_cast<Impl *>(data)->draw(cr); return FALSE;
        }), this);
        // GtkBox has a transparent background: draw the plate beneath its children.
        g_signal_connect(navigation, "draw", G_CALLBACK(+[](GtkWidget *, cairo_t *cr, gpointer data) -> gboolean {
            static_cast<Impl *>(data)->drawNavigation(cr); return FALSE;
        }), this);
        g_signal_connect(content, "size-allocate", G_CALLBACK(+[](GtkWidget *, GtkAllocation *size, gpointer data) {
            auto &self = *static_cast<Impl *>(data);
            if (size->width != self.width || size->height != self.height) self.finish();
            self.width = size->width; self.height = size->height;
        }), this);
        g_signal_connect(content, "notify::scale-factor", G_CALLBACK(+[](GObject *, GParamSpec *, gpointer data) {
            static_cast<Impl *>(data)->finish();
        }), this);
        g_signal_connect(content, "unmap", G_CALLBACK(+[](GtkWidget *, gpointer data) {
            static_cast<Impl *>(data)->finish();
        }), this);
        g_signal_connect(window, "event", G_CALLBACK(+[](GtkWidget *, GdkEvent *event, gpointer data) -> gboolean {
            if (event->type == GDK_KEY_PRESS || event->type == GDK_BUTTON_PRESS || event->type == GDK_SCROLL) {
                auto &self = *static_cast<Impl *>(data);
                self.finishTheme(); self.cancelHint();
            }
            return FALSE;
        }), this);
        g_signal_connect_after(window, "draw", G_CALLBACK(+[](GtkWidget *, cairo_t *, gpointer data) -> gboolean {
            auto &self = *static_cast<Impl *>(data);
            if (self.directThemeFrame) {
                self.directThemeFrame = false;
                gtk_style_context_remove_class(gtk_widget_get_style_context(self.window), "modernime-theme-static");
            }
            return FALSE;
        }), this);
        settings = gtk_widget_get_settings(window);
        g_signal_connect(settings, "notify::gtk-enable-animations", G_CALLBACK(+[](GObject *, GParamSpec *, gpointer data) {
            auto &self = *static_cast<Impl *>(data);
            if (!self.enabled()) self.finish();
        }), this);
    }

    ~Impl() {
        finish();
        for (auto *object : {G_OBJECT(layer), G_OBJECT(navigation), G_OBJECT(content),
                            G_OBJECT(window), G_OBJECT(settings)})
            g_signal_handlers_disconnect_by_data(object, this);
        g_weak_ref_clear(&hintTarget);
    }

    bool enabled() const {
        gboolean animations = TRUE;
        g_object_get(settings, "gtk-enable-animations", &animations, nullptr);
        return animations && gtk_widget_get_mapped(content);
    }
    gint64 now() const {
        auto *clock = gtk_widget_get_frame_clock(content);
        return clock ? gdk_frame_clock_get_frame_time(clock) : g_get_monotonic_time();
    }
    static double elapsed(gint64 time, gint64 start, unsigned duration) {
        return std::clamp(static_cast<double>(time - start) / (duration * 1000.0), 0.0, 1.0);
    }
    void ensureTick() {
        if (tick || !enabled()) return;
        tick = gtk_widget_add_tick_callback(content, +[](GtkWidget *, GdkFrameClock *clock, gpointer data) -> gboolean {
            auto &self = *static_cast<Impl *>(data);
            self.advance(gdk_frame_clock_get_frame_time(clock));
            if (self.active()) return G_SOURCE_CONTINUE;
            self.tick = 0;
            return G_SOURCE_REMOVE;
        }, this, nullptr);
    }
    bool active() const { return oldTheme || moving || hintPending || hintStart || oldStatus || messageMoving; }

    void changeTheme(std::string_view previous, std::string_view next, GtkWidget *origin, unsigned duration) {
        if (previous == next) return;
        cancelHint();
        // GtkPopover is drawn outside the content subtree. Remove it before the reveal.
        if (GTK_IS_MENU_BUTTON(origin)) {
            auto *popover = gtk_menu_button_get_popover(GTK_MENU_BUTTON(origin));
            if (popover) gtk_widget_hide(GTK_WIDGET(popover));
        }
        if (!enabled() || previous == "contrast" || next == "contrast") {
            finishTheme();
            directThemeFrame = true;
            gtk_style_context_add_class(gtk_widget_get_style_context(window), "modernime-theme-static");
            applySettingsTheme(next);
            return;
        }
        auto visible = snapshot(content, true);
        if (oldTheme && visible) {
            auto *cr = cairo_create(visible.get());
            drawTheme(cr); // Retarget from what the user currently sees, without queuing.
            cairo_destroy(cr);
        }
        oldTheme = std::move(visible);
        if (!oldTheme) { finishTheme(); applySettingsTheme(next); return; }
        Rect anchor;
        if (!bounds(origin, content, anchor)) anchor = {width / 2.0, height / 2.0, 0, 0};
        originX = anchor.x + anchor.width / 2;
        originY = anchor.y + anchor.height / 2;
        radius = std::hypot(std::max(originX, width - originX), std::max(originY, height - originY));
        themeDuration = duration; themeStart = now(); themeProgress = 0;
        // During the reveal every live widget must already use the final palette.
        gtk_style_context_add_class(gtk_widget_get_style_context(window), "modernime-theme-reveal");
        applySettingsTheme(next);
        ensureTick(); gtk_widget_queue_draw(layer);
    }
    void finishTheme() {
        oldTheme.reset();
        directThemeFrame = false;
        gtk_style_context_remove_class(gtk_widget_get_style_context(window), "modernime-theme-static");
        gtk_style_context_remove_class(gtk_widget_get_style_context(window), "modernime-theme-reveal");
        gtk_widget_queue_draw(layer);
    }
    void selectNavigation(GtkWidget *button) {
        if (selected == button) return;
        Rect target;
        const bool allocated = bounds(button, navigation, target);
        if (!moving && selected) bounds(selected, navigation, plate);
        selected = button;
        from = plate;
        if (!enabled() || !allocated || plate.height == 0) {
            plate = target; moving = false;
        } else {
            navStart = now(); moving = true;
        }
        ensureTick(); gtk_widget_queue_draw(navigation);
    }
    void drawNavigation(cairo_t *cr) {
        if (!selected) return;
        if (!moving && !bounds(selected, navigation, plate)) return;
        if (plate.width <= 1 || plate.height <= 1) return;
        Rect r{plate.x + .5, plate.y + .5, plate.width - 1, plate.height - 1};
        cairo_save(cr);
        rounded(cr, r, 10);
        color(cr, navigation, "modernime_accent_surface", navOpacity); cairo_fill_preserve(cr);
        color(cr, navigation, "modernime_nav_border_color", navOpacity);
        cairo_set_line_width(cr, 1); cairo_stroke(cr);
        rounded(cr, {r.x + 4, r.y + 10, 3, std::max(0.0, r.height - 20)}, 1.5);
        color(cr, navigation, "modernime_nav_marker_color", navOpacity); cairo_fill(cr);
        cairo_restore(cr);
    }
    void hintFocus(GtkWidget *target) {
        cancelHint();
        if (!target || !enabled()) return;
        g_weak_ref_set(&hintTarget, target);
        hintPending = true; hintRequested = now();
        ensureTick();
    }
    void cancelHint() {
        hintPending = false; hintStart = 0; hintAlpha = 0;
        g_weak_ref_set(&hintTarget, nullptr);
        gtk_widget_queue_draw(layer);
    }
    bool hintBounds(Rect &rect) {
        auto *target = GTK_WIDGET(g_weak_ref_get(&hintTarget));
        if (!target) return false;
        auto *row = target;
        for (auto *parent = target; parent && parent != content; parent = gtk_widget_get_parent(parent)) {
            if (gtk_style_context_has_class(gtk_widget_get_style_context(parent), "modernime-setting-row")) {
                row = parent; break;
            }
        }
        const bool found = bounds(row, content, rect);
        // Clip to the visible page viewport, not the entire scrolled child.
        for (auto *parent = row; found && parent && parent != content; parent = gtk_widget_get_parent(parent)) {
            if (GTK_IS_SCROLLED_WINDOW(parent)) {
                Rect viewport;
                if (bounds(parent, content, viewport)) {
                    const double right = std::min(rect.x + rect.width, viewport.x + viewport.width);
                    const double bottom = std::min(rect.y + rect.height, viewport.y + viewport.height);
                    rect.x = std::max(rect.x, viewport.x); rect.y = std::max(rect.y, viewport.y);
                    rect.width = right - rect.x; rect.height = bottom - rect.y;
                }
            }
        }
        g_object_unref(target);
        return found && rect.width > 0 && rect.height > 0;
    }
    void transitionStatus(unsigned duration) {
        if (!enabled()) return;
        gtk_widget_set_opacity(edit, 1);
        oldStatus = snapshot(edit);
        if (!oldStatus) return;
        bounds(edit, content, statusRect);
        statusStart = now(); statusDuration = duration;
        gtk_widget_set_opacity(edit, 0);
        ensureTick();
    }
    void messageFade(bool visible) {
        messageFrom = gtk_widget_get_opacity(message);
        messageTo = visible ? 1 : 0;
        if (visible && !messageMoving) messageFrom = 0;
        messageStart = now(); messageMoving = enabled();
        if (!messageMoving) finishMessage();
        else gtk_widget_set_opacity(message, messageFrom);
        ensureTick();
    }
    void finishMessage() {
        messageMoving = false;
        gtk_widget_set_opacity(message, messageTo);
        if (messageTo == 0) gtk_label_set_text(GTK_LABEL(message), "");
    }
    void finish() {
        if (tick) { gtk_widget_remove_tick_callback(content, tick); tick = 0; }
        finishTheme(); cancelHint(); moving = false; navOpacity = 1;
        oldStatus.reset(); gtk_widget_set_opacity(edit, 1);
        finishMessage(); gtk_widget_queue_draw(navigation);
    }
    void advance(gint64 time) {
        if (!enabled()) { finish(); return; }
        if (oldTheme) {
            themeProgress = ease(elapsed(time, themeStart, themeDuration));
            if (themeProgress >= 1) finishTheme();
        }
        if (moving) {
            Rect target;
            if (bounds(selected, navigation, target)) {
                const double t = elapsed(time, navStart, 180), p = ease(t);
                plate = {from.x + (target.x - from.x) * p, from.y + (target.y - from.y) * p,
                         from.width + (target.width - from.width) * p,
                         from.height + (target.height - from.height) * p};
                navOpacity = 1 - .65 * std::sin(pi * t) * std::min(1.0, std::abs(target.y - from.y) / 280);
                if (t >= 1) { moving = false; navOpacity = 1; }
            } else moving = false;
            gtk_widget_queue_draw(navigation);
        }
        if (hintPending) {
            auto *target = GTK_WIDGET(g_weak_ref_get(&hintTarget));
            auto *focus = gtk_window_get_focus(GTK_WINDOW(window));
            Rect rect;
            if (target && focus && (target == focus || gtk_widget_is_ancestor(focus, target)) && hintBounds(rect)) {
                // Wait for page/expander allocation to settle, keeping native focus immediate.
                bool transitioning = false;
                for (auto *p = target; p; p = gtk_widget_get_parent(p)) {
                    if (GTK_IS_STACK(p) && gtk_stack_get_transition_running(GTK_STACK(p))) transitioning = true;
                    if (GTK_IS_REVEALER(p) && !gtk_revealer_get_child_revealed(GTK_REVEALER(p))) transitioning = true;
                }
                if (!transitioning) { hintPending = false; hintStart = time; }
            }
            if (target) g_object_unref(target);
            if (time - hintRequested > 1500000) cancelHint();
        }
        if (hintStart) {
            const double ms = static_cast<double>(time - hintStart) / 1000;
            hintAlpha = ms < 100 ? ease(ms / 100) : ms < 350 ? 1 : 1 - ease((ms - 350) / 350);
            if (ms >= 700) cancelHint();
        }
        if (oldStatus) {
            const double p = ease(elapsed(time, statusStart, statusDuration));
            gtk_widget_set_opacity(edit, p);
            if (p >= 1) oldStatus.reset();
        }
        if (messageMoving) {
            const double p = ease(elapsed(time, messageStart, 120));
            gtk_widget_set_opacity(message, messageFrom + (messageTo - messageFrom) * p);
            if (p >= 1) finishMessage();
        }
        gtk_widget_queue_draw(layer);
    }
    void drawTheme(cairo_t *cr) {
        if (!oldTheme) return;
        cairo_save(cr);
        cairo_rectangle(cr, 0, 0, width, height);
        cairo_new_sub_path(cr);
        cairo_arc(cr, originX, originY, radius * themeProgress, 0, 2 * pi);
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD); cairo_clip(cr);
        cairo_set_source_surface(cr, oldTheme.get(), 0, 0); cairo_paint(cr);
        cairo_restore(cr);
    }
    void draw(cairo_t *cr) {
        if (hintAlpha > 0) {
            Rect rect;
            if (hintBounds(rect)) {
                rect.x += .5; rect.y += .5; rect.width -= 1; rect.height -= 1;
                rounded(cr, rect, 8);
                color(cr, content, "modernime_focus_hint_fill", hintAlpha); cairo_fill_preserve(cr);
                color(cr, content, "modernime_focus_hint_border", hintAlpha);
                cairo_set_line_width(cr, 1); cairo_stroke(cr);
            }
        }
        if (oldStatus) {
            cairo_set_source_surface(cr, oldStatus.get(), statusRect.x, statusRect.y);
            cairo_paint_with_alpha(cr, 1 - gtk_widget_get_opacity(edit));
        }
        drawTheme(cr);
    }

    GtkWidget *window, *overlay, *content, *navigation, *edit, *message, *layer = nullptr;
    GtkSettings *settings = nullptr;
    guint tick = 0;
    int width = 0, height = 0;
    Surface oldTheme{nullptr, cairo_surface_destroy}, oldStatus{nullptr, cairo_surface_destroy};
    double originX = 0, originY = 0, radius = 0, themeProgress = 0;
    gint64 themeStart = 0, navStart = 0, hintStart = 0, hintRequested = 0, statusStart = 0, messageStart = 0;
    unsigned themeDuration = 440, statusDuration = 150;
    GtkWidget *selected = nullptr;
    Rect plate, from, statusRect;
    bool moving = false, hintPending = false, messageMoving = false, directThemeFrame = false;
    double navOpacity = 1, hintAlpha = 0, messageFrom = 0, messageTo = 1;
    GWeakRef hintTarget;
};

SettingsMotion::SettingsMotion(GtkWidget *window, GtkWidget *overlay, GtkWidget *content,
                               GtkWidget *navigation, GtkWidget *editStatus, GtkWidget *taskStatus)
    : impl_(std::make_unique<Impl>(window, overlay, content, navigation, editStatus, taskStatus)) {}
SettingsMotion::~SettingsMotion() = default;
void SettingsMotion::changeTheme(std::string_view previous, std::string_view next, GtkWidget *origin, unsigned duration) {
    impl_->changeTheme(previous, next, origin, duration);
}
void SettingsMotion::finishTheme() { impl_->finishTheme(); }
void SettingsMotion::selectNavigation(GtkWidget *button) { impl_->selectNavigation(button); }
void SettingsMotion::hintFocus(GtkWidget *target) { impl_->hintFocus(target); }
void SettingsMotion::cancelHint() { impl_->cancelHint(); }
void SettingsMotion::transitionStatus(unsigned duration) { impl_->transitionStatus(duration); }
void SettingsMotion::showMessage() { impl_->messageFade(true); }
void SettingsMotion::hideMessage() { impl_->messageFade(false); }
void SettingsMotion::finish() { impl_->finish(); }
bool SettingsMotion::animating() const { return impl_->active(); }
} // namespace modernime::settings
