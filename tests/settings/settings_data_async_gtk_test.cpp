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
#include <cstring>
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
    std::atomic<int> sampleCount{0};
    guint sourceId = 0;

    void start() {
        lastTick = std::chrono::steady_clock::now();
        maxLatency = std::chrono::milliseconds{0};
        sampleCount.store(0);
        active = true;
        sourceId = g_timeout_add(10, [](gpointer data) -> gboolean {
            auto *self = static_cast<HeartbeatMonitor *>(data);
            if (!self->active.load()) {
                return G_SOURCE_REMOVE;
            }
            self->sampleCount.fetch_add(1, std::memory_order_relaxed);
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
        if (!active.load()) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTick);
        const auto latency = elapsed > std::chrono::milliseconds(10)
                                 ? elapsed - std::chrono::milliseconds(10)
                                 : std::chrono::milliseconds(0);
        if (latency > maxLatency) {
            maxLatency = latency;
        }
        active = false;
        if (sourceId != 0) {
            g_source_remove(sourceId);
            sourceId = 0;
        }
    }

    ~HeartbeatMonitor() {
        stop();
    }
};

#if defined(__SANITIZE_ADDRESS__)
#define MODERNIME_TEST_IS_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define MODERNIME_TEST_IS_ASAN 1
#else
#define MODERNIME_TEST_IS_ASAN 0
#endif
#else
#define MODERNIME_TEST_IS_ASAN 0
#endif

constexpr auto kMaxAllowedLatency = MODERNIME_TEST_IS_ASAN
    ? std::chrono::milliseconds(300)
    : std::chrono::milliseconds(100);

void drainEvents(int maxIterations = 500) {
    int count = 0;
    while (g_main_context_iteration(nullptr, FALSE) && ++count < maxIterations) {}
}

void pumpEventsUntil(const std::function<bool()> &condition,
                     std::chrono::milliseconds timeout = std::chrono::seconds(15)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (!g_main_context_iteration(nullptr, FALSE)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (condition()) {
            return;
        }
    } while (std::chrono::steady_clock::now() < deadline);
}

GtkWidget *findTarget(GtkWidget *widget, std::string_view target) {
    if (widget == nullptr) {
        return nullptr;
    }
    const auto *id = static_cast<const char *>(g_object_get_data(
        G_OBJECT(widget), "modernime-settings-target"));
    if (id != nullptr && target == id) {
        return widget;
    }
    if (!GTK_IS_CONTAINER(widget)) {
        return nullptr;
    }
    auto *children = gtk_container_get_children(GTK_CONTAINER(widget));
    GtkWidget *result = nullptr;
    for (auto *item = children; item != nullptr && result == nullptr;
         item = item->next) {
        result = findTarget(GTK_WIDGET(item->data), target);
    }
    g_list_free(children);
    return result;
}

void clickWithConfirmation(GtkWidget *button) {
    g_timeout_add(10, [](gpointer) -> gboolean {
        GList *w = gtk_window_list_toplevels();
        bool found = false;
        for (auto *i = w; i != nullptr; i = i->next) {
            if (GTK_IS_MESSAGE_DIALOG(i->data)) {
                gtk_dialog_response(GTK_DIALOG(i->data), GTK_RESPONSE_YES);
                found = true;
            }
        }
        g_list_free(w);
        return found ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
    }, nullptr);
    gtk_button_clicked(GTK_BUTTON(button));
}

const char *samplePinyin(int index) {
    static const char *syls[] = {"ce'shi", "ni'hao", "zhong'guo", "pin'yin", "xue'xi"};
    return syls[index % 5];
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
        // COUNT(*) should be near-instantaneous (< 50ms)
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

    std::string notifyMsg;
    auto page = std::make_unique<modernime::settings::DictionaryPage>(
        dictPath,
        [&notifyMsg](std::string msg) { notifyMsg = std::move(msg); });

    auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_container_add(GTK_CONTAINER(window), page->widget());
    gtk_widget_show_all(window);

    HeartbeatMonitor heartbeat;
    heartbeat.start();

    GtkTreeView *view = nullptr;
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
    assert(heartbeat.maxLatency < kMaxAllowedLatency);

    // Verify search does NOT touch disk
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

// 4. Test DictionaryPage real production slow I/O safety & 2s GTK heartbeat < 100ms
void testDictionaryPageAsyncIoAndSlowIoSafety(const std::filesystem::path &tempDir) {
    std::cout << "[Test] Running testDictionaryPageAsyncIoAndSlowIoSafety..." << std::endl;

    const auto dictPath = tempDir / "user_dict_r2_slow.txt";
    {
        std::ofstream out(dictPath);
        for (int i = 0; i < 500; ++i) {
            out << samplePinyin(i) << "\t词" << i << "\t1.0\n";
        }
    }

    // Part 1: Verify 2-second real production I/O (loadDictionary) keeps GTK heartbeat < 100ms
    auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    auto page = std::make_unique<modernime::settings::DictionaryPage>(dictPath, [](std::string) {});
    gtk_container_add(GTK_CONTAINER(window), page->widget());
    gtk_widget_show_all(window);
    drainEvents();

    std::atomic<bool> slowLoadEntered{false};
    std::atomic<bool> slowLoadFinished{false};
    modernime::settings::DataController::setIoHookForTesting([&](std::string_view op) {
        if (op == "loadDictionary") {
            slowLoadEntered = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            slowLoadFinished = true;
        }
    });

    HeartbeatMonitor heartbeat;
    heartbeat.start();

    page->refresh();

    // Pump events during the 2-second real production loadDictionary worker
    pumpEventsUntil([&] { return slowLoadFinished.load(); }, std::chrono::seconds(5));
    assert(slowLoadEntered.load());
    assert(slowLoadFinished.load());

    heartbeat.stop();
    std::cout << "  Max heartbeat latency during 2s production loadDictionary: "
              << heartbeat.maxLatency.count() << " ms (target < 100ms)"
              << " samples: " << heartbeat.sampleCount.load() << std::endl;
    assert(heartbeat.sampleCount.load() >= 10);
    assert(heartbeat.maxLatency < kMaxAllowedLatency);

    modernime::settings::DataController::setIoHookForTesting(nullptr);
    drainEvents();

    // Part 2: Verify window destruction while slow production saveDictionary is in-flight (0 UAF)
    std::atomic<bool> slowSaveEntered{false};
    std::atomic<bool> releaseSlowSave{false};
    std::atomic<bool> slowSaveFinished{false};
    modernime::settings::DataController::setIoHookForTesting([&](std::string_view op) {
        if (op == "saveDictionary") {
            slowSaveEntered = true;
            while (!releaseSlowSave.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            slowSaveFinished = true;
        }
    });

    // Find delete button
    auto *deleteBtn = findTarget(page->widget(), "dictionary-delete");
    if (deleteBtn == nullptr) {
        std::function<void(GtkWidget*)> scanBtn = [&](GtkWidget *w) {
            if (GTK_IS_BUTTON(w)) {
                const auto *label = gtk_button_get_label(GTK_BUTTON(w));
                if (label != nullptr && std::strcmp(label, "删除") == 0) {
                    deleteBtn = w;
                    return;
                }
            }
            if (GTK_IS_CONTAINER(w)) {
                gtk_container_forall(GTK_CONTAINER(w), [](GtkWidget *c, gpointer d) {
                    (*static_cast<std::function<void(GtkWidget*)>*>(d))(c);
                }, &scanBtn);
            }
        };
        scanBtn(page->widget());
    }

    // Wait until entries are populated in treeview
    GtkTreeView *view = nullptr;
    std::function<void(GtkWidget*)> scanView = [&](GtkWidget *w) {
        if (GTK_IS_TREE_VIEW(w)) { view = GTK_TREE_VIEW(w); return; }
        if (GTK_IS_CONTAINER(w)) {
            gtk_container_forall(GTK_CONTAINER(w), [](GtkWidget *c, gpointer d) {
                (*static_cast<std::function<void(GtkWidget*)>*>(d))(c);
            }, &scanView);
        }
    };
    scanView(page->widget());
    assert(view != nullptr);

    GtkTreeModel *model = gtk_tree_view_get_model(view);
    pumpEventsUntil([&] { return gtk_tree_model_iter_n_children(model, nullptr) >= 500; }, std::chrono::seconds(5));

    // Select row 0
    auto *rowPath = gtk_tree_path_new_from_indices(0, -1);
    gtk_tree_view_set_cursor(view, rowPath, nullptr, FALSE);
    gtk_tree_path_free(rowPath);
    pumpEventsUntil([&] { return deleteBtn != nullptr && gtk_widget_get_sensitive(deleteBtn); }, std::chrono::seconds(5));
    drainEvents();

    assert(deleteBtn != nullptr && gtk_widget_get_sensitive(deleteBtn));
    clickWithConfirmation(deleteBtn);

    // Wait until worker enters saveDictionary
    pumpEventsUntil([&] { return slowSaveEntered.load(); }, std::chrono::seconds(5));
    assert(slowSaveEntered.load());

    // While saveDictionary is in progress, destroy page and window!
    page.reset();
    gtk_widget_destroy(window);

    // Release worker thread
    releaseSlowSave = true;

    pumpEventsUntil([&] { return slowSaveFinished.load(); }, std::chrono::seconds(5));
    assert(slowSaveFinished.load());
    drainEvents();

    modernime::settings::DataController::setIoHookForTesting(nullptr);
    std::cout << "[Test] testDictionaryPageAsyncIoAndSlowIoSafety PASSED!" << std::endl;
}

// 5. Test safe window closing during slow I/O (No UAF)
void testSafeWindowClosingUnderSlowIo(const std::filesystem::path &tempDir) {
    std::cout << "[Test] Running testSafeWindowClosingUnderSlowIo..." << std::endl;

    const auto dictPath = tempDir / "user_dict_slow.txt";
    {
        std::ofstream out(dictPath);
        for (int i = 0; i < 10000; ++i) {
            out << samplePinyin(i) << "\t词" << i << "\t1.0\n";
        }
    }

    std::atomic<bool> loadPaused{false};
    std::atomic<bool> releaseLoadPause{false};
    std::atomic<bool> loadFinished{false};
    modernime::settings::DataController::setIoHookForTesting([&](std::string_view op) {
        if (op == "loadDictionary") {
            loadPaused = true;
            while (!releaseLoadPause.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            loadFinished = true;
        }
    });

    auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    auto page = std::make_unique<modernime::settings::DictionaryPage>(dictPath, [](std::string) {});
    gtk_container_add(GTK_CONTAINER(window), page->widget());
    gtk_widget_show_all(window);

    pumpEventsUntil([&] { return loadPaused.load(); }, std::chrono::seconds(5));
    assert(loadPaused.load());

    // Immediately destroy the page and window while worker thread is paused in loadDictionary
    page.reset();
    gtk_widget_destroy(window);

    // Release worker thread
    releaseLoadPause = true;

    pumpEventsUntil([&] { return loadFinished.load(); }, std::chrono::seconds(5));
    assert(loadFinished.load());
    drainEvents();

    modernime::settings::DataController::setIoHookForTesting(nullptr);
    std::cout << "[Test] testSafeWindowClosingUnderSlowIo PASSED!" << std::endl;
}

// 6. Test backup failure does not clear learning store
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

// 7. Test continuous rapid search with debounce handling, batch interruption, and exact row verification
void testContinuousSearch(const std::filesystem::path &tempDir) {
    std::cout << "[Test] Running testContinuousSearch..." << std::endl;

    const auto dictPath = tempDir / "user_dict_cont_search.txt";
    {
        std::ofstream out(dictPath);
        for (int i = 0; i < 20000; ++i) {
            const std::string py = samplePinyin(i);
            const std::string ph = "词" + std::to_string(i);
            const float w = 1.0f + static_cast<float>(i % 100);
            out << py << "\t" << ph << "\t" << w << "\n";
        }
    }
    const auto allEntries = modernime::settings::DataController::loadDictionary(dictPath);

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

    // Wait for initial load (20,000 entries)
    pumpEventsUntil([&]() {
        return gtk_tree_model_iter_n_children(model, nullptr) >= 20000;
    }, std::chrono::seconds(10));
    assert(gtk_tree_model_iter_n_children(model, nullptr) == 20000);

    HeartbeatMonitor heartbeat;
    heartbeat.start();

    struct FilterObservation {
        GtkSearchEntry *entry = nullptr;
        GtkTreeModel *model = nullptr;
        int xueOldRowsBeforeFilter = -1;
        int zhongOldRowsBeforeFilter = -1;
    } filterObs{searchEntry, model};

    const guint signalId = g_signal_lookup("search-changed", GTK_TYPE_SEARCH_ENTRY);
    const gulong emissionHook = g_signal_add_emission_hook(
        signalId, 0,
        +[](GSignalInvocationHint *, guint, const GValue *paramValues, gpointer data) -> gboolean {
            auto *obs = static_cast<FilterObservation *>(data);
            if (paramValues != nullptr && g_value_get_object(&paramValues[0]) == obs->entry) {
                const auto *query = gtk_entry_get_text(GTK_ENTRY(obs->entry));
                const int currentRows = gtk_tree_model_iter_n_children(obs->model, nullptr);
                if (std::strcmp(query, "xue") == 0 && obs->xueOldRowsBeforeFilter < 0) {
                    obs->xueOldRowsBeforeFilter = currentRows;
                    std::cout << "  before_production_filter query=xue old_rows=" << currentRows << std::endl;
                } else if (std::strcmp(query, "zhong") == 0 && obs->zhongOldRowsBeforeFilter < 0) {
                    obs->zhongOldRowsBeforeFilter = currentRows;
                    std::cout << "  before_production_filter query=zhong old_rows=" << currentRows << std::endl;
                }
            }
            return TRUE;
        },
        &filterObs, nullptr);

    int observedSearchSignals = 0;
    g_signal_connect(searchEntry, "search-changed",
                     G_CALLBACK(+[](GtkSearchEntry *, gpointer data) {
                         ++*static_cast<int *>(data);
                     }), &observedSearchSignals);

    // Multi-round rapid search with actual in-flight batch interruption:
    // GtkSearchEntry has a 150ms timeout debounce.
    const int totalPin = static_cast<int>(modernime::settings::DataController::filterDictionary(allEntries, "pin").size());
    assert(totalPin == 4000);
    const int totalXue = static_cast<int>(modernime::settings::DataController::filterDictionary(allEntries, "xue").size());
    assert(totalXue == 4000);

    // Round 1: set text "pin" -> wait for search-changed to fire and idle batch render to start (> 0 rows)
    gtk_entry_set_text(GTK_ENTRY(searchEntry), "pin");
    pumpEventsUntil([&] {
        return observedSearchSignals >= 1 && gtk_tree_model_iter_n_children(model, nullptr) > 0;
    }, std::chrono::seconds(5));
    assert(observedSearchSignals >= 1);
    const int round1Rows = gtk_tree_model_iter_n_children(model, nullptr);
    std::cout << "  round1_before_interrupt_rows=" << round1Rows
              << " expected_total=" << totalPin << std::endl;
    assert(round1Rows > 0 && round1Rows < totalPin);

    // While "pin" is actively rendering in batches (only partial rows rendered), interrupt it with round 2!
    gtk_entry_set_text(GTK_ENTRY(searchEntry), "xue");
    g_signal_emit_by_name(searchEntry, "search-changed");
    pumpEventsUntil([&] {
        return observedSearchSignals >= 2 && gtk_tree_model_iter_n_children(model, nullptr) > 0;
    }, std::chrono::seconds(5));
    assert(observedSearchSignals >= 2);
    const int round2Rows = gtk_tree_model_iter_n_children(model, nullptr);
    std::cout << "  round2_before_interrupt_rows=" << round2Rows
              << " expected_total=" << totalXue << std::endl;
    assert(round2Rows > 0 && round2Rows < totalXue);

    // Now interrupt round 2 in-flight with final query "zhong"
    gtk_entry_set_text(GTK_ENTRY(searchEntry), "zhong");
    g_signal_emit_by_name(searchEntry, "search-changed");
    pumpEventsUntil([&] {
        return observedSearchSignals >= 3;
    }, std::chrono::seconds(5));
    assert(observedSearchSignals >= 3);

    // Wait for final query "zhong" batch rendering to fully finish
    const auto expectedIndices = modernime::settings::DataController::filterDictionary(allEntries, "zhong");
    assert(!expectedIndices.empty());

    pumpEventsUntil([&] {
        const int count = gtk_tree_model_iter_n_children(model, nullptr);
        return count == static_cast<int>(expectedIndices.size());
    }, std::chrono::seconds(10));

    const int finalCount = gtk_tree_model_iter_n_children(model, nullptr);
    std::cout << "  Actual search-changed signals: " << observedSearchSignals << std::endl;
    std::cout << "  Rendered rows for 'zhong': " << finalCount
              << " (expected " << expectedIndices.size() << ")" << std::endl;
    assert(finalCount == static_cast<int>(expectedIndices.size()));

    // EXACT verification of each row: col 0 = pinyin, col 1 = phrase, col 3 = sourceIndex
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
    for (std::size_t i = 0; i < expectedIndices.size(); ++i) {
        assert(valid);
        gchar *py = nullptr;
        gchar *ph = nullptr;
        guint sourceIndex = 0;
        gtk_tree_model_get(model, &iter, 0, &py, 1, &ph, 3, &sourceIndex, -1);

        const auto &expected = allEntries[expectedIndices[i]];
        assert(sourceIndex == expectedIndices[i]);
        assert(expected.pinyin == py);
        assert(expected.phrase == ph);

        g_free(py);
        g_free(ph);
        valid = gtk_tree_model_iter_next(model, &iter);
    }
    assert(!valid); // Exactly expectedIndices.size() rows, no more, no old rows leaked!

    g_signal_remove_emission_hook(signalId, emissionHook);

    // Formally assert that BEFORE production filtering started, the old batch was truly in-flight (0 < oldRows < totalOldRows)
    std::cout << "  asserting before_production_filter: xue_old_rows=" << filterObs.xueOldRowsBeforeFilter
              << " (expected < " << totalPin << "), zhong_old_rows=" << filterObs.zhongOldRowsBeforeFilter
              << " (expected < " << totalXue << ")" << std::endl;
    assert(filterObs.xueOldRowsBeforeFilter > 0 && filterObs.xueOldRowsBeforeFilter < totalPin);
    assert(filterObs.zhongOldRowsBeforeFilter > 0 && filterObs.zhongOldRowsBeforeFilter < totalXue);

    heartbeat.stop();
    std::cout << "  Max heartbeat latency during continuous search: "
              << heartbeat.maxLatency.count() << " ms (target < 100ms)" << std::endl;
    assert(heartbeat.maxLatency < kMaxAllowedLatency);

    // Verify search does NOT touch disk (rename file and search again)
    const auto dictPathMoved = tempDir / "user_dict_cont_search_moved.txt";
    std::filesystem::rename(dictPath, dictPathMoved);

    gtk_entry_set_text(GTK_ENTRY(searchEntry), "ceshi");
    const auto expectedCeShi = modernime::settings::DataController::filterDictionary(allEntries, "ceshi");
    assert(!expectedCeShi.empty());
    pumpEventsUntil([&] {
        return gtk_tree_model_iter_n_children(model, nullptr) == static_cast<int>(expectedCeShi.size());
    }, std::chrono::seconds(10));
    assert(gtk_tree_model_iter_n_children(model, nullptr) == static_cast<int>(expectedCeShi.size()));

    std::filesystem::rename(dictPathMoved, dictPath);

    gtk_widget_destroy(window);
    drainEvents();
    std::cout << "[Test] testContinuousSearch PASSED!" << std::endl;
}

// 8. Test LearningPage stale refresh and clear reentrancy (R1 Solidification)
void testLearningPageStaleRefreshAndClearReentry(const std::filesystem::path &tempDir) {
    std::cout << "[Test] Running testLearningPageStaleRefreshAndClearReentry..." << std::endl;

    // --- Scenario A: Stale refresh cannot override newer state ---
    const auto dirA = tempDir / "r1_stale";
    std::filesystem::create_directories(dirA);
    const auto dbPathA = dirA / "learning.sqlite3";
    {
        modernime::core::LearningStore store(dbPathA);
        assert(store.open());
        assert(store.recordSelection("你好", "ni'hao", "", "", 1000));
        assert(store.entryCount().value_or(0) == 1);
    }

    std::atomic<bool> armCountHook{false};
    std::atomic<bool> countHookEntered{false};
    std::atomic<bool> releaseCountHook{false};
    std::atomic<bool> refresh1Finished{false};

    modernime::settings::DataController::setIoHookForTesting([&](std::string_view op) {
        if (op == "learningEntryCount:afterCount" && armCountHook.exchange(false)) {
            countHookEntered = true;
            while (!releaseCountHook.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            refresh1Finished = true;
        }
    });

    modernime::settings::SettingsWindowModel modelA(dirA / "settings.conf");
    int notifyCountA = 0;
    auto pageA = std::make_unique<modernime::settings::LearningPage>(
        modelA, dbPathA, [] {}, [&](std::string) { ++notifyCountA; });
    auto *winA = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_container_add(GTK_CONTAINER(winA), pageA->widget());
    gtk_widget_show_all(winA);

    auto *clearBtnA = findTarget(pageA->widget(), "learning-clear");
    assert(clearBtnA != nullptr);

    // Initial load: button becomes sensitive because count == 1
    pumpEventsUntil([&] { return gtk_widget_get_sensitive(clearBtnA); }, std::chrono::seconds(5));
    assert(gtk_widget_get_sensitive(clearBtnA));

    // Arm hook to pause refresh #1 AFTER reading count == 1
    armCountHook = true;
    countHookEntered = false;
    releaseCountHook = false;
    refresh1Finished = false;

    // Request refresh #1 (reads count=1, then pauses in afterCount hook)
    pageA->refresh(false);
    pumpEventsUntil([&] { return countHookEntered.load(); }, std::chrono::seconds(5));
    assert(countHookEntered.load());

    // While refresh #1 is paused (having already read count=1), clear all entries from DB
    {
        sqlite3 *db = nullptr;
        assert(sqlite3_open(dbPathA.c_str(), &db) == SQLITE_OK);
        assert(sqlite3_exec(db, "DELETE FROM learning_entries;", nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);
    }

    // Request refresh #2 (reads count=0 and applies to UI)
    const int notifiesBefore = notifyCountA;
    pageA->refresh(true);
    pumpEventsUntil([&] {
        return notifyCountA > notifiesBefore && !gtk_widget_get_sensitive(clearBtnA);
    }, std::chrono::seconds(5));

    // Confirm that refresh #2 completed and button is now disabled (count == 0)
    assert(!gtk_widget_get_sensitive(clearBtnA));

    // Now release refresh #1's worker thread (which carries old result count=1)
    releaseCountHook = true;
    pumpEventsUntil([&] { return refresh1Finished.load(); }, std::chrono::seconds(5));
    assert(refresh1Finished.load());
    drainEvents();

    // Refresh #1 has older generation: must be discarded and MUST NOT re-enable the button!
    assert(!gtk_widget_get_sensitive(clearBtnA));

    pageA.reset();
    gtk_widget_destroy(winA);
    drainEvents();

    // --- Scenario B: Clear in progress refuses reentrancy and cannot be re-enabled by stale refresh ---
    const auto dirB = tempDir / "r1_reentry";
    std::filesystem::create_directories(dirB);
    const auto dbPathB = dirB / "learning.sqlite3";
    {
        modernime::core::LearningStore store(dbPathB);
        assert(store.open());
        assert(store.recordSelection("世界", "shi'jie", "", "", 1000));
        assert(store.entryCount().value_or(0) == 1);
    }

    std::atomic<bool> armBackupHook{false};
    std::atomic<bool> backupHookEntered{false};
    std::atomic<bool> releaseBackupHook{false};
    std::atomic<int> backupCallCount{0};
    std::atomic<bool> refreshBFinished{false};
    std::atomic<bool> backupFinished{false};

    modernime::settings::DataController::setIoHookForTesting([&](std::string_view op) {
        if (op == "learningEntryCount" && armCountHook.exchange(false)) {
            countHookEntered = true;
            while (!releaseCountHook.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            refreshBFinished = true;
        } else if (op == "backupAndClearLearning") {
            ++backupCallCount;
            if (armBackupHook.exchange(false)) {
                backupHookEntered = true;
                while (!releaseBackupHook.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                backupFinished = true;
            }
        }
    });

    modernime::settings::SettingsWindowModel modelB(dirB / "settings.conf");
    int notifyCountB = 0;
    auto pageB = std::make_unique<modernime::settings::LearningPage>(
        modelB, dbPathB, [] {}, [&](std::string) { ++notifyCountB; });
    auto *winB = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_container_add(GTK_CONTAINER(winB), pageB->widget());
    gtk_widget_show_all(winB);

    auto *clearBtnB = findTarget(pageB->widget(), "learning-clear");
    assert(clearBtnB != nullptr);
    pumpEventsUntil([&] { return gtk_widget_get_sensitive(clearBtnB); }, std::chrono::seconds(5));
    assert(gtk_widget_get_sensitive(clearBtnB));

    // Pause a pending refresh in worker
    armCountHook = true;
    countHookEntered = false;
    releaseCountHook = false;
    refreshBFinished = false;
    pageB->refresh(false);
    pumpEventsUntil([&] { return countHookEntered.load(); }, std::chrono::seconds(5));
    assert(countHookEntered.load());

    // Arm backup hook and click clear button with confirmation
    armBackupHook = true;
    backupHookEntered = false;
    releaseBackupHook = false;
    backupFinished = false;
    clickWithConfirmation(clearBtnB);

    // Wait until backup worker is paused inside backupAndClearLearning
    pumpEventsUntil([&] { return backupHookEntered.load(); }, std::chrono::seconds(5));
    assert(backupHookEntered.load());
    assert(backupCallCount.load() == 1);

    // Clear is in progress: button must be insensitive
    assert(!gtk_widget_get_sensitive(clearBtnB));

    // Now release the pending refresh
    releaseCountHook = true;
    pumpEventsUntil([&] { return refreshBFinished.load(); }, std::chrono::seconds(5));
    assert(refreshBFinished.load());
    drainEvents();

    // Refresh completing during clear MUST NOT re-enable the clear button!
    assert(!gtk_widget_get_sensitive(clearBtnB));

    // Attempt second clear click / call - must be ignored / reentrancy refused
    gtk_button_clicked(GTK_BUTTON(clearBtnB));
    drainEvents();
    assert(backupCallCount.load() == 1); // No second backup started!

    // Release the backup worker
    releaseBackupHook = true;
    pumpEventsUntil([&] {
        if (!backupFinished.load() || notifyCountB == 0) {
            return false;
        }
        modernime::core::LearningStore store(dbPathB);
        if (!store.openReadOnly()) {
            return false;
        }
        const auto count = store.entryCount();
        return count.has_value() && *count == 0;
    }, std::chrono::seconds(5));
    assert(backupFinished.load());
    drainEvents();

    // Verify DB was cleared and final unified refresh completed
    assert(!gtk_widget_get_sensitive(clearBtnB));
    {
        modernime::core::LearningStore store(dbPathB);
        assert(store.openReadOnly());
        const auto count = store.entryCount();
        assert(count.has_value());
        assert(*count == 0);
    }

    modernime::settings::DataController::setIoHookForTesting(nullptr);
    pageB.reset();
    gtk_widget_destroy(winB);
    drainEvents();

    std::cout << "[Test] testLearningPageStaleRefreshAndClearReentry PASSED!" << std::endl;
}

// 9. Test LearningPage real production slow I/O with heartbeat < 100ms and safe lifecycle
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

    // 1. Hook real production learningEntryCount with 2s delay
    auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    modernime::settings::SettingsWindowModel model(tempDir / "settings.json");
    auto page = std::make_unique<modernime::settings::LearningPage>(
        model, storePath, []{}, [](std::string){});
    gtk_container_add(GTK_CONTAINER(window), page->widget());
    gtk_widget_show_all(window);
    drainEvents();

    std::atomic<bool> slowCountEntered{false};
    std::atomic<bool> slowCountFinished{false};
    modernime::settings::DataController::setIoHookForTesting([&](std::string_view op) {
        if (op == "learningEntryCount") {
            slowCountEntered = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            slowCountFinished = true;
        }
    });

    HeartbeatMonitor heartbeat;
    heartbeat.start();

    page->refresh(false);

    pumpEventsUntil([&] { return slowCountFinished.load(); }, std::chrono::seconds(5));
    assert(slowCountEntered.load());
    assert(slowCountFinished.load());

    heartbeat.stop();
    std::cout << "  Max heartbeat latency during 2s production learningEntryCount: "
              << heartbeat.maxLatency.count() << " ms (target < 100ms)"
              << " samples: " << heartbeat.sampleCount.load() << std::endl;
    assert(heartbeat.sampleCount.load() >= 10);
    assert(heartbeat.maxLatency < kMaxAllowedLatency);

    modernime::settings::DataController::setIoHookForTesting(nullptr);
    drainEvents();

    // 2. Hook and test safe window closing during in-flight learningEntryCount
    std::atomic<bool> countHookPaused{false};
    std::atomic<bool> releaseCountPause{false};
    std::atomic<bool> countHookCompleted{false};
    modernime::settings::DataController::setIoHookForTesting([&](std::string_view op) {
        if (op == "learningEntryCount") {
            countHookPaused = true;
            while (!releaseCountPause.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            countHookCompleted = true;
        }
    });

    page->refresh(false);
    pumpEventsUntil([&] { return countHookPaused.load(); }, std::chrono::seconds(5));
    assert(countHookPaused.load());

    // Immediately destroy LearningPage and window while async task is in flight
    page.reset();
    gtk_widget_destroy(window);

    // Release worker
    releaseCountPause = true;

    pumpEventsUntil([&] { return countHookCompleted.load(); }, std::chrono::seconds(5));
    assert(countHookCompleted.load());
    drainEvents();

    modernime::settings::DataController::setIoHookForTesting(nullptr);
    std::cout << "[Test] testLearningPageAsyncAndSlowIo PASSED!" << std::endl;
}

// 10. Test IoHook thread safety and concurrent reset during in-flight worker execution (S1 Regression)
void testIoHookLifetimeAndConcurrentReset(const std::filesystem::path &tempDir) {
    std::cout << "[Test] Running testIoHookLifetimeAndConcurrentReset..." << std::endl;
    const auto dictPath = tempDir / "hook_lifetime_test.txt";
    {
        std::ofstream out(dictPath);
        out << "nihao\t你好\t1.0\n";
    }

    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<bool> hookCompleted{false};

    modernime::settings::DataController::setIoHookForTesting(
        [payload = std::make_shared<int>(42), &entered, &release, &hookCompleted](std::string_view) {
            entered = true;
            while (!release.load()) {
                std::this_thread::yield();
            }
            volatile int observed = *payload;
            assert(observed == 42);
            hookCompleted = true;
        });

    std::thread worker([&] {
        modernime::settings::DataController::loadDictionary(dictPath);
    });

    while (!entered.load()) {
        std::this_thread::yield();
    }

    // Concurrent reset: while worker is blocked inside the hook, main thread resets the hook!
    modernime::settings::DataController::setIoHookForTesting(nullptr);

    // Release worker
    release = true;
    worker.join();

    assert(hookCompleted.load());
    std::cout << "[Test] testIoHookLifetimeAndConcurrentReset PASSED!" << std::endl;
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
        testDictionaryPageAsyncIoAndSlowIoSafety(tempDir);
        testSafeWindowClosingUnderSlowIo(tempDir);
        testBackupFailureDoesNotClearStore(tempDir);
        testContinuousSearch(tempDir);
        testLearningPageStaleRefreshAndClearReentry(tempDir);
        testLearningPageAsyncAndSlowIo(tempDir);
        testIoHookLifetimeAndConcurrentReset(tempDir);
        std::cout << "\nALL MODERNIME SETTINGS DATA ASYNC GTK TESTS PASSED!\n" << std::endl;
    } catch (const std::exception &e) {
        std::cerr << "FATAL TEST EXCEPTION: " << e.what() << std::endl;
        std::filesystem::remove_all(tempDir);
        return 1;
    }

    std::filesystem::remove_all(tempDir);
    return 0;
}
