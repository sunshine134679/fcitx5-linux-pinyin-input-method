#include "modernime/settings/settings_ui_contract.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "settings UI contract test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    const auto pages = modernime::settings::settingsPageDefinitions();
    assertTrue(pages.size() == 6, "all six settings pages are declared");
    assertTrue(pages[0].name == "basic" && pages[0].title == "基本设置",
               "basic page contract is stable");
    assertTrue(pages[1].name == "candidate" &&
                   pages[1].title == "候选设置",
               "candidate page contract is stable");
    assertTrue(pages[2].name == "clipboard" && pages[2].title == "剪贴板",
               "clipboard page contract is stable");
    assertTrue(pages[3].name == "learning" && pages[3].title == "智能学习",
               "learning page contract is stable");
    assertTrue(pages[4].name == "dictionary" &&
                   pages[4].title == "用户词典",
               "dictionary page contract is stable");
    assertTrue(pages[5].name == "status" && pages[5].title == "输入法状态",
               "status page contract is stable");

    const auto actions = modernime::settings::settingsActionLabels();
    assertTrue(actions.size() == 4, "all primary actions are declared");
    assertTrue(actions[0] == "应用" && actions[1] == "保存并关闭" &&
                   actions[2] == "恢复修改" && actions[3] == "恢复默认",
               "primary action labels have clear semantics");
    return EXIT_SUCCESS;
}
