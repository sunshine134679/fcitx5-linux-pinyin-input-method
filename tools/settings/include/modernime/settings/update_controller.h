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

    // 显式检查更新时由工作线程调用。waitForCompletion 等待自身事务的最终结果，
    // 初次等待超时仍由既有后台采集器跟踪；状态不明时保留操作锁并返回失败。
    static RuntimeResult refreshPackageCache(const UpdateStatus &status,
                                             const Environment &environment = {},
                                             bool waitForCompletion = false);

    // 客户端的实际升级仍采用复制命令到终端执行，不自动跨渠道安装。
    static RuntimeResult runSystemUpgrade(const UpdateStatus &status,
                                          const Environment &environment = {});

    // 确保桌面快捷方式存在，若已存在且图标发生变化则同步更新图标，并保留用户定制内容
    static bool ensureDesktopShortcut(const Environment &environment = {});

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
