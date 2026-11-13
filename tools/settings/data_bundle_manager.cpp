#include "modernime/settings/data_bundle_manager.h"
#include "modernime/settings/backend_client.h"
#include "modernime/settings/settings_disclosure.h"
#include "modernime/settings/settings_widgets.h"
#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace modernime::settings {
namespace {
constexpr std::array ids{"settings","dictionary","quick-phrases","learning","clipboard-history","clipboard-favorites"};
constexpr std::array names{"设置","个人词典","快捷短语","学习记录","剪贴板历史","剪贴板收藏"};
void target(GtkWidget *w,const char *id) {g_object_set_data_full(G_OBJECT(w),"modernime-settings-target",g_strdup(id),g_free);}
GtkWidget *label(const std::string &text) {
    auto *w=gtk_label_new(text.c_str());gtk_label_set_xalign(GTK_LABEL(w),0);gtk_label_set_line_wrap(GTK_LABEL(w),TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(w),PANGO_WRAP_WORD_CHAR);gtk_label_set_max_width_chars(GTK_LABEL(w),1);gtk_widget_set_hexpand(w,TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(w),"modernime-description");return w;
}
}
class DataBundleManager::Impl final {
public:
    GtkWidget *root=nullptr,*exportButton=nullptr,*restoreButton=nullptr,*status=nullptr,*dialog=nullptr,*dialogStatus=nullptr,*accept=nullptr;
    std::array<GtkWidget *,6> choices{};
    std::unique_ptr<BackendClient> client=std::make_unique<BackendClient>();
    std::function<bool()> canRestore;std::function<void()> restored;
    bool busy=false,restoreMode=false,committing=false;unsigned generation=0;std::string directory,token;
    std::vector<std::string> supportedComponents;
    Impl(std::function<bool()> ready,std::function<void()> done):canRestore(std::move(ready)),restored(std::move(done)) {
        root=createSectionCard("备份与恢复","");target(root,"data-bundle");
        auto *actions=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
        exportButton=gtk_button_new_with_label("创建备份");restoreButton=gtk_button_new_with_label("从备份恢复");
        target(exportButton,"bundle-export");target(restoreButton,"bundle-restore");
        gtk_style_context_add_class(gtk_widget_get_style_context(exportButton),"suggested-action");
        gtk_box_pack_start(GTK_BOX(actions),exportButton,FALSE,FALSE,0);gtk_box_pack_start(GTK_BOX(actions),restoreButton,FALSE,FALSE,0);
        gtk_box_pack_start(GTK_BOX(root),actions,FALSE,FALSE,0);
        status=label("剪贴板内容默认不备份。");target(status,"bundle-status");gtk_label_set_selectable(GTK_LABEL(status),TRUE);
        gtk_box_pack_start(GTK_BOX(root),status,FALSE,FALSE,0);
        gtk_widget_set_tooltip_text(root,"备份用户数据至本地文件夹，不包含程序、内置资源及词库包。请只恢复自己信任的备份。");
        g_signal_connect(exportButton,"clicked",G_CALLBACK(+[](GtkButton *,gpointer p){static_cast<Impl *>(p)->choose(false);}),this);
        g_signal_connect(restoreButton,"clicked",G_CALLBACK(+[](GtkButton *,gpointer p){static_cast<Impl *>(p)->choose(true);}),this);
        g_signal_connect(root,"destroy",G_CALLBACK(+[](GtkWidget *,gpointer p){auto &s=*static_cast<Impl *>(p);s.client.reset();++s.generation;if(s.dialog)gtk_widget_destroy(s.dialog);s.root=nullptr;}),this);
        setSettingsFocusChain(root,{exportButton,restoreButton});
    }
    ~Impl(){if(root)gtk_widget_destroy(root);}
    GtkWindow *parent() const {auto *top=gtk_widget_get_toplevel(root);return GTK_IS_WINDOW(top)?GTK_WINDOW(top):nullptr;}
    void state(bool value,const std::string &message) {
        busy=value;gtk_widget_set_sensitive(exportButton,!value);gtk_widget_set_sensitive(restoreButton,!value);
        gtk_label_set_text(GTK_LABEL(status),message.c_str());
    }
    void ownDialog(GtkWidget *value) {
        dialog=value;gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog),TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(dialog),"modernime-settings");
        g_signal_connect(dialog,"delete-event",G_CALLBACK(+[](GtkWidget *,GdkEvent *,gpointer p)->gboolean{return static_cast<Impl *>(p)->committing;}),this);
        g_signal_connect(dialog,"destroy",G_CALLBACK(+[](GtkWidget *,gpointer p){auto &s=*static_cast<Impl *>(p);s.dialog=nullptr;s.accept=nullptr;s.dialogStatus=nullptr;s.choices.fill(nullptr);}),this);
    }
    void choose(bool restore) {
        if(busy || dialog || !client)return;
        if(restore && canRestore && !canRestore()){state(false,"请先保存或放弃设置和短语草稿，等待数据操作完成后再恢复。");return;}
        restoreMode=restore;
        auto *chooser=gtk_file_chooser_dialog_new(restore?"选择备份文件夹":"保存备份文件夹",parent(),
            restore?GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER:GTK_FILE_CHOOSER_ACTION_SAVE,
            "取消",GTK_RESPONSE_CANCEL,restore?"查看差异":"下一步",GTK_RESPONSE_ACCEPT,nullptr);
        if(!restore) {
            auto *now=g_date_time_new_now_local();auto *name=g_date_time_format(now,"ModernIME-备份-%Y%m%d-%H%M%S");
            gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(chooser),name);g_free(name);g_date_time_unref(now);
        }
        ownDialog(chooser);target(dialog,restore?"bundle-restore-directory":"bundle-export-directory");
        g_signal_connect(chooser,"response",G_CALLBACK(+[](GtkDialog *d,gint response,gpointer p){
            auto &s=*static_cast<Impl *>(p);std::string path;
            if(response==GTK_RESPONSE_ACCEPT){auto *filename=gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));if(filename)path=filename;g_free(filename);}
            gtk_widget_destroy(GTK_WIDGET(d));if(path.empty() || !s.root)return;s.directory=std::move(path);
            if(s.restoreMode)s.preview();else s.exportChoices();
        }),this);
        gtk_widget_show(chooser);
    }
    void makeChoicesDialog(const char *title,const char *action) {
        auto *newDialog=gtk_dialog_new_with_buttons(title,parent(),GTK_DIALOG_MODAL,"取消",GTK_RESPONSE_CANCEL,action,GTK_RESPONSE_ACCEPT,nullptr);
        ownDialog(newDialog);target(newDialog,restoreMode?"bundle-restore-preview":"bundle-export-selection");
        gtk_window_set_default_size(GTK_WINDOW(newDialog),510,-1);
        auto *body=gtk_dialog_get_content_area(GTK_DIALOG(newDialog));gtk_container_set_border_width(GTK_CONTAINER(body),20);gtk_box_set_spacing(GTK_BOX(body),12);
        auto *path=label(directory);gtk_label_set_selectable(GTK_LABEL(path),TRUE);gtk_box_pack_start(GTK_BOX(body),path,FALSE,FALSE,0);
        for(std::size_t n=0;n<ids.size();++n){choices[n]=gtk_check_button_new_with_label(names[n]);target(choices[n],ids[n]);gtk_box_pack_start(GTK_BOX(body),choices[n],FALSE,FALSE,0);
            g_signal_connect(choices[n],"toggled",G_CALLBACK(+[](GtkToggleButton *,gpointer p){static_cast<Impl *>(p)->updateAccept();}),this);}
        dialogStatus=label("");gtk_box_pack_start(GTK_BOX(body),dialogStatus,FALSE,FALSE,0);
        accept=gtk_dialog_get_widget_for_response(GTK_DIALOG(newDialog),GTK_RESPONSE_ACCEPT);target(accept,restoreMode?"bundle-restore-confirm":"bundle-export-confirm");
        gtk_style_context_add_class(gtk_widget_get_style_context(accept),restoreMode?"destructive-action":"suggested-action");
        gtk_widget_set_sensitive(accept,FALSE);
        g_signal_connect(newDialog,"response",G_CALLBACK(+[](GtkDialog *d,gint response,gpointer p){auto &s=*static_cast<Impl *>(p);
            if(s.committing)return;
            if(response!=GTK_RESPONSE_ACCEPT){gtk_widget_destroy(GTK_WIDGET(d));s.token.clear();return;}
            s.commit();
        }),this);
        gtk_widget_show_all(newDialog);
    }
    std::string selected() const {
        std::string csv;
        for(std::size_t n=0;n<ids.size();++n)if(choices[n] && gtk_widget_get_sensitive(choices[n]) && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(choices[n]))){if(!csv.empty())csv+=',';csv+=ids[n];}
        return csv;
    }
    void updateAccept(){if(accept)gtk_widget_set_sensitive(accept,!busy && !selected().empty());}
    void exportChoices() {
        makeChoicesDialog("创建本地备份","创建备份");
        state(true,"正在读取备份选项…");const auto run=++generation;
        client->execute("capabilities",{},[this,run](BackendReply reply){if(!root || run!=generation)return;
            state(false,reply.ok?"选择要备份的数据。":reply.message);
            if(!dialog)return;
            const auto &supported=reply.data.array("selectionComponents");
            for(std::size_t n=0;n<ids.size();++n){const bool available=reply.ok && std::any_of(supported.begin(),supported.end(),[n](const auto &v){return v.text()==ids[n];});
                gtk_widget_set_sensitive(choices[n],available);gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(choices[n]),available && n<4);}
            gtk_label_set_text(GTK_LABEL(dialogStatus),reply.ok?"剪贴板历史和收藏默认不选；备份未加密，请妥善保管。":reply.message.c_str());updateAccept();
        });
    }
    void preview() {
        if(busy || !client)return;
        token.clear();state(true,"正在比较备份与当前数据…");const auto run=++generation;
        client->execute("capabilities",{},[this,run](BackendReply caps){
            if(!root || run!=generation)return;
            if(!caps.ok){state(false,caps.message);return;}
            supportedComponents.clear();for(const auto &v:caps.data.array("selectionComponents"))supportedComponents.push_back(v.text());
        client->execute("bundle.preview",{directory},[this,run](BackendReply reply){if(!root || run!=generation)return;
            state(false,reply.ok?"请选择要恢复的数据。":reply.message);if(!reply.ok)return;
            token=reply.data.text("token");if(token.empty()){state(false,"未获得有效预览，请重新选择备份。");return;}
            makeChoicesDialog("查看差异并恢复","恢复所选数据");
            for(std::size_t n=0;n<ids.size();++n){
                const auto &diffs=reply.data.array("differences");const auto found=std::find_if(diffs.begin(),diffs.end(),[n](const auto &v){return v.text("component")==ids[n];});
                const bool available=found!=diffs.end() && found->boolean("included") && found->boolean("present") &&
                    std::find(supportedComponents.begin(),supportedComponents.end(),ids[n])!=supportedComponents.end();
                std::string text=names[n];
                if(!available)text+=" · 备份中未包含";
                else if(!found->boolean("changed"))text+=" · 与当前相同";
                else if(n==0)text+=" · 设置有变化";
                else text+=" · 当前 "+std::to_string(found->number("currentEntries"))+" → 备份 "+std::to_string(found->number("bundleEntries"));
                gtk_button_set_label(GTK_BUTTON(choices[n]),text.c_str());gtk_widget_set_sensitive(choices[n],available);gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(choices[n]),FALSE);
            }
            gtk_label_set_text(GTK_LABEL(dialogStatus),"恢复会替换所选数据。仅恢复你信任的本地备份；未选项目保持原状。");updateAccept();
        });
        });
    }
    void commit() {
        if(busy || !client)return;
        const auto csv=selected();if(csv.empty())return;
        if(restoreMode && canRestore && !canRestore()){gtk_label_set_text(GTK_LABEL(dialogStatus),"请先保存或放弃未保存的修改，然后重新预览。");gtk_widget_set_sensitive(accept,FALSE);token.clear();return;}
        if(restoreMode && token.empty())return;
        committing=true;state(true,restoreMode?"正在恢复，请稍候…":"正在创建备份…");gtk_widget_set_sensitive(dialog,FALSE);
        const auto run=++generation;const bool restoring=restoreMode;
        std::vector<std::string> args=restoring?std::vector<std::string>{directory,token,csv}:std::vector<std::string>{directory,csv};token.clear();
        client->execute(restoring?"bundle.restore":"bundle.export",std::move(args),[this,run,restoring](BackendReply reply){if(!root || run!=generation)return;
            if(reply.uncertain){
                const auto message=reply.message;
                state(true,"操作结果未确认，正在重新查询服务状态…");
                client->execute("capabilities",{},[this,run,restoring,message](BackendReply checked){
                    if(!root || run!=generation)return;
                    committing=false;if(dialog)gtk_widget_destroy(dialog);
                    state(false,message+(restoring?" 请重新查看差异后再恢复。":" 请核对备份目录后再操作。"));
                    if(checked.ok && restoring && restored)restored();
                });return;
            }
            committing=false;if(dialog)gtk_widget_destroy(dialog);
            if(reply.ok){state(false,restoring?"所选数据已恢复。":"备份已保存至："+directory);if(restoring && restored)restored();}
            else state(false,reply.message+(restoring?" 请重新查看差异后再恢复。":""));
        });
    }
};
DataBundleManager::DataBundleManager(std::function<bool()> ready,std::function<void()> done):impl_(std::make_unique<Impl>(std::move(ready),std::move(done))){}
DataBundleManager::~DataBundleManager()=default;
GtkWidget *DataBundleManager::widget()const{return impl_->root;}
bool DataBundleManager::focusTarget(std::string_view id){if(id=="bundle-export" || id=="data-bundle")return focusWidgetOrFallback(impl_->exportButton);if(id=="bundle-restore")return focusWidgetOrFallback(impl_->restoreButton);return false;}
}
