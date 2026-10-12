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
    assert(dnfStatus.updateAvailable);
    assert(dnfStatus.manager == PackageManagerKind::Dnf);
    assert(dnfStatus.candidateVersion == "0.1.9-1.oe2409");
    assert(dnfStatus.packageSource.find("modernime-openeuler") != std::string::npos);
    assert(dnfStatus.packageSource.find("channel=rpm") != std::string::npos);
    assert(dnfStatus.upgradeCommand == "sudo dnf upgrade modernime");

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

    // 5. Offline / unreachable repository distinguishes from software corruption
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
    assert(!offlineStatus.updateAvailable);
    assert(offlineStatus.statusMessage.find("未发现软件损坏") != std::string::npos);

    // 6. System upgrade delegation via pkexec (never downloads or overwrites in-process)
    const auto pkexecLog = tempRoot / "pkexec-invoked.log";
    writeExecutable(aptBinDir / "apt-get", "#!/bin/sh\nexit 0\n");
    writeExecutable(
        aptBinDir / "pkexec",
        "#!/bin/sh\n"
        "printf '%s\\n' \"$*\" > \"" +
            pkexecLog.string() +
            "\"\n"
            "exit 0\n");

    const auto upgradeRes = UpdateController::runSystemUpgrade(aptStatus, aptEnv);
    assert(upgradeRes.success);
    std::ifstream logIn(pkexecLog);
    std::string loggedArgs;
    std::getline(logIn, loggedArgs);
    assert(loggedArgs.find("apt-get install --only-upgrade -y modernime") != std::string::npos);

    std::filesystem::remove_all(tempRoot);
    return 0;
}
