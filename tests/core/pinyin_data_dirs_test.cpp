#include "modernime/pinyin/pinyin_candidate_provider.h"
#include <libime/pinyin/pinyindictionary.h>

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {
void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "pinyin data directory test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
void makeDictionary(const std::filesystem::path &path, std::string_view pinyin,
                    std::string_view phrase) {
    libime::PinyinDictionary dictionary;
    dictionary.addWord(0, pinyin, phrase, 10.0f);
    dictionary.save(0, path.c_str(), libime::PinyinDictFormat::Binary);
}
void makeEnglishDictionary(const std::filesystem::path &path) {
    std::ofstream output(path, std::ios::binary);
    output.write("MODEDIC1", 8);
    const auto write = [&output](auto value) {
        output.write(reinterpret_cast<const char *>(&value), sizeof(value));
    };
    write(std::uint32_t{1}); // one entry
    write(std::uint32_t{28}); // string pool follows header and 12-byte index
    write(std::uint32_t{0}); // apple offset
    write(std::uint16_t{5});
    write(std::uint16_t{12}); // four Chinese characters
    write(std::uint32_t{5});
    output << "apple便携释义";
    require(output.good(), "custom English fixture saves");
}
void expectCandidate(modernime::pinyin::PinyinCandidateProvider &provider,
                     std::string_view input, std::string_view expected) {
    provider.reset();
    require(provider.append(input), "input is accepted");
    for (const auto &candidate : provider.page().items) {
        if (candidate.text == expected) {
            return;
        }
    }
    require(false, expected);
}
} // namespace

int main(int argc, char **argv) {
    if (argc == 2) {
        // Smoke-test the dictionaries supplied by a real installed archive.
        ::setenv("XDG_DATA_DIRS", argv[1], 1);
        modernime::pinyin::PinyinProviderOptions options;
        options.learningEnabled = false;
        options.englishDefinitionEnabled = true;
        modernime::pinyin::PinyinCandidateProvider provider(
            modernime::pinyin::PinyinDataPaths{}, options);
        expectCandidate(provider, "hstz", "画蛇添足");
        expectCandidate(provider, "yyds", "永远的神");
        expectCandidate(provider, "apple", "苹果；家伙");
        return EXIT_SUCCESS;
    }
    auto rootTemplate = (std::filesystem::temp_directory_path() /
                         "modernime-pinyin-data-dirs-test.XXXXXX").string();
    const auto *created = ::mkdtemp(rootTemplate.data());
    require(created != nullptr, "isolated fixture directory is created");
    const std::filesystem::path root(created);
    const auto data = root / "custom-prefix/share/modernime/pinyin";
    std::filesystem::create_directories(data);
    std::filesystem::create_directories(root / "home");
    makeDictionary(data / "modernime-knowledge.dict", "bian'xie'zhi'shi'jia",
                   "便携知识甲");
    makeDictionary(data / "modernime-hotwords.dict", "gui'dang're'ci'yi",
                   "归档热词乙");
    makeEnglishDictionary(data / "modernime-english-dict.bin");
    ::setenv("HOME", (root / "home").c_str(), 1);
    ::setenv("XDG_DATA_HOME", (root / "empty-user-data").c_str(), 1);
    const auto dirs = (root / "missing").string() + ":relative::" +
                      (root / "custom-prefix/share").string();
    ::setenv("XDG_DATA_DIRS", dirs.c_str(), 1);
    ::unsetenv("MODERNIME_PINYIN_KNOWLEDGE_DICTIONARY");
    ::unsetenv("MODERNIME_PINYIN_HOTWORDS_DICTIONARY");
    ::unsetenv("MODERNIME_ENGLISH_DEFINITION_DICTIONARY");
    modernime::pinyin::PinyinProviderOptions options;
    options.learningEnabled = false;
    options.englishDefinitionEnabled = true;
    modernime::pinyin::PinyinCandidateProvider provider(modernime::pinyin::PinyinDataPaths{}, options);
    expectCandidate(provider, "bianxiezhishijia", "便携知识甲");
    expectCandidate(provider, "guidangreciyi", "归档热词乙");
    expectCandidate(provider, "apple", "便携释义");
    std::filesystem::remove_all(root);
}
