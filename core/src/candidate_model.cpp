#include "modernime/core/candidate_model.h"

namespace modernime::core {

bool CandidatePage::select(std::size_t index) {
    if (index >= items.size()) {
        return false;
    }
    cursor = index;
    return true;
}

void CandidatePage::clear() {
    preedit.clear();
    items.clear();
    cursor = 0;
    generation = 0;
}

} // namespace modernime::core
