#include "lmdb_environment.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <system_error>

namespace modernime::core::detail {

namespace {

constexpr int64_t kLmdbSchemaVersion = 2;

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

std::string normalizePathKey(const std::filesystem::path &p) {
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(p, ec);
    if (!ec && !canon.empty()) {
        return canon.string();
    }
    return p.lexically_normal().string();
}

LmdbEnvironment::LmdbEnvironment(std::string path_key_in, std::filesystem::path path_in, bool read_only_in)
    : read_only(read_only_in), path_key(std::move(path_key_in)), path(std::move(path_in)) {}

LmdbEnvironment::~LmdbEnvironment() {
    close();
    LmdbRegistry::instance().unregister(path_key, this);
}

void LmdbEnvironment::close() noexcept {
    std::unique_lock lock(lifecycle_gate);
    if (env != nullptr) {
        mdb_env_close(env);
        env = nullptr;
        words_dbi = 0;
        meta_dbi = 0;
    }
}

bool LmdbEnvironment::initialize(size_t initial_map_size) {
    std::unique_lock lock(lifecycle_gate);
    if (env != nullptr) {
        return true;
    }
    MDB_env *new_env = nullptr;
    if (mdb_env_create(&new_env) != MDB_SUCCESS) {
        return false;
    }
    if (mdb_env_set_maxdbs(new_env, 4) != MDB_SUCCESS ||
        mdb_env_set_mapsize(new_env, std::max<size_t>(64 * 1024, initial_map_size)) != MDB_SUCCESS) {
        mdb_env_close(new_env);
        return false;
    }

    unsigned int flags = MDB_NOSUBDIR | MDB_NOTLS;
    if (read_only) {
        flags |= MDB_RDONLY;
    }
    if (mdb_env_open(new_env, path.c_str(), flags, 0644) != MDB_SUCCESS) {
        mdb_env_close(new_env);
        return false;
    }

    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(new_env, nullptr, read_only ? MDB_RDONLY : 0, &txn) != MDB_SUCCESS) {
        mdb_env_close(new_env);
        return false;
    }

    const unsigned int words_flags = read_only ? MDB_DUPSORT : (MDB_CREATE | MDB_DUPSORT);
    const unsigned int meta_flags = read_only ? 0 : MDB_CREATE;
    if (mdb_dbi_open(txn, "words", words_flags, &words_dbi) != MDB_SUCCESS ||
        mdb_set_dupsort(txn, words_dbi, &compareLmdbWordRecord) != MDB_SUCCESS ||
        mdb_dbi_open(txn, "meta", meta_flags, &meta_dbi) != MDB_SUCCESS) {
        if (txn != nullptr) {
            mdb_txn_abort(txn);
        }
        mdb_env_close(new_env);
        return false;
    }

    if (!read_only) {
        if (!getMetaInt64(txn, meta_dbi, "schema_version").has_value()) {
            putMetaInt64(txn, meta_dbi, "schema_version", kLmdbSchemaVersion);
        }
        if (!getMetaInt64(txn, meta_dbi, "reset_revision").has_value()) {
            putMetaInt64(txn, meta_dbi, "reset_revision", 0);
        }
    }
    if (mdb_txn_commit(txn) != MDB_SUCCESS) {
        mdb_env_close(new_env);
        return false;
    }

    env = new_env;
    return true;
}

bool LmdbEnvironment::upgradeToWritable(size_t initial_map_size) {
    std::unique_lock lock(lifecycle_gate);
    if (!read_only && env != nullptr) {
        return true;
    }
    if (env != nullptr) {
        mdb_env_close(env);
        env = nullptr;
        words_dbi = 0;
        meta_dbi = 0;
    }

    MDB_env *upgraded_env = nullptr;
    if (mdb_env_create(&upgraded_env) != MDB_SUCCESS ||
        mdb_env_set_maxdbs(upgraded_env, 4) != MDB_SUCCESS ||
        mdb_env_set_mapsize(upgraded_env, std::max<size_t>(64 * 1024, initial_map_size)) != MDB_SUCCESS ||
        mdb_env_open(upgraded_env, path.c_str(), MDB_NOSUBDIR | MDB_NOTLS, 0644) != MDB_SUCCESS) {
        if (upgraded_env != nullptr) {
            mdb_env_close(upgraded_env);
        }
        return false;
    }

    MDB_txn *txn = nullptr;
    if (mdb_txn_begin(upgraded_env, nullptr, 0, &txn) != MDB_SUCCESS ||
        mdb_dbi_open(txn, "words", MDB_CREATE | MDB_DUPSORT, &words_dbi) != MDB_SUCCESS ||
        mdb_set_dupsort(txn, words_dbi, &compareLmdbWordRecord) != MDB_SUCCESS ||
        mdb_dbi_open(txn, "meta", MDB_CREATE, &meta_dbi) != MDB_SUCCESS) {
        if (txn != nullptr) {
            mdb_txn_abort(txn);
        }
        mdb_env_close(upgraded_env);
        return false;
    }

    if (!getMetaInt64(txn, meta_dbi, "schema_version").has_value()) {
        putMetaInt64(txn, meta_dbi, "schema_version", kLmdbSchemaVersion);
    }
    if (!getMetaInt64(txn, meta_dbi, "reset_revision").has_value()) {
        putMetaInt64(txn, meta_dbi, "reset_revision", 0);
    }
    if (mdb_txn_commit(txn) != MDB_SUCCESS) {
        mdb_env_close(upgraded_env);
        return false;
    }

    env = upgraded_env;
    read_only = false;
    return true;
}

bool LmdbEnvironment::ensureMapCapacity(size_t required_extra_bytes) {
    std::unique_lock lock(lifecycle_gate);
    if (env == nullptr || read_only) {
        return false;
    }
    MDB_envinfo info{};
    if (mdb_env_info(env, &info) != MDB_SUCCESS) {
        return false;
    }
    size_t next_size = std::max<size_t>(info.me_mapsize * 2, 128 * 1024);
    if (required_extra_bytes > 0 && info.me_mapsize + required_extra_bytes * 4 > next_size) {
        next_size = info.me_mapsize + required_extra_bytes * 4;
    }
    // 4 KiB page aligned
    next_size = (next_size + 4095ULL) & ~4095ULL;
    return mdb_env_set_mapsize(env, next_size) == MDB_SUCCESS;
}

LmdbRegistry &LmdbRegistry::instance() {
    static LmdbRegistry reg;
    return reg;
}

std::shared_ptr<LmdbEnvironment> LmdbRegistry::getOrCreate(
    const std::filesystem::path &path,
    bool read_only,
    size_t initial_map_size) {
    const std::string path_key = normalizePathKey(path);
    std::shared_ptr<LmdbEnvironment> handle;
    {
        std::lock_guard lock(registry_mutex_);
        auto it = slots_.find(path_key);
        if (it != slots_.end()) {
            handle = it->second.lock();
        }
        if (!handle) {
            handle = std::make_shared<LmdbEnvironment>(path_key, path, read_only);
            slots_[path_key] = handle;
        }
    }

    bool ok = true;
    bool need_init = false;
    bool need_upgrade = false;
    {
        std::shared_lock env_lock(handle->lifecycle_gate);
        need_init = (handle->env == nullptr);
        need_upgrade = (handle->read_only && !read_only);
    }
    if (need_init) {
        ok = handle->initialize(initial_map_size);
    } else if (need_upgrade) {
        ok = handle->upgradeToWritable(initial_map_size);
    }
    if (!ok) {
        unregister(path_key, handle.get());
        return nullptr;
    }
    return handle;
}

void LmdbRegistry::unregister(const std::string &path_key, LmdbEnvironment *expected) {
    std::lock_guard lock(registry_mutex_);
    auto it = slots_.find(path_key);
    if (it != slots_.end()) {
        auto existing = it->second.lock();
        if (!existing || existing.get() == expected) {
            slots_.erase(it);
        }
    }
}

} // namespace modernime::core::detail
