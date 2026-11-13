#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace modernime::core {

class LearningWriter;

enum class UserDataComponent {
    Settings, UserDictionary, QuickPhrases, Learning,
    ClipboardHistory, ClipboardFavorites
};

struct UserDataSelection final {
    bool settings = true;
    bool userDictionary = true;
    bool quickPhrases = true;
    bool learning = true;
    bool clipboardHistory = false;
    bool clipboardFavorites = false;
    bool includes(UserDataComponent component) const;
};

struct UserDataPaths final {
    std::filesystem::path settings;
    std::filesystem::path userDictionary;
    std::filesystem::path quickPhrases;
    std::filesystem::path clipboardHistory;
    std::filesystem::path clipboardFavorites;
};

struct UserDataDifference final {
    UserDataComponent component = UserDataComponent::Settings;
    bool included = false;
    bool present = false;
    bool currentPresent = false;
    bool changed = false;
    std::size_t bundleEntries = 0;
    std::size_t currentEntries = 0;
    std::string bundleFingerprint;
    std::string currentFingerprint;
};

struct UserDataPreview final {
    int version = 1;
    std::string bundleFingerprint;
    std::vector<UserDataDifference> differences;
};

// Blocking worker-side API. Learning must be the existing shared Writer, never
// another writer opened for the same active database. Restore holds the stable
// per-file writer locks through validation, replacement and rollback. The
// consumer pauses file reloads around restore to avoid observing half a batch.
// The directory package is a trusted local backup, not an external dictionary
// import; FNV-1a checks detect corruption and do not authenticate its origin.
// Missing source files are recorded and skipped on restore; no implicit delete.
class UserDataBundle final {
public:
    explicit UserDataBundle(UserDataPaths paths, LearningWriter *learning = nullptr);
    bool exportTo(const std::filesystem::path &directory,
                  UserDataSelection selection = {}, std::string *error = nullptr) const;
    std::optional<UserDataPreview> preview(const std::filesystem::path &directory,
                                         std::string *error = nullptr) const;
    // Rechecks the package and selected current data against the supplied
    // preview. Replaces selected present components, preserving all others.
    // Stages old files before any replacement; learning commits last through
    // Writer's reset-generation-aware transaction. Ordinary failures roll back
    // replaced files. A process crash is not a cross-file atomic transaction.
    bool restore(const std::filesystem::path &directory,
                 UserDataSelection selection, const UserDataPreview &expected,
                 std::string *error = nullptr) const;

private:
    UserDataPaths paths_;
    LearningWriter *learning_;
};

std::string_view userDataComponentName(UserDataComponent component);

} // namespace modernime::core
