#pragma once

#include "modernime/settings/package_operation_state.h"
#include "modernime/settings/runtime_controller.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace modernime::settings {

enum class PackageManagerKind {
    Apt,
    Dnf,
    Unknown,
};

struct UpdateStatus final {
    bool checked = false;
    bool offlineOrUnreachable = false;
    bool cacheMayBeStale = false;
    bool updateAvailable = false;
    bool packageManagerLocked = false;
    PackageManagerKind manager = PackageManagerKind::Unknown;
    std::string currentVersion;
    std::string candidateVersion;
    std::string packageSource;
    std::string changelogSummary;
    std::string installChannel;
    std::string refreshCacheCommand;
    std::string upgradeCommand;
    std::string privilegedCommand;
    std::string statusMessage;
};

class UpdateController final {
public:
    static int compareVersions(std::string_view lhs, std::string_view rhs);

    static bool isPackageManagerLocked(const Environment &environment = {});

    static std::string detectInstallChannel(const Environment &environment = {});

    static UpdateStatus checkForUpdates(const Environment &environment = {});

    static RuntimeResult refreshPackageCache(const UpdateStatus &status,
                                             const Environment &environment = {});

    static RuntimeResult runSystemUpgrade(const UpdateStatus &status,
                                          const Environment &environment = {});

    static bool hasActivePackageOperation();

    static bool waitForActivePackageOperation(unsigned int timeoutMs = 5000);

    static std::optional<RuntimeResult> takeCompletedPackageOperationResult(
        std::optional<std::uint64_t> id = std::nullopt);

    static std::string lastCompletedPackageOperationStdout();

    static std::optional<std::uint64_t> tryBeginPackageOperation(PackageOperationKind kind);

    static bool advancePackageOperation(std::uint64_t id,
                                         PackageOperationPhase expected,
                                         PackageOperationPhase next);

    static bool finishPackageOperation(std::uint64_t id,
                                       const RuntimeResult &result,
                                       std::string stdoutText = {},
                                       std::string stderrText = {},
                                       bool recordUnconsumed = true);

    static PackageOperationPhase currentPackageOperationPhase();

    static std::optional<std::uint64_t> currentPackageOperationId();

    static std::size_t unconsumedPackageOperationCount();

    static void resetPackageOperationStateForTesting();
};

} // namespace modernime::settings
