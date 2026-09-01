#include "modernime/core/candidate_model.h"

namespace modernime::core {

std::string candidateOrderKey(std::string_view text,
                              std::string_view fullPinyin) {
    std::string key;
    key.reserve(text.size() + fullPinyin.size() + 1);
    key.append(text);
    key.push_back('\x1f');
    key.append(fullPinyin);
    return key;
}

bool CandidatePage::select(std::size_t index) {
    if (index >= items.size()) {
        return false;
    }
    cursor = index;
    return true;
}

void CandidatePage::clear() {
    preedit.clear();
    preeditCursor = kCursorAtEnd;
    items.clear();
    cursor = 0;
    generation = 0;
    mode = CandidatePageMode::Pinyin;
}

} // namespace modernime::core
