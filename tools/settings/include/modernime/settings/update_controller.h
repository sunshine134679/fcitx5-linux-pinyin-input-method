#pragma once

#include "modernime/settings/runtime_controller.h"

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

    static UpdateStatus checkForUpdates(const Environment &environment = {});

    static RuntimeResult refreshPackageCache(const UpdateStatus &status,
                                             const Environment &environment = {});

    static RuntimeResult runSystemUpgrade(const UpdateStatus &status,
                                          const Environment &environment = {});
};

} // namespace modernime::settings
