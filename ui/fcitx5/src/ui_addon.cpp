#include "modernime/ui/ui_addon.h"

#include "modernime/ui/cairo_render_surface.h"
#include "modernime/ui/candidate_bar_layout.h"
#include "modernime/ui/candidate_bar_renderer.h"
#include "modernime/ui/status_indicator.h"

#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx-utils/event.h>

#include <gtk/gtk.h>
#include <libayatana-appindicator/app-indicator.h>

#include <cmath>
#include <memory>

namespace modernime::ui {

struct ModernIMEUserInterface::Impl final {
    GtkWidget *window = nullptr;
    GtkWidget *drawingArea = nullptr;
    AppIndicator *indicator = nullptr;
    GtkWidget *indicatorMenu = nullptr;
    CandidateBarLayout layout;
    RenderStyle style = RenderStyle::reference();
    CandidateBarMetrics metrics = CandidateBarMetrics::reference();
    bool gtkAvailable = false;
    bool suspended = false;
    std::unique_ptr<fcitx::EventSourceTime> gtkEventSource;

    static constexpr double originX = 0.0;
    static constexpr double originY = 0.0;

    void setWindowSize(double scale) {
        const int width = static_cast<int>(std::ceil(
            (metrics.panelX + metrics.panelWidth + style.shadowSpread - originX) *
            scale));
        const int height = static_cast<int>(std::ceil(
            (metrics.panelY + metrics.panelHeight + style.shadowSpread - originY +
             style.shadowOffsetY) *
            scale));
        gtk_widget_set_size_request(drawingArea, width, height);
        gtk_window_resize(GTK_WINDOW(window), width, height);
    }
};

namespace {

void toggleInputMethod(GtkMenuItem *, gpointer data) {
    if (auto *instance = static_cast<fcitx::Instance *>(data);
        instance != nullptr) {
        instance->toggle();
    }
}

void drawCallback(GtkWidget *, cairo_t *context, gpointer data) {
    static_cast<ModernIMEUserInterface *>(data)->draw(context);
}

void configureWindow(GtkWidget *window) {
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(window), TRUE);
    gtk_window_set_accept_focus(GTK_WINDOW(window), FALSE);
    gtk_window_set_type_hint(GTK_WINDOW(window), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
    gtk_widget_set_app_paintable(window, TRUE);

    GdkScreen *screen = gtk_widget_get_screen(window);
    if (screen != nullptr && gdk_screen_is_composited(screen)) {
        GdkVisual *visual = gdk_screen_get_rgba_visual(screen);
        if (visual != nullptr) {
            gtk_widget_set_visual(window, visual);
        }
    }
}

core::CandidatePage pageFromInputPanel(const fcitx::InputPanel &panel) {
    core::CandidatePage page;
    page.preedit = panel.preedit().toString();
    const auto candidates = panel.candidateList();
    if (candidates == nullptr) {
        return page;
    }
    const int cursor = candidates->cursorIndex();
    page.cursor = cursor >= 0 ? static_cast<std::size_t>(cursor) : 0;
    for (int index = 0; index < candidates->size() && index < 9; ++index) {
        const auto &candidate = candidates->candidate(index);
        page.items.push_back({candidate.text().toString(), {},
                              static_cast<std::size_t>(index)});
    }
    return page;
}

} // namespace

ModernIMEUserInterface::ModernIMEUserInterface(fcitx::Instance *instance)
    : impl_(std::make_unique<Impl>()) {
    impl_->gtkAvailable = gtk_init_check(nullptr, nullptr);
    if (!impl_->gtkAvailable) {
        return;
    }
    impl_->window = gtk_window_new(GTK_WINDOW_POPUP);
    configureWindow(impl_->window);
    impl_->indicator = app_indicator_new(
        "modernime-fcitx5", std::string(StatusIndicator::iconName()).c_str(),
        APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    app_indicator_set_title(impl_->indicator,
                            std::string(StatusIndicator::title()).c_str());
    app_indicator_set_status(impl_->indicator, APP_INDICATOR_STATUS_ACTIVE);
    impl_->indicatorMenu = gtk_menu_new();
    auto *indicatorItem = gtk_menu_item_new_with_label(
        std::string(StatusIndicator::title()).c_str());
    g_signal_connect(indicatorItem, "activate",
                     G_CALLBACK(toggleInputMethod), instance);
    gtk_menu_shell_append(GTK_MENU_SHELL(impl_->indicatorMenu), indicatorItem);
    gtk_widget_show_all(impl_->indicatorMenu);
    app_indicator_set_menu(impl_->indicator,
                           GTK_MENU(impl_->indicatorMenu));
    impl_->drawingArea = gtk_drawing_area_new();
    gtk_container_add(GTK_CONTAINER(impl_->window), impl_->drawingArea);
    g_signal_connect(impl_->drawingArea, "draw", G_CALLBACK(drawCallback), this);
    gtk_widget_set_size_request(impl_->drawingArea, 1, 1);
    if (instance != nullptr) {
        impl_->gtkEventSource = instance->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 10000, 1000,
            [](fcitx::EventSourceTime *source, uint64_t) {
                g_main_context_iteration(nullptr, FALSE);
                source->setNextInterval(10000);
                source->setEnabled(true);
                return true;
            });
    }
}

ModernIMEUserInterface::~ModernIMEUserInterface() {
    if (impl_->window != nullptr) {
        gtk_widget_destroy(impl_->window);
    }
    if (impl_->indicatorMenu != nullptr) {
        gtk_widget_destroy(impl_->indicatorMenu);
    }
    if (impl_->indicator != nullptr) {
        g_object_unref(impl_->indicator);
    }
}

void ModernIMEUserInterface::draw(cairo_t *context) {
    if (impl_->suspended) {
        return;
    }
    cairo_save(context);
    cairo_translate(context, -Impl::originX, -Impl::originY);
    CairoRenderSurface surface(context);
    CandidateBarRenderer::render(surface, impl_->layout, impl_->style);
    cairo_restore(context);
}

void ModernIMEUserInterface::update(fcitx::UserInterfaceComponent component,
                                    fcitx::InputContext *inputContext) {
    if (!impl_->gtkAvailable ||
        component != fcitx::UserInterfaceComponent::InputPanel ||
        inputContext == nullptr) {
        return;
    }
    const auto page = pageFromInputPanel(inputContext->inputPanel());
    if (page.items.empty()) {
        gtk_widget_hide(impl_->window);
        return;
    }

    impl_->layout = CandidateBarLayout::measure(page, impl_->metrics);
    impl_->setWindowSize(inputContext->scaleFactor());
    const auto &cursor = inputContext->cursorRect();
    gtk_window_move(GTK_WINDOW(impl_->window), cursor.left(),
                    cursor.top() + cursor.height());
    gtk_widget_queue_draw(impl_->drawingArea);
    if (!impl_->suspended) {
        gtk_widget_show_all(impl_->window);
    }
}

bool ModernIMEUserInterface::available() { return impl_->gtkAvailable; }

void ModernIMEUserInterface::suspend() {
    impl_->suspended = true;
    if (impl_->window != nullptr) {
        gtk_widget_hide(impl_->window);
    }
}

void ModernIMEUserInterface::resume() { impl_->suspended = false; }

} // namespace modernime::ui

namespace {

class ModernIMEUIAddonFactory final : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new modernime::ui::ModernIMEUserInterface(
            manager != nullptr ? manager->instance() : nullptr);
    }
};

} // namespace

FCITX_ADDON_FACTORY(ModernIMEUIAddonFactory)
