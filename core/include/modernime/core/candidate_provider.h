#pragma once

#include "modernime/core/candidate_model.h"

#include <cstddef>
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
    virtual bool remove(std::size_t) { return false; }
    virtual void setContext(std::string_view, std::string_view) {}
    virtual void reset() = 0;
    virtual const CandidatePage &page() const = 0;
};

} // namespace modernime::core
