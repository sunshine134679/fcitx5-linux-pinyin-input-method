#include "modernime/settings/overview_model.h"

#include "modernime/settings/clipboard_history_model.h"
#include "modernime/settings/data_controller.h"

#include <fstream>
#include <string>
#include <string_view>
#include <utility>

namespace modernime::settings {
namespace {

std::string runtimeSummary(const RuntimeStatus &runtime) {
    if (runtime.modernimeActive) {
        return "ModernIME 正在运行";
    }
    if (!runtime.available) {
        return "无法检测 Fcitx5";
    }
    if (!runtime.running) {
        return "Fcitx5 未运行";
    }
    if (!runtime.modernimeAvailable) {
        return "ModernIME 未加载";
    }
    if (!runtime.inputContextAvailable) {
        return "ModernIME 已就绪";
    }
    return "ModernIME 未激活";
}

void addDiagnosticsNotice(OverviewSnapshot &snapshot, std::string message) {
    snapshot.notices.push_back(
        {SettingsPageId::Diagnostics, std::move(message)});
}

bool canReadExistingFile(const std::filesystem::path &path,
                         std::string_view label,
                         OverviewSnapshot &snapshot) {
    if (path.empty()) {
        addDiagnosticsNotice(snapshot,
                             std::string(label) + "路径未配置");
        return false;
    }

    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        addDiagnosticsNotice(snapshot, "无法检查" + std::string(label) +
                                           "：" + error.message());
        return false;
    }
    if (!exists) {
        return false;
    }
    if (!std::filesystem::is_regular_file(path, error) || error) {
        addDiagnosticsNotice(snapshot, "无法读取" + std::string(label));
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        addDiagnosticsNotice(snapshot, "无法读取" + std::string(label));
        return false;
    }
    return true;
}

void addRuntimeNotice(const RuntimeStatus &runtime,
                      OverviewSnapshot &snapshot) {
    if (runtime.modernimeActive ||
        (runtime.running && runtime.modernimeAvailable &&
         !runtime.inputContextAvailable)) {
        return;
    }
    if (!runtime.message.empty()) {
        addDiagnosticsNotice(snapshot, runtime.message);
    } else {
        addDiagnosticsNotice(snapshot, snapshot.runtimeSummary);
    }
}

} // namespace

OverviewSnapshot collectOverviewSnapshot(
    const core::SettingsPaths &paths,
    const core::ModernIMESettings &settings,
    const RuntimeStatus &runtime) {
    OverviewSnapshot snapshot;
    snapshot.runtimeSummary = runtimeSummary(runtime);
    if (!settings.inputEnabled) {
        snapshot.setupAction = OverviewSetupAction::Configure;
        snapshot.setupActionLabel = "打开输入设置";
        snapshot.setupDescription = "输入已关闭";
    } else if (!runtime.available) {
        snapshot.setupAction = OverviewSetupAction::Install;
        snapshot.setupActionLabel = "检查安装";
        snapshot.setupDescription = "无法连接 Fcitx5";
    } else if (!runtime.running) {
        snapshot.setupAction = OverviewSetupAction::Start;
        snapshot.setupActionLabel = "启动输入法";
        snapshot.setupDescription = "Fcitx5 未运行";
    } else if (!runtime.modernimeAvailable) {
        snapshot.setupAction = OverviewSetupAction::Reload;
        snapshot.setupActionLabel = "重新加载插件";
        snapshot.setupDescription = "ModernIME 未加载";
    } else if (runtime.desktopPanel.state != DesktopPanelState::NotApplicable &&
               runtime.desktopPanel.state != DesktopPanelState::Active) {
        snapshot.setupAction = OverviewSetupAction::DesktopPanel;
        snapshot.setupActionLabel = "检查候选面板";
        snapshot.setupDescription = runtime.desktopPanel.title.empty()
            ? "候选面板待检查" : runtime.desktopPanel.title;
    } else if (!runtime.modernimeActive) {
        snapshot.setupAction = OverviewSetupAction::Activate;
        snapshot.setupActionLabel = "启用 ModernIME";
        snapshot.setupDescription = "ModernIME 未激活";
    } else {
        snapshot.setupDescription = "ModernIME 正在运行";
    }
    snapshot.clipboardShortcut = settings.clipboardEnabled
        ? "输入 " + settings.clipboardTrigger + " 打开历史" : "剪贴板已关闭";
    snapshot.defaultMode = settings.defaultMode == core::InputMode::Chinese
                               ? "中文"
                               : "英文";
    snapshot.toggleKey = settings.toggleKey;
    addRuntimeNotice(runtime, snapshot);

    if (canReadExistingFile(paths.userDictionary, "个人词典", snapshot)) {
        snapshot.dictionaryEntries =
            DataController::loadDictionary(paths.userDictionary).size();
    }

    ClipboardHistoryModel clipboard(paths.clipboardHistory);
    std::string error;
    if (clipboard.reload(&error)) {
        snapshot.clipboardEntries = clipboard.entries().size();
    } else {
        addDiagnosticsNotice(
            snapshot, error.empty() ? "无法读取剪贴板历史"
                                    : "无法读取剪贴板历史：" + error);
    }

    error.clear();
    snapshot.learningEntries =
        DataController::learningEntryCount(paths.learningStore, &error);
    if (!error.empty()) {
        addDiagnosticsNotice(snapshot, "无法读取学习记录：" + error);
    }
    return snapshot;
}

} // namespace modernime::settings
