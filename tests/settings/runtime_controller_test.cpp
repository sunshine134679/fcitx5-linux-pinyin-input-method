#include "modernime/settings/runtime_controller.h"

#include <chrono>
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
        {"FCITX_ADDON_DIRS", "/demo-prefix/lib/fcitx5:/usr/lib/fcitx5"}};

    const auto status = modernime::settings::RuntimeController::probe(
        argv[1], baseEnvironment);
    assertTrue(status.available && status.running,
               "available remote reports a running service");
    assertTrue(status.inputContextAvailable && status.inputMethodEnabled,
               "probe identifies an active input context");
    assertTrue(status.currentInputMethod == "modernime" &&
                   status.modernimeAvailable &&
                   status.modernimeActive,
               "probe parses the active ModernIME state");

    auto noContextEnvironment = baseEnvironment;
    noContextEnvironment.emplace_back("FAKE_REMOTE_INPUT_METHOD_EMPTY", "1");
    noContextEnvironment.emplace_back("FAKE_REMOTE_NO_CONTEXT", "1");
    noContextEnvironment.emplace_back(
        "FAKE_FCITX5_LOG", (directory / "no-context-fcitx.log").string());
    const auto noContext = modernime::settings::RuntimeController::probe(
        argv[1], noContextEnvironment);
    assertTrue(noContext.available && noContext.running &&
                   !noContext.inputContextAvailable &&
                   !noContext.inputMethodEnabled &&
                   noContext.currentInputMethod.empty() &&
                   noContext.modernimeAvailable && !noContext.modernimeActive,
               "probe distinguishes a running service without an input context");
    assertTrue(noContext.message.find("输入上下文") != std::string::npos,
               "no-context status explains why current input method is empty");

    const auto noContextReload = modernime::settings::RuntimeController::reload(
        argv[2], argv[1], noContextEnvironment);
    assertTrue(noContextReload.success,
               "ModernIME reload succeeds when no window currently owns an input context");

    auto inactiveEnvironment = baseEnvironment;
    inactiveEnvironment.emplace_back("FAKE_REMOTE_STATUS", "1");
    const auto inactive = modernime::settings::RuntimeController::probe(
        argv[1], inactiveEnvironment);
    assertTrue(inactive.running && inactive.inputContextAvailable &&
                   !inactive.inputMethodEnabled &&
                   !inactive.modernimeActive,
               "probe does not call an inactive input context activated");

    auto missingModernimeEnvironment = noContextEnvironment;
    missingModernimeEnvironment.emplace_back(
        "FAKE_REMOTE_MODERNIME_AVAILABLE", "0");
    const auto missingModernime = modernime::settings::RuntimeController::probe(
        argv[1], missingModernimeEnvironment);
    assertTrue(missingModernime.running &&
                   !missingModernime.modernimeAvailable &&
                   !missingModernime.modernimeActive &&
                   missingModernime.message.find("未找到 ModernIME") !=
                       std::string::npos,
               "probe reports a running service without a registered ModernIME");

    auto runningEnvironment = baseEnvironment;
    runningEnvironment.emplace_back("FAKE_FCITX5_LOG",
                                    runningFcitxLog.string());
    runningEnvironment.emplace_back("FAKE_REMOTE_LOG",
                                    runningRemoteLog.string());
    const auto runningReload = modernime::settings::RuntimeController::reload(
        argv[2], argv[1], runningEnvironment);
    assertTrue(runningReload.success,
               "running Fcitx5 reloads through the existing service");

    std::ifstream runningFcitxStream(runningFcitxLog);
    std::stringstream runningFcitxContents;
    runningFcitxContents << runningFcitxStream.rdbuf();
    assertTrue(runningFcitxContents.str().find(
                   "args -d --replace -u modernime-ui") !=
                   std::string::npos,
               "running Fcitx5 is replaced with the ModernIME UI");
    assertTrue(runningFcitxContents.str().find(
                   "addon=/demo-prefix/lib/fcitx5:/usr/lib/fcitx5") !=
                   std::string::npos,
               "replacement receives the current ModernIME addon directory");

    std::ifstream runningRemoteStream(runningRemoteLog);
    std::stringstream runningRemoteContents;
    runningRemoteContents << runningRemoteStream.rdbuf();
    assertTrue(runningRemoteContents.str().find("remote:-r") ==
                   std::string::npos,
               "running Fcitx5 does not use configuration reload for plugins");

    auto daemonizedEnvironment = baseEnvironment;
    daemonizedEnvironment.emplace_back(
        "FAKE_FCITX5_LOG", (directory / "daemonized-fcitx.log").string());
    daemonizedEnvironment.emplace_back(
        "FAKE_FCITX5_DAEMONIZE", "1");
    daemonizedEnvironment.emplace_back(
        "FAKE_FCITX5_DAEMON_SECONDS", "2");
    const auto daemonizedStart = std::chrono::steady_clock::now();
    const auto daemonizedReload =
        modernime::settings::RuntimeController::reload(
            argv[2], argv[1], daemonizedEnvironment);
    const auto daemonizedElapsed = std::chrono::duration_cast<
        std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                   daemonizedStart);
    assertTrue(daemonizedReload.success,
               "daemonized Fcitx5 replacement still reloads successfully");
    assertTrue(daemonizedElapsed < std::chrono::seconds(1),
               "daemonized Fcitx5 replacement does not wait for inherited "
               "stderr");

    const auto failedReplaceLog = directory / "failed-replace-fcitx.log";
    auto failedReplaceEnvironment = baseEnvironment;
    failedReplaceEnvironment.emplace_back("FAKE_FCITX5_LOG",
                                          failedReplaceLog.string());
    failedReplaceEnvironment.emplace_back("FAKE_FCITX5_EXIT", "7");
    failedReplaceEnvironment.emplace_back("FAKE_FCITX5_ERROR",
                                          "fake fcitx5 replace error");
    const auto failedReplace = modernime::settings::RuntimeController::reload(
        argv[2], argv[1], failedReplaceEnvironment);
    assertTrue(!failedReplace.success &&
                   failedReplace.message.find("fake fcitx5 replace error") !=
                       std::string::npos,
               "Fcitx5 replacement errors are returned to the client");

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

    // Test runtime application state reporting and confirmation contract
    const auto runtimeStateFile = directory / "runtime.state";
    {
        std::ofstream stateOut(runtimeStateFile);
        stateOut << "applied_content_hash=aabbccdd11223344\n";
        stateOut << "settings_generation=42\n";
    }

    const auto statusWithState = modernime::settings::RuntimeController::probe(
        argv[1], runtimeStateFile, baseEnvironment);
    assertTrue(statusWithState.appliedHashAvailable, "probe finds applied hash in state file");
    assertTrue(statusWithState.appliedContentHash == "aabbccdd11223344", "probe reads correct hash");
    assertTrue(statusWithState.settingsGeneration == 42, "probe reads settings generation");

    // describeSettingsApplication tests
    modernime::settings::RuntimeStatus testStatus;
    testStatus.running = false;
    assertTrue(modernime::settings::describeSettingsApplication(testStatus, "aabbccdd11223344") ==
                   "已保存，下次启用时应用",
               "stopped engine reports next time apply");

    testStatus.running = true;
    testStatus.appliedHashAvailable = false;
    assertTrue(modernime::settings::describeSettingsApplication(testStatus, "aabbccdd11223344") ==
                   "已保存，应用状态待确认",
               "missing state file / older plugin reports confirmation pending");

    testStatus.appliedHashAvailable = true;
    testStatus.appliedContentHash = "old_hash_0000";
    assertTrue(modernime::settings::describeSettingsApplication(testStatus, "aabbccdd11223344") ==
                   "已保存，等待输入法读取",
               "stale applied hash reports waiting for input method to read");

    testStatus.appliedContentHash = "aabbccdd11223344";
    testStatus.inputContextAvailable = false;
    assertTrue(modernime::settings::describeSettingsApplication(testStatus, "aabbccdd11223344") ==
                   "已保存，等待新输入上下文生效",
               "applied with no context reports waiting for new context");

    testStatus.inputContextAvailable = true;
    assertTrue(modernime::settings::describeSettingsApplication(testStatus, "aabbccdd11223344") ==
                   "已保存并应用",
               "applied with active context reports saved and applied");

    assertTrue(modernime::settings::describeSettingsApplication(testStatus, "") ==
                   "设置已保存",
               "empty saved hash reports saved");

    std::filesystem::remove_all(directory, cleanupError);
    return EXIT_SUCCESS;
}
