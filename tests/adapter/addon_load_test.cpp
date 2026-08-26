#include <dlfcn.h>

#include <cstdlib>
#include <iostream>

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "expected an addon path\n";
        return EXIT_FAILURE;
    }
    void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        std::cerr << "unable to load addon: " << dlerror() << '\n';
        return EXIT_FAILURE;
    }
    dlclose(handle);
    return EXIT_SUCCESS;
}
