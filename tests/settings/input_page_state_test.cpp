#include "modernime/settings/pages/input_page.h"

#include <cassert>
#include <filesystem>
#include <string>

int main() {
    const auto path = std::filesystem::temp_directory_path() /
                      "modernime-input-page-state-test.conf";
    std::error_code error;
    std::filesystem::remove(path, error);

    modernime::settings::SettingsWindowModel model(path);
    auto settings = model.settings();
    settings.inputEnabled = false;
    settings.toggleKey = "Ctrl Space";
    model.setSettings(settings);

    const auto state = modernime::settings::deriveInputPageState(model);
    assert(!state.dependentControlsSensitive);
    assert(!state.toggleKeyValid);
    assert(state.toggleKeyMessage.find("只能包含") != std::string::npos);
    assert(!state.canApply);
    return 0;
}
