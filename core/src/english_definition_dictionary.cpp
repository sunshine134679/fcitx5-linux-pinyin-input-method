#include "modernime/core/english_definition_dictionary.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace modernime::core {
namespace {

#pragma pack(push, 1)
struct RawIndexEntry final {
    std::uint32_t wordOffset;
    std::uint16_t wordLen;
    std::uint16_t defLen;
    std::uint32_t defOffset;
};
#pragma pack(pop)

static_assert(sizeof(RawIndexEntry) == 12, "RawIndexEntry must be 12 bytes");

constexpr char kMagic[8] = {'M', 'O', 'D', 'E', 'D', 'I', 'C', '1'};

std::size_t utf8CodePointCount(std::string_view text) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if ((byte & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

std::size_t utf8ByteLengthForCodePoints(std::string_view text, std::size_t maxPoints) {
    std::size_t points = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if ((byte & 0xC0) != 0x80) {
            if (points == maxPoints) {
                return i;
            }
            ++points;
        }
    }
    return text.size();
}

bool containsChineseCharacter(std::string_view text) {
    for (std::size_t i = 0; i + 2 < text.size(); ++i) {
        const auto b0 = static_cast<unsigned char>(text[i]);
        const auto b1 = static_cast<unsigned char>(text[i + 1]);
        const auto b2 = static_cast<unsigned char>(text[i + 2]);
        // Common CJK Unified Ideographs in UTF-8: E4 B8 80 to E9 BE BF
        if (b0 >= 0xE4 && b0 <= 0xE9 && (b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80) {
            return true;
        }
    }
    return false;
}

} // namespace

class EnglishDefinitionDictionary::Impl final {
public:
    Impl() = default;
    ~Impl() { close(); }

    Impl(const Impl &) = delete;
    Impl &operator=(const Impl &) = delete;

    bool load(const std::filesystem::path &path) {
        close();
        if (path.empty()) {
            return false;
        }
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            return false;
        }

        struct stat st;
        if (::fstat(fd, &st) != 0 || st.st_size < 16) {
            ::close(fd);
            return false;
        }

        const auto fileSize = static_cast<std::size_t>(st.st_size);
        void *mapped = ::mmap(nullptr, fileSize, PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);

        if (mapped == MAP_FAILED || mapped == nullptr) {
            return false;
        }

        data_ = static_cast<const std::uint8_t *>(mapped);
        size_ = fileSize;

        if (std::memcmp(data_, kMagic, 8) != 0) {
            close();
            return false;
        }

        std::uint32_t count = 0;
        std::uint32_t stringOffset = 0;
        std::memcpy(&count, data_ + 8, 4);
        std::memcpy(&stringOffset, data_ + 12, 4);

        if (stringOffset > size_ || stringOffset < 16 + count * sizeof(RawIndexEntry)) {
            close();
            return false;
        }

        entryCount_ = count;
        stringPool_ = reinterpret_cast<const char *>(data_ + stringOffset);
        stringPoolSize_ = size_ - stringOffset;
        entries_ = reinterpret_cast<const RawIndexEntry *>(data_ + 16);

        return true;
    }

    void close() noexcept {
        if (data_ != nullptr && size_ > 0) {
            ::munmap(const_cast<std::uint8_t *>(data_), size_);
        }
        data_ = nullptr;
        size_ = 0;
        entryCount_ = 0;
        entries_ = nullptr;
        stringPool_ = nullptr;
        stringPoolSize_ = 0;
    }

    bool isLoaded() const noexcept { return entries_ != nullptr; }

    std::size_t entryCount() const noexcept { return entryCount_; }

    std::string_view lookup(std::string_view word) const noexcept {
        if (!isLoaded() || word.empty()) {
            return {};
        }

        // Small stack buffer for lowercase folding to avoid heap allocation
        char stackBuf[128];
        std::string_view lowerWord;
        std::string heapLower;
        if (word.size() < sizeof(stackBuf)) {
            for (std::size_t i = 0; i < word.size(); ++i) {
                stackBuf[i] = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(word[i])));
            }
            lowerWord = std::string_view(stackBuf, word.size());
        } else {
            heapLower.reserve(word.size());
            for (char c : word) {
                heapLower.push_back(static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c))));
            }
            lowerWord = heapLower;
        }

        const auto *it = std::lower_bound(
            entries_, entries_ + entryCount_, lowerWord,
            [this](const RawIndexEntry &entry, std::string_view target) {
                if (entry.wordOffset + entry.wordLen > stringPoolSize_) {
                    return false;
                }
                const std::string_view entryWord(stringPool_ + entry.wordOffset,
                                                 entry.wordLen);
                return entryWord < target;
            });

        if (it != entries_ + entryCount_) {
            if (it->wordOffset + it->wordLen <= stringPoolSize_ &&
                it->defOffset + it->defLen <= stringPoolSize_) {
                const std::string_view entryWord(stringPool_ + it->wordOffset,
                                                 it->wordLen);
                if (entryWord == lowerWord) {
                    return std::string_view(stringPool_ + it->defOffset,
                                            it->defLen);
                }
            }
        }
        return {};
    }

private:
    const std::uint8_t *data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t entryCount_ = 0;
    const RawIndexEntry *entries_ = nullptr;
    const char *stringPool_ = nullptr;
    std::size_t stringPoolSize_ = 0;
};

EnglishDefinitionDictionary::EnglishDefinitionDictionary() noexcept
    : impl_(std::make_unique<Impl>()) {}

EnglishDefinitionDictionary::~EnglishDefinitionDictionary() = default;

EnglishDefinitionDictionary::EnglishDefinitionDictionary(
    EnglishDefinitionDictionary &&) noexcept = default;

EnglishDefinitionDictionary &EnglishDefinitionDictionary::operator=(
    EnglishDefinitionDictionary &&) noexcept = default;

bool EnglishDefinitionDictionary::load(const std::filesystem::path &path) {
    return impl_->load(path);
}

void EnglishDefinitionDictionary::close() noexcept { impl_->close(); }

bool EnglishDefinitionDictionary::isLoaded() const noexcept {
    return impl_->isLoaded();
}

std::size_t EnglishDefinitionDictionary::entryCount() const noexcept {
    return impl_->entryCount();
}

std::string_view EnglishDefinitionDictionary::lookup(
    std::string_view word) const noexcept {
    return impl_->lookup(word);
}

std::string EnglishDefinitionDictionary::cleanDefinition(
    std::string_view rawTranslation, std::size_t maxChars) {
    if (rawTranslation.empty()) {
        return {};
    }

    std::vector<std::string> meanings;

    std::size_t start = 0;
    while (start < rawTranslation.size()) {
        auto nextNl = rawTranslation.find('\n', start);
        if (nextNl == std::string_view::npos) {
            nextNl = rawTranslation.size();
        }
        auto line = rawTranslation.substr(start, nextNl - start);
        start = nextNl + 1;

        if (line.size() >= 2 && line.substr(0, 2) == "\\n") {
            line = line.substr(2);
        }

        // Strip leading whitespace
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t' ||
                                 line.front() == '\r')) {
            line.remove_prefix(1);
        }

        // Strip POS prefixes like "n. ", "vt. ", "[计] "
        while (!line.empty()) {
            if (line.front() == '[') {
                auto closeBracket = line.find(']');
                if (closeBracket != std::string_view::npos) {
                    line.remove_prefix(closeBracket + 1);
                    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
                        line.remove_prefix(1);
                    }
                    continue;
                }
            }
            // Check for lowercase POS letters followed by dot: e.g. "n. ", "vt. "
            std::size_t dotPos = 0;
            while (dotPos < line.size() && std::isalpha(static_cast<unsigned char>(line[dotPos]))) {
                ++dotPos;
            }
            if (dotPos > 0 && dotPos < line.size() && line[dotPos] == '.') {
                line.remove_prefix(dotPos + 1);
                while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
                    line.remove_prefix(1);
                }
                continue;
            }
            break;
        }

        if (line.empty()) {
            continue;
        }

        // Split line by comma or semicolon safely
        std::size_t itemStart = 0;
        std::size_t pos = 0;
        while (pos <= line.size()) {
            bool isDelim = false;
            std::size_t delimLen = 0;
            if (pos == line.size()) {
                isDelim = true;
                delimLen = 0;
            } else if (line[pos] == ',' || line[pos] == ';') {
                isDelim = true;
                delimLen = 1;
            } else if (pos + 2 < line.size() &&
                       static_cast<unsigned char>(line[pos]) == 0xEF &&
                       static_cast<unsigned char>(line[pos + 1]) == 0xBC &&
                       (static_cast<unsigned char>(line[pos + 2]) == 0x8C ||
                        static_cast<unsigned char>(line[pos + 2]) == 0x9B)) {
                isDelim = true;
                delimLen = 3;
            }

            if (isDelim) {
                if (pos > itemStart) {
                    auto part = line.substr(itemStart, pos - itemStart);
                    // Remove brackets from part
                    std::string cleanedPart;
                    cleanedPart.reserve(part.size());
                    bool inParen = false;
                    for (std::size_t k = 0; k < part.size(); ++k) {
                        char ch = part[k];
                        if (ch == '(' || ch == '[') {
                            inParen = true;
                        } else if (ch == ')' || ch == ']') {
                            inParen = false;
                        } else if (!inParen) {
                            cleanedPart.push_back(ch);
                        }
                    }

                    // Trim cleanedPart
                    auto first = cleanedPart.find_first_not_of(" \t\r\n.,-");
                    if (first != std::string::npos) {
                        auto last = cleanedPart.find_last_not_of(" \t\r\n.,-");
                        std::string item = cleanedPart.substr(first, last - first + 1);

                        if (containsChineseCharacter(item)) {
                            if (std::find(meanings.begin(), meanings.end(), item) == meanings.end()) {
                                meanings.push_back(std::move(item));
                                if (meanings.size() >= 2) {
                                    break;
                                }
                            }
                        }
                    }
                }
                if (pos == line.size()) {
                    break;
                }
                pos += delimLen;
                itemStart = pos;
            } else {
                ++pos;
            }
        }
        if (meanings.size() >= 2) {
            break;
        }
    }

    if (meanings.empty()) {
        return {};
    }

    std::string result;
    if (meanings.size() == 1) {
        result = meanings[0];
    } else {
        result = meanings[0] + "；" + meanings[1];
    }

    if (utf8CodePointCount(result) > maxChars) {
        if (meanings.size() > 1 && utf8CodePointCount(meanings[0]) <= maxChars) {
            result = meanings[0];
        } else {
            auto cutLen = utf8ByteLengthForCodePoints(result, maxChars);
            result = result.substr(0, cutLen);
        }
    }

    return result;
}

} // namespace modernime::core
