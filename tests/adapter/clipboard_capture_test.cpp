// Exercises the production shared addon and its real timer. Only the external
// clipboard source and keyboard layout are synthetic; no desktop is connected.
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
#include <fcitx-utils/event.h>
#include <fcitx-utils/testing.h>
#include <modernime/core/settings.h>
#include <modernime/core/clipboard_history.h>
#include <modernime/core/clipboard_queries.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <sys/wait.h>
#include <fcitx/inputpanel.h>
#include <fcitx/candidatelist.h>
#include <fcitx-utils/keysymgen.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>
#include <stdexcept>
#include <iterator>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "../support/clipboard_io_gate.h"

FCITX_ADDON_DECLARE_FUNCTION(Clipboard, clipboard, std::string(const fcitx::InputContext *));
class Clipboard final : public fcitx::AddonInstance {
public:
    std::string text;
    bool failReads = false;
    int reads = 0;
    std::string clipboard(const fcitx::InputContext *) {
        ++reads;
        if (failReads) {
            throw std::runtime_error("synthetic read failure");
        }
        return text;
    }
    FCITX_ADDON_EXPORT_FUNCTION(Clipboard, clipboard)
};
class Keyboard final : public fcitx::InputMethodEngine {
public:
    std::vector<fcitx::InputMethodEntry> listInputMethods() override {
        std::vector<fcitx::InputMethodEntry> entries;
        entries.emplace_back("keyboard-us", "Test keyboard", "en", "keyboard");
        return entries;
    }
    void keyEvent(const fcitx::InputMethodEntry &, fcitx::KeyEvent &) override {}
};
template<class T> class Factory final : public fcitx::AddonFactory {
    fcitx::AddonInstance *create(fcitx::AddonManager *) override { return new T; }
};
class Context final : public fcitx::InputContext {
public:
    explicit Context(fcitx::InputContextManager &manager) : InputContext(manager,"synthetic-test") { created(); }
    ~Context() override { destroy(); }
    const char *frontend() const override { return "synthetic-test"; }
    std::vector<std::string> commits;
    void commitStringImpl(const std::string &text) override { commits.push_back(text); }
    void deleteSurroundingTextImpl(int,unsigned int) override {}
    void forwardKeyImpl(const fcitx::ForwardKeyEvent &) override {}
    void updatePreeditImpl() override {}
};

namespace {
void require(bool value, const std::string &message) {
    if (!value) throw std::runtime_error(message);
}
struct Cleanup {
    std::filesystem::path root;
    ~Cleanup() { std::error_code ec; std::filesystem::remove_all(root, ec); }
};
void key(Context &ic, fcitx::KeySym symbol) {
    fcitx::KeyEvent event(&ic, fcitx::Key(symbol));
    ic.keyEvent(event);
}
void verifyCandidates(Context &ic, const std::vector<std::string> &expected) {
    key(ic, FcitxKey_Escape);
    key(ic, FcitxKey_v);
    key(ic, FcitxKey_2);
    const auto list = ic.inputPanel().candidateList();
    require(list != nullptr, "V+2 publishes a real candidate list");
    if (expected.empty()) {
        require(list->size() == 1 && list->candidate(0).text().toString() == "(剪贴板为空)",
                "V+2 shows empty history after external clear");
    } else {
        require(list->size() == static_cast<int>(expected.size()), "V+2 visible history size");
        for (size_t i=0; i<expected.size(); ++i)
            require(list->candidate(static_cast<int>(i)).text().toString() == expected[i], "V+2 exact history order");
    }
    key(ic, FcitxKey_Escape);
}

class HeldClipboardIo {
public:
    ~HeldClipboardIo() { release(); if (watchdog.joinable()) watchdog.join(); }
    void start(const std::filesystem::path &path, int operation) {
        modernime_clipboard_gate_arm(path.c_str(), operation);
        watchdog = std::thread([this] {
            reached = modernime_clipboard_gate_wait(4000);
            std::unique_lock lock(mutex);
            if (!changed.wait_for(lock, std::chrono::seconds(2), [&] { return progressed; })) expired = true;
            lock.unlock();
            modernime_clipboard_gate_release(); released = true;
        });
    }
    void release() { std::lock_guard lock(mutex); progressed = true; changed.notify_all(); }
    std::atomic<bool> reached{false}, expired{false}, released{false};
private:
    std::thread watchdog;
    std::mutex mutex;
    std::condition_variable changed;
    bool progressed = false;
};
int asyncScenario(fcitx::Instance &instance, Context &ic, Clipboard &clipboard,
                  const modernime::core::SettingsPaths &paths, const std::string &mode,
                  HeldClipboardIo &io) {
    const bool writing = mode == "async-write";
    const bool cancel = mode == "async-cancel";
    const auto start = fcitx::now(CLOCK_MONOTONIC);
    int phase = writing ? 0 : 1, heldBeats = 0, readsAtRelease = 0;
    bool typed = false;
    std::string failure;
    auto top = [&]() -> std::string {
        const auto list = ic.inputPanel().candidateList();
        return list && list->size() ? list->candidate(0).text().toString() : "";
    };
    auto open = [&] { key(ic, FcitxKey_Escape); key(ic, FcitxKey_v); key(ic, FcitxKey_2); };
    auto timer = instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC, start + 10000, 1,
        [&](fcitx::EventSourceTime *source, uint64_t) {
            try {
                require(fcitx::now(CLOCK_MONOTONIC) - start < 12000000, "async scenario completes");
                require(!io.expired, "main loop and keys progress while clipboard I/O is held");
                if (phase == 0 && clipboard.reads >= 1) {
                    io.start(paths.clipboardHistory, clipboardBeforeFsync);
                    clipboard.text = "async-copy";
                    phase = 1;
                }
                if (phase == 1 && io.reached) {
                    ++heldBeats;
                    if (!typed) {
                        for (char c : std::string("nihao")) key(ic, static_cast<fcitx::KeySym>(c));
                        typed = true;
                    }
                    if (top() == "你好" && heldBeats >= 3) {
                        key(ic, FcitxKey_space);
                        require(ic.commits == std::vector<std::string>{"你好"}, "real pinyin commit completes during held I/O");
                        if (!writing) {
                            open();
                            require(top() == "(正在读取剪贴板…)", "slow load exposes noncommitting Loading state");
                            if (cancel) key(ic, FcitxKey_Escape);
                        }
                        readsAtRelease = clipboard.reads;
                        io.release();
                        phase = 2;
                    }
                } else if (phase == 2 && io.released) {
                    if (cancel) {
                        require(top() != "load-fixture" && top() != "(正在读取剪贴板…)", "completion never reopens cancelled menu");
                        if (clipboard.reads < readsAtRelease + 2) {
                            source->setNextInterval(10000); source->setEnabled(true); return true;
                        }
                        open();
                    } else if (writing) open();
                    if (top() == (writing ? "async-copy" : "load-fixture")) {
                        key(ic, FcitxKey_1);
                        require(ic.commits == std::vector<std::string>({"你好", writing ? "async-copy" : "load-fixture"}),
                                "loaded history commits exact original text");
                        source->setEnabled(false); instance.eventLoop().exit(); return true;
                    }
                    if (cancel || writing) key(ic, FcitxKey_Escape);
                }
            } catch (const std::exception &error) {
                failure = error.what(); io.release();
                source->setEnabled(false); instance.eventLoop().exit(); return true;
            }
            source->setNextInterval(10000); source->setEnabled(true); return true;
        });
    instance.eventLoop().exec();
    io.release();
    if (!failure.empty()) { std::cerr << "FAIL " << mode << ": " << failure << '\n'; return 1; }
    std::cout << "PASS " << mode << " held-heartbeats=" << heldBeats << '\n';
    return 0;
}

int navigationScenario(fcitx::Instance &instance, Context &ic, Clipboard &clipboard,
                       const modernime::core::SettingsPaths &paths, const std::string &mode,
                       HeldClipboardIo &io) {
    const bool copying = mode == "navigation-copy";
    const auto start = fcitx::now(CLOCK_MONOTONIC);
    int phase = 0, readsAtRelease = 0;
    std::shared_ptr<fcitx::CandidateList> selected;
    std::string expected, failure;
    auto timer = instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC, start + 10000, 1,
        [&](fcitx::EventSourceTime *source, uint64_t) {
            try {
                require(fcitx::now(CLOCK_MONOTONIC) - start < 12000000, "navigation scenario completes");
                require(!io.expired, "navigation runs while actual clipboard I/O is held");
                if (phase == 0 && clipboard.reads >= 2) {
                    if (copying) {
                        io.start(paths.clipboardHistory, clipboardBeforeFsync);
                        clipboard.text = "navigation-copy";
                    } else {
                        // Force a real reload with identical empty contents. The
                        // clipboard observation and published entries stay unchanged.
                        modernime::core::ClipboardHistory unchanged;
                        require(unchanged.save(paths.clipboardHistory), "unchanged history replaced");
                        io.start(paths.clipboardHistory, clipboardAfterRead);
                    }
                    phase = 1;
                }
                if (phase == 1 && io.reached) {
                    for (char c : std::string("shi")) key(ic, static_cast<fcitx::KeySym>(c));
                    auto list = ic.inputPanel().candidateList();
                    require(list && list->toPageable() && list->toPageable()->hasNext(), "real pinyin has a second page");
                    list->toPageable()->next();
                    list = ic.inputPanel().candidateList();
                    require(list->size() > 1 && list->toCursorMovable(), "second page supports navigation");
                    list->toCursorMovable()->nextCandidate();
                    selected = ic.inputPanel().candidateList();
                    require(selected->toPageable()->currentPage() == 1 && selected->cursorIndex() == 1,
                            "nonfirst candidate on second physical page selected");
                    expected = selected->candidate(1).text().toString();
                    readsAtRelease = clipboard.reads;
                    io.release();
                    phase = 2;
                } else if (phase == 2 && io.released) {
                    const auto list = ic.inputPanel().candidateList();
                    // A rebuilt page has a new list and selection generation.
                    require(list == selected && list->cursorIndex() == 1 && list->toPageable()->currentPage() == 1,
                            "clipboard completion preserves pinyin page, cursor and selection identity");
                    if (clipboard.reads >= readsAtRelease + 2) {
                        modernime::core::ClipboardHistory disk;
                        require(disk.load(paths.clipboardHistory), "completed clipboard history readable");
                        require(disk.entries() == (copying ? std::vector<std::string>{"navigation-copy"} : std::vector<std::string>{}),
                                "clipboard task completed with exact expected contents");
                        if (copying) key(ic, FcitxKey_space);
                        else selected->candidate(1).select(&ic); // original mouse selection remains valid
                        require(ic.commits == std::vector<std::string>{expected}, "original navigated candidate commits exactly");
                        source->setEnabled(false); instance.eventLoop().exit(); return true;
                    }
                }
            } catch (const std::exception &error) {
                failure = error.what(); io.release();
                source->setEnabled(false); instance.eventLoop().exit(); return true;
            }
            source->setNextInterval(10000); source->setEnabled(true); return true;
        });
    instance.eventLoop().exec();
    io.release();
    if (!failure.empty()) { std::cerr << "FAIL " << mode << ": " << failure << '\n'; return 1; }
    std::cout << "PASS " << mode << " exact-navigation-commit\n";
    return 0;
}

int worker(const std::string &addonDirectory, const std::string &mode) {
    char path[]="/tmp/modernime-clipboard-probe-XXXXXX";
    const auto *tmp=mkdtemp(path);
    require(tmp != nullptr, "isolated directory created");
    const std::filesystem::path root(tmp);
    Cleanup cleanup{root};
    std::filesystem::create_directories(root/"metadata/addon");
    std::ofstream(root/"metadata/addon/clipboard.conf") << "[Addon]\nName=Test clipboard\nType=StaticLibrary\nLibrary=testclipboard\nCategory=Module\nVersion=1.0\nOnDemand=False\n";
    std::ofstream(root/"metadata/addon/keyboard.conf") << "[Addon]\nName=Test keyboard\nType=StaticLibrary\nLibrary=testkeyboard\nCategory=InputMethod\nVersion=1.0\nOnDemand=False\n";
    std::ofstream(root/"metadata/addon/modernime.conf") << "[Addon]\nName=ModernIME\nType=SharedLibrary\nLibrary=modernime_fcitx5\nCategory=InputMethod\nVersion=0.1.6\nOnDemand=False\n";
    fcitx::setupTestingEnvironment(root.string(), {addonDirectory}, {"metadata"});
    for (const auto *name : {"HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME",
                             "XDG_CACHE_HOME", "XDG_STATE_HOME", "XDG_RUNTIME_DIR"}) {
        setenv(name, tmp, 1);
    }
    // Prevent caller-specific model overrides from reading personal fixtures.
    for (const auto *name : {"MODERNIME_PINYIN_DICT", "LIBIME_PINYIN_DICT",
                             "MODERNIME_PINYIN_LM", "LIBIME_PINYIN_LM",
                             "MODERNIME_PINYIN_KNOWLEDGE_DICTIONARY",
                             "MODERNIME_PINYIN_HOTWORDS_DICTIONARY",
                             "MODERNIME_ENGLISH_DEFINITION_DICTIONARY"}) {
        unsetenv(name);
    }
    unsetenv("DISPLAY");
    unsetenv("WAYLAND_DISPLAY");
    unsetenv("DBUS_SESSION_BUS_ADDRESS");
    auto settings=modernime::core::defaultSettings();
    settings.learningEnabled=false;
    const auto paths=modernime::core::SettingsPaths::fromEnvironment(tmp,tmp,tmp);
    require(modernime::core::SettingsStore::save(paths.settingsFile,settings), "settings saved");
    if (mode == "search-favorites") {
        modernime::core::ClipboardHistory seed; seed.observe("中文地址"); seed.observe("Alpha42");
        require(seed.save(paths.clipboardHistory), "search fixture saved");
    }
    HeldClipboardIo heldIo;
    if (mode == "async-load" || mode == "async-cancel") {
        modernime::core::ClipboardHistory seed; seed.observe("load-fixture");
        require(seed.save(paths.clipboardHistory), "slow-load fixture saved");
        heldIo.start(paths.clipboardHistory, clipboardAfterRead);
    }
    Factory<Clipboard> cf;
    Factory<Keyboard> kf;
    fcitx::StaticAddonRegistry registry{{"clipboard",&cf},{"keyboard",&kf}};
    char a0[]="clipboard-probe",a1[]="--disable=all",a2[]="--enable=clipboard,keyboard,modernime";
    char *args[]={a0,a1,a2,nullptr};
    fcitx::Instance instance(3,args);
    instance.addonManager().registerDefaultLoader(&registry);
    instance.initialize();
    auto *clipboard=dynamic_cast<Clipboard *>(instance.addonManager().addon("clipboard",true));
    require(clipboard && instance.addonManager().addon("modernime",true), "real built ModernIME addon loaded");
    auto &ims=instance.inputMethodManager();
    fcitx::InputMethodGroup group("Test");
    group.setDefaultLayout("us");
    group.inputMethodList().emplace_back("keyboard-us");
    group.inputMethodList().emplace_back("modernime");
    group.setDefaultInputMethod("modernime");
    ims.addEmptyGroup("Test"); ims.setGroup(std::move(group)); ims.setCurrentGroup("Test");
    Context ic(instance.inputContextManager());
    ic.setCapabilityFlags(fcitx::CapabilityFlag::Preedit);
    ic.focusIn();
    instance.setCurrentInputMethod(&ic,"modernime",false);
    require(instance.inputMethod(&ic) == "modernime" && ic.hasFocus(), "real focused ModernIME selected");

    if (mode == "search-favorites") {
        int phase = 0; std::string failure;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        auto timer = instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 10000, 10000,
            [&](fcitx::EventSourceTime *source, std::uint64_t) {
                try {
                    require(std::chrono::steady_clock::now() < deadline, "clipboard search/favorite completion deadline");
                    modernime::core::ClipboardFavorites favorites(paths.clipboardFavorites);
                    require(favorites.load(), "favorite state readable");
                    if (phase == 0 || phase == 2 || phase == 3) {
                        key(ic,FcitxKey_Escape); key(ic,FcitxKey_v); key(ic,FcitxKey_2);
                        const auto list = ic.inputPanel().candidateList();
                        require(list != nullptr, "history candidate list exists");
                        if (phase == 0 && list->candidate(0).text().toString() == "Alpha42") {
                            fcitx::KeyEvent search(&ic,fcitx::Key(FcitxKey_f,fcitx::KeyState::Ctrl)); ic.keyEvent(search);
                            require(search.accepted(), "Ctrl+F enters native clipboard search");
                            for (char c : std::string("ALPHA4")) key(ic,static_cast<fcitx::KeySym>(c));
                            const auto filtered = ic.inputPanel().candidateList();
                            require(filtered && filtered->size() == 1 && filtered->candidate(0).text().toString() == "Alpha42", "native search retains digit and full matching text");
                            fcitx::KeyEvent save(&ic,fcitx::Key(FcitxKey_d,fcitx::KeyState::Ctrl)); ic.keyEvent(save);
                            require(save.accepted(), "Ctrl+D queues native favorite"); phase = 1;
                        } else if (phase == 2 && list->size() == 1 && list->candidate(0).text().toString() == "Alpha42") {
                            require(favorites.entries() == std::vector<std::string>{"Alpha42"}, "history clear keeps persisted favorite");
                            fcitx::KeyEvent remove(&ic,fcitx::Key(FcitxKey_d,fcitx::KeyState::Ctrl)); ic.keyEvent(remove);
                            require(remove.accepted(), "Ctrl+D removes native favorite"); phase = 3;
                        } else if (phase == 3 && favorites.entries().empty() && list->candidate(0).text().toString() == "(剪贴板为空)") {
                            require(ic.commits == std::vector<std::string>{"Alpha42"}, "favorite operations never commit labels or change text");
                            source->setEnabled(false); instance.eventLoop().exit(); return true;
                        }
                    } else if (phase == 1 && favorites.entries() == std::vector<std::string>{"Alpha42"}) {
                        key(ic,FcitxKey_Return);
                        require(ic.commits == std::vector<std::string>{"Alpha42"}, "native query result commits");
                        require(modernime::core::ClipboardHistory::updateFile(paths.clipboardHistory, [](auto &h) { h.clear(); return true; }), "clear history separately");
                        phase = 2;
                    }
                } catch (const std::exception &e) { failure = e.what(); source->setEnabled(false); instance.eventLoop().exit(); return true; }
                source->setNextInterval(10000); source->setEnabled(true); return true;
            });
        instance.eventLoop().exec();
        if (!failure.empty()) { std::cerr << failure << '\n'; return 1; }
        std::cout << "PASS clipboard native search and favorites\n"; return 0;
    }

    if (mode.starts_with("async-")) return asyncScenario(instance, ic, *clipboard, paths, mode, heldIo);
    if (mode.starts_with("navigation-")) return navigationScenario(instance, ic, *clipboard, paths, mode, heldIo);

    clipboard->text = mode == "startup" ? "synthetic-before-start" : "";
    auto entries = [&]() {
        modernime::core::ClipboardHistory disk;
        std::string error;
        require(disk.load(paths.clipboardHistory, &error), "history readable: " + error);
        return disk.entries();
    };
    auto saveSettings = [&]() {
        require(modernime::core::SettingsStore::save(paths.settingsFile, settings), "hot settings saved");
        return modernime::core::SettingsStore::load(paths.settingsFile).contentHash;
    };
    auto settingsApplied = [&](const std::string &hash) {
        std::ifstream input(paths.runtimeState);
        const std::string content((std::istreambuf_iterator<char>(input)), {});
        return content.find("applied_content_hash=" + hash + "\n") != std::string::npos;
    };
    auto restore = [&]() {
        if (mode.find("focus") != std::string::npos) ic.focusIn();
        if (mode.find("sensitive") != std::string::npos || mode == "password")
            ic.setCapabilityFlags(fcitx::CapabilityFlag::Preedit);
        if (mode.find("ime") != std::string::npos || mode == "password")
            instance.setCurrentInputMethod(&ic, "modernime", false);
        clipboard->failReads = false;
    };
    const bool configMode = mode == "clipboard-disabled" || mode == "input-disabled";
    const bool fast = mode.starts_with("fast-");
    int phase=0, readsBeforeGap=0, resumeReads=0;
    uint64_t gapStarted=0;
    std::string settingsHash, failure;
    const auto started=fcitx::now(CLOCK_MONOTONIC);
    auto timer=instance.eventLoop().addTimeEvent(CLOCK_MONOTONIC,started+20000,0,
      [&](fcitx::EventSourceTime *source,uint64_t) {
        try {
            const auto now=fcitx::now(CLOCK_MONOTONIC);
            require(now-started < 25000000, "scenario deadline; phase=" + std::to_string(phase));
            if (phase == 0 && clipboard->reads >= 1) {
                require(entries().empty(), "startup value must only establish a baseline");
                clipboard->text="synthetic-normal";
                phase=1;
            } else if (phase == 1 && entries() == std::vector<std::string>{"synthetic-normal"}) {
                verifyCandidates(ic, {"synthetic-normal"});
                readsBeforeGap=clipboard->reads;
                if (configMode) {
                    if (mode == "clipboard-disabled") settings.clipboardEnabled=false;
                    else settings.inputEnabled=false;
                    settingsHash=saveSettings();
                    phase=2;
                } else {
                    if (mode.find("sensitive") != std::string::npos)
                        ic.setCapabilityFlags(fcitx::CapabilityFlag::Sensitive);
                    else if (mode == "password") ic.setCapabilityFlags(fcitx::CapabilityFlag::Password);
                    else if (mode.find("focus") != std::string::npos) ic.focusOut();
                    else if (mode.find("ime") != std::string::npos)
                        instance.setCurrentInputMethod(&ic,"keyboard-us",false);
                    else if (mode == "read-failure") clipboard->failReads=true;
                    clipboard->text="synthetic-excluded";
                    gapStarted=now;
                    if (fast) { restore(); resumeReads=clipboard->reads; phase=4; }
                    else if (mode == "startup" || mode == "ordinary-capability") {
                        // Normal capability updates must not discard a legitimate copy.
                        ic.setCapabilityFlags(fcitx::CapabilityFlag::SurroundingText);
                        clipboard->text="synthetic-next";
                        phase=5;
                    } else phase=3;
                }
            } else if (phase == 2 && settingsApplied(settingsHash)) {
                // Begin the excluded copy only after production acknowledges the config.
                readsBeforeGap=clipboard->reads;
                clipboard->text="synthetic-excluded";
                gapStarted=now;
                phase=3;
            } else if (phase == 3 && now-gapStarted >= 1500000) {
                if (mode == "read-failure") require(clipboard->reads > readsBeforeGap, "read failure injected");
                else require(clipboard->reads == readsBeforeGap, "excluded interval must not query Clipboard");
                require(entries() == std::vector<std::string>{"synthetic-normal"}, "excluded interval must not persist content");
                restore(); resumeReads=clipboard->reads;
                if (configMode) {
                    settings.clipboardEnabled=true; settings.inputEnabled=true;
                    settingsHash=saveSettings();
                }
                phase=4;
            } else if (phase == 4 && clipboard->reads >= resumeReads+2) {
                if (configMode) require(settingsApplied(settingsHash), "reenabled config applied");
                require(entries() == std::vector<std::string>{"synthetic-normal"}, "resume must not catch up excluded content");
                verifyCandidates(ic, {"synthetic-normal"});
                clipboard->text="synthetic-next";
                phase=5;
            } else if (phase == 5 && entries() == std::vector<std::string>{"synthetic-next", "synthetic-normal"}) {
                verifyCandidates(ic, {"synthetic-next", "synthetic-normal"});
                require(modernime::core::ClipboardHistory::updateFile(paths.clipboardHistory,
                    [](auto &history) { history.clear(); return true; }), "external clear commits");
                resumeReads=clipboard->reads;
                phase=6;
            } else if (phase == 6 && clipboard->reads >= resumeReads+2) {
                require(entries().empty(), "unchanged clipboard cannot resurrect external clear");
                verifyCandidates(ic, {});
                clipboard->text=""; resumeReads=clipboard->reads; phase=7;
            } else if (phase == 7 && clipboard->reads > resumeReads) {
                clipboard->text="synthetic-next"; phase=8;
            } else if (phase == 8 && entries() == std::vector<std::string>{"synthetic-next"}) {
                verifyCandidates(ic, {"synthetic-next"});
                key(ic, FcitxKey_v); key(ic, FcitxKey_2); key(ic, FcitxKey_1);
                require(ic.commits == std::vector<std::string>{"synthetic-next"}, "real selection commits allowed copy");
                source->setEnabled(false); instance.eventLoop().exit(); return true;
            }
        } catch (const std::exception &error) {
            failure=error.what();
            source->setEnabled(false); instance.eventLoop().exit(); return true;
        }
        source->setNextInterval(20000); source->setEnabled(true); return true;
      });
    instance.eventLoop().exec();
    if (!failure.empty()) { std::cerr << "FAIL " << mode << ": " << failure << '\n'; return 1; }
    std::cout << "PASS " << mode << " reads=" << clipboard->reads << '\n';
    return 0;
}
} // namespace
int main(int argc, char **argv) {
    try {
        const bool asyncOnly = argc == 3 && std::string(argv[2]) == "--async";
        if (argc == 3 && !asyncOnly) return worker(argv[1],argv[2]);
        require(argc == 2 || asyncOnly, "addon directory argument required");
        int failures=0;
        const std::vector<const char *> modes = asyncOnly ? std::vector<const char *>{"async-load", "async-write", "async-cancel", "navigation-unchanged", "navigation-copy"} : std::vector<const char *>{"startup", "sensitive", "password", "focus", "other-ime",
                                "fast-sensitive", "fast-focus", "fast-ime", "read-failure",
                                "clipboard-disabled", "input-disabled", "ordinary-capability"};
        for (const auto *mode : modes) {
            const auto pid=fork();
            require(pid >= 0, "worker fork");
            if (!pid) { execl(argv[0],argv[0],argv[1],mode,nullptr); _exit(127); }
            int status=0;
            require(waitpid(pid,&status,0)==pid, "worker wait");
            if (!WIFEXITED(status) || WEXITSTATUS(status)) ++failures;
        }
        std::cout << "clipboard capture scenarios: " << modes.size()-failures << "/" << modes.size() << " passed\n";
        return failures ? 1 : 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
