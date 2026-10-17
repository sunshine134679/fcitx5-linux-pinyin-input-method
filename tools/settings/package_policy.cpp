#include "modernime/settings/detail/package_policy.h"

#include <glib.h>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace modernime::settings::detail {
namespace {

std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

int charOrder(char c) {
    if (c == '~') {
        return -1;
    }
    if (c == '\0') {
        return 0;
    }
    if (std::isalpha(static_cast<unsigned char>(c))) {
        return static_cast<unsigned char>(c);
    }
    return static_cast<int>(static_cast<unsigned char>(c)) + 256;
}

} // namespace

int compareDebianSegment(std::string_view a, std::string_view b) {
    std::size_t ia = 0;
    std::size_t ib = 0;
    while (ia < a.size() || ib < b.size()) {
        while ((ia < a.size() && !std::isdigit(static_cast<unsigned char>(a[ia]))) ||
               (ib < b.size() && !std::isdigit(static_cast<unsigned char>(b[ib])))) {
            const char ca = (ia < a.size() && !std::isdigit(static_cast<unsigned char>(a[ia])))
                                ? a[ia++]
                                : '\0';
            const char cb = (ib < b.size() && !std::isdigit(static_cast<unsigned char>(b[ib])))
                                ? b[ib++]
                                : '\0';
            if (ca != cb) {
                return charOrder(ca) < charOrder(cb) ? -1 : 1;
            }
        }
        while (ia < a.size() && a[ia] == '0') {
            ++ia;
        }
        while (ib < b.size() && b[ib] == '0') {
            ++ib;
        }
        std::size_t da = ia;
        while (ia < a.size() && std::isdigit(static_cast<unsigned char>(a[ia]))) {
            ++ia;
        }
        std::size_t db = ib;
        while (ib < b.size() && std::isdigit(static_cast<unsigned char>(b[ib]))) {
            ++ib;
        }
        const std::size_t lenA = ia - da;
        const std::size_t lenB = ib - db;
        if (lenA != lenB) {
            return lenA < lenB ? -1 : 1;
        }
        const int cmp = a.substr(da, lenA).compare(b.substr(db, lenB));
        if (cmp != 0) {
            return cmp < 0 ? -1 : 1;
        }
    }
    return 0;
}

int compareVersions(std::string_view lhsRaw, std::string_view rhsRaw) {
    const std::string lhsStorage = trim(lhsRaw);
    const std::string rhsStorage = trim(rhsRaw);
    std::string_view lhs = lhsStorage;
    std::string_view rhs = rhsStorage;
    if (lhs.starts_with('v') || lhs.starts_with('V')) {
        lhs.remove_prefix(1);
    }
    if (rhs.starts_with('v') || rhs.starts_with('V')) {
        rhs.remove_prefix(1);
    }

    auto splitEpoch = [](std::string_view s) -> std::pair<std::string_view, std::string_view> {
        const auto pos = s.find(':');
        if (pos == std::string_view::npos) {
            return {"0", s};
        }
        return {s.substr(0, pos), s.substr(pos + 1)};
    };
    auto splitRev = [](std::string_view s) -> std::pair<std::string_view, std::string_view> {
        const auto pos = s.rfind('-');
        if (pos == std::string_view::npos) {
            return {s, "0"};
        }
        return {s.substr(0, pos), s.substr(pos + 1)};
    };

    const auto [epochA, restA] = splitEpoch(lhs);
    const auto [epochB, restB] = splitEpoch(rhs);
    if (const int c = compareDebianSegment(epochA, epochB); c != 0) {
        return c;
    }
    const auto [upA, revA] = splitRev(restA);
    const auto [upB, revB] = splitRev(restB);
    if (const int c = compareDebianSegment(upA, upB); c != 0) {
        return c;
    }
    return compareDebianSegment(revA, revB);
}

AptPolicyResult parseAptPolicyOutput(std::string_view text, bool truncated) {
    AptPolicyResult res;
    if (truncated || text.empty()) {
        return res;
    }

    std::istringstream iss{std::string(text)};
    std::string line;
    while (std::getline(iss, line)) {
        const auto trimmed = trim(line);
        if (trimmed.starts_with("Candidate:")) {
            res.candidateVersion = trim(std::string_view(trimmed).substr(10));
            res.hasCandidate = !res.candidateVersion.empty() && res.candidateVersion != "(none)";
        } else if (!trimmed.empty() && std::isdigit(static_cast<unsigned char>(trimmed[0]))) {
            const auto spacePos = trimmed.find(' ');
            if (spacePos != std::string::npos) {
                const auto rest = trim(std::string_view(trimmed).substr(spacePos + 1));
                if (rest.find("://") != std::string::npos || rest.find("Packages") != std::string::npos) {
                    if (rest.find("/dpkg/status") == std::string::npos) {
                        res.hasRemoteRepo = true;
                        if (res.packageSource.empty()) {
                            res.packageSource = rest;
                        }
                    }
                }
            }
        }
    }
    return res;
}

std::string parseAptShowDescription(std::string_view text, bool truncated) {
    if (truncated || text.empty()) {
        return {};
    }
    std::istringstream iss{std::string(text)};
    std::string line;
    while (std::getline(iss, line)) {
        const auto trimmed = trim(line);
        if (trimmed.starts_with("Description:") || trimmed.starts_with("Description-en:")) {
            const auto colon = trimmed.find(':');
            return trim(std::string_view(trimmed).substr(colon + 1));
        }
    }
    return {};
}

DnfInfoResult parseDnfInfoOutput(std::string_view text, bool truncated) {
    DnfInfoResult res;
    if (truncated || text.empty()) {
        return res;
    }

    std::istringstream iss{std::string(text)};
    std::string line;
    enum class DnfSection { None, Installed, Available };
    DnfSection currentSection = DnfSection::None;

    std::string installedVer;
    std::string installedRel;
    std::string installedFromRepo;
    std::string installedSummary;

    std::string pendingAvailVer;
    std::string pendingAvailRel;
    std::string pendingAvailRepo;
    std::string pendingAvailSummary;

    std::string bestAvailCandidate;
    std::string bestAvailRepo;
    std::string bestAvailSummary;

    auto isRemoteRepo = [](std::string_view r) {
        return !r.empty() && r != "@System" && r != "installed" &&
               r != "@commandline" && r != "cmdline" &&
               !r.starts_with("@System") && !r.starts_with("/");
    };

    auto commitAvailableCandidate = [&]() {
        if (pendingAvailVer.empty() || pendingAvailRepo.empty()) {
            pendingAvailVer.clear();
            pendingAvailRel.clear();
            pendingAvailRepo.clear();
            pendingAvailSummary.clear();
            return;
        }
        const std::string cand = pendingAvailRel.empty()
                                     ? pendingAvailVer
                                     : (pendingAvailVer + "-" + pendingAvailRel);
        if (bestAvailCandidate.empty() ||
            compareVersions(cand, bestAvailCandidate) > 0) {
            bestAvailCandidate = cand;
            bestAvailRepo = pendingAvailRepo;
            if (!pendingAvailSummary.empty()) {
                bestAvailSummary = pendingAvailSummary;
            }
        }
        pendingAvailVer.clear();
        pendingAvailRel.clear();
        pendingAvailRepo.clear();
        pendingAvailSummary.clear();
    };

    while (std::getline(iss, line)) {
        const auto trimmed = trim(line);
        if (trimmed == "Installed Packages") {
            commitAvailableCandidate();
            currentSection = DnfSection::Installed;
            continue;
        }
        if (trimmed == "Available Packages") {
            commitAvailableCandidate();
            currentSection = DnfSection::Available;
            continue;
        }

        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const auto key = trim(std::string_view(line).substr(0, colon));
        const auto val = trim(std::string_view(line).substr(colon + 1));

        if (key == "Name" && currentSection == DnfSection::Available) {
            commitAvailableCandidate();
        }

        if (currentSection == DnfSection::Available || currentSection == DnfSection::None) {
            if (key == "Version") {
                pendingAvailVer = val;
            } else if (key == "Release") {
                pendingAvailRel = val;
            } else if (key == "Repository") {
                if (isRemoteRepo(val)) {
                    pendingAvailRepo = val;
                }
            } else if (key == "Summary" && pendingAvailSummary.empty()) {
                pendingAvailSummary = val;
            }
        }
        if (currentSection == DnfSection::Installed || currentSection == DnfSection::None) {
            if (key == "Version") {
                installedVer = val;
            } else if (key == "Release") {
                installedRel = val;
            } else if (key == "From repo") {
                if (isRemoteRepo(val)) {
                    installedFromRepo = val;
                }
            } else if (key == "Summary" && installedSummary.empty()) {
                installedSummary = val;
            }
        }
    }
    commitAvailableCandidate();

    if (!bestAvailCandidate.empty()) {
        res.hasRemoteRepo = true;
        res.hasCandidate = true;
        res.isInstalledOnly = false;
        res.candidateVersion = bestAvailCandidate;
        res.packageSource = "DNF 仓库 " + bestAvailRepo;
        res.summary = bestAvailSummary;
    } else if (!installedFromRepo.empty()) {
        res.hasRemoteRepo = true;
        res.hasCandidate = true;
        res.isInstalledOnly = true;
        const std::string cand = installedRel.empty() ? installedVer : (installedVer + "-" + installedRel);
        res.candidateVersion = cand;
        res.packageSource = "DNF 仓库 " + installedFromRepo;
        res.summary = installedSummary;
    }
    return res;
}

bool parseTrustedMetadata(std::string_view content, UpdateStatus &status) {
    if (content.empty()) {
        return false;
    }
    std::istringstream in{std::string(content)};
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line.starts_with('#')) {
            continue;
        }
        const auto eq = line.find('=');
        const auto colon = line.find(':');
        std::size_t sep = std::string::npos;
        if (eq != std::string::npos && (colon == std::string::npos || eq < colon)) {
            sep = eq;
        } else if (colon != std::string::npos) {
            sep = colon;
        }
        if (sep == std::string::npos) {
            continue;
        }
        const std::string key = trim(std::string_view(line).substr(0, sep));
        const std::string val = trim(std::string_view(line).substr(sep + 1));
        if (key == "Version" || key == "candidate_version") {
            status.candidateVersion = val;
        } else if (key == "Source" || key == "Origin" || key == "package_source") {
            status.packageSource = val;
        } else if (key == "Summary" || key == "Description" || key == "changelog_summary") {
            status.changelogSummary = val;
        } else if (key == "Manager" || key == "manager") {
            if (val == "dnf" || val == "rpm") {
                status.manager = PackageManagerKind::Dnf;
            } else if (val == "apt" || val == "deb") {
                status.manager = PackageManagerKind::Apt;
            }
        }
    }
    return !status.candidateVersion.empty();
}

std::string detectInstallChannelFromContent(std::string_view content) {
    std::istringstream in{std::string(content)};
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.starts_with("channel=")) {
            return trim(std::string_view(line).substr(8));
        }
    }
    return "unknown";
}

void evaluateUpdateDecision(UpdateStatus &status, bool queryTruncated, bool queryFailed) {
    status.checked = true;

    if (status.manager == PackageManagerKind::Dnf) {
        status.refreshCacheCommand = "sudo dnf makecache";
        status.upgradeCommand = "sudo dnf makecache && sudo dnf upgrade modernime";
        status.privilegedCommand = "pkexec dnf upgrade -y modernime";
    } else {
        if (status.manager == PackageManagerKind::Unknown) {
            status.manager = PackageManagerKind::Apt;
        }
        status.refreshCacheCommand = "sudo apt update";
        status.upgradeCommand = "sudo apt update && sudo apt install --only-upgrade modernime";
        status.privilegedCommand = "pkexec apt-get install --only-upgrade -y modernime";
    }

    if (status.installChannel == "script") {
        const char *targetCh = (status.manager == PackageManagerKind::Dnf) ? "rpm" : "deb";
        const char *pkgCmd = (status.manager == PackageManagerKind::Dnf)
                                 ? "sudo dnf makecache && sudo dnf install -y modernime"
                                 : "sudo apt update && sudo apt install -y modernime";
        status.upgradeCommand =
            std::string("modernime-migrate-channel --to-channel ") + targetCh +
            " --package-cmd \"" + pkgCmd + "\"";
    }

    if (queryTruncated) {
        status.offlineOrUnreachable = true;
        status.cacheMayBeStale = false;
        status.updateAvailable = false;
        status.candidateVersion = "暂不可用（查询输出截断）";
        if (status.packageSource.empty()) {
            status.packageSource = "软件包查询输出超出缓冲区上限";
        }
        if (status.changelogSummary.empty()) {
            status.changelogSummary = "软件包信息输出截断，无法验证完整元数据";
        }
        status.statusMessage =
            "软件包查询输出被截断（超出安全缓冲区上限），无法确认最新版本状态。请先在终端执行 " +
            status.refreshCacheCommand + " 刷新缓存后重试。";
        return;
    }

    if (queryFailed || status.candidateVersion.empty() || status.candidateVersion == "(none)") {
        status.offlineOrUnreachable = true;
        status.cacheMayBeStale = false;
        status.updateAvailable = false;
        status.candidateVersion = "暂不可用（离线或未配置软件源）";
        if (status.packageSource.empty()) {
            status.packageSource = "未检测到已配置的 APT / DNF 软件源索引";
        }
        if (status.changelogSummary.empty()) {
            status.changelogSummary =
                "离线、未配置软件源或尚未刷新本地缓存，本地功能与词库完好无损";
        }
        status.statusMessage =
            "当前处于离线状态、尚未配置 ModernIME 系统软件源或未刷新缓存（可先执行 " +
            status.refreshCacheCommand +
            " 或点击“刷新软件源缓存”）；本地输入法与数据运行正常（未发现软件损坏）。";
        return;
    }

    status.offlineOrUnreachable = false;
    const int cmp = compareVersions(status.candidateVersion, status.currentVersion);
    if (cmp > 0) {
        status.updateAvailable = true;
        status.cacheMayBeStale = false;
        if (status.changelogSummary.empty()) {
            status.changelogSummary =
                "包含候选排序优化、词库更新与 SQLite/LMDB 事务安全升级保护";
        }
        status.statusMessage =
            "发现新版本 " + status.candidateVersion + "（当前版本 " +
            status.currentVersion +
            "）。实际安装与升级必须通过系统包管理器（apt / dnf）执行。";
    } else {
        status.updateAvailable = false;
        status.cacheMayBeStale = true;
        if (status.changelogSummary.empty()) {
            status.changelogSummary =
                "本地软件源缓存中当前安装的 ModernIME 已是最新版本";
        }
        status.statusMessage =
            "本地软件源缓存中当前已是最新版本（" + status.currentVersion +
            "）。提示：包管理器默认读取本地缓存索引，请先点击“刷新软件源缓存”或在终端执行 " +
            status.refreshCacheCommand + " 同步远端最新元数据后再检查。";
    }

    if (status.packageManagerLocked) {
        status.statusMessage =
            "系统包管理器正忙（检测到锁占用，可能有自动更新或包管理操作正在运行），请稍后重试。";
    }
}

std::string buildFallbackCommand(PackageManagerKind manager, PackageOperationKind opKind) {
    if (manager == PackageManagerKind::Dnf) {
        return (opKind == PackageOperationKind::SystemUpgrade)
                   ? "sudo dnf makecache && sudo dnf upgrade modernime"
                   : "sudo dnf makecache";
    }
    return (opKind == PackageOperationKind::SystemUpgrade)
               ? "sudo apt update && sudo apt install --only-upgrade modernime"
               : "sudo apt update";
}

std::vector<std::string> buildRefreshCacheArgv(PackageManagerKind manager,
                                               const std::string &pkexecPath,
                                               const std::string &managerPath) {
    if (manager == PackageManagerKind::Dnf) {
        return {pkexecPath, managerPath, "makecache"};
    }
    return {pkexecPath, managerPath, "update"};
}

std::vector<std::string> buildUpgradeArgv(PackageManagerKind manager,
                                         const std::string &pkexecPath,
                                         const std::string &managerPath) {
    if (manager == PackageManagerKind::Dnf) {
        return {pkexecPath, managerPath, "upgrade", "-y", "modernime"};
    }
    return {pkexecPath, managerPath, "install", "--only-upgrade", "-y", "modernime"};
}

std::string formatFailureMessage(const CommandResult &result,
                                 std::string_view action,
                                 const std::string &fallbackCommand) {
    std::string message(action);
    message += "未完成";
    if (result.state == CommandState::NotStarted) {
        message += "（无法启动系统认证或包管理器）";
    } else if (result.exitStatus >= 0) {
        message += "（退出码 " + std::to_string(result.exitStatus) + "）";
    }
    message += "。";

    if (result.stderrText.find("No authentication agent found") != std::string::npos ||
        result.stderrText.find("Error creating textual authentication agent") != std::string::npos) {
        message += "当前会话没有图形授权代理，请关闭此窗口，从桌面应用菜单重新打开 ModernIME 设置后重试。";
    }

    const auto rawDiagnostic = trim(result.stderrText.empty() ? result.stdoutText
                                                              : result.stderrText);
    if (!rawDiagnostic.empty()) {
        gchar *valid = g_utf8_make_valid(rawDiagnostic.data(), rawDiagnostic.size());
        constexpr glong kMaxDiagnosticChars = 600;
        const glong diagnosticLength = g_utf8_strlen(valid, -1);
        const bool truncated = diagnosticLength > kMaxDiagnosticChars;
        gchar *excerpt = g_utf8_substring(valid, 0,
                                          truncated ? kMaxDiagnosticChars : diagnosticLength);
        message += "\n系统返回：";
        message += excerpt;
        if (truncated) {
            message += "…";
        }
        g_free(excerpt);
        g_free(valid);
    }
    message += "\n也可在终端执行：" + fallbackCommand;
    return message;
}

} // namespace modernime::settings::detail
