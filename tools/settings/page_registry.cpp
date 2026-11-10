#include "modernime/settings/page_registry.h"

#include <array>
#include <string>

namespace modernime::settings {
namespace {

constexpr std::array kPages{
    SettingsPageDefinition{SettingsPageId::Overview, "overview", "概览",
                           "查看 ModernIME 设置和本地数据概况", "主页"},
    SettingsPageDefinition{SettingsPageId::Input, "input", "输入与按键",
                           "配置输入状态、中英文切换键与以词定字", "输入"},
    SettingsPageDefinition{SettingsPageId::Candidates, "candidates", "候选与外观",
                           "候选词个数、字体字号与外观预览", "输入"},
    SettingsPageDefinition{SettingsPageId::Dictionary, "dictionary", "个人词典",
                           "维护个人词条和专业名词", "数据与工具"},
    SettingsPageDefinition{SettingsPageId::Clipboard, "clipboard", "剪贴板",
                           "管理 V+2 剪贴板功能和历史内容", "数据与工具"},
    SettingsPageDefinition{SettingsPageId::Learning, "learning", "智能学习",
                           "管理用户习惯学习和上下文排序", "数据与工具"},
    SettingsPageDefinition{SettingsPageId::Updates, "updates", "更新与安装",
                           "检查新版本、安装渠道与升级引导", "系统与支持"},
    SettingsPageDefinition{SettingsPageId::Diagnostics, "diagnostics",
                           "系统与诊断", "检查 Fcitx5 和 ModernIME 运行状态",
                           "系统与支持"},
};

constexpr std::array kSearchEntries{
    SettingsSearchEntry{SettingsPageId::Overview, "", "概览",
                        "查看 ModernIME 设置和本地数据概况", "overview home"},
    SettingsSearchEntry{SettingsPageId::Input, "", "输入与按键",
                        "配置输入状态、中英文切换键与以词定字", "input"},
    SettingsSearchEntry{SettingsPageId::Candidates, "", "候选与外观",
                        "候选词个数、字体字号与外观预览", "candidates appearance preview"},
    SettingsSearchEntry{SettingsPageId::Dictionary, "", "个人词典",
                        "维护个人词条和专业名词", "dictionary"},
    SettingsSearchEntry{SettingsPageId::Clipboard, "", "剪贴板",
                        "管理 V+2 剪贴板功能和历史内容", "clipboard"},
    SettingsSearchEntry{SettingsPageId::Learning, "", "智能学习",
                        "管理用户习惯学习和上下文排序", "learning"},
    SettingsSearchEntry{SettingsPageId::Updates, "", "更新与安装",
                        "检查新版本、安装渠道与升级引导", "updates upgrade install channel"},
    SettingsSearchEntry{SettingsPageId::Diagnostics, "", "系统与诊断",
                        "检查 Fcitx5 和 ModernIME 运行状态", "diagnostics system"},
    SettingsSearchEntry{SettingsPageId::Input, "input-enabled", "启用 ModernIME",
                        "控制 ModernIME 是否接收键盘输入", "enable input"},
    SettingsSearchEntry{SettingsPageId::Input, "default-mode", "默认输入状态",
                        "选择启动时默认使用中文或英文", "default mode chinese english"},
    SettingsSearchEntry{SettingsPageId::Input, "pinyin.scheme", "拼音方案",
                        "全拼、简拼、小鹤双拼或自然码双拼", "shuangpin pinyin xiaohe ziranma 双拼"},
    SettingsSearchEntry{SettingsPageId::Input, "pinyin.fuzzy_z_zh", "模糊音 z / zh",
                        "混用平舌和翘舌发音", "fuzzy pinyin z zh 模糊音"},
    SettingsSearchEntry{SettingsPageId::Input, "pinyin.fuzzy_c_ch", "模糊音 c / ch",
                        "混用平舌和翘舌发音", "fuzzy pinyin c ch 模糊音"},
    SettingsSearchEntry{SettingsPageId::Input, "pinyin.fuzzy_s_sh", "模糊音 s / sh",
                        "混用平舌和翘舌发音", "fuzzy pinyin s sh 模糊音"},
    SettingsSearchEntry{SettingsPageId::Input, "pinyin.fuzzy_l_n", "模糊音 l / n",
                        "混用边音和鼻音", "fuzzy pinyin l n 模糊音"},
    SettingsSearchEntry{SettingsPageId::Input, "pinyin.fuzzy_en_eng", "模糊音 en / eng",
                        "混用前鼻音和后鼻音", "fuzzy pinyin en eng 模糊音"},
    SettingsSearchEntry{SettingsPageId::Input, "pinyin.fuzzy_in_ing", "模糊音 in / ing",
                        "混用前鼻音和后鼻音", "fuzzy pinyin in ing 模糊音"},
    SettingsSearchEntry{SettingsPageId::Input, "toggle-key", "中英文切换快捷键",
                        "设置中英文输入状态之间的切换按键", "shortcut hotkey toggle ctrl space shift"},
    SettingsSearchEntry{SettingsPageId::Input, "character-extraction", "以词定字快捷键",
                        "输入拼音时使用 [ 提取首字上屏，] 提取尾字上屏", "character extraction bracket 以词定字"},
    SettingsSearchEntry{SettingsPageId::Input, "punctuation", "中文标点",
                        "在中文状态下使用全角标点", "punctuation full width"},
    SettingsSearchEntry{SettingsPageId::Input, "punctuation.auto_pair", "自动补全成对标点",
                        "补全括号、书名号和引号，光标留在中间", "punctuation pair bracket quote 括号 引号 补对"},
    SettingsSearchEntry{SettingsPageId::Input, "punctuation.smart", "智能保留英文标点",
                        "在网址、路径和英文标识符内保留半角", "smart punctuation url path 半角 网址 路径"},
    SettingsSearchEntry{SettingsPageId::Input, "punctuation.underscore", "下划线输出",
                        "智能、始终 _ 或始终 ——", "underscore 下划线 破折号"},
    SettingsSearchEntry{SettingsPageId::Input, "punctuation.map", "逐键自定义标点",
                        "为英文标点按键指定固定输出，覆盖智能转换与配对", "punctuation mapping override 自定义标点 斜杠 固定输出"},
    SettingsSearchEntry{SettingsPageId::Input, "input.remember_per_app", "按应用记住中英状态",
                        "在本次运行期间记住每个应用的中英切换结果", "remember per app 应用 记忆 中英状态"},
    SettingsSearchEntry{SettingsPageId::Input, "input.app", "应用默认输入状态",
                        "管理应用的默认中文或英文规则", "application mode code 应用规则 应用模式"},
    SettingsSearchEntry{SettingsPageId::Input, "input.undo_commit", "撤销上屏并重新选词",
                        "Ctrl+Shift+BackSpace 恢复最近一次候选提交的拼音", "undo commit 撤销 重选 重新选词"},
    SettingsSearchEntry{SettingsPageId::Input, "input.temporary_english", "临时英文",
                        "Ctrl+Shift+E 输入一段英文，结束后回到中文", "temporary english 临时英文"},
    SettingsSearchEntry{SettingsPageId::Input, "input.component_lookup", "拆字查字",
                        "输入 u+ 和部件拼音查找汉字", "component lookup u+ 拆字 部件"},
    SettingsSearchEntry{SettingsPageId::Input, "component-preview", "试用拆字查字",
                        "用部件或拼音预览离线查字结果", "component lookup 试查 试用拆字"},
    SettingsSearchEntry{SettingsPageId::Input, "input.calculator", "离线计算器",
                        "输入 v= 后计算表达式，Enter 或空格上屏", "calculator calculate v= 计算器 算式"},
    SettingsSearchEntry{SettingsPageId::Input, "calculator-preview", "试用离线计算器",
                        "预览表达式结果与错误提示", "calculator preview 试算 试用计算器"},
    SettingsSearchEntry{SettingsPageId::Input, "script-traditional", "简繁输出",
                        "读取 Fcitx 当前状态，立即切换简体或繁体输出", "traditional simplified 繁体 简体 简繁 Ctrl+Shift+F"},
    SettingsSearchEntry{SettingsPageId::Input, "script-refresh", "刷新简繁状态",
                        "重新读取当前简繁状态和自定义快捷键", "script refresh 简繁 快捷键"},
    SettingsSearchEntry{SettingsPageId::Input, "symbols.enabled", "符号搜索",
                        "输入 v; 按名称或拼音查找符号", "symbol symbols v; 符号 搜索 emoji 表情"},
    SettingsSearchEntry{SettingsPageId::Input, "symbol-browser", "符号与收藏",
                        "浏览分类、搜索、预览和复制，管理收藏符号", "symbol favorite 收藏符号 分类 标点 数学 箭头 单位"},
    SettingsSearchEntry{SettingsPageId::Candidates, "candidate.pin", "候选置顶规则",
                        "按输入方案和按键选择首选词，管理与取消置顶", "candidate pin 置顶 固定首选 取消置顶"},
    SettingsSearchEntry{SettingsPageId::Candidates, "candidate-page-size", "候选栏长度",
                        "以可容纳的单字候选数为基准（3–9 个），词组按长度自动分页", "candidate page size 候选词数 每页候选词数 单字 容量"},
    SettingsSearchEntry{SettingsPageId::Candidates, "client-theme", "界面主题",
                        "选择清爽、暖色、柔和、深色或高对比主题", "theme appearance dark 客户端 界面 主题 配色 深色 夜间 暖砂 玫瑰 雾紫 高对比"},
    SettingsSearchEntry{SettingsPageId::Candidates, "candidate-font-size", "候选字号大小",
                        "设置候选窗显示的文字大小（14 ~ 28 pt）", "candidate font size 字号 大小"},
    SettingsSearchEntry{SettingsPageId::Candidates, "candidate-font-family", "候选字体",
                        "选择系统中已安装的字体", "candidate font family 字体"},
    SettingsSearchEntry{SettingsPageId::Candidates, "candidate-theme", "候选栏配色",
                        "九套候选栏配色，选中即可预览", "candidate theme color 候选配色 配色 主题 深色 黑白 青竹 暖砂 玫瑰 雾紫 深海 高对比"},
    SettingsSearchEntry{SettingsPageId::Candidates, "candidate-shape", "候选轮廓",
                        "圆润、方正或胶囊形状", "candidate shape radius 轮廓 形状 圆角 胶囊 方正"},
    SettingsSearchEntry{SettingsPageId::Candidates, "candidate-spacing", "候选间距",
                        "紧凑、标准或宽松", "candidate spacing compact 间距 紧凑 密度"},
    SettingsSearchEntry{SettingsPageId::Candidates, "number-selection", "数字键选择候选",
                        "使用数字键选择当前候选项", "number selection candidate"},
    SettingsSearchEntry{SettingsPageId::Candidates, "arrow-navigation", "方向键编辑与选词",
                        "左右移动拼音光标，上下切换候选项",
                        "arrow navigation candidate cursor composition"},
    SettingsSearchEntry{SettingsPageId::Candidates, "page-navigation", "候选翻页",
                        "使用 PageUp / PageDown 和 + / = 翻页",
                        "page navigation candidate"},
    SettingsSearchEntry{SettingsPageId::Candidates, "english-definition", "显示英文单词中文释义",
                        "输入英文时提示词性与简明中文释义",
                        "english definition translation 英文 释义 英文释义 翻译 候选"},
    SettingsSearchEntry{SettingsPageId::Dictionary, "dictionary-add", "添加词条",
                        "向个人词典添加一条词条", "dictionary add entry"},
    SettingsSearchEntry{SettingsPageId::Dictionary, "quick-phrases", "快捷短语",
                        "用字母缩写输入固定文本，管理与应用快捷短语", "quick phrase abbreviation 缩写 固定文本 快速输入 Super"},
    SettingsSearchEntry{SettingsPageId::Dictionary, "quick-phrase-add", "添加快捷短语",
                        "为常用单行文本设置字母缩写", "quick phrase add 添加短语 缩写"},
    SettingsSearchEntry{SettingsPageId::Dictionary, "quick-phrase-search", "搜索快捷短语",
                        "按缩写或文本查找，选择后编辑或删除", "quick phrase search 编辑 删除 搜索短语"},
    SettingsSearchEntry{SettingsPageId::Dictionary, "quick-phrase-save", "保存快捷短语",
                        "保存短语草稿并应用，应用失败时可重试", "quick phrase save apply 保存 应用 重载 短语"},
    SettingsSearchEntry{SettingsPageId::Dictionary, "dictionary-edit", "编辑词条",
                        "编辑选中的个人词典词条", "dictionary edit entry"},
    SettingsSearchEntry{SettingsPageId::Dictionary, "dictionary-delete", "删除词条",
                        "删除选中的个人词典词条", "dictionary delete entry"},
    SettingsSearchEntry{SettingsPageId::Dictionary, "dictionary-import", "导入词典",
                        "从文本文件导入个人词典词条", "dictionary import"},
    SettingsSearchEntry{SettingsPageId::Dictionary, "dictionary-export", "导出词典",
                        "将当前个人词典导出为文本文件", "dictionary export"},
    SettingsSearchEntry{SettingsPageId::Clipboard, "clipboard-enabled", "启用剪贴板",
                        "开启本地剪贴板历史功能", "clipboard enable V+2"},
    SettingsSearchEntry{SettingsPageId::Clipboard, "clipboard-trigger", "剪贴板触发键",
                        "设置打开剪贴板历史的触发键", "clipboard trigger shortcut"},
    SettingsSearchEntry{SettingsPageId::Clipboard, "clipboard-search", "搜索剪贴板",
                        "按中文或英文查找历史和收藏内容", "clipboard search filter 搜索 筛选"},
    SettingsSearchEntry{SettingsPageId::Clipboard, "clipboard-favorites", "只看收藏",
                        "查看独立保存的剪贴板收藏", "clipboard favorites 收藏 常用"},
    SettingsSearchEntry{SettingsPageId::Clipboard, "clipboard-favorite", "收藏选中内容",
                        "收藏或取消收藏完整文字，不删除历史", "clipboard favorite 收藏 取消收藏"},
    SettingsSearchEntry{SettingsPageId::Clipboard, "clipboard-clear-favorites", "清空剪贴板收藏",
                        "单独清空收藏，保留剪贴板历史", "clipboard clear favorites 清空收藏"},
    SettingsSearchEntry{SettingsPageId::Clipboard, "clipboard-copy", "复制选中",
                        "复制选中的剪贴板历史内容", "clipboard copy"},
    SettingsSearchEntry{SettingsPageId::Clipboard, "clipboard-delete", "删除选中",
                        "删除选中的剪贴板历史内容", "clipboard delete"},
    SettingsSearchEntry{SettingsPageId::Clipboard, "clipboard-clear", "清空历史",
                        "清空全部本地剪贴板历史内容", "clipboard clear history"},
    SettingsSearchEntry{SettingsPageId::Clipboard, "clipboard-refresh", "刷新历史",
                        "重新读取磁盘上的剪贴板历史", "clipboard refresh"},
    SettingsSearchEntry{SettingsPageId::Learning, "learning-enabled", "记忆用户候选选择",
                        "记录主动选择的候选词", "learning remember candidate"},
    SettingsSearchEntry{SettingsPageId::Learning, "context-learning", "上下文学习",
                        "根据光标前后文调整候选排序", "context learning"},
    SettingsSearchEntry{SettingsPageId::Learning, "learning-clear", "清空学习记录",
                        "备份后清空本地学习排序", "learning clear reset"},
    SettingsSearchEntry{SettingsPageId::Diagnostics, "diagnostics-refresh", "刷新状态",
                        "重新查询 Fcitx5 和 ModernIME 状态", "fcitx fcitx5 diagnostics refresh"},
    SettingsSearchEntry{SettingsPageId::Diagnostics, "diagnostics-reload", "重新加载 ModernIME",
                        "保存配置后重新加载 ModernIME", "fcitx fcitx5 reload restart"},
    SettingsSearchEntry{SettingsPageId::Updates, "updates-check", "检查更新",
                        "在线查询官方最新版本，无需管理员授权", "update upgrade apt dnf 检查更新 升级 版本"},
    SettingsSearchEntry{SettingsPageId::Updates, "updates-refresh-cache", "重新检查版本",
                        "重新获取官方发布信息，无需刷新系统软件源", "refresh cache apt dnf makecache update 刷新缓存 刷新软件源缓存"},
    SettingsSearchEntry{SettingsPageId::Updates, "updates-download", "下载更新",
                        "自动下载、校验并安装更新，重启后生效", "download release 下载 更新 安装包"},
    SettingsSearchEntry{SettingsPageId::Updates, "updates-copy-command", "复制升级命令",
                        "复制安全升级命令至终端中执行", "copy command upgrade terminal 复制命令 升级"},
};

bool isAsciiWhitespace(char character) {
    return character == ' ' || character == '\t' || character == '\n' ||
           character == '\r' || character == '\f' || character == '\v';
}

std::string trimAndFoldAscii(std::string_view value) {
    while (!value.empty() && isAsciiWhitespace(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && isAsciiWhitespace(value.back())) {
        value.remove_suffix(1);
    }

    std::string result(value);
    for (auto &character : result) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character + ('a' - 'A'));
        }
    }
    return result;
}

bool contains(std::string_view value, const std::string &query) {
    return trimAndFoldAscii(value).find(query) != std::string::npos;
}

} // namespace

std::span<const SettingsPageDefinition> settingsPageDefinitions() {
    return kPages;
}

std::span<const SettingsSearchEntry> settingsSearchEntries() {
    return kSearchEntries;
}

std::vector<SettingsSearchEntry> searchSettings(std::string_view query) {
    const auto normalizedQuery = trimAndFoldAscii(query);
    if (normalizedQuery.empty()) {
        return {};
    }

    std::vector<SettingsSearchEntry> exactTitle;
    std::vector<SettingsSearchEntry> titleSubstring;
    std::vector<SettingsSearchEntry> descriptionOrKeywordSubstring;
    for (const auto &entry : kSearchEntries) {
        const auto normalizedTitle = trimAndFoldAscii(entry.title);
        if (normalizedTitle == normalizedQuery) {
            exactTitle.push_back(entry);
        } else if (normalizedTitle.find(normalizedQuery) != std::string::npos) {
            titleSubstring.push_back(entry);
        } else if (contains(entry.description, normalizedQuery) ||
                   contains(entry.keywords, normalizedQuery)) {
            descriptionOrKeywordSubstring.push_back(entry);
        }
    }

    exactTitle.insert(exactTitle.end(), titleSubstring.begin(),
                      titleSubstring.end());
    exactTitle.insert(exactTitle.end(), descriptionOrKeywordSubstring.begin(),
                      descriptionOrKeywordSubstring.end());
    return exactTitle;
}

std::optional<SettingsPageId> parseSettingsPage(std::string_view name) {
    const auto normalized = trimAndFoldAscii(name);
    if (normalized.empty()) {
        return std::nullopt;
    }

    for (const auto &page : kPages) {
        if (normalized == page.name || normalized == page.title) {
            return page.id;
        }
    }

    if (normalized == "home") {
        return SettingsPageId::Overview;
    }
    if (normalized == "basic") {
        return SettingsPageId::Input;
    }
    if (normalized == "candidate" || normalized == "appearance") {
        return SettingsPageId::Candidates;
    }
    if (normalized == "dict") {
        return SettingsPageId::Dictionary;
    }
    if (normalized == "clip") {
        return SettingsPageId::Clipboard;
    }
    if (normalized == "update" || normalized == "upgrade") {
        return SettingsPageId::Updates;
    }
    if (normalized == "diagnostic" || normalized == "status" ||
        normalized == "diag" || normalized == "system") {
        return SettingsPageId::Diagnostics;
    }

    return std::nullopt;
}

} // namespace modernime::settings
