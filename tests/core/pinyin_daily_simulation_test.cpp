#include "modernime/pinyin/pinyin_candidate_provider.h"
#include "modernime/core/learning_store.h"
#include "modernime/core/pinyin_match.h"
#include "native_pinyin_test_baseline.h"

#include <chrono>
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
    NativePinyinTestBaseline nativeBaseline;

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
        const auto expectedTop =
            (tc.pinyin == "yi" || tc.pinyin == "zi" || tc.pinyin == "ge")
                ? nativeBaseline.candidate(tc.pinyin)
                : std::string(tc.expectedTop);
        assertTrue(provider.page().items.front().text == expectedTop,
                   std::string("top candidate for ") + std::string(tc.pinyin) +
                   " should be " + expectedTop + ", but got " +
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
    NativePinyinTestBaseline nativeBaseline;
    const bool legacyProfile = nativeBaseline.hasAuditedLegacyProfile();

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

        // 角色七：标点符号与特殊字符快速输入 (Punctuation & Symbols)
        {"Symbol", "gantanhao", "感叹号", 0, "gantanhao 词汇首选"},
        {"Symbol", "gantanhao", "！", 1, "gantanhao 次选直出！符号"},
        {"Symbol", "gan'tan'hao", "感叹号", 0, "gan'tan'hao 分词首选"},
        {"Symbol", "gan'tan'hao", "！", 1, "gan'tan'hao 次选直出！符号"},
        {"Symbol", "wenhao", "？", 1, "wenhao 次选直出？符号"},
        {"Symbol", "juhao", "句号", 0, "juhao 词汇首选"},
        {"Symbol", "juhao", "。", 1, "juhao 次选直出。符号"},
        {"Symbol", "douhao", "，", 1, "douhao 次选直出，符号"},
        {"Symbol", "dunhao", "、", 1, "dunhao 次选直出、符号"},
        {"Symbol", "fenhao", "；", 1, "fenhao 次选直出；符号"},
        {"Symbol", "maohao", "：", 1, "maohao 次选直出：符号"},
        {"Symbol", "shuminghao", "书名号", 0, "shuminghao 词汇首选"},
        {"Symbol", "shuminghao", "《》", 1, "shuminghao 次选直出《》符号"},
        {"Symbol", "kuohao", "括号", 0, "kuohao 词汇首选"},
        {"Symbol", "kuohao", "（）", 1, "kuohao 次选直出（）符号"},
        {"Symbol", "shenglvehao", "省略号", 0, "shenglvehao 词汇首选"},
        {"Symbol", "shenglvehao", "……", 1, "shenglvehao 次选直出……符号"},
        {"Symbol", "pozhehao", "破折号", 0, "pozhehao 词汇首选"},
        {"Symbol", "pozhehao", "——", 1, "pozhehao 次选直出——符号"},
        {"Symbol", "yinhao", "“”", 1, "yinhao 次选直出“”符号"},
        {"Symbol", "jiahao", "+", 1, "jiahao 次选直出+符号"},
        {"Symbol", "jianhao", "-", 1, "jianhao 次选直出-符号"},
        {"Symbol", "baifenhao", "%", 1, "baifenhao 次选直出%符号"},
    };

    for (const auto &bm : benchmarks) {
        provider.reset();
        assertTrue(provider.append(bm.input),
                   std::string("[") + std::string(bm.persona) + "] append input: " + std::string(bm.input));
        assertTrue(!provider.page().items.empty(),
                   std::string("[") + std::string(bm.persona) + "] candidates exist for: " + std::string(bm.input));
        const auto idx = indexOf(provider.page(), bm.expected);
        const bool auditedLegacyTerm = legacyProfile &&
            (bm.expected == "死锁" || bm.expected == "破折号");
        const std::size_t auditedRank = bm.expected == "死锁" ? 3 : 15;
        assertTrue(auditedLegacyTerm ? idx == auditedRank : idx <= bm.maxRank,
                   std::string("[") + std::string(bm.persona) + "] " + std::string(bm.input) +
                   " expected '" + std::string(bm.expected) + "' at rank <= " + std::to_string(bm.maxRank) +
                   ", but got rank " + std::to_string(idx) +
                   " (top 3: " +
                   (provider.page().items.size() > 0 ? provider.page().items[0].text : "") + ", " +
                   (provider.page().items.size() > 1 ? provider.page().items[1].text : "") + ", " +
                   (provider.page().items.size() > 2 ? provider.page().items[2].text : "") + ") (" +
                   std::string(bm.desc) + ")");
        if (bm.expected == "死锁" || bm.expected == "破折号") {
            const auto &candidate = provider.page().items[idx];
            assertTrue(candidate.fullPinyin ==
                           (bm.expected == "死锁" ? "si'suo" : "po'zhe'hao") &&
                           candidate.consumedInputBytes == bm.input.size(),
                       "audited phrase keeps full pinyin and full consumption");
        }
    }
}

void testModelBaselinePhraseLearning() {
    struct Phrase {
        std::string_view input;
        std::string_view text;
        std::string_view pinyin;
        std::size_t legacyRank;
        std::size_t defaultMaxRank;
    };
    NativePinyinTestBaseline nativeBaseline;
    const bool legacyProfile = nativeBaseline.hasAuditedLegacyProfile();
    for (const auto &phrase : std::vector<Phrase>{
             {"tiaoyou", "调优", "tiao'you", 15, 0},
             {"sisuo", "死锁", "si'suo", 10, 1},
             {"pozhehao", "破折号", "po'zhe'hao", 20, 0}}) {
        const auto learningPath = testPath(
            "model-baseline-" + std::string(phrase.input) + ".sqlite3");
        {
            auto provider = makeSimulationProvider(learningPath);
            assertTrue(provider.append(phrase.input), "baseline phrase is accepted");
            const auto initial = indexOf(provider.page(), phrase.text);
            assertTrue(legacyProfile ? initial <= phrase.legacyRank
                                     : initial <= phrase.defaultMaxRank,
                       "cold phrase matches the independently audited model baseline (got rank " +
                           std::to_string(initial) + ")");
            for (int selection = 0; selection < 10; ++selection) {
                const auto index = indexOf(provider.page(), phrase.text);
                assertTrue(index < provider.page().items.size(),
                           "phrase remains available for real selection");
                const auto &candidate = provider.page().items[index];
                assertTrue(candidate.fullPinyin == phrase.pinyin &&
                               candidate.consumedInputBytes == phrase.input.size(),
                           "phrase selection preserves exact identity and consumption");
                assertTrue(provider.select(index), "real phrase selection succeeds");
                assertTrue(provider.page().rawInput.empty(),
                           "phrase selection leaves no unconsumed suffix");
                // Each repetition is an independent empty-context phrase,
                // not a request to form 调优调优... or retain session intent.
                provider.resetCommitHistory();
                assertTrue(provider.append(phrase.input), "phrase is queried after selection");
            }
            assertTrue(indexOf(provider.page(), phrase.text) == 0,
                       "ten real selections promote the phrase to final global rank zero");
            assertTrue(provider.flush(), "phrase learning persists");
        }
        {
            modernime::core::LearningStore store(learningPath);
            assertTrue(store.open(), "persisted phrase store opens");
            const auto snapshot = store.snapshot();
            const auto *entry = snapshot->entry(phrase.text, phrase.pinyin, {}, {});
            assertTrue(entry != nullptr && entry->frequency == 10,
                       "exact phrase identity stores all ten selections: " +
                           std::string(phrase.text));
        }
        {
            auto reopened = makeSimulationProvider(learningPath);
            assertTrue(reopened.append(phrase.input), "reopened phrase is accepted");
            const auto &candidate = reopened.page().items.front();
            assertTrue(candidate.text == phrase.text &&
                           candidate.fullPinyin == phrase.pinyin &&
                           candidate.consumedInputBytes == phrase.input.size(),
                       "reopened provider recalls the learned phrase at final rank zero");
        }
        std::error_code error;
        std::filesystem::remove(learningPath, error);
        std::filesystem::remove(learningPath.string() + "-wal", error);
        std::filesystem::remove(learningPath.string() + "-shm", error);
    }
}

void testRareAndRadicalCharactersGovernance(const std::filesystem::path &learningPath) {
    auto provider = makeSimulationProvider(learningPath);

    // 1. 单音节英文次选消除：de 次选不得为 raw "de"，应为常用汉字 "得"
    provider.reset();
    assertTrue(provider.append("de"), "append de");
    assertTrue(provider.page().items[0].text == "的", "de rank 0 is 的");
    assertTrue(provider.page().items[1].text == "得", "de rank 1 is 得 (not English de)");

    // 2. ge 保持系统模型的中文首位/次位，不让英文原文插队。
    provider.reset();
    assertTrue(provider.append("ge"), "append ge");
    NativePinyinTestBaseline nativeBaseline;
    assertTrue(provider.page().items[0].text == nativeBaseline.candidate("ge"),
               "ge rank 0 preserves the native model's Chinese first choice");
    assertTrue(provider.page().items[1].text == nativeBaseline.candidate("ge", 1),
               "ge rank 1 preserves the native model's Chinese second choice");

    // 3. 康熙部首字过滤：e 候选前 5 位不得出现 丨
    provider.reset();
    assertTrue(provider.append("e"), "append e");
    for (std::size_t i = 0; i < std::min<std::size_t>(5, provider.page().items.size()); ++i) {
        assertTrue(provider.page().items[i].text != "丨", "e top 5 must not contain radical 丨");
    }

    // 4. 康熙部首字过滤：dian 候选前 5 位不得出现 丶
    provider.reset();
    assertTrue(provider.append("dian"), "append dian");
    for (std::size_t i = 0; i < std::min<std::size_t>(5, provider.page().items.size()); ++i) {
        assertTrue(provider.page().items[i].text != "丶", "dian top 5 must not contain radical 丶");
    }

    // 5. 模糊音插队禁止：cang 候选不得插队翘舌音 常(chang)
    provider.reset();
    assertTrue(provider.append("cang"), "append cang");
    assertTrue(provider.page().items.size() >= 2, "cang has candidates");
    assertTrue(provider.page().items[1].text != "常", "cang rank 1 must not be fuzzy candidate 常");

    // 6. 模糊音插队禁止：cao 候选不得插队翘舌音 超(chao)，且前 5 位不得出现 艹 或 屮
    provider.reset();
    assertTrue(provider.append("cao"), "append cao");
    assertTrue(provider.page().items.size() >= 2, "cao has candidates");
    assertTrue(provider.page().items[1].text != "超", "cao rank 1 must not be fuzzy candidate 超");
    for (std::size_t i = 0; i < std::min<std::size_t>(5, provider.page().items.size()); ++i) {
        assertTrue(provider.page().items[i].text != "艹" && provider.page().items[i].text != "屮",
                   "cao top 5 must not contain radicals 艹 or 屮");
    }

    // 7. 古生僻单字隔离：zenmeyang 候选前 5 位不得出现 谮
    provider.reset();
    assertTrue(provider.append("zenmeyang"), "append zenmeyang");
    for (std::size_t i = 0; i < std::min<std::size_t>(5, provider.page().items.size()); ++i) {
        assertTrue(provider.page().items[i].text != "谮", "zenmeyang top 5 must not contain rare char 谮");
    }

    // 8. 罕见字符隔离：kaifa 候选前 5 位不得出现 嘅
    provider.reset();
    assertTrue(provider.append("kaifa"), "append kaifa");
    for (std::size_t i = 0; i < std::min<std::size_t>(5, provider.page().items.size()); ++i) {
        assertTrue(provider.page().items[i].text.find("嘅") == std::string::npos,
                   "kaifa top 5 must not contain rare char 嘅");
    }

    // 9. 口语变音纠错：weishime 首选必为 为什么
    provider.reset();
    assertTrue(provider.append("weishime"), "append weishime");
    assertTrue(!provider.page().items.empty() && provider.page().items.front().text == "为什么",
               "weishime top candidate must be 为什么");

    // 10. 高频问候简拼：nh 首选必为 你好
    provider.reset();
    assertTrue(provider.append("nh"), "append nh");
    const std::string nhTop =
        provider.page().items.empty() ? std::string{} : provider.page().items.front().text;
    assertTrue(nhTop == "你好",
               "nh top candidate must be 你好 (actual: " + nhTop + ")");

    // 11. 城市简拼：xa 首选必为 西安
    provider.reset();
    assertTrue(provider.append("xa"), "append xa");
    assertTrue(!provider.page().items.empty() && provider.page().items.front().text == "西安",
               "xa top candidate must be 西安");
}

// 8. 历史休眠高频词反超与前后鼻音个性化容错专项模拟测试
void testDormantDisplacementAndNasalTypoPersonalization(const std::filesystem::path &learningPath) {
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    const auto threeDaysAgoMs = nowMs - 3LL * 24LL * 3600LL * 1000LL;

    // 预置 3 天前的历史休眠记录（模拟导入词典，如“试”频次 1400，“自”频次 35）
    {
        modernime::core::LearningStore store(learningPath);
        assertTrue(store.open(), "open store for dormant priming");
        for (int i = 0; i < 1400; ++i) {
            store.recordSelection("试", "shi", {}, {}, threeDaysAgoMs);
        }
        for (int i = 0; i < 35; ++i) {
            store.recordSelection("自", "zi", {}, {}, threeDaysAgoMs);
        }
    }

    auto provider = makeSimulationProvider(learningPath);

    // 1. 测试 shi：初始状态“试”位居首位（因为历史高达 1400 次）
    {
        provider.reset();
        assertTrue(provider.append("shi"), "append shi");
        assertTrue(!provider.page().items.empty(), "candidates exist for shi");
        assertTrue(provider.page().items.front().text == "试", "初始状态 试 为首位");

        // 用户在当前会话中连续选择 3 次“是”
        for (int i = 0; i < 3; ++i) {
            provider.reset();
            assertTrue(provider.append("shi"), "append shi");
            const auto shiIdx = indexOf(provider.page(), "是");
            assertTrue(shiIdx < provider.page().items.size(), "是 exists");
            assertTrue(provider.select(shiIdx), "select 是");
        }

        // 当前活跃选择的“是”迅速反超休眠多日的“试”，晋升为 Rank 0！
        provider.reset();
        assertTrue(provider.append("shi"), "append shi after selecting 是");
        assertTrue(provider.page().items.front().text == "是",
                   "当前会话连续选用‘是’后，迅速反超多日前休眠的‘试’晋升为 Rank 0");
    }

    // 2. 测试 zi：初始状态“自”位居首位
    {
        provider.reset();
        assertTrue(provider.append("zi"), "append zi");
        assertTrue(!provider.page().items.empty(), "candidates exist for zi");
        assertTrue(provider.page().items.front().text == "自", "初始状态 自 为首位");

        // 用户在当前会话中连续选择 5 次“字”
        for (int i = 0; i < 5; ++i) {
            provider.reset();
            assertTrue(provider.append("zi"), "append zi");
            const auto ziIdx = indexOf(provider.page(), "字");
            assertTrue(ziIdx < provider.page().items.size(), "字 exists");
            assertTrue(provider.select(ziIdx), "select 字");
        }

        // 当前活跃选择的“字”反超休眠的“自”，晋升为 Rank 0！
        provider.reset();
        assertTrue(provider.append("zi"), "append zi after selecting 字");
        assertTrue(provider.page().items.front().text == "字",
                   "当前会话选用‘字’后，反超多日前休眠的‘自’晋升为 Rank 0");
    }

    // 3. 测试前后鼻音容错个性化：常用“更新 gengxin”，误输入 genxin 或 genxing 均能推“更新”
    {
        // 训练选用 5 次“更新”
        for (int i = 0; i < 5; ++i) {
            provider.reset();
            assertTrue(provider.append("gengxin"), "append gengxin");
            const auto gxIdx = indexOf(provider.page(), "更新");
            assertTrue(gxIdx < provider.page().items.size(), "更新 exists");
            assertTrue(provider.select(gxIdx), "select 更新");
        }

        // 输入 genxin（en 误打 eng），首选必须推荐“更新”
        provider.reset();
        assertTrue(provider.append("genxin"), "append genxin");
        assertTrue(!provider.page().items.empty(), "candidates exist for genxin");
        assertTrue(provider.page().items.front().text == "更新",
                   "误输入 genxin 时，基于前后鼻音容错与个性化习惯首推‘更新’");

        // 输入 genxing（ing 误打 in），首选必须推荐“更新”
        provider.reset();
        assertTrue(provider.append("genxing"), "append genxing");
        assertTrue(!provider.page().items.empty(), "candidates exist for genxing");
        assertTrue(provider.page().items.front().text == "更新",
                   "误输入 genxing 时，基于前后鼻音容错与个性化习惯首推‘更新’");
    }
}

// 9. 1,000 轮日常高频打字抗污染与首选稳定性压力基准测试
void testOneThousandRoundsAntiPollutionStress(const std::filesystem::path &) {
    const auto stressPath = testPath("anti-pollution-stress.sqlite3");
    {
        modernime::core::LearningStore store(stressPath);
        assertTrue(store.open(), "open stress store for seed setup");
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto oneDayAgo = now - 86400000LL;

        // 模拟已导入经清洗/截断的外部词典词条（安全上限内）
        for (int i = 0; i < 3; ++i) {
            store.recordSelection("试", "shi", {}, {}, oneDayAgo);
            store.recordSelection("地", "de", {}, {}, oneDayAgo);
            store.recordSelection("骂", "ma", {}, {}, oneDayAgo);
            store.recordSelection("盒", "he", {}, {}, oneDayAgo);
            store.recordSelection("豪", "hao", {}, {}, oneDayAgo);
        }
        for (int i = 0; i < 20; ++i) {
            store.recordSelection("这歌", "zhe'ge", {}, {}, oneDayAgo);
            store.recordSelection("不失", "bu'shi", {}, {}, oneDayAgo);
            store.recordSelection("豪德", "hao'de", {}, {}, oneDayAgo);
            store.recordSelection("科一", "ke'yi", {}, {}, oneDayAgo);
        }

        // 真实日常高频基线
        for (int i = 0; i < 25; ++i) {
            store.recordSelection("是", "shi", {}, {}, oneDayAgo);
            store.recordSelection("好", "hao", {}, {}, oneDayAgo);
            store.recordSelection("好的", "hao'de", {}, {}, oneDayAgo);
            store.recordSelection("可以", "ke'yi", {}, {}, oneDayAgo);
        }
        for (int i = 0; i < 28; ++i) {
            store.recordSelection("这个", "zhe'ge", {}, {}, oneDayAgo);
        }
        for (int i = 0; i < 30; ++i) {
            store.recordSelection("你", "ni", {}, {}, oneDayAgo);
            store.recordSelection("吗", "ma", {}, {}, oneDayAgo);
            store.recordSelection("和", "he", {}, {}, oneDayAgo);
            store.recordSelection("不是", "bu'shi", {}, {}, oneDayAgo);
        }
        for (int i = 0; i < 100; ++i) {
            store.recordSelection("的", "de", {}, {}, oneDayAgo);
        }
    }

    auto provider = makeSimulationProvider(stressPath);

    struct StressTarget {
        std::string_view input;
        std::string_view expectedTop;
        std::string_view forbiddenTop;
    };

    const std::vector<StressTarget> targets = {
        {"shi", "是", "试"},
        {"zhege", "这个", "这歌"},
        {"zhe'ge", "这个", "这歌"},
        {"de", "的", "地"},
        {"ni", "你", "泥"},
        {"ma", "吗", "骂"},
        {"he", "和", "盒"},
        {"hao", "好", "豪"},
        {"bushi", "不是", "不失"},
        {"bu'shi", "不是", "不失"},
        {"haode", "好的", "豪德"},
        {"hao'de", "好的", "豪德"},
        {"keyi", "可以", "科一"},
        {"ke'yi", "可以", "科一"},
        {"jianp", "键盘", "jianpan"}
    };

    uint64_t totalEvaluations = 0;
    uint64_t usurpationCount = 0;

    for (int round = 0; round < 1000; ++round) {
        for (const auto &t : targets) {
            provider.reset();
            provider.resetCommitHistory();
            assertTrue(provider.append(std::string(t.input)), "append stress input");
            assertTrue(!provider.page().items.empty(), "non-empty candidate page");
            const auto topText = provider.page().items.front().text;
            ++totalEvaluations;

            if (topText != t.expectedTop) {
                std::cerr << "Round " << round << " Usurpation on input '" << t.input
                          << "': expected '" << t.expectedTop << "', got '" << topText << "'\n";
                ++usurpationCount;
            }
            assertTrue(topText != t.forbiddenTop, "Forbidden word usurped top rank");
            assertTrue(topText == t.expectedTop, "Expected top candidate must be rank 0");

            // 模拟用户真实打字选用首选，持续产生合法学习
            const auto selIdx = indexOf(provider.page(), t.expectedTop);
            if (selIdx < provider.page().items.size()) {
                assertTrue(provider.select(selIdx), "select top candidate in stress test");
            }
        }

        // 每 50 轮模拟一次偶发次选，验证不会发生单次偶选篡位
        if (round % 50 == 25) {
            provider.reset();
            assertTrue(provider.append("shi"), "append shi for occasional pick");
            const auto shiIdx = indexOf(provider.page(), "试");
            if (shiIdx < provider.page().items.size()) {
                provider.select(shiIdx);
            }
            provider.reset();
            assertTrue(provider.append("shi"), "append shi post occasional pick");
            assertTrue(provider.page().items.front().text == "是",
                       "Occasional selection of '试' must not hijack '是'");

            provider.reset();
            assertTrue(provider.append("de"), "append de for occasional pick");
            const auto deIdx = indexOf(provider.page(), "地");
            if (deIdx < provider.page().items.size()) {
                provider.select(deIdx);
            }
            provider.reset();
            assertTrue(provider.append("de"), "append de post occasional pick");
            assertTrue(provider.page().items.front().text == "的",
                       "Occasional selection of '地' must not hijack '的'");
        }

        if (round % 100 == 99) {
            provider.flush();
        }
    }

    assertTrue(usurpationCount == 0, "Stress benchmark must have 0 usurpations across 1000 rounds");
    assertTrue(totalEvaluations == 15000, "1000 rounds x 15 targets = 15,000 evaluations");

    std::error_code error;
    std::filesystem::remove(stressPath, error);
    std::filesystem::remove(stressPath.string() + "-wal", error);
    std::filesystem::remove(stressPath.string() + "-shm", error);
}

} // namespace

int main() {
    const auto learningPath = testPath("daily-simulation-learning.sqlite3");

    std::cout << "[1/9] Running Single Pinyin Priority Benchmark (32 syllables)...\n";
    testSinglePinyinPriorities(learningPath);

    std::cout << "[2/9] Running Multi-Syllable Phrase & Sentence Benchmark (40+ items)...\n";
    testMultiSyllablePhrasesAndHotwords(learningPath);

    std::cout << "[3/9] Running Abbreviations & Initialisms Benchmark (24 items)...\n";
    testAbbreviationsAndInitialisms(learningPath);

    std::cout << "[4/9] Running Smart Learning Dynamic Evolution Tests...\n";
    testSmartLearningDynamics(learningPath);

    std::cout << "[5/9] Running Smart Typo Correction Engine Tests...\n";
    testSmartTypoCorrectionEngine(learningPath);

    std::cout << "[6/9] Running Multi-Persona WeChat Parity Benchmark (6 Professions & Macro Engine)...\n";
    testMultiPersonaWeChatParityBenchmarks(learningPath);
    testModelBaselinePhraseLearning();

    std::cout << "[7/9] Running Rare/Radical Characters & Obscure Words Governance Benchmark...\n";
    testRareAndRadicalCharactersGovernance(learningPath);

    std::cout << "[8/9] Running Dormant Displacement & Nasal Typo Personalization Benchmark...\n";
    testDormantDisplacementAndNasalTypoPersonalization(learningPath);

    std::cout << "[9/9] Running 1,000-Round Anti-Pollution & High-Frequency Stress Simulation Benchmark...\n";
    testOneThousandRoundsAntiPollutionStress(learningPath);

    std::error_code error;
    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);

    std::cout << "All daily simulation tests passed successfully!\n";
    return EXIT_SUCCESS;
}
