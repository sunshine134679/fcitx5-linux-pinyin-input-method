#include "modernime/settings/backend_client.h"
#include "modernime/settings/data_bundle_manager.h"
#include "modernime/settings/learning_manager.h"
#include "modernime/settings/settings_widgets.h"
#include <gtk/gtk.h>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>
using namespace modernime::settings;
namespace {
GtkWidget *find(GtkWidget *root,std::string_view id) {
    const auto *value=static_cast<const char *>(g_object_get_data(G_OBJECT(root),"modernime-settings-target"));
    if(value && id==value)return root;
    if(!GTK_IS_CONTAINER(root))return nullptr;
    auto *children=gtk_container_get_children(GTK_CONTAINER(root));GtkWidget *result=nullptr;
    for(auto *p=children;p && !result;p=p->next)result=find(GTK_WIDGET(p->data),id);
    g_list_free(children);return result;
}
GtkWidget *dialog(std::string_view id) {
    auto *windows=gtk_window_list_toplevels();GtkWidget *result=nullptr;
    for(auto *p=windows;p && !result;p=p->next)result=find(GTK_WIDGET(p->data),id);
    g_list_free(windows);return result;
}
void wait(const std::function<bool()> &ready) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!ready() && std::chrono::steady_clock::now()<deadline){while(g_main_context_iteration(nullptr,FALSE)){};std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    assert(ready());
}
void click(GtkWidget *w){assert(GTK_IS_BUTTON(w));gtk_button_clicked(GTK_BUTTON(w));}
std::string text(GtkWidget *w){return gtk_label_get_text(GTK_LABEL(w));}
struct Service {
    std::string action;std::vector<std::string> args;bool suppressed=true,forgotten=false;unsigned writes=0,exports=0,restores=0;
    static void call(GDBusConnection *,const gchar *,const gchar *,const gchar *,const gchar *,GVariant *params,GDBusMethodInvocation *inv,gpointer p){
        auto &s=*static_cast<Service *>(p);const char *name=nullptr;gchar **args=nullptr;g_variant_get(params,"(&s^as)",&name,&args);
        s.action=name;s.args.clear();for(auto **it=args;*it;++it)s.args.emplace_back(*it);g_strfreev(args);std::string json="{\"ok\":true}";
        if(s.action=="capabilities")json=R"({"ok":true,"version":1,"actions":["learning.list","learning.search","learning.forget","learning.unsuppress","bundle.export","bundle.preview","bundle.restore"],"selectionComponents":["settings","dictionary","quick-phrases","learning","clipboard-history","clipboard-favorites"]})";
        else if(s.action=="learning.list" || s.action=="learning.search") {
            const auto offset=s.args[s.action=="learning.search"?1:0];
            json=s.forgotten?R"({"ok":true,"total":0,"records":[]})":std::string(R"({"ok":true,"total":101,"records":[{"phrase":")")+(offset=="100"?"第二页":"合成词")+R"(","pinyin":"hechengci","frequency":5,"lastSelectedMs":1000,"negativeFeedback":0,"contextCount":2,"suppressed":)"+(s.suppressed?"true":"false")+"}]}";
        } else if(s.action=="learning.unsuppress"){assert((s.args==std::vector<std::string>{"合成词","hechengci"}));s.suppressed=false;++s.writes;}
        else if(s.action=="learning.forget"){assert((s.args==std::vector<std::string>{"合成词","hechengci"}));s.forgotten=true;++s.writes;}
        else if(s.action=="bundle.export"){++s.exports;assert(s.args.size()==2);assert(s.args[1]=="settings,dictionary,quick-phrases,learning");}
        else if(s.action=="bundle.preview")json=R"({"ok":true,"token":"synthetic-preview","differences":[{"component":"settings","included":true,"present":true,"changed":true,"currentEntries":1,"bundleEntries":1},{"component":"learning","included":true,"present":true,"changed":true,"currentEntries":9,"bundleEntries":3},{"component":"clipboard-history","included":true,"present":true,"changed":true,"currentEntries":9,"bundleEntries":3}]})";
        else if(s.action=="bundle.restore"){++s.restores;assert(s.args.size()==3);assert(s.args[1]=="synthetic-preview");assert(s.args[2]=="learning");}
        else assert(false);
        g_dbus_method_invocation_return_value(inv,g_variant_new("(s)",json.c_str()));
    }
};
}
int main(int argc,char **argv) {
    assert(parseBackendReply(R"({"ok":true,"value":"\u4e2d\u6587\ud83d\ude00","n":18446744073709551615})").data.text("value")=="中文😀");
    assert(parseBackendReply(R"({"ok":true,"n":18446744073709551615})").data.number("n")==UINT64_MAX);
    assert(!parseBackendReply(R"({"ok":true,"ok":false})").ok);
    if(!gtk_init_check(&argc,&argv))return 77;
    installSettingsStyles();GError *error=nullptr;auto *bus=g_bus_get_sync(G_BUS_TYPE_SESSION,nullptr,&error);assert(bus && !error);
    auto *owned=g_dbus_connection_call_sync(bus,"org.freedesktop.DBus","/org/freedesktop/DBus","org.freedesktop.DBus","RequestName",g_variant_new("(su)","org.modernime.Backend1",0U),G_VARIANT_TYPE("(u)"),G_DBUS_CALL_FLAGS_NONE,1000,nullptr,&error);assert(owned && !error);guint ownerResult=0;g_variant_get(owned,"(u)",&ownerResult);assert(ownerResult==1);g_variant_unref(owned);
    auto *info=g_dbus_node_info_new_for_xml("<node><interface name='org.modernime.Backend1'><method name='Execute'><arg type='s' direction='in'/><arg type='as' direction='in'/><arg type='s' direction='out'/></method></interface></node>",&error);assert(info && !error);
    Service fake;static const GDBusInterfaceVTable table{Service::call,nullptr,nullptr,{nullptr}};
    auto registration=g_dbus_connection_register_object(bus,"/org/modernime/Backend1",info->interfaces[0],&table,&fake,nullptr,&error);assert(registration && !error);
    auto *window=gtk_window_new(GTK_WINDOW_TOPLEVEL);auto *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,8);gtk_container_add(GTK_CONTAINER(window),box);
    LearningManager learning([](std::string){});gtk_box_pack_start(GTK_BOX(box),learning.widget(),FALSE,FALSE,0);gtk_widget_show_all(window);learning.refresh();
    auto *list=find(window,"learning-records");auto *next=find(window,"learning-record-next");wait([&]{return gtk_widget_get_sensitive(next);});
    click(next);wait([&]{return fake.action=="learning.list" && fake.args[0]=="100" && gtk_widget_get_sensitive(list);});
    click(find(window,"learning-record-previous"));wait([&]{return fake.action=="learning.list" && fake.args[0]=="0" && gtk_widget_get_sensitive(list);});
    GtkTreeIter row;auto *model=gtk_tree_view_get_model(GTK_TREE_VIEW(list));assert(gtk_tree_model_get_iter_first(model,&row));gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(list)),&row);
    auto *unsuppress=find(window,"learning-unsuppress");assert(gtk_widget_get_sensitive(unsuppress));click(unsuppress);click(unsuppress);
    wait([&]{return fake.writes==1 && gtk_widget_get_sensitive(list);});assert(!gtk_widget_get_sensitive(unsuppress));
    click(find(window,"learning-forget"));auto *windows=gtk_window_list_toplevels();GtkWidget *confirmation=nullptr;for(auto *p=windows;p;p=p->next)if(GTK_IS_MESSAGE_DIALOG(p->data))confirmation=GTK_WIDGET(p->data);g_list_free(windows);assert(confirmation);
    gtk_dialog_response(GTK_DIALOG(confirmation),GTK_RESPONSE_ACCEPT);wait([&]{return fake.writes==2 && text(find(window,"learning-record-status")).find("暂无")!=std::string::npos;});
    bool allow=false,restored=false;DataBundleManager bundles([&]{return allow;},[&]{restored=true;});gtk_box_pack_start(GTK_BOX(box),bundles.widget(),FALSE,FALSE,0);gtk_widget_show_all(window);
    click(find(window,"bundle-restore"));assert(!dialog("bundle-restore-directory"));assert(text(find(window,"bundle-status")).find("草稿")!=std::string::npos);allow=true;
    char pattern[]="/tmp/modernime-bundle-client-XXXXXX";auto *temporary=mkdtemp(pattern);assert(temporary);const std::filesystem::path folder=temporary;
    click(find(window,"bundle-export"));auto *chooser=dialog("bundle-export-directory");assert(chooser);gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser),temporary);gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(chooser),"backup");
    wait([&]{auto *path=gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));const bool ready=path!=nullptr;g_free(path);return ready;});gtk_dialog_response(GTK_DIALOG(chooser),GTK_RESPONSE_ACCEPT);
    wait([&]{return dialog("bundle-export-confirm") && gtk_widget_get_sensitive(dialog("bundle-export-confirm"));});
    auto *selection=dialog("bundle-export-selection");assert(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(selection,"clipboard-history"))));assert(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(selection,"clipboard-favorites"))));
    gtk_dialog_response(GTK_DIALOG(selection),GTK_RESPONSE_ACCEPT);wait([&]{return fake.exports==1 && gtk_widget_get_sensitive(find(window,"bundle-export"));});
    click(find(window,"bundle-restore"));chooser=dialog("bundle-restore-directory");assert(chooser);gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(chooser),temporary);
    wait([&]{auto *path=gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));const bool ready=path && std::string(path)==temporary;g_free(path);return ready;});gtk_dialog_response(GTK_DIALOG(chooser),GTK_RESPONSE_ACCEPT);
    wait([&]{return dialog("bundle-restore-preview")!=nullptr;});selection=dialog("bundle-restore-preview");assert(!gtk_widget_get_sensitive(find(selection,"bundle-restore-confirm")));
    assert(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(selection,"clipboard-history"))));gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(selection,"learning")),TRUE);
    gtk_dialog_response(GTK_DIALOG(selection),GTK_RESPONSE_ACCEPT);wait([&]{return restored && fake.restores==1;});
    gtk_widget_destroy(window);std::filesystem::remove_all(folder);g_dbus_connection_unregister_object(bus,registration);g_dbus_node_info_unref(info);g_object_unref(bus);
}
