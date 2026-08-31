#include "modernime/ui/ui_addon.h"

#include "modernime/ui/cairo_render_surface.h"
#include "modernime/ui/candidate_bar_layout.h"
#include "modernime/ui/candidate_bar_renderer.h"
#include "modernime/ui/status_indicator.h"
#include "modernime/ui/window_anchor.h"

#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx-utils/event.h>

#include <gtk/gtk.h>
#include <libayatana-appindicator/app-indicator.h>
#include <pango/pango.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <memory>

namespace modernime::ui {

struct ModernIMEUserInterface::Impl final {
    fcitx::Instance *instance = nullptr;
    GtkWidget *window = nullptr;
    GtkWidget *drawingArea = nullptr;
    AppIndicator *indicator = nullptr;
    GtkWidget *indicatorMenu = nullptr;
    CandidateBarLayout layout;
    WindowAnchor windowAnchor;
    RenderStyle style = RenderStyle::reference();
    CandidateBarMetrics metrics = CandidateBarMetrics::reference();
    bool gtkAvailable = false;
    bool suspended = false;
    bool wayland = false;
    bool waylandAnchorWarningLogged = false;
    std::unique_ptr<fcitx::EventSourceTime> gtkEventSource;

    static constexpr double originX = 0.0;
    static constexpr double originY = 0.0;

    void setWindowSize(double scale) {
        gtk_widget_set_size_request(drawingArea, windowWidth(scale),
                                    windowHeight(scale));
        gtk_window_resize(GTK_WINDOW(window), windowWidth(scale),
                          windowHeight(scale));
    }

    int windowWidth(double scale) const {
        return static_cast<int>(std::ceil(
            (layout.panel.x + layout.panel.width + style.shadowSpread -
             originX) *
            scale));
    }

    int windowHeight(double scale) const {
        return static_cast<int>(std::ceil(
            (layout.panel.y + layout.panel.height + style.shadowSpread -
             originY + style.shadowOffsetY) *
            scale));
    }

    // 把候选栏锚定到输入光标下方；光标所在显示器放不下时翻到光标上方，
    // 并把窗口完全钳制在工作区内。
    // 注意：Wayland 不支持客户端定位浮动窗口（gtk_window_move 是空操作），
    // 位置由合成器决定；尚未接入 fcitx5 窗口系统（windowing）接口前，
    // Wayland 会话中的候选栏定位属于平台限制。
    void positionWindow(fcitx::InputContext *inputContext) {
        if (inputContext == nullptr) {
            return;
        }
        if (wayland) {
            if (!waylandAnchorWarningLogged) {
                g_warning(
                    "ModernIME candidate bar cannot be anchored on Wayland; "
                    "the compositor decides its position. X11 sessions anchor "
                    "the bar to the text cursor.");
                waylandAnchorWarningLogged = true;
            }
            return;
        }
        const auto &cursor = inputContext->cursorRect();
        if (cursor.isEmpty()) {
            return;  // 前端未提供有效光标，保留上次位置
        }
        const auto scale = inputContext->scaleFactor();
        int desiredX = cursor.left();
        int desiredY = cursor.top() + cursor.height();
        const auto panelWidth = windowWidth(scale);
        const auto panelHeight = windowHeight(scale);
        if (GdkDisplay *display = gtk_widget_get_display(window);
            display != nullptr) {
            GdkRectangle workarea{};
            GdkMonitor *monitor = gdk_display_get_monitor_at_point(
                display, desiredX, desiredY);
            gdk_monitor_get_workarea(monitor, &workarea);
            const auto maxX = std::max(
                workarea.x, workarea.x + workarea.width - panelWidth);
            desiredX = std::clamp(desiredX, workarea.x, maxX);
            if (desiredY + panelHeight > workarea.y + workarea.height) {
                desiredY = cursor.top() - panelHeight;
            }
            const auto maxY = std::max(
                workarea.y, workarea.y + workarea.height - panelHeight);
            desiredY = std::clamp(desiredY, workarea.y, maxY);
        }
        if (windowAnchor.capture(desiredX, desiredY)) {
            gtk_window_move(GTK_WINDOW(window), windowAnchor.x,
                            windowAnchor.y);
        }
    }

    double textWidth(std::string_view value,
                     const TextStyle &textStyle) const {
        PangoContext *context = gtk_widget_get_pango_context(drawingArea);
        PangoLayout *textLayout = pango_layout_new(context);
        PangoFontDescription *font = pango_font_description_new();
        pango_font_description_set_family(font, textStyle.family.c_str());
        pango_font_description_set_absolute_size(
            font, textStyle.size * PANGO_SCALE);
        pango_font_description_set_weight(
            font, textStyle.weight >= 700 ? PANGO_WEIGHT_BOLD
                                          : PANGO_WEIGHT_NORMAL);
        pango_layout_set_font_description(textLayout, font);
        pango_layout_set_text(textLayout, value.data(),
                              static_cast<int>(value.size()));
        int width = 0;
        int height = 0;
        pango_layout_get_pixel_size(textLayout, &width, &height);
        (void)height;
        pango_font_description_free(font);
        g_object_unref(textLayout);
        return static_cast<double>(width);
    }

    double textWidth(std::string_view value) const {
        const auto separator = value.find('.');
        if (separator == std::string_view::npos) {
            return textWidth(value, style.candidateText);
        }
        return textWidth(value.substr(0, separator + 1),
                         style.candidateNumberText) +
               textWidth(value.substr(separator + 1), style.candidateText);
    }

    void updateIndicator(fcitx::InputContext *inputContext) {
        if (indicator == nullptr || instance == nullptr || inputContext == nullptr) {
            return;
        }
        const auto inputMethod = instance->inputMethod(inputContext);
        const auto label =
            std::string(StatusIndicator::labelForInputMethod(inputMethod));
        const auto title =
            std::string(StatusIndicator::titleForInputMethod(inputMethod));
        app_indicator_set_label(indicator, label.c_str(), "");
        app_indicator_set_title(indicator, title.c_str());
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
    page.mode = candidates->layoutHint() == fcitx::CandidateLayoutHint::Vertical
                    ? core::CandidatePageMode::Clipboard
                    : core::CandidatePageMode::Pinyin;
    for (int index = 0; index < candidates->size(); ++index) {
        const auto &candidate = candidates->candidate(index);
        page.items.push_back({candidate.text().toString(), {},
                              static_cast<std::size_t>(index)});
    }
    return page;
}

} // namespace

ModernIMEUserInterface::ModernIMEUserInterface(fcitx::Instance *instance)
    : impl_(std::make_unique<Impl>()) {
    impl_->instance = instance;
    impl_->gtkAvailable = gtk_init_check(nullptr, nullptr);
    if (!impl_->gtkAvailable) {
        return;
    }
    impl_->window = gtk_window_new(GTK_WINDOW_POPUP);
    configureWindow(impl_->window);
    if (GdkDisplay *display = gtk_widget_get_display(impl_->window);
        display != nullptr) {
        const auto *backendName = gdk_display_get_name(display);
        impl_->wayland = backendName != nullptr &&
                         std::string_view(backendName).starts_with("wayland");
    }
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
    if (!impl_->gtkAvailable || inputContext == nullptr) {
        return;
    }
    impl_->updateIndicator(inputContext);
    if (component == fcitx::UserInterfaceComponent::InputPanel) {
        const auto page = pageFromInputPanel(inputContext->inputPanel());
        if (page.items.empty()) {
            gtk_widget_hide(impl_->window);
            impl_->windowAnchor.reset();
            return;
        }

        const auto textWidth = candidateTextWidthForMode(
            page.mode,
            [impl = impl_.get()](std::string_view value) {
                return impl->textWidth(value);
            },
            [impl = impl_.get()](std::string_view value) {
                return impl->textWidth(value, impl->style.clipboardText);
            });
        impl_->layout = CandidateBarLayout::measure(
            page, impl_->metrics, textWidth);
        impl_->setWindowSize(inputContext->scaleFactor());
        impl_->positionWindow(inputContext);
        gtk_widget_queue_draw(impl_->drawingArea);
        if (!impl_->suspended) {
            gtk_widget_show_all(impl_->window);
        }
        return;
    }
    // 光标移动（CursorRect）通知也重新跟随光标定位；
    // 无效光标时保留上次位置。
    impl_->positionWindow(inputContext);
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
