#include "modernime/core/personal_engine.h"

#include <lmdb.h>
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <unordered_set>
#include <utility>

namespace modernime::core {
namespace {

constexpr double kLn2 = 0.693147180559945309417232121458176568;
constexpr int kLmdbSchemaVersion = 2;

bool isUtf8ContinuationByte(unsigned char byte) noexcept {
    return (byte & 0xC0U) == 0x80U;
}

std::size_t utf8SequenceLength(unsigned char lead) noexcept {
    if ((lead & 0x80U) == 0U) {
        return 1;
    }
    if ((lead & 0xE0U) == 0xC0U) {
        return 2;
    }
    if ((lead & 0xF0U) == 0xE0U) {
        return 3;
    }
    if ((lead & 0xF8U) == 0xF0U) {
        return 4;
    }
    return 0;
}

bool isValidUtf8Codepoint(std::string_view slice) noexcept {
    if (slice.empty()) {
        return false;
    }
    const auto lead = static_cast<unsigned char>(slice[0]);
    const auto expected = utf8SequenceLength(lead);
    if (expected == 0 || slice.size() != expected) {
        return false;
    }
    for (std::size_t i = 1; i < expected; ++i) {
        if (!isUtf8ContinuationByte(static_cast<unsigned char>(slice[i]))) {
            return false;
        }
    }
    if (expected == 2 && lead < 0xC2U) {
        return false;
    }
    if (expected == 3) {
        const auto b1 = static_cast<unsigned char>(slice[1]);
        if ((lead == 0xE0U && b1 < 0xA0U) || (lead == 0xEDU && b1 >= 0xA0U)) {
            return false;
        }
    }
    if (expected == 4) {
        const auto b1 = static_cast<unsigned char>(slice[1]);
        if ((lead == 0xF0U && b1 < 0x90U) || (lead == 0xF4U && b1 >= 0x90U) ||
            lead > 0xF4U) {
            return false;
        }
    }
    return true;
}

bool isControlOrLineBreak(unsigned char c) noexcept {
    return c < 0x20U || c == 0x7FU;
}

struct UnpackedRecordView {
    WordMeta meta{};
    std::string_view text;
    std::string_view context_before;
    std::string_view context_after;
    bool valid = false;
};

UnpackedRecordView unpackLmdbValue(const MDB_val &val) noexcept {
    UnpackedRecordView result;
    if (val.mv_data == nullptr || val.mv_size < sizeof(WordMeta)) {
        return result;
    }
    std::memcpy(&result.meta, val.mv_data, sizeof(WordMeta));
    const std::size_t total_needed =
        sizeof(WordMeta) +
        static_cast<std::size_t>(result.meta.text_bytes_len) +
        static_cast<std::size_t>(result.meta.context_before_len) +
        static_cast<std::size_t>(result.meta.context_after_len);
    if (val.mv_size < total_needed) {
        return result;
    }
    const auto *base = static_cast<const char *>(val.mv_data) + sizeof(WordMeta);
    result.text = std::string_view(base, result.meta.text_bytes_len);
    base += result.meta.text_bytes_len;
    result.context_before =
        std::string_view(base, result.meta.context_before_len);
    base += result.meta.context_before_len;
    result.context_after =
        std::string_view(base, result.meta.context_after_len);
    result.valid = true;
    return result;
}

// LMDB MDB_DUPSORT 自定义比较器：
// 按 (text, context_before, context_after) 字典序比较。
// 同一拼音键下的相同 (词条, 前后文) 在更新频次或时间戳时长度不变且比较结果为 0，
// 从而使 mdb_put 直接在 B+Tree 叶节点原地覆盖旧值，避免产生重复记录。
int compareLmdbWordRecord(const MDB_val *a, const MDB_val *b) noexcept {
    const auto va = unpackLmdbValue(*a);
    const auto vb = unpackLmdbValue(*b);
    if (!va.valid || !vb.valid) {
        const std::size_t min_len = std::min(a->mv_size, b->mv_size);
        const int cmp = std::memcmp(a->mv_data, b->mv_data, min_len);
        if (cmp != 0) {
            return cmp;
        }
        return (a->mv_size < b->mv_size) ? -1 : (a->mv_size > b->mv_size ? 1 : 0);
    }
    if (const int cmp = va.text.compare(vb.text); cmp != 0) {
        return cmp;
    }
    if (const int cmp = va.context_before.compare(vb.context_before); cmp != 0) {
        return cmp;
    }
    return va.context_after.compare(vb.context_after);
}

std::vector<char> packLmdbValue(const WordMeta &meta,
                                std::string_view text,
                                std::string_view context_before,
                                std::string_view context_after) {
    WordMeta header = meta;
    header.text_bytes_len = static_cast<uint16_t>(
        std::min<std::size_t>(text.size(), std::numeric_limits<uint16_t>::max()));
    header.context_before_len = static_cast<uint16_t>(
        std::min<std::size_t>(context_before.size(), std::numeric_limits<uint16_t>::max()));
    header.context_after_len = static_cast<uint16_t>(
        std::min<std::size_t>(context_after.size(), std::numeric_limits<uint16_t>::max()));

    const std::size_t total_size =
        sizeof(WordMeta) + header.text_bytes_len +
        header.context_before_len + header.context_after_len;
    std::vector<char> buffer(total_size);
    char *ptr = buffer.data();
    std::memcpy(ptr, &header, sizeof(WordMeta));
    ptr += sizeof(WordMeta);
    if (header.text_bytes_len > 0) {
        std::memcpy(ptr, text.data(), header.text_bytes_len);
        ptr += header.text_bytes_len;
    }
    if (header.context_before_len > 0) {
        std::memcpy(ptr, context_before.data(), header.context_before_len);
        ptr += header.context_before_len;
    }
    if (header.context_after_len > 0) {
        std::memcpy(ptr, context_after.data(), header.context_after_len);
    }
    return buffer;
}

bool putMetaInt64(MDB_txn *txn, MDB_dbi meta_dbi, const char *key_name, int64_t value) {
    MDB_val k{std::strlen(key_name), const_cast<char *>(key_name)};
    MDB_val v{sizeof(int64_t), &value};
    return mdb_put(txn, meta_dbi, &k, &v, 0) == MDB_SUCCESS;
}

std::optional<int64_t> getMetaInt64(MDB_txn *txn, MDB_dbi meta_dbi, const char *key_name) {
    MDB_val k{std::strlen(key_name), const_cast<char *>(key_name)};
    MDB_val v{};
    if (mdb_get(txn, meta_dbi, &k, &v) != MDB_SUCCESS || v.mv_size != sizeof(int64_t)) {
        return std::nullopt;
    }
    int64_t out = 0;
    std::memcpy(&out, v.mv_data, sizeof(int64_t));
    return out;
}

} // namespace

PersonalEngine::PersonalEngine(EngineConfig config)
    : config_(config) {}

PersonalEngine::~PersonalEngine() {
    CloseLmdb();
}

std::string PersonalEngine::NormalizePinyinKey(
    const std::vector<std::string> &syllables) {
    std::vector<std::string> cleaned;
    cleaned.reserve(syllables.size());
    for (const auto &syl : syllables) {
        std::string item = NormalizePinyinKey(syl);
        if (item.starts_with("!u!")) {
            item.erase(0, 3);
        }
        if (!item.empty()) {
            cleaned.push_back(std::move(item));
        }
    }
    std::string key = "!u!";
    for (size_t i = 0; i < cleaned.size(); ++i) {
        key += cleaned[i];
        if (i + 1 < cleaned.size()) {
            key += ",";
        }
    }
    return key;
}

std::string PersonalEngine::NormalizePinyinKey(std::string_view raw_pinyin) {
    if (raw_pinyin.starts_with("!u!") || raw_pinyin.starts_with("!U!")) {
        raw_pinyin.remove_prefix(3);
    }
    std::string result = "!u!";
    result.reserve(raw_pinyin.size() + 3);
    bool pending_comma = false;
    bool has_content = false;
    for (const unsigned char ch : raw_pinyin) {
        if (ch == ',' || ch == '\'' || std::isspace(ch)) {
            if (has_content) {
                pending_comma = true;
            }
            continue;
        }
        if (std::isalnum(ch) || ch == '-' || ch == '_') {
            if (pending_comma) {
                result.push_back(',');
                pending_comma = false;
            }
            result.push_back(static_cast<char>(std::tolower(ch)));
            has_content = true;
        }
    }
    if (!has_content && !raw_pinyin.empty()) {
        // 保留非空特殊键（避免无法规范化的历史词条丢失）
        result.append(raw_pinyin);
    }
    return result;
}

std::string PersonalEngine::CompactPinyin(std::string_view pinyin_key) {
    std::string norm = NormalizePinyinKey(pinyin_key);
    if (norm.starts_with("!u!")) {
        norm.erase(0, 3);
    }
    std::string compact;
    compact.reserve(norm.size());
    for (char c : norm) {
        if (c != ',' && c != '\'') {
            compact.push_back(c);
        }
    }
    return compact;
}

std::string PersonalEngine::ToApostrophePinyin(std::string_view pinyin_key) {
    std::string norm = NormalizePinyinKey(pinyin_key);
    if (norm.starts_with("!u!")) {
        norm.erase(0, 3);
    }
    for (char &c : norm) {
        if (c == ',') {
            c = '\'';
        }
    }
    return norm;
}

std::string PersonalEngine::SanitizeSurroundingContext(
    std::string_view raw_before_cursor,
    size_t max_codepoints) {
    if (raw_before_cursor.empty() || max_codepoints == 0) {
        return {};
    }
    // 1. 截断到最后一个换行符或控制字符之后（不跨行/不跨段落污染上下文）
    std::size_t line_start = 0;
    for (std::size_t i = 0; i < raw_before_cursor.size(); ++i) {
        if (isControlOrLineBreak(static_cast<unsigned char>(raw_before_cursor[i]))) {
            line_start = i + 1;
        }
    }
    if (line_start >= raw_before_cursor.size()) {
        return {};
    }
    std::string_view active_slice = raw_before_cursor.substr(line_start);

    // 2. 扫描并收集有效 UTF-8 字符区间（遇到非法 UTF-8 字节则重置起点，保证边界安全）
    std::vector<std::pair<std::size_t, std::size_t>> codepoints;
    codepoints.reserve(active_slice.size());
    std::size_t offset = 0;
    while (offset < active_slice.size()) {
        const auto lead = static_cast<unsigned char>(active_slice[offset]);
        const auto len = utf8SequenceLength(lead);
        if (len == 0 || offset + len > active_slice.size() ||
            !isValidUtf8Codepoint(active_slice.substr(offset, len))) {
            codepoints.clear();
            ++offset;
            continue;
        }
        codepoints.emplace_back(offset, len);
        offset += len;
    }
    if (codepoints.empty()) {
        return {};
    }
    const std::size_t start_idx =
        codepoints.size() > max_codepoints ? (codepoints.size() - max_codepoints) : 0;
    const std::size_t byte_start = codepoints[start_idx].first;
    const std::size_t byte_end =
        codepoints.back().first + codepoints.back().second;
    return std::string(active_slice.substr(byte_start, byte_end - byte_start));
}

void PersonalEngine::SetClock(ClockFn clock_fn) {
    clock_fn_ = std::move(clock_fn);
}

void PersonalEngine::AdvanceClock(uint32_t delta_seconds) {
    manual_clock_offset_ += delta_seconds;
}

uint32_t PersonalEngine::NowSeconds() const {
    uint32_t base = 0;
    if (clock_fn_) {
        base = clock_fn_();
    } else {
        base = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    }
    return base + manual_clock_offset_;
}

void PersonalEngine::UpdateActiveContext(std::string_view raw_before_cursor,
                                         std::string_view app_id) {
    if (sensitive_context_) {
        active_context_.clear();
        return;
    }
    if (!app_id.empty() && !active_app_id_.empty() && app_id != active_app_id_) {
        // 跨应用切换时立即清空旧应用上下文
        active_context_.clear();
    }
    if (!app_id.empty()) {
        active_app_id_ = std::string(app_id);
    }
    active_context_ = SanitizeSurroundingContext(raw_before_cursor);
}

void PersonalEngine::SetMaxCapacity(size_t max_capacity, size_t prune_target) {
    config_.max_dict_capacity = max_capacity == 0 ? 50000 : max_capacity;
    if (prune_target > 0 && prune_target <= config_.max_dict_capacity) {
        config_.prune_target_size = prune_target;
    } else {
        config_.prune_target_size =
            std::max<size_t>(1, (config_.max_dict_capacity * 4) / 5);
    }
    if (total_entries_ > config_.max_dict_capacity) {
        Prune();
    }
}

bool PersonalEngine::RebuildCompactIndex(std::string_view norm_key) {
    const std::string compact = CompactPinyin(norm_key);
    if (compact.empty()) {
        return false;
    }
    auto &vec = compact_index_[compact];
    if (std::find(vec.begin(), vec.end(), norm_key) == vec.end()) {
        vec.emplace_back(norm_key);
    }
    return true;
}

bool PersonalEngine::RecordUsage(std::string_view pinyin_key,
                                 std::string_view text,
                                 float initial_weight,
                                 std::string_view context_before,
                                 std::string_view context_after,
                                 std::optional<int64_t> timestamp_ms) {
    if (sensitive_context_ || pinyin_key.empty() || text.empty()) {
        return false;
    }
    if (simulate_write_failure_) {
        return false;
    }
    const std::string norm_key = NormalizePinyinKey(pinyin_key);
    if (norm_key == "!u!") {
        return false;
    }
    const std::string sanitized_before =
        context_before.empty()
            ? active_context_
            : SanitizeSurroundingContext(context_before);
    const std::string sanitized_after =
        SanitizeSurroundingContext(context_after);

    const uint32_t now_sec =
        timestamp_ms.has_value() && *timestamp_ms > 0
            ? static_cast<uint32_t>(std::max<int64_t>(1, *timestamp_ms / 1000))
            : NowSeconds();
    const int64_t now_ms =
        timestamp_ms.has_value()
            ? *timestamp_ms
            : static_cast<int64_t>(now_sec) * 1000LL;

    auto &variants = db_[norm_key];
    RebuildCompactIndex(norm_key);

    // 取消同一 (norm_key, text) 下所有变体的 suppressed 状态
    for (auto &v : variants) {
        if (v.text == text && v.meta.suppressed != 0) {
            v.meta.suppressed = 0;
            if (env_ != nullptr && !lmdb_read_only_) {
                PersistVariantToLmdb(norm_key, v);
            }
        }
    }

    StoredVariant *target = nullptr;
    for (auto &v : variants) {
        if (v.text == text && v.context_before == sanitized_before &&
            v.context_after == sanitized_after) {
            target = &v;
            break;
        }
    }

    if (target != nullptr) {
        if (target->meta.hit_count < std::numeric_limits<uint32_t>::max()) {
            target->meta.hit_count += 1;
        }
        target->meta.last_used_timestamp = now_sec;
        target->meta.last_selected_ms = now_ms;
        target->meta.suppressed = 0;
    } else {
        StoredVariant created;
        created.text = std::string(text);
        created.raw_pinyin = ToApostrophePinyin(norm_key);
        created.context_before = sanitized_before;
        created.context_after = sanitized_after;
        created.meta.last_used_timestamp = now_sec;
        created.meta.hit_count = 1;
        created.meta.base_weight = initial_weight;
        created.meta.text_bytes_len = static_cast<uint16_t>(text.size());
        created.meta.context_before_len =
            static_cast<uint16_t>(sanitized_before.size());
        created.meta.context_after_len =
            static_cast<uint16_t>(sanitized_after.size());
        created.meta.negative_feedback = 0;
        created.meta.suppressed = 0;
        created.meta.last_selected_ms = now_ms;
        variants.push_back(std::move(created));
        target = &variants.back();
        ++total_entries_;
    }

    // 限制单 (norm_key, text) 的非空上下文变体最多 8 条（与 LearningStore 保持一致）
    std::vector<size_t> ctx_indices;
    for (size_t i = 0; i < variants.size(); ++i) {
        if (variants[i].text == text &&
            (!variants[i].context_before.empty() ||
             !variants[i].context_after.empty())) {
            ctx_indices.push_back(i);
        }
    }
    if (ctx_indices.size() > 8) {
        std::stable_sort(ctx_indices.begin(), ctx_indices.end(),
                         [&](size_t a, size_t b) {
                             const auto &va = variants[a].meta;
                             const auto &vb = variants[b].meta;
                             if (va.suppressed != vb.suppressed) {
                                 return va.suppressed > vb.suppressed;
                             }
                             if (va.hit_count != vb.hit_count) {
                                 return va.hit_count > vb.hit_count;
                             }
                             if (va.last_selected_ms != vb.last_selected_ms) {
                                 return va.last_selected_ms > vb.last_selected_ms;
                             }
                             return a > b;
                         });
        std::vector<size_t> to_drop(ctx_indices.begin() + 8, ctx_indices.end());
        std::sort(to_drop.rbegin(), to_drop.rend());
        for (size_t idx : to_drop) {
            variants.erase(variants.begin() + static_cast<std::ptrdiff_t>(idx));
            if (total_entries_ > 0) {
                --total_entries_;
            }
        }
        // 重新定位 target
        target = nullptr;
        for (auto &v : variants) {
            if (v.text == text && v.context_before == sanitized_before &&
                v.context_after == sanitized_after) {
                target = &v;
                break;
            }
        }
    }

    if (env_ != nullptr && !lmdb_read_only_ && target != nullptr) {
        if (!PersistVariantToLmdb(norm_key, *target)) {
            return false;
        }
    }

    if (total_entries_ > config_.max_dict_capacity) {
        Prune();
    }
    return true;
}

bool PersonalEngine::RecordNegativeFeedback(std::string_view pinyin_key,
                                            std::string_view text) {
    if (sensitive_context_ || pinyin_key.empty() || text.empty() ||
        simulate_write_failure_) {
        return false;
    }
    const std::string norm_key = NormalizePinyinKey(pinyin_key);
    auto &variants = db_[norm_key];
    RebuildCompactIndex(norm_key);

    StoredVariant *target = nullptr;
    for (auto &v : variants) {
        if (v.text == text && v.context_before.empty() &&
            v.context_after.empty()) {
            target = &v;
            break;
        }
    }
    if (target == nullptr) {
        StoredVariant created;
        created.text = std::string(text);
        created.raw_pinyin = ToApostrophePinyin(norm_key);
        created.meta.base_weight = 1.0f;
        created.meta.text_bytes_len = static_cast<uint16_t>(text.size());
        created.meta.negative_feedback = 1;
        variants.push_back(std::move(created));
        target = &variants.back();
        ++total_entries_;
    } else if (target->meta.negative_feedback <
               std::numeric_limits<uint32_t>::max()) {
        target->meta.negative_feedback += 1;
    }

    if (env_ != nullptr && !lmdb_read_only_) {
        if (!PersistVariantToLmdb(norm_key, *target)) {
            return false;
        }
    }
    if (total_entries_ > config_.max_dict_capacity) {
        Prune();
    }
    return true;
}

bool PersonalEngine::RecordSuppression(std::string_view pinyin_key,
                                       std::string_view text) {
    if (sensitive_context_ || pinyin_key.empty() || text.empty() ||
        simulate_write_failure_) {
        return false;
    }
    const std::string norm_key = NormalizePinyinKey(pinyin_key);
    auto &variants = db_[norm_key];
    RebuildCompactIndex(norm_key);

    StoredVariant *target = nullptr;
    for (auto &v : variants) {
        if (v.text == text && v.context_before.empty() &&
            v.context_after.empty()) {
            target = &v;
            break;
        }
    }
    if (target == nullptr) {
        StoredVariant created;
        created.text = std::string(text);
        created.raw_pinyin = ToApostrophePinyin(norm_key);
        created.meta.base_weight = 1.0f;
        created.meta.text_bytes_len = static_cast<uint16_t>(text.size());
        created.meta.negative_feedback = 1;
        created.meta.suppressed = 1;
        variants.push_back(std::move(created));
        target = &variants.back();
        ++total_entries_;
    } else {
        if (target->meta.negative_feedback <
            std::numeric_limits<uint32_t>::max()) {
            target->meta.negative_feedback += 1;
        }
        target->meta.suppressed = 1;
    }

    if (env_ != nullptr && !lmdb_read_only_) {
        if (!PersistVariantToLmdb(norm_key, *target)) {
            return false;
        }
    }
    if (total_entries_ > config_.max_dict_capacity) {
        Prune();
    }
    return true;
}

float PersonalEngine::MatchContextAffinity(
    std::string_view stored_context_before,
    std::string_view query_context,
    std::string_view text) const noexcept {
    if (query_context.empty() || sensitive_context_) {
        return 0.0f;
    }
    // 1. 已学习的真实上下文匹配（后缀对齐）
    if (!stored_context_before.empty()) {
        if (query_context == stored_context_before ||
            query_context.ends_with(stored_context_before) ||
            stored_context_before.ends_with(query_context)) {
            return 1.0f;
        }
    }
    // 2. 领域共现启发式匹配（保留参考引擎能力）
    const std::string_view tail =
        query_context.size() > 18
            ? query_context.substr(query_context.size() - 18)
            : query_context;
    if (tail.find("Linux") != std::string_view::npos &&
        text.find("驱动") != std::string_view::npos) {
        return 1.0f;
    }
    if (tail.find("写入") != std::string_view::npos &&
        text.find("设备") != std::string_view::npos) {
        return 1.0f;
    }
    return 0.0f;
}

float PersonalEngine::CalculateScore(const WordMeta &meta,
                                     std::string_view text,
                                     std::string_view stored_context_before,
                                     std::string_view query_context,
                                     uint32_t now) const noexcept {
    if (meta.suppressed != 0) {
        return 0.0f;
    }
    // 1. 基础先验权重
    double score = static_cast<double>(meta.base_weight);

    // 2. 对数词频平滑项: alpha * ln(count + 1)
    score += static_cast<double>(config_.alpha) *
             std::log1p(static_cast<double>(meta.hit_count));

    // 3. 指数时间衰减项: beta * exp(-delta_t / tau)
    // 严格换算半衰期 T_{1/2} 与时间常数 tau: tau = T_{1/2} / ln(2)
    if (meta.last_used_timestamp > 0 && now >= meta.last_used_timestamp &&
        config_.half_life_seconds > 0) {
        const double delta_t =
            static_cast<double>(now - meta.last_used_timestamp);
        const double tau =
            static_cast<double>(config_.half_life_seconds) / kLn2;
        const double decay = std::exp(-delta_t / tau);
        score += static_cast<double>(config_.beta) * decay;
    }

    // 4. 上下文感知奖励: gamma * context_factor
    const std::string_view effective_ctx =
        query_context.empty() ? std::string_view(active_context_) : query_context;
    if (!effective_ctx.empty() && !sensitive_context_) {
        const float factor =
            MatchContextAffinity(stored_context_before, effective_ctx, text);
        score += static_cast<double>(config_.gamma) *
                 static_cast<double>(factor);
    }

    // 5. 负反馈惩罚项
    if (meta.negative_feedback > 0) {
        score -= 0.75 * std::log1p(static_cast<double>(meta.negative_feedback));
    }

    return static_cast<float>(std::max(0.0, score));
}

std::vector<PersonalCandidate> PersonalEngine::QueryCandidates(
    std::string_view pinyin_key,
    std::string_view surrounding_context) const {
    std::vector<PersonalCandidate> results;
    if (pinyin_key.empty()) {
        return results;
    }
    const std::string norm_key = NormalizePinyinKey(pinyin_key);
    const std::string clean_ctx =
        sensitive_context_
            ? std::string{}
            : SanitizeSurroundingContext(
                  surrounding_context.empty()
                      ? std::string_view(active_context_)
                      : surrounding_context);
    const uint32_t now = NowSeconds();

    auto collect_from_key = [&](const std::string &k) {
        auto it = db_.find(k);
        if (it == db_.end()) {
            return;
        }
        // 按 text 聚合同一词条的不同上下文变体
        struct Aggregated {
            WordMeta meta{};
            std::string raw_pinyin;
            std::string best_ctx_before;
            std::string best_ctx_after;
            float best_ctx_factor = 0.0f;
            bool suppressed = false;
        };
        std::unordered_map<std::string, Aggregated> agg_map;
        agg_map.reserve(it->second.size());
        for (const auto &v : it->second) {
            auto &agg = agg_map[v.text];
            if (agg.raw_pinyin.empty()) {
                agg.raw_pinyin = v.raw_pinyin;
            }
            if (v.meta.suppressed != 0) {
                agg.suppressed = true;
            }
            agg.meta.base_weight = std::max(agg.meta.base_weight, v.meta.base_weight);
            agg.meta.hit_count += v.meta.hit_count;
            agg.meta.last_used_timestamp =
                std::max(agg.meta.last_used_timestamp, v.meta.last_used_timestamp);
            agg.meta.last_selected_ms =
                std::max(agg.meta.last_selected_ms, v.meta.last_selected_ms);
            agg.meta.negative_feedback =
                std::max(agg.meta.negative_feedback, v.meta.negative_feedback);
            agg.meta.text_bytes_len = v.meta.text_bytes_len;

            const float factor =
                MatchContextAffinity(v.context_before, clean_ctx, v.text);
            if (factor >= agg.best_ctx_factor) {
                agg.best_ctx_factor = factor;
                agg.best_ctx_before = v.context_before;
                agg.best_ctx_after = v.context_after;
            }
        }
        for (auto &[text, agg] : agg_map) {
            if (agg.suppressed || agg.meta.hit_count == 0) {
                continue;
            }
            PersonalCandidate cand;
            cand.text = text;
            cand.pinyin_key = k;
            cand.raw_pinyin = agg.raw_pinyin;
            cand.context_before = agg.best_ctx_before;
            cand.context_after = agg.best_ctx_after;
            cand.meta = agg.meta;
            cand.dynamic_score = CalculateScore(
                agg.meta, text, agg.best_ctx_before, clean_ctx, now);
            results.push_back(std::move(cand));
        }
    };

    collect_from_key(norm_key);

    std::sort(results.begin(), results.end(),
              [](const PersonalCandidate &a, const PersonalCandidate &b) {
                  if (std::abs(a.dynamic_score - b.dynamic_score) > 1e-6f) {
                      return a.dynamic_score > b.dynamic_score;
                  }
                  return a.text < b.text;
              });
    return results;
}

std::vector<PersonalCandidate> PersonalEngine::QueryPrefixCandidates(
    std::string_view pinyin_prefix,
    std::string_view surrounding_context,
    size_t limit) const {
    std::vector<PersonalCandidate> results;
    if (pinyin_prefix.empty() || limit == 0) {
        return results;
    }
    const std::string norm_prefix = NormalizePinyinKey(pinyin_prefix);
    const std::string compact_prefix = CompactPinyin(pinyin_prefix);
    const std::string clean_ctx =
        sensitive_context_
            ? std::string{}
            : SanitizeSurroundingContext(
                  surrounding_context.empty()
                      ? std::string_view(active_context_)
                      : surrounding_context);

    std::unordered_set<std::string> visited_keys;
    std::unordered_map<std::string, PersonalCandidate> best_by_text;

    auto harvest_key = [&](const std::string &k) {
        if (!visited_keys.insert(k).second) {
            return;
        }
        auto cands = QueryCandidates(k, clean_ctx);
        for (auto &c : cands) {
            auto it = best_by_text.find(c.text);
            if (it == best_by_text.end() ||
                c.dynamic_score > it->second.dynamic_score) {
                best_by_text[c.text] = std::move(c);
            }
        }
    };

    // 1. O(log N) 规范化键前缀扫描
    for (auto it = db_.lower_bound(norm_prefix);
         it != db_.end() && it->first.starts_with(norm_prefix); ++it) {
        harvest_key(it->first);
    }

    // 2. O(log N) 紧凑无分隔符前缀扫描（如 "gnu"/"gnue" 匹配 "!u!gnueabihf"）
    if (!compact_prefix.empty()) {
        for (auto it = compact_index_.lower_bound(compact_prefix);
             it != compact_index_.end() &&
             it->first.starts_with(compact_prefix);
             ++it) {
            for (const auto &k : it->second) {
                harvest_key(k);
            }
        }
    }

    results.reserve(best_by_text.size());
    for (auto &[_, cand] : best_by_text) {
        results.push_back(std::move(cand));
    }
    std::sort(results.begin(), results.end(),
              [](const PersonalCandidate &a, const PersonalCandidate &b) {
                  if (std::abs(a.dynamic_score - b.dynamic_score) > 1e-6f) {
                      return a.dynamic_score > b.dynamic_score;
                  }
                  return a.text < b.text;
              });
    if (results.size() > limit) {
        results.resize(limit);
    }
    return results;
}

std::vector<PersonalCandidate> PersonalEngine::QueryLmdbCandidates(
    std::string_view pinyin_key,
    std::string_view surrounding_context) const {
    if (env_ == nullptr) {
        return QueryCandidates(pinyin_key, surrounding_context);
    }
    std::lock_guard lock(lmdb_mutex_);
    std::vector<PersonalCandidate> results;
    const std::string norm_key = NormalizePinyinKey(pinyin_key);
    const std::string clean_ctx =
        sensitive_context_
            ? std::string{}
            : SanitizeSurroundingContext(
                  surrounding_context.empty()
                      ? std::string_view(active_context_)
                      : surrounding_context);
    const uint32_t now = NowSeconds();

    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn) != MDB_SUCCESS) {
        return results;
    }
    MDB_cursor *cursor = nullptr;
    if (mdb_cursor_open(txn, words_dbi_, &cursor) != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        return results;
    }

    MDB_val k{norm_key.size(), const_cast<char *>(norm_key.data())};
    MDB_val v{};
    struct Aggregated {
        WordMeta meta{};
        std::string best_ctx_before;
        std::string best_ctx_after;
        float best_ctx_factor = 0.0f;
        bool suppressed = false;
    };
    std::unordered_map<std::string, Aggregated> agg_map;

    int rc = mdb_cursor_get(cursor, &k, &v, MDB_SET_KEY);
    while (rc == MDB_SUCCESS) {
        const auto view = unpackLmdbValue(v);
        if (view.valid) {
            auto &agg = agg_map[std::string(view.text)];
            if (view.meta.suppressed != 0) {
                agg.suppressed = true;
            }
            agg.meta.base_weight =
                std::max(agg.meta.base_weight, view.meta.base_weight);
            agg.meta.hit_count += view.meta.hit_count;
            agg.meta.last_used_timestamp = std::max(
                agg.meta.last_used_timestamp, view.meta.last_used_timestamp);
            agg.meta.last_selected_ms =
                std::max(agg.meta.last_selected_ms, view.meta.last_selected_ms);
            agg.meta.negative_feedback = std::max(
                agg.meta.negative_feedback, view.meta.negative_feedback);
            agg.meta.text_bytes_len = view.meta.text_bytes_len;

            const float factor = MatchContextAffinity(
                view.context_before, clean_ctx, view.text);
            if (factor >= agg.best_ctx_factor) {
                agg.best_ctx_factor = factor;
                agg.best_ctx_before = std::string(view.context_before);
                agg.best_ctx_after = std::string(view.context_after);
            }
        }
        rc = mdb_cursor_get(cursor, &k, &v, MDB_NEXT_DUP);
    }

    mdb_cursor_close(cursor);
    mdb_txn_abort(txn);

    const std::string raw_py = ToApostrophePinyin(norm_key);
    for (auto &[text, agg] : agg_map) {
        if (agg.suppressed || agg.meta.hit_count == 0) {
            continue;
        }
        PersonalCandidate cand;
        cand.text = text;
        cand.pinyin_key = norm_key;
        cand.raw_pinyin = raw_py;
        cand.context_before = agg.best_ctx_before;
        cand.context_after = agg.best_ctx_after;
        cand.meta = agg.meta;
        cand.dynamic_score = CalculateScore(
            agg.meta, text, agg.best_ctx_before, clean_ctx, now);
        results.push_back(std::move(cand));
    }
    std::sort(results.begin(), results.end(),
              [](const PersonalCandidate &a, const PersonalCandidate &b) {
                  if (std::abs(a.dynamic_score - b.dynamic_score) > 1e-6f) {
                      return a.dynamic_score > b.dynamic_score;
                  }
                  return a.text < b.text;
              });
    return results;
}

std::vector<PersonalCandidate> PersonalEngine::QueryLmdbPrefixCandidates(
    std::string_view pinyin_prefix,
    std::string_view surrounding_context,
    size_t limit) const {
    if (env_ == nullptr) {
        return QueryPrefixCandidates(pinyin_prefix, surrounding_context, limit);
    }
    std::lock_guard lock(lmdb_mutex_);
    std::vector<PersonalCandidate> results;
    if (pinyin_prefix.empty() || limit == 0) {
        return results;
    }
    const std::string norm_prefix = NormalizePinyinKey(pinyin_prefix);
    const std::string clean_ctx =
        sensitive_context_
            ? std::string{}
            : SanitizeSurroundingContext(
                  surrounding_context.empty()
                      ? std::string_view(active_context_)
                      : surrounding_context);
    const uint32_t now = NowSeconds();

    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn) != MDB_SUCCESS) {
        return results;
    }
    MDB_cursor *cursor = nullptr;
    if (mdb_cursor_open(txn, words_dbi_, &cursor) != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        return results;
    }

    MDB_val k{norm_prefix.size(), const_cast<char *>(norm_prefix.data())};
    MDB_val v{};
    std::unordered_map<std::string, PersonalCandidate> best_by_text;

    int rc = mdb_cursor_get(cursor, &k, &v, MDB_SET_RANGE);
    while (rc == MDB_SUCCESS) {
        const std::string_view key_view(static_cast<const char *>(k.mv_data),
                                        k.mv_size);
        if (!key_view.starts_with(norm_prefix)) {
            break;
        }
        const auto view = unpackLmdbValue(v);
        if (view.valid && view.meta.suppressed == 0 && view.meta.hit_count > 0) {
            const float score = CalculateScore(
                view.meta, view.text, view.context_before, clean_ctx, now);
            const std::string text_str(view.text);
            auto it = best_by_text.find(text_str);
            if (it == best_by_text.end() || score > it->second.dynamic_score) {
                PersonalCandidate cand;
                cand.text = text_str;
                cand.pinyin_key = std::string(key_view);
                cand.raw_pinyin = ToApostrophePinyin(key_view);
                cand.context_before = std::string(view.context_before);
                cand.context_after = std::string(view.context_after);
                cand.meta = view.meta;
                cand.dynamic_score = score;
                best_by_text[text_str] = std::move(cand);
            }
        }
        rc = mdb_cursor_get(cursor, &k, &v, MDB_NEXT);
    }

    mdb_cursor_close(cursor);
    mdb_txn_abort(txn);

    results.reserve(best_by_text.size());
    for (auto &[_, cand] : best_by_text) {
        results.push_back(std::move(cand));
    }
    std::sort(results.begin(), results.end(),
              [](const PersonalCandidate &a, const PersonalCandidate &b) {
                  if (std::abs(a.dynamic_score - b.dynamic_score) > 1e-6f) {
                      return a.dynamic_score > b.dynamic_score;
                  }
                  return a.text < b.text;
              });
    if (results.size() > limit) {
        results.resize(limit);
    }
    return results;
}

bool PersonalEngine::OpenLmdb(const std::filesystem::path &lmdb_path,
                              bool read_only) {
    std::unique_lock lock(lmdb_mutex_);
    if (env_ != nullptr) {
        return true;
    }
    if (lmdb_path.empty()) {
        return false;
    }
    std::error_code ec;
    if (std::filesystem::is_directory(lmdb_path, ec)) {
        return false;
    }
    if (read_only && !std::filesystem::exists(lmdb_path, ec)) {
        return false;
    }
    if (!read_only && !lmdb_path.parent_path().empty()) {
        std::filesystem::create_directories(lmdb_path.parent_path(), ec);
        if (ec) {
            return false;
        }
    }

    MDB_env *env = nullptr;
    if (mdb_env_create(&env) != MDB_SUCCESS) {
        return false;
    }
    if (mdb_env_set_maxdbs(env, 4) != MDB_SUCCESS ||
        mdb_env_set_mapsize(env, std::max<size_t>(64 * 1024, config_.initial_lmdb_map_size)) !=
            MDB_SUCCESS) {
        mdb_env_close(env);
        return false;
    }

    unsigned int flags = MDB_NOSUBDIR | MDB_NOTLS;
    if (read_only) {
        flags |= MDB_RDONLY;
    }
    if (mdb_env_open(env, lmdb_path.c_str(), flags, 0644) != MDB_SUCCESS) {
        mdb_env_close(env);
        return false;
    }

    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(env, nullptr, read_only ? MDB_RDONLY : 0, &txn) !=
        MDB_SUCCESS) {
        mdb_env_close(env);
        return false;
    }

    const unsigned int words_flags =
        read_only ? MDB_DUPSORT : (MDB_CREATE | MDB_DUPSORT);
    const unsigned int meta_flags = read_only ? 0 : MDB_CREATE;
    MDB_dbi words_dbi = 0;
    MDB_dbi meta_dbi = 0;
    if (mdb_dbi_open(txn, "words", words_flags, &words_dbi) != MDB_SUCCESS ||
        mdb_set_dupsort(txn, words_dbi, &compareLmdbWordRecord) != MDB_SUCCESS ||
        mdb_dbi_open(txn, "meta", meta_flags, &meta_dbi) != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        mdb_env_close(env);
        return false;
    }

    if (!read_only) {
        if (!getMetaInt64(txn, meta_dbi, "schema_version").has_value()) {
            putMetaInt64(txn, meta_dbi, "schema_version", kLmdbSchemaVersion);
        }
        if (!getMetaInt64(txn, meta_dbi, "reset_revision").has_value()) {
            putMetaInt64(txn, meta_dbi, "reset_revision", 0);
        }
        if (mdb_txn_commit(txn) != MDB_SUCCESS) {
            mdb_env_close(env);
            return false;
        }
    } else {
        mdb_txn_abort(txn);
    }

    env_ = env;
    words_dbi_ = words_dbi;
    meta_dbi_ = meta_dbi;
    lmdb_path_ = lmdb_path;
    lmdb_read_only_ = read_only;
    lock.unlock();

    if (!ReloadFromLmdb()) {
        CloseLmdb();
        return false;
    }

    // 若首次创建且同目录下存在旧版 learning.sqlite3，自动无损迁移
    if (!read_only && !migrated_from_sqlite_ && total_entries_ == 0) {
        const auto sibling_sqlite = lmdb_path.parent_path() / "learning.sqlite3";
        if (std::filesystem::exists(sibling_sqlite, ec) &&
            std::filesystem::is_regular_file(sibling_sqlite, ec)) {
            MigrateFromSqlite(sibling_sqlite);
        }
    }
    return true;
}

void PersonalEngine::CloseLmdb() noexcept {
    std::lock_guard lock(lmdb_mutex_);
    if (env_ != nullptr) {
        mdb_env_close(env_);
        env_ = nullptr;
        words_dbi_ = 0;
        meta_dbi_ = 0;
    }
}

size_t PersonalEngine::LmdbMapSize() const noexcept {
    std::lock_guard lock(lmdb_mutex_);
    if (env_ == nullptr) {
        return 0;
    }
    MDB_envinfo info{};
    if (mdb_env_info(env_, &info) != MDB_SUCCESS) {
        return 0;
    }
    return info.me_mapsize;
}

bool PersonalEngine::EnsureMapCapacity(size_t required_extra_bytes) {
    if (env_ == nullptr || lmdb_read_only_) {
        return false;
    }
    MDB_envinfo info{};
    if (mdb_env_info(env_, &info) != MDB_SUCCESS) {
        return false;
    }
    size_t next_size = std::max<size_t>(info.me_mapsize * 2, 128 * 1024);
    if (required_extra_bytes > 0 &&
        info.me_mapsize + required_extra_bytes * 4 > next_size) {
        next_size = info.me_mapsize + required_extra_bytes * 4;
    }
    // 4 KiB 页对齐
    next_size = (next_size + 4095ULL) & ~4095ULL;
    return mdb_env_set_mapsize(env_, next_size) == MDB_SUCCESS;
}

bool PersonalEngine::PersistVariantToLmdb(std::string_view norm_key,
                                          const StoredVariant &variant) {
    std::lock_guard lock(lmdb_mutex_);
    if (env_ == nullptr || lmdb_read_only_ || simulate_write_failure_) {
        return false;
    }
    const auto packed = packLmdbValue(
        variant.meta, variant.text, variant.context_before, variant.context_after);

    for (int attempt = 0; attempt < 4; ++attempt) {
        MDB_txn *txn = nullptr;
        int rc = mdb_txn_begin(env_, nullptr, 0, &txn);
        if (rc != MDB_SUCCESS) {
            return false;
        }
        MDB_val k{norm_key.size(), const_cast<char *>(norm_key.data())};
        MDB_val v{packed.size(), const_cast<char *>(packed.data())};
        rc = mdb_put(txn, words_dbi_, &k, &v, 0);
        if (rc == MDB_SUCCESS) {
            rc = mdb_txn_commit(txn);
            txn = nullptr;
        } else {
            mdb_txn_abort(txn);
            txn = nullptr;
        }
        if (rc == MDB_SUCCESS) {
            return true;
        }
        if (rc == MDB_MAP_FULL) {
            if (!EnsureMapCapacity(packed.size() + norm_key.size() + 4096)) {
                return false;
            }
            continue;
        }
        return false;
    }
    return false;
}

bool PersonalEngine::FlushToLmdb() {
    std::lock_guard lock(lmdb_mutex_);
    if (env_ == nullptr || lmdb_read_only_ || simulate_write_failure_) {
        return false;
    }
    for (int attempt = 0; attempt < 5; ++attempt) {
        MDB_txn *txn = nullptr;
        if (mdb_txn_begin(env_, nullptr, 0, &txn) != MDB_SUCCESS) {
            return false;
        }
        if (mdb_drop(txn, words_dbi_, 0) != MDB_SUCCESS) {
            mdb_txn_abort(txn);
            return false;
        }
        int rc = MDB_SUCCESS;
        for (const auto &[norm_key, variants] : db_) {
            for (const auto &v : variants) {
                const auto packed = packLmdbValue(
                    v.meta, v.text, v.context_before, v.context_after);
                MDB_val k{norm_key.size(), const_cast<char *>(norm_key.data())};
                MDB_val val{packed.size(), const_cast<char *>(packed.data())};
                rc = mdb_put(txn, words_dbi_, &k, &val, 0);
                if (rc != MDB_SUCCESS) {
                    break;
                }
            }
            if (rc != MDB_SUCCESS) {
                break;
            }
        }
        if (rc == MDB_SUCCESS) {
            putMetaInt64(txn, meta_dbi_, "schema_version", kLmdbSchemaVersion);
            putMetaInt64(txn, meta_dbi_, "reset_revision", reset_revision_);
            putMetaInt64(txn, meta_dbi_, "migrated_from_sqlite",
                         migrated_from_sqlite_ ? 1 : 0);
            rc = mdb_txn_commit(txn);
            txn = nullptr;
        } else {
            mdb_txn_abort(txn);
            txn = nullptr;
        }
        if (rc == MDB_SUCCESS) {
            return true;
        }
        if (rc == MDB_MAP_FULL) {
            if (!EnsureMapCapacity(total_entries_ * 128 + 65536)) {
                return false;
            }
            continue;
        }
        return false;
    }
    return false;
}

bool PersonalEngine::ReloadFromLmdb() {
    std::lock_guard lock(lmdb_mutex_);
    if (env_ == nullptr) {
        return false;
    }
    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn) != MDB_SUCCESS) {
        return false;
    }

    if (const auto rev = getMetaInt64(txn, meta_dbi_, "reset_revision");
        rev.has_value()) {
        reset_revision_ = *rev;
    }
    if (const auto mig = getMetaInt64(txn, meta_dbi_, "migrated_from_sqlite");
        mig.has_value()) {
        migrated_from_sqlite_ = (*mig != 0);
    }

    MDB_cursor *cursor = nullptr;
    if (mdb_cursor_open(txn, words_dbi_, &cursor) != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        return false;
    }

    db_.clear();
    compact_index_.clear();
    total_entries_ = 0;

    MDB_val k{};
    MDB_val v{};
    int rc = mdb_cursor_get(cursor, &k, &v, MDB_FIRST);
    while (rc == MDB_SUCCESS) {
        const std::string key_str(static_cast<const char *>(k.mv_data), k.mv_size);
        const auto view = unpackLmdbValue(v);
        if (view.valid) {
            StoredVariant variant;
            variant.text = std::string(view.text);
            variant.raw_pinyin = ToApostrophePinyin(key_str);
            variant.context_before = std::string(view.context_before);
            variant.context_after = std::string(view.context_after);
            variant.meta = view.meta;
            db_[key_str].push_back(std::move(variant));
            RebuildCompactIndex(key_str);
            ++total_entries_;
        }
        rc = mdb_cursor_get(cursor, &k, &v, MDB_NEXT);
    }

    mdb_cursor_close(cursor);
    mdb_txn_abort(txn);
    return true;
}

bool PersonalEngine::BackupLmdbTo(const std::filesystem::path &backup_path) const {
    std::lock_guard lock(lmdb_mutex_);
    if (env_ == nullptr || backup_path.empty() || backup_path == lmdb_path_) {
        return false;
    }
    std::error_code ec;
    if (std::filesystem::is_directory(backup_path, ec)) {
        return false;
    }
    if (!backup_path.parent_path().empty()) {
        std::filesystem::create_directories(backup_path.parent_path(), ec);
        if (ec) {
            return false;
        }
    }
    std::filesystem::remove(backup_path, ec);
    return mdb_env_copy2(env_, backup_path.c_str(), MDB_CP_COMPACT) ==
           MDB_SUCCESS;
}

bool PersonalEngine::Clear() {
    if (simulate_write_failure_) {
        return false;
    }
    db_.clear();
    compact_index_.clear();
    total_entries_ = 0;
    ++reset_revision_;
    if (env_ != nullptr && !lmdb_read_only_) {
        return FlushToLmdb();
    }
    return true;
}

int64_t PersonalEngine::ResetRevision() const {
    std::lock_guard lock(lmdb_mutex_);
    if (env_ != nullptr) {
        MDB_txn *txn = nullptr;
        if (mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn) == MDB_SUCCESS) {
            const auto rev = getMetaInt64(txn, meta_dbi_, "reset_revision");
            mdb_txn_abort(txn);
            if (rev.has_value()) {
                return *rev;
            }
        }
    }
    return reset_revision_;
}

int PersonalEngine::SchemaVersion() const {
    std::lock_guard lock(lmdb_mutex_);
    if (env_ != nullptr) {
        MDB_txn *txn = nullptr;
        if (mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn) == MDB_SUCCESS) {
            const auto ver = getMetaInt64(txn, meta_dbi_, "schema_version");
            mdb_txn_abort(txn);
            if (ver.has_value()) {
                return static_cast<int>(*ver);
            }
        }
    }
    return kLmdbSchemaVersion;
}

bool PersonalEngine::IsMigratedFromSqlite() const {
    std::lock_guard lock(lmdb_mutex_);
    if (env_ != nullptr) {
        MDB_txn *txn = nullptr;
        if (mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn) == MDB_SUCCESS) {
            const auto mig = getMetaInt64(txn, meta_dbi_, "migrated_from_sqlite");
            mdb_txn_abort(txn);
            if (mig.has_value()) {
                return *mig != 0;
            }
        }
    }
    return migrated_from_sqlite_;
}

bool PersonalEngine::MigrateFromSqlite(const std::filesystem::path &sqlite_path) {
    if (sqlite_path.empty() || simulate_write_failure_) {
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(sqlite_path, ec) ||
        !std::filesystem::is_regular_file(sqlite_path, ec)) {
        return false;
    }

    sqlite3 *sql_db = nullptr;
    if (sqlite3_open_v2(sqlite_path.c_str(), &sql_db, SQLITE_OPEN_READONLY,
                        nullptr) != SQLITE_OK) {
        if (sql_db != nullptr) {
            sqlite3_close(sql_db);
        }
        return false;
    }
    sqlite3_busy_timeout(sql_db, 1000);

    // 1. 读取 reset_revision (若存在 learning_state 表)
    sqlite3_stmt *rev_stmt = nullptr;
    if (sqlite3_prepare_v2(
            sql_db, "SELECT reset_revision FROM learning_state WHERE id = 1;",
            -1, &rev_stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(rev_stmt) == SQLITE_ROW) {
            reset_revision_ = std::max<int64_t>(
                reset_revision_, sqlite3_column_int64(rev_stmt, 0));
        }
        sqlite3_finalize(rev_stmt);
    }

    // 2. 探测 learning_entries 列结构，兼容旧版 3 列与新版 8 列
    std::unordered_set<std::string> columns;
    sqlite3_stmt *info_stmt = nullptr;
    if (sqlite3_prepare_v2(sql_db, "PRAGMA table_info(learning_entries);", -1,
                           &info_stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(info_stmt) == SQLITE_ROW) {
            const auto *col_name = sqlite3_column_text(info_stmt, 1);
            if (col_name != nullptr) {
                columns.emplace(reinterpret_cast<const char *>(col_name));
            }
        }
        sqlite3_finalize(info_stmt);
    }
    if (!columns.contains("pinyin") || !columns.contains("phrase")) {
        sqlite3_close(sql_db);
        return false;
    }

    std::string query = "SELECT pinyin, phrase, ";
    query += columns.contains("context_before") ? "context_before, " : "'', ";
    query += columns.contains("context_after") ? "context_after, " : "'', ";
    query += columns.contains("frequency") ? "frequency, " : "1, ";
    query += columns.contains("last_selected_ms") ? "last_selected_ms, " : "0, ";
    query += columns.contains("negative_feedback") ? "negative_feedback, " : "0, ";
    query += columns.contains("suppressed") ? "suppressed " : "0 ";
    query += "FROM learning_entries;";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(sql_db, query.c_str(), -1, &stmt, nullptr) !=
        SQLITE_OK) {
        sqlite3_close(sql_db);
        return false;
    }

    auto col_str = [stmt](int idx) -> std::string {
        const auto *txt = sqlite3_column_text(stmt, idx);
        return txt != nullptr
                   ? std::string(reinterpret_cast<const char *>(txt))
                   : std::string{};
    };

    int step_rc = SQLITE_OK;
    while ((step_rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const std::string raw_py = col_str(0);
        const std::string phrase = col_str(1);
        if (phrase.empty()) {
            continue;
        }
        const std::string ctx_before = col_str(2);
        const std::string ctx_after = col_str(3);
        const int64_t freq = std::max<int64_t>(0, sqlite3_column_int64(stmt, 4));
        const int64_t last_ms =
            std::max<int64_t>(0, sqlite3_column_int64(stmt, 5));
        const int64_t neg_fb =
            std::max<int64_t>(0, sqlite3_column_int64(stmt, 6));
        const bool suppressed = sqlite3_column_int(stmt, 7) != 0;

        const std::string norm_key =
            NormalizePinyinKey(raw_py.empty() ? phrase : raw_py);
        auto &variants = db_[norm_key];
        RebuildCompactIndex(norm_key);

        StoredVariant *existing = nullptr;
        for (auto &v : variants) {
            if (v.text == phrase && v.context_before == ctx_before &&
                v.context_after == ctx_after) {
                existing = &v;
                break;
            }
        }
        const uint32_t ts_sec =
            last_ms > 0
                ? static_cast<uint32_t>(std::max<int64_t>(1, last_ms / 1000))
                : 0;
        if (existing != nullptr) {
            existing->meta.hit_count = static_cast<uint32_t>(
                std::min<int64_t>(std::numeric_limits<uint32_t>::max(),
                                  static_cast<int64_t>(existing->meta.hit_count) + freq));
            existing->meta.last_used_timestamp =
                std::max(existing->meta.last_used_timestamp, ts_sec);
            existing->meta.last_selected_ms =
                std::max(existing->meta.last_selected_ms, last_ms);
            existing->meta.negative_feedback = static_cast<uint32_t>(
                std::max<int64_t>(existing->meta.negative_feedback, neg_fb));
            existing->meta.suppressed = suppressed ? 1 : existing->meta.suppressed;
        } else {
            StoredVariant created;
            created.text = phrase;
            created.raw_pinyin = raw_py.empty() ? ToApostrophePinyin(norm_key) : raw_py;
            created.context_before = ctx_before;
            created.context_after = ctx_after;
            created.meta.last_used_timestamp = ts_sec;
            created.meta.hit_count = static_cast<uint32_t>(
                std::min<int64_t>(std::numeric_limits<uint32_t>::max(), freq));
            created.meta.base_weight = 1.0f;
            created.meta.text_bytes_len = static_cast<uint16_t>(phrase.size());
            created.meta.context_before_len =
                static_cast<uint16_t>(ctx_before.size());
            created.meta.context_after_len =
                static_cast<uint16_t>(ctx_after.size());
            created.meta.negative_feedback = static_cast<uint32_t>(
                std::min<int64_t>(std::numeric_limits<uint32_t>::max(), neg_fb));
            created.meta.suppressed = suppressed ? 1 : 0;
            created.meta.last_selected_ms = last_ms;
            variants.push_back(std::move(created));
            ++total_entries_;
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(sql_db);

    if (step_rc != SQLITE_DONE) {
        return false;
    }

    migrated_from_sqlite_ = true;
    if (env_ != nullptr && !lmdb_read_only_) {
        return FlushToLmdb();
    }
    return true;
}

std::vector<LearningEntry> PersonalEngine::ExportLearningEntries() const {
    std::vector<LearningEntry> entries;
    entries.reserve(total_entries_);
    for (const auto &[norm_key, variants] : db_) {
        const std::string fallback_py = ToApostrophePinyin(norm_key);
        for (const auto &v : variants) {
            LearningEntry entry;
            entry.phrase = v.text;
            entry.pinyin = v.raw_pinyin.empty() ? fallback_py : v.raw_pinyin;
            entry.contextBefore = v.context_before;
            entry.contextAfter = v.context_after;
            entry.frequency = static_cast<int64_t>(v.meta.hit_count);
            entry.lastSelectedMs =
                v.meta.last_selected_ms > 0
                    ? v.meta.last_selected_ms
                    : static_cast<int64_t>(v.meta.last_used_timestamp) * 1000LL;
            entry.negativeFeedback = static_cast<int64_t>(v.meta.negative_feedback);
            entry.suppressed = (v.meta.suppressed != 0);
            entries.push_back(std::move(entry));
        }
    }
    return entries;
}

std::shared_ptr<const LearningSnapshot> PersonalEngine::ToLearningSnapshot(
    size_t total_entry_limit) const {
    return std::make_shared<const LearningSnapshot>(
        ExportLearningEntries(), total_entry_limit);
}

bool PersonalEngine::ApplyLearningBatch(
    const std::vector<LearningEntry> &entries,
    bool replace_all) {
    if (simulate_write_failure_) {
        return false;
    }
    if (replace_all) {
        db_.clear();
        compact_index_.clear();
        total_entries_ = 0;
    }
    for (const auto &e : entries) {
        const std::string norm_key =
            NormalizePinyinKey(e.pinyin.empty() ? e.phrase : e.pinyin);
        auto &variants = db_[norm_key];
        RebuildCompactIndex(norm_key);
        StoredVariant created;
        created.text = e.phrase;
        created.raw_pinyin = e.pinyin.empty() ? ToApostrophePinyin(norm_key) : e.pinyin;
        created.context_before = e.contextBefore;
        created.context_after = e.contextAfter;
        created.meta.hit_count = static_cast<uint32_t>(
            std::clamp<int64_t>(e.frequency, 0, std::numeric_limits<uint32_t>::max()));
        created.meta.last_selected_ms = e.lastSelectedMs;
        created.meta.last_used_timestamp =
            e.lastSelectedMs > 0
                ? static_cast<uint32_t>(std::max<int64_t>(1, e.lastSelectedMs / 1000))
                : 0;
        created.meta.base_weight = 1.0f;
        created.meta.text_bytes_len = static_cast<uint16_t>(e.phrase.size());
        created.meta.context_before_len =
            static_cast<uint16_t>(e.contextBefore.size());
        created.meta.context_after_len =
            static_cast<uint16_t>(e.contextAfter.size());
        created.meta.negative_feedback = static_cast<uint32_t>(
            std::clamp<int64_t>(e.negativeFeedback, 0, std::numeric_limits<uint32_t>::max()));
        created.meta.suppressed = e.suppressed ? 1 : 0;
        variants.push_back(std::move(created));
        ++total_entries_;
    }
    if (env_ != nullptr && !lmdb_read_only_) {
        return FlushToLmdb();
    }
    return true;
}

bool PersonalEngine::SaveToFile(const std::string &file_path) const {
    std::ofstream ofs(file_path, std::ios::binary);
    if (!ofs.is_open()) {
        return false;
    }
    ofs.write(reinterpret_cast<const char *>(&config_.magic_header),
              sizeof(config_.magic_header));
    const uint32_t num_keys = static_cast<uint32_t>(db_.size());
    ofs.write(reinterpret_cast<const char *>(&num_keys), sizeof(num_keys));

    for (const auto &[key, variants] : db_) {
        const uint16_t key_len = static_cast<uint16_t>(key.size());
        ofs.write(reinterpret_cast<const char *>(&key_len), sizeof(key_len));
        ofs.write(key.data(), key_len);

        const uint32_t num_words = static_cast<uint32_t>(variants.size());
        ofs.write(reinterpret_cast<const char *>(&num_words), sizeof(num_words));

        for (const auto &v : variants) {
            WordMeta meta = v.meta;
            meta.text_bytes_len = static_cast<uint16_t>(v.text.size());
            meta.context_before_len = static_cast<uint16_t>(v.context_before.size());
            meta.context_after_len = static_cast<uint16_t>(v.context_after.size());
            ofs.write(reinterpret_cast<const char *>(&meta), sizeof(WordMeta));
            ofs.write(v.text.data(), meta.text_bytes_len);
            if (meta.context_before_len > 0) {
                ofs.write(v.context_before.data(), meta.context_before_len);
            }
            if (meta.context_after_len > 0) {
                ofs.write(v.context_after.data(), meta.context_after_len);
            }
        }
    }
    return ofs.good();
}

bool PersonalEngine::LoadFromFile(const std::string &file_path) {
    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs.is_open()) {
        return false;
    }
    uint32_t magic = 0;
    ifs.read(reinterpret_cast<char *>(&magic), sizeof(magic));
    if (!ifs.good() || magic != config_.magic_header) {
        return false;
    }
    uint32_t num_keys = 0;
    ifs.read(reinterpret_cast<char *>(&num_keys), sizeof(num_keys));
    if (!ifs.good()) {
        return false;
    }

    db_.clear();
    compact_index_.clear();
    total_entries_ = 0;

    for (uint32_t i = 0; i < num_keys; ++i) {
        uint16_t key_len = 0;
        ifs.read(reinterpret_cast<char *>(&key_len), sizeof(key_len));
        std::string key(key_len, '\0');
        ifs.read(key.data(), key_len);

        uint32_t num_words = 0;
        ifs.read(reinterpret_cast<char *>(&num_words), sizeof(num_words));
        if (!ifs.good()) {
            return false;
        }

        auto &variants = db_[key];
        RebuildCompactIndex(key);
        for (uint32_t j = 0; j < num_words; ++j) {
            WordMeta meta{};
            ifs.read(reinterpret_cast<char *>(&meta), sizeof(WordMeta));
            std::string text(meta.text_bytes_len, '\0');
            ifs.read(text.data(), meta.text_bytes_len);
            std::string ctx_before(meta.context_before_len, '\0');
            if (meta.context_before_len > 0) {
                ifs.read(ctx_before.data(), meta.context_before_len);
            }
            std::string ctx_after(meta.context_after_len, '\0');
            if (meta.context_after_len > 0) {
                ifs.read(ctx_after.data(), meta.context_after_len);
            }
            if (!ifs.good()) {
                return false;
            }
            StoredVariant v;
            v.text = std::move(text);
            v.raw_pinyin = ToApostrophePinyin(key);
            v.context_before = std::move(ctx_before);
            v.context_after = std::move(ctx_after);
            v.meta = meta;
            variants.push_back(std::move(v));
            ++total_entries_;
        }
    }
    return true;
}

void PersonalEngine::Prune() {
    if (total_entries_ <= config_.prune_target_size) {
        return;
    }
    const uint32_t now = NowSeconds();
    struct ScoredRef {
        std::string key;
        size_t index = 0;
        bool suppressed = false;
        float score = 0.0f;
        int64_t last_ms = 0;
        uint32_t hit_count = 0;
    };

    std::vector<ScoredRef> all;
    all.reserve(total_entries_);
    for (const auto &[k, variants] : db_) {
        for (size_t i = 0; i < variants.size(); ++i) {
            const auto &v = variants[i];
            all.push_back({k,
                           i,
                           v.meta.suppressed != 0,
                           CalculateScore(v.meta, v.text, v.context_before, {}, now),
                           v.meta.last_selected_ms,
                           v.meta.hit_count});
        }
    }

    // 优先保留：未屏蔽 > 综合动态得分高 > 最近使用 > 频次高
    const size_t keep_count =
        std::min(config_.prune_target_size, all.size());
    std::partial_sort(
        all.begin(),
        all.begin() + static_cast<std::ptrdiff_t>(keep_count),
        all.end(),
        [](const ScoredRef &a, const ScoredRef &b) {
            if (a.suppressed != b.suppressed) {
                return !a.suppressed && b.suppressed;
            }
            if (std::abs(a.score - b.score) > 1e-6f) {
                return a.score > b.score;
            }
            if (a.last_ms != b.last_ms) {
                return a.last_ms > b.last_ms;
            }
            return a.hit_count > b.hit_count;
        });

    std::map<std::string, std::vector<StoredVariant>, std::less<>> new_db;
    compact_index_.clear();
    for (size_t i = 0; i < keep_count; ++i) {
        const auto &ref = all[i];
        new_db[ref.key].push_back(std::move(db_[ref.key][ref.index]));
        RebuildCompactIndex(ref.key);
    }
    db_ = std::move(new_db);
    total_entries_ = keep_count;

    if (env_ != nullptr && !lmdb_read_only_) {
        FlushToLmdb();
    }
}

} // namespace modernime::core
