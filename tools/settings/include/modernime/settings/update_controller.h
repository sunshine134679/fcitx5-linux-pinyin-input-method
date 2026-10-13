#pragma once

#include "modernime/settings/runtime_controller.h"

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

    static bool isPackageManagerLocked();

    static UpdateStatus checkForUpdates(const Environment &environment = {});

    static RuntimeResult refreshPackageCache(const UpdateStatus &status,
                                             const Environment &environment = {});

    static RuntimeResult runSystemUpgrade(const UpdateStatus &status,
                                          const Environment &environment = {});

    static bool hasActivePackageOperation();

    static bool waitForActivePackageOperation(unsigned int timeoutMs = 5000);

    static std::optional<RuntimeResult> takeCompletedPackageOperationResult();

    static std::string lastCompletedPackageOperationStdout();
};

} // namespace modernime::settings
