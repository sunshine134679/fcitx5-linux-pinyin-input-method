#include "modernime/core/component_lookup.h"

#include <algorithm>
#include <fstream>
#include <system_error>
#include <utility>

namespace modernime::core {
namespace {
constexpr std::string_view kHeader = "# ModernIME component lookup v1";

std::string normalizeQuery(std::string_view query) {
    std::string key;
    key.reserve(query.size());
    for (const auto ch : query) {
        // Accept compact components and familiar pinyin separators alike.
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
            ch == '\'' || ch == '+') {
            continue;
        }
        key.push_back(ch >= 'A' && ch <= 'Z' ?
                          static_cast<char>(ch - 'A' + 'a') : ch);
    }
    return key;
}

#if defined(MODERNIME_COMPONENT_LOOKUP_PATH) || defined(MODERNIME_COMPONENT_LOOKUP_SOURCE_PATH)
bool regularFile(const std::filesystem::path &path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}
#endif
} // namespace

ComponentLookup::ComponentLookup(std::filesystem::path path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        loadError_ = "拆字数据不可读：" + path.string();
        return;
    }
    std::string line;
    if (!std::getline(input, line) || line != kHeader) {
        loadError_ = "拆字数据格式不匹配：" + path.string();
        return;
    }
    std::size_t lineNumber = 1;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto first = line.find('\t');
        const auto second = first == std::string::npos ? first :
                                                          line.find('\t', first + 1);
        if (first == std::string::npos || second == std::string::npos ||
            first == 0 || second == first + 1 || second + 1 == line.size() ||
            line.find('\t', second + 1) != std::string::npos) {
            loadError_ = "拆字数据行无效：" + path.string() + ":" +
                         std::to_string(lineNumber);
            index_.clear();
            return;
        }
        auto &candidates = index_[line.substr(0, first)];
        auto text = line.substr(first + 1, second - first - 1);
        if (std::none_of(candidates.begin(), candidates.end(),
                         [&text](const auto &candidate) {
            return candidate.text == text;
        })) {
            candidates.push_back({std::move(text), line.substr(second + 1)});
        }
    }
    if (input.bad() || index_.empty()) {
        loadError_ = "拆字数据未完整读取或为空：" + path.string();
        index_.clear();
    }
}

std::filesystem::path ComponentLookup::defaultPath() {
#ifdef MODERNIME_COMPONENT_LOOKUP_PATH
    const std::filesystem::path installed(MODERNIME_COMPONENT_LOOKUP_PATH);
    if (regularFile(installed)) {
        return installed;
    }
#endif
#ifdef MODERNIME_COMPONENT_LOOKUP_SOURCE_PATH
    const std::filesystem::path source(MODERNIME_COMPONENT_LOOKUP_SOURCE_PATH);
    if (regularFile(source)) {
        return source;
    }
#endif
#ifdef MODERNIME_COMPONENT_LOOKUP_PATH
    // Keep the expected installation path in the diagnostic when missing.
    return installed;
#else
    return "data/components/component_lookup.tsv";
#endif
}

std::vector<ComponentCandidate> ComponentLookup::lookup(std::string_view query,
                                                       std::size_t limit) const {
    if (limit == 0) {
        return {};
    }
    const auto found = index_.find(normalizeQuery(query));
    if (found == index_.end()) {
        return {};
    }
    const auto size = std::min(limit, found->second.size());
    return {found->second.begin(), found->second.begin() + size};
}

bool ComponentLookup::available() const noexcept {
    return loadError_.empty() && !index_.empty();
}

const std::string &ComponentLookup::loadError() const noexcept {
    return loadError_;
}
} // namespace modernime::core
