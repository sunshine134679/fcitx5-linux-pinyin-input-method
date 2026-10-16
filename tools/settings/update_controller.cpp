#include "modernime/settings/update_controller.h"

#include <gio/gio.h>
#include <glib.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace modernime::settings {
namespace {

#ifndef MODERNIME_VERSION
#define MODERNIME_VERSION "0.1.10"
#endif

#ifndef MODERNIME_INSTALL_PREFIX
#define MODERNIME_INSTALL_PREFIX "/usr"
#endif

constexpr guint kQueryTimeoutMs = 5000;
constexpr guint kPackageOperationTimeoutMs = 300000;


struct CommandOutput final {
    bool started = false;
    bool exitedZero = false;
    bool timedOut = false;
    bool stillRunningOnTimeout = false;
    int exitStatus = -1;
    std::string stdoutText;
    std::string stderrText;
};

struct SubprocessState final {
    GMainContext *context = nullptr;
    GMainLoop *loop = nullptr;
    GCancellable *cancellable = nullptr;
    GSubprocess *proc = nullptr;
    int stdoutFd = -1;
    int stderrFd = -1;
    bool forceKillOnTimeout = true;
    bool timedOut = false;
    bool stillRunningOnTimeout = false;
    bool finished = false;
    std::string out;
    std::string err;

    ~SubprocessState() {
        if (stdoutFd >= 0) {
            ::close(stdoutFd);
        }
        if (stderrFd >= 0) {
            ::close(stderrFd);
        }
    }
};



int createUnlinkedTempFd() {
    const char *tmpDir = g_get_tmp_dir();
    std::string tmpl = std::string(tmpDir != nullptr ? tmpDir : "/tmp") +
                       "/modernime-subproc-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    const int fd = ::mkostemp(buf.data(), O_CLOEXEC);
    if (fd >= 0) {
        ::unlink(buf.data());
    }
    return fd;
}

std::string readAllFromFd(int fd) {
    if (fd < 0) {
        return {};
    }
    if (::lseek(fd, 0, SEEK_SET) < 0) {
        return {};
    }
    std::string content;
    char buffer[4096];
    while (true) {
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n > 0) {
            content.append(buffer, static_cast<std::size_t>(n));
        } else if (n == 0) {
            break;
        } else if (errno != EINTR) {
            break;
        }
    }
    return content;
}

std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

std::string packageOperationFailureMessage(const CommandOutput &output,
                                          std::string_view action,
                                          const std::string &fallbackCommand) {
    std::string message(action);
    message += "未完成";
    if (!output.started) {
        message += "（无法启动系统认证或包管理器）";
    } else if (output.exitStatus >= 0) {
        message += "（退出码 " + std::to_string(output.exitStatus) + "）";
    }
    message += "。";
    if (output.stderrText.find("No authentication agent found") != std::string::npos ||
        output.stderrText.find("Error creating textual authentication agent") != std::string::npos) {
        message += "当前会话没有图形授权代理，请关闭此窗口，从桌面应用菜单重新打开 ModernIME 设置后重试。";
    }

    const auto rawDiagnostic = trim(output.stderrText.empty() ? output.stdoutText
                                                            : output.stderrText);
    if (!rawDiagnostic.empty()) {
        gchar *valid = g_utf8_make_valid(rawDiagnostic.data(), rawDiagnostic.size());
        constexpr glong kMaxDiagnosticChars = 600;
        const glong diagnosticLength = g_utf8_strlen(valid, -1);
        const bool truncated = diagnosticLength > kMaxDiagnosticChars;
        gchar *excerpt = g_utf8_substring(valid, 0,
                                         truncated ? kMaxDiagnosticChars : diagnosticLength);
        message += "\n系统返回：";
        message += excerpt;
        if (truncated) {
            message += "…";
        }
        g_free(excerpt);
        g_free(valid);
    }
    message += "\n也可在终端执行：" + fallbackCommand;
    return message;
}

std::string lookupEnv(const Environment &environment, std::string_view key) {
    for (const auto &[k, v] : environment) {
        if (k == key) {
            return v;
        }
    }
    if (const char *val = std::getenv(std::string(key).c_str())) {
        return val;
    }
    return {};
}

guint resolvePackageOperationTimeoutMs(const Environment &environment) {
    const auto raw = trim(lookupEnv(environment, "MODERNIME_PACKAGE_OP_TIMEOUT_MS"));
    if (!raw.empty()) {
        char *endPtr = nullptr;
        const unsigned long parsed = std::strtoul(raw.c_str(), &endPtr, 10);
        if (endPtr != raw.c_str() && *endPtr == '\0' && parsed > 0) {
            return static_cast<guint>(parsed);
        }
    }
    return kPackageOperationTimeoutMs;
}

bool isSubprocessAlive(GSubprocess *proc) {
    if (proc == nullptr) {
        return false;
    }
    const gchar *ident = g_subprocess_get_identifier(proc);
    if (ident == nullptr || *ident == '\0') {
        return false;
    }
    char *endPtr = nullptr;
    const long pidVal = std::strtol(ident, &endPtr, 10);
    if (endPtr == ident || pidVal <= 0) {
        return false;
    }
    if (::kill(static_cast<pid_t>(pidVal), 0) != 0) {
        return false;
    }
    std::ifstream statIn("/proc/" + std::to_string(pidVal) + "/stat");
    if (statIn) {
        std::string statLine;
        std::getline(statIn, statLine);
        const auto rparen = statLine.rfind(')');
        if (rparen != std::string::npos && rparen + 2 < statLine.size()) {
            const char procState = statLine[rparen + 2];
            if (procState == 'Z' || procState == 'X' || procState == 'x') {
                return false;
            }
        }
    }
    return true;
}

std::filesystem::path findExecutableInPath(std::string_view name,
                                           const Environment &environment) {
    if (name.empty()) {
        return {};
    }
    const std::filesystem::path candidate(name);
    if (candidate.has_parent_path()) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return candidate;
        }
        return {};
    }

    std::string pathEnv = lookupEnv(environment, "PATH");
    if (pathEnv.empty()) {
        pathEnv = "/usr/bin:/bin:/usr/local/bin";
    }

    std::stringstream ss(pathEnv);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty()) {
            continue;
        }
        const auto full = std::filesystem::path(dir) / name;
        std::error_code ec;
        if (std::filesystem::is_regular_file(full, ec) &&
            g_file_test(full.c_str(), G_FILE_TEST_IS_EXECUTABLE)) {
            return full;
        }
    }
    return {};
}

void onWaitDone(GObject *source, GAsyncResult *res, gpointer userData) {
    auto *state = static_cast<SubprocessState *>(userData);
    GError *error = nullptr;
    state->finished =
        g_subprocess_wait_finish(G_SUBPROCESS(source), res, &error);
    state->out = readAllFromFd(state->stdoutFd);
    state->err = readAllFromFd(state->stderrFd);
    g_clear_error(&error);
    g_main_loop_quit(state->loop);
}

gboolean onCommandTimeout(gpointer userData) {
    auto *state = static_cast<SubprocessState *>(userData);
    state->timedOut = true;
    state->stillRunningOnTimeout = isSubprocessAlive(state->proc);
    if (state->forceKillOnTimeout || !state->stillRunningOnTimeout) {
        g_cancellable_cancel(state->cancellable);
    } else {
        // Keep g_subprocess_wait_async active while the child continues writing to
        // its unlinked regular files (immune to SIGPIPE even if modernime-settings exits).
        g_main_loop_quit(state->loop);
    }
    return G_SOURCE_REMOVE;
}

void startBackgroundPackageTracking(std::shared_ptr<SubprocessState> state,
                                    std::uint64_t operationId,
                                    PackageOperationKind opKind,
                                    std::string fallbackCmd) {
    std::thread([state = std::move(state), operationId, opKind,
                 fallbackCmd = std::move(fallbackCmd)]() {
        g_main_context_push_thread_default(state->context);
        if (!state->finished) {
            g_main_loop_run(state->loop);
        }
        g_subprocess_wait(state->proc, nullptr, nullptr);
        if (state->out.empty()) {
            state->out = readAllFromFd(state->stdoutFd);
        }
        if (state->err.empty()) {
            state->err = readAllFromFd(state->stderrFd);
        }

        const bool ifExited = g_subprocess_get_if_exited(state->proc);
        const bool exitedZero = ifExited &&
                                (g_subprocess_get_exit_status(state->proc) == 0);

        RuntimeResult finalRes;
        finalRes.success = exitedZero;
        if (opKind == PackageOperationKind::SystemUpgrade) {
            finalRes.message =
                exitedZero
                    ? "后台系统包管理器升级已完成，请点击“重新加载 ModernIME”使其生效。"
                    : ("后台系统包管理器升级未成功完成，请在终端检查状态或执行：" +
                       fallbackCmd);
        } else {
            finalRes.message =
                exitedZero
                    ? "后台软件源缓存刷新已完成，可点击“检查更新”同步最新候选版本。"
                    : ("后台软件源缓存刷新未成功完成，请在终端手动执行：" +
                       fallbackCmd);
        }

        g_object_unref(state->cancellable);
        g_main_loop_unref(state->loop);
        g_main_context_pop_thread_default(state->context);
        g_main_context_unref(state->context);
        g_object_unref(state->proc);

        if (!ifExited) {
            advancePackageOperation(operationId,
                                    PackageOperationPhase::Continuing,
                                    PackageOperationPhase::Uncertain);
            return;
        }

        finishPackageOperation(operationId, finalRes, std::move(state->out), std::move(state->err));
    }).detach();
}

CommandOutput runSubprocess(const std::vector<std::string> &argv,
                            const Environment &environment,
                            guint timeoutMs = kQueryTimeoutMs,
                            bool forceKillOnTimeout = true,
                            std::optional<std::uint64_t> operationId = std::nullopt,
                            std::optional<PackageOperationKind> opKind = std::nullopt,
                            std::string fallbackCmd = {}) {
    CommandOutput result;
    if (argv.empty()) {
        return result;
    }

    const int stdoutFd = createUnlinkedTempFd();
    const int stderrFd = createUnlinkedTempFd();
    const int childStdoutFd =
        (stdoutFd >= 0) ? ::fcntl(stdoutFd, F_DUPFD_CLOEXEC, 0) : -1;
    const int childStderrFd =
        (stderrFd >= 0) ? ::fcntl(stderrFd, F_DUPFD_CLOEXEC, 0) : -1;
    if (stdoutFd < 0 || stderrFd < 0 || childStdoutFd < 0 || childStderrFd < 0) {
        if (stdoutFd >= 0) {
            ::close(stdoutFd);
        }
        if (stderrFd >= 0) {
            ::close(stderrFd);
        }
        if (childStdoutFd >= 0) {
            ::close(childStdoutFd);
        }
        if (childStderrFd >= 0) {
            ::close(childStderrFd);
        }
        result.stderrText = "无法创建临时描述符重定向子进程输出";
        return result;
    }

    // Redirect child stdout/stderr to unlinked regular files instead of anonymous pipes.
    // This guarantees that if modernime-settings exits while a package manager transaction
    // is still running in the background, the child's stdout/stderr FDs remain valid regular
    // file descriptions and never receive SIGPIPE/EPIPE on subsequent writes.
    GSubprocessLauncher *launcher =
        g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
    g_subprocess_launcher_take_stdout_fd(launcher, childStdoutFd);
    g_subprocess_launcher_take_stderr_fd(launcher, childStderrFd);
    g_subprocess_launcher_setenv(launcher, "LC_ALL", "C", TRUE);
    g_subprocess_launcher_setenv(launcher, "LANG", "C", TRUE);
    for (const auto &[k, v] : environment) {
        if (k == "PATH") {
            const std::string subPath =
                v.empty() ? "/usr/bin:/bin" : (v + ":/usr/bin:/bin");
            g_subprocess_launcher_setenv(launcher, "PATH", subPath.c_str(), TRUE);
        } else {
            g_subprocess_launcher_setenv(launcher, k.c_str(), v.c_str(), TRUE);
        }
    }

    std::vector<const gchar *> rawArgv;
    rawArgv.reserve(argv.size() + 1);
    for (const auto &arg : argv) {
        rawArgv.push_back(arg.c_str());
    }
    rawArgv.push_back(nullptr);

    GError *error = nullptr;
    GSubprocess *proc = g_subprocess_launcher_spawnv(launcher, rawArgv.data(), &error);
    // The launcher owns these descriptors and closes them when replaced. Do not
    // close their old numbers again: another thread may already have reused them.
    g_subprocess_launcher_take_stdout_fd(launcher, -1);
    g_subprocess_launcher_take_stderr_fd(launcher, -1);
    g_object_unref(launcher);
    if (proc == nullptr) {
        ::close(stdoutFd);
        ::close(stderrFd);
        if (error != nullptr) {
            result.stderrText = error->message;
        }
        g_clear_error(&error);
        return result;
    }

    if (operationId.has_value()) {
        advancePackageOperation(*operationId,
                                PackageOperationPhase::Starting,
                                PackageOperationPhase::Running);
    }

    result.started = true;
    GMainContext *context = g_main_context_new();
    g_main_context_push_thread_default(context);
    GMainLoop *loop = g_main_loop_new(context, FALSE);
    GCancellable *cancellable = g_cancellable_new();

    auto state = std::make_shared<SubprocessState>();
    state->context = context;
    state->loop = loop;
    state->cancellable = cancellable;
    state->proc = proc;
    state->stdoutFd = stdoutFd;
    state->stderrFd = stderrFd;
    state->forceKillOnTimeout = forceKillOnTimeout;

    GSource *timer = g_timeout_source_new(timeoutMs);
    g_source_set_callback(timer, onCommandTimeout, state.get(), nullptr);
    g_source_attach(timer, context);

    g_subprocess_wait_async(proc, cancellable, onWaitDone, state.get());
    g_main_loop_run(loop);

    g_source_destroy(timer);
    g_source_unref(timer);
    g_main_context_pop_thread_default(context);

    if (state->timedOut) {
        result.timedOut = true;
        result.stillRunningOnTimeout =
            state->stillRunningOnTimeout || isSubprocessAlive(proc);
        if (forceKillOnTimeout && result.stillRunningOnTimeout) {
            g_subprocess_force_exit(proc);
        } else if (!forceKillOnTimeout && result.stillRunningOnTimeout && !state->finished) {
            if (operationId.has_value() && opKind.has_value()) {
                advancePackageOperation(*operationId,
                                        PackageOperationPhase::Running,
                                        PackageOperationPhase::Continuing);
                startBackgroundPackageTracking(state, *operationId, *opKind, std::move(fallbackCmd));
            }
            return result;
        }
    }

    g_subprocess_wait(proc, nullptr, nullptr);
    if (state->out.empty()) {
        state->out = readAllFromFd(state->stdoutFd);
    }
    if (state->err.empty()) {
        state->err = readAllFromFd(state->stderrFd);
    }
    result.stdoutText = std::move(state->out);
    result.stderrText = std::move(state->err);
    const bool ifExited = g_subprocess_get_if_exited(proc);
    if (state->finished && !state->timedOut && ifExited) {
        result.exitStatus = g_subprocess_get_exit_status(proc);
        result.exitedZero = (result.exitStatus == 0);
    } else if (!ifExited && !result.timedOut) {
        result.exitStatus = -1;
        result.exitedZero = false;
        if (operationId.has_value()) {
            advancePackageOperation(*operationId,
                                    PackageOperationPhase::Running,
                                    PackageOperationPhase::Uncertain);
        }
    }

    g_object_unref(cancellable);
    g_main_loop_unref(loop);
    g_main_context_unref(context);
    g_object_unref(proc);
    return result;
}

int charOrder(char c) {
    if (c == '~') {
        return -1;
    }
    if (c == '\0') {
        return 0;
    }
    if (std::isalpha(static_cast<unsigned char>(c))) {
        return static_cast<unsigned char>(c);
    }
    return static_cast<int>(static_cast<unsigned char>(c)) + 256;
}

int compareDebianSegment(std::string_view a, std::string_view b) {
    std::size_t ia = 0;
    std::size_t ib = 0;
    while (ia < a.size() || ib < b.size()) {
        while ((ia < a.size() && !std::isdigit(static_cast<unsigned char>(a[ia]))) ||
               (ib < b.size() && !std::isdigit(static_cast<unsigned char>(b[ib])))) {
            const char ca = (ia < a.size() && !std::isdigit(static_cast<unsigned char>(a[ia]))) ? a[ia++] : '\0';
            const char cb = (ib < b.size() && !std::isdigit(static_cast<unsigned char>(b[ib]))) ? b[ib++] : '\0';
            if (ca != cb) {
                return charOrder(ca) < charOrder(cb) ? -1 : 1;
            }
        }
        while (ia < a.size() && a[ia] == '0') {
            ++ia;
        }
        while (ib < b.size() && b[ib] == '0') {
            ++ib;
        }
        std::size_t da = ia;
        while (ia < a.size() && std::isdigit(static_cast<unsigned char>(a[ia]))) {
            ++ia;
        }
        std::size_t db = ib;
        while (ib < b.size() && std::isdigit(static_cast<unsigned char>(b[ib]))) {
            ++ib;
        }
        const std::size_t lenA = ia - da;
        const std::size_t lenB = ib - db;
        if (lenA != lenB) {
            return lenA < lenB ? -1 : 1;
        }
        const int cmp = a.substr(da, lenA).compare(b.substr(db, lenB));
        if (cmp != 0) {
            return cmp < 0 ? -1 : 1;
        }
    }
    return 0;
}

std::string detectInstallChannel(const Environment &environment) {
    std::vector<std::filesystem::path> candidates;
    const auto explicitChannelFile = lookupEnv(environment, "MODERNIME_INSTALL_CHANNEL_FILE");
    if (!explicitChannelFile.empty()) {
        candidates.emplace_back(explicitChannelFile);
    }
    const auto prefixEnv = lookupEnv(environment, "MODERNIME_PREFIX");
    if (!prefixEnv.empty()) {
        candidates.push_back(std::filesystem::path(prefixEnv) / "share/modernime/install-channel.txt");
    }
    candidates.push_back(std::filesystem::path(MODERNIME_INSTALL_PREFIX) / "share/modernime/install-channel.txt");
    candidates.emplace_back("/usr/share/modernime/install-channel.txt");

    for (const auto &path : candidates) {
        std::ifstream in(path);
        if (!in) {
            continue;
        }
        std::string line;
        while (std::getline(in, line)) {
            if (line.starts_with("channel=")) {
                return trim(std::string_view(line).substr(8));
            }
        }
    }
    return "unknown";
}

bool parseTrustedMetadataFile(const std::filesystem::path &path,
                              UpdateStatus &status) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line.starts_with('#')) {
            continue;
        }
        const auto eq = line.find('=');
        const auto colon = line.find(':');
        std::size_t sep = std::string::npos;
        if (eq != std::string::npos && (colon == std::string::npos || eq < colon)) {
            sep = eq;
        } else if (colon != std::string::npos) {
            sep = colon;
        }
        if (sep == std::string::npos) {
            continue;
        }
        const std::string key = trim(std::string_view(line).substr(0, sep));
        const std::string val = trim(std::string_view(line).substr(sep + 1));
        if (key == "Version" || key == "candidate_version") {
            status.candidateVersion = val;
        } else if (key == "Source" || key == "Origin" || key == "package_source") {
            status.packageSource = val;
        } else if (key == "Summary" || key == "Description" || key == "changelog_summary") {
            status.changelogSummary = val;
        } else if (key == "Manager" || key == "manager") {
            if (val == "dnf" || val == "rpm") {
                status.manager = PackageManagerKind::Dnf;
            } else if (val == "apt" || val == "deb") {
                status.manager = PackageManagerKind::Apt;
            }
        }
    }
    return !status.candidateVersion.empty();
}

bool isPathLocked(const char *path) {
    std::string_view pathView(path);
    if (pathView.ends_with(".pid")) {
        std::ifstream pidFile(path);
        pid_t pid = 0;
        if (pidFile >> pid && pid > 0) {
            if (::kill(pid, 0) == 0 || errno == EPERM) {
                return true;
            }
        }
    }

    int fd = ::open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fd = ::open(path, O_RDONLY | O_CLOEXEC);
    }
    if (fd >= 0) {
        struct flock fl{};
        fl.l_type = F_WRLCK;
        fl.l_whence = SEEK_SET;
        fl.l_start = 0;
        fl.l_len = 0;
        const int ret = ::fcntl(fd, F_GETLK, &fl);
        ::close(fd);
        if (ret == 0 && fl.l_type != F_UNLCK) {
            return true;
        }
    }

    struct stat st{};
    if (::stat(path, &st) == 0) {
        const auto targetDev = st.st_dev;
        const auto targetIno = st.st_ino;
        const unsigned int maj = major(targetDev);
        const unsigned int min = minor(targetDev);

        std::ifstream procLocks("/proc/locks");
        if (procLocks.is_open()) {
            std::string line;
            while (std::getline(procLocks, line)) {
                std::istringstream iss(line);
                std::string token;
                int col = 0;
                while (iss >> token) {
                    if (col == 5) {
                        const auto colon1 = token.find(':');
                        const auto colon2 = token.rfind(':');
                        if (colon1 != std::string::npos && colon2 != std::string::npos &&
                            colon1 != colon2) {
                            try {
                                const auto lMaj = static_cast<unsigned int>(
                                    std::stoul(token.substr(0, colon1), nullptr, 16));
                                const auto lMin = static_cast<unsigned int>(
                                    std::stoul(token.substr(colon1 + 1, colon2 - colon1 - 1),
                                               nullptr, 16));
                                const unsigned long lIno =
                                    std::stoul(token.substr(colon2 + 1));
                                if (lMaj == maj && lMin == min &&
                                    lIno == static_cast<unsigned long>(targetIno)) {
                                    return true;
                                }
                            } catch (...) {
                            }
                        }
                        break;
                    }
                    ++col;
                }
            }
        }
    }
    return false;
}

void finalizeStatus(UpdateStatus &status, bool hasRemoteRepoIndex) {
    status.checked = true;
    if (status.packageManagerLocked) {
        status.statusMessage =
            "系统包管理器正忙（检测到锁占用，可能有自动更新或包管理操作正在运行），请稍后重试。";
    }
    if (status.manager == PackageManagerKind::Dnf) {
        status.refreshCacheCommand = "sudo dnf makecache";
        status.upgradeCommand = "sudo dnf makecache && sudo dnf upgrade modernime";
        status.privilegedCommand = "pkexec dnf upgrade -y modernime";
    } else {
        if (status.manager == PackageManagerKind::Unknown) {
            status.manager = PackageManagerKind::Apt;
        }
        status.refreshCacheCommand = "sudo apt update";
        status.upgradeCommand = "sudo apt update && sudo apt install --only-upgrade modernime";
        status.privilegedCommand = "pkexec apt-get install --only-upgrade -y modernime";
    }

    if (status.installChannel == "script") {
        const char *targetCh = (status.manager == PackageManagerKind::Dnf) ? "rpm" : "deb";
        const char *pkgCmd = (status.manager == PackageManagerKind::Dnf)
                                 ? "sudo dnf makecache && sudo dnf install -y modernime"
                                 : "sudo apt update && sudo apt install -y modernime";
        status.upgradeCommand =
            std::string("modernime-migrate-channel --to-channel ") + targetCh +
            " --package-cmd \"" + pkgCmd + "\"";
    }

    if (status.candidateVersion.empty() || status.candidateVersion == "(none)" ||
        !hasRemoteRepoIndex) {
        status.offlineOrUnreachable = true;
        status.cacheMayBeStale = false;
        status.updateAvailable = false;
        status.candidateVersion = "暂不可用（离线或未配置软件源）";
        if (status.packageSource.empty()) {
            status.packageSource = "未检测到已配置的 APT / DNF 软件源索引";
        }
        if (status.changelogSummary.empty()) {
            status.changelogSummary =
                "离线、未配置软件源或尚未刷新本地缓存，本地功能与词库完好无损";
        }
        status.statusMessage =
            "当前处于离线状态、尚未配置 ModernIME 系统软件源或未刷新缓存（可先执行 " +
            status.refreshCacheCommand +
            " 或点击“刷新软件源缓存”）；本地输入法与数据运行正常（未发现软件损坏）。";
        return;
    }

    status.offlineOrUnreachable = false;
    const int cmp = UpdateController::compareVersions(status.candidateVersion,
                                                      status.currentVersion);
    if (cmp > 0) {
        status.updateAvailable = true;
        status.cacheMayBeStale = false;
        if (status.changelogSummary.empty()) {
            status.changelogSummary =
                "包含候选排序优化、词库更新与 SQLite/LMDB 事务安全升级保护";
        }
        status.statusMessage =
            "发现新版本 " + status.candidateVersion + "（当前版本 " +
            status.currentVersion +
            "）。实际安装与升级必须通过系统包管理器（apt / dnf）执行。";
    } else {
        status.updateAvailable = false;
        status.cacheMayBeStale = true;
        if (status.changelogSummary.empty()) {
            status.changelogSummary =
                "本地软件源缓存中当前安装的 ModernIME 已是最新版本";
        }
        status.statusMessage =
            "本地软件源缓存中当前已是最新版本（" + status.currentVersion +
            "）。提示：包管理器默认读取本地缓存索引，请先点击“刷新软件源缓存”或在终端执行 " +
            status.refreshCacheCommand + " 同步远端最新元数据后再检查。";
    }
}

} // namespace

std::string UpdateController::detectInstallChannel(const Environment &environment) {
    return ::modernime::settings::detectInstallChannel(environment);
}

int UpdateController::compareVersions(std::string_view lhsRaw, std::string_view rhsRaw) {
    const std::string lhsStorage = trim(lhsRaw);
    const std::string rhsStorage = trim(rhsRaw);
    std::string_view lhs = lhsStorage;
    std::string_view rhs = rhsStorage;
    if (lhs.starts_with('v') || lhs.starts_with('V')) {
        lhs.remove_prefix(1);
    }
    if (rhs.starts_with('v') || rhs.starts_with('V')) {
        rhs.remove_prefix(1);
    }

    auto splitEpoch = [](std::string_view s) -> std::pair<std::string_view, std::string_view> {
        const auto pos = s.find(':');
        if (pos == std::string_view::npos) {
            return {"0", s};
        }
        return {s.substr(0, pos), s.substr(pos + 1)};
    };
    auto splitRev = [](std::string_view s) -> std::pair<std::string_view, std::string_view> {
        const auto pos = s.rfind('-');
        if (pos == std::string_view::npos) {
            return {s, "0"};
        }
        return {s.substr(0, pos), s.substr(pos + 1)};
    };

    const auto [epochA, restA] = splitEpoch(lhs);
    const auto [epochB, restB] = splitEpoch(rhs);
    if (const int c = compareDebianSegment(epochA, epochB); c != 0) {
        return c;
    }
    const auto [upA, revA] = splitRev(restA);
    const auto [upB, revB] = splitRev(restB);
    if (const int c = compareDebianSegment(upA, upB); c != 0) {
        return c;
    }
    return compareDebianSegment(revA, revB);
}

bool UpdateController::isPackageManagerLocked() {
    const char *mockLock = std::getenv("MODERNIME_PACKAGE_MANAGER_LOCK_FILE");
    if (mockLock != nullptr && *mockLock != '\0') {
        std::error_code ec;
        if (std::filesystem::exists(mockLock, ec) && isPathLocked(mockLock)) {
            return true;
        }
    }
    const char *forceLocked = std::getenv("MODERNIME_MOCK_PACKAGE_MANAGER_LOCKED");
    if (forceLocked != nullptr && std::string_view(forceLocked) == "1") {
        return true;
    }
    for (const char *lockPath : {
             "/var/lib/dpkg/lock-frontend",
             "/var/lib/dpkg/lock",
             "/var/lib/apt/lists/lock",
             "/var/cache/apt/archives/lock",
             "/var/lib/rpm/.rpm.lock",
             "/var/cache/dnf/metadata_lock.pid",
         }) {
        std::error_code ec;
        if (std::filesystem::exists(lockPath, ec) && isPathLocked(lockPath)) {
            return true;
        }
    }
    return false;
}

UpdateStatus UpdateController::checkForUpdates(const Environment &environment) {
    UpdateStatus status;
    status.packageManagerLocked = isPackageManagerLocked();
    const auto currentVerEnv = lookupEnv(environment, "MODERNIME_CURRENT_VERSION");
    status.currentVersion = currentVerEnv.empty() ? MODERNIME_VERSION : currentVerEnv;
    status.installChannel = detectInstallChannel(environment);

    // 1. Check trusted local Release metadata file if configured
    const auto metadataFile = lookupEnv(environment, "MODERNIME_UPDATE_METADATA_FILE");
    if (!metadataFile.empty()) {
        if (parseTrustedMetadataFile(metadataFile, status)) {
            if (!status.installChannel.empty() && status.installChannel != "unknown") {
                status.packageSource += " (channel=" + status.installChannel + ")";
            }
            finalizeStatus(status, true);
            return status;
        }
    }

    // 2. Probe APT via apt-cache policy & apt-cache show
    const auto aptCache = findExecutableInPath("apt-cache", environment);
    const auto dnfBin = findExecutableInPath("dnf", environment);
    const bool rpmChannel = status.installChannel == "rpm";
    const bool debChannel = status.installChannel == "deb";
    if (rpmChannel) status.manager = PackageManagerKind::Dnf;
    if (debChannel) status.manager = PackageManagerKind::Apt;
    if (!aptCache.empty() && !rpmChannel) {
        status.manager = PackageManagerKind::Apt;
        bool hasRemoteRepoIndex = false;
        const auto policyOut =
            runSubprocess({aptCache.string(), "policy", "modernime"}, environment);
        if (policyOut.started && policyOut.exitedZero && !policyOut.stdoutText.empty()) {
            std::istringstream iss(policyOut.stdoutText);
            std::string line;
            while (std::getline(iss, line)) {
                const auto trimmed = trim(line);
                if (trimmed.starts_with("Candidate:")) {
                    status.candidateVersion =
                        trim(std::string_view(trimmed).substr(10));
                } else if (!trimmed.empty() &&
                           std::isdigit(static_cast<unsigned char>(trimmed[0]))) {
                    const auto spacePos = trimmed.find(' ');
                    if (spacePos != std::string::npos) {
                        const auto rest = trim(std::string_view(trimmed).substr(spacePos + 1));
                        // Version lines look like "0.1.8 700"; origin lines look like "700 https://... noble/main amd64 Packages"
                        if (rest.find("://") != std::string::npos ||
                            rest.find("Packages") != std::string::npos) {
                            if (rest.find("/dpkg/status") == std::string::npos) {
                                hasRemoteRepoIndex = true;
                                if (status.packageSource.empty()) {
                                    status.packageSource = rest;
                                }
                            }
                        }
                    }
                }
            }
        }
        if (hasRemoteRepoIndex && !status.candidateVersion.empty() &&
            status.candidateVersion != "(none)") {
            const auto showOut =
                runSubprocess({aptCache.string(), "show", "modernime"}, environment);
            if (showOut.started && showOut.exitedZero && !showOut.stdoutText.empty()) {
                std::istringstream iss(showOut.stdoutText);
                std::string line;
                while (std::getline(iss, line)) {
                    const auto trimmed = trim(line);
                    if (trimmed.starts_with("Description:") ||
                        trimmed.starts_with("Description-en:")) {
                        const auto colon = trimmed.find(':');
                        status.changelogSummary =
                            trim(std::string_view(trimmed).substr(colon + 1));
                        break;
                    }
                }
            }
        }
        if (hasRemoteRepoIndex) {
            if (status.packageSource.empty()) {
                status.packageSource = "APT 软件源 (channel=" + status.installChannel + ")";
            } else {
                status.packageSource += " (channel=" + status.installChannel + ")";
            }
        }
        finalizeStatus(status, hasRemoteRepoIndex);
        return status;
    }

    // 3. Probe DNF on openEuler / Fedora / RHEL
    if (!dnfBin.empty() && !debChannel) {
        status.manager = PackageManagerKind::Dnf;
        bool hasRemoteRepoIndex = false;
        const auto infoOut = runSubprocess(
            {dnfBin.string(), "--cacheonly", "-q", "info", "modernime"}, environment);
        // Cache refresh is a separate explicit action. A failed cache query
        // must not start an implicit network request or parse partial output.
        const auto &text = infoOut.stdoutText;
        if (infoOut.started && infoOut.exitedZero && !text.empty()) {
            std::istringstream iss(text);
            std::string line;
            enum class DnfSection { None, Installed, Available };
            DnfSection currentSection = DnfSection::None;

            std::string installedVer;
            std::string installedRel;
            std::string installedFromRepo;
            std::string installedSummary;

            std::string pendingAvailVer;
            std::string pendingAvailRel;
            std::string pendingAvailRepo;
            std::string pendingAvailSummary;

            std::string bestAvailCandidate;
            std::string bestAvailRepo;
            std::string bestAvailSummary;

            auto isRemoteRepo = [](std::string_view r) {
                return !r.empty() && r != "@System" && r != "installed" &&
                       r != "@commandline" && r != "cmdline" &&
                       !r.starts_with("@System") && !r.starts_with("/");
            };

            auto commitAvailableCandidate = [&]() {
                if (pendingAvailVer.empty() || pendingAvailRepo.empty()) {
                    pendingAvailVer.clear();
                    pendingAvailRel.clear();
                    pendingAvailRepo.clear();
                    pendingAvailSummary.clear();
                    return;
                }
                const std::string cand = pendingAvailRel.empty()
                                             ? pendingAvailVer
                                             : (pendingAvailVer + "-" + pendingAvailRel);
                if (bestAvailCandidate.empty() ||
                    UpdateController::compareVersions(cand, bestAvailCandidate) > 0) {
                    bestAvailCandidate = cand;
                    bestAvailRepo = pendingAvailRepo;
                    if (!pendingAvailSummary.empty()) {
                        bestAvailSummary = pendingAvailSummary;
                    }
                }
                pendingAvailVer.clear();
                pendingAvailRel.clear();
                pendingAvailRepo.clear();
                pendingAvailSummary.clear();
            };

            while (std::getline(iss, line)) {
                const auto trimmed = trim(line);
                if (trimmed == "Installed Packages") {
                    commitAvailableCandidate();
                    currentSection = DnfSection::Installed;
                    continue;
                }
                if (trimmed == "Available Packages") {
                    commitAvailableCandidate();
                    currentSection = DnfSection::Available;
                    continue;
                }

                const auto colon = line.find(':');
                if (colon == std::string::npos) {
                    continue;
                }
                const auto key = trim(std::string_view(line).substr(0, colon));
                const auto val = trim(std::string_view(line).substr(colon + 1));

                if (key == "Name" && currentSection == DnfSection::Available) {
                    commitAvailableCandidate();
                }

                if (currentSection == DnfSection::Available || currentSection == DnfSection::None) {
                    if (key == "Version") {
                        pendingAvailVer = val;
                    } else if (key == "Release") {
                        pendingAvailRel = val;
                    } else if (key == "Repository") {
                        if (isRemoteRepo(val)) {
                            pendingAvailRepo = val;
                        }
                    } else if (key == "Summary" && pendingAvailSummary.empty()) {
                        pendingAvailSummary = val;
                    }
                }
                if (currentSection == DnfSection::Installed || currentSection == DnfSection::None) {
                    if (key == "Version") {
                        installedVer = val;
                    } else if (key == "Release") {
                        installedRel = val;
                    } else if (key == "From repo") {
                        if (isRemoteRepo(val)) {
                            installedFromRepo = val;
                        }
                    } else if (key == "Summary" && installedSummary.empty()) {
                        installedSummary = val;
                    }
                }
            }
            commitAvailableCandidate();

            if (!bestAvailCandidate.empty()) {
                hasRemoteRepoIndex = true;
                status.candidateVersion = bestAvailCandidate;
                status.packageSource = "DNF 仓库 " + bestAvailRepo;
                if (!bestAvailSummary.empty()) {
                    status.changelogSummary = bestAvailSummary;
                }
            } else if (!installedFromRepo.empty()) {
                hasRemoteRepoIndex = true;
                status.candidateVersion = status.currentVersion;
                status.packageSource = "DNF 仓库 " + installedFromRepo;
                if (!installedSummary.empty()) {
                    status.changelogSummary = installedSummary;
                }
            }
        }
        if (hasRemoteRepoIndex) {
            if (status.packageSource.empty()) {
                status.packageSource = "DNF 软件源 (channel=" + status.installChannel + ")";
            } else {
                status.packageSource += " (channel=" + status.installChannel + ")";
            }
        }
        finalizeStatus(status, hasRemoteRepoIndex);
        return status;
    }

    finalizeStatus(status, false);
    return status;
}

RuntimeResult UpdateController::refreshPackageCache(const UpdateStatus &status,
                                                    const Environment &environment) {
    auto opId = tryBeginPackageOperation(PackageOperationKind::RefreshCache);
    if (!opId.has_value()) {
        if (hasActivePackageOperation()) {
            return {false,
                    "检测到后台仍有未完成的系统包管理器事务正在运行，请等待其结束后再试。"};
        }
        return {false,
                "包操作结果队列已满（最多保留8项未消费结果），请先消费已有结果。"};
    }
    if (isPackageManagerLocked()) {
        RuntimeResult res{false,
                "系统包管理器正忙（检测到锁占用，可能有其他包管理进程或自动更新正在运行），请等待其结束后重试。"};
        finishPackageOperation(*opId, res);
        return res;
    }

    PackageManagerKind manager = status.manager;
    if (manager == PackageManagerKind::Unknown) {
        if (!findExecutableInPath("apt-get", environment).empty() ||
            !findExecutableInPath("apt-cache", environment).empty()) {
            manager = PackageManagerKind::Apt;
        } else if (!findExecutableInPath("dnf", environment).empty()) {
            manager = PackageManagerKind::Dnf;
        }
    }

    const std::string fallbackCmd =
        (manager == PackageManagerKind::Dnf) ? "sudo dnf makecache" : "sudo apt update";

    const auto pkexecBin = findExecutableInPath("pkexec", environment);
    if (pkexecBin.empty()) {
        RuntimeResult res{false,
                "未检测到系统认证组件 pkexec，请在终端执行 " + fallbackCmd +
                    " 刷新软件源缓存后再点击“检查更新”。"};
        finishPackageOperation(*opId, res, {}, {}, false);
        return res;
    }

    std::vector<std::string> argv;
    if (manager == PackageManagerKind::Dnf) {
        const auto dnfBin = findExecutableInPath("dnf", environment);
        if (dnfBin.empty()) {
            RuntimeResult res{false, "未找到 dnf 包管理器，请手动执行：" + fallbackCmd};
            finishPackageOperation(*opId, res, {}, {}, false);
            return res;
        }
        argv = {pkexecBin.string(), dnfBin.string(), "makecache"};
    } else {
        const auto aptGetBin = findExecutableInPath("apt-get", environment);
        if (aptGetBin.empty()) {
            RuntimeResult res{false, "未找到 apt-get 包管理器，请手动执行：" + fallbackCmd};
            finishPackageOperation(*opId, res, {}, {}, false);
            return res;
        }
        argv = {pkexecBin.string(), aptGetBin.string(), "update"};
    }

    const guint opTimeoutMs = resolvePackageOperationTimeoutMs(environment);
    const auto out = runSubprocess(argv, environment, opTimeoutMs, false,
                                   opId, PackageOperationKind::RefreshCache, fallbackCmd);
    if (!out.started || !out.exitedZero) {
        if (out.timedOut) {
            const std::string procHint =
                (manager == PackageManagerKind::Dnf) ? "ps aux | grep -E 'dnf|rpm'"
                                                     : "ps aux | grep -E 'apt|dpkg'";
            if (out.stillRunningOnTimeout) {
                return {false,
                        "刷新软件源缓存等待超时（检测到包管理器子进程仍在运行或等待锁，为保护系统状态未强制终止进程；后台将继续读取输出并跟踪该进程直至结束，期间已锁定重复操作）。可在终端通过 " +
                            procHint + " 检查进程状态，或待其完成后执行：" + fallbackCmd};
            }
            RuntimeResult res{false,
                    "刷新软件源缓存操作超时（子进程已退出，未检测到后台仍在运行的包管理器进程），请在终端手动执行：" +
                        fallbackCmd};
            finishPackageOperation(*opId, res, out.stdoutText, out.stderrText, false);
            return res;
        }
        if (currentPackageOperationPhase() == PackageOperationPhase::Uncertain) {
            return {false,
                    "刷新软件源缓存子进程异常中断（状态无法确定），为避免损坏系统包管理器未自动释放操作锁。请在终端检查进程状态后手动恢复并执行：" +
                        fallbackCmd};
        }
        RuntimeResult res{false, packageOperationFailureMessage(out, "刷新软件源缓存", fallbackCmd)};
        finishPackageOperation(*opId, res, out.stdoutText, out.stderrText, false);
        return res;
    }
    RuntimeResult res{true, "已刷新系统软件源缓存，正在重新检查可用版本…"};
    finishPackageOperation(*opId, res, out.stdoutText, out.stderrText, false);
    return res;
}

RuntimeResult UpdateController::runSystemUpgrade(const UpdateStatus &status,
                                                 const Environment &environment) {
    auto opId = tryBeginPackageOperation(PackageOperationKind::SystemUpgrade);
    if (!opId.has_value()) {
        if (hasActivePackageOperation()) {
            return {false,
                    "检测到后台仍有未完成的系统包管理器事务正在运行，请等待其结束后再试。"};
        }
        return {false,
                "包操作结果队列已满（最多保留8项未消费结果），请先消费已有结果。"};
    }
    if (isPackageManagerLocked()) {
        RuntimeResult res{false,
                "系统包管理器正忙（检测到锁占用，可能有其他包管理进程或自动更新正在运行），请等待其结束后重试。"};
        finishPackageOperation(*opId, res, {}, {}, false);
        return res;
    }
    if (!status.updateAvailable) {
        RuntimeResult res{false, "当前没有可安装的更新版本。"};
        finishPackageOperation(*opId, res, {}, {}, false);
        return res;
    }
    if (status.installChannel == "script") {
        RuntimeResult res{
            false,
            "当前为脚本安装渠道 (channel=script)，请复制升级命令在终端运行 modernime-migrate-channel 迁移至系统软件源。"};
        finishPackageOperation(*opId, res, {}, {}, false);
        return res;
    }

    const auto pkexecBin = findExecutableInPath("pkexec", environment);
    if (pkexecBin.empty()) {
        RuntimeResult res{false,
                "未检测到系统认证组件 pkexec，请复制升级命令在终端执行：" +
                    status.upgradeCommand};
        finishPackageOperation(*opId, res, {}, {}, false);
        return res;
    }

    std::vector<std::string> argv;
    if (status.manager == PackageManagerKind::Dnf) {
        const auto dnfBin = findExecutableInPath("dnf", environment);
        if (dnfBin.empty()) {
            RuntimeResult res{false, "未找到 dnf 包管理器，请手动执行：" + status.upgradeCommand};
            finishPackageOperation(*opId, res, {}, {}, false);
            return res;
        }
        argv = {pkexecBin.string(), dnfBin.string(), "upgrade", "-y", "modernime"};
    } else {
        const auto aptGetBin = findExecutableInPath("apt-get", environment);
        if (aptGetBin.empty()) {
            RuntimeResult res{false,
                    "未找到 apt-get 包管理器，请手动执行：" + status.upgradeCommand};
            finishPackageOperation(*opId, res, {}, {}, false);
            return res;
        }
        argv = {pkexecBin.string(), aptGetBin.string(), "install",
                "--only-upgrade", "-y", "modernime"};
    }

    const guint opTimeoutMs = resolvePackageOperationTimeoutMs(environment);
    const auto out =
        runSubprocess(argv, environment, opTimeoutMs, false,
                      opId, PackageOperationKind::SystemUpgrade, status.upgradeCommand);
    if (!out.started || !out.exitedZero) {
        if (out.timedOut) {
            if (status.manager == PackageManagerKind::Dnf) {
                if (out.stillRunningOnTimeout) {
                    return {false,
                            "系统包管理器升级等待超时（检测到 dnf/rpm 子进程仍在运行或等待锁，为避免中断升级事务未强制终止进程；后台将继续读取输出并跟踪该进程直至结束，期间已锁定重复升级触发）。请在终端通过 ps aux | grep -E 'dnf|rpm' 检查进度；待后台事务完成后再重新检查，或手动执行：" +
                                status.upgradeCommand};
                }
                RuntimeResult res{false,
                        "系统包管理器升级操作超时（子进程已退出，未检测到后台仍在运行的 dnf/rpm 进程）。请在终端检查状态后手动执行：" +
                            status.upgradeCommand};
                finishPackageOperation(*opId, res, out.stdoutText, out.stderrText, false);
                return res;
            }
            if (out.stillRunningOnTimeout) {
                return {false,
                        "系统包管理器升级等待超时（检测到 apt/dpkg 子进程仍在运行或等待锁，为避免中断升级事务未强制终止进程；后台将继续读取输出并跟踪该进程直至结束，期间已锁定重复升级触发）。请勿强行终止，可在终端通过 ps aux | grep -E 'apt|dpkg' 查看进度；若事务异常中断，请执行 sudo dpkg --configure -a && sudo apt-get install -f 恢复后再运行：" +
                            status.upgradeCommand};
            }
            RuntimeResult res{false,
                    "系统包管理器升级操作超时（子进程已退出，未检测到后台仍在运行的 apt/dpkg 进程）。若此前包状态未完成配置，请先执行 sudo dpkg --configure -a && sudo apt-get install -f 恢复，随后在终端运行：" +
                        status.upgradeCommand};
            finishPackageOperation(*opId, res, out.stdoutText, out.stderrText, false);
            return res;
        }
        if (currentPackageOperationPhase() == PackageOperationPhase::Uncertain) {
            return {false,
                    "系统包管理器升级子进程异常中断（状态无法确定），为保护系统未自动释放操作锁。请在终端检查包状态（如 dpkg --audit 等）后手动恢复。"};
        }
        RuntimeResult res{false, packageOperationFailureMessage(out, "系统包管理器升级",
                                                     status.upgradeCommand)};
        finishPackageOperation(*opId, res, out.stdoutText, out.stderrText, false);
        return res;
    }
    RuntimeResult res{true,
            "已通过系统包管理器完成 ModernIME 升级，请点击“重新加载 ModernIME”使其生效。"};
    finishPackageOperation(*opId, res, out.stdoutText, out.stderrText, false);
    return res;
}

bool UpdateController::hasActivePackageOperation() {
    return modernime::settings::hasActivePackageOperation();
}

bool UpdateController::waitForActivePackageOperation(unsigned int timeoutMs) {
    return modernime::settings::waitForActivePackageOperation(timeoutMs);
}

std::optional<RuntimeResult> UpdateController::takeCompletedPackageOperationResult(
    std::optional<std::uint64_t> id) {
    return modernime::settings::takeCompletedPackageOperationResult(id);
}

std::string UpdateController::lastCompletedPackageOperationStdout() {
    return modernime::settings::lastCompletedPackageOperationStdout();
}

std::optional<std::uint64_t> UpdateController::tryBeginPackageOperation(PackageOperationKind kind) {
    return modernime::settings::tryBeginPackageOperation(kind);
}

bool UpdateController::advancePackageOperation(std::uint64_t id,
                                               PackageOperationPhase expected,
                                               PackageOperationPhase next) {
    return modernime::settings::advancePackageOperation(id, expected, next);
}

bool UpdateController::finishPackageOperation(std::uint64_t id,
                                              const RuntimeResult &result,
                                              std::string stdoutText,
                                              std::string stderrText,
                                              bool recordUnconsumed) {
    return modernime::settings::finishPackageOperation(
        id, result, std::move(stdoutText), std::move(stderrText), recordUnconsumed);
}

PackageOperationPhase UpdateController::currentPackageOperationPhase() {
    return modernime::settings::currentPackageOperationPhase();
}

std::optional<std::uint64_t> UpdateController::currentPackageOperationId() {
    return modernime::settings::currentPackageOperationId();
}

std::size_t UpdateController::unconsumedPackageOperationCount() {
    return modernime::settings::unconsumedPackageOperationCount();
}

void UpdateController::resetPackageOperationStateForTesting() {
    modernime::settings::resetPackageOperationStateForTesting();
}

} // namespace modernime::settings
