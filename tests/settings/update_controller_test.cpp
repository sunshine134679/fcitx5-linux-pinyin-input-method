#include "modernime/settings/update_controller.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
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

int main() {
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

    // 7b. Package operation timeout when child has already exited (stillRunningOnTimeout == false):
    // Spawn a subshell holding stdout open briefly while the main pkexec process exits immediately (zombie 'Z' at timeout)
    writeExecutable(
        aptBinDir / "pkexec",
        "#!/bin/sh\n"
        "(sleep 0.2) &\n"
        "exit 1\n");
    const auto exitedTimeoutRes = UpdateController::runSystemUpgrade(aptStatus, timeoutEnv);
    assert(!exitedTimeoutRes.success);
    assert(exitedTimeoutRes.message.find("子进程已退出，未检测到后台仍在运行的 apt/dpkg 进程") !=
           std::string::npos);
    assert(!UpdateController::hasActivePackageOperation());

    std::filesystem::remove_all(tempRoot);
    return 0;
}
