#include "modernime/settings/runtime_controller.h"

#include <gio/gio.h>
#include <glib.h>

#include <algorithm>
#include <string_view>

namespace modernime::settings {
namespace {

constexpr guint waitTimeoutMs = 1500;

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
    const std::filesystem::path &executable, const Environment &environment) {
    const auto result = runCommand(executable, {"-r"}, environment);
    if (!result.started) {
        return {false, result.error};
    }
    if (!result.successful) {
        return {false, failureMessage(result, "ModernIME reload")};
    }
    return {true, "ModernIME reload requested"};
}

} // namespace modernime::settings
