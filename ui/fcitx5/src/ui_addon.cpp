#include "modernime/ui/ui_addon.h"

#include "modernime/fcitx5/fcitx_engine.h"
#include "modernime/ui/cairo_render_surface.h"
#include "modernime/ui/candidate_bar_layout.h"
#include "modernime/ui/candidate_pagination.h"
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
#include <string>
#include <unordered_map>

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
    // 测量缓存：textWidth 在每次按键的布局/分页路径上对全部候选反复调用，
    // 原实现每次都新建再销毁 PangoLayout 和字体描述，并重复测量相同文本。
    // 复用单个布局对象 + 按样式缓存字体描述 + 按文本缓存测量结果。
    mutable PangoLayout *sharedTextLayout = nullptr;
    mutable std::unordered_map<std::string, PangoFontDescription *>
        sharedFontCache;
    mutable std::unordered_map<std::string, double> sharedWidthCache;
    // AppIndicator 的 set_label/set_title 每次调用都会触发 DBus 属性通知，
    // 值未变化时跳过，避免每键一次 DBus 往返。
    std::string lastIndicatorLabel;
    std::string lastIndicatorTitle;
    // 最近一次下发的窗口尺寸：布局尺寸不变时跳过 resize，避免每键
    // 一次 XConfigureWindow 往返。
    int lastWindowWidth = -1;
    int lastWindowHeight = -1;

    static constexpr double originX = 0.0;
    static constexpr double originY = 0.0;

    void setWindowSize() {
        const int width = windowWidth();
        const int height = windowHeight();
        if (width == lastWindowWidth && height == lastWindowHeight) {
            return;
        }
        lastWindowWidth = width;
        lastWindowHeight = height;
        gtk_widget_set_size_request(drawingArea, width, height);
        gtk_window_resize(GTK_WINDOW(window), width, height);
    }

    // 立刻派发已就绪的 GTK 事件。update() 里的 queue_draw/show/hide 只
    // 标记请求，真正送到 X server 要等 GTK 主循环运转；这里同步泵一次，
    // 让候选窗在本键处理内就完成显示/隐藏/重绘请求的下发，而不是等
    // 下一个周期泵（活跃期 8ms、空闲期 50ms）。
    void pumpPendingGtkEvents() {
        int iterations = 32;
        while (iterations-- > 0 && g_main_context_iteration(nullptr, FALSE)) {}
        if (auto *disp = gdk_display_get_default()) {
            gdk_display_flush(disp);
        }
    }

    // 窗口与绘制内容共用逻辑坐标；GTK3 在 HiDPI 显示器上会自动按设备
    // 缩放放大（绘制上下文已应用 scale），这里不能再乘 scaleFactor，
    // 否则会双重缩放导致面板尺寸和位置全面错位。
    int windowWidth() const {
        return static_cast<int>(std::ceil(
            layout.panel.x + layout.panel.width + style.shadowSpread -
            originX));
    }

    int windowHeight() const {
        return static_cast<int>(std::ceil(
            layout.panel.y + layout.panel.height + style.shadowSpread -
            originY + style.shadowOffsetY));
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
        int desiredX = cursor.left();
        int desiredY = cursorAnchorBottom(cursor.top(), cursor.height());
        const auto panelWidth = windowWidth();
        const auto panelHeight = windowHeight();
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

    // 复用单个 PangoLayout 测量文本；布局对象在首次测量时创建，
    // 之后仅更新字体与文本，避免每候选每键一次 layout 创建销毁。
    PangoLayout *acquireTextLayout() const {
        if (sharedTextLayout == nullptr) {
            PangoContext *context = gtk_widget_get_pango_context(drawingArea);
            sharedTextLayout = pango_layout_new(context);
        }
        return sharedTextLayout;
    }

    const PangoFontDescription *fontForStyle(
        const TextStyle &textStyle) const {
        std::string key = textStyle.family;
        key += '|';
        key += std::to_string(textStyle.size);
        key += '|';
        key += std::to_string(textStyle.weight);
        if (const auto found = sharedFontCache.find(key);
            found != sharedFontCache.end()) {
            return found->second;
        }
        PangoFontDescription *font = pango_font_description_new();
        pango_font_description_set_family(font, textStyle.family.c_str());
        pango_font_description_set_absolute_size(
            font, textStyle.size * PANGO_SCALE);
        pango_font_description_set_weight(
            font, textStyle.weight >= 700 ? PANGO_WEIGHT_BOLD
                                          : PANGO_WEIGHT_NORMAL);
        sharedFontCache.emplace(std::move(key), font);
        return font;
    }

    double textWidth(std::string_view value,
                     const TextStyle &textStyle) const {
        PangoLayout *textLayout = acquireTextLayout();
        pango_layout_set_font_description(textLayout,
                                          fontForStyle(textStyle));
        pango_layout_set_text(textLayout, value.data(),
                              static_cast<int>(value.size()));
        int width = 0;
        int height = 0;
        pango_layout_get_pixel_size(textLayout, &width, &height);
        (void)height;
        return static_cast<double>(width);
    }

    double textWidth(std::string_view value) const {
        // 按完整文本缓存测量结果：同一次组合里布局与分页会反复测量
        // 相同候选，缓存命中时完全跳过 Pango。缓存超限时整体清空，
        // 防止长会话下无限增长（候选串空间有限，通常远达不到上限）。
        const std::string cacheKey(value);
        if (const auto found = sharedWidthCache.find(cacheKey);
            found != sharedWidthCache.end()) {
            return found->second;
        }
        double width = 0.0;
        const auto separator = value.find('.');
        if (separator == std::string_view::npos) {
            width = textWidth(value, style.candidateText);
        } else {
            width = textWidth(value.substr(0, separator + 1),
                              style.candidateNumberText) +
                    textWidth(value.substr(separator + 1),
                              style.candidateText);
        }
        if (sharedWidthCache.size() >= 4096) {
            sharedWidthCache.clear();
        }
        sharedWidthCache.emplace(cacheKey, width);
        return width;
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
        if (label != lastIndicatorLabel) {
            app_indicator_set_label(indicator, label.c_str(), "");
            lastIndicatorLabel = std::move(label);
        }
        if (title != lastIndicatorTitle) {
            app_indicator_set_title(indicator, title.c_str());
            lastIndicatorTitle = std::move(title);
        }
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
    const auto *variableCandidates =
        dynamic_cast<const fcitx5::FcitxCandidateList *>(candidates.get());
    const auto pageBegin = variableCandidates == nullptr
                               ? std::size_t{0}
                               : variableCandidates->pageBegin();
    for (int index = 0; index < candidates->size(); ++index) {
        const auto &candidate = candidates->candidate(index);
        page.items.push_back({candidate.text().toString(), {},
                              pageBegin + static_cast<std::size_t>(index)});
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
    gtk_widget_realize(impl_->window);
    // 预热 Pango 布局与 Fontconfig 字体库缓存，消除首键测量中文字体的冷启动开销
    impl_->textWidth("你好世界");
    impl_->textWidth("1234567890");
    if (instance != nullptr) {
        impl_->gtkEventSource = instance->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 30000, 30000,
            [this](fcitx::EventSourceTime *source, uint64_t) {
                g_main_context_iteration(nullptr, FALSE);
                // 候选窗可见（正在打字）时用 8ms 快泵，及时处理 X 的异步
                // 回包（expose/configure 等）；空闲时退到 50ms 慢泵保底
                // 处理托盘菜单等零星 GTK 事件，避免高频空转。
                const bool typing = impl_->window != nullptr &&
                                    gtk_widget_get_visible(impl_->window);
                source->setNextInterval(typing ? 8000 : 50000);
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
        auto &panel = inputContext->inputPanel();
        const auto list = panel.candidateList();
        if (list == nullptr || list->empty()) {
            gtk_widget_hide(impl_->window);
            impl_->windowAnchor.reset();
            impl_->pumpPendingGtkEvents();
            return;
        }

        const auto mode =
            list->layoutHint() == fcitx::CandidateLayoutHint::Vertical
                ? core::CandidatePageMode::Clipboard
                : core::CandidatePageMode::Pinyin;
        const auto textWidth = candidateTextWidthForMode(
            mode,
            [impl = impl_.get()](std::string_view value) {
                return impl->textWidth(value);
            },
            [impl = impl_.get()](std::string_view value) {
                return impl->textWidth(value, impl->style.clipboardText);
            });
        if (mode == core::CandidatePageMode::Pinyin) {
            if (auto *variableCandidates =
                    dynamic_cast<fcitx5::FcitxCandidateList *>(list.get());
                variableCandidates != nullptr) {
                std::vector<core::CandidateItem> allItems;
                const auto *bulk = list->toBulk();
                const auto total = bulk == nullptr ? 0 : bulk->totalSize();
                allItems.reserve(static_cast<std::size_t>(std::max(0, total)));
                for (int index = 0; index < total; ++index) {
                    allItems.push_back(
                        {bulk->candidateFromAll(index).text().toString(), {},
                         static_cast<std::size_t>(index)});
                }
                variableCandidates->setPageBoundaries(
                    CandidatePagination::partition(allItems, impl_->metrics,
                                                   textWidth));
            }
        }
        const auto page = pageFromInputPanel(panel);
        impl_->layout = CandidateBarLayout::measure(
            page, impl_->metrics, textWidth);
        impl_->setWindowSize();
        impl_->positionWindow(inputContext);
        gtk_widget_queue_draw(impl_->drawingArea);
        if (!impl_->suspended) {
            gtk_widget_show_all(impl_->window);
            if (impl_->gtkEventSource) {
                impl_->gtkEventSource->setNextInterval(8000);
                impl_->gtkEventSource->setEnabled(true);
            }
        }
        impl_->pumpPendingGtkEvents();
        return;
    }
    // 光标移动（CursorRect）通知也重新跟随光标定位；
    // 无效光标时保留上次位置。
    impl_->positionWindow(inputContext);
    impl_->pumpPendingGtkEvents();
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
