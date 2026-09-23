#include "modernime/pinyin/pinyin_candidate_provider.h"
#include "modernime/core/pinyin_match.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "daily simulation test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::filesystem::path testPath(std::string_view name) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("modernime-sim-" + std::string(name));
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.string() + "-wal", error);
    std::filesystem::remove(path.string() + "-shm", error);
    return path;
}

std::size_t indexOf(const modernime::core::CandidatePage &page,
                    std::string_view text) {
    for (std::size_t index = 0; index < page.items.size(); ++index) {
        if (page.items[index].text == text) {
            return index;
        }
    }
    return page.items.size();
}

modernime::pinyin::PinyinCandidateProvider
makeSimulationProvider(const std::filesystem::path &learningPath) {
    modernime::pinyin::PinyinDataPaths paths;
    paths.extensionDictionary = MODERNIME_PINYIN_KNOWLEDGE_BUILD_BINARY;
    paths.hotwordDictionary = MODERNIME_PINYIN_HOTWORDS_BUILD_BINARY;
    paths.learningStore = learningPath.string();
    modernime::pinyin::PinyinProviderOptions options;
    options.learningEnabled = true;
    options.contextLearningEnabled = true;
    return modernime::pinyin::PinyinCandidateProvider(paths, options);
}

// 1. 核心高频单字基准测试（32组核心音节）
void testSinglePinyinPriorities(const std::filesystem::path &learningPath) {
    auto provider = makeSimulationProvider(learningPath);

    struct SingleTestCase {
        std::string_view pinyin;
        std::string_view expectedTop;
        std::string_view expectedTop3;
    };

    const std::vector<SingleTestCase> testCases = {
        {"ni", "你", "你"},
        {"wo", "我", "我"},
        {"ta", "他", "他"},
        {"de", "的", "的"},
        {"shi", "是", "是"},
        {"le", "了", "了"},
        {"zai", "在", "在"},
        {"you", "有", "有"},
        {"ren", "人", "人"},
        {"zhe", "这", "这"},
        {"zhong", "中", "中"},
        {"da", "大", "大"},
        {"guo", "过", "国"}, // 过为助词首位，国在前二
        {"shuo", "说", "说"},
        {"sheng", "生", "生"},
        {"nian", "年", "年"},
        {"dao", "到", "到"},
        {"yi", "以", "一"},  // 语法介词首位，一在前三
        {"zhi", "之", "之"},
        {"zi", "自", "字"},  // 自首位，字在前三
        {"hui", "会", "会"},
        {"ge", "各", "个"},  // 各首位，个在前三
        {"men", "们", "们"},
        {"lai", "来", "来"},
        {"yao", "要", "要"},
        {"shang", "上", "上"},
        {"xia", "下", "下"},
        {"dui", "对", "对"},
        {"zheng", "正", "正"},
        {"he", "和", "和"},
        {"qu", "去", "去"},
        {"neng", "能", "能"}
    };

    for (const auto &tc : testCases) {
        provider.reset();
        assertTrue(provider.append(tc.pinyin),
                   std::string("append input: ") + std::string(tc.pinyin));
        assertTrue(!provider.page().items.empty(),
                   std::string("candidates exist for: ") + std::string(tc.pinyin));
        assertTrue(provider.page().items.front().text == tc.expectedTop,
                   std::string("top candidate for ") + std::string(tc.pinyin) +
                   " should be " + std::string(tc.expectedTop) + ", but got " +
                   provider.page().items.front().text);
        if (tc.expectedTop != tc.expectedTop3) {
            const auto idx = indexOf(provider.page(), tc.expectedTop3);
            assertTrue(idx < 3,
                       std::string("expected top 3 candidate ") +
                       std::string(tc.expectedTop3) + " for " +
                       std::string(tc.pinyin) + " is within rank 3");
        }
    }
}

// 2. 多字词组与日常会话测试（40+组日常词汇、科技热词与长句）
void testMultiSyllablePhrasesAndHotwords(const std::filesystem::path &learningPath) {
    auto provider = makeSimulationProvider(learningPath);

    struct PhraseTestCase {
        std::string_view pinyin;
        std::string_view expected;
    };

    const std::vector<PhraseTestCase> testCases = {
        // 日常高频会话词
        {"nihao", "你好"},
        {"xiexie", "谢谢"},
        {"gongzuo", "工作"},
        {"xuexi", "学习"},
        {"mingtian", "明天"},
        {"jintian", "今天"},
        {"shijian", "时间"},
        {"shezhi", "设置"},
        {"diannao", "电脑"},
        {"shurufa", "输入法"},
        {"kaixin", "开心"},
        // 现代科技与AI词汇
        {"rengongzhineng", "人工智能"},
        {"ziranyuyanchuli", "自然语言处理"},
        {"liangzijisuan", "量子计算"},
        {"damoxing", "大模型"},
        {"tishici", "提示词"},
        {"zhinengti", "智能体"},
        {"yunyuansheng", "云原生"},
        {"rongqihua", "容器化"},
        {"gaokeyong", "高可用"},
        {"duxiefenli", "读写分离"},
        {"fenbushisuo", "分布式锁"},
        // 新增网络热词
        {"tangping", "躺平"},
        {"bailan", "摆烂"},
        {"saibopengke", "赛博朋克"},
        {"jiangweidaji", "降维打击"},
        {"dianzimuyu", "电子木鱼"},
        {"zundujiadu", "尊嘟假嘟"},
        {"qingxujiazhi", "情绪价值"},
        {"dianzan", "点赞"},
        {"haodeshoudao", "好的收到"},
        {"xinkuliao", "辛苦了"},
        {"meiwenti", "没问题"},
        {"mashangchuli", "马上处理"},
        {"chongdianbao", "充电宝"},
        {"saomazhifu", "扫码支付"},
        // 典型整句测试
        {"jintiantianqihenhao", "今天天气很好"},
        {"qingbangwodakaiwenjian", "请帮我打开文件"}
    };

    for (const auto &tc : testCases) {
        provider.reset();
        assertTrue(provider.append(tc.pinyin),
                   std::string("append input: ") + std::string(tc.pinyin));
        assertTrue(!provider.page().items.empty(),
                   std::string("candidates exist for: ") + std::string(tc.pinyin));
        // 对于词组，首选词必须完全匹配预期
        assertTrue(provider.page().items.front().text == tc.expected,
                   std::string("top candidate for ") + std::string(tc.pinyin) +
                   " should be " + std::string(tc.expected) + ", but got " +
                   provider.page().items.front().text);
    }
}

// 3. 高频缩写与简拼优先测试（20+组网络缩写、地名与常用简拼）
void testAbbreviationsAndInitialisms(const std::filesystem::path &learningPath) {
    auto provider = makeSimulationProvider(learningPath);

    struct AbbrevTestCase {
        std::string_view abbrev;
        std::string_view expected;
    };

    const std::vector<AbbrevTestCase> testCases = {
        // 网络流行缩写
        {"yyds", "永远的神"},
        {"dddd", "懂的都懂"},
        {"kswl", "磕死我了"},
        {"tql", "太强了"},
        {"bdjw", "不懂就问"},
        {"ssfd", "瑟瑟发抖"},
        {"dbq", "对不起"},
        {"gkd", "搞快点"},
        {"xswl", "笑死我了"},
        // 核心城市简拼
        {"bj", "北京"},
        {"gz", "广州"},
        {"hz", "杭州"},
        {"cd", "成都"},
        {"wh", "武汉"},
        {"cq", "重庆"},
        {"nj", "南京"},
        // 系统操作与高频词简拼
        {"dl", "登录"},
        {"wsm", "为什么"},
        {"cx", "查询"},
        {"qx", "取消"},
        // 知识库代表简拼
        {"aspl", "阿司匹林"},
        {"yxyy", "一心一意"},
        {"zryycl", "自然语言处理"}
    };

    for (const auto &tc : testCases) {
        provider.reset();
        assertTrue(provider.append(tc.abbrev),
                   std::string("append abbrev input: ") + std::string(tc.abbrev));
        assertTrue(!provider.page().items.empty(),
                   std::string("candidates exist for abbrev: ") + std::string(tc.abbrev));
        assertTrue(provider.page().items.front().text == tc.expected,
                   std::string("abbrev candidate for ") + std::string(tc.abbrev) +
                   " should rank " + std::string(tc.expected) + " at rank 0, but got " +
                   provider.page().items.front().text);
    }
}

// 4. 智能学习全周期动态演进测试
void testSmartLearningDynamics(const std::filesystem::path &learningPath) {
    auto provider = makeSimulationProvider(learningPath);

    // 场景 1：单次偶发选择单字（如泥），绝不能篡位核心首选单字（你）
    {
        provider.reset();
        assertTrue(provider.append("ni"), "append ni");
        assertTrue(provider.page().items.front().text == "你",
                   "ni starts with 你 at rank 0");
        const auto mudIndex = indexOf(provider.page(), "泥");
        assertTrue(mudIndex < provider.page().items.size() && mudIndex > 0,
                   "泥 is present but not rank 0 initially");

        // 偶发选择一次 泥
        assertTrue(provider.select(mudIndex), "select 泥 once");
        provider.reset();

        // 再次输入 ni，首选必须依然是 你！
        assertTrue(provider.append("ni"), "re-append ni after 1-shot select");
        assertTrue(!provider.page().items.empty(), "candidates exist");
        assertTrue(provider.page().items.front().text == "你",
                   "单次偶发选择 泥 后，ni 首选词必须保持‘你’不变（消除智障夺位）");
        
        // 但 泥 应该受到适度激励，向前移动且位于第一页前列（<= 3，且高于原位置 4）
        const auto newMudIndex = indexOf(provider.page(), "泥");
        assertTrue(newMudIndex < mudIndex && newMudIndex <= 3,
                   "单次偶发选择 泥 后，泥 应该前移至首页前列（<= 3 且优于原先位置）");
    }

    // 场景 2：语境记忆学习（上下文精准命中）
    {
        provider.reset();
        provider.setContext("水泥", "");
        assertTrue(provider.append("ni"), "append ni under context 水泥");
        const auto mudContextIdx = indexOf(provider.page(), "泥");
        assertTrue(mudContextIdx < provider.page().items.size(), "泥 exists");
        // 用户在“水泥”语境下选择“泥”
        assertTrue(provider.select(mudContextIdx), "select 泥 under context 水泥");
        provider.reset();

        // 再次处于“水泥”语境下，泥 立即因语境命中精准成为 Rank 0！
        provider.setContext("水泥", "");
        assertTrue(provider.append("ni"), "re-append ni under context 水泥");
        assertTrue(!provider.page().items.empty(), "candidates exist");
        assertTrue(provider.page().items.front().text == "泥",
                   "在‘水泥’语境下，语境加分使 泥 精准命中 Rank 0！");

        // 一旦离开该语境（无上下文），ni 的首选依然是通用核心字‘你’
        provider.reset();
        provider.setContext("", "");
        assertTrue(provider.append("ni"), "append ni without context");
        assertTrue(provider.page().items.front().text == "你",
                   "脱离上下文后，无语境的 ni 首选依然是‘你’");
    }

    // 场景 3：连续多次选择确立用户习惯，顺利晋升为 Rank 0
    {
        provider.reset();
        // 再连续选择 2 次 泥（累计达到 3 次）
        for (int i = 0; i < 2; ++i) {
            provider.reset();
            assertTrue(provider.append("ni"), "append ni for habit training");
            const auto mudIdx = indexOf(provider.page(), "泥");
            assertTrue(mudIdx < provider.page().items.size(), "泥 exists");
            assertTrue(provider.select(mudIdx), "select 泥");
        }

        // 累计达到 3 次后，确立为个人习惯偏好，泥 晋升为 Rank 0
        provider.reset();
        assertTrue(provider.append("ni"), "append ni after 3 selections");
        assertTrue(provider.page().items.front().text == "泥",
                   "累计选择 3 次后确立习惯，泥 晋升为 Rank 0");
    }

    // 场景 4：用户反悔纠正，重新连续选 你，你 重新夺回 Rank 0
    {
        for (int i = 0; i < 3; ++i) {
            provider.reset();
            assertTrue(provider.append("ni"), "append ni for correction");
            const auto niIdx = indexOf(provider.page(), "你");
            assertTrue(niIdx < provider.page().items.size(), "你 exists");
            assertTrue(provider.select(niIdx), "select 你");
        }

        provider.reset();
        assertTrue(provider.append("ni"), "append ni after correction");
        assertTrue(provider.page().items.front().text == "你",
                   "纠正选择后，你 成功恢复为 Rank 0");
    }

    // 场景 5：容错纠错候选单次选择保护
    {
        provider.reset();
        assertTrue(provider.append("zhegn"), "append typo zhegn");
        assertTrue(provider.page().items.front().text == "正",
                   "zhegn typo correction initial top is 正");
        const auto zhengIndex = indexOf(provider.page(), "整");
        assertTrue(zhengIndex < provider.page().items.size() && zhengIndex > 0,
                   "整 is present as secondary candidate");

        // 偶然选一次 整
        assertTrue(provider.select(zhengIndex), "select 整 once");
        provider.reset();

        // 再次输入 zhegn，正 依然必须稳居 Rank 0
        assertTrue(provider.append("zhegn"), "re-append zhegn after selecting 整 once");
        assertTrue(provider.page().items.front().text == "正",
                   "zhegn 偶发选一次‘整’后，首选词依然必须保持‘正’！");
    }
}

// 5. 智能拼音容错引擎日常测试（漏击补全、抖动去重、邻键置换、倒序修复）
void testSmartTypoCorrectionEngine(const std::filesystem::path &learningPath) {
    auto provider = makeSimulationProvider(learningPath);

    struct TypoCase {
        std::string_view input;
        std::string_view expected;
        std::string_view desc;
        std::size_t maxRank = 0;
    };

    const std::vector<TypoCase> testCases = {
        // 漏打韵尾/三元音
        {"zhogguo", "中国", "zhogguo 漏打n自动纠错补全为中国", 0},
        {"xuesheg", "学生", "xuesheg 漏打n自动纠错补全为学生", 0},
        {"beijig", "北京", "beijig 漏打n自动纠错补全为北京", 0},
        {"shaghai", "上海", "shaghai 漏打n自动纠错补全为上海", 0},
        {"pengyo", "朋友", "pengyo 漏打u自动纠错呈现朋友于前二", 1},
        {"guag", "光", "guag 漏打n自动纠错补全为光", 0},
        {"fagzi", "房子", "fagzi 漏打n自动纠错补全为房子", 0},
        // 连续双元音抖动
        {"zhoongguo", "中国", "zhoongguo 双元音抖动自动去重为中国", 0},
        {"sheeng", "生", "sheeng 双元音抖动自动去重为生", 0},
        // 邻键置换与字母颠倒
        {"yop", "有", "yop 邻键误触纠错为有", 0},
        {"xiab", "先", "xiab 邻键误触纠错为先", 0},
        {"yuedign", "约定", "yuedign 颠倒纠错为约定", 0},
        {"zhegn", "正", "zhegn 颠倒纠错为正", 0},
        {"shuei", "水", "shuei 三元音纠错为水", 0},
        {"luen", "论", "luen 纠错为论", 0},
        {"tina", "天", "tina 倒序纠错呈现天于前三", 2},
        // 长句复合容错
        {"jintiantianqibucuo", "今天天气不错", "正确长句准确识别", 0},
        {"jintiantianqibucup", "今天天气不错", "jintiantianqibucup 邻键误触长句纠错为今天天气不错", 0},
    };

    for (const auto &tc : testCases) {
        provider.reset();
        assertTrue(provider.append(tc.input),
                   std::string("append typo input: ") + std::string(tc.input));
        assertTrue(!provider.page().items.empty(),
                   std::string("candidates exist for: ") + std::string(tc.input));
        const auto idx = indexOf(provider.page(), tc.expected);
        assertTrue(idx <= tc.maxRank,
                   std::string("candidate for ") + std::string(tc.input) +
                   " should be rank <= " + std::to_string(tc.maxRank) +
                   ", but got rank " + std::to_string(idx) + " (first is " +
                   provider.page().items.front().text + ") (" + std::string(tc.desc) + ")");
    }
}

// 6. 微信输入法深度对标：6大典型行业角色与全功能宏引擎基准
void testMultiPersonaWeChatParityBenchmarks(const std::filesystem::path &learningPath) {
    auto provider = makeSimulationProvider(learningPath);

    struct PersonaBenchmark {
        std::string_view persona;
        std::string_view input;
        std::string_view expected;
        std::size_t maxRank;
        std::string_view desc;
    };

    const std::vector<PersonaBenchmark> benchmarks = {
        // 角色一：软件开发 / 架构师 (Developer)
        {"Developer", "weifuwu", "微服务", 0, "微服务 首选置顶"},
        {"Developer", "sisuo", "死锁", 1, "死锁 前二呈现"},
        {"Developer", "duanyan", "断言", 1, "断言 前二候选"},
        {"Developer", "fanxiangdaili", "反向代理", 0, "反向代理 首选置顶"},
        {"Developer", "xiaoxiduilie", "消息队列", 0, "消息队列 首选置顶"},
        {"Developer", "fuzaijunheng", "负载均衡", 0, "负载均衡 首选置顶"},
        {"Developer", "chixujicheng", "持续集成", 0, "持续集成 首选置顶"},
        {"Developer", "lajihuishou", "垃圾回收", 0, "垃圾回收 首选置顶"},
        {"Developer", "xianchenganquan", "线程安全", 0, "线程安全 首选置顶"},
        {"Developer", "yilaizhuru", "依赖注入", 0, "依赖注入 首选置顶"},
        {"Developer", "shejimoshi", "设计模式", 0, "设计模式 首选置顶"},

        // 角色二：财务会计 / 投行分析师 (Finance)
        {"Finance", "zichanfuzhaibiao", "资产负债表", 0, "资产负债表 首选置顶"},
        {"Finance", "xianjinliuliangbiao", "现金流量表", 0, "现金流量表 首选置顶"},
        {"Finance", "lirunbiao", "利润表", 0, "利润表 首选置顶"},
        {"Finance", "zengzhishuifapiao", "增值税发票", 0, "增值税发票 首选置顶"},
        {"Finance", "v12345", "壹万贰仟叁佰肆拾伍元整", 0, "v12345 大写金额宏首选置顶"},
        {"Finance", "v1000", "壹仟元整", 0, "v1000 整数金额宏首选置顶"},
        {"Finance", "v88.5", "捌拾捌元伍角整", 0, "v88.5 角分金额宏首选置顶"},
        {"Finance", "rmb", "¥", 1, "rmb 货币符号宏前二呈现"},
        {"Finance", "dollar", "$", 2, "dollar 美元符号宏前三呈现"},

        // 角色三：临床医学 / 药剂师 (Medical)
        {"Medical", "xuechanggui", "血常规", 0, "血常规 首选置顶"},
        {"Medical", "xindiantu", "心电图", 0, "心电图 首选置顶"},
        {"Medical", "hecigongzhen", "核磁共振", 0, "核磁共振 首选置顶"},
        {"Medical", "guanxinbing", "冠心病", 0, "冠心病 首选置顶"},
        {"Medical", "buluofen", "布洛芬", 0, "布洛芬 首选置顶"},
        {"Medical", "amoxilin", "阿莫西林", 0, "阿莫西林 首选置顶"},
        {"Medical", "sheshidu", "℃", 1, "sheshidu 医学温度单位宏前二呈现"},

        // 角色四：法律法务 / 律师 (Legal)
        {"Legal", "minfadian", "民法典", 0, "民法典 首选置顶"},
        {"Legal", "bukekangli", "不可抗力", 0, "不可抗力 首选置顶"},
        {"Legal", "weiyuezeren", "违约责任", 0, "违约责任 首选置顶"},
        {"Legal", "liandaizeren", "连带责任", 0, "连带责任 首选置顶"},
        {"Legal", "budangdeli", "不当得利", 0, "不当得利 首选置顶"},
        {"Legal", "wuyinguanli", "无因管理", 0, "无因管理 首选置顶"},
        {"Legal", "jiefu", "§", 1, "jiefu 法条节符宏前二呈现"},
        {"Legal", "xuhao", "①", 1, "xuhao 序号符号宏前二呈现"},

        // 角色五：社交网民 / 聊天达人 (Social)
        {"Social", "haha", "😄", 2, "haha 常用笑脸Emoji前三呈现"},
        {"Social", "zan", "👍", 2, "zan 点赞Emoji前三呈现"},
        {"Social", "dui", "√", 2, "dui 对号符号宏前三呈现"},
        {"Social", "cuo", "×", 2, "cuo 叉号符号宏前三呈现"},
        {"Social", "jiantou", "→", 2, "jiantou 箭头符号宏前三呈现"},
        {"Social", "daniu", "🐂", 2, "daniu 🐂Emoji前三呈现"},

        // 角色六：学术科研 / 数据科学家 (Academic)
        {"Academic", "xianzhuxingchayi", "显著性差异", 0, "显著性差异 首选置顶"},
        {"Academic", "zhengtaifenbu", "正态分布", 0, "正态分布 首选置顶"},
        {"Academic", "huiguifenxi", "回归分析", 0, "回归分析 首选置顶"},
        {"Academic", "zhixinqujian", "置信区间", 0, "置信区间 首选置顶"},
        {"Academic", "maerkefulian", "马尔可夫链", 0, "马尔可夫链 首选置顶"},
        {"Academic", "pi", "π", 2, "pi 希腊字母宏前三呈现"},
        {"Academic", "alpha", "α", 2, "alpha 希腊字母宏前三呈现"},
        {"Academic", "pingfang", "²", 2, "pingfang 上标平方宏前三呈现"},
        {"Academic", "wuxian", "∞", 2, "wuxian 无穷大符号宏前三呈现"},
    };

    for (const auto &bm : benchmarks) {
        provider.reset();
        assertTrue(provider.append(bm.input),
                   std::string("[") + std::string(bm.persona) + "] append input: " + std::string(bm.input));
        assertTrue(!provider.page().items.empty(),
                   std::string("[") + std::string(bm.persona) + "] candidates exist for: " + std::string(bm.input));
        const auto idx = indexOf(provider.page(), bm.expected);
        assertTrue(idx <= bm.maxRank,
                   std::string("[") + std::string(bm.persona) + "] " + std::string(bm.input) +
                   " expected '" + std::string(bm.expected) + "' at rank <= " + std::to_string(bm.maxRank) +
                   ", but got rank " + std::to_string(idx) +
                   " (top 3: " +
                   (provider.page().items.size() > 0 ? provider.page().items[0].text : "") + ", " +
                   (provider.page().items.size() > 1 ? provider.page().items[1].text : "") + ", " +
                   (provider.page().items.size() > 2 ? provider.page().items[2].text : "") + ") (" +
                   std::string(bm.desc) + ")");
    }
}

} // namespace

int main() {
    const auto learningPath = testPath("daily-simulation-learning.sqlite3");

    std::cout << "[1/6] Running Single Pinyin Priority Benchmark (32 syllables)...\n";
    testSinglePinyinPriorities(learningPath);

    std::cout << "[2/6] Running Multi-Syllable Phrase & Sentence Benchmark (40+ items)...\n";
    testMultiSyllablePhrasesAndHotwords(learningPath);

    std::cout << "[3/6] Running Abbreviations & Initialisms Benchmark (24 items)...\n";
    testAbbreviationsAndInitialisms(learningPath);

    std::cout << "[4/6] Running Smart Learning Dynamic Evolution Tests...\n";
    testSmartLearningDynamics(learningPath);

    std::cout << "[5/6] Running Smart Typo Correction Engine Tests...\n";
    testSmartTypoCorrectionEngine(learningPath);

    std::cout << "[6/6] Running Multi-Persona WeChat Parity Benchmark (6 Professions & Macro Engine)...\n";
    testMultiPersonaWeChatParityBenchmarks(learningPath);

    std::error_code error;
    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);

    std::cout << "All daily simulation tests passed successfully!\n";
    return EXIT_SUCCESS;
}
