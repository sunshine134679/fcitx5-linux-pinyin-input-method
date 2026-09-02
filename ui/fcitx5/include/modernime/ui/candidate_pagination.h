#pragma once

#include "modernime/core/candidate_model.h"

#include <functional>
#include <string_view>
#include <vector>

namespace modernime::ui {

struct CandidateBarMetrics;

class CandidatePagination final {
public:
    static std::vector<core::PageBoundary> partition(
        const std::vector<core::CandidateItem> &items,
        const CandidateBarMetrics &metrics,
        const std::function<double(std::string_view)> &textWidth);
};

} // namespace modernime::ui
