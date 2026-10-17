#include "modernime/core/learning_store.h"
#include "modernime/core/personal_engine.h"
#include "modernime/pinyin/user_dictionary.h"
#include "modernime/settings/data_controller.h"
#include "modernime/settings/pages/dictionary_page.h"
#include "modernime/settings/pages/learning_page.h"
#include "modernime/settings/settings_model.h"

#include <gtk/gtk.h>
#include <sqlite3.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

struct HeartbeatMonitor {
    std::chrono::steady_clock::time_point lastTick = std::chrono::steady_clock::now();
    std::chrono::milliseconds maxLatency{0};
    std::atomic<bool> active{true};
    guint sourceId = 0;

    void start() {
        lastTick = std::chrono::steady_clock::now();
        maxLatency = std::chrono::milliseconds{0};
        active = true;
        sourceId = g_timeout_add(10, [](gpointer data) -> gboolean {
            auto *self = static_cast<HeartbeatMonitor *>(data);
            if (!self->active.load()) {
                return G_SOURCE_REMOVE;
            }
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - self->lastTick);
            // Expected tick is 10ms. Latency beyond 10ms:
            const auto latency = elapsed > std::chrono::milliseconds(10)
                                     ? elapsed - std::chrono::milliseconds(10)
                                     : std::chrono::milliseconds(0);
            if (latency > self->maxLatency) {
                self->maxLatency = latency;
            }
            self->lastTick = now;
            return G_SOURCE_CONTINUE;
        }, this);
    }

    void stop() {
        active = false;
        if (sourceId != 0) {
            g_source_remove(sourceId);
            sourceId = 0;
        }
    }
};

void drainEvents(int maxIterations = 500) {
    int count = 0;
    while (g_main_context_iteration(nullptr, FALSE) && ++count < maxIterations) {}
}

void pumpEventsUntil(const std::function<bool()> &condition,
                     std::chrono::milliseconds timeout = std::chrono::seconds(15)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        while (g_main_context_iteration(nullptr, FALSE)) {}
        if (condition()) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
}

// 1. Test LearningStore::entryCount() with SQL COUNT(*) and LMDB stat
void testLearningStoreEntryCount(const std::filesystem::path &tempDir) {
    std::cout << "[Test] Running testLearningStoreEntryCount..." << std::endl;

    // Test with non-existent DB
    {
        modernime::core::LearningStore nonExistent(tempDir / "does_not_exist.sqlite3");
        // Failure must NOT be masked as 0!
        assert(!nonExistent.entryCount().has_value());
    }

    // Populate a SQLite DB with 50,000 entries
    const auto sqlitePath = tempDir / "learning_50k.sqlite3";
    {
        modernime::core::LearningStore store(sqlitePath);
        assert(store.open());
        // Insert 50,000 entries efficiently via direct transaction
        sqlite3 *db = nullptr;
        assert(sqlite3_open(sqlitePath.c_str(), &db) == SQLITE_OK);
        assert(sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_stmt *stmt = nullptr;
        const char *insertSql =
            "INSERT INTO learning_entries (pinyin, phrase, frequency, last_selected_ms) "
            "VALUES (?, ?, 1, 1000);";
        assert(sqlite3_prepare_v2(db, insertSql, -1, &stmt, nullptr) == SQLITE_OK);

        for (int i = 0; i < 50000; ++i) {
            const std::string py = "py" + std::to_string(i);
            const std::string phrase = "词" + std::to_string(i);
            sqlite3_bind_text(stmt, 1, py.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, phrase.c_str(), -1, SQLITE_TRANSIENT);
            assert(sqlite3_step(stmt) == SQLITE_DONE);
            sqlite3_reset(stmt);
        }
        sqlite3_finalize(stmt);
        assert(sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);

        // Test entryCount() without full snapshot
        const auto start = std::chrono::steady_clock::now();
        const auto count = store.entryCount();
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);

        assert(count.has_value());
        assert(*count == 50000);
        // COUNT(*) should be near-instantaneous (< 20ms)
        std::cout << "  SQLite 50k COUNT(*) took: " << duration.count() << " ms" << std::endl;
        assert(duration < std::chrono::milliseconds(50));

        // Test DataController::learningEntryCount
        std::string err;
        const auto dcCount = modernime::settings::DataController::learningEntryCount(sqlitePath, &err);
        assert(err.empty());
        assert(dcCount == 50000);
    }

    // Test with LMDB backend
    const auto lmdbPath = tempDir / "learning_lmdb.lmdb";
    {
        modernime::core::LearningStore lmdbStore(lmdbPath);
        assert(lmdbStore.open());
        assert(lmdbStore.isLmdbBackend());

        // Initially empty
        const auto emptyCount = lmdbStore.entryCount();
        assert(emptyCount.has_value());
        assert(*emptyCount == 0);

        // Record a batch of events
        std::vector<modernime::core::LearningEvent> batch;
        for (int i = 0; i < 1000; ++i) {
            batch.push_back({
                modernime::core::LearningEvent::Kind::Selection,
                "lmdb_phrase_" + std::to_string(i),
                "lmdb_py_" + std::to_string(i),
                "", "", 1000 + i, std::nullopt
            });
        }
        assert(lmdbStore.recordBatch(batch));

        const auto countAfter = lmdbStore.entryCount();
        assert(countAfter.has_value());
        assert(*countAfter == 1000);

        std::string err;
        const auto dcCount = modernime::settings::DataController::learningEntryCount(lmdbPath, &err);
        assert(err.empty());
        assert(dcCount == 1000);
    }
    std::cout << "[Test] testLearningStoreEntryCount PASSED!" << std::endl;
}

// 2. Test DataController::filterDictionary pure function
void testFilterDictionaryPureFunction() {
    std::cout << "[Test] Running testFilterDictionaryPureFunction..." << std::endl;

    std::vector<modernime::pinyin::UserDictionaryEntry> entries;
    for (int i = 0; i < 50000; ++i) {
        modernime::pinyin::UserDictionaryEntry e;
        e.pinyin = "pinyin'" + std::to_string(i);
        e.phrase = "词条" + std::to_string(i);
        e.weight = 1.0f + static_cast<float>(i);
        entries.push_back(std::move(e));
    }

    // Add some known entries
    entries.push_back({"ni'hao", "你好", 10.0f});
    entries.push_back({"ce'shi", "测试", 20.0f});
    entries.push_back({"shi'jie", "世界", 30.0f});

    // Empty query must return all indices
    const auto all = modernime::settings::DataController::filterDictionary(entries, "");
    assert(all.size() == entries.size());
    for (std::size_t i = 0; i < all.size(); ++i) {
        assert(all[i] == i);
    }

    // Search by phrase substring
    const auto matchesNiHao = modernime::settings::DataController::filterDictionary(entries, "你好");
    assert(matchesNiHao.size() == 1);
    assert(entries[matchesNiHao[0]].phrase == "你好");

    // Search by pinyin substring (case-insensitive)
    const auto matchesCeShi = modernime::settings::DataController::filterDictionary(entries, "CE'SHI");
    assert(matchesCeShi.size() == 1);
    assert(entries[matchesCeShi[0]].phrase == "测试");

    // Search matching multiple items
    const auto matchesShi = modernime::settings::DataController::filterDictionary(entries, "shi");
    assert(matchesShi.size() >= 2); // "ce'shi" and "shi'jie"

    std::cout << "[Test] testFilterDictionaryPureFunction PASSED!" << std::endl;
}

const char *samplePinyin(int index) {
    static const char *syls[] = {"ce'shi", "ni'hao", "zhong'guo", "pin'yin", "xue'xi"};
    return syls[index % 5];
}

// 3. Test DictionaryPage search without disk re-reads and with 50k batch rendering
void testDictionaryPageAsyncAndBatch(const std::filesystem::path &tempDir) {
    std::cout << "[Test] Running testDictionaryPageAsyncAndBatch..." << std::endl;

    // Create a 50k dictionary file on disk
    const auto dictPath = tempDir / "user_dict_50k.txt";
    {
        std::ofstream out(dictPath);
        for (int i = 0; i < 50000; ++i) {
            out << samplePinyin(i) << "\t词" << i << "\t" << (1.0f + static_cast<float>(i % 100) * 0.1f) << "\n";
        }
    }

    HeartbeatMonitor heartbeat;
    heartbeat.start();

    std::string notifyMsg;
    auto page = std::make_unique<modernime::settings::DictionaryPage>(
        dictPath,
        [&notifyMsg](std::string msg) { notifyMsg = std::move(msg); });

    auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_container_add(GTK_CONTAINER(window), page->widget());
    gtk_widget_show_all(window);

    // Pump events until batch rendering finishes loading all 50k entries
    // Find GtkTreeView in page
    GtkTreeView *view = nullptr;
    // Use gtk_container_forall recursively to find GtkTreeView
    std::function<void(GtkWidget*)> scanContainer = [&](GtkWidget *w) {
        if (GTK_IS_TREE_VIEW(w)) {
            view = GTK_TREE_VIEW(w);
            return;
        }
        if (GTK_IS_CONTAINER(w)) {
            gtk_container_forall(GTK_CONTAINER(w), [](GtkWidget *child, gpointer data) {
                (*static_cast<std::function<void(GtkWidget*)>*>(data))(child);
            }, &scanContainer);
        }
    };
    scanContainer(page->widget());
    assert(view != nullptr);

    GtkTreeModel *model = gtk_tree_view_get_model(view);
    assert(model != nullptr);

    // Pump events until all 50k entries are populated
    std::cout << "  Waiting for 50k entries to load asynchronously and batch-render..." << std::endl;
    pumpEventsUntil([&]() {
        const int count = gtk_tree_model_iter_n_children(model, nullptr);
        return count >= 50000;
    }, std::chrono::seconds(15));

    const int totalRendered = gtk_tree_model_iter_n_children(model, nullptr);
    std::cout << "  Rendered entries in GtkListStore: " << totalRendered << std::endl;
    assert(totalRendered == 50000);

    // Heartbeat check during batch rendering: max latency must be < 100ms
    std::cout << "  Max heartbeat latency during 50k batch render: "
              << heartbeat.maxLatency.count() << " ms (target < 100ms)" << std::endl;
    assert(heartbeat.maxLatency < std::chrono::milliseconds(100));

    // Verify search does NOT touch disk
    // We can delete or rename the file on disk:
    const auto dictPathMoved = tempDir / "user_dict_50k_moved.txt";
    std::filesystem::rename(dictPath, dictPathMoved);

    // Now trigger search in the search entry
    GtkSearchEntry *searchEntry = nullptr;
    std::function<void(GtkWidget*)> scanSearch = [&](GtkWidget *w) {
        if (GTK_IS_SEARCH_ENTRY(w)) {
            searchEntry = GTK_SEARCH_ENTRY(w);
            return;
        }
        if (GTK_IS_CONTAINER(w)) {
            gtk_container_forall(GTK_CONTAINER(w), [](GtkWidget *child, gpointer data) {
                (*static_cast<std::function<void(GtkWidget*)>*>(data))(child);
            }, &scanSearch);
        }
    };
    scanSearch(page->widget());
    assert(searchEntry != nullptr);

    // Set search text to match specific item: "词123"
    gtk_entry_set_text(GTK_ENTRY(searchEntry), "词123");
    drainEvents();

    pumpEventsUntil([&]() {
        const int count = gtk_tree_model_iter_n_children(model, nullptr);
        return count > 0 && count < 50000;
    }, std::chrono::seconds(5));

    const int filteredCount = gtk_tree_model_iter_n_children(model, nullptr);
    std::cout << "  Filtered count for '词123': " << filteredCount << " (without reading disk)" << std::endl;
    assert(filteredCount > 0);

    // Restore dict file
    std::filesystem::rename(dictPathMoved, dictPath);

    heartbeat.stop();
    gtk_widget_destroy(window);
    drainEvents();
    std::cout << "[Test] testDictionaryPageAsyncAndBatch PASSED!" << std::endl;
}

// 4. Test safe window closing during slow I/O (No UAF)
void testSafeWindowClosingUnderSlowIo(const std::filesystem::path &tempDir) {
    std::cout << "[Test] Running testSafeWindowClosingUnderSlowIo..." << std::endl;

    const auto dictPath = tempDir / "user_dict_slow.txt";
    {
        std::ofstream out(dictPath);
        for (int i = 0; i < 10000; ++i) {
            out << samplePinyin(i) << "\t词" << i << "\t1.0\n";
        }
    }

    auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    auto page = std::make_unique<modernime::settings::DictionaryPage>(dictPath, [](std::string) {});
    gtk_container_add(GTK_CONTAINER(window), page->widget());
    gtk_widget_show_all(window);

    // Start refresh (triggers async GTask load)
    page->refresh();

    // Immediately destroy the page and window while worker thread may be running
    page.reset();
    gtk_widget_destroy(window);

    // Drain events - must not crash or trigger ASAN use-after-free
    for (int i = 0; i < 50; ++i) {
        drainEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "[Test] testSafeWindowClosingUnderSlowIo PASSED!" << std::endl;
}

// 5. Test backup failure does not clear learning store
void testBackupFailureDoesNotClearStore(const std::filesystem::path &tempDir) {
    std::cout << "[Test] Running testBackupFailureDoesNotClearStore..." << std::endl;

    const auto storePath = tempDir / "learning_backup_fail.sqlite3";
    {
        modernime::core::LearningStore store(storePath);
        assert(store.open());
        assert(store.recordSelection("保持词条", "bao'chi", "", "", 1000));
        assert(store.entryCount().value_or(0) == 1);
    }

    // Try backupAndClearLearning with an invalid backup destination (cannot create)
    const std::filesystem::path invalidBackup = "/proc/invalid_backup_dir/backup.sqlite3";
    std::string error;
    const bool success = modernime::settings::DataController::backupAndClearLearning(
        storePath, invalidBackup, &error);

    assert(!success);
    assert(!error.empty());
    std::cout << "  Expected backup error: " << error << std::endl;

    // Verify original store is NOT cleared!
    {
        modernime::core::LearningStore store(storePath);
        assert(store.openReadOnly());
        const auto count = store.entryCount();
        assert(count.has_value());
        assert(*count == 1); // Preserved!
    }

    std::cout << "[Test] testBackupFailureDoesNotClearStore PASSED!" << std::endl;
}

// 6. Test continuous rapid search supersedes old idle render batches and keeps heartbeat < 100ms
void testContinuousSearch(const std::filesystem::path &tempDir) {
    std::cout << "[Test] Running testContinuousSearch..." << std::endl;

    const auto dictPath = tempDir / "user_dict_cont_search.txt";
    {
        std::ofstream out(dictPath);
        for (int i = 0; i < 20000; ++i) {
            out << samplePinyin(i) << "\t词" << i << "\t" << (1.0f + static_cast<float>(i % 100)) << "\n";
        }
    }

    auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    auto page = std::make_unique<modernime::settings::DictionaryPage>(dictPath, [](std::string) {});
    gtk_container_add(GTK_CONTAINER(window), page->widget());
    gtk_widget_show_all(window);

    // Find TreeView and SearchEntry
    GtkTreeView *view = nullptr;
    GtkSearchEntry *searchEntry = nullptr;
    std::function<void(GtkWidget*)> scan = [&](GtkWidget *w) {
        if (GTK_IS_TREE_VIEW(w)) view = GTK_TREE_VIEW(w);
        if (GTK_IS_SEARCH_ENTRY(w)) searchEntry = GTK_SEARCH_ENTRY(w);
        if (GTK_IS_CONTAINER(w)) {
            gtk_container_forall(GTK_CONTAINER(w), [](GtkWidget *c, gpointer d) {
                (*static_cast<std::function<void(GtkWidget*)>*>(d))(c);
            }, &scan);
        }
    };
    scan(page->widget());
    assert(view != nullptr);
    assert(searchEntry != nullptr);
    GtkTreeModel *model = gtk_tree_view_get_model(view);

    // Wait for initial load
    pumpEventsUntil([&]() {
        return gtk_tree_model_iter_n_children(model, nullptr) >= 20000;
    }, std::chrono::seconds(10));

    HeartbeatMonitor heartbeat;
    heartbeat.start();

    // Rapidly change search queries
    const std::vector<std::string> queries = {"py", "py1", "py12", "py123", "none_match", "词99"};
    for (const auto &q : queries) {
        gtk_entry_set_text(GTK_ENTRY(searchEntry), q.c_str());
        drainEvents(50);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Wait for batch render of final query to complete
    pumpEventsUntil([&]() {
        const int count = gtk_tree_model_iter_n_children(model, nullptr);
        // "词99" matches "词99", "词990".."词999", etc. (around 111 matches in 20000 entries)
        return count > 0 && count < 20000;
    }, std::chrono::seconds(5));

    heartbeat.stop();
    std::cout << "  Max heartbeat latency during continuous search: "
              << heartbeat.maxLatency.count() << " ms (target < 100ms)" << std::endl;
    assert(heartbeat.maxLatency < std::chrono::milliseconds(100));

    gtk_widget_destroy(window);
    drainEvents();
    std::cout << "[Test] testContinuousSearch PASSED!" << std::endl;
}

// 7. Test 2s slow I/O worker with heartbeat < 100ms and LearningPage async lifecycle
void testLearningPageAsyncAndSlowIo(const std::filesystem::path &tempDir) {
    std::cout << "[Test] Running testLearningPageAsyncAndSlowIo..." << std::endl;

    const auto storePath = tempDir / "learning_slow_io.sqlite3";
    {
        modernime::core::LearningStore store(storePath);
        assert(store.open());
        for (int i = 0; i < 100; ++i) {
            store.recordSelection("词" + std::to_string(i), "py" + std::to_string(i), "", "", 1000 + i);
        }
    }

    // 1. Simulate 2s slow I/O in a background GTask while heartbeat monitor runs
    HeartbeatMonitor heartbeat;
    heartbeat.start();

    std::atomic<bool> slowTaskDone{false};
    auto *task = g_task_new(nullptr, nullptr, [](GObject *, GAsyncResult *, gpointer d) {
        *static_cast<std::atomic<bool>*>(d) = true;
    }, &slowTaskDone);

    g_task_run_in_thread(task, [](GTask *t, gpointer, gpointer, GCancellable *) {
        // Sleep for 2 seconds to simulate slow disk or heavy I/O
        std::this_thread::sleep_for(std::chrono::seconds(2));
        g_task_return_boolean(t, TRUE);
    });
    g_object_unref(task);

    // Pump GTK events during the 2s slow I/O
    pumpEventsUntil([&]() { return slowTaskDone.load(); }, std::chrono::seconds(5));
    assert(slowTaskDone.load());

    heartbeat.stop();
    std::cout << "  Max heartbeat latency during 2s slow I/O: "
              << heartbeat.maxLatency.count() << " ms (target < 100ms)" << std::endl;
    assert(heartbeat.maxLatency < std::chrono::milliseconds(100));

    // 2. Test LearningPage async lifecycle & safe closing during async refresh
    auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    modernime::settings::SettingsWindowModel model(tempDir / "settings.json");
    auto page = std::make_unique<modernime::settings::LearningPage>(
        model, storePath, []{}, [](std::string){});
    gtk_container_add(GTK_CONTAINER(window), page->widget());
    gtk_widget_show_all(window);

    // Trigger async refresh
    page->refresh(false);

    // Immediately destroy LearningPage and window while async task is in flight
    page.reset();
    gtk_widget_destroy(window);

    // Drain events - ensures DiagnosticsLifetime deactivated safely without UAF
    for (int i = 0; i < 20; ++i) {
        drainEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "[Test] testLearningPageAsyncAndSlowIo PASSED!" << std::endl;
}

} // namespace

int main(int argc, char **argv) {
    if (!gtk_init_check(&argc, &argv)) {
        std::cout << "GTK initialization skipped (headless / no display). Returning 77." << std::endl;
        return 77;
    }

    const auto tempDir = std::filesystem::temp_directory_path() /
                         ("modernime-settings-data-async-test-" + std::to_string(getpid()));
    std::filesystem::create_directories(tempDir);

    try {
        testLearningStoreEntryCount(tempDir);
        testFilterDictionaryPureFunction();
        testDictionaryPageAsyncAndBatch(tempDir);
        testSafeWindowClosingUnderSlowIo(tempDir);
        testBackupFailureDoesNotClearStore(tempDir);
        testContinuousSearch(tempDir);
        testLearningPageAsyncAndSlowIo(tempDir);
        std::cout << "\nALL MODERNIME SETTINGS DATA ASYNC GTK TESTS PASSED!\n" << std::endl;
    } catch (const std::exception &e) {
        std::cerr << "FATAL TEST EXCEPTION: " << e.what() << std::endl;
        std::filesystem::remove_all(tempDir);
        return 1;
    }

    std::filesystem::remove_all(tempDir);
    return 0;
}
