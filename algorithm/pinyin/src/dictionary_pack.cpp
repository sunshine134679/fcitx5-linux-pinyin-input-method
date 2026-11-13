#include "modernime/pinyin/dictionary_pack.h"

#include "modernime/pinyin/mixed_word_index.h"
#include "modernime/core/pinyin_match.h"

#include <libime/pinyin/pinyinencoder.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <fstream>
#include <map>
#include <system_error>
#include <unistd.h>

namespace modernime::pinyin {
namespace {

bool fail(std::string *error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

bool validId(std::string_view id) {
    return !id.empty() && id.size() <= 80 &&
        std::all_of(id.begin(), id.end(), [](char character) {
            return (character >= 'a' && character <= 'z') ||
                (character >= '0' && character <= '9') || character == '-';
        });
}

bool ownedDirectory(const std::filesystem::path &path) {
    std::error_code error;
    return std::filesystem::is_directory(std::filesystem::symlink_status(path, error)) && !error;
}

std::atomic<unsigned> sequence{0};

} // namespace

DictionaryPackStore::DictionaryPackStore(std::filesystem::path root) : root_(std::move(root)) {}

std::optional<DictionaryPack> DictionaryPackStore::validate(
    const std::filesystem::path &directory, std::string *error) const {
    if (error) error->clear();
    if (!ownedDirectory(directory)) {
        fail(error, "词库包目录不存在或为符号链接");
        return {};
    }
    for (const auto *file : {"manifest.ini", "entries.tsv"}) {
        std::error_code fileError;
        const auto path = directory / file;
        const auto status = std::filesystem::symlink_status(path, fileError);
        if (fileError || !std::filesystem::is_regular_file(status) ||
            std::filesystem::file_size(path, fileError) > 32 * 1024 * 1024 || fileError) {
            fail(error, "词库包文件缺失、非普通文件或超过32MiB");
            return {};
        }
    }
    std::ifstream input(directory / "manifest.ini");
    std::map<std::string, std::string> fields;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.front() == '#') continue;
        const auto separator = line.find('=');
        if (separator == std::string::npos ||
            !fields.emplace(line.substr(0, separator), line.substr(separator + 1)).second) {
            fail(error, "manifest字段格式错误或重复");
            return {};
        }
    }
    DictionaryPack pack;
    auto &manifest = pack.manifest;
    manifest.id = fields["id"];
    manifest.name = fields["name"];
    manifest.source = fields["source"];
    manifest.license = fields["license"];
    manifest.version = fields["version"];
    const auto &count = fields["entry_count"];
    const auto parsed = std::from_chars(count.data(), count.data() + count.size(), manifest.entryCount);
    if (input.bad() || !validId(manifest.id) || manifest.name.empty() ||
        manifest.source.empty() || manifest.license.empty() || manifest.version.empty() ||
        parsed.ec != std::errc{} || parsed.ptr != count.data() + count.size() ||
        manifest.entryCount == 0 || manifest.entryCount > 200000) {
        fail(error, "manifest需有效ID、名称、来源、许可、版本和词条数(1–200000)");
        return {};
    }
    auto snapshot = UserDictionary::loadSnapshot(directory / "entries.tsv");
    if (!snapshot.readable || !snapshot.error.empty() || !snapshot.diagnostics.empty() ||
        snapshot.entries.size() != manifest.entryCount) {
        fail(error, "词条解析失败或词条数与manifest不符");
        return {};
    }
    for (const auto &entry : snapshot.entries) {
        if (entry.pinyin.size() > 128 || entry.phrase.size() > 512 || entry.weight > 50) {
            fail(error, "词条键/文本过长或权重超过50");
            return {};
        }
        if (!MixedWordIndex::isMixedWord(entry.phrase)) {
            try {
                libime::PinyinEncoder::encodeFullPinyin(
                    core::PinyinMatchPolicy::segmentPinyin(entry.pinyin));
            } catch (const std::exception &) {
                fail(error, "非混合词条包含无效的完整拼音");
                return {};
            }
        }
    }
    pack.entries = std::move(snapshot.entries);
    std::error_code stateError;
    manifest.enabled = !std::filesystem::exists(directory / "disabled", stateError);
    if (stateError) {
        fail(error, stateError.message());
        return {};
    }
    return pack;
}

std::vector<DictionaryPackManifest> DictionaryPackStore::list() const {
    std::vector<DictionaryPackManifest> result;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(root_, error), end;
         iterator != end && !error; iterator.increment(error)) {
        if (auto pack = validate(iterator->path())) {
            if (pack->manifest.id == iterator->path().filename())
                result.push_back(std::move(pack->manifest));
        }
    }
    std::sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
        return left.id < right.id;
    });
    return result;
}

std::vector<DictionaryPack> DictionaryPackStore::loadEnabled(std::string *error) const {
    if (error) error->clear();
    std::vector<DictionaryPack> result;
    std::error_code directoryError;
    if (!std::filesystem::exists(root_, directoryError) && !directoryError) return result;
    for (std::filesystem::directory_iterator iterator(root_, directoryError), end;
         iterator != end && !directoryError; iterator.increment(directoryError)) {
        if (iterator->path().filename().string().starts_with('.') ||
            !ownedDirectory(iterator->path())) continue;
        std::error_code manifestError;
        if (!std::filesystem::exists(iterator->path() / "manifest.ini", manifestError) && !manifestError)
            continue;
        std::string validationError;
        auto pack = validate(iterator->path(), &validationError);
        if (!pack || pack->manifest.id != iterator->path().filename()) {
            if (error) {
                *error = validationError.empty() ? "词库包目录与稳定ID不符" : validationError;
                return {};
            }
            continue;
        }
        if (pack->manifest.enabled) result.push_back(std::move(*pack));
    }
    if (directoryError && error) *error = directoryError.message();
    std::sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
        return left.manifest.id < right.manifest.id;
    });
    return result;
}

bool DictionaryPackStore::importPack(const std::filesystem::path &directory,
                                     bool replace, std::string *error) const {
    const auto pack = validate(directory, error);
    if (!pack) return false;
    std::error_code fileError;
    std::filesystem::create_directories(root_, fileError);
    if (fileError) return fail(error, fileError.message());
    const auto target = root_ / pack->manifest.id;
    const bool exists = std::filesystem::exists(target, fileError);
    if (fileError) return fail(error, fileError.message());
    if (exists && (!replace || !validate(target)))
        return fail(error, "同ID词库包已存在或目标不是受管词库包");
    const auto token = ".stage-" + std::to_string(getpid()) + "-" + std::to_string(sequence++);
    const auto stage = root_ / token;
    const auto backup = root_ / (token + "-old");
    std::filesystem::create_directory(stage, fileError);
    if (fileError) return fail(error, fileError.message());
    for (const auto *file : {"manifest.ini", "entries.tsv"}) {
        std::filesystem::copy_file(directory / file, stage / file, fileError);
        if (fileError) {
            const auto message = fileError.message();
            std::filesystem::remove_all(stage, fileError);
            return fail(error, message);
        }
    }
    // Updates retain installed activation state; no personal dictionary or
    // learning data is ever imported, removed or replaced by this operation.
    if (exists && std::filesystem::exists(target / "disabled")) {
        std::ofstream marker(stage / "disabled");
        marker.put('\n');
        marker.close();
        if (!marker) {
            std::filesystem::remove_all(stage, fileError);
            return fail(error, "无法保留词库包启停状态");
        }
    }
    if (exists) {
        std::filesystem::rename(target, backup, fileError);
        if (fileError) {
            const auto message = fileError.message();
            std::filesystem::remove_all(stage, fileError);
            return fail(error, message);
        }
    }
    std::filesystem::rename(stage, target, fileError);
    if (fileError) {
        const auto message = fileError.message();
        if (exists) std::filesystem::rename(backup, target, fileError);
        std::filesystem::remove_all(stage, fileError);
        return fail(error, message);
    }
    if (exists) std::filesystem::remove_all(backup, fileError);
    return true;
}

bool DictionaryPackStore::setEnabled(std::string_view id, bool enabled, std::string *error) const {
    if (!validId(id) || !validate(root_ / id, error))
        return fail(error, "未找到有效受管词库包");
    const auto markerPath = root_ / id / "disabled";
    std::error_code fileError;
    if (enabled) {
        std::filesystem::remove(markerPath, fileError);
    } else if (!std::filesystem::exists(markerPath, fileError) && !fileError) {
        std::ofstream marker(markerPath);
        marker.put('\n');
        marker.close();
        if (!marker) return fail(error, "保存词库启停状态失败");
    }
    return fileError ? fail(error, fileError.message()) : true;
}

bool DictionaryPackStore::remove(std::string_view id, std::string *error) const {
    if (!validId(id) || !validate(root_ / id, error))
        return fail(error, "仅可移除有效受管词库包");
    std::error_code fileError;
    std::filesystem::remove_all(root_ / id, fileError);
    return fileError ? fail(error, fileError.message()) : true;
}

} // namespace modernime::pinyin
