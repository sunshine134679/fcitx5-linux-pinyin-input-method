#pragma once

#include "modernime/core/learning_snapshot.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct MDB_env;

namespace modernime::core {

namespace detail {
class LmdbEnvironment;
}

struct LearningEvent;

// 词条元数据结构体（常驻内存 / LMDB 二进制零拷贝序列化存储）
#pragma pack(push, 1)
struct WordMeta {
    uint32_t last_used_timestamp = 0;  // 最后使用时间戳 (Unix Epoch Seconds)
    uint32_t hit_count = 0;            // 历史使用频次
    float base_weight = 1.0f;          // 基础先验权重
    uint16_t text_bytes_len = 0;       // 文本 UTF-8 字节长度
    uint16_t context_before_len = 0;   // 光标前上下文 UTF-8 字节长度
    uint16_t context_after_len = 0;    // 光标后上下文 UTF-8 字节长度
    uint32_t negative_feedback = 0;    // 负反馈计数（删词/回退修正）
    uint8_t suppressed = 0;            // 显式屏蔽标记（1 表示已屏蔽）
    int64_t last_selected_ms = 0;      // 毫秒级时间戳（与 LearningEntry 无损对齐）
};
#pragma pack(pop)

// 个性化候选词条
struct PersonalCandidate {
    std::string text;           // 候选短语 (UTF-8)
    std::string pinyin_key;     // 规范化拼音键名，如 "!u!qu,dong"
    std::string raw_pinyin;     // 原始拼音，如 "qu'dong"
    std::string context_before; // 关联前文
    std::string context_after;  // 关联后文
    float dynamic_score = 0.0f; // 综合动态得分
    WordMeta meta{};            // 元数据
};

// 个性化自适应引擎配置
struct EngineConfig {
    float alpha = 1.2f;                     // 频次权重系数: alpha * ln(count + 1)
    float beta = 2.5f;                      // 时间衰减系数: beta * exp(-delta_t / tau)
    float gamma = 3.0f;                     // 上下文命中奖励: gamma * context_factor
    uint32_t half_life_seconds = 7 * 86400; // 时间衰减半衰期 T_1/2 (默认 7 天 = 604800 秒)
    size_t max_dict_capacity = 200000;      // 用户词库最大容量，超出自动剪枝
    size_t prune_target_size = 160000;      // 剪枝后保留的最优词条数
    uint32_t magic_header = 0xA55AA55A;     // 存储校验魔数
    size_t initial_lmdb_map_size = 16ULL * 1024ULL * 1024ULL; // LMDB 初始映射容量 (默认 16 MiB)
};

class PersonalEngine {
public:
    using ClockFn = std::function<uint32_t()>;

    explicit PersonalEngine(EngineConfig config = EngineConfig());
    ~PersonalEngine();

    PersonalEngine(const PersonalEngine &) = delete;
    PersonalEngine &operator=(const PersonalEngine &) = delete;

    // 格式化拼音键名：转小写并用逗号严格分割音节，前缀 "!u!"
    static std::string NormalizePinyinKey(const std::vector<std::string> &syllables);
    static std::string NormalizePinyinKey(std::string_view raw_pinyin);
    // 提取紧凑无分隔符拼音（用于前缀索引与 LearningSnapshot 对齐）
    static std::string CompactPinyin(std::string_view pinyin_key);
    // 还原标准单引号分隔拼音（用于 LearningEntry 对齐）
    static std::string ToApostrophePinyin(std::string_view pinyin_key);

    // 安全清洗光标前周围文本：截断换行/控制字符，严格对齐 UTF-8 字符边界并限制窗口长度
    static std::string SanitizeSurroundingContext(std::string_view raw_before_cursor,
                                                  size_t max_codepoints = 12);

    // 注入测试时钟与快进支持（默认使用系统时钟）
    void SetClock(ClockFn clock_fn);
    void AdvanceClock(uint32_t delta_seconds);
    uint32_t NowSeconds() const;

    // 隐私与会话边界控制（Password / Sensitive 场景禁用学习与上下文）
    void SetSensitiveContext(bool sensitive) noexcept {
        sensitive_context_ = sensitive;
        if (sensitive) {
            active_context_.clear();
        }
    }
    bool IsSensitiveContext() const noexcept { return sensitive_context_; }

    void UpdateActiveContext(std::string_view raw_before_cursor,
                             std::string_view app_id = {});
    void InvalidateContext() noexcept { active_context_.clear(); }
    std::string_view ActiveContext() const noexcept { return active_context_; }

    void SetMaxCapacity(size_t max_capacity, size_t prune_target = 0);

    // 学习/记录用户选词习惯
    bool RecordUsage(std::string_view pinyin_key,
                     std::string_view text,
                     float initial_weight = 1.0f,
                     std::string_view context_before = {},
                     std::string_view context_after = {},
                     std::optional<int64_t> timestamp_ms = std::nullopt);

    // Applies all events and reset checks in one LMDB transaction.
    // Empty input repairs excess context variants left by older versions.
    bool RecordLearningBatch(const std::vector<LearningEvent> &events,
                             float initial_weight = 1.0f);

    bool RecordNegativeFeedback(std::string_view pinyin_key,
                                std::string_view text);
    bool RecordSuppression(std::string_view pinyin_key,
                           std::string_view text);

    // 精确查询候选词，并根据用户习惯与上下文进行动态排序
    std::vector<PersonalCandidate> QueryCandidates(
        std::string_view pinyin_key,
        std::string_view surrounding_context = {}) const;

    // 前缀范围查询（基于有序键 / LMDB MDB_SET_RANGE 游标，避免全表扫描）
    std::vector<PersonalCandidate> QueryPrefixCandidates(
        std::string_view pinyin_prefix,
        std::string_view surrounding_context = {},
        size_t limit = 20) const;

    // 直接通过 LMDB mmap 零拷贝游标执行精确查询与前缀查询
    std::vector<PersonalCandidate> QueryLmdbCandidates(
        std::string_view pinyin_key,
        std::string_view surrounding_context = {}) const;
    std::vector<PersonalCandidate> QueryLmdbPrefixCandidates(
        std::string_view pinyin_prefix,
        std::string_view surrounding_context = {},
        size_t limit = 20) const;

    // 计算综合个性化得分（严格遵循 tau = T_{1/2} / ln(2)）
    float CalculateScore(const WordMeta &meta,
                         std::string_view text,
                         std::string_view stored_context_before,
                         std::string_view query_context,
                         uint32_t now) const noexcept;

    float CalculateScore(const WordMeta &meta,
                         std::string_view text,
                         std::string_view query_context,
                         uint32_t now) const noexcept {
        return CalculateScore(meta, text, {}, query_context, now);
    }

    // LMDB 持久化生命周期与迁移接口
    bool OpenLmdb(const std::filesystem::path &lmdb_path, bool read_only = false);
    void CloseLmdb() noexcept;
    bool IsLmdbOpen() const noexcept { return env_ != nullptr; }
    size_t LmdbMapSize() const noexcept;
    bool FlushToLmdb();
    bool ReloadFromLmdb();
    // Exports entries and their clear revision from the same read transaction.
    bool ReadLearningState(std::vector<LearningEntry> &entries,
                           int64_t &reset_revision) const;
    bool BackupLmdbTo(const std::filesystem::path &backup_path) const;
    bool Clear();

    // 从现有 SQLite (learning.sqlite3) 无损迁移至 LMDB，保留原 SQLite 文件不删除
    bool MigrateFromSqlite(const std::filesystem::path &sqlite_path);
    bool IsMigratedFromSqlite() const;

    // 故障注入开关（用于测试事务失败与自动恢复）
    void SetSimulateWriteFailure(bool fail) noexcept { simulate_write_failure_ = fail; }

    int64_t ResetRevision() const;
    int SchemaVersion() const;

    // 与 ModernIME LearningStore / LearningSnapshot 无损互转
    std::vector<LearningEntry> ExportLearningEntries() const;
    std::shared_ptr<const LearningSnapshot> ToLearningSnapshot(
        size_t total_entry_limit = kMaxLearningEntries) const;
    bool ApplyLearningBatch(const std::vector<LearningEntry> &entries,
                            bool replace_all = false);

    // 兼容平面二进制文件序列化（支持 PersonalEngine.hpp 原生 14B WordMeta 与扩展格式无损导入）
    bool SaveToFile(const std::string &file_path) const;
    struct StoredVariant {
        std::string text;
        std::string raw_pinyin;
        std::string context_before;
        std::string context_after;
        WordMeta meta{};
    };

    bool LoadFromFile(const std::string &file_path);
    bool ImportFromFile(const std::string &file_path, bool merge = true);

    void Prune();
    size_t GetTotalEntries() const noexcept { return total_entries_; }

private:

    float MatchContextAffinity(std::string_view stored_context_before,
                               std::string_view query_context,
                               std::string_view text) const noexcept;

    void PruneContextVariants();
    bool EnsureMapCapacity(size_t required_extra_bytes = 0);
    bool EnsureMapCapacityLocked(size_t required_extra_bytes = 0);
    bool RebuildCompactIndex(std::string_view norm_key);

    EngineConfig config_;
    ClockFn clock_fn_;
    uint32_t manual_clock_offset_ = 0;
    bool sensitive_context_ = false;
    bool simulate_write_failure_ = false;
    std::string active_app_id_;
    std::string active_context_;

    // 内存双索引：
    // 1. 有序 B-Tree 索引 (支持 O(log N) 前缀游标扫描与精确查找，内层连续 vector 保证缓存局部性)
    std::map<std::string, std::vector<StoredVariant>, std::less<>> db_;
    // 2. 紧凑无分隔符拼音 -> 规范化键列表 (支持 "gnu"/"qudong" 等紧凑前缀/全拼 O(log N) 召回)
    std::map<std::string, std::vector<std::string>, std::less<>> compact_index_;
    size_t total_entries_ = 0;
    int64_t reset_revision_ = 0;
    bool migrated_from_sqlite_ = false;
    int64_t migrated_bin_size_ = 0;

    // LMDB 句柄
    std::shared_ptr<detail::LmdbEnvironment> lmdb_env_;
    std::filesystem::path lmdb_path_;
    MDB_env *env_ = nullptr;
    unsigned int words_dbi_ = 0;
    unsigned int meta_dbi_ = 0;
    bool lmdb_read_only_ = false;
    size_t last_lmdb_txn_id_ = 0;
    mutable std::mutex lmdb_mutex_;
};

} // namespace modernime::core

namespace modern_ime {
using WordMeta = modernime::core::WordMeta;
using Candidate = modernime::core::PersonalCandidate;
using EngineConfig = modernime::core::EngineConfig;
using PersonalEngine = modernime::core::PersonalEngine;
} // namespace modern_ime
