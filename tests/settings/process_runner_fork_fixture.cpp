// Compile in isolation so allocator instrumentation never affects the main suite.
// Exercise the production reaper while it is destroying a queued path. A fork
// child must reach exec without destroying any inherited C++ objects.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <sys/wait.h>
#include <unistd.h>

namespace {
std::atomic<void *> watchedPath{nullptr};
std::atomic<bool> armed{false};
std::atomic<bool> deletionPaused{false};
std::atomic<bool> resumeDeletion{false};
pid_t ownerPid = 0;

void deleteTracked(void *ptr) noexcept {
    if (armed.load() && ptr == watchedPath.load() && ::getpid() != ownerPid &&
        deletionPaused.load()) {
        constexpr char message[] = "fork child tried to free an already freed reaper path\n";
        (void)::write(STDERR_FILENO, message, sizeof(message) - 1);
        // Detect the invalid second free independently of allocator/tcache policy.
        ::_exit(86);
    }
    std::free(ptr);
    if (armed.load() && ptr == watchedPath.load() && ::getpid() == ownerPid) {
        deletionPaused.store(true);
        while (!resumeDeletion.load()) ::usleep(1000);
    }
}
} // namespace

void *operator new(std::size_t size) {
    if (void *ptr = std::malloc(size == 0 ? 1 : size)) return ptr;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *ptr) noexcept { deleteTracked(ptr); }
void operator delete(void *ptr, std::size_t) noexcept { deleteTracked(ptr); }
void operator delete[](void *ptr) noexcept { deleteTracked(ptr); }
void operator delete[](void *ptr, std::size_t) noexcept { deleteTracked(ptr); }

// Include the actual implementation to place the deterministic allocator barrier
// on its queued path; no production test hooks or substitute reaper are involved.
#include "../../tools/settings/process_runner.cpp"

int main(int argc, char **argv) {
    using namespace modernime::settings::detail;
    if (argc != 2) return 2;
    ownerPid = ::getpid();
    int gate[2];
    if (::pipe(gate) != 0) return 3;
    const pid_t target = ::fork();
    if (target < 0) return 4;
    if (target == 0) {
        ::close(gate[1]);
        char byte;
        (void)::read(gate[0], &byte, 1);
        ::_exit(0);
    }
    ::close(gate[0]);
    const std::string ownPath = std::string(argv[1]) + "/" + std::string(128, 'x');
    std::filesystem::create_directory(ownPath);
    enqueuePendingReap(target, ownPath);
    {
        std::lock_guard<std::mutex> lock(g_reaperMutex);
        watchedPath.store(const_cast<char *>(g_pendingReaps.front().opDir.data()));
        armed.store(true);
    }
    (void)::write(gate[1], "R", 1);
    ::close(gate[1]);
    const auto pauseDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!deletionPaused.load() && std::chrono::steady_clock::now() < pauseDeadline) {
        ::usleep(1000);
    }
    if (!deletionPaused.load()) {
        armed.store(false);
        shutdownBackgroundReaper();
        return 5;
    }
    const pid_t child = ::fork();
    if (child == 0) {
        char arg0[] = "true";
        char *args[] = {arg0, nullptr};
        ::execv("/bin/true", args);
        ::_exit(127);
    }
    int status = 0;
    bool exited = false;
    const auto childDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (child > 0 && std::chrono::steady_clock::now() < childDeadline) {
        if (::waitpid(child, &status, WNOHANG) == child) {
            exited = true;
            break;
        }
        ::usleep(1000);
    }
    if (child > 0 && !exited) {
        ::kill(child, SIGKILL);
        ::waitpid(child, &status, 0);
    }
    armed.store(false);
    resumeDeletion.store(true);
    shutdownBackgroundReaper();
    std::printf("child_exited=%d child_exit=%d child_signal=%d\n", exited,
                exited && WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    if (!exited || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return 6;
    if (std::filesystem::exists(ownPath)) return 7;
    // Only the parent reaps its collector; it must already have done so.
    if (::waitpid(target, &status, WNOHANG) != -1 || errno != ECHILD) return 8;
    std::puts("FORK_EXEC_OK_PARENT_REAPED");
    return 0;
}
