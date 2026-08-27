#include <cstdlib>
#include <iostream>
#include <string_view>

int main(int argc, char **argv) {
    if (argc == 2 && std::string_view(argv[1]) == "-n") {
        std::cout << "modernime\n";
        return EXIT_SUCCESS;
    }
    if (argc == 1) {
        std::cout << "2\n";
        return EXIT_SUCCESS;
    }
    if (argc == 2 && std::string_view(argv[1]) == "-r") {
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
