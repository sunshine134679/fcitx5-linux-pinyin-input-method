#include "modernime/fcitx5/engine.h"
#include "modernime/fcitx5/fcitx_engine.h"

#include <fcitx-utils/keysymgen.h>

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "clipboard trigger test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool fires(std::string_view preedit, const fcitx::Key &key,
           std::string_view trigger) {
    return modernime::fcitx5::clipboardTriggerFire(preedit, key, trigger);
}

} // namespace

int main() {
    const fcitx::Key digitTwo(FcitxKey_2);
    const fcitx::Key digitOne(FcitxKey_1);
    const fcitx::Key digitSeven(FcitxKey_7);
    const fcitx::Key ctrlDigitTwo(
        FcitxKey_2, fcitx::KeyStates(fcitx::KeyState::Ctrl));
    const auto shift = fcitx::KeyStates(fcitx::KeyState::Shift);
    const fcitx::Key shiftDigitTwo(FcitxKey_2, shift);

    // 组合恰好是单个触发字母时，触发数字打开剪贴板。
    assertTrue(fires("v", digitTwo, "V+2"),
               "single-letter preedit fires with the trigger digit");
    assertTrue(fires("V", digitTwo, "V+2"),
               "preedit case is normalized before matching");
    assertTrue(fires("b", digitSeven, "B+7"), "custom trigger letter fires");
    assertTrue(fires("B", digitSeven, "B+7"), "custom uppercase letter fires");

    // 组合不是单个触发字母或按键不匹配时，不触发任何东西。
    assertTrue(!fires("vn", digitTwo, "V+2"),
               "a longer preedit never fires the trigger");
    assertTrue(!fires("", digitTwo, "V+2"), "an empty preedit never fires");
    assertTrue(!fires("v", digitOne, "V+2"),
               "a different digit never fires the trigger");
    assertTrue(!fires("v", digitTwo, "B+7"),
               "a matching key with a different trigger does not fire");
    assertTrue(!fires("v", shiftDigitTwo, "V+2"),
               "shift-modified digits never fire");
    assertTrue(!fires("v", ctrlDigitTwo, "V+2"),
               "ctrl-modified digits never fire");
    assertTrue(!fires("v", digitTwo, "V+0"),
               "an invalid trigger never fires");
    return EXIT_SUCCESS;
}