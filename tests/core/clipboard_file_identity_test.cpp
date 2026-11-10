#include "modernime/core/persistent_clipboard_history.h"
#include "../support/clipboard_io_gate.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

using namespace modernime::core;
void require(bool ok, const char *message) {
    if (!ok) { std::cerr << "clipboard identity failed: " << message << '\n'; std::exit(1); }
}
int main(int argc, char **argv) {
    require(argc == 2, "scenario provided");
    char tmp[] = "/tmp/modernime-clipboard-identity-XXXXXX";
    require(mkdtemp(tmp) != nullptr, "private root created");
    const auto path = std::filesystem::path(tmp) / "history.bin";
    ClipboardHistory seed; seed.observe("old"); require(seed.save(path), "seed saved");
    PersistentClipboardHistory history(path);
    const std::string scenario = argv[1];
    if (scenario == "load") {
        modernime_clipboard_gate_arm(path.c_str(), clipboardAfterRead);
        bool loaded = false;
        std::thread reader([&] { loaded = history.load(); });
        const bool hit = modernime_clipboard_gate_wait(3000);
        seed.clear(); require(seed.save(path), "external clear after old read");
        modernime_clipboard_gate_release(); reader.join();
        require(hit && loaded, "read completion gate hit");
        history.reloadIfExternallyChanged();
        require(history.entries().empty(), "old read must not accept new file identity");
    } else if (scenario == "save") {
        require(history.load(), "history loaded");
        modernime_clipboard_gate_arm(path.c_str(), clipboardAfterUnlock);
        std::thread writer([&] { history.observe("new"); });
        const bool hit = modernime_clipboard_gate_wait(3000);
        // The gate is after releasing the actual cross-process file lock.
        seed.clear(); require(seed.save(path), "external clear after committed save");
        modernime_clipboard_gate_release(); writer.join();
        require(hit, "save unlock gate hit");
        history.reloadIfExternallyChanged();
        require(history.entries().empty(), "save result must not accept later clear identity");
    } else if (scenario == "retry") {
        require(history.load(), "history loaded");
        seed.observe("updated"); require(seed.save(path), "external replacement saved");
        modernime_clipboard_fail_reads(path.c_str(), true);
        std::string error;
        require(!history.reloadIfExternallyChanged(&error) && !error.empty(), "reload actually failed");
        modernime_clipboard_fail_reads(path.c_str(), false);
        require(history.reloadIfExternallyChanged(&error) && error.empty() && history.entries() == seed.entries(),
                "unchanged file identity retries after transient read failure");
    } else require(false, "known scenario");
    std::filesystem::remove_all(tmp);
    std::cout << "clipboard identity " << scenario << " passed\n";
}
