#include "modernime/settings/update_controller.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <pthread.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

std::filesystem::path makeTempDir() {
    std::string tmpl = "/tmp/modernime-update-test-XXXXXX";
    char *created = ::mkdtemp(tmpl.data());
    assert(created != nullptr);
    return std::filesystem::path(created);
}

void writeExecutable(const std::filesystem::path &path, const std::string &content) {
    std::ofstream out(path);
    assert(out.good());
    out << content;
    out.close();
    assert(::chmod(path.c_str(), 0755) == 0);
}

void writeTextFile(const std::filesystem::path &path, const std::string &content) {
    std::ofstream out(path);
    assert(out.good());
    out << content;
    out.close();
}

} // namespace

int main(int argc, char **argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--mock-zombie-exit") {
        std::thread([]() { ::usleep(200000); }).detach();
        ::pthread_exit(nullptr);
    }

    using namespace modernime::settings;

    // 1. Version comparison (Debian/RPM semantics)
    assert(UpdateController::compareVersions("0.1.9", "0.1.8") > 0);
    assert(UpdateController::compareVersions("0.1.8", "0.1.9") < 0);
    assert(UpdateController::compareVersions("v0.1.8", "0.1.8") == 0);
    assert(UpdateController::compareVersions("0.1.8-2", "0.1.8-1") > 0);
    assert(UpdateController::compareVersions("0.1.8", "0.1.8~rc1") > 0);
    assert(UpdateController::compareVersions("0.1.10", "0.1.9") > 0);
    assert(UpdateController::compareVersions("1:0.1.0", "0.9.9") > 0);

    const auto tempRoot = makeTempDir();
    const auto aptBinDir = tempRoot / "apt-bin";
    const auto dnfBinDir = tempRoot / "dnf-bin";
    const auto offlineBinDir = tempRoot / "offline-bin";
    std::filesystem::create_directories(aptBinDir);
    std::filesystem::create_directories(dnfBinDir);
    std::filesystem::create_directories(offlineBinDir);

    const auto debChannelFile = tempRoot / "channel-deb.txt";
    writeTextFile(debChannelFile, "channel=deb\npackage_version=0.1.8\n");

    const auto rpmChannelFile = tempRoot / "channel-rpm.txt";
    writeTextFile(rpmChannelFile, "channel=rpm\npackage_version=0.1.8\n");

    const auto scriptChannelFile = tempRoot / "channel-script.txt";
    writeTextFile(scriptChannelFile, "channel=script\npackage_version=0.1.8\n");

    // 2. Fake APT check with available update
    writeExecutable(
        aptBinDir / "apt-cache",
        "#!/bin/sh\n"
        "if [ \"$1\" = \"policy\" ]; then\n"
        "  cat <<'EOF'\n"
        "modernime:\n"
        "  Installed: 0.1.8\n"
        "  Candidate: 0.1.9-1\n"
        "  Version table:\n"
        "     0.1.9-1 700\n"
        "        700 https://packages.modernime.org/apt noble/main amd64 Packages\n"
        " *** 0.1.8 100\n"
        "        100 /var/lib/dpkg/status\n"
        "EOF\n"
        "  exit 0\n"
        "fi\n"
        "if [ \"$1\" = \"show\" ]; then\n"
        "  cat <<'EOF'\n"
        "Package: modernime\n"
        "Version: 0.1.9-1\n"
        "Description: ModernIME 0.1.9 adds signed APT/DNF repo upgrade and WAL protection\n"
        "EOF\n"
        "  exit 0\n"
        "fi\n"
        "exit 1\n");

    const Environment aptEnv{
        {"PATH", aptBinDir.string()},
        {"MODERNIME_CURRENT_VERSION", "0.1.8"},
        {"MODERNIME_INSTALL_CHANNEL_FILE", debChannelFile.string()},
    };
    const auto aptStatus = UpdateController::checkForUpdates(aptEnv);
    assert(aptStatus.checked);
    assert(!aptStatus.offlineOrUnreachable);
    assert(aptStatus.updateAvailable);
    assert(aptStatus.manager == PackageManagerKind::Apt);
    assert(aptStatus.currentVersion == "0.1.8");
    assert(aptStatus.candidateVersion == "0.1.9-1");
    assert(aptStatus.packageSource.find("https://packages.modernime.org/apt") != std::string::npos);
    assert(aptStatus.packageSource.find("channel=deb") != std::string::npos);
    assert(aptStatus.changelogSummary.find("ModernIME 0.1.9") != std::string::npos);
    assert(aptStatus.upgradeCommand ==
           "sudo apt update && sudo apt install --only-upgrade modernime");

    // 3. Fake DNF check with available update
    writeExecutable(
        dnfBinDir / "dnf",
        "#!/bin/sh\n"
        "cat <<'EOF'\n"
        "Available Packages\n"
        "Name         : modernime\n"
        "Version      : 0.1.9\n"
        "Release      : 1.oe2409\n"
        "Architecture : x86_64\n"
        "Repository   : modernime-openeuler\n"
        "Summary      : ModernIME openEuler signed RPM upgrade\n"
        "EOF\n"
        "exit 0\n");

    const Environment dnfEnv{
        {"PATH", dnfBinDir.string()},
        {"MODERNIME_CURRENT_VERSION", "0.1.8"},
        {"MODERNIME_INSTALL_CHANNEL_FILE", rpmChannelFile.string()},
    };
    const auto dnfStatus = UpdateController::checkForUpdates(dnfEnv);
    assert(dnfStatus.checked);
    assert(!dnfStatus.offlineOrUnreachable);
    assert(!dnfStatus.cacheMayBeStale);
    assert(dnfStatus.updateAvailable);
    assert(dnfStatus.manager == PackageManagerKind::Dnf);
    assert(dnfStatus.candidateVersion == "0.1.9-1.oe2409");
    assert(dnfStatus.packageSource.find("modernime-openeuler") != std::string::npos);
    assert(dnfStatus.packageSource.find("channel=rpm") != std::string::npos);
    assert(dnfStatus.refreshCacheCommand == "sudo dnf makecache");
    assert(dnfStatus.upgradeCommand == "sudo dnf makecache && sudo dnf upgrade modernime");

    // 4. Script channel directs user to modernime-migrate-channel and refuses direct pkexec
    const Environment scriptEnv{
        {"PATH", aptBinDir.string()},
        {"MODERNIME_CURRENT_VERSION", "0.1.8"},
        {"MODERNIME_INSTALL_CHANNEL_FILE", scriptChannelFile.string()},
    };
    const auto scriptStatus = UpdateController::checkForUpdates(scriptEnv);
    assert(scriptStatus.updateAvailable);
    assert(scriptStatus.upgradeCommand.find("modernime-migrate-channel --to-channel deb") !=
           std::string::npos);
    const auto scriptUpgradeRes = UpdateController::runSystemUpgrade(scriptStatus, scriptEnv);
    assert(!scriptUpgradeRes.success);
    assert(scriptUpgradeRes.message.find("modernime-migrate-channel") != std::string::npos);

    // 5a. Offline / unreachable repository (Candidate: (none)) distinguishes from software corruption
    writeExecutable(
        offlineBinDir / "apt-cache",
        "#!/bin/sh\n"
        "cat <<'EOF'\n"
        "modernime:\n"
        "  Installed: 0.1.8\n"
        "  Candidate: (none)\n"
        "  Version table:\n"
        "EOF\n"
        "exit 0\n");

    const Environment offlineEnv{
        {"PATH", offlineBinDir.string()},
        {"MODERNIME_CURRENT_VERSION", "0.1.8"},
        {"MODERNIME_INSTALL_CHANNEL_FILE", debChannelFile.string()},
    };
    const auto offlineStatus = UpdateController::checkForUpdates(offlineEnv);
    assert(offlineStatus.checked);
    assert(offlineStatus.offlineOrUnreachable);
    assert(!offlineStatus.cacheMayBeStale);
    assert(!offlineStatus.updateAvailable);
    assert(offlineStatus.statusMessage.find("未发现软件损坏") != std::string::npos);

    // 5b. Locally installed via dpkg -i ONLY (100 /var/lib/dpkg/status, no remote APT repo index)
    // Must report offlineOrUnreachable=true instead of falsely claiming "已是最新版本"
    const auto dpkgOnlyBinDir = tempRoot / "dpkg-only-bin";
    std::filesystem::create_directories(dpkgOnlyBinDir);
    writeExecutable(
        dpkgOnlyBinDir / "apt-cache",
        "#!/bin/sh\n"
        "cat <<'EOF'\n"
        "modernime:\n"
        "  Installed: 0.1.8\n"
        "  Candidate: 0.1.8\n"
        "  Version table:\n"
        " *** 0.1.8 100\n"
        "        100 /var/lib/dpkg/status\n"
        "EOF\n"
        "exit 0\n");
    const Environment dpkgOnlyEnv{
        {"PATH", dpkgOnlyBinDir.string()},
        {"MODERNIME_CURRENT_VERSION", "0.1.8"},
        {"MODERNIME_INSTALL_CHANNEL_FILE", debChannelFile.string()},
    };
    const auto dpkgOnlyStatus = UpdateController::checkForUpdates(dpkgOnlyEnv);
    assert(dpkgOnlyStatus.checked);
    assert(dpkgOnlyStatus.offlineOrUnreachable);
    assert(!dpkgOnlyStatus.cacheMayBeStale);
    assert(!dpkgOnlyStatus.updateAvailable);
    assert(dpkgOnlyStatus.statusMessage.find("sudo apt update") != std::string::npos);

    // 5c. Up-to-date in local APT repository cache (Candidate == Installed with remote repo entry)
    // Must set cacheMayBeStale=true and prompt user to refresh cache (sudo apt update)
    const auto cachedUpToDateBinDir = tempRoot / "cached-uptodate-bin";
    std::filesystem::create_directories(cachedUpToDateBinDir);
    writeExecutable(
        cachedUpToDateBinDir / "apt-cache",
        "#!/bin/sh\n"
        "cat <<'EOF'\n"
        "modernime:\n"
        "  Installed: 0.1.8\n"
        "  Candidate: 0.1.8\n"
        "  Version table:\n"
        " *** 0.1.8 700\n"
        "        700 https://sunshine134679.github.io/fcitx5-linux-pinyin-input-method/apt noble/main amd64 Packages\n"
        "        100 /var/lib/dpkg/status\n"
        "EOF\n"
        "exit 0\n");
    const Environment cachedUpToDateEnv{
        {"PATH", cachedUpToDateBinDir.string()},
        {"MODERNIME_CURRENT_VERSION", "0.1.8"},
        {"MODERNIME_INSTALL_CHANNEL_FILE", debChannelFile.string()},
    };
    const auto cachedUpToDateStatus = UpdateController::checkForUpdates(cachedUpToDateEnv);
    assert(cachedUpToDateStatus.checked);
    assert(!cachedUpToDateStatus.offlineOrUnreachable);
    assert(!cachedUpToDateStatus.updateAvailable);
    assert(cachedUpToDateStatus.cacheMayBeStale);
    assert(cachedUpToDateStatus.statusMessage.find("本地软件源缓存") != std::string::npos);
    assert(cachedUpToDateStatus.statusMessage.find("sudo apt update") != std::string::npos);

    // 6. System cache refresh & upgrade delegation via pkexec (never downloads or overwrites in-process)
    const auto pkexecLog = tempRoot / "pkexec-invoked.log";
    writeExecutable(aptBinDir / "apt-get", "#!/bin/sh\nexit 0\n");
    writeExecutable(
        aptBinDir / "pkexec",
        "#!/bin/sh\n"
        "printf '%s\\n' \"$*\" > \"" +
            pkexecLog.string() +
            "\"\n"
            "exit 0\n");

    const auto refreshRes = UpdateController::refreshPackageCache(aptStatus, aptEnv);
    assert(refreshRes.success);
    {
        std::ifstream refreshLogIn(pkexecLog);
        std::string refreshLoggedArgs;
        std::getline(refreshLogIn, refreshLoggedArgs);
        assert(refreshLoggedArgs.find("apt-get update") != std::string::npos);
    }

    const auto upgradeRes = UpdateController::runSystemUpgrade(aptStatus, aptEnv);
    assert(upgradeRes.success);
    std::ifstream logIn(pkexecLog);
    std::string loggedArgs;
    std::getline(logIn, loggedArgs);
    assert(loggedArgs.find("apt-get install --only-upgrade -y modernime") != std::string::npos);

    // 7a. Package operation timeout (stillRunningOnTimeout == true):
    // - Must NOT force-kill active dpkg/apt transaction or cancel pipe communication
    // - Must keep draining stdout/stderr in the background tracker until completion
    // - Must block duplicate upgrade/refresh triggers while background transaction is still running
    // - Must record final completion result once child exits
    const auto completedMarker = tempRoot / "pkg-op-completed-gracefully.txt";
    writeExecutable(
        aptBinDir / "pkexec",
        "#!/bin/sh\n"
        "printf 'PRE_TIMEOUT_LINE\\n'\n"
        "sleep 0.25\n"
        "printf 'POST_TIMEOUT_UPGRADE_OUTPUT_OK\\n'\n"
        "printf 'completed\\n' > \"" +
            completedMarker.string() +
            "\"\n"
            "exit 0\n");

    const Environment timeoutEnv{
        {"PATH", aptBinDir.string()},
        {"MODERNIME_CURRENT_VERSION", "0.1.8"},
        {"MODERNIME_INSTALL_CHANNEL_FILE", debChannelFile.string()},
        {"MODERNIME_PACKAGE_OP_TIMEOUT_MS", "60"},
    };
    const auto timeoutRes = UpdateController::runSystemUpgrade(aptStatus, timeoutEnv);
    assert(!timeoutRes.success);
    assert(timeoutRes.message.find("检测到 apt/dpkg 子进程仍在运行或等待锁") != std::string::npos);
    assert(timeoutRes.message.find("未强制终止进程") != std::string::npos);
    assert(timeoutRes.message.find("期间已锁定重复升级触发") != std::string::npos);
    assert(timeoutRes.message.find("dpkg --configure -a") != std::string::npos);
    assert(UpdateController::hasActivePackageOperation());

    // Duplicate upgrade or cache refresh MUST be rejected while background transaction is active
    const auto dupUpgradeRes = UpdateController::runSystemUpgrade(aptStatus, timeoutEnv);
    assert(!dupUpgradeRes.success);
    assert(dupUpgradeRes.message.find("未完成的系统包管理器事务正在运行") != std::string::npos);
    const auto dupRefreshRes = UpdateController::refreshPackageCache(aptStatus, timeoutEnv);
    assert(!dupRefreshRes.success);
    assert(dupRefreshRes.message.find("未完成的系统包管理器事务正在运行") != std::string::npos);

    // Wait for background tracker to finish draining output and reaping the child process
    assert(UpdateController::waitForActivePackageOperation(2000));
    assert(!UpdateController::hasActivePackageOperation());
    assert(std::filesystem::exists(completedMarker));
    const auto capturedOut = UpdateController::lastCompletedPackageOperationStdout();
    assert(capturedOut.find("PRE_TIMEOUT_LINE") != std::string::npos);
    assert(capturedOut.find("POST_TIMEOUT_UPGRADE_OUTPUT_OK") != std::string::npos);
    const auto bgDoneRes = UpdateController::takeCompletedPackageOperationResult();
    assert(bgDoneRes.has_value());
    assert(bgDoneRes->success);
    assert(bgDoneRes->message.find("后台系统包管理器升级已完成") != std::string::npos);

    // 7b. Package operation timeout when child main task is already in zombie 'Z' state (stillRunningOnTimeout == false):
    const auto selfExe = std::filesystem::canonical("/proc/self/exe");
    writeExecutable(
        aptBinDir / "pkexec",
        "#!/bin/sh\n"
        "exec \"" +
            selfExe.string() +
            "\" --mock-zombie-exit\n");
    const auto exitedTimeoutRes = UpdateController::runSystemUpgrade(aptStatus, timeoutEnv);
    assert(!exitedTimeoutRes.success);
    assert(exitedTimeoutRes.message.find("子进程已退出，未检测到后台仍在运行的 apt/dpkg 进程") !=
           std::string::npos);
    assert(!UpdateController::hasActivePackageOperation());

    // 7c. Process exit while background package operation is still active:
    // - MUST NOT hang in static destruction ([P1] Round 4)
    // - Surviving package manager child writing to stdout and stderr AFTER the settings client
    //   process has exited and closed all its file descriptors MUST NOT receive SIGPIPE/EPIPE
    //   and MUST complete its transaction cleanly ([P1] Round 5)
    const auto pidFile = tempRoot / "bg-pkexec.pid";
    const auto clientExitedMarker = tempRoot / "settings-client-exited.txt";
    const auto postExitDoneMarker = tempRoot / "bg-pkexec-post-exit-done.txt";
    writeExecutable(
        aptBinDir / "pkexec",
        "#!/bin/sh\n"
        "set -eu\n"
        "printf '%s\\n' \"$$\" > \"" +
            pidFile.string() +
            "\"\n"
            "printf 'PRE_CLIENT_EXIT_STDOUT\\n'\n"
            "printf 'PRE_CLIENT_EXIT_STDERR\\n' >&2\n"
            "i=0\n"
            "while [ ! -f \"" +
            clientExitedMarker.string() +
            "\" ] && [ \"$i\" -lt 200 ]; do\n"
            "  sleep 0.02\n"
            "  i=$((i + 1))\n"
            "done\n"
            "if [ ! -f \"" +
            clientExitedMarker.string() +
            "\" ]; then\n"
            "  exit 2\n"
            "fi\n"
            "printf 'POST_CLIENT_EXIT_STDOUT_LINE_1\\n'\n"
            "printf 'POST_CLIENT_EXIT_STDERR_LINE_1\\n' >&2\n"
            "printf 'POST_CLIENT_EXIT_STDOUT_LINE_2\\n'\n"
            "printf 'POST_CLIENT_EXIT_STDERR_LINE_2\\n' >&2\n"
            "printf 'completed-after-client-exit\\n' > \"" +
            postExitDoneMarker.string() +
            "\"\n"
            "exit 0\n");

    const pid_t childPid = ::fork();
    assert(childPid >= 0);
    if (childPid == 0) {
        const auto res = UpdateController::runSystemUpgrade(aptStatus, timeoutEnv);
        assert(!res.success);
        assert(UpdateController::hasActivePackageOperation());
        // Full C++ std::exit(0) runs static destructors and closes all FDs in settings client
        std::exit(0);
    }

    int waitStatus = 0;
    bool childExitedFast = false;
    for (int i = 0; i < 40; ++i) {
        const pid_t waited = ::waitpid(childPid, &waitStatus, WNOHANG);
        if (waited == childPid) {
            childExitedFast = WIFEXITED(waitStatus) && (WEXITSTATUS(waitStatus) == 0);
            break;
        }
        ::usleep(25000); // 40 * 25ms = 1.0s max
    }
    if (!childExitedFast) {
        ::kill(childPid, SIGKILL);
        ::waitpid(childPid, &waitStatus, 0);
    }
    assert(childExitedFast);

    // Signal the surviving background pkexec process that the settings client process has
    // completely terminated and all of its file descriptors have been closed by the kernel.
    writeTextFile(clientExitedMarker, "client-exited\n");

    bool bgCompletedAfterClientExit = false;
    for (int i = 0; i < 60; ++i) {
        if (std::filesystem::exists(postExitDoneMarker)) {
            bgCompletedAfterClientExit = true;
            break;
        }
        ::usleep(25000); // 60 * 25ms = 1.5s max
    }
    if (!bgCompletedAfterClientExit && std::filesystem::exists(pidFile)) {
        std::ifstream pidIn(pidFile);
        long bgPid = 0;
        if (pidIn >> bgPid && bgPid > 0) {
            ::kill(static_cast<pid_t>(bgPid), SIGTERM);
        }
    }
    assert(bgCompletedAfterClientExit);

    // 7d. File descriptor stability and FD_CLOEXEC verification across repeated successful
    // and failed subprocess launches ([P2] Round 6):
    // 1) Verify inside the spawned child that no extra modernime-subproc-* FDs above fd 2
    //    are inherited across execve (FD_CLOEXEC set on both parent and childStdoutFd/childStderrFd).
    // 2) Verify synchronously that zero modernime-subproc-* FDs remain in /proc/self/fd after
    //    each phase, and that total /proc/self/fd count stays strictly equal to baselineFdCount
    //    (accounting for GLib's glib_worker thread asynchronously closing its internal pidfd).
    auto countOpenFds = []() -> std::size_t {
        std::size_t count = 0;
        std::error_code ec;
        for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd", ec)) {
            (void)entry;
            ++count;
        }
        return count;
    };

    auto countSubprocTempFds = []() -> std::size_t {
        std::size_t count = 0;
        std::error_code ec;
        for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd", ec)) {
            std::error_code linkEc;
            const auto target = std::filesystem::read_symlink(entry.path(), linkEc);
            if (!linkEc && target.string().find("modernime-subproc-") != std::string::npos) {
                ++count;
            }
        }
        return count;
    };

    auto waitForFdCount = [&](std::size_t expected) -> bool {
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (countOpenFds() == expected) {
                return true;
            }
            ::usleep(2000);
        }
        return countOpenFds() == expected;
    };

    const auto childFdAuditFile = tempRoot / "child-fd-audit.txt";
    writeExecutable(
        aptBinDir / "pkexec",
        "#!/bin/sh\n"
        "leaked=0\n"
        "for fdpath in /proc/$$/fd/*; do\n"
        "  fdnum=$(basename \"$fdpath\")\n"
        "  if [ \"$fdnum\" -gt 2 ]; then\n"
        "    target=$(readlink \"$fdpath\" 2>/dev/null || true)\n"
        "    case \"$target\" in\n"
        "      *modernime-subproc-*) leaked=$((leaked + 1)) ;;\n"
        "    esac\n"
        "  fi\n"
        "done\n"
        "printf '%s\\n' \"$leaked\" > \"" +
            childFdAuditFile.string() +
            "\"\n"
            "exit 0\n");

    // Warm up once so any one-time GLib thread/context initialization FDs are settled
    const auto warmupRes = UpdateController::runSystemUpgrade(aptStatus, aptEnv);
    assert(warmupRes.success);
    assert(countSubprocTempFds() == 0);
    {
        std::ifstream auditIn(childFdAuditFile);
        int leakedChildFds = -1;
        auditIn >> leakedChildFds;
        assert(leakedChildFds == 0);
    }

    // Let GLib's glib_worker thread finish closing the warmup child's pidfd before recording baseline
    ::usleep(10000);
    const std::size_t baselineFdCount = countOpenFds();
    for (int i = 0; i < 15; ++i) {
        // Successful query + upgrade
        const auto st = UpdateController::checkForUpdates(aptEnv);
        assert(st.checked);
        const auto okUp = UpdateController::runSystemUpgrade(aptStatus, aptEnv);
        assert(okUp.success);
    }
    assert(countSubprocTempFds() == 0);
    assert(waitForFdCount(baselineFdCount));

    // Non-zero child exit path
    writeExecutable(aptBinDir / "pkexec", "#!/bin/sh\nexit 1\n");
    for (int i = 0; i < 15; ++i) {
        const auto failUp = UpdateController::runSystemUpgrade(aptStatus, aptEnv);
        assert(!failUp.success);
    }
    assert(countSubprocTempFds() == 0);
    assert(waitForFdCount(baselineFdCount));

    // g_subprocess_launcher_spawnv() failure path (executable exists with 0755, but invalid shebang interpreter)
    writeExecutable(
        aptBinDir / "pkexec",
        "#!/nonexistent/modernime_invalid_interpreter_for_spawnv_test\n");
    for (int i = 0; i < 15; ++i) {
        const auto spawnFailRes = UpdateController::runSystemUpgrade(aptStatus, aptEnv);
        assert(!spawnFailRes.success);
        const auto spawnFailRefresh = UpdateController::refreshPackageCache(aptStatus, aptEnv);
        assert(!spawnFailRefresh.success);
    }
    assert(countSubprocTempFds() == 0);
    assert(waitForFdCount(baselineFdCount));

    std::filesystem::remove_all(tempRoot);
    return 0;
}
