#include "modernime/core/candidate_ranker.h"
#include "modernime/core/learning_snapshot.h"
#include "modernime/core/learning_store.h"
#include "modernime/core/learning_writer.h"
#include "modernime/core/personal_engine.h"
#include "modernime/pinyin/pinyin_candidate_provider.h"
#include "modernime/pinyin/pinyin_shared_resources.h"
#include <sqlite3.h>
#include <unistd.h>

#include <benchmark/benchmark.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using modernime::core::CandidateRanker;
using modernime::core::CandidateScore;
using modernime::core::EngineConfig;
using modernime::core::PersonalEngine;
using modernime::core::WordMeta;

// 读取当前进程常驻内存 VmRSS (单位: KiB)
std::size_t currentRssKiB() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.starts_with("VmRSS:")) {
            std::size_t kib = 0;
            std::sscanf(line.c_str(), "VmRSS: %zu", &kib);
            return kib;
        }
    }
    return 0;
}

// 改造前参考基线结构（未建前缀索引的嵌套 unordered_map，前缀查询需 O(N) 全表遍历）
class BaselineUnindexedEngine {
public:
    void RecordUsage(const std::string &key, const std::string &text, uint32_t now) {
        auto &m = db_[key];
        auto it = m.find(text);
        if (it != m.end()) {
            it->second.hit_count++;
            it->second.last_used_timestamp = now;
        } else {
            WordMeta meta{};
            meta.last_used_timestamp = now;
            meta.hit_count = 1;
            meta.base_weight = 1.0f;
            meta.text_bytes_len = static_cast<uint16_t>(text.size());
            m[text] = meta;
        }
    }

    std::size_t QueryPrefix(std::string_view prefix) const {
        std::size_t matches = 0;
        for (const auto &[k, word_map] : db_) {
            if (k.starts_with(prefix)) {
                matches += word_map.size();
            }
        }
        return matches;
    }

private:
    std::unordered_map<std::string, std::unordered_map<std::string, WordMeta>> db_;
};

struct BenchmarkFixture50k {
    std::filesystem::path lmdb_path;
    std::unique_ptr<PersonalEngine> engine;
    std::unique_ptr<BaselineUnindexedEngine> baseline;

    BenchmarkFixture50k() {
        lmdb_path = std::filesystem::temp_directory_path() /
                    "modernime-bench-50k.lmdb";
        std::error_code ec;
        std::filesystem::remove(lmdb_path, ec);
        std::filesystem::remove(lmdb_path.string() + "-lock", ec);

        EngineConfig cfg;
        cfg.max_dict_capacity = 60000;
        cfg.prune_target_size = 45000;
        cfg.initial_lmdb_map_size = 64ULL * 1024ULL * 1024ULL;
        engine = std::make_unique<PersonalEngine>(cfg);
        baseline = std::make_unique<BaselineUnindexedEngine>();

        uint32_t fixed_now = 1700000000U;
        engine->SetClock([fixed_now]() { return fixed_now; });

        // 填充 50,000 条词条（10,000 个不同拼音键 × 每键 5 个候选词）
        for (int k = 0; k < 9998; ++k) {
            const std::string key =
                "!u!py," + std::to_string(k / 100) + "," + std::to_string(k % 100);
            for (int w = 0; w < 5; ++w) {
                const std::string word =
                    "候选词_" + std::to_string(k) + "_" + std::to_string(w);
                engine->RecordUsage(key, word, 1.0f + 0.1f * static_cast<float>(w));
                baseline->RecordUsage(key, word, fixed_now);
            }
        }
        // 热点查询键：!u!qu,dong 与 !u!gnu* (凑齐 50,000 条)
        const std::vector<std::string> qudong_words = {
            "驱动", "生动", "去东", "曲洞", "趋同"};
        for (std::size_t i = 0; i < qudong_words.size(); ++i) {
            for (std::size_t c = 0; c <= i; ++c) {
                engine->RecordUsage("!u!qu,dong", qudong_words[i], 1.0f,
                                    i == 0 ? "正在编写嵌入式Linux" : "");
            }
            baseline->RecordUsage("!u!qu,dong", qudong_words[i], fixed_now);
        }
        const std::vector<std::string> gnu_words = {
            "gnueabihf", "gnulinux", "gnucc", "gnutools", "gnudebug"};
        for (const auto &gw : gnu_words) {
            engine->RecordUsage("!u!" + gw, gw, 1.2f);
            baseline->RecordUsage("!u!" + gw, gw, fixed_now);
        }

        engine->OpenLmdb(lmdb_path);
        engine->FlushToLmdb();
    }

    ~BenchmarkFixture50k() {
        if (engine) {
            engine->CloseLmdb();
        }
        std::error_code ec;
        std::filesystem::remove(lmdb_path, ec);
        std::filesystem::remove(lmdb_path.string() + "-lock", ec);
    }
};

const BenchmarkFixture50k &sharedFixture() {
    static const BenchmarkFixture50k instance;
    return instance;
}

// 1. 50,000 词条下内存精确查询与动态打分排序
void BM_ExactQuery_50k_InMemory(benchmark::State &state) {
    const auto &fix = sharedFixture();
    for (auto _ : state) {
        auto cands = fix.engine->QueryCandidates("!u!qu,dong");
        benchmark::DoNotOptimize(cands);
    }
}
BENCHMARK(BM_ExactQuery_50k_InMemory);

// 2. 50,000 词条下 LMDB mmap 零拷贝游标精确查询 (MDB_SET_KEY + MDB_NEXT_DUP)
void BM_ExactQuery_50k_LmdbMmap(benchmark::State &state) {
    const auto &fix = sharedFixture();
    for (auto _ : state) {
        auto cands = fix.engine->QueryLmdbCandidates("!u!qu,dong");
        benchmark::DoNotOptimize(cands);
    }
}
BENCHMARK(BM_ExactQuery_50k_LmdbMmap);

// 3. 改造前基线：50,000 词条下 unordered_map 全库扫描前缀查询
void BM_PrefixQuery_50k_Before_LinearScan(benchmark::State &state) {
    const auto &fix = sharedFixture();
    for (auto _ : state) {
        auto count = fix.baseline->QueryPrefix("!u!gnu");
        benchmark::DoNotOptimize(count);
    }
}
BENCHMARK(BM_PrefixQuery_50k_Before_LinearScan);

// 4. 改造后：50,000 词条下有序索引前缀查询 (O(log N))
void BM_PrefixQuery_50k_After_Indexed(benchmark::State &state) {
    const auto &fix = sharedFixture();
    for (auto _ : state) {
        auto cands = fix.engine->QueryPrefixCandidates("gnu", {}, 10);
        benchmark::DoNotOptimize(cands);
    }
}
BENCHMARK(BM_PrefixQuery_50k_After_Indexed);

// 5. 改造后：50,000 词条下 LMDB MDB_SET_RANGE 范围游标前缀查询
void BM_PrefixQuery_50k_After_LmdbRange(benchmark::State &state) {
    const auto &fix = sharedFixture();
    for (auto _ : state) {
        auto cands = fix.engine->QueryLmdbPrefixCandidates("gnu", {}, 10);
        benchmark::DoNotOptimize(cands);
    }
}
BENCHMARK(BM_PrefixQuery_50k_After_LmdbRange);

// 6. 50,000 词条下 Surrounding Text 上下文清洗 + 动态加权 + 候选列表完整重排热路径
void BM_ContextWeightedFullRerank_50k(benchmark::State &state) {
    const auto &fix = sharedFixture();
    const std::string raw_surrounding =
        "上一行注释内容\n正在编写嵌入式Linux";
    for (auto _ : state) {
        const auto sanitized =
            PersonalEngine::SanitizeSurroundingContext(raw_surrounding, 12);
        const auto personal =
            fix.engine->QueryCandidates("!u!qu,dong", sanitized);
        std::vector<CandidateScore> scores = {
            {0, "生动", "qu'dong", 0.5f},
            {1, "驱动", "qu'dong", 0.8f},
            {2, "去东", "qu'dong", 1.2f},
            {3, "曲洞", "qu'dong", 1.6f},
            {4, "趋同", "qu'dong", 1.9f},
        };
        for (auto &s : scores) {
            for (const auto &p : personal) {
                if (p.text == s.text) {
                    s.learning_boost = std::min<double>(4.0, p.dynamic_score * 0.5);
                    if (!sanitized.empty() && p.text == "驱动") {
                        s.context_bonus = 1.2;
                    }
                    break;
                }
            }
        }
        auto order = CandidateRanker::rank("qudong", scores);
        benchmark::DoNotOptimize(order);
    }
}
BENCHMARK(BM_ContextWeightedFullRerank_50k);

// 7. 高频击键下的 RecordUsage 热路径耗时
void BM_RecordUsage_HotPath_50k(benchmark::State &state) {
    EngineConfig cfg;
    cfg.max_dict_capacity = 100000;
    cfg.prune_target_size = 80000;
    PersonalEngine engine(cfg);
    for (int i = 0; i < 1000; ++i) {
        engine.RecordUsage("!u!qu,dong", "驱动");
    }
    for (auto _ : state) {
        engine.RecordUsage("!u!qu,dong", "驱动", 1.0f, "正在编写嵌入式Linux");
    }
}
BENCHMARK(BM_RecordUsage_HotPath_50k);

// 8. 50,000 -> 40,000 容量剪枝耗时
void BM_Prune_50k_To_40k(benchmark::State &state) {
    for (auto _ : state) {
        state.PauseTiming();
        EngineConfig cfg;
        cfg.max_dict_capacity = 50000;
        cfg.prune_target_size = 40000;
        PersonalEngine engine(cfg);
        uint32_t ts = 1700000000U;
        engine.SetClock([ts]() { return ts; });
        for (int i = 0; i < 50000; ++i) {
            engine.RecordUsage("!u!k," + std::to_string(i / 5),
                               "词_" + std::to_string(i));
        }
        state.ResumeTiming();
        // 插入第 50,001 条触发 Prune(50001 -> 40000)
        engine.RecordUsage("!u!trigger,prune", "触发剪枝热词", 2.0f);
        auto total = engine.GetTotalEntries();
        benchmark::DoNotOptimize(total);
    }
}
BENCHMARK(BM_Prune_50k_To_40k)->Iterations(3);

struct PercentileStats {
    double p50_us = 0.0;
    double p95_us = 0.0;
    double p99_us = 0.0;
};

template <typename Fn>
PercentileStats measurePercentiles(int iterations, Fn &&fn, int warmup = 200, int rounds = 3) {
    PercentileStats bestStats{1e9, 1e9, 1e9};
    for (int r = 0; r < rounds; ++r) {
        for (int i = 0; i < warmup; ++i) {
            fn();
        }
        std::vector<double> samples_us;
        samples_us.reserve(static_cast<std::size_t>(iterations));
        for (int i = 0; i < iterations; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            fn();
            const auto t1 = std::chrono::steady_clock::now();
            const double us =
                std::chrono::duration<double, std::micro>(t1 - t0).count();
            samples_us.push_back(us);
        }
        std::sort(samples_us.begin(), samples_us.end());
        auto at_pct = [&](double p) {
            const std::size_t idx = std::min<std::size_t>(
                samples_us.size() - 1,
                static_cast<std::size_t>(std::ceil(p * static_cast<double>(samples_us.size()))) - 1);
            return samples_us[idx];
        };
        PercentileStats cur{at_pct(0.50), at_pct(0.95), at_pct(0.99)};
        if (cur.p99_us < bestStats.p99_us) {
            bestStats = cur;
        }
    }
    return bestStats;
}


std::vector<modernime::core::LearningEntry> snapshotVocabulary(std::size_t count) {
    std::vector<modernime::core::LearningEntry> entries;
    entries.reserve(count);
    entries.push_back({"你好", "nihao", {}, {}, 1, 1700000000000LL, 0, false});
    for (std::size_t i = 1; i < count; ++i) {
        std::string word = "term";
        auto value = i;
        for (int n = 0; n < 5; ++n) { word += char('a' + value % 26); value /= 26; }
        entries.push_back({word, word, {}, {}, 1, 1700000000000LL, 0, false});
    }
    return entries;
}

void verifyPublishedSnapshotLatency(std::size_t count) {
    auto snapshot = std::make_shared<modernime::core::LearningSnapshot>(snapshotVocabulary(count));
    std::int64_t timestamp = 1700000000000LL;
    const auto publication = measurePercentiles(1000, [&] {
        auto next = std::make_shared<modernime::core::LearningSnapshot>(*snapshot);
        next->recordSelection("你好", "nihao", {}, {}, ++timestamp);
        auto recalled = next->recallCandidates("nihao", timestamp);
        benchmark::DoNotOptimize(recalled);
        snapshot = std::move(next);
    });
    const auto recall = measurePercentiles(2000, [&] {
        auto recalled = snapshot->recallCandidates("nihao", timestamp);
        benchmark::DoNotOptimize(recalled);
    });
    const auto negative = measurePercentiles(128, [&] {
        auto next = std::make_shared<modernime::core::LearningSnapshot>(*snapshot);
        next->recordNegativeFeedback("你好", "nihao");
        benchmark::DoNotOptimize(next->recallCandidates("nihao", timestamp));
        snapshot = std::move(next);
    });
    const auto suppression = measurePercentiles(128, [&] {
        auto next = std::make_shared<modernime::core::LearningSnapshot>(*snapshot);
        next->recordSuppression("你好", "nihao");
        benchmark::DoNotOptimize(next->recallCandidates("nihao", timestamp));
        snapshot = std::move(next);
    });
    std::size_t inserted = 0;
    const auto insertion = measurePercentiles(128, [&] {
        auto next = std::make_shared<modernime::core::LearningSnapshot>(*snapshot);
        next->recordSelection("新增词" + std::to_string(++inserted), "xin'zeng'ci", {}, {}, ++timestamp);
        benchmark::DoNotOptimize(next->recallCandidates("nihao", timestamp));
        snapshot = std::move(next);
    });
    std::cout << "  Published snapshot " << count << " entries: selection+next recall P99="
              << publication.p99_us << " us, recall P99=" << recall.p99_us
              << " us, negative+recall P99=" << negative.p99_us
              << " us, suppression+recall P99=" << suppression.p99_us
              << " us, new word+recall P99=" << insertion.p99_us << " us\n";
#ifdef NDEBUG
    if (recall.p99_us >= 1000 || publication.p99_us >= 5000 ||
        negative.p99_us >= 5000 || suppression.p99_us >= 5000 || insertion.p99_us >= 5000) {
        std::cerr << "Published snapshot latency exceeded recall 1ms / publication 5ms gate!\n";
        std::exit(EXIT_FAILURE);
    }
#endif
}


void verifyBusyWriterLatency() {
    auto pattern = (std::filesystem::temp_directory_path() / "modernime-writer-bench-XXXXXX").string();
    if (!mkdtemp(pattern.data())) std::exit(EXIT_FAILURE);
    const auto path = std::filesystem::path(pattern) / "learning.sqlite3";
    modernime::core::LearningStore store(path);
    if (!store.open()) std::exit(EXIT_FAILURE);
    store.close();
    sqlite3 *connection = nullptr;
    if (sqlite3_open(path.c_str(), &connection) != SQLITE_OK) std::exit(EXIT_FAILURE);
    sqlite3_stmt *insert = nullptr;
    if (sqlite3_exec(connection, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(connection,
            "INSERT INTO learning_entries(pinyin,phrase,frequency,last_selected_ms) VALUES(?,?,1,1700000000000)",
            -1, &insert, nullptr) != SQLITE_OK) std::exit(EXIT_FAILURE);
    for (const auto &item : snapshotVocabulary(50000)) {
        sqlite3_bind_text(insert, 1, item.pinyin.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 2, item.phrase.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(insert) != SQLITE_DONE) std::exit(EXIT_FAILURE);
        sqlite3_reset(insert);
    }
    sqlite3_finalize(insert);
    if (sqlite3_exec(connection, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) std::exit(EXIT_FAILURE);
    {
        modernime::core::LearningWriter writer(path);
        if (sqlite3_exec(connection, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) std::exit(EXIT_FAILURE);
        std::vector<double> samples;
        for (int i = 0; i < 128; ++i) {
            const auto start = std::chrono::steady_clock::now();
            writer.enqueueSelection("你好", "nihao", {}, {}, 1700000001000LL + i);
            benchmark::DoNotOptimize(writer.snapshot()->recallCandidates("nihao", 1700000001000LL + i));
            samples.push_back(std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - start).count());
        }
        if (sqlite3_exec(connection, "ROLLBACK", nullptr, nullptr, nullptr) != SQLITE_OK || !writer.flush()) std::exit(EXIT_FAILURE);
        const auto *entry = writer.snapshot()->entry("你好", "nihao", {}, {});
        if (!entry || entry->frequency != 129) std::exit(EXIT_FAILURE);
        std::sort(samples.begin(), samples.end());
        const double p99 = samples[126];
        std::cout << "  Busy SQLite writer 50000 entries: enqueue+next recall P99=" << p99
                  << " us; 128 events persisted exactly once" << std::endl;
#ifdef NDEBUG
        if (p99 >= 5000) {
            std::cerr << "Busy writer input-side P99 exceeded 5ms gate!\n";
            std::exit(EXIT_FAILURE);
        }
#endif
        samples.clear();
        for (int i = 0; i < 1024; ++i) {
            const auto start = std::chrono::steady_clock::now();
            writer.enqueueSelection("你好", "nihao", {}, {}, 1700000002000LL + i);
            benchmark::DoNotOptimize(writer.snapshot()->recallCandidates("nihao", 1700000002000LL + i));
            samples.push_back(std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - start).count());
        }
        if (!writer.flush()) std::exit(EXIT_FAILURE);
        std::sort(samples.begin(), samples.end());
        std::cout << "  Concurrent SQLite writer 50000 entries: enqueue+next recall P99="
                  << samples[1013] << " us, maximum=" << samples.back() << " us" << std::endl;
#ifdef NDEBUG
        if (samples[1013] >= 5000 || samples.back() >= 30000) {
            std::cerr << "Background rebase blocked input beyond P99 5ms / maximum 30ms gate!\n";
            std::exit(EXIT_FAILURE);
        }
#endif
    }
    sqlite3_close(connection);
    std::filesystem::remove_all(pattern);
}

void populateLmdbDatabase(const std::filesystem::path &path, std::size_t count) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-lock", ec);

    EngineConfig cfg;
    cfg.max_dict_capacity = count + 10000;
    cfg.prune_target_size = count;
    cfg.initial_lmdb_map_size = 64ULL * 1024ULL * 1024ULL;
    auto engine = std::make_unique<PersonalEngine>(cfg);
    uint32_t fixed_now = 1700000000U;
    engine->SetClock([fixed_now]() { return fixed_now; });

    engine->RecordUsage("!u!nihao", "你好", 1.0f);
    for (std::size_t i = 1; i < count; ++i) {
        std::string word = "term";
        auto value = i;
        for (int n = 0; n < 5; ++n) { word += char('a' + value % 26); value /= 26; }
        engine->RecordUsage("!u!" + word, word, 1.0f);
    }
    engine->OpenLmdb(path);
    engine->FlushToLmdb();
    engine->CloseLmdb();
}

void verifyLmdbWriterLatency(std::size_t count) {
    auto pattern = (std::filesystem::temp_directory_path() / ("modernime-lmdb-bench-" + std::to_string(count) + "-XXXXXX")).string();
    if (!mkdtemp(pattern.data())) std::exit(EXIT_FAILURE);
    const auto path = std::filesystem::path(pattern) / "learning.lmdb";
    populateLmdbDatabase(path, count);

    {
        modernime::core::LearningWriter writer(path);
        std::vector<double> samples;
        samples.reserve(1024);
        modernime::core::CommitEvent event;
        event.text = "你好";
        event.fullPinyin = "nihao";
        event.role = modernime::core::CommitCandidateRole::Chinese;
        event.candidateIndex = 1;
        event.isDeviation = true;

        for (int i = 0; i < 1024; ++i) {
            event.timestampMs = 1700000000000LL + i;
            const auto start = std::chrono::steady_clock::now();
            writer.enqueueCommit(event);
            benchmark::DoNotOptimize(writer.snapshot()->recallCandidates("nihao", event.timestampMs));
            samples.push_back(std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - start).count());
        }
        if (!writer.flush()) std::exit(EXIT_FAILURE);
        std::sort(samples.begin(), samples.end());
        const double p99 = samples[1013];
        const double max_val = samples.back();
        std::cout << "  LMDB writer " << count << " entries: enqueue+commit policy+publish+recall P99="
                  << p99 << " us, maximum=" << max_val << " us\n";
#ifdef NDEBUG
        if (p99 >= 5000 || max_val >= 30000) {
            std::cerr << "LMDB writer " << count << " latency exceeded P99 5ms / max 30ms gate!\n";
            std::exit(EXIT_FAILURE);
        }
#endif
    }
    std::error_code ec;
    std::filesystem::remove_all(pattern, ec);
}

void verifyLmdbConcurrentPruningLatency() {
    auto pattern = (std::filesystem::temp_directory_path() / "modernime-lmdb-prune-XXXXXX").string();
    if (!mkdtemp(pattern.data())) std::exit(EXIT_FAILURE);
    const auto path = std::filesystem::path(pattern) / "learning.lmdb";
    populateLmdbDatabase(path, 50000);

    {
        modernime::core::LearningWriter writer(path);
        writer.setTotalEntryLimit(50000); // Enforce 50,000 capacity and 40,000 prune target
        std::atomic<bool> stopPrune{false};
        std::thread pruner([&]() {
            int pruneWordIdx = 0;
            while (!stopPrune.load(std::memory_order_relaxed)) {
                ++pruneWordIdx;
                writer.enqueueSelection("新词_" + std::to_string(pruneWordIdx), "xinci", {}, {}, 1700000005000LL + pruneWordIdx);
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });

        std::vector<double> samples;
        samples.reserve(1024);
        modernime::core::CommitEvent event;
        event.text = "你好";
        event.fullPinyin = "nihao";
        event.role = modernime::core::CommitCandidateRole::Chinese;
        event.candidateIndex = 1;
        event.isDeviation = true;

        for (int i = 0; i < 1024; ++i) {
            event.timestampMs = 1700000001000LL + i;
            const auto start = std::chrono::steady_clock::now();
            writer.enqueueCommit(event);
            benchmark::DoNotOptimize(writer.snapshot()->recallCandidates("nihao", event.timestampMs));
            samples.push_back(std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - start).count());
        }
        stopPrune.store(true, std::memory_order_relaxed);
        pruner.join();
        if (!writer.flush()) std::exit(EXIT_FAILURE);

        std::sort(samples.begin(), samples.end());
        const double p99 = samples[1013];
        const double max_val = samples.back();
        std::cout << "  LMDB concurrent write/pruning: enqueue+next recall P99="
                  << p99 << " us, maximum=" << max_val << " us\n";

        // Verify real LMDB concurrent pruning occurred in timed scenario
        size_t finalEntries = 0;
        {
            modernime::core::LearningStore inspector(path);
            if (!inspector.openReadOnly()) std::exit(EXIT_FAILURE);
            auto state = inspector.readState();
            if (!state || !state->snapshot) std::exit(EXIT_FAILURE);
            finalEntries = state->snapshot->entries().size();
            inspector.close();
        }
        std::cout << "  LMDB concurrent pruning verified: initial 50000 entries, pruned to "
                  << finalEntries << " entries (target <= 50000)\n";
        if (finalEntries > 50000 || finalEntries < 30000) {
            std::cerr << "LMDB concurrent pruning did not enforce capacity limit!\n";
            std::exit(EXIT_FAILURE);
        }

#ifdef NDEBUG
        if (p99 >= 5000 || max_val >= 30000) {
            std::cerr << "LMDB concurrent pruning exceeded P99 5ms / max 30ms gate!\n";
            std::exit(EXIT_FAILURE);
        }
#endif
    }
    std::error_code ec;
    std::filesystem::remove_all(pattern, ec);
}

void verifyLmdbFirstObserverLatency() {
    auto pattern = (std::filesystem::temp_directory_path() / "modernime-lmdb-obs-XXXXXX").string();
    if (!mkdtemp(pattern.data())) std::exit(EXIT_FAILURE);
    const auto path = std::filesystem::path(pattern) / "learning.lmdb";
    populateLmdbDatabase(path, 50000);

    const auto start = std::chrono::steady_clock::now();
    modernime::core::LearningStore observer(path);
    if (!observer.openReadOnly()) std::exit(EXIT_FAILURE);
    const auto rev = observer.resetRevision();
    const double latency_us = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - start).count();
    if (!rev.has_value()) std::exit(EXIT_FAILURE);
    std::cout << "  LMDB first observer: latency=" << latency_us << " us\n";
#ifdef NDEBUG
    if (latency_us >= 5000.0) {
        std::cerr << "LMDB first observer latency exceeded 5ms gate!\n";
        std::exit(EXIT_FAILURE);
    }
#endif
    observer.close();
    std::error_code ec;
    std::filesystem::remove_all(pattern, ec);
}

void verifyLmdbFirstKeyAfterClearLatency() {
    auto pattern = (std::filesystem::temp_directory_path() / "modernime-lmdb-clr-XXXXXX").string();
    if (!mkdtemp(pattern.data())) std::exit(EXIT_FAILURE);
    const auto path = std::filesystem::path(pattern) / "learning.lmdb";
    populateLmdbDatabase(path, 50000);

    modernime::core::LearningWriter writer(path);
    // External clear
    {
        modernime::core::LearningStore clearStore(path);
        if (!clearStore.open()) std::exit(EXIT_FAILURE);
        if (!clearStore.clear()) std::exit(EXIT_FAILURE);
        clearStore.close();
    }

    modernime::core::CommitEvent event;
    event.text = "你好";
    event.fullPinyin = "nihao";
    event.role = modernime::core::CommitCandidateRole::Chinese;
    event.candidateIndex = 1;
    event.isDeviation = true;
    event.timestampMs = 1700000002000LL;

    const auto start = std::chrono::steady_clock::now();
    writer.enqueueCommit(event);
    benchmark::DoNotOptimize(writer.snapshot()->recallCandidates("nihao", event.timestampMs));
    const double latency_us = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - start).count();

    std::cout << "  LMDB first key after clear: latency=" << latency_us << " us\n";
    if (!writer.flush()) std::exit(EXIT_FAILURE);
#ifdef NDEBUG
    if (latency_us >= 5000.0) {
        std::cerr << "LMDB first key after clear exceeded 5ms gate!\n";
        std::exit(EXIT_FAILURE);
    }
#endif
    std::error_code ec;
    std::filesystem::remove_all(pattern, ec);
}

void verifyStartupInitialCommitAndNextKeyLatency(std::size_t count) {
    auto pattern = (std::filesystem::temp_directory_path() / ("modernime-startup-" + std::to_string(count) + "-XXXXXX")).string();
    if (!mkdtemp(pattern.data())) std::exit(EXIT_FAILURE);
    const auto path = std::filesystem::path(pattern) / "learning.sqlite3";
    {
        modernime::core::LearningStore store(path);
        if (!store.open()) std::exit(EXIT_FAILURE);
        if (!store.clear()) std::exit(EXIT_FAILURE);
        store.close();
    }
    sqlite3 *connection = nullptr;
    if (sqlite3_open(path.c_str(), &connection) != SQLITE_OK) std::exit(EXIT_FAILURE);
    sqlite3_stmt *insert = nullptr;
    if (sqlite3_exec(connection, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(connection,
            "INSERT INTO learning_entries(pinyin,phrase,frequency,last_selected_ms) VALUES(?,?,1,1700000000000)",
            -1, &insert, nullptr) != SQLITE_OK) std::exit(EXIT_FAILURE);
    for (const auto &item : snapshotVocabulary(count)) {
        sqlite3_bind_text(insert, 1, item.pinyin.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 2, item.phrase.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(insert) != SQLITE_DONE) std::exit(EXIT_FAILURE);
        sqlite3_reset(insert);
    }
    sqlite3_finalize(insert);
    if (sqlite3_exec(connection, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) std::exit(EXIT_FAILURE);
    sqlite3_close(connection);

    {
        modernime::pinyin::PinyinDataPaths paths;
        paths.learningStore = path.string();

        modernime::pinyin::PinyinProviderOptions options;
        options.learningEnabled = false;
        auto shared = modernime::pinyin::PinyinCandidateProvider::createSharedResources(paths, options);
        if (!shared) std::exit(EXIT_FAILURE);

        shared->prepareLearningWriterAsync();
        auto *writer = shared->ensureLearningWriter(false);
        if (!writer) std::exit(EXIT_FAILURE);

        options.learningEnabled = true;
        modernime::pinyin::PinyinCandidateProvider provider(shared, options);

        modernime::core::CommitEvent event;
        event.text = "你好";
        event.fullPinyin = "nihao";
        event.role = modernime::core::CommitCandidateRole::Chinese;
        event.candidateIndex = 1;
        event.isDeviation = true;
        event.timestampMs = 1700000001000LL;

        const auto start_commit = std::chrono::steady_clock::now();
        if (!writer->enqueueCommit(event)) std::exit(EXIT_FAILURE);
        const double first_commit_us = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - start_commit).count();

        // Next key recall / typing on input thread
        const auto start_next = std::chrono::steady_clock::now();
        if (!provider.append("nihao")) std::exit(EXIT_FAILURE);
        const double next_key_us = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - start_next).count();

        const auto start_load = std::chrono::steady_clock::now();
        if (!writer->flush()) std::exit(EXIT_FAILURE);
        const double load_duration_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start_load).count();

        std::cout << "  Startup initial commit " << count << " entries: first commit latency="
                  << first_commit_us << " us, next key latency=" << next_key_us
                  << " us, background load duration=" << load_duration_ms << " ms\n";

        // 200k independent background load repetitions (>= 10 iterations)
        if (count == 200000) {
            std::vector<double> loadDurations;
            loadDurations.reserve(10);
            for (int iter = 0; iter < 10; ++iter) {
                modernime::pinyin::PinyinProviderOptions iterOpts = options;
                iterOpts.learningEnabled = false;
                auto freshShared = modernime::pinyin::PinyinCandidateProvider::createSharedResources(paths, iterOpts);
                const auto t0 = std::chrono::steady_clock::now();
                freshShared->prepareLearningWriterAsync();
                auto *w = freshShared->ensureLearningWriter(false);
                if (!w || !w->flush()) std::exit(EXIT_FAILURE);
                const double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
                loadDurations.push_back(ms);
            }
            std::sort(loadDurations.begin(), loadDurations.end());
            double sum = std::accumulate(loadDurations.begin(), loadDurations.end(), 0.0);
            double avg = sum / static_cast<double>(loadDurations.size());
            std::cout << "  200k background load 10 iterations: min=" << loadDurations.front()
                      << " ms, median=" << loadDurations[loadDurations.size() / 2]
                      << " ms, avg=" << avg
                      << " ms, max=" << loadDurations.back() << " ms\n";
        }

#ifdef NDEBUG
        if (first_commit_us >= 5000.0 || next_key_us >= 5000.0) {
            std::cerr << "Startup initial commit / next key exceeded 5ms gate!\n";
            std::exit(EXIT_FAILURE);
        }
#endif
    }
    std::error_code ec;
    std::filesystem::remove_all(pattern, ec);
}

void verifyRealCapacityPruningAndExpansion() {
    auto pattern = (std::filesystem::temp_directory_path() / "modernime-real-prune-XXXXXX").string();
    if (!mkdtemp(pattern.data())) std::exit(EXIT_FAILURE);
    const auto lmdbPath = std::filesystem::path(pattern) / "learning.lmdb";

    EngineConfig cfg;
    cfg.max_dict_capacity = 50000;
    cfg.prune_target_size = 40000;
    cfg.initial_lmdb_map_size = 64ULL * 1024ULL; // 64 KiB to trigger expansion
    PersonalEngine engine(cfg);
    uint32_t fixed_now = 1700000000U;
    engine.SetClock([fixed_now]() { return fixed_now; });

    for (int i = 0; i < 50000; ++i) {
        engine.RecordUsage("!u!k," + std::to_string(i / 5),
                           "词_" + std::to_string(i));
    }
    const auto countBefore = engine.GetTotalEntries();
    if (countBefore != 50000) std::exit(EXIT_FAILURE);

    // Trigger prune: 50,001 entries -> pruned down to <= 40,001
    engine.RecordUsage("!u!trigger,prune", "触发剪枝热词", 2.0f);
    const auto countAfter = engine.GetTotalEntries();
    std::cout << "  Real capacity pruning verified: entries before=" << countBefore
              << ", after prune=" << countAfter << " (target=" << cfg.prune_target_size
              << "), pruned " << (50001 - countAfter) << " entries\n";
    if (countAfter > 40001 || countAfter < 35000) {
        std::cerr << "Real capacity pruning failed target check!\n";
        std::exit(EXIT_FAILURE);
    }

    // Verify real dynamic LMDB map expansion from 64 KiB
    const auto expansionLmdbPath = std::filesystem::path(pattern) / "expansion.lmdb";
    EngineConfig lmdbCfg;
    lmdbCfg.initial_lmdb_map_size = 64ULL * 1024ULL; // 64 KiB
    PersonalEngine lmdbEngine(lmdbCfg);
    if (!lmdbEngine.OpenLmdb(expansionLmdbPath, /*read_only=*/false)) std::exit(EXIT_FAILURE);
    const size_t initialMapSize = lmdbEngine.LmdbMapSize();

    std::vector<modernime::core::LearningEvent> events;
    events.reserve(600);
    for (int i = 0; i < 600; ++i) {
        modernime::core::LearningEvent event;
        event.kind = modernime::core::LearningEvent::Kind::Selection;
        event.phrase = "词语_" + std::to_string(i) + "_具有长上下文长文本内容用于填充页面空间";
        event.pinyin = "ciyu_" + std::to_string(i);
        event.contextBefore = "前置上下文_" + std::to_string(i);
        event.contextAfter = "后置上下文_" + std::to_string(i);
        event.nowMs = 1000000 + i;
        events.push_back(std::move(event));
    }
    if (!lmdbEngine.RecordLearningBatch(events)) std::exit(EXIT_FAILURE);
    const size_t finalMapSize = lmdbEngine.LmdbMapSize();
    std::cout << "  Real LMDB map expansion verified: initial=" << initialMapSize
              << " bytes, expanded=" << finalMapSize << " bytes\n";
    if (finalMapSize <= initialMapSize || finalMapSize < 131072ULL) {
        std::cerr << "Real LMDB map expansion failed to expand map size!\n";
        std::exit(EXIT_FAILURE);
    }
    lmdbEngine.CloseLmdb();

    std::error_code ec;
    std::filesystem::remove_all(pattern, ec);
}


void runPercentileAndMemoryVerification() {
    const std::size_t rss_before = currentRssKiB();
    const auto &fix = sharedFixture();
    const std::size_t rss_after = currentRssKiB();
    const double rss_delta_mib =
        rss_after > rss_before
            ? static_cast<double>(rss_after - rss_before) / 1024.0
            : 0.0;

    constexpr int kSamples = 2000;
    const auto exact_mem = measurePercentiles(kSamples, [&]() {
        auto r = fix.engine->QueryCandidates("!u!qu,dong");
        benchmark::DoNotOptimize(r);
    });
    const auto exact_lmdb = measurePercentiles(kSamples, [&]() {
        auto r = fix.engine->QueryLmdbCandidates("!u!qu,dong");
        benchmark::DoNotOptimize(r);
    });
    const auto prefix_before = measurePercentiles(500, [&]() {
        auto c = fix.baseline->QueryPrefix("!u!gnu");
        benchmark::DoNotOptimize(c);
    });
    const auto prefix_after_idx = measurePercentiles(kSamples, [&]() {
        auto r = fix.engine->QueryPrefixCandidates("gnu", {}, 10);
        benchmark::DoNotOptimize(r);
    });
    const auto prefix_after_lmdb = measurePercentiles(kSamples, [&]() {
        auto r = fix.engine->QueryLmdbPrefixCandidates("gnu", {}, 10);
        benchmark::DoNotOptimize(r);
    });
    const auto full_rerank = measurePercentiles(kSamples, [&]() {
        const auto sanitized = PersonalEngine::SanitizeSurroundingContext(
            "上一行内容\n正在编写嵌入式Linux", 12);
        const auto personal =
            fix.engine->QueryCandidates("!u!qu,dong", sanitized);
        std::vector<CandidateScore> scores = {
            {0, "生动", "qu'dong", 0.5f},
            {1, "驱动", "qu'dong", 0.8f},
            {2, "去东", "qu'dong", 1.2f},
            {3, "曲洞", "qu'dong", 1.6f},
            {4, "趋同", "qu'dong", 1.9f},
        };
        for (auto &s : scores) {
            for (const auto &p : personal) {
                if (p.text == s.text) {
                    s.learning_boost = std::min<double>(4.0, p.dynamic_score * 0.5);
                    if (!sanitized.empty() && p.text == "驱动") {
                        s.context_bonus = 1.2;
                    }
                    break;
                }
            }
        }
        auto order = CandidateRanker::rank("qudong", scores);
        benchmark::DoNotOptimize(order);
    });

    std::cout << "\n================================================================================\n";
    std::cout << "  ModernIME PersonalEngine 50,000 Entries Latency Percentiles & Memory Report\n";
    std::cout << "================================================================================\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  Total Entries         : " << fix.engine->GetTotalEntries() << "\n";
    std::cout << "  Process VmRSS Total   : " << (static_cast<double>(rss_after) / 1024.0)
              << " MiB (50k Fixture Delta: ~" << rss_delta_mib << " MiB)\n";
    std::cout << "--------------------------------------------------------------------------------\n";
    std::cout << "  Operation (50,000 entries)             |   P50 (us) |   P95 (us) |   P99 (us)\n";
    std::cout << "--------------------------------------------------------------------------------\n";
    auto print_row = [](const char *name, const PercentileStats &s) {
        std::cout << "  " << std::left << std::setw(39) << name << "| "
                  << std::right << std::setw(10) << s.p50_us << " | "
                  << std::setw(10) << s.p95_us << " | "
                  << std::setw(10) << s.p99_us << "\n";
    };
    print_row("Exact Query (In-Memory Indexed)", exact_mem);
    print_row("Exact Query (LMDB mmap Zero-Copy)", exact_lmdb);
    print_row("Prefix Query [Before: Unindexed Scan]", prefix_before);
    print_row("Prefix Query [After: Ordered Index]", prefix_after_idx);
    print_row("Prefix Query [After: LMDB Range Cursor]", prefix_after_lmdb);
    print_row("Context Sanitize + Query + Full Rerank", full_rerank);
    std::cout << "================================================================================\n\n";

    // 热路径硬性门禁校验：50,000 词条下查询与重排 P99 必须低于 1ms (1000 us)
    if (exact_mem.p99_us >= 1000.0 || exact_lmdb.p99_us >= 1000.0 ||
        prefix_after_idx.p99_us >= 1000.0 ||
        prefix_after_lmdb.p99_us >= 1000.0 || full_rerank.p99_us >= 1000.0) {
        std::cerr << "Hot-path P99 latency exceeded 1ms target!\n";
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main(int argc, char **argv) {
    runPercentileAndMemoryVerification();
    verifyPublishedSnapshotLatency(50000);
    verifyPublishedSnapshotLatency(200000);
    verifyBusyWriterLatency();
    verifyLmdbWriterLatency(50000);
    verifyLmdbWriterLatency(200000);
    verifyLmdbConcurrentPruningLatency();
    verifyLmdbFirstObserverLatency();
    verifyLmdbFirstKeyAfterClearLatency();
    verifyStartupInitialCommitAndNextKeyLatency(50000);
    verifyStartupInitialCommitAndNextKeyLatency(200000);
    verifyRealCapacityPruningAndExpansion();
    ::benchmark::Initialize(&argc, argv);
    if (::benchmark::ReportUnrecognizedArguments(argc, argv)) {
        return 1;
    }
    ::benchmark::RunSpecifiedBenchmarks();
    ::benchmark::Shutdown();
    return 0;
}
