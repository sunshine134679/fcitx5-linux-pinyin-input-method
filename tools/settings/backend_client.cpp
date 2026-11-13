#include "modernime/settings/backend_client.h"

#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <utility>

namespace modernime::settings {
namespace {
const BackendValue missing;
class Reader final {
public:
    explicit Reader(std::string_view input) : input_(input) {}
    BackendValue read() {
        if (input_.size() > 16 * 1024 * 1024 || !g_utf8_validate(input_.data(), static_cast<gssize>(input_.size()), nullptr)) fail();
        auto value = parse(0); space(); if (at_ != input_.size()) fail(); return value;
    }
private:
    [[noreturn]] static void fail() { throw std::runtime_error("暂时无法读取结果，请重启输入法后重试。"); }
    void space() { while (at_ < input_.size() && (input_[at_]==' ' || input_[at_]=='\n' || input_[at_]=='\r' || input_[at_]=='\t')) ++at_; }
    bool take(char ch) { space(); if (at_ < input_.size() && input_[at_]==ch) { ++at_; return true; } return false; }
    unsigned hex() {
        if (at_+4 > input_.size()) fail();
        unsigned value=0;
        for (unsigned n=0;n<4;++n) {
            const char ch=input_[at_++]; value <<= 4;
            if (ch>='0' && ch<='9') value+=static_cast<unsigned>(ch-'0');
            else if (ch>='a' && ch<='f') value+=static_cast<unsigned>(ch-'a'+10);
            else if (ch>='A' && ch<='F') value+=static_cast<unsigned>(ch-'A'+10);
            else fail();
        }
        return value;
    }
    std::string string() {
        if (!take('"')) fail();
        std::string out;
        while (at_ < input_.size()) {
            const auto ch=static_cast<unsigned char>(input_[at_++]);
            if (ch=='"') return out;
            if (ch<0x20) fail();
            if (ch!='\\') { out+=static_cast<char>(ch); continue; }
            if (at_==input_.size()) fail();
            switch(input_[at_++]) {
            case '"':out+='"';break; case '\\':out+='\\';break; case '/':out+='/';break;
            case 'b':out+='\b';break;case 'f':out+='\f';break;case 'n':out+='\n';break;case 'r':out+='\r';break;case 't':out+='\t';break;
            case 'u': {
                unsigned code=hex();
                if (code>=0xd800 && code<=0xdbff) {
                    if (at_+2>input_.size() || input_.substr(at_,2)!="\\u") fail();
                    at_+=2;
                    const auto low=hex(); if (low<0xdc00 || low>0xdfff) fail();
                    code=0x10000+((code-0xd800)<<10)+(low-0xdc00);
                } else if (code>=0xdc00 && code<=0xdfff) fail();
                if (code==0) fail(); // GTK strings cannot represent embedded NUL.
                char bytes[6]; const int size=g_unichar_to_utf8(code,bytes); out.append(bytes,static_cast<std::size_t>(size)); break;
            }
            default:fail();
            }
        }
        fail();
    }
    BackendValue parse(unsigned depth) {
        if (depth>32) fail();
        space();
        if (at_==input_.size()) fail();
        BackendValue result;
        if (take('{')) {
            result.kind=BackendValue::Kind::Object; if (take('}')) return result;
            do { const auto key=string(); if (!take(':')) fail(); if (!result.members.emplace(key,parse(depth+1)).second) fail(); } while(take(','));
            if (!take('}')) fail();
            return result;
        }
        if (take('[')) {
            result.kind=BackendValue::Kind::Array; if (take(']')) return result;
            do { result.items.push_back(parse(depth+1)); } while(take(','));
            if (!take(']')) fail();
            return result;
        }
        if (input_[at_]=='"') { result.kind=BackendValue::Kind::String; result.scalar=string(); return result; }
        for (const auto literal : {std::string_view("true"),std::string_view("false"),std::string_view("null")}) {
            if (input_.substr(at_,literal.size())==literal) {
                at_+=literal.size(); result.kind=literal=="null" ? BackendValue::Kind::Null : BackendValue::Kind::Boolean;
                result.scalar=literal; return result;
            }
        }
        const auto begin=at_; if (input_[at_]=='-') ++at_;
        if (at_==input_.size() || input_[at_]<'0' || input_[at_]>'9') fail();
        if (input_[at_]=='0') ++at_;
        else while(at_<input_.size() && input_[at_]>='0' && input_[at_]<='9') ++at_;
        if (at_<input_.size() && input_[at_]=='.') {
            ++at_; const auto start=at_; while(at_<input_.size() && input_[at_]>='0' && input_[at_]<='9') ++at_; if(start==at_) fail();
        }
        if (at_<input_.size() && (input_[at_]=='e' || input_[at_]=='E')) {
            ++at_; if(at_<input_.size() && (input_[at_]=='+' || input_[at_]=='-')) ++at_;
            const auto start=at_; while(at_<input_.size() && input_[at_]>='0' && input_[at_]<='9') ++at_; if(start==at_) fail();
        }
        result.kind=BackendValue::Kind::Number; result.scalar=input_.substr(begin,at_-begin); return result;
    }
    std::string_view input_; std::size_t at_=0;
};
using Variant=std::unique_ptr<GVariant,decltype(&g_variant_unref)>;
using Object=std::unique_ptr<GDBusConnection,decltype(&g_object_unref)>;
constexpr auto destination="org.modernime.Backend1";
constexpr auto objectPath="/org/modernime/Backend1";
bool mutating(std::string_view action) {
    return action=="learning.forget" || action=="learning.unsuppress" || action=="bundle.restore" || action=="bundle.export" ||
        (action.starts_with("pack.") && action!="pack.list" && action!="pack.inspect");
}
BackendReply invoke(GDBusConnection *bus,const char *owner,const std::string &action,const std::vector<std::string> &args,GCancellable *cancel) {
    std::vector<const char *> pointers; for(const auto &arg:args) pointers.push_back(arg.c_str()); pointers.push_back(nullptr);
    GError *error=nullptr;
    Variant reply(g_dbus_connection_call_sync(bus,owner,objectPath,destination,"Execute",
        g_variant_new("(s^as)",action.c_str(),pointers.data()),
        G_VARIANT_TYPE("(s)"),G_DBUS_CALL_FLAGS_NO_AUTO_START,30000,cancel,&error),g_variant_unref);
    if (!reply) {
        BackendReply failure; failure.error="transport"; failure.uncertain=mutating(action);
        failure.message=failure.uncertain ? "操作结果尚未确认，请刷新状态后再继续。" : "无法连接输入法，请确认 ModernIME 正在运行后重试。";
        if(error && g_error_matches(error,G_IO_ERROR,G_IO_ERROR_CANCELLED)) failure.error="cancelled";
        g_clear_error(&error); return failure;
    }
    const char *json=nullptr; g_variant_get(reply.get(),"(&s)",&json);
    auto parsed=parseBackendReply(json);
    if(parsed.error=="invalid_reply") parsed.uncertain=mutating(action);
    return parsed;
}
} // namespace

const BackendValue &BackendValue::get(std::string_view key) const { const auto it=members.find(key); return it==members.end()?missing:it->second; }
std::string BackendValue::text(std::string_view key) const { const auto &v=key.empty()?*this:get(key); return v.kind==Kind::String?v.scalar:std::string{}; }
std::uint64_t BackendValue::number(std::string_view key) const {
    const auto &v=key.empty()?*this:get(key); std::uint64_t n=0;
    if(v.kind!=Kind::Number) return 0;
    const auto parsed=std::from_chars(v.scalar.data(),v.scalar.data()+v.scalar.size(),n);
    return parsed.ec==std::errc{} && parsed.ptr==v.scalar.data()+v.scalar.size()?n:0;
}
bool BackendValue::boolean(std::string_view key) const { const auto &v=key.empty()?*this:get(key); return v.kind==Kind::Boolean && v.scalar=="true"; }
const std::vector<BackendValue> &BackendValue::array(std::string_view key) const { return (key.empty()?*this:get(key)).items; }
BackendReply parseBackendReply(std::string_view json) {
    BackendReply reply;
    try {
        reply.data=Reader(json).read();
        if(reply.data.kind!=BackendValue::Kind::Object || reply.data.get("ok").kind!=BackendValue::Kind::Boolean) throw std::runtime_error("暂时无法读取结果，请重试。");
        reply.ok=reply.data.boolean("ok"); reply.error=reply.data.text("error"); reply.message=reply.data.text("message");
        if(!reply.ok && reply.message.empty()) reply.message="操作未完成，请刷新状态后重试。";
    } catch(const std::exception &e) { reply.ok=false;reply.error="invalid_reply";reply.message=e.what(); }
    return reply;
}
struct BackendClient::State final {
    bool alive=true;
    GCancellable *cancel=g_cancellable_new();
    ~State(){ g_object_unref(cancel); }
};
BackendClient::BackendClient():state_(std::make_shared<State>()){}
BackendClient::~BackendClient(){state_->alive=false;g_cancellable_cancel(state_->cancel);}
void BackendClient::execute(std::string action,std::vector<std::string> args,Callback callback) {
    struct Work { std::shared_ptr<State> state;std::string action;std::vector<std::string> args;Callback callback;BackendReply reply; };
    auto *task=g_task_new(nullptr,state_->cancel,+[](GObject *,GAsyncResult *result,gpointer){
        auto &work=*static_cast<Work *>(g_task_get_task_data(G_TASK(result)));
        if(work.state->alive && work.callback) work.callback(std::move(work.reply));
    },nullptr);
    g_task_set_task_data(task,new Work{state_,std::move(action),std::move(args),std::move(callback),{}},+[](gpointer p){delete static_cast<Work *>(p);});
    g_task_run_in_thread(task,+[](GTask *workerTask,gpointer,gpointer p,GCancellable *cancel){
        auto &work=*static_cast<Work *>(p); GError *error=nullptr;
        Object bus(g_bus_get_sync(G_BUS_TYPE_SESSION,cancel,&error),g_object_unref);
        Variant owner(bus ? g_dbus_connection_call_sync(bus.get(),"org.freedesktop.DBus","/org/freedesktop/DBus","org.freedesktop.DBus","GetNameOwner",
            g_variant_new("(s)",destination),G_VARIANT_TYPE("(s)"),G_DBUS_CALL_FLAGS_NO_AUTO_START,2000,cancel,&error) : nullptr,g_variant_unref);
        if (!owner) {
            g_clear_error(&error);work.reply.error="unavailable";work.reply.message="请先切换到 ModernIME，再点“刷新”。";
        } else {
            const char *unique=nullptr;g_variant_get(owner.get(),"(&s)",&unique);
            auto capabilities=invoke(bus.get(),unique,"capabilities",{},cancel);
            const auto &actions=capabilities.data.array("actions");
            if(!capabilities.ok || work.action=="capabilities") work.reply=std::move(capabilities);
            else if(std::none_of(actions.begin(),actions.end(),[&](const auto &v){return v.text()==work.action;})) {
                work.reply.error="unsupported";work.reply.message="当前输入法不支持此操作，请更新并重启输入法。";
            } else work.reply=invoke(bus.get(),unique,work.action,work.args,cancel);
        }
        g_task_return_boolean(workerTask,TRUE);
    });
    g_object_unref(task);
}
} // namespace modernime::settings
