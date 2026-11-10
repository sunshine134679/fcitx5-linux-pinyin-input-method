#include "clipboard_io_gate.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <sys/file.h>
#include <unistd.h>

namespace {
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
char target[PATH_MAX]{};
char lockTarget[PATH_MAX]{};
char tempPrefix[PATH_MAX]{};
char failureTarget[PATH_MAX]{};
int gateOperation = 0;
bool armed = false, entered = false, released = false, failReads = false;

void descriptorPath(int fd, char *path) {
    char link[64];
    std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    const auto n = ::readlink(link, path, PATH_MAX - 1);
    path[n >= 0 ? n : 0] = '\0';
}
void pauseAt(const char *path, int operation) {
    pthread_mutex_lock(&mutex);
    const bool matches = operation == clipboardBeforeFsync
        ? std::strncmp(path, tempPrefix, std::strlen(tempPrefix)) == 0
        : std::strcmp(path, operation == clipboardAfterUnlock ? lockTarget : target) == 0;
    if (armed && gateOperation == operation && matches) {
        armed = false;
        entered = true;
        pthread_cond_broadcast(&condition);
        while (!released) pthread_cond_wait(&condition, &mutex);
    }
    pthread_mutex_unlock(&mutex);
}
bool shouldFail(const char *path) {
    pthread_mutex_lock(&mutex);
    const bool result = failReads && std::strcmp(path, failureTarget) == 0;
    pthread_mutex_unlock(&mutex);
    return result;
}
}
extern "C" void modernime_clipboard_gate_arm(const char *path, int operation) {
    pthread_mutex_lock(&mutex);
    std::snprintf(target, sizeof(target), "%s", path);
    const auto *slash = std::strrchr(path, '/');
    const int parentLength = slash ? static_cast<int>(slash - path + 1) : 0;
    const char *name = slash ? slash + 1 : path;
    std::snprintf(lockTarget, sizeof(lockTarget), "%.*s.%s.lock", parentLength, path, name);
    std::snprintf(tempPrefix, sizeof(tempPrefix), "%.*s.%s.tmp.", parentLength, path, name);
    gateOperation = operation;
    armed = true; entered = false; released = false;
    pthread_mutex_unlock(&mutex);
}
extern "C" bool modernime_clipboard_gate_wait(unsigned timeoutMs) {
    timespec deadline{};
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeoutMs / 1000;
    deadline.tv_nsec += static_cast<long>(timeoutMs % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
    pthread_mutex_lock(&mutex);
    while (!entered) {
        if (pthread_cond_timedwait(&condition, &mutex, &deadline) == ETIMEDOUT) break;
    }
    const bool result = entered;
    pthread_mutex_unlock(&mutex);
    return result;
}
extern "C" void modernime_clipboard_gate_release() {
    pthread_mutex_lock(&mutex);
    released = true; armed = false;
    pthread_cond_broadcast(&condition);
    pthread_mutex_unlock(&mutex);
}
extern "C" void modernime_clipboard_fail_reads(const char *path, bool fail) {
    pthread_mutex_lock(&mutex);
    std::snprintf(failureTarget, sizeof(failureTarget), "%s", path);
    failReads = fail;
    pthread_mutex_unlock(&mutex);
}
extern "C" FILE *fopen(const char *path, const char *mode) {
    static auto real = reinterpret_cast<FILE *(*)(const char *, const char *)>(dlsym(RTLD_NEXT, "fopen"));
    if (shouldFail(path)) { errno = EACCES; return nullptr; }
    return real(path, mode);
}
extern "C" FILE *fopen64(const char *path, const char *mode) {
    static auto real = reinterpret_cast<FILE *(*)(const char *, const char *)>(dlsym(RTLD_NEXT, "fopen64"));
    if (shouldFail(path)) { errno = EACCES; return nullptr; }
    return real(path, mode);
}
extern "C" int fclose(FILE *file) {
    static auto real = reinterpret_cast<int (*)(FILE *)>(dlsym(RTLD_NEXT, "fclose"));
    char path[PATH_MAX]{};
    descriptorPath(::fileno(file), path);
    const int result = real(file);
    pauseAt(path, clipboardAfterRead);
    return result;
}
extern "C" int fsync(int fd) {
    static auto real = reinterpret_cast<int (*)(int)>(dlsym(RTLD_NEXT, "fsync"));
    char path[PATH_MAX]{};
    descriptorPath(fd, path);
    pauseAt(path, clipboardBeforeFsync);
    return real(fd);
}
extern "C" int close(int fd) {
    static auto real = reinterpret_cast<int (*)(int)>(dlsym(RTLD_NEXT, "close"));
    char path[PATH_MAX]{};
    descriptorPath(fd, path);
    const int result = real(fd);
    // The production lock guard releases flock by closing its descriptor.
    pauseAt(path, clipboardAfterUnlock);
    return result;
}
extern "C" int rename(const char *from, const char *to) {
    static auto real = reinterpret_cast<int (*)(const char *, const char *)>(dlsym(RTLD_NEXT, "rename"));
    if (shouldFail(from)) { errno = EACCES; return -1; }
    return real(from, to);
}
