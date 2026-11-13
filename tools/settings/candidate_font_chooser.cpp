#include "modernime/settings/candidate_font_chooser.h"

#include "modernime/settings/detail/diagnostics_lifetime.h"
#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/settings_widgets.h"

#include <pango/pangocairo.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>

namespace modernime::settings {
namespace {

using ChineseFonts = std::unordered_set<std::string>;
constexpr const char *kFontDialog = "modernime-candidate-font-dialog";

void addStyleClass(GtkWidget *widget, std::string_view name) {
    gtk_style_context_add_class(gtk_widget_get_style_context(widget), std::string(name).c_str());
}

std::string fontDescription(std::string_view name) {
    for (const auto &[prefix, label] : {
            std::pair{"Noto Sans CJK ", "Noto 中文黑体"},
            std::pair{"Noto Serif CJK ", "Noto 中文宋体"},
            std::pair{"Noto Sans Mono CJK ", "Noto 中文等宽字体"}}) {
        if (!name.starts_with(prefix)) continue;
        const auto region = name.substr(std::string_view(prefix).size());
        for (const auto &[code, text] : {
                std::pair{"SC", "简体字形"}, {"TC", "繁体字形"},
                {"HK", "香港字形"}, {"JP", "日文字形"}, {"KR", "韩文字形"}}) {
            if (region == code) return std::string(label) + " · " + text;
        }
    }
    if (name.starts_with("MiSans")) return "MiSans · 中文无衬线字体";
    if (name.starts_with("AR PL UKai")) return "文鼎 PL 楷体";
    if (name.starts_with("AR PL UMing")) return "文鼎 PL 明体（宋体）";
    return "支持常用中文";
}

struct FontDialog {
    using Lifetime = detail::DiagnosticsLifetime<FontDialog>;
    GtkWidget *dialog;
    GtkWidget *onlyChinese;
    GtkWidget *status;
    GtkWidget *description;
    ChineseFonts chinese;
    bool ready = false;
    detail::GObjectHandle<GCancellable> cancel =
        detail::GObjectHandle<GCancellable>::adopt(g_cancellable_new());
    std::shared_ptr<Lifetime> lifetime = std::make_shared<Lifetime>(this);

    explicit FontDialog(GtkWidget *owner) : dialog(owner),
        onlyChinese(gtk_check_button_new_with_label("仅显示中文字体")),
        status(gtk_label_new("正在识别…")), description(gtk_label_new("")) {}

    ~FontDialog() { lifetime->deactivate(); g_cancellable_cancel(cancel.get()); }

    void describeSelection() {
        if (!ready) return;
        auto *selected = gtk_font_chooser_get_font_family(GTK_FONT_CHOOSER(dialog));
        const auto *name = selected ? pango_font_family_get_name(selected) : nullptr;
        const auto text = !name ? std::string("选择字体，查看中文预览") : chinese.contains(name)
            ? fontDescription(name) : "不含常用中文字形，中文将使用系统回退字体";
        gtk_label_set_text(GTK_LABEL(description), text.c_str());
    }

    void refilter() {
        gtk_font_chooser_set_filter_func(GTK_FONT_CHOOSER(dialog),
            +[](const PangoFontFamily *family, const PangoFontFace *, gpointer data) -> gboolean {
                auto &self = *static_cast<FontDialog *>(data);
                return !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(self.onlyChinese)) ||
                    (self.ready && self.chinese.contains(pango_font_family_get_name(const_cast<PangoFontFamily *>(family))));
            }, this, nullptr);
        describeSelection();
    }
};

void prepareFontDialog(GtkWidget *dialog) {
    if (g_object_get_data(G_OBJECT(dialog), kFontDialog)) return;
    auto *state = new FontDialog(dialog);
    g_object_set_data_full(G_OBJECT(dialog), kFontDialog, state,
        +[](gpointer data) { delete static_cast<FontDialog *>(data); });
    auto *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    auto *filters = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->onlyChinese), TRUE);
    gtk_widget_set_sensitive(state->onlyChinese, FALSE);
    gtk_widget_set_tooltip_text(state->onlyChinese, "取消勾选可查看全部系统字体");
    g_object_set_data_full(G_OBJECT(state->onlyChinese), "modernime-settings-target", g_strdup("candidate-font-chinese-only"), g_free);
    gtk_box_pack_start(GTK_BOX(filters), state->onlyChinese, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(filters), state->status, FALSE, FALSE, 0);
    addStyleClass(state->status, "modernime-description");
    gtk_box_pack_start(GTK_BOX(content), filters, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(content), filters, 0);
    gtk_label_set_xalign(GTK_LABEL(state->description), 0);
    gtk_label_set_line_wrap(GTK_LABEL(state->description), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(state->description), 38);
    addStyleClass(state->description, "modernime-description");
    gtk_widget_set_margin_top(state->description, 8);
    gtk_box_pack_end(GTK_BOX(content), state->description, FALSE, FALSE, 0);
    gtk_widget_show_all(filters);
    gtk_widget_show(state->description);
    g_signal_connect(state->onlyChinese, "toggled", G_CALLBACK(+[](GtkToggleButton *, gpointer data) {
        static_cast<FontDialog *>(data)->refilter();
    }), state);
    g_signal_connect(dialog, "notify::font", G_CALLBACK(+[](GObject *, GParamSpec *, gpointer data) {
        static_cast<FontDialog *>(data)->describeSelection();
    }), state);
    g_signal_connect(dialog, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) {
        auto &self = *static_cast<FontDialog *>(data);
        self.lifetime->deactivate();
        g_cancellable_cancel(self.cancel.get());
    }), state);
    state->refilter();
    // Only immutable family names leave the worker. Pango maps/fonts stay on
    // their owning thread, and no font files are loaded in the filter callback.
    auto *task = g_task_new(nullptr, state->cancel.get(), +[](GObject *, GAsyncResult *result, gpointer) {
        GError *error = nullptr;
        std::unique_ptr<ChineseFonts> fonts(static_cast<ChineseFonts *>(g_task_propagate_pointer(G_TASK(result), &error)));
        const auto &life = *static_cast<std::shared_ptr<FontDialog::Lifetime> *>(g_task_get_task_data(G_TASK(result)));
        life->withOwner([&](FontDialog &self) {
            self.ready = true;
            if (fonts) self.chinese = std::move(*fonts);
            const auto text = !fonts ? std::string("识别失败，可查看全部字体") : self.chinese.empty()
                ? std::string("未找到中文字体") : std::to_string(self.chinese.size()) + " 款";
            gtk_label_set_text(GTK_LABEL(self.status), text.c_str());
            gtk_widget_set_sensitive(self.onlyChinese, TRUE);
            if (!fonts || self.chinese.empty()) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self.onlyChinese), FALSE);
            self.refilter();
        });
        g_clear_error(&error);
    }, nullptr);
    g_task_set_task_data(task, new std::shared_ptr<FontDialog::Lifetime>(state->lifetime),
        +[](gpointer data) { delete static_cast<std::shared_ptr<FontDialog::Lifetime> *>(data); });
    g_task_run_in_thread(task, +[](GTask *current, gpointer, gpointer, GCancellable *cancel) {
        auto map = detail::GObjectHandle<PangoFontMap>::adopt(pango_cairo_font_map_new());
        auto context = detail::GObjectHandle<PangoContext>::adopt(pango_font_map_create_context(map.get()));
        PangoFontFamily **families = nullptr;
        int count = 0;
        pango_font_map_list_families(map.get(), &families, &count);
        auto fonts = std::make_unique<ChineseFonts>();
        for (int i = 0; i < count && !g_cancellable_is_cancelled(cancel); ++i) {
            const auto *name = pango_font_family_get_name(families[i]);
            auto *description = pango_font_description_new();
            pango_font_description_set_family(description, name);
            pango_font_description_set_size(description, 12 * PANGO_SCALE);
            auto font = detail::GObjectHandle<PangoFont>::adopt(pango_font_map_load_font(map.get(), context.get(), description));
            pango_font_description_free(description);
            if (!font.get()) continue;
            auto *coverage = pango_font_get_coverage(font.get(), pango_language_from_string("zh-cn"));
            constexpr std::u32string_view sample = U"中文你好输入世界";
            const bool chinese = std::all_of(sample.begin(), sample.end(), [&](char32_t ch) {
                return pango_coverage_get(coverage, static_cast<int>(ch)) == PANGO_COVERAGE_EXACT;
            });
#if PANGO_VERSION_CHECK(1, 44, 0)
            g_object_unref(coverage);
#else
            pango_coverage_unref(coverage);
#endif
            if (chinese) fonts->insert(name);
        }
        g_free(families);
        g_task_return_pointer(current, fonts.release(), +[](gpointer data) { delete static_cast<ChineseFonts *>(data); });
    });
    g_object_unref(task);
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
        gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
        gtk_window_set_default_size(GTK_WINDOW(dialog), 440, 400);
        gtk_container_set_border_width(GTK_CONTAINER(dialog), 0);
        auto *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
        gtk_container_set_border_width(GTK_CONTAINER(content), 12);
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL,
                                       "取消", "保留原来的候选字体");
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_OK,
                                       "使用字体", "在候选预览中使用选中的字体，保存设置后应用");
        styleFontChooserChild(content, nullptr);
        prepareFontDialog(dialog);
        gtk_window_resize(GTK_WINDOW(dialog), 440, 400);
    }
    g_list_free(windows);
}


} // namespace

GtkWidget *createCandidateFontButton() {
    auto *button = gtk_font_button_new();
    gtk_font_button_set_title(GTK_FONT_BUTTON(button), "选择候选字体");
    gtk_font_button_set_show_size(GTK_FONT_BUTTON(button), FALSE);
    gtk_font_button_set_show_style(GTK_FONT_BUTTON(button), FALSE);
    gtk_font_chooser_set_level(GTK_FONT_CHOOSER(button), GTK_FONT_CHOOSER_LEVEL_FAMILY);
    gtk_font_chooser_set_preview_text(GTK_FONT_CHOOSER(button), "你好，世界。ModernIME");
    g_signal_connect_after(button, "clicked", G_CALLBACK(styleFontChooser), nullptr);
    return button;
}

} // namespace modernime::settings
