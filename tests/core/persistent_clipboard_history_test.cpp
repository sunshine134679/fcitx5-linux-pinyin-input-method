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

void bumpMtime(const std::filesystem::path &path) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(path, error);
    assertTrue(!error, "external fixture timestamp is readable");
    std::filesystem::last_write_time(path, time + std::chrono::seconds(2), error);
    assertTrue(!error, "external fixture timestamp changes");
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
    // Polling the same system clipboard after an external clear or removal
    // must not reinterpret that deletion as another copy event.
    const auto pollingPath = directory / "polling.bin";
    modernime::core::PersistentClipboardHistory polling(pollingPath);
    assertTrue(polling.load(&error) && polling.observe("older", &error) &&
                   polling.observe("current", &error), "polling fixture starts");
    modernime::core::ClipboardHistory editor;
    assertTrue(editor.load(pollingPath, &error) && editor.remove(0) &&
                   editor.save(pollingPath, &error), "editor removes current entry");
    bumpMtime(pollingPath);
    assertTrue(polling.reloadIfExternallyChanged(&error), "removal is reloaded");
    assertTrue(!polling.observe("current", &error) &&
                   polling.entries() == std::vector<std::string>{"older"},
               "unchanged clipboard does not restore a removed entry");
    editor.clear();
    assertTrue(editor.save(pollingPath, &error), "editor clears history");
    bumpMtime(pollingPath);
    assertTrue(polling.reloadIfExternallyChanged(&error), "clear is reloaded");
    assertTrue(!polling.observe("current", &error) && polling.entries().empty(),
               "unchanged clipboard does not restore cleared history");
    assertTrue(polling.observe("changed", &error) &&
                   polling.observe("current", &error),
               "a changed clipboard followed by a re-copy is collected");
    editor.clear();
    assertTrue(editor.save(pollingPath, &error), "editor clears recopied history");
    bumpMtime(pollingPath);
    assertTrue(polling.reloadIfExternallyChanged(&error), "second clear reloads");
    assertTrue(!polling.observe("", &error) && polling.entries().empty(),
               "empty clipboard is observed without adding history");
    assertTrue(polling.observe("current", &error) &&
                   polling.entries() == std::vector<std::string>{"current"},
               "copying the previous value after an empty clipboard is new");

    // A failed save remains retryable even when the next poll has the same
    // clipboard value. A real directory at the temporary file blocks saving.
    const auto retryPath = directory / "retry.bin";
    modernime::core::PersistentClipboardHistory retry(retryPath);
    assertTrue(retry.load(&error), "retry fixture loads");
    auto blockedTemporary = retryPath;
    blockedTemporary += ".tmp";
    std::filesystem::create_directory(blockedTemporary, filesystemError);
    assertTrue(!filesystemError, "temporary history write is blocked");
    assertTrue(retry.observe("retry value", &error) && !error.empty(),
               "failed persistence retains a new observation");
    assertTrue(!retry.observe("retry value", &error) && !error.empty(),
               "same clipboard retries persistence without a new observation");
    std::filesystem::remove(blockedTemporary, filesystemError);
    assertTrue(!filesystemError, "temporary history write is unblocked");
    assertTrue(!retry.observe("retry value", &error) && error.empty(),
               "same clipboard successfully retries a pending save");
    assertTrue(editor.load(retryPath, &error) &&
                   editor.entries() == std::vector<std::string>{"retry value"},
               "retry preserves the original observation on disk");

    return EXIT_SUCCESS;
}
