#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void logInvocation(int argc, char **argv) {
    const auto *path = std::getenv("FAKE_REMOTE_LOG");
    if (path == nullptr || *path == '\0') {
        return;
    }
    std::ofstream log(path, std::ios::app);
    log << "remote:";
    for (int index = 1; index < argc; ++index) {
        log << (index == 1 ? "" : " ") << argv[index];
    }
    log << '\n';
}

const char *environmentValue(const char *name, const char *fallback) {
    const auto *value = std::getenv(name);
    return value == nullptr ? fallback : value;
}

bool activated() {
    const auto *statePath = std::getenv("FAKE_REMOTE_STATE_FILE");
    return statePath != nullptr && *statePath != '\0' &&
           std::ifstream(statePath).good();
}

void markActivated() {
    const auto *statePath = std::getenv("FAKE_REMOTE_STATE_FILE");
    if (statePath != nullptr && *statePath != '\0') {
        std::ofstream(statePath) << "active\n";
    }
}

} // namespace

int main(int argc, char **argv) {
    logInvocation(argc, argv);
    if (argc == 2 && std::string_view(argv[1]) == "-n") {
        std::cout << environmentValue("FAKE_REMOTE_INPUT_METHOD", "modernime")
                  << '\n';
        return EXIT_SUCCESS;
    }
    if (argc == 1) {
        const auto *status = activated()
                                 ? "2"
                                 : environmentValue("FAKE_REMOTE_STATUS", "2");
        std::cout << status << '\n';
        return EXIT_SUCCESS;
    }
    if (argc == 2 && std::string_view(argv[1]) == "-r") {
        if (std::string_view(environmentValue("FAKE_REMOTE_RELOAD", "ok")) ==
            "fail") {
            return EXIT_FAILURE;
        }
        markActivated();
        return EXIT_SUCCESS;
    }
    if (argc == 2 && std::string_view(argv[1]) == "-o") {
        return EXIT_SUCCESS;
    }
    if (argc == 3 && std::string_view(argv[1]) == "-s" &&
        std::string_view(argv[2]) == "modernime") {
        markActivated();
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
