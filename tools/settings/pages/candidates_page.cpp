#include "modernime/settings/pages/candidates_page.h"
#include "modernime/settings/candidate_pin_dialog.h"

#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/settings_widgets.h"
#include "modernime/ui/cairo_render_surface.h"
#include "modernime/ui/candidate_pagination.h"

#include <gtk/gtk.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace modernime::settings {
namespace {

void addStyleClass(GtkWidget *widget, std::string_view className) {
    gtk_style_context_add_class(gtk_widget_get_style_context(widget),
                                std::string(className).c_str());
}

void setTarget(GtkWidget *widget, std::string_view target) {
    g_object_set_data_full(G_OBJECT(widget), "modernime-settings-target",
                           g_strdup(std::string(target).c_str()), g_free);
}

void compactFontCellPadding(GObject *cell, GParamSpec *, gpointer) {
    guint padding = 0;
    g_object_get(cell, "ypad", &padding, nullptr);
    // GTK restores 10px padding in its cell-data function for every row.
    // Keep the native renderer and its font attributes; only reduce padding.
    if (padding != 4) g_object_set(cell, "ypad", 4U, nullptr);
}

void styleFontChooserChild(GtkWidget *widget, gpointer) {
    if (GTK_IS_FONT_CHOOSER_WIDGET(widget)) {
        addStyleClass(widget, "modernime-font-chooser");
    } else if (GTK_IS_SEARCH_ENTRY(widget)) {
        addStyleClass(widget, "modernime-font-search");
        gtk_entry_set_placeholder_text(GTK_ENTRY(widget), "搜索字体名称");
    } else if (GTK_IS_ENTRY(widget) && !GTK_IS_SPIN_BUTTON(widget)) {
        addStyleClass(widget, "modernime-font-preview");
    } else if (GTK_IS_TREE_VIEW(widget)) {
        addStyleClass(widget, "modernime-font-list");
        // FontChooser renders font names with an explicit Pango size, which
        // takes precedence over CSS. Scale that sample without replacing its
        // native font-family, search model or selection callbacks.
        auto *columns = gtk_tree_view_get_columns(GTK_TREE_VIEW(widget));
        for (auto *column = columns; column; column = column->next) {
            auto *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(column->data));
            for (auto *cell = cells; cell; cell = cell->next) {
                if (GTK_IS_CELL_RENDERER_TEXT(cell->data)) {
                    if (!g_object_get_data(G_OBJECT(cell->data), "modernime-compact-font-cell")) {
                        g_object_set_data(G_OBJECT(cell->data), "modernime-compact-font-cell", GINT_TO_POINTER(1));
                        g_signal_connect(cell->data, "notify::ypad", G_CALLBACK(compactFontCellPadding), nullptr);
                    }
                    g_object_set(cell->data, "scale", 0.62, "scale-set", TRUE,
                                 "ypad", 4U, nullptr);
                    gtk_cell_renderer_set_fixed_size(GTK_CELL_RENDERER(cell->data), -1, -1);
                }
            }
            g_list_free(cells);
        }
        g_list_free(columns);
    } else if (GTK_IS_SCROLLED_WINDOW(widget)) {
        gtk_widget_set_size_request(widget, -1, 168);
        gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(widget), 168);
    }
    if (GTK_IS_CONTAINER(widget))
        gtk_container_foreach(GTK_CONTAINER(widget), styleFontChooserChild, nullptr);
}

void styleFontChooser(GtkButton *button, gpointer) {
    // GtkFontButton owns this dialog and its selection/response lifecycle.
    // Scope presentation to its transient window, including subsequent opens.
    auto *parent = gtk_widget_get_toplevel(GTK_WIDGET(button));
    auto *windows = gtk_window_list_toplevels();
    for (auto *item = windows; item != nullptr; item = item->next) {
        auto *dialog = GTK_WIDGET(item->data);
        if (!GTK_IS_FONT_CHOOSER_DIALOG(dialog) ||
            gtk_window_get_transient_for(GTK_WINDOW(dialog)) != GTK_WINDOW(parent)) continue;
        addStyleClass(dialog, kSettingsWindowClass);
        addStyleClass(dialog, "modernime-font-dialog");
        gtk_window_set_default_size(GTK_WINDOW(dialog), 400, 320);
        gtk_container_set_border_width(GTK_CONTAINER(dialog), 0);
        auto *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
        gtk_container_set_border_width(GTK_CONTAINER(content), 12);
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL,
                                       "取消", "保留原来的候选字体");
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_OK,
                                       "使用字体", "在候选预览中使用选中的字体，保存设置后应用");
        styleFontChooserChild(content, nullptr);
        gtk_window_resize(GTK_WINDOW(dialog), 400, 320);
    }
    g_list_free(windows);
}


void finishPanelPreferences(GWeakRef *status, GError *error) {
    if (auto *label = g_weak_ref_get(status)) {
        const auto message = error == nullptr ? std::string("已打开 GNOME 面板设置，修改立即生效")
            : std::string("无法打开 GNOME 面板设置：") + error->message;
        gtk_label_set_text(GTK_LABEL(label), message.c_str());
        g_object_unref(label);
    }
    g_clear_error(&error);
    g_weak_ref_clear(status);
    delete status;
}

void openPanelPreferences(GtkButton *, gpointer label) {
    gtk_label_set_text(GTK_LABEL(label), "正在打开 GNOME 面板设置…");
    auto *status = new GWeakRef;
    g_weak_ref_init(status, label);
    g_bus_get(G_BUS_TYPE_SESSION, nullptr, +[](GObject *, GAsyncResult *result, gpointer data) {
        GError *error = nullptr;
        auto *connection = g_bus_get_finish(result, &error);
        auto *pendingStatus = static_cast<GWeakRef *>(data);
        if (connection == nullptr) {
            finishPanelPreferences(pendingStatus, error);
            return;
        }
        g_dbus_connection_call(connection, "org.gnome.Shell.Extensions",
            "/org/gnome/Shell/Extensions", "org.gnome.Shell.Extensions", "OpenExtensionPrefs",
            g_variant_new("(ss@a{sv})", "modernime-panel@sunshine134679.github.io", "",
                          g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0)),
            nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr,
            +[](GObject *source, GAsyncResult *reply, gpointer state) {
                GError *callError = nullptr;
                auto *value = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), reply, &callError);
                if (value != nullptr) g_variant_unref(value);
                finishPanelPreferences(static_cast<GWeakRef *>(state), callError);
            }, pendingStatus);
        g_object_unref(connection);
    }, status);
}

double previewMix(double from, double to, double progress) {
    return from + (to - from) * progress;
}

ui::Rect previewMix(const ui::Rect &from, const ui::Rect &to, double progress) {
    return {previewMix(from.x, to.x, progress), previewMix(from.y, to.y, progress),
            previewMix(from.width, to.width, progress),
            previewMix(from.height, to.height, progress)};
}

ui::Color previewMix(const ui::Color &from, const ui::Color &to, double progress) {
    return {previewMix(from.red, to.red, progress), previewMix(from.green, to.green, progress),
            previewMix(from.blue, to.blue, progress), previewMix(from.alpha, to.alpha, progress)};
}

double previewProgress(gint64 elapsed, double duration) {
    const double remaining = 1.0 - std::clamp(elapsed / duration, 0.0, 1.0);
    return 1.0 - remaining * remaining * remaining;
}

// Only the settings preview clips transitioning tail text. All measurements,
// glyphs, panel and pill drawing still go through the native renderer.
class PreviewRenderSurface final : public ui::RenderSurface {
public:
    PreviewRenderSurface(cairo_t *context, const ui::Rect &clip)
        : context_(context), surface_(context), clip_(clip) {}

    void roundedRect(const ui::Rect &bounds, double radius, const ui::Color &color,
                     bool fill) override {
        surface_.roundedRect(bounds, radius, color, fill);
    }
    void gradientRoundedRect(const ui::Rect &bounds, double radius, const ui::Color &top,
                             const ui::Color &bottom, bool fill) override {
        surface_.gradientRoundedRect(bounds, radius, top, bottom, fill);
    }
    void illuminatedRoundedRect(const ui::Rect &bounds, double radius, const ui::Color &top,
                                const ui::Color &bottom, const ui::Color &light) override {
        surface_.illuminatedRoundedRect(bounds, radius, top, bottom, light);
    }
    void shadowRoundedRect(const ui::Rect &bounds, double radius, const ui::Color &color,
                           double blurRadius) override {
        surface_.shadowRoundedRect(bounds, radius, color, blurRadius);
    }
    ui::TextMetrics textMetrics(std::string_view value, const ui::TextStyle &style) const override {
        return surface_.textMetrics(value, style);
    }
    void text(std::string_view value, double x, double baseline, const ui::TextStyle &style,
              const ui::Color &color) override {
        cairo_save(context_);
        cairo_rectangle(context_, clip_.x, clip_.y, clip_.width, clip_.height);
        cairo_clip(context_);
        surface_.text(value, x, baseline, style, color);
        cairo_restore(context_);
    }

private:
    cairo_t *context_;
    ui::CairoRenderSurface surface_;
    ui::Rect clip_;
};

} // namespace

class CandidatesPage::Impl final {
public:
    Impl(SettingsWindowModel &modelRef, std::function<void()> changedCallback)
        : model(modelRef), changed(std::move(changedCallback)) {
        buildUi();
    }

    ~Impl() {
        if (pinDialog) gtk_widget_destroy(pinDialog);
        finishPreviewTransition();
        if (candidatePreview != nullptr) {
            g_object_set_data(G_OBJECT(candidatePreview), "modernime-preview-owner", nullptr);
            g_signal_handlers_disconnect_by_data(candidatePreview, this);
        }
    }

    GtkWidget *widget() const { return page; }

    void refresh() {
        if (pinRules) {
            const auto text = "管理规则 · " + std::to_string(model.settings().candidatePins.size()) + " 条";
            gtk_button_set_label(GTK_BUTTON(pinRules), text.c_str());
        }
        refreshing = true;
        const auto &settings = model.settings();
        for (std::size_t i = 0; i < core::kClientThemes.size(); ++i)
            if (core::kClientThemes[i].key == settings.clientTheme)
                setSettingsChoiceActive(clientTheme, static_cast<int>(i));
        setSettingsChoiceActive(candidatePageSize,
                                 settings.candidatePageSize - core::kMinimumCandidatePageSize);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(candidateFontSize),
                                  settings.candidateFontSize);
        auto *font = pango_font_description_new();
        pango_font_description_set_family(font, settings.candidateFontFamily.c_str());
        gtk_font_chooser_set_font_desc(GTK_FONT_CHOOSER(candidateFontFamily), font);
        pango_font_description_free(font);
        for (std::size_t i = 0; i < core::kCandidateThemes.size(); ++i) {
            if (core::kCandidateThemes[i].value == settings.candidateTheme)
                setSettingsChoiceActive(candidateTheme, static_cast<int>(i));
        }
        for (std::size_t i = 0; i < core::kCandidateSpacings.size(); ++i) {
            if (core::kCandidateSpacings[i].value == settings.candidateSpacing)
                setSettingsChoiceActive(candidateSpacing, static_cast<int>(i));
        }
        for (std::size_t i = 0; i < core::kCandidateShapes.size(); ++i)
            if (core::kCandidateShapes[i].value == settings.candidateShape)
                setSettingsChoiceActive(candidateShape, static_cast<int>(i));
        for (std::size_t i = 0; i < core::kCandidateStyles.size(); ++i)
            if (core::kCandidateStyles[i].value == settings.candidateStyle)
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(candidateStyles[i]), TRUE);
        gtk_switch_set_active(GTK_SWITCH(numberSelection),
                              settings.numberSelection);
        gtk_switch_set_active(GTK_SWITCH(arrowNavigation),
                              settings.arrowNavigation);
        gtk_switch_set_active(GTK_SWITCH(pageNavigation),
                              settings.pageNavigation);
        gtk_switch_set_active(GTK_SWITCH(englishDefinition),
                              settings.englishDefinitionEnabled);
        updatePreview();
        refreshing = false;
    }

    bool focusTarget(std::string_view target) {
        if (target == "candidate.pin") return focusWidgetOrFallback(pinRules, pinRulesRow);
        if (target == "client-theme") return focusWidgetOrFallback(clientTheme);
        if (target == "candidate-page-size") {
            return focusWidgetOrFallback(candidatePageSize, candidatePageSizeFallback);
        }
        if (target == "candidate-font-size") {
            return focusWidgetOrFallback(candidateFontSize, candidateFontSizeFallback);
        }
        if (target == "candidate-font-family")
            return focusWidgetOrFallback(candidateFontFamily, candidateFontSizeFallback);
        if (target == "candidate-theme")
            return focusWidgetOrFallback(candidateTheme, candidateThemeFallback);
        if (target == "candidate-spacing")
            return focusWidgetOrFallback(candidateSpacing, candidateSpacingFallback);
        if (target == "candidate-shape")
            return focusWidgetOrFallback(candidateShape, candidateShapeFallback);
        if (target == "candidate-style") {
            for (auto *button : candidateStyles)
                if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button)))
                    return focusWidgetOrFallback(button, previewCard);
        }
        if (target == "number-selection") {
            return focusWidgetOrFallback(numberSelection, numberSelectionFallback);
        }
        if (target == "arrow-navigation") {
            return focusWidgetOrFallback(arrowNavigation, arrowNavigationFallback);
        }
        if (target == "page-navigation") {
            return focusWidgetOrFallback(pageNavigation, pageNavigationFallback);
        }
        if (target == "english-definition") {
            return focusWidgetOrFallback(englishDefinition, englishDefinitionFallback);
        }
        return false;
    }

private:
    void buildUi() {
        detail::GtkWidgetGuard pageGuard(
            createPageShell("候选与外观", "调整候选显示、操作与指定输入的置顶偏好"));
        page = pageGuard.get();

        buildPreviewCard();
        buildClientAppearance();
        buildSettingsCard();
        auto *pins = createSectionCard("候选置顶", "为指定方案和输入选择首选词，其他输入继续使用现有排序。");
        pinRules = gtk_button_new_with_label("管理规则");
        setTarget(pinRules, "candidate.pin");
        pinRulesRow = createSettingRow("置顶规则", "从实际完整候选中选择；取消置顶不删除词条或学习记录。", pinRules);
        gtk_box_pack_start(GTK_BOX(pins), pinRulesRow, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(page), pins, FALSE, FALSE, 0);
        g_signal_connect(pinRules, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) {
            auto &o = *static_cast<Impl *>(p);
            if (o.pinDialog) { gtk_window_present(GTK_WINDOW(o.pinDialog)); return; }
            auto *top = gtk_widget_get_toplevel(o.page);
            o.pinDialog = createCandidatePinDialog(GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : nullptr, o.model.settings(), [&o](std::vector<core::CandidatePin> rules) {
                auto settings = o.model.settings(); settings.candidatePins = std::move(rules); o.model.setSettings(std::move(settings));
                o.refresh(); o.changed();
            });
            g_signal_connect(o.pinDialog, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) { static_cast<Impl *>(data)->pinDialog = nullptr; }), &o);
            gtk_widget_show_all(o.pinDialog);
        }), this);

        setSettingsFocusChain(
            page, {clientTheme, previewExample, candidateStyles[0], candidateStyles[1], candidateStyles[2],
                   candidateTheme, candidateShape, nativePanelSettings,
                   candidatePageSize, candidateFontFamily, candidateFontSize, candidateSpacing, numberSelection,
                   englishDefinition, arrowNavigation, pageNavigation, pinRules});
        refresh();
        pageGuard.release();
    }

    void buildPreviewCard() {
        previewCard = createSectionCard("", "");
        addStyleClass(previewCard, "modernime-preview-card");
        gtk_box_pack_start(GTK_BOX(page), previewCard, FALSE, FALSE, 0);

        auto *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        auto *headerLabel = gtk_label_new("候选预览 · 不记录学习");
        addStyleClass(headerLabel, "modernime-preview-header");
        gtk_widget_set_halign(headerLabel, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(header), headerLabel, TRUE, TRUE, 0);
        previewExample = createSettingsChoice();
        for (const auto *text : {"单字", "双字", "混合词组"}) {
            appendSettingsChoice(previewExample, text);
        }
        setSettingsChoiceActive(previewExample, 1);
        setTarget(previewExample, "candidate-preview-example");
        setAccessibleWidgetText(previewExample, "预览示例", "切换单字、双字或不同长度的候选，比较同一候选栏的容量");
        gtk_box_pack_end(GTK_BOX(header), previewExample, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(previewCard), header, FALSE, FALSE, 0);

        compositionLabel = gtk_label_new(nullptr);
        addStyleClass(compositionLabel, "modernime-composition-text");
        gtk_widget_set_halign(compositionLabel, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(previewCard), compositionLabel, FALSE, FALSE, 0);

        // Draw the real candidate bar. Its width depends on the capacity and
        // font settings, never on the length of the example words.
        candidatePreview = gtk_drawing_area_new();
        setTarget(candidatePreview, "candidate-preview");
        gtk_widget_set_hexpand(candidatePreview, TRUE);
        gtk_box_pack_start(GTK_BOX(previewCard), candidatePreview, FALSE, FALSE, 0);
        g_signal_connect(candidatePreview, "draw", G_CALLBACK(onPreviewDraw), this);
        g_signal_connect(candidatePreview, "size-allocate",
                         G_CALLBACK(+[](GtkWidget *, GtkAllocation *allocation, gpointer data) {
                             auto *self = static_cast<Impl *>(data);
                             if (self->previewAllocatedWidth != allocation->width) {
                                 self->previewAllocatedWidth = allocation->width;
                                 self->finishPreviewTransition();
                             }
                             self->updateScaleHint();
                         }), this);
        g_signal_connect(candidatePreview, "unmap", G_CALLBACK(+[](GtkWidget *, gpointer data) {
            static_cast<Impl *>(data)->finishPreviewTransition();
        }), this);
        g_signal_connect(candidatePreview, "notify::scale-factor",
                         G_CALLBACK(+[](GObject *, GParamSpec *, gpointer data) {
                             static_cast<Impl *>(data)->finishPreviewTransition();
                         }), this);
        g_signal_connect(candidatePreview, "destroy", G_CALLBACK(+[](GtkWidget *preview, gpointer data) {
            auto *self = static_cast<Impl *>(data);
            self->finishPreviewTransition();
            g_object_set_data(G_OBJECT(preview), "modernime-preview-owner", nullptr);
            self->candidatePreview = nullptr;
        }), this);
        // The widget owns this settings subscription, so no callback can outlive
        // the preview. Its data is cleared before the C++ owner is destroyed.
        g_object_set_data(G_OBJECT(candidatePreview), "modernime-preview-owner", this);
        g_signal_connect_object(gtk_widget_get_settings(candidatePreview), "notify::gtk-enable-animations",
            G_CALLBACK(+[](GObject *, GParamSpec *, GtkWidget *preview) {
                auto *self = static_cast<Impl *>(g_object_get_data(
                    G_OBJECT(preview), "modernime-preview-owner"));
                if (self != nullptr && !self->previewAnimationsEnabled()) self->finishPreviewTransition();
            }), candidatePreview, static_cast<GConnectFlags>(0));
        g_signal_connect(previewExample, "changed", G_CALLBACK(onExampleChanged), this);
        previewSummary = gtk_label_new(nullptr);
        addStyleClass(previewSummary, "modernime-description");
        gtk_label_set_xalign(GTK_LABEL(previewSummary), 0.0F);
        gtk_label_set_line_wrap(GTK_LABEL(previewSummary), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(previewSummary), 1);
        setTarget(previewSummary, "candidate-preview-summary");
        auto *previewMeta = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_box_pack_start(GTK_BOX(previewCard), previewMeta, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(previewMeta), previewSummary, TRUE, TRUE, 0);
        previewScaleHint = gtk_label_new("预览随窗口宽度适配");
        addStyleClass(previewScaleHint, "modernime-description");
        gtk_label_set_xalign(GTK_LABEL(previewScaleHint), 1.0F);
        gtk_label_set_line_wrap(GTK_LABEL(previewScaleHint), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(previewScaleHint), 1);
        setTarget(previewScaleHint, "candidate-preview-scale");
        gtk_box_pack_end(GTK_BOX(previewMeta), previewScaleHint, TRUE, TRUE, 0);
        appearanceActions = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(appearanceActions), 12);
        gtk_grid_set_row_spacing(GTK_GRID(appearanceActions), 8);
        gtk_widget_set_margin_top(appearanceActions, 4);
        gtk_box_pack_start(GTK_BOX(previewCard), appearanceActions, FALSE, FALSE, 0);
        if (g_strcmp0(g_getenv("XDG_SESSION_TYPE"), "wayland") == 0 ||
            g_getenv("WAYLAND_DISPLAY") != nullptr) {
            auto *nativeHint = gtk_label_new(
                "此预览用于自绘候选栏；Wayland 外观由桌面或应用面板管理。");
            addStyleClass(nativeHint, "modernime-description");
            gtk_label_set_xalign(GTK_LABEL(nativeHint), 0.0F);
            gtk_label_set_line_wrap(GTK_LABEL(nativeHint), TRUE);
            gtk_box_pack_start(GTK_BOX(previewCard), nativeHint, FALSE, FALSE, 0);
            const auto *desktop = g_getenv("XDG_CURRENT_DESKTOP");
            if (desktop != nullptr && std::string_view(desktop).find("GNOME") != std::string_view::npos) {
                nativePanelSettings = gtk_button_new_with_label("打开 GNOME 面板设置");
                setTarget(nativePanelSettings, "native-panel-settings");
                gtk_widget_set_halign(nativePanelSettings, GTK_ALIGN_START);
                g_signal_connect(nativePanelSettings, "clicked", G_CALLBACK(openPanelPreferences), nativeHint);
                gtk_box_pack_start(GTK_BOX(previewCard), nativePanelSettings, FALSE, FALSE, 0);
            }
        }
        definitionPreview = gtk_label_new("hello · int. 你好；喂");
        addStyleClass(definitionPreview, "modernime-description");
        gtk_widget_set_halign(definitionPreview, GTK_ALIGN_START);
        gtk_widget_set_margin_top(definitionPreview, 4);
        gtk_widget_set_no_show_all(definitionPreview, TRUE);
        gtk_box_pack_start(GTK_BOX(previewCard), definitionPreview, FALSE, FALSE, 0);
    }

    void buildSettingsCard() {
        auto *card = createSectionCard("候选显示", "");
        gtk_box_pack_start(GTK_BOX(page), card, FALSE, FALSE, 0);

        // Compact native controls keep all supported values keyboard-accessible.
        candidatePageSize = createSettingsChoice();
        for (int value = core::kMinimumCandidatePageSize;
             value <= core::kMaximumCandidatePageSize; ++value) {
            appendSettingsChoice(candidatePageSize,
                                          (std::to_string(value) + " 个").c_str());
        }
        setTarget(candidatePageSize, "candidate-page-size");
        candidatePageSizeFallback = createSettingRow(
            "候选栏长度", "以可容纳的单字候选数为基准（3–9 个），词组按长度自动分页", candidatePageSize);
        gtk_box_pack_start(GTK_BOX(card), candidatePageSizeFallback, FALSE, FALSE, 0);

        auto *fontControl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        candidateFontFamily = gtk_font_button_new();
        gtk_font_button_set_title(GTK_FONT_BUTTON(candidateFontFamily), "选择候选字体");
        gtk_font_button_set_show_size(GTK_FONT_BUTTON(candidateFontFamily), FALSE);
        gtk_font_button_set_show_style(GTK_FONT_BUTTON(candidateFontFamily), FALSE);
        gtk_font_chooser_set_level(GTK_FONT_CHOOSER(candidateFontFamily), GTK_FONT_CHOOSER_LEVEL_FAMILY);
        gtk_font_chooser_set_preview_text(GTK_FONT_CHOOSER(candidateFontFamily), "你好，世界。ModernIME");
        g_signal_connect_after(candidateFontFamily, "clicked", G_CALLBACK(styleFontChooser), nullptr);
        setTarget(candidateFontFamily, "candidate-font-family");
        setAccessibleWidgetText(candidateFontFamily, "候选字体", "选择系统中已安装的字体");
        gtk_box_pack_start(GTK_BOX(fontControl), candidateFontFamily, FALSE, FALSE, 0);
        candidateFontSize = gtk_spin_button_new_with_range(
            core::kMinimumCandidateFontSize, core::kMaximumCandidateFontSize, 1.0);
        gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(candidateFontSize), TRUE);
        gtk_entry_set_width_chars(GTK_ENTRY(candidateFontSize), 2);
        setTarget(candidateFontSize, "candidate-font-size");
        gtk_box_pack_start(GTK_BOX(fontControl), candidateFontSize, FALSE, FALSE, 0);
        auto *unit = gtk_label_new("pt");
        addStyleClass(unit, "modernime-description");
        gtk_box_pack_start(GTK_BOX(fontControl), unit, FALSE, FALSE, 0);
        candidateFontSizeFallback = createSettingRow(
            "候选字体", "选择已安装字体，字号可设置 14–28 pt", fontControl);
        gtk_box_pack_start(GTK_BOX(card), candidateFontSizeFallback, FALSE, FALSE, 0);

        candidateSpacing = createSettingsChoice();
        for (const auto &option : core::kCandidateSpacings)
            appendSettingsChoice(candidateSpacing, option.label);
        setTarget(candidateSpacing, "candidate-spacing");
        candidateSpacingFallback = createSettingRow("候选间距", "紧凑节省横向空间，宽松便于浏览", candidateSpacing);
        gtk_box_pack_start(GTK_BOX(card), candidateSpacingFallback, FALSE, FALSE, 0);
        candidateTheme = createSettingsChoice();
        for (const auto &option : core::kCandidateThemes) {
            auto sample = core::defaultSettings();
            sample.candidateTheme = option.value;
            const auto palette = ui::RenderStyle::fromSettings(sample);
            const auto rgba = [](const ui::Color &color) -> GdkRGBA {
                return {color.red, color.green, color.blue, color.alpha};
            };
            appendSettingsPaletteChoice(candidateTheme, option.label,
                {rgba(palette.panel), rgba(palette.text), rgba(palette.selected)});
        }
        setTarget(candidateTheme, "candidate-theme");
        setAccessibleWidgetText(candidateTheme, "候选栏配色", "选择候选栏和剪贴板菜单的配色，立即预览，不改变界面主题");
        candidateThemeFallback = previewCard;
        auto *paletteLabel = gtk_label_new("候选栏配色");
        gtk_label_set_mnemonic_widget(GTK_LABEL(paletteLabel), candidateTheme);
        gtk_grid_attach(GTK_GRID(appearanceActions), paletteLabel, 0, 1, 1, 1);
        gtk_grid_attach(GTK_GRID(appearanceActions), candidateTheme, 1, 1, 1, 1);
        candidateShape = createSettingsChoice();
        for (const auto &option : core::kCandidateShapes)
            appendSettingsChoice(candidateShape, option.label);
        setTarget(candidateShape, "candidate-shape");
        setAccessibleWidgetText(candidateShape, "候选轮廓", "选择圆润、方正或胶囊形状，立即预览");
        candidateShapeFallback = previewCard;
        gtk_grid_attach(GTK_GRID(appearanceActions), gtk_label_new("轮廓"), 2, 1, 1, 1);
        gtk_grid_attach(GTK_GRID(appearanceActions), candidateShape, 3, 1, 1, 1);

        auto *styleLabel = gtk_label_new("候选风格");
        gtk_label_set_xalign(GTK_LABEL(styleLabel), 0);
        gtk_grid_attach(GTK_GRID(appearanceActions), styleLabel, 0, 0, 1, 1);
        auto *selector = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        addStyleClass(selector, "modernime-style-selector");
        setTarget(selector, "candidate-style");
        gtk_widget_set_halign(selector, GTK_ALIGN_START);
        for (std::size_t i = 0; i < core::kCandidateStyles.size(); ++i) {
            const auto &option = core::kCandidateStyles[i];
            auto *button = gtk_radio_button_new_with_label_from_widget(
                i ? GTK_RADIO_BUTTON(candidateStyles[0]) : nullptr, std::string(option.label).c_str());
            candidateStyles[i] = button;
            gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(button), FALSE);
            addStyleClass(button, "modernime-style-option");
            setTarget(button, "candidate-style-" + std::string(option.key));
            setAccessibleWidgetText(button, "候选风格：" + std::string(option.label), "即时预览，保存设置后应用到实际候选栏");
            gtk_widget_set_tooltip_text(button, styleDescription(i));
            gtk_box_pack_start(GTK_BOX(selector), button, FALSE, FALSE, 0);
            g_signal_connect(button, "toggled", G_CALLBACK(+[](GtkToggleButton *control, gpointer data) {
                if (gtk_toggle_button_get_active(control)) onSpinChanged(GTK_WIDGET(control), data);
            }), this);
        }
        gtk_grid_attach(GTK_GRID(appearanceActions), selector, 1, 0, 3, 1);
        styleHint = gtk_label_new("");
        addStyleClass(styleHint, "modernime-description");
        gtk_label_set_xalign(GTK_LABEL(styleHint), 0);
        gtk_label_set_line_wrap(GTK_LABEL(styleHint), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(styleHint), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_max_width_chars(GTK_LABEL(styleHint), 1);
        gtk_grid_attach(GTK_GRID(appearanceActions), styleHint, 0, 2, 4, 1);

        // Row 3: 数字键选词
        numberSelection = gtk_switch_new();
        gtk_widget_set_tooltip_text(numberSelection, "空格确认首选，数字键选择对应候选");
        setTarget(numberSelection, "number-selection");
        numberSelectionFallback = createSettingRow(
            "数字键选词", "空格确认首选，数字键选择对应候选", numberSelection);
        gtk_box_pack_start(GTK_BOX(card), numberSelectionFallback, FALSE,
                           FALSE, 0);

        // Row 4: 显示英文释义
        englishDefinition = gtk_switch_new();
        gtk_widget_set_tooltip_text(
            englishDefinition, "解释仅作为附注显示，不占编号、不改变排序");
        setTarget(englishDefinition, "english-definition");
        englishDefinitionFallback = createSettingRow(
            "显示英文释义", "解释仅作为附注显示，不占编号、不改变排序",
            englishDefinition);
        gtk_box_pack_start(GTK_BOX(card), englishDefinitionFallback, FALSE,
                           FALSE, 0);

        auto *navigation = createSectionCard("候选导航", "控制方向键与翻页键的行为");
        gtk_box_pack_start(GTK_BOX(page), navigation, FALSE, FALSE, 0);
        arrowNavigation = gtk_switch_new();
        pageNavigation = gtk_switch_new();
        setTarget(arrowNavigation, "arrow-navigation");
        setTarget(pageNavigation, "page-navigation");
        arrowNavigationFallback = createSettingRow(
            "方向键编辑与选词", "左右移动拼音光标，上下切换候选项", arrowNavigation);
        pageNavigationFallback = createSettingRow(
            "候选翻页", "使用 PageUp / PageDown 和 + / = 翻页", pageNavigation);
        gtk_box_pack_start(GTK_BOX(navigation), arrowNavigationFallback, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(navigation), pageNavigationFallback, FALSE, FALSE, 0);

        g_signal_connect(candidatePageSize, "changed", G_CALLBACK(onSpinChanged), this);
        g_signal_connect(candidateFontSize, "value-changed", G_CALLBACK(onSpinChanged), this);
        g_signal_connect(candidateFontFamily, "font-set", G_CALLBACK(onSpinChanged), this);
        g_signal_connect(candidateTheme, "changed", G_CALLBACK(onSpinChanged), this);
        g_signal_connect(candidateShape, "changed", G_CALLBACK(onSpinChanged), this);
        g_signal_connect(candidateSpacing, "changed", G_CALLBACK(onSpinChanged), this);

        for (auto *control : {numberSelection, arrowNavigation, pageNavigation,
                              englishDefinition}) {
            g_signal_connect(control, "notify::active",
                             G_CALLBACK(onSwitchChanged), this);
        }
    }

    void buildClientAppearance() {
        clientTheme = createSettingsChoice();
        for (const auto &option : core::kClientThemes)
            appendSettingsPaletteChoice(clientTheme, option.label, settingsThemeColors(option.key));
        setTarget(clientTheme, "client-theme");
        setAccessibleWidgetText(clientTheme, "界面主题", "即时预览客户端配色，保存后记住选择，不改变候选栏配色");
        auto *themeControl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        auto *themeLabel = gtk_label_new("界面主题");
        addStyleClass(themeLabel, "modernime-description");
        gtk_label_set_mnemonic_widget(GTK_LABEL(themeLabel), clientTheme);
        gtk_widget_set_valign(themeLabel, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(clientTheme, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(themeControl), themeLabel, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(themeControl), clientTheme, FALSE, FALSE, 0);
        setPageHeaderAction(page, themeControl);
        g_signal_connect(clientTheme, "changed", G_CALLBACK(+[](GtkWidget *, gpointer data) {
            auto *self = static_cast<Impl *>(data);
            if (self->refreshing) return;
            auto settings = self->model.settings();
            const int selected = settingsChoiceActive(self->clientTheme);
            if (selected < 0 || static_cast<std::size_t>(selected) >= core::kClientThemes.size()) return;
            settings.clientTheme = core::kClientThemes[selected].key;
            self->model.setSettings(std::move(settings));
            if (self->changed) self->changed();
        }), this);
    }

    void updatePreview() {
        if (candidatePreview == nullptr) return;
        static constexpr std::array single{
            "你", "尼", "呢", "泥", "拟", "逆", "妮", "倪", "腻"};
        static constexpr std::array doubleWords{
            "你好", "你号", "拟好", "拟合", "霓虹", "倪皓", "逆号", "泥壕", "妮好"};
        static constexpr std::array mixed{
            "你好", "你号", "拟好", "你好啊", "你好呀", "你好吗", "你还好", "你好吧", "你好哇"};
        const int example = settingsChoiceActive(previewExample);
        const auto &samples = example == 0 ? single : example == 1 ? doubleWords : mixed;
        const auto settings = controlSettings();
        for (std::size_t i = 0; i < core::kCandidateStyles.size(); ++i)
            if (core::kCandidateStyles[i].value == settings.candidateStyle) {
                std::string hint = styleDescription(i);
                if (settings.candidateTheme == core::CandidateTheme::HighContrast)
                    hint += " · 高对比配色保留清晰的实色边界";
                gtk_label_set_text(GTK_LABEL(styleHint), hint.c_str());
            }
        gtk_widget_set_visible(definitionPreview, settings.englishDefinitionEnabled);
        const bool fontChanged = previewSettings.candidateFontSize != settings.candidateFontSize ||
            previewSettings.candidateFontFamily != settings.candidateFontFamily;
        const bool shapeChanged = previewSettings.candidateShape != settings.candidateShape;
        const bool appearanceChanged = !previewInitialized || fontChanged || shapeChanged ||
            previewSettings.candidateStyle != settings.candidateStyle ||
            previewSettings.candidateTheme != settings.candidateTheme ||
            previewSettings.candidateSpacing != settings.candidateSpacing ||
            previewSettings.candidatePageSize != settings.candidatePageSize || previewSample != example;
        if (!appearanceChanged) return;

        const bool animate = previewInitialized && !fontChanged && previewSample == example &&
            gtk_widget_get_mapped(candidatePreview) && previewAnimationsEnabled();
        if (previewTick != 0) advancePreview(g_get_monotonic_time());
        previewFromLayout = displayedLayout;
        previewFromStyle = displayedStyle;
        previewFromWidth = displayedCanvasWidth;
        previewSettings = settings;
        previewSample = example;
        metrics = ui::CandidateBarMetrics::fromSettings(settings);
        renderStyle = ui::RenderStyle::fromSettings(settings);
        core::CandidatePage candidatePage;
        for (const auto *sample : samples) {
            core::CandidateItem item;
            item.text = sample;
            candidatePage.items.push_back(std::move(item));
        }
        auto *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        {
            ui::CairoRenderSurface measure(surface);
            const auto textWidth = [&](std::string_view value) {
                const auto separator = value.find('.');
                if (separator == std::string_view::npos) {
                    return measure.textWidth(value, renderStyle.candidateText);
                }
                return measure.textWidth(value.substr(0, separator + 1), renderStyle.candidateNumberText) +
                       measure.textWidth(value.substr(separator + 1), renderStyle.candidateText);
            };
            const auto boundaries = ui::CandidatePagination::partition(candidatePage.items, metrics, textWidth);
            if (!boundaries.empty()) candidatePage.items.resize(boundaries.front().end);
            previewLayout = ui::CandidateBarLayout::measure(candidatePage, metrics, textWidth);
        }
        cairo_surface_destroy(surface);
        previewInitialized = true;
        if (animate) {
            previewStarted = g_get_monotonic_time();
            previewDuration = previewFromStyle.panelRadius != renderStyle.panelRadius ||
                previewFromStyle.selectedRadius != renderStyle.selectedRadius ? 220000 : 180000;
            if (previewTick == 0)
                previewTick = gtk_widget_add_tick_callback(candidatePreview, onPreviewTick, this, nullptr);
        } else {
            finishPreviewTransition();
        }
        gtk_widget_set_size_request(candidatePreview, 1, static_cast<int>(std::ceil(metrics.canvasHeight)));
        gtk_label_set_markup(GTK_LABEL(compositionLabel), example == 0
            ? "ni<b>|</b>" : "nihao<b>|</b>");
        std::string accessibleCandidates;
        for (const auto &candidate : previewLayout.candidates) {
            if (!accessibleCandidates.empty()) accessibleCandidates += " · ";
            accessibleCandidates += candidate.displayText;
        }
        setAccessibleWidgetText(candidatePreview, "候选栏预览", accessibleCandidates);
        const auto summary = "当前页 " + std::to_string(previewLayout.candidates.size()) +
            " 个 · 按词长自动分页";
        gtk_label_set_text(GTK_LABEL(previewSummary), summary.c_str());
        updateScaleHint();
        gtk_widget_queue_draw(candidatePreview);
    }

    bool previewAnimationsEnabled() const {
        gboolean enabled = FALSE;
        if (candidatePreview != nullptr)
            g_object_get(gtk_widget_get_settings(candidatePreview), "gtk-enable-animations", &enabled, nullptr);
        return enabled;
    }

    void finishPreviewTransition() {
        if (previewTick != 0 && candidatePreview != nullptr)
            gtk_widget_remove_tick_callback(candidatePreview, previewTick);
        previewTick = 0;
        displayedLayout = previewLayout;
        displayedStyle = renderStyle;
        displayedCanvasWidth = metrics.canvasWidth;
        previewFrame.reset();
        if (candidatePreview != nullptr) gtk_widget_queue_draw(candidatePreview);
    }

    void advancePreview(gint64 now) {
        const gint64 elapsed = std::max<gint64>(0, now - previewStarted);
        const double progress = previewProgress(elapsed, 180000.0);
        const double shapeProgress = previewProgress(elapsed, 220000.0);
        displayedLayout = previewLayout;
        displayedLayout.panel = previewMix(previewFromLayout.panel, previewLayout.panel, progress);
        displayedLayout.selectedPill = previewMix(previewFromLayout.selectedPill,
                                                  previewLayout.selectedPill, progress);
        displayedCanvasWidth = previewMix(previewFromWidth, metrics.canvasWidth, progress);
        if (progress < 1.0) {
            // The real pagination result stays in previewLayout. Transitional
            // tail geometry is paint-only and never becomes a candidate page.
            const auto count = std::max(previewFromLayout.candidates.size(), previewLayout.candidates.size());
            displayedLayout.candidates.resize(count);
            for (std::size_t index = 0; index < count; ++index) {
                const bool hasFrom = index < previewFromLayout.candidates.size();
                const bool hasTo = index < previewLayout.candidates.size();
                const auto &candidate = hasTo ? previewLayout.candidates[index]
                                             : previewFromLayout.candidates[index];
                auto from = hasFrom ? previewFromLayout.candidates[index].bounds : candidate.bounds;
                auto to = hasTo ? previewLayout.candidates[index].bounds : candidate.bounds;
                if (!hasFrom)
                    from.x = std::max(from.x, previewFromLayout.panel.x + previewFromLayout.panel.width);
                if (!hasTo)
                    to.x = std::max(to.x, previewLayout.panel.x + previewLayout.panel.width);
                displayedLayout.candidates[index] = candidate;
                displayedLayout.candidates[index].bounds = previewMix(from, to, progress);
            }
        }
        displayedStyle = renderStyle;
        for (const auto color : {&ui::RenderStyle::shadow, &ui::RenderStyle::panel,
                                 &ui::RenderStyle::border, &ui::RenderStyle::selected,
                                 &ui::RenderStyle::text, &ui::RenderStyle::selectedText,
                                 &ui::RenderStyle::preedit, &ui::RenderStyle::annotation,
                                 &ui::RenderStyle::selectedAnnotation,
                                 &ui::RenderStyle::panelTop, &ui::RenderStyle::panelBottom,
                                 &ui::RenderStyle::borderTop, &ui::RenderStyle::borderBottom,
                                 &ui::RenderStyle::selectedTop, &ui::RenderStyle::selectedBottom,
                                 &ui::RenderStyle::selectedEdgeTop, &ui::RenderStyle::selectedEdgeBottom,
                                 &ui::RenderStyle::selectedShadow,
                                 &ui::RenderStyle::panelLight, &ui::RenderStyle::selectedLight}) {
            displayedStyle.*color = previewMix(previewFromStyle.*color, renderStyle.*color, progress);
        }
        displayedStyle.panelRadius = previewMix(previewFromStyle.panelRadius,
                                                 renderStyle.panelRadius, shapeProgress);
        displayedStyle.selectedRadius = previewMix(previewFromStyle.selectedRadius,
                                                    renderStyle.selectedRadius, shapeProgress);
    }

    static gboolean onPreviewTick(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
        auto *self = static_cast<Impl *>(data);
        const auto now = gdk_frame_clock_get_frame_time(clock);
        if (!self->previewAnimationsEnabled() || now - self->previewStarted >= self->previewDuration) {
            // GTK removes the current callback after G_SOURCE_REMOVE; clearing
            // its ID first avoids removing it twice from the finish helper.
            self->previewTick = 0;
            self->finishPreviewTransition();
            return G_SOURCE_REMOVE;
        }
        self->advancePreview(now);
        gtk_widget_queue_draw(widget);
        return G_SOURCE_CONTINUE;
    }

    double previewScale() const {
        return std::min(1.0, gtk_widget_get_allocated_width(candidatePreview) / metrics.canvasWidth);
    }

    void updateScaleHint() {
        if (gtk_widget_get_allocated_width(candidatePreview) <= 1) {
            return;
        }
        const double scale = previewScale();
        const auto text = scale >= 1.0 ? std::string("实际大小 · 100%")
            : "缩放 " + std::to_string(static_cast<int>(std::lround(scale * 100.0))) +
                "% · 字号不变";
        gtk_label_set_text(GTK_LABEL(previewScaleHint), text.c_str());
    }

    static gboolean onPreviewDraw(GtkWidget *, cairo_t *context, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        const double scale = std::min(1.0, gtk_widget_get_allocated_width(impl->candidatePreview) /
                                             std::max(1.0, impl->displayedCanvasWidth));
        cairo_save(context);
        if (impl->previewTick != 0) {
            cairo_scale(context, scale, scale);
            auto clip = impl->displayedLayout.panel;
            clip.x += impl->metrics.horizontalPadding;
            clip.width = std::max(0.0, clip.width - 2.0 * impl->metrics.horizontalPadding);
            PreviewRenderSurface surface(context, clip);
            ui::CandidateBarRenderer::render(surface, impl->displayedLayout, impl->displayedStyle);
        } else {
            // Popover movement exposes this preview on every frame. Reuse its
            // settled pixels instead of laying out text and painting shadows
            // again while an unrelated menu is animating above it.
            const int deviceScale = gtk_widget_get_scale_factor(impl->candidatePreview);
            const int width = gtk_widget_get_allocated_width(impl->candidatePreview) * deviceScale;
            const int height = gtk_widget_get_allocated_height(impl->candidatePreview) * deviceScale;
            if (!impl->previewFrame || cairo_image_surface_get_width(impl->previewFrame.get()) != width ||
                cairo_image_surface_get_height(impl->previewFrame.get()) != height) {
                impl->previewFrame.reset(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height));
                cairo_surface_set_device_scale(impl->previewFrame.get(), deviceScale, deviceScale);
                auto *frame = cairo_create(impl->previewFrame.get());
                cairo_scale(frame, scale, scale);
                {
                    ui::CairoRenderSurface surface(frame);
                    ui::CandidateBarRenderer::render(surface, impl->previewLayout, impl->renderStyle);
                }
                cairo_destroy(frame);
            }
            cairo_set_source_surface(context, impl->previewFrame.get(), 0, 0);
            cairo_paint(context);
        }
        cairo_restore(context);
        return FALSE;
    }

    static void onExampleChanged(GtkWidget *, gpointer data) {
        // This is a local example; it must not mark user settings as modified.
        static_cast<Impl *>(data)->updatePreview();
    }

    static void onSpinChanged(GtkWidget *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (impl == nullptr || impl->refreshing) return;
        impl->updatePreview();
        impl->applyToModel();
    }

    static void onSwitchChanged(GObject *, GParamSpec *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (impl == nullptr || impl->refreshing) {
            return;
        }
        impl->updatePreview();
        impl->applyToModel();
    }

    core::ModernIMESettings controlSettings() const {
        auto settings = model.settings();
        settings.candidatePageSize = settingsChoiceActive(candidatePageSize) + core::kMinimumCandidatePageSize;
        settings.candidateFontSize = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(candidateFontSize));
        if (auto *font = gtk_font_chooser_get_font_desc(GTK_FONT_CHOOSER(candidateFontFamily))) {
            if (const auto *family = pango_font_description_get_family(font)) settings.candidateFontFamily = family;
            pango_font_description_free(font);
        }
        const int theme = settingsChoiceActive(candidateTheme);
        if (theme >= 0 && static_cast<std::size_t>(theme) < core::kCandidateThemes.size())
            settings.candidateTheme = core::kCandidateThemes[theme].value;
        const int shape = settingsChoiceActive(candidateShape);
        if (shape >= 0 && static_cast<std::size_t>(shape) < core::kCandidateShapes.size())
            settings.candidateShape = core::kCandidateShapes[shape].value;
        for (std::size_t i = 0; i < core::kCandidateStyles.size(); ++i)
            if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(candidateStyles[i])))
                settings.candidateStyle = core::kCandidateStyles[i].value;
        const int spacing = settingsChoiceActive(candidateSpacing);
        if (spacing >= 0 && static_cast<std::size_t>(spacing) < core::kCandidateSpacings.size())
            settings.candidateSpacing = core::kCandidateSpacings[spacing].value;
        settings.numberSelection = gtk_switch_get_active(GTK_SWITCH(numberSelection));
        settings.arrowNavigation = gtk_switch_get_active(GTK_SWITCH(arrowNavigation));
        settings.pageNavigation = gtk_switch_get_active(GTK_SWITCH(pageNavigation));
        settings.englishDefinitionEnabled = gtk_switch_get_active(GTK_SWITCH(englishDefinition));
        return settings;
    }

    void applyToModel() {
        model.setSettings(controlSettings());
        if (changed) {
            changed();
        }
    }

    static const char *styleDescription(std::size_t index) {
        static constexpr std::array descriptions{
            "细腻哑光、轻薄边缘与淡彩选中", "清晰边界、实色选中与柔和阴影", "柔光磨砂、微光内缘与悬浮选中"};
        return descriptions[index];
    }

    SettingsWindowModel &model;
    std::function<void()> changed;
    bool refreshing = false;

    GtkWidget *page = nullptr;
    GtkWidget *clientTheme = nullptr;
    GtkWidget *previewCard = nullptr;
    GtkWidget *appearanceActions = nullptr;
    GtkWidget *candidatePreview = nullptr;
    GtkWidget *previewExample = nullptr;
    GtkWidget *compositionLabel = nullptr;
    GtkWidget *previewSummary = nullptr;
    GtkWidget *previewScaleHint = nullptr;
    GtkWidget *nativePanelSettings = nullptr;
    ui::CandidateBarMetrics metrics;
    ui::CandidateBarLayout previewLayout;
    ui::RenderStyle renderStyle;
    ui::CandidateBarLayout displayedLayout;
    ui::RenderStyle displayedStyle;
    ui::CandidateBarLayout previewFromLayout;
    ui::RenderStyle previewFromStyle;
    core::ModernIMESettings previewSettings;
    double displayedCanvasWidth = 1.0;
    double previewFromWidth = 1.0;
    gint64 previewStarted = 0;
    gint64 previewDuration = 0;
    guint previewTick = 0;
    std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)> previewFrame{nullptr, cairo_surface_destroy};
    int previewSample = -1;
    int previewAllocatedWidth = 0;
    bool previewInitialized = false;
    GtkWidget *definitionPreview = nullptr;

    GtkWidget *candidatePageSize = nullptr;
    GtkWidget *candidatePageSizeFallback = nullptr;

    GtkWidget *candidateFontSize = nullptr;
    GtkWidget *candidateFontSizeFallback = nullptr;

    GtkWidget *candidateFontFamily = nullptr;
    GtkWidget *candidateTheme = nullptr;
    GtkWidget *candidateThemeFallback = nullptr;
    GtkWidget *candidateShape = nullptr;
    GtkWidget *candidateShapeFallback = nullptr;
    std::array<GtkWidget *, 3> candidateStyles{};
    GtkWidget *styleHint = nullptr;
    GtkWidget *candidateSpacing = nullptr;
    GtkWidget *candidateSpacingFallback = nullptr;
    GtkWidget *numberSelection = nullptr;
    GtkWidget *numberSelectionFallback = nullptr;
    GtkWidget *arrowNavigation = nullptr;
    GtkWidget *arrowNavigationFallback = nullptr;
    GtkWidget *pageNavigation = nullptr;
    GtkWidget *pageNavigationFallback = nullptr;
    GtkWidget *englishDefinition = nullptr;
    GtkWidget *englishDefinitionFallback = nullptr;
    GtkWidget *pinRules = nullptr, *pinRulesRow = nullptr, *pinDialog = nullptr;
};

CandidatesPage::CandidatesPage(SettingsWindowModel &model,
                               std::function<void()> changed)
    : impl_(std::make_unique<Impl>(model, std::move(changed))) {}

CandidatesPage::~CandidatesPage() = default;

GtkWidget *CandidatesPage::widget() const {
    return impl_->widget();
}

void CandidatesPage::refresh() {
    impl_->refresh();
}

bool CandidatesPage::focusTarget(std::string_view target) {
    return impl_->focusTarget(target);
}

} // namespace modernime::settings
