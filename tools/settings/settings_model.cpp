#include "modernime/settings/settings_model.h"

#include <utility>

namespace modernime::settings {

SettingsWindowModel::SettingsWindowModel(std::filesystem::path path)
    : path_(std::move(path)) {
    const auto loaded = core::SettingsStore::load(path_);
    loaded_ = loaded.settings;
    edited_ = loaded_;
    loadDiagnostics_ = loaded.diagnostics;
    loadedHash_ = loaded.contentHash;
    savedHash_ = loaded.contentHash;
    rawLines_ = loaded.rawLines;
}

bool SettingsWindowModel::dirty() const {
    return defaultsEdited_ || edited_ != loaded_;
}

std::vector<SettingsChange> SettingsWindowModel::changes() const {
    std::vector<SettingsChange> result;
    const auto add = [&](auto member, std::string_view group, std::string_view title) {
        if (edited_.*member != loaded_.*member) result.push_back({group, title});
    };
    using S = core::ModernIMESettings;
    add(&S::inputEnabled, "输入习惯", "启用 ModernIME");
    add(&S::defaultMode, "输入习惯", "默认输入状态");
    add(&S::toggleKey, "输入习惯", "中英文切换键");
    add(&S::pinyinScheme, "输入习惯", "拼音方案");
    for (const auto &option : core::kFuzzyPinyinOptions) {
        if (edited_.fuzzyPinyin.*option.member != loaded_.fuzzyPinyin.*option.member)
            result.push_back({"输入习惯 · 模糊音", option.label});
    }
    add(&S::punctuationEnabled, "输入习惯", "中文标点");
    add(&S::autoPairPunctuation, "输入习惯", "成对标点");
    add(&S::smartPunctuation, "输入习惯", "智能标点");
    add(&S::underscoreStyle, "输入习惯", "下划线输出");
    add(&S::punctuationOverrides, "输入习惯", "自定义标点");
    add(&S::rememberModePerApp, "输入习惯", "按应用记住中英状态");
    add(&S::appInputModes, "输入习惯", "应用默认输入状态");
    add(&S::undoCommitEnabled, "输入习惯", "撤销上屏");
    add(&S::temporaryEnglishEnabled, "输入习惯", "临时英文");
    add(&S::componentLookupEnabled, "输入习惯", "拆字查字");
    add(&S::calculatorEnabled, "输入习惯", "离线计算器");
    add(&S::symbolSearchEnabled, "输入习惯", "符号搜索");
    add(&S::symbolFavorites, "输入习惯", "收藏符号");
    add(&S::characterExtractionEnabled, "输入习惯", "以词定字");
    add(&S::learningEnabled, "输入习惯", "词频学习");
    add(&S::contextLearningEnabled, "输入习惯", "上下文学习");
    add(&S::candidatePageSize, "候选与外观", "候选栏长度");
    add(&S::candidateFontSize, "候选与外观", "候选字号");
    add(&S::candidateFontFamily, "候选与外观", "候选字体");
    add(&S::candidateTheme, "候选与外观", "候选配色");
    add(&S::candidateShape, "候选与外观", "候选轮廓");
    add(&S::candidateStyle, "候选与外观", "候选风格");
    add(&S::candidateSpacing, "候选与外观", "候选间距");
    add(&S::numberSelection, "候选与外观", "数字选词");
    add(&S::arrowNavigation, "候选与外观", "方向键选词");
    add(&S::pageNavigation, "候选与外观", "翻页键");
    add(&S::englishDefinitionEnabled, "候选与外观", "英文释义");
    add(&S::candidatePins, "候选与外观", "候选置顶规则");
    add(&S::clientTheme, "候选与外观", "界面主题");
    add(&S::clipboardEnabled, "剪贴板", "剪贴板历史");
    add(&S::clipboardTrigger, "剪贴板", "剪贴板触发键");
    if (result.empty() && defaultsEdited_) result.push_back({"维护", "写入默认设置"});
    return result;
}

bool SettingsWindowModel::hasExternalModification() const {
    const bool fileExists = std::filesystem::exists(path_);
    if (savedHash_.empty()) {
        return fileExists;
    }
    if (!fileExists) {
        return true;
    }
    return core::computeFileContentHash(path_) != savedHash_;
}

bool SettingsWindowModel::reloadFromDisk(std::string *error) {
    if (error != nullptr) {
        error->clear();
    }
    const auto loaded = core::SettingsStore::load(path_);
    if (loaded.isIoError()) {
        lastError_ = "无法重新读取设置文件，请稍后重试。";
        if (error != nullptr) {
            *error = lastError_;
        }
        return false;
    }
    loaded_ = loaded.settings;
    edited_ = loaded_;
    loadDiagnostics_ = loaded.diagnostics;
    loadedHash_ = loaded.contentHash;
    savedHash_ = loaded.contentHash;
    rawLines_ = loaded.rawLines;
    defaultsEdited_ = false;
    lastError_.clear();
    return true;
}

void SettingsWindowModel::setSettings(core::ModernIMESettings settings) {
    edited_ = std::move(settings);
    lastError_.clear();
}

void SettingsWindowModel::setCandidateAppearance(int candidatePageSize,
                                                 int candidateFontSize) {
    auto settings = edited_;
    settings.candidatePageSize = candidatePageSize;
    settings.candidateFontSize = candidateFontSize;
    setSettings(std::move(settings));
}

void SettingsWindowModel::setCandidateOptions(bool numberSelection,
                                              bool arrowNavigation,
                                              bool pageNavigation) {
    auto settings = edited_;
    settings.numberSelection = numberSelection;
    settings.arrowNavigation = arrowNavigation;
    settings.pageNavigation = pageNavigation;
    setSettings(std::move(settings));
}

void SettingsWindowModel::setCandidateSettings(int candidatePageSize,
                                              int candidateFontSize,
                                              bool numberSelection,
                                              bool arrowNavigation,
                                              bool pageNavigation,
                                              bool englishDefinitionEnabled) {
    auto settings = edited_;
    settings.candidatePageSize = candidatePageSize;
    settings.candidateFontSize = candidateFontSize;
    settings.numberSelection = numberSelection;
    settings.arrowNavigation = arrowNavigation;
    settings.pageNavigation = pageNavigation;
    settings.englishDefinitionEnabled = englishDefinitionEnabled;
    setSettings(std::move(settings));
}

void SettingsWindowModel::setCharacterExtractionEnabled(bool enabled) {
    auto settings = edited_;
    settings.characterExtractionEnabled = enabled;
    setSettings(std::move(settings));
}

void SettingsWindowModel::setClipboardOptions(bool enabled, std::string trigger) {
    auto settings = edited_;
    settings.clipboardEnabled = enabled;
    settings.clipboardTrigger = std::move(trigger);
    setSettings(std::move(settings));
}

void SettingsWindowModel::editDefaults() {
    setSettings(core::defaultSettings());
    defaultsEdited_ = true;
}

bool SettingsWindowModel::save(std::string *error) {
    const bool changed = edited_ != loaded_;
    auto saveResult = core::SettingsStore::saveWithResult(
        path_, edited_, savedHash_, &rawLines_);
    lastSaveResult_ = saveResult;
    if (saveResult.status != core::SettingsSaveStatus::Saved) {
        lastError_ = saveResult.error;
        if (error != nullptr) {
            *error = saveResult.error;
        }
        return false;
    }
    loaded_ = std::move(saveResult.settings);
    defaultsEdited_ = false;
    savedHash_ = std::move(saveResult.contentHash);
    loadedHash_ = savedHash_;
    rawLines_ = std::move(saveResult.rawLines);
    if (changed) {
        ++savedRevision_;
    }
    lastError_.clear();
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void SettingsWindowModel::resetEdits() {
    edited_ = loaded_;
    defaultsEdited_ = false;
    lastError_.clear();
}

} // namespace modernime::settings
