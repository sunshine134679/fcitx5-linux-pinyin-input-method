#include "modernime/settings/detail/gtk_raii.h"

#include <glib-object.h>

#include <cassert>
#include <type_traits>

namespace {

bool cleanupCalled = false;

struct MarkCleanup final {
    void operator()(GInitiallyUnowned *) const { cleanupCalled = true; }
};

void onFinalized(gpointer data, GObject *) {
    *static_cast<bool *>(data) = true;
}

} // namespace

int main() {
    using modernime::settings::detail::GObjectHandle;
    using modernime::settings::detail::FloatingObjectGuard;
    using modernime::settings::detail::GtkWidgetGuard;

    static_assert(!std::is_copy_constructible_v<GObjectHandle<GObject>>);
    static_assert(std::is_move_constructible_v<GObjectHandle<GObject>>);
    static_assert(!std::is_copy_constructible_v<GtkWidgetGuard>);

    bool finalized = false;
    auto *object = G_OBJECT(g_object_new(G_TYPE_OBJECT, nullptr));
    g_object_weak_ref(object, onFinalized, &finalized);
    {
        auto owner = GObjectHandle<GObject>::adopt(object);
        assert(owner.get() == object);
        auto moved = std::move(owner);
        assert(owner.get() == nullptr);
        assert(moved.get() == object);
    }
    assert(finalized);

    finalized = false;
    cleanupCalled = false;
    auto *floating = G_INITIALLY_UNOWNED(
        g_object_new(G_TYPE_INITIALLY_UNOWNED, nullptr));
    assert(g_object_is_floating(floating));
    g_object_weak_ref(G_OBJECT(floating), onFinalized, &finalized);
    try {
        FloatingObjectGuard<GInitiallyUnowned, MarkCleanup> guard(floating);
        throw 7;
    } catch (int value) {
        assert(value == 7);
    }
    assert(cleanupCalled);
    assert(finalized);

    finalized = false;
    floating = G_INITIALLY_UNOWNED(
        g_object_new(G_TYPE_INITIALLY_UNOWNED, nullptr));
    g_object_weak_ref(G_OBJECT(floating), onFinalized, &finalized);
    {
        FloatingObjectGuard<GInitiallyUnowned> guard(floating);
        assert(guard.release() == floating);
    }
    assert(!finalized);
    g_object_ref_sink(floating);
    g_object_unref(floating);
    assert(finalized);
}
