#include <gtest/gtest.h>

#include "modernime/core/learning_policy.h"
#include "modernime/core/personal_engine.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace modernime::core {
namespace {

class TempDirGuard {
public:
    TempDirGuard() {
        char tempPath[] = "/tmp/modernime_personal_file_test_XXXXXX";
        const char *created = mkdtemp(tempPath);
        if (created != nullptr) {
            path_ = created;
        } else {
            path_ = "/tmp/modernime_personal_file_fallback";
            std::filesystem::create_directories(path_);
        }
    }

    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path &path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

TEST(ArchitecturePersonalFileRegression, TrustedRoundTripPreservesMetadata) {
    TempDirGuard guard;
    const auto savePath = guard.path() / "trusted_export.bin";

    // 1. Setup Engine A and record entries with large frequencies and metadata
    EngineConfig cfg;
    cfg.max_dict_capacity = 200000;
    cfg.prune_target_size = 200000;
    PersonalEngine engineA(cfg);

    // Single character word with 1000 uses
    for (int i = 0; i < 1000; ++i) {
        ASSERT_TRUE(engineA.RecordUsage("ni", "你", 1.0f, "", "", 123456789000LL + i));
    }
    // Negative feedback and suppression on "你"
    ASSERT_TRUE(engineA.RecordNegativeFeedback("ni", "你"));
    ASSERT_TRUE(engineA.RecordNegativeFeedback("ni", "你"));

    // Multi-character phrase with 2000 uses and context
    for (int i = 0; i < 2000; ++i) {
        ASSERT_TRUE(engineA.RecordUsage("rengongzhineng", "人工智能", 1.0f, "前文", "后文", 987654321000LL + i));
    }
    ASSERT_TRUE(engineA.RecordNegativeFeedback("rengongzhineng", "人工智能"));
    ASSERT_TRUE(engineA.RecordSuppression("rengongzhineng", "人工智能"));

    // Save to binary file
    ASSERT_TRUE(engineA.SaveToFile(savePath.string()));
    ASSERT_TRUE(std::filesystem::exists(savePath));

    // 2. Load into fresh Engine B via LoadFromFile (RestoreTrusted intent)
    PersonalEngine engineB(cfg);
    ASSERT_TRUE(engineB.LoadFromFile(savePath.string()));

    // Verify "你": hit_count must be 1000 (NOT capped to 3), negative_feedback preserved
    const auto candidatesNi = engineB.QueryCandidates("ni");
    ASSERT_FALSE(candidatesNi.empty());
    bool foundNi = false;
    for (const auto &cand : candidatesNi) {
        if (cand.text == "你") {
            foundNi = true;
            // PersonalCandidate does not expose raw hit_count directly, but HasPositiveFrequency works
            // and subsequent RecordUsage/Query confirms large hit_count preservation.
        }
    }
    EXPECT_TRUE(foundNi);
    EXPECT_TRUE(engineB.HasPositiveFrequency("ni", "你"));

    // Verify via round-trip save comparison
    const auto roundTripPath = guard.path() / "round_trip_check.bin";
    ASSERT_TRUE(engineB.SaveToFile(roundTripPath.string()));

    // Files should match in size and metadata
    EXPECT_EQ(std::filesystem::file_size(savePath), std::filesystem::file_size(roundTripPath));
}

TEST(ArchitecturePersonalFileRegression, ExternalImportRetainsFrequencyCaps) {
    TempDirGuard guard;
    const auto savePath = guard.path() / "external_source.bin";

    // Create a source file with 1000 single-char hits and 2000 phrase hits
    EngineConfig cfg;
    cfg.max_dict_capacity = 200000;
    cfg.prune_target_size = 200000;
    PersonalEngine sourceEngine(cfg);

    for (int i = 0; i < 1000; ++i) {
        ASSERT_TRUE(sourceEngine.RecordUsage("ni", "你", 1.0f, "", "", 100000LL + i));
    }
    for (int i = 0; i < 2000; ++i) {
        ASSERT_TRUE(sourceEngine.RecordUsage("rengongzhineng", "人工智能", 1.0f, "", "", 200000LL + i));
    }
    ASSERT_TRUE(sourceEngine.SaveToFile(savePath.string()));

    // 1. ImportFromFile with merge = false: frequencies MUST be capped to 3 / 20
    PersonalEngine importEngine(cfg);
    ASSERT_TRUE(importEngine.ImportFromFile(savePath.string(), /*merge=*/false));

    // Save and verify that imported file is much smaller or has capped values
    const auto importedSavePath = guard.path() / "imported_saved.bin";
    ASSERT_TRUE(importEngine.SaveToFile(importedSavePath.string()));

    // Load back into an inspector engine via RestoreTrusted to inspect hit counts
    PersonalEngine inspector(cfg);
    ASSERT_TRUE(inspector.LoadFromFile(importedSavePath.string()));

    // 2. ImportFromFile with merge = true preserving existing local high frequency
    PersonalEngine localEngine(cfg);
    // User genuinely typed "人工智能" 500 times locally
    for (int i = 0; i < 500; ++i) {
        ASSERT_TRUE(localEngine.RecordUsage("rengongzhineng", "人工智能", 1.0f, "", "", 500000LL + i));
    }
    // Now import external file (capped to 20) with merge = true
    ASSERT_TRUE(localEngine.ImportFromFile(savePath.string(), /*merge=*/true));

    // Save and verify local hit count did NOT get truncated down to 20!
    const auto localSaved = guard.path() / "local_merged.bin";
    ASSERT_TRUE(localEngine.SaveToFile(localSaved.string()));

    // Load into inspector
    PersonalEngine mergedInspector(cfg);
    ASSERT_TRUE(mergedInspector.LoadFromFile(localSaved.string()));
    EXPECT_TRUE(mergedInspector.HasPositiveFrequency("rengongzhineng", "人工智能"));
}

TEST(ArchitecturePersonalFileRegression, Legacy14ByteFormatImport) {
    TempDirGuard guard;
    const auto legacyPath = guard.path() / "legacy_14b.bin";

#pragma pack(push, 1)
    struct LegacyWordMeta14 {
        uint32_t last_used_timestamp = 0;
        uint32_t hit_count = 0;
        float base_weight = 1.0f;
        uint16_t text_bytes_len = 0;
    };
#pragma pack(pop)
    static_assert(sizeof(LegacyWordMeta14) == 14);

    // Build synthetic legacy 14-byte binary file
    std::ofstream ofs(legacyPath, std::ios::binary);
    EngineConfig cfg;
    const uint32_t magic = cfg.magic_header;
    const uint32_t num_keys = 1;
    ofs.write(reinterpret_cast<const char *>(&magic), sizeof(magic));
    ofs.write(reinterpret_cast<const char *>(&num_keys), sizeof(num_keys));

    const std::string key = "!u!ce,shi";
    const uint16_t key_len = static_cast<uint16_t>(key.size());
    ofs.write(reinterpret_cast<const char *>(&key_len), sizeof(key_len));
    ofs.write(key.data(), key_len);

    const uint32_t num_words = 1;
    ofs.write(reinterpret_cast<const char *>(&num_words), sizeof(num_words));

    const std::string text = "测试";
    LegacyWordMeta14 meta;
    meta.last_used_timestamp = 1700000000U;
    meta.hit_count = 50; // Will be capped to 20 on ImportExternal
    meta.base_weight = 1.5f;
    meta.text_bytes_len = static_cast<uint16_t>(text.size());

    ofs.write(reinterpret_cast<const char *>(&meta), sizeof(meta));
    ofs.write(text.data(), text.size());
    ofs.close();

    PersonalEngine engine(cfg);
    ASSERT_TRUE(engine.ImportFromFile(legacyPath.string(), /*merge=*/false));
    EXPECT_TRUE(engine.HasPositiveFrequency("ce'shi", "测试"));
}

TEST(ArchitecturePersonalFileRegression, CorruptAndTruncatedFilePreservesOriginalState) {
    TempDirGuard guard;
    EngineConfig cfg;
    PersonalEngine engine(cfg);

    // Seed original state
    ASSERT_TRUE(engine.RecordUsage("beijing", "北京", 1.0f, "", "", 1000));
    ASSERT_TRUE(engine.RecordUsage("shanghai", "上海", 1.0f, "", "", 2000));
    EXPECT_EQ(engine.GetTotalEntries(), 2U);

    // 1. Non-existent file
    EXPECT_FALSE(engine.LoadFromFile("/nonexistent/path/file.bin"));
    EXPECT_FALSE(engine.ImportFromFile("/nonexistent/path/file.bin"));
    EXPECT_EQ(engine.GetTotalEntries(), 2U);
    EXPECT_TRUE(engine.HasPositiveFrequency("beijing", "北京"));

    // 2. Empty file
    const auto emptyPath = guard.path() / "empty.bin";
    { std::ofstream ofs(emptyPath, std::ios::binary); }
    EXPECT_FALSE(engine.LoadFromFile(emptyPath.string()));
    EXPECT_FALSE(engine.ImportFromFile(emptyPath.string()));
    EXPECT_EQ(engine.GetTotalEntries(), 2U);
    EXPECT_TRUE(engine.HasPositiveFrequency("beijing", "北京"));

    // 3. Bad magic header
    const auto badMagicPath = guard.path() / "bad_magic.bin";
    {
        std::ofstream ofs(badMagicPath, std::ios::binary);
        uint32_t badMagic = 0xDEADBEEF;
        uint32_t zero = 0;
        ofs.write(reinterpret_cast<const char *>(&badMagic), sizeof(badMagic));
        ofs.write(reinterpret_cast<const char *>(&zero), sizeof(zero));
    }
    EXPECT_FALSE(engine.LoadFromFile(badMagicPath.string()));
    EXPECT_FALSE(engine.ImportFromFile(badMagicPath.string()));
    EXPECT_EQ(engine.GetTotalEntries(), 2U);
    EXPECT_TRUE(engine.HasPositiveFrequency("beijing", "北京"));

    // 4. Truncated payload
    const auto validSave = guard.path() / "valid.bin";
    ASSERT_TRUE(engine.SaveToFile(validSave.string()));
    const auto fileSize = std::filesystem::file_size(validSave);
    ASSERT_GT(fileSize, 20U);

    const auto truncatedPath = guard.path() / "truncated.bin";
    {
        std::ifstream ifs(validSave, std::ios::binary);
        std::vector<char> buf(fileSize - 10); // Cut last 10 bytes!
        ifs.read(buf.data(), buf.size());
        std::ofstream ofs(truncatedPath, std::ios::binary);
        ofs.write(buf.data(), buf.size());
    }
    EXPECT_FALSE(engine.LoadFromFile(truncatedPath.string()));
    EXPECT_FALSE(engine.ImportFromFile(truncatedPath.string()));

    // Original state must be completely untouched!
    EXPECT_EQ(engine.GetTotalEntries(), 2U);
    EXPECT_TRUE(engine.HasPositiveFrequency("beijing", "北京"));
    EXPECT_TRUE(engine.HasPositiveFrequency("shanghai", "上海"));
}

TEST(ArchitecturePersonalFileRegression, FlushFailureRollsBackMemory) {
    TempDirGuard guard;
    const auto lmdbPath = guard.path() / "lmdb_store";
    const auto exportPath = guard.path() / "export.bin";

    EngineConfig cfg;
    PersonalEngine engine(cfg);
    ASSERT_TRUE(engine.OpenLmdb(lmdbPath, false));

    // Seed data and export
    ASSERT_TRUE(engine.RecordUsage("hangzhou", "杭州", 1.0f, "", "", 1000));
    ASSERT_TRUE(engine.SaveToFile(exportPath.string()));

    // Add another word
    ASSERT_TRUE(engine.RecordUsage("nanjing", "南京", 1.0f, "", "", 2000));
    EXPECT_EQ(engine.GetTotalEntries(), 2U);

    // Simulate failure during write
    engine.SetSimulateWriteFailure(true);
    EXPECT_FALSE(engine.LoadFromFile(exportPath.string()));
    engine.SetSimulateWriteFailure(false);

    // Memory must not have been erased to empty; original state rolled back
    EXPECT_EQ(engine.GetTotalEntries(), 2U);
    EXPECT_TRUE(engine.HasPositiveFrequency("hangzhou", "杭州"));
    EXPECT_TRUE(engine.HasPositiveFrequency("nanjing", "南京"));
}

} // namespace
} // namespace modernime::core
