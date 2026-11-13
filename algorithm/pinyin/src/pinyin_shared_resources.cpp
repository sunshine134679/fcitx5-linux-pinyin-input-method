#include "modernime/pinyin/pinyin_shared_resources.h"
#include <libime/pinyin/shuangpinprofile.h>
#include "modernime/pinyin/user_dictionary.h"

#include "modernime/core/pinyin_match.h"
#include "modernime/core/settings.h"

#include <libime/core/userlanguagemodel.h>
#include <libime/pinyin/pinyindictionary.h>
#include <libime/pinyin/pinyinime.h>

#include <algorithm>
#include <chrono>
#include <sys/stat.h>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <system_error>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

libime::PinyinFuzzyFlags fuzzyFlags(const modernime::core::FuzzyPinyinOptions &options) {
    // Keep valid internal syllable boundaries (xian -> xi'an, jie -> ji'e)
    // in the native graph. These are exact alternative readings, not fuzzy sounds.
    libime::PinyinFuzzyFlags flags{libime::PinyinFuzzyFlag::Inner};
    flags |= libime::PinyinFuzzyFlag::InnerShort;
    if (options.zZh) flags |= libime::PinyinFuzzyFlag::Z_ZH;
    if (options.cCh) flags |= libime::PinyinFuzzyFlag::C_CH;
    if (options.sSh) flags |= libime::PinyinFuzzyFlag::S_SH;
    if (options.lN) flags |= libime::PinyinFuzzyFlag::L_N;
    if (options.enEng) flags |= libime::PinyinFuzzyFlag::EN_ENG;
    if (options.inIng) flags |= libime::PinyinFuzzyFlag::IN_ING;
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

std::uint64_t packDirectoryStamp(const std::filesystem::path &local,
                                 const std::filesystem::path &bundled) {
    std::vector<std::filesystem::path> files;
    for (const auto &root : {local, bundled}) {
        if (root.empty()) continue;
        std::error_code error;
        if (!std::filesystem::exists(root, error)) {
            if (error) throw std::filesystem::filesystem_error("词库包目录读取失败", root, error);
            continue;
        }
        files.push_back(root);
        for (std::filesystem::directory_iterator iterator(root), end; iterator != end; ++iterator) {
            if (!iterator->is_directory() || iterator->is_symlink()) continue;
            files.push_back(iterator->path());
            for (const auto *name : {"manifest.ini", "entries.tsv", "disabled"}) {
                const auto path = iterator->path() / name;
                if (std::filesystem::exists(path)) files.push_back(path);
            }
        }
    }
    std::sort(files.begin(), files.end());
    std::uint64_t stamp = 14695981039346656037ULL;
    const auto mix = [&](std::string_view bytes) {
        for (const unsigned char byte : bytes) { stamp ^= byte; stamp *= 1099511628211ULL; }
    };
    for (const auto &path : files) {
        struct stat status {};
        if (::lstat(path.c_str(), &status) != 0) throw std::runtime_error("词库包状态读取失败");
        mix(path.string());
        for (const auto value : {static_cast<std::uint64_t>(status.st_ino),
             static_cast<std::uint64_t>(status.st_size), static_cast<std::uint64_t>(status.st_mtim.tv_sec),
             static_cast<std::uint64_t>(status.st_mtim.tv_nsec), static_cast<std::uint64_t>(status.st_ctim.tv_sec),
             static_cast<std::uint64_t>(status.st_ctim.tv_nsec)}) mix(std::to_string(value));
    }
    return stamp;
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

std::filesystem::path resolveContextLanguageModelPath(std::string_view configuredPath) {
    // An explicit path also allows diagnostics to exercise the old-model fallback.
    if (!configuredPath.empty()) return std::filesystem::path(configuredPath);
    std::vector<std::filesystem::path> candidates;
    if (const auto *dataHome = std::getenv("XDG_DATA_HOME"); dataHome && *dataHome) {
        candidates.emplace_back(std::filesystem::path(dataHome) / "modernime/pinyin/modernime-context.lm");
    } else if (const auto *home = std::getenv("HOME"); home && *home) {
        candidates.emplace_back(std::filesystem::path(home) / ".local/share/modernime/pinyin/modernime-context.lm");
    }
    appendDataDirectoryCandidates(candidates, "modernime-context.lm");
#ifdef MODERNIME_CONTEXT_MODEL_INSTALL_BINARY
    candidates.emplace_back(MODERNIME_CONTEXT_MODEL_INSTALL_BINARY);
#endif
#ifdef MODERNIME_CONTEXT_MODEL_BUILD_BINARY
    candidates.emplace_back(MODERNIME_CONTEXT_MODEL_BUILD_BINARY);
#endif
    for (const auto &candidate : candidates)
        if (isRegularFile(candidate)) return candidate;
    return {};
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
        const std::filesystem::path path(configuredPath);
        if (isRegularFile(path)) {
            return path;
        }
        std::cerr << "Warning: configured extension dictionary path does not exist: "
                  << configuredPath << ", falling back to default search paths.\n";
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
        const std::filesystem::path path(configuredPath);
        if (isRegularFile(path)) {
            return path;
        }
        std::cerr << "Warning: configured hotword dictionary path does not exist: "
                  << configuredPath << ", falling back to default search paths.\n";
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
        const std::filesystem::path path(configuredPath);
        if (isRegularFile(path)) {
            return path;
        }
        std::cerr << "Warning: configured english definition dictionary path does not exist: "
                  << configuredPath << ", falling back to default search paths.\n";
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
    const PinyinDataPaths &paths, const PinyinProviderOptions &options, bool initializeLearning) {
    auto resources = std::make_shared<PinyinCandidateProvider::SharedResources>();
    resources->enginePaths_ = paths;
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
    const auto contextPath = resolveContextLanguageModelPath(paths.contextLanguageModel);
    if (!contextPath.empty()) {
        try {
            resources->contextLanguageModel =
                std::make_shared<const libime::StaticLanguageModelFile>(contextPath.c_str());
            resources->contextLanguageModelPath = contextPath;
        } catch (const std::exception &error) {
            std::cerr << "ModernIME 警告: 语境模型加载失败，沿用系统模型: "
                      << contextPath << ": " << error.what() << '\n';
        }
    }
    const auto *packDataHome = std::getenv("XDG_DATA_HOME");
    const auto *packHome = std::getenv("HOME");
    resources->dictionaryPacksPath_ = !paths.dictionaryPacks.empty()
        ? std::filesystem::path(paths.dictionaryPacks)
        : (packDataHome && *packDataHome ? std::filesystem::path(packDataHome)
           : std::filesystem::path(packHome && *packHome ? packHome : "/nonexistent") / ".local/share")
          / "modernime/dictionary-packs/user";
    if (!paths.bundledDictionaryPacks.empty()) {
        resources->bundledDictionaryPacksPath_ = paths.bundledDictionaryPacks;
    } else {
        std::vector<std::filesystem::path> candidates;
        if (packDataHome && *packDataHome)
            candidates.emplace_back(std::filesystem::path(packDataHome) / "modernime/dictionary-packs");
        if (packHome && *packHome)
            candidates.emplace_back(std::filesystem::path(packHome) / ".local/share/modernime/dictionary-packs");
        const auto *dataDirectories = std::getenv("XDG_DATA_DIRS");
        const std::string_view directories = dataDirectories && *dataDirectories
            ? dataDirectories : "/usr/local/share:/usr/share";
        for (std::size_t start = 0; start <= directories.size();) {
            const auto end = directories.find(':', start);
            const std::filesystem::path directory(directories.substr(start, end - start));
            if (directory.is_absolute()) candidates.emplace_back(directory / "modernime/dictionary-packs");
            if (end == std::string_view::npos) break;
            start = end + 1;
        }
#ifdef MODERNIME_DICTIONARY_PACKS_INSTALL_DIR
        candidates.emplace_back(MODERNIME_DICTIONARY_PACKS_INSTALL_DIR);
#endif
#ifdef MODERNIME_DICTIONARY_PACKS_SOURCE_DIR
        candidates.emplace_back(MODERNIME_DICTIONARY_PACKS_SOURCE_DIR);
#endif
        for (const auto &candidate : candidates) {
            std::error_code error;
            bool containsPack = false;
            for (std::filesystem::directory_iterator iterator(candidate, error), end;
                 iterator != end && !error; iterator.increment(error)) {
                if (isRegularFile(iterator->path() / "manifest.ini") &&
                    isRegularFile(iterator->path() / "entries.tsv")) { containsPack = true; break; }
            }
            if (containsPack) { resources->bundledDictionaryPacksPath_ = candidate; break; }
        }
    }
    resources->dictionaryPackLayer_ = resources->ime->dict()->dictSize();
    resources->ime->dict()->addEmptyDict();
    resources->reloadDictionaryPacks();
    resources->setFuzzyOptions(options.fuzzyPinyin);
    resources->setPinyinScheme(options.pinyinScheme);
    resources->ime->setNBest(32);
    // 在后台预热线程中一体化加载学习写入器，首键打字时直接读取内存快照，
    // 避免首键触发数十毫秒的 SQLite 磁盘打开与全表反序列化。
    if (initializeLearning) {
        if (options.learningEnabled) resources->ensureLearningWriter();
        else resources->reloadVisibility();
    }
    try {
        resources->dictionaryPackStamp_ = packDirectoryStamp(
            resources->dictionaryPacksPath_, resources->bundledDictionaryPacksPath_);
    } catch (const std::exception &) { /* timer retries transient directory failures */ }

    resources->dictionaryGeneration_.store(1, std::memory_order_release);
    resources->learningRevision_.store(
        resources->learning != nullptr ? resources->learning->resetRevision() : resources->visibilityRevision,
        std::memory_order_release);

    return resources;
}

bool PinyinSharedResources::setPinyinScheme(core::PinyinScheme scheme) {
    if (pinyinScheme_ == scheme) return false;
    pinyinScheme_ = scheme;
    ++inputConfigurationGeneration_;
    if (usesShuangpin()) {
        const auto profile = scheme == core::PinyinScheme::Xiaohe
            ? libime::ShuangpinBuiltinProfile::Xiaohe : libime::ShuangpinBuiltinProfile::Ziranma;
        ime->setShuangpinProfile(std::make_shared<libime::ShuangpinProfile>(profile));
    }
    return true;
}

bool PinyinSharedResources::setFuzzyOptions(const core::FuzzyPinyinOptions &options) {
    const auto flags = fuzzyFlags(options);
    if (ime->fuzzyFlags() == flags) return false;
    ++inputConfigurationGeneration_;
    ime->setFuzzyFlags(flags);
    return true;
}

ResourceChange PinyinSharedResources::reloadUserDictionary() {
    std::lock_guard<std::recursive_mutex> lock(resourceMutex_);
    auto updated = UserDictionary::loadText(userDictionaryPath);
    if (ime && ime->dict()) {
        ime->dict()->clear(1);
        updated.addTo(*ime->dict(), 1);
    }
    userDictionary = std::move(updated);
    MixedWordIndex mixed;
    for (const auto &entry : userDictionary.entries()) mixed.add(entry);
    for (const auto &pack : activeDictionaryPacks_)
        for (const auto &entry : pack.entries) mixed.add(entry);
    mixedWords = std::move(mixed);
    ++dictionaryGeneration_;
    return ResourceChange{dictionaryGeneration_.load(std::memory_order_acquire),
                          learningRevision_.load(std::memory_order_acquire), true};
}

void PinyinSharedResources::requestDictionaryPackRefresh() {
    if (pendingPackRefresh_.valid()) return;
    auto paths = enginePaths_;
    paths.dictionaryPacks = dictionaryPacksPath_.string();
    paths.bundledDictionaryPacks = bundledDictionaryPacksPath_.string();
    const auto previousStamp = dictionaryPackStamp_;
    const auto baseline = dictionaryGeneration();
    pendingPackRefresh_ = std::async(std::launch::async,
        [paths, previousStamp, baseline,
         retiredResources = std::move(retiredPackResources_),
         retiredEngines = std::move(retiredPinyinEngines_)]() mutable {
        retiredResources.clear();
        retiredEngines.clear();
        PreparedPackRefresh result;
        result.baselineGeneration = baseline;
        try {
            result.stamp = packDirectoryStamp(paths.dictionaryPacks, paths.bundledDictionaryPacks);
            if (result.stamp == previousStamp) return result;
            // An incomplete external edit must not silently drop an active
            // package. Leave the published engine intact and retry next tick.
            for (const auto &root : {paths.dictionaryPacks, paths.bundledDictionaryPacks}) {
                std::string error;
                DictionaryPackStore(root).loadEnabled(&error);
                if (!error.empty()) throw std::runtime_error(error);
            }
            PinyinProviderOptions options;
            options.learningEnabled = false;
            result.resources = create(paths, options, false);
            if (result.stamp != packDirectoryStamp(paths.dictionaryPacks, paths.bundledDictionaryPacks))
                result.resources.reset();
        } catch (const std::exception &) {
            result.resources.reset();
        }
        return result;
    });
}

ResourceChange PinyinSharedResources::consumeDictionaryPackRefresh() {
    if (!pendingPackRefresh_.valid() ||
        pendingPackRefresh_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return {dictionaryGeneration(), learningRevision(), false};
    auto prepared = pendingPackRefresh_.get();
    if (!prepared.resources) return {dictionaryGeneration(), learningRevision(), false};
    if (prepared.baselineGeneration != dictionaryGeneration()) {
        retiredPackResources_.push_back(std::move(prepared.resources));
        return {dictionaryGeneration(), learningRevision(), false};
    }
    auto &updated = *prepared.resources;
    updated.ime->setFuzzyFlags(ime->fuzzyFlags());
    updated.ime->setShuangpinProfile(ime->shuangpinProfile());
    updated.ime->setNBest(ime->nbest());
    // Keep replaced heavy objects alive until the next worker request releases
    // them; publication does not run dictionary/model/index destruction.
    std::swap(ime, updated.ime);
    std::swap(userDictionary, updated.userDictionary);
    std::swap(mixedWords, updated.mixedWords);
    std::swap(activeDictionaryPacks_, updated.activeDictionaryPacks_);
    dictionaryPackLayer_ = updated.dictionaryPackLayer_;
    dictionaryPackStamp_ = prepared.stamp;
    retiredPackResources_.push_back(std::move(prepared.resources));
    ++dictionaryGeneration_;
    return {dictionaryGeneration(), learningRevision(), true};
}

std::vector<DictionaryPackManifest> PinyinSharedResources::dictionaryPacks() const {
    auto manifests = DictionaryPackStore(bundledDictionaryPacksPath_).list();
    const auto local = DictionaryPackStore(dictionaryPacksPath_).list();
    for (const auto &item : local) {
        std::erase_if(manifests, [&](const auto &old) { return old.id == item.id; });
        manifests.push_back(item);
    }
    return manifests;
}

ResourceChange PinyinSharedResources::reloadDictionaryPacks() {
    std::lock_guard<std::recursive_mutex> lock(resourceMutex_);
    auto active = DictionaryPackStore(bundledDictionaryPacksPath_).loadEnabled();
    const auto localManifests = DictionaryPackStore(dictionaryPacksPath_).list();
    for (const auto &item : localManifests)
        std::erase_if(active, [&](const auto &old) { return old.manifest.id == item.id; });
    auto local = DictionaryPackStore(dictionaryPacksPath_).loadEnabled();
    for (auto &pack : local) active.push_back(std::move(pack));
    MixedWordIndex mixed;
    for (const auto &entry : userDictionary.entries()) mixed.add(entry);
    UserDictionary native;
    for (const auto &pack : active) for (const auto &entry : pack.entries) {
        if (MixedWordIndex::isMixedWord(entry.phrase)) mixed.add(entry);
        else native.upsert(entry.pinyin, entry.phrase, 0.0F);
    }
    if (ime && ime->dict()) {
        auto &dictionary = *ime->dict();
        dictionary.clear(dictionaryPackLayer_);
        // A conservative shared layer: no positive curated weights and no
        // fragment matching. The normal ranker still owns every final seat.
        native.addTo(dictionary, dictionaryPackLayer_, -8.0F);
        dictionary.setFlags(dictionaryPackLayer_, libime::PinyinDictFlag::FullMatch);
    }
    mixedWords = std::move(mixed);
    activeDictionaryPacks_ = std::move(active);
    ++dictionaryGeneration_;
    return {dictionaryGeneration(), learningRevision(), true};
}

bool PinyinSharedResources::importDictionaryPack(const std::filesystem::path &directory,
                                                bool replace, std::string *error) {
    if (!DictionaryPackStore(dictionaryPacksPath_).importPack(directory, replace, error)) return false;
    reloadDictionaryPacks(); return true;
}
bool PinyinSharedResources::setDictionaryPackEnabled(std::string_view id, bool enabled,
                                                    std::string *error) {
    DictionaryPackStore local(dictionaryPacksPath_);
    // Bundle activation is represented by a private managed copy; package
    // removal or upgrade never modifies the read-only installation directory.
    const auto manifests = local.list();
    if (std::none_of(manifests.begin(), manifests.end(), [&](const auto &m) { return m.id == id; })) {
        if (!local.importPack(bundledDictionaryPacksPath_ / id, false, error)) return false;
    }
    if (!local.setEnabled(id, enabled, error)) return false;
    reloadDictionaryPacks(); return true;
}
bool PinyinSharedResources::removeDictionaryPack(std::string_view id, std::string *error) {
    if (!DictionaryPackStore(dictionaryPacksPath_).remove(id, error)) return false;
    reloadDictionaryPacks(); return true;
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
            learningRevision_.store(learning->resetRevision(), std::memory_order_release);
        }
    } else {
        changed = reloadVisibility();
        if (changed) {
            learningRevision_.store(visibilityRevision, std::memory_order_release);
        }
    }
    return ResourceChange{dictionaryGeneration_.load(std::memory_order_acquire),
                          learningRevision_.load(std::memory_order_acquire), changed};
}

ResourceChange PinyinSharedResources::refreshLearningStore() {
    std::lock_guard<std::recursive_mutex> lock(resourceMutex_);
    if (learning != nullptr) {
        const bool reloaded = learning->reloadIfChanged();
        const auto revision = learning->resetRevision();
        // A restore performed by this Writer already advanced its revision.
        // reloadIfChanged only detects external resets, so publish that local
        // transition too before providers authorize any old candidate page.
        if (reloaded || revision != learningRevision_.load(std::memory_order_acquire)) {
            if (phraseStats != nullptr) {
                phraseStats->clear();
            }
            learningRevision_.store(revision, std::memory_order_release);
            return ResourceChange{dictionaryGeneration_.load(std::memory_order_acquire),
                                  learningRevision_.load(std::memory_order_acquire), true};
        }
        return ResourceChange{dictionaryGeneration_.load(std::memory_order_acquire),
                              learningRevision_.load(std::memory_order_acquire), false};
    }
    if (visibilityReader != nullptr) {
        const auto revision = visibilityReader->resetRevision();
        if (revision && *revision != visibilityRevision && reloadVisibility()) {
            if (phraseStats != nullptr) {
                phraseStats->clear();
            }
            learningRevision_.store(visibilityRevision, std::memory_order_release);
            return ResourceChange{dictionaryGeneration_.load(std::memory_order_acquire),
                                  learningRevision_.load(std::memory_order_acquire), true};
        }
    }
    return ResourceChange{dictionaryGeneration_.load(std::memory_order_acquire),
                          learningRevision_.load(std::memory_order_acquire), false};
}

std::shared_ptr<const core::LearningSnapshot> PinyinSharedResources::visibleLearning() const {
    std::lock_guard<std::recursive_mutex> lock(resourceMutex_);
    return learning != nullptr ? learning->snapshot() : visibilitySnapshot;
}

std::uint64_t PinyinSharedResources::refreshLearningForgetState() {
    std::lock_guard<std::recursive_mutex> lock(resourceMutex_);
    if (!learning) return 0;
    const auto generation = learning->forgetGeneration();
    if (generation == phraseStatsForgetGeneration_) return generation;
    const auto changes = learning->forgottenSince(phraseStatsForgetGeneration_);
    if (phraseStats) {
        if (!changes.complete) phraseStats->clear();
        else for (const auto &[text, pinyin] : changes.identities) phraseStats->forget(text, pinyin);
    }
    phraseStatsForgetGeneration_ = changes.generation;
    return changes.generation;
}

core::ForgottenPreferences PinyinSharedResources::forgottenSince(std::uint64_t generation) const {
    std::lock_guard<std::recursive_mutex> lock(resourceMutex_);
    return learning != nullptr ? learning->forgottenSince(generation) : core::ForgottenPreferences{};
}

core::LearningWriter *PinyinSharedResources::ensureLearningWriter(bool synchronousInitialLoad) {
    std::lock_guard<std::recursive_mutex> lock(resourceMutex_);
    if (learning == nullptr && !learningStorePath.empty()) {
        learning = std::make_unique<core::LearningWriter>(learningStorePath, synchronousInitialLoad);
        if (onReadyCallback_) {
            learning->setOnReadyCallback(onReadyCallback_);
        }
        learningRevision_.store(learning->resetRevision(), std::memory_order_release);
    }
    return learning.get();
}

core::EpochViewState PinyinSharedResources::epochViewState() const {
    std::lock_guard<std::recursive_mutex> lock(resourceMutex_);
    if (learning != nullptr) {
        return learning->epochViewState();
    }
    return core::EpochViewState::Ready;
}

void PinyinSharedResources::prepareLearningWriterAsync() {
    ensureLearningWriter(/*synchronousInitialLoad=*/false);
}

void PinyinSharedResources::setOnReadyCallback(std::function<void()> callback) {
    std::lock_guard<std::recursive_mutex> lock(resourceMutex_);
    onReadyCallback_ = std::move(callback);
    if (learning != nullptr) {
        learning->setOnReadyCallback(onReadyCallback_);
    }
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
