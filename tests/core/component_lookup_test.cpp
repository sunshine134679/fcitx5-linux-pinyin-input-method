#include "modernime/core/component_lookup.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {
void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "component lookup test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void expectCharacter(const modernime::core::ComponentLookup &lookup,
                     std::string_view query, std::string_view character,
                     std::string_view components) {
    const auto results = lookup.lookup(query);
    const auto found = std::find_if(results.begin(), results.end(),
                                    [character](const auto &candidate) {
        return candidate.text == character;
    });
    require(found != results.end(), std::string(query) + " finds " +
                                    std::string(character));
    require(found->components == components, "original decomposition retained");
    require(std::count_if(results.begin(), results.end(),
                         [character](const auto &candidate) {
        return candidate.text == character;
    }) == 1, "one result per character");
}
} // namespace

int main(int argc, char **argv) {
    const modernime::core::ComponentLookup lookup(
        argc > 1 ? std::filesystem::path(argv[1]) :
                   modernime::core::ComponentLookup::defaultPath());
    require(lookup.available(), lookup.loadError());
    expectCharacter(lookup, "木木", "林", "木 木");
    expectCharacter(lookup, "mumu", "林", "木 木");
    expectCharacter(lookup, "木木木", "森", "木 木 木");
    expectCharacter(lookup, " Mu + MU + mu ", "森", "木 木 木");
    expectCharacter(lookup, "日日日", "晶", "日 日 日");
    expectCharacter(lookup, "ri'ri'ri", "晶", "日 日 日");
    expectCharacter(lookup, "riyue", "明", "日 月");
    expectCharacter(lookup, "nvzi", "好", "女 子");
    expectCharacter(lookup, "shuishuishui", "淼", "水 水 水");
    require(lookup.lookup("不存在的部件").empty(), "unknown components are empty");
    require(lookup.lookup("xyzxyzxyz").empty(), "unknown pinyin is empty");
    require(lookup.lookup("").empty(), "empty query is empty");
    require(lookup.lookup("mumu", 0).empty(), "zero limit is empty");
    require(lookup.lookup("mumu", 1).size() == 1, "result limit is respected");

    const modernime::core::ComponentLookup missing(
        std::filesystem::path("/modernime-component-data-does-not-exist.tsv"));
    require(!missing.available() && !missing.loadError().empty(),
            "missing data is reported");
    require(missing.lookup("mumu").empty(), "load errors are not candidates");
    std::cout << "component lookup tests passed\n";
}
