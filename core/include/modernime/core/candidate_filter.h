#pragma once

#include "modernime/core/candidate_model.h"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace modernime::core {

// Construct during resource preheating. Predicates own their immutable evidence;
// filtering never reads disk or changes a candidate's identity or rank.
class CandidateFilterIndex final {
public:
    CandidateFilterIndex(std::filesystem::path components = {},
                         std::filesystem::path strokes = {});
    bool available() const noexcept;
    const std::string &error() const noexcept { return error_; }
    // s:hspnz (or bare h/s/p/n/z) filters by stroke prefix;
    // c:mumu (or another component pinyin prefix) filters by decomposition.
    // Character offset is measured in Unicode codepoints, not UTF-8 bytes.
    std::optional<std::function<bool(const CandidateItem &)>> predicate(
        std::string_view expression, std::size_t characterOffset = 0) const;
    static std::filesystem::path defaultStrokePath();
private:
    std::map<std::string, std::unordered_set<std::string>> components_;
    std::unordered_map<std::string, std::vector<std::string>> strokes_;
    std::string error_;
};

} // namespace modernime::core
