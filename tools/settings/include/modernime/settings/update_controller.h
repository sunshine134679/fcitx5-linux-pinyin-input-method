#pragma once

#include "modernime/settings/package_operation_state.h"
#include "modernime/settings/runtime_controller.h"

#include <cstdint>
#include <functional>
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
    bool checkedOnline = false;
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
    std::string downloadUrl;
};

struct DownloadedUpdate final {
    RuntimeResult result{false, {}};
    std::string directory;
    std::string version;
};

struct UpdateProgress final {
    std::string phase;
    std::uint64_t received = 0;
    std::uint64_t total = 0;
};

using UpdateProgressCallback = std::function<void(const UpdateProgress &)>;

class UpdateController final {
public:
    static int compareVersions(std::string_view lhs, std::string_view rhs);

    static bool isPackageManagerLocked(const Environment &environment = {});

    static std::string detectInstallChannel(const Environment &environment = {});

    static UpdateStatus checkForUpdates(const Environment &environment = {});

    // 只读取官方发布信息，不刷新系统软件源、不请求管理员授权。
    static UpdateStatus checkOnlineRelease(const Environment &environment = {});

    static DownloadedUpdate downloadUpdate(const UpdateStatus &status,
        const Environment &environment = {}, UpdateProgressCallback progress = {});
    static RuntimeResult installUpdate(const DownloadedUpdate &download,
        const Environment &environment = {}, UpdateProgressCallback progress = {});
    // 启动独立助手等待本进程退出，再重启输入法与设置窗口。
    static RuntimeResult restartUpdatedClient(const DownloadedUpdate &download,
        const Environment &environment = {});

    // 显式维护系统软件源时由工作线程调用。waitForCompletion 等待自身事务的最终结果，
    // 初次等待超时仍由既有后台采集器跟踪；状态不明时保留操作锁并返回失败。
    static RuntimeResult refreshPackageCache(const UpdateStatus &status,
                                             const Environment &environment = {},
                                             bool waitForCompletion = false);

    // 已有系统包事务入口；保持原渠道，不自动迁移脚本安装。
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
