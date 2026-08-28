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

} // namespace

int main(int argc, char **argv) {
    logInvocation(argc, argv);
    if (argc == 2 && std::string_view(argv[1]) == "-n") {
        std::cout << "modernime\n";
        return EXIT_SUCCESS;
    }
    if (argc == 1) {
        std::cout << "2\n";
        return EXIT_SUCCESS;
    }
    if (argc == 2 && std::string_view(argv[1]) == "-o") {
        return EXIT_SUCCESS;
    }
    if (argc == 3 && std::string_view(argv[1]) == "-s" &&
        std::string_view(argv[2]) == "modernime") {
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
