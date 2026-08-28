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

    const auto missingPath = directory / "missing.bin";
    modernime::settings::ClipboardHistoryModel missing(missingPath);
    assertTrue(missing.reload(&error) && missing.entries().empty(),
               "missing history is shown as an empty list");
    return EXIT_SUCCESS;
}
