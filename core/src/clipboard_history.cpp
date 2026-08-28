#include "modernime/core/clipboard_history.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <utility>

namespace modernime::core {
namespace {

constexpr std::string_view fileMagic = "ModernIME Clipboard History\n";

void setError(std::string *error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

bool writeUint64(std::ostream &output, std::uint64_t value) {
    std::array<char, sizeof(value)> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<char>(value >> (index * 8));
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

bool readUint64(std::istream &input, std::uint64_t &value) {
    std::array<char, sizeof(value)> bytes{};
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        return false;
    }
    value = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        value |= static_cast<std::uint64_t>(
                     static_cast<unsigned char>(bytes[index]))
                 << (index * 8);
    }
    return true;
}

} // namespace

bool ClipboardHistory::observe(std::string_view text) {
    if (text.empty() || text.size() > kMaxEntryBytes) {
        return false;
    }

    const auto existing = std::find(entries_.begin(), entries_.end(), text);
    if (existing != entries_.end()) {
        if (existing == entries_.begin()) {
            return false;
        }
        std::string value = std::move(*existing);
        entries_.erase(existing);
        entries_.insert(entries_.begin(), std::move(value));
        return true;
    }

    entries_.insert(entries_.begin(), std::string(text));
    if (entries_.size() > kMaxEntries) {
        entries_.pop_back();
    }
    return true;
}

bool ClipboardHistory::load(const std::filesystem::path &path,
                            std::string *error) {
    entries_.clear();
    if (error != nullptr) {
        error->clear();
    }
    if (path.empty()) {
        setError(error, "clipboard history path is empty");
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::error_code filesystemError;
        if (!std::filesystem::exists(path, filesystemError) &&
            !filesystemError) {
            return true;
        }
        setError(error, "unable to open clipboard history");
        return false;
    }

    std::array<char, fileMagic.size()> magic{};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!input || !std::equal(magic.begin(), magic.end(), fileMagic.begin())) {
        setError(error, "invalid clipboard history header");
        return false;
    }

    std::uint64_t count = 0;
    if (!readUint64(input, count) || count > kMaxEntries) {
        setError(error, "invalid clipboard history entry count");
        return false;
    }

    std::vector<std::string> loaded;
    loaded.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0; index < count; ++index) {
        std::uint64_t length = 0;
        if (!readUint64(input, length) || length == 0 ||
            length > kMaxEntryBytes ||
            length > std::numeric_limits<std::size_t>::max()) {
            setError(error, "invalid clipboard history entry size");
            return false;
        }
        std::string value(static_cast<std::size_t>(length), '\0');
        input.read(value.data(), static_cast<std::streamsize>(value.size()));
        if (!input) {
            setError(error, "truncated clipboard history entry");
            return false;
        }
        loaded.push_back(std::move(value));
    }

    input.peek();
    if (!input.eof()) {
        setError(error, "unexpected clipboard history data");
        return false;
    }
    entries_ = std::move(loaded);
    return true;
}

bool ClipboardHistory::save(const std::filesystem::path &path,
                            std::string *error) const {
    if (error != nullptr) {
        error->clear();
    }
    if (path.empty()) {
        setError(error, "clipboard history path is empty");
        return false;
    }

    std::error_code filesystemError;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(),
                                            filesystemError);
        if (filesystemError) {
            setError(error, "unable to create clipboard history directory: " +
                               filesystemError.message());
            return false;
        }
    }

    auto temporary = path;
    temporary += ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        setError(error, "unable to open temporary clipboard history");
        return false;
    }
    output.write(fileMagic.data(), static_cast<std::streamsize>(fileMagic.size()));
    const auto count = std::min<std::size_t>(entries_.size(), kMaxEntries);
    if (!output || !writeUint64(output, count)) {
        output.close();
        std::filesystem::remove(temporary, filesystemError);
        setError(error, "unable to write clipboard history header");
        return false;
    }
    for (std::size_t index = 0; index < count; ++index) {
        const auto &entry = entries_[index];
        if (entry.empty() || entry.size() > kMaxEntryBytes ||
            !writeUint64(output, entry.size())) {
            output.close();
            std::filesystem::remove(temporary, filesystemError);
            setError(error, "invalid clipboard history entry");
            return false;
        }
        output.write(entry.data(), static_cast<std::streamsize>(entry.size()));
        if (!output) {
            output.close();
            std::filesystem::remove(temporary, filesystemError);
            setError(error, "unable to write clipboard history entry");
            return false;
        }
    }
    output.close();
    if (!output) {
        std::filesystem::remove(temporary, filesystemError);
        setError(error, "unable to finish clipboard history");
        return false;
    }

    std::filesystem::rename(temporary, path, filesystemError);
    if (filesystemError) {
        std::filesystem::remove(temporary, filesystemError);
        setError(error, "unable to replace clipboard history: " +
                           filesystemError.message());
        return false;
    }
    std::filesystem::permissions(
        path, std::filesystem::perms::owner_read |
                  std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, filesystemError);
    if (filesystemError) {
        setError(error, "unable to protect clipboard history: " +
                           filesystemError.message());
        return false;
    }
    return true;
}

void ClipboardHistory::clear() { entries_.clear(); }

} // namespace modernime::core
