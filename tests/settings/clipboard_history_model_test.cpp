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

    // The displayed index identifies the original text, even when another
    // writer inserts or reorders entries before the settings action runs.
    history.clear();
    history.observe("first");
    history.observe("second");
    assertTrue(history.save(path, &error) && model.reload(&error),
               "external insert fixture loads");
    history.observe("third");
    assertTrue(history.save(path, &error), "external insert saves");
    assertTrue(model.remove(1, &error), "remove follows the displayed text");
    assertTrue(persisted.load(path, &error) &&
                   persisted.entries() == std::vector<std::string>{"third", "second"},
               "external insert does not cause deletion of a different entry");

    history.clear();
    history.observe("first");
    history.observe("second");
    assertTrue(history.save(path, &error) && model.reload(&error),
               "external reorder fixture loads");
    history.observe("first");
    assertTrue(history.save(path, &error), "external reorder saves");
    assertTrue(model.remove(1, &error), "remove follows reordered target");
    assertTrue(persisted.load(path, &error) &&
                   persisted.entries() == std::vector<std::string>{"second"},
               "external reorder does not cause deletion of a different entry");

    history.clear();
    history.observe("first");
    history.observe("second");
    assertTrue(history.save(path, &error) && model.reload(&error),
               "vanished target fixture loads");
    assertTrue(history.remove(0) && history.save(path, &error),
               "another writer removes the displayed target");
    assertTrue(!model.remove(0, &error) && !error.empty(),
               "vanished target removal is refused");
    assertTrue(persisted.load(path, &error) &&
                   persisted.entries() == std::vector<std::string>{"first"},
               "a vanished target cannot delete the replacement entry");
    assertTrue(!model.remove(99, &error), "invalid displayed index is refused");

    const auto missingPath = directory / "missing.bin";
    modernime::settings::ClipboardHistoryModel missing(missingPath);
    assertTrue(missing.reload(&error) && missing.entries().empty(),
               "missing history is shown as an empty list");
    return EXIT_SUCCESS;
}
