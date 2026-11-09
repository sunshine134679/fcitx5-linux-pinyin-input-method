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
        snapshot.setupDescription = "输入功能已关闭。打开输入设置，启用 ModernIME 后再试用。";
    } else if (!runtime.available) {
        snapshot.setupAction = OverviewSetupAction::Install;
        snapshot.setupActionLabel = "检查安装";
        snapshot.setupDescription = "尚无法连接 Fcitx5。请先检查安装，再启动输入法。";
    } else if (!runtime.running) {
        snapshot.setupAction = OverviewSetupAction::Start;
        snapshot.setupActionLabel = "启动输入法";
        snapshot.setupDescription = "Fcitx5 尚未运行。启动后即可试用 ModernIME。";
    } else if (!runtime.modernimeAvailable) {
        snapshot.setupAction = OverviewSetupAction::Reload;
        snapshot.setupActionLabel = "重新加载插件";
        snapshot.setupDescription = "Fcitx5 尚未加载 ModernIME。安装后可重新加载；仍失败时查看系统与诊断。";
    } else if (!runtime.modernimeActive) {
        snapshot.setupAction = OverviewSetupAction::Activate;
        snapshot.setupActionLabel = "启用 ModernIME";
        snapshot.setupDescription = "ModernIME 已加载，启用后即可在下方试用。";
    } else {
        snapshot.setupDescription = "ModernIME 已启用，可以开始输入。";
    }
    const auto example = settings.pinyinScheme == core::PinyinScheme::Xiaohe ? "nihc"
        : settings.pinyinScheme == core::PinyinScheme::Ziranma ? "nihk" : "nihao";
    snapshot.trialInstructions = "输入 " + std::string(example) +
        "，按空格选择“你好”。若直接出现字母，可按 " + settings.toggleKey + " 切换到中文。";
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
