#include "modernime/fcitx5/fcitx_engine.h"
#include "modernime/core/settings.h"

#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/event.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/keysymgen.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

class TestInputContext final : public fcitx::InputContext {
public:
    explicit TestInputContext(fcitx::InputContextManager &m)
        : InputContext(m, "modernime-recovery-test") {
        created();
    }
    ~TestInputContext() override { destroy(); }
    const char *frontend() const override { return "modernime-recovery-test"; }

protected:
    void updatePreeditImpl() override {}
    void commitStringImpl(const std::string &) override {}
    void deleteSurroundingTextImpl(int, unsigned) override {}
    void forwardKeyImpl(const fcitx::ForwardKeyEvent &) override {}
};

void writeConfigFile(const std::filesystem::path &p, bool enabled, int fontSize) {
    std::ofstream f(p);
    f << "input.enabled=" << (enabled ? "true" : "false") << "\n"
      << "candidate.font_size=" << fontSize << "\n"
      << "learning.enabled=false\n"
      << "learning.context_enabled=false\n";
}

int runWorker(const std::string &mode, const std::string &root) {
    setenv("XDG_CONFIG_HOME", root.c_str(), 1);
    setenv("XDG_DATA_HOME", root.c_str(), 1);

    const auto paths = modernime::core::SettingsPaths::fromEnvironment(root, root, root);
    std::filesystem::create_directories(paths.settingsFile.parent_path());

    // Write initial configuration: input.enabled=false, candidate.font_size=14
    writeConfigFile(paths.settingsFile, false, 14);
    const auto initialStamp = modernime::core::SettingsFileStamp::fromPath(paths.settingsFile);

    // Prepare replacement file .b: input.enabled=true, candidate.font_size=28
    const auto fileB = paths.settingsFile.string() + ".b";
    writeConfigFile(fileB, true, 28);

    const bool isStartup = (mode.rfind("startup_", 0) == 0);
    if (isStartup || mode == "negative_permanent_failure") {
        setenv("MODERNIME_FAULT_CONFIG_PATH", paths.settingsFile.c_str(), 1);
        setenv("MODERNIME_FAULT_REPLACEMENT", fileB.c_str(), 1);
        if (mode == "startup_swap") {
            setenv("MODERNIME_FAULT_ACTION", "swap_after_read", 1);
        } else if (mode == "startup_long_failure" || mode == "negative_permanent_failure") {
            setenv("MODERNIME_FAULT_ACTION", "long_read_failure", 1);
        } else {
            setenv("MODERNIME_FAULT_ACTION", "read_failure", 1);
        }
    }

    char prog[] = "recovery_worker";
    char dis[] = "--disable=all";
    char *args[] = {prog, dis, nullptr};
    optind = 0;
    fcitx::Instance instance(2, args);
    instance.initialize();

    modernime::fcitx5::ModernIMEInputMethod engine(&instance.addonManager());
    TestInputContext ic(instance.inputContextManager());
    ic.setCapabilityFlags(fcitx::CapabilityFlag::Preedit);
    auto entries = engine.listInputMethods();
    if (entries.empty()) {
        std::cerr << "[WORKER ERROR] listInputMethods() returned empty list\n";
        return 1;
    }

    fcitx::InputContextEvent activation(&ic, fcitx::EventType::InputContextFocusIn);
    engine.activate(entries.front(), activation);
    auto *state = dynamic_cast<modernime::fcitx5::FcitxInputContextState *>(
        ic.property("modernime-fcitx5-state"));
    if (!state) {
        std::cerr << "[WORKER ERROR] modernime-fcitx5-state property not found\n";
        return 2;
    }

    std::cout << "[WORKER] before_active=" << state->controller().active()
              << " before_gen=" << state->settingsGeneration() << "\n";

    if (!isStartup && mode != "negative_permanent_failure") {
        auto fileA = paths.settingsFile.string() + ".a";
        writeConfigFile(fileA, false, 14);
        std::filesystem::rename(fileA, paths.settingsFile);
        setenv("MODERNIME_FAULT_CONFIG_PATH", paths.settingsFile.c_str(), 1);
        setenv("MODERNIME_FAULT_REPLACEMENT", fileB.c_str(), 1);
        setenv("MODERNIME_FAULT_ACTION", mode.c_str(), 1);
    }

    auto countFn = reinterpret_cast<int (*)()>(dlsym(RTLD_DEFAULT, "modernime_fault_open_count"));
    if (!countFn) {
        countFn = reinterpret_cast<int (*)()>(dlsym(RTLD_DEFAULT, "review_open_count"));
    }

    bool unblocked = false;
    uint64_t maxDurationUs = (mode == "startup_long_failure") ? 26000000 :
                             (mode == "negative_permanent_failure") ? 11500000 : 8500000;

    auto poller = instance.eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 100000, 0,
        [&](fcitx::EventSourceTime *source, uint64_t) {
            if (mode == "startup_long_failure" && countFn && countFn() >= 6 && !unblocked) {
                setenv("MODERNIME_FAULT_UNBLOCKED", "1", 1);
                unblocked = true;
                std::cout << "[WORKER] fault unblocked without file change at count=" << countFn() << "\n";
            }
            if (mode == "startup_long_failure" && unblocked && state->settingsGeneration() >= 1 &&
                countFn && countFn() >= 7) {
                instance.eventLoop().exit();
                return false;
            }
            if (mode == "swap_after_read" && state->settingsGeneration() >= 1) {
                instance.eventLoop().exit();
                return false;
            }
            source->setNextInterval(100000);
            source->setEnabled(true);
            return true;
        });

    auto endTimer = instance.eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + maxDurationUs, 0,
        [&](fcitx::EventSourceTime *, uint64_t) {
            instance.eventLoop().exit();
            return false;
        });

    instance.eventLoop().exec();

    const int observedOpens = countFn ? countFn() : -1;
    std::cout << "[WORKER] observed_load_opens=" << observedOpens << "\n";

    fcitx::KeyEvent key(&ic, fcitx::Key(FcitxKey_n));
    engine.keyEvent(entries.front(), key);

    const bool afterActive = state->controller().active();
    const uint64_t afterGen = state->settingsGeneration();
    const bool keyAccepted = key.accepted();
    const std::string preedit = ic.inputPanel().clientPreedit().toString();

    std::cout << "[WORKER] after_active=" << afterActive << " generation=" << afterGen
              << " accepted=" << keyAccepted << " preedit=" << preedit << "\n";

    // Read runtime.state
    std::string runtimeHash;
    uint64_t runtimeGen = 0;
    std::ifstream diag(paths.runtimeState);
    std::string line;
    while (std::getline(diag, line)) {
        if (line.rfind("applied_content_hash=", 0) == 0) {
            runtimeHash = line.substr(21);
        } else if (line.rfind("settings_generation=", 0) == 0) {
            runtimeGen = std::stoull(line.substr(20));
        }
    }

    // Unset fault action so post-check inspection can read actual disk content
    unsetenv("MODERNIME_FAULT_ACTION");
    unsetenv("REVIEW_CONFIG_ACTION");

    auto disk = modernime::core::SettingsStore::load(paths.settingsFile);
    const auto afterStamp = modernime::core::SettingsFileStamp::fromPath(paths.settingsFile);

    std::cout << "[WORKER] runtime_hash=" << runtimeHash << " runtime_gen=" << runtimeGen << "\n";
    std::cout << "[WORKER] disk_enabled=" << disk.settings.inputEnabled
              << " disk_hash=" << disk.contentHash << "\n";

    // Strict behavioral assertions per scenario:
    if (mode == "startup_long_failure") {
        if (observedOpens < 7) {
            std::cerr << "[FAIL] startup_long_failure expected >= 7 opens, got " << observedOpens << "\n";
            return 10;
        }
        if (!unblocked) {
            std::cerr << "[FAIL] startup_long_failure fault was never unblocked\n";
            return 11;
        }
        if (afterStamp != initialStamp) {
            std::cerr << "[FAIL] startup_long_failure stamp changed unexpectedly\n";
            return 12;
        }
        if (afterActive != false) {
            std::cerr << "[FAIL] startup_long_failure afterActive must be false, got " << afterActive << "\n";
            return 13;
        }
        if (afterGen != 1 || runtimeGen != 1) {
            std::cerr << "[FAIL] startup_long_failure generation must be 1, got afterGen=" << afterGen
                      << " runtimeGen=" << runtimeGen << "\n";
            return 14;
        }
        if (runtimeHash != disk.contentHash) {
            std::cerr << "[FAIL] startup_long_failure runtimeHash mismatch: " << runtimeHash
                      << " vs disk " << disk.contentHash << "\n";
            return 15;
        }
        if (keyAccepted || !preedit.empty()) {
            std::cerr << "[FAIL] startup_long_failure key must not be accepted into preedit when disabled\n";
            return 16;
        }
    } else if (mode == "swap_after_read") {
        if (observedOpens < 2) {
            std::cerr << "[FAIL] swap_after_read expected >= 2 opens, got " << observedOpens << "\n";
            return 20;
        }
        if (afterActive != true || afterGen != 1 || runtimeGen != 1) {
            std::cerr << "[FAIL] swap_after_read active/gen mismatch: active=" << afterActive
                      << " gen=" << afterGen << "\n";
            return 21;
        }
        if (runtimeHash != disk.contentHash) {
            std::cerr << "[FAIL] swap_after_read runtimeHash mismatch\n";
            return 22;
        }
        if (!keyAccepted || preedit != "n") {
            std::cerr << "[FAIL] swap_after_read key must be accepted into preedit\n";
            return 23;
        }
    } else if (mode == "read_failure") {
        if (observedOpens < 2) {
            std::cerr << "[FAIL] read_failure expected >= 2 opens, got " << observedOpens << "\n";
            return 30;
        }
        if (afterActive != false || afterGen != 0 || runtimeGen != 0) {
            std::cerr << "[FAIL] read_failure must preserve last valid configuration\n";
            return 31;
        }
        if (runtimeHash != disk.contentHash) {
            std::cerr << "[FAIL] read_failure runtimeHash mismatch\n";
            return 32;
        }
    } else if (mode == "startup_swap") {
        if (observedOpens < 2) {
            std::cerr << "[FAIL] startup_swap expected >= 2 opens, got " << observedOpens << "\n";
            return 40;
        }
        if (afterActive != true || afterGen != 0 || runtimeGen != 0) {
            std::cerr << "[FAIL] startup_swap active/gen mismatch\n";
            return 41;
        }
        if (runtimeHash != disk.contentHash) {
            std::cerr << "[FAIL] startup_swap runtimeHash mismatch\n";
            return 42;
        }
    } else if (mode == "startup_read_failure") {
        if (observedOpens < 2) {
            std::cerr << "[FAIL] startup_read_failure expected >= 2 opens, got " << observedOpens << "\n";
            return 50;
        }
        if (afterActive != false || afterGen != 1 || runtimeGen != 1) {
            std::cerr << "[FAIL] startup_read_failure active/gen mismatch\n";
            return 51;
        }
        if (runtimeHash != disk.contentHash) {
            std::cerr << "[FAIL] startup_read_failure runtimeHash mismatch\n";
            return 52;
        }
    } else if (mode == "negative_permanent_failure") {
        // Fault is NEVER unblocked.
        if (observedOpens < 5) {
            std::cerr << "[FAIL] negative_permanent_failure expected >= 5 opens, got " << observedOpens << "\n";
            return 60;
        }
        if (afterGen != 0 || runtimeGen != 0) {
            std::cerr << "[FAIL] negative_permanent_failure generation must NOT advance\n";
            return 61;
        }
        if (runtimeHash == disk.contentHash) {
            std::cerr << "[FAIL] negative_permanent_failure must NOT adopt new disk hash\n";
            return 62;
        }
    } else {
        std::cerr << "[FAIL] Unknown scenario mode: " << mode << "\n";
        return 99;
    }

    std::cout << "[WORKER PASS] scenario " << mode << " assertions satisfied\n";
    return 0;
}

std::filesystem::path findHooksLibrary(const std::filesystem::path &selfDir) {
    const char *envLib = getenv("MODERNIME_FAULT_HOOKS_LIB");
    if (envLib != nullptr) {
        if (std::filesystem::is_regular_file(envLib)) {
            return std::filesystem::canonical(envLib);
        }
        std::cerr << "[ERROR] MODERNIME_FAULT_HOOKS_LIB is set to '" << envLib
                  << "', but that file does not exist or is not a regular file.\n";
        return {};
    }
    const std::vector<std::filesystem::path> candidates = {
        selfDir / "config_fault_hooks.so",
        selfDir / "libconfig_fault_hooks.so",
        selfDir / "tests" / "config_fault_hooks.so",
        selfDir / "tests" / "libconfig_fault_hooks.so",
        selfDir / ".." / "tests" / "config_fault_hooks.so",
        selfDir / ".." / "tests" / "libconfig_fault_hooks.so",
    };
    for (const auto &cand : candidates) {
        if (std::filesystem::is_regular_file(cand)) {
            return std::filesystem::canonical(cand);
        }
    }
    return {};
}

} // namespace

int main(int argc, char *argv[]) {
    std::cout.setf(std::ios::unitbuf);

    // If invoked in worker mode:
    if (argc >= 4 && std::string(argv[1]) == "--scenario") {
        return runWorker(argv[2], argv[3]);
    }

    // Orchestrator mode:
    std::cout << "==> ModernIME Adapter Fault Recovery Test Suite (W3 Regression)\n";

    const std::filesystem::path selfPath = std::filesystem::canonical("/proc/self/exe");
    const std::filesystem::path selfDir = selfPath.parent_path();
    const std::filesystem::path hooksLib = findHooksLibrary(selfDir);

    if (hooksLib.empty()) {
        std::cerr << "[ERROR] Cannot locate config_fault_hooks.so. Set MODERNIME_FAULT_HOOKS_LIB.\n";
        return 1;
    }
    std::cout << "Using hooks library: " << hooksLib.string() << "\n";

    const std::vector<std::string> scenarios = {
        "swap_after_read",
        "read_failure",
        "startup_swap",
        "startup_read_failure",
        "startup_long_failure",
        "negative_permanent_failure",
    };

    for (const auto &scenario : scenarios) {
        char tempPattern[] = "/tmp/modernime-recovery-XXXXXX";
        char *tempDir = mkdtemp(tempPattern);
        if (!tempDir) {
            std::cerr << "[ERROR] Failed to create temporary directory for " << scenario << "\n";
            return 2;
        }

        std::cout << "[RUN ] Scenario: " << scenario << "\n";

        pid_t pid = fork();
        if (pid < 0) {
            std::cerr << "[ERROR] fork() failed for " << scenario << "\n";
            std::filesystem::remove_all(tempDir);
            return 3;
        }

        if (pid == 0) {
            // Child process: set LD_PRELOAD and exec worker
            setenv("LD_PRELOAD", hooksLib.c_str(), 1);
            setenv("HOME", tempDir, 1);
            setenv("TMPDIR", tempDir, 1);

            execl(selfPath.c_str(), selfPath.c_str(), "--scenario", scenario.c_str(), tempDir, nullptr);
            _exit(127);
        }

        int status = 0;
        waitpid(pid, &status, 0);

        std::filesystem::remove_all(tempDir);

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            std::cerr << "[FAIL] Scenario " << scenario << " failed with exit code " << code << "\n";
            return 4;
        }

        std::cout << "[PASS] Scenario: " << scenario << "\n";
    }

    // Negative mutant check: confirm that a deliberate assertion mismatch causes worker to exit non-zero
    {
        std::cout << "[RUN ] Negative mutant check: unhandled failure causes non-zero exit\n";
        char tempPattern[] = "/tmp/modernime-recovery-mutant-XXXXXX";
        char *tempDir = mkdtemp(tempPattern);
        if (tempDir) {
            pid_t pid = fork();
            if (pid == 0) {
                setenv("LD_PRELOAD", hooksLib.c_str(), 1);
                setenv("HOME", tempDir, 1);
                // Deliberately ask for an unknown scenario which must fail
                execl(selfPath.c_str(), selfPath.c_str(), "--scenario", "invalid_mutant_scenario", tempDir, nullptr);
                _exit(127);
            }
            int status = 0;
            waitpid(pid, &status, 0);
            std::filesystem::remove_all(tempDir);

            if (!WIFEXITED(status) || WEXITSTATUS(status) == 0) {
                std::cerr << "[FAIL] Negative mutant check unexpectedly succeeded!\n";
                return 5;
            }
            std::cout << "[PASS] Negative mutant check correctly exited non-zero\n";
        }
    }

    std::cout << "========================================================\n";
    std::cout << "All adapter fault recovery scenarios PASSED (6/6)\n";
    std::cout << "========================================================\n";
    return 0;
}
