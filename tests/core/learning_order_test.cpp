#include <gtest/gtest.h>

#include "modernime/core/candidate_model.h"
#include "modernime/core/candidate_ranker.h"
#include "modernime/core/learning_snapshot.h"
#include "modernime/core/personal_engine.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

namespace modernime::core {
namespace {

// Helper to check strict weak ordering properties on any comparator over a set of items.
template <typename T, typename Comparator>
void AssertStrictWeakOrdering(const std::vector<T> &items, Comparator comp) {
    const size_t n = items.size();
    auto equiv = [&](const T &a, const T &b) {
        return !comp(a, b) && !comp(b, a);
    };

    // 1. Irreflexivity: !(x < x)
    for (size_t i = 0; i < n; ++i) {
        EXPECT_FALSE(comp(items[i], items[i])) << "Irreflexivity violated at index " << i;
    }

    // 2. Asymmetry: x < y => !(y < x)
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < n; ++j) {
            if (comp(items[i], items[j])) {
                EXPECT_FALSE(comp(items[j], items[i]))
                    << "Asymmetry violated at (" << i << ", " << j << ")";
            }
        }
    }

    // 3. Transitivity: x < y and y < z => x < z
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < n; ++j) {
            if (!comp(items[i], items[j])) continue;
            for (size_t k = 0; k < n; ++k) {
                if (comp(items[j], items[k])) {
                    EXPECT_TRUE(comp(items[i], items[k]))
                        << "Transitivity violated at (" << i << ", " << j << ", " << k << ")";
                }
            }
        }
    }

    // 4. Transitivity of equivalence: x ~ y and y ~ z => x ~ z
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < n; ++j) {
            if (!equiv(items[i], items[j])) continue;
            for (size_t k = 0; k < n; ++k) {
                if (equiv(items[j], items[k])) {
                    EXPECT_TRUE(equiv(items[i], items[k]))
                        << "Transitivity of equivalence violated at (" << i << ", " << j << ", " << k << ")";
                }
            }
        }
    }
}

// Comparator definition for PersonalCandidate used across Query methods
auto PersonalCandidateComparator = [](const PersonalCandidate &a, const PersonalCandidate &b) {
    const bool aFin = std::isfinite(a.dynamic_score);
    const bool bFin = std::isfinite(b.dynamic_score);
    if (aFin != bFin) {
        return aFin;
    }
    if (aFin && a.dynamic_score != b.dynamic_score) {
        return a.dynamic_score > b.dynamic_score;
    }
    if (a.meta.hit_count != b.meta.hit_count) {
        return a.meta.hit_count > b.meta.hit_count;
    }
    if (a.meta.last_selected_ms != b.meta.last_selected_ms) {
        return a.meta.last_selected_ms > b.meta.last_selected_ms;
    }
    if (a.text != b.text) {
        return a.text < b.text;
    }
    return a.raw_pinyin < b.raw_pinyin;
};

struct ScoredRefTest {
    std::string key;
    size_t index = 0;
    bool suppressed = false;
    float score = 0.0f;
    int64_t last_ms = 0;
    uint32_t hit_count = 0;
};

auto PruneComparator = [](const ScoredRefTest &a, const ScoredRefTest &b) {
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
};

auto EnglishWordComparator = [](const RecalledEnglishWord &a, const RecalledEnglishWord &b) {
    const bool aFin = std::isfinite(a.score);
    const bool bFin = std::isfinite(b.score);
    if (aFin != bFin) return aFin;
    if (aFin && a.score != b.score) return a.score > b.score;
    if (a.frequency != b.frequency) return a.frequency > b.frequency;
    if (a.lastSelectedMs != b.lastSelectedMs) return a.lastSelectedMs > b.lastSelectedMs;
    return a.text < b.text;
};

TEST(ArchitectureLearningOrderRegression, EpsilonTripleIsTransitive) {
    constexpr float eps = 1e-6f;
    const float val0 = 0.0f;
    const float val1 = 0.75f * eps;
    const float val2 = 1.5f * eps;

    // Construct 3 candidates with scores 0.0, 0.75e-6, 1.5e-6
    PersonalCandidate c0;
    c0.text = "zero";
    c0.raw_pinyin = "ling";
    c0.dynamic_score = val0;
    c0.meta.hit_count = 10;
    c0.meta.last_selected_ms = 1000;

    PersonalCandidate c1;
    c1.text = "point_seven_five";
    c1.raw_pinyin = "ling";
    c1.dynamic_score = val1;
    c1.meta.hit_count = 10;
    c1.meta.last_selected_ms = 1000;

    PersonalCandidate c2;
    c2.text = "one_point_five";
    c2.raw_pinyin = "ling";
    c2.dynamic_score = val2;
    c2.meta.hit_count = 10;
    c2.meta.last_selected_ms = 1000;

    // Verify all 6 permutations sort to exactly the same order: c2, c1, c0
    std::vector<PersonalCandidate> base = {c0, c1, c2};
    std::vector<int> perm = {0, 1, 2};
    int perm_count = 0;
    do {
        std::vector<PersonalCandidate> current = {base[perm[0]], base[perm[1]], base[perm[2]]};
        std::sort(current.begin(), current.end(), PersonalCandidateComparator);
        EXPECT_EQ(current[0].text, "one_point_five");
        EXPECT_EQ(current[1].text, "point_seven_five");
        EXPECT_EQ(current[2].text, "zero");
        ++perm_count;
    } while (std::next_permutation(perm.begin(), perm.end()));
    EXPECT_EQ(perm_count, 6);

    // Verify strict weak ordering on the epsilon triple + variations
    std::vector<PersonalCandidate> test_set = {c0, c1, c2};
    AssertStrictWeakOrdering(test_set, PersonalCandidateComparator);

    // Test equivalence transitivity with identical scores and identical fields
    PersonalCandidate dup0 = c0;
    PersonalCandidate dup0_b = c0;
    PersonalCandidate dup0_c = c0;
    std::vector<PersonalCandidate> eq_set = {dup0, dup0_b, dup0_c};
    AssertStrictWeakOrdering(eq_set, PersonalCandidateComparator);

    // Test ties in score broken by hit_count, timestamp, text, raw_pinyin
    PersonalCandidate tieA = c0;
    tieA.text = "alpha";
    tieA.dynamic_score = 1.0f;
    tieA.meta.hit_count = 5;
    tieA.meta.last_selected_ms = 200;

    PersonalCandidate tieB = c0;
    tieB.text = "beta";
    tieB.dynamic_score = 1.0f;
    tieB.meta.hit_count = 10; // higher hit_count wins
    tieB.meta.last_selected_ms = 100;

    PersonalCandidate tieC = c0;
    tieC.text = "charlie";
    tieC.dynamic_score = 1.0f;
    tieC.meta.hit_count = 5;
    tieC.meta.last_selected_ms = 300; // higher timestamp wins over tieA

    std::vector<PersonalCandidate> ties = {tieA, tieB, tieC};
    std::vector<int> tie_perm = {0, 1, 2};
    do {
        std::vector<PersonalCandidate> current = {ties[tie_perm[0]], ties[tie_perm[1]], ties[tie_perm[2]]};
        std::sort(current.begin(), current.end(), PersonalCandidateComparator);
        // tieB has hit_count 10, tieC has hit_count 5 ms 300, tieA has hit_count 5 ms 200
        EXPECT_EQ(current[0].text, "beta");
        EXPECT_EQ(current[1].text, "charlie");
        EXPECT_EQ(current[2].text, "alpha");
    } while (std::next_permutation(tie_perm.begin(), tie_perm.end()));

    std::vector<PersonalCandidate> all_ties = {c0, c1, c2, tieA, tieB, tieC};
    AssertStrictWeakOrdering(all_ties, PersonalCandidateComparator);
}

TEST(ArchitectureLearningOrderRegression, FiniteVsNonFiniteOrdering) {
    const float nanVal = std::numeric_limits<float>::quiet_NaN();
    const float infVal = std::numeric_limits<float>::infinity();
    const float negInfVal = -std::numeric_limits<float>::infinity();

    PersonalCandidate finiteHigh;
    finiteHigh.text = "high";
    finiteHigh.dynamic_score = 10.0f;

    PersonalCandidate finiteLow;
    finiteLow.text = "low";
    finiteLow.dynamic_score = -5.0f;

    PersonalCandidate nanCandA;
    nanCandA.text = "nan_a";
    nanCandA.dynamic_score = nanVal;
    nanCandA.meta.hit_count = 20;

    PersonalCandidate nanCandB;
    nanCandB.text = "nan_b";
    nanCandB.dynamic_score = nanVal;
    nanCandB.meta.hit_count = 10;

    PersonalCandidate infCand;
    infCand.text = "inf";
    infCand.dynamic_score = infVal;
    infCand.meta.hit_count = 5;

    PersonalCandidate negInfCand;
    negInfCand.text = "neg_inf";
    negInfCand.dynamic_score = negInfVal;
    negInfCand.meta.hit_count = 2;

    std::vector<PersonalCandidate> mixed = {
        nanCandA, finiteLow, infCand, finiteHigh, negInfCand, nanCandB
    };

    AssertStrictWeakOrdering(mixed, PersonalCandidateComparator);

    std::sort(mixed.begin(), mixed.end(), PersonalCandidateComparator);

    // Finite candidates must precede all non-finite candidates
    EXPECT_EQ(mixed[0].text, "high");
    EXPECT_EQ(mixed[1].text, "low");

    // All non-finite candidates unified as lowest effective evidence, ordered by stable tie-breaker (hit_count desc)
    EXPECT_EQ(mixed[2].text, "nan_a"); // hit_count 20
    EXPECT_EQ(mixed[3].text, "nan_b"); // hit_count 10
    EXPECT_EQ(mixed[4].text, "inf");   // hit_count 5
    EXPECT_EQ(mixed[5].text, "neg_inf"); // hit_count 2
}

TEST(ArchitectureLearningOrderRegression, SignedZeroTreatedAsEqualScore) {
    PersonalCandidate posZero;
    posZero.text = "pos_zero";
    posZero.dynamic_score = +0.0f;
    posZero.meta.hit_count = 5;
    posZero.meta.last_selected_ms = 100;

    PersonalCandidate negZero;
    negZero.text = "neg_zero";
    negZero.dynamic_score = -0.0f;
    negZero.meta.hit_count = 10; // Should win tie-break
    negZero.meta.last_selected_ms = 100;

    std::vector<PersonalCandidate> zeros = {posZero, negZero};
    AssertStrictWeakOrdering(zeros, PersonalCandidateComparator);

    std::sort(zeros.begin(), zeros.end(), PersonalCandidateComparator);
    // Equal score falls through to hit_count desc, so negZero (hit_count 10) comes first
    EXPECT_EQ(zeros[0].text, "neg_zero");
    EXPECT_EQ(zeros[1].text, "pos_zero");
}

TEST(ArchitectureLearningOrderRegression, PruneDeterministicStrictWeakOrdering) {
    ScoredRefTest unsuppressedHigh{"key1", 0, false, 10.0f, 100, 1};
    ScoredRefTest unsuppressedMid{"key2", 0, false, 5.0f, 100, 1};
    ScoredRefTest unsuppressedNan{"key3", 0, false, std::numeric_limits<float>::quiet_NaN(), 100, 1};
    ScoredRefTest suppressedHigh{"key4", 0, true, 10.0f, 100, 1};
    ScoredRefTest tieKeyA{"apple", 0, false, 5.0f, 100, 1};
    ScoredRefTest tieKeyB{"banana", 1, false, 5.0f, 100, 1};

    std::vector<ScoredRefTest> items = {
        suppressedHigh, unsuppressedNan, tieKeyB, unsuppressedHigh, tieKeyA, unsuppressedMid
    };

    AssertStrictWeakOrdering(items, PruneComparator);

    std::sort(items.begin(), items.end(), PruneComparator);

    // Unsuppressed items precede suppressed
    EXPECT_FALSE(items[0].suppressed);
    EXPECT_FALSE(items[1].suppressed);
    EXPECT_FALSE(items[2].suppressed);
    EXPECT_FALSE(items[3].suppressed);
    EXPECT_FALSE(items[4].suppressed);
    EXPECT_TRUE(items[5].suppressed);

    // Finite scores precede NaN
    EXPECT_EQ(items[0].score, 10.0f);
    // Items with score 5.0f tie-break by key asc ("apple" < "banana" < "key2")
    EXPECT_EQ(items[1].key, "apple");
    EXPECT_EQ(items[2].key, "banana");
    EXPECT_EQ(items[3].key, "key2");
    // NaN comes after finite unsuppressed
    EXPECT_TRUE(std::isnan(items[4].score));
}

TEST(ArchitectureLearningOrderRegression, EnglishWordStrictWeakOrdering) {
    RecalledEnglishWord w1{"world", 100, 1000, 2.5};
    RecalledEnglishWord w2{"word", 100, 1000, 2.5}; // score tie, frequency tie, timestamp tie -> text asc
    RecalledEnglishWord w3{"work", 50, 1000, 2.5};  // lower frequency
    RecalledEnglishWord w4{"won", 100, 1000, 3.0};  // higher score
    RecalledEnglishWord wNan{"nan", 100, 1000, std::numeric_limits<double>::quiet_NaN()};

    std::vector<RecalledEnglishWord> words = {w1, w2, w3, w4, wNan};
    AssertStrictWeakOrdering(words, EnglishWordComparator);

    std::sort(words.begin(), words.end(), EnglishWordComparator);
    EXPECT_EQ(words[0].text, "won");
    EXPECT_EQ(words[1].text, "word");  // "word" < "world"
    EXPECT_EQ(words[2].text, "world");
    EXPECT_EQ(words[3].text, "work");
    EXPECT_EQ(words[4].text, "nan");
}

TEST(ArchitectureLearningOrderRegression, CandidateRankerStrictWeakOrderingWithNonFinite) {
    std::vector<CandidateScore> candidates;

    CandidateScore cFinite1;
    cFinite1.text = "好的";
    cFinite1.full_pinyin = "haode";
    cFinite1.source_index = 0;
    cFinite1.learning_boost = 0.5;

    CandidateScore cFinite2;
    cFinite2.text = "号的";
    cFinite2.full_pinyin = "haode";
    cFinite2.source_index = 1;
    cFinite2.learning_boost = 0.1;

    CandidateScore cNan;
    cNan.text = "豪的";
    cNan.full_pinyin = "haode";
    cNan.source_index = 2;
    cNan.learning_boost = std::numeric_limits<double>::quiet_NaN();

    CandidateScore cInf;
    cInf.text = "浩的";
    cInf.full_pinyin = "haode";
    cInf.source_index = 3;
    cInf.learning_boost = -std::numeric_limits<double>::infinity();

    candidates = {cNan, cFinite2, cInf, cFinite1};

    // Rank should handle non-finite scores gracefully without crashing or violating ordering
    const auto order = CandidateRanker::rank("haode", candidates);
    ASSERT_EQ(order.size(), 4);

    // Finite candidates must be ranked before non-finite candidates
    // cFinite1 has higher learning boost (lower cost), so should be first
    EXPECT_EQ(candidates[order[0]].text, "好的");
    EXPECT_EQ(candidates[order[1]].text, "号的");
    // Non-finite candidates ranked afterwards
    const std::string third = candidates[order[2]].text;
    const std::string fourth = candidates[order[3]].text;
    EXPECT_TRUE(third == "豪的" || third == "浩的");
    EXPECT_TRUE(fourth == "豪的" || fourth == "浩的");
}

TEST(ArchitectureLearningOrderRegression, PersonalEngineQueryEpsilonTriple) {
    EngineConfig cfg;
    PersonalEngine engine(cfg);

    const std::string key = "!u!ce,shi";
    const uint32_t now = 100000;

    // Record words with hit_counts that yield close scores
    engine.RecordUsage(key, "测试一", 1.0f, "", "", static_cast<int64_t>(now) * 1000LL);
    engine.RecordUsage(key, "测试二", 1.0f, "", "", static_cast<int64_t>(now) * 1000LL);
    engine.RecordUsage(key, "测试三", 1.0f, "", "", static_cast<int64_t>(now) * 1000LL);

    const auto results = engine.QueryCandidates(key);
    ASSERT_EQ(results.size(), 3);

    // Assert that querying multiple times yields identical deterministic order
    for (int iter = 0; iter < 10; ++iter) {
        const auto check = engine.QueryCandidates(key);
        ASSERT_EQ(check.size(), 3);
        EXPECT_EQ(check[0].text, results[0].text);
        EXPECT_EQ(check[1].text, results[1].text);
        EXPECT_EQ(check[2].text, results[2].text);
    }
}

} // namespace
} // namespace modernime::core
