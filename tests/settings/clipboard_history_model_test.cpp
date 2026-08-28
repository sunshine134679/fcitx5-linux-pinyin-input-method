#include "modernime/core/clipboard_history.h"
#include "modernime/settings/clipboard_history_model.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "clipboard history model test failed: " << message
                  << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
                           "modernime-clipboard-history-model-test";
    std::error_code filesystemError;
    std::filesystem::remove_all(directory, filesystemError);
    std::filesystem::create_directories(directory, filesystemError);
    assertTrue(!filesystemError, "model test directory is created");

    const auto path = directory / "clipboard-history.bin";
    modernime::core::ClipboardHistory history;
    history.observe("first");
    history.observe("second");
    std::string error;
    assertTrue(history.save(path, &error), "fixture history saves: " + error);

    modernime::settings::ClipboardHistoryModel model(path);
    assertTrue(model.reload(&error), "model reloads history: " + error);
    assertTrue(model.entries().size() == 2 &&
                   model.entries()[0] == "second" &&
                   model.entries()[1] == "first",
               "model exposes newest history first");

    assertTrue(model.remove(0, &error),
               "model removes a selected history entry");
    assertTrue(model.entries().size() == 1 && model.entries()[0] == "first",
               "model updates after removing an entry");
    assertTrue(model.clear(&error), "model clears all history entries");
    assertTrue(model.entries().empty(), "model exposes an empty history");
    modernime::core::ClipboardHistory persisted;
    assertTrue(persisted.load(path, &error) && persisted.entries().empty(),
               "history removal and clear are persisted atomically");

    const auto missingPath = directory / "missing.bin";
    modernime::settings::ClipboardHistoryModel missing(missingPath);
    assertTrue(missing.reload(&error) && missing.entries().empty(),
               "missing history is shown as an empty list");
    return EXIT_SUCCESS;
}
