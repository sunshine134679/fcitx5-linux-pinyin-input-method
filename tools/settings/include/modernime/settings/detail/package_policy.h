#pragma once

#include "modernime/settings/package_operation_state.h"
#include "modernime/settings/update_controller.h"
#include "modernime/settings/detail/process_runner.h"

#include <string>
#include <string_view>
#include <vector>

namespace modernime::settings::detail {

struct AptPolicyResult {
    bool hasCandidate = false;
    std::string candidateVersion;
    bool hasRemoteRepo = false;
    std::string packageSource;
};

struct DnfInfoResult {
    bool hasCandidate = false;
    std::string candidateVersion;
    bool hasRemoteRepo = false;
    bool isInstalledOnly = false;
    std::string packageSource;
    std::string summary;
};

int compareDebianSegment(std::string_view a, std::string_view b);
int compareVersions(std::string_view lhs, std::string_view rhs);

AptPolicyResult parseAptPolicyOutput(std::string_view text, bool truncated = false);
std::string parseAptShowDescription(std::string_view text, bool truncated = false);
DnfInfoResult parseDnfInfoOutput(std::string_view text, bool truncated = false);

bool parseTrustedMetadata(std::string_view content, UpdateStatus &status);
std::string detectInstallChannelFromContent(std::string_view content);

void evaluateUpdateDecision(UpdateStatus &status, bool queryTruncated, bool queryFailed);

std::string buildFallbackCommand(PackageManagerKind manager, PackageOperationKind opKind);
std::vector<std::string> buildRefreshCacheArgv(PackageManagerKind manager,
                                               const std::string &pkexecPath,
                                               const std::string &managerPath);
std::vector<std::string> buildUpgradeArgv(PackageManagerKind manager,
                                         const std::string &pkexecPath,
                                         const std::string &managerPath);

std::string formatFailureMessage(const CommandResult &result,
                                 std::string_view action,
                                 const std::string &fallbackCommand);

} // namespace modernime::settings::detail
