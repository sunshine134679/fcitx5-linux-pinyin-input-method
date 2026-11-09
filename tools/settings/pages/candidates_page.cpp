#include "modernime/settings/pages/candidates_page.h"

#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/settings_widgets.h"
#include "modernime/ui/cairo_render_surface.h"
#include "modernime/ui/candidate_pagination.h"

#include <gtk/gtk.h>

#include <array>
#include <algorithm>
#include <cmath>
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

} // namespace

class CandidatesPage::Impl final {
public:
    Impl(SettingsWindowModel &modelRef, std::function<void()> changedCallback)
        : model(modelRef), changed(std::move(changedCallback)) {
        buildUi();
    }

    GtkWidget *widget() const { return page; }

    void refresh() {
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
            createPageShell("候选与外观", "调整候选栏的显示方式，不改变候选排序规则"));
        page = pageGuard.get();

        buildPreviewCard();
        buildClientAppearance();
        buildSettingsCard();

        setSettingsFocusChain(
            page, {clientTheme, previewExample, candidateTheme, candidateShape, nativePanelSettings,
                   candidatePageSize, candidateFontFamily, candidateFontSize, candidateSpacing, numberSelection,
                   englishDefinition, arrowNavigation, pageNavigation});
        refresh();
        pageGuard.release();
    }

    void buildPreviewCard() {
        previewCard = createSectionCard("", "");
        addStyleClass(previewCard, "modernime-preview-card");
        gtk_box_pack_start(GTK_BOX(page), previewCard, FALSE, FALSE, 0);

        auto *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        auto *headerLabel = gtk_label_new("外观预览 · 不记录学习");
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
                         G_CALLBACK(+[](GtkWidget *, GtkAllocation *, gpointer data) {
                             static_cast<Impl *>(data)->updateScaleHint();
                         }), this);
        g_signal_connect(previewExample, "changed", G_CALLBACK(onExampleChanged), this);
        previewSummary = gtk_label_new(nullptr);
        addStyleClass(previewSummary, "modernime-description");
        gtk_label_set_xalign(GTK_LABEL(previewSummary), 0.0F);
        gtk_label_set_line_wrap(GTK_LABEL(previewSummary), TRUE);
        setTarget(previewSummary, "candidate-preview-summary");
        gtk_box_pack_start(GTK_BOX(previewCard), previewSummary, FALSE, FALSE, 0);
        previewScaleHint = gtk_label_new("预览随窗口宽度适配");
        addStyleClass(previewScaleHint, "modernime-description");
        gtk_label_set_xalign(GTK_LABEL(previewScaleHint), 0.0F);
        gtk_label_set_line_wrap(GTK_LABEL(previewScaleHint), TRUE);
        setTarget(previewScaleHint, "candidate-preview-scale");
        gtk_box_pack_start(GTK_BOX(previewCard), previewScaleHint, FALSE, FALSE, 0);
        appearanceActions = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(appearanceActions), 12);
        gtk_widget_set_margin_top(appearanceActions, 6);
        gtk_box_pack_start(GTK_BOX(previewCard), appearanceActions, FALSE, FALSE, 0);
        if (g_strcmp0(g_getenv("XDG_SESSION_TYPE"), "wayland") == 0 ||
            g_getenv("WAYLAND_DISPLAY") != nullptr) {
            auto *nativeHint = gtk_label_new(
                "Wayland 候选由桌面或应用面板显示；此预览用于 ModernIME 自绘候选栏。"
                "GNOME 面板的字体、配色、轮廓与间距可在面板设置中调整。");
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
        gtk_widget_set_margin_top(definitionPreview, 8);
        gtk_widget_set_no_show_all(definitionPreview, TRUE);
        gtk_box_pack_start(GTK_BOX(previewCard), definitionPreview, FALSE, FALSE, 0);
    }

    void buildSettingsCard() {
        auto *card = createSectionCard("", "");
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
        setAccessibleWidgetText(candidateTheme, "候选配色", "选择候选栏和剪贴板菜单的配色，立即预览");
        candidateThemeFallback = previewCard;
        gtk_grid_attach(GTK_GRID(appearanceActions), gtk_label_new("配色"), 0, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(appearanceActions), candidateTheme, 1, 0, 1, 1);
        candidateShape = createSettingsChoice();
        for (const auto &option : core::kCandidateShapes)
            appendSettingsChoice(candidateShape, option.label);
        setTarget(candidateShape, "candidate-shape");
        setAccessibleWidgetText(candidateShape, "候选轮廓", "选择圆润、方正或胶囊形状，立即预览");
        candidateShapeFallback = previewCard;
        gtk_grid_attach(GTK_GRID(appearanceActions), gtk_label_new("轮廓"), 2, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(appearanceActions), candidateShape, 3, 0, 1, 1);

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
        setAccessibleWidgetText(clientTheme, "客户端主题", "即时预览客户端配色，保存后记住选择");
        setPageHeaderAction(page, clientTheme);
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
        const int capacity = settings.candidatePageSize;
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
        gtk_widget_set_size_request(candidatePreview, 1, static_cast<int>(std::ceil(metrics.canvasHeight)));
        gtk_label_set_markup(GTK_LABEL(compositionLabel), example == 0
            ? "ni<b>|</b>" : "nihao<b>|</b>");
        std::string accessibleCandidates;
        for (const auto &candidate : previewLayout.candidates) {
            if (!accessibleCandidates.empty()) accessibleCandidates += " · ";
            accessibleCandidates += candidate.displayText;
        }
        setAccessibleWidgetText(candidatePreview, "候选栏预览", accessibleCandidates);
        const auto summary = "长度可容纳 " + std::to_string(capacity) +
            " 个单字候选 · 当前页 " + std::to_string(previewLayout.candidates.size()) + " 个 · 按词长自动分页";
        gtk_label_set_text(GTK_LABEL(previewSummary), summary.c_str());
        updateScaleHint();
        gtk_widget_queue_draw(candidatePreview);
        gtk_widget_set_visible(definitionPreview, gtk_switch_get_active(GTK_SWITCH(englishDefinition)));
    }

    double previewScale() const {
        return std::min(1.0, gtk_widget_get_allocated_width(candidatePreview) / metrics.canvasWidth);
    }

    void updateScaleHint() {
        if (gtk_widget_get_allocated_width(candidatePreview) <= 1) {
            return;
        }
        const double scale = previewScale();
        const auto text = scale >= 1.0 ? std::string("按实际大小预览（100%）")
            : "预览约为 " + std::to_string(static_cast<int>(std::lround(scale * 100.0))) +
                "% · 设置字号不变";
        gtk_label_set_text(GTK_LABEL(previewScaleHint), text.c_str());
    }

    static gboolean onPreviewDraw(GtkWidget *, cairo_t *context, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        const double scale = impl->previewScale();
        cairo_save(context);
        cairo_scale(context, scale, scale);
        ui::CairoRenderSurface surface(context);
        ui::CandidateBarRenderer::render(surface, impl->previewLayout, impl->renderStyle);
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
