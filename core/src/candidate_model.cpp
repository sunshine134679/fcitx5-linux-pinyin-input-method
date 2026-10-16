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

CandidateIdentity candidateIdentity(const CandidateItem &item,
                                    const CandidatePage &page) {
    CandidateIdentity id;
    id.text = item.text;
    id.fullPinyin = item.fullPinyin;
    if (item.commitRole.has_value()) {
        id.role = *item.commitRole;
    } else {
        switch (item.source) {
            case CandidateSource::Command:
                id.role = CommitCandidateRole::Command;
                break;
            case CandidateSource::Raw:
                id.role = CommitCandidateRole::Raw;
                break;
            case CandidateSource::EnglishDefinition:
                id.role = CommitCandidateRole::English;
                break;
            case CandidateSource::UserDictionary:
                id.role = CommitCandidateRole::UserDictionary;
                break;
            case CandidateSource::Engine:
            case CandidateSource::Learned:
            default:
                id.role = CommitCandidateRole::Chinese;
                break;
        }
    }
    id.consumedInputBytes = (item.consumedInputBytes == 0)
                                ? page.rawInput.size()
                                : item.consumedInputBytes;
    return id;
}

bool CandidatePage::select(std::size_t index) {
    if (index >= items.size()) {
        return false;
    }
    cursor = index;
    return true;
}

void CandidatePage::clear() {
    rawInput.clear();
    preedit.clear();
    preeditCursor = kCursorAtEnd;
    items.clear();
    pageBoundaries.clear();
    cursor = 0;
    generation = 0;
    sessionId = 0;
    displayGeneration = 0;
    mode = CandidatePageMode::Pinyin;
}

} // namespace modernime::core
