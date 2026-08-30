#pragma once

#include <glib-object.h>
#include <gtk/gtk.h>

#include <utility>

namespace modernime::settings::detail {

template <typename Object>
class GObjectHandle final {
public:
    GObjectHandle() = default;
    ~GObjectHandle() { reset(); }

    GObjectHandle(const GObjectHandle &) = delete;
    GObjectHandle &operator=(const GObjectHandle &) = delete;

    GObjectHandle(GObjectHandle &&other) noexcept
        : object_(std::exchange(other.object_, nullptr)) {}

    GObjectHandle &operator=(GObjectHandle &&other) noexcept {
        if (this != &other) {
            reset();
            object_ = std::exchange(other.object_, nullptr);
        }
        return *this;
    }

    static GObjectHandle adopt(Object *object) {
        return GObjectHandle(object);
    }

    static GObjectHandle sink(Object *object) {
        if (object != nullptr) {
            g_object_ref_sink(object);
        }
        return GObjectHandle(object);
    }

    Object *get() const { return object_; }

    Object *release() { return std::exchange(object_, nullptr); }

    void reset(Object *object = nullptr) {
        if (object_ != nullptr) {
            g_object_unref(object_);
        }
        object_ = object;
    }

private:
    explicit GObjectHandle(Object *object) : object_(object) {}

    Object *object_ = nullptr;
};

struct GtkWidgetDestroy final {
    void operator()(GtkWidget *widget) const {
        gtk_widget_destroy(widget);
    }
};

struct NoObjectCleanup final {
    template <typename Object>
    void operator()(Object *) const noexcept {}
};

template <typename Object, typename Cleanup = NoObjectCleanup>
class FloatingObjectGuard final {
public:
    explicit FloatingObjectGuard(Object *object = nullptr) : object_(object) {}
    ~FloatingObjectGuard() { reset(); }

    FloatingObjectGuard(const FloatingObjectGuard &) = delete;
    FloatingObjectGuard &operator=(const FloatingObjectGuard &) = delete;

    FloatingObjectGuard(FloatingObjectGuard &&other) noexcept
        : object_(std::exchange(other.object_, nullptr)) {}

    FloatingObjectGuard &operator=(FloatingObjectGuard &&other) noexcept {
        if (this != &other) {
            reset();
            object_ = std::exchange(other.object_, nullptr);
        }
        return *this;
    }

    Object *get() const { return object_; }

    Object *release() { return std::exchange(object_, nullptr); }

    void reset(Object *object = nullptr) {
        if (object_ != nullptr) {
            // Fresh GTK widgets and GInitiallyUnowned objects carry a floating
            // reference. Sink it before cleanup so the final unref is valid
            // even when construction failed before a parent adopted it.
            g_object_ref_sink(object_);
            Cleanup{}(object_);
            g_object_unref(object_);
        }
        object_ = object;
    }

private:
    Object *object_ = nullptr;
};

using GtkWidgetHandle = GObjectHandle<GtkWidget>;
using PlainGObjectHandle = GObjectHandle<GObject>;
using GtkWidgetGuard = FloatingObjectGuard<GtkWidget, GtkWidgetDestroy>;

} // namespace modernime::settings::detail
