#include "modernime/core/clipboard_history.h"
#include "modernime/core/persistent_clipboard_history.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

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
    char temporary[] = "/tmp/modernime-persistent-clipboard-history-XXXXXX";
    const auto *created = ::mkdtemp(temporary);
    assertTrue(created != nullptr, "isolated history directory is created");
    const std::filesystem::path directory(created);
    std::error_code filesystemError;

    const auto batchPath = directory / "batched.bin";
    modernime::core::ClipboardHistory batchSeed;
    batchSeed.observe("B"); batchSeed.observe("A");
    assertTrue(batchSeed.save(batchPath), "batch fixture saves");
    modernime::core::PersistentClipboardHistory batched(batchPath);
    assertTrue(batched.load(), "batch fixture loads");
    batched.setObservationBaseline("A");
    auto batchBlock = batchPath;
    batchBlock += ".tmp";
    std::filesystem::create_directory(batchBlock);
    modernime::core::ClipboardObservationBatch batch;
    batch.observe("B"); batch.observe("A"); batch.observe("B");
    std::string batchError;
    assertTrue(batched.observeBatch(batch, &batchError) && !batchError.empty(),
               "whole batch applies before one failed persistence attempt");
    std::filesystem::remove(batchBlock);
    batchSeed.clear();
    assertTrue(batchSeed.save(batchPath) && batched.flush(), "batch retries after external clear");
    assertTrue(batchSeed.load(batchPath) && batchSeed.entries() == std::vector<std::string>{"B", "A"},
               "compacted batch keeps both legitimate pending copies");
    batchSeed.clear();
    assertTrue(batchSeed.save(batchPath) && batched.reloadIfExternallyChanged(), "second external clear reloads");
    modernime::core::ClipboardObservationBatch recopy;
    recopy.observe("A");
    assertTrue(batched.observeBatch(recopy) && batchSeed.load(batchPath) &&
                   batchSeed.entries() == std::vector<std::string>{"A"},
               "prequalified recopy is not filtered by old synchronous baseline");

    const auto deletedPath = directory / "externally-deleted.bin";
    modernime::core::ClipboardHistory deletedSeed;
    deletedSeed.observe("removed externally");
    assertTrue(deletedSeed.save(deletedPath), "deletion fixture saves");
    modernime::core::PersistentClipboardHistory deleted(deletedPath);
    assertTrue(deleted.load(), "deletion fixture loads");
    assertTrue(std::filesystem::remove(deletedPath), "external unlink succeeds");
    assertTrue(deleted.reloadIfExternallyChanged() && deleted.entries().empty(),
               "external file deletion clears a clean in-memory history");
    assertTrue(deleted.flush() && !std::filesystem::exists(deletedPath),
               "clean flush does not resurrect an externally deleted file");

    // A resume baseline is an observation, not a new copy. It must never
    // enter visible history or become a pending write, even after flush.
    const auto baselinePath = directory / "baseline.bin";
    modernime::core::PersistentClipboardHistory baseline(baselinePath);
    std::string baselineError;
    assertTrue(baseline.load(&baselineError), "baseline history loads");
    baseline.setObservationBaseline("synthetic-excluded");
    assertTrue(baseline.entries().empty() && baseline.flush(&baselineError) &&
                   !std::filesystem::exists(baselinePath),
               "baseline does not create a history file");
    assertTrue(!baseline.observe("synthetic-excluded", &baselineError) &&
                   baseline.entries().empty(),
               "unchanged baseline is never recorded");
    assertTrue(baseline.observe("allowed copy", &baselineError),
               "changed allowed content is recorded");
    auto baselineBlocked = baselinePath;
    baselineBlocked += ".tmp";
    std::filesystem::create_directory(baselineBlocked);
    assertTrue(baseline.observe("allowed pending", &baselineError) &&
                   !baselineError.empty(), "allowed copy has a failed save");
    baseline.setObservationBaseline("synthetic-resume");
    std::filesystem::remove(baselineBlocked);
    modernime::core::ClipboardHistory clearedBaseline;
    assertTrue(clearedBaseline.save(baselinePath, &baselineError),
               "external clear replaces history during failed save");
    assertTrue(!baseline.observe("synthetic-resume", &baselineError) &&
                   baselineError.empty(), "same baseline retries only allowed pending copy");
    assertTrue(clearedBaseline.load(baselinePath, &baselineError) &&
                   clearedBaseline.entries() == std::vector<std::string>{"allowed pending"},
               "baseline preserves pending copy but cannot resurrect cleared base");
    assertTrue(!baseline.observe("", &baselineError) &&
                   baseline.observe("synthetic-resume", &baselineError),
               "explicit content change permits a later recopy of baseline text");

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

    // Atomic saves may share a filesystem timestamp, including rewrites of
    // the same size. Such edits must still replace the runtime's stale state.
    const auto collisionPath = directory / "timestamp-collision.bin";
    modernime::core::PersistentClipboardHistory collision(collisionPath);
    assertTrue(collision.load(&error) && collision.observe("original", &error),
               "timestamp collision fixture starts");
    const auto collisionMtime = std::filesystem::last_write_time(collisionPath);
    const auto collisionSize = std::filesystem::file_size(collisionPath);
    modernime::core::ClipboardHistory collisionEditor;
    collisionEditor.observe("replaced");
    assertTrue(collisionEditor.save(collisionPath, &error),
               "editor replaces history with the same serialized size");
    std::filesystem::last_write_time(collisionPath, collisionMtime);
    assertTrue(std::filesystem::file_size(collisionPath) == collisionSize &&
                   std::filesystem::last_write_time(collisionPath) == collisionMtime,
               "external replacement preserves timestamp and size");
    assertTrue(collision.reloadIfExternallyChanged(&error) &&
                   collision.entries() == std::vector<std::string>{"replaced"},
               "same-timestamp replacement is reloaded");
    assertTrue(!collision.reloadIfExternallyChanged(&error),
               "unchanged replacement is not repeatedly reloaded");
    collisionEditor.clear();
    assertTrue(collisionEditor.save(collisionPath, &error),
               "editor clears history without advancing its timestamp");
    std::filesystem::last_write_time(collisionPath, collisionMtime);
    assertTrue(collision.reloadIfExternallyChanged(&error) &&
                   collision.entries().empty(),
               "same-timestamp clear is reloaded");
    assertTrue(!collision.observe("original", &error) && collision.entries().empty(),
               "same clipboard does not resurrect a same-timestamp clear");

    // Permission failures must keep the real file and the last visible history.
    const auto unreadablePath = directory / "unreadable.bin";
    modernime::core::PersistentClipboardHistory unreadable(unreadablePath);
    assertTrue(unreadable.load() && unreadable.observe("preserve me"), "unreadable fixture saves");
    assertTrue(::chmod(unreadablePath.c_str(), 0000) == 0, "remove read permission");
    assertTrue(!unreadable.reloadIfExternallyChanged(&error) && !error.empty() &&
                   std::filesystem::exists(unreadablePath) &&
                   unreadable.entries() == std::vector<std::string>{"preserve me"},
               "failed read retains original file and previous history");
    assertTrue(::chmod(unreadablePath.c_str(), 0600) == 0, "restore read permission");
    assertTrue(unreadable.reloadIfExternallyChanged(&error) && error.empty() &&
                   unreadable.entries() == std::vector<std::string>{"preserve me"},
               "history recovers when access returns");

    // Recovery must obey the same lock as an editor replacing failed data.
    const auto replacementPath = directory / "replacement.bin";
    { std::ofstream output(replacementPath); output << "broken"; }
    const auto lockPath = directory / ".replacement.bin.lock";
    const int lockFd = ::open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    assertTrue(lockFd >= 0 && ::flock(lockFd, LOCK_EX) == 0, "editor owns the history lock");
    modernime::core::PersistentClipboardHistory replacement(replacementPath);
    assertTrue(!replacement.load(&error) && std::filesystem::exists(replacementPath),
               "recovery leaves failed file alone while editor owns the lock");
    modernime::core::ClipboardHistory validReplacement;
    validReplacement.observe("valid external replacement");
    assertTrue(validReplacement.save(replacementPath), "editor replaces failed data while holding lock");
    ::close(lockFd);
    assertTrue(replacement.reloadIfExternallyChanged(&error) && error.empty() &&
                   replacement.entries() == validReplacement.entries(),
               "recovery reads valid external replacement without quarantining it");
    for (const auto &entry : std::filesystem::directory_iterator(directory)) {
        const auto name = entry.path().filename().string();
        assertTrue(name.rfind("replacement.bin.corrupt-", 0) != 0 &&
                       name.rfind("unreadable.bin.corrupt-", 0) != 0,
                   "valid and unreadable histories have no corrupt backups");
    }

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

    // Runtime saves must not rewrite a clean, stale history over an editor's
    // clear when no subsequent polling/reload has happened yet.
    const auto flushPath = directory / "flush.bin";
    modernime::core::PersistentClipboardHistory runtime(flushPath);
    assertTrue(runtime.load(&error) && runtime.observe("stale", &error),
               "runtime flush fixture starts");
    editor.clear();
    assertTrue(editor.save(flushPath, &error), "editor clears runtime history");
    bumpMtime(flushPath);
    const auto clearedMtime = std::filesystem::last_write_time(flushPath);
    assertTrue(runtime.flush(&error), "clean runtime flush succeeds");
    assertTrue(editor.load(flushPath, &error) && editor.entries().empty() &&
                   std::filesystem::last_write_time(flushPath) == clearedMtime,
               "clean runtime flush does not rewrite or resurrect external clear");

    // Failed writes retry only pending copy observations over the latest disk
    // state. Entries deleted externally are not part of that pending change.
    const auto dirtyPath = directory / "dirty-flush.bin";
    modernime::core::PersistentClipboardHistory dirty(dirtyPath);
    assertTrue(dirty.load(&error) && dirty.observe("old base", &error),
               "dirty flush fixture starts");
    auto dirtyTemporary = dirtyPath;
    dirtyTemporary += ".tmp";
    std::filesystem::create_directory(dirtyTemporary, filesystemError);
    assertTrue(!filesystemError && dirty.observe("pending copy", &error) &&
                   !error.empty(), "a new clipboard copy has a pending save");
    assertTrue(!dirty.flush(&error) && !error.empty(),
               "flush reports a failed dirty retry");
    std::filesystem::remove(dirtyTemporary, filesystemError);
    assertTrue(!filesystemError && dirty.flush(&error),
               "flush retries and saves dirty history once writable");
    assertTrue(editor.load(dirtyPath, &error) &&
                   editor.entries() == std::vector<std::string>{"pending copy", "old base"},
               "dirty flush without external changes preserves history");

    std::filesystem::create_directory(dirtyTemporary, filesystemError);
    assertTrue(!filesystemError && dirty.observe("new pending copy", &error) &&
                   !error.empty(), "a second copy has a pending save");
    std::filesystem::remove(dirtyTemporary, filesystemError);
    editor.clear();
    assertTrue(!filesystemError && editor.save(dirtyPath, &error),
               "editor clears history while a copy is pending");
    bumpMtime(dirtyPath);
    assertTrue(dirty.flush(&error), "dirty retry accepts an external clear");
    assertTrue(editor.load(dirtyPath, &error) &&
                   editor.entries() == std::vector<std::string>{"new pending copy"},
               "dirty retry persists only the pending copy after external clear");

    // The merge performed during a dirty retry needs the same collision
    // detection as polling, or it can silently resurrect a cleared base.
    const auto retryMtime = std::filesystem::last_write_time(dirtyPath);
    std::filesystem::create_directory(dirtyTemporary, filesystemError);
    assertTrue(!filesystemError && dirty.observe("collision pending", &error) &&
                   !error.empty(), "same-timestamp retry has a pending copy");
    std::filesystem::remove(dirtyTemporary, filesystemError);
    editor.clear();
    assertTrue(!filesystemError && editor.save(dirtyPath, &error),
               "editor clears history before same-timestamp retry");
    std::filesystem::last_write_time(dirtyPath, retryMtime);
    assertTrue(dirty.flush(&error), "same-timestamp dirty retry succeeds");
    assertTrue(editor.load(dirtyPath, &error) &&
                   editor.entries() == std::vector<std::string>{"collision pending"},
               "same-timestamp retry saves only the pending copy");

    // Dual-process concurrency and fence test:
    // 1) Verify that concurrent writes between child and parent do not corrupt the file.
    // 2) Verify that copy vs clear concurrency does not resurrect cleared history.
    const auto concurrentPath = directory / "concurrent-fence.bin";
    modernime::core::PersistentClipboardHistory parentHist(concurrentPath);
    assertTrue(parentHist.load(&error) && parentHist.observe("base item", &error),
               "concurrent fixture initializes");

    pid_t pid = fork();
    assertTrue(pid >= 0, "fork succeeds");
    if (pid == 0) {
        // Child process: performs writes and clear operations
        for (int i = 0; i < 20; ++i) {
            std::string childError;
            modernime::core::ClipboardHistory childWorker;
            modernime::core::ClipboardHistory::updateFile(
                concurrentPath,
                [i](modernime::core::ClipboardHistory &hist) {
                    hist.observe("child-entry-" + std::to_string(i));
                    return true;
                },
                &childWorker,
                &childError,
                false);
            usleep(500);
        }
        std::exit(EXIT_SUCCESS);
    } else {
        // Parent process: also performs writes concurrently
        for (int i = 0; i < 20; ++i) {
            std::string parentError;
            modernime::core::ClipboardHistory parentWorker;
            modernime::core::ClipboardHistory::updateFile(
                concurrentPath,
                [i](modernime::core::ClipboardHistory &hist) {
                    hist.observe("parent-entry-" + std::to_string(i));
                    return true;
                },
                &parentWorker,
                &parentError,
                false);
            usleep(500);
        }

        int status = 0;
        waitpid(pid, &status, 0);
        assertTrue(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                   "child worker exited cleanly");

        // Verify file is clean and uncorrupted
        modernime::core::ClipboardHistory checkHist;
        assertTrue(checkHist.load(concurrentPath, &error),
                   "file remains valid after concurrent process writes: " + error);
        assertTrue(!checkHist.entries().empty(),
                   "concurrent entries are recorded");
    }

    // Now test copy vs clear fence across processes:
    // Process A (child) clears the file; Process B (parent) observes a new item.
    // Old entries in parent's memory must not be resurrected.
    const auto fencePath = directory / "fence-test.bin";
    modernime::core::PersistentClipboardHistory runtimeFence(fencePath);
    assertTrue(runtimeFence.load(&error), "fence fixture loads");
    assertTrue(runtimeFence.observe("stale A", &error) &&
                   runtimeFence.observe("stale B", &error),
               "fence fixture populates stale base");
    assertTrue(runtimeFence.flush(&error), "fence fixture flushes stale base");

    pid = fork();
    assertTrue(pid >= 0, "second fork succeeds");
    if (pid == 0) {
        // Child clears the history via updateFile (simulating settings UI action)
        std::string childError;
        modernime::core::ClipboardHistory childWorker;
        bool cleared = modernime::core::ClipboardHistory::updateFile(
            fencePath,
            [](modernime::core::ClipboardHistory &hist) {
                hist.clear();
                return true;
            },
            &childWorker,
            &childError,
            false);
        if (!cleared) {
            std::exit(EXIT_FAILURE);
        }
        std::exit(EXIT_SUCCESS);
    } else {
        int status = 0;
        waitpid(pid, &status, 0);
        assertTrue(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                   "child clear exited cleanly");

        // Runtime observes a new item AFTER child cleared on disk
        assertTrue(runtimeFence.observe("fresh item", &error),
                   "runtime observes fresh item: " + error);
        assertTrue(runtimeFence.flush(&error),
                   "runtime flushes fresh item: " + error);

        modernime::core::ClipboardHistory finalCheck;
        assertTrue(finalCheck.load(fencePath, &error),
                   "fence file is readable: " + error);
        // The file must contain ONLY "fresh item", stale A and stale B must NOT be resurrected!
        assertTrue(finalCheck.entries().size() == 1 &&
                       finalCheck.entries().front() == "fresh item",
                   "copy vs clear concurrency does not resurrect cleared history");
    }

    std::filesystem::remove_all(directory, filesystemError);
    assertTrue(!filesystemError, "isolated history directory is removed");
    return EXIT_SUCCESS;
}
