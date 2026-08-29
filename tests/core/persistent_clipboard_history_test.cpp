#include "modernime/core/clipboard_history.h"
#include "modernime/core/persistent_clipboard_history.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "persistent clipboard history test failed: " << message
                  << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
                           "modernime-persistent-clipboard-history-test";
    std::error_code filesystemError;
    std::filesystem::remove_all(directory, filesystemError);
    std::filesystem::create_directories(directory, filesystemError);
    assertTrue(!filesystemError, "persistent history test directory is created");

    const auto path = directory / "clipboard-history.bin";
    modernime::core::ClipboardHistory legacy;
    legacy.observe("old second");
    legacy.observe("old first");
    std::string error;
    assertTrue(legacy.save(path, &error),
               "existing clipboard history saves: " + error);

    modernime::core::PersistentClipboardHistory history(path);
    assertTrue(history.load(&error),
               "persistent clipboard history loads: " + error);
    assertTrue(history.entries().size() == 2 &&
                   history.entries()[0] == "old first" &&
                   history.entries()[1] == "old second",
               "existing history is loaded before new observations");

    assertTrue(history.observe("new value", &error),
               "new clipboard value is observed and saved: " + error);
    modernime::core::ClipboardHistory afterObservation;
    assertTrue(afterObservation.load(path, &error),
               "history can be loaded after observation: " + error);
    assertTrue(afterObservation.entries().size() == 3 &&
                   afterObservation.entries()[0] == "new value" &&
                   afterObservation.entries()[1] == "old first" &&
                   afterObservation.entries()[2] == "old second",
               "new value is persisted without overwriting existing history");

    assertTrue(history.flush(&error),
               "history flushes during runtime reload: " + error);

    // An external writer (the settings client) replaces the file behind our
    // back; the persistent wrapper must adopt the on-disk state instead of
    // overwriting it with stale in-memory entries.
    modernime::core::ClipboardHistory external;
    external.observe("external only");
    assertTrue(external.save(path, &error),
               "external writer saves a fresh history: " + error);
    {
        std::error_code mtimeError;
        const auto bumped = std::filesystem::last_write_time(
                                path, mtimeError) +
                            std::chrono::seconds(2);
        std::filesystem::last_write_time(path, bumped, mtimeError);
        assertTrue(!mtimeError, "external modification timestamp is applied");
    }
    assertTrue(history.reloadIfExternallyChanged(&error),
               "an external change triggers a reload: " + error);
    assertTrue(history.entries().size() == 1 &&
                   history.entries().front() == "external only",
               "reloaded history reflects the external state");
    assertTrue(!history.reloadIfExternallyChanged(&error),
               "an unchanged file does not trigger another reload");
    return EXIT_SUCCESS;
}
