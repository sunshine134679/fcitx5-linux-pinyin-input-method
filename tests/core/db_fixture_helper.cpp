#include "modernime/core/learning_store.h"
#include "modernime/core/settings.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

std::filesystem::path resolveDatabasePathFromEnv(std::string_view backend,
                                                std::filesystem::path &outDataDir) {
    const char *xdgCfg = std::getenv("XDG_CONFIG_HOME");
    std::string_view cfg = (xdgCfg != nullptr) ? xdgCfg : "";

    const char *xdgData = std::getenv("XDG_DATA_HOME");
    std::string_view data = (xdgData != nullptr) ? xdgData : "";

    const char *home = std::getenv("HOME");
    std::string_view homeView = (home != nullptr) ? home : "";

    const auto paths = modernime::core::SettingsPaths::fromEnvironment(cfg, data, homeView);
    outDataDir = paths.learningStore.parent_path();

    if (backend == "lmdb") {
        return outDataDir / "learning.lmdb";
    }
    return paths.learningStore;
}

} // namespace

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: modernime_db_fixture_helper [--from-env [--backend sqlite|lmdb]] "
                  << "[<db_path>] [--record pinyin word] [--inspect word]\n";
        return 1;
    }

    std::filesystem::path dbPath;
    std::filesystem::path resolvedDataDir;
    std::string backend = "sqlite";
    std::string pinyin = "nihao";
    std::string word = "你好";
    std::string inspectWord;
    bool fromEnv = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--from-env") {
            fromEnv = true;
        } else if (arg == "--backend" && i + 1 < argc) {
            backend = argv[++i];
        } else if (arg == "--record" && i + 2 < argc) {
            pinyin = argv[++i];
            word = argv[++i];
        } else if (arg == "--inspect" && i + 1 < argc) {
            inspectWord = argv[++i];
        } else if (dbPath.empty() && !arg.starts_with("--")) {
            dbPath = arg;
        }
    }

    if (fromEnv) {
        dbPath = resolveDatabasePathFromEnv(backend, resolvedDataDir);
        std::cout << "RESOLVED_DATA_DIR: " << resolvedDataDir.string() << "\n";
        std::cout << "RESOLVED_DB_PATH: " << dbPath.string() << "\n";
    }

    if (dbPath.empty()) {
        std::cerr << "Error: No database path specified or resolved from environment\n";
        return 1;
    }

    // Inspect mode
    if (!inspectWord.empty()) {
        modernime::core::LearningStore store(dbPath);
        if (!store.openReadOnly() && !store.open()) {
            std::cerr << "Failed to open LearningStore for inspection at " << dbPath << "\n";
            return 2;
        }
        auto state = store.readState();
        if (!state.has_value() || state->snapshot == nullptr) {
            std::cerr << "Failed to readState for inspection from " << dbPath << "\n";
            return 3;
        }
        bool found = false;
        for (const auto &entry : state->snapshot->entries()) {
            if (entry.phrase == inspectWord) {
                found = true;
                break;
            }
        }
        store.close();
        if (found) {
            std::cout << "INSPECT_FOUND: " << inspectWord << " in " << dbPath << "\n";
            return 0;
        }
        std::cout << "INSPECT_NOT_FOUND: " << inspectWord << " in " << dbPath << "\n";
        return 4;
    }

    // Record mode
    std::error_code ec;
    if (!dbPath.parent_path().empty()) {
        std::filesystem::create_directories(dbPath.parent_path(), ec);
    }

    {
        modernime::core::LearningStore store(dbPath);
        if (!store.open()) {
            std::cerr << "Failed to open LearningStore at " << dbPath << "\n";
            return 2;
        }

        modernime::core::CommitEvent event;
        event.text = word;
        event.fullPinyin = pinyin;
        event.rawKeystrokes = pinyin;
        event.role = modernime::core::CommitCandidateRole::Chinese;
        if (!store.recordCommit(event)) {
            std::cerr << "Failed to recordCommit at " << dbPath << "\n";
            return 3;
        }
        store.close();
    }

    // Reopen to verify persistence
    {
        modernime::core::LearningStore reopened(dbPath);
        if (!reopened.open()) {
            std::cerr << "Failed to reopen LearningStore at " << dbPath << "\n";
            return 4;
        }
        auto state = reopened.readState();
        if (!state.has_value() || state->snapshot == nullptr) {
            std::cerr << "Failed to readState from " << dbPath << "\n";
            return 5;
        }
        bool found = false;
        for (const auto &entry : state->snapshot->entries()) {
            if (entry.phrase == word) {
                found = true;
                break;
            }
        }
        if (!found) {
            std::cerr << "Recorded word not found in reopened snapshot at " << dbPath << "\n";
            return 6;
        }
        std::cout << "RECORDED: " << word << " (" << pinyin << ") in " << dbPath
                  << " (snapshot revision " << state->resetRevision << ")\n";
        reopened.close();
    }

    return 0;
}
