#include "modernime/settings/settings_ui_contract.h"
#include "modernime/settings/page_registry.h"

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
    assertTrue(pages[0].title == "概览", "overview is the first page");
    assertTrue(pages[1].title == "输入体验", "input page follows overview");
    assertTrue(pages[2].title == "个人词典", "dictionary uses approved copy");
    assertTrue(pages[3].title == "剪贴板", "clipboard uses approved copy");
    assertTrue(pages[4].title == "智能学习", "learning uses approved copy");
    assertTrue(pages[5].title == "系统与诊断",
               "diagnostics uses approved copy");

    const auto actions = modernime::settings::settingsActionLabels();
    assertTrue(actions.size() == 3, "all bottom bar actions are declared");
    assertTrue(actions[0] == "恢复修改" && actions[1] == "应用" &&
                   actions[2] == "保存并关闭",
               "bottom bar actions follow the approved order");

    const auto defaultSize = modernime::settings::settingsDefaultWindowSize();
    assertTrue(defaultSize.width == 860 && defaultSize.height == 620,
               "settings window uses the approved default size");
    const auto minimumSize = modernime::settings::settingsMinimumWindowSize();
    assertTrue(minimumSize.width == 720 && minimumSize.height == 520,
               "settings window uses the approved minimum size");

    const auto clipboardActions =
        modernime::settings::settingsClipboardActionLabels();
    assertTrue(clipboardActions.size() == 3,
               "clipboard history exposes all management actions");
    assertTrue(clipboardActions[0] == "复制选中" &&
                   clipboardActions[1] == "删除选中" &&
                   clipboardActions[2] == "清空历史",
               "clipboard action labels have clear semantics");

    const auto runtimeLabels =
        modernime::settings::settingsRuntimeStatusLabels();
    assertTrue(runtimeLabels.size() == 4,
               "runtime page exposes all four status dimensions");
    assertTrue(runtimeLabels[0] == "fcitx5-remote" &&
                   runtimeLabels[1] == "Fcitx5 服务" &&
                   runtimeLabels[2] == "当前输入法" &&
                   runtimeLabels[3] == "ModernIME",
               "runtime status labels have clear semantics");

    const auto pageSurfaceClasses =
        modernime::settings::settingsPageSurfaceStyleClasses();
    assertTrue(pageSurfaceClasses.size() == 4 &&
                   pageSurfaceClasses[0] == "modernime-page-scroller" &&
                   pageSurfaceClasses[1] == "modernime-page-viewport" &&
                   pageSurfaceClasses[2] == "modernime-page-stack" &&
                   pageSurfaceClasses[3] == "modernime-page-surface",
               "page stack, scroller, viewport and surface have explicit styles");
    return EXIT_SUCCESS;
}
