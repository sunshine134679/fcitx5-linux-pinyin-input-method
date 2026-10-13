#include "modernime/settings/runtime_controller.h"

#include <gio/gio.h>
#include <glib.h>

#include <algorithm>
#include <fstream>
#include <string_view>

namespace modernime::settings {
namespace {

constexpr guint waitTimeoutMs = 1500;
constexpr guint startCheckTimeoutMs = 200;
constexpr int reloadAttempts = 20;
constexpr guint reloadIntervalUs = 100000;

struct ProcessResult final {
    bool started = false;
    bool successful = false;
    bool timedOut = false;
    std::string standardOutput;
    std::string standardError;
    std::string error;
};

// Each command owns its context, including its timeout. A worker must never
// pump GTK's default context or dispatch another worker's stack callbacks.
struct CommandLoop final {
    GMainContext *context = g_main_context_new();
    GMainLoop *loop = g_main_loop_new(context, FALSE);
    GSource *timeout = nullptr;

    CommandLoop() { g_main_context_push_thread_default(context); }
    ~CommandLoop() {
        if (timeout != nullptr) {
            g_source_destroy(timeout);
            g_source_unref(timeout);
        }
        g_main_context_pop_thread_default(context);
        g_main_loop_unref(loop);
        g_main_context_unref(context);
    }
    void setTimeout(guint milliseconds, GSourceFunc callback, gpointer data) {
        timeout = g_timeout_source_new(milliseconds);
        g_source_set_callback(timeout, callback, data, nullptr);
        g_source_attach(timeout, context);
    }
};

struct StartWaitState final {
    GMainLoop *loop = nullptr;
    GCancellable *cancellable = nullptr;
    bool timedOut = false;
    bool successful = false;
    std::string error;
};

struct CommunicationState final {
    GMainLoop *loop = nullptr;
    GCancellable *cancellable = nullptr;
    GSubprocess *process = nullptr;
    bool timedOut = false;
    bool successful = false;
    std::string standardOutput;
    std::string standardError;
    std::string error;
};

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    value.erase(last + 1);
    value.erase(0, first);
    return value;
}

void startFinished(GObject *object, GAsyncResult *result, gpointer data) {
    auto *state = static_cast<StartWaitState *>(data);
    GError *error = nullptr;
    state->successful = g_subprocess_wait_check_finish(
        G_SUBPROCESS(object), result, &error);
    if (error != nullptr) {
        state->error = error->message;
    }
    g_clear_error(&error);
    g_main_loop_quit(state->loop);
}

gboolean cancelStartAfterTimeout(gpointer data) {
    auto *state = static_cast<StartWaitState *>(data);
    state->timedOut = true;
    g_cancellable_cancel(state->cancellable);
    return G_SOURCE_REMOVE;
}

void communicationFinished(GObject *object, GAsyncResult *result,
                           gpointer data) {
    auto *state = static_cast<CommunicationState *>(data);
    gchar *standardOutput = nullptr;
    gchar *standardError = nullptr;
    GError *error = nullptr;
    state->successful = g_subprocess_communicate_utf8_finish(
        G_SUBPROCESS(object), result, &standardOutput, &standardError, &error);
    if (standardOutput != nullptr) {
        state->standardOutput = standardOutput;
    }
    if (standardError != nullptr) {
        state->standardError = standardError;
    }
    if (error != nullptr) {
        state->error = error->message;
    }
    g_free(standardOutput);
    g_free(standardError);
    g_clear_error(&error);
    g_main_loop_quit(state->loop);
}

gboolean cancelCommunicationAfterTimeout(gpointer data) {
    auto *state = static_cast<CommunicationState *>(data);
    state->timedOut = true;
    g_subprocess_force_exit(state->process);
    g_cancellable_cancel(state->cancellable);
    return G_SOURCE_REMOVE;
}

ProcessResult runCommand(const std::filesystem::path &executable,
                         const std::vector<std::string> &arguments,
                         const Environment &environment) {
    ProcessResult result;
    if (executable.empty()) {
        result.error = "fcitx5-remote path is empty";
        return result;
    }

    GSubprocessLauncher *launcher = g_subprocess_launcher_new(
        static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                      G_SUBPROCESS_FLAGS_STDERR_PIPE));
    for (const auto &[name, value] : environment) {
        g_subprocess_launcher_setenv(launcher, name.c_str(), value.c_str(),
                                     TRUE);
    }

    std::vector<const gchar *> argv;
    argv.reserve(arguments.size() + 1);
    argv.push_back(executable.c_str());
    for (const auto &argument : arguments) {
        argv.push_back(argument.c_str());
    }
    argv.push_back(nullptr);

    GError *spawnError = nullptr;
    GSubprocess *process = g_subprocess_launcher_spawnv(
        launcher, argv.data(), &spawnError);
    g_object_unref(launcher);
    if (process == nullptr) {
        result.error = spawnError == nullptr ? "unable to start fcitx5-remote"
                                             : spawnError->message;
        g_clear_error(&spawnError);
        return result;
    }
    result.started = true;

    CommandLoop loop;
    auto *cancellable = g_cancellable_new();
    CommunicationState state{loop.loop, cancellable, process, false, false,
                             {}, {}, {}};
    // Drain output while waiting. The deadline also covers descendants that
    // inherit a pipe after the direct child has exited.
    g_subprocess_communicate_utf8_async(process, nullptr, cancellable,
                                      communicationFinished, &state);
    loop.setTimeout(waitTimeoutMs, cancelCommunicationAfterTimeout, &state);
    g_main_loop_run(loop.loop);
    g_object_unref(cancellable);
    result.standardOutput = std::move(state.standardOutput);
    result.standardError = std::move(state.standardError);
    result.error = std::move(state.error);
    result.timedOut = state.timedOut;
    result.successful = !result.timedOut && state.successful &&
                        g_subprocess_get_successful(process);
    g_object_unref(process);
    return result;
}

ProcessResult startCommand(const std::filesystem::path &executable,
                           const std::vector<std::string> &arguments,
                           const Environment &environment) {
    ProcessResult result;
    if (executable.empty()) {
        result.error = "fcitx5 path is empty";
        return result;
    }

    GSubprocessLauncher *launcher = g_subprocess_launcher_new(
        static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                      G_SUBPROCESS_FLAGS_STDERR_SILENCE));
    // A daemon may inherit stderr long after this probe returns. Closing a
    // captured pipe would terminate it with SIGPIPE on its next log write.
    for (const auto &[name, value] : environment) {
        g_subprocess_launcher_setenv(launcher, name.c_str(), value.c_str(),
                                     TRUE);
    }

    std::vector<const gchar *> argv;
    argv.reserve(arguments.size() + 1);
    argv.push_back(executable.c_str());
    for (const auto &argument : arguments) {
        argv.push_back(argument.c_str());
    }
    argv.push_back(nullptr);

    GError *spawnError = nullptr;
    GSubprocess *process = g_subprocess_launcher_spawnv(
        launcher, argv.data(), &spawnError);
    g_object_unref(launcher);
    if (process == nullptr) {
        result.error = spawnError == nullptr ? "unable to start fcitx5"
                                             : spawnError->message;
        g_clear_error(&spawnError);
        return result;
    }

    result.started = true;

    CommandLoop loop;
    auto *cancellable = g_cancellable_new();
    StartWaitState state{loop.loop, cancellable, false, false, {}};
    g_subprocess_wait_check_async(process, cancellable, startFinished, &state);
    loop.setTimeout(startCheckTimeoutMs, cancelStartAfterTimeout, &state);
    g_main_loop_run(loop.loop);
    g_object_unref(cancellable);

    if (state.timedOut) {
        // A running daemon is expected not to exit during the short startup
        // check. Leave it alive and let the remote probe verify readiness.
        result.successful = true;
        g_object_unref(process);
        return result;
    }

    result.successful = state.successful;
    if (!result.successful) {
        result.error = state.error;
    }
    g_object_unref(process);
    return result;
}

std::string failureMessage(const ProcessResult &result,
                           std::string_view operation) {
    if (result.timedOut) {
        return std::string(operation) + " timed out";
    }
    if (!result.error.empty()) {
        return result.error;
    }
    if (!result.standardError.empty()) {
        return trim(result.standardError);
    }
    return std::string(operation) + " failed";
}

bool isFcitxState(std::string_view state) {
    return state == "0" || state == "1" || state == "2";
}

bool modernimeReady(const RuntimeStatus &status) {
    if (!status.running || !status.modernimeAvailable) {
        return false;
    }
    if (!status.inputContextAvailable) {
        // fcitx5-remote -n is empty when no application owns an input
        // context. In that state the global method cannot be reported, but
        // a successfully registered ModernIME is ready for the next context.
        return true;
    }
    return status.inputMethodEnabled && status.currentInputMethod == "modernime";
}

} // namespace

RuntimeStatus RuntimeController::probe(
    const std::filesystem::path &executable, const Environment &environment) {
    return probe(executable, {}, environment);
}

RuntimeStatus RuntimeController::probe(
    const std::filesystem::path &executable,
    const std::filesystem::path &runtimeStatePath,
    const Environment &environment) {
    RuntimeStatus status;
    const auto inputMethod = runCommand(executable, {"-n"}, environment);
    if (!inputMethod.started) {
        status.message = inputMethod.error;
        return status;
    }
    status.available = true;
    if (!inputMethod.successful) {
        status.message = failureMessage(inputMethod, "input method query");
        return status;
    }
    status.currentInputMethod = trim(inputMethod.standardOutput);

    const auto running = runCommand(executable, {}, environment);
    if (!running.successful) {
        status.message = failureMessage(running, "Fcitx5 status query");
        return status;
    }
    const auto state = trim(running.standardOutput);
    if (!isFcitxState(state)) {
        status.message = "无法解析 Fcitx5 状态";
        return status;
    }
    status.running = true;
    status.inputContextAvailable = state != "0";
    status.inputMethodEnabled = state == "2";

    const auto modernime =
        runCommand(executable, {"-m", "modernime"}, environment);
    status.modernimeAvailable = modernime.successful;
    status.modernimeActive = status.inputMethodEnabled &&
                             status.currentInputMethod == "modernime";
    if (!status.modernimeAvailable) {
        status.message = "Fcitx5 正在运行，但未找到 ModernIME";
    } else if (!status.inputContextAvailable) {
        status.message = "Fcitx5 正在运行，当前窗口暂无输入上下文";
    } else {
        status.message = "Fcitx5 正在运行";
    }

    if (!runtimeStatePath.empty()) {
        std::error_code ec;
        if (std::filesystem::exists(runtimeStatePath, ec)) {
            std::ifstream in(runtimeStatePath);
            if (in) {
                std::string line;
                while (std::getline(in, line)) {
                    const auto eq = line.find('=');
                    if (eq == std::string::npos) {
                        continue;
                    }
                    const auto key = line.substr(0, eq);
                    const auto val = line.substr(eq + 1);
                    if (key == "applied_content_hash") {
                        status.appliedContentHash = val;
                        status.appliedHashAvailable = true;
                    } else if (key == "settings_generation") {
                        try {
                            status.settingsGeneration = std::stoull(val);
                        } catch (...) {}
                    }
                }
            }
        }
    }
    return status;
}

RuntimeResult RuntimeController::reload(
    const std::filesystem::path &fcitxExecutable,
    const std::filesystem::path &remoteExecutable,
    const Environment &environment) {
    if (remoteExecutable.empty()) {
        return {false, "fcitx5-remote path is empty"};
    }

    const auto current = RuntimeController::probe(remoteExecutable, environment);
    if (!current.available) {
        return {false, current.message.empty() ? "无法连接 fcitx5-remote"
                                               : current.message};
    }

    if (current.running) {
        const auto replace = startCommand(
            fcitxExecutable, {"-d", "--replace", "-u", "modernime-ui"},
            environment);
        if (!replace.started || !replace.successful) {
            return {false, failureMessage(replace, "Fcitx5 替换")};
        }
    } else {
        const auto start =
            startCommand(fcitxExecutable, {"-d", "-u", "modernime-ui"},
                         environment);
        if (!start.started || !start.successful) {
            return {false, failureMessage(start, "Fcitx5 启动")};
        }
    }

    std::string lastError;
    for (int attempt = 0; attempt < reloadAttempts; ++attempt) {
        const auto select = runCommand(
            remoteExecutable, {"-s", "modernime"}, environment);
        const auto enable = runCommand(remoteExecutable, {"-o"}, environment);
        const auto status = probe(remoteExecutable, environment);
        if (select.successful && enable.successful &&
            modernimeReady(status)) {
            return {true, "ModernIME 已重新加载并激活"};
        }

        if (!select.successful) {
            lastError = failureMessage(select, "ModernIME 激活");
        } else if (!enable.successful) {
            lastError = failureMessage(enable, "ModernIME 启用");
        } else if (!status.message.empty()) {
            lastError = status.message;
        }
        if (attempt + 1 < reloadAttempts) {
            g_usleep(reloadIntervalUs);
        }
    }

    return {false, lastError.empty() ? "ModernIME 激活失败" : lastError};
}

std::string describeSettingsApplication(
    const RuntimeStatus &status,
    std::string_view savedContentHash) {
    if (savedContentHash.empty()) {
        return "设置已保存";
    }
    if (!status.running) {
        return "已保存，下次启用时应用";
    }
    if (!status.appliedHashAvailable) {
        return "已保存，应用状态待确认";
    }
    if (status.appliedContentHash != savedContentHash) {
        return "已保存，等待输入法读取";
    }
    if (status.inputContextAvailable) {
        return "已保存并应用";
    }
    return "已保存，等待新输入上下文生效";
}

} // namespace modernime::settings
