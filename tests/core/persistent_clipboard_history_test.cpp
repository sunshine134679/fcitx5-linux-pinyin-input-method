#include "modernime/core/clipboard_history.h"
#include "modernime/core/persistent_clipboard_history.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

    // A corrupt history file must be quarantined and rebuilt instead of
    // permanently disabling the clipboard feature.
    const auto corruptPath = directory / "corrupt-history.bin";
    {
        std::ofstream output(corruptPath, std::ios::binary);
        output << "not a clipboard history file at all";
        output.close();
        assertTrue(output.good(), "corrupt fixture file is written");
    }
    modernime::core::PersistentClipboardHistory corrupt(corruptPath);
    assertTrue(corrupt.load(&error),
               "a corrupt history file is recovered on load: " + error);
    assertTrue(corrupt.entries().empty(),
               "recovered history starts empty");
    int corruptBackups = 0;
    for (const auto &entry :
         std::filesystem::directory_iterator(directory, filesystemError)) {
        if (entry.path().filename().string().rfind("corrupt-history.bin.corrupt-",
                                                   0) == 0) {
            ++corruptBackups;
        }
    }
    assertTrue(!filesystemError && corruptBackups >= 1,
               "the corrupt file is preserved under a backup name");
    assertTrue(corrupt.observe("fresh after corruption", &error),
               "recovered history accepts new observations: " + error);

    // 外部把文件写坏后，重新加载同样自愈而不是永久失效。
    const auto externallyCorruptPath = directory / "externally-corrupt.bin";
    modernime::core::PersistentClipboardHistory externallyCorrupt(
        externallyCorruptPath);
    assertTrue(externallyCorrupt.load(&error),
               "external corruption fixture loads: " + error);
    assertTrue(externallyCorrupt.observe("before corruption", &error),
               "external corruption fixture starts with one entry: " + error);
    {
        std::ofstream output(externallyCorruptPath, std::ios::trunc);
        output << "garbage";
        output.close();
    }
    {
        std::error_code mtimeError;
        const auto bumped = std::filesystem::last_write_time(
                                externallyCorruptPath, mtimeError) +
                            std::chrono::seconds(2);
        std::filesystem::last_write_time(externallyCorruptPath, bumped,
                                         mtimeError);
        assertTrue(!mtimeError, "external corruption timestamp is applied");
    }
    assertTrue(externallyCorrupt.reloadIfExternallyChanged(&error),
               "a corrupt external rewrite triggers recovery");
    assertTrue(externallyCorrupt.entries().empty(),
               "recovery discards the corrupt rewrite");
    return EXIT_SUCCESS;
}
