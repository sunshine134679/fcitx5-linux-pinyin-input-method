#include "modernime/settings/dictionary_pronunciation.h"
#include <libime/pinyin/pinyindictionary.h>
#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <unistd.h>

int main() {
    using modernime::settings::DictionaryPronunciation;
    const auto root = std::filesystem::temp_directory_path() /
                      ("modernime-pronunciation-" + std::to_string(getpid()));
    std::filesystem::create_directories(root);
    const auto file = root / "fixture.dict";
    libime::PinyinDictionary dictionary;
    dictionary.addWord(0, "chong'qing", "重庆");
    dictionary.addWord(0, "zhong'liang", "重量");
    dictionary.addWord(0, "zhong'xin", "重心");
    dictionary.addWord(0, "yin'hang", "银行");
    dictionary.addWord(0, "xing'zhang", "行长");
    dictionary.addWord(0, "hang'zhang", "行长");
    dictionary.addWord(0, "wang", "王");
    dictionary.addWord(0, "xiao", "小");
    dictionary.addWord(0, "ming", "明");
    dictionary.save(0, file.c_str(), libime::PinyinDictFormat::Binary);
    const auto model = DictionaryPronunciation::load({root / "missing", file});
    assert(model.available());
    const auto city = model.suggest("重庆");
    assert(!city.approximate);
    assert(city.readings == std::vector<std::string>{"chong'qing"});
    const auto name = model.suggest("王小明");
    assert(name.approximate);
    assert(name.readings == std::vector<std::string>{"wang'xiao'ming"});
    const auto polyphonic = model.suggest("行长");
    assert(polyphonic.readings.size() == 2);
    assert(std::find(polyphonic.readings.begin(), polyphonic.readings.end(),
                     "hang'zhang") != polyphonic.readings.end());
    assert(model.suggest("").readings.empty());
    assert(model.suggest("未知🙂").readings.empty());
    assert(model.suggest("abc").readings.empty());
    assert(model.suggest(std::string("\xff")).readings.empty());
    assert(model.suggest(std::string(2048, 'a')).readings.empty());
    assert(!DictionaryPronunciation::load({root / "missing"}).available());
    assert(!DictionaryPronunciation::load({file}, [] { return true; }).available());
    std::ofstream(root / "broken.dict") << "invalid binary";
    assert(!DictionaryPronunciation::load({root / "broken.dict"}).available());
    std::filesystem::remove_all(root);
}
