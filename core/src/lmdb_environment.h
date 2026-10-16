#pragma once

#include "modernime/core/personal_engine.h"

#include <lmdb.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace modernime::core::detail {

struct UnpackedRecordView {
    WordMeta meta{};
    std::string_view text;
    std::string_view context_before;
    std::string_view context_after;
    bool valid = false;
};

UnpackedRecordView unpackLmdbValue(const MDB_val &val) noexcept;
int compareLmdbWordRecord(const MDB_val *a, const MDB_val *b) noexcept;
std::vector<char> packLmdbValue(const WordMeta &meta,
                                std::string_view text,
                                std::string_view context_before,
                                std::string_view context_after);

class LmdbEnvironment final : public std::enable_shared_from_this<LmdbEnvironment> {
public:
    mutable std::shared_mutex lifecycle_gate;
    MDB_env *env = nullptr;
    MDB_dbi words_dbi = 0;
    MDB_dbi meta_dbi = 0;
    bool read_only = false;
    std::string path_key;
    std::filesystem::path path;

    explicit LmdbEnvironment(std::string path_key, std::filesystem::path path, bool read_only);
    ~LmdbEnvironment();

    LmdbEnvironment(const LmdbEnvironment &) = delete;
    LmdbEnvironment &operator=(const LmdbEnvironment &) = delete;

    bool initialize(size_t initial_map_size);
    bool upgradeToWritable(size_t initial_map_size);
    bool ensureMapCapacity(size_t required_extra_bytes);
    void close() noexcept;
};

class LmdbRegistry final {
public:
    static LmdbRegistry &instance();

    std::shared_ptr<LmdbEnvironment> getOrCreate(
        const std::filesystem::path &path,
        bool read_only,
        size_t initial_map_size);

    void unregister(const std::string &path_key, LmdbEnvironment *expected);

private:
    LmdbRegistry() = default;
    std::mutex registry_mutex_;
    std::unordered_map<std::string, std::weak_ptr<LmdbEnvironment>> slots_;
};

std::string normalizePathKey(const std::filesystem::path &p);

} // namespace modernime::core::detail
