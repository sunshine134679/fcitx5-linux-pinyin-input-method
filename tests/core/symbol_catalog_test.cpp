#include "modernime/core/symbol_catalog.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {
void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "symbol catalog test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool includes(const std::vector<const modernime::core::SymbolEntry *> &entries,
              std::string_view text) {
    return std::any_of(entries.begin(), entries.end(), [text](const auto *entry) {
        return entry->text == text;
    });
}
} // namespace

int main() {
    using modernime::core::SymbolCatalog;
    using modernime::core::symbolMacroValues;
    require(includes(SymbolCatalog::search("感叹号"), "！"), "Chinese name search");
    require(includes(SymbolCatalog::search("gan tan hao"), "！"), "pinyin alias search");
    require(includes(SymbolCatalog::search("RIGHT ARROW"), "→"), "English phrase search");
    require(includes(SymbolCatalog::search("→"), "→"), "symbol substring search");
    require(includes(SymbolCatalog::search("“"), "“”"), "symbol substrings include pairs");
    require(includes(SymbolCatalog::search("摄氏", "units"), "℃"), "unit name and category");
    require(includes(SymbolCatalog::search("微笑", "emoji"), "😄"), "emoji name search");
    require(SymbolCatalog::search("感叹号", "math").empty(), "category excludes other symbols");

    const auto all = SymbolCatalog::search("", "", {}, false, 1000);
    require(all.size() == SymbolCatalog::entries().size(), "empty query lists catalog");
    std::unordered_set<std::string> ids;
    for (const auto *entry : all) {
        require(!entry->name.empty() && entry->id == entry->text,
                "symbol has name and stable text identity");
        require(ids.insert(entry->id).second && SymbolCatalog::find(entry->id) == entry,
                "IDs are unique and result pointers refer to catalog");
    }
    const std::vector<std::string> favorites = {"→", "❤️", "not-a-symbol"};
    const auto favoriteOnly = SymbolCatalog::search("", "", favorites, true);
    require(favoriteOnly.size() == 2 && includes(favoriteOnly, "→") &&
                includes(favoriteOnly, "❤️"), "favorites filter ignores unknown IDs");
    const auto favoriteFirst = SymbolCatalog::search("", "", favorites, false, 2);
    require(favoriteFirst.size() == 2 && includes(favoriteFirst, "→") &&
                includes(favoriteFirst, "❤️"), "favorites take priority before limit");
    const auto arrows = SymbolCatalog::search("", "arrows", favorites, true);
    require(arrows.size() == 1 && arrows.front()->text == "→",
            "category and favorites filters compose");
    require(SymbolCatalog::find("not-a-symbol") == nullptr &&
                SymbolCatalog::search("no-such-symbol-query").empty(), "unknown search is empty");

    require(symbolMacroValues("gantanhao") == std::vector<std::string>({"！", "!", "❗️"}),
            "punctuation macro values and order preserved");
    require(symbolMacroValues("dayudengyu") == std::vector<std::string>({"≥", ">="}),
            "math alternatives preserved");
    require(symbolMacroValues("jiantou") == std::vector<std::string>({"→", "←", "↑", "↓"}),
            "arrow macro order preserved");
    require(symbolMacroValues("fanxiegang") == std::vector<std::string>({"\\"}),
            "escaped punctuation preserved");
    require(symbolMacroValues("xin") == std::vector<std::string>({"❤️"}),
            "emoji variation selector preserved");
    require(symbolMacroValues("rq").empty() && symbolMacroValues("v123").empty(),
            "dynamic macros stay with provider");
    std::cout << "symbol catalog tests passed: " << all.size() << " entries\n";
}
