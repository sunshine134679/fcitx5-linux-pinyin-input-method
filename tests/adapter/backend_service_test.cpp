#include "modernime/fcitx5/backend_service.h"
#include "modernime/core/learning_writer.h"
#include <fcitx-utils/dbus/bus.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <iostream>
#include <thread>
#include <vector>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <cerrno>

extern char **environ;

namespace {
void require(bool value, const char *message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
}
int main() {
    using modernime::fcitx5::BackendService;
    std::string pattern = (std::filesystem::temp_directory_path() / "modernime-dbus-backend-XXXXXX").string();
    require(mkdtemp(pattern.data()) != nullptr, "temporary directory");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{pattern};
    auto writer = std::make_shared<modernime::core::LearningWriter>(cleanup.path / "learning.sqlite3");
    require(writer->enqueueSelection("你好", "nihao", {}, {}, 1000), "seed selection");
    const auto mainThread = std::this_thread::get_id();
    std::atomic<unsigned> maintenanceStarts{0}, maintenanceEnds{0};
    std::atomic<bool> workerMaintenance{false};
    BackendService::Config config;
    config.packRoot = cleanup.path / "packs";
    config.bundledRoot = cleanup.path / "bundled";
    config.paths.settings = cleanup.path / "settings.conf";
    config.paths.userDictionary = cleanup.path / "dictionary.txt";
    config.paths.quickPhrases = cleanup.path / "quick-phrases.mb";
    config.paths.clipboardHistory = cleanup.path / "clipboard-history.bin";
    config.paths.clipboardFavorites = cleanup.path / "clipboard-favorites.bin";
    config.compatibility = std::make_shared<modernime::core::InputCompatibilityTracker>();
    config.getWriter = [writer] { return writer; };
    config.beginMaintenance = [&] { ++maintenanceStarts; workerMaintenance = std::this_thread::get_id() != mainThread; return true; };
    config.endMaintenance = [&] { ++maintenanceEnds; };
    BackendService service(config);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!service.ready() && service.startupError().empty() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(service.ready(), "DBus service ready");
    fcitx::dbus::Bus client(fcitx::dbus::BusType::Session);
    auto invoke = [&](const std::string &action, const std::vector<std::string> &args = {}) {
        auto request = client.createMethodCall(BackendService::serviceName, BackendService::objectPath,
                                               BackendService::interfaceName, "Execute");
        request << action << args;
        auto reply = request.call(5'000'000);
        require(reply && !reply.isError(), "Execute transport");
        std::string result;
        reply >> result;
        return result;
    };
    require(invoke("capabilities").find("\"ok\":true") != std::string::npos, "capabilities");
    char *nativeArgs[]{const_cast<char *>("gdbus"), const_cast<char *>("call"), const_cast<char *>("--session"),
        const_cast<char *>("--dest"), const_cast<char *>(BackendService::serviceName),
        const_cast<char *>("--object-path"), const_cast<char *>(BackendService::objectPath),
        const_cast<char *>("--method"), const_cast<char *>("org.modernime.Backend1.Execute"),
        const_cast<char *>("capabilities"), const_cast<char *>("[]"), nullptr};
    pid_t nativeClient = -1;
    require(posix_spawnp(&nativeClient, "gdbus", nullptr, nullptr, nativeArgs, environ) == 0, "start cross-process gdbus client");
    int nativeStatus = 0;
    pid_t waited;
    do { waited = waitpid(nativeClient, &nativeStatus, 0); } while (waited < 0 && errno == EINTR);
    require(waited == nativeClient && WIFEXITED(nativeStatus) && WEXITSTATUS(nativeStatus) == 0,
            "cross-process gdbus Execute call");
    require(invoke("unknown").find("\"ok\":false") != std::string::npos, "unknown action rejected");
    require(invoke("learning.list", {"-1", "100", "false"}).find("\"ok\":false") != std::string::npos, "negative offset rejected");
    require(invoke("learning.search", {"你好"}).find("你好") != std::string::npos, "learning search");
    require(invoke("learning.forget", {"你好", "nihao"}).find("\"ok\":true") != std::string::npos, "forget flush");
    require(!writer->snapshot()->hasPositiveFrequency("你好", "nihao"), "in-process snapshot updated");
    {
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto staleMs = nowMs - 14LL * 24 * 60 * 60 * 1000;
        require(writer->enqueueSelection("陈旧单次词", "chenjiudanci", {}, {}, staleMs), "seed stale singleton");
        require(writer->enqueueSelection("误触单次词", "wuchudanci", {}, {}, nowMs), "seed recent singleton");
        require(writer->enqueueNegativeFeedback("误触单次词", "wuchudanci"), "seed singleton negative feedback");
        require(writer->enqueueSelection("近期保留词", "jinqibaoliu", {}, {}, nowMs), "seed recent keep");
        require(writer->enqueueSelection("高频习惯词", "gaopinxiguan", {}, {}, staleMs), "seed habit 1");
        require(writer->enqueueSelection("高频习惯词", "gaopinxiguan", {}, {}, staleMs + 100), "seed habit 2");
        require(writer->enqueueSelection("屏蔽陈旧词", "pingbichenjiu", {}, {}, staleMs), "seed suppressed singleton");
        require(writer->enqueueSuppression("屏蔽陈旧词", "pingbichenjiu"), "suppress singleton");
        require(writer->flush(), "flush prune_stale fixtures");
        const auto pruneReply = invoke("learning.prune_stale", {"7"});
        require(pruneReply.find("\"ok\":true") != std::string::npos &&
                pruneReply.find("\"removed\":2") != std::string::npos,
                "learning.prune_stale removes stale and rejected singletons only");
        const auto snap = writer->snapshot();
        require(snap->entry("陈旧单次词", "chenjiudanci", {}, {}) == nullptr, "stale singleton pruned");
        require(snap->entry("误触单次词", "wuchudanci", {}, {}) == nullptr, "rejected singleton pruned");
        require(snap->hasPositiveFrequency("近期保留词", "jinqibaoliu"), "recent singleton preserved");
        require(snap->hasPositiveFrequency("高频习惯词", "gaopinxiguan"), "multi-use habit preserved");
        require(snap->isSuppressed("屏蔽陈旧词", "pingbichenjiu"), "suppressed singleton preserved");
        require(invoke("learning.forget", {"近期保留词", "jinqibaoliu"}).find("\"ok\":true") != std::string::npos, "clean recent");
        require(invoke("learning.forget", {"高频习惯词", "gaopinxiguan"}).find("\"ok\":true") != std::string::npos, "clean habit");
        require(invoke("learning.forget", {"屏蔽陈旧词", "pingbichenjiu"}).find("\"ok\":true") != std::string::npos, "clean suppressed");
    }
    require(invoke("bundle.restore", {(cleanup.path / "backup").string(), "missing-token", "learning"}).find("\"ok\":false") != std::string::npos, "unpreviewed restore rejected");
    require(invoke("bundle.export", {(cleanup.path / "backup").string()}).find("\"ok\":true") != std::string::npos, "bundle export");
    const auto preview = invoke("bundle.preview", {(cleanup.path / "backup").string()});
    require(preview.find("\"token\":") != std::string::npos, "preview token");
    const auto tokenStart = preview.find("\"token\":\"");
    require(tokenStart != std::string::npos, "token field");
    const auto tokenOffset = tokenStart + std::string("\"token\":\"").size();
    const auto token = preview.substr(tokenOffset, preview.find('"', tokenOffset) - tokenOffset);
    require(invoke("bundle.restore", {(cleanup.path / "other").string(), token, "learning"}).find("invalid_preview") != std::string::npos,
            "token is bound to previewed path");
    require(invoke("bundle.restore", {(cleanup.path / "backup").string(), token, ""}).find("invalid_arguments") != std::string::npos,
            "restore requires explicit nonempty selection");
    require(invoke("bundle.restore", {(cleanup.path / "backup").string(), token, "learning"}).find("\"ok\":true") != std::string::npos,
            "confirmed selected restore");
    require(maintenanceStarts == 1 && maintenanceEnds == 1 && workerMaintenance,
            "restore maintenance fence runs on service worker and is released");
    require(invoke("bundle.restore", {(cleanup.path / "backup").string(), token, "learning"}).find("invalid_preview") != std::string::npos,
            "restore token is consumed");
    const auto source = cleanup.path / "pack-source";
    std::filesystem::create_directory(source);
    std::ofstream(source / "manifest.ini") << "id=service-test\nname=测试词库\nsource=合成夹具\nlicense=CC0-1.0\nversion=1\nentry_count=1\n";
    std::ofstream(source / "entries.tsv") << "ni'hao\t你好\t0\n";
    require(invoke("pack.inspect", {source.string()}).find("service-test") != std::string::npos, "inspect offline package");
    require(invoke("pack.import", {source.string()}).find("\"ok\":true") != std::string::npos, "import offline package");
    require(invoke("pack.list").find("service-test") != std::string::npos, "list installed package");
    require(invoke("pack.disable", {"service-test"}).find("\"ok\":true") != std::string::npos, "disable package");
    require(invoke("pack.update", {source.string()}).find("\"ok\":true") != std::string::npos, "update package");
    require(invoke("pack.list").find("\"enabled\":false") != std::string::npos, "update retains disabled state");
    require(invoke("pack.enable", {"service-test"}).find("\"ok\":true") != std::string::npos, "enable package");
    require(invoke("pack.remove", {"service-test"}).find("\"ok\":true") != std::string::npos, "remove only managed package");
    require(invoke("diagnostics").find("\"ok\":true") != std::string::npos, "diagnostics");
    require(invoke("applications").find("\"ok\":true") != std::string::npos, "applications");
    std::cout << "backend DBus service checks passed\n";
}
