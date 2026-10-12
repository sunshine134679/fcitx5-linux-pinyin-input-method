#include "modernime/core/candidate_model.h"
#include "modernime/core/learning_snapshot.h"
#include "modernime/core/learning_store.h"
#include "modernime/core/learning_writer.h"
#include "modernime/core/personal_engine.h"
#include "modernime/core/phrase_formation.h"
#include "modernime/fcitx5/engine.h"
#include "modernime/pinyin/pinyin_candidate_provider.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

using modernime::core::CandidatePage;
using modernime::core::CommitEvent;
using modernime::core::EngineConfig;
using modernime::core::LearningSnapshot;
using modernime::core::LearningStore;
using modernime::core::PersonalEngine;
using modernime::core::WordMeta;

std::filesystem::path makeTempPath(std::string_view suffix) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("modernime-gtest-" + std::string(suffix));
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::remove(path.string() + "-lock", ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
    return path;
}

std::size_t indexOf(const CandidatePage &page, std::string_view text) {
    for (std::size_t i = 0; i < page.items.size(); ++i) {
        if (page.items[i].text == text) {
            return i;
        }
    }
    return page.items.size();
}

struct RecordingHost final : modernime::fcitx5::EngineHost {
    std::vector<CandidatePage> pages;
    std::vector<std::string> commits;

    void publishPage(const CandidatePage &page) override {
        pages.push_back(page);
    }

    void commit(std::string_view text) override {
        commits.emplace_back(text);
    }
};

} // namespace

// 1. 拼音规范化及不同拼音键之间的隔离
TEST(PersonalEngineTest, PinyinNormalizationAndKeyIsolation) {
    EXPECT_EQ(PersonalEngine::NormalizePinyinKey(
                  std::vector<std::string>{"Qu", "Dong"}),
              "!u!qu,dong");
    EXPECT_EQ(PersonalEngine::NormalizePinyinKey("qu'dong"), "!u!qu,dong");
    EXPECT_EQ(PersonalEngine::NormalizePinyinKey("QU DONG"), "!u!qu,dong");
    EXPECT_EQ(PersonalEngine::NormalizePinyinKey("!u!Qu,Dong"), "!u!qu,dong");
    EXPECT_EQ(PersonalEngine::CompactPinyin("!u!qu,dong"), "qudong");
    EXPECT_EQ(PersonalEngine::ToApostrophePinyin("!u!qu,dong"), "qu'dong");

    PersonalEngine engine;
    ASSERT_TRUE(engine.RecordUsage("qu'dong", "驱动"));
    ASSERT_TRUE(engine.RecordUsage("sheng'dong", "生动"));
    ASSERT_TRUE(engine.RecordUsage("xi'an", "西安"));
    ASSERT_TRUE(engine.RecordUsage("xian", "先"));

    const auto qudong = engine.QueryCandidates("!u!qu,dong");
    ASSERT_EQ(qudong.size(), 1u);
    EXPECT_EQ(qudong[0].text, "驱动");

    const auto shengdong = engine.QueryCandidates("!u!sheng,dong");
    ASSERT_EQ(shengdong.size(), 1u);
    EXPECT_EQ(shengdong[0].text, "生动");

    // 验证 "xi,an" 与 "xian" 严格隔离，不混淆音节切分边界
    const auto xian_two = engine.QueryCandidates("!u!xi,an");
    ASSERT_EQ(xian_two.size(), 1u);
    EXPECT_EQ(xian_two[0].text, "西安");

    const auto xian_one = engine.QueryCandidates("!u!xian");
    ASSERT_EQ(xian_one.size(), 1u);
    EXPECT_EQ(xian_one[0].text, "先");
}

// 2. 使用频次增长与对数平滑得分变化
TEST(PersonalEngineTest, FrequencyLogarithmicGrowth) {
    EngineConfig cfg;
    cfg.alpha = 1.2f;
    cfg.beta = 2.5f;
    PersonalEngine engine(cfg);
    uint32_t fake_now = 1700000000U;
    engine.SetClock([&fake_now]() { return fake_now; });

    const std::string key = PersonalEngine::NormalizePinyinKey(
        std::vector<std::string>{"qu", "dong"});
    ASSERT_TRUE(engine.RecordUsage(key, "生动", 1.0f));
    ASSERT_TRUE(engine.RecordUsage(key, "驱动", 1.0f));

    const float score_1 = engine.QueryCandidates(key)[0].dynamic_score;
    ASSERT_TRUE(engine.RecordUsage(key, "驱动", 1.0f));
    const float score_2 = engine.QueryCandidates(key)[0].dynamic_score;
    for (int i = 0; i < 8; ++i) {
        ASSERT_TRUE(engine.RecordUsage(key, "驱动", 1.0f));
    }
    const auto ranked = engine.QueryCandidates(key);
    ASSERT_EQ(ranked.size(), 2u);
    EXPECT_EQ(ranked[0].text, "驱动");
    EXPECT_EQ(ranked[0].meta.hit_count, 10u);

    const float score_10 = ranked[0].dynamic_score;
    ASSERT_TRUE(engine.RecordUsage(key, "驱动", 1.0f));
    const float score_11 = engine.QueryCandidates(key)[0].dynamic_score;

    EXPECT_GT(score_2, score_1);
    EXPECT_GT(score_10, score_2);
    // 对数边际收益递减：从 1->2 的增幅显著大于从 10->11 的增幅
    EXPECT_GT(score_2 - score_1, score_11 - score_10);
}

// 3. 可注入时间源，严格验证 tau = T_{1/2}/ln(2) 半衰期换算及 30 天衰减
TEST(PersonalEngineTest, InjectableClockTauHalfLifeAnd30DayDecay) {
    EngineConfig cfg;
    cfg.alpha = 1.2f;
    cfg.beta = 2.5f;
    cfg.half_life_seconds = 7 * 86400; // 7 天半衰期
    PersonalEngine engine(cfg);

    uint32_t current_time = 1700000000U;
    engine.SetClock([&current_time]() { return current_time; });

    const std::string key = "!u!qu,dong";
    // 在 t=0 时将“旧高频词”点击 4 次
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(engine.RecordUsage(key, "旧高频词", 1.0f));
    }

    const auto at_t0 = engine.QueryCandidates(key);
    ASSERT_EQ(at_t0.size(), 1u);
    const float expected_base_plus_freq =
        1.0f + cfg.alpha * std::log(4.0f + 1.0f);
    EXPECT_NEAR(at_t0[0].dynamic_score, expected_base_plus_freq + cfg.beta, 1e-4f);

    // 快进恰好 1 个半衰期（7 天）：时间衰减项必须严格等于 0.5 * beta = 1.25
    engine.AdvanceClock(7 * 86400);
    const auto at_7d = engine.QueryCandidates(key);
    ASSERT_EQ(at_7d.size(), 1u);
    EXPECT_NEAR(at_7d[0].dynamic_score,
                expected_base_plus_freq + 0.5f * cfg.beta, 1e-4f);

    // 快进至 30 天（再前进 23 天）：衰减因子 exp(-30/7 * ln2) = 2^(-30/7) ≈ 0.05127
    engine.AdvanceClock(23 * 86400);
    const auto at_30d = engine.QueryCandidates(key);
    ASSERT_EQ(at_30d.size(), 1u);
    const float expected_30d_decay =
        cfg.beta * static_cast<float>(std::pow(0.5, 30.0 / 7.0));
    EXPECT_NEAR(at_30d[0].dynamic_score,
                expected_base_plus_freq + expected_30d_decay, 1e-4f);

    // 在第 30 天新选 2 次“新热词”，因近期加权高，应超越 30 天前选过 4 次的“旧高频词”
    ASSERT_TRUE(engine.RecordUsage(key, "新热词", 1.0f));
    ASSERT_TRUE(engine.RecordUsage(key, "新热词", 1.0f));
    const auto ranked = engine.QueryCandidates(key);
    ASSERT_EQ(ranked.size(), 2u);
    EXPECT_EQ(ranked[0].text, "新热词");
    EXPECT_EQ(ranked[1].text, "旧高频词");

    // 同步验证 LearningSnapshot::boostAt 30 天半衰期的精确换算
    LearningSnapshot snap;
    const int64_t t0_ms = 1000000000LL;
    const int64_t thirty_days_ms = 30LL * 24LL * 3600LL * 1000LL;
    snap.recordSelection("测试词", "ceshici", {}, {}, t0_ms);
    snap.recordSelection("测试词", "ceshici", {}, {}, t0_ms);
    snap.recordSelection("测试词", "ceshici", {}, {}, t0_ms);
    const double boost_t0 = snap.boostAt("测试词", "ceshici", t0_ms);
    const double boost_30d = snap.boostAt("测试词", "ceshici", t0_ms + thirty_days_ms);
    // recentScale(freq=3) = 0.90，经过 1 个 30 天半衰期后近期分恰好衰减 0.45
    EXPECT_NEAR(boost_t0 - boost_30d, 0.45, 1e-4);
}

// 4. 上下文关联、UTF-8 边界清洗、上下文失效、无上下文回退及敏感场景保护
TEST(PersonalEngineTest, SurroundingContextAffinityInvalidationAndPrivacy) {
    // 4.1 验证换行符、控制字符过滤与 UTF-8 多字节字符边界保护
    const std::string raw_ctx = "第一行旧内容\n第二行\r\t正在编写嵌入式Linux";
    EXPECT_EQ(PersonalEngine::SanitizeSurroundingContext(raw_ctx, 12),
              "正在编写嵌入式Linux");
    // 窗口截断为最近 7 个 UTF-8 字符（"入式Linux" = 2 个汉字 + 5 个字母 = 7 字符）
    EXPECT_EQ(PersonalEngine::SanitizeSurroundingContext(raw_ctx, 7),
              "入式Linux");

    // 构造损坏的 UTF-8 前缀字节 + 合法后续文本，验证不会切出半个 UTF-8 字符
    std::string broken_utf8 = "\xE4\xB8"; // 截断的 3 字节 UTF-8
    broken_utf8 += "合法上下文";
    EXPECT_EQ(PersonalEngine::SanitizeSurroundingContext(broken_utf8, 8),
              "合法上下文");

    // 4.2 上下文加权、失效与回退
    PersonalEngine engine;
    const std::string key = "!u!qu,dong";
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(engine.RecordUsage(key, "生动", 1.0f));
    }
    ASSERT_TRUE(engine.RecordUsage(key, "驱动", 1.0f, "正在编写嵌入式Linux"));

    // 无上下文时：“生动”（频次 3）排第一
    const auto no_ctx = engine.QueryCandidates(key);
    ASSERT_EQ(no_ctx.size(), 2u);
    EXPECT_EQ(no_ctx[0].text, "生动");

    // 有匹配上下文时：“驱动”获得 gamma 上下文加权跃居第一
    engine.UpdateActiveContext("前缀段落\n正在编写嵌入式Linux", "org.kde.kate");
    const auto with_ctx = engine.QueryCandidates(key);
    ASSERT_EQ(with_ctx.size(), 2u);
    EXPECT_EQ(with_ctx[0].text, "驱动");

    // 跨应用切换或手动失效后：旧上下文立即清空，回退为无上下文排序
    engine.UpdateActiveContext("", "org.mozilla.firefox");
    EXPECT_TRUE(engine.ActiveContext().empty());
    const auto after_switch = engine.QueryCandidates(key);
    ASSERT_EQ(after_switch.size(), 2u);
    EXPECT_EQ(after_switch[0].text, "生动");

    // 4.3 密码与敏感输入保护（Password / Sensitive）
    engine.UpdateActiveContext("正在编写嵌入式Linux", "org.kde.kate");
    engine.SetSensitiveContext(true);
    EXPECT_TRUE(engine.ActiveContext().empty());
    EXPECT_FALSE(engine.RecordUsage("!u!mi,ma", "密码词"));
    EXPECT_TRUE(engine.QueryCandidates("!u!mi,ma").empty());
    // 敏感模式下即使传入上下文也不得触发上下文提权
    const auto sensitive_query =
        engine.QueryCandidates(key, "正在编写嵌入式Linux");
    ASSERT_EQ(sensitive_query.size(), 2u);
    EXPECT_EQ(sensitive_query[0].text, "生动");
    engine.SetSensitiveContext(false);
}

// 5. 容量上限与剪枝行为
TEST(PersonalEngineTest, CapacityLimitAndPruning) {
    EngineConfig cfg;
    cfg.max_dict_capacity = 100;
    cfg.prune_target_size = 80;
    PersonalEngine engine(cfg);

    uint32_t current_time = 1700000000U;
    engine.SetClock([&current_time]() { return current_time; });

    // 先插入 91 个冷词（30 天前仅使用 1 次）
    for (int i = 0; i < 91; ++i) {
        ASSERT_TRUE(engine.RecordUsage(
            "!u!cold," + std::to_string(i), "冷词" + std::to_string(i), 0.5f));
    }
    // 插入 5 个被屏蔽词
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(engine.RecordSuppression(
            "!u!sup," + std::to_string(i), "屏蔽词" + std::to_string(i)));
    }

    // 时间快进 30 天后，插入 5 个高频近期热词（触发超过 100 条上限自动剪枝至 80 条）
    engine.AdvanceClock(30 * 86400);
    for (int i = 0; i < 5; ++i) {
        const std::string hot_key = "!u!hot," + std::to_string(i);
        const std::string hot_word = "热词" + std::to_string(i);
        ASSERT_TRUE(engine.RecordUsage(hot_key, hot_word, 2.0f));
        ASSERT_TRUE(engine.RecordUsage(hot_key, hot_word, 2.0f));
    }

    EXPECT_LE(engine.GetTotalEntries(), 80u);
    // 所有近期热词必须 100% 保留
    for (int i = 0; i < 5; ++i) {
        const auto res = engine.QueryCandidates("!u!hot," + std::to_string(i));
        ASSERT_EQ(res.size(), 1u);
        EXPECT_EQ(res[0].text, "热词" + std::to_string(i));
    }
}

// 6. LMDB 持久化、动态扩容、重启恢复、SQLite 迁移与事务失败恢复
TEST(PersonalEngineTest, LmdbPersistenceMapResizeMigrationAndRecovery) {
    const auto dir = makeTempPath("lmdb-suite");
    std::filesystem::create_directories(dir);
    const auto sqlite_path = dir / "learning.sqlite3";
    const auto lmdb_path = dir / "learning.lmdb";
    const auto backup_path = dir / "learning-backup.lmdb";

    // 6.1 准备含完整状态的旧版 learning.sqlite3 数据（含上下文、负反馈、屏蔽标记与 reset_revision）
    {
        LearningStore sql_store(sqlite_path);
        ASSERT_TRUE(sql_store.open());
        ASSERT_TRUE(sql_store.clear()); // reset_revision 递增为 1
        ASSERT_TRUE(sql_store.recordSelection(
            "王绍龙", "wang'shao'long", "联系人：", "", 1700000000000LL));
        ASSERT_TRUE(sql_store.recordSelection(
            "王绍龙", "wang'shao'long", "联系人：", "", 1700000005000LL));
        ASSERT_TRUE(sql_store.recordSelection(
            "gnueabihf", "gnueabihf", "", "", 1700000010000LL));
        ASSERT_TRUE(sql_store.recordSuppression("废弃词", "feiqici"));
        sql_store.close();
    }

    // 6.2 打开同目录 LMDB（设置 64 KiB 极小初始 map_size 以验证自动扩容与自动 SQLite 迁移）
    EngineConfig cfg;
    cfg.initial_lmdb_map_size = 64 * 1024;
    {
        PersonalEngine lmdb_engine(cfg);
        ASSERT_TRUE(lmdb_engine.OpenLmdb(lmdb_path));
        EXPECT_TRUE(lmdb_engine.IsMigratedFromSqlite());
        EXPECT_EQ(lmdb_engine.ResetRevision(), 1);
        // 原 SQLite 数据库必须完好保留，不得被静默删除
        EXPECT_TRUE(std::filesystem::exists(sqlite_path));

        // 验证迁移词条的精确查询与前缀查询
        const auto wsl_cands =
            lmdb_engine.QueryLmdbCandidates("wang'shao'long", "联系人：");
        ASSERT_EQ(wsl_cands.size(), 1u);
        EXPECT_EQ(wsl_cands[0].text, "王绍龙");
        EXPECT_EQ(wsl_cands[0].meta.hit_count, 2u);

        const auto gnu_prefix = lmdb_engine.QueryLmdbPrefixCandidates("gnu");
        ASSERT_FALSE(gnu_prefix.empty());
        EXPECT_EQ(gnu_prefix[0].text, "gnueabihf");

        // 写入 1500 条记录迫使 LMDB 触发 MDB_MAP_FULL 自动翻倍扩容
        for (int i = 0; i < 1500; ++i) {
            ASSERT_TRUE(lmdb_engine.RecordUsage(
                "!u!kuo,rong," + std::to_string(i),
                "扩容测试词条_" + std::to_string(i),
                1.0f,
                "上下文前缀"));
        }
        EXPECT_GT(lmdb_engine.LmdbMapSize(), 64u * 1024u);

        // 6.3 注入写入故障：验证失败不会破坏已有状态，恢复后可继续正常写入
        lmdb_engine.SetSimulateWriteFailure(true);
        EXPECT_FALSE(lmdb_engine.RecordUsage("!u!fail,key", "不应写入的词"));
        lmdb_engine.SetSimulateWriteFailure(false);
        ASSERT_TRUE(lmdb_engine.RecordUsage("!u!recover,key", "故障恢复词"));

        // 6.4 验证 LMDB 紧凑备份
        ASSERT_TRUE(lmdb_engine.BackupLmdbTo(backup_path));
        lmdb_engine.CloseLmdb();
    }

    // 6.5 重启恢复验证与 LearningStore (.lmdb) 统一接口验证
    {
        LearningStore lmdb_store(lmdb_path);
        ASSERT_TRUE(lmdb_store.isLmdbBackend());
        ASSERT_TRUE(lmdb_store.open());
        EXPECT_EQ(lmdb_store.resetRevision().value_or(0), 1);

        const auto snap = lmdb_store.snapshot();
        ASSERT_NE(snap, nullptr);
        EXPECT_NE(snap->entry("王绍龙", "wang'shao'long", "联系人：", ""), nullptr);
        EXPECT_NE(snap->entry("故障恢复词", "recover'key", "", ""), nullptr);
        EXPECT_EQ(snap->entry("不应写入的词", "fail'key", "", ""), nullptr);
        EXPECT_TRUE(snap->isSuppressed("废弃词", "feiqici"));
        lmdb_store.close();
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// 7. 实际提交事件不重复学习 & 部分音节消费范围一致性 & 释义/命令/取消不学习
TEST(PersonalEngineTest, CommitDeduplicationAndConsumedRangeConsistency) {
    const auto lmdb_path = makeTempPath("commit-dedup.lmdb");
    modernime::pinyin::PinyinDataPaths paths;
    paths.learningStore = lmdb_path.string();
    modernime::pinyin::PinyinProviderOptions options;
    options.learningEnabled = true;

    {
        modernime::pinyin::PinyinCandidateProvider provider(paths, options);
        RecordingHost host;
        modernime::fcitx5::ModernIMEController controller(host, &provider);

        // 7.1 整词提交：仅触发 1 次提交事件，词频精确为 1
        for (char c : std::string_view("nihao")) {
            ASSERT_TRUE(controller.handle(
                {modernime::fcitx5::KeyKind::Character, c, 0}));
        }
        ASSERT_TRUE(controller.handle(
            {modernime::fcitx5::KeyKind::Space, 0, 0}));
        ASSERT_EQ(host.commits.size(), 1u);
        EXPECT_EQ(host.commits.back(), "你好");

        // 7.2 部分音节选择：输入 "nihaoma" 先选 "你好"，验证记录的 fullPinyin 为 "ni'hao" 而非 "ni'hao'ma"
        for (char c : std::string_view("nihaoma")) {
            ASSERT_TRUE(controller.handle(
                {modernime::fcitx5::KeyKind::Character, c, 0}));
        }
        const auto idx_nihao = indexOf(controller.page(), "你好");
        ASSERT_LT(idx_nihao, controller.page().items.size());
        ASSERT_TRUE(controller.select(idx_nihao));
        EXPECT_EQ(host.commits.back(), "你好");
        EXPECT_EQ(controller.page().rawInput, "ma");

        // 取消剩余的 "ma" 输入，验证取消的预编辑不会产生学习记录
        ASSERT_TRUE(controller.handle(
            {modernime::fcitx5::KeyKind::Escape, 0, 0}));
        EXPECT_TRUE(controller.page().rawInput.empty());

        // 7.3 敏感输入场景（Password / Sensitive）下提交不记录学习
        controller.setSensitive(true);
        for (char c : std::string_view("mima")) {
            ASSERT_TRUE(controller.handle(
                {modernime::fcitx5::KeyKind::Character, c, 0}));
        }
        ASSERT_TRUE(controller.handle(
            {modernime::fcitx5::KeyKind::Space, 0, 0}));
        controller.setSensitive(false);

        provider.flush();

        LearningStore verify_store(lmdb_path);
        ASSERT_TRUE(verify_store.open());
        const auto snap = verify_store.snapshot();
        // 7.1 无上下文整词记录恰好 1 次，7.2 携带会话上文 "你好" 部分选择记录恰好 1 次（均无重复记录，且拼音严格为 nihao 而非 nihaoma）
        const auto *nihao_base = snap->entry("你好", "ni'hao", "", "");
        const auto *nihao_ctx = snap->entry("你好", "ni'hao", "你好", "");
        ASSERT_NE(nihao_base, nullptr);
        ASSERT_NE(nihao_ctx, nullptr);
        EXPECT_EQ(nihao_base->frequency, 1);
        EXPECT_EQ(nihao_ctx->frequency, 1);
        EXPECT_EQ(snap->entry("你好", "ni'hao'ma", "", ""), nullptr);
        EXPECT_FALSE(snap->hasPositiveFrequency("密码", "mi'ma"));
        verify_store.close();
    }

    std::error_code ec;
    std::filesystem::remove(lmdb_path, ec);
    std::filesystem::remove(lmdb_path.string() + "-lock", ec);
}

// 8. 中英统一仲裁：学习 developer 后输入 de 不压过“的”；学习 gnueabihf 后 gnu/gnue 正确补全
TEST(PersonalEngineTest, EnglishArbitrationDeveloperVsDeAndGnueabihfPrefixRecall) {
    const auto lmdb_path = makeTempPath("arbitration.lmdb");
    modernime::pinyin::PinyinDataPaths paths;
    paths.learningStore = lmdb_path.string();
    modernime::pinyin::PinyinProviderOptions options;
    options.learningEnabled = true;

    {
        modernime::pinyin::PinyinCandidateProvider provider(paths, options);

        // 8.1 连续学习 developer 5 次
        for (int i = 0; i < 5; ++i) {
            provider.reset();
            ASSERT_TRUE(provider.append("developer"));
            const auto dev_idx = indexOf(provider.page(), "developer");
            ASSERT_LT(dev_idx, provider.page().items.size());
            ASSERT_TRUE(provider.select(dev_idx));
        }

        // 8.2 学习专业术语 gnueabihf 2 次
        for (int i = 0; i < 2; ++i) {
            provider.reset();
            ASSERT_TRUE(provider.append("gnueabihf"));
            const auto gnu_idx = indexOf(provider.page(), "gnueabihf");
            ASSERT_LT(gnu_idx, provider.page().items.size());
            ASSERT_TRUE(provider.select(gnu_idx));
        }
        provider.flush();

        // 8.3 输入 de：首选必须保持高频中文“的”，developer 仅作为第 2 候选
        provider.reset();
        ASSERT_TRUE(provider.append("de"));
        ASSERT_GE(provider.page().items.size(), 2u);
        EXPECT_EQ(provider.page().items[0].text, "的");
        EXPECT_EQ(provider.page().items[1].text, "developer");

        // 8.4 输入 dev / devel：无高置信度中文冲突，developer 正常排第 1 位
        provider.reset();
        ASSERT_TRUE(provider.append("dev"));
        ASSERT_FALSE(provider.page().items.empty());
        EXPECT_EQ(provider.page().items[0].text, "developer");

        provider.reset();
        ASSERT_TRUE(provider.append("devel"));
        ASSERT_FALSE(provider.page().items.empty());
        EXPECT_EQ(provider.page().items[0].text, "developer");

        // 8.5 在中文上下文中输入 gnu / gnue：均能正确按前缀补全 gnueabihf
        provider.setContext("正在配置交叉编译工具链", "");
        for (std::string_view prefix : {"gnu", "gnue"}) {
            provider.reset();
            ASSERT_TRUE(provider.append(prefix));
            const auto idx = indexOf(provider.page(), "gnueabihf");
            EXPECT_LT(idx, provider.page().items.size())
                << "prefix " << prefix << " should recall gnueabihf";
            EXPECT_EQ(provider.page().items[0].text, "gnueabihf");
        }
    }

    std::error_code ec;
    std::filesystem::remove(lmdb_path, ec);
    std::filesystem::remove(lmdb_path.string() + "-lock", ec);
}

// 9. 分段造词（王→绍→龙 => 王绍龙；候选→栏 => 候选栏）与以词定字无回归
TEST(PersonalEngineTest, SegmentedPhraseFormationAndSingleCharExtraction) {
    const auto lmdb_path = makeTempPath("phrase-formation.lmdb");
    modernime::pinyin::PinyinDataPaths paths;
    paths.learningStore = lmdb_path.string();
    modernime::pinyin::PinyinProviderOptions options;
    options.learningEnabled = true;

    {
        modernime::pinyin::PinyinCandidateProvider provider(paths, options);
        RecordingHost host;
        modernime::fcitx5::ModernIMEController controller(host, &provider);

        auto commitSegment = [&](std::string_view py,
                                 std::string_view expected_text) {
            ASSERT_TRUE(provider.append(py));
            const auto idx = indexOf(provider.page(), expected_text);
            ASSERT_LT(idx, provider.page().items.size())
                << "missing segment candidate: " << expected_text;
            ASSERT_TRUE(provider.select(idx));
        };

        // 9.1 反复分段输入“王 → 绍 → 龙” 3 次，自动合成并学会人名“王绍龙”
        for (int round = 0; round < 3; ++round) {
            commitSegment("wang", "王");
            commitSegment("shao", "绍");
            commitSegment("long", "龙");
            provider.resetCommitHistory();
        }
        provider.flush();

        provider.reset();
        ASSERT_TRUE(provider.append("wangshaolong"));
        ASSERT_FALSE(provider.page().items.empty());
        EXPECT_EQ(provider.page().items[0].text, "王绍龙");

        // 9.2 反复分段输入“候选 → 栏” 3 次，自动合成并学会新词“候选栏”
        provider.reset();
        for (int round = 0; round < 3; ++round) {
            commitSegment("houxuan", "候选");
            commitSegment("lan", "栏");
            provider.resetCommitHistory();
        }
        provider.flush();

        provider.reset();
        ASSERT_TRUE(provider.append("houxuanlan"));
        ASSERT_FALSE(provider.page().items.empty());
        EXPECT_EQ(provider.page().items[0].text, "候选栏");

        // 9.3 单字提取（以词定字 `[` 首字 / `]` 末字）
        controller.reset();
        for (char c : std::string_view("nihao")) {
            ASSERT_TRUE(controller.handle(
                {modernime::fcitx5::KeyKind::Character, c, 0}));
        }
        ASSERT_TRUE(controller.handle(
            {modernime::fcitx5::KeyKind::CommitFirstCharacter, '[', 0}));
        ASSERT_FALSE(host.commits.empty());
        EXPECT_EQ(host.commits.back(), "你");

        for (char c : std::string_view("nihao")) {
            ASSERT_TRUE(controller.handle(
                {modernime::fcitx5::KeyKind::Character, c, 0}));
        }
        ASSERT_TRUE(controller.handle(
            {modernime::fcitx5::KeyKind::CommitLastCharacter, ']', 0}));
        EXPECT_EQ(host.commits.back(), "好");
    }

    std::error_code ec;
    std::filesystem::remove(lmdb_path, ec);
    std::filesystem::remove(lmdb_path.string() + "-lock", ec);
}
