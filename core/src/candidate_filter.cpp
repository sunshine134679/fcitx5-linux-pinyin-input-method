#include "modernime/core/candidate_filter.h"
#include "modernime/core/component_lookup.h"

#include <algorithm>
#include <fstream>
#include <memory>
#include <sstream>

namespace modernime::core {
namespace {
std::string characterAt(std::string_view text, std::size_t offset) {
    std::size_t start = 0;
    for (std::size_t i = 0; start < text.size(); ++i) {
        std::size_t end = start + 1;
        while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xc0U) == 0x80U) ++end;
        if (i == offset) return std::string(text.substr(start, end - start));
        start = end;
    }
    return {};
}
} // namespace

CandidateFilterIndex::CandidateFilterIndex(std::filesystem::path components,
                                         std::filesystem::path strokes) {
    if (components.empty()) components = ComponentLookup::defaultPath();
    if (strokes.empty()) strokes = defaultStrokePath();
    std::ifstream componentFile(components);
    std::string line;
    if (!std::getline(componentFile, line) || line != "# ModernIME component lookup v1") {
        error_ = "部件筛选数据不可用";
    } else {
        while (std::getline(componentFile, line)) {
            if (line.empty() || line[0] == '#') continue;
            const auto split = line.find('\t');
            const auto end = split == std::string::npos ? split : line.find('\t', split + 1);
            if (split == std::string::npos || end == std::string::npos) { components_.clear(); break; }
            const auto key = line.substr(0, split);
            if (key.find_first_not_of("abcdefghijklmnopqrstuvwxyz") == std::string::npos)
                components_[key].insert(line.substr(split + 1, end - split - 1));
        }
        if (componentFile.bad()) components_.clear();
    }
    std::ifstream strokeFile(strokes);
    while (std::getline(strokeFile, line)) {
        std::istringstream fields(line);
        std::string sequence, character, extra;
        if (!(fields >> sequence >> character) || fields >> extra ||
            sequence.find_first_not_of("12345") != std::string::npos ||
            character.empty() || characterAt(character, 0) != character) continue;
        strokes_[character].push_back(std::move(sequence));
    }
    if (strokeFile.bad()) strokes_.clear();
    if (components_.empty() || strokes_.empty()) error_ = "部件或笔画筛选数据不可用";
}

bool CandidateFilterIndex::available() const noexcept {
    return !components_.empty() && !strokes_.empty();
}

std::filesystem::path CandidateFilterIndex::defaultStrokePath() {
#ifdef MODERNIME_STROKE_LOOKUP_PATH
    const std::filesystem::path installed(MODERNIME_STROKE_LOOKUP_PATH);
    if (std::filesystem::is_regular_file(installed)) return installed;
#endif
#ifdef MODERNIME_STROKE_LOOKUP_SOURCE_PATH
    return MODERNIME_STROKE_LOOKUP_SOURCE_PATH;
#else
    return "data/components/stroke_lookup.txt";
#endif
}

std::optional<std::function<bool(const CandidateItem &)>> CandidateFilterIndex::predicate(
    std::string_view expression, std::size_t characterOffset) const {
    if (expression.empty() || expression.size() > 64 || characterOffset > 31) return std::nullopt;
    std::string key(expression);
    for (auto &ch : key) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    bool stroke = key.starts_with("s:");
    const bool component = key.starts_with("c:");
    if (stroke || component) key.erase(0, 2);
    else stroke = key.find_first_not_of("hspnz12345") == std::string::npos;
    if (key.empty()) return std::nullopt;
    auto allowed = std::make_shared<std::unordered_set<std::string>>();
    if (stroke) {
        if (strokes_.empty()) return std::nullopt;
        for (auto &ch : key) {
            const auto pos = std::string_view("hspnz").find(ch);
            if (pos != std::string_view::npos) ch = static_cast<char>('1' + pos);
            else if (ch < '1' || ch > '5') return std::nullopt;
        }
        for (const auto &[character, sequences] : strokes_) {
            if (std::any_of(sequences.begin(), sequences.end(), [&key](const auto &s) { return s.starts_with(key); }))
                allowed->insert(character);
        }
    } else {
        if (components_.empty() || key.find_first_not_of("abcdefghijklmnopqrstuvwxyz") != std::string::npos)
            return std::nullopt;
        for (auto it = components_.lower_bound(key); it != components_.end() && it->first.starts_with(key); ++it)
            allowed->insert(it->second.begin(), it->second.end());
    }
    return [allowed = std::move(allowed), characterOffset](const CandidateItem &item) {
        return item.source != CandidateSource::None && allowed->contains(characterAt(item.text, characterOffset));
    };
}
} // namespace modernime::core
