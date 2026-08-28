#include "modernime/core/clipboard_history.h"

#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "clipboard history test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    modernime::core::ClipboardHistory history;
    assertTrue(history.entries().empty(), "history starts empty");
    assertTrue(!history.observe(""), "empty clipboard values are ignored");

    assertTrue(history.observe("first"), "first value is recorded");
    assertTrue(history.observe("second"), "second value is recorded");
    assertTrue(history.entries().size() == 2 &&
                   history.entries()[0] == "second" &&
                   history.entries()[1] == "first",
               "new values are ordered newest first");
    assertTrue(!history.observe("second"),
               "observing the newest value does not change the order");

    modernime::core::ClipboardHistory editable;
    editable.observe("keep me");
    editable.observe("remove me");
    assertTrue(editable.remove(0), "history removes a selected entry");
    assertTrue(editable.entries().size() == 1 &&
                   editable.entries().front() == "keep me",
               "history removal updates the newest-first list");
    assertTrue(!editable.remove(1), "history rejects an out-of-range entry");
    editable.clear();
    assertTrue(editable.entries().empty(), "history can clear all entries");

    assertTrue(history.observe("first"),
               "reobserving an older value changes its recency");
    assertTrue(history.entries()[0] == "first" &&
                   history.entries()[1] == "second",
               "duplicate values are moved to the front");

    for (std::size_t index = 0;
         index < modernime::core::ClipboardHistory::kMaxEntries + 5; ++index) {
        history.observe("item-" + std::to_string(index));
    }
    assertTrue(history.entries().size() ==
                   modernime::core::ClipboardHistory::kMaxEntries,
               "history is bounded");
    assertTrue(history.entries().front() == "item-34",
               "latest item remains at the front");
    assertTrue(history.entries().back() == "item-5",
               "oldest item is evicted when the limit is reached");

    std::string oversized(
        modernime::core::ClipboardHistory::kMaxEntryBytes + 1, 'x');
    assertTrue(!history.observe(oversized),
               "oversized clipboard values are ignored safely");

    const auto directory = std::filesystem::temp_directory_path() /
                           "modernime-clipboard-history-test";
    std::error_code filesystemError;
    std::filesystem::remove_all(directory, filesystemError);
    std::filesystem::create_directories(directory, filesystemError);
    assertTrue(!filesystemError, "history test directory is created");
    const auto path = directory / "clipboard-history.bin";
    modernime::core::ClipboardHistory persisted;
    persisted.observe("line one\nline two");
    persisted.observe("second");
    std::string error;
    assertTrue(persisted.save(path, &error),
               "clipboard history saves: " + error);
    modernime::core::ClipboardHistory reloaded;
    assertTrue(reloaded.load(path, &error),
               "clipboard history loads: " + error);
    assertTrue(reloaded.entries().size() == 2 &&
                   reloaded.entries()[0] == "second" &&
                   reloaded.entries()[1] == "line one\nline two",
               "clipboard history preserves order and newlines");

    std::ofstream malformed(path, std::ios::binary | std::ios::trunc);
    malformed << "not a ModernIME clipboard history";
    malformed.close();
    error.clear();
    assertTrue(!reloaded.load(path, &error) && !error.empty(),
               "malformed clipboard history is rejected");

    history.clear();
    assertTrue(history.entries().empty(), "history can be cleared");
    return EXIT_SUCCESS;
}
