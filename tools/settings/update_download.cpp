#include "modernime/settings/update_controller.h"
#include "modernime/settings/detail/process_runner.h"
#include "modernime/settings/runtime_environment.h"

#include <glib.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <unistd.h>

namespace modernime::settings {
namespace {
std::string envValue(const Environment &environment, const char *key) {
    for (const auto &[name, value] : environment) {
        if (name == key) return value;
    }
    const char *value = g_getenv(key);
    return value ? value : "";
}

std::filesystem::path updatePrefix(const Environment &environment) {
    return currentInstallationPrefix(environment, MODERNIME_INSTALL_PREFIX);
}

std::string pythonExecutable(const Environment &environment) {
    std::istringstream paths(envValue(environment, "PATH"));
    std::string path;
    while (std::getline(paths, path, ':')) {
        if (path.empty()) continue;
        const auto python = std::filesystem::path(path) / "python3";
        if (::access(python.c_str(), X_OK) == 0) return python.string();
    }
    return {};
}

void readProgress(const std::string &directory, const UpdateProgressCallback &callback,
                  UpdateProgress &last) {
    if (!callback) return;
    std::ifstream stream(std::filesystem::path(directory) / "progress.ini");
    char content[4097];
    stream.read(content, sizeof(content));
    const auto size = stream.gcount();
    if (size <= 0 || size > 4096) return;
    auto *file = g_key_file_new();
    if (g_key_file_load_from_data(file, content, size, G_KEY_FILE_NONE, nullptr)) {
        gchar *phase = g_key_file_get_string(file, "update", "phase", nullptr);
        UpdateProgress current{phase ? phase : "",
            g_key_file_get_uint64(file, "update", "received", nullptr),
            g_key_file_get_uint64(file, "update", "total", nullptr)};
        g_free(phase);
        if (current.phase != last.phase || current.received != last.received || current.total != last.total) {
            last = current;
            callback(current);
        }
    }
    g_key_file_unref(file);
}

detail::CommandResult runWithProgress(const detail::CommandSpec &spec,
    const std::string &directory, const UpdateProgressCallback &progress) {
    auto command = std::async(std::launch::async, [&spec] { return detail::runCommand(spec); });
    UpdateProgress last;
    while (command.wait_for(std::chrono::milliseconds(120)) != std::future_status::ready) {
        readProgress(directory, progress, last);
    }
    readProgress(directory, progress, last);
    return command.get();
}

detail::CommandSpec helperCommand(const char *action, const std::string &directory,
                                  const Environment &environment) {
    detail::CommandSpec spec;
    spec.argv = {pythonExecutable(environment),
        (std::filesystem::path(directory) / "update-helper.py").string(), action,
        "--directory", directory, "--prefix", updatePrefix(environment).string(),
        "--channel", UpdateController::detectInstallChannel(environment)};
    spec.environment = environment;
    spec.limits.perStreamBufferBytes = 65536;
    spec.limits.perStreamResultBytes = 65536;
    spec.limits.perStreamFileBytes = 1048576;
    return spec;
}

std::string failureDetail(const detail::CommandResult &result) {
    return result.stderrText.empty() ? result.stdoutText : result.stderrText;
}
} // namespace

DownloadedUpdate UpdateController::downloadUpdate(const UpdateStatus &previous,
    const Environment &environment, UpdateProgressCallback progress) {
    DownloadedUpdate download;
    const auto status = previous.checkedOnline ? previous : checkOnlineRelease(environment);
    if (!status.checkedOnline || !status.updateAvailable) {
        download.result.message = status.statusMessage.empty() ? "未找到可下载的新版本，请重新检查。" : status.statusMessage;
        return download;
    }
    const auto channel = detectInstallChannel(environment);
    if (channel != "script" && channel != "deb" && channel != "rpm") {
        download.result.message = "未找到当前客户端的安装记录，请修复安装后再试。";
        return download;
    }
    if (pythonExecutable(environment).empty()) {
        download.result.message = "缺少下载所需的 python3，请修复客户端安装。";
        return download;
    }
    const auto prefix = updatePrefix(environment);
    const auto helper = prefix / "bin/modernime-update";
    std::error_code error;
    if (!std::filesystem::is_regular_file(helper, error)) {
        download.result.message = "更新助手缺失，请修复客户端安装。";
        return download;
    }
    auto cache = envValue(environment, "XDG_CACHE_HOME");
    if (cache.empty()) cache = g_get_user_cache_dir();
    const auto cacheRoot = std::filesystem::path(cache) / "modernime";
    std::filesystem::create_directories(cacheRoot, error);
    auto pattern = (cacheRoot / "update-XXXXXX").string();
    if (error || !g_mkdtemp_full(pattern.data(), 0700)) {
        download.result.message = "无法创建更新下载目录，请检查磁盘空间和目录权限。";
        return download;
    }
    download.directory = pattern;
    download.version = status.candidateVersion;
    // 使用本轮助手的快照，安装新版本替换原文件后仍可完成重启。
    std::filesystem::copy_file(helper, std::filesystem::path(pattern) / "update-helper.py", error);
    if (error) {
        download.result.message = "无法准备更新助手：" + error.message();
    } else {
        auto spec = helperCommand("download", pattern, environment);
        spec.argv.insert(spec.argv.end(), {"--version", download.version});
        spec.mode = detail::CommandMode::Query;
        spec.timeoutMs = 600000;
        const auto result = runWithProgress(spec, pattern, progress);
        download.result.success = result.state == detail::CommandState::Exited && result.exitStatus == 0;
        download.result.message = download.result.success ? "下载与校验完成，已准备好更新。"
            : "下载未完成：" + failureDetail(result);
    }
    if (!download.result.success) {
        std::filesystem::remove_all(pattern, error);
        download.directory.clear();
    }
    return download;
}

RuntimeResult UpdateController::installUpdate(const DownloadedUpdate &download,
    const Environment &environment, UpdateProgressCallback progress) {
    if (!download.result.success || download.directory.empty()) return {false, "请先下载更新。"};
    const auto operation = tryBeginPackageOperation(PackageOperationKind::SystemUpgrade);
    if (!operation) return {false, "已有更新事务尚未结束，请等待完成。"};
    if (isPackageManagerLocked(environment)) {
        RuntimeResult result{false, "系统包管理器正忙，请稍后重试安装。"};
        finishPackageOperation(*operation, result, {}, {}, false);
        return result;
    }
    auto spec = helperCommand("install", download.directory, environment);
    spec.mode = detail::CommandMode::PackageTransaction;
    spec.operationId = *operation;
    spec.timeoutMs = 1000;
    advancePackageOperation(*operation, PackageOperationPhase::Starting, PackageOperationPhase::Running);
    auto result = runWithProgress(spec, download.directory, progress);
    if (result.state == detail::CommandState::Continuing) {
        advancePackageOperation(*operation, PackageOperationPhase::Running, PackageOperationPhase::Continuing);
        UpdateProgress last;
        do {
            result = detail::waitForOperation(*operation, 200);
            readProgress(download.directory, progress, last);
        } while (result.state == detail::CommandState::Continuing);
    }
    if (result.state == detail::CommandState::Unknown) {
        advancePackageOperation(*operation, currentPackageOperationPhase(), PackageOperationPhase::Uncertain);
        return {false, "安装状态暂时无法确认。为保护正在进行的安装，已停止重复操作。"};
    }
    const bool success = result.state == detail::CommandState::Exited && result.exitStatus == 0;
    RuntimeResult installed{success, success ? "更新已安装，正在准备重启。"
                                            : "安装未完成：" + failureDetail(result)};
    finishPackageOperation(*operation, installed, result.stdoutText, result.stderrText, false);
    return installed;
}

RuntimeResult UpdateController::restartUpdatedClient(const DownloadedUpdate &download,
                                                     const Environment &environment) {
    const auto python = pythonExecutable(environment);
    const auto helper = (std::filesystem::path(download.directory) / "update-helper.py").string();
    if (python.empty() || ::access(helper.c_str(), R_OK) != 0) return {false, "重启助手不可用，请重新打开设置。"};
    std::ifstream stat("/proc/self/stat");
    std::string text;
    std::getline(stat, text);
    const auto end = text.rfind(')');
    if (end == std::string::npos) return {false, "无法确认当前进程，暂未重启。"};
    std::istringstream fields(text.substr(end + 2));
    std::string startTime;
    for (int field = 3; field <= 22; ++field) fields >> startTime;
    std::vector<std::string> args{python, helper, "restart", "--prefix", updatePrefix(environment).string(),
        "--parent-pid", std::to_string(::getpid()), "--parent-start-time", startTime,
        "--directory", download.directory};
    std::vector<gchar *> argv;
    for (auto &arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    gchar **env = g_get_environ();
    for (const auto &[key, value] : environment) env = g_environ_setenv(env, key.c_str(), value.c_str(), TRUE);
    GError *error = nullptr;
    const bool spawned = g_spawn_async(nullptr, argv.data(), env,
        static_cast<GSpawnFlags>(G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL),
        nullptr, nullptr, nullptr, &error);
    g_strfreev(env);
    RuntimeResult result{spawned, spawned ? "正在重启以完成更新…"
        : std::string("未能启动重启助手：") + (error ? error->message : "未知错误")};
    g_clear_error(&error);
    return result;
}
} // namespace modernime::settings
