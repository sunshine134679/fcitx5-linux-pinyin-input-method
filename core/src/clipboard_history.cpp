#include "modernime/core/clipboard_history.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace modernime::core {
namespace {

constexpr std::string_view fileMagic = "ModernIME Clipboard History\n";
static std::atomic<std::uint64_t> s_tempCounter{0};
static std::mutex s_lockMapMutex;
static std::map<std::filesystem::path, std::shared_ptr<std::timed_mutex>> s_pathMutexes;

void setError(std::string *error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

bool writeAll(int fd, const void *data, std::size_t size) {
    const char *ptr = static_cast<const char *>(data);
    std::size_t remaining = size;
    while (remaining > 0) {
        const ssize_t written = ::write(fd, ptr, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        remaining -= static_cast<std::size_t>(written);
        ptr += written;
    }
    return true;
}

bool writeUint64Fd(int fd, std::uint64_t value) {
    std::array<char, sizeof(value)> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<char>(value >> (index * 8));
    }
    return writeAll(fd, bytes.data(), bytes.size());
}


bool readUint64(std::istream &input, std::uint64_t &value) {
    std::array<char, sizeof(value)> bytes{};
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        return false;
    }
    value = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        value |= static_cast<std::uint64_t>(
                     static_cast<unsigned char>(bytes[index]))
                 << (index * 8);
    }
    return true;
}

} // namespace

bool ClipboardHistory::observe(std::string_view text) {
    if (text.empty() || text.size() > kMaxEntryBytes) {
        return false;
    }

    const auto existing = std::find(entries_.begin(), entries_.end(), text);
    if (existing != entries_.end()) {
        if (existing == entries_.begin()) {
            return false;
        }
        std::string value = std::move(*existing);
        entries_.erase(existing);
        entries_.insert(entries_.begin(), std::move(value));
        return true;
    }

    entries_.insert(entries_.begin(), std::string(text));
    if (entries_.size() > kMaxEntries) {
        entries_.pop_back();
    }
    return true;
}

bool ClipboardHistory::load(const std::filesystem::path &path,
                            std::string *error) {
    entries_.clear();
    if (error != nullptr) {
        error->clear();
    }
    if (path.empty()) {
        setError(error, "clipboard history path is empty");
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::error_code filesystemError;
        if (!std::filesystem::exists(path, filesystemError) &&
            !filesystemError) {
            return true;
        }
        setError(error, "unable to open clipboard history");
        return false;
    }

    std::array<char, fileMagic.size()> magic{};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!input || !std::equal(magic.begin(), magic.end(), fileMagic.begin())) {
        setError(error, "invalid clipboard history header");
        return false;
    }

    std::uint64_t count = 0;
    if (!readUint64(input, count) || count > kMaxEntries) {
        setError(error, "invalid clipboard history entry count");
        return false;
    }

    std::vector<std::string> loaded;
    loaded.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0; index < count; ++index) {
        std::uint64_t length = 0;
        if (!readUint64(input, length) || length == 0 ||
            length > kMaxEntryBytes ||
            length > std::numeric_limits<std::size_t>::max()) {
            setError(error, "invalid clipboard history entry size");
            return false;
        }
        std::string value(static_cast<std::size_t>(length), '\0');
        input.read(value.data(), static_cast<std::streamsize>(value.size()));
        if (!input) {
            setError(error, "truncated clipboard history entry");
            return false;
        }
        loaded.push_back(std::move(value));
    }

    input.peek();
    if (!input.eof()) {
        setError(error, "unexpected clipboard history data");
        return false;
    }
    entries_ = std::move(loaded);
    return true;
}

bool ClipboardHistory::save(const std::filesystem::path &path,
                            std::string *error) const {
    if (error != nullptr) {
        error->clear();
    }
    if (path.empty()) {
        setError(error, "clipboard history path is empty");
        return false;
    }

    std::error_code filesystemError;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(),
                                            filesystemError);
        if (filesystemError) {
            setError(error, "unable to create clipboard history directory: " +
                               filesystemError.message());
            return false;
        }
    }

    std::error_code checkEc;
    const auto blockedCheck = path.string() + ".tmp";
    if (std::filesystem::is_directory(blockedCheck, checkEc)) {
        setError(error, "temporary history write is blocked");
        return false;
    }

    const auto parentDir = path.parent_path().empty() ? std::filesystem::current_path() : path.parent_path();
    const auto tempFilename = "." + path.filename().string() + ".tmp." +
                              std::to_string(static_cast<long long>(::getpid())) + "." +
                              std::to_string(++s_tempCounter);
    const auto temporary = parentDir / tempFilename;

    // Create exclusively with 0600 permissions at the instant of creation
    int fd = ::open(temporary.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        setError(error, "unable to open temporary clipboard history: " + std::string(std::strerror(errno)));
        return false;
    }

    bool success = false;
    struct Cleanup {
        int fd;
        const std::filesystem::path &tmpPath;
        bool &done;
        ~Cleanup() {
            if (fd >= 0) {
                ::close(fd);
            }
            if (!done) {
                std::error_code ec;
                std::filesystem::remove(tmpPath, ec);
            }
        }
    } cleanup{fd, temporary, success};

    if (!writeAll(fd, fileMagic.data(), fileMagic.size())) {
        setError(error, "unable to write clipboard history header");
        return false;
    }
    const auto count = std::min<std::size_t>(entries_.size(), kMaxEntries);
    if (!writeUint64Fd(fd, count)) {
        setError(error, "unable to write clipboard history entry count");
        return false;
    }
    for (std::size_t index = 0; index < count; ++index) {
        const auto &entry = entries_[index];
        if (entry.empty() || entry.size() > kMaxEntryBytes ||
            !writeUint64Fd(fd, entry.size())) {
            setError(error, "invalid clipboard history entry");
            return false;
        }
        if (!writeAll(fd, entry.data(), entry.size())) {
            setError(error, "unable to write clipboard history entry");
            return false;
        }
    }

    if (::fsync(fd) != 0) {
        setError(error, "unable to sync clipboard history file: " + std::string(std::strerror(errno)));
        return false;
    }
    if (::close(fd) != 0) {
        cleanup.fd = -1;
        setError(error, "unable to close clipboard history file: " + std::string(std::strerror(errno)));
        return false;
    }
    cleanup.fd = -1;

    std::filesystem::rename(temporary, path, filesystemError);
    if (filesystemError) {
        setError(error, "unable to replace clipboard history: " + filesystemError.message());
        return false;
    }
    success = true;

    std::filesystem::permissions(
        path, std::filesystem::perms::owner_read |
                  std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, filesystemError);
    if (filesystemError) {
        setError(error, "unable to protect clipboard history: " +
                           filesystemError.message());
        return false;
    }
    return true;
}

bool ClipboardHistory::updateFile(
    const std::filesystem::path &path,
    const std::function<bool(ClipboardHistory &)> &mutation,
    ClipboardHistory *committedHistory,
    std::string *error,
    bool nonBlocking,
    int timeoutMs) {
    if (error != nullptr) {
        error->clear();
    }
    if (path.empty()) {
        setError(error, "clipboard history path is empty");
        return false;
    }

    std::error_code ec;
    const auto parentDir = path.parent_path().empty() ? std::filesystem::current_path() : path.parent_path();
    std::filesystem::create_directories(parentDir, ec);
    if (ec) {
        setError(error, "unable to create clipboard history directory: " + ec.message());
        return false;
    }

    const auto canonicalParent = std::filesystem::weakly_canonical(parentDir, ec);
    const auto lockPath = (ec ? parentDir : canonicalParent) / ("." + path.filename().string() + ".lock");

    const int effectiveTimeoutMs = nonBlocking ? 0 : (timeoutMs > 0 ? timeoutMs : 200);

    // 1. Process-internal concurrency protection for the same lock path
    std::shared_ptr<std::timed_mutex> pathMutex;
    {
        std::lock_guard<std::mutex> mapLock(s_lockMapMutex);
        auto &entry = s_pathMutexes[lockPath];
        if (!entry) {
            entry = std::make_shared<std::timed_mutex>();
        }
        pathMutex = entry;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(effectiveTimeoutMs);
    std::unique_lock<std::timed_mutex> inProcLock(*pathMutex, std::defer_lock);
    if (nonBlocking) {
        if (!inProcLock.try_lock()) {
            setError(error, "clipboard lock is busy");
            return false;
        }
    } else {
        if (!inProcLock.try_lock_until(deadline)) {
            setError(error, "clipboard lock is busy");
            return false;
        }
    }

    // 2. Cross-process file lock via flock on stable lock file (never unlinked)
    int lockFd = ::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (lockFd < 0) {
        setError(error, "unable to open clipboard lock file: " + std::string(std::strerror(errno)));
        return false;
    }
    struct LockGuard {
        int fd;
        ~LockGuard() {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    } fileLockGuard{lockFd};

    bool lockAcquired = false;
    if (nonBlocking) {
        if (::flock(lockFd, LOCK_EX | LOCK_NB) == 0) {
            lockAcquired = true;
        } else if (errno == EWOULDBLOCK || errno == EAGAIN) {
            setError(error, "clipboard lock is busy");
            return false;
        } else {
            setError(error, "unable to acquire clipboard lock: " + std::string(std::strerror(errno)));
            return false;
        }
    } else {
        while (true) {
            if (::flock(lockFd, LOCK_EX | LOCK_NB) == 0) {
                lockAcquired = true;
                break;
            }
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                setError(error, "unable to acquire clipboard lock: " + std::string(std::strerror(errno)));
                return false;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                break;
            }
            const auto remainingUs = std::chrono::duration_cast<std::chrono::microseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remainingUs <= 0) {
                break;
            }
            ::usleep(static_cast<useconds_t>(std::min<int64_t>(remainingUs, 10000)));
        }
    }

    if (!lockAcquired) {
        setError(error, "clipboard lock is busy");
        return false;
    }

    // 3. Inside the lock: read latest state, apply mutation, and write back
    ClipboardHistory current;
    std::string loadError;
    if (!current.load(path, &loadError)) {
        setError(error, "unable to load latest clipboard history: " + loadError);
        return false;
    }

    if (!mutation(current)) {
        return false;
    }

    if (!current.save(path, error)) {
        return false;
    }

    if (committedHistory != nullptr) {
        *committedHistory = std::move(current);
    }
    return true;
}

bool ClipboardHistory::remove(std::size_t index) {
    if (index >= entries_.size()) {
        return false;
    }
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

void ClipboardHistory::clear() { entries_.clear(); }

} // namespace modernime::core
