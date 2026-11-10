#include "modernime/core/symbol_catalog.h"

#include <algorithm>
#include <initializer_list>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace modernime::core {
namespace {
struct MacroGroup {
    std::string_view keys;
    std::string_view name;
    std::string_view category;
    std::string_view keywords;
    std::initializer_list<std::string_view> values;
};

// The existing provider's 160 exact macro keys and ordered value lists live
// here once. Identical lists share a group; aliases also feed catalog search.
constexpr MacroGroup kGroups[] = {
    {"gantanhao tanhao jingtanhao", "感叹号", "punctuation", "exclamation mark;bang", {"！", "!", "❗️"}},
    {"wenhao", "问号", "punctuation", "question mark", {"？", "?", "❓"}},
    {"douhao", "逗号", "punctuation", "comma", {"，", ","}},
    {"juhao", "句号", "punctuation", "period;full stop", {"。", "."}},
    {"dunhao", "顿号", "punctuation", "enumeration comma", {"、"}},
    {"fenhao", "分号", "punctuation", "semicolon", {"；", ";"}},
    {"maohao", "冒号", "punctuation", "colon", {"：", ":"}},
    {"shenglvehao shengvehao", "省略号", "punctuation", "ellipsis", {"……", "…"}},
    {"pozhehao pozehao", "破折号", "punctuation", "em dash;dash", {"——", "—"}},
    {"bolangxian bolanghao bozhehao boxian", "波浪线", "punctuation", "tilde;wave", {"～", "~"}},
    {"shuminghao", "书名号", "punctuation", "book title brackets", {"《》", "《", "》"}},
    {"zuoshuminghao", "左书名号", "punctuation", "left book title bracket", {"《", "〈"}},
    {"youshuminghao", "右书名号", "punctuation", "right book title bracket", {"》", "〉"}},
    {"yinhao", "引号", "punctuation", "quotation mark;quote", {"“”", "‘’", "“", "”"}},
    {"shuangyinhao", "双引号", "punctuation", "double quotation mark;double quote", {"“”", "“", "”"}},
    {"danyinhao", "单引号", "punctuation", "single quotation mark;single quote", {"‘’", "‘", "’"}},
    {"zuoyinhao", "左引号", "punctuation", "left quotation mark;left quote", {"“", "‘"}},
    {"youyinhao", "右引号", "punctuation", "right quotation mark;right quote", {"”", "’"}},
    {"kuohao", "圆括号", "punctuation", "parentheses;round brackets", {"（）", "()", "（", "）"}},
    {"yuankuohao xiaokuohao", "圆括号", "punctuation", "parentheses;round brackets", {"（）", "()"}},
    {"fangkuohao zhongkuohao", "方括号", "punctuation", "square brackets", {"【】", "[]"}},
    {"dakuohao huakuohao", "大括号", "punctuation", "curly braces;curly brackets", {"｛｝", "{}"}},
    {"zuokuohao", "左括号", "punctuation", "left bracket;left parenthesis", {"（", "(", "【", "["}},
    {"youkuohao", "右括号", "punctuation", "right bracket;right parenthesis", {"）", ")", "】", "]"}},
    {"jiangehao", "间隔号", "punctuation", "middle dot;bullet", {"·", "•"}},
    {"xiahuaxian", "下划线", "punctuation", "underscore", {"_"}},
    {"lianjiehao jianhao", "连字符与减号", "punctuation", "hyphen;minus;minus sign", {"-", "－"}},
    {"xiegang zhengxiegang", "正斜杠", "punctuation", "slash;forward slash", {"/"}},
    {"fanxiegang", "反斜杠", "punctuation", "backslash", {"\\"}},
    {"pi", "圆周率派", "math", "pi;greek letter", {"π", "Π"}},
    {"alpha", "希腊字母阿尔法", "math", "alpha;greek letter", {"α", "Α"}},
    {"beta", "希腊字母贝塔", "math", "beta;greek letter", {"β", "Β"}},
    {"gamma", "希腊字母伽马", "math", "gamma;greek letter", {"γ", "Γ"}},
    {"delta", "希腊字母德尔塔", "math", "delta;greek letter", {"δ", "Δ"}},
    {"theta", "希腊字母西塔", "math", "theta;greek letter", {"θ", "Θ"}},
    {"lambda", "希腊字母兰姆达", "math", "lambda;greek letter", {"λ", "Λ"}},
    {"mu", "希腊字母缪", "math", "mu;micro;greek letter", {"μ"}},
    {"sigma", "希腊字母西格玛", "math", "sigma;greek letter", {"σ", "Σ"}},
    {"omega", "希腊字母欧米伽", "math", "omega;greek letter", {"ω", "Ω"}},
    {"epsilon", "希腊字母艾普西龙", "math", "epsilon;greek letter", {"ε"}},
    {"phi", "希腊字母斐", "math", "phi;greek letter", {"φ", "Φ"}},
    {"tau", "希腊字母陶", "math", "tau;greek letter", {"τ"}},
    {"rho", "希腊字母柔", "math", "rho;greek letter", {"ρ"}},
    {"jiahao", "加号", "math", "plus;addition", {"+", "＋"}},
    {"chenghao", "乘号", "math", "multiplication;multiply;times;asterisk", {"×", "*"}},
    {"chuhao", "除号", "math", "division;divide", {"÷", "/"}},
    {"denghao dengyu", "等号", "math", "equal;equals", {"=", "＝"}},
    {"budeng budenghao", "不等号", "math", "not equal;inequality", {"≠"}},
    {"yuedeng yuedenghao", "约等号", "math", "approximately equal;approximate", {"≈"}},
    {"dayu dayuhao", "大于号", "math", "greater than", {">", "＞"}},
    {"xiaoyu xiaoyuhao", "小于号", "math", "less than", {"<", "＜"}},
    {"dayudeng", "大于等于号", "math", "greater than or equal", {"≥"}},
    {"dayudengyu dayudengyuhao", "大于等于号", "math", "greater than or equal", {"≥", ">="}},
    {"xiaoyudeng", "小于等于号", "math", "less than or equal", {"≤"}},
    {"xiaoyudengyu xiaoyudengyuhao", "小于等于号", "math", "less than or equal", {"≤", "<="}},
    {"zhengfu zhengfuhao", "正负号", "math", "plus minus", {"±"}},
    {"kaifang genghao", "根号", "math", "square root;radical", {"√"}},
    {"baifenhao baifenbi", "百分号", "math", "percent;percentage", {"%", "％"}},
    {"qianfenhao qianfenbi", "千分号", "math", "per mille;permille", {"‰"}},
    {"wuxian wuxianhao wuqiong wuqionghao", "无穷大", "math", "infinity;infinite", {"∞"}},
    {"sheshidu", "摄氏度", "units", "celsius;centigrade;temperature", {"℃"}},
    {"huashidu", "华氏度", "units", "fahrenheit;temperature", {"℉"}},
    {"du jiaodu", "角度", "units", "degree;angle", {"°"}},
    {"pingfang", "平方", "units", "squared;square;superscript two", {"²"}},
    {"lifang", "立方", "units", "cubed;cube;superscript three", {"³"}},
    {"suoyi", "所以", "math", "therefore", {"∴"}},
    {"yuanyin yinwei", "因为", "math", "because;since", {"∵"}},
    {"dui duihao gou", "对号", "other", "check mark;tick;correct", {"√", "✔"}},
    {"cuo cuohao cha", "错号", "other", "cross mark;incorrect;wrong", {"×", "✖"}},
    {"jiantou", "箭头", "arrows", "arrow", {"→", "←", "↑", "↓"}},
    {"xiangyou youjiantou you", "向右箭头", "arrows", "right arrow;arrow right", {"→"}},
    {"xiangzuo zuojiantou zuo", "向左箭头", "arrows", "left arrow;arrow left", {"←"}},
    {"xiangshang shangjiantou shang", "向上箭头", "arrows", "up arrow;arrow up", {"↑"}},
    {"xiangxia xiajiantou xia", "向下箭头", "arrows", "down arrow;arrow down", {"↓"}},
    {"sanjiao sanjiaoxing", "三角形", "other", "triangle", {"▲", "△"}},
    {"wujiaoxing xingxing", "五角星", "other", "star", {"★", "☆"}},
    {"lingxing", "菱形", "other", "diamond", {"◆", "◇"}},
    {"yuan", "圆形", "other", "circle", {"●", "○"}},
    {"rmb renminbi", "人民币符号", "units", "renminbi;yuan;currency", {"¥", "￥"}},
    {"dollar meiyuan", "美元符号", "units", "dollar;currency", {"$", "＄"}},
    {"ouyuan", "欧元符号", "units", "euro;currency", {"€"}},
    {"bang yingbang", "英镑符号", "units", "pound sterling;pound;currency", {"£"}},
    {"riyuan", "日元符号", "units", "yen;currency", {"¥", "円"}},
    {"jinghao", "井号", "punctuation", "hash;number sign;hashtag", {"#"}},
    {"aite at", "艾特符号", "punctuation", "at sign;at", {"@"}},
    {"yuhao and", "与符号", "punctuation", "ampersand;and", {"&"}},
    {"jiefu", "章节号", "other", "section sign;section", {"§"}},
    {"duanluo", "段落符号", "other", "pilcrow;paragraph", {"¶"}},
    {"banquan", "版权符号", "other", "copyright", {"©"}},
    {"shangbiao", "商标符号", "other", "trademark;registered", {"®", "™"}},
    {"xuhao", "带圈序号", "other", "circled number;ordinal", {"①", "②", "③", "④", "⑤", "⑥", "⑦", "⑧", "⑨", "⑩"}},
    {"yuanquan", "圆圈", "other", "circle;circled number", {"①", "②", "③", "④", "⑤", "○", "●"}},
    {"haha", "开心微笑", "emoji", "smile;happy;laugh", {"😄", "😂"}},
    {"xixi", "眯眼笑", "emoji", "laugh;squint", {"😆"}},
    {"heihei", "露齿笑", "emoji", "grin;smile", {"😁"}},
    {"zan dianzan", "点赞", "emoji", "thumbs up;like", {"👍"}},
    {"xin aixin", "爱心", "emoji", "heart;love", {"❤️"}},
    {"ku", "哭泣", "emoji", "cry;sob", {"😭"}},
    {"ok", "好的手势", "emoji", "ok hand;okay", {"👌"}},
    {"bixin", "比心", "emoji", "finger heart", {"🫰"}},
    {"bai zaijian", "挥手再见", "emoji", "wave;goodbye", {"👋"}},
    {"baoquan", "抱拳合十", "emoji", "folded hands;pray;thanks", {"🙏"}},
    {"fennu", "愤怒", "emoji", "angry;anger", {"😡"}},
    {"niu daniu niubi liuliuliu", "牛", "emoji", "ox;bull;awesome", {"🐂"}},
    {"doge", "小狗", "emoji", "dog;doge", {"🐶"}},
    {"tangping", "躺平睡觉", "emoji", "bed;sleep;rest", {"🛌"}},
    {"jiayou", "加油力量", "emoji", "flexed biceps;muscle;strong", {"💪"}},
};

std::string normalize(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char ch : value) {
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            continue;
        }
        result.push_back(ch >= 'A' && ch <= 'Z' ?
                             static_cast<char>(ch - 'A' + 'a') : ch);
    }
    return result;
}

std::vector<std::string> split(std::string_view value, char separator) {
    std::vector<std::string> result;
    while (!value.empty()) {
        const auto end = value.find(separator);
        const auto item = value.substr(0, end);
        if (!item.empty()) {
            result.emplace_back(item);
        }
        if (end == std::string_view::npos) {
            break;
        }
        value.remove_prefix(end + 1);
    }
    return result;
}

struct CatalogData {
    std::vector<SymbolEntry> entries;
    std::vector<std::vector<std::string>> searchKeys;
    std::unordered_map<std::string, std::size_t> byId;
    std::unordered_map<std::string, std::vector<std::string>> macros;
};

const CatalogData &data() {
    static const CatalogData catalog = [] {
        CatalogData result;
        for (const auto &group : kGroups) {
            auto aliases = split(group.keys, ' ');
            const auto macroKeys = aliases;
            auto keywords = split(group.keywords, ';');
            aliases.insert(aliases.end(), keywords.begin(), keywords.end());
            aliases.emplace_back(group.name);
            for (const auto text : group.values) {
                const auto inserted = result.byId.emplace(std::string(text), result.entries.size());
                if (inserted.second) {
                    // These two emoji intentionally retain their exact legacy
                    // UTF-8 spelling, including the exclamation variation selector.
                    const auto category = text == "❗️" || text == "❓" ?
                                              "emoji" : group.category;
                    result.entries.push_back({std::string(text), std::string(text),
                                              std::string(group.name), std::string(category), {}});
                }
                auto &entry = result.entries[inserted.first->second];
                for (const auto &alias : aliases) {
                    if (std::find(entry.aliases.begin(), entry.aliases.end(), alias) ==
                        entry.aliases.end()) {
                        entry.aliases.push_back(alias);
                    }
                }
                // More specific one-symbol groups name individual arrows.
                if (group.values.size() == 1 && group.category == "arrows") {
                    entry.name = group.name;
                }
            }
            for (const auto &key : macroKeys) {
                auto &values = result.macros[key];
                for (const auto text : group.values) {
                    values.emplace_back(text);
                }
            }
        }
        // A few common symbols have no legacy macro. They only extend the
        // searchable catalog and do not introduce new input-method macros.
        const SymbolEntry extra[] = {
            {"↔", "↔", "左右双向箭头", "arrows", {"shuangxiangjiantou", "left right arrow", "bidirectional arrow"}},
            {"⇌", "⇌", "可逆反应箭头", "arrows", {"kenifanying", "equilibrium", "reversible reaction"}},
            {"∑", "∑", "求和符号", "math", {"qiuhe", "summation", "sum"}},
            {"∫", "∫", "积分符号", "math", {"jifen", "integral"}},
            {"∈", "∈", "属于符号", "math", {"shuyu", "element of", "membership"}},
            {"∅", "∅", "空集", "math", {"kongji", "empty set"}},
            {"㎏", "㎏", "千克", "units", {"qianke", "kilogram", "kg"}},
            {"㎡", "㎡", "平方米", "units", {"pingfangmi", "square metre", "square meter"}},
        };
        for (const auto &entry : extra) {
            if (result.byId.emplace(entry.id, result.entries.size()).second) {
                result.entries.push_back(entry);
            }
        }
        result.searchKeys.reserve(result.entries.size());
        for (const auto &entry : result.entries) {
            std::vector<std::string> keys = {normalize(entry.text), normalize(entry.name)};
            for (const auto &alias : entry.aliases) {
                keys.push_back(normalize(alias));
            }
            result.searchKeys.push_back(std::move(keys));
        }
        return result;
    }();
    return catalog;
}
} // namespace

const std::vector<SymbolEntry> &SymbolCatalog::entries() {
    return data().entries;
}

const SymbolEntry *SymbolCatalog::find(std::string_view id) {
    const auto &catalog = data();
    const auto found = catalog.byId.find(std::string(id));
    return found == catalog.byId.end() ? nullptr : &catalog.entries[found->second];
}

std::vector<const SymbolEntry *> SymbolCatalog::search(
    std::string_view query, std::string_view category,
    const std::vector<std::string> &favoriteIds, bool favoritesOnly,
    std::size_t limit) {
    std::vector<const SymbolEntry *> results;
    if (limit == 0) {
        return results;
    }
    const auto &catalog = data();
    const auto key = normalize(query);
    const std::unordered_set<std::string_view> favorites(favoriteIds.begin(), favoriteIds.end());
    results.reserve(std::min(limit, catalog.entries.size()));
    for (int favoritePass = 1; favoritePass >= (favoritesOnly ? 1 : 0); --favoritePass) {
        for (std::size_t i = 0; i < catalog.entries.size(); ++i) {
            const auto &entry = catalog.entries[i];
            const bool favorite = favorites.find(entry.id) != favorites.end();
            if (favorite != (favoritePass != 0) ||
                (!category.empty() && entry.category != category)) {
                continue;
            }
            const auto &keys = catalog.searchKeys[i];
            if (!key.empty() && std::none_of(keys.begin(), keys.end(), [&key](const auto &value) {
                    return value.find(key) != std::string::npos;
                })) {
                continue;
            }
            results.push_back(&entry);
            if (results.size() == limit) {
                return results;
            }
        }
    }
    return results;
}

std::vector<std::string> symbolMacroValues(std::string_view key) {
    const auto &macros = data().macros;
    const auto found = macros.find(std::string(key));
    return found == macros.end() ? std::vector<std::string>{} : found->second;
}
} // namespace modernime::core
