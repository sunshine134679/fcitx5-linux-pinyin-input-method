#include "modernime/settings/detail/process_runner.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

extern char **environ;

namespace modernime::settings::detail {
namespace {

std::mutex g_opRefsMutex;
std::map<std::uint64_t, OperationRef> g_activeOpRefs;

struct PendingReap {
    pid_t pid = 0;
    std::string opDir;
};

std::mutex g_reaperMutex;
std::condition_variable g_reaperCv;
std::vector<PendingReap> g_pendingReaps;
std::uint64_t g_reaperEnqueueSeq = 0;
std::atomic<bool> g_reaperStop{false};
std::thread g_reaperThread;
} // namespace

void shutdownBackgroundReaper() {
    {
        std::lock_guard<std::mutex> lk(g_reaperMutex);
        if (!g_reaperThread.joinable()) {
            return;
        }
        g_reaperStop.store(true);
        g_reaperCv.notify_all();
    }
    if (g_reaperThread.joinable()) {
        g_reaperThread.join();
    }
}

namespace {
struct ReaperCleaner {
    ~ReaperCleaner() {
        shutdownBackgroundReaper();
    }
};
static ReaperCleaner g_reaperCleaner;

void enqueuePendingReap(pid_t pid, const std::string &opDir) {
    if (pid <= 0) return;
    std::lock_guard<std::mutex> lock(g_reaperMutex);
    if (g_reaperStop.load()) return;
    g_pendingReaps.push_back({pid, opDir});
    ++g_reaperEnqueueSeq;
    if (!g_reaperThread.joinable()) {
        g_reaperThread = std::thread([]() {
            std::uint64_t lastSeq = 0;
            while (true) {
                std::vector<PendingReap> current;
                {
                    std::unique_lock<std::mutex> lk(g_reaperMutex);
                    if (g_reaperStop.load()) {
                        break;
                    }
                    if (g_pendingReaps.empty()) {
                        g_reaperCv.wait(lk, [&]() {
                            return g_reaperStop.load() || !g_pendingReaps.empty();
                        });
                    } else {
                        g_reaperCv.wait_for(lk, std::chrono::milliseconds(50), [&]() {
                            return g_reaperStop.load() || (g_reaperEnqueueSeq != lastSeq);
                        });
                    }
                    if (g_reaperStop.load()) {
                        break;
                    }
                    lastSeq = g_reaperEnqueueSeq;
                    current = g_pendingReaps;
                }

                if (current.empty()) {
                    continue;
                }

                std::vector<pid_t> reapedPids;
                for (const auto &item : current) {
                    int st = 0;
                    pid_t wp = ::waitpid(item.pid, &st, WNOHANG);
                    if (wp == item.pid || (wp < 0 && errno == ECHILD)) {
                        reapedPids.push_back(item.pid);
                        if (!item.opDir.empty()) {
                            std::error_code ec;
                            std::filesystem::remove_all(item.opDir, ec);
                        }
                    }
                }

                if (!reapedPids.empty()) {
                    std::lock_guard<std::mutex> lk(g_reaperMutex);
                    g_pendingReaps.erase(
                        std::remove_if(g_pendingReaps.begin(), g_pendingReaps.end(),
                                       [&](const PendingReap &p) {
                                           return std::find(reapedPids.begin(), reapedPids.end(), p.pid) != reapedPids.end();
                                       }),
                        g_pendingReaps.end());
                }
            }
        });
    } else {
        g_reaperCv.notify_all();
    }
}

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

std::string generateRandomToken() {
    static const char hexChars[] = "0123456789abcdef";
    std::string token;
    token.reserve(16);
    int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        unsigned char buf[8];
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        ::close(fd);
        if (n == sizeof(buf)) {
            for (int i = 0; i < 8; ++i) {
                token.push_back(hexChars[(buf[i] >> 4) & 0x0f]);
                token.push_back(hexChars[buf[i] & 0x0f]);
            }
            return token;
        }
    }
    std::mt19937_64 rng(std::chrono::system_clock::now().time_since_epoch().count());
    const uint64_t v = rng();
    std::ostringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(16) << v;
    return ss.str();
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

struct PreparedEnvironment {
    std::vector<std::string> envStrings;
    std::vector<char *> envp;
};

PreparedEnvironment prepareEnvironment(const Environment &overrides) {
    PreparedEnvironment prep;
    std::map<std::string, std::string> envMap;

    if (environ != nullptr) {
        for (char **e = environ; *e != nullptr; ++e) {
            const std::string_view entry(*e);
            const auto eq = entry.find('=');
            if (eq != std::string_view::npos) {
                envMap[std::string(entry.substr(0, eq))] = std::string(entry.substr(eq + 1));
            }
        }
    }

    for (const auto &[k, v] : overrides) {
        if (k == "PATH") {
            if (v.empty()) {
                envMap["PATH"] = "/usr/bin:/bin";
            } else {
                auto it = envMap.find("PATH");
                if (it != envMap.end() && !it->second.empty()) {
                    envMap["PATH"] = v + ":" + it->second;
                } else {
                    envMap["PATH"] = v + ":/usr/bin:/bin";
                }
            }
        } else {
            envMap[k] = v;
        }
    }

    if (envMap.find("PATH") == envMap.end() || envMap["PATH"].empty()) {
        envMap["PATH"] = "/usr/bin:/bin";
    }

    envMap["LC_ALL"] = "C";
    envMap["LANG"] = "C";

    prep.envStrings.reserve(envMap.size());
    for (const auto &[k, v] : envMap) {
        prep.envStrings.push_back(k + "=" + v);
    }
    prep.envp.reserve(prep.envStrings.size() + 1);
    for (auto &s : prep.envStrings) {
        prep.envp.push_back(s.data());
    }
    prep.envp.push_back(nullptr);

    return prep;
}

struct ParsedState {
    std::uint64_t operationId = 0;
    std::string token;
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

enum class JsonTokenType {
    LeftBrace,
    RightBrace,
    Colon,
    Comma,
    String,
    Number,
    Boolean,
    End,
    Error
};

struct JsonToken {
    JsonTokenType type = JsonTokenType::Error;
    std::string stringVal;
    long long intVal = 0;
    bool boolVal = false;
};

class JsonLexer {
public:
    explicit JsonLexer(std::string_view input) : m_input(input), m_pos(0) {}

    JsonToken nextToken() {
        skipWhitespace();
        if (m_pos >= m_input.size()) {
            return {JsonTokenType::End, {}, 0, false};
        }

        const char c = m_input[m_pos];
        if (c == '{') {
            ++m_pos;
            return {JsonTokenType::LeftBrace, {}, 0, false};
        }
        if (c == '}') {
            ++m_pos;
            return {JsonTokenType::RightBrace, {}, 0, false};
        }
        if (c == ':') {
            ++m_pos;
            return {JsonTokenType::Colon, {}, 0, false};
        }
        if (c == ',') {
            ++m_pos;
            return {JsonTokenType::Comma, {}, 0, false};
        }
        if (c == '"') {
            return readString();
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return readNumber();
        }
        if (c == 't' || c == 'f') {
            return readBool();
        }

        return {JsonTokenType::Error, {}, 0, false};
    }

private:
    void skipWhitespace() {
        while (m_pos < m_input.size() && (m_input[m_pos] == ' ' || m_input[m_pos] == '\t' ||
                                          m_input[m_pos] == '\r' || m_input[m_pos] == '\n')) {
            ++m_pos;
        }
    }

    JsonToken readString() {
        ++m_pos; // Skip opening quote
        std::string s;
        while (m_pos < m_input.size()) {
            const char c = m_input[m_pos++];
            if (c == '"') {
                return {JsonTokenType::String, s, 0, false};
            }
            if (c == '\\') {
                if (m_pos >= m_input.size()) {
                    return {JsonTokenType::Error, {}, 0, false};
                }
                const char esc = m_input[m_pos++];
                if (esc == '"' || esc == '\\' || esc == '/') s.push_back(esc);
                else if (esc == 'b') s.push_back('\b');
                else if (esc == 'f') s.push_back('\f');
                else if (esc == 'n') s.push_back('\n');
                else if (esc == 'r') s.push_back('\r');
                else if (esc == 't') s.push_back('\t');
                else return {JsonTokenType::Error, {}, 0, false};
            } else {
                s.push_back(c);
            }
        }
        return {JsonTokenType::Error, {}, 0, false};
    }

    JsonToken readNumber() {
        const size_t start = m_pos;
        if (m_input[m_pos] == '-') {
            ++m_pos;
        }
        if (m_pos >= m_input.size() || m_input[m_pos] < '0' || m_input[m_pos] > '9') {
            return {JsonTokenType::Error, {}, 0, false};
        }
        while (m_pos < m_input.size() && m_input[m_pos] >= '0' && m_input[m_pos] <= '9') {
            ++m_pos;
        }
        if (m_pos < m_input.size()) {
            const char nextC = m_input[m_pos];
            if (nextC != ' ' && nextC != '\t' && nextC != '\r' && nextC != '\n' &&
                nextC != ',' && nextC != '}') {
                return {JsonTokenType::Error, {}, 0, false};
            }
        }
        const std::string numStr(m_input.substr(start, m_pos - start));
        try {
            const long long val = std::stoll(numStr);
            return {JsonTokenType::Number, {}, val, false};
        } catch (...) {
            return {JsonTokenType::Error, {}, 0, false};
        }
    }

    JsonToken readBool() {
        if (m_input.substr(m_pos).starts_with("true")) {
            m_pos += 4;
            if (m_pos < m_input.size()) {
                const char nextC = m_input[m_pos];
                if (nextC != ' ' && nextC != '\t' && nextC != '\r' && nextC != '\n' &&
                    nextC != ',' && nextC != '}') {
                    return {JsonTokenType::Error, {}, 0, false};
                }
            }
            return {JsonTokenType::Boolean, {}, 0, true};
        }
        if (m_input.substr(m_pos).starts_with("false")) {
            m_pos += 5;
            if (m_pos < m_input.size()) {
                const char nextC = m_input[m_pos];
                if (nextC != ' ' && nextC != '\t' && nextC != '\r' && nextC != '\n' &&
                    nextC != ',' && nextC != '}') {
                    return {JsonTokenType::Error, {}, 0, false};
                }
            }
            return {JsonTokenType::Boolean, {}, 0, false};
        }
        return {JsonTokenType::Error, {}, 0, false};
    }

    std::string_view m_input;
    size_t m_pos = 0;
};

ParsedState parseStateFile(const std::string &path, std::uint64_t expectedOpId, const std::string &expectedToken, pid_t expectedCollectorPid = 0) {
    ParsedState s;
    std::error_code ec;
    const auto sz = std::filesystem::file_size(path, ec);
    if (ec || sz == 0 || sz > 4096) {
        return s;
    }

    const std::string content = readFileBounded(path, 4096);
    if (content.empty()) {
        return s;
    }

    JsonLexer lexer(content);
    JsonToken tok = lexer.nextToken();
    if (tok.type != JsonTokenType::LeftBrace) {
        return s;
    }

    struct Val {
        JsonTokenType type;
        std::string str;
        long long num = 0;
        bool b = false;
    };
    std::map<std::string, Val> entries;

    bool expectKey = true;
    while (true) {
        tok = lexer.nextToken();
        if (tok.type == JsonTokenType::RightBrace) {
            if (expectKey && !entries.empty()) {
                return s;
            }
            break;
        }
        if (!expectKey) {
            return s;
        }
        if (tok.type != JsonTokenType::String) {
            return s;
        }
        const std::string key = tok.stringVal;
        if (entries.find(key) != entries.end()) {
            return s; // Duplicate key rejected
        }

        tok = lexer.nextToken();
        if (tok.type != JsonTokenType::Colon) {
            return s;
        }

        tok = lexer.nextToken();
        if (tok.type == JsonTokenType::String) {
            entries[key] = {tok.type, tok.stringVal, 0, false};
        } else if (tok.type == JsonTokenType::Number) {
            entries[key] = {tok.type, {}, tok.intVal, false};
        } else if (tok.type == JsonTokenType::Boolean) {
            entries[key] = {tok.type, {}, 0, tok.boolVal};
        } else {
            return s;
        }

        tok = lexer.nextToken();
        if (tok.type == JsonTokenType::Comma) {
            expectKey = true;
        } else if (tok.type == JsonTokenType::RightBrace) {
            break;
        } else {
            return s;
        }
    }

    tok = lexer.nextToken();
    if (tok.type != JsonTokenType::End) {
        return s;
    }

    static const char *const requiredKeys[] = {
        "operationId", "token", "collectorPid", "childPid", "state", "exitStatus",
        "stdoutTruncated", "stderrTruncated", "stdoutBytes", "stderrBytes",
        "bufferPeak", "filePeak"
    };
    if (entries.size() != 12) {
        return s;
    }
    for (const char *rk : requiredKeys) {
        if (entries.find(rk) == entries.end()) {
            return s;
        }
    }

    if (entries["operationId"].type != JsonTokenType::Number || entries["operationId"].num <= 0) return s;
    s.operationId = static_cast<std::uint64_t>(entries["operationId"].num);

    if (entries["token"].type != JsonTokenType::String) return s;
    s.token = entries["token"].str;

    if (entries["collectorPid"].type != JsonTokenType::Number) return s;
    const long long cPid = entries["collectorPid"].num;
    if (cPid <= 0 || cPid > 4194304LL) return s;
    s.collectorPid = static_cast<pid_t>(cPid);

    if (entries["childPid"].type != JsonTokenType::Number) return s;
    const long long chPid = entries["childPid"].num;
    if (chPid < 0 || chPid > 4194304LL) return s;
    s.childPid = static_cast<pid_t>(chPid);

    if (entries["state"].type != JsonTokenType::String) return s;
    s.stateStr = entries["state"].str;
    if (s.stateStr != "Starting" && s.stateStr != "Running" &&
        s.stateStr != "Exited" && s.stateStr != "Unknown") {
        return s;
    }

    if (entries["exitStatus"].type != JsonTokenType::Number) return s;
    const long long rawExit = entries["exitStatus"].num;
    if (s.stateStr == "Exited") {
        if (rawExit < 0 || rawExit > 255) return s;
    } else if (s.stateStr == "Running" || s.stateStr == "Starting") {
        if (rawExit != -1) return s;
    } else if (s.stateStr == "Unknown") {
        if (rawExit < -1 || rawExit > 255) return s;
    }
    s.exitStatus = static_cast<int>(rawExit);

    if (entries["stdoutTruncated"].type != JsonTokenType::Boolean) return s;
    s.stdoutTruncated = entries["stdoutTruncated"].b;

    if (entries["stderrTruncated"].type != JsonTokenType::Boolean) return s;
    s.stderrTruncated = entries["stderrTruncated"].b;

    if (entries["stdoutBytes"].type != JsonTokenType::Number) return s;
    const long long soBytes = entries["stdoutBytes"].num;
    if (soBytes < 0 || soBytes > 1073741824LL) return s;
    s.stdoutBytes = static_cast<std::size_t>(soBytes);

    if (entries["stderrBytes"].type != JsonTokenType::Number) return s;
    const long long seBytes = entries["stderrBytes"].num;
    if (seBytes < 0 || seBytes > 1073741824LL) return s;
    s.stderrBytes = static_cast<std::size_t>(seBytes);

    if (entries["bufferPeak"].type != JsonTokenType::Number) return s;
    const long long bPeak = entries["bufferPeak"].num;
    if (bPeak < 0 || bPeak > 1073741824LL) return s;
    s.bufferPeak = static_cast<std::size_t>(bPeak);

    if (entries["filePeak"].type != JsonTokenType::Number) return s;
    const long long fPeak = entries["filePeak"].num;
    if (fPeak < 0 || fPeak > 1073741824LL) return s;
    s.filePeak = static_cast<std::size_t>(fPeak);

    if (expectedOpId != 0 && s.operationId != expectedOpId) {
        return s;
    }
    if (!expectedToken.empty() && s.token != expectedToken) {
        return s;
    }
    if (expectedCollectorPid > 0 && s.collectorPid != expectedCollectorPid) {
        return s;
    }

    s.valid = true;
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

    PreparedEnvironment prepEnv = prepareEnvironment(spec.environment);
    std::vector<char *> argvPtrs;
    argvPtrs.reserve(spec.argv.size() + 1);
    for (const auto &arg : spec.argv) {
        argvPtrs.push_back(const_cast<char *>(arg.c_str()));
    }
    argvPtrs.push_back(nullptr);

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

        ::execvpe(argvPtrs[0], argvPtrs.data(), prepEnv.envp.data());
        const int err = errno;
        ::dprintf(STDERR_FILENO, "Failed to execute %s: %s\n", argvPtrs[0], std::strerror(err));
        ::_exit(127);
    }

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

    while (outOpen || errOpen || !leaderExited) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            result.timedOut = true;
            if (!leaderExited) {
                ::kill(pid, SIGKILL);
                ::waitpid(pid, &childStatus, 0);
                leaderExited = true;
            }
            break;
        }

        if (!leaderExited) {
            const pid_t w = ::waitpid(pid, &childStatus, WNOHANG);
            if (w == pid) {
                leaderExited = true;
            }
        }

        if (!outOpen && !errOpen) {
            if (!leaderExited) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            break;
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

        int pollTimeout = leaderExited ? 50 : 100;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        if (remaining > 0 && remaining < pollTimeout) {
            pollTimeout = static_cast<int>(remaining);
        }

        const int pRet = (nfds > 0) ? ::poll(fds, nfds, pollTimeout) : 0;

        if (pRet > 0) {
            if (outIdx >= 0 && (fds[outIdx].revents & (POLLIN | POLLHUP | POLLERR))) {
                size_t readChunk = 0;
                while (readChunk < 65536) {
                    char buf[4096];
                    const size_t toRead = std::min(sizeof(buf), 65536 - readChunk);
                    const ssize_t n = ::read(outPipe[0], buf, toRead);
                    if (n > 0) {
                        readChunk += static_cast<size_t>(n);
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
                size_t readChunk = 0;
                while (readChunk < 65536) {
                    char buf[4096];
                    const size_t toRead = std::min(sizeof(buf), 65536 - readChunk);
                    const ssize_t n = ::read(errPipe[0], buf, toRead);
                    if (n > 0) {
                        readChunk += static_cast<size_t>(n);
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

        if (leaderExited) {
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
            break;
        }
    }

    ::close(outPipe[0]);
    ::close(errPipe[0]);

    if (!leaderExited) {
        while (std::chrono::steady_clock::now() < deadline) {
            const pid_t w = ::waitpid(pid, &childStatus, WNOHANG);
            if (w == pid) {
                leaderExited = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!leaderExited) {
            result.timedOut = true;
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
    const std::string token = generateRandomToken();
    const std::string opDir = tmpDir + "/modernime-op-" + std::to_string(spec.operationId) + "-" + token;

    if (::mkdir(opDir.c_str(), 0700) != 0) {
        result.state = CommandState::NotStarted;
        result.stderrText = "Failed to create private operation directory";
        return result;
    }

    OperationRef opRef;
    opRef.operationId = spec.operationId;
    opRef.token = token;
    opRef.opDir = opDir;
    opRef.collectorPid = 0;
    opRef.isOwnedChild = true;

    {
        std::lock_guard<std::mutex> lock(g_opRefsMutex);
        g_activeOpRefs[spec.operationId] = opRef;
    }

    std::ostringstream ss;
    ss << "operationId=" << spec.operationId << "\n";
    ss << "token=" << token << "\n";
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
        std::error_code ec;
        std::filesystem::remove_all(opDir, ec);
        {
            std::lock_guard<std::mutex> lock(g_opRefsMutex);
            g_activeOpRefs.erase(spec.operationId);
        }
        result.state = CommandState::NotStarted;
        result.stderrText = "Failed to write operation spec file";
        return result;
    }

    int handshakeFds[2];
    if (::pipe2(handshakeFds, O_CLOEXEC) != 0) {
        std::error_code ec;
        std::filesystem::remove_all(opDir, ec);
        {
            std::lock_guard<std::mutex> lock(g_opRefsMutex);
            g_activeOpRefs.erase(spec.operationId);
        }
        result.state = CommandState::NotStarted;
        result.stderrText = "Failed to create handshake pipe";
        return result;
    }

    int execPipe[2];
    if (::pipe2(execPipe, O_CLOEXEC) != 0) {
        ::close(handshakeFds[0]);
        ::close(handshakeFds[1]);
        std::error_code ec;
        std::filesystem::remove_all(opDir, ec);
        {
            std::lock_guard<std::mutex> lock(g_opRefsMutex);
            g_activeOpRefs.erase(spec.operationId);
        }
        result.state = CommandState::NotStarted;
        result.stderrText = "Failed to create exec pipe";
        return result;
    }

    const std::string settingsExe = resolveSettingsExecutable(spec.environment);

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

    const pid_t collectorPid = ::fork();
    if (collectorPid < 0) {
        ::close(handshakeFds[0]);
        ::close(handshakeFds[1]);
        ::close(execPipe[0]);
        ::close(execPipe[1]);
        std::error_code ec;
        std::filesystem::remove_all(opDir, ec);
        {
            std::lock_guard<std::mutex> lock(g_opRefsMutex);
            g_activeOpRefs.erase(spec.operationId);
        }
        result.state = CommandState::NotStarted;
        result.stderrText = "Failed to fork collector process";
        return result;
    }

    if (collectorPid == 0) {
        ::close(handshakeFds[0]);
        ::close(execPipe[0]);
        const int flags = ::fcntl(handshakeFds[1], F_GETFD);
        if (flags >= 0) {
            ::fcntl(handshakeFds[1], F_SETFD, flags & ~FD_CLOEXEC);
        }

        ::execvp(cArgv[0], cArgv.data());
        const int err = errno;
        ssize_t w = ::write(execPipe[1], &err, sizeof(err));
        (void)w;
        ::_exit(127);
    }

    ::close(execPipe[1]);
    ::close(handshakeFds[1]);

    {
        std::lock_guard<std::mutex> lock(g_opRefsMutex);
        g_activeOpRefs[spec.operationId].collectorPid = collectorPid;
        g_activeOpRefs[spec.operationId].isOwnedChild = true;
    }

    auto setNonBlocking = [](int fd) {
        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
    };
    setNonBlocking(execPipe[0]);
    setNonBlocking(handshakeFds[0]);

    const auto startTime = std::chrono::steady_clock::now();
    const auto deadline = startTime + std::chrono::milliseconds(spec.timeoutMs);

    int childErrno = 0;
    std::size_t execBytesRead = 0;
    char *errnoBuf = reinterpret_cast<char *>(&childErrno);
    bool execPipeClosed = false;

    char handshakeByte = 0;
    bool handshakeReceived = false;

    while (std::chrono::steady_clock::now() < deadline) {
        const auto now = std::chrono::steady_clock::now();
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        const int waitMs = std::max(0, static_cast<int>(remaining));

        struct pollfd pfds[2];
        nfds_t nfds = 0;
        int execIdx = -1;
        int hsIdx = -1;

        if (!execPipeClosed && execBytesRead < sizeof(childErrno)) {
            execIdx = static_cast<int>(nfds++);
            pfds[execIdx].fd = execPipe[0];
            pfds[execIdx].events = POLLIN;
            pfds[execIdx].revents = 0;
        }
        if (!handshakeReceived) {
            hsIdx = static_cast<int>(nfds++);
            pfds[hsIdx].fd = handshakeFds[0];
            pfds[hsIdx].events = POLLIN;
            pfds[hsIdx].revents = 0;
        }

        if (nfds == 0) {
            break;
        }

        int pRet = ::poll(pfds, nfds, waitMs);
        if (pRet < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pRet == 0) {
            break; // Deadline expired
        }

        if (execIdx >= 0 && (pfds[execIdx].revents & (POLLIN | POLLHUP | POLLERR))) {
            while (execBytesRead < sizeof(childErrno)) {
                ssize_t n = ::read(execPipe[0], errnoBuf + execBytesRead,
                                   sizeof(childErrno) - execBytesRead);
                if (n > 0) {
                    execBytesRead += static_cast<std::size_t>(n);
                } else if (n == 0) {
                    execPipeClosed = true;
                    break;
                } else {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    execPipeClosed = true;
                    break;
                }
            }
            if (execBytesRead == sizeof(childErrno)) {
                break;
            }
        }

        if (hsIdx >= 0 && (pfds[hsIdx].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = ::read(handshakeFds[0], &handshakeByte, 1);
            if (n > 0) {
                handshakeReceived = true;
                break;
            } else if (n == 0) {
                break;
            } else {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Non-blocking would block, keep waiting
                } else {
                    break;
                }
            }
        }
    }

    ::close(execPipe[0]);
    ::close(handshakeFds[0]);

    if (execBytesRead == sizeof(childErrno) && childErrno != 0) {
        int st = 0;
        ::waitpid(collectorPid, &st, 0);
        std::error_code ec;
        std::filesystem::remove_all(opDir, ec);
        {
            std::lock_guard<std::mutex> lock(g_opRefsMutex);
            g_activeOpRefs.erase(spec.operationId);
        }
        result.state = CommandState::NotStarted;
        result.exitStatus = -1;
        result.stderrText = "Failed to execute collector: " + std::string(std::strerror(childErrno));
        return result;
    }

    if (handshakeReceived && handshakeByte == 'N') {
        int st = 0;
        ::waitpid(collectorPid, &st, 0);
        std::error_code ec;
        std::filesystem::remove_all(opDir, ec);
        {
            std::lock_guard<std::mutex> lock(g_opRefsMutex);
            g_activeOpRefs.erase(spec.operationId);
        }
        result.state = CommandState::NotStarted;
        result.exitStatus = -1;
        result.stderrText = "Target process was not started";
        return result;
    }

    if (!handshakeReceived || (handshakeByte != 'R' && handshakeByte != '1')) {
        int st = 0;
        pid_t wp = ::waitpid(collectorPid, &st, WNOHANG);
        if (wp == 0) {
            enqueuePendingReap(collectorPid, "");
        }
        result.state = CommandState::Unknown;
        result.exitStatus = -1;
        result.stderrText = "Collector process failed or crashed before handshake confirmation";
        return result;
    }

    // handshakeByte == 'R' or '1'
    const std::string statePath = opDir + "/state.json";

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        const ParsedState ps = parseStateFile(statePath, spec.operationId, token, collectorPid);

        if (ps.valid && ps.operationId == spec.operationId) {
            if (ps.stateStr == "Exited") {
                result.state = CommandState::Exited;
                result.exitStatus = ps.exitStatus;
                result.stdoutTruncated = ps.stdoutTruncated;
                result.stderrTruncated = ps.stderrTruncated;
                result.stdoutText = readFileBounded(opDir + "/stdout.tail", spec.limits.perStreamResultBytes);
                result.stderrText = readFileBounded(opDir + "/stderr.tail", spec.limits.perStreamResultBytes);
                result.bufferPeak = ps.bufferPeak;
                result.filePeak = ps.filePeak;
                {
                    std::lock_guard<std::mutex> lock(g_opRefsMutex);
                    g_activeOpRefs.erase(spec.operationId);
                }
                int st = 0;
                pid_t wp = ::waitpid(collectorPid, &st, WNOHANG);
                if (wp == collectorPid) {
                    std::error_code ec;
                    std::filesystem::remove_all(opDir, ec);
                } else {
                    enqueuePendingReap(collectorPid, opDir);
                }
                return result;
            }
            if (ps.stateStr == "Unknown") {
                result.state = CommandState::Unknown;
                result.exitStatus = ps.exitStatus;
                result.stdoutTruncated = ps.stdoutTruncated;
                result.stderrTruncated = ps.stderrTruncated;
                result.stdoutText = readFileBounded(opDir + "/stdout.tail", spec.limits.perStreamResultBytes);
                result.stderrText = readFileBounded(opDir + "/stderr.tail", spec.limits.perStreamResultBytes);
                result.bufferPeak = ps.bufferPeak;
                result.filePeak = ps.filePeak;
                int st = 0;
                pid_t wp = ::waitpid(collectorPid, &st, WNOHANG);
                if (wp == 0) {
                    enqueuePendingReap(collectorPid, "");
                }
                return result;
            }
        }

        if (!isPidAlive(collectorPid)) {
            const ParsedState psFinal = parseStateFile(statePath, spec.operationId, token, collectorPid);
            if (psFinal.valid && psFinal.operationId == spec.operationId && psFinal.stateStr == "Exited") {
                result.state = CommandState::Exited;
                result.exitStatus = psFinal.exitStatus;
                result.stdoutTruncated = psFinal.stdoutTruncated;
                result.stderrTruncated = psFinal.stderrTruncated;
                result.stdoutText = readFileBounded(opDir + "/stdout.tail", spec.limits.perStreamResultBytes);
                result.stderrText = readFileBounded(opDir + "/stderr.tail", spec.limits.perStreamResultBytes);
                result.bufferPeak = psFinal.bufferPeak;
                result.filePeak = psFinal.filePeak;
                {
                    std::lock_guard<std::mutex> lock(g_opRefsMutex);
                    g_activeOpRefs.erase(spec.operationId);
                }
                int st = 0;
                ::waitpid(collectorPid, &st, 0);
                std::error_code ec;
                std::filesystem::remove_all(opDir, ec);
                return result;
            }
            int st = 0;
            ::waitpid(collectorPid, &st, 0);
            result.state = CommandState::Unknown;
            result.exitStatus = -1;
            result.stderrText = "Collector terminated abnormally or status unreadable";
            return result;
        }

        if (now >= deadline) {
            result.timedOut = true;
            if (isPidAlive(collectorPid) || (ps.valid && ps.childPid > 0 && isPidAlive(ps.childPid))) {
                result.state = CommandState::Continuing;
                return result;
            }
            int st = 0;
            ::waitpid(collectorPid, &st, 0);
            result.state = CommandState::Unknown;
            result.exitStatus = -1;
            result.stderrText = "Process timed out without valid Exited state";
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
            const char errByte = 'N';
            (void)::write(handshakeFd, &errByte, 1);
            ::close(handshakeFd);
        }
        return 1;
    }

    const std::string specPath = opDir + "/spec.dat";
    std::ifstream in(specPath);
    if (!in) {
        const char errByte = 'N';
        (void)::write(handshakeFd, &errByte, 1);
        ::close(handshakeFd);
        return 1;
    }

    std::uint64_t operationId = 0;
    std::string token;
    std::uint32_t timeoutMs = 5000;
    (void)timeoutMs;
    std::size_t perStreamBufferBytes = 1048576;
    (void)perStreamBufferBytes;
    std::size_t perStreamResultBytes = 65536;
    std::size_t perStreamFileBytes = 1048576;
    std::vector<std::string> childArgv;
    Environment childEnv;

    std::string line;
    while (std::getline(in, line)) {
        if (line.starts_with("operationId=")) {
            operationId = std::stoull(line.substr(12));
        } else if (line.starts_with("token=")) {
            token = line.substr(6);
        } else if (line.starts_with("timeoutMs=")) {
            timeoutMs = static_cast<std::uint32_t>(std::stoul(line.substr(10)));
        } else if (line.starts_with("perStreamBufferBytes=")) {
            perStreamBufferBytes = std::stoul(line.substr(21));
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
        const char errByte = 'N';
        (void)::write(handshakeFd, &errByte, 1);
        ::close(handshakeFd);
        return 1;
    }

    std::ostringstream initSs;
    initSs << "{\n"
           << "  \"operationId\": " << operationId << ",\n"
           << "  \"token\": \"" << token << "\",\n"
           << "  \"collectorPid\": " << ::getpid() << ",\n"
           << "  \"childPid\": 0,\n"
           << "  \"state\": \"Starting\",\n"
           << "  \"exitStatus\": -1,\n"
           << "  \"stdoutTruncated\": false,\n"
           << "  \"stderrTruncated\": false,\n"
           << "  \"stdoutBytes\": 0,\n"
           << "  \"stderrBytes\": 0,\n"
           << "  \"bufferPeak\": 0,\n"
           << "  \"filePeak\": 0\n"
           << "}\n";

    if (!writeAtomically(opDir + "/state.json", initSs.str(), 0600)) {
        const char errByte = 'N';
        (void)::write(handshakeFd, &errByte, 1);
        ::close(handshakeFd);
        return 1;
    }

    const int diagOutFd = ::open((opDir + "/stdout.diag").c_str(),
                                 O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    const int diagErrFd = ::open((opDir + "/stderr.diag").c_str(),
                                 O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

    int outPipe[2];
    int errPipe[2];
    if (::pipe2(outPipe, O_CLOEXEC | O_NONBLOCK) != 0 || ::pipe2(errPipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        const char errByte = 'N';
        (void)::write(handshakeFd, &errByte, 1);
        ::close(handshakeFd);
        if (diagOutFd >= 0) ::close(diagOutFd);
        if (diagErrFd >= 0) ::close(diagErrFd);
        return 1;
    }

    PreparedEnvironment prepEnv = prepareEnvironment(childEnv);
    std::vector<char *> childArgvPtrs;
    childArgvPtrs.reserve(childArgv.size() + 1);
    for (const auto &arg : childArgv) {
        childArgvPtrs.push_back(const_cast<char *>(arg.c_str()));
    }
    childArgvPtrs.push_back(nullptr);

    const pid_t childPid = ::fork();
    if (childPid < 0) {
        const char errByte = 'N';
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

        ::execvpe(childArgvPtrs[0], childArgvPtrs.data(), prepEnv.envp.data());
        const int err = errno;
        ::dprintf(STDERR_FILENO, "Failed to execute child process \"%s\": %s\n", childArgvPtrs[0], std::strerror(err));
        ::_exit(127);
    }

    ::close(outPipe[1]);
    ::close(errPipe[1]);

    std::ostringstream runSs;
    runSs << "{\n"
          << "  \"operationId\": " << operationId << ",\n"
          << "  \"token\": \"" << token << "\",\n"
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

    if (!writeAtomically(opDir + "/state.json", runSs.str(), 0600)) {
        const char errByte = 'U';
        (void)::write(handshakeFd, &errByte, 1);
        ::close(handshakeFd);
    } else {
        const char okByte = 'R';
        (void)::write(handshakeFd, &okByte, 1);
        ::close(handshakeFd);
    }

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

        const int pollRet = (nfds > 0) ? ::poll(pfds, nfds, childFinished ? 50 : 100) : 0;

        if (pollRet > 0) {
            if (outIdx >= 0 && (pfds[outIdx].revents & (POLLIN | POLLHUP | POLLERR))) {
                size_t readThisTurn = 0;
                while (readThisTurn < 65536) {
                    char buf[8192];
                    const size_t toRead = std::min(sizeof(buf), 65536 - readThisTurn);
                    const ssize_t n = ::read(outPipe[0], buf, toRead);
                    if (n > 0) {
                        readThisTurn += static_cast<size_t>(n);
                        stdoutTotalBytes += static_cast<size_t>(n);
                        stdoutTail.append(buf, static_cast<size_t>(n));
                        bufferPeak = std::max(bufferPeak, stdoutTail.size());
                        if (stdoutTail.size() > perStreamResultBytes) {
                            stdoutTail.erase(0, stdoutTail.size() - perStreamResultBytes);
                        }

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
                size_t readThisTurn = 0;
                while (readThisTurn < 65536) {
                    char buf[8192];
                    const size_t toRead = std::min(sizeof(buf), 65536 - readThisTurn);
                    const ssize_t n = ::read(errPipe[0], buf, toRead);
                    if (n > 0) {
                        readThisTurn += static_cast<size_t>(n);
                        stderrTotalBytes += static_cast<size_t>(n);
                        stderrTail.append(buf, static_cast<size_t>(n));
                        bufferPeak = std::max(bufferPeak, stderrTail.size());
                        if (stderrTail.size() > perStreamResultBytes) {
                            stderrTail.erase(0, stderrTail.size() - perStreamResultBytes);
                        }

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

    std::ostringstream finalSs;
    finalSs << "{\n"
            << "  \"operationId\": " << operationId << ",\n"
            << "  \"token\": \"" << token << "\",\n"
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

CommandResult waitForOperation(const OperationRef &ref, unsigned int timeoutMs) {
    CommandResult result;
    result.operationId = ref.operationId;

    if (ref.opDir.empty()) {
        result.state = CommandState::Unknown;
        result.stderrText = "Invalid operation directory";
        return result;
    }

    const auto startTime = std::chrono::steady_clock::now();
    const auto deadline = startTime + std::chrono::milliseconds(timeoutMs);
    const std::string statePath = ref.opDir + "/state.json";

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        const ParsedState ps = parseStateFile(statePath, ref.operationId, ref.token, ref.collectorPid);

        if (ps.valid && ps.operationId == ref.operationId) {
            if (ps.stateStr == "Exited") {
                result.state = CommandState::Exited;
                result.exitStatus = ps.exitStatus;
                result.stdoutTruncated = ps.stdoutTruncated;
                result.stderrTruncated = ps.stderrTruncated;
                result.stdoutText = readFileBounded(ref.opDir + "/stdout.tail", 65536);
                result.stderrText = readFileBounded(ref.opDir + "/stderr.tail", 65536);
                result.bufferPeak = ps.bufferPeak;
                result.filePeak = ps.filePeak;
                {
                    std::lock_guard<std::mutex> lock(g_opRefsMutex);
                    g_activeOpRefs.erase(ref.operationId);
                }
                if (ref.isOwnedChild && ref.collectorPid > 0) {
                    int st = 0;
                    pid_t wp = ::waitpid(ref.collectorPid, &st, WNOHANG);
                    if (wp == ref.collectorPid) {
                        std::error_code ec;
                        std::filesystem::remove_all(ref.opDir, ec);
                    } else {
                        enqueuePendingReap(ref.collectorPid, ref.opDir);
                    }
                }
                return result;
            }
            if (ps.stateStr == "Unknown") {
                result.state = CommandState::Unknown;
                result.exitStatus = ps.exitStatus;
                result.stdoutTruncated = ps.stdoutTruncated;
                result.stderrTruncated = ps.stderrTruncated;
                result.stdoutText = readFileBounded(ref.opDir + "/stdout.tail", 65536);
                result.stderrText = readFileBounded(ref.opDir + "/stderr.tail", 65536);
                result.bufferPeak = ps.bufferPeak;
                result.filePeak = ps.filePeak;
                if (ref.isOwnedChild && ref.collectorPid > 0) {
                    int st = 0;
                    pid_t wp = ::waitpid(ref.collectorPid, &st, WNOHANG);
                    if (wp == 0) {
                        enqueuePendingReap(ref.collectorPid, "");
                    }
                }
                return result;
            }
        }

        if (ref.collectorPid > 0 && !isPidAlive(ref.collectorPid)) {
            const ParsedState psFinal = parseStateFile(statePath, ref.operationId, ref.token, ref.collectorPid);
            if (psFinal.valid && psFinal.operationId == ref.operationId && psFinal.stateStr == "Exited") {
                result.state = CommandState::Exited;
                result.exitStatus = psFinal.exitStatus;
                result.stdoutTruncated = psFinal.stdoutTruncated;
                result.stderrTruncated = psFinal.stderrTruncated;
                result.stdoutText = readFileBounded(ref.opDir + "/stdout.tail", 65536);
                result.stderrText = readFileBounded(ref.opDir + "/stderr.tail", 65536);
                result.bufferPeak = psFinal.bufferPeak;
                result.filePeak = psFinal.filePeak;
                {
                    std::lock_guard<std::mutex> lock(g_opRefsMutex);
                    g_activeOpRefs.erase(ref.operationId);
                }
                if (ref.isOwnedChild) {
                    int st = 0;
                    ::waitpid(ref.collectorPid, &st, 0);
                    std::error_code ec;
                    std::filesystem::remove_all(ref.opDir, ec);
                }
                return result;
            }
            if (ref.isOwnedChild) {
                int st = 0;
                ::waitpid(ref.collectorPid, &st, 0);
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

CommandResult waitForOperation(std::uint64_t operationId, unsigned int timeoutMs) {
    OperationRef ref;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_opRefsMutex);
        auto it = g_activeOpRefs.find(operationId);
        if (it != g_activeOpRefs.end()) {
            ref = it->second;
            found = true;
        }
    }

    if (!found) {
        CommandResult result;
        result.operationId = operationId;
        result.state = CommandState::Unknown;
        result.stderrText = "Operation directory not found or not registered for ID " + std::to_string(operationId);
        return result;
    }

    return waitForOperation(ref, timeoutMs);
}

std::optional<OperationRef> getActiveOperationRef(std::uint64_t operationId) {
    std::lock_guard<std::mutex> lock(g_opRefsMutex);
    auto it = g_activeOpRefs.find(operationId);
    if (it != g_activeOpRefs.end()) {
        return it->second;
    }
    return std::nullopt;
}

void registerOperationRef(const OperationRef &ref) {
    std::lock_guard<std::mutex> lock(g_opRefsMutex);
    g_activeOpRefs[ref.operationId] = ref;
}

void cleanupOperation(std::uint64_t operationId) {
    std::error_code ec;
    std::string opDir;
    {
        std::lock_guard<std::mutex> lock(g_opRefsMutex);
        auto it = g_activeOpRefs.find(operationId);
        if (it != g_activeOpRefs.end()) {
            opDir = it->second.opDir;
            g_activeOpRefs.erase(it);
        }
    }
    if (!opDir.empty()) {
        std::filesystem::remove_all(opDir, ec);
    }
}

} // namespace modernime::settings::detail
