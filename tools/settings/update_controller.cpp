#include "modernime/settings/update_controller.h"

#include <gio/gio.h>
#include <glib.h>

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/types.h>
#include <vector>

namespace modernime::settings {
namespace {

#ifndef MODERNIME_VERSION
#define MODERNIME_VERSION "0.1.9"
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
    std::string stdoutText;
    std::string stderrText;
};

struct SubprocessState final {
    GMainLoop *loop = nullptr;
    GCancellable *cancellable = nullptr;
    GSubprocess *proc = nullptr;
    bool timedOut = false;
    bool stillRunningOnTimeout = false;
    bool finished = false;
    std::string out;
    std::string err;
};

std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
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
    return ::kill(static_cast<pid_t>(pidVal), 0) == 0;
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

void onCommunicateDone(GObject *source, GAsyncResult *res, gpointer userData) {
    auto *state = static_cast<SubprocessState *>(userData);
    gchar *stdoutBuf = nullptr;
    gchar *stderrBuf = nullptr;
    GError *error = nullptr;
    state->finished = g_subprocess_communicate_utf8_finish(
        G_SUBPROCESS(source), res, &stdoutBuf, &stderrBuf, &error);
    if (stdoutBuf != nullptr) {
        state->out = stdoutBuf;
        g_free(stdoutBuf);
    }
    if (stderrBuf != nullptr) {
        state->err = stderrBuf;
        g_free(stderrBuf);
    }
    g_clear_error(&error);
    g_main_loop_quit(state->loop);
}

gboolean onCommandTimeout(gpointer userData) {
    auto *state = static_cast<SubprocessState *>(userData);
    state->timedOut = true;
    state->stillRunningOnTimeout = isSubprocessAlive(state->proc);
    g_cancellable_cancel(state->cancellable);
    return G_SOURCE_REMOVE;
}

CommandOutput runSubprocess(const std::vector<std::string> &argv,
                            const Environment &environment,
                            guint timeoutMs = kQueryTimeoutMs,
                            bool forceKillOnTimeout = true) {
    CommandOutput result;
    if (argv.empty()) {
        return result;
    }

    GSubprocessLauncher *launcher = g_subprocess_launcher_new(
        static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                      G_SUBPROCESS_FLAGS_STDERR_PIPE));
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
    g_object_unref(launcher);
    if (proc == nullptr) {
        g_clear_error(&error);
        return result;
    }

    result.started = true;
    GMainContext *context = g_main_context_new();
    g_main_context_push_thread_default(context);
    GMainLoop *loop = g_main_loop_new(context, FALSE);
    GCancellable *cancellable = g_cancellable_new();

    SubprocessState state{loop, cancellable, proc, false, false, false, {}, {}};
    GSource *timer = g_timeout_source_new(timeoutMs);
    g_source_set_callback(timer, onCommandTimeout, &state, nullptr);
    g_source_attach(timer, context);

    g_subprocess_communicate_utf8_async(proc, nullptr, cancellable,
                                        onCommunicateDone, &state);
    g_main_loop_run(loop);

    if (state.timedOut) {
        result.timedOut = true;
        result.stillRunningOnTimeout =
            state.stillRunningOnTimeout || isSubprocessAlive(proc);
        if (forceKillOnTimeout && result.stillRunningOnTimeout) {
            g_subprocess_force_exit(proc);
        }
    }

    result.stdoutText = std::move(state.out);
    result.stderrText = std::move(state.err);
    if (state.finished && !state.timedOut && g_subprocess_get_if_exited(proc)) {
        result.exitedZero = (g_subprocess_get_exit_status(proc) == 0);
    }

    g_source_destroy(timer);
    g_source_unref(timer);
    g_object_unref(cancellable);
    g_main_loop_unref(loop);
    g_main_context_pop_thread_default(context);
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

void finalizeStatus(UpdateStatus &status, bool hasRemoteRepoIndex) {
    status.checked = true;
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

UpdateStatus UpdateController::checkForUpdates(const Environment &environment) {
    UpdateStatus status;
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
    if (!aptCache.empty()) {
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
    const auto dnfBin = findExecutableInPath("dnf", environment);
    if (!dnfBin.empty()) {
        status.manager = PackageManagerKind::Dnf;
        bool hasRemoteRepoIndex = false;
        const auto infoOut = runSubprocess(
            {dnfBin.string(), "--cacheonly", "-q", "info", "modernime"}, environment);
        const auto &text = !infoOut.stdoutText.empty()
                               ? infoOut.stdoutText
                               : runSubprocess({dnfBin.string(), "-q", "info", "modernime"},
                                               environment)
                                     .stdoutText;
        if (!text.empty()) {
            std::istringstream iss(text);
            std::string line;
            std::string ver;
            std::string rel;
            std::string repoName;
            while (std::getline(iss, line)) {
                const auto colon = line.find(':');
                if (colon == std::string::npos) {
                    continue;
                }
                const auto key = trim(std::string_view(line).substr(0, colon));
                const auto val = trim(std::string_view(line).substr(colon + 1));
                if (key == "Version") {
                    ver = val;
                } else if (key == "Release") {
                    rel = val;
                } else if (key == "Repository" || key == "From repo") {
                    repoName = val;
                    if (val != "@System" && val != "installed") {
                        hasRemoteRepoIndex = true;
                        status.packageSource = "DNF 仓库 " + val;
                    }
                } else if (key == "Summary" && status.changelogSummary.empty()) {
                    status.changelogSummary = val;
                }
            }
            if (!ver.empty() && hasRemoteRepoIndex) {
                status.candidateVersion = rel.empty() ? ver : (ver + "-" + rel);
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
        return {false,
                "未检测到系统认证组件 pkexec，请在终端执行 " + fallbackCmd +
                    " 刷新软件源缓存后再点击“检查更新”。"};
    }

    std::vector<std::string> argv;
    if (manager == PackageManagerKind::Dnf) {
        const auto dnfBin = findExecutableInPath("dnf", environment);
        if (dnfBin.empty()) {
            return {false, "未找到 dnf 包管理器，请手动执行：" + fallbackCmd};
        }
        argv = {pkexecBin.string(), dnfBin.string(), "makecache"};
    } else {
        const auto aptGetBin = findExecutableInPath("apt-get", environment);
        if (aptGetBin.empty()) {
            return {false, "未找到 apt-get 包管理器，请手动执行：" + fallbackCmd};
        }
        argv = {pkexecBin.string(), aptGetBin.string(), "update"};
    }

    const guint opTimeoutMs = resolvePackageOperationTimeoutMs(environment);
    const auto out = runSubprocess(argv, environment, opTimeoutMs, false);
    if (!out.started || !out.exitedZero) {
        if (out.timedOut) {
            const std::string procHint =
                (manager == PackageManagerKind::Dnf) ? "ps aux | grep -E 'dnf|rpm'"
                                                     : "ps aux | grep -E 'apt|dpkg'";
            if (out.stillRunningOnTimeout) {
                return {false,
                        "刷新软件源缓存等待超时（检测到包管理器子进程仍在运行或等待锁，为保护系统状态未强制终止进程）。请稍候待后台任务结束，或在终端通过 " +
                            procHint + " 检查进程状态后执行：" + fallbackCmd};
            }
            return {false,
                    "刷新软件源缓存操作超时，请在终端手动执行：" +
                        fallbackCmd};
        }
        return {false,
                "刷新软件源缓存未完成（可能已取消授权或网络不可达），您也可以在终端执行：" +
                    fallbackCmd};
    }
    return {true, "已刷新系统软件源缓存，正在重新检查可用版本…"};
}

RuntimeResult UpdateController::runSystemUpgrade(const UpdateStatus &status,
                                                 const Environment &environment) {
    if (!status.updateAvailable) {
        return {false, "当前没有可安装的更新版本。"};
    }
    if (status.installChannel == "script") {
        return {
            false,
            "当前为脚本安装渠道 (channel=script)，请复制升级命令在终端运行 modernime-migrate-channel 迁移至系统软件源。"};
    }

    const auto pkexecBin = findExecutableInPath("pkexec", environment);
    if (pkexecBin.empty()) {
        return {false,
                "未检测到系统认证组件 pkexec，请复制升级命令在终端执行：" +
                    status.upgradeCommand};
    }

    std::vector<std::string> argv;
    if (status.manager == PackageManagerKind::Dnf) {
        const auto dnfBin = findExecutableInPath("dnf", environment);
        if (dnfBin.empty()) {
            return {false, "未找到 dnf 包管理器，请手动执行：" + status.upgradeCommand};
        }
        argv = {pkexecBin.string(), dnfBin.string(), "upgrade", "-y", "modernime"};
    } else {
        const auto aptGetBin = findExecutableInPath("apt-get", environment);
        if (aptGetBin.empty()) {
            return {false,
                    "未找到 apt-get 包管理器，请手动执行：" + status.upgradeCommand};
        }
        argv = {pkexecBin.string(), aptGetBin.string(), "install",
                "--only-upgrade", "-y", "modernime"};
    }

    const guint opTimeoutMs = resolvePackageOperationTimeoutMs(environment);
    const auto out = runSubprocess(argv, environment, opTimeoutMs, false);
    if (!out.started || !out.exitedZero) {
        if (out.timedOut) {
            if (status.manager == PackageManagerKind::Dnf) {
                return {false,
                        "系统包管理器升级等待超时（检测到 dnf/rpm 子进程仍在运行或等待锁，为避免中断升级事务未强制终止进程）。请在终端通过 ps aux | grep -E 'dnf|rpm' 检查进度；待后台事务完成后再重新检查，或手动执行：" +
                            status.upgradeCommand};
            }
            return {false,
                    "系统包管理器升级等待超时（检测到 apt/dpkg 子进程仍在运行或等待锁，为避免中断升级事务未强制终止进程）。请勿强行终止，可在终端通过 ps aux | grep -E 'apt|dpkg' 查看进度；若事务异常中断，请执行 sudo dpkg --configure -a && sudo apt-get install -f 恢复后再运行：" +
                        status.upgradeCommand};
        }
        return {false,
                "系统包管理器升级未完成（可能已取消授权或网络受限），您也可以在终端执行：" +
                    status.upgradeCommand};
    }
    return {true,
            "已通过系统包管理器完成 ModernIME 升级，请点击“重新加载 ModernIME”使其生效。"};
}

} // namespace modernime::settings
