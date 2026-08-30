#include "modernime/settings/settings_shell_state.h"

#include <cassert>
#include <memory>
#include <string_view>

namespace {

struct FakeLifetime final {
    void deactivate() { deactivated = true; }
    bool deactivated = false;
};

} // namespace

int main() {
    using namespace modernime::settings;

    assert(chooseSettingsFocusDestination(true, true) ==
           SettingsFocusDestination::Target);
    assert(chooseSettingsFocusDestination(false, true) ==
           SettingsFocusDestination::Fallback);
    assert(chooseSettingsFocusDestination(false, false) ==
           SettingsFocusDestination::None);

    const auto toggle = settingsFocusRouteForIssue("input.toggle_key");
    assert(toggle.has_value());
    assert(toggle->page == SettingsPageId::Input);
    assert(toggle->target == "toggle-key");

    const auto clipboard = settingsFocusRouteForIssue("clipboard.trigger");
    assert(clipboard.has_value());
    assert(clipboard->page == SettingsPageId::Clipboard);
    assert(clipboard->target == "clipboard-trigger");
    assert(!settingsFocusRouteForIssue("unknown.issue").has_value());

    OverviewRefreshState refresh;
    const auto first = refresh.request();
    assert(first.has_value() && *first == 1);
    assert(!refresh.request().has_value());
    assert(refresh.generation() == 2);
    assert(!refresh.complete(*first));
    const auto replacement = refresh.startPending();
    assert(replacement.has_value() && *replacement == 2);
    assert(refresh.complete(*replacement));
    assert(!refresh.startPending().has_value());

    auto lifetime = std::make_shared<FakeLifetime>();
    try {
        ScopedLifetimeDeactivation<FakeLifetime> guard(lifetime);
        assert(guard.state() == lifetime);
        assert(!lifetime->deactivated);
        throw 7;
    } catch (int value) {
        assert(value == 7);
    }
    assert(lifetime->deactivated);
}
