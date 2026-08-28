#include "modernime/settings/runtime_controller.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "runtime controller test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main(int argc, char **argv) {
    assertTrue(argc == 3, "test receives fake remote and fcitx executables");
    const auto directory = std::filesystem::temp_directory_path() /
                           "modernime-runtime-controller-test";
    const auto runningFcitxLog = directory / "running-fcitx.log";
    const auto runningRemoteLog = directory / "running-remote.log";
    const auto stoppedFcitxLog = directory / "stopped-fcitx.log";
    const auto stoppedRemoteLog = directory / "stopped-remote.log";
    const auto stoppedState = directory / "stopped.state";
    const auto missingFcitxState = directory / "missing-fcitx.state";
    std::error_code cleanupError;
    std::filesystem::remove_all(directory, cleanupError);
    std::filesystem::create_directories(directory, cleanupError);
    assertTrue(!cleanupError, "runtime test directory is created");
    const modernime::settings::Environment baseEnvironment{
        {"DISPLAY", ":0"},
        {"DBUS_SESSION_BUS_ADDRESS", "unix:path=/tmp/dbus"},
        {"FCITX_ADDON_DIRS", "/home/wsl/.local/lib/fcitx5:/usr/lib/fcitx5"}};

    const auto status = modernime::settings::RuntimeController::probe(
        argv[1], baseEnvironment);
    assertTrue(status.available && status.running,
               "available remote reports a running service");
    assertTrue(status.currentInputMethod == "modernime" &&
                   status.modernimeActive,
               "probe parses the active ModernIME state");

    auto runningEnvironment = baseEnvironment;
    runningEnvironment.emplace_back("FAKE_FCITX5_LOG",
                                    runningFcitxLog.string());
    runningEnvironment.emplace_back("FAKE_REMOTE_LOG",
                                    runningRemoteLog.string());
    const auto runningReload = modernime::settings::RuntimeController::reload(
        argv[2], argv[1], runningEnvironment);
    assertTrue(runningReload.success,
               "running Fcitx5 reloads through the existing service");
    assertTrue(!std::filesystem::exists(runningFcitxLog),
               "running Fcitx5 is not started a second time");

    std::ifstream runningRemoteStream(runningRemoteLog);
    std::stringstream runningRemoteContents;
    runningRemoteContents << runningRemoteStream.rdbuf();
    assertTrue(runningRemoteContents.str().find("remote:-r") !=
                   std::string::npos,
               "running Fcitx5 is reloaded through fcitx5-remote");

    auto stoppedEnvironment = baseEnvironment;
    stoppedEnvironment.emplace_back("FAKE_REMOTE_STATUS", "3");
    stoppedEnvironment.emplace_back("FAKE_REMOTE_STATE_FILE",
                                    stoppedState.string());
    stoppedEnvironment.emplace_back("FAKE_FCITX5_LOG",
                                    stoppedFcitxLog.string());
    stoppedEnvironment.emplace_back("FAKE_REMOTE_LOG",
                                    stoppedRemoteLog.string());
    const auto stoppedReload = modernime::settings::RuntimeController::reload(
        argv[2], argv[1], stoppedEnvironment);
    assertTrue(stoppedReload.success,
               "stopped Fcitx5 is started and ModernIME is activated");

    std::ifstream stoppedFcitxStream(stoppedFcitxLog);
    std::stringstream stoppedFcitxContents;
    stoppedFcitxContents << stoppedFcitxStream.rdbuf();
    assertTrue(stoppedFcitxContents.str().find("args -d -u modernime-ui") !=
                   std::string::npos,
               "stopped Fcitx5 starts with the ModernIME UI override");
    assertTrue(stoppedFcitxContents.str().find("args -d -r") ==
                   std::string::npos,
               "stopped Fcitx5 does not receive a duplicate restart flag");

    const auto failedStartState = directory / "failed-start.state";
    auto failedStartEnvironment = baseEnvironment;
    failedStartEnvironment.emplace_back("FAKE_REMOTE_STATUS", "3");
    failedStartEnvironment.emplace_back("FAKE_REMOTE_STATE_FILE",
                                        failedStartState.string());
    failedStartEnvironment.emplace_back("FAKE_FCITX5_LOG",
                                        (directory / "failed-start-fcitx.log")
                                            .string());
    failedStartEnvironment.emplace_back(
        "FAKE_FCITX5_EXIT", "7");
    failedStartEnvironment.emplace_back("FAKE_FCITX5_ERROR",
                                        "fake fcitx5 start error");
    const auto failedStart = modernime::settings::RuntimeController::reload(
        argv[2], argv[1], failedStartEnvironment);
    assertTrue(!failedStart.success &&
                   failedStart.message.find("fake fcitx5 start error") !=
                       std::string::npos,
               "fcitx5 startup errors are returned to the client");

    const auto missing = modernime::settings::RuntimeController::probe(
        "/definitely/missing/fcitx5-remote", baseEnvironment);
    assertTrue(!missing.available && !missing.running &&
                   !missing.message.empty(),
               "missing remote is reported distinctly");

    const auto failed = modernime::settings::RuntimeController::reload(
        argv[2], "/bin/false", baseEnvironment);
    assertTrue(!failed.success && !failed.message.empty(),
               "non-zero remote command is reported");

    auto missingFcitxEnvironment = baseEnvironment;
    missingFcitxEnvironment.emplace_back("FAKE_REMOTE_STATUS", "3");
    missingFcitxEnvironment.emplace_back("FAKE_REMOTE_STATE_FILE",
                                         missingFcitxState.string());
    const auto missingFcitx = modernime::settings::RuntimeController::reload(
        "/definitely/missing/fcitx5", argv[1], missingFcitxEnvironment);
    assertTrue(!missingFcitx.success && !missingFcitx.message.empty(),
               "missing Fcitx5 executable is reported");

    std::filesystem::remove_all(directory, cleanupError);
    return EXIT_SUCCESS;
}
