#include "modernime/settings/update_controller.h"
#include "modernime/settings/detail/package_policy.h"
#include "modernime/settings/detail/process_runner.h"

#include <glib.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <memory>
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

void startBackgroundPackageTracking(std::uint64_t operationId,
                                    PackageOperationKind opKind,
                                    std::string fallbackCmd) {
    std::thread([operationId, opKind, fallbackCmd = std::move(fallbackCmd)]() {
        detail::CommandResult out;
        while (true) {
            out = detail::waitForOperation(operationId, 300000);
            if (out.state == detail::CommandState::Continuing) {
                continue;
            }
            break;
        }
        if (out.state == detail::CommandState::Exited) {
            const bool exitedZero = (out.exitStatus == 0);
            RuntimeResult finalRes;
            finalRes.success = exitedZero;
            if (opKind == PackageOperationKind::SystemUpgrade) {
                finalRes.message =
                    exitedZero
                        ? "后台系统包管理器升级已完成，请点击“重新加载 ModernIME”使其生效。"
                        : ("后台系统包管理器升级未成功完成，请在终端检查状态或执行：" + fallbackCmd);
            } else {
                finalRes.message =
                    exitedZero
                        ? "后台软件源缓存刷新已完成，可点击“检查更新”同步最新候选版本。"
                        : ("后台软件源缓存刷新未成功完成，请在终端手动执行：" + fallbackCmd);
            }
            finishPackageOperation(operationId, finalRes, out.stdoutText, out.stderrText, true);
        } else {
            advancePackageOperation(operationId,
                                    PackageOperationPhase::Continuing,
                                    PackageOperationPhase::Uncertain);
        }
    }).detach();
}

} // namespace

std::string UpdateController::detectInstallChannel(const Environment &environment) {
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
        std::stringstream buffer;
        buffer << in.rdbuf();
        const std::string content = buffer.str();
        const std::string channel = detail::detectInstallChannelFromContent(content);
        if (channel != "unknown") {
            return channel;
        }
    }
    return "unknown";
}

int UpdateController::compareVersions(std::string_view lhsRaw, std::string_view rhsRaw) {
    return detail::compareVersions(lhsRaw, rhsRaw);
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
        std::ifstream in(metadataFile);
        if (in) {
            std::stringstream buffer;
            buffer << in.rdbuf();
            if (detail::parseTrustedMetadata(buffer.str(), status)) {
                if (!status.installChannel.empty() && status.installChannel != "unknown") {
                    status.packageSource += " (channel=" + status.installChannel + ")";
                }
                detail::evaluateUpdateDecision(status, false, false);
                return status;
            }
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
        detail::CommandSpec policySpec;
        policySpec.mode = detail::CommandMode::Query;
        policySpec.timeoutMs = kQueryTimeoutMs;
        policySpec.argv = {aptCache.string(), "policy", "modernime"};
        policySpec.environment = environment;
        policySpec.limits.perStreamBufferBytes = 1048576;
        policySpec.limits.perStreamResultBytes = 1048576;
        policySpec.limits.perStreamFileBytes = 1048576;

        const auto policyOut = detail::runCommand(policySpec);
        const bool queryTruncated = policyOut.stdoutTruncated || policyOut.stderrTruncated;
        const bool queryFailed = (policyOut.state != detail::CommandState::Exited || policyOut.exitStatus != 0);

        if (queryTruncated) {
            detail::evaluateUpdateDecision(status, true, false);
            return status;
        }

        const auto policyRes = detail::parseAptPolicyOutput(policyOut.stdoutText, queryTruncated);
        if (policyRes.hasCandidate) {
            status.candidateVersion = policyRes.candidateVersion;
        }
        if (policyRes.hasRemoteRepo) {
            status.packageSource = policyRes.packageSource;
            if (!status.candidateVersion.empty() && status.candidateVersion != "(none)") {
                detail::CommandSpec showSpec;
                showSpec.mode = detail::CommandMode::Query;
                showSpec.timeoutMs = kQueryTimeoutMs;
                showSpec.argv = {aptCache.string(), "show", "modernime"};
                showSpec.environment = environment;
                showSpec.limits.perStreamBufferBytes = 1048576;
                showSpec.limits.perStreamResultBytes = 1048576;
                showSpec.limits.perStreamFileBytes = 1048576;

                const auto showOut = detail::runCommand(showSpec);
                if (!showOut.stdoutTruncated && showOut.exitStatus == 0) {
                    status.changelogSummary = detail::parseAptShowDescription(showOut.stdoutText, false);
                }
            }
            if (status.packageSource.empty()) {
                status.packageSource = "APT 软件源 (channel=" + status.installChannel + ")";
            } else {
                status.packageSource += " (channel=" + status.installChannel + ")";
            }
        }

        detail::evaluateUpdateDecision(status, false, queryFailed || !policyRes.hasRemoteRepo);
        return status;
    }

    // 3. Probe DNF on openEuler / Fedora / RHEL
    if (!dnfBin.empty() && !debChannel) {
        status.manager = PackageManagerKind::Dnf;
        detail::CommandSpec infoSpec;
        infoSpec.mode = detail::CommandMode::Query;
        infoSpec.timeoutMs = kQueryTimeoutMs;
        infoSpec.argv = {dnfBin.string(), "--cacheonly", "-q", "info", "modernime"};
        infoSpec.environment = environment;
        infoSpec.limits.perStreamBufferBytes = 1048576;
        infoSpec.limits.perStreamResultBytes = 1048576;
        infoSpec.limits.perStreamFileBytes = 1048576;

        const auto infoOut = detail::runCommand(infoSpec);
        const bool queryTruncated = infoOut.stdoutTruncated || infoOut.stderrTruncated;
        const bool queryFailed = (infoOut.state != detail::CommandState::Exited || infoOut.exitStatus != 0);

        if (queryTruncated) {
            detail::evaluateUpdateDecision(status, true, false);
            return status;
        }

        const auto dnfRes = detail::parseDnfInfoOutput(infoOut.stdoutText, queryTruncated);
        if (dnfRes.hasRemoteRepo) {
            status.candidateVersion = dnfRes.isInstalledOnly ? status.currentVersion : dnfRes.candidateVersion;
            status.packageSource = dnfRes.packageSource;
            status.changelogSummary = dnfRes.summary;
            if (status.packageSource.empty()) {
                status.packageSource = "DNF 软件源 (channel=" + status.installChannel + ")";
            } else {
                status.packageSource += " (channel=" + status.installChannel + ")";
            }
        }

        detail::evaluateUpdateDecision(status, false, queryFailed || !dnfRes.hasRemoteRepo);
        return status;
    }

    detail::evaluateUpdateDecision(status, false, true);
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
        finishPackageOperation(*opId, res, {}, {}, false);
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

    const std::string fallbackCmd = detail::buildFallbackCommand(manager, PackageOperationKind::RefreshCache);

    const auto pkexecBin = findExecutableInPath("pkexec", environment);
    if (pkexecBin.empty()) {
        RuntimeResult res{false,
                "未检测到系统认证组件 pkexec，请在终端执行 " + fallbackCmd +
                    " 刷新软件源缓存后再点击“检查更新”。"};
        finishPackageOperation(*opId, res, {}, {}, false);
        return res;
    }

    std::filesystem::path managerBin;
    if (manager == PackageManagerKind::Dnf) {
        managerBin = findExecutableInPath("dnf", environment);
        if (managerBin.empty()) {
            RuntimeResult res{false, "未找到 dnf 包管理器，请手动执行：" + fallbackCmd};
            finishPackageOperation(*opId, res, {}, {}, false);
            return res;
        }
    } else {
        managerBin = findExecutableInPath("apt-get", environment);
        if (managerBin.empty()) {
            RuntimeResult res{false, "未找到 apt-get 包管理器，请手动执行：" + fallbackCmd};
            finishPackageOperation(*opId, res, {}, {}, false);
            return res;
        }
    }

    const std::vector<std::string> argv =
        detail::buildRefreshCacheArgv(manager, pkexecBin.string(), managerBin.string());

    const guint opTimeoutMs = resolvePackageOperationTimeoutMs(environment);

    detail::CommandSpec spec;
    spec.mode = detail::CommandMode::PackageTransaction;
    spec.operationId = *opId;
    spec.timeoutMs = opTimeoutMs;
    spec.argv = argv;
    spec.environment = environment;
    spec.limits.perStreamBufferBytes = 1048576;
    spec.limits.perStreamResultBytes = 65536;
    spec.limits.perStreamFileBytes = 1048576;

    advancePackageOperation(*opId, PackageOperationPhase::Starting, PackageOperationPhase::Running);

    const auto out = detail::runCommand(spec);

    if (out.state == detail::CommandState::NotStarted) {
        RuntimeResult res{false, detail::formatFailureMessage(out, "刷新软件源缓存", fallbackCmd)};
        finishPackageOperation(*opId, res, out.stdoutText, out.stderrText, false);
        return res;
    }

    if (out.timedOut && out.state == detail::CommandState::Exited) {
        RuntimeResult res{false,
                "刷新软件源缓存操作超时（子进程已退出，未检测到后台仍在运行的包管理器进程），请在终端手动执行：" +
                    fallbackCmd};
        finishPackageOperation(*opId, res, out.stdoutText, out.stderrText, false);
        return res;
    }

    if (out.state == detail::CommandState::Continuing) {
        advancePackageOperation(*opId, PackageOperationPhase::Running, PackageOperationPhase::Continuing);
        startBackgroundPackageTracking(*opId, PackageOperationKind::RefreshCache, fallbackCmd);
        const std::string procHint = (manager == PackageManagerKind::Dnf) ? "ps aux | grep -E 'dnf|rpm'"
                                                                         : "ps aux | grep -E 'apt|dpkg'";
        return {false,
                "刷新软件源缓存等待超时（检测到包管理器子进程仍在运行或等待锁，为保护系统状态未强制终止进程；后台将继续读取输出并跟踪该进程直至结束，期间已锁定重复操作）。可在终端通过 " +
                    procHint + " 检查进程状态，或待其完成后执行：" + fallbackCmd};
    }

    if (out.state == detail::CommandState::Unknown) {
        advancePackageOperation(*opId, PackageOperationPhase::Running, PackageOperationPhase::Uncertain);
        return {false,
                "刷新软件源缓存子进程异常中断（状态无法确定），为避免损坏系统包管理器未自动释放操作锁。请在终端检查进程状态后手动恢复并执行：" +
                    fallbackCmd};
    }

    if (out.exitStatus != 0) {
        RuntimeResult res{false, detail::formatFailureMessage(out, "刷新软件源缓存", fallbackCmd)};
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

    std::filesystem::path managerBin;
    if (status.manager == PackageManagerKind::Dnf) {
        managerBin = findExecutableInPath("dnf", environment);
        if (managerBin.empty()) {
            RuntimeResult res{false, "未找到 dnf 包管理器，请手动执行：" + status.upgradeCommand};
            finishPackageOperation(*opId, res, {}, {}, false);
            return res;
        }
    } else {
        managerBin = findExecutableInPath("apt-get", environment);
        if (managerBin.empty()) {
            RuntimeResult res{false,
                    "未找到 apt-get 包管理器，请手动执行：" + status.upgradeCommand};
            finishPackageOperation(*opId, res, {}, {}, false);
            return res;
        }
    }

    const std::vector<std::string> argv =
        detail::buildUpgradeArgv(status.manager, pkexecBin.string(), managerBin.string());

    const guint opTimeoutMs = resolvePackageOperationTimeoutMs(environment);

    detail::CommandSpec spec;
    spec.mode = detail::CommandMode::PackageTransaction;
    spec.operationId = *opId;
    spec.timeoutMs = opTimeoutMs;
    spec.argv = argv;
    spec.environment = environment;
    spec.limits.perStreamBufferBytes = 1048576;
    spec.limits.perStreamResultBytes = 65536;
    spec.limits.perStreamFileBytes = 1048576;

    advancePackageOperation(*opId, PackageOperationPhase::Starting, PackageOperationPhase::Running);

    const auto out = detail::runCommand(spec);

    if (out.state == detail::CommandState::NotStarted) {
        RuntimeResult res{false, detail::formatFailureMessage(out, "系统包管理器升级", status.upgradeCommand)};
        finishPackageOperation(*opId, res, out.stdoutText, out.stderrText, false);
        return res;
    }

    if (out.timedOut && out.state == detail::CommandState::Exited) {
        if (status.manager == PackageManagerKind::Dnf) {
            RuntimeResult res{false,
                    "系统包管理器升级操作超时（子进程已退出，未检测到后台仍在运行的 dnf/rpm 进程）。请在终端检查状态后手动执行：" +
                        status.upgradeCommand};
            finishPackageOperation(*opId, res, out.stdoutText, out.stderrText, false);
            return res;
        }
        RuntimeResult res{false,
                "系统包管理器升级操作超时（子进程已退出，未检测到后台仍在运行的 apt/dpkg 进程）。若此前包状态未完成配置，请先执行 sudo dpkg --configure -a && sudo apt-get install -f 恢复，随后在终端运行：" +
                    status.upgradeCommand};
        finishPackageOperation(*opId, res, out.stdoutText, out.stderrText, false);
        return res;
    }

    if (out.state == detail::CommandState::Continuing) {
        advancePackageOperation(*opId, PackageOperationPhase::Running, PackageOperationPhase::Continuing);
        startBackgroundPackageTracking(*opId, PackageOperationKind::SystemUpgrade, status.upgradeCommand);
        if (status.manager == PackageManagerKind::Dnf) {
            return {false,
                    "系统包管理器升级等待超时（检测到 dnf/rpm 子进程仍在运行或等待锁，为避免中断升级事务未强制终止进程；后台将继续读取输出并跟踪该进程直至结束，期间已锁定重复升级触发）。请在终端通过 ps aux | grep -E 'dnf|rpm' 检查进度；待后台事务完成后再重新检查，或手动执行：" +
                        status.upgradeCommand};
        }
        return {false,
                "系统包管理器升级等待超时（检测到 apt/dpkg 子进程仍在运行或等待锁，为避免中断升级事务未强制终止进程；后台将继续读取输出并跟踪该进程直至结束，期间已锁定重复升级触发）。请勿强行终止，可在终端通过 ps aux | grep -E 'apt|dpkg' 查看进度；若事务异常中断，请执行 sudo dpkg --configure -a && sudo apt-get install -f 恢复后再运行：" +
                    status.upgradeCommand};
    }

    if (out.state == detail::CommandState::Unknown) {
        advancePackageOperation(*opId, PackageOperationPhase::Running, PackageOperationPhase::Uncertain);
        return {false,
                "系统包管理器升级子进程异常中断（状态无法确定），为保护系统未自动释放操作锁。请在终端检查包状态（如 dpkg --audit 等）后手动恢复。"};
    }

    if (out.exitStatus != 0) {
        RuntimeResult res{false, detail::formatFailureMessage(out, "系统包管理器升级", status.upgradeCommand)};
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
