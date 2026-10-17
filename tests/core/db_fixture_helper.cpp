#include "modernime/core/learning_store.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: modernime_db_fixture_helper <path_to_db> [--record pinyin word]\n";
        return 1;
    }
    const std::filesystem::path dbPath = argv[1];

    std::error_code ec;
    if (!dbPath.parent_path().empty()) {
        std::filesystem::create_directories(dbPath.parent_path(), ec);
    }

    std::string pinyin = "nihao";
    std::string word = "你好";
    if (argc >= 4 && std::string(argv[2]) == "--record") {
        pinyin = argv[3];
        word = (argc >= 5) ? argv[4] : pinyin;
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
        std::cout << "Successfully verified " << dbPath
                  << " with active snapshot revision " << state->resetRevision << "\n";
        reopened.close();
    }

    return 0;
}
