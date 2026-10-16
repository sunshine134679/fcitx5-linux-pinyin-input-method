#pragma once

#include "modernime/core/candidate_model.h"
#include "modernime/core/commit_event.h"

#include <cstddef>
#include <optional>
#include <string_view>

namespace modernime::core {

class CandidateProvider {
public:
    virtual ~CandidateProvider() = default;

    virtual bool append(std::string_view input) = 0;
    virtual bool eraseLast() = 0;
    virtual bool replaceInput(std::string_view input) {
        reset();
        return input.empty() || append(input);
    }
    virtual bool select(std::size_t index) = 0;
    virtual std::optional<CandidateSelectionOutcome> selectConfirmed(
        const CandidateSelectionRequest &request,
        std::uint64_t providerGeneration) {
        if (providerGeneration != page().generation) {
            return std::nullopt;
        }
        if (request.globalIndex >= page().items.size()) {
            return std::nullopt;
        }
        const auto id = candidateIdentity(page().items[request.globalIndex], page());
        if (!(id == request.expected)) {
            return std::nullopt;
        }
        if (!select(request.globalIndex)) {
            return std::nullopt;
        }
        CandidateSelectionOutcome outcome;
        outcome.selected = id;
        outcome.action = (id.role == CommitCandidateRole::Command)
            ? SelectionAction::OpenClipboard
            : SelectionAction::CommitText;
        outcome.remainingInput = page().rawInput;
        return outcome;
    }
    virtual bool extractCharacter(std::size_t, bool, std::string * = nullptr) {
        return false;
    }
    virtual bool remove(std::size_t) { return false; }
    virtual void setContext(std::string_view, std::string_view) {}
    virtual void setLearningEnabled(bool) {}
    virtual void setContextLearningEnabled(bool) {}
    virtual void onCommitEvent(const CommitEvent &) {}
    virtual void onNegativeFeedback(std::string_view, std::string_view) {}
    virtual void reset() = 0;
    virtual void resetCommitHistory() {}
    virtual const CandidatePage &page() const = 0;
};

} // namespace modernime::core
