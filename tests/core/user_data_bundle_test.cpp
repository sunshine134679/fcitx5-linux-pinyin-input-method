#include "modernime/core/user_data_bundle.h"
#include "modernime/core/clipboard_history.h"
#include "modernime/core/learning_writer.h"
#include "modernime/core/settings.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sqlite3.h>
#include <unistd.h>

using namespace modernime::core;
namespace {
void require(bool value, std::string_view message) {
    if (!value) { std::cerr << "user data bundle: " << message << '\n'; std::exit(1); }
}
void write(const std::filesystem::path &path, std::string_view text) {
    std::ofstream output(path, std::ios::binary); output << text;
    require(bool(output), "fixture write");
}
std::string read(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
struct Fixture {
    std::filesystem::path root;
    UserDataPaths paths;
    Fixture() {
        std::string pattern = (std::filesystem::temp_directory_path() / "modernime-bundle-XXXXXX").string();
        auto *created = ::mkdtemp(pattern.data()); require(created != nullptr, "unique temporary directory");
        root = created;
        paths = {root/"settings.conf", root/"dictionary.txt", root/"quick.mb", root/"clipboard.bin", root/"favorites.bin"};
        require(SettingsStore::save(paths.settings, defaultSettings()), "settings fixture");
        write(paths.userDictionary, "ni'hao\t你好\t88\n");
        write(paths.quickPhrases, "yx\t\"你好 世界\"\n");
        ClipboardHistory history; history.observe("synthetic clipboard secret");
        require(history.save(paths.clipboardHistory), "history fixture");
        require(history.save(paths.clipboardFavorites), "favorites fixture");
    }
    ~Fixture() { std::error_code ec; std::filesystem::remove_all(root, ec); }
};

// Catches default clipboard leakage, loss of WAL/LMDB committed learning,
// partial-selection restoration and loss of suppression/contexts/frequencies.
void roundTrip(const char *backend) {
    Fixture f;
    LearningWriter writer(f.root / backend);
    require(writer.enqueueSelection("你好", "nihao", "之前", "之后", 123456), "queue learning");
    require(writer.enqueueSuppression("泥", "ni"), "queue suppression");
    UserDataBundle bundle(f.paths, &writer);
    std::string error;
    const auto directory = f.root / "backup";
    require(bundle.exportTo(directory, {}, &error), "default export: " + error);
    require(!std::filesystem::exists(directory/"clipboard-history.bin") &&
            !std::filesystem::exists(directory/"clipboard-favorites.bin"), "clipboard excluded by default");
    require(read(directory/"manifest.txt").find("clipboard-history\texcluded") != std::string::npos,
            "manifest records privacy exclusion");
    write(f.paths.userDictionary, "xin\t新\t1\n");
    write(f.paths.quickPhrases, "x\tnew\n");
    auto changedSettings = defaultSettings(); changedSettings.candidateFontSize = 14;
    require(SettingsStore::save(f.paths.settings, changedSettings), "change settings");
    require(writer.enqueueSelection("新词", "xinci", {}, {}, 222222) && writer.flush(), "later learning");
    const auto priorRevision = writer.resetRevision();
    const auto preview = bundle.preview(directory, &error);
    require(preview.has_value(), "preview: " + error);
    require(preview->differences.size() == 6 && preview->differences[1].changed &&
            preview->differences[3].bundleEntries == 2, "preview reflects real differences");
    UserDataSelection selected; selected.quickPhrases = false;
    require(bundle.restore(directory, selected, *preview, &error), "selective restore: " + error);
    require(read(f.paths.userDictionary) == "ni'hao\t你好\t88\n", "dictionary restored");
    require(read(f.paths.quickPhrases) == "x\tnew\n", "unselected phrases preserved");
    require(SettingsStore::load(f.paths.settings).settings.candidateFontSize == 20, "settings restored");
    auto snapshot = writer.snapshot();
    const auto *entry = snapshot->entry("你好", "nihao", "之前", "之后");
    require(entry && entry->frequency == 1 && entry->lastSelectedMs == 123456, "learning full fields restored");
    require(snapshot->isSuppressed("泥", "ni") && !snapshot->hasPositiveFrequency("新词", "xinci"), "suppression restored and later entries replaced");
    require(writer.resetRevision() > priorRevision, "restore advances reset generation");
    UserDataSelection phrasesOnly{false,false,true,false,false,false};
    require(bundle.restore(directory, phrasesOnly, *preview, &error), "selected phrase restore: " + error);
    require(read(f.paths.quickPhrases) == "yx\t\"你好 世界\"\n", "phrase bytes preserved");
    require(bundle.preview(directory).has_value(), "restored package stays readable");
}

// Catches overwriting post-preview changes, trusting corrupt content and
// following a redirected payload or destination path.
void rejectsChangedAndUnsafeData() {
    Fixture f;
    UserDataSelection selected; selected.learning = false;
    UserDataBundle bundle(f.paths);
    std::string error;
    const auto directory = f.root/"backup";
    require(bundle.exportTo(directory, selected, &error), "export files: " + error);
    auto preview = bundle.preview(directory, &error);
    require(preview.has_value(), "preview files");
    write(f.paths.userDictionary, "hou\t后\t2\n");
    require(!bundle.restore(directory, selected, *preview, &error), "stale preview rejected");
    require(read(f.paths.userDictionary) == "hou\t后\t2\n", "stale target preserved");
    write(directory/"dictionary.txt", "broken");
    require(!bundle.preview(directory, &error), "corrupt payload rejected");
    require(!bundle.restore(directory, selected, *preview, &error), "corrupt restore rejected");
    std::filesystem::remove(directory/"dictionary.txt");
    std::filesystem::create_symlink(f.paths.userDictionary, directory/"dictionary.txt");
    require(!bundle.preview(directory, &error), "symlink payload rejected");
    std::filesystem::create_directory_symlink(f.root, f.root/"redirect");
    auto redirected = f.paths; redirected.userDictionary = f.root/"redirect/dictionary.txt";
    require(!UserDataBundle(redirected).exportTo(f.root/"unsafe", selected, &error), "symlink ancestor rejected");
}

void clipboardRequiresExplicitSelection() {
    Fixture f; UserDataSelection selected; selected.learning = false;
    selected.clipboardHistory = true; selected.clipboardFavorites = true;
    UserDataBundle bundle(f.paths); std::string error;
    require(bundle.exportTo(f.root/"backup", selected, &error), "explicit clipboard export: " + error);
    ClipboardHistory history; history.observe("later clipboard");
    require(history.save(f.paths.clipboardHistory), "change clipboard");
    require(history.save(f.paths.clipboardFavorites), "change favorites");
    auto preview = bundle.preview(f.root/"backup", &error);
    require(preview.has_value(), "explicit clipboard preview");
    UserDataSelection historyOnly{false,false,false,false,true,true};
    require(bundle.restore(f.root/"backup", historyOnly, *preview, &error), "clipboard selected restore: " + error);
    require(history.load(f.paths.clipboardHistory) && history.entries().size()==1 &&
            history.entries()[0] == "synthetic clipboard secret", "clipboard content restored");
    require(history.load(f.paths.clipboardFavorites) && history.entries()[0] == "synthetic clipboard secret", "favorites restored independently");
}

// A real SQLite reserved writer lock lets preview read WAL data but makes the
// final restore transaction fail, after file replacement has started.
void learningFailureRollsBackFiles() {
    Fixture f; LearningWriter writer(f.root/"learning.sqlite3");
    require(writer.enqueueSelection("你好", "nihao", {}, {}, 123) && writer.flush(), "failure fixture learning");
    UserDataBundle bundle(f.paths, &writer); std::string error;
    require(bundle.exportTo(f.root/"backup", {}, &error), "failure fixture backup: " + error);
    write(f.paths.userDictionary, "hou\t后\t2\n");
    auto preview = bundle.preview(f.root/"backup", &error);
    require(preview.has_value(), "failure fixture preview");
    sqlite3 *database = nullptr;
    require(sqlite3_open((f.root/"learning.sqlite3").c_str(), &database) == SQLITE_OK, "open competing writer");
    require(sqlite3_exec(database, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK, "hold competing writer lock");
    require(!bundle.restore(f.root/"backup", {}, *preview, &error), "busy learning restore fails");
    require(read(f.paths.userDictionary) == "hou\t后\t2\n", "replaced dictionary rolled back");
    require(writer.snapshot()->entry("你好", "nihao", {}, {})->frequency == 1, "learning failure preserves data");
    sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr); sqlite3_close(database);
    for (const auto &entry : std::filesystem::directory_iterator(f.root))
        require(!entry.path().filename().string().starts_with(".modernime-bundle-"), "successful rollback cleans its temporary files");
}
}
int main() {
    roundTrip("learning.sqlite3"); roundTrip("learning.lmdb");
    rejectsChangedAndUnsafeData(); clipboardRequiresExplicitSelection();
    learningFailureRollsBackFiles();
    std::cout << "user data bundle tests passed\n";
}
