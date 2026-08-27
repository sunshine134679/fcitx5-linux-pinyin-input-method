#include "modernime/settings/runtime_controller.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
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
    assertTrue(argc == 2, "test receives fake remote executable");
    const modernime::settings::Environment environment{
        {"DISPLAY", ":0"}, {"DBUS_SESSION_BUS_ADDRESS", "unix:path=/tmp/dbus"}};

    const auto status = modernime::settings::RuntimeController::probe(
        argv[1], environment);
    assertTrue(status.available && status.running,
               "available remote reports a running service");
    assertTrue(status.currentInputMethod == "modernime" &&
                   status.modernimeActive,
               "probe parses the active ModernIME state");

    const auto reload = modernime::settings::RuntimeController::reload(
        argv[1], environment);
    assertTrue(reload.success, "reload succeeds for a working remote");

    const auto missing = modernime::settings::RuntimeController::probe(
        "/definitely/missing/fcitx5-remote", environment);
    assertTrue(!missing.available && !missing.running &&
                   !missing.message.empty(),
               "missing remote is reported distinctly");

    const auto failed = modernime::settings::RuntimeController::reload(
        "/bin/false", environment);
    assertTrue(!failed.success && !failed.message.empty(),
               "non-zero remote command is reported");
    return EXIT_SUCCESS;
}
