#include "modernime/core/clipboard_history.h"

#include <cstdlib>
#include <iostream>
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

    history.clear();
    assertTrue(history.entries().empty(), "history can be cleared");
    return EXIT_SUCCESS;
}
