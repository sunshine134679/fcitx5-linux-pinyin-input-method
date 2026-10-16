#include "modernime/pinyin/pinyin_shared_resources.h"
#include "modernime/pinyin/user_dictionary.h"

#include "modernime/core/pinyin_match.h"
#include "modernime/core/settings.h"

#include <libime/core/userlanguagemodel.h>
#include <libime/pinyin/pinyindictionary.h>
#include <libime/pinyin/pinyinime.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

namespace {

inline libime::PinyinFuzzyFlags getDefaultFuzzyFlags() {
    libime::PinyinFuzzyFlags flags{
        libime::PinyinFuzzyFlag::Z_ZH,
        libime::PinyinFuzzyFlag::C_CH,
        libime::PinyinFuzzyFlag::S_SH,
        libime::PinyinFuzzyFlag::L_N,
        libime::PinyinFuzzyFlag::EN_ENG,
        libime::PinyinFuzzyFlag::IN_ING,
    };
#ifdef MODERNIME_HAS_PINYIN_COMMON_TYPO
    flags |= libime::PinyinFuzzyFlag::CommonTypo;
#endif
    return flags;
}

bool isRegularFile(const std::filesystem::path &path) {
    if (path.empty()) {
        return false;
    }
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

void appendDataDirectoryCandidates(
    std::vector<std::filesystem::path> &candidates, std::string_view filename) {
    const auto *environment = std::getenv("XDG_DATA_DIRS");
    const std::string_view directories =
        environment != nullptr && *environment != '\0'
            ? std::string_view(environment)
            : std::string_view("/usr/local/share:/usr/share");
    std::size_t start = 0;
    while (start <= directories.size()) {
        const auto end = directories.find(':', start);
        const auto directory = directories.substr(start, end - start);
        const std::filesystem::path path(directory);
        if (path.is_absolute()) {
            candidates.emplace_back(path / "modernime" / "pinyin" / filename);
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
}

std::filesystem::path defaultLearningPath() {
    const auto *xdgConfigHome = std::getenv("XDG_CONFIG_HOME");
    const auto *dataHome = std::getenv("XDG_DATA_HOME");
    const auto *home = std::getenv("HOME");
    return modernime::core::SettingsPaths::fromEnvironment(
               xdgConfigHome == nullptr ? std::string_view{}
                                        : std::string_view(xdgConfigHome),
               dataHome == nullptr ? std::string_view{}
                                   : std::string_view(dataHome),
               home == nullptr ? std::string_view{}
                               : std::string_view(home))
        .learningStore;
}

std::filesystem::path defaultUserDictionaryPath() {
    const auto *xdgConfigHome = std::getenv("XDG_CONFIG_HOME");
    const auto *dataHome = std::getenv("XDG_DATA_HOME");
    const auto *home = std::getenv("HOME");
    return modernime::core::SettingsPaths::fromEnvironment(
               xdgConfigHome == nullptr ? std::string_view{}
                                        : std::string_view(xdgConfigHome),
               dataHome == nullptr ? std::string_view{}
                                   : std::string_view(dataHome),
               home == nullptr ? std::string_view{}
                               : std::string_view(home))
        .userDictionary;
}

std::filesystem::path extensionDictionaryPath(
    std::string_view configuredPath) {
    if (!configuredPath.empty()) {
        return std::filesystem::path(configuredPath);
    }

    std::vector<std::filesystem::path> candidates;
    if (const auto *environment = std::getenv(
            "MODERNIME_PINYIN_KNOWLEDGE_DICTIONARY");
        environment != nullptr && *environment != '\0') {
        candidates.emplace_back(environment);
    }

    const auto *dataHome = std::getenv("XDG_DATA_HOME");
    if (dataHome != nullptr && *dataHome != '\0') {
        candidates.emplace_back(std::filesystem::path(dataHome) / "modernime" /
                                "pinyin" / "modernime-knowledge.dict");
    } else if (const auto *home = std::getenv("HOME"); home != nullptr &&
               *home != '\0') {
        candidates.emplace_back(std::filesystem::path(home) / ".local" /
                                "share" / "modernime" / "pinyin" /
                                "modernime-knowledge.dict");
    }

    appendDataDirectoryCandidates(candidates, "modernime-knowledge.dict");

#ifdef MODERNIME_PINYIN_KNOWLEDGE_INSTALL_BINARY
    candidates.emplace_back(MODERNIME_PINYIN_KNOWLEDGE_INSTALL_BINARY);
#endif
#ifdef MODERNIME_PINYIN_KNOWLEDGE_BUILD_BINARY
    candidates.emplace_back(MODERNIME_PINYIN_KNOWLEDGE_BUILD_BINARY);
#endif
    candidates.emplace_back(
        "/usr/share/modernime/pinyin/modernime-knowledge.dict");

    for (const auto &candidate : candidates) {
        if (isRegularFile(candidate)) {
            return candidate;
        }
    }
    return {};
}

std::filesystem::path hotwordDictionaryPath(std::string_view configuredPath) {
    if (!configuredPath.empty()) {
        return std::filesystem::path(configuredPath);
    }

    std::vector<std::filesystem::path> candidates;
    if (const auto *environment = std::getenv(
            "MODERNIME_PINYIN_HOTWORDS_DICTIONARY");
        environment != nullptr && *environment != '\0') {
        candidates.emplace_back(environment);
    }

    const auto *dataHome = std::getenv("XDG_DATA_HOME");
    if (dataHome != nullptr && *dataHome != '\0') {
        candidates.emplace_back(std::filesystem::path(dataHome) / "modernime" /
                                "pinyin" / "modernime-hotwords.dict");
    } else if (const auto *home = std::getenv("HOME"); home != nullptr &&
               *home != '\0') {
        candidates.emplace_back(std::filesystem::path(home) / ".local" /
                                "share" / "modernime" / "pinyin" /
                                "modernime-hotwords.dict");
    }

    appendDataDirectoryCandidates(candidates, "modernime-hotwords.dict");

#ifdef MODERNIME_PINYIN_HOTWORDS_INSTALL_BINARY
    candidates.emplace_back(MODERNIME_PINYIN_HOTWORDS_INSTALL_BINARY);
#endif
#ifdef MODERNIME_PINYIN_HOTWORDS_BUILD_BINARY
    candidates.emplace_back(MODERNIME_PINYIN_HOTWORDS_BUILD_BINARY);
#endif
    candidates.emplace_back(
        "/usr/share/modernime/pinyin/modernime-hotwords.dict");

    for (const auto &candidate : candidates) {
        if (isRegularFile(candidate)) {
            return candidate;
        }
    }
    return {};
}

std::filesystem::path englishDefinitionDictionaryPath(
    std::string_view configuredPath) {
    if (!configuredPath.empty()) {
        return std::filesystem::path(configuredPath);
    }

    std::vector<std::filesystem::path> candidates;
    if (const auto *environment = std::getenv(
            "MODERNIME_ENGLISH_DEFINITION_DICTIONARY");
        environment != nullptr && *environment != '\0') {
        candidates.emplace_back(environment);
    }

    const auto *dataHome = std::getenv("XDG_DATA_HOME");
    if (dataHome != nullptr && *dataHome != '\0') {
        candidates.emplace_back(std::filesystem::path(dataHome) / "modernime" /
                                "pinyin" / "modernime-english-dict.bin");
    } else if (const auto *home = std::getenv("HOME"); home != nullptr &&
               *home != '\0') {
        candidates.emplace_back(std::filesystem::path(home) / ".local" /
                                "share" / "modernime" / "pinyin" /
                                "modernime-english-dict.bin");
    }

    appendDataDirectoryCandidates(candidates, "modernime-english-dict.bin");

#ifdef MODERNIME_ENGLISH_DICT_INSTALL_BINARY
    candidates.emplace_back(MODERNIME_ENGLISH_DICT_INSTALL_BINARY);
#endif
#ifdef MODERNIME_ENGLISH_DICT_SOURCE_BINARY
    candidates.emplace_back(MODERNIME_ENGLISH_DICT_SOURCE_BINARY);
#endif
    candidates.emplace_back(
        "/usr/share/modernime/pinyin/modernime-english-dict.bin");

    for (const auto &candidate : candidates) {
        if (isRegularFile(candidate)) {
            return candidate;
        }
    }
    return {};
}

void loadExtensionDictionary(libime::PinyinDictionary &dictionary,
                             const std::filesystem::path &path) {
    if (!isRegularFile(path)) {
        return;
    }

    const auto index = dictionary.dictSize();
    dictionary.addEmptyDict();
    try {
        dictionary.load(index, path.c_str(), libime::PinyinDictFormat::Binary);
    } catch (...) {
        dictionary.clear(index);
    }
}

} // namespace

namespace modernime::pinyin {

std::string defaultDictionaryPath() {
    if (const auto *env = std::getenv("MODERNIME_PINYIN_DICT");
        env != nullptr && *env != '\0') {
        return env;
    }
    if (const auto *env = std::getenv("LIBIME_PINYIN_DICT");
        env != nullptr && *env != '\0') {
        return env;
    }

    static const char *const kCandidates[] = {
        "/usr/share/libime/sc.dict",
        "/usr/lib64/libime/sc.dict",
        "/usr/lib/x86_64-linux-gnu/libime/sc.dict",
        "/usr/lib/aarch64-linux-gnu/libime/sc.dict",
        "/usr/lib/riscv64-linux-gnu/libime/sc.dict",
        "/usr/lib/loongarch64-linux-gnu/libime/sc.dict",
        "/usr/lib/libime/sc.dict",
        "/usr/local/share/libime/sc.dict",
        "/usr/local/lib64/libime/sc.dict",
        "/usr/local/lib/libime/sc.dict",
    };

    for (const char *candidate : kCandidates) {
        if (isRegularFile(candidate)) {
            return candidate;
        }
    }
    return "/usr/share/libime/sc.dict";
}

std::string defaultLanguageModelPath() {
    if (const auto *env = std::getenv("MODERNIME_PINYIN_LM");
        env != nullptr && *env != '\0') {
        return env;
    }
    if (const auto *env = std::getenv("LIBIME_PINYIN_LM");
        env != nullptr && *env != '\0') {
        return env;
    }

    static const char *const kCandidates[] = {
        "/usr/lib64/libime/zh_CN.lm",
        "/usr/lib/x86_64-linux-gnu/libime/zh_CN.lm",
        "/usr/lib/aarch64-linux-gnu/libime/zh_CN.lm",
        "/usr/lib/riscv64-linux-gnu/libime/zh_CN.lm",
        "/usr/lib/loongarch64-linux-gnu/libime/zh_CN.lm",
        "/usr/share/libime/zh_CN.lm",
        "/usr/lib/libime/zh_CN.lm",
        "/usr/local/lib64/libime/zh_CN.lm",
        "/usr/local/lib/libime/zh_CN.lm",
        "/usr/local/share/libime/zh_CN.lm",
    };

    for (const char *candidate : kCandidates) {
        if (isRegularFile(candidate)) {
            return candidate;
        }
    }

    std::error_code ec;
    if (std::filesystem::is_directory("/usr/lib64", ec)) {
        return "/usr/lib64/libime/zh_CN.lm";
    }
    return "/usr/lib/x86_64-linux-gnu/libime/zh_CN.lm";
}

PinyinSharedResources::PinyinSharedResources() = default;
PinyinSharedResources::~PinyinSharedResources() = default;

std::shared_ptr<PinyinSharedResources> PinyinSharedResources::create(
    const PinyinDataPaths &paths, const PinyinProviderOptions &options) {
    auto resources = std::make_shared<PinyinCandidateProvider::SharedResources>();
    resources->userDictionaryPath =
        paths.userDictionary.empty()
            ? defaultUserDictionaryPath()
            : std::filesystem::path(paths.userDictionary);
    resources->userDictionary =
        UserDictionary::loadText(resources->userDictionaryPath);
    resources->learningStorePath =
        paths.learningStore.empty()
            ? defaultLearningPath()
            : std::filesystem::path(paths.learningStore);
    resources->englishDefinitionDictPath =
        englishDefinitionDictionaryPath(paths.englishDefinitionDictionary);

    auto dictionary = std::make_unique<libime::PinyinDictionary>();
    if (!paths.dictionary.empty()) {
        if (isRegularFile(paths.dictionary)) {
            dictionary->load(0, paths.dictionary.c_str(),
                             libime::PinyinDictFormat::Binary);
        } else {
            std::cerr << "ModernIME 警告: 拼音系统词典文件不存在: '"
                      << paths.dictionary
                      << "'，拼音基础候选将无法生成！请检查 libime 安装或 PinyinDataPaths 配置。"
                      << std::endl;
        }
    }
    resources->userDictionary.addTo(*dictionary, 1);
    loadExtensionDictionary(
        *dictionary,
        extensionDictionaryPath(paths.extensionDictionary));
    // 热词表加载在知识库之后的下一层（当前为 layer 3）；正 cost 让
    // 简拼命中的热词越过系统词典组合，排在候选列表前列。
    loadExtensionDictionary(
        *dictionary, hotwordDictionaryPath(paths.hotwordDictionary));

    std::unique_ptr<libime::UserLanguageModel> model;
    if (!paths.languageModel.empty() && isRegularFile(paths.languageModel)) {
        model = std::make_unique<libime::UserLanguageModel>(
            paths.languageModel.c_str());
    } else {
        if (!paths.languageModel.empty()) {
            std::cerr << "ModernIME 警告: 拼音语言模型文件不存在: '"
                      << paths.languageModel
                      << "'，已降级为空语言模型，整句预测与候选词排序将受严重影响！请检查 libime 安装或 PinyinDataPaths 配置。"
                      << std::endl;
        }
        model = std::make_unique<libime::UserLanguageModel>();
    }

    resources->ime = std::make_unique<libime::PinyinIME>(std::move(dictionary),
                                                         std::move(model));
    resources->ime->setFuzzyFlags(getDefaultFuzzyFlags());
    resources->ime->setNBest(32);
    // 在后台预热线程中一体化加载学习写入器，首键打字时直接读取内存快照，
    // 避免首键触发数十毫秒的 SQLite 磁盘打开与全表反序列化。
    if (options.learningEnabled) {
        resources->ensureLearningWriter();
    } else {
        resources->reloadVisibility();
    }

    resources->dictionaryGeneration_ = 1;
    resources->learningRevision_ =
        resources->learning != nullptr ? resources->learning->resetRevision() : resources->visibilityRevision;

    return resources;
}

ResourceChange PinyinSharedResources::reloadUserDictionary() {
    std::lock_guard<std::recursive_mutex> lock(resourceMutex_);
    auto updated = UserDictionary::loadText(userDictionaryPath);
    if (ime && ime->dict()) {
        ime->dict()->clear(1);
        updated.addTo(*ime->dict(), 1);
    }
    userDictionary = std::move(updated);
    ++dictionaryGeneration_;
    return ResourceChange{dictionaryGeneration_, learningRevision_, true};
}

ResourceChange PinyinSharedResources::reloadLearningStore() {
    std::lock_guard<std::recursive_mutex> lock(resourceMutex_);
    if (phraseStats != nullptr) {
        phraseStats->clear();
    }
    bool changed = false;
    if (learning != nullptr) {
        changed = learning->reload();
        if (changed) {
            learningRevision_ = learning->resetRevision();
        }
    } else {
        changed = reloadVisibility();
        if (changed) {
            learningRevision_ = visibilityRevision;
        }
    }
    return ResourceChange{dictionaryGeneration_, learningRevision_, changed};
}

ResourceChange PinyinSharedResources::refreshLearningStore() {
    std::lock_guard<std::recursive_mutex> lock(resourceMutex_);
    if (learning != nullptr) {
        if (learning->reloadIfChanged()) {
            if (phraseStats != nullptr) {
                phraseStats->clear();
            }
            learningRevision_ = learning->resetRevision();
            return ResourceChange{dictionaryGeneration_, learningRevision_, true};
        }
        return ResourceChange{dictionaryGeneration_, learningRevision_, false};
    }
    if (visibilityReader != nullptr) {
        const auto revision = visibilityReader->resetRevision();
        if (revision && *revision != visibilityRevision && reloadVisibility()) {
            if (phraseStats != nullptr) {
                phraseStats->clear();
            }
            learningRevision_ = visibilityRevision;
            return ResourceChange{dictionaryGeneration_, learningRevision_, true};
        }
    }
    return ResourceChange{dictionaryGeneration_, learningRevision_, false};
}

std::shared_ptr<const core::LearningSnapshot> PinyinSharedResources::visibleLearning() const {
    std::lock_guard<std::recursive_mutex> lock(resourceMutex_);
    return learning != nullptr ? learning->snapshot() : visibilitySnapshot;
}

core::LearningWriter *PinyinSharedResources::ensureLearningWriter() {
    std::lock_guard<std::recursive_mutex> lock(resourceMutex_);
    if (learning == nullptr && !learningStorePath.empty()) {
        learning = std::make_unique<core::LearningWriter>(learningStorePath);
        learningRevision_ = learning->resetRevision();
    }
    return learning.get();
}

core::EnglishDefinitionDictionary *PinyinSharedResources::ensureEnglishDefinitionDictionary() {
    std::lock_guard<std::recursive_mutex> lock(resourceMutex_);
    if (!englishDefinitionDict.isLoaded() && !englishDefinitionDictPath.empty()) {
        englishDefinitionDict.load(englishDefinitionDictPath);
    }
    return englishDefinitionDict.isLoaded() ? &englishDefinitionDict : nullptr;
}

bool PinyinSharedResources::reloadVisibility() {
    std::lock_guard<std::recursive_mutex> lock(resourceMutex_);
    if (visibilityReader == nullptr) {
        if (learningStorePath.empty()) {
            return false;
        }
        visibilityReader = std::make_unique<core::LearningStore>(learningStorePath);
        if (!visibilityReader->openReadOnly()) {
            visibilityReader.reset();
            return false;
        }
    }
    const auto state = visibilityReader->readState();
    if (!state) {
        return false;
    }
    visibilitySnapshot = state->snapshot;
    visibilityRevision = state->resetRevision;
    learningRevision_ = visibilityRevision;
    return true;
}

} // namespace modernime::pinyin
