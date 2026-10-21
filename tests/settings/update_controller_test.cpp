#include "modernime/settings/update_controller.h"

#include <cassert>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <glib.h>
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

    // RPM channel takes precedence even if APT tools are also installed.
    const Environment mixedRpmEnv{
        {"PATH", aptBinDir.string() + ":" + dnfBinDir.string()},
        {"MODERNIME_CURRENT_VERSION", "0.1.8"},
        {"MODERNIME_INSTALL_CHANNEL_FILE", rpmChannelFile.string()},
    };
    const auto mixedRpmStatus = UpdateController::checkForUpdates(mixedRpmEnv);
    assert(mixedRpmStatus.manager == PackageManagerKind::Dnf);
    assert(mixedRpmStatus.candidateVersion == "0.1.9-1.oe2409");
    assert(mixedRpmStatus.upgradeCommand.find("dnf upgrade") != std::string::npos);

    // Checking updates never refreshes the remote DNF cache implicitly.
    const auto emptyDnfBinDir = tempRoot / "empty-dnf-bin";
    std::filesystem::create_directories(emptyDnfBinDir);
    const auto dnfCalls = tempRoot / "dnf-check-calls.log";
    writeExecutable(emptyDnfBinDir / "dnf", "#!/bin/sh\nprintf '%s\\n' \"$*\" >> \"" +
                    dnfCalls.string() + "\"\nexit 1\n");
    const Environment emptyDnfEnv{
        {"PATH", emptyDnfBinDir.string()},
        {"MODERNIME_CURRENT_VERSION", "0.1.8"},
        {"MODERNIME_INSTALL_CHANNEL_FILE", rpmChannelFile.string()},
    };
    const auto emptyDnfStatus = UpdateController::checkForUpdates(emptyDnfEnv);
    assert(emptyDnfStatus.offlineOrUnreachable);
    assert(!emptyDnfStatus.updateAvailable);
    std::ifstream calls(dnfCalls);
    std::string call;
    assert(static_cast<bool>(std::getline(calls, call)));
    assert(call.find("--cacheonly") != std::string::npos);
    assert(call.find("info") != std::string::npos);
    assert(call.find("modernime") != std::string::npos);
    assert(call.find("--available") == std::string::npos);
    assert(!std::getline(calls, call));

    // 3b. Up-to-date DNF repository package (Installed Packages with remote From repo, no Available Packages)
    // Must NOT be falsely marked offline; should report up-to-date with cacheMayBeStale=true.
    const auto upToDateDnfBinDir = tempRoot / "uptodate-dnf-bin";
    std::filesystem::create_directories(upToDateDnfBinDir);
    writeExecutable(
        upToDateDnfBinDir / "dnf",
        "#!/bin/sh\n"
        "cat <<'EOF'\n"
        "Installed Packages\n"
        "Name         : modernime\n"
        "Version      : 0.1.10\n"
        "Release      : 1.oe2409\n"
        "Architecture : x86_64\n"
        "Repository   : @System\n"
        "From repo    : modernime-openeuler\n"
        "Summary      : ModernIME openEuler signed RPM\n"
        "EOF\n"
        "exit 0\n");
    const Environment upToDateDnfEnv{
        {"PATH", upToDateDnfBinDir.string()},
        {"MODERNIME_CURRENT_VERSION", "0.1.10"},
        {"MODERNIME_INSTALL_CHANNEL_FILE", rpmChannelFile.string()},
    };
    const auto upToDateDnfStatus = UpdateController::checkForUpdates(upToDateDnfEnv);
    assert(upToDateDnfStatus.checked);
    assert(!upToDateDnfStatus.offlineOrUnreachable);
    assert(!upToDateDnfStatus.updateAvailable);
    assert(upToDateDnfStatus.cacheMayBeStale);
    assert(upToDateDnfStatus.candidateVersion == "0.1.10");
    assert(upToDateDnfStatus.packageSource.find("modernime-openeuler") != std::string::npos);
    assert(upToDateDnfStatus.packageSource.find("channel=rpm") != std::string::npos);
    assert(upToDateDnfStatus.statusMessage.find("本地软件源缓存中当前已是最新版本") != std::string::npos);
    assert(upToDateDnfStatus.statusMessage.find("sudo dnf makecache") != std::string::npos);

    // 3c. DNF output with both Installed Packages and a newer Available Packages section
    const auto multiSectionDnfBinDir = tempRoot / "multisection-dnf-bin";
    std::filesystem::create_directories(multiSectionDnfBinDir);
    writeExecutable(
        multiSectionDnfBinDir / "dnf",
        "#!/bin/sh\n"
        "cat <<'EOF'\n"
        "Installed Packages\n"
        "Name         : modernime\n"
        "Version      : 0.1.8\n"
        "Release      : 1.oe2409\n"
        "Architecture : x86_64\n"
        "Repository   : @System\n"
        "From repo    : modernime-openeuler\n"
        "Summary      : ModernIME installed package\n"
        "\n"
        "Available Packages\n"
        "Name         : modernime\n"
        "Version      : 0.1.10\n"
        "Release      : 1.oe2409\n"
        "Architecture : x86_64\n"
        "Repository   : modernime-openeuler\n"
        "Summary      : ModernIME new release\n"
        "EOF\n"
        "exit 0\n");
    const Environment multiSectionDnfEnv{
        {"PATH", multiSectionDnfBinDir.string()},
        {"MODERNIME_CURRENT_VERSION", "0.1.8"},
        {"MODERNIME_INSTALL_CHANNEL_FILE", rpmChannelFile.string()},
    };
    const auto multiSectionDnfStatus = UpdateController::checkForUpdates(multiSectionDnfEnv);
    assert(multiSectionDnfStatus.checked);
    assert(!multiSectionDnfStatus.offlineOrUnreachable);
    assert(multiSectionDnfStatus.updateAvailable);
    assert(multiSectionDnfStatus.candidateVersion == "0.1.10-1.oe2409");
    assert(multiSectionDnfStatus.packageSource.find("modernime-openeuler") != std::string::npos);
    assert(multiSectionDnfStatus.changelogSummary == "ModernIME new release");
    assert(multiSectionDnfStatus.statusMessage.find("发现新版本 0.1.10-1.oe2409") != std::string::npos);

    // 3d. Locally installed RPM without repository (From repo: @commandline)
    // Must be marked offlineOrUnreachable=true
    const auto localRpmDnfBinDir = tempRoot / "local-rpm-dnf-bin";
    std::filesystem::create_directories(localRpmDnfBinDir);
    writeExecutable(
        localRpmDnfBinDir / "dnf",
        "#!/bin/sh\n"
        "cat <<'EOF'\n"
        "Installed Packages\n"
        "Name         : modernime\n"
        "Version      : 0.1.10\n"
        "Release      : 1.oe2409\n"
        "Architecture : x86_64\n"
        "Repository   : @System\n"
        "From repo    : @commandline\n"
        "Summary      : ModernIME local RPM\n"
        "EOF\n"
        "exit 0\n");
    const Environment localRpmDnfEnv{
        {"PATH", localRpmDnfBinDir.string()},
        {"MODERNIME_CURRENT_VERSION", "0.1.10"},
        {"MODERNIME_INSTALL_CHANNEL_FILE", rpmChannelFile.string()},
    };
    const auto localRpmDnfStatus = UpdateController::checkForUpdates(localRpmDnfEnv);
    assert(localRpmDnfStatus.checked);
    assert(localRpmDnfStatus.offlineOrUnreachable);
    assert(!localRpmDnfStatus.updateAvailable);
    assert(!localRpmDnfStatus.cacheMayBeStale);
    assert(localRpmDnfStatus.statusMessage.find("sudo dnf makecache") != std::string::npos);

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

    // Preserve the actual authorization failure instead of guessing network loss.
    writeExecutable(aptBinDir / "pkexec",
                    "#!/bin/sh\necho 'Error executing command as another user: No authentication agent found.' >&2\nexit 127\n");
    const auto noAgentRefresh = UpdateController::refreshPackageCache(aptStatus, aptEnv);
    assert(!noAgentRefresh.success);
    assert(noAgentRefresh.message.find("No authentication agent found") != std::string::npos);
    assert(noAgentRefresh.message.find("桌面应用菜单") != std::string::npos);
    assert(noAgentRefresh.message.find("sudo apt update") != std::string::npos);
    const auto noAgentUpgrade = UpdateController::runSystemUpgrade(aptStatus, aptEnv);
    assert(!noAgentUpgrade.success);
    assert(noAgentUpgrade.message.find("No authentication agent found") != std::string::npos);

    // pkexec's fallback text agent also fails when a GUI process has no controlling TTY.
    writeExecutable(aptBinDir / "pkexec",
                    "#!/bin/sh\necho 'Error creating textual authentication agent: Error opening current controlling terminal' >&2\nexit 127\n");
    const auto noTerminalRefresh = UpdateController::refreshPackageCache(aptStatus, aptEnv);
    assert(!noTerminalRefresh.success);
    assert(noTerminalRefresh.message.find("桌面应用菜单") != std::string::npos);
    assert(noTerminalRefresh.message.find("Error creating textual authentication agent") != std::string::npos);

    // Package-manager diagnostics and the failing exit status remain visible.
    writeExecutable(aptBinDir / "pkexec",
                    "#!/bin/sh\necho 'E: Could not resolve packages.example.test' >&2\nexit 100\n");
    const auto networkRefresh = UpdateController::refreshPackageCache(aptStatus, aptEnv);
    assert(!networkRefresh.success);
    assert(networkRefresh.message.find("Could not resolve packages.example.test") != std::string::npos);
    assert(networkRefresh.message.find("100") != std::string::npos);

    // Long / invalid subprocess output must remain bounded and valid for GTK labels.
    writeExecutable(aptBinDir / "pkexec",
                    "#!/bin/sh\nprintf '\\377' >&2\ni=0\n"
                    "while [ \"$i\" -lt 1000 ]; do printf '错' >&2; i=$((i+1)); done\nexit 1\n");
    const auto longDiagnostic = UpdateController::refreshPackageCache(aptStatus, aptEnv);
    assert(!longDiagnostic.success);
    assert(g_utf8_validate(longDiagnostic.message.c_str(), -1, nullptr));
    assert(g_utf8_strlen(longDiagnostic.message.c_str(), -1) < 800);
    assert(longDiagnostic.message.find("…") != std::string::npos);
    assert(longDiagnostic.message.find("sudo apt update") != std::string::npos);

    writeExecutable(aptBinDir / "pkexec",
                    "#!/bin/sh\nprintf '%s\\n' \"$*\" > \"" + pkexecLog.string() + "\"\nexit 0\n");
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

    // 7b. Package operation timeout when child main task is in zombie 'Z' state while secondary thread continues:
    // Contract (S3.2): Must transition to Continuing and background tracking, NOT incorrectly assume exited on main thread Z.
    const auto selfExe = std::filesystem::canonical("/proc/self/exe");
    writeExecutable(
        aptBinDir / "pkexec",
        "#!/bin/sh\n"
        "exec \"" +
            selfExe.string() +
            "\" --mock-zombie-exit\n");
    const auto exitedTimeoutRes = UpdateController::runSystemUpgrade(aptStatus, timeoutEnv);
    assert(!exitedTimeoutRes.success);
    assert(exitedTimeoutRes.message.find("系统包管理器升级等待超时") != std::string::npos);
    assert(UpdateController::hasActivePackageOperation());
    assert(UpdateController::waitForActivePackageOperation(2000));
    assert(!UpdateController::hasActivePackageOperation());
    auto zBgRes = UpdateController::takeCompletedPackageOperationResult();
    assert(zBgRes.has_value());
    assert(zBgRes->success);

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
    for (int i = 0; i < 120; ++i) {
        const pid_t waited = ::waitpid(childPid, &waitStatus, WNOHANG);
        if (waited == childPid) {
            childExitedFast = WIFEXITED(waitStatus) && (WEXITSTATUS(waitStatus) == 0);
            break;
        }
        ::usleep(25000); // 120 * 25ms = 3.0s max
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
        assert(spawnFailRes.message.find("系统返回：") != std::string::npos);
        const auto spawnFailRefresh = UpdateController::refreshPackageCache(aptStatus, aptEnv);
        assert(!spawnFailRefresh.success);
        assert(spawnFailRefresh.message.find("系统返回：") != std::string::npos);
    }
    assert(countSubprocTempFds() == 0);
    // 8. Package manager lock detection
    const auto mockLockPath = tempRoot / "mock.lock";
    writeTextFile(mockLockPath, "lock-placeholder");
    ::setenv("MODERNIME_PACKAGE_MANAGER_LOCK_FILE", mockLockPath.c_str(), 1);
    assert(!UpdateController::isPackageManagerLocked()); // Unlocked file returns false
    {
        const pid_t lockChild = ::fork();
        assert(lockChild >= 0);
        if (lockChild == 0) {
            int lockFd = ::open(mockLockPath.c_str(), O_RDWR);
            struct flock fl{};
            fl.l_type = F_WRLCK;
            fl.l_whence = SEEK_SET;
            fl.l_start = 0;
            fl.l_len = 0;
            ::fcntl(lockFd, F_SETLK, &fl);
            ::pause();
            ::_exit(0);
        }
        ::usleep(20000); // wait for child to obtain lock
        assert(UpdateController::isPackageManagerLocked()); // Actively locked file returns true
        ::kill(lockChild, SIGTERM);
        ::waitpid(lockChild, nullptr, 0);
    }
    assert(!UpdateController::isPackageManagerLocked()); // Unlocked returns false
    ::unsetenv("MODERNIME_PACKAGE_MANAGER_LOCK_FILE");

    // 9. T12 Required Test 1: NormalWaitRejectsConcurrentOperation
    // Fake pkexec blocks on a barrier during normal wait (before timeout).
    // Verify that concurrent refresh or upgrade requests are rejected, and exactly 1 spawn occurs.
    {
        UpdateController::resetPackageOperationStateForTesting();
        const auto spawnCountFile = tempRoot / "t12-spawn-count.txt";
        const auto barrierStart = tempRoot / "t12-barrier-start.txt";
        const auto barrierRelease = tempRoot / "t12-barrier-release.txt";
        std::filesystem::remove(spawnCountFile);
        std::filesystem::remove(barrierStart);
        std::filesystem::remove(barrierRelease);

        writeExecutable(
            aptBinDir / "pkexec",
            "#!/bin/sh\n"
            "printf 'spawn\\n' >> \"" + spawnCountFile.string() + "\"\n"
            "touch \"" + barrierStart.string() + "\"\n"
            "while [ ! -f \"" + barrierRelease.string() + "\" ]; do\n"
            "  sleep 0.05\n"
            "done\n"
            "exit 0\n");

        std::thread worker([&]() {
            const auto res = UpdateController::refreshPackageCache(aptStatus, aptEnv);
            assert(res.success);
        });

        // Wait until fake pkexec has spawned and reached the barrier
        while (!std::filesystem::exists(barrierStart)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // At this moment, worker is inside normal wait (g_main_loop_run)
        assert(UpdateController::hasActivePackageOperation());
        assert(UpdateController::currentPackageOperationPhase() == PackageOperationPhase::Running);
        const auto activeId = UpdateController::currentPackageOperationId();
        assert(activeId.has_value() && *activeId > 0);

        // Concurrent refresh MUST be rejected immediately
        const auto dupRefresh = UpdateController::refreshPackageCache(aptStatus, aptEnv);
        assert(!dupRefresh.success);
        assert(dupRefresh.message.find("未完成的系统包管理器事务正在运行") != std::string::npos);

        // Concurrent upgrade MUST also be rejected immediately
        const auto dupUpgrade = UpdateController::runSystemUpgrade(aptStatus, aptEnv);
        assert(!dupUpgrade.success);
        assert(dupUpgrade.message.find("未完成的系统包管理器事务正在运行") != std::string::npos);

        // Release the barrier
        writeTextFile(barrierRelease, "release\n");
        worker.join();

        // Must be back to Idle
        assert(!UpdateController::hasActivePackageOperation());
        assert(UpdateController::currentPackageOperationPhase() == PackageOperationPhase::Idle);

        // Check spawn count: MUST be exactly 1
        std::ifstream spawnIn(spawnCountFile);
        int spawnCount = 0;
        std::string line;
        while (std::getline(spawnIn, line)) {
            if (!line.empty()) {
                ++spawnCount;
            }
        }
        assert(spawnCount == 1);
    }

    // 10. T12 Required Test 2: AuthenticationFailureReleasesSlot
    // Auth failure (exit 127) or spawn failure releases slot with matching ID; subsequent operations can run.
    {
        UpdateController::resetPackageOperationStateForTesting();

        // 10a. Authentication failure (exit 127)
        writeExecutable(
            aptBinDir / "pkexec",
            "#!/bin/sh\n"
            "printf 'Error executing command as another user: Not authorized\\n' >&2\n"
            "exit 127\n");

        const auto authFailRes = UpdateController::refreshPackageCache(aptStatus, aptEnv);
        assert(!authFailRes.success);
        assert(!UpdateController::hasActivePackageOperation());
        assert(UpdateController::currentPackageOperationPhase() == PackageOperationPhase::Idle);

        // 10b. Spawn failure (invalid interpreter)
        writeExecutable(
            aptBinDir / "pkexec",
            "#!/nonexistent/modernime_bad_interpreter_for_t12\n");

        const auto spawnFailRes = UpdateController::runSystemUpgrade(aptStatus, aptEnv);
        assert(!spawnFailRes.success);
        assert(!UpdateController::hasActivePackageOperation());
        assert(UpdateController::currentPackageOperationPhase() == PackageOperationPhase::Idle);

        // 10c. Subsequent operation can succeed
        writeExecutable(
            aptBinDir / "pkexec",
            "#!/bin/sh\n"
            "exit 0\n");

        const auto successRes = UpdateController::refreshPackageCache(aptStatus, aptEnv);
        assert(successRes.success);
        assert(!UpdateController::hasActivePackageOperation());
    }

    // 11. T12 Required Test 3: CompletionConsumedOnce
    // Completed result binds to operationId and is consumed only once; new operations do not overwrite unread results.
    {
        UpdateController::resetPackageOperationStateForTesting();

        const auto id1 = UpdateController::tryBeginPackageOperation(PackageOperationKind::RefreshCache);
        assert(id1.has_value());
        assert(UpdateController::advancePackageOperation(*id1, PackageOperationPhase::Starting, PackageOperationPhase::Running));
        assert(UpdateController::advancePackageOperation(*id1, PackageOperationPhase::Running, PackageOperationPhase::Continuing));
        assert(UpdateController::finishPackageOperation(*id1, RuntimeResult{true, "Op1-Refresh-Success"}, "out1", "err1", true));

        // Start operation 2 without consuming operation 1's result
        const auto id2 = UpdateController::tryBeginPackageOperation(PackageOperationKind::SystemUpgrade);
        assert(id2.has_value());
        assert(*id2 != *id1);
        assert(UpdateController::advancePackageOperation(*id2, PackageOperationPhase::Starting, PackageOperationPhase::Running));
        assert(UpdateController::finishPackageOperation(*id2, RuntimeResult{false, "Op2-Upgrade-Failed"}, "out2", "err2", true));

        assert(UpdateController::unconsumedPackageOperationCount() == 2);

        // Consume id1 by ID
        const auto res1 = UpdateController::takeCompletedPackageOperationResult(*id1);
        assert(res1.has_value());
        assert(res1->success);
        assert(res1->message == "Op1-Refresh-Success");

        // Try consuming id1 again: must return nullopt (consumed once)
        const auto res1Again = UpdateController::takeCompletedPackageOperationResult(*id1);
        assert(!res1Again.has_value());

        // id2 is still preserved and unconsumed
        assert(UpdateController::unconsumedPackageOperationCount() == 1);
        const auto res2 = UpdateController::takeCompletedPackageOperationResult(*id2);
        assert(res2.has_value());
        assert(!res2->success);
        assert(res2->message == "Op2-Upgrade-Failed");

        // Now queue is empty
        assert(UpdateController::unconsumedPackageOperationCount() == 0);
        assert(!UpdateController::takeCompletedPackageOperationResult().has_value());
    }

    // 12. T12 Required Test 4: OldCompletionCannotFinishNewOperation
    // Attempting to finish or advance with an old operationId cannot release or modify a newer active operation.
    {
        UpdateController::resetPackageOperationStateForTesting();

        const auto idOld = UpdateController::tryBeginPackageOperation(PackageOperationKind::RefreshCache);
        assert(idOld.has_value());
        assert(UpdateController::finishPackageOperation(*idOld, RuntimeResult{true, "Old-Completed"}, {}, {}, false));
        assert(!UpdateController::hasActivePackageOperation());

        // Begin new operation
        const auto idNew = UpdateController::tryBeginPackageOperation(PackageOperationKind::SystemUpgrade);
        assert(idNew.has_value());
        assert(*idNew != *idOld);
        assert(UpdateController::advancePackageOperation(*idNew, PackageOperationPhase::Starting, PackageOperationPhase::Running));
        assert(UpdateController::hasActivePackageOperation());
        assert(UpdateController::currentPackageOperationId() == idNew);
        assert(UpdateController::currentPackageOperationPhase() == PackageOperationPhase::Running);

        // Attempting to finish using idOld MUST fail and NOT affect idNew
        assert(!UpdateController::finishPackageOperation(*idOld, RuntimeResult{false, "Malicious-Old-Finish"}, {}, {}, true));
        assert(UpdateController::hasActivePackageOperation());
        assert(UpdateController::currentPackageOperationId() == idNew);
        assert(UpdateController::currentPackageOperationPhase() == PackageOperationPhase::Running);

        // Attempting to advance using idOld MUST also fail
        assert(!UpdateController::advancePackageOperation(*idOld, PackageOperationPhase::Running, PackageOperationPhase::Completed));
        assert(UpdateController::hasActivePackageOperation());
        assert(UpdateController::currentPackageOperationId() == idNew);

        // Finish idNew properly
        assert(UpdateController::finishPackageOperation(*idNew, RuntimeResult{true, "New-Completed"}, {}, {}, false));
        assert(!UpdateController::hasActivePackageOperation());
        assert(UpdateController::currentPackageOperationPhase() == PackageOperationPhase::Idle);
    }

    // 13. T12 Required Test 5: UnconsumedResultsHaveBound
    // Up to 8 unconsumed results are retained; 9th begin is rejected and preserves existing results.
    // Consuming 1 result allows beginning again.
    {
        UpdateController::resetPackageOperationStateForTesting();

        for (int i = 1; i <= 8; ++i) {
            const auto id = UpdateController::tryBeginPackageOperation(PackageOperationKind::RefreshCache);
            assert(id.has_value());
            assert(UpdateController::finishPackageOperation(
                *id, RuntimeResult{true, "Unconsumed-Result-" + std::to_string(i)}, {}, {}, true));
        }
        assert(UpdateController::unconsumedPackageOperationCount() == 8);

        // 9th begin MUST be rejected
        const auto id9 = UpdateController::tryBeginPackageOperation(PackageOperationKind::SystemUpgrade);
        assert(!id9.has_value());

        // refreshPackageCache and runSystemUpgrade also report bounded queue rejection
        const auto fullRej = UpdateController::refreshPackageCache(aptStatus, aptEnv);
        assert(!fullRej.success);
        assert(fullRej.message.find("队列已满") != std::string::npos);

        // All 8 results must still be preserved
        assert(UpdateController::unconsumedPackageOperationCount() == 8);

        // Consume 1 result (FIFO)
        const auto popped1 = UpdateController::takeCompletedPackageOperationResult();
        assert(popped1.has_value());
        assert(popped1->message == "Unconsumed-Result-1");
        assert(UpdateController::unconsumedPackageOperationCount() == 7);

        // Now begin SUCCEEDS
        const auto id9Retry = UpdateController::tryBeginPackageOperation(PackageOperationKind::SystemUpgrade);
        assert(id9Retry.has_value());
        assert(UpdateController::finishPackageOperation(
            *id9Retry, RuntimeResult{true, "Unconsumed-Result-9"}, {}, {}, true));
        assert(UpdateController::unconsumedPackageOperationCount() == 8);

        // Drain the remaining results in FIFO order
        for (int i = 2; i <= 9; ++i) {
            const auto item = UpdateController::takeCompletedPackageOperationResult();
            assert(item.has_value());
            assert(item->message == "Unconsumed-Result-" + std::to_string(i));
        }
        assert(UpdateController::unconsumedPackageOperationCount() == 0);
    }

    // 14. T12 Required Test 6: SynchronousLockRejectionDoesNotPolluteUnconsumedQueue (R1 / P1 Regression)
    // Synchronous lock rejection in refreshPackageCache must NOT enqueue an unconsumed result.
    // 9+ consecutive lock rejections: each fails, Idle, unconsumed count does not increase, spawnCount == 0.
    // Pre-existing unconsumed result is preserved with original ID/content and can be consumed once.
    // After unsetting the lock, next operation succeeds and spawns exactly once.
    {
        UpdateController::resetPackageOperationStateForTesting();

        // 1. Pre-enqueue 1 genuine unconsumed background completion
        const auto preId = UpdateController::tryBeginPackageOperation(PackageOperationKind::RefreshCache);
        assert(preId.has_value());
        assert(UpdateController::finishPackageOperation(
            *preId, RuntimeResult{true, "Pre-existing background completion"}, {}, {}, true));
        assert(UpdateController::unconsumedPackageOperationCount() == 1);

        const auto r1SpawnCountFile = tempRoot / "t12-r1-spawn-count.txt";
        std::filesystem::remove(r1SpawnCountFile);
        writeExecutable(
            aptBinDir / "pkexec",
            "#!/bin/sh\n"
            "printf 'spawn\\n' >> \"" + r1SpawnCountFile.string() + "\"\n"
            "exit 0\n");

        // 2. Set mock lock and invoke refreshPackageCache 9 times
        ::setenv("MODERNIME_MOCK_PACKAGE_MANAGER_LOCKED", "1", 1);
        for (int i = 1; i <= 9; ++i) {
            const auto res = UpdateController::refreshPackageCache(aptStatus, aptEnv);
            assert(!res.success);
            assert(res.message.find("系统包管理器正忙") != std::string::npos);
            assert(!UpdateController::hasActivePackageOperation());
            assert(UpdateController::currentPackageOperationPhase() == PackageOperationPhase::Idle);
            assert(UpdateController::unconsumedPackageOperationCount() == 1); // MUST NOT INCREASE
        }
        ::unsetenv("MODERNIME_MOCK_PACKAGE_MANAGER_LOCKED");

        // Spawn count must be 0
        assert(!std::filesystem::exists(r1SpawnCountFile) || std::filesystem::file_size(r1SpawnCountFile) == 0);

        // 3. Pre-existing background result remains intact and can be consumed once
        const auto preRes = UpdateController::takeCompletedPackageOperationResult(*preId);
        assert(preRes.has_value());
        assert(preRes->success);
        assert(preRes->message == "Pre-existing background completion");
        assert(UpdateController::unconsumedPackageOperationCount() == 0);
        assert(!UpdateController::takeCompletedPackageOperationResult(*preId).has_value());

        // 4. After unlock, next operation succeeds and spawns exactly once
        const auto afterUnlock = UpdateController::refreshPackageCache(aptStatus, aptEnv);
        assert(afterUnlock.success);
        assert(!UpdateController::hasActivePackageOperation());
        assert(UpdateController::currentPackageOperationPhase() == PackageOperationPhase::Idle);

        std::ifstream spawnIn(r1SpawnCountFile);
        int spawnCount = 0;
        std::string line;
        while (std::getline(spawnIn, line)) {
            if (!line.empty()) {
                ++spawnCount;
            }
        }
        assert(spawnCount == 1);
    }

    // 15. T12 Required Test 7: StatusTransitionsUnitContract
    // Verify pure state-machine transitions: Starting -> Running -> Continuing -> Uncertain -> Completed (via explicit finish).
    // Operations remain blocked while in Continuing or Uncertain, and Uncertain does not auto-finish.
    {
        UpdateController::resetPackageOperationStateForTesting();

        const auto id = UpdateController::tryBeginPackageOperation(PackageOperationKind::SystemUpgrade);
        assert(id.has_value());
        assert(UpdateController::advancePackageOperation(*id, PackageOperationPhase::Starting, PackageOperationPhase::Running));

        // Active operation is preserved in Running
        assert(UpdateController::hasActivePackageOperation());
        assert(UpdateController::currentPackageOperationId() == id);
        assert(UpdateController::currentPackageOperationPhase() == PackageOperationPhase::Running);

        // Timeout transition: Running -> Continuing
        assert(UpdateController::advancePackageOperation(*id, PackageOperationPhase::Running, PackageOperationPhase::Continuing));
        assert(UpdateController::hasActivePackageOperation());
        assert(UpdateController::currentPackageOperationPhase() == PackageOperationPhase::Continuing);

        // Duplicate operations are blocked while in Continuing
        assert(!UpdateController::tryBeginPackageOperation(PackageOperationKind::RefreshCache).has_value());
        const auto dupInContinuing = UpdateController::refreshPackageCache(aptStatus, aptEnv);
        assert(!dupInContinuing.success);
        assert(dupInContinuing.message.find("未完成的系统包管理器事务正在运行") != std::string::npos);

        // Indeterminate/unknown result transitions Continuing -> Uncertain
        assert(UpdateController::advancePackageOperation(*id, PackageOperationPhase::Continuing, PackageOperationPhase::Uncertain));
        assert(UpdateController::hasActivePackageOperation());
        assert(UpdateController::currentPackageOperationPhase() == PackageOperationPhase::Uncertain);

        // While Uncertain, operations remain blocked (no auto-retry, no duplicate upgrade)
        assert(!UpdateController::tryBeginPackageOperation(PackageOperationKind::SystemUpgrade).has_value());
        const auto dupInUncertain = UpdateController::runSystemUpgrade(aptStatus, aptEnv);
        assert(!dupInUncertain.success);
        assert(dupInUncertain.message.find("未完成的系统包管理器事务正在运行") != std::string::npos);

        // Uncertain does NOT automatically release or complete
        assert(UpdateController::hasActivePackageOperation());
        assert(UpdateController::currentPackageOperationPhase() == PackageOperationPhase::Uncertain);

        // Only explicit verification/finish releases Uncertain
        assert(UpdateController::finishPackageOperation(
            *id, RuntimeResult{false, "Manual administrative intervention required"}, {}, {}, false));
        assert(!UpdateController::hasActivePackageOperation());
        assert(UpdateController::currentPackageOperationPhase() == PackageOperationPhase::Idle);
    }

    // 16. T12 Required Test 8: NormalWaitSignalTerminationTransitionsToUncertain
    // Abnormal child exit (signal interruption) during normal wait enters Uncertain state.
    // Verifies slot stays active, concurrent operations are blocked, no unconsumed result queued.
    {
        UpdateController::resetPackageOperationStateForTesting();
        const auto uncertainDir = tempRoot / "uncertain-normal-wait";
        std::filesystem::create_directories(uncertainDir);
        writeExecutable(
            uncertainDir / "pkexec",
            "#!/bin/sh\n"
            "kill -TERM $$\n");
        writeExecutable(
            uncertainDir / "apt-get",
            "#!/bin/sh\nexit 0\n");

        UpdateStatus status;
        status.manager = PackageManagerKind::Apt;
        Environment env{{"PATH", uncertainDir.string()}};

        const auto res = UpdateController::refreshPackageCache(status, env);
        assert(!res.success);
        assert(res.message.find("异常中断（状态无法确定）") != std::string::npos);

        assert(UpdateController::hasActivePackageOperation());
        assert(UpdateController::currentPackageOperationPhase() == PackageOperationPhase::Uncertain);
        assert(UpdateController::unconsumedPackageOperationCount() == 0);
        const auto activeId = UpdateController::currentPackageOperationId();
        assert(activeId.has_value());

        // Concurrent operations are blocked while Uncertain
        const auto dup = UpdateController::refreshPackageCache(status, env);
        assert(!dup.success);
        assert(dup.message.find("未完成的系统包管理器事务正在运行") != std::string::npos);

        // Only explicit manual finish releases Uncertain slot
        assert(UpdateController::finishPackageOperation(
            *activeId, RuntimeResult{false, "Manual recovery"}, {}, {}, false));
        assert(!UpdateController::hasActivePackageOperation());
        assert(UpdateController::currentPackageOperationPhase() == PackageOperationPhase::Idle);
    }

    // 17. T12 Required Test 9: TimeoutBackgroundSignalTerminationTransitionsToUncertain
    // Abnormal child exit (signal interruption) in background tracking after timeout enters Uncertain state.
    // Verifies background tracker transitions Continuing -> Uncertain, slot stays active, no auto-finish.
    {
        UpdateController::resetPackageOperationStateForTesting();
        const auto uncertainDir = tempRoot / "uncertain-timeout-bg";
        std::filesystem::create_directories(uncertainDir);
        writeExecutable(
            uncertainDir / "pkexec",
            "#!/bin/sh\n"
            "sleep 0.15\n"
            "kill -TERM $$\n");
        writeExecutable(
            uncertainDir / "apt-get",
            "#!/bin/sh\nexit 0\n");

        UpdateStatus status;
        status.manager = PackageManagerKind::Apt;
        Environment env{{"PATH", uncertainDir.string()}, {"MODERNIME_PACKAGE_OP_TIMEOUT_MS", "30"}};

        const auto res = UpdateController::refreshPackageCache(status, env);
        assert(!res.success);
        assert(res.message.find("刷新软件源缓存等待超时") != std::string::npos);

        // Wait with bounded deadline for background thread to detect signal termination and transition to Uncertain
        for (int i = 0; i < 100 && UpdateController::currentPackageOperationPhase() != PackageOperationPhase::Uncertain; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        assert(UpdateController::hasActivePackageOperation());
        assert(UpdateController::currentPackageOperationPhase() == PackageOperationPhase::Uncertain);
        assert(UpdateController::unconsumedPackageOperationCount() == 0);
        const auto activeId = UpdateController::currentPackageOperationId();
        assert(activeId.has_value());

        // Concurrent operations are blocked while Uncertain
        const auto dup = UpdateController::runSystemUpgrade(status, env);
        assert(!dup.success);
        assert(dup.message.find("未完成的系统包管理器事务正在运行") != std::string::npos);

        // Only explicit manual finish releases Uncertain slot
        assert(UpdateController::finishPackageOperation(
            *activeId, RuntimeResult{false, "Manual recovery"}, {}, {}, false));
        assert(!UpdateController::hasActivePackageOperation());
        assert(UpdateController::currentPackageOperationPhase() == PackageOperationPhase::Idle);
    }

    std::filesystem::remove_all(tempRoot);
    return 0;
}
