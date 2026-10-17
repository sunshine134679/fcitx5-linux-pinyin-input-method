#pragma once

#include "modernime/settings/runtime_controller.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <optional>

namespace modernime::settings::detail {

enum class CommandMode {
    Query,
    PackageTransaction,
};

enum class CommandState {
    NotStarted,
    Exited,
    Continuing,
    Unknown,
};

struct CaptureLimits {
    std::size_t perStreamBufferBytes = 1048576; // 1 MiB default
    std::size_t perStreamResultBytes = 1048576; // 1 MiB for Query, 64 KiB for PackageTransaction
    std::size_t perStreamFileBytes = 1048576;   // 1 MiB diagnostic file
};

struct CommandSpec {
    std::vector<std::string> argv;
    Environment environment;
    std::uint32_t timeoutMs = 5000;
    CommandMode mode = CommandMode::Query;
    std::uint64_t operationId = 0;
    CaptureLimits limits;
};

struct CommandResult {
    CommandState state = CommandState::NotStarted;
    std::uint64_t operationId = 0;
    int exitStatus = -1;
    bool stdoutTruncated = false;
    bool stderrTruncated = false;
    bool timedOut = false;
    std::string stdoutText;
    std::string stderrText;
    std::size_t bufferPeak = 0;
    std::size_t filePeak = 0;
};

struct OperationRef {
    std::uint64_t operationId = 0;
    std::string token;
    std::string opDir;
    pid_t collectorPid = 0;
    bool isOwnedChild = false;
};

CommandResult runCommand(const CommandSpec &spec);
int runCommandCaptureChild(int argc, char **argv);
CommandResult waitForOperation(std::uint64_t operationId, unsigned int timeoutMs = 300000);
CommandResult waitForOperation(const OperationRef &ref, unsigned int timeoutMs = 300000);
std::optional<OperationRef> getActiveOperationRef(std::uint64_t operationId);
void registerOperationRef(const OperationRef &ref);
void cleanupOperation(std::uint64_t operationId);

} // namespace modernime::settings::detail
