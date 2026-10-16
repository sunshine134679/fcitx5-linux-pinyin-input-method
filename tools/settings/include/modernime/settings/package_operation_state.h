#pragma once

#include "modernime/settings/runtime_controller.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

namespace modernime::settings {

enum class PackageOperationKind {
    RefreshCache,
    SystemUpgrade,
};

enum class PackageOperationPhase {
    Idle,
    Starting,
    Running,
    Continuing,
    Uncertain,
    Completed,
};

struct CompletedPackageOperation final {
    std::uint64_t operationId = 0;
    PackageOperationKind kind = PackageOperationKind::RefreshCache;
    RuntimeResult result;
    std::string stdoutText;
    std::string stderrText;
};

constexpr std::size_t kMaxUnconsumedResults = 8;

std::optional<std::uint64_t> tryBeginPackageOperation(PackageOperationKind kind);

bool advancePackageOperation(std::uint64_t id,
                             PackageOperationPhase expected,
                             PackageOperationPhase next);

bool finishPackageOperation(std::uint64_t id,
                            const RuntimeResult &result,
                            std::string stdoutText = {},
                            std::string stderrText = {},
                            bool recordUnconsumed = true);

bool hasActivePackageOperation();

PackageOperationPhase currentPackageOperationPhase();

std::optional<std::uint64_t> currentPackageOperationId();

std::size_t unconsumedPackageOperationCount();

bool waitForActivePackageOperation(unsigned int timeoutMs = 5000);

std::optional<RuntimeResult> takeCompletedPackageOperationResult(
    std::optional<std::uint64_t> id = std::nullopt);

std::string lastCompletedPackageOperationStdout();

void resetPackageOperationStateForTesting();

} // namespace modernime::settings
