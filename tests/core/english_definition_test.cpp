#include "modernime/core/english_definition_dictionary.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "Assertion failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void testCleanDefinitionRules() {
    using modernime::core::EnglishDefinitionDictionary;

    // 1. 去除常规词性前缀并用分号连接常用前2个义项
    {
        const auto res = EnglishDefinitionDictionary::cleanDefinition(
            "n. 车库, 汽车修理厂, 机库\nvt. 把车送入修车场");
        assertTrue(res == "车库；汽车修理厂", "garage clean test");
    }

    // 2. 去除 [医]、[计] 等行业方括号标签
    {
        const auto res = EnglishDefinitionDictionary::cleanDefinition(
            "n. 苹果, 家伙\n[医] 苹果");
        assertTrue(res == "苹果；家伙", "apple clean test");
    }

    // 3. 多词性单义项提取
    {
        const auto res = EnglishDefinitionDictionary::cleanDefinition("interj. 喂, 嘿");
        assertTrue(res == "喂；嘿", "hello clean test");
    }

    // 4. 长度限制截断（12个字符）
    {
        const auto res = EnglishDefinitionDictionary::cleanDefinition(
            "n. 这是一个非常长非常长非常长的中文释义项一, 另一个非常长非常长非常长的中文释义项二", 12);
        assertTrue(res.size() <= 12 * 3, "length limit test (byte-safe)");
        assertTrue(!res.empty(), "length limit non-empty");
    }

    // 5. 空串或无中文
    {
        assertTrue(EnglishDefinitionDictionary::cleanDefinition("").empty(), "empty string");
        assertTrue(EnglishDefinitionDictionary::cleanDefinition("n. abc def").empty(), "no chinese");
    }
}

void testDictionaryLookup() {
    using modernime::core::EnglishDefinitionDictionary;

#ifndef MODERNIME_ENGLISH_DICT_FILE
#define MODERNIME_ENGLISH_DICT_FILE "data/pinyin/modernime-english-dict.bin"
#endif

    EnglishDefinitionDictionary dict;
    assertTrue(!dict.isLoaded(), "initially not loaded");
    assertTrue(dict.lookup("apple").empty(), "lookup on unloaded returns empty");

    const bool loaded = dict.load(MODERNIME_ENGLISH_DICT_FILE);
    assertTrue(loaded, "loads binary dictionary successfully");
    assertTrue(dict.isLoaded(), "isLoaded is true");
    assertTrue(dict.entryCount() > 50000, "entry count is over 50,000");

    // 精确查找与前缀查找验证
    const auto garageDef = dict.lookup("garage");
    assertTrue(!garageDef.empty(), "garage lookup not empty");
    assertTrue(garageDef.find("车库") != std::string_view::npos, "garage contains 车库");

    const auto appleDef = dict.lookup("apple");
    assertTrue(!appleDef.empty(), "apple lookup not empty");
    assertTrue(appleDef.find("苹果") != std::string_view::npos, "apple contains 苹果");

    // 大小写不敏感测试
    const auto garageUpper = dict.lookup("GARAGE");
    assertTrue(garageUpper == garageDef, "GARAGE uppercase lookup matches lowercase");

    const auto appleCapitalized = dict.lookup("Apple");
    assertTrue(appleCapitalized == appleDef, "Apple capitalized lookup matches lowercase");

    // 不存在的词
    assertTrue(dict.lookup("nonexistentwordxyz123").empty(), "nonexistent word returns empty");
    assertTrue(dict.lookup("").empty(), "empty word returns empty");

    // 关闭后行为
    dict.close();
    assertTrue(!dict.isLoaded(), "not loaded after close");
    assertTrue(dict.lookup("apple").empty(), "lookup after close returns empty");
}

} // namespace

int main() {
    testCleanDefinitionRules();
    testDictionaryLookup();
    return EXIT_SUCCESS;
}
