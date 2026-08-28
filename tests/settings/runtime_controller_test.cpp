#include "modernime/settings/runtime_controller.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string_view>
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
    const auto fcitxLog = std::filesystem::temp_directory_path() /
                          "modernime-runtime-controller-fcitx.log";
    const auto remoteLog = std::filesystem::temp_directory_path() /
                           "modernime-runtime-controller-remote.log";
    std::error_code cleanupError;
    std::filesystem::remove(fcitxLog, cleanupError);
    std::filesystem::remove(remoteLog, cleanupError);
    const modernime::settings::Environment environment{
        {"DISPLAY", ":0"},
        {"DBUS_SESSION_BUS_ADDRESS", "unix:path=/tmp/dbus"},
        {"FCITX_ADDON_DIRS", "/home/wsl/.local/lib/fcitx5:/usr/lib/fcitx5"},
        {"FAKE_FCITX5_LOG", fcitxLog.string()},
        {"FAKE_REMOTE_LOG", remoteLog.string()}};

    const auto status = modernime::settings::RuntimeController::probe(
        argv[1], environment);
    assertTrue(status.available && status.running,
               "available remote reports a running service");
    assertTrue(status.currentInputMethod == "modernime" &&
                   status.modernimeActive,
               "probe parses the active ModernIME state");

    const auto reload = modernime::settings::RuntimeController::reload(
        argv[2], argv[1], environment);
    assertTrue(reload.success, "reload succeeds for a working remote");

    std::ifstream fcitxLogStream(fcitxLog);
    std::stringstream fcitxLogContents;
    fcitxLogContents << fcitxLogStream.rdbuf();
    assertTrue(fcitxLogContents.str().find("args -d -r -u modernime-ui") !=
                   std::string::npos,
               "reload starts Fcitx5 with the ModernIME UI override");
    assertTrue(fcitxLogContents.str().find(
                   "addon=/home/wsl/.local/lib/fcitx5:/usr/lib/fcitx5") !=
                   std::string::npos,
               "reload passes the ModernIME addon search path");

    std::ifstream remoteLogStream(remoteLog);
    std::stringstream remoteLogContents;
    remoteLogContents << remoteLogStream.rdbuf();
    assertTrue(remoteLogContents.str().find("remote:-s modernime") !=
                   std::string::npos,
               "reload activates the ModernIME input method");
    assertTrue(remoteLogContents.str().find("remote:-o") != std::string::npos,
               "reload enables the input method after selecting it");

    const auto missing = modernime::settings::RuntimeController::probe(
        "/definitely/missing/fcitx5-remote", environment);
    assertTrue(!missing.available && !missing.running &&
                   !missing.message.empty(),
               "missing remote is reported distinctly");

    const auto failed = modernime::settings::RuntimeController::reload(
        argv[2], "/bin/false", environment);
    assertTrue(!failed.success && !failed.message.empty(),
               "non-zero remote command is reported");

    const auto missingFcitx = modernime::settings::RuntimeController::reload(
        "/definitely/missing/fcitx5", argv[1], environment);
    assertTrue(!missingFcitx.success && !missingFcitx.message.empty(),
               "missing Fcitx5 executable is reported");

    std::filesystem::remove(fcitxLog, cleanupError);
    std::filesystem::remove(remoteLog, cleanupError);
    return EXIT_SUCCESS;
}
