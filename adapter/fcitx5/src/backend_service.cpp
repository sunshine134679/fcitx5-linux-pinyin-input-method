#include "modernime/fcitx5/backend_service.h"
#include "modernime/core/learning_store.h"
#include "modernime/core/learning_writer.h"
#include "modernime/pinyin/dictionary_pack.h"
#include <fcitx-utils/dbus/bus.h>
#include <fcitx-utils/dbus/objectvtable.h>
#include <fcitx-utils/event.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string_view>
#include <sys/random.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace modernime::fcitx5 {
namespace {
std::string quote(std::string_view text) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (const unsigned char ch : text) {
        if (ch == '"' || ch == '\\') { result += '\\'; result += static_cast<char>(ch); }
        else if (ch < 0x20) { result += "\\u00"; result += hex[ch >> 4]; result += hex[ch & 15]; }
        else result += static_cast<char>(ch);
    }
    return result + '"';
}
std::string boolean(bool value) { return value ? "true" : "false"; }
std::string error(std::string_view code, std::string_view message) {
    return "{\"ok\":false,\"error\":" + quote(code) + ",\"message\":" + quote(message) + "}";
}
std::string success() { return "{\"ok\":true}"; }
[[noreturn]] void invalid() { throw std::invalid_argument("参数数量、格式或范围无效"); }
void count(const std::vector<std::string> &args, std::size_t required) {
    if (args.size() != required) invalid();
}
std::size_t number(std::string_view text, std::size_t maximum) {
    std::size_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size() || value > maximum) invalid();
    return value;
}
bool parseBool(std::string_view text) {
    if (text == "true") return true;
    if (text == "false") return false;
    invalid();
}
std::filesystem::path absolutePath(const std::string &text) {
    if (text.empty() || !std::filesystem::path(text).is_absolute()) invalid();
    std::error_code code;
    auto result = std::filesystem::weakly_canonical(text, code);
    if (code) throw std::runtime_error("无法解析指定路径");
    return result;
}
bool validId(std::string_view id) {
    return !id.empty() && id.size() <= 80 && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
}
core::UserDataSelection selection(std::string_view text) {
    core::UserDataSelection result{false, false, false, false, false, false};
    std::set<std::string_view> seen;
    if (text.empty()) invalid();
    while (!text.empty()) {
        const auto separator = text.find(',');
        const auto component = text.substr(0, separator);
        if (!seen.insert(component).second) invalid();
        if (component == "settings") result.settings = true;
        else if (component == "dictionary") result.userDictionary = true;
        else if (component == "quick-phrases") result.quickPhrases = true;
        else if (component == "learning") result.learning = true;
        else if (component == "clipboard-history") result.clipboardHistory = true;
        else if (component == "clipboard-favorites") result.clipboardFavorites = true;
        else invalid();
        if (separator == std::string_view::npos) break;
        text.remove_prefix(separator + 1);
        if (text.empty()) invalid();
    }
    return result;
}
std::string manifestJson(const pinyin::DictionaryPackManifest &item, bool bundled) {
    return "{\"id\":" + quote(item.id) + ",\"name\":" + quote(item.name) +
        ",\"source\":" + quote(item.source) + ",\"license\":" + quote(item.license) +
        ",\"version\":" + quote(item.version) + ",\"entryCount\":" + std::to_string(item.entryCount) +
        ",\"enabled\":" + boolean(item.enabled) + ",\"bundled\":" + boolean(bundled) + "}";
}
std::string randomToken() {
    unsigned char bytes[16];
    std::size_t offset = 0;
    while (offset < sizeof(bytes)) {
        const auto size = getrandom(bytes + offset, sizeof(bytes) - offset, 0);
        if (size < 0 && errno == EINTR) continue;
        if (size <= 0) throw std::runtime_error("无法生成预览令牌");
        offset += static_cast<std::size_t>(size);
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string token;
    for (const auto byte : bytes) { token += hex[byte >> 4]; token += hex[byte & 15]; }
    return token;
}
}

class BackendService::Impl final {
public:
    explicit Impl(Config config) : config_(std::move(config)) {
        if (pipe2(stopPipe_, O_CLOEXEC | O_NONBLOCK) != 0) {
            setError("无法创建后端服务退出通知");
            return;
        }
        try { thread_ = std::thread([this] { run(); }); }
        catch (...) { close(stopPipe_[0]); close(stopPipe_[1]); stopPipe_[0] = stopPipe_[1] = -1; throw; }
    }
    ~Impl() {
        stopping_.store(true, std::memory_order_release);
        if (stopPipe_[1] >= 0) {
            const char stop = 'x';
            ssize_t written;
            do { written = write(stopPipe_[1], &stop, 1); } while (written < 0 && errno == EINTR);
        }
        if (thread_.joinable()) thread_.join();
        for (const auto fd : stopPipe_) if (fd >= 0) close(fd);
    }
    bool ready() const noexcept { return ready_.load(std::memory_order_acquire); }
    std::string startupError() const { std::lock_guard lock(errorMutex_); return startupError_; }
private:
    class Endpoint final : public fcitx::dbus::ObjectVTable<Endpoint> {
    public:
        explicit Endpoint(Impl &owner) : owner_(owner) {}
        std::string execute(const std::string &action, const std::vector<std::string> &args) {
            return owner_.execute(action, args);
        }
        FCITX_OBJECT_VTABLE_METHOD(execute, "Execute", "sas", "s");
    private:
        Impl &owner_;
    };
    struct Preview final {
        std::filesystem::path path;
        core::UserDataPreview preview;
        std::chrono::steady_clock::time_point created;
    };
    void setError(std::string message) {
        std::lock_guard lock(errorMutex_);
        startupError_ = std::move(message);
    }
    void run() noexcept {
        try {
            fcitx::EventLoop loop;
            fcitx::dbus::Bus bus(fcitx::dbus::BusType::Session);
            if (!bus.isOpen()) { setError("无法连接会话DBus"); return; }
            bus.attachEventLoop(&loop);
            Endpoint endpoint(*this);
            auto stop = loop.addIOEvent(stopPipe_[0], fcitx::IOEventFlag::In,
                [&](fcitx::EventSourceIO *, int, fcitx::IOEventFlags) { loop.exit(); return true; });
            if (!stop || !bus.addObjectVTable(objectPath, interfaceName, endpoint) ||
                !bus.requestName(serviceName, fcitx::Flags<fcitx::dbus::RequestNameFlag>{})) {
                setError("后端DBus服务注册失败或名称已占用");
                return;
            }
            ready_.store(true, std::memory_order_release);
            if (!stopping_.load(std::memory_order_acquire)) loop.exec();
            ready_.store(false, std::memory_order_release);
            endpoint.releaseSlot();
            bus.releaseName(serviceName);
            bus.detachEventLoop();
        } catch (...) { ready_.store(false, std::memory_order_release); setError("后端DBus服务运行失败"); }
    }
    std::shared_ptr<core::LearningWriter> writer() const {
        return config_.getWriter ? config_.getWriter() : nullptr;
    }
    std::string execute(const std::string &action, const std::vector<std::string> &args) noexcept {
        try {
            if (stopping_.load(std::memory_order_acquire)) return error("stopping", "后端服务正在退出");
            if (action.empty() || action.size() > 64 || args.size() > 8) invalid();
            for (const auto &arg : args) if (arg.size() > 4096 || arg.find('\0') != std::string::npos) invalid();
            if (action == "capabilities") {
                count(args, 0);
                return "{\"ok\":true,\"version\":1,\"actions\":[\"learning.list\",\"learning.search\","
                    "\"learning.forget\",\"learning.unsuppress\",\"pack.list\",\"pack.inspect\",\"pack.import\","
                    "\"pack.update\",\"pack.enable\",\"pack.disable\",\"pack.remove\",\"bundle.export\","
                    "\"bundle.preview\",\"bundle.restore\",\"diagnostics\",\"applications\"],"
                    "\"selectionComponents\":[\"settings\",\"dictionary\",\"quick-phrases\",\"learning\","
                    "\"clipboard-history\",\"clipboard-favorites\"],\"clipboardDefault\":false,"
                    "\"learningPageLimit\":200,\"restoreNeedsPreview\":true}";
            }
            if (action.starts_with("learning.")) return learning(action, args);
            if (action.starts_with("pack.")) return pack(action, args);
            if (action.starts_with("bundle.")) return bundle(action, args);
            if (action == "applications") {
                count(args, 0);
                std::string result = "{\"ok\":true,\"applications\":[";
                bool first = true;
                if (config_.compatibility) for (const auto &application : config_.compatibility->applications()) {
                    if (!first) result += ',';
                    first = false; result += quote(application);
                }
                return result + "]}";
            }
            if (action == "diagnostics") return diagnostics(args);
            return error("unknown_action", "未知后端操作");
        } catch (const std::invalid_argument &) { return error("invalid_arguments", "参数数量、格式或范围无效"); }
        catch (const std::exception &) { return error("operation_failed", "后端操作失败，请检查路径、权限或数据格式"); }
        catch (...) { return error("internal_error", "后端操作无法完成"); }
    }
    std::string learning(const std::string &action, const std::vector<std::string> &args) {
        if (action != "learning.list" && action != "learning.search" &&
            action != "learning.forget" && action != "learning.unsuppress")
            return error("unknown_action", "未知学习管理操作");
        if (action == "learning.forget" || action == "learning.unsuppress") {
            count(args, 2);
            if (args[0].empty() || args[0].size() > 512 || args[1].empty() || args[1].size() > 128) invalid();
            auto activeWriter = writer();
            if (!activeWriter) return error("learning_unavailable", "学习存储尚未就绪");
            const bool accepted = action == "learning.forget"
                ? activeWriter->enqueueForget(args[0], args[1])
                : activeWriter->enqueueUnsuppression(args[0], args[1]);
            if (!accepted) return error("learning_unavailable", "学习管理操作未被接受，请等待存储就绪");
            return activeWriter->flush() ? success()
                : error("persistence_failed", "操作尚未持久化，后台保留重试状态");
        }
        core::LearningQuery query;
        const bool search = action == "learning.search";
        const std::size_t base = search ? 1 : 0;
        if (args.size() != base && args.size() != base + 3) invalid();
        if (search) { if (args[0].size() > 512) invalid(); query.search = args[0]; }
        if (args.size() == base + 3) {
            query.offset = number(args[base], core::kMaxLearningEntries);
            query.limit = number(args[base + 1], 200);
            if (query.limit == 0) invalid();
            if (parseBool(args[base + 2])) query.suppressed = true;
        }
        auto activeWriter = writer();
        if (!activeWriter) return error("learning_unavailable", "学习存储尚未就绪");
        activeWriter->reloadIfChanged();
        if (activeWriter->epochViewState() != core::EpochViewState::Ready)
            return error("learning_reloading", "学习记录正在重新加载");
        const auto page = activeWriter->snapshot()->queryLearningEntries(query);
        std::string result = "{\"ok\":true,\"total\":" + std::to_string(page.total) + ",\"records\":[";
        bool first = true;
        for (const auto &record : page.records) {
            if (!first) result += ',';
            first = false;
            result += "{\"phrase\":" + quote(record.phrase) + ",\"pinyin\":" + quote(record.pinyin) +
                ",\"frequency\":" + std::to_string(record.frequency) + ",\"lastSelectedMs\":" +
                std::to_string(record.lastSelectedMs) + ",\"negativeFeedback\":" +
                std::to_string(record.negativeFeedback) + ",\"suppressed\":" + boolean(record.suppressed) +
                ",\"contextCount\":" + std::to_string(record.contextCount) + "}";
        }
        return result + "]}";
    }
    std::string pack(const std::string &action, const std::vector<std::string> &args) {
        pinyin::DictionaryPackStore local(config_.packRoot), bundled(config_.bundledRoot);
        if (action == "pack.list") {
            count(args, 0);
            std::map<std::string, std::pair<pinyin::DictionaryPackManifest, bool>> manifests;
            for (const auto &item : bundled.list()) manifests[item.id] = {item, true};
            for (const auto &item : local.list()) manifests[item.id] = {item, false};
            std::string result = "{\"ok\":true,\"packs\":[";
            bool first = true;
            for (const auto &[id, item] : manifests) {
                (void)id;
                if (!first) result += ',';
                first = false; result += manifestJson(item.first, item.second);
            }
            return result + "]}";
        }
        std::string message;
        if (action == "pack.inspect" || action == "pack.import" || action == "pack.update") {
            count(args, 1);
            const auto directory = absolutePath(args[0]);
            const auto data = local.validate(directory, &message);
            if (!data) return error("invalid_pack", message);
            if (action == "pack.inspect") return "{\"ok\":true,\"pack\":" + manifestJson(data->manifest, false) + "}";
            const auto localManifests = local.list();
            const auto bundledManifests = bundled.list();
            const auto hasId = [&](const auto &items) { return std::any_of(items.begin(), items.end(),
                [&](const auto &item) { return item.id == data->manifest.id; }); };
            const bool exists = hasId(localManifests) || hasId(bundledManifests);
            if (action == "pack.import" && exists) return error("pack_exists", "同ID词库包已存在，请使用更新");
            if (action == "pack.update" && !exists) return error("pack_missing", "未找到需要更新的词库包");
            if (!local.importPack(directory, action == "pack.update", &message)) return error("pack_write_failed", message);
            return success();
        }
        if (action != "pack.enable" && action != "pack.disable" && action != "pack.remove")
            return error("unknown_action", "未知词库包操作");
        count(args, 1);
        if (!validId(args[0])) invalid();
        if (action == "pack.remove") return local.remove(args[0], &message) ? success() : error("pack_remove_failed", message);
        const auto manifests = local.list();
        const bool installed = std::any_of(manifests.begin(), manifests.end(), [&](const auto &item) { return item.id == args[0]; });
        if (!installed && !local.importPack(config_.bundledRoot / args[0], false, &message))
            return error("pack_missing", message);
        return local.setEnabled(args[0], action == "pack.enable", &message) ? success() : error("pack_write_failed", message);
    }
    std::string bundle(const std::string &action, const std::vector<std::string> &args) {
        if (action != "bundle.export" && action != "bundle.preview" && action != "bundle.restore")
            return error("unknown_action", "未知数据备份操作");
        if (action == "bundle.export") { if (args.size() != 1 && args.size() != 2) invalid(); }
        else count(args, action == "bundle.preview" ? 1 : 3);
        const auto path = absolutePath(args[0]);
        auto activeWriter = writer();
        core::UserDataBundle data(config_.paths, activeWriter.get());
        std::string message;
        if (action == "bundle.export") {
            const auto selected = args.size() == 2 ? selection(args[1]) : core::UserDataSelection{};
            if (selected.learning && !activeWriter) return error("learning_unavailable", "学习存储尚未就绪");
            return data.exportTo(path, selected, &message) ? success() : error("bundle_export_failed", message);
        }
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(previews_, [&](const auto &item) { return now - item.second.created > std::chrono::minutes(10); });
        if (action == "bundle.preview") {
            const auto preview = data.preview(path, &message);
            if (!preview) return error("bundle_preview_failed", message);
            while (previews_.size() >= 8) {
                const auto oldest = std::min_element(previews_.begin(), previews_.end(),
                    [](const auto &a, const auto &b) { return a.second.created < b.second.created; });
                previews_.erase(oldest);
            }
            const auto token = randomToken();
            previews_.emplace(token, Preview{path, *preview, now});
            std::string result = "{\"ok\":true,\"token\":" + quote(token) + ",\"fingerprint\":" +
                quote(preview->bundleFingerprint) + ",\"differences\":[";
            bool first = true;
            for (const auto &difference : preview->differences) {
                if (!first) result += ',';
                first = false;
                result += "{\"component\":" + quote(core::userDataComponentName(difference.component)) +
                    ",\"included\":" + boolean(difference.included) + ",\"present\":" + boolean(difference.present) +
                    ",\"currentPresent\":" + boolean(difference.currentPresent) + ",\"changed\":" + boolean(difference.changed) +
                    ",\"bundleEntries\":" + std::to_string(difference.bundleEntries) +
                    ",\"currentEntries\":" + std::to_string(difference.currentEntries) + "}";
            }
            return result + "]}";
        }
        const auto found = previews_.find(args[1]);
        if (found == previews_.end() || found->second.path != path)
            return error("invalid_preview", "预览令牌不存在、已过期或与路径不符");
        const auto selected = selection(args[2]);
        if (selected.learning && !activeWriter) return error("learning_unavailable", "学习存储尚未就绪");
        if (!config_.beginMaintenance || !config_.endMaintenance || !config_.beginMaintenance())
            return error("maintenance_unavailable", "无法暂停文件重载，恢复尚未开始");
        struct Maintenance final {
            std::function<void()> &finish;
            ~Maintenance() { try { finish(); } catch (...) {} }
        } maintenance{config_.endMaintenance};
        const auto preview = found->second.preview;
        previews_.erase(found); // Each confirmation authorizes one attempted restore.
        return data.restore(path, selected, preview, &message) ? success() : error("bundle_restore_failed", message);
    }
    std::string diagnostics(const std::vector<std::string> &args) const {
        if (args.size() > 1) invalid();
        if (!config_.compatibility) return error("diagnostics_unavailable", "兼容性观察尚未就绪");
        const auto report = config_.compatibility->report(args.empty() ? std::string{} : args[0]);
        const auto &facts = report.facts;
        std::string result = "{\"ok\":true,\"observed\":" + boolean(report.observed) +
            ",\"application\":" + quote(facts.application) + ",\"frontend\":" + quote(facts.frontend) +
            ",\"display\":" + quote(facts.display) + ",\"transport\":" + quote(report.transport) +
            ",\"toolkit\":" + quote(report.toolkit) + ",\"sandbox\":" + quote(report.sandbox) +
            ",\"focused\":" + boolean(facts.focused) + ",\"sensitive\":" + boolean(facts.sensitive) +
            ",\"surroundingText\":" + boolean(facts.surroundingText) + ",\"clientPreedit\":" + boolean(facts.clientPreedit) +
            ",\"cursorRectangle\":" + boolean(facts.cursorRectangle) + ",\"modernimeSelected\":" + boolean(facts.modernimeSelected) +
            ",\"observedAtMs\":" + std::to_string(facts.observedAtMs) + ",\"handledKeys\":" + std::to_string(report.handledKeys) +
            ",\"maximumDispatchUs\":" + std::to_string(report.maximumDispatchUs) + ",\"suggestions\":[";
        bool first = true;
        for (const auto &suggestion : report.suggestions) { if (!first) result += ','; first = false; result += quote(suggestion); }
        return result + "]}";
    }
    Config config_;
    int stopPipe_[2]{-1, -1};
    std::thread thread_;
    std::atomic<bool> ready_{false}, stopping_{false};
    mutable std::mutex errorMutex_;
    std::string startupError_;
    std::map<std::string, Preview> previews_;
};
BackendService::BackendService(Config config) : impl_(std::make_unique<Impl>(std::move(config))) {}
BackendService::~BackendService() = default;
bool BackendService::ready() const noexcept { return impl_->ready(); }
std::string BackendService::startupError() const { return impl_->startupError(); }
} // namespace modernime::fcitx5
