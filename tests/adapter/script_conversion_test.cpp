#include "modernime/fcitx5/script_conversion.h"
#include "modernime/core/settings.h"
#include <fcitx/action.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonloader.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/inputmethodgroup.h>
#include <fcitx/instance.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputpanel.h>
#include <fcitx/userinterfacemanager.h>
#include <fcitx/statusarea.h>
#include <fcitx-config/configuration.h>
#include <fcitx-utils/testing.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
void require(bool ok, const char *message) { if (!ok) { std::cerr << message << '\n'; std::exit(1); } }
class Keyboard final : public fcitx::InputMethodEngine {
public:
    std::vector<fcitx::InputMethodEntry> listInputMethods() override {
        std::vector<fcitx::InputMethodEntry> result;
        result.emplace_back("keyboard-us", "Keyboard", "en", "keyboard"); return result;
    }
    void keyEvent(const fcitx::InputMethodEntry &, fcitx::KeyEvent &) override {}
};
class Factory final : public fcitx::AddonFactory {
    fcitx::AddonInstance *create(fcitx::AddonManager *) override { return new Keyboard; }
};
class Context final : public fcitx::InputContext {
public:
    explicit Context(fcitx::InputContextManager &manager) : InputContext(manager, "script-test") { created(); }
    ~Context() override { destroy(); }
    const char *frontend() const override { return "script-test"; }
    std::string committed;
    void commitStringImpl(const std::string &text) override { committed += text; }
    void deleteSurroundingTextImpl(int, unsigned) override {}
    void forwardKeyImpl(const fcitx::ForwardKeyEvent &) override {}
    void updatePreeditImpl() override {}
};
}
int main(int argc, char **argv) {
    require(argc == 4, "expected built addon, installed chttrans and native data");
    char rootName[] = "/tmp/modernime-script-XXXXXX";
    require(mkdtemp(rootName), "isolated directory");
    const std::filesystem::path root(rootName);
    std::filesystem::create_directories(root / "metadata/addon");
    std::filesystem::create_directories(root / "metadata/inputmethod");
    std::ofstream(root / "metadata/inputmethod/modernime.conf") << "[InputMethod]\nName=ModernIME\nLangCode=zh_CN\nAddon=modernime\nConfigurable=False\n";
    std::filesystem::create_directories(root / "metadata/chttrans");
    std::filesystem::copy_file(argv[3], root / "metadata/chttrans/gbks2t.tab");
    std::ofstream(root / "metadata/addon/keyboard.conf") << "[Addon]\nName=Keyboard\nType=StaticLibrary\nLibrary=testkeyboard\nCategory=InputMethod\nVersion=1.0\nOnDemand=False\n";
    std::ofstream(root / "metadata/addon/modernime.conf") << "[Addon]\nName=ModernIME\nType=SharedLibrary\nLibrary=modernime_fcitx5\nCategory=InputMethod\nVersion=1.0\nOnDemand=True\n\n[Addon/OptionalDependencies]\n0=chttrans\n";
    std::ofstream(root / "metadata/addon/chttrans.conf") << "[Addon]\nName=Chinese conversion\nType=SharedLibrary\nLibrary=libchttrans\nCategory=Module\nVersion=1.0\nOnDemand=True\nConfigurable=True\n";
    fcitx::setupTestingEnvironment(root.string(), {argv[1], std::filesystem::path(argv[2]).parent_path().string()}, {"metadata"});
    for (const auto *name : {"HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME", "XDG_RUNTIME_DIR"}) setenv(name, rootName, 1);
    unsetenv("DISPLAY"); unsetenv("WAYLAND_DISPLAY"); unsetenv("DBUS_SESSION_BUS_ADDRESS");
    std::filesystem::create_directories(root / "fcitx5/conf");
    std::ofstream(root / "fcitx5/conf/chttrans.conf") << "Engine=Native\n";
    auto settings = modernime::core::defaultSettings(); settings.learningEnabled = false;
    const auto paths = modernime::core::SettingsPaths::fromEnvironment(rootName,rootName,rootName);
    require(modernime::core::SettingsStore::save(paths.settingsFile, settings), "isolated settings");
    {
        Factory factory;
        fcitx::StaticAddonRegistry registry{{"keyboard", &factory}};
        char arg0[]="script-test",arg1[]="--disable=all",arg2[]="--enable=keyboard,modernime,chttrans";
        char *args[]={arg0,arg1,arg2,nullptr};
        fcitx::Instance instance(3,args);
        instance.addonManager().registerDefaultLoader(&registry); instance.initialize();
        require(instance.addonManager().addon("modernime", true), "production dependency metadata loads the real engine");
        auto *conversion = instance.addonManager().addon("chttrans", true);
        require(conversion, "real chttrans addon loaded");
        auto &ims = instance.inputMethodManager(); fcitx::InputMethodGroup group("Test");
        group.setDefaultLayout("us"); group.inputMethodList().emplace_back("keyboard-us"); group.inputMethodList().emplace_back("modernime"); group.setDefaultInputMethod("modernime");
        ims.addEmptyGroup("Test"); ims.setGroup(std::move(group)); ims.setCurrentGroup("Test");
        Context ic(instance.inputContextManager()); ic.focusIn(); instance.setCurrentInputMethod(&ic,"modernime",false);
        auto *action = instance.userInterfaceManager().lookupAction("chttrans");
        require(action && action->isParent(&ic.statusArea()), "ModernIME exposes native script action");
        auto key = [&](fcitx::Key stroke) { fcitx::KeyEvent event(&ic,stroke); ic.keyEvent(event); };
        auto type = [&](std::string_view text) { for (char c : text) key(fcitx::Key(std::string(1,c))); };
        key(fcitx::Key(FcitxKey_F, fcitx::KeyStates({fcitx::KeyState::Ctrl,fcitx::KeyState::Shift})));
        type("ruanjian");
        auto list = ic.inputPanel().candidateList();
        require(list && list->candidate(0).text().toString() == "软件", "learning identity stays simplified");
        require(instance.outputFilter(&ic,list->candidate(0).text()).toString() == "軟件", "native output filter renders traditional");
        key(fcitx::Key(FcitxKey_space));
        require(ic.committed == "軟件", "real key selection commits traditional");
        fcitx::RawConfig config; conversion->getConfig()->save(config);
        require(modernime::fcitx5::traditionalOutput(config), "shortcut state is queryable");
        config["EnabledIM/1"] = "other-input";
        const auto engine = *config.valueByPath("Engine");
        modernime::fcitx5::setTraditionalOutput(config,false);
        require(!modernime::fcitx5::traditionalOutput(config) && config.valueByPath("EnabledIM/0") &&
                *config.valueByPath("EnabledIM/0") == "other-input" && *config.valueByPath("Engine") == engine, "editing script preserves other input methods and converter");
        conversion->setConfig(config);
        type("ruanjian"); key(fcitx::Key(FcitxKey_space));
        ic.commitString("User_42@example.com");
        require(ic.committed == "軟件软件User_42@example.com", "switch back and ASCII passthrough");
    }
    std::filesystem::remove_all(root);
    std::cout << "script conversion test passed\n";
}
