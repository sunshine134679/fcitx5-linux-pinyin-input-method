#include "personal_engine_state.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace modernime::core::detail {

bool PersonalEngineState::rebuildCompactIndex(std::string_view norm_key) {
    const std::string compact = PersonalEngine::CompactPinyin(norm_key);
    if (compact.empty()) {
        return false;
    }
    auto &vec = compact_index[compact];
    if (std::find(vec.begin(), vec.end(), norm_key) == vec.end()) {
        vec.emplace_back(norm_key);
    }
    return true;
}

void PersonalEngineState::pruneContextVariants() {
    for (auto &[key, variants] : db) {
        (void)key;
        std::unordered_map<std::string, std::vector<size_t>> by_text;
        for (size_t i = 0; i < variants.size(); ++i) {
            if (!variants[i].context_before.empty() ||
                !variants[i].context_after.empty()) {
                by_text[variants[i].text].push_back(i);
            }
        }
        std::vector<size_t> to_drop;
        for (auto &[text, indices] : by_text) {
            if (indices.size() <= 8) {
                continue;
            }
            std::stable_sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
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
            to_drop.insert(to_drop.end(), indices.begin() + 8, indices.end());
        }
        if (!to_drop.empty()) {
            std::sort(to_drop.rbegin(), to_drop.rend());
            for (size_t idx : to_drop) {
                variants.erase(variants.begin() + static_cast<std::ptrdiff_t>(idx));
                if (total_entries > 0) {
                    --total_entries;
                }
            }
        }
    }
}

bool PersonalEngineState::recordUsage(std::string_view pinyin_key,
                                     std::string_view text,
                                     float initial_weight,
                                     std::string_view context_before,
                                     std::string_view context_after,
                                     uint32_t now_sec,
                                     int64_t now_ms) {
    if (pinyin_key.empty() || text.empty()) {
        return false;
    }
    const std::string norm_key = PersonalEngine::NormalizePinyinKey(pinyin_key);
    if (norm_key == "!u!") {
        return false;
    }
    const std::string sanitized_before =
        context_before.empty()
            ? active_context
            : PersonalEngine::SanitizeSurroundingContext(context_before);
    const std::string sanitized_after =
        PersonalEngine::SanitizeSurroundingContext(context_after);

    auto &variants = db[norm_key];
    rebuildCompactIndex(norm_key);

    for (auto &v : variants) {
        if (v.text == text && v.meta.suppressed != 0) {
            v.meta.suppressed = 0;
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
        created.raw_pinyin = PersonalEngine::ToApostrophePinyin(norm_key);
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
        ++total_entries;
    }

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
            if (total_entries > 0) {
                --total_entries;
            }
        }
    }

    if (total_entries > config.max_dict_capacity) {
        prune(now_sec);
    }
    return true;
}

bool PersonalEngineState::recordNegativeFeedback(std::string_view pinyin_key,
                                                 std::string_view text) {
    if (pinyin_key.empty() || text.empty()) {
        return false;
    }
    const std::string norm_key = PersonalEngine::NormalizePinyinKey(pinyin_key);
    auto &variants = db[norm_key];
    rebuildCompactIndex(norm_key);

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
        created.raw_pinyin = PersonalEngine::ToApostrophePinyin(norm_key);
        created.meta.base_weight = 1.0f;
        created.meta.text_bytes_len = static_cast<uint16_t>(text.size());
        created.meta.negative_feedback = 1;
        variants.push_back(std::move(created));
        target = &variants.back();
        ++total_entries;
    } else if (target->meta.negative_feedback <
               std::numeric_limits<uint32_t>::max()) {
        target->meta.negative_feedback += 1;
    }

    if (total_entries > config.max_dict_capacity) {
        prune(0);
    }
    return true;
}

bool PersonalEngineState::recordSuppression(std::string_view pinyin_key,
                                            std::string_view text) {
    if (pinyin_key.empty() || text.empty()) {
        return false;
    }
    const std::string norm_key = PersonalEngine::NormalizePinyinKey(pinyin_key);
    auto &variants = db[norm_key];
    rebuildCompactIndex(norm_key);

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
        created.raw_pinyin = PersonalEngine::ToApostrophePinyin(norm_key);
        created.meta.base_weight = 1.0f;
        created.meta.text_bytes_len = static_cast<uint16_t>(text.size());
        created.meta.negative_feedback = 1;
        created.meta.suppressed = 1;
        created.meta.hit_count = 0;
        variants.push_back(std::move(created));
        target = &variants.back();
        ++total_entries;
    } else {
        target->meta.suppressed = 1;
    }

    if (total_entries > config.max_dict_capacity) {
        prune(0);
    }
    return true;
}

void PersonalEngineState::prune(uint32_t now) {
    if (total_entries <= config.prune_target_size) {
        return;
    }
    struct ScoredRef {
        std::string key;
        size_t index = 0;
        bool suppressed = false;
        float score = 0.0f;
        int64_t last_ms = 0;
        uint32_t hit_count = 0;
    };

    std::vector<ScoredRef> all;
    all.reserve(total_entries);
    PersonalEngine pe(config);
    for (const auto &[k, variants] : db) {
        for (size_t i = 0; i < variants.size(); ++i) {
            const auto &v = variants[i];
            all.push_back({k,
                           i,
                           v.meta.suppressed != 0,
                           pe.CalculateScore(v.meta, v.text, v.context_before, {}, now),
                           v.meta.last_selected_ms,
                           v.meta.hit_count});
        }
    }

    const size_t keep_count =
        std::min(config.prune_target_size, all.size());
    std::partial_sort(
        all.begin(),
        all.begin() + static_cast<std::ptrdiff_t>(keep_count),
        all.end(),
        [](const ScoredRef &a, const ScoredRef &b) {
            if (a.suppressed != b.suppressed) {
                return !a.suppressed && b.suppressed;
            }
            const bool aFin = std::isfinite(a.score);
            const bool bFin = std::isfinite(b.score);
            if (aFin != bFin) {
                return aFin;
            }
            if (aFin && a.score != b.score) {
                return a.score > b.score;
            }
            if (a.last_ms != b.last_ms) {
                return a.last_ms > b.last_ms;
            }
            if (a.hit_count != b.hit_count) {
                return a.hit_count > b.hit_count;
            }
            if (a.key != b.key) {
                return a.key < b.key;
            }
            return a.index < b.index;
        });

    std::map<std::string, std::vector<StoredVariant>, std::less<>> new_db;
    compact_index.clear();
    for (size_t i = 0; i < keep_count; ++i) {
        const auto &ref = all[i];
        new_db[ref.key].push_back(std::move(db[ref.key][ref.index]));
        rebuildCompactIndex(ref.key);
    }
    db = std::move(new_db);
    total_entries = keep_count;
}

} // namespace modernime::core::detail
