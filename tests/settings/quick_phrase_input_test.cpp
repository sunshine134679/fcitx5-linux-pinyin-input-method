#include "modernime/settings/quick_phrase_store.h"
#include <fcitx/instance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <unistd.h>
using namespace modernime::settings;
class Context final : public fcitx::InputContext {
public:
    explicit Context(fcitx::InputContextManager &manager) : InputContext(manager, "modernime-quickphrase-test") { created(); }
    ~Context() override { destroy(); }
    const char *frontend() const override { return "modernime-quickphrase-test"; }
    std::string committed;
private:
    void commitStringImpl(const std::string &text) override { committed += text; }
    void deleteSurroundingTextImpl(int, unsigned int) override {}
    void forwardKeyImpl(const fcitx::ForwardKeyEvent &) override {}
    void updatePreeditImpl() override {}
};
int main() {
    auto root = std::filesystem::temp_directory_path() / ("modernime-phrase-input-" + std::to_string(getpid()));
    setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
    setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
    const auto path = quickPhrasePath();
    const std::string text = " 地址 \\ A栋 \"前台\" ";
    assert(saveQuickPhrases(path, loadQuickPhrases(path), {{"zzmodernime", text}}).saved());
    char program[] = "modernime-phrase-input-test";
    char disable[] = "--disable=all";
    char enable[] = "--enable=quickphrase";
    char *argv[]{program, disable, enable, nullptr};
    {
        fcitx::Instance instance(3, argv);
        instance.addonManager().registerDefaultLoader(nullptr);
        instance.initialize();
        assert(instance.addonManager().addon("quickphrase", true));
        Context context(instance.inputContextManager());
        context.focusIn();
        const auto key = [&](const fcitx::Key &value) {
            fcitx::KeyEvent event(&context, value);
            context.keyEvent(event);
        };
        const auto trigger = [&] { key(fcitx::Key("Super+semicolon")); };
        const auto type = [&] {
            for (const auto c : std::string("zzmodernime")) key(fcitx::Key(static_cast<fcitx::KeySym>(c)));
        };
        trigger(); type();
        const auto list = context.inputPanel().candidateList();
        assert(list && list->size() > 0);
        assert(list->candidate(0).text().toString().find(text) == 0);
        key(fcitx::Key(FcitxKey_space));
        assert(context.committed == text);
        auto snapshot = loadQuickPhrases(path);
        assert(saveQuickPhrases(path, snapshot, {{"zzmodernime", "已编辑"}}).saved());
        instance.reloadAddonConfig("quickphrase");
        trigger(); type(); key(fcitx::Key(FcitxKey_1));
        assert(context.committed == text + "已编辑");
        trigger(); type(); key(fcitx::Key(FcitxKey_Escape));
        assert(context.committed == text + "已编辑");
    }
    std::filesystem::remove_all(root);
}
