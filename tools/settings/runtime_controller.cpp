#include "modernime/settings/runtime_controller.h"

#include <gio/gio.h>
#include <glib.h>

#include <algorithm>
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

struct WaitState final {
    GMainLoop *loop = nullptr;
    GSubprocess *process = nullptr;
    bool timedOut = false;
};

struct StartWaitState final {
    GMainLoop *loop = nullptr;
    GCancellable *cancellable = nullptr;
    bool timedOut = false;
    bool successful = false;
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

void waitFinished(GObject *object, GAsyncResult *result, gpointer data) {
    auto *state = static_cast<WaitState *>(data);
    GError *error = nullptr;
    g_subprocess_wait_check_finish(G_SUBPROCESS(object), result, &error);
    g_clear_error(&error);
    g_main_loop_quit(state->loop);
}

gboolean killAfterTimeout(gpointer data) {
    auto *state = static_cast<WaitState *>(data);
    state->timedOut = true;
    g_subprocess_force_exit(state->process);
    return G_SOURCE_REMOVE;
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

    auto *loop = g_main_loop_new(nullptr, FALSE);
    WaitState state{loop, process, false};
    g_subprocess_wait_check_async(process, nullptr, waitFinished, &state);
    const auto timeoutSource =
        g_timeout_add(waitTimeoutMs, killAfterTimeout, &state);
    g_main_loop_run(loop);
    if (!state.timedOut) {
        g_source_remove(timeoutSource);
    }
    g_main_loop_unref(loop);

    gchar *standardOutput = nullptr;
    gchar *standardError = nullptr;
    GError *communicationError = nullptr;
    if (!g_subprocess_communicate_utf8(process, nullptr, nullptr,
                                       &standardOutput, &standardError,
                                       &communicationError)) {
        result.error = communicationError == nullptr
                           ? "unable to read fcitx5-remote output"
                           : communicationError->message;
        g_clear_error(&communicationError);
    }
    if (standardOutput != nullptr) {
        result.standardOutput = standardOutput;
    }
    if (standardError != nullptr) {
        result.standardError = standardError;
    }
    g_free(standardOutput);
    g_free(standardError);
    result.timedOut = state.timedOut;
    result.successful = !result.timedOut && g_subprocess_get_successful(process);
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
        result.error = spawnError == nullptr ? "unable to start fcitx5"
                                             : spawnError->message;
        g_clear_error(&spawnError);
        return result;
    }

    result.started = true;

    auto *loop = g_main_loop_new(nullptr, FALSE);
    auto *cancellable = g_cancellable_new();
    StartWaitState state{loop, cancellable, false, false, {}};
    g_subprocess_wait_check_async(process, cancellable, startFinished, &state);
    const auto timeoutSource =
        g_timeout_add(startCheckTimeoutMs, cancelStartAfterTimeout, &state);
    g_main_loop_run(loop);
    if (!state.timedOut) {
        g_source_remove(timeoutSource);
    }
    g_object_unref(cancellable);
    g_main_loop_unref(loop);

    if (state.timedOut) {
        // A running daemon is expected not to exit during the short startup
        // check. Leave it alive and let the remote probe verify readiness.
        result.successful = true;
        g_object_unref(process);
        return result;
    }

    gchar *standardError = nullptr;
    GError *communicationError = nullptr;
    if (!g_subprocess_communicate_utf8(process, nullptr, nullptr, nullptr,
                                       &standardError, &communicationError)) {
        result.error = communicationError == nullptr
                           ? state.error
                           : communicationError->message;
        g_clear_error(&communicationError);
    }
    if (standardError != nullptr && result.error.empty()) {
        result.error = trim(standardError);
    }
    g_free(standardError);
    result.successful = state.successful;
    if (!result.successful && result.error.empty()) {
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

} // namespace

RuntimeStatus RuntimeController::probe(
    const std::filesystem::path &executable, const Environment &environment) {
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
    status.running = state == "0" || state == "1" || state == "2";
    status.modernimeActive = status.running &&
                             status.currentInputMethod == "modernime";
    status.message = status.running ? "Fcitx5 正在运行" : "Fcitx5 未激活";
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
        const auto reload = runCommand(remoteExecutable, {"-r"}, environment);
        if (!reload.successful) {
            return {false, failureMessage(reload, "Fcitx5 重载")};
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
            status.modernimeActive) {
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

} // namespace modernime::settings
