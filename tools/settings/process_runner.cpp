#include "modernime/settings/detail/process_runner.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

namespace modernime::settings::detail {
namespace {

std::mutex g_opDirsMutex;
std::map<std::uint64_t, std::string> g_activeOpDirs;

std::string toHex(std::string_view s) {
    static const char hexChars[] = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        out.push_back(hexChars[c >> 4]);
        out.push_back(hexChars[c & 0x0f]);
    }
    return out;
}

std::string fromHex(std::string_view hex) {
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    std::string out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<char>((hexVal(hex[i]) << 4) | hexVal(hex[i + 1])));
    }
    return out;
}

bool isPidAlive(pid_t pid) {
    if (pid <= 0) {
        return false;
    }
    if (::kill(pid, 0) != 0) {
        return false;
    }
    std::ifstream statIn("/proc/" + std::to_string(pid) + "/stat");
    if (statIn) {
        std::string statLine;
        std::getline(statIn, statLine);
        const auto rparen = statLine.rfind(')');
        if (rparen != std::string::npos && rparen + 2 < statLine.size()) {
            const char procState = statLine[rparen + 2];
            if (procState == 'Z' || procState == 'X' || procState == 'x') {
                return false;
            }
        }
    }
    return true;
}

std::string getTempDirectory() {
    const char *tmp = std::getenv("TMPDIR");
    if (tmp != nullptr && *tmp != '\0') {
        return tmp;
    }
    return "/tmp";
}

std::string resolveSettingsExecutable(const Environment &environment) {
    for (const auto &[k, v] : environment) {
        if (k == "MODERNIME_SETTINGS_EXECUTABLE" && !v.empty()) {
            return v;
        }
    }
    if (const char *envVal = std::getenv("MODERNIME_SETTINGS_EXECUTABLE")) {
        if (*envVal != '\0') {
            return envVal;
        }
    }

    char exeBuf[PATH_MAX];
    const ssize_t len = ::readlink("/proc/self/exe", exeBuf, sizeof(exeBuf) - 1);
    if (len > 0) {
        exeBuf[len] = '\0';
        std::filesystem::path selfPath(exeBuf);
        if (selfPath.filename() == "modernime-settings") {
            return selfPath.string();
        }
        std::error_code ec;
        auto sibling = selfPath.parent_path().parent_path() / "tools/settings/modernime-settings";
        if (std::filesystem::is_regular_file(sibling, ec) && ::access(sibling.c_str(), X_OK) == 0) {
            return sibling.string();
        }
    }

#ifdef MODERNIME_INSTALL_PREFIX
    const std::string installed = std::string(MODERNIME_INSTALL_PREFIX) + "/bin/modernime-settings";
    if (::access(installed.c_str(), X_OK) == 0) {
        return installed;
    }
#endif

    return "modernime-settings";
}

bool writeAtomically(const std::string &path, const std::string &content, mode_t mode = 0600) {
    const std::string tmpPath = path + ".tmp";
    const int fd = ::open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) {
        return false;
    }
    ::fchmod(fd, mode);
    size_t written = 0;
    while (written < content.size()) {
        const ssize_t n = ::write(fd, content.data() + written, content.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            ::unlink(tmpPath.c_str());
            return false;
        }
        written += static_cast<size_t>(n);
    }
    ::fsync(fd);
    ::close(fd);
    return ::rename(tmpPath.c_str(), path.c_str()) == 0;
}

std::string readFileBounded(const std::string &path, std::size_t maxBytes) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return {};
    }
    std::string content;
    char buffer[4096];
    while (content.size() < maxBytes) {
        const size_t toRead = std::min(sizeof(buffer), maxBytes - content.size());
        const ssize_t n = ::read(fd, buffer, toRead);
        if (n > 0) {
            content.append(buffer, static_cast<size_t>(n));
        } else if (n == 0) {
            break;
        } else if (errno != EINTR) {
            break;
        }
    }
    ::close(fd);
    return content;
}

struct ParsedState {
    std::uint64_t operationId = 0;
    pid_t collectorPid = 0;
    pid_t childPid = 0;
    std::string stateStr;
    int exitStatus = -1;
    bool stdoutTruncated = false;
    bool stderrTruncated = false;
    std::size_t stdoutBytes = 0;
    std::size_t stderrBytes = 0;
    std::size_t bufferPeak = 0;
    std::size_t filePeak = 0;
    bool valid = false;
};

ParsedState parseStateFile(const std::string &path) {
    ParsedState s;
    const std::string content = readFileBounded(path, 4096);
    if (content.empty()) {
        return s;
    }

    auto extractStr = [&](std::string_view key) -> std::string {
        const auto pos = content.find("\"" + std::string(key) + "\"");
        if (pos == std::string::npos) return {};
        const auto colon = content.find(':', pos);
        if (colon == std::string::npos) return {};
        const auto quote1 = content.find('"', colon);
        if (quote1 == std::string::npos) return {};
        const auto quote2 = content.find('"', quote1 + 1);
        if (quote2 == std::string::npos) return {};
        return content.substr(quote1 + 1, quote2 - quote1 - 1);
    };

    auto extractInt = [&](std::string_view key, long defaultVal = 0) -> long {
        const auto pos = content.find("\"" + std::string(key) + "\"");
        if (pos == std::string::npos) return defaultVal;
        const auto colon = content.find(':', pos);
        if (colon == std::string::npos) return defaultVal;
        auto start = content.find_first_not_of(" \t\r\n", colon + 1);
        if (start == std::string::npos) return defaultVal;
        char *endPtr = nullptr;
        const long val = std::strtol(content.c_str() + start, &endPtr, 10);
        return val;
    };

    auto extractBool = [&](std::string_view key) -> bool {
        const auto pos = content.find("\"" + std::string(key) + "\"");
        if (pos == std::string::npos) return false;
        const auto colon = content.find(':', pos);
        if (colon == std::string::npos) return false;
        const auto start = content.find_first_not_of(" \t\r\n", colon + 1);
        if (start == std::string::npos) return false;
        return content.substr(start, 4) == "true";
    };

    s.operationId = static_cast<std::uint64_t>(extractInt("operationId"));
    s.collectorPid = static_cast<pid_t>(extractInt("collectorPid"));
    s.childPid = static_cast<pid_t>(extractInt("childPid"));
    s.stateStr = extractStr("state");
    s.exitStatus = static_cast<int>(extractInt("exitStatus", -1));
    s.stdoutTruncated = extractBool("stdoutTruncated");
    s.stderrTruncated = extractBool("stderrTruncated");
    s.stdoutBytes = static_cast<std::size_t>(extractInt("stdoutBytes"));
    s.stderrBytes = static_cast<std::size_t>(extractInt("stderrBytes"));
    s.bufferPeak = static_cast<std::size_t>(extractInt("bufferPeak"));
    s.filePeak = static_cast<std::size_t>(extractInt("filePeak"));
    s.valid = !s.stateStr.empty() && s.operationId != 0;
    return s;
}

CommandResult runQueryCommand(const CommandSpec &spec) {
    CommandResult result;
    result.operationId = spec.operationId;
    if (spec.argv.empty()) {
        result.state = CommandState::NotStarted;
        result.stderrText = "Empty command argv";
        return result;
    }

    int outPipe[2];
    int errPipe[2];
    if (::pipe2(outPipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        result.state = CommandState::NotStarted;
        result.stderrText = "Failed to create stdout pipe";
        return result;
    }
    if (::pipe2(errPipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        result.state = CommandState::NotStarted;
        result.stderrText = "Failed to create stderr pipe";
        return result;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        ::close(errPipe[0]);
        ::close(errPipe[1]);
        result.state = CommandState::NotStarted;
        result.stderrText = "Failed to fork child process";
        return result;
    }

    if (pid == 0) {
        // Child
        ::dup2(outPipe[1], STDOUT_FILENO);
        ::dup2(errPipe[1], STDERR_FILENO);
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        ::close(errPipe[0]);
        ::close(errPipe[1]);

        const int fOut = ::fcntl(STDOUT_FILENO, F_GETFL);
        if (fOut >= 0) ::fcntl(STDOUT_FILENO, F_SETFL, fOut & ~O_NONBLOCK);
        const int fErr = ::fcntl(STDERR_FILENO, F_GETFL);
        if (fErr >= 0) ::fcntl(STDERR_FILENO, F_SETFL, fErr & ~O_NONBLOCK);

        std::vector<std::string> envStrings;
        envStrings.push_back("LC_ALL=C");
        envStrings.push_back("LANG=C");
        bool pathFound = false;
        for (const auto &[k, v] : spec.environment) {
            if (k == "PATH") {
                pathFound = true;
                envStrings.push_back(v.empty() ? "PATH=/usr/bin:/bin" : ("PATH=" + v + ":/usr/bin:/bin"));
            } else {
                envStrings.push_back(k + "=" + v);
            }
        }
        if (!pathFound) {
            const char *origPath = std::getenv("PATH");
            envStrings.push_back(std::string("PATH=") + (origPath != nullptr ? origPath : "/usr/bin:/bin"));
        }

        std::vector<char *> envp;
        envp.reserve(envStrings.size() + 1);
        for (auto &s : envStrings) {
            envp.push_back(s.data());
        }
        envp.push_back(nullptr);

        std::vector<char *> argvPtrs;
        argvPtrs.reserve(spec.argv.size() + 1);
        for (const auto &arg : spec.argv) {
            argvPtrs.push_back(const_cast<char *>(arg.c_str()));
        }
        argvPtrs.push_back(nullptr);

        ::execvpe(argvPtrs[0], argvPtrs.data(), envp.data());
        const int err = errno;
        ::dprintf(STDERR_FILENO, "Failed to execute %s: %s\n", argvPtrs[0], std::strerror(err));
        ::_exit(127);
    }

    // Parent
    ::close(outPipe[1]);
    ::close(errPipe[1]);

    const auto startTime = std::chrono::steady_clock::now();
    const auto deadline = startTime + std::chrono::milliseconds(spec.timeoutMs);

    std::string stdoutText;
    std::string stderrText;
    std::size_t stdoutTotal = 0;
    std::size_t stderrTotal = 0;
    const std::size_t maxBuffer = spec.limits.perStreamBufferBytes;
    const std::size_t maxResult = spec.limits.perStreamResultBytes;

    bool leaderExited = false;
    int childStatus = 0;
    bool outOpen = true;
    bool errOpen = true;

    char discardBuf[4096];

    while (outOpen || errOpen) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            result.timedOut = true;
            // Timeout reached! Kill query child leader.
            if (!leaderExited) {
                ::kill(pid, SIGKILL);
                ::waitpid(pid, &childStatus, 0);
                leaderExited = true;
            }
            break;
        }

        // Check if leader exited
        if (!leaderExited) {
            const pid_t w = ::waitpid(pid, &childStatus, WNOHANG);
            if (w == pid) {
                leaderExited = true;
            }
        }

        struct pollfd fds[2];
        nfds_t nfds = 0;
        int outIdx = -1;
        int errIdx = -1;

        if (outOpen) {
            outIdx = static_cast<int>(nfds);
            fds[nfds].fd = outPipe[0];
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            ++nfds;
        }
        if (errOpen) {
            errIdx = static_cast<int>(nfds);
            fds[nfds].fd = errPipe[0];
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            ++nfds;
        }

        const int pollTimeout = leaderExited ? 50 : 100;
        const int pRet = ::poll(fds, nfds, pollTimeout);

        if (pRet > 0) {
            if (outIdx >= 0 && (fds[outIdx].revents & (POLLIN | POLLHUP | POLLERR))) {
                while (true) {
                    char buf[4096];
                    const ssize_t n = ::read(outPipe[0], buf, sizeof(buf));
                    if (n > 0) {
                        stdoutTotal += static_cast<size_t>(n);
                        if (stdoutTotal > maxBuffer) {
                            result.stdoutTruncated = true;
                        }
                        if (stdoutText.size() < maxResult) {
                            const size_t take = std::min(static_cast<size_t>(n), maxResult - stdoutText.size());
                            stdoutText.append(buf, take);
                        }
                    } else if (n == 0) {
                        outOpen = false;
                        break;
                    } else {
                        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                            outOpen = false;
                        }
                        break;
                    }
                }
            }
            if (errIdx >= 0 && (fds[errIdx].revents & (POLLIN | POLLHUP | POLLERR))) {
                while (true) {
                    char buf[4096];
                    const ssize_t n = ::read(errPipe[0], buf, sizeof(buf));
                    if (n > 0) {
                        stderrTotal += static_cast<size_t>(n);
                        if (stderrTotal > maxBuffer) {
                            result.stderrTruncated = true;
                        }
                        if (stderrText.size() < maxResult) {
                            const size_t take = std::min(static_cast<size_t>(n), maxResult - stderrText.size());
                            stderrText.append(buf, take);
                        }
                    } else if (n == 0) {
                        errOpen = false;
                        break;
                    } else {
                        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                            errOpen = false;
                        }
                        break;
                    }
                }
            }
        }

        // T13-A02: If leader already exited, drain with short deadline, don't wait for descendant FDs!
        if (leaderExited) {
            // Drain non-blocking one more time
            while (outOpen) {
                const ssize_t n = ::read(outPipe[0], discardBuf, sizeof(discardBuf));
                if (n > 0) {
                    stdoutTotal += n;
                    if (stdoutTotal > maxBuffer) result.stdoutTruncated = true;
                    if (stdoutText.size() < maxResult) {
                        stdoutText.append(discardBuf, std::min(static_cast<size_t>(n), maxResult - stdoutText.size()));
                    }
                } else {
                    break;
                }
            }
            while (errOpen) {
                const ssize_t n = ::read(errPipe[0], discardBuf, sizeof(discardBuf));
                if (n > 0) {
                    stderrTotal += n;
                    if (stderrTotal > maxBuffer) result.stderrTruncated = true;
                    if (stderrText.size() < maxResult) {
                        stderrText.append(discardBuf, std::min(static_cast<size_t>(n), maxResult - stderrText.size()));
                    }
                } else {
                    break;
                }
            }
            break; // Stop waiting! Descendants will not hang Query!
        }
    }

    ::close(outPipe[0]);
    ::close(errPipe[0]);

    if (!leaderExited) {
        int w = ::waitpid(pid, &childStatus, WNOHANG);
        if (w == 0) {
            for (int i = 0; i < 50; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                w = ::waitpid(pid, &childStatus, WNOHANG);
                if (w == pid) break;
            }
        }
        if (w != pid) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &childStatus, 0);
            result.exitStatus = -1;
        } else {
            result.exitStatus = WIFEXITED(childStatus) ? WEXITSTATUS(childStatus) : -1;
        }
    } else {
        result.exitStatus = WIFEXITED(childStatus) ? WEXITSTATUS(childStatus) : -1;
    }

    result.state = CommandState::Exited;
    result.stdoutText = std::move(stdoutText);
    result.stderrText = std::move(stderrText);
    return result;
}

CommandResult runPackageTransactionCommand(const CommandSpec &spec) {
    CommandResult result;
    result.operationId = spec.operationId;

    if (spec.argv.empty()) {
        result.state = CommandState::NotStarted;
        result.stderrText = "Empty package operation argv";
        return result;
    }

    const std::string tmpDir = getTempDirectory();
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    const std::string opDir = tmpDir + "/modernime-op-" + std::to_string(spec.operationId) +
                              "-" + std::to_string(nowMs) + "-" + std::to_string(::getpid());

    if (::mkdir(opDir.c_str(), 0700) != 0) {
        result.state = CommandState::NotStarted;
        result.stderrText = "Failed to create private operation directory";
        return result;
    }

    {
        std::lock_guard<std::mutex> lock(g_opDirsMutex);
        g_activeOpDirs[spec.operationId] = opDir;
    }

    // Write spec.dat (0600)
    std::ostringstream ss;
    ss << "operationId=" << spec.operationId << "\n";
    ss << "timeoutMs=" << spec.timeoutMs << "\n";
    ss << "mode=" << static_cast<int>(spec.mode) << "\n";
    ss << "perStreamBufferBytes=" << spec.limits.perStreamBufferBytes << "\n";
    ss << "perStreamResultBytes=" << spec.limits.perStreamResultBytes << "\n";
    ss << "perStreamFileBytes=" << spec.limits.perStreamFileBytes << "\n";
    ss << "argc=" << spec.argv.size() << "\n";
    for (const auto &arg : spec.argv) {
        ss << "argv_hex=" << toHex(arg) << "\n";
    }
    ss << "envc=" << spec.environment.size() << "\n";
    for (const auto &[k, v] : spec.environment) {
        ss << "env_hex=" << toHex(k) << "=" << toHex(v) << "\n";
    }

    if (!writeAtomically(opDir + "/spec.dat", ss.str(), 0600)) {
        result.state = CommandState::NotStarted;
        result.stderrText = "Failed to write operation spec file";
        return result;
    }

    int handshakeFds[2];
    if (::pipe2(handshakeFds, O_CLOEXEC) != 0) {
        result.state = CommandState::NotStarted;
        result.stderrText = "Failed to create handshake pipe";
        return result;
    }

    const std::string settingsExe = resolveSettingsExecutable(spec.environment);

    const pid_t collectorPid = ::fork();
    if (collectorPid < 0) {
        ::close(handshakeFds[0]);
        ::close(handshakeFds[1]);
        result.state = CommandState::NotStarted;
        result.stderrText = "Failed to fork collector process";
        return result;
    }

    if (collectorPid == 0) {
        // Collector child
        ::close(handshakeFds[0]);
        // Remove CLOEXEC from handshake write fd so it is inherited across exec
        const int flags = ::fcntl(handshakeFds[1], F_GETFD);
        if (flags >= 0) {
            ::fcntl(handshakeFds[1], F_SETFD, flags & ~FD_CLOEXEC);
        }

        std::vector<std::string> cArgs = {
            settingsExe,
            "--internal-capture-collector",
            "--op-dir", opDir,
            "--handshake-fd", std::to_string(handshakeFds[1])
        };
        std::vector<char *> cArgv;
        cArgv.reserve(cArgs.size() + 1);
        for (const auto &a : cArgs) {
            cArgv.push_back(const_cast<char *>(a.c_str()));
        }
        cArgv.push_back(nullptr);

        ::execvp(cArgv[0], cArgv.data());
        ::_exit(127);
    }

    ::close(handshakeFds[1]);

    // Read handshake byte
    char handshakeByte = 0;
    const ssize_t hRead = ::read(handshakeFds[0], &handshakeByte, 1);
    ::close(handshakeFds[0]);

    if (hRead != 1 || handshakeByte != '1') {
        // T13-A07: Collector start failure -> 0 spawn of real command!
        result.state = CommandState::NotStarted;
        result.exitStatus = -1;
        result.stderrText = "Collector process failed to start or handshake";
        // Reap collector if needed
        int st = 0;
        ::waitpid(collectorPid, &st, WNOHANG);
        return result;
    }

    // Wait loop up to spec.timeoutMs
    const auto startTime = std::chrono::steady_clock::now();
    const auto deadline = startTime + std::chrono::milliseconds(spec.timeoutMs);
    const std::string statePath = opDir + "/state.json";

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        const ParsedState ps = parseStateFile(statePath);

        if (ps.valid && ps.operationId == spec.operationId) {
            if (ps.stateStr == "Exited" || ps.stateStr == "Unknown") {
                result.state = (ps.stateStr == "Exited") ? CommandState::Exited : CommandState::Unknown;
                result.exitStatus = ps.exitStatus;
                result.stdoutTruncated = ps.stdoutTruncated;
                result.stderrTruncated = ps.stderrTruncated;
                result.stdoutText = readFileBounded(opDir + "/stdout.tail", spec.limits.perStreamResultBytes);
                result.stderrText = readFileBounded(opDir + "/stderr.tail", spec.limits.perStreamResultBytes);
                return result;
            }
        }

        // Check if collector is still alive
        if (!isPidAlive(collectorPid)) {
            // Re-check state once more in case it just exited
            const ParsedState psFinal = parseStateFile(statePath);
            if (psFinal.valid && psFinal.operationId == spec.operationId &&
                (psFinal.stateStr == "Exited" || psFinal.stateStr == "Unknown")) {
                result.state = (psFinal.stateStr == "Exited") ? CommandState::Exited : CommandState::Unknown;
                result.exitStatus = psFinal.exitStatus;
                result.stdoutTruncated = psFinal.stdoutTruncated;
                result.stderrTruncated = psFinal.stderrTruncated;
                result.stdoutText = readFileBounded(opDir + "/stdout.tail", spec.limits.perStreamResultBytes);
                result.stderrText = readFileBounded(opDir + "/stderr.tail", spec.limits.perStreamResultBytes);
                return result;
            }
            // T13-A08: Collector exited abnormally / corrupted -> Unknown!
            result.state = CommandState::Unknown;
            result.exitStatus = -1;
            result.stderrText = "Collector terminated abnormally or status unreadable";
            return result;
        }

        if (now >= deadline) {
            result.timedOut = true;
            const bool childAlive = (ps.childPid > 0 && isPidAlive(ps.childPid));
            if (childAlive) {
                // T13-A05: Timeout -> Continuing! Do NOT kill collector or child!
                result.state = CommandState::Continuing;
                return result;
            }
            // Child is already gone or zombie!
            result.state = CommandState::Exited;
            result.exitStatus = ps.exitStatus;
            result.stdoutTruncated = ps.stdoutTruncated;
            result.stderrTruncated = ps.stderrTruncated;
            result.stdoutText = readFileBounded(opDir + "/stdout.tail", spec.limits.perStreamResultBytes);
            result.stderrText = readFileBounded(opDir + "/stderr.tail", spec.limits.perStreamResultBytes);
            return result;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

} // namespace

CommandResult runCommand(const CommandSpec &spec) {
    if (spec.mode == CommandMode::Query) {
        return runQueryCommand(spec);
    }
    return runPackageTransactionCommand(spec);
}

int runCommandCaptureChild(int argc, char **argv) {
    // Detach from parent session and ignore SIGPIPE
    ::setsid();
    ::signal(SIGPIPE, SIG_IGN);

    std::string opDir;
    int handshakeFd = -1;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--op-dir" && i + 1 < argc) {
            opDir = argv[++i];
        } else if (arg == "--handshake-fd" && i + 1 < argc) {
            handshakeFd = std::atoi(argv[++i]);
        }
    }

    if (opDir.empty() || handshakeFd < 0) {
        if (handshakeFd >= 0) {
            const char errByte = '0';
            (void)::write(handshakeFd, &errByte, 1);
            ::close(handshakeFd);
        }
        return 1;
    }

    const std::string specPath = opDir + "/spec.dat";
    std::ifstream in(specPath);
    if (!in) {
        const char errByte = '0';
        (void)::write(handshakeFd, &errByte, 1);
        ::close(handshakeFd);
        return 1;
    }

    std::uint64_t operationId = 0;
    std::size_t perStreamBufferBytes = 1048576;
    std::size_t perStreamResultBytes = 65536;
    std::size_t perStreamFileBytes = 1048576;
    std::vector<std::string> childArgv;
    Environment childEnv;

    std::string line;
    while (std::getline(in, line)) {
        if (line.starts_with("operationId=")) {
            operationId = std::stoull(line.substr(12));
        } else if (line.starts_with("perStreamBufferBytes=")) {
            perStreamBufferBytes = std::stoul(line.substr(21));
            (void)perStreamBufferBytes;
        } else if (line.starts_with("perStreamResultBytes=")) {
            perStreamResultBytes = std::stoul(line.substr(21));
        } else if (line.starts_with("perStreamFileBytes=")) {
            perStreamFileBytes = std::stoul(line.substr(19));
        } else if (line.starts_with("argv_hex=")) {
            childArgv.push_back(fromHex(line.substr(9)));
        } else if (line.starts_with("argv=")) {
            childArgv.push_back(line.substr(5));
        } else if (line.starts_with("env_hex=")) {
            const auto eq = line.find('=', 8);
            if (eq != std::string::npos) {
                childEnv.emplace_back(fromHex(line.substr(8, eq - 8)), fromHex(line.substr(eq + 1)));
            }
        } else if (line.starts_with("env=")) {
            const auto eq = line.find('=', 4);
            if (eq != std::string::npos) {
                childEnv.emplace_back(line.substr(4, eq - 4), line.substr(eq + 1));
            }
        }
    }

    if (childArgv.empty() || operationId == 0) {
        const char errByte = '0';
        (void)::write(handshakeFd, &errByte, 1);
        ::close(handshakeFd);
        return 1;
    }

    // Open diagnostic files (0600)
    const int diagOutFd = ::open((opDir + "/stdout.diag").c_str(),
                                 O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    const int diagErrFd = ::open((opDir + "/stderr.diag").c_str(),
                                 O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

    int outPipe[2];
    int errPipe[2];
    if (::pipe2(outPipe, O_CLOEXEC | O_NONBLOCK) != 0 || ::pipe2(errPipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        const char errByte = '0';
        (void)::write(handshakeFd, &errByte, 1);
        ::close(handshakeFd);
        if (diagOutFd >= 0) ::close(diagOutFd);
        if (diagErrFd >= 0) ::close(diagErrFd);
        return 1;
    }

    const pid_t childPid = ::fork();
    if (childPid < 0) {
        const char errByte = '0';
        (void)::write(handshakeFd, &errByte, 1);
        ::close(handshakeFd);
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        ::close(errPipe[0]);
        ::close(errPipe[1]);
        if (diagOutFd >= 0) ::close(diagOutFd);
        if (diagErrFd >= 0) ::close(diagErrFd);
        return 1;
    }

    if (childPid == 0) {
        // In real target command process
        ::dup2(outPipe[1], STDOUT_FILENO);
        ::dup2(errPipe[1], STDERR_FILENO);
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        ::close(errPipe[0]);
        ::close(errPipe[1]);
        const int fOut = ::fcntl(STDOUT_FILENO, F_GETFL);
        if (fOut >= 0) ::fcntl(STDOUT_FILENO, F_SETFL, fOut & ~O_NONBLOCK);
        const int fErr = ::fcntl(STDERR_FILENO, F_GETFL);
        if (fErr >= 0) ::fcntl(STDERR_FILENO, F_SETFL, fErr & ~O_NONBLOCK);
        if (diagOutFd >= 0) ::close(diagOutFd);
        if (diagErrFd >= 0) ::close(diagErrFd);
        ::close(handshakeFd);

        std::vector<std::string> envStrings;
        envStrings.push_back("LC_ALL=C");
        envStrings.push_back("LANG=C");
        bool pathFound = false;
        for (const auto &[k, v] : childEnv) {
            if (k == "PATH") {
                pathFound = true;
                envStrings.push_back(v.empty() ? "PATH=/usr/bin:/bin" : ("PATH=" + v + ":/usr/bin:/bin"));
            } else {
                envStrings.push_back(k + "=" + v);
            }
        }
        if (!pathFound) {
            const char *origPath = std::getenv("PATH");
            envStrings.push_back(std::string("PATH=") + (origPath != nullptr ? origPath : "/usr/bin:/bin"));
        }

        std::vector<char *> envp;
        envp.reserve(envStrings.size() + 1);
        for (auto &s : envStrings) {
            envp.push_back(s.data());
        }
        envp.push_back(nullptr);

        std::vector<char *> argvPtrs;
        argvPtrs.reserve(childArgv.size() + 1);
        for (const auto &arg : childArgv) {
            argvPtrs.push_back(const_cast<char *>(arg.c_str()));
        }
        argvPtrs.push_back(nullptr);

        ::execvpe(argvPtrs[0], argvPtrs.data(), envp.data());
        const int err = errno;
        ::dprintf(STDERR_FILENO, "Failed to execute child process \"%s\": %s\n", argvPtrs[0], std::strerror(err));
        ::_exit(127);
    }

    ::close(outPipe[1]);
    ::close(errPipe[1]);

    // Target command spawn succeeded!
    // Write initial state.json (mode 0600)
    std::ostringstream initSs;
    initSs << "{\n"
           << "  \"operationId\": " << operationId << ",\n"
           << "  \"collectorPid\": " << ::getpid() << ",\n"
           << "  \"childPid\": " << childPid << ",\n"
           << "  \"state\": \"Running\",\n"
           << "  \"exitStatus\": -1,\n"
           << "  \"stdoutTruncated\": false,\n"
           << "  \"stderrTruncated\": false,\n"
           << "  \"stdoutBytes\": 0,\n"
           << "  \"stderrBytes\": 0,\n"
           << "  \"bufferPeak\": 0,\n"
           << "  \"filePeak\": 0\n"
           << "}\n";
    writeAtomically(opDir + "/state.json", initSs.str(), 0600);

    // Write handshake success byte and close handshake fd
    const char okByte = '1';
    (void)::write(handshakeFd, &okByte, 1);
    ::close(handshakeFd);

    // Event loop continuously draining stdout and stderr pipes
    std::string stdoutTail;
    std::string stderrTail;
    std::size_t stdoutTotalBytes = 0;
    std::size_t stderrTotalBytes = 0;
    std::size_t stdoutDiagBytes = 0;
    std::size_t stderrDiagBytes = 0;
    std::size_t bufferPeak = 0;
    std::size_t filePeak = 0;

    bool outOpen = true;
    bool errOpen = true;
    bool childFinished = false;
    int childStatus = 0;

    while (outOpen || errOpen) {
        if (!childFinished) {
            const pid_t w = ::waitpid(childPid, &childStatus, WNOHANG);
            if (w == childPid) {
                childFinished = true;
            }
        }

        struct pollfd pfds[2];
        nfds_t nfds = 0;
        int outIdx = -1;
        int errIdx = -1;

        if (outOpen) {
            outIdx = static_cast<int>(nfds);
            pfds[nfds].fd = outPipe[0];
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            ++nfds;
        }
        if (errOpen) {
            errIdx = static_cast<int>(nfds);
            pfds[nfds].fd = errPipe[0];
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            ++nfds;
        }

        const int pollRet = ::poll(pfds, nfds, childFinished ? 50 : 200);

        if (pollRet > 0) {
            if (outIdx >= 0 && (pfds[outIdx].revents & (POLLIN | POLLHUP | POLLERR))) {
                while (true) {
                    char buf[8192];
                    const ssize_t n = ::read(outPipe[0], buf, sizeof(buf));
                    if (n > 0) {
                        stdoutTotalBytes += static_cast<size_t>(n);
                        stdoutTail.append(buf, static_cast<size_t>(n));
                        if (stdoutTail.size() > perStreamResultBytes) {
                            stdoutTail.erase(0, stdoutTail.size() - perStreamResultBytes);
                        }
                        bufferPeak = std::max(bufferPeak, stdoutTail.size());

                        if (diagOutFd >= 0 && stdoutDiagBytes < perStreamFileBytes) {
                            const size_t toWrite = std::min(static_cast<size_t>(n),
                                                           perStreamFileBytes - stdoutDiagBytes);
                            (void)::write(diagOutFd, buf, toWrite);
                            stdoutDiagBytes += toWrite;
                            filePeak = std::max(filePeak, stdoutDiagBytes);
                        }
                    } else if (n == 0) {
                        outOpen = false;
                        break;
                    } else {
                        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                            outOpen = false;
                        }
                        break;
                    }
                }
            }

            if (errIdx >= 0 && (pfds[errIdx].revents & (POLLIN | POLLHUP | POLLERR))) {
                while (true) {
                    char buf[8192];
                    const ssize_t n = ::read(errPipe[0], buf, sizeof(buf));
                    if (n > 0) {
                        stderrTotalBytes += static_cast<size_t>(n);
                        stderrTail.append(buf, static_cast<size_t>(n));
                        if (stderrTail.size() > perStreamResultBytes) {
                            stderrTail.erase(0, stderrTail.size() - perStreamResultBytes);
                        }
                        bufferPeak = std::max(bufferPeak, stderrTail.size());

                        if (diagErrFd >= 0 && stderrDiagBytes < perStreamFileBytes) {
                            const size_t toWrite = std::min(static_cast<size_t>(n),
                                                           perStreamFileBytes - stderrDiagBytes);
                            (void)::write(diagErrFd, buf, toWrite);
                            stderrDiagBytes += toWrite;
                            filePeak = std::max(filePeak, stderrDiagBytes);
                        }
                    } else if (n == 0) {
                        errOpen = false;
                        break;
                    } else {
                        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                            errOpen = false;
                        }
                        break;
                    }
                }
            }
        }

        if (childFinished) {
            // Drain remaining and close
            char flushBuf[8192];
            while (outOpen) {
                const ssize_t n = ::read(outPipe[0], flushBuf, sizeof(flushBuf));
                if (n > 0) {
                    stdoutTotalBytes += static_cast<size_t>(n);
                    stdoutTail.append(flushBuf, static_cast<size_t>(n));
                    if (stdoutTail.size() > perStreamResultBytes) {
                        stdoutTail.erase(0, stdoutTail.size() - perStreamResultBytes);
                    }
                    bufferPeak = std::max(bufferPeak, stdoutTail.size());
                    if (diagOutFd >= 0 && stdoutDiagBytes < perStreamFileBytes) {
                        const size_t toWrite = std::min(static_cast<size_t>(n),
                                                       perStreamFileBytes - stdoutDiagBytes);
                        (void)::write(diagOutFd, flushBuf, toWrite);
                        stdoutDiagBytes += toWrite;
                        filePeak = std::max(filePeak, stdoutDiagBytes);
                    }
                } else {
                    break;
                }
            }
            while (errOpen) {
                const ssize_t n = ::read(errPipe[0], flushBuf, sizeof(flushBuf));
                if (n > 0) {
                    stderrTotalBytes += static_cast<size_t>(n);
                    stderrTail.append(flushBuf, static_cast<size_t>(n));
                    if (stderrTail.size() > perStreamResultBytes) {
                        stderrTail.erase(0, stderrTail.size() - perStreamResultBytes);
                    }
                    bufferPeak = std::max(bufferPeak, stderrTail.size());
                    if (diagErrFd >= 0 && stderrDiagBytes < perStreamFileBytes) {
                        const size_t toWrite = std::min(static_cast<size_t>(n),
                                                       perStreamFileBytes - stderrDiagBytes);
                        (void)::write(diagErrFd, flushBuf, toWrite);
                        stderrDiagBytes += toWrite;
                        filePeak = std::max(filePeak, stderrDiagBytes);
                    }
                } else {
                    break;
                }
            }
            break;
        }
    }

    ::close(outPipe[0]);
    ::close(errPipe[0]);
    if (diagOutFd >= 0) ::close(diagOutFd);
    if (diagErrFd >= 0) ::close(diagErrFd);

    if (!childFinished) {
        ::waitpid(childPid, &childStatus, 0);
    }

    const bool ifExited = WIFEXITED(childStatus);
    const int exitStatus = ifExited ? WEXITSTATUS(childStatus) : -1;
    const std::string finalState = ifExited ? "Exited" : "Unknown";
    const bool stdoutTruncated = (stdoutTotalBytes > perStreamResultBytes);
    const bool stderrTruncated = (stderrTotalBytes > perStreamResultBytes);

    writeAtomically(opDir + "/stdout.tail", stdoutTail, 0600);
    writeAtomically(opDir + "/stderr.tail", stderrTail, 0600);

    // Final state.json (mode 0600, <= 4 KiB)
    std::ostringstream finalSs;
    finalSs << "{\n"
            << "  \"operationId\": " << operationId << ",\n"
            << "  \"collectorPid\": " << ::getpid() << ",\n"
            << "  \"childPid\": " << childPid << ",\n"
            << "  \"state\": \"" << finalState << "\",\n"
            << "  \"exitStatus\": " << exitStatus << ",\n"
            << "  \"stdoutTruncated\": " << (stdoutTruncated ? "true" : "false") << ",\n"
            << "  \"stderrTruncated\": " << (stderrTruncated ? "true" : "false") << ",\n"
            << "  \"stdoutBytes\": " << stdoutTotalBytes << ",\n"
            << "  \"stderrBytes\": " << stderrTotalBytes << ",\n"
            << "  \"bufferPeak\": " << bufferPeak << ",\n"
            << "  \"filePeak\": " << filePeak << "\n"
            << "}\n";
    writeAtomically(opDir + "/state.json", finalSs.str(), 0600);

    return 0;
}

CommandResult waitForOperation(std::uint64_t operationId, unsigned int timeoutMs) {
    CommandResult result;
    result.operationId = operationId;

    std::string opDir;
    {
        std::lock_guard<std::mutex> lock(g_opDirsMutex);
        auto it = g_activeOpDirs.find(operationId);
        if (it != g_activeOpDirs.end()) {
            opDir = it->second;
        }
    }

    if (opDir.empty()) {
        const std::string tmpDir = getTempDirectory();
        std::error_code ec;
        std::filesystem::path latestPath;
        std::filesystem::file_time_type latestTime;
        for (const auto &entry : std::filesystem::directory_iterator(tmpDir, ec)) {
            if (entry.is_directory(ec)) {
                const std::string name = entry.path().filename().string();
                if (name.starts_with("modernime-op-" + std::to_string(operationId) + "-")) {
                    const auto mtime = entry.last_write_time(ec);
                    if (latestPath.empty() || mtime > latestTime) {
                        latestPath = entry.path();
                        latestTime = mtime;
                    }
                }
            }
        }
        if (!latestPath.empty()) {
            opDir = latestPath.string();
        }
    }

    if (opDir.empty()) {
        result.state = CommandState::Unknown;
        result.stderrText = "Operation directory not found for ID " + std::to_string(operationId);
        return result;
    }

    const auto startTime = std::chrono::steady_clock::now();
    const auto deadline = startTime + std::chrono::milliseconds(timeoutMs);
    const std::string statePath = opDir + "/state.json";

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        const ParsedState ps = parseStateFile(statePath);

        if (ps.valid && ps.operationId == operationId) {
            if (ps.stateStr == "Exited" || ps.stateStr == "Unknown") {
                result.state = (ps.stateStr == "Exited") ? CommandState::Exited : CommandState::Unknown;
                result.exitStatus = ps.exitStatus;
                result.stdoutTruncated = ps.stdoutTruncated;
                result.stderrTruncated = ps.stderrTruncated;
                result.stdoutText = readFileBounded(opDir + "/stdout.tail", 65536);
                result.stderrText = readFileBounded(opDir + "/stderr.tail", 65536);
                return result;
            }
        }

        if (ps.collectorPid > 0 && !isPidAlive(ps.collectorPid)) {
            const ParsedState psFinal = parseStateFile(statePath);
            if (psFinal.valid && psFinal.operationId == operationId &&
                (psFinal.stateStr == "Exited" || psFinal.stateStr == "Unknown")) {
                result.state = (psFinal.stateStr == "Exited") ? CommandState::Exited : CommandState::Unknown;
                result.exitStatus = psFinal.exitStatus;
                result.stdoutTruncated = psFinal.stdoutTruncated;
                result.stderrTruncated = psFinal.stderrTruncated;
                result.stdoutText = readFileBounded(opDir + "/stdout.tail", 65536);
                result.stderrText = readFileBounded(opDir + "/stderr.tail", 65536);
                return result;
            }
            result.state = CommandState::Unknown;
            result.exitStatus = -1;
            result.stderrText = "Collector process exited without producing valid Exited state";
            return result;
        }

        if (now >= deadline) {
            result.state = CommandState::Continuing;
            return result;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

} // namespace modernime::settings::detail
